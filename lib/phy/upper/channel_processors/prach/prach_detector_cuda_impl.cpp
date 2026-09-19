// SPDX-FileCopyrightText: Copyright (C) 2021-2026 DeepSig Inc
// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "prach_detector_cuda_impl.h"
#include "../../channel_coding/ldpc/cuda/cuda_rt_utils.h"
#include "ocudu/adt/interval.h"
#include "ocudu/ocudulog/ocudulog.h"
#include "ocudu/phy/support/prach_buffer.h"
#include "ocudu/ran/prach/prach_preamble_information.h"
#include "ocudu/support/error_handling.h"
#include "ocudu/support/math/math_utils.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cuda_runtime.h>
#include <limits>
#include <string>

using namespace ocudu;

namespace {

bool env_flag_enabled(const char* name, bool default_value = false)
{
  const char* value = std::getenv(name);
  if (value == nullptr) {
    return default_value;
  }
  std::string mode(value);
  return (mode == "1") || (mode == "true") || (mode == "on") || (mode == "yes") || (mode == "enabled");
}

unsigned env_unsigned(const char* name, unsigned default_value)
{
  const char* value = std::getenv(name);
  if (value == nullptr) {
    return default_value;
  }
  char*         end    = nullptr;
  unsigned long parsed = std::strtoul(value, &end, 10);
  if ((end == value) || (parsed > std::numeric_limits<unsigned>::max())) {
    return default_value;
  }
  return static_cast<unsigned>(parsed);
}

bool is_success(nr_ldpc_status_t status)
{
  return status == NR_LDPC_SUCCESS;
}

} // namespace

prach_detector_cuda_impl::prach_detector_cuda_impl(std::unique_ptr<prach_generator> generator_,
                                                   std::unique_ptr<prach_detector>  fallback_,
                                                   bool                             force_gpu_path_,
                                                   int                              cuda_device_id_) :
  generator(std::move(generator_)),
  fallback(std::move(fallback_)),
  force_gpu_path(force_gpu_path_),
  device_id(cuda_device_id_)
{
  ocudu_assert(generator, "Invalid PRACH generator.");
  ocudu_assert(fallback, "Invalid fallback PRACH detector.");

  int device_count = 0;
  if ((cudaGetDeviceCount(&device_count) != cudaSuccess) || (device_count == 0)) {
    cudaGetLastError();
    return;
  }

  if (cudaSetDevice(device_id) != cudaSuccess) {
    return;
  }

  cudaError_t status = cudaSetDeviceFlags(cudaDeviceScheduleYield);
  if (status == cudaErrorSetOnActiveProcess) {
    cudaGetLastError();
  } else if (status != cudaSuccess) {
    return;
  }

  cudaStream_t cuda_stream = nullptr;
  status                   = ocudu::cudaStreamCreateUpperPhy(&cuda_stream);
  if (status != cudaSuccess) {
    return;
  }
  stream = cuda_stream;

  if (!is_success(prach_detector_create(&handle))) {
    cudaStreamDestroy(cuda_stream);
    stream = nullptr;
    handle = nullptr;
    return;
  }

  gpu_available      = true;
  synchronous_warmup = !env_flag_enabled("OCUDU_PRACH_ACCELERATION_ASYNC_WARMUP", true);
}

prach_detector_cuda_impl::~prach_detector_cuda_impl()
{
  // Stop the warm-up helper before releasing anything it touches. A build already under way is allowed to finish;
  // it holds detector_mutex and runs for at most the time of one plan build.
  {
    std::lock_guard<std::mutex> lock(warmup_mutex);
    warmup_stop = true;
  }
  warmup_cv.notify_all();
  if (warmup_thread.joinable()) {
    warmup_thread.join();
  }

  std::lock_guard<std::mutex> lock(detector_mutex);
  if (gpu_available) {
    (void)cudaSetDevice(device_id);
  }
  if (handle != nullptr) {
    prach_detector_destroy(handle);
    handle = nullptr;
  }
  if (stream != nullptr) {
    cudaStreamDestroy(static_cast<cudaStream_t>(stream));
    stream = nullptr;
  }
}

