// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/runtime/pusch_route_table.h"
#include "ocudu/dapp/use_cases/v1/channel_estimator.h"
#include "ocudu/dapp/use_cases/v1/equalizer.h"
#include "ocudu/dapp/use_cases/v1/receiver.h"
#include "ocudu/dapp/use_cases/v1/scheduler.h"
#include <atomic>
#include <gtest/gtest.h>
#include <thread>
#include <vector>

using namespace ocudu::dapp;
using management::identifier;
using management::result_code;

namespace {

identifier id(uint8_t value)
{
  identifier result;
  result.bytes[15] = value;
  return result;
}

pusch_route_target to_instance(uint8_t value)
{
  return {pusch_route_target::kind::instance, id(value)};
}

pusch_route_target conventional()
{
  return {pusch_route_target::kind::conventional, {}};
}

pusch_route_entry entry(uint16_t rnti, pusch_route_target target, std::optional<uint16_t> cell = std::nullopt)
{
  return {cell, rnti, target};
}

} // namespace

TEST(dapp_pusch_route_table, maps_only_the_class_a_pusch_interfaces)
{
  EXPECT_EQ(pusch_route_hook_for_interface(OCUDU_DAPP_INTERFACE_CHANNEL_ESTIMATOR_V1),
            pusch_route_hook::channel_estimator);
  EXPECT_EQ(pusch_route_hook_for_interface(OCUDU_DAPP_INTERFACE_EQUALIZER_V1), pusch_route_hook::equalizer);
  EXPECT_EQ(pusch_route_hook_for_interface(OCUDU_DAPP_INTERFACE_RECEIVER_V1), pusch_route_hook::receiver);
  EXPECT_FALSE(pusch_route_hook_for_interface(OCUDU_DAPP_INTERFACE_SCHEDULER_V1));
}

TEST(dapp_pusch_route_table, rejects_slot_counts_outside_the_supported_range)
{
  EXPECT_THROW(pusch_route_table(0), std::invalid_argument);
  EXPECT_THROW(pusch_route_table(pusch_route_table::max_slots_per_hook + 1), std::invalid_argument);
  EXPECT_NO_THROW(pusch_route_table(pusch_route_table::max_slots_per_hook));
}

TEST(dapp_pusch_route_table, an_empty_table_is_the_host_without_routing)
{
  pusch_route_table table(2);
  EXPECT_EQ(table.resolve(0, 0x4601), pusch_route::primary());
  EXPECT_EQ(table.resolve(7, 0xFFEF), pusch_route::primary());
  const auto state = table.state();
  EXPECT_EQ(state.generation, 0U);
  EXPECT_EQ(state.slots_per_hook, 2U);
  EXPECT_TRUE(state.entries.empty());
  EXPECT_EQ(state.default_target.type, pusch_route_target::kind::primary);
}

TEST(dapp_pusch_route_table, exact_cell_entries_win_over_any_cell_entries_and_the_default)
{
  pusch_route_table table(2);
  ASSERT_TRUE(table.bind(id(1), pusch_route_hook::receiver, 0));
  ASSERT_TRUE(table.bind(id(2), pusch_route_hook::receiver, 1));
  ASSERT_TRUE(
      table.replace(conventional(),
                    {entry(0x4601, to_instance(1)), entry(0x4601, to_instance(2), 3), entry(0x4602, to_instance(2))}));

  EXPECT_EQ(table.resolve(0, 0x4601), pusch_route::instance(pusch_route_hook::receiver, 0));
  EXPECT_EQ(table.resolve(3, 0x4601), pusch_route::instance(pusch_route_hook::receiver, 1));
  EXPECT_EQ(table.resolve(3, 0x4602), pusch_route::instance(pusch_route_hook::receiver, 1));
  EXPECT_EQ(table.resolve(0, 0x4603), pusch_route::conventional());
  EXPECT_EQ(table.state().generation, 3U) << "two binds and one replace each republish the snapshot";
}

