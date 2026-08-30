// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/scheduler/dapp/quiet_reservation.h"
#include <array>
#include <atomic>
#include <gtest/gtest.h>
#include <thread>

using namespace ocudu;

namespace {

ocudu_dapp_radio_time_v1 to_radio_time(slot_point slot) noexcept
{
  return {static_cast<uint16_t>(slot.sfn()),
          static_cast<uint16_t>(slot.slot_index()),
          static_cast<uint16_t>(slot.numerology()),
          0};
}

slot_point from_radio_time(const ocudu_dapp_radio_time_v1& time) noexcept
{
  return {time.numerology, time.sfn, time.slot};
}

quiet_reservation_request make_request(slot_point base,
                                       uint64_t   request_id = 1,
                                       uint32_t   cell_id    = 0,
                                       uint64_t   generation = 1,
                                       uint64_t   sequence   = 1)
{
  quiet_reservation_request request{};
  request.intent.struct_size         = sizeof(request.intent);
  request.intent.abi_major           = OCUDU_DAPP_ABI_MAJOR_V1;
  request.intent.abi_minor           = OCUDU_DAPP_ABI_MINOR_V1;
  request.intent.request_id[0]       = request_id;
  request.intent.earliest_start      = to_radio_time(base + 4);
  request.intent.latest_start        = to_radio_time(base + 6);
  request.intent.duration_slots      = 2;
  request.intent.repeat_period_slots = 4;
  request.intent.repeat_count        = 2;
  request.intent.first_prb           = 5;
  request.intent.nof_prbs            = 4;
  request.intent.requested_level     = OCUDU_DAPP_QUIET_NO_NEW_PUSCH_V1;
  request.intent.budget_class        = 1;
  request.intent.confidence_q16      = 1U << 15U;
  request.intent.expiry_monotonic_ns = 10000;
  request.source_instance_id         = {17, 23};
  request.source_generation          = generation;
  request.source_sequence            = sequence;
  request.cell_id                    = cell_id;
  request.observed_slot              = base;
  request.publish_monotonic_ns       = 1000;
  return request;
}

} // namespace

TEST(quiet_reservation, rejects_invalid_factory_inputs)
{
  std::array<uint16_t, 1> widths{52};
  auto                    policy = quiet_reservation_policy{};
  policy.minimum_lead_slots      = 0;
  EXPECT_EQ(create_quiet_reservation_manager(widths, policy), nullptr);

  policy                         = quiet_reservation_policy{};
  policy.duty_cycle_window_slots = 0;
  EXPECT_EQ(create_quiet_reservation_manager(widths, policy), nullptr);

  policy                                    = quiet_reservation_policy{};
  policy.maximum_quiet_prb_slots_per_window = policy.maximum_quiet_prbs_per_slot * policy.duty_cycle_window_slots + 1;
  EXPECT_EQ(create_quiet_reservation_manager(widths, policy), nullptr);

  policy                                           = quiet_reservation_policy{};
  policy.maximum_source_quiet_prb_slots_per_window = policy.maximum_quiet_prb_slots_per_window + 1;
  EXPECT_EQ(create_quiet_reservation_manager(widths, policy), nullptr);

  widths[0] = 1;
  EXPECT_EQ(create_quiet_reservation_manager(widths), nullptr);
  EXPECT_EQ(create_quiet_reservation_manager({}), nullptr);
}

