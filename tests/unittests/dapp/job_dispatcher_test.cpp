// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/runtime/job_dispatcher.h"
#include <cstring>
#include <future>
#include <gtest/gtest.h>

using namespace ocudu::dapp;
using namespace ocudu::dapp::management;
using namespace std::chrono_literals;

namespace {

identifier job_test_id(uint64_t value)
{
  identifier result;
  std::memcpy(result.bytes.data() + 8, &value, sizeof(value));
  return result;
}

operation_result success(identifier transaction)
{
  operation_result result;
  result.code           = result_code::ok;
  result.transaction_id = transaction;
  return result;
}

} // namespace

TEST(dapp_job_dispatcher, executes_and_retains_successful_result)
{
  job_dispatcher dispatcher;
  const auto     transaction = job_test_id(1);
  auto           submitted   = dispatcher.submit(transaction, 10, [transaction] { return success(transaction); });
  ASSERT_TRUE(submitted && submitted.job_id);
  auto completed = dispatcher.wait(*submitted.job_id, std::chrono::steady_clock::now() + 1s);
  ASSERT_TRUE(completed && completed->result);
  EXPECT_EQ(completed->state, job_state::succeeded);
  EXPECT_EQ(completed->progress_percent, 100U);
  EXPECT_EQ(completed->result->code, result_code::ok);
}

TEST(dapp_job_dispatcher, transaction_replay_returns_same_job_and_mismatch_is_rejected)
{
  job_dispatcher dispatcher;
  const auto     transaction = job_test_id(2);
  auto           first       = dispatcher.submit(transaction, 20, [transaction] { return success(transaction); });
  ASSERT_TRUE(first && first.job_id);
  auto replay = dispatcher.submit(transaction, 20, [] { return operation_result{}; });
  EXPECT_EQ(replay.code, job_submit_code::replayed);
  EXPECT_EQ(replay.job_id, first.job_id);
  auto mismatch = dispatcher.submit(transaction, 21, [] { return operation_result{}; });
  EXPECT_EQ(mismatch.code, job_submit_code::transaction_mismatch);
}

TEST(dapp_job_dispatcher, exception_is_contained_as_failed_job)
{
  job_dispatcher dispatcher;
  auto           submitted = dispatcher.submit(
      job_test_id(3), 30, []() -> operation_result { throw std::runtime_error("job fixture exception"); });
  ASSERT_TRUE(submitted && submitted.job_id);
  auto completed = dispatcher.wait(*submitted.job_id, std::chrono::steady_clock::now() + 1s);
  ASSERT_TRUE(completed && completed->result);
  EXPECT_EQ(completed->state, job_state::failed);
  EXPECT_EQ(completed->result->code, result_code::lifecycle_failed);
  EXPECT_EQ(completed->detail, "job fixture exception");
}

TEST(dapp_job_dispatcher, accepted_queued_job_can_be_cancelled_without_running)
{
  job_dispatcher     dispatcher(2, 8);
  std::promise<void> running;
  std::promise<void> release;
  auto               release_future = release.get_future().share();
  auto               first          = dispatcher.submit(job_test_id(4), 40, [&, release_future] {
    running.set_value();
    release_future.wait();
    return success(job_test_id(4));
  });
  ASSERT_TRUE(first);
  running.get_future().wait();

  bool second_ran = false;
  auto second     = dispatcher.submit(job_test_id(5), 50, [&] {
    second_ran = true;
    return success(job_test_id(5));
  });
  ASSERT_TRUE(second && second.job_id);
  EXPECT_TRUE(dispatcher.cancel(*second.job_id));
  release.set_value();
  auto cancelled = dispatcher.wait(*second.job_id, std::chrono::steady_clock::now() + 1s);
  ASSERT_TRUE(cancelled);
  EXPECT_EQ(cancelled->state, job_state::cancelled);
  EXPECT_FALSE(second_ran);
}

TEST(dapp_job_dispatcher, stop_rejects_new_work)
{
  job_dispatcher dispatcher;
  dispatcher.stop();
  auto submitted = dispatcher.submit(job_test_id(6), 60, [] { return operation_result{}; });
  EXPECT_EQ(submitted.code, job_submit_code::stopping);
}