TEST(dapp_pusch_route_table, requests_are_validated_before_anything_changes)
{
  pusch_route_table table(1);
  ASSERT_TRUE(table.bind(id(1), pusch_route_hook::equalizer, 0));
  ASSERT_TRUE(table.replace({}, {entry(0x4601, to_instance(1))}));
  const auto before = table.state();

  EXPECT_EQ(table.replace({}, {entry(0x4602, to_instance(9))}).code, result_code::not_found);
  EXPECT_EQ(table.replace(to_instance(9), {}).code, result_code::not_found);
  EXPECT_EQ(table.replace({}, {entry(0, conventional())}).code, result_code::invalid_request);
  EXPECT_EQ(table.replace({}, {entry(0xFFF0, conventional())}).code, result_code::invalid_request);
  EXPECT_EQ(table.replace({}, {entry(0x4601, conventional(), 0xFFFF)}).code, result_code::invalid_request);
  EXPECT_EQ(table.replace({}, {entry(0x4601, conventional()), entry(0x4601, to_instance(1))}).code,
            result_code::invalid_request);
  EXPECT_EQ(table.replace({}, {{std::nullopt, 0x4601, {pusch_route_target::kind::instance, {}}}}).code,
            result_code::invalid_request);
  std::vector<pusch_route_entry> oversized;
  for (uint16_t rnti = 1; rnti <= pusch_route_table::max_entries + 1; ++rnti) {
    oversized.push_back(entry(rnti, conventional()));
  }
  EXPECT_EQ(table.replace({}, oversized).code, result_code::resource_exhausted);
  EXPECT_EQ(table.update({entry(0x4603, to_instance(9))}).code, result_code::not_found);

  const auto after = table.state();
  EXPECT_EQ(after.generation, before.generation);
  ASSERT_EQ(after.entries.size(), 1U);
  EXPECT_EQ(table.resolve(0, 0x4601), pusch_route::instance(pusch_route_hook::equalizer, 0));
}

TEST(dapp_pusch_route_table, a_full_table_of_ues_resolves)
{
  pusch_route_table              table(1);
  std::vector<pusch_route_entry> entries;
  for (uint16_t rnti = 1; rnti <= pusch_route_table::max_entries; ++rnti) {
    entries.push_back(entry(static_cast<uint16_t>(0x4000 + rnti), conventional()));
  }
  ASSERT_TRUE(table.replace({}, entries));
  for (uint16_t rnti = 1; rnti <= pusch_route_table::max_entries; ++rnti) {
    ASSERT_EQ(table.resolve(0, static_cast<uint16_t>(0x4000 + rnti)), pusch_route::conventional());
  }
  EXPECT_EQ(table.resolve(0, 0x4000), pusch_route::primary());
}

TEST(dapp_pusch_route_table, update_overwrites_inserts_and_primary_removes)
{
  pusch_route_table table(2);
  ASSERT_TRUE(table.bind(id(1), pusch_route_hook::receiver, 0));
  ASSERT_TRUE(table.bind(id(2), pusch_route_hook::channel_estimator, 1));
  ASSERT_TRUE(table.replace(conventional(), {entry(0x4601, to_instance(1)), entry(0x4602, to_instance(1))}));

  ASSERT_TRUE(table.update({entry(0x4601, to_instance(2)), entry(0x4602, {}), entry(0x4603, to_instance(1))}));
  EXPECT_EQ(table.resolve(0, 0x4601), pusch_route::instance(pusch_route_hook::channel_estimator, 1));
  EXPECT_EQ(table.resolve(0, 0x4602), pusch_route::conventional()) << "removed: the default applies again";
  EXPECT_EQ(table.resolve(0, 0x4603), pusch_route::instance(pusch_route_hook::receiver, 0));
  EXPECT_EQ(table.state().default_target, conventional()) << "update keeps the default";
  EXPECT_EQ(table.state().entries.size(), 2U);
}

TEST(dapp_pusch_route_table, validate_only_checks_without_applying)
{
  pusch_route_table table(1);
  ASSERT_TRUE(table.bind(id(1), pusch_route_hook::receiver, 0));
  const auto generation = table.state().generation;

  EXPECT_TRUE(table.replace(conventional(), {entry(0x4601, to_instance(1))}, true));
  EXPECT_TRUE(table.update({entry(0x4601, to_instance(1))}, true));
  EXPECT_EQ(table.update({entry(0x4601, to_instance(4))}, true).code, result_code::not_found);
  EXPECT_EQ(table.state().generation, generation);
  EXPECT_TRUE(table.state().entries.empty());
  EXPECT_EQ(table.resolve(0, 0x4601), pusch_route::primary());
}

TEST(dapp_pusch_route_table, clear_restores_the_host_without_routing)
{
  pusch_route_table table(1);
  ASSERT_TRUE(table.bind(id(1), pusch_route_hook::receiver, 0));
  ASSERT_TRUE(table.replace(to_instance(1), {entry(0x4601, conventional())}));
  ASSERT_TRUE(table.clear());
  EXPECT_EQ(table.resolve(0, 0x4601), pusch_route::primary());
  EXPECT_EQ(table.resolve(0, 0x4602), pusch_route::primary());
  EXPECT_TRUE(table.state().entries.empty());
  ASSERT_EQ(table.state().bindings.size(), 1U) << "bindings belong to the registry, not to the table";
}