bool ocudu::is_prach_detector_cuda_available()
{
  int device_count = 0;
  if ((cudaGetDeviceCount(&device_count) != cudaSuccess) || (device_count == 0)) {
    cudaGetLastError();
    return false;
  }
  return true;
}

prach_detection_result prach_detector_cuda_impl::detect(const prach_buffer&                  input,
                                                        const prach_detector::configuration& config)
{
  const detector_geometry geometry = detail::get_prach_detector_geometry(config);

  if (!should_use_gpu(input, config, geometry)) {
    return fallback->detect(input, config);
  }

  // Preparing the backend for a workload shape that it has not seen compiles a kernel, which is far too slow to do
  // here: this runs on a worker the slot chain shares. Hand the occasion to the host detector and let the helper thread
  // prepare the backend for the next one.
  const uint64_t plan_key = make_plan_key(config, geometry);
  if (!synchronous_warmup && (warm_plan_key.load(std::memory_order_acquire) != plan_key)) {
    request_warmup(config, plan_key);
    return fallback->detect(input, config);
  }

  std::lock_guard<std::mutex> lock(detector_mutex);
  // The helper thread can have prepared the backend for a different shape
  // between the check above and this lock, it holds one plan at a time.
  // Going ahead anyway would rebuild it here, but it's the thing that we avoid.
  if (!synchronous_warmup && (warm_plan_key.load(std::memory_order_acquire) != plan_key)) {
    return fallback->detect(input, config);
  }
  return detect_gpu(input, config, geometry);
}

bool prach_detector_cuda_impl::should_use_gpu(const prach_buffer&      input,
                                              const configuration&     config,
                                              const detector_geometry& geometry) const
{
  if (!gpu_available || (handle == nullptr) || (stream == nullptr)) {
    report_fatal_error_if_not(!force_gpu_path, "Accelerated PRACH detector requested but CUDA is not available.");
    return false;
  }

  if (force_gpu_path || env_flag_enabled("OCUDU_PRACH_ACCELERATION_FORCE")) {
    return true;
  }

  if (is_short_preamble(config.format)) {
    unsigned min_short_work             = env_unsigned("OCUDU_PRACH_ACCELERATION_MIN_SHORT_WORK", 64);
    unsigned min_device_short_preambles = env_unsigned("OCUDU_PRACH_ACCELERATION_MIN_DEVICE_SHORT_PREAMBLES", 1);
    unsigned short_work                 = config.nof_rx_ports * config.nof_preamble_indices;
    return (short_work >= min_short_work) ||
           (input.supports_device_prach_buffer_reading() && config.nof_preamble_indices >= min_device_short_preambles);
  }

  unsigned min_long_sequence_work = env_unsigned("OCUDU_PRACH_ACCELERATION_MIN_LONG_SEQUENCE_WORK", 4);
  unsigned long_sequence_work     = config.nof_rx_ports * geometry.active_nof_sequences;
  return long_sequence_work >= min_long_sequence_work;
}

prach_detector_config_t prach_detector_cuda_impl::make_cuda_config(const configuration&     config,
                                                                   const detector_geometry& geometry) const
{
  prach_detector_config_t cuda_config = {};
  cuda_config.sequence_length         = static_cast<int>(geometry.sequence_length);
  cuda_config.dft_size                = static_cast<int>(geometry.dft_size);
  cuda_config.nof_rx_ports            = static_cast<int>(config.nof_rx_ports);
  cuda_config.nof_symbols             = static_cast<int>(geometry.nof_symbols);
  cuda_config.nof_sequences           = static_cast<int>(geometry.active_nof_sequences);
  cuda_config.sequence_start          = static_cast<int>(geometry.active_sequence_start);
  cuda_config.nof_shifts              = static_cast<int>(geometry.nof_shifts);
  cuda_config.n_cs                    = static_cast<int>(geometry.n_cs);
  cuda_config.win_width               = static_cast<int>(geometry.win_width);
  cuda_config.win_margin              = static_cast<int>(geometry.win_margin);
  cuda_config.max_delay_samples       = static_cast<int>(geometry.max_delay_samples);
  cuda_config.start_preamble_index    = static_cast<int>(config.start_preamble_index);
  cuda_config.nof_preamble_indices    = static_cast<int>(config.nof_preamble_indices);
  cuda_config.combine_symbols         = geometry.combine_symbols ? 1 : 0;
  cuda_config.threshold               = geometry.threshold;
  uint64_t active_root_key =
      cached_root_key ^ (0x9e3779b97f4a7c15ULL + (static_cast<uint64_t>(geometry.active_sequence_start) << 6) +
                         (static_cast<uint64_t>(geometry.active_sequence_start) >> 2));
  cuda_config.root_cache_key = active_root_key;
  return cuda_config;
}

