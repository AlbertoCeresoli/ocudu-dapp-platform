// SPDX-FileCopyrightText: Copyright (C) 2021-2026 DeepSig Inc
// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "iq_compression_cuda.h"
#include "ocudu/ofh/compression/compression_factory.h"
#include "ocudu/ofh/compression/compression_properties.h"
#include "ocudu/phy/support/prach_buffer.h"
#include "ocudu/phy/support/resource_grid_reader.h"
#include "ocudu/phy/support/resource_grid_writer.h"
#include "ocudu/support/error_handling.h"
#include "ocudu/support/ocudu_assert.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <limits>

#ifdef ENABLE_CUDA
#include "ofh_compression.h"
#endif

using namespace ocudu;
using namespace ofh;

static int to_cuda_compression_type(compression_type type)
{
  switch (type) {
    case compression_type::none:
      return 0;
    case compression_type::BFP:
      return 1;
    default:
      return -1;
  }
}

bool ocudu::ofh::is_iq_compression_cuda_available()
{
#ifdef ENABLE_CUDA
  return ocudu_ofh_compression_available() != 0;
#else
  return false;
#endif
}

/// Reads the test hook that forces every data path call to take the CPU fallback.
static bool read_force_fallback()
{
  const char* env_value = std::getenv("OCUDU_OFH_COMPRESSION_FORCE_FALLBACK");
  return (env_value != nullptr) && (env_value[0] != '\0') && (std::strcmp(env_value, "0") != 0);
}

/// Reads the test hook that fails only the first GPU call made by an object. The retry is still executed by CUDA.
static bool read_force_first_gpu_failure()
{
  const char* env_value = std::getenv("OCUDU_OFH_COMPRESSION_FORCE_FIRST_GPU_FAILURE");
  return (env_value != nullptr) && (env_value[0] != '\0') && (std::strcmp(env_value, "0") != 0);
}

iq_compression_cuda::iq_compression_cuda(compression_type                         type_,
                                         ocudulog::basic_logger&                  logger_,
                                         float                                    iq_scaling_,
                                         iq_compression_cuda_preallocation_config preallocation) :
  type(type_),
  logger(logger_),
  iq_scaling(iq_scaling_),
  force_fallback(read_force_fallback()),
  inject_next_gpu_failure(read_force_first_gpu_failure())
{
#ifdef ENABLE_CUDA
  report_fatal_error_if_not((type == compression_type::none) || (type == compression_type::BFP),
                            "CUDA OFH compression supports none and BFP only.");

  // CPU implementations used when the accelerated path fails at run-time. They are built up-front so that the fallback
  // does not allocate on the data path. Requesting "cpu_auto" cannot resolve back to CUDA, so this does not recurse.
  fallback_compressor   = create_iq_compressor(type, logger, iq_scaling, "cpu_auto");
  fallback_decompressor = create_iq_decompressor(type, logger, "cpu_auto");

  // CUDA allocation APIs may synchronize unrelated pending work. Reserve every configured receive or transmit handle
  // and its workspaces at construction so allocation never enters the normal real-time path.
  report_fatal_error_if_not(preallocation.host_buffer_compression_capacity <= std::numeric_limits<unsigned>::max(),
                            "CUDA OFH compression workspace exceeds the backend size limit.");
  if (preallocation.direct_decompression_concurrency != 0U) {
    const size_t required_capacity =
        std::min(preallocation.direct_decompression_concurrency, static_cast<size_t>(max_nof_handles));
    handle_capacity = 1U;
    while (handle_capacity < required_capacity) {
      handle_capacity *= 2U;
    }
  }
  const bool preallocate_pool =
      (preallocation.direct_decompression_concurrency != 0U) || (preallocation.host_buffer_compression_capacity != 0U);
  const unsigned initial_nof_handles = preallocate_pool ? handle_capacity : 1U;
  all_handles.reserve(handle_capacity);
  available_handles.reserve(handle_capacity);
  pending_handles.reserve(handle_capacity);
  for (unsigned i_handle = 0; i_handle != initial_nof_handles; ++i_handle) {
    ocudu_ofh_compression_handle* handle = nullptr;
    report_fatal_error_if_not(ocudu_ofh_compression_create(&handle) != 0,
                              "Failed to create CUDA OFH compression backend.");
    if (preallocation.direct_decompression_concurrency != 0U) {
      ru_compression_params max_params{type, MAX_IQ_WIDTH};
      unsigned max_input_bytes  = static_cast<unsigned>(MAX_NOF_PRBS * get_compressed_prb_size(max_params).value());
      unsigned max_output_bytes = static_cast<unsigned>(MAX_NOF_PRBS * NOF_SUBCARRIERS_PER_RB * sizeof(cbf16_t));
      report_fatal_error_if_not(
          ocudu_ofh_compression_reserve_direct_decompression(handle, max_input_bytes, max_output_bytes) != 0,
          "Failed to reserve CUDA OFH direct decompression resources.");
    }
    if (preallocation.host_buffer_compression_capacity != 0U) {
      report_fatal_error_if_not(ocudu_ofh_compression_reserve_host_buffer_compression(
                                    handle, static_cast<unsigned>(preallocation.host_buffer_compression_capacity)) != 0,
                                "Failed to reserve CUDA OFH host-buffer compression resources.");
    }
    all_handles.push_back(handle);
    available_handles.push_back(handle);
  }
  nof_handles = initial_nof_handles;
  if (preallocation.direct_decompression_concurrency != 0U) {
    reclaimer_thread = unique_thread("ofh_cuda_recl", [this]() { run_async_handle_reclaimer(); });
  }
#else
  (void)preallocation;
  report_fatal_error("CUDA OFH compression backend is not available in this build.");
#endif
}

