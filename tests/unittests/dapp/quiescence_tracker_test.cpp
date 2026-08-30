// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/runtime/quiescence_tracker.h"
#include <atomic>
#include <gtest/gtest.h>
#include <thread>

using namespace ocudu::dapp;
using namespace std::chrono_literals;

TEST(dapp_quiescence_tracker, admission_gate_and_all_activity_classes_are_accounted)
{
  quiescence_tracker tracker;
  EXPECT_FALSE(tracker.try_acquire(activity_kind::invocation));
  tracker.open_admission();
  auto invocation = tracker.try_acquire(activity_kind::invocation);
  auto lease      = tracker.try_acquire(activity_kind::lease);
  auto callback   = tracker.try_acquire(activity_kind::callback);
  auto gpu_work   = tracker.try_acquire(activity_kind::accelerator_work);
  ASSERT_TRUE(invocation && lease && callback && gpu_work);
  auto snapshot = tracker.snapshot();
  EXPECT_EQ(snapshot.invocations, 1U);
  EXPECT_EQ(snapshot.leases, 1U);
  EXPECT_EQ(snapshot.callbacks, 1U);
  EXPECT_EQ(snapshot.accelerator_work, 1U);

  tracker.close_admission();
  EXPECT_FALSE(tracker.try_acquire(activity_kind::invocation));
  EXPECT_FALSE(tracker.wait_until_empty(std::chrono::steady_clock::now() + 1ms));
  invocation.reset();
  lease.reset();
  callback.reset();
  gpu_work.reset();
  EXPECT_TRUE(tracker.wait_until_empty(std::chrono::steady_clock::now() + 20ms));
}

TEST(dapp_quiescence_tracker, moved_token_releases_exactly_once)
{
  quiescence_tracker tracker;
  tracker.open_admission();
  auto           first = tracker.try_acquire(activity_kind::lease);
  activity_token second(std::move(first));
  EXPECT_FALSE(first);
  EXPECT_EQ(tracker.snapshot().leases, 1U);
  second.reset();
  EXPECT_EQ(tracker.snapshot().leases, 0U);
}

TEST(dapp_quiescence_tracker, closing_gate_races_safely_with_hot_acquisition)
{
  quiescence_tracker tracker;
  tracker.open_admission();
  std::atomic<bool>     run{true};
  std::atomic<uint32_t> acquisitions{0};
  std::thread           producer([&] {
    while (run.load(std::memory_order_relaxed)) {
      auto token = tracker.try_acquire(activity_kind::invocation);
      if (token) {
        acquisitions.fetch_add(1, std::memory_order_relaxed);
        std::this_thread::yield();
      }
    }
  });
  while (acquisitions.load(std::memory_order_relaxed) < 100) {
    std::this_thread::yield();
  }
  tracker.close_admission();
  run.store(false, std::memory_order_relaxed);
  producer.join();
  EXPECT_FALSE(tracker.try_acquire(activity_kind::invocation));
  EXPECT_TRUE(tracker.wait_until_empty(std::chrono::steady_clock::now() + 100ms));
}