prach_detection_result prach_detector_cuda_impl::detect_gpu(const prach_buffer&      input,
                                                            const configuration&     config,
                                                            const detector_geometry& geometry)
{
  prepare_roots(config, geometry);

  prach_detector_config_t cuda_config = make_cuda_config(config, geometry);

  const void* prach_input = nullptr;
  if (input.supports_device_prach_buffer_reading() && input.prepare_device_prach_buffer_reading(stream)) {
    const auto* device_base         = static_cast<const uint32_t*>(input.get_device_prach_buffer_cbf16());
    unsigned    offset0             = input.get_device_prach_symbol_offset(0, 0, 0, 0);
    prach_input                     = device_base + offset0;
    cuda_config.input_is_device     = 1;
    cuda_config.input_symbol_stride = (input.get_max_nof_symbols() > 1)
                                          ? static_cast<int>(input.get_device_prach_symbol_offset(0, 0, 0, 1) - offset0)
                                          : static_cast<int>(geometry.sequence_length);
    cuda_config.input_port_stride   = (config.nof_rx_ports > 1)
                                          ? static_cast<int>(input.get_device_prach_symbol_offset(1, 0, 0, 0) - offset0)
                                          : static_cast<int>(geometry.nof_symbols * geometry.sequence_length);
  } else {
    host_prach_packed.resize(static_cast<size_t>(config.nof_rx_ports) * geometry.nof_symbols *
                             geometry.sequence_length);
    for (unsigned i_port = 0; i_port != config.nof_rx_ports; ++i_port) {
      for (unsigned i_symbol = 0; i_symbol != geometry.nof_symbols; ++i_symbol) {
        span<const cbf16_t> symbol = input.get_symbol(i_port, 0, 0, i_symbol);
        std::memcpy(host_prach_packed.data() + (i_port * geometry.nof_symbols + i_symbol) * geometry.sequence_length,
                    symbol.data(),
                    geometry.sequence_length * sizeof(cbf16_t));
      }
    }
    prach_input                     = host_prach_packed.data();
    cuda_config.input_is_device     = 0;
    cuda_config.input_symbol_stride = static_cast<int>(geometry.sequence_length);
    cuda_config.input_port_stride   = static_cast<int>(geometry.nof_symbols * geometry.sequence_length);
  }

  prach_detector_result_t cuda_result = {};
  const cf_t*             active_roots =
      host_roots.data() + static_cast<size_t>(geometry.active_sequence_start) * geometry.sequence_length;
  nr_ldpc_status_t status =
      prach_detector_detect(handle, prach_input, active_roots, &cuda_config, &cuda_result, stream);
  if (!is_success(status)) {
    report_fatal_error_if_not(!force_gpu_path, "Accelerated PRACH detection failed.");
    return fallback->detect(input, config);
  }

  prach_detection_result result;
  result.rssi_dB         = convert_power_to_dB(cuda_result.rssi);
  result.time_resolution = phy_time_unit::from_seconds(1.0 / geometry.sampling_rate_hz);
  result.time_advance_max =
      phy_time_unit::from_seconds(static_cast<double>(geometry.max_delay_samples) * 0.8 / geometry.sampling_rate_hz);
  result.preambles.clear();
  for (int i = 0; i != cuda_result.nof_candidates; ++i) {
    const prach_detector_candidate_t&            candidate = cuda_result.candidates[i];
    prach_detection_result::preamble_indication& info      = result.preambles.emplace_back();
    info.preamble_index                                    = static_cast<unsigned>(candidate.preamble_index);
    info.time_advance =
        phy_time_unit::from_seconds(static_cast<double>(candidate.delay_samples) / geometry.sampling_rate_hz);
    info.detection_metric  = candidate.detection_metric;
    info.preamble_power_dB = convert_power_to_dB(candidate.preamble_power);
  }

  return result;
}

