// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "ocudu/adt/span.h"
#include "ocudu/dapp/use_cases/v1/coordination.h"
#include "ocudu/ran/slot_point.h"
#include <array>
#include <cstdint>
#include <memory>

namespace ocudu {

enum class quiet_reservation_result : uint8_t { accepted, clamped, rejected, invalid, busy };

/// Internal policy detail for a v1 rejection. The wire-level reason remains the stable, coarser quiet reason.
enum class quiet_reservation_constraint : uint8_t { none, cell_budget, source_budget };

struct quiet_reservation_policy {
  uint16_t minimum_lead_slots{4};
  uint16_t maximum_horizon_slots{1024};
  uint16_t maximum_start_window_slots{64};
  uint16_t maximum_duration_slots{8};
  uint16_t maximum_repeat_count{4};
  uint16_t maximum_reserved_prbs{16};
  uint16_t maximum_budget_class{3};
  /// Total retained PRB-slot volume across all live decisions in one cell.
  uint32_t maximum_slot_prb_budget{512};
  /// Exact per-slot and rolling-window admission guardrails.
  uint16_t maximum_quiet_prbs_per_slot{16};
  uint16_t duty_cycle_window_slots{100};
  uint32_t maximum_quiet_prb_slots_per_window{64};
  /// Per-cell rolling ceiling for reservations attributed to one authenticated dApp instance.
  uint32_t                maximum_source_quiet_prb_slots_per_window{64};
  uint64_t                maximum_residence_ns{1'000'000'000};
  std::array<uint64_t, 2> policy_owner_id{UINT64_C(0x4f43554455000001), UINT64_C(1)};
};

struct quiet_reservation_request {
  ocudu_dapp_quiet_period_intent_v1 intent;
  std::array<uint64_t, 2>           source_instance_id{};
  uint64_t                          source_generation{};
  uint64_t                          source_sequence{};
  uint32_t                          cell_id{};
  slot_point                        observed_slot;
  uint64_t                          publish_monotonic_ns{};
};

struct quiet_reservation_decision {
  quiet_reservation_result               result{quiet_reservation_result::invalid};
  ocudu_dapp_quiet_period_reservation_v1 reservation{};
  quiet_reservation_constraint           constraint{quiet_reservation_constraint::none};
};

struct quiet_reservation_lookup {
  uint64_t reservation_generation{};
  uint32_t reservation_count{};
  uint8_t  achieved_level{OCUDU_DAPP_QUIET_NO_NEW_PUSCH_V1};
};

constexpr uint32_t quiet_reservation_max_records = 32;

struct quiet_reservation_snapshot {
  uint64_t generation{};
  uint32_t count{};
  bool     valid_cell{};
  bool     busy{};
};

/// Host-owned bounded reservation authority. Writers never wait for active readers.
class quiet_reservation_manager
{
public:
  virtual ~quiet_reservation_manager() = default;

  virtual quiet_reservation_decision submit(const quiet_reservation_request& request) noexcept = 0;
  /// Removes one generation, or every generation when source_generation is zero.
  virtual quiet_reservation_result invalidate_source(std::array<uint64_t, 2> source_instance_id,
                                                     uint64_t                source_generation) noexcept = 0;

  /// Writes the union of authoritative quiet PRBs for one transmission slot into caller-owned storage.
  virtual quiet_reservation_lookup lookup(uint32_t      cell_id,
                                          slot_point    transmission_slot,
                                          uint64_t      now_monotonic_ns,
                                          span<uint8_t> quiet_prb_mask) noexcept = 0;

  /// Copies retained, unexpired authoritative decisions into caller-owned storage.
  /// Output must hold quiet_reservation_max_records entries. The query never waits and reports contention as busy.
  virtual quiet_reservation_snapshot
  query(uint32_t cell_id, uint64_t now_monotonic_ns, span<ocudu_dapp_quiet_period_reservation_v1> output) noexcept = 0;
};

std::shared_ptr<quiet_reservation_manager>
create_quiet_reservation_manager(span<const uint16_t> cell_nof_prbs, quiet_reservation_policy policy = {}) noexcept;

} // namespace ocudu