TEST(quiet_reservation, accepts_replays_and_looks_up_repeated_occurrences)
{
  const slot_point        base{0, 1023, 7};
  std::array<uint16_t, 1> widths{52};
  auto                    manager = create_quiet_reservation_manager(widths);
  ASSERT_NE(manager, nullptr);

  const auto request = make_request(base);
  const auto first   = manager->submit(request);
  ASSERT_EQ(first.result, quiet_reservation_result::accepted);
  EXPECT_EQ(from_radio_time(first.reservation.start), base + 4);
  EXPECT_EQ(manager->submit(request).reservation.reservation_generation, first.reservation.reservation_generation);

  std::array<uint8_t, 52> mask{};
  auto                    lookup = manager->lookup(0, base + 4, 1001, mask);
  EXPECT_EQ(lookup.reservation_count, 1U);
  EXPECT_EQ(mask[4], 0);
  EXPECT_EQ(mask[5], 1);
  EXPECT_EQ(mask[8], 1);
  EXPECT_EQ(mask[9], 0);
  EXPECT_EQ(manager->lookup(0, base + 8, 1001, mask).reservation_count, 1U);
  EXPECT_EQ(manager->lookup(0, base + 10, 1001, mask).reservation_count, 0U);
}

TEST(quiet_reservation, rejects_changed_replay_and_finds_nonconflicting_start)
{
  const slot_point        base{1, 10, 4};
  std::array<uint16_t, 1> widths{52};
  auto                    manager = create_quiet_reservation_manager(widths);
  ASSERT_NE(manager, nullptr);

  auto first = make_request(base, 1);
  ASSERT_EQ(manager->submit(first).result, quiet_reservation_result::accepted);
  first.intent.nof_prbs = 3;
  auto replay           = manager->submit(first);
  EXPECT_EQ(replay.result, quiet_reservation_result::rejected);
  EXPECT_EQ(replay.reservation.reason, OCUDU_DAPP_QUIET_REASON_CONFLICT_V1);

  auto second                       = make_request(base, 2, 0, 1, 2);
  second.intent.repeat_count        = 1;
  second.intent.repeat_period_slots = 0;
  second.intent.latest_start        = to_radio_time(base + 8);
  const auto shifted                = manager->submit(second);
  ASSERT_EQ(shifted.result, quiet_reservation_result::accepted);
  EXPECT_EQ(from_radio_time(shifted.reservation.start), base + 6);
}

TEST(quiet_reservation, clamps_unsupported_authority_and_resource_width)
{
  const slot_point        base{0, 4, 1};
  std::array<uint16_t, 1> widths{52};
  auto                    manager = create_quiet_reservation_manager(widths);
  ASSERT_NE(manager, nullptr);

  auto request                   = make_request(base);
  request.intent.nof_prbs        = 20;
  request.intent.requested_level = OCUDU_DAPP_QUIET_RF_SILENCE_REQUEST_V1;
  const auto result              = manager->submit(request);
  ASSERT_EQ(result.result, quiet_reservation_result::clamped);
  EXPECT_EQ(result.reservation.nof_prbs, 16);
  EXPECT_EQ(result.reservation.achieved_level, OCUDU_DAPP_QUIET_NO_NEW_PUSCH_V1);
  EXPECT_EQ(result.reservation.reason, OCUDU_DAPP_QUIET_REASON_CAPABILITY_V1);
}

TEST(quiet_reservation, enforces_causality_horizon_expiry_and_budget)
{
  const slot_point        base{0, 4, 1};
  std::array<uint16_t, 1> widths{52};
  auto                    manager = create_quiet_reservation_manager(widths);
  ASSERT_NE(manager, nullptr);

  auto request                  = make_request(base);
  request.intent.earliest_start = to_radio_time(base + 3);
  EXPECT_EQ(manager->submit(request).reservation.reason, OCUDU_DAPP_QUIET_REASON_INSUFFICIENT_LEAD_V1);

  request                     = make_request(base);
  request.intent.latest_start = to_radio_time(base + 1030);
  EXPECT_EQ(manager->submit(request).reservation.reason, OCUDU_DAPP_QUIET_REASON_OUTSIDE_HORIZON_V1);

  request                            = make_request(base);
  request.intent.expiry_monotonic_ns = request.publish_monotonic_ns;
  EXPECT_EQ(manager->submit(request).reservation.reason, OCUDU_DAPP_QUIET_REASON_INVALID_V1);

  auto policy                                      = quiet_reservation_policy{};
  policy.maximum_slot_prb_budget                   = 15;
  policy.maximum_quiet_prb_slots_per_window        = 15;
  policy.maximum_source_quiet_prb_slots_per_window = 15;
  manager                                          = create_quiet_reservation_manager(widths, policy);
  EXPECT_EQ(manager->submit(make_request(base)).reservation.reason, OCUDU_DAPP_QUIET_REASON_BUDGET_V1);
}

