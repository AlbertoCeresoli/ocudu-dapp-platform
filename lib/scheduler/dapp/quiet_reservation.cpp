// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/scheduler/dapp/quiet_reservation.h"
#include "ocudu/ran/du_cell_index.h"
#include "ocudu/ran/resource_block.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <limits>

using namespace ocudu;

namespace {

constexpr uint32_t no_active_snapshot    = 3;
constexpr uint32_t maximum_reservations  = quiet_reservation_max_records;
constexpr uint32_t maximum_radio_horizon = 4096;
constexpr uint32_t maximum_duty_samples  = maximum_radio_horizon * 2;

using duty_profile = std::array<uint16_t, maximum_duty_samples>;

bool nonzero(std::array<uint64_t, 2> value) noexcept
{
  return value[0] != 0 || value[1] != 0;
}

bool valid_radio_time(const ocudu_dapp_radio_time_v1& time) noexcept
{
  return time.numerology <= 4 && time.sfn < NOF_SFNS && time.slot < NOF_SUBFRAMES_PER_FRAME * (1U << time.numerology) &&
         time.reserved == 0;
}

slot_point to_slot(const ocudu_dapp_radio_time_v1& time) noexcept
{
  return slot_point{time.numerology, time.sfn, time.slot};
}

ocudu_dapp_radio_time_v1 to_radio_time(slot_point slot) noexcept
{
  return {static_cast<uint16_t>(slot.sfn()),
          static_cast<uint16_t>(slot.slot_index()),
          static_cast<uint16_t>(slot.numerology()),
          0};
}

bool same_source(const ocudu_dapp_quiet_period_reservation_v1& reservation, std::array<uint64_t, 2> source) noexcept
{
  return reservation.request_source_instance_id[0] == source[0] &&
         reservation.request_source_instance_id[1] == source[1];
}

bool same_request_id(const ocudu_dapp_quiet_period_reservation_v1& reservation,
                     const ocudu_dapp_quiet_period_intent_v1&      intent) noexcept
{
  return reservation.request_id[0] == intent.request_id[0] && reservation.request_id[1] == intent.request_id[1];
}

bool same_intent(const ocudu_dapp_quiet_period_intent_v1& left, const ocudu_dapp_quiet_period_intent_v1& right) noexcept
{
  return left.request_id[0] == right.request_id[0] && left.request_id[1] == right.request_id[1] &&
         left.earliest_start.sfn == right.earliest_start.sfn && left.earliest_start.slot == right.earliest_start.slot &&
         left.earliest_start.numerology == right.earliest_start.numerology &&
         left.latest_start.sfn == right.latest_start.sfn && left.latest_start.slot == right.latest_start.slot &&
         left.latest_start.numerology == right.latest_start.numerology && left.duration_slots == right.duration_slots &&
         left.repeat_period_slots == right.repeat_period_slots && left.repeat_count == right.repeat_count &&
         left.first_prb == right.first_prb && left.nof_prbs == right.nof_prbs &&
         left.requested_level == right.requested_level && left.budget_class == right.budget_class &&
         left.confidence_q16 == right.confidence_q16 && left.expiry_monotonic_ns == right.expiry_monotonic_ns;
}

bool prbs_overlap(uint16_t left_start, uint16_t left_count, uint16_t right_start, uint16_t right_count) noexcept
{
  return left_start < static_cast<uint32_t>(right_start) + right_count &&
         right_start < static_cast<uint32_t>(left_start) + left_count;
}

bool slot_ranges_overlap(slot_point left_start,
                         uint16_t   left_duration,
                         slot_point right_start,
                         uint16_t   right_duration) noexcept
{
  return left_start < right_start + right_duration && right_start < left_start + left_duration;
}

bool reservation_applies(const ocudu_dapp_quiet_period_reservation_v1& reservation, slot_point slot) noexcept
{
  if (reservation.start.numerology != slot.numerology()) {
    return false;
  }
  const slot_point start = to_slot(reservation.start);
  for (uint16_t repeat = 0; repeat != reservation.repeat_count; ++repeat) {
    const slot_point occurrence = start + static_cast<uint32_t>(repeat) * reservation.repeat_period_slots;
    if (slot >= occurrence && slot < occurrence + reservation.duration_slots) {
      return true;
    }
  }
  return false;
}

bool reservations_conflict(const ocudu_dapp_quiet_period_reservation_v1& left,
                           slot_point                                    start,
                           uint16_t                                      duration,
                           uint16_t                                      period,
                           uint16_t                                      repeats,
                           uint16_t                                      first_prb,
                           uint16_t                                      nof_prbs) noexcept
{
  if (left.start.numerology != start.numerology() ||
      !prbs_overlap(left.first_prb, left.nof_prbs, first_prb, nof_prbs)) {
    return false;
  }
  const slot_point left_start = to_slot(left.start);
  for (uint16_t lhs = 0; lhs != left.repeat_count; ++lhs) {
    const slot_point left_occurrence = left_start + static_cast<uint32_t>(lhs) * left.repeat_period_slots;
    for (uint16_t rhs = 0; rhs != repeats; ++rhs) {
      const slot_point right_occurrence = start + static_cast<uint32_t>(rhs) * period;
      if (slot_ranges_overlap(left_occurrence, left.duration_slots, right_occurrence, duration)) {
        return true;
      }
    }
  }
  return false;
}

bool add_duty(duty_profile& profile,
              slot_point    observed,
              slot_point    start,
              uint16_t      duration,
              uint16_t      period,
              uint16_t      repeats,
              uint16_t      nof_prbs,
              uint16_t      window,
              uint16_t      horizon) noexcept
{
  const int origin = window - 1;
  for (uint16_t repeat = 0; repeat != repeats; ++repeat) {
    const slot_point occurrence = start + static_cast<uint32_t>(repeat) * period;
    const int        offset     = occurrence - observed;
    for (uint16_t slot = 0; slot != duration; ++slot) {
      const int relative = offset + slot;
      if (relative < -origin || relative >= horizon) {
        continue;
      }
      auto& value = profile[origin + relative];
      if (value > std::numeric_limits<uint16_t>::max() - nof_prbs) {
        return false;
      }
      value += nof_prbs;
    }
  }
  return true;
}

bool duty_is_admissible(const duty_profile&             base,
                        slot_point                      observed,
                        slot_point                      start,
                        uint16_t                        duration,
                        uint16_t                        period,
                        uint16_t                        repeats,
                        uint16_t                        nof_prbs,
                        uint16_t                        cell_width,
                        const quiet_reservation_policy& policy,
                        uint32_t                        maximum_prb_slots_per_window) noexcept
{
  duty_profile candidate = base;
  if (!add_duty(candidate,
                observed,
                start,
                duration,
                period,
                repeats,
                nof_prbs,
                policy.duty_cycle_window_slots,
                policy.maximum_horizon_slots)) {
    return false;
  }
  const uint16_t per_slot_limit = std::min<uint16_t>(policy.maximum_quiet_prbs_per_slot, cell_width - 1);
  const uint32_t sample_count   = policy.duty_cycle_window_slots + policy.maximum_horizon_slots - 1;
  uint32_t       rolling        = 0;
  for (uint32_t index = 0; index != sample_count; ++index) {
    if (candidate[index] > per_slot_limit) {
      return false;
    }
    rolling += candidate[index];
    if (index >= policy.duty_cycle_window_slots) {
      rolling -= candidate[index - policy.duty_cycle_window_slots];
    }
    if (index + 1 >= policy.duty_cycle_window_slots && rolling > maximum_prb_slots_per_window) {
      return false;
    }
  }
  return true;
}

bool policy_is_valid(const quiet_reservation_policy& policy) noexcept
{
  return policy.minimum_lead_slots != 0 && policy.maximum_horizon_slots >= policy.minimum_lead_slots &&
         policy.maximum_horizon_slots <= maximum_radio_horizon && policy.maximum_start_window_slots != 0 &&
         policy.maximum_start_window_slots <= policy.maximum_horizon_slots && policy.maximum_duration_slots != 0 &&
         policy.maximum_repeat_count != 0 &&
         policy.maximum_repeat_count <= OCUDU_DAPP_QUIET_RESERVATION_MAX_REPEAT_V1 &&
         policy.maximum_reserved_prbs != 0 && policy.maximum_reserved_prbs <= MAX_NOF_PRBS &&
         policy.maximum_slot_prb_budget != 0 && policy.maximum_quiet_prbs_per_slot != 0 &&
         policy.maximum_quiet_prbs_per_slot <= MAX_NOF_PRBS && policy.duty_cycle_window_slots != 0 &&
         policy.duty_cycle_window_slots <= maximum_radio_horizon && policy.maximum_quiet_prb_slots_per_window != 0 &&
         policy.maximum_quiet_prb_slots_per_window <=
             static_cast<uint32_t>(policy.maximum_quiet_prbs_per_slot) * policy.duty_cycle_window_slots &&
         policy.maximum_source_quiet_prb_slots_per_window != 0 &&
         policy.maximum_source_quiet_prb_slots_per_window <= policy.maximum_quiet_prb_slots_per_window &&
         policy.maximum_slot_prb_budget >= policy.maximum_quiet_prb_slots_per_window &&
         policy.maximum_residence_ns != 0 && nonzero(policy.policy_owner_id);
}

} // namespace

