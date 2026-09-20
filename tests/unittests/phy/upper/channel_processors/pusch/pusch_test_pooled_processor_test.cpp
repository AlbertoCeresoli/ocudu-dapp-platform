// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear
#include "../../../support/resource_grid_test_doubles.h"
#include "pusch_test_pooled_processor.h"
#include <functional>
#include <gtest/gtest.h>
#include <thread>

using namespace ocudu;
namespace {
class test_sink final : public pusch_processor_result_notifier
{
public:
  unsigned results{};
  void     on_uci(const pusch_processor_result_control&) override {}
  void     on_sch(const pusch_processor_result_data&) override { ++results; }
};
class test_processor final : public pusch_processor
{
public:
  bool                             synchronous = true;
  pusch_processor_result_notifier* target{};
  std::function<void()>            tail;
  void                             process(span<uint8_t>,
                                           unique_rx_buffer,
                                           pusch_processor_result_notifier& notifier,
                                           const resource_grid_reader&,
                                           const pdu_t&) override
  {
    target = &notifier;
    if (synchronous) {
      target->on_sch({});
    }
    if (tail) {
      tail();
    }
  }
};
struct fixture {
  resource_grid_reader_spy    grid;
  test_sink                   sink;
  test_processor*             inner{};
  pusch_test_pooled_processor owner;
  fixture() : owner(make_processor()) {}
  std::unique_ptr<pusch_processor> make_processor()
  {
    auto processor = std::make_unique<test_processor>();
    inner          = processor.get();
    return processor;
  }
  void process() { owner.process({}, unique_rx_buffer{}, sink, grid, {}); }
};
} // namespace

TEST(pusch_test_pooled_processor, synchronous_tail_retains_real_pool_entry)
{
  fixture f;
  f.inner->tail = [&] {
    EXPECT_EQ(f.sink.results, 1U);
    auto activity = f.inner->target->acquire_processing_activity();
    EXPECT_TRUE(activity);
    EXPECT_THROW(f.owner.require_idle(), std::runtime_error);
  };
  f.process();
  EXPECT_NO_THROW(f.owner.require_idle());
}

TEST(pusch_test_pooled_processor, delayed_result_retains_processor)
{
  fixture f;
  f.inner->synchronous = false;
  f.process();
  EXPECT_THROW(f.owner.require_idle(), std::runtime_error);
  f.inner->target->on_sch({});
  EXPECT_NO_THROW(f.owner.require_idle());
}

TEST(pusch_test_pooled_processor, sch_does_not_release_deferred_consumer)
{
  fixture f;
  f.inner->synchronous = false;
  f.process();
  auto activity = f.inner->target->acquire_processing_activity();
  ASSERT_TRUE(activity);
  f.inner->target->on_sch({});
  EXPECT_EQ(f.sink.results, 1U);
  EXPECT_THROW(f.owner.require_idle(), std::runtime_error);
  auto moved = std::move(activity);
  EXPECT_FALSE(activity);
  moved.reset();
  EXPECT_NO_THROW(f.owner.require_idle());
}

TEST(pusch_test_pooled_processor, busy_lane_rejects_new_grant_without_overwriting_owner)
{
  fixture f;
  f.inner->synchronous = false;
  f.process();
  EXPECT_THROW(f.process(), std::runtime_error);
  f.inner->target->on_sch({});
  EXPECT_EQ(f.sink.results, 1U);
  EXPECT_NO_THROW(f.owner.require_idle());
}

TEST(pusch_test_pooled_processor, last_cross_thread_consumer_releases_lane)
{
  fixture f;
  f.inner->synchronous = false;
  f.process();
  auto first = f.inner->target->acquire_processing_activity();
  auto last  = f.inner->target->acquire_processing_activity();
  f.inner->target->on_sch({});
  std::thread release([activity = std::move(first)]() mutable { activity.reset(); });
  release.join();
  EXPECT_THROW(f.owner.require_idle(), std::runtime_error);
  last.reset();
  EXPECT_NO_THROW(f.owner.require_idle());
}

TEST(pusch_test_pooled_processor, repeated_grants_balance_activity_without_new_processors)
{
  fixture f;
  for (unsigned i = 0; i != 1000; ++i) {
    f.inner->synchronous = i % 2;
    f.process();
    if (!f.inner->synchronous) {
      f.inner->target->on_sch({});
    }
    EXPECT_NO_THROW(f.owner.require_idle());
  }
  EXPECT_EQ(f.sink.results, 1000U);
}
