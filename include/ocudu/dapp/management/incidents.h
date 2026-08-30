// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "ocudu/dapp/abi/v1/base.h"
#include <array>
#include <cstddef>
#include <cstdint>

namespace ocudu::dapp {

enum class incident_phase : uint8_t {
  query,
  create,
  configure,
  prepare,
  warmup,
  invoke,
  completion,
  report,
  stop,
  quiesce,
  unload
};

enum class incident_cause : uint8_t {
  returned_error,
  standard_exception,
  unknown_exception,
  invalid_input,
  invalid_output,
  deadline_miss,
  device_error,
  resource_exhausted,
  process_exit,
  heartbeat_loss,
  lease_expired,
  quiescence_timeout,
  security_violation,
  policy_rejection
};

enum class incident_action : uint8_t {
  observed,
  invocation_fallback,
  breaker_opened,
  disabled,
  model_rollback,
  quarantined,
  process_restarted,
  du_restart_required
};

struct incident_context {
  uint64_t       instance_id[2]{};
  uint64_t       package_id[2]{};
  uint64_t       app_generation{};
  uint64_t       model_generation{};
  uint64_t       invocation_sequence{};
  uint32_t       hook_id{};
  uint16_t       cell_id{};
  uint16_t       worker_id{};
  incident_phase phase{incident_phase::invoke};
};

/// Fixed-size and pointer-free so it can safely cross a bounded RT incident ring.
struct incident_record {
  static constexpr std::size_t message_capacity = 192;

  uint64_t                           incident_id{};
  uint64_t                           monotonic_ns{};
  incident_context                   context{};
  incident_cause                     cause{incident_cause::returned_error};
  incident_action                    action{incident_action::observed};
  int32_t                            native_status{};
  uint32_t                           recurrence_count{1};
  uint64_t                           exception_type_hash{};
  uint64_t                           observed_latency_ns{};
  uint64_t                           deadline_ns{};
  bool                               fallback_succeeded{};
  bool                               breaker_open{};
  uint16_t                           message_size{};
  std::array<char, message_capacity> sanitized_message{};
};

struct incident_health_summary {
  uint64_t total{};
  uint64_t standard_exceptions{};
  uint64_t unknown_exceptions{};
  uint64_t deadline_misses{};
  uint64_t invalid_inputs{};
  uint64_t invalid_outputs{};
  uint64_t device_errors{};
  uint64_t resource_exhaustions{};
  uint64_t quiescence_timeouts{};
  uint64_t breaker_opened{};
  uint64_t fallbacks{};
  uint64_t quarantined{};
  uint64_t history_evictions{};
  uint64_t source_ring_drops{};
  uint64_t last_incident_id{};
};

} // namespace ocudu::dapp
