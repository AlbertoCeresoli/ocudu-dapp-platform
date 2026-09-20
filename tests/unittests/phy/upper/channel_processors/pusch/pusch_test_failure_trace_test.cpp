// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear
#include "pusch_test_failure_trace.h"
#include <csignal>
#include <gtest/gtest.h>
#include <stdexcept>

TEST(pusch_test_failure_trace, explicit_termination_emits_stack_and_remains_sigabrt)
{
  ASSERT_EXIT(
      {
        ocudu::test_support::enable_test_termination_trace();
        std::terminate();
      },
      ::testing::KilledBySignal(SIGABRT),
      "PUSCH_TEST_TERMINATE_STACK_BEGIN.*pusch_test_failure_trace_test.*PUSCH_TEST_TERMINATE_STACK_END");
}

TEST(pusch_test_failure_trace, noexcept_violation_emits_stack_and_cannot_resume)
{
  ASSERT_EXIT(
      {
        ocudu::test_support::enable_test_termination_trace();
        auto fail     = [] { throw std::runtime_error("injected test failure"); };
        auto boundary = [&]() noexcept { fail(); };
        boundary();
        std::_Exit(0);
      },
      ::testing::KilledBySignal(SIGABRT),
      "PUSCH_TEST_TERMINATE_STACK_BEGIN.*pusch_test_failure_trace_test.*PUSCH_TEST_TERMINATE_STACK_END");
}
