// SPDX-FileCopyrightText: Copyright (C) 2021-2026 DeepSig Inc
// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#pragma once

#include "prach_detector_geometry.h"
#include "ocudu/phy/upper/channel_processors/prach/prach_detector.h"
#include "ocudu/phy/upper/channel_processors/prach/prach_generator.h"
#include <atomic>
#include <chrono>
#include <complex>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <prach_detector.h>
#include <thread>
#include <vector>

namespace ocudu {

/// \brief PRACH detector backed by the CUDA accelerator, falling back to a host detector.
///
/// \section warmup Plan warm-up
///
/// The backend builds its correlation IDFT plan for a given workload shape the first time it sees that shape, and
/// building it compiles a kernel with NVRTC - tens of milliseconds. The PRACH executor shares its workers with the
/// slot chain, so paying that inside \ref detect() stalls the cell: every queued request behind it is flushed late.
///
/// Instead, the first detection of a shape, the backend is not yet prepared for, is served by the host fallback and
/// the plan is built on a helper thread. The fallback is the detector the non-accelerated build uses for every
/// occasion, so it cannot miss a deadline, and by the next occasion of that shape the accelerated path is ready.
/// Taking the shape from the live configuration is what keeps the warm-up honest: it is exactly the workload the
/// cell asks for, including formats and preamble ranges that only appear once the cell is running.
class prach_detector_cuda_impl : public prach_detector
{
public:
  prach_detector_cuda_impl(std::unique_ptr<prach_generator> generator_,
                           std::unique_ptr<prach_detector>  fallback_,
                           bool                             force_gpu_path_,
                           int                              cuda_device_id_ = 0);

  ~prach_detector_cuda_impl() override;

  prach_detector_cuda_impl(const prach_detector_cuda_impl&)            = delete;
  prach_detector_cuda_impl& operator=(const prach_detector_cuda_impl&) = delete;

  prach_detection_result detect(const prach_buffer& input, const configuration& config) override;

  bool is_gpu_available() const { return gpu_available; }

  /// \brief Blocks until no plan build is ready or until \c timeout elapses.
  ///
  /// Lets a caller, that is allowed, to block -- a test, or a startup hook -- wait for
  /// the accelerated path to become available after an occasion was served by the fallback.
  /// Never call it from the slot path; see \ref warmup.
  ///
  /// \return True if no plan build is in flight on return, false on timeout.
  bool wait_for_warmup(std::chrono::milliseconds timeout = std::chrono::seconds(30));

  /// \brief Returns true when the backend is prepared for \c config, so a detection for it runs accelerated.
  ///
  /// The backend holds one plan, so this answers for one workload shape at a time. See \ref warmup.
  bool is_warm_for(const configuration& config) const;

private:
  using detector_geometry = detail::prach_detector_geometry;

  bool should_use_gpu(const prach_buffer& input, const configuration& config, const detector_geometry& geometry) const;

  prach_detection_result
  detect_gpu(const prach_buffer& input, const configuration& config, const detector_geometry& geometry);

  /// Fills the backend configuration for a geometry. Input placement and strides are left to the caller.
  prach_detector_config_t make_cuda_config(const configuration& config, const detector_geometry& geometry) const;

  void prepare_roots(const configuration& config, const detector_geometry& geometry);

  uint64_t make_root_cache_key(const configuration& config, const detector_geometry& geometry) const;

  /// \brief Identifies the backend IDFT plan a geometry needs.
  ///
  /// The backend rebuilds the plan whenever the IDFT size or the batch count changes, so those two are its whole
  /// identity. Never zero, which stands for "no plan prepared yet".
  static uint64_t make_plan_key(const configuration& config, const detector_geometry& geometry);

  /// \brief Asks the helper thread to prepare the backend for \c config.
  ///
  /// \return True if the request was taken, false if one is already in flight or the shape was prepared before.
  bool request_warmup(const configuration& config, uint64_t plan_key);

  /// Runs one detection on zeroed input so the plan, device buffers, kernels and graph exist for \c config.
  void build_plan(const configuration& config);

  /// Body of the helper thread: prepares requested shapes until the detector is destroyed.
  void warmup_worker();

  std::unique_ptr<prach_generator> generator;
  std::unique_ptr<prach_detector>  fallback;
  bool                             force_gpu_path = false;
  bool                             gpu_available  = false;
  int                              device_id      = 0;
  void*                            stream         = nullptr;
  prach_detector_handle_t          handle         = nullptr;
  std::mutex                       detector_mutex;
  std::vector<uint32_t>            host_prach_packed;
  std::vector<std::complex<float>> host_roots;
  uint64_t                         cached_root_key = 0;

  /// \brief Plan key the backend is prepared for, or zero.
  ///
  /// Read on the slot path before every accelerated detection, so it is deliberately readable without a lock.
  std::atomic<uint64_t> warm_plan_key{0};
  /// Set when the helper thread is disabled and the slot path prepares the backend itself, as it did before.
  bool synchronous_warmup = false;

  /// Guards everything below, and is never held across a plan build.
  std::mutex warmup_mutex;
  /// Set from the moment a request is taken until the helper thread has finished with it.
  bool warmup_pending = false;
  /// Set by the destructor to retire the helper thread.
  bool warmup_stop = false;
  /// Configuration the helper thread prepares for.
  configuration warmup_config = {};
  /// Plan keys already requested once; see \ref request_warmup.
  std::vector<uint64_t> warmed_plan_keys;
  /// Created on the first request and joined by the destructor.
  std::thread             warmup_thread;
  std::condition_variable warmup_cv;
};

/// Returns true when the CUDA PRACH detector backend can be instantiated.
bool is_prach_detector_cuda_available();

} // namespace ocudu