class quiet_reservation_manager_impl final : public quiet_reservation_manager
{
  struct reservation_entry {
    ocudu_dapp_quiet_period_reservation_v1 reservation{};
    ocudu_dapp_quiet_period_intent_v1      source_intent{};
  };

  struct alignas(64) snapshot {
    std::atomic<uint32_t>                               readers{};
    uint32_t                                            count{};
    uint64_t                                            generation{};
    std::array<reservation_entry, maximum_reservations> entries;
  };

  struct alignas(64) cell_state {
    std::atomic<uint32_t>   active{no_active_snapshot};
    std::atomic_flag        publishing = ATOMIC_FLAG_INIT;
    uint64_t                next_generation{};
    std::array<snapshot, 3> snapshots;
  };

public:
  quiet_reservation_manager_impl(span<const uint16_t> widths, quiet_reservation_policy policy_) : policy(policy_)
  {
    std::copy(widths.begin(), widths.end(), cell_nof_prbs.begin());
    nof_cells = widths.size();
  }

  quiet_reservation_decision submit(const quiet_reservation_request& request) noexcept override
  {
    const auto validation = validate(request);
    if (validation != OCUDU_DAPP_QUIET_REASON_NONE_V1) {
      return rejected(request, quiet_reservation_result::invalid, validation);
    }

    cell_state& cell = cells[request.cell_id];
    if (cell.publishing.test_and_set(std::memory_order_acquire)) {
      return rejected(request, quiet_reservation_result::busy, OCUDU_DAPP_QUIET_REASON_NONE_V1);
    }
    struct flag_guard {
      std::atomic_flag& flag;
      ~flag_guard() { flag.clear(std::memory_order_release); }
    } guard{cell.publishing};

    const uint32_t active   = cell.active.load(std::memory_order_acquire);
    uint32_t       selected = no_active_snapshot;
    for (uint32_t index = 0; index != cell.snapshots.size(); ++index) {
      if (index != active && cell.snapshots[index].readers.load(std::memory_order_acquire) == 0) {
        selected = index;
        break;
      }
    }
    if (selected == no_active_snapshot) {
      return rejected(request, quiet_reservation_result::busy, OCUDU_DAPP_QUIET_REASON_NONE_V1);
    }

    snapshot& destination = cell.snapshots[selected];
    destination.count     = 0;
    if (active != no_active_snapshot) {
      const snapshot& source = cell.snapshots[active];
      for (uint32_t index = 0; index != source.count; ++index) {
        const auto& entry = source.entries[index];
        if (entry.reservation.expiry_monotonic_ns <= request.publish_monotonic_ns ||
            (same_source(entry.reservation, request.source_instance_id) &&
             entry.reservation.request_source_generation != request.source_generation)) {
          continue;
        }
        if (same_source(entry.reservation, request.source_instance_id) &&
            entry.reservation.request_source_generation == request.source_generation &&
            same_request_id(entry.reservation, request.intent)) {
          if (entry.reservation.request_source_sequence == request.source_sequence &&
              same_intent(entry.source_intent, request.intent)) {
            return {entry.reservation.status == OCUDU_DAPP_RESERVATION_CLAMPED_V1 ? quiet_reservation_result::clamped
                                                                                  : quiet_reservation_result::accepted,
                    entry.reservation};
          }
          return rejected(request, quiet_reservation_result::rejected, OCUDU_DAPP_QUIET_REASON_CONFLICT_V1);
        }
        destination.entries[destination.count++] = entry;
      }
    }
    if (destination.count == destination.entries.size()) {
      return rejected(request,
                      quiet_reservation_result::rejected,
                      OCUDU_DAPP_QUIET_REASON_BUDGET_V1,
                      quiet_reservation_constraint::cell_budget);
    }

    const uint16_t width         = cell_nof_prbs[request.cell_id];
    const uint16_t maximum_prbs  = std::min<uint16_t>(policy.maximum_reserved_prbs, width - 1);
    const uint16_t accepted_prbs = std::min(request.intent.nof_prbs, maximum_prbs);
    const uint32_t requested_budget =
        static_cast<uint32_t>(request.intent.duration_slots) * request.intent.repeat_count * accepted_prbs;
    uint32_t used_budget = 0;
    for (uint32_t index = 0; index != destination.count; ++index) {
      const auto& reservation = destination.entries[index].reservation;
      used_budget +=
          static_cast<uint32_t>(reservation.duration_slots) * reservation.repeat_count * reservation.nof_prbs;
    }
    if (requested_budget > policy.maximum_slot_prb_budget ||
        used_budget > policy.maximum_slot_prb_budget - requested_budget) {
      return rejected(request,
                      quiet_reservation_result::rejected,
                      OCUDU_DAPP_QUIET_REASON_BUDGET_V1,
                      quiet_reservation_constraint::cell_budget);
    }

    duty_profile duty{};
    duty_profile source_duty{};
    for (uint32_t index = 0; index != destination.count; ++index) {
      const auto& reservation = destination.entries[index].reservation;
      if (reservation.start.numerology != request.observed_slot.numerology() ||
          !add_duty(duty,
                    request.observed_slot,
                    to_slot(reservation.start),
                    reservation.duration_slots,
                    reservation.repeat_period_slots,
                    reservation.repeat_count,
                    reservation.nof_prbs,
                    policy.duty_cycle_window_slots,
                    policy.maximum_horizon_slots)) {
        return rejected(request,
                        quiet_reservation_result::rejected,
                        OCUDU_DAPP_QUIET_REASON_BUDGET_V1,
                        quiet_reservation_constraint::cell_budget);
      }
      if (same_source(reservation, request.source_instance_id) && !add_duty(source_duty,
                                                                            request.observed_slot,
                                                                            to_slot(reservation.start),
                                                                            reservation.duration_slots,
                                                                            reservation.repeat_period_slots,
                                                                            reservation.repeat_count,
                                                                            reservation.nof_prbs,
                                                                            policy.duty_cycle_window_slots,
                                                                            policy.maximum_horizon_slots)) {
        return rejected(request,
                        quiet_reservation_result::rejected,
                        OCUDU_DAPP_QUIET_REASON_BUDGET_V1,
                        quiet_reservation_constraint::source_budget);
      }
    }

    const slot_point earliest = to_slot(request.intent.earliest_start);
    const slot_point latest   = to_slot(request.intent.latest_start);
    slot_point       selected_start;
    bool             nonconflicting_candidate = false;
    bool             cell_budget_candidate    = false;
    for (slot_point candidate = earliest; candidate <= latest; ++candidate) {
      bool conflict = false;
      for (uint32_t index = 0; index != destination.count && !conflict; ++index) {
        conflict = reservations_conflict(destination.entries[index].reservation,
                                         candidate,
                                         request.intent.duration_slots,
                                         request.intent.repeat_period_slots,
                                         request.intent.repeat_count,
                                         request.intent.first_prb,
                                         accepted_prbs);
      }
      if (!conflict) {
        nonconflicting_candidate = true;
        if (duty_is_admissible(duty,
                               request.observed_slot,
                               candidate,
                               request.intent.duration_slots,
                               request.intent.repeat_period_slots,
                               request.intent.repeat_count,
                               accepted_prbs,
                               width,
                               policy,
                               policy.maximum_quiet_prb_slots_per_window)) {
          cell_budget_candidate = true;
          if (duty_is_admissible(source_duty,
                                 request.observed_slot,
                                 candidate,
                                 request.intent.duration_slots,
                                 request.intent.repeat_period_slots,
                                 request.intent.repeat_count,
                                 accepted_prbs,
                                 width,
                                 policy,
                                 policy.maximum_source_quiet_prb_slots_per_window)) {
            selected_start = candidate;
            break;
          }
        }
      }
    }
    if (!selected_start.valid()) {
      if (!nonconflicting_candidate) {
        return rejected(request, quiet_reservation_result::rejected, OCUDU_DAPP_QUIET_REASON_CONFLICT_V1);
      }
      return rejected(request,
                      quiet_reservation_result::rejected,
                      OCUDU_DAPP_QUIET_REASON_BUDGET_V1,
                      cell_budget_candidate ? quiet_reservation_constraint::source_budget
                                            : quiet_reservation_constraint::cell_budget);
    }

    uint64_t generation = ++cell.next_generation;
    if (generation == 0) {
      generation = ++cell.next_generation;
    }
    const bool clamped =
        accepted_prbs != request.intent.nof_prbs || request.intent.requested_level != OCUDU_DAPP_QUIET_NO_NEW_PUSCH_V1;
    reservation_entry& entry = destination.entries[destination.count++];
    entry.source_intent      = request.intent;
    entry.reservation        = make_reservation(request, selected_start, accepted_prbs, generation, clamped);
    destination.generation   = generation;
    cell.active.store(selected, std::memory_order_release);
    return {clamped ? quiet_reservation_result::clamped : quiet_reservation_result::accepted, entry.reservation};
  }

