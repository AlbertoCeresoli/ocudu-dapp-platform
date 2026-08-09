// SPDX-FileCopyrightText: Copyright (C) 2021-2026 DeepSig Inc
// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#pragma once

#include "ocudu/ocudulog/logger.h"
#include "ocudu/ofh/compression/compression_params.h"
#include "ocudu/ofh/compression/iq_compressor.h"
#include "ocudu/ofh/compression/iq_decompressor.h"
#include "ocudu/support/executors/unique_thread.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <vector>

struct ocudu_ofh_compression_handle;

namespace ocudu {
namespace ofh {

bool is_iq_compression_cuda_available();

struct iq_compression_cuda_preallocation_config {
  size_t direct_decompression_concurrency = 0;
  size_t host_buffer_compression_capacity = 0;
};

/// \brief CUDA accelerated IQ data compressor and decompressor.
///
/// A failed CUDA handle is quarantined and operations are retried once on a fresh handle when the destination can be
/// safely overwritten. Host-buffer operations use the equivalent CPU implementation only after recovery fails. Direct
/// device-destination operations return a structured terminal error and never enter host IQ decompression. Handles
/// with asynchronous work in flight are not reused, so a delayed device consumer cannot block the fronthaul receiver.
class iq_compression_cuda : public iq_compressor, public iq_decompressor
{
public:
  explicit iq_compression_cuda(compression_type        type_,
                               ocudulog::basic_logger& logger_,
                               float                   iq_scaling_ = 1.0F,
                               iq_compression_cuda_preallocation_config preallocation = {});
  ~iq_compression_cuda() override;

  iq_compression_cuda(const iq_compression_cuda&)            = delete;
  iq_compression_cuda& operator=(const iq_compression_cuda&) = delete;

  void compress(span<uint8_t> buffer, span<const cbf16_t> iq_data, const ru_compression_params& params) override;

  bool compress_device_symbol(span<uint8_t>                buffer,
                              const resource_grid_reader&  grid,
                              unsigned                     port,
                              unsigned                     symbol,
                              unsigned                     start_prb,
                              unsigned                     nof_prbs,
                              const ru_compression_params& params) override;

  bool compress_device_symbols(span<uint8_t>                buffer,
                               unsigned                     port_stride_bytes,
                               const resource_grid_reader&  grid,
                               unsigned                     first_port,
                               unsigned                     nof_ports,
                               unsigned                     symbol,
                               unsigned                     start_prb,
                               unsigned                     nof_prbs,
                               const ru_compression_params& params) override;

  bool compress_device_symbols_to_buffers(span<span<uint8_t>>          buffers,
                                          const resource_grid_reader&  grid,
                                          unsigned                     first_port,
                                          unsigned                     nof_ports,
                                          unsigned                     symbol,
                                          unsigned                     start_prb,
                                          unsigned                     nof_prbs,
                                          const ru_compression_params& params) override;

  bool compress_device_symbol_batch(span<uint8_t>                buffer,
                                    unsigned                     symbol_stride_bytes,
                                    unsigned                     port_stride_bytes,
                                    const resource_grid_reader&  grid,
                                    unsigned                     first_port,
                                    unsigned                     nof_ports,
                                    unsigned                     first_symbol,
                                    unsigned                     nof_symbols,
                                    unsigned                     start_prb,
                                    unsigned                     nof_prbs,
                                    const ru_compression_params& params) override;

  bool compress_device_symbol_batch_to_buffers(span<span<uint8_t>>          buffers,
                                               const resource_grid_reader&  grid,
                                               unsigned                     first_port,
                                               unsigned                     nof_ports,
                                               unsigned                     first_symbol,
                                               unsigned                     nof_symbols,
                                               unsigned                     start_prb,
                                               unsigned                     nof_prbs,
                                               const ru_compression_params& params) override;

  void
  decompress(span<cbf16_t> iq_data, span<const uint8_t> compressed_data, const ru_compression_params& params) override;

  bool supports_resource_grid_decompression() const override { return true; }

  bool prefers_full_symbol_decompression(const ru_compression_params& params) const override
  {
    return params.type == type;
  }

  bool supports_prach_buffer_decompression() const override { return true; }