void prach_detector_cuda_impl::prepare_roots(const configuration& config, const detector_geometry& geometry)
{
  uint64_t root_key = make_root_cache_key(config, geometry);
  if (root_key == cached_root_key && host_roots.size() == geometry.nof_sequences * geometry.sequence_length) {
    return;
  }

  host_roots.resize(static_cast<size_t>(geometry.nof_sequences) * geometry.sequence_length);
  for (unsigned i_sequence = 0; i_sequence != geometry.nof_sequences; ++i_sequence) {
    prach_generator::configuration generator_config;
    generator_config.format                = config.format;
    generator_config.root_sequence_index   = config.root_sequence_index;
    generator_config.preamble_index        = i_sequence * geometry.nof_shifts;
    generator_config.restricted_set        = config.restricted_set;
    generator_config.zero_correlation_zone = config.zero_correlation_zone;

    span<const cf_t> root = generator->generate(generator_config);
    std::copy(root.begin(), root.end(), host_roots.begin() + i_sequence * geometry.sequence_length);
  }
  cached_root_key = root_key;
}

uint64_t prach_detector_cuda_impl::make_root_cache_key(const configuration&     config,
                                                       const detector_geometry& geometry) const
{
  uint64_t key = 0;
  key ^= static_cast<uint64_t>(config.root_sequence_index & 0xffffU);
  key ^= static_cast<uint64_t>(config.zero_correlation_zone & 0xffU) << 16;
  key ^= static_cast<uint64_t>(static_cast<unsigned>(config.format) & 0xffU) << 24;
  key ^= static_cast<uint64_t>(static_cast<unsigned>(config.ra_scs) & 0xffU) << 32;
  key ^= static_cast<uint64_t>(static_cast<unsigned>(config.restricted_set) & 0x3U) << 40;
  key ^= static_cast<uint64_t>(geometry.nof_sequences & 0xffU) << 42;
  key ^= static_cast<uint64_t>(geometry.nof_shifts & 0x3fffU) << 50;
  return key;
}

uint64_t prach_detector_cuda_impl::make_plan_key(const configuration& config, const detector_geometry& geometry)
{
  // The backend keys its IDFT plan on the transform size and the batch count, and rebuilds whenever either moves.
  // The top bit keeps a valid key away from zero, which stands for "no plan prepared yet".
  constexpr uint64_t valid_flag = uint64_t(1) << 63;
  return valid_flag | (static_cast<uint64_t>(geometry.dft_size) << 32) |
         static_cast<uint64_t>(detail::get_nof_idft_batches(config, geometry));
}

bool prach_detector_cuda_impl::request_warmup(const configuration& config, uint64_t plan_key)
{
  std::lock_guard<std::mutex> lock(warmup_mutex);
  if (warmup_stop || warmup_pending) {
    return false;
  }

  // Each shape is prepared once. The backend holds a single plan, so a cell that alternates
  // between 2 shapes -- for instance, (1) a contention-free occasion next to (2) a contention-based one --
  // would otherwise have the helper thread rebuilt for each of them in turn forever.
  // Preparing each shape once (a) lets whichever one wins the plan run accelerated
  // and (b) leaves the other with the host detector, which is where it would end up anyway.
  if (std::find(warmed_plan_keys.begin(), warmed_plan_keys.end(), plan_key) != warmed_plan_keys.end()) {
    return false;
  }
  warmed_plan_keys.push_back(plan_key);

  warmup_config  = config;
  warmup_pending = true;
  if (!warmup_thread.joinable()) {
    warmup_thread = std::thread([this]() { warmup_worker(); });
  }
  warmup_cv.notify_one();
  return true;
}

