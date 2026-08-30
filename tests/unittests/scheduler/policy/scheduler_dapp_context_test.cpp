// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/scheduler/dapp/scheduler_dapp_context.h"
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

scheduler_dapp_interference_update make_update(slot_point          created,
                                               slot_point          valid_from,
                                               slot_point          valid_until,
                                               uint64_t            sequence,
                                               span<const uint8_t> mask,
                                               uint64_t            now = 1000)
{
  scheduler_dapp_interference_update update{};
  update.context.struct_size             = sizeof(update.context);
  update.context.schema_id               = OCUDU_DAPP_SCHEDULER_CONTEXT_INTERFERENCE_MAP_V1;
  update.context.schema_version          = 1;
  update.context.producer_instance_id[0] = 17;
  update.context.producer_generation     = 3;
  update.context.source_sequence         = sequence;
  update.context.cell_id                 = 0;
  update.context.confidence_q16          = 1U << 15U;
  update.context.created                 = to_radio_time(created);
  update.context.valid_from              = to_radio_time(valid_from);
  update.context.valid_until             = to_radio_time(valid_until);
  update.prb_confidence                  = mask;
  update.publish_monotonic_ns            = now;
  return update;
}

} // namespace

TEST(scheduler_dapp_context, publishes_only_future_valid_full_cell_snapshots)
{
  auto cache = create_scheduler_dapp_interference_cache(std::chrono::nanoseconds(100));
  ASSERT_NE(cache, nullptr);
  std::array<uint8_t, 52> mask{};
  mask[7] = 201;
  const slot_point base{0, 5, 3};
  auto             update = make_update(base, base + 1, base + 3, 9, mask);
  ASSERT_EQ(cache->publish(update), scheduler_dapp_context_result::accepted);

  auto lease = cache->acquire(0, mask.size(), base + 1, 1050);
  ASSERT_TRUE(lease);
  EXPECT_EQ(lease.context().source_sequence, 9U);
  EXPECT_EQ(lease.mask().layout_id, OCUDU_DAPP_SCHEDULER_LAYOUT_PRB_INTERFERENCE_U8_V1);
  EXPECT_EQ(lease.mask().dimensions[0], mask.size());
  const auto* values = reinterpret_cast<const uint8_t*>(lease.mask().native_handle);
  ASSERT_NE(values, nullptr);
  EXPECT_EQ(values[7], mask[7]);

  EXPECT_FALSE(cache->acquire(0, mask.size(), base, 1050));
  EXPECT_FALSE(cache->acquire(0, mask.size(), base + 4, 1050));
  EXPECT_FALSE(cache->acquire(0, mask.size() - 1, base + 1, 1050));
  EXPECT_FALSE(cache->acquire(1, mask.size(), base + 1, 1050));
  EXPECT_FALSE(cache->acquire(0, mask.size(), base + 1, 1101));
}

TEST(scheduler_dapp_context, rejects_malformed_or_same_slot_causal_updates)
{
  auto cache = create_scheduler_dapp_interference_cache();
  ASSERT_NE(cache, nullptr);
  std::array<uint8_t, 52> mask{};
  const slot_point        base{0, 5, 3};

  auto update = make_update(base, base, base + 1, 1, mask);
  EXPECT_EQ(cache->publish(update), scheduler_dapp_context_result::invalid);
  update                   = make_update(base, base + 1, base + 2, 1, mask);
  update.context.schema_id = 0;
  EXPECT_EQ(cache->publish(update), scheduler_dapp_context_result::invalid);
  update = make_update(base, base + 1, base + 2, 0, mask);
  EXPECT_EQ(cache->publish(update), scheduler_dapp_context_result::invalid);
  update = make_update(base, base + 1, base + 2, 1, {});
  EXPECT_EQ(cache->publish(update), scheduler_dapp_context_result::invalid);
}

TEST(scheduler_dapp_context, rejects_replay_or_source_change_until_route_invalidation)
{
  auto cache = create_scheduler_dapp_interference_cache();
  ASSERT_NE(cache, nullptr);
  std::array<uint8_t, 52> mask{};
  const slot_point        base{0, 5, 3};
  auto                    update = make_update(base, base + 1, base + 2, 7, mask);
  ASSERT_EQ(cache->publish(update), scheduler_dapp_context_result::accepted);
  EXPECT_EQ(cache->publish(update), scheduler_dapp_context_result::invalid);
  update.context.source_sequence = 6;
  EXPECT_EQ(cache->publish(update), scheduler_dapp_context_result::invalid);
  update.context.source_sequence         = 1;
  update.context.producer_instance_id[0] = 99;
  EXPECT_EQ(cache->publish(update), scheduler_dapp_context_result::invalid);

  ASSERT_EQ(cache->invalidate(0), scheduler_dapp_context_result::accepted);
  EXPECT_FALSE(cache->acquire(0, mask.size(), base + 1, 1001));
  EXPECT_EQ(cache->publish(update), scheduler_dapp_context_result::accepted);
}