static void log_backend_failure(ocudulog::basic_logger&             logger,
                                const char*                         operation,
                                const char*                         recovery_action,
                                const ocudu_ofh_compression_handle* failed_handle,
                                bool                                injected,
                                uint64_t                            count)
{
#ifdef ENABLE_CUDA
  ocudu_ofh_compression_error_info_t error_info = {};
  if (injected) {
    logger.warning("CUDA OFH {} test failure injected; {}. Total events: {}", operation, recovery_action, count);
  } else if ((failed_handle != nullptr) && ocudu_ofh_compression_get_last_error(failed_handle, &error_info) &&
             (error_info.cuda_error_code != 0)) {
    logger.warning("CUDA OFH {} failed during '{}': {} ({}): {}; {}. Total events: {}",
                   operation,
                   error_info.operation,
                   error_info.error_name,
                   error_info.cuda_error_code,
                   error_info.error_description,
                   recovery_action,
                   count);
  } else {
    logger.warning(
        "CUDA OFH {} failed without a CUDA runtime error; {}. Total events: {}", operation, recovery_action, count);
  }
#else
  (void)logger;
  (void)operation;
  (void)recovery_action;
  (void)failed_handle;
  (void)injected;
  (void)count;
#endif
}

void iq_compression_cuda::note_acceleration_retry(const char*                         operation,
                                                  const ocudu_ofh_compression_handle* failed_handle,
                                                  bool                                injected)
{
  uint64_t nof_retries = nof_acceleration_retries.fetch_add(1, std::memory_order_relaxed) + 1;
  if ((nof_retries == 1) || ((nof_retries % failure_log_period) == 0)) {
    log_backend_failure(logger, operation, "retrying on a fresh CUDA handle", failed_handle, injected, nof_retries);
  }
}

void iq_compression_cuda::note_acceleration_failure(const char*                         operation,
                                                    const ocudu_ofh_compression_handle* failed_handle,
                                                    bool                                injected,
                                                    const char*                         recovery_action)
{
  uint64_t nof_failures = nof_acceleration_failures.fetch_add(1, std::memory_order_relaxed) + 1;
  if ((nof_failures == 1) || ((nof_failures % failure_log_period) == 0)) {
    log_backend_failure(logger, operation, recovery_action, failed_handle, injected, nof_failures);
  }
}

iq_compression_cuda::~iq_compression_cuda()
{
#ifdef ENABLE_CUDA
  std::vector<ocudu_ofh_compression_handle*> handles;
  {
    std::lock_guard<std::mutex> lock(mutex);
    shutting_down = true;
  }
  reclaimer_cv.notify_all();
  reclaimer_thread.join();
  {
    std::lock_guard<std::mutex> lock(mutex);
    handles = all_handles;
    all_handles.clear();
    available_handles.clear();
    pending_handles.clear();
  }
  for (ocudu_ofh_compression_handle* handle : handles) {
    ocudu_ofh_compression_destroy(handle);
  }
#endif
}

ocudu_ofh_compression_handle* iq_compression_cuda::acquire_handle()
{
#ifdef ENABLE_CUDA
  ocudu_ofh_compression_handle* acquired_handle = nullptr;
  bool                          create_handle   = false;
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (!available_handles.empty()) {
      acquired_handle = available_handles.back();
      available_handles.pop_back();
    }
    if ((acquired_handle == nullptr) && (nof_handles < handle_capacity)) {
      ++nof_handles;
      create_handle = true;
    }

    // The asynchronous reclaimer is deliberately kept off the real-time receive executor, but it can be delayed by
    // host scheduling. On pool exhaustion, inspect the stream-ordered completion flags here before rejecting the
    // packet. This is a bounded, nonblocking query and avoids dropping GPU-resident work merely because a completed
    // handle has not yet been moved back to the available list.
    if ((acquired_handle == nullptr) && !create_handle) {
      auto completed = std::find_if(pending_handles.begin(), pending_handles.end(), [](auto* handle) {
        return ocudu_ofh_compression_query_completion(handle) > 0;
      });
      if (completed != pending_handles.end()) {
        acquired_handle = *completed;
        pending_handles.erase(completed);
      }
    }
  }

  if (acquired_handle != nullptr) {
    return acquired_handle;
  }
  if (!create_handle) {
    return nullptr;
  }

  ocudu_ofh_compression_handle* handle = nullptr;
  if (ocudu_ofh_compression_create(&handle) == 0) {
    std::lock_guard<std::mutex> lock(mutex);
    --nof_handles;
    return nullptr;
  }
  {
    std::lock_guard<std::mutex> lock(mutex);
    all_handles.push_back(handle);
  }
  return handle;