TEST(quiet_reservation, enforces_per_slot_and_rolling_prb_duty_budgets)
{
  const slot_point        base{0, 4, 1};
  std::array<uint16_t, 1> widths{52};
  auto                    policy                   = quiet_reservation_policy{};
  policy.maximum_slot_prb_budget                   = 512;
  policy.maximum_quiet_prbs_per_slot               = 6;
  policy.duty_cycle_window_slots                   = 8;
  policy.maximum_quiet_prb_slots_per_window        = 24;
  policy.maximum_source_quiet_prb_slots_per_window = 24;
  auto manager                                     = create_quiet_reservation_manager(widths, policy);
  ASSERT_NE(manager, nullptr);

  auto first                = make_request(base, 1, 0, 1, 1);
  first.intent.latest_start = first.intent.earliest_start;
  ASSERT_EQ(manager->submit(first).result, quiet_reservation_result::accepted);

  auto per_slot                 = make_request(base, 2, 0, 1, 2);
  per_slot.intent.latest_start  = per_slot.intent.earliest_start;
  per_slot.intent.first_prb     = 20;
  const auto per_slot_rejection = manager->submit(per_slot);
  EXPECT_EQ(per_slot_rejection.result, quiet_reservation_result::rejected);
  EXPECT_EQ(per_slot_rejection.reservation.reason, OCUDU_DAPP_QUIET_REASON_BUDGET_V1);

  policy.maximum_quiet_prbs_per_slot               = 16;
  policy.maximum_quiet_prb_slots_per_window        = 24;
  policy.maximum_source_quiet_prb_slots_per_window = 24;
  manager                                          = create_quiet_reservation_manager(widths, policy);
  ASSERT_NE(manager, nullptr);
  ASSERT_EQ(manager->submit(first).result, quiet_reservation_result::accepted);

  auto rolling                  = make_request(base, 3, 0, 1, 3);
  rolling.intent.earliest_start = to_radio_time(base + 6);
  rolling.intent.latest_start   = rolling.intent.earliest_start;
  rolling.intent.first_prb      = 20;
  const auto rolling_rejection  = manager->submit(rolling);
  EXPECT_EQ(rolling_rejection.result, quiet_reservation_result::rejected);
  EXPECT_EQ(rolling_rejection.reservation.reason, OCUDU_DAPP_QUIET_REASON_BUDGET_V1);

  auto later                  = make_request(base, 4, 0, 1, 4);
  later.intent.earliest_start = to_radio_time(base + 20);
  later.intent.latest_start   = later.intent.earliest_start;
  later.intent.first_prb      = 20;
  EXPECT_EQ(manager->submit(later).result, quiet_reservation_result::accepted);
}