TEST(dapp_pusch_route_table, an_entry_follows_its_instance_and_never_the_slot)
{
  pusch_route_table table(2);
  ASSERT_TRUE(table.bind(id(1), pusch_route_hook::receiver, 1));
  ASSERT_TRUE(table.replace({}, {entry(0x4601, to_instance(1))}));
  ASSERT_EQ(table.resolve(0, 0x4601), pusch_route::instance(pusch_route_hook::receiver, 1));

  // Deactivated: the UE gets the stock receiver, never slot 1's next owner.
  table.unbind(id(1));
  EXPECT_EQ(table.resolve(0, 0x4601), pusch_route::conventional());
  ASSERT_TRUE(table.bind(id(7), pusch_route_hook::receiver, 1));
  EXPECT_EQ(table.resolve(0, 0x4601), pusch_route::conventional());
  EXPECT_EQ(table.state().unavailable_lookups, 2U);

  // Reactivated on another slot: the entry follows the instance there.
  table.unbind(id(7));
  ASSERT_TRUE(table.bind(id(1), pusch_route_hook::receiver, 0));
  EXPECT_EQ(table.resolve(0, 0x4601), pusch_route::instance(pusch_route_hook::receiver, 0));
  ASSERT_EQ(table.state().entries.size(), 1U) << "the operator's entry survives the deactivation";
}

TEST(dapp_pusch_route_table, a_default_instance_that_deactivates_falls_back_to_the_stock_receiver)
{
  pusch_route_table table(1);
  ASSERT_TRUE(table.bind(id(3), pusch_route_hook::equalizer, 0));
  ASSERT_TRUE(table.replace(to_instance(3), {}));
  EXPECT_EQ(table.resolve(0, 0x4601), pusch_route::instance(pusch_route_hook::equalizer, 0));
  table.unbind(id(3));
  EXPECT_EQ(table.resolve(0, 0x4601), pusch_route::conventional());
  EXPECT_EQ(table.state().unavailable_lookups, 1U);
}

TEST(dapp_pusch_route_table, bind_rejects_invalid_slots_and_replaces_a_slot_owner)
{
  pusch_route_table table(2);
  EXPECT_FALSE(table.bind(id(1), pusch_route_hook::receiver, 2));
  EXPECT_FALSE(table.bind({}, pusch_route_hook::receiver, 0));
  ASSERT_TRUE(table.bind(id(1), pusch_route_hook::receiver, 0));
  ASSERT_TRUE(table.bind(id(1), pusch_route_hook::equalizer, 1));
  const auto bindings = table.state().bindings;
  ASSERT_EQ(bindings.size(), 1U) << "an instance holds one routable binding";
  EXPECT_EQ(bindings.front().hook, pusch_route_hook::equalizer);
  EXPECT_EQ(bindings.front().slot, 1U);
}

TEST(dapp_pusch_route_table, concurrent_lookups_never_see_a_partially_written_snapshot)
{
  // Readers resolve continuously while the writer alternates between two tables that differ in
  // every entry and in the default. A lookup may see either table, but a buffer read while it is
  // being rebuilt would produce a route neither table contains.
  pusch_route_table table(2);
  ASSERT_TRUE(table.bind(id(1), pusch_route_hook::receiver, 0));
  ASSERT_TRUE(table.bind(id(2), pusch_route_hook::receiver, 1));
  std::vector<pusch_route_entry> first;
  std::vector<pusch_route_entry> second;
  for (uint16_t rnti = 1; rnti <= 200; ++rnti) {
    first.push_back(entry(rnti, to_instance(1)));
    second.push_back(entry(static_cast<uint16_t>(rnti + 1000), to_instance(2)));
  }
  const auto a = pusch_route::instance(pusch_route_hook::receiver, 0);
  const auto b = pusch_route::instance(pusch_route_hook::receiver, 1);
  ASSERT_TRUE(table.replace({}, first));

  std::atomic<bool>        stop{};
  std::atomic<uint64_t>    lookups{};
  std::atomic<uint64_t>    inconsistent{};
  std::vector<std::thread> readers;
  for (int reader = 0; reader != 4; ++reader) {
    readers.emplace_back([&, reader] {
      uint16_t rnti = static_cast<uint16_t>(1 + reader);
      while (!stop.load(std::memory_order_acquire)) {
        const auto in_first  = table.resolve(0, rnti);
        const auto in_second = table.resolve(0, static_cast<uint16_t>(rnti + 1000));
        // Table one: rnti -> a, rnti+1000 -> default (primary). Table two: rnti -> default
        // (conventional), rnti+1000 -> b. Each lookup is individually one of those.
        if ((in_first != a && in_first != pusch_route::conventional()) ||
            (in_second != b && in_second != pusch_route::primary())) {
          inconsistent.fetch_add(1, std::memory_order_relaxed);
        }
        lookups.fetch_add(2, std::memory_order_relaxed);
        rnti = static_cast<uint16_t>(1 + (rnti % 200));
      }
    });
  }
  for (int swap = 0; swap != 2000; ++swap) {
    ASSERT_TRUE(swap % 2 == 0 ? table.replace({}, first) : table.replace(conventional(), second));
  }
  stop.store(true, std::memory_order_release);
  for (auto& reader : readers) {
    reader.join();
  }
  EXPECT_EQ(inconsistent.load(), 0U);
  EXPECT_GT(lookups.load(), 0U);
}
