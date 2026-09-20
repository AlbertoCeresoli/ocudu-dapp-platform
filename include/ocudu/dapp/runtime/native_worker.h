// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "instance_runtime.h"
#include "ocudu/dapp/use_cases/v1/channel_estimator.h"
#include "ocudu/dapp/use_cases/v1/equalizer.h"
#include "ocudu/dapp/use_cases/v1/receiver.h"
#include "ocudu/dapp/use_cases/v1/receiver_completion.h"
#include "ocudu/dapp/use_cases/v1/scheduler.h"
#include "ocudu/dapp/use_cases/v1/spectrum.h"
#include "ocudu/dapp/use_cases/v1/srs_isac.h"
#include <cstdint>
#include <memory>
#include <string>

namespace ocudu::dapp {

class native_instance;
class native_worker;

struct native_worker_create_result {
  ocudu_dapp_status_v1           status{OCUDU_DAPP_INTERNAL_ERROR_V1};
  std::string                    detail;
  std::shared_ptr<native_worker> worker;
  explicit operator bool() const noexcept { return status == OCUDU_DAPP_OK_V1 && worker != nullptr; }
};

struct native_invocation_result {
  ocudu_dapp_status_v1 status{OCUDU_DAPP_INTERNAL_ERROR_V1};
  bool                 invoke_fallback{true};
  explicit             operator bool() const noexcept { return status == OCUDU_DAPP_OK_V1; }
};

/// Host-owned lookup key, meaningful only on the ORIGINAL retained native_worker.
struct native_receiver_ticket {
  uint64_t worker_generation{};
  uint64_t sequence{};
};

/// Number of worker states intentionally retained after unsafe destruction with pending receiver work.
std::size_t quarantined_native_receiver_worker_count() noexcept;

/// One host-owned module context bound to one PHY worker and one interface.
class native_worker
{
public:
  ~native_worker();
  native_worker(const native_worker&)            = delete;
  native_worker& operator=(const native_worker&) = delete;

  uint32_t interface_id() const noexcept;
  uint32_t worker_id() const noexcept;
  uint32_t backend_id() const noexcept;
  bool receiver_completion_required() const noexcept;
  unsigned pending_receiver_completions() const noexcept;

  /// Caller retains this worker, its module and authentic global tensor loans through all consumers.
  /// A nonempty ticket on failure still needs release/retirement; invoke_fallback=false forbids overwriting
  /// a possibly queued output. Empty ticket storage is required on entry. Four fixed outstanding slots.
  native_invocation_result invoke_receiver_deferred(const ocudu_dapp_receiver_input_v1& input,
                                                     ocudu_dapp_receiver_output_v1& output,
                                                     native_receiver_ticket& ticket) noexcept;
  /// Only OK/ready/valid admits the grant. Does not release it. Works after admission closes.
  ocudu_dapp_status_v1 inspect_receiver(const native_receiver_ticket& ticket,
                                        ocudu_dapp_receiver_completion_result_v1& result) noexcept;
  /// Call after every consumer finishes/fences. Success retires this exact ticket, never another worker's.
  ocudu_dapp_status_v1 release_receiver(const native_receiver_ticket& ticket) noexcept;
  /// Control-only, all consumers withdrawn/fenced. Try-based cleanup, including partial/invalid receipts;
  /// backend destruction has no real-time wall-clock guarantee.
  ocudu_dapp_status_v1 retire_receiver() noexcept;

  native_invocation_result invoke(const ocudu_dapp_channel_estimator_input_v1& input,
                                  ocudu_dapp_channel_estimator_output_v1&      output) noexcept;
  native_invocation_result invoke(const ocudu_dapp_equalizer_input_v1& input,
                                  ocudu_dapp_equalizer_output_v1&      output) noexcept;
  native_invocation_result invoke(const ocudu_dapp_receiver_input_v1& input,
                                  ocudu_dapp_receiver_output_v1&      output) noexcept;
  native_invocation_result invoke(const ocudu_dapp_scheduler_input_v1& input,
                                  ocudu_dapp_scheduler_output_v1&      output) noexcept;
  /// Records a host-side validation failure discovered immediately after a successful callback.
  void report_failure(const ocudu_dapp_invocation_v1& invocation, ocudu_dapp_status_v1 status) noexcept;
  /// Records an asynchronous resident result that became visible after its declared deadline.
  void report_completion_deadline(const ocudu_dapp_invocation_v1& invocation,
                                  uint64_t                        observed_latency_ns,
                                  uint64_t                        deadline_ns) noexcept;
  /// Records a host-selected Class-A fallback that occurred before or after a module callback.
  void record_class_a_fallback(class_a_fallback_reason reason) noexcept;
  /// Adds one host-measured Class-B policy outcome to this worker's bounded atomic counters.
  void                     record_class_b_policy(const class_b_policy_measurement& measurement) noexcept;
  native_invocation_result publish(const ocudu_dapp_spectrum_input_v1& input) noexcept;
  native_invocation_result publish(const ocudu_dapp_ul_resource_plan_v1& plan) noexcept;
  native_invocation_result publish(const ocudu_dapp_srs_isac_input_v1& input) noexcept;

private:
  friend class native_instance;
  struct implementation;
  explicit native_worker(std::unique_ptr<implementation> implementation_);
  static native_worker_create_result create(native_instance& owner, uint32_t interface_id, uint32_t worker_id);
  native_invocation_result invoke_receiver_impl(const ocudu_dapp_receiver_input_v1& input,
                                                ocudu_dapp_receiver_output_v1& output,
                                                native_receiver_ticket* ticket) noexcept;

  std::unique_ptr<implementation> state;
};

} // namespace ocudu::dapp