  quiet_reservation_result invalidate_source(std::array<uint64_t, 2> source_instance_id,
                                             uint64_t                source_generation) noexcept override
  {
    if (!nonzero(source_instance_id)) {
      return quiet_reservation_result::invalid;
    }
    std::array<bool, MAX_NOF_DU_CELLS>     locked{};
    std::array<uint32_t, MAX_NOF_DU_CELLS> selected{};
    selected.fill(no_active_snapshot);

    // Acquire every affected cell before publishing any change. Lifecycle invalidation is therefore all-or-nothing.
    for (uint32_t cell_id = 0; cell_id != nof_cells; ++cell_id) {
      cell_state& cell = cells[cell_id];
      if (cell.publishing.test_and_set(std::memory_order_acquire)) {
        release_publish_locks(locked);
        return quiet_reservation_result::busy;
      }
      locked[cell_id]       = true;
      const uint32_t active = cell.active.load(std::memory_order_acquire);
      if (active == no_active_snapshot) {
        continue;
      }
      for (uint32_t index = 0; index != cell.snapshots.size(); ++index) {
        if (index != active && cell.snapshots[index].readers.load(std::memory_order_acquire) == 0) {
          selected[cell_id] = index;
          break;
        }
      }
      if (selected[cell_id] == no_active_snapshot) {
        release_publish_locks(locked);
        return quiet_reservation_result::busy;
      }
    }

    bool changed = false;
    for (uint32_t cell_id = 0; cell_id != nof_cells; ++cell_id) {
      cell_state&    cell   = cells[cell_id];
      const uint32_t active = cell.active.load(std::memory_order_relaxed);
      if (active == no_active_snapshot) {
        continue;
      }
      const snapshot& source      = cell.snapshots[active];
      snapshot&       destination = cell.snapshots[selected[cell_id]];
      destination.count           = 0;
      for (uint32_t index = 0; index != source.count; ++index) {
        const auto& entry = source.entries[index];
        if (same_source(entry.reservation, source_instance_id) &&
            (source_generation == 0 || entry.reservation.request_source_generation == source_generation)) {
          changed = true;
          continue;
        }
        destination.entries[destination.count++] = entry;
      }
      if (destination.count != source.count) {
        uint64_t generation = ++cell.next_generation;
        if (generation == 0) {
          generation = ++cell.next_generation;
        }
        destination.generation = generation;
        cell.active.store(selected[cell_id], std::memory_order_release);
      }
    }
    release_publish_locks(locked);
    return changed ? quiet_reservation_result::accepted : quiet_reservation_result::rejected;
  }

