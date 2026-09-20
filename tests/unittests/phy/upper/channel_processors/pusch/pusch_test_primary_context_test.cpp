// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear
#include "pusch_test_primary_context.h"
#include <gtest/gtest.h>
#include <string>
#include <vector>

using namespace ocudu::test_support;

namespace {
struct operations {
  using context_type                   = unsigned;
  unsigned                 active      = 17;
  unsigned                 previous    = 17;
  unsigned                 calls       = 0;
  unsigned                 fail_at     = 0;
  unsigned                 releases    = 0;
  unsigned                 pops        = 0;
  bool                     bad_pop     = false;
  bool                     bad_restore = false;
  bool                     bad_runtime = false;
  std::vector<std::string> trace;

  bool step(const char* name)
  {
    trace.emplace_back(name);
    return ++calls != fail_at;
  }
  bool driver_init() { return step("init"); }
  bool current(unsigned& value)
  {
    value = active;
    return step("current");
  }
  bool device(int& value)
  {
    value = 0;
    return step("device");
  }
  bool retain(unsigned& value, int device)
  {
    EXPECT_EQ(device, 0);
    value = 23;
    return step("retain");
  }
  bool push(unsigned value)
  {
    active = value;
    return step("push");
  }
  bool runtime_device(int& value)
  {
    value = bad_runtime ? 1 : 0;
    return step("runtime");
  }
  bool pop(unsigned& value)
  {
    ++pops;
    value  = bad_pop ? 99 : active;
    active = bad_restore ? 98 : previous;
    return step("pop");
  }
  bool release(int device)
  {
    EXPECT_EQ(device, 0);
    ++releases;
    return step("release");
  }
};
} // namespace

TEST(pusch_test_primary_context, retains_until_explicit_retirement_and_restores_previous_context)
{
  operations                 ops;
  pusch_test_primary_context owner(ops);
  ASSERT_TRUE(owner.initialize());
  EXPECT_FALSE(owner.initialize());
  EXPECT_EQ(ops.active, 23U);
  EXPECT_EQ(ops.releases, 0U);
  ASSERT_TRUE(owner.close());
  EXPECT_TRUE(owner.close());
  EXPECT_EQ(ops.active, 17U);
  EXPECT_EQ(ops.releases, 1U);
  EXPECT_EQ(ops.pops, 1U);
}

TEST(pusch_test_primary_context, every_setup_failure_leaves_uncertain_ownership_untouched)
{
  for (unsigned failure = 1; failure <= 8; ++failure) {
    operations ops;
    ops.fail_at = failure;
    {
      pusch_test_primary_context owner(ops);
      EXPECT_FALSE(owner.initialize());
      const auto calls = ops.calls;
      EXPECT_FALSE(owner.initialize());
      EXPECT_FALSE(owner.close());
      EXPECT_EQ(ops.calls, calls);
    }
    EXPECT_EQ(ops.releases, 0U);
    EXPECT_EQ(ops.pops, 0U);
  }
}

TEST(pusch_test_primary_context, every_close_failure_is_not_retried_or_reset)
{
  for (unsigned failure = 9; failure <= 12; ++failure) {
    operations ops;
    ops.fail_at = failure;
    pusch_test_primary_context owner(ops);
    ASSERT_TRUE(owner.initialize());
    EXPECT_FALSE(owner.close());
    const auto calls = ops.calls;
    EXPECT_FALSE(owner.close());
    EXPECT_EQ(ops.calls, calls);
    EXPECT_EQ(ops.releases, failure == 12 ? 1U : 0U);
  }
}

TEST(pusch_test_primary_context, wrong_context_or_runtime_device_cannot_be_released)
{
  for (unsigned fault = 0; fault != 4; ++fault) {
    operations ops;
    ops.bad_runtime = fault == 0;
    pusch_test_primary_context owner(ops);
    if (fault == 0) {
      EXPECT_FALSE(owner.initialize());
    } else {
      ASSERT_TRUE(owner.initialize());
      if (fault == 1) {
        ops.active = 29;
      }
      ops.bad_pop     = fault == 2;
      ops.bad_restore = fault == 3;
    }
    EXPECT_FALSE(owner.close());
    EXPECT_EQ(ops.releases, 0U);
  }
}

TEST(pusch_test_primary_context, destructor_never_releases_active_consumers)
{
  operations ops;
  {
    pusch_test_primary_context owner(ops);
    ASSERT_TRUE(owner.initialize());
  }
  EXPECT_EQ(ops.pops, 0U);
  EXPECT_EQ(ops.releases, 0U);
}
