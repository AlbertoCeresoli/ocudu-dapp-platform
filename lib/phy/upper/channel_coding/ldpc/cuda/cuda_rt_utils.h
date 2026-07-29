// SPDX-FileCopyrightText: Copyright (C) 2021-2026 DeepSig Inc
// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#pragma once

#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cuda_runtime.h>
#include <sched.h>

namespace ocudu {

enum class upper_phy_cuda_stream_priority_mode { normal, high, low };

inline upper_phy_cuda_stream_priority_mode parse_upper_phy_cuda_stream_priority(const char* value)
{
  if ((value == nullptr) || (*value == '\0')) {
    return upper_phy_cuda_stream_priority_mode::normal;
  }

  if ((std::strcmp(value, "1") == 0) || (std::strcmp(value, "true") == 0) || (std::strcmp(value, "on") == 0) ||
      (std::strcmp(value, "yes") == 0) || (std::strcmp(value, "high") == 0) || (std::strcmp(value, "realtime") == 0)) {
    return upper_phy_cuda_stream_priority_mode::high;
  }

  if ((std::strcmp(value, "low") == 0) || (std::strcmp(value, "lowest") == 0) ||
      (std::strcmp(value, "background") == 0)) {
    return upper_phy_cuda_stream_priority_mode::low;
  }

  return upper_phy_cuda_stream_priority_mode::normal;
}

inline upper_phy_cuda_stream_priority_mode get_upper_phy_cuda_stream_priority(
    const char*                         role_env_name = nullptr,
    upper_phy_cuda_stream_priority_mode default_mode  = upper_phy_cuda_stream_priority_mode::normal)
{
  const char* value = (role_env_name != nullptr) ? std::getenv(role_env_name) : nullptr;
  if ((value != nullptr) && (*value != '\0')) {
    return parse_upper_phy_cuda_stream_priority(value);
  }

  value = std::getenv("OCUDU_UPPER_PHY_CUDA_STREAM_PRIORITY");
  if ((value != nullptr) && (*value != '\0')) {
    return parse_upper_phy_cuda_stream_priority(value);
  }

  value = std::getenv("OCUDU_UPPER_PHY_CUDA_HIGH_PRIORITY");
  if ((value == nullptr) || (*value == '\0')) {
    return default_mode;
  }

  return parse_upper_phy_cuda_stream_priority(value);
}

inline cudaError_t
cudaStreamCreateUpperPhy(cudaStream_t*                       stream,
                         const char*                         role_env_name = nullptr,
                         upper_phy_cuda_stream_priority_mode default_mode = upper_phy_cuda_stream_priority_mode::normal)
{
  const upper_phy_cuda_stream_priority_mode priority_mode =
      get_upper_phy_cuda_stream_priority(role_env_name, default_mode);
  if (priority_mode == upper_phy_cuda_stream_priority_mode::normal) {
    return cudaStreamCreateWithFlags(stream, cudaStreamNonBlocking);
  }

  int         least_priority    = 0;
  int         greatest_priority = 0;
  cudaError_t status            = cudaDeviceGetStreamPriorityRange(&least_priority, &greatest_priority);
  if (status == cudaSuccess) {
    const int requested_priority =
        (priority_mode == upper_phy_cuda_stream_priority_mode::high) ? greatest_priority : least_priority;
    status = cudaStreamCreateWithPriority(stream, cudaStreamNonBlocking, requested_priority);
    if (status == cudaSuccess) {
      return cudaSuccess;
    }
    (void)cudaGetLastError();
  }

  return cudaStreamCreateWithFlags(stream, cudaStreamNonBlocking);
}

/// \brief RT-friendly CUDA stream synchronization.
///
/// This function synchronizes on a CUDA stream while yielding the CPU to other threads.
/// When the calling thread has real-time (SCHED_FIFO/SCHED_RR) priority, a blocking
/// cudaStreamSynchronize() can cause priority inversion by preventing GPU driver threads
/// (which run at normal priority) from making progress.
///
/// This function uses cudaStreamQuery() in a loop with sched_yield() to allow other
/// threads to run while waiting for the GPU to complete.
///
/// \param stream The CUDA stream to synchronize on.
/// \return cudaSuccess on success, or an error code if the stream had an error.
/// Experiment knob: OCUDU_CUDA_SYNC_BLOCKING=1 switches the yielding query loops to
/// plain blocking cudaStreamSynchronize/cudaEventSynchronize. On integrated GPUs the
/// ~1 MHz cudaEventQuery polling can starve driver-side dispatch of queued stream work.
inline bool cuda_sync_blocking_enabled()
{
  static const bool enabled = []() {
    const char* v = std::getenv("OCUDU_CUDA_SYNC_BLOCKING");
    return (v != nullptr) && (v[0] != '0');
  }();
  return enabled;
}

/// Experiment knob: OCUDU_CUDA_SYNC_SLEEP_US=N throttles the query loops to one
/// cudaStreamQuery/cudaEventQuery every N microseconds (0 = poll continuously with
/// sched_yield only). Throttling keeps host wake-up latency in the low microseconds
/// while freeing the driver to dispatch queued stream work between polls.
inline unsigned cuda_sync_sleep_us()
{
  static const unsigned sleep_us = []() -> unsigned {
    const char* v = std::getenv("OCUDU_CUDA_SYNC_SLEEP_US");
    if (v == nullptr || *v == '\0') {
      return 0;
    }
    return static_cast<unsigned>(std::strtoul(v, nullptr, 10));
  }();
  return sleep_us;
}

/// Experiment knob: OCUDU_CUDA_SYNC_SPIN_US=N busy-spins ~N microseconds between
/// queries (sched_yield only, no CUDA calls, no timers). Unlike nanosleep this is not
/// subject to timer slack (~50us for non-RT threads), so the wake latency stays ~N us
/// while the driver is left alone to dispatch queued stream work between polls.
inline unsigned cuda_sync_spin_us()
{
  static const unsigned spin_us = []() -> unsigned {
    const char* v = std::getenv("OCUDU_CUDA_SYNC_SPIN_US");
    if (v == nullptr || *v == '\0') {
      return 0;
    }
    return static_cast<unsigned>(std::strtoul(v, nullptr, 10));
  }();
  return spin_us;
}

inline void cuda_sync_poll_pause()
{
  const unsigned spin_us = cuda_sync_spin_us();
  if (spin_us != 0) {
    timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    const long budget_ns = static_cast<long>(spin_us) * 1000L;
    for (;;) {
      sched_yield();
      timespec t1;
      clock_gettime(CLOCK_MONOTONIC, &t1);
      long elapsed_ns = (t1.tv_sec - t0.tv_sec) * 1000000000L + (t1.tv_nsec - t0.tv_nsec);
      if (elapsed_ns >= budget_ns) {
        return;
      }
    }
  }
  const unsigned sleep_us = cuda_sync_sleep_us();
  if (sleep_us == 0) {
    sched_yield();
    return;
  }
  timespec ts = {0, static_cast<long>(sleep_us) * 1000L};
  nanosleep(&ts, nullptr);
}

inline cudaError_t cudaStreamSynchronizeYielding(cudaStream_t stream)
{
  if (cuda_sync_blocking_enabled()) {
    return cudaStreamSynchronize(stream);
  }
  cudaError_t err;
  while ((err = cudaStreamQuery(stream)) == cudaErrorNotReady) {
    cuda_sync_poll_pause();
  }
  return err;
}

/// \brief RT-friendly CUDA event synchronization.
///
/// This function synchronizes on a CUDA event while yielding the CPU to other threads.
/// Similar to cudaStreamSynchronizeYielding, this avoids priority inversion by polling
/// the event status and yielding between checks.
///
/// \param event The CUDA event to synchronize on.
/// \return cudaSuccess on success, or an error code if the event had an error.
inline cudaError_t cudaEventSynchronizeYielding(cudaEvent_t event)
{
  if (cuda_sync_blocking_enabled()) {
    return cudaEventSynchronize(event);
  }
  cudaError_t err;
  while ((err = cudaEventQuery(event)) == cudaErrorNotReady) {
    cuda_sync_poll_pause();
  }
  return err;
}

} // namespace ocudu