#else
  return nullptr;
#endif
}

void iq_compression_cuda::release_handle(ocudu_ofh_compression_handle* acquired_handle)
{
#ifdef ENABLE_CUDA
  if (acquired_handle == nullptr) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex);
  available_handles.push_back(acquired_handle);
#else
  (void)acquired_handle;
#endif
}

void iq_compression_cuda::release_async_handle(ocudu_ofh_compression_handle* acquired_handle)
{
#ifdef ENABLE_CUDA
  ocudu_assert(acquired_handle != nullptr, "Cannot asynchronously release a null CUDA OFH handle");
  ocudu_assert(reclaimer_thread.running(), "CUDA OFH direct decompression requires an asynchronous handle reclaimer");
  bool notify_reclaimer = false;
  {
    std::lock_guard<std::mutex> lock(mutex);
    notify_reclaimer = pending_handles.empty();
    pending_handles.push_back(acquired_handle);
  }
  if (notify_reclaimer) {
    reclaimer_cv.notify_one();
  }
#else
  (void)acquired_handle;
#endif
}

void iq_compression_cuda::run_async_handle_reclaimer()
{
#ifdef ENABLE_CUDA
  std::vector<ocudu_ofh_compression_handle*> handles_to_query;
  std::vector<ocudu_ofh_compression_handle*> completed_handles;
  std::vector<ocudu_ofh_compression_handle*> incomplete_handles;
  std::vector<ocudu_ofh_compression_handle*> failed_handles;
  handles_to_query.reserve(handle_capacity);
  completed_handles.reserve(handle_capacity);
  incomplete_handles.reserve(handle_capacity);
  failed_handles.reserve(handle_capacity);

  while (true) {
    {
      std::unique_lock<std::mutex> lock(mutex);
      if (pending_handles.empty()) {
        reclaimer_cv.wait(lock, [this]() { return shutting_down || !pending_handles.empty(); });
      } else {
        (void)reclaimer_cv.wait_for(lock, async_handle_poll_period, [this]() { return shutting_down; });
      }
      if (shutting_down) {
        return;
      }
      handles_to_query.swap(pending_handles);
    }

    completed_handles.clear();
    incomplete_handles.clear();
    failed_handles.clear();
    for (ocudu_ofh_compression_handle* handle : handles_to_query) {
      int completion = ocudu_ofh_compression_query_completion(handle);
      if (completion > 0) {
        completed_handles.push_back(handle);
      } else if (completion < 0) {
        failed_handles.push_back(handle);
      } else {
        incomplete_handles.push_back(handle);
      }
    }

    {
      std::lock_guard<std::mutex> lock(mutex);
      if (shutting_down) {
        return;
      }
      available_handles.insert(available_handles.end(), completed_handles.begin(), completed_handles.end());
      pending_handles.insert(pending_handles.end(), incomplete_handles.begin(), incomplete_handles.end());
      for (ocudu_ofh_compression_handle* failed_handle : failed_handles) {
        auto it = std::find(all_handles.begin(), all_handles.end(), failed_handle);
        ocudu_assert(it != all_handles.end(), "Failed CUDA OFH handle is not owned by this pool");
        all_handles.erase(it);
        --nof_handles;
      }
    }

    for (ocudu_ofh_compression_handle* failed_handle : failed_handles) {
      note_acceleration_failure("asynchronous completion", failed_handle, false, "quarantining the failed CUDA handle");
      ocudu_ofh_compression_destroy(failed_handle);
    }
    handles_to_query.clear();
  }
#endif
}

void iq_compression_cuda::discard_handle(ocudu_ofh_compression_handle* failed_handle)
{
#ifdef ENABLE_CUDA
  if (failed_handle != nullptr) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      auto                        it = std::find(all_handles.begin(), all_handles.end(), failed_handle);
      ocudu_assert(it != all_handles.end(), "Discarded CUDA OFH handle is not owned by this pool");
      all_handles.erase(it);
      --nof_handles;
    }
    ocudu_ofh_compression_destroy(failed_handle);
  }
#else
  (void)failed_handle;
#endif
}

bool iq_compression_cuda::should_inject_gpu_failure()
{
  return inject_next_gpu_failure.exchange(false, std::memory_order_relaxed);
}

