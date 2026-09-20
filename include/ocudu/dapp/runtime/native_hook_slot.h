// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "native_completion_barrier.h"
#include "native_worker.h"
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>

namespace ocudu::dapp {

/// Stable, instance-owned publication gate shared by every interface in one activation transaction.
struct native_activation_gate {
  std::atomic<bool> enabled{};
};

/// One atomically published, pre-bound function-hook target for a PHY producer lane.
class native_hook_slot
{
public:
  explicit native_hook_slot(uint32_t interface_id_) noexcept : required_interface_id(interface_id_) {}
  ~native_hook_slot();
  native_hook_slot(const native_hook_slot&)            = delete;
  native_hook_slot& operator=(const native_hook_slot&) = delete;

  ocudu_dapp_status_v1 bind(std::shared_ptr<native_worker>          worker,
                            std::shared_ptr<native_activation_gate> gate) noexcept;
  /// Unpublishes the target and releases it after all current readers leave.
  /// A deadline failure leaves ownership retained so a later call can finish the grace period safely.
  ocudu_dapp_status_v1 clear(std::chrono::steady_clock::time_point deadline) noexcept;
  bool                 bound() const noexcept { return target.load(std::memory_order_acquire) != nullptr; }
  bool                 active() noexcept;
  bool                 set_completion_barrier(std::shared_ptr<native_completion_barrier> barrier_) noexcept;

  native_invocation_result invoke(const ocudu_dapp_channel_estimator_input_v1& input,
                                  ocudu_dapp_channel_estimator_output_v1&      output) noexcept;
  native_invocation_result invoke(const ocudu_dapp_equalizer_input_v1& input,
                                  ocudu_dapp_equalizer_output_v1&      output) noexcept;
  native_invocation_result invoke(const ocudu_dapp_receiver_input_v1& input,
                                  ocudu_dapp_receiver_output_v1&      output) noexcept;
  /// Retains the exact issuing worker before releasing publication protection.
  /// owner must be empty and remains set for any nonempty ticket, even on an uncertain enqueue error.
  native_invocation_result invoke_receiver_deferred(const ocudu_dapp_receiver_input_v1& input,
                                                     ocudu_dapp_receiver_output_v1& output,
                                                     native_receiver_ticket& ticket,
                                                     std::shared_ptr<native_worker>& owner) noexcept;
  native_invocation_result invoke(const ocudu_dapp_scheduler_input_v1& input,
                                  ocudu_dapp_scheduler_output_v1&      output) noexcept;
  void report_failure(const ocudu_dapp_invocation_v1& invocation, ocudu_dapp_status_v1 status) noexcept;
  void report_completion_deadline(const ocudu_dapp_invocation_v1& invocation,
                                  uint64_t                        observed_latency_ns,
                                  uint64_t                        deadline_ns) noexcept;
  void record_class_a_fallback(class_a_fallback_reason reason) noexcept;
  void record_class_b_policy(const class_b_policy_measurement& measurement) noexcept;
  native_invocation_result publish(const ocudu_dapp_spectrum_input_v1& input) noexcept;
  native_invocation_result publish(const ocudu_dapp_ul_resource_plan_v1& plan) noexcept;
  native_invocation_result publish(const ocudu_dapp_srs_isac_input_v1& input) noexcept;

private:
  struct published_target {
    std::shared_ptr<native_worker>          worker;
    std::shared_ptr<native_activation_gate> gate;
  };

  class reader_guard
  {
  public:
    explicit reader_guard(native_hook_slot& owner_) noexcept;
    ~reader_guard();
    native_worker* get() const noexcept;
    std::shared_ptr<native_worker> retain() const noexcept;

  private:
    native_hook_slot& owner;
    native_worker*    worker;
    published_target* publication{};
  };

  const uint32_t                             required_interface_id;
  std::atomic<published_target*>             target{};
  std::atomic<uint32_t>                      readers{};
  std::mutex                                 control_mutex;
  std::unique_ptr<published_target>          owned_target;
  std::shared_ptr<native_completion_barrier> completion_barrier;
};

} // namespace ocudu::dapp
