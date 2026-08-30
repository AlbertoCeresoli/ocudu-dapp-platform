// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "circuit_breaker.h"
#include "quiescence_tracker.h"
#include "ocudu/dapp/abi/v1/management.h"
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

namespace ocudu::dapp {

class incident_store;

struct instance_operation_result {
  ocudu_dapp_status_v1          status{OCUDU_DAPP_INTERNAL_ERROR_V1};
  ocudu_dapp_lifecycle_state_v1 state{OCUDU_DAPP_STATE_ERROR_V1};
  std::string                   detail;
  explicit                      operator bool() const noexcept { return status == OCUDU_DAPP_OK_V1; }
};

struct management_call_result {
  instance_operation_result operation;
  uint32_t                  count{};
  uint64_t                  generation{};
  explicit                  operator bool() const noexcept { return static_cast<bool>(operation); }
};

/// Outcome of one Class-B scheduler invocation: the decision was committed,
/// validated but discarded (evaluation mode), replaced by the conventional
/// policy, or rejected by output validation.
enum class class_b_policy_outcome : uint8_t { accepted, evaluation_accepted, fallback, validation_rejection };

struct class_b_policy_measurement {
  uint32_t               direction{};
  uint32_t               nof_candidates{};
  uint32_t               cell_nof_prbs{};
  bool                   interference_context_present{};
  bool                   avoidance_output{};
  uint64_t               conventional_time_ns{};
  uint64_t               feature_time_ns{};
  uint64_t               dapp_time_ns{};
  uint64_t               validation_time_ns{};
  uint64_t               commit_time_ns{};
  uint64_t               total_time_ns{};
  class_b_policy_outcome outcome{class_b_policy_outcome::fallback};
  bool                   deadline_miss{};
};

struct class_b_policy_snapshot {
  uint64_t invocations{};
  uint64_t conventional_time_ns{};
  uint64_t feature_time_ns{};
  uint64_t dapp_time_ns{};
  uint64_t validation_time_ns{};
  uint64_t commit_time_ns{};
  uint64_t total_time_ns{};
  uint64_t accepted{};
  uint64_t evaluation_accepted{};
  uint64_t fallbacks{};
  uint64_t validation_rejections{};
  uint64_t deadline_misses{};

  class_b_policy_snapshot& operator+=(const class_b_policy_snapshot& other) noexcept
  {
    invocations += other.invocations;
    conventional_time_ns += other.conventional_time_ns;
    feature_time_ns += other.feature_time_ns;
    dapp_time_ns += other.dapp_time_ns;
    validation_time_ns += other.validation_time_ns;
    commit_time_ns += other.commit_time_ns;
    total_time_ns += other.total_time_ns;
    accepted += other.accepted;
    evaluation_accepted += other.evaluation_accepted;
    fallbacks += other.fallbacks;
    validation_rejections += other.validation_rejections;
    deadline_misses += other.deadline_misses;
    return *this;
  }
};

/// Host-owned reasons a Class-A invocation fell back to the conventional
/// PHY path (`class_a.fallback.*` counters). Fallback restores conventional
/// authority for that invocation only; it is distinct from breaker-open.
enum class class_a_fallback_reason : uint8_t {
  admission_closed,
  breaker_open,
  overlapped_hook,
  unsupported_metadata,
  unsupported_layers,
  unsupported_rx_ports,
  unsupported_allocation,
  unsupported_modulation,
  unsupported_dmrs,
  unsupported_transform_precoding,
  unsupported_dc_overlap,
  module_bypass,
  module_unsupported,
  invalid_input,
  invalid_output,
  deadline,
  exception,
  device,
  resource,
  returned_error,
  count
};

constexpr std::size_t nof_class_a_fallback_reasons = static_cast<std::size_t>(class_a_fallback_reason::count);

struct class_a_fallback_snapshot {
  std::array<uint64_t, nof_class_a_fallback_reasons> reasons{};