  quiet_reservation_lookup lookup(uint32_t      cell_id,
                                  slot_point    transmission_slot,
                                  uint64_t      now_monotonic_ns,
                                  span<uint8_t> quiet_prb_mask) noexcept override
  {
    quiet_reservation_lookup result{};
    if (cell_id >= nof_cells || !transmission_slot.valid() || now_monotonic_ns == 0 ||
        quiet_prb_mask.size() != cell_nof_prbs[cell_id]) {
      return result;
    }
    std::fill(quiet_prb_mask.begin(), quiet_prb_mask.end(), uint8_t{0});
    cell_state& cell = cells[cell_id];
    for (unsigned attempt = 0; attempt != 3; ++attempt) {
      const uint32_t index = cell.active.load(std::memory_order_acquire);
      if (index == no_active_snapshot) {
        return result;
      }
      snapshot& source = cell.snapshots[index];
      source.readers.fetch_add(1, std::memory_order_acquire);
      if (cell.active.load(std::memory_order_acquire) != index) {
        source.readers.fetch_sub(1, std::memory_order_release);
        continue;
      }
      result.reservation_generation = source.generation;
      for (uint32_t entry_index = 0; entry_index != source.count; ++entry_index) {
        const auto& reservation = source.entries[entry_index].reservation;
        if (reservation.expiry_monotonic_ns <= now_monotonic_ns ||
            !reservation_applies(reservation, transmission_slot)) {
          continue;
        }
        ++result.reservation_count;
        result.achieved_level = std::max(result.achieved_level, reservation.achieved_level);
        std::fill(quiet_prb_mask.begin() + reservation.first_prb,
                  quiet_prb_mask.begin() + reservation.first_prb + reservation.nof_prbs,
                  uint8_t{1});
      }
      source.readers.fetch_sub(1, std::memory_order_release);
      return result;
    }
    return {};
  }

