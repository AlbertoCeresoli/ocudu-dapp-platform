// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear
#pragma once

#include "ocudu/dapp/runtime/native_worker.h"
#include <array>

namespace ocudu {
/// Tracks the selected replacement's overwrite permission, independently of whether usable output exists.
class pusch_inline_replacement
{
public:
  void invoked(const dapp::native_invocation_result& result) noexcept
  {
    selected  = result.status == OCUDU_DAPP_OK_V1 && !result.invoke_fallback;
    forbidden = !selected && !result.invoke_fallback;
  }
  void continuation_failed() noexcept
  {
    // A selected module may already be writing symbols/LLRs or feeding a diagnostic side stream.
    forbidden = forbidden || selected;
  }
  bool must_discard() const noexcept { return forbidden; }

private:
  bool selected{}, forbidden{};
};

/// Exceptional path only. Drain every distinct producer/transfer stream, even if an earlier drain failed.
/// False does NOT authorize buffer release, pool reuse or a conventional overwrite.
template <typename Stream, typename Drain>
bool drain_inline_streams(const std::array<Stream, 3>& streams, Drain&& drain)
{
  bool complete = true;
  for (unsigned i = 0; i != streams.size(); ++i) {
    if (streams[i] == Stream{}) {
      continue;
    }
    bool duplicate = false;
    for (unsigned j = 0; j != i; ++j) {
      duplicate = duplicate || streams[j] == streams[i];
    }
    if (!duplicate) {
      complete = drain(streams[i]) && complete;
    }
  }
  return complete;
}
} // namespace ocudu