TEST(scheduler_dapp_context, drops_instead_of_overwriting_a_held_snapshot)
{
  auto cache = create_scheduler_dapp_interference_cache(std::chrono::seconds(1));
  ASSERT_NE(cache, nullptr);
  std::array<uint8_t, 52> mask{};
  const slot_point        base{0, 5, 3};

  mask.fill(1);
  ASSERT_EQ(cache->publish(make_update(base, base + 1, base + 3, 1, mask)), scheduler_dapp_context_result::accepted);
  auto first = cache->acquire(0, mask.size(), base + 1, 1001);
  ASSERT_TRUE(first);
  mask.fill(2);
  ASSERT_EQ(cache->publish(make_update(base, base + 1, base + 3, 2, mask)), scheduler_dapp_context_result::accepted);
  auto second = cache->acquire(0, mask.size(), base + 1, 1001);
  ASSERT_TRUE(second);
  mask.fill(3);
  ASSERT_EQ(cache->publish(make_update(base, base + 1, base + 3, 3, mask)), scheduler_dapp_context_result::accepted);
  auto third = cache->acquire(0, mask.size(), base + 1, 1001);
  ASSERT_TRUE(third);

  mask.fill(4);
  EXPECT_EQ(cache->publish(make_update(base, base + 1, base + 3, 4, mask)), scheduler_dapp_context_result::busy);
  EXPECT_EQ(reinterpret_cast<const uint8_t*>(first.mask().native_handle)[0], 1);
  EXPECT_EQ(reinterpret_cast<const uint8_t*>(second.mask().native_handle)[0], 2);
  EXPECT_EQ(reinterpret_cast<const uint8_t*>(third.mask().native_handle)[0], 3);

  first = {};
  EXPECT_EQ(cache->publish(make_update(base, base + 1, base + 3, 4, mask)), scheduler_dapp_context_result::accepted);
  EXPECT_EQ(reinterpret_cast<const uint8_t*>(second.mask().native_handle)[0], 2);
  EXPECT_EQ(reinterpret_cast<const uint8_t*>(third.mask().native_handle)[0], 3);
}

TEST(scheduler_dapp_context, concurrent_publication_never_tears_a_snapshot)
{
  auto cache = create_scheduler_dapp_interference_cache(std::chrono::seconds(1));
  ASSERT_NE(cache, nullptr);
  constexpr uint32_t      iterations = 20000;
  std::array<uint8_t, 52> initial{};
  const slot_point        base{0, 5, 3};
  initial.fill(1);
  ASSERT_EQ(cache->publish(make_update(base, base + 1, base + 3, 1, initial)), scheduler_dapp_context_result::accepted);

  std::atomic<bool>     start{};
  std::atomic<bool>     torn{};
  std::atomic<uint32_t> reads{};
  std::thread           writer([&] {
    std::array<uint8_t, 52> mask{};
    while (!start.load(std::memory_order_acquire)) {
    }
    for (uint64_t sequence = 2; sequence != iterations + 2; ++sequence) {
      mask.fill(static_cast<uint8_t>(sequence));
      cache->publish(make_update(base, base + 1, base + 3, sequence, mask, 1000 + sequence));
    }
  });
  std::thread           reader([&] {
    start.store(true, std::memory_order_release);
    for (uint32_t index = 0; index != iterations; ++index) {
      auto lease = cache->acquire(0, initial.size(), base + 1, 100000);
      if (!lease) {
        continue;
      }
      const auto* mask     = reinterpret_cast<const uint8_t*>(lease.mask().native_handle);
      const auto  expected = static_cast<uint8_t>(lease.context().source_sequence);
      for (uint8_t value : span<const uint8_t>(mask, initial.size())) {
        if (value != expected) {
          torn.store(true, std::memory_order_relaxed);
        }
      }
      reads.fetch_add(1, std::memory_order_relaxed);
    }
  });
  writer.join();
  reader.join();
  EXPECT_GT(reads.load(std::memory_order_relaxed), 0U);
  EXPECT_FALSE(torn.load(std::memory_order_relaxed));
}