  quiet_reservation_snapshot query(uint32_t                                     cell_id,
                                   uint64_t                                     now_monotonic_ns,
                                   span<ocudu_dapp_quiet_period_reservation_v1> output) noexcept override
  {
    quiet_reservation_snapshot result{};
    if (cell_id >= nof_cells || now_monotonic_ns == 0 || output.size() < maximum_reservations) {
      return result;
    }
    result.valid_cell = true;
    cell_state& cell  = cells[cell_id];
    for (unsigned attempt = 0; attempt != 3; ++attempt) {
      const uint32_t index = cell.active.load(std::memory_order_acquire);
      if (index == no_active_snapshot) {
        return result;
      }
      snapshot& source = cell.snapshots[index];
      source.readers.fetch_add(1, std::memory_order_acquire);
      if (cell.active.load(std::memory_order_acquire) != index) {
        source.readers.fetch_sub(1, std::memory_order_release);
        continue;
      }
      result.generation = source.generation;
      for (uint32_t entry_index = 0; entry_index != source.count; ++entry_index) {
        const auto& reservation = source.entries[entry_index].reservation;
        if (reservation.expiry_monotonic_ns > now_monotonic_ns) {
          output[result.count++] = reservation;
        }
      }
      source.readers.fetch_sub(1, std::memory_order_release);
      return result;
    }
    result.busy = true;
    return result;
  }

private:
  void release_publish_locks(const std::array<bool, MAX_NOF_DU_CELLS>& locked) noexcept
  {
    for (uint32_t cell_id = 0; cell_id != nof_cells; ++cell_id) {
      if (locked[cell_id]) {
        cells[cell_id].publishing.clear(std::memory_order_release);
      }
    }
  }

