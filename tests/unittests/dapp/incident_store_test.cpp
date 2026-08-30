// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/runtime/incident_store.h"
#include <gtest/gtest.h>

using namespace ocudu::dapp;

namespace {

incident_record record(uint64_t        id,
                       uint64_t        time,
                       incident_cause  cause  = incident_cause::invalid_output,
                       incident_action action = incident_action::invocation_fallback)
{
  incident_record value{};
  value.incident_id            = id;
  value.monotonic_ns           = time;
  value.context.instance_id[0] = 7;
  value.context.instance_id[1] = 9;
  value.context.hook_id        = 3;
  value.context.phase          = incident_phase::invoke;
  value.cause                  = cause;
  value.action                 = action;
  value.native_status          = OCUDU_DAPP_INVALID_OUTPUT_V1;
  value.recurrence_count       = 1;
  return value;
}

} // namespace

TEST(dapp_incident_store, deduplicates_adjacent_recurrence_but_preserves_cumulative_health)
{
  incident_store store(8, 100);
  store.ingest(record(1, 1000));
  store.ingest(record(2, 1050));
  ASSERT_EQ(store.size(), 1U);
  auto records = store.query();
  ASSERT_EQ(records.size(), 1U);
  EXPECT_EQ(records[0].incident_id, 2U);
  EXPECT_EQ(records[0].recurrence_count, 2U);
  auto health = store.health();
  EXPECT_EQ(health.total, 2U);
  EXPECT_EQ(health.invalid_outputs, 2U);
  EXPECT_EQ(health.fallbacks, 2U);
}

TEST(dapp_incident_store, bounded_history_evicts_oldest_and_reports_eviction)
{
  incident_store store(2, 0);
  store.ingest(record(1, 1));
  store.ingest(record(2, 2, incident_cause::deadline_miss));
  store.ingest(record(3, 3, incident_cause::device_error));
  auto records = store.query();
  ASSERT_EQ(records.size(), 2U);
  EXPECT_EQ(records[0].incident_id, 2U);
  EXPECT_EQ(records[1].incident_id, 3U);
  EXPECT_EQ(store.health().history_evictions, 1U);
}

TEST(dapp_incident_store, filters_by_instance_cause_phase_and_cursor)
{
  incident_store store;
  store.ingest(record(1, 1));
  auto second                   = record(2, 2, incident_cause::device_error);
  second.context.instance_id[0] = 99;
  second.context.phase          = incident_phase::completion;
  store.ingest(second);

  incident_filter filter;
  filter.instance_id       = std::array<uint64_t, 2>{99, 9};
  filter.cause             = incident_cause::device_error;
  filter.phase             = incident_phase::completion;
  filter.after_incident_id = 1;
  auto records             = store.query(filter);
  ASSERT_EQ(records.size(), 1U);
  EXPECT_EQ(records[0].incident_id, 2U);
}

TEST(dapp_incident_store, clearing_history_does_not_clear_health_or_last_incident)
{
  incident_store store;
  store.ingest(record(10, 10));
  store.ingest(record(11, 11, incident_cause::device_error));
  EXPECT_EQ(store.clear_through(10), 1U);
  EXPECT_EQ(store.size(), 1U);
  EXPECT_EQ(store.health().total, 2U);
  EXPECT_EQ(store.health().last_incident_id, 11U);
}

TEST(dapp_incident_store, drains_ring_and_accounts_only_new_drop_delta)
{
  incident_ring<2> ring;
  ASSERT_TRUE(ring.try_push(record(1, 1)));
  ASSERT_TRUE(ring.try_push(record(2, 2, incident_cause::device_error)));
  EXPECT_FALSE(ring.try_push(record(3, 3)));
  incident_store store;
  EXPECT_EQ(store.drain(ring), 2U);
  EXPECT_EQ(store.health().source_ring_drops, 1U);
  EXPECT_EQ(store.drain(ring), 0U);
  EXPECT_EQ(store.health().source_ring_drops, 1U);
}

TEST(dapp_incident_store, accounts_drop_baselines_independently_for_each_worker_ring)
{
  incident_ring<2> first;
  incident_ring<2> second;
  ASSERT_TRUE(first.try_push(record(1, 1)));
  ASSERT_TRUE(first.try_push(record(2, 2)));
  EXPECT_FALSE(first.try_push(record(3, 3)));
  ASSERT_TRUE(second.try_push(record(4, 4)));
  ASSERT_TRUE(second.try_push(record(5, 5)));
  EXPECT_FALSE(second.try_push(record(6, 6)));

  incident_store store;
  store.drain(first);
  store.drain(second);
  EXPECT_EQ(store.health().source_ring_drops, 2U);
}