void iq_compression_cuda::compress(span<uint8_t>                buffer,
                                   span<const cbf16_t>          iq_data,
                                   const ru_compression_params& params)
{
#ifdef ENABLE_CUDA
  ocudu_assert(params.type == type, "CUDA OFH compression object used with unexpected compression type.");
  unsigned nof_prbs = iq_data.size() / NOF_SUBCARRIERS_PER_RB;
  unsigned prb_size = get_compressed_prb_size(params).value();
  ocudu_assert(buffer.size() >= prb_size * nof_prbs, "Output buffer does not have enough space for compressed PRBs.");
  if (nof_prbs == 0) {
    return;
  }
  bool success       = false;
  bool first_failure = false;
  if (!force_fallback) {
    if (ocudu_ofh_compression_handle* handle = acquire_handle(); handle != nullptr) {
      bool injected = should_inject_gpu_failure();
      success       = !injected && (ocudu_ofh_compress(handle,
                                                 to_cuda_compression_type(type),
                                                 buffer.data(),
                                                 iq_data.data(),
                                                 nof_prbs,
                                                 params.data_width,
                                                 iq_scaling) != 0);
      if (success) {
        release_handle(handle);
      } else {
        first_failure = true;
        note_acceleration_retry("compression", handle, injected);
        discard_handle(handle);
      }
    } else {
      first_failure = true;
      note_acceleration_retry("compression handle creation", nullptr, false);
    }

    if (first_failure) {
      if (ocudu_ofh_compression_handle* retry_handle = acquire_handle(); retry_handle != nullptr) {
        success = ocudu_ofh_compress(retry_handle,
                                     to_cuda_compression_type(type),
                                     buffer.data(),
                                     iq_data.data(),
                                     nof_prbs,
                                     params.data_width,
                                     iq_scaling) != 0;
        if (success) {
          release_handle(retry_handle);
        } else {
          note_acceleration_failure("compression retry", retry_handle);
          discard_handle(retry_handle);
        }
      } else {
        note_acceleration_failure("compression retry handle creation");
      }
    }
  }

  // A failure of the accelerated backend must not bring the fronthaul down: compress the symbol on the CPU instead.
  if (!success) {
    if (force_fallback) {
      note_acceleration_failure("compression", nullptr, true);
    }
    fallback_compressor->compress(buffer, iq_data, params);
  }
#else
  (void)buffer;
  (void)iq_data;
  (void)params;
  report_fatal_error("CUDA OFH compression backend is not available in this build.");
#endif
}

bool iq_compression_cuda::compress_device_symbol(span<uint8_t>                buffer,
                                                 const resource_grid_reader&  grid,
                                                 unsigned                     port,
                                                 unsigned                     symbol,
                                                 unsigned                     start_prb,
                                                 unsigned                     nof_prbs,
                                                 const ru_compression_params& params)
{
#ifdef ENABLE_CUDA
  if (!grid.supports_device_grid_reading()) {
    return false;
  }
  if ((params.type != type) || (nof_prbs == 0)) {
    return false;
  }

  unsigned prb_size = get_compressed_prb_size(params).value();
  if (buffer.size() < prb_size * nof_prbs) {
    return false;
  }

  ocudu_ofh_compression_handle* handle = acquire_handle();
  if (handle == nullptr) {
    return false;
  }

  void* stream = ocudu_ofh_compression_get_stream(handle);
  if (!grid.prepare_device_grid_reading(stream)) {
    release_handle(handle);
    return false;
  }
  bool success = ocudu_ofh_compress_device_grid(handle,
                                                to_cuda_compression_type(type),
                                                buffer.data(),
                                                grid.get_device_grid_cbf16(),
                                                grid.get_nof_symbols(),
                                                grid.get_nof_subc(),
                                                port,
                                                symbol,
                                                start_prb,
                                                nof_prbs,
                                                params.data_width,
                                                iq_scaling) != 0;
  if (success) {
    release_handle(handle);
  } else {
    note_acceleration_failure("device-grid compression", handle);
    discard_handle(handle);
  }
  return success;
#else
  (void)buffer;
  (void)grid;
  (void)port;
  (void)symbol;
  (void)start_prb;
  (void)nof_prbs;
  (void)params;
  return false;
#endif
}

bool iq_compression_cuda::compress_device_symbols(span<uint8_t>                buffer,
                                                  unsigned                     port_stride_bytes,
                                                  const resource_grid_reader&  grid,
                                                  unsigned                     first_port,
                                                  unsigned                     nof_ports,
                                                  unsigned                     symbol,
                                                  unsigned                     start_prb,
                                                  unsigned                     nof_prbs,
                                                  const ru_compression_params& params)
{
#ifdef ENABLE_CUDA
  if (!grid.supports_device_grid_reading()) {
    return false;
  }
  if ((params.type != type) || (nof_ports == 0) || (nof_prbs == 0)) {
    return false;
  }

  unsigned prb_size   = get_compressed_prb_size(params).value();
  unsigned port_bytes = prb_size * nof_prbs;
  if ((port_stride_bytes < port_bytes) ||
      (buffer.size() < static_cast<size_t>(nof_ports - 1U) * port_stride_bytes + port_bytes)) {
    return false;
  }

  ocudu_ofh_compression_handle* handle = acquire_handle();
  if (handle == nullptr) {
    return false;
  }

  void* stream = ocudu_ofh_compression_get_stream(handle);
  if (!grid.prepare_device_grid_reading(stream)) {
    release_handle(handle);
    return false;
  }
  bool success = ocudu_ofh_compress_device_grid_ports(handle,
                                                      to_cuda_compression_type(type),
                                                      buffer.data(),
                                                      port_stride_bytes,
                                                      grid.get_device_grid_cbf16(),
                                                      grid.get_nof_symbols(),
                                                      grid.get_nof_subc(),
                                                      first_port,
                                                      nof_ports,
                                                      symbol,
                                                      start_prb,
                                                      nof_prbs,
                                                      params.data_width,
                                                      iq_scaling) != 0;
  if (success) {
    release_handle(handle);
  } else {
    note_acceleration_failure("device-grid port compression", handle);
    discard_handle(handle);
  }
  return success;
#else
  (void)buffer;
  (void)port_stride_bytes;
  (void)grid;
  (void)first_port;
  (void)nof_ports;
  (void)symbol;
  (void)start_prb;
  (void)nof_prbs;
  (void)params;
  return false;
#endif
}

