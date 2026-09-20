// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear
#pragma once

#include <cstdlib>
#include <exception>
#include <execinfo.h>
#include <unistd.h>

namespace ocudu::test_support {

/// Isolated Linux test diagnostic, not a signal handler or a production recovery
/// mechanism. Print bounded stack depth, then preserve abnormal termination.
/// Addresses are resolved offline against the exact pinned executable. The
/// failing process must never resume or release potentially borrowed storage.
[[noreturn]] inline void trace_test_termination() noexcept
{
  constexpr char              begin[]       = "PUSCH_TEST_TERMINATE_STACK_BEGIN\n";
  constexpr char              end[]         = "PUSCH_TEST_TERMINATE_STACK_END\n";
  [[maybe_unused]] const auto begin_written = ::write(STDERR_FILENO, begin, sizeof(begin) - 1);
  void*                       frames[48]{};
  const int                   count = ::backtrace(frames, 48);
  ::backtrace_symbols_fd(frames, count, STDERR_FILENO);
  [[maybe_unused]] const auto end_written = ::write(STDERR_FILENO, end, sizeof(end) - 1);
  std::abort();
}

inline void enable_test_termination_trace() noexcept
{
  // Resolve the unwinder before CUDA setup, not on the first receiver grant.
  // This is diagnostics, not a guarantee of allocation-free crash handling.
  void* frame[1]{};
  static_cast<void>(::backtrace(frame, 1));
  std::set_terminate(&trace_test_termination);
}
} // namespace ocudu::test_support
