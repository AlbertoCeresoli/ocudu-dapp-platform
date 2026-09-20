// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear
#include "pusch_test_receiver_retirement_trace.h"
#include "ocudu/dapp/runtime/native_worker.h"
#include <algorithm>
#include <cuda_runtime_api.h>
#include <gtest/gtest.h>

namespace {
unsigned                                   query_calls{}, inspect_calls{}, release_calls{};
cudaStream_t                               expected_stream{};
ocudu::dapp::native_worker*                expected_worker{};
const ocudu::dapp::native_receiver_ticket* expected_ticket{};
} // namespace

extern "C" cudaError_t __real_cudaStreamQuery(cudaStream_t stream)
{
  ++query_calls;
  EXPECT_EQ(stream, expected_stream);
  return cudaErrorNotReady;
}
extern "C" cudaError_t __wrap_cudaStreamQuery(cudaStream_t);
ocudu_dapp_status_v1
real_receiver_inspect(ocudu::dapp::native_worker*,
                      const ocudu::dapp::native_receiver_ticket&,
                      ocudu_dapp_receiver_completion_result_v1&) asm("__real_" PUSCH_TEST_INSPECT_SYMBOL);
ocudu_dapp_status_v1
                     wrap_receiver_inspect(ocudu::dapp::native_worker*,
                                           const ocudu::dapp::native_receiver_ticket&,
                                           ocudu_dapp_receiver_completion_result_v1&) asm("__wrap_" PUSCH_TEST_INSPECT_SYMBOL);
ocudu_dapp_status_v1 real_receiver_inspect(ocudu::dapp::native_worker*                worker,
                                           const ocudu::dapp::native_receiver_ticket& ticket,
                                           ocudu_dapp_receiver_completion_result_v1&  output)
{
  ++inspect_calls;
  EXPECT_EQ(worker, expected_worker);
  EXPECT_EQ(&ticket, expected_ticket);
  output.ready = 1;
  output.valid = 0;
  return OCUDU_DAPP_DEVICE_ERROR_V1;
}
ocudu_dapp_status_v1
real_receiver_release(ocudu::dapp::native_worker*,
                      const ocudu::dapp::native_receiver_ticket&) asm("__real_" PUSCH_TEST_RELEASE_SYMBOL);
ocudu_dapp_status_v1
                     wrap_receiver_release(ocudu::dapp::native_worker*,
                                           const ocudu::dapp::native_receiver_ticket&) asm("__wrap_" PUSCH_TEST_RELEASE_SYMBOL);
ocudu_dapp_status_v1 real_receiver_release(ocudu::dapp::native_worker*                worker,
                                           const ocudu::dapp::native_receiver_ticket& ticket)
{
  ++release_calls;
  EXPECT_EQ(worker, expected_worker);
  EXPECT_EQ(&ticket, expected_ticket);
  return OCUDU_DAPP_RESOURCE_V1;
}

namespace {
void check_forwarding(unsigned count)
{
  ocudu::dapp::native_receiver_ticket ticket{17, 23};
  expected_ticket = &ticket;
  // CPU fakes only: never dereferenced or handed to CUDA/native module code.
  expected_stream = reinterpret_cast<cudaStream_t>(uintptr_t{1});
  expected_worker = reinterpret_cast<ocudu::dapp::native_worker*>(uintptr_t{2});
  query_calls = inspect_calls = release_calls = 0;
  for (unsigned i = 0; i != count; ++i) {
    EXPECT_EQ(__wrap_cudaStreamQuery(expected_stream), cudaErrorNotReady);
    ocudu_dapp_receiver_completion_result_v1 result{};
    EXPECT_EQ(wrap_receiver_inspect(expected_worker, ticket, result), OCUDU_DAPP_DEVICE_ERROR_V1);
    EXPECT_EQ(result.ready, 1U);
    EXPECT_EQ(result.valid, 0U);
    EXPECT_EQ(wrap_receiver_release(expected_worker, ticket), OCUDU_DAPP_RESOURCE_V1);
  }
  EXPECT_EQ(query_calls, count);
  EXPECT_EQ(inspect_calls, count);
  EXPECT_EQ(release_calls, count);
  EXPECT_EQ(ticket.worker_generation, 17U);
  EXPECT_EQ(ticket.sequence, 23U);
}
} // namespace

TEST(pusch_test_receiver_retirement_trace, disabled_trace_preserves_calls_and_does_not_write)
{
  ocudu::test_support::enable_receiver_retirement_trace(false);
  testing::internal::CaptureStderr();
  check_forwarding(3);
  EXPECT_TRUE(testing::internal::GetCapturedStderr().empty());
}

TEST(pusch_test_receiver_retirement_trace, enabled_trace_is_bounded_and_never_changes_failed_statuses)
{
  ocudu::test_support::enable_receiver_retirement_trace(true);
  testing::internal::CaptureStderr();
  check_forwarding(200);
  const auto output = testing::internal::GetCapturedStderr();
  ocudu::test_support::enable_receiver_retirement_trace(false);
  EXPECT_EQ(std::count(output.begin(), output.end(), '\n'), 128);
  EXPECT_NE(output.find("phase=stream_query"), std::string::npos);
  EXPECT_NE(output.find("phase=inspect"), std::string::npos);
  EXPECT_NE(output.find("phase=release"), std::string::npos);
}
