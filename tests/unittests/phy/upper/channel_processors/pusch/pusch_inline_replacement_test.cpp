// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear
#include "pusch_inline_replacement.h"
#include <gtest/gtest.h>
#include <vector>

using namespace ocudu;

TEST(pusch_inline_replacement, overwrite_permission_is_independent_of_status)
{
  for (auto status : {OCUDU_DAPP_OK_V1,
                      OCUDU_DAPP_BYPASS_V1,
                      OCUDU_DAPP_UNSUPPORTED_V1,
                      OCUDU_DAPP_INVALID_INPUT_V1,
                      OCUDU_DAPP_RESOURCE_V1,
                      OCUDU_DAPP_DEVICE_ERROR_V1,
                      OCUDU_DAPP_EXCEPTION_V1}) {
    for (bool fallback : {false, true}) {
      pusch_inline_replacement state;
      state.invoked({status, fallback});
      EXPECT_EQ(state.must_discard(), status != OCUDU_DAPP_OK_V1 && !fallback);
    }
  }
}

TEST(pusch_inline_replacement, selected_continuation_failure_forbids_overwrite)
{
  pusch_inline_replacement selected;
  selected.invoked({OCUDU_DAPP_OK_V1, false});
  EXPECT_FALSE(selected.must_discard());
  selected.continuation_failed();
  EXPECT_TRUE(selected.must_discard());
  pusch_inline_replacement declined;
  declined.invoked({OCUDU_DAPP_BYPASS_V1, true});
  declined.continuation_failed();
  EXPECT_FALSE(declined.must_discard());
  pusch_inline_replacement uncertain;
  uncertain.invoked({OCUDU_DAPP_DEVICE_ERROR_V1, false});
  uncertain.continuation_failed();
  EXPECT_TRUE(uncertain.must_discard());
}

TEST(pusch_inline_replacement, drains_every_distinct_stream_and_retains_failure)
{
  for (unsigned failed = 0; failed != 8; ++failed) {
    std::vector<unsigned> calls;
    const bool            complete = drain_inline_streams(std::array<unsigned, 3>{1, 2, 3}, [&](unsigned stream) {
      calls.push_back(stream);
      return (failed & (1U << (stream - 1))) == 0;
    });
    EXPECT_EQ(calls, (std::vector<unsigned>{1, 2, 3}));
    EXPECT_EQ(complete, failed == 0);
  }
  unsigned calls = 0;
  EXPECT_TRUE(drain_inline_streams(std::array<unsigned, 3>{1, 0, 1}, [&](unsigned stream) {
    EXPECT_EQ(stream, 1U);
    ++calls;
    return true;
  }));
  EXPECT_EQ(calls, 1U);
}
