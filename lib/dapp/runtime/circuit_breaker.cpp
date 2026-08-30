// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/runtime/circuit_breaker.h"

using namespace ocudu::dapp;

bool circuit_breaker::observe(ocudu_dapp_status_v1 status) noexcept
{
  if (status == OCUDU_DAPP_OK_V1) {
    if (state() == breaker_state::closed) {
      failures.store(0, std::memory_order_relaxed);
    }
    return false;
  }
  if (status == OCUDU_DAPP_BYPASS_V1 || status == OCUDU_DAPP_UNSUPPORTED_V1) {
    return false;
  }

  const bool immediate = state() == breaker_state::half_open || status == OCUDU_DAPP_EXCEPTION_V1 ||
                         status == OCUDU_DAPP_DEVICE_ERROR_V1 || status == OCUDU_DAPP_INTERNAL_ERROR_V1;
  const auto count = failures.fetch_add(1, std::memory_order_relaxed) + 1;
  if (!immediate && count < configured_policy.recoverable_failure_threshold) {
    return false;
  }

  breaker_state expected = current_state.load(std::memory_order_relaxed);
  while (expected != breaker_state::open) {
    if (current_state.compare_exchange_weak(
            expected, breaker_state::open, std::memory_order_release, std::memory_order_relaxed)) {
      return true;
    }
  }
  return false;
}

bool circuit_breaker::begin_authorized_probe() noexcept
{
  breaker_state expected = breaker_state::open;
  return current_state.compare_exchange_strong(
      expected, breaker_state::half_open, std::memory_order_acq_rel, std::memory_order_relaxed);
}

void circuit_breaker::close_after_successful_probe() noexcept
{
  failures.store(0, std::memory_order_relaxed);
  current_state.store(breaker_state::closed, std::memory_order_release);
}

void circuit_breaker::force_open() noexcept
{
  current_state.store(breaker_state::open, std::memory_order_release);
}

void circuit_breaker::force_close() noexcept
{
  failures.store(0, std::memory_order_relaxed);
  current_state.store(breaker_state::closed, std::memory_order_release);
}