  iq_decompression_result decompress_to_resource_grid(resource_grid_writer&        grid,
                                                      unsigned                     port,
                                                      unsigned                     symbol,
                                                      unsigned                     start_prb,
                                                      unsigned                     nof_prbs,
                                                      span<const uint8_t>          compressed_data,
                                                      const ru_compression_params& params) override;

  iq_decompression_result decompress_to_prach_buffer(prach_buffer&                buffer,
                                                     unsigned                     port,
                                                     unsigned                     td_occasion,
                                                     unsigned                     fd_occasion,
                                                     unsigned                     symbol,
                                                     unsigned                     start_re,
                                                     unsigned                     input_start_re,
                                                     unsigned                     nof_re,
                                                     unsigned                     nof_prbs,
                                                     span<const uint8_t>          compressed_data,
                                                     const ru_compression_params& params) override;

  /// Returns the number of times the accelerated path failed and the CPU implementation was used instead.
  uint64_t get_nof_acceleration_failures() const { return nof_acceleration_failures.load(std::memory_order_relaxed); }

  /// Returns the number of fresh-handle GPU retries attempted after an accelerated backend failure.
  uint64_t get_nof_acceleration_retries() const { return nof_acceleration_retries.load(std::memory_order_relaxed); }

private:
  ocudu_ofh_compression_handle* acquire_handle();
  void                          release_handle(ocudu_ofh_compression_handle* acquired_handle);
  void                          release_async_handle(ocudu_ofh_compression_handle* acquired_handle);
  void                          discard_handle(ocudu_ofh_compression_handle* failed_handle);
  void                          run_async_handle_reclaimer();

  /// Consumes the one-shot failure injection hook used to exercise fresh-handle recovery.
  bool should_inject_gpu_failure();

  /// Counts a fresh-handle retry and logs the failed CUDA operation at a bounded rate.
  void note_acceleration_retry(const char* operation, const ocudu_ofh_compression_handle* failed_handle, bool injected);

  /// Counts a failure of the accelerated path and logs it at a bounded rate.
  void note_acceleration_failure(const char*                         operation,
                                 const ocudu_ofh_compression_handle* failed_handle = nullptr,
                                 bool                                injected      = false,
                                 const char* recovery_action = "using the CPU implementation for this symbol");

  /// Number of failures between two consecutive log entries. Failures happen once per symbol and eAxC in the worst
  /// case, so logging every one of them would flood the log backend.
  static constexpr uint64_t failure_log_period = 1000;
  /// Initial attempt plus one bounded retry on a fresh CUDA handle.
  static constexpr unsigned max_direct_decompression_attempts = 2;
  /// A pool of 64 covers a full 14-symbol receive burst over four ports, with headroom for independent grids. It is
  /// bounded so exhaustion is reported instead of blocking a real-time path.
  static constexpr unsigned max_nof_handles = 64U;
  /// Polling runs outside the realtime receive executor. This period keeps reclamation comfortably inside the pool
  /// headroom while avoiding continuous CUDA completion queries.
  static constexpr std::chrono::microseconds async_handle_poll_period{50};

  compression_type                           type;
  ocudulog::basic_logger&                    logger;
  float                                      iq_scaling;
  bool                                       force_fallback;
  std::atomic<bool>                          inject_next_gpu_failure;
  std::unique_ptr<iq_compressor>             fallback_compressor;
  std::unique_ptr<iq_decompressor>           fallback_decompressor;
  std::atomic<uint64_t>                      nof_acceleration_retries  = {0};
  std::atomic<uint64_t>                      nof_acceleration_failures = {0};
  std::vector<ocudu_ofh_compression_handle*> all_handles;
  std::vector<ocudu_ofh_compression_handle*> available_handles;
  std::vector<ocudu_ofh_compression_handle*> pending_handles;
  unsigned                                   handle_capacity = max_nof_handles;
  unsigned                                   nof_handles     = 0;
  bool                                       shutting_down   = false;
  std::mutex                                 mutex;
  std::condition_variable                    reclaimer_cv;
  unique_thread                              reclaimer_thread;
};

} // namespace ofh
} // namespace ocudu