bool iq_compression_cuda::compress_device_symbols_to_buffers(span<span<uint8_t>>          buffers,
                                                             const resource_grid_reader&  grid,
                                                             unsigned                     first_port,
                                                             unsigned                     nof_ports,
                                                             unsigned                     symbol,
                                                             unsigned                     start_prb,
                                                             unsigned                     nof_prbs,
                                                             const ru_compression_params& params)
{
#ifdef ENABLE_CUDA
  if (!grid.supports_device_grid_reading()) {
    return false;
  }
  if ((params.type != type) || (nof_ports == 0) || (nof_prbs == 0) || (buffers.size() < nof_ports)) {
    return false;
  }

  unsigned                        prb_size   = get_compressed_prb_size(params).value();
  unsigned                        port_bytes = prb_size * nof_prbs;
  thread_local std::vector<void*> output_buffers;
  output_buffers.resize(nof_ports);
  for (unsigned i_port = 0; i_port != nof_ports; ++i_port) {
    if (buffers[i_port].size() < port_bytes) {
      return false;
    }
    output_buffers[i_port] = buffers[i_port].data();
  }

  ocudu_ofh_compression_handle* handle = acquire_handle();
  if (handle == nullptr) {
    return false;
  }

  void* stream = ocudu_ofh_compression_get_stream(handle);
  if (!grid.prepare_device_grid_reading(stream)) {
    release_handle(handle);
    return false;
  }
  bool success = ocudu_ofh_compress_device_grid_ports_to_host_buffers(handle,
                                                                      to_cuda_compression_type(type),
                                                                      output_buffers.data(),
                                                                      nof_ports,
                                                                      port_bytes,
                                                                      grid.get_device_grid_cbf16(),
                                                                      grid.get_nof_symbols(),
                                                                      grid.get_nof_subc(),
                                                                      first_port,
                                                                      nof_ports,
                                                                      symbol,
                                                                      start_prb,
                                                                      nof_prbs,
                                                                      params.data_width,
                                                                      iq_scaling) != 0;
  if (success) {
    release_handle(handle);
  } else {
    note_acceleration_failure("device-grid host-buffer compression", handle);
    discard_handle(handle);
  }
  return success;
#else
  (void)buffers;
  (void)grid;
  (void)first_port;
  (void)nof_ports;
  (void)symbol;
  (void)start_prb;
  (void)nof_prbs;
  (void)params;
  return false;
#endif
}

bool iq_compression_cuda::compress_device_symbol_batch(span<uint8_t>                buffer,
                                                       unsigned                     symbol_stride_bytes,
                                                       unsigned                     port_stride_bytes,
                                                       const resource_grid_reader&  grid,
                                                       unsigned                     first_port,
                                                       unsigned                     nof_ports,
                                                       unsigned                     first_symbol,
                                                       unsigned                     nof_symbols,
                                                       unsigned                     start_prb,
                                                       unsigned                     nof_prbs,
                                                       const ru_compression_params& params)
{
#ifdef ENABLE_CUDA
  if (!grid.supports_device_grid_reading()) {
    return false;
  }
  if ((params.type != type) || (nof_ports == 0) || (nof_symbols == 0) || (nof_prbs == 0)) {
    return false;
  }

  unsigned prb_size      = get_compressed_prb_size(params).value();
  unsigned port_bytes    = prb_size * nof_prbs;
  size_t   required_size = static_cast<size_t>(nof_symbols - 1U) * symbol_stride_bytes +
                         static_cast<size_t>(nof_ports - 1U) * port_stride_bytes + port_bytes;
  if ((port_stride_bytes < port_bytes) ||
      (symbol_stride_bytes < static_cast<size_t>(nof_ports - 1U) * port_stride_bytes + port_bytes) ||
      (buffer.size() < required_size)) {
    return false;
  }

  ocudu_ofh_compression_handle* handle = acquire_handle();
  if (handle == nullptr) {
    return false;
  }

  void* stream = ocudu_ofh_compression_get_stream(handle);
  if (!grid.prepare_device_grid_reading(stream)) {
    release_handle(handle);
    return false;
  }
  bool success = ocudu_ofh_compress_device_grid_symbol_batch(handle,
                                                             to_cuda_compression_type(type),
                                                             buffer.data(),
                                                             symbol_stride_bytes,
                                                             port_stride_bytes,
                                                             grid.get_device_grid_cbf16(),
                                                             grid.get_nof_symbols(),
                                                             grid.get_nof_subc(),
                                                             first_port,
                                                             nof_ports,
                                                             first_symbol,
                                                             nof_symbols,
                                                             start_prb,
                                                             nof_prbs,
                                                             params.data_width,
                                                             iq_scaling) != 0;
  if (success) {
    release_handle(handle);
  } else {
    note_acceleration_failure("device-grid symbol-batch compression", handle);
    discard_handle(handle);
  }
  return success;
#else
  (void)buffer;
  (void)symbol_stride_bytes;
  (void)port_stride_bytes;
  (void)grid;
  (void)first_port;
  (void)nof_ports;
  (void)first_symbol;
  (void)nof_symbols;
  (void)start_prb;
  (void)nof_prbs;
  (void)params;
  return false;
#endif
}

