// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "ocudu/dapp/runtime/circuit_breaker.h"
#include "ocudu/dapp/runtime/incident.h"
#include "ocudu/dapp/runtime/instance_runtime.h"
#include <atomic>

namespace ocudu::dapp {

struct native_worker_incident_source {
  circuit_breaker                                                 breaker;
  incident_ring<64>                                               incidents;
  incident_context                                                identity;
  std::atomic<uint64_t>                                           app_generation{};
  std::atomic<uint32_t>                                           input_cell_bitmap{};
  std::array<std::atomic<uint64_t>, nof_class_a_fallback_reasons> class_a_fallbacks{};
  std::atomic<uint64_t>                                           class_a_completion_deadline_misses{};
  std::atomic<uint64_t>                                           class_b_invocations{};
  std::atomic<uint64_t>                                           class_b_conventional_time_ns{};
  std::atomic<uint64_t>                                           class_b_feature_time_ns{};
  std::atomic<uint64_t>                                           class_b_dapp_time_ns{};
  std::atomic<uint64_t>                                           class_b_validation_time_ns{};
  std::atomic<uint64_t>                                           class_b_commit_time_ns{};
  std::atomic<uint64_t>                                           class_b_total_time_ns{};
  std::atomic<uint64_t>                                           class_b_accepted{};
  std::atomic<uint64_t>                                           class_b_evaluation_accepted{};
  std::atomic<uint64_t>                                           class_b_fallbacks{};
  std::atomic<uint64_t>                                           class_b_validation_rejections{};
  std::atomic<uint64_t>                                           class_b_deadline_misses{};

  incident_context context() const noexcept
  {
    incident_context result = identity;
    result.app_generation   = app_generation.load(std::memory_order_acquire);
    return result;
  }

  void                        record(const class_b_policy_measurement& measurement) noexcept;
  void                        record(class_a_fallback_reason reason) noexcept;
  void                        record_completion_deadline() noexcept;
  void                        observe_cell(uint32_t cell_id) noexcept;
  uint32_t                    observed_cells() const noexcept;
  class_a_fallback_snapshot   class_a_snapshot() const noexcept;
  class_a_completion_snapshot class_a_completion() const noexcept;
  class_b_policy_snapshot     class_b_snapshot() const noexcept;
};

} // namespace ocudu::dapp