void prach_detector_cuda_impl::warmup_worker()
{
  for (;;) {
    configuration config;
    {
      std::unique_lock<std::mutex> lock(warmup_mutex);
      warmup_cv.wait(lock, [this]() { return warmup_stop || warmup_pending; });
      if (warmup_stop) {
        return;
      }
      config = warmup_config;
    }

    build_plan(config);

    {
      std::lock_guard<std::mutex> lock(warmup_mutex);
      warmup_pending = false;
    }
    warmup_cv.notify_all();
  }
}

void prach_detector_cuda_impl::build_plan(const configuration& config)
{
  if (!gpu_available || (handle == nullptr) || (stream == nullptr)) {
    return;
  }
  // The helper thread needs the detector's device bound to it as well.
  if (cudaSetDevice(device_id) != cudaSuccess) {
    cudaGetLastError();
    return;
  }

  ocudulog::basic_logger& logger = ocudulog::fetch_basic_logger("PHY");

  const auto              t0       = std::chrono::steady_clock::now();
  const detector_geometry geometry = detail::get_prach_detector_geometry(config);
  const uint64_t          plan_key = make_plan_key(config, geometry);

  nr_ldpc_status_t status = NR_LDPC_SUCCESS;
  {
    std::lock_guard<std::mutex> lock(detector_mutex);
    prepare_roots(config, geometry);

    // One detection on zeroed input of the live shape: ports x symbols x sequence length, one packed sample per
    // word. It builds the plan, the device buffers, the kernels, and the graph. The live occasion that follows
    // re-uploads the root sequences and recaptures the graph, which costs microseconds.
    host_prach_packed.assign(static_cast<size_t>(config.nof_rx_ports) * geometry.nof_symbols * geometry.sequence_length,
                             0U);

    prach_detector_config_t cuda_config = make_cuda_config(config, geometry);
    cuda_config.input_is_device         = 0;
    cuda_config.input_symbol_stride     = static_cast<int>(geometry.sequence_length);
    cuda_config.input_port_stride       = static_cast<int>(geometry.nof_symbols * geometry.sequence_length);

    const cf_t* active_roots =
        host_roots.data() + static_cast<size_t>(geometry.active_sequence_start) * geometry.sequence_length;
    prach_detector_result_t cuda_result = {};
    status = prach_detector_detect(handle, host_prach_packed.data(), active_roots, &cuda_config, &cuda_result, stream);
  }

  const double elapsed_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  if (!is_success(status)) {
    report_fatal_error_if_not(!force_gpu_path, "Accelerated PRACH detector could not be prepared.");
    logger.warning("CUDA: PRACH detector preparation failed (status {}) after {:.1f} ms; {} x {} batches stay on the "
                   "host detector",
                   static_cast<int>(status),
                   elapsed_ms,
                   geometry.dft_size,
                   detail::get_nof_idft_batches(config, geometry));
    return;
  }

  warm_plan_key.store(plan_key, std::memory_order_release);
  logger.info("CUDA: PRACH detector prepared for format {}, {} rx port(s), zcz {} in {:.1f} ms (IDFT {} x {} batches). "
              "The occasion that triggered it was served by the host detector",
              to_string(config.format),
              config.nof_rx_ports,
              config.zero_correlation_zone,
              elapsed_ms,
              geometry.dft_size,
              detail::get_nof_idft_batches(config, geometry));
}

bool prach_detector_cuda_impl::is_warm_for(const configuration& config) const
{
  const detector_geometry geometry = detail::get_prach_detector_geometry(config);
  return warm_plan_key.load(std::memory_order_acquire) == make_plan_key(config, geometry);
}

bool prach_detector_cuda_impl::wait_for_warmup(std::chrono::milliseconds timeout)
{
  std::unique_lock<std::mutex> lock(warmup_mutex);
  return warmup_cv.wait_for(lock, timeout, [this]() { return !warmup_pending; });
}