bool iq_compression_cuda::compress_device_symbol_batch_to_buffers(span<span<uint8_t>>          buffers,
                                                                  const resource_grid_reader&  grid,
                                                                  unsigned                     first_port,
                                                                  unsigned                     nof_ports,
                                                                  unsigned                     first_symbol,
                                                                  unsigned                     nof_symbols,
                                                                  unsigned                     start_prb,
                                                                  unsigned                     nof_prbs,
                                                                  const ru_compression_params& params)
{
#ifdef ENABLE_CUDA
  if (!grid.supports_device_grid_reading()) {
    return false;
  }
  if ((params.type != type) || (nof_ports == 0) || (nof_symbols == 0) || (nof_prbs == 0)) {
    return false;
  }

  unsigned nof_buffers = nof_symbols * nof_ports;
  if (buffers.size() < nof_buffers) {
    return false;
  }

  unsigned                        prb_size   = get_compressed_prb_size(params).value();
  unsigned                        port_bytes = prb_size * nof_prbs;
  thread_local std::vector<void*> output_buffers;
  output_buffers.resize(nof_buffers);
  for (unsigned i_buffer = 0; i_buffer != nof_buffers; ++i_buffer) {
    if (buffers[i_buffer].size() < port_bytes) {
      return false;
    }
    output_buffers[i_buffer] = buffers[i_buffer].data();
  }

  ocudu_ofh_compression_handle* handle = acquire_handle();
  if (handle == nullptr) {
    return false;
  }

  void* stream = ocudu_ofh_compression_get_stream(handle);
  if (!grid.prepare_device_grid_reading(stream)) {
    release_handle(handle);
    return false;
  }
  bool success = ocudu_ofh_compress_device_grid_symbol_batch_to_host_buffers(handle,
                                                                             to_cuda_compression_type(type),
                                                                             output_buffers.data(),
                                                                             nof_buffers,
                                                                             port_bytes,
                                                                             grid.get_device_grid_cbf16(),
                                                                             grid.get_nof_symbols(),
                                                                             grid.get_nof_subc(),
                                                                             first_port,
                                                                             nof_ports,
                                                                             first_symbol,
                                                                             nof_symbols,
                                                                             start_prb,
                                                                             nof_prbs,
                                                                             params.data_width,
                                                                             iq_scaling) != 0;
  if (success) {
    release_handle(handle);
  } else {
    note_acceleration_failure("device-grid host-buffer symbol-batch compression", handle);
    discard_handle(handle);
  }
  return success;
#else
  (void)buffers;
  (void)grid;
  (void)first_port;
  (void)nof_ports;
  (void)first_symbol;
  (void)nof_symbols;
  (void)start_prb;
  (void)nof_prbs;
  (void)params;
  return false;
#endif
}

