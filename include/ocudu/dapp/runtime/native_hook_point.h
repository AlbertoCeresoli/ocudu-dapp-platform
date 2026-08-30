// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "native_hook_slot.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace ocudu::dapp {

class native_instance;
class native_hook_point;

struct native_hook_point_create_result {
  ocudu_dapp_status_v1               status{OCUDU_DAPP_INTERNAL_ERROR_V1};
  std::string                        detail;
  std::unique_ptr<native_hook_point> point;
  explicit operator bool() const noexcept { return status == OCUDU_DAPP_OK_V1 && point != nullptr; }
};

/// A fixed set of per-PHY-lane native hook slots enabled as one logical hook point.
class native_hook_point
{
public:
  static native_hook_point_create_result
  create(uint32_t interface_id, uint32_t first_worker_id, uint32_t nof_lanes) noexcept;

  ~native_hook_point();
  native_hook_point(const native_hook_point&)            = delete;
  native_hook_point& operator=(const native_hook_point&) = delete;

  ocudu_dapp_status_v1 activate(native_instance& instance);
  ocudu_dapp_status_v1 deactivate(std::chrono::steady_clock::time_point deadline);

  /// Stages every worker without making the point callable. The gate must be unique to this instance.
  ocudu_dapp_status_v1 stage(native_instance& instance, std::shared_ptr<native_activation_gate> gate);
  /// Clears a staged or active point after the shared gate has been disabled.
  ocudu_dapp_status_v1 clear(const std::shared_ptr<native_activation_gate>& gate,
                             std::chrono::steady_clock::time_point          deadline);

  bool     active() const noexcept;
  bool     bound() const noexcept { return is_bound.load(std::memory_order_acquire); }
  uint32_t interface_id() const noexcept { return required_interface_id; }
  uint32_t lane_count() const noexcept { return static_cast<uint32_t>(lanes.size()); }
  uint32_t worker_id(uint32_t lane) const noexcept { return first_worker + lane; }
  uint32_t backend_id() const noexcept { return selected_backend.load(std::memory_order_acquire); }
  /// Bitmask of ocudu_dapp_backend_v1 values this hook's L1 data path may drive.
  /// Zero means unrestricted (tests / unset topology). Non-zero → stage() rejects
  /// packages whose selected backend is outside the mask.
  uint32_t allowed_backend_mask() const noexcept { return allowed_backends.load(std::memory_order_acquire); }
  void     set_allowed_backend_mask(uint32_t mask) noexcept
  {
    allowed_backends.store(mask, std::memory_order_release);
  }
  bool set_completion_barrier(uint32_t lane, std::shared_ptr<native_completion_barrier> barrier) noexcept;

  native_invocation_result invoke(uint32_t                                     lane,
                                  const ocudu_dapp_channel_estimator_input_v1& input,
                                  ocudu_dapp_channel_estimator_output_v1&      output) noexcept;
  native_invocation_result
  invoke(uint32_t lane, const ocudu_dapp_equalizer_input_v1& input, ocudu_dapp_equalizer_output_v1& output) noexcept;
  native_invocation_result
  invoke(uint32_t lane, const ocudu_dapp_receiver_input_v1& input, ocudu_dapp_receiver_output_v1& output) noexcept;
  native_invocation_result
  invoke(uint32_t lane, const ocudu_dapp_scheduler_input_v1& input, ocudu_dapp_scheduler_output_v1& output) noexcept;
  void report_failure(uint32_t lane, const ocudu_dapp_invocation_v1& invocation, ocudu_dapp_status_v1 status) noexcept;
  void report_completion_deadline(uint32_t                        lane,
                                  const ocudu_dapp_invocation_v1& invocation,
                                  uint64_t                        observed_latency_ns,
                                  uint64_t                        deadline_ns) noexcept;
  void record_class_a_fallback(uint32_t lane, class_a_fallback_reason reason) noexcept;
  void record_class_b_policy(uint32_t lane, const class_b_policy_measurement& measurement) noexcept;
  native_invocation_result publish(uint32_t lane, const ocudu_dapp_spectrum_input_v1& input) noexcept;
  native_invocation_result publish(uint32_t lane, const ocudu_dapp_ul_resource_plan_v1& plan) noexcept;
  native_invocation_result publish(uint32_t lane, const ocudu_dapp_srs_isac_input_v1& input) noexcept;

private:
  native_hook_point(uint32_t interface_id_, uint32_t first_worker_id_, uint32_t nof_lanes);
  ocudu_dapp_status_v1 stage_locked(native_instance& instance, std::shared_ptr<native_activation_gate> gate);
  ocudu_dapp_status_v1 clear_locked(const std::shared_ptr<native_activation_gate>& gate,
                                    std::chrono::steady_clock::time_point          deadline);

  template <typename Callable>
  native_invocation_result dispatch(uint32_t lane, Callable&& callable) noexcept
  {
    if (lane >= lanes.size()) {
      return {OCUDU_DAPP_INVALID_INPUT_V1, true};
    }
    return callable(*lanes[lane]);
  }

  const uint32_t                                 required_interface_id;
  const uint32_t                                 first_worker;
  std::vector<std::unique_ptr<native_hook_slot>> lanes;
  std::shared_ptr<native_activation_gate>        activation;
  std::atomic<bool>                              is_bound{};
  std::atomic<uint32_t>                          selected_backend{OCUDU_DAPP_BACKEND_HOST_V1};
  /// 0 = unrestricted; else (1u << OCUDU_DAPP_BACKEND_*) bits allowed for bind.
  std::atomic<uint32_t>                          allowed_backends{0};
  std::mutex                                     control_mutex;
  bool                                           draining{};
};

} // namespace ocudu::dapp
