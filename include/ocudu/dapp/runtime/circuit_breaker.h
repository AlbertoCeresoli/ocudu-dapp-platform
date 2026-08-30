// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "ocudu/dapp/abi/v1/base.h"
#include <atomic>
#include <cstdint>

namespace ocudu::dapp {

enum class breaker_state : uint8_t { closed, open, half_open };

struct breaker_policy {
  // Deadline / invalid-output / resource are recoverable. OTA Class-A EQ
  // (PBC under multi-UE + channel-emul + spectrum) can drip a few 1–2 ms
  // overruns in a burst without being broken; 3 was latching good lanes off.
  uint32_t recoverable_failure_threshold{8};
};

class circuit_breaker
{
public:
  explicit circuit_breaker(breaker_policy policy = {}) noexcept : configured_policy(policy) {}

  bool          allows_invocation() const noexcept { return state() != breaker_state::open; }
  breaker_state state() const noexcept { return current_state.load(std::memory_order_acquire); }
  uint32_t      consecutive_failures() const noexcept { return failures.load(std::memory_order_relaxed); }

  /// Returns true exactly when this observation opens the breaker.
  bool observe(ocudu_dapp_status_v1 status) noexcept;
  bool begin_authorized_probe() noexcept;
  void close_after_successful_probe() noexcept;
  void force_open() noexcept;
  /// Operator reset: close the latch and clear consecutive failures.
  void force_close() noexcept;

private:
  breaker_policy             configured_policy;
  std::atomic<breaker_state> current_state{breaker_state::closed};
  std::atomic<uint32_t>      failures{0};
};

} // namespace ocudu::dapp