  uint16_t validate(const quiet_reservation_request& request) const noexcept
  {
    constexpr uint32_t minimum = offsetof(ocudu_dapp_quiet_period_intent_v1, reserved);
    const auto&        intent  = request.intent;
    if (request.cell_id >= nof_cells || cell_nof_prbs[request.cell_id] <= 1 || !request.observed_slot.valid() ||
        !nonzero(request.source_instance_id) || request.source_generation == 0 || request.source_sequence == 0 ||
        request.publish_monotonic_ns == 0 || intent.struct_size < minimum ||
        intent.abi_major != OCUDU_DAPP_ABI_MAJOR_V1 || (intent.request_id[0] == 0 && intent.request_id[1] == 0) ||
        !valid_radio_time(intent.earliest_start) || !valid_radio_time(intent.latest_start) ||
        intent.earliest_start.numerology != request.observed_slot.numerology() ||
        intent.latest_start.numerology != request.observed_slot.numerology() || intent.duration_slots == 0 ||
        intent.duration_slots > policy.maximum_duration_slots || intent.repeat_count == 0 ||
        intent.repeat_count > policy.maximum_repeat_count || intent.nof_prbs == 0 ||
        static_cast<uint32_t>(intent.first_prb) + intent.nof_prbs > cell_nof_prbs[request.cell_id] ||
        intent.requested_level > OCUDU_DAPP_QUIET_RF_SILENCE_REQUEST_V1 ||
        intent.budget_class > policy.maximum_budget_class || intent.confidence_q16 == 0 ||
        intent.confidence_q16 > (1U << 16U) || intent.expiry_monotonic_ns <= request.publish_monotonic_ns ||
        intent.expiry_monotonic_ns - request.publish_monotonic_ns > policy.maximum_residence_ns ||
        intent.reserved0 != 0 ||
        std::any_of(
            std::begin(intent.reserved), std::end(intent.reserved), [](uint64_t value) { return value != 0; })) {
      return OCUDU_DAPP_QUIET_REASON_INVALID_V1;
    }
    if (intent.repeat_count > 1 &&
        (intent.repeat_period_slots < intent.duration_slots || intent.repeat_period_slots == 0)) {
      return OCUDU_DAPP_QUIET_REASON_INVALID_V1;
    }
    const slot_point earliest  = to_slot(intent.earliest_start);
    const slot_point latest    = to_slot(intent.latest_start);
    const int        lead      = earliest - request.observed_slot;
    const int        last_lead = latest - request.observed_slot;
    if (lead < policy.minimum_lead_slots) {
      return OCUDU_DAPP_QUIET_REASON_INSUFFICIENT_LEAD_V1;
    }
    if (last_lead < lead || last_lead > policy.maximum_horizon_slots ||
        last_lead - lead > policy.maximum_start_window_slots) {
      return OCUDU_DAPP_QUIET_REASON_OUTSIDE_HORIZON_V1;
    }
    const uint32_t last_end = static_cast<uint32_t>(last_lead) +
                              static_cast<uint32_t>(intent.repeat_count - 1) * intent.repeat_period_slots +
                              intent.duration_slots;
    return last_end <= policy.maximum_horizon_slots ? OCUDU_DAPP_QUIET_REASON_NONE_V1
                                                    : OCUDU_DAPP_QUIET_REASON_OUTSIDE_HORIZON_V1;
  }

