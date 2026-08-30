// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/runtime/circuit_breaker.h"
#include <gtest/gtest.h>

using namespace ocudu::dapp;

TEST(dapp_circuit_breaker, exception_opens_immediately)
{
  circuit_breaker breaker;
  EXPECT_TRUE(breaker.observe(OCUDU_DAPP_EXCEPTION_V1));
  EXPECT_EQ(breaker.state(), breaker_state::open);
  EXPECT_FALSE(breaker.allows_invocation());
}

TEST(dapp_circuit_breaker, recoverable_error_opens_at_configured_threshold)
{
  circuit_breaker breaker({2});
  EXPECT_FALSE(breaker.observe(OCUDU_DAPP_DEADLINE_V1));
  EXPECT_TRUE(breaker.allows_invocation());
  EXPECT_TRUE(breaker.observe(OCUDU_DAPP_DEADLINE_V1));
  EXPECT_FALSE(breaker.allows_invocation());
}

TEST(dapp_circuit_breaker, default_recoverable_threshold_is_eight)
{
  circuit_breaker breaker;
  for (unsigned i = 0; i != 7; ++i) {
    EXPECT_FALSE(breaker.observe(OCUDU_DAPP_DEADLINE_V1));
    EXPECT_TRUE(breaker.allows_invocation());
  }
  EXPECT_TRUE(breaker.observe(OCUDU_DAPP_DEADLINE_V1));
  EXPECT_FALSE(breaker.allows_invocation());
}

TEST(dapp_circuit_breaker, bypass_is_not_a_failure)
{
  circuit_breaker breaker({1});
  EXPECT_FALSE(breaker.observe(OCUDU_DAPP_BYPASS_V1));
  EXPECT_EQ(breaker.consecutive_failures(), 0U);
}

TEST(dapp_circuit_breaker, unsupported_is_accounted_as_fallback_without_opening)
{
  circuit_breaker breaker({1});
  EXPECT_FALSE(breaker.observe(OCUDU_DAPP_UNSUPPORTED_V1));
  EXPECT_EQ(breaker.state(), breaker_state::closed);
  EXPECT_EQ(breaker.consecutive_failures(), 0U);
}

TEST(dapp_circuit_breaker, only_authorized_probe_can_reclose)
{
  circuit_breaker breaker;
  breaker.force_open();
  EXPECT_TRUE(breaker.begin_authorized_probe());
  EXPECT_EQ(breaker.state(), breaker_state::half_open);
  breaker.close_after_successful_probe();
  EXPECT_EQ(breaker.state(), breaker_state::closed);
}

TEST(dapp_circuit_breaker, failed_half_open_probe_reopens_immediately)
{
  circuit_breaker breaker({100});
  breaker.force_open();
  ASSERT_TRUE(breaker.begin_authorized_probe());
  EXPECT_TRUE(breaker.observe(OCUDU_DAPP_INVALID_OUTPUT_V1));
  EXPECT_EQ(breaker.state(), breaker_state::open);
}

TEST(dapp_circuit_breaker, force_close_clears_latch_and_failures)
{
  circuit_breaker breaker({2});
  EXPECT_FALSE(breaker.observe(OCUDU_DAPP_DEADLINE_V1));
  EXPECT_TRUE(breaker.observe(OCUDU_DAPP_DEADLINE_V1));
  EXPECT_EQ(breaker.state(), breaker_state::open);
  EXPECT_EQ(breaker.consecutive_failures(), 2U);
  breaker.force_close();
  EXPECT_EQ(breaker.state(), breaker_state::closed);
  EXPECT_EQ(breaker.consecutive_failures(), 0U);
  EXPECT_TRUE(breaker.allows_invocation());
}
