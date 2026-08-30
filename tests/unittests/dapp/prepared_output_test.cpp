// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/runtime/prepared_output.h"
#include <array>
#include <gtest/gtest.h>

using namespace ocudu::dapp;

namespace {

struct capture {
  uint32_t               commits{};
  uint32_t               value{};
  prepared_output_source source{};
  ocudu_dapp_status_v1   status{OCUDU_DAPP_OK_V1};
};

ocudu_dapp_status_v1
commit(void* destination, const void* object, uint32_t size, const prepared_output_source& source) noexcept
{
  auto* output = static_cast<capture*>(destination);
  if (output == nullptr || object == nullptr || size != sizeof(uint32_t)) {
    return OCUDU_DAPP_INVALID_OUTPUT_V1;
  }
  ++output->commits;
  output->value  = *static_cast<const uint32_t*>(object);
  output->source = source;
  return output->status;
}

prepared_output_source source()
{
  prepared_output_source result{};
  result.identity.instance_id[0]     = 17;
  result.identity.app_generation     = 3;
  result.identity.hook_id            = 0x10001;
  result.invocation.invocation_id    = 41;
  result.invocation.input_generation = 9;
  return result;
}

} // namespace

TEST(dapp_prepared_output, commits_copied_records_only_after_explicit_success)
{
  prepared_output_transaction transaction;
  capture                     output;
  const uint32_t              value = 77;
  {
    prepared_output_scope scope(&transaction, source());
    ASSERT_NE(current_prepared_output_source(), nullptr);
    ASSERT_EQ(stage_prepared_output(1, &output, commit, &value, sizeof(value)), OCUDU_DAPP_OK_V1);
    EXPECT_EQ(output.commits, 0U);
    ASSERT_EQ(transaction.commit(), OCUDU_DAPP_OK_V1);
  }
  EXPECT_EQ(current_prepared_output_source(), nullptr);
  EXPECT_EQ(output.commits, 1U);
  EXPECT_EQ(output.value, value);
  EXPECT_EQ(output.source.identity.instance_id[0], 17U);
  EXPECT_EQ(output.source.invocation.invocation_id, 41U);
}

TEST(dapp_prepared_output, aborts_uncommitted_records_and_rejects_duplicate_or_unscoped_staging)
{
  prepared_output_transaction transaction;
  capture                     output;
  const uint32_t              value = 99;
  EXPECT_EQ(stage_prepared_output(1, &output, commit, &value, sizeof(value)), OCUDU_DAPP_CONFLICT_V1);
  {
    prepared_output_scope scope(&transaction, source());
    ASSERT_EQ(stage_prepared_output(1, &output, commit, &value, sizeof(value)), OCUDU_DAPP_OK_V1);
    EXPECT_EQ(stage_prepared_output(1, &output, commit, &value, sizeof(value)), OCUDU_DAPP_CONFLICT_V1);
  }
  EXPECT_EQ(output.commits, 0U);
  EXPECT_EQ(transaction.commit(), OCUDU_DAPP_INVALID_INPUT_V1);
}

TEST(dapp_prepared_output, propagates_commit_backpressure_without_running_later_records)
{
  prepared_output_transaction transaction;
  capture                     first;
  capture                     second;
  first.status                = OCUDU_DAPP_BYPASS_V1;
  const uint32_t        value = 5;
  prepared_output_scope scope(&transaction, source());
  ASSERT_EQ(stage_prepared_output(1, &first, commit, &value, sizeof(value)), OCUDU_DAPP_OK_V1);
  ASSERT_EQ(stage_prepared_output(2, &second, commit, &value, sizeof(value)), OCUDU_DAPP_OK_V1);
  EXPECT_EQ(transaction.commit(), OCUDU_DAPP_BYPASS_V1);
  EXPECT_EQ(first.commits, 1U);
  EXPECT_EQ(second.commits, 0U);
}