  quiet_reservation_decision
  rejected(const quiet_reservation_request& request,
           quiet_reservation_result         result,
           uint16_t                         reason,
           quiet_reservation_constraint     constraint = quiet_reservation_constraint::none) const noexcept
  {
    quiet_reservation_decision decision{};
    decision.result                                    = result;
    decision.constraint                                = constraint;
    decision.reservation.struct_size                   = sizeof(decision.reservation);
    decision.reservation.abi_major                     = OCUDU_DAPP_ABI_MAJOR_V1;
    decision.reservation.abi_minor                     = OCUDU_DAPP_ABI_MINOR_V1;
    decision.reservation.request_id[0]                 = request.intent.request_id[0];
    decision.reservation.request_id[1]                 = request.intent.request_id[1];
    decision.reservation.request_source_instance_id[0] = request.source_instance_id[0];
    decision.reservation.request_source_instance_id[1] = request.source_instance_id[1];
    decision.reservation.request_source_generation     = request.source_generation;
    decision.reservation.request_source_sequence       = request.source_sequence;
    decision.reservation.status                        = OCUDU_DAPP_RESERVATION_REJECTED_V1;
    decision.reservation.reason                        = reason;
    return decision;
  }

  ocudu_dapp_quiet_period_reservation_v1 make_reservation(const quiet_reservation_request& request,
                                                          slot_point                       start,
                                                          uint16_t                         nof_prbs,
                                                          uint64_t                         generation,
                                                          bool                             clamped) const noexcept
  {
    ocudu_dapp_quiet_period_reservation_v1 result{};
    result.struct_size                     = sizeof(result);
    result.abi_major                       = OCUDU_DAPP_ABI_MAJOR_V1;
    result.abi_minor                       = OCUDU_DAPP_ABI_MINOR_V1;
    result.context.struct_size             = sizeof(result.context);
    result.context.schema_id               = OCUDU_DAPP_CONTEXT_QUIET_RESERVATION_V1;
    result.context.schema_version          = 1;
    result.context.producer_instance_id[0] = policy.policy_owner_id[0];
    result.context.producer_instance_id[1] = policy.policy_owner_id[1];
    result.context.producer_generation     = generation;
    result.context.source_sequence         = generation;
    result.context.cell_id                 = request.cell_id;
    result.context.confidence_q16          = request.intent.confidence_q16;
    result.context.created                 = to_radio_time(request.observed_slot);
    result.context.valid_from              = to_radio_time(start);
    const uint32_t last_offset =
        static_cast<uint32_t>(request.intent.repeat_count - 1) * request.intent.repeat_period_slots +
        request.intent.duration_slots - 1;
    result.context.valid_until           = to_radio_time(start + last_offset);
    result.request_id[0]                 = request.intent.request_id[0];
    result.request_id[1]                 = request.intent.request_id[1];
    result.reservation_id[0]             = (static_cast<uint64_t>(request.cell_id) + 1) << 32U | generation;
    result.reservation_id[1]             = policy.policy_owner_id[0] ^ request.intent.request_id[0] ^ generation;
    result.reservation_generation        = generation;
    result.request_source_instance_id[0] = request.source_instance_id[0];
    result.request_source_instance_id[1] = request.source_instance_id[1];
    result.request_source_generation     = request.source_generation;
    result.request_source_sequence       = request.source_sequence;
    result.start                         = to_radio_time(start);
    result.duration_slots                = request.intent.duration_slots;
    result.repeat_period_slots           = request.intent.repeat_period_slots;
    result.repeat_count                  = request.intent.repeat_count;
    result.first_prb                     = request.intent.first_prb;
    result.nof_prbs                      = nof_prbs;
    result.achieved_level                = OCUDU_DAPP_QUIET_NO_NEW_PUSCH_V1;
    result.status              = clamped ? OCUDU_DAPP_RESERVATION_CLAMPED_V1 : OCUDU_DAPP_RESERVATION_ACCEPTED_V1;
    result.reason              = clamped ? OCUDU_DAPP_QUIET_REASON_CAPABILITY_V1 : OCUDU_DAPP_QUIET_REASON_NONE_V1;
    result.budget_class        = request.intent.budget_class;
    result.confidence_q16      = request.intent.confidence_q16;
    result.expiry_monotonic_ns = request.intent.expiry_monotonic_ns;
    return result;
  }

  quiet_reservation_policy                 policy;
  uint32_t                                 nof_cells{};
  std::array<uint16_t, MAX_NOF_DU_CELLS>   cell_nof_prbs{};
  std::array<cell_state, MAX_NOF_DU_CELLS> cells;
};

std::shared_ptr<quiet_reservation_manager>
ocudu::create_quiet_reservation_manager(span<const uint16_t> cell_nof_prbs, quiet_reservation_policy policy) noexcept
{
  if (cell_nof_prbs.empty() || cell_nof_prbs.size() > MAX_NOF_DU_CELLS || !policy_is_valid(policy) ||
      std::any_of(cell_nof_prbs.begin(),
                  cell_nof_prbs.end(),
                  [](uint16_t value) { return value == 1 || value > MAX_NOF_PRBS; }) ||
      std::none_of(cell_nof_prbs.begin(), cell_nof_prbs.end(), [](uint16_t value) { return value > 1; })) {
    return {};
  }
  try {
    return std::make_shared<quiet_reservation_manager_impl>(cell_nof_prbs, policy);
  } catch (...) {
    return {};
  }
}