TEST(quiet_reservation, prevents_one_source_from_monopolizing_a_cell_rolling_budget)
{
  const slot_point        base{0, 4, 1};
  std::array<uint16_t, 2> widths{52, 52};
  auto                    policy                   = quiet_reservation_policy{};
  policy.maximum_slot_prb_budget                   = 128;
  policy.maximum_quiet_prbs_per_slot               = 16;
  policy.duty_cycle_window_slots                   = 16;
  policy.maximum_quiet_prb_slots_per_window        = 32;
  policy.maximum_source_quiet_prb_slots_per_window = 16;
  auto manager                                     = create_quiet_reservation_manager(widths, policy);
  ASSERT_NE(manager, nullptr);

  auto source_a                = make_request(base, 1, 0, 1, 1);
  source_a.intent.latest_start = source_a.intent.earliest_start;
  ASSERT_EQ(manager->submit(source_a).result, quiet_reservation_result::accepted);

  auto same_source                 = make_request(base, 2, 0, 1, 2);
  same_source.intent.latest_start  = same_source.intent.earliest_start;
  same_source.intent.first_prb     = 20;
  const auto source_budget_reached = manager->submit(same_source);
  EXPECT_EQ(source_budget_reached.result, quiet_reservation_result::rejected);
  EXPECT_EQ(source_budget_reached.reservation.reason, OCUDU_DAPP_QUIET_REASON_BUDGET_V1);
  EXPECT_EQ(source_budget_reached.constraint, quiet_reservation_constraint::source_budget);

  auto source_b                 = same_source;
  source_b.intent.request_id[0] = 3;
  source_b.source_instance_id   = {31, 41};
  source_b.source_sequence      = 1;
  ASSERT_EQ(manager->submit(source_b).result, quiet_reservation_result::accepted);

  // The authenticated source quota is per cell, so the same advisor may reserve independently on another cell.
  auto cell1                 = same_source;
  cell1.cell_id              = 1;
  cell1.intent.request_id[0] = 4;
  ASSERT_EQ(manager->submit(cell1).result, quiet_reservation_result::accepted);

  auto source_c                  = same_source;
  source_c.intent.request_id[0]  = 5;
  source_c.intent.first_prb      = 30;
  source_c.source_instance_id    = {51, 61};
  source_c.source_sequence       = 1;
  const auto cell_budget_reached = manager->submit(source_c);
  EXPECT_EQ(cell_budget_reached.result, quiet_reservation_result::rejected);
  EXPECT_EQ(cell_budget_reached.reservation.reason, OCUDU_DAPP_QUIET_REASON_BUDGET_V1);
  EXPECT_EQ(cell_budget_reached.constraint, quiet_reservation_constraint::cell_budget);

  auto later                  = same_source;
  later.intent.request_id[0]  = 6;
  later.source_sequence       = 3;
  later.intent.earliest_start = to_radio_time(base + 24);
  later.intent.latest_start   = later.intent.earliest_start;
  EXPECT_EQ(manager->submit(later).result, quiet_reservation_result::accepted);
}

TEST(quiet_reservation, source_generation_replaces_history_and_invalidation_is_cross_cell)
{
  const slot_point        base{0, 8, 2};
  std::array<uint16_t, 2> widths{52, 24};
  auto                    manager = create_quiet_reservation_manager(widths);
  ASSERT_NE(manager, nullptr);

  ASSERT_EQ(manager->submit(make_request(base, 1, 0)).result, quiet_reservation_result::accepted);
  ASSERT_EQ(manager->submit(make_request(base, 2, 1, 1, 2)).result, quiet_reservation_result::accepted);

  auto replacement             = make_request(base, 3, 0, 2, 1);
  replacement.intent.first_prb = 20;
  ASSERT_EQ(manager->submit(replacement).result, quiet_reservation_result::accepted);
  std::array<uint8_t, 52> cell0{};
  EXPECT_EQ(manager->lookup(0, base + 4, 1001, cell0).reservation_count, 1U);
  EXPECT_EQ(cell0[5], 0);
  EXPECT_EQ(cell0[20], 1);

  EXPECT_EQ(manager->invalidate_source({17, 23}, 1), quiet_reservation_result::accepted);
  std::array<uint8_t, 24> cell1{};
  EXPECT_EQ(manager->lookup(1, base + 4, 1001, cell1).reservation_count, 0U);
  EXPECT_EQ(manager->lookup(0, base + 4, 1001, cell0).reservation_count, 1U);
  EXPECT_EQ(manager->invalidate_source({17, 23}, 2), quiet_reservation_result::accepted);
  EXPECT_EQ(manager->lookup(0, base + 4, 1001, cell0).reservation_count, 0U);
}

