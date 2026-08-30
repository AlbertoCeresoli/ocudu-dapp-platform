// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/runtime/lifecycle.h"
#include <gtest/gtest.h>

using namespace ocudu::dapp;

TEST(dapp_lifecycle, follows_nominal_activation_and_retirement_sequence)
{
  lifecycle state;
  EXPECT_TRUE(state.transition_to(OCUDU_DAPP_STATE_LOADED_V1));
  EXPECT_TRUE(state.transition_to(OCUDU_DAPP_STATE_PREPARED_V1));
  EXPECT_TRUE(state.transition_to(OCUDU_DAPP_STATE_ARMED_V1));
  EXPECT_TRUE(state.transition_to(OCUDU_DAPP_STATE_SHADOW_V1));
  EXPECT_TRUE(state.transition_to(OCUDU_DAPP_STATE_ACTIVE_V1));
  EXPECT_TRUE(state.transition_to(OCUDU_DAPP_STATE_DRAINING_V1));
  EXPECT_TRUE(state.transition_to(OCUDU_DAPP_STATE_INACTIVE_V1));
  EXPECT_TRUE(state.transition_to(OCUDU_DAPP_STATE_UNLOADED_V1));
}

TEST(dapp_lifecycle, illegal_transition_is_non_mutating)
{
  lifecycle state;
  EXPECT_FALSE(state.transition_to(OCUDU_DAPP_STATE_ACTIVE_V1));
  EXPECT_EQ(state.state(), OCUDU_DAPP_STATE_DISCOVERED_V1);
}

TEST(dapp_lifecycle, failure_is_available_from_every_live_state)
{
  lifecycle state(OCUDU_DAPP_STATE_ACTIVE_V1);
  EXPECT_TRUE(state.transition_to(OCUDU_DAPP_STATE_ERROR_V1));
  EXPECT_TRUE(state.transition_to(OCUDU_DAPP_STATE_DRAINING_V1));
}

TEST(dapp_lifecycle, quiescence_timeout_enters_resident_quarantine)
{
  lifecycle state(OCUDU_DAPP_STATE_DRAINING_V1);
  EXPECT_TRUE(state.transition_to(OCUDU_DAPP_STATE_QUARANTINED_RESIDENT_V1));
  EXPECT_FALSE(state.transition_to(OCUDU_DAPP_STATE_UNLOADED_V1));
  EXPECT_TRUE(state.transition_to(OCUDU_DAPP_STATE_INACTIVE_V1));
  EXPECT_TRUE(state.transition_to(OCUDU_DAPP_STATE_UNLOADED_V1));
}