void iq_compression_cuda::decompress(span<cbf16_t>                iq_data,
                                     span<const uint8_t>          compressed_data,
                                     const ru_compression_params& params)
{
#ifdef ENABLE_CUDA
  ocudu_assert(params.type == type, "CUDA OFH decompression object used with unexpected compression type.");
  unsigned nof_prbs = iq_data.size() / NOF_SUBCARRIERS_PER_RB;
  unsigned prb_size = get_compressed_prb_size(params).value();
  ocudu_assert(compressed_data.size() >= nof_prbs * prb_size,
               "Input does not contain enough bytes to decompress {} PRBs",
               nof_prbs);
  if (nof_prbs == 0) {
    return;
  }
  bool success       = false;
  bool first_failure = false;
  if (!force_fallback) {
    if (ocudu_ofh_compression_handle* handle = acquire_handle(); handle != nullptr) {
      bool injected = should_inject_gpu_failure();
      success       = !injected && (ocudu_ofh_decompress(handle,
                                                   to_cuda_compression_type(type),
                                                   iq_data.data(),
                                                   compressed_data.data(),
                                                   nof_prbs,
                                                   params.data_width) != 0);
      if (success) {
        release_handle(handle);
      } else {
        first_failure = true;
        note_acceleration_retry("decompression", handle, injected);
        discard_handle(handle);
      }
    } else {
      first_failure = true;
      note_acceleration_retry("decompression handle creation", nullptr, false);
    }

    if (first_failure) {
      if (ocudu_ofh_compression_handle* retry_handle = acquire_handle(); retry_handle != nullptr) {
        success = ocudu_ofh_decompress(retry_handle,
                                       to_cuda_compression_type(type),
                                       iq_data.data(),
                                       compressed_data.data(),
                                       nof_prbs,
                                       params.data_width) != 0;
        if (success) {
          release_handle(retry_handle);
        } else {
          note_acceleration_failure("decompression retry", retry_handle);
          discard_handle(retry_handle);
        }
      } else {
        note_acceleration_failure("decompression retry handle creation");
      }
    }
  }

  // See the comment in compress(): fall back to the CPU implementation instead of terminating the process.
  if (!success) {
    if (force_fallback) {
      note_acceleration_failure("decompression", nullptr, true);
    }
    fallback_decompressor->decompress(iq_data, compressed_data, params);
  }
#else
  (void)iq_data;
  (void)compressed_data;
  (void)params;
  report_fatal_error("CUDA OFH compression backend is not available in this build.");
#endif
}

iq_decompression_result iq_compression_cuda::decompress_to_resource_grid(resource_grid_writer&        grid,
                                                                         unsigned                     port,
                                                                         unsigned                     symbol,
                                                                         unsigned                     start_prb,
                                                                         unsigned                     nof_prbs,
                                                                         span<const uint8_t>          compressed_data,
                                                                         const ru_compression_params& params)
{
#ifdef ENABLE_CUDA
  if (!grid.supports_device_grid_mapping()) {
    return iq_decompression_result::unsupported;
  }
  if ((params.type != type) || (params.data_width == 0) || (params.data_width > MAX_IQ_WIDTH) || (nof_prbs == 0)) {
    return iq_decompression_result::invalid_input;
  }

  unsigned prb_size = get_compressed_prb_size(params).value();
  if (compressed_data.size() < prb_size * nof_prbs) {
    return iq_decompression_result::invalid_input;
  }

  if (force_fallback) {
    note_acceleration_failure(
        "device-grid decompression", nullptr, true, "dropping the affected Open Fronthaul message");
    return iq_decompression_result::acceleration_failure;
  }

  for (unsigned attempt = 0; attempt != max_direct_decompression_attempts; ++attempt) {
    ocudu_ofh_compression_handle* handle = acquire_handle();
    if (handle == nullptr) {
      if (attempt == 0) {
        note_acceleration_retry("device-grid decompression handle creation", nullptr, false);
        continue;
      }
      note_acceleration_failure(
          "device-grid decompression handle creation", nullptr, false, "dropping the affected Open Fronthaul message");
      return iq_decompression_result::resource_unavailable;
    }

    void* stream = ocudu_ofh_compression_get_stream(handle);
    if (!grid.prepare_device_grid_mapping(stream)) {
      release_handle(handle);
      return iq_decompression_result::destination_failure;
    }

    bool injected        = should_inject_gpu_failure();
    bool backend_success = !injected && (ocudu_ofh_decompress_to_device_grid_async(handle,
                                                                                   to_cuda_compression_type(type),
                                                                                   grid.get_device_grid_bf16(),
                                                                                   compressed_data.data(),
                                                                                   grid.get_nof_symbols(),
                                                                                   grid.get_nof_subc(),
                                                                                   port,
                                                                                   symbol,
                                                                                   start_prb,
                                                                                   nof_prbs,
                                                                                   params.data_width) != 0);
    if (!backend_success) {
      bool synchronized = ocudu_ofh_compression_synchronize(handle) != 0;
      bool cancelled    = grid.cancel_device_grid_mapping();
      if ((attempt == 0) && synchronized && cancelled) {
        note_acceleration_retry("device-grid decompression", handle, injected);
        discard_handle(handle);
        continue;
      }
      note_acceleration_failure(
          "device-grid decompression", handle, injected, "dropping the affected Open Fronthaul message");
      discard_handle(handle);
      return (synchronized && !cancelled) ? iq_decompression_result::destination_failure
                                          : iq_decompression_result::acceleration_failure;
    }

    if (!grid.on_device_grid_mapping_enqueued(stream)) {
      bool synchronized = ocudu_ofh_compression_synchronize(handle) != 0;
      (void)grid.cancel_device_grid_mapping();
      if (!synchronized) {
        note_acceleration_failure(
            "device-grid decompression synchronization", handle, false, "dropping the affected Open Fronthaul message");
        discard_handle(handle);
        return iq_decompression_result::acceleration_failure;
      }
      release_handle(handle);
      return iq_decompression_result::destination_failure;
    }

    release_async_handle(handle);
    return (attempt == 0) ? iq_decompression_result::success : iq_decompression_result::success_after_retry;
  }

  return iq_decompression_result::acceleration_failure;
#else
  (void)grid;
  (void)port;
  (void)symbol;
  (void)start_prb;
  (void)nof_prbs;
  (void)compressed_data;
  (void)params;
  return iq_decompression_result::unsupported;
#endif
}

