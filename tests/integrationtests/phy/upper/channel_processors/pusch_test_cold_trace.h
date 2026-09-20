// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear
#pragma once

#include "fmt/format.h"
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace ocudu::test_support {

/// Offline-harness evidence only. No CUDA calls, production hooks or frame-timer instrumentation.
/// Each record is flushed so a killed process retains the last entered setup/teardown phase.
class pusch_cold_trace
{
public:
  explicit pusch_cold_trace(const std::string& path)
  {
    if (!path.empty()) {
      output = std::fopen(path.c_str(), "wx");
      if (!output) {
        throw std::runtime_error("A new writable cold-trace JSONL path is required.");
      }
    }
  }
  pusch_cold_trace(const pusch_cold_trace&)            = delete;
  pusch_cold_trace& operator=(const pusch_cold_trace&) = delete;
  ~pusch_cold_trace()
  {
    if (output)
      std::fclose(output);
  }

  template <typename Function>
  auto measure(const char* phase, Function&& function)
  {
    static_assert(!std::is_reference_v<std::invoke_result_t<Function>>, "Trace value/void operations only");
    record(phase, "begin");
    try {
      if constexpr (std::is_void_v<std::invoke_result_t<Function>>) {
        std::forward<Function>(function)();
        record(phase, "end");
      } else {
        auto result = std::forward<Function>(function)();
        record(phase, "end");
        return result;
      }
    } catch (...) {
      // Preserve the operation's exception even if evidence storage also fails.
      try {
        record(phase, "failed");
      } catch (...) {
      }
      throw;
    }
  }

  void record(const char* phase, const char* boundary)
  {
    if (!output)
      return;
    const auto elapsed = std::chrono::steady_clock::now() - start;
    const auto epoch   = std::chrono::system_clock::now().time_since_epoch();
    // phase/boundary are fixed internal labels, never CLI-controlled strings.
    fmt::print(output,
               "{{\"kind\":\"cold_phase\",\"sequence\":{},\"phase\":\"{}\",\"boundary\":\"{}\","
               "\"epoch_s\":{},\"elapsed_ns\":{}}}\n",
               sequence++,
               phase,
               boundary,
               std::chrono::duration<double>(epoch).count(),
               std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
    if (std::fflush(output) || std::ferror(output)) {
      throw std::runtime_error("Writing PUSCH cold trace failed.");
    }
  }

private:
  FILE*                                 output   = nullptr;
  unsigned                              sequence = 0;
  std::chrono::steady_clock::time_point start    = std::chrono::steady_clock::now();
};
} // namespace ocudu::test_support