  class_a_fallback_snapshot& operator+=(const class_a_fallback_snapshot& other) noexcept
  {
    for (std::size_t index = 0; index != reasons.size(); ++index) {
      reasons[index] += other.reasons[index];
    }
    return *this;
  }
};

/// Soft CUDA completion-wall accounting (`class_a.completion.deadline_misses`).
/// A miss is an observation against the configured soft wall budget, not a
/// 3GPP slot deadline and not a breaker-open event.
struct class_a_completion_snapshot {
  uint64_t deadline_misses{};

  class_a_completion_snapshot& operator+=(const class_a_completion_snapshot& other) noexcept
  {
    deadline_misses += other.deadline_misses;
    return *this;
  }
};

struct instance_health_snapshot {
  ocudu_dapp_lifecycle_state_v1 state{OCUDU_DAPP_STATE_ERROR_V1};
  breaker_state                 breaker{breaker_state::open};
  uint32_t                      consecutive_failures{};
  quiescence_snapshot           activity;
  uint64_t                      dropped_incidents{};
  uint32_t                      input_cell_bitmap{};
  uint32_t                      native_workers{};
  uint32_t                      active_native_workers{};
  class_a_fallback_snapshot     class_a_fallbacks;
  class_a_completion_snapshot   class_a_completion;
  class_b_policy_snapshot       class_b_policy;
};

/// Placement-neutral management owner of one dApp instance (in-process native
/// or supervised process); no method is called from a dApp hot path. Lifecycle
/// methods mirror the E3 actions: prepare -> arm -> enter_shadow -> activate,
/// then drain (deactivate) and unload.
class instance_runtime
{
public:
  virtual ~instance_runtime() = default;

  /// Module create/prepare with the placement-specific typed configuration.
  virtual instance_operation_result prepare(const void* typed_config, const void* prepare_args) = 0;
  /// Worker readiness with the admitted invocation shapes.
  virtual instance_operation_result arm(const void* admitted_shapes) = 0;
  /// Warm execution without hook authority.
  virtual instance_operation_result enter_shadow() = 0;
  /// Bind hooks; the instance may affect the radio path from here on.
  virtual instance_operation_result activate() = 0;
  /// Remove authority and wait for in-flight work until the deadline.
  virtual instance_operation_result drain(std::chrono::steady_clock::time_point deadline) = 0;
  /// Retry an uncertain native quiescence (e.g. resident GPU work) until the deadline.
  virtual instance_operation_result retry_quiescence(std::chrono::steady_clock::time_point deadline) = 0;
  /// Destroy the instance after quiescence.
  virtual instance_operation_result unload() = 0;
  /// Close every latch owned by this instance (lifecycle + leftover workers).
  virtual instance_operation_result reset_breakers() { return {OCUDU_DAPP_OK_V1, state(), {}}; }

  virtual uint64_t                 management_capabilities() const                                                  = 0;
  virtual instance_health_snapshot health() const                                                                   = 0;
  virtual management_call_result   describe_config(ocudu_dapp_config_descriptor_v1* descriptors, uint32_t capacity) = 0;
  virtual management_call_result   get_config(ocudu_dapp_config_entry_v1* entries, uint32_t capacity)               = 0;
  virtual management_call_result   set_config(const ocudu_dapp_config_update_v1& update)                            = 0;
  virtual management_call_result describe_metrics(ocudu_dapp_metric_descriptor_v1* descriptors, uint32_t capacity)  = 0;
  virtual management_call_result read_metrics(ocudu_dapp_metric_sample_v1* samples, uint32_t capacity)              = 0;
  virtual management_call_result manage_model(const ocudu_dapp_model_request_v1& request)                           = 0;

  virtual ocudu_dapp_lifecycle_state_v1 state() const                                = 0;
  virtual std::size_t                   drain_incidents(incident_store& destination) = 0;
  virtual void
  set_incident_identity(const uint64_t instance_id[2], const uint64_t package_id[2], uint64_t generation) = 0;
};

} // namespace ocudu::dapp