TEST(quiet_reservation, zero_generation_invalidates_every_generation_of_one_source)
{
  const slot_point        base{0, 8, 2};
  std::array<uint16_t, 2> widths{52, 24};
  auto                    manager = create_quiet_reservation_manager(widths);
  ASSERT_NE(manager, nullptr);
  ASSERT_EQ(manager->submit(make_request(base, 1, 0, 1, 1)).result, quiet_reservation_result::accepted);
  ASSERT_EQ(manager->submit(make_request(base, 2, 1, 2, 1)).result, quiet_reservation_result::accepted);

  EXPECT_EQ(manager->invalidate_source({17, 23}, 0), quiet_reservation_result::accepted);
  std::array<uint8_t, 52> cell0{};
  std::array<uint8_t, 24> cell1{};
  EXPECT_EQ(manager->lookup(0, base + 4, 1001, cell0).reservation_count, 0U);
  EXPECT_EQ(manager->lookup(1, base + 4, 1001, cell1).reservation_count, 0U);
}

TEST(quiet_reservation, snapshots_unexpired_decisions_into_caller_storage)
{
  const slot_point        base{0, 8, 2};
  std::array<uint16_t, 2> widths{52, 24};
  auto                    manager = create_quiet_reservation_manager(widths);
  ASSERT_NE(manager, nullptr);
  const auto accepted = manager->submit(make_request(base, 7, 1, 3, 9));
  ASSERT_EQ(accepted.result, quiet_reservation_result::accepted);

  std::array<ocudu_dapp_quiet_period_reservation_v1, quiet_reservation_max_records> records{};
  auto snapshot = manager->query(1, 1001, records);
  ASSERT_TRUE(snapshot.valid_cell);
  ASSERT_EQ(snapshot.count, 1U);
  EXPECT_EQ(snapshot.generation, accepted.reservation.reservation_generation);
  EXPECT_EQ(records[0].request_id[0], 7U);
  EXPECT_EQ(records[0].request_source_generation, 3U);
  EXPECT_EQ(records[0].request_source_sequence, 9U);

  EXPECT_EQ(manager->query(1, 10000, records).count, 0U);
  EXPECT_FALSE(manager->query(2, 1001, records).valid_cell);
  EXPECT_FALSE(manager->query(1, 1001, span<ocudu_dapp_quiet_period_reservation_v1>{records}.first(1)).valid_cell);
}

TEST(quiet_reservation, readers_and_source_replacement_are_snapshot_safe)
{
  const slot_point        base{0, 11, 2};
  std::array<uint16_t, 1> widths{52};
  auto                    manager = create_quiet_reservation_manager(widths);
  ASSERT_NE(manager, nullptr);
  ASSERT_EQ(manager->submit(make_request(base)).result, quiet_reservation_result::accepted);

  std::atomic<bool> stop{false};
  std::thread       reader([&]() {
    std::array<uint8_t, 52>                                                           mask{};
    std::array<ocudu_dapp_quiet_period_reservation_v1, quiet_reservation_max_records> records{};
    while (!stop.load(std::memory_order_relaxed)) {
      const auto result = manager->lookup(0, base + 4, 1001, mask);
      EXPECT_LE(result.reservation_count, 1U);
      const auto snapshot = manager->query(0, 1001, records);
      EXPECT_TRUE(snapshot.valid_cell);
      EXPECT_LE(snapshot.count, 1U);
    }
  });
  for (uint64_t generation = 2; generation != 100; ++generation) {
    auto       update = make_request(base, generation, 0, generation, 1);
    const auto result = manager->submit(update).result;
    EXPECT_TRUE(result == quiet_reservation_result::accepted || result == quiet_reservation_result::busy);
  }
  stop.store(true, std::memory_order_relaxed);
  reader.join();
}