iq_decompression_result iq_compression_cuda::decompress_to_prach_buffer(prach_buffer&                buffer,
                                                                        unsigned                     port,
                                                                        unsigned                     td_occasion,
                                                                        unsigned                     fd_occasion,
                                                                        unsigned                     symbol,
                                                                        unsigned                     start_re,
                                                                        unsigned                     input_start_re,
                                                                        unsigned                     nof_re,
                                                                        unsigned                     nof_prbs,
                                                                        span<const uint8_t>          compressed_data,
                                                                        const ru_compression_params& params)
{
#ifdef ENABLE_CUDA
  if (!buffer.supports_device_prach_buffer_mapping()) {
    return iq_decompression_result::unsupported;
  }
  if ((params.type != type) || (params.data_width == 0) || (params.data_width > MAX_IQ_WIDTH) || (nof_prbs == 0) ||
      (nof_re == 0)) {
    return iq_decompression_result::invalid_input;
  }

  unsigned prb_size = get_compressed_prb_size(params).value();
  if (compressed_data.size() < prb_size * nof_prbs) {
    return iq_decompression_result::invalid_input;
  }

  if (force_fallback) {
    note_acceleration_failure(
        "device PRACH decompression", nullptr, true, "dropping the affected Open Fronthaul message");
    return iq_decompression_result::acceleration_failure;
  }

  for (unsigned attempt = 0; attempt != max_direct_decompression_attempts; ++attempt) {
    ocudu_ofh_compression_handle* handle = acquire_handle();
    if (handle == nullptr) {
      if (attempt == 0) {
        note_acceleration_retry("device PRACH decompression handle creation", nullptr, false);
        continue;
      }
      note_acceleration_failure(
          "device PRACH decompression handle creation", nullptr, false, "dropping the affected Open Fronthaul message");
      return iq_decompression_result::resource_unavailable;
    }

    void* stream = ocudu_ofh_compression_get_stream(handle);
    if (!buffer.prepare_device_prach_buffer_mapping(stream)) {
      release_handle(handle);
      return iq_decompression_result::destination_failure;
    }

    unsigned output_offset_re =
        buffer.get_device_prach_symbol_offset(port, td_occasion, fd_occasion, symbol) + start_re;
    bool injected = should_inject_gpu_failure();
    bool backend_success =
        !injected && (ocudu_ofh_decompress_to_device_prach_buffer_async(handle,
                                                                        to_cuda_compression_type(type),
                                                                        buffer.get_device_prach_buffer_cbf16(),
                                                                        output_offset_re,
                                                                        compressed_data.data(),
                                                                        input_start_re,
                                                                        nof_re,
                                                                        nof_prbs,
                                                                        params.data_width) != 0);
    if (!backend_success) {
      bool synchronized = ocudu_ofh_compression_synchronize(handle) != 0;
      bool cancelled    = buffer.cancel_device_prach_buffer_mapping();
      if ((attempt == 0) && synchronized && cancelled) {
        note_acceleration_retry("device PRACH decompression", handle, injected);
        discard_handle(handle);
        continue;
      }
      note_acceleration_failure(
          "device PRACH decompression", handle, injected, "dropping the affected Open Fronthaul message");
      discard_handle(handle);
      return (synchronized && !cancelled) ? iq_decompression_result::destination_failure
                                          : iq_decompression_result::acceleration_failure;
    }

    if (!buffer.on_device_prach_buffer_mapping_enqueued(stream)) {
      bool synchronized = ocudu_ofh_compression_synchronize(handle) != 0;
      (void)buffer.cancel_device_prach_buffer_mapping();
      if (!synchronized) {
        note_acceleration_failure("device PRACH decompression synchronization",
                                  handle,
                                  false,
                                  "dropping the affected Open Fronthaul message");
        discard_handle(handle);
        return iq_decompression_result::acceleration_failure;
      }
      release_handle(handle);
      return iq_decompression_result::destination_failure;
    }

    release_async_handle(handle);
    return (attempt == 0) ? iq_decompression_result::success : iq_decompression_result::success_after_retry;
  }

  return iq_decompression_result::acceleration_failure;
#else
  (void)buffer;
  (void)port;
  (void)td_occasion;
  (void)fd_occasion;
  (void)symbol;
  (void)start_re;
  (void)input_start_re;
  (void)nof_re;
  (void)nof_prbs;
  (void)compressed_data;
  (void)params;
  return iq_decompression_result::unsupported;
#endif
}
