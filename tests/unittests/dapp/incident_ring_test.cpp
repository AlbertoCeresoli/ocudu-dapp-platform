// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/runtime/incident.h"
#include <gtest/gtest.h>

using namespace ocudu::dapp;

TEST(dapp_incident_ring, preserves_order_and_reports_bounded_overflow)
{
  incident_ring<2> ring;
  incident_record  first{};
  incident_record  second{};
  incident_record  third{};
  first.incident_id  = 1;
  second.incident_id = 2;
  third.incident_id  = 3;

  EXPECT_TRUE(ring.try_push(first));
  EXPECT_TRUE(ring.try_push(second));
  EXPECT_FALSE(ring.try_push(third));
  EXPECT_EQ(ring.dropped(), 1U);

  incident_record output;
  ASSERT_TRUE(ring.try_pop(output));
  EXPECT_EQ(output.incident_id, 1U);
  ASSERT_TRUE(ring.try_pop(output));
  EXPECT_EQ(output.incident_id, 2U);
  EXPECT_FALSE(ring.try_pop(output));
}
