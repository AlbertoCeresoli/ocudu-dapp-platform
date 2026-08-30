// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/runtime/quiescence_tracker.h"
#include <limits>

using namespace ocudu::dapp;

activity_token::activity_token(activity_token&& other) noexcept : owner(other.owner), kind(other.kind)
{
  other.owner = nullptr;
}

activity_token& activity_token::operator=(activity_token&& other) noexcept
{
  if (this != &other) {
    reset();
    owner       = other.owner;
    kind        = other.kind;
    other.owner = nullptr;
  }
  return *this;
}

activity_token::~activity_token()
{
  reset();
}

void activity_token::reset() noexcept
{
  if (owner != nullptr) {
    owner->release(kind);
    owner = nullptr;
  }
}

activity_token quiescence_tracker::try_acquire(activity_kind kind) noexcept
{
  const auto index = static_cast<std::size_t>(kind);
  if (index >= count || !accepting.load(std::memory_order_acquire)) {
    return {};
  }

  uint32_t current = active[index].load(std::memory_order_relaxed);
  do {
    if (current == std::numeric_limits<uint32_t>::max()) {
      return {};
    }
  } while (
      !active[index].compare_exchange_weak(current, current + 1, std::memory_order_acquire, std::memory_order_relaxed));

  if (!accepting.load(std::memory_order_acquire)) {
    release(kind);
    return {};
  }
  return activity_token(this, kind);
}

void quiescence_tracker::close_admission() noexcept
{
  accepting.store(false, std::memory_order_release);
}

void quiescence_tracker::open_admission() noexcept
{
  accepting.store(true, std::memory_order_release);
}

quiescence_snapshot quiescence_tracker::snapshot() const noexcept
{
  return {active[static_cast<std::size_t>(activity_kind::invocation)].load(std::memory_order_acquire),
          active[static_cast<std::size_t>(activity_kind::lease)].load(std::memory_order_acquire),
          active[static_cast<std::size_t>(activity_kind::callback)].load(std::memory_order_acquire),
          active[static_cast<std::size_t>(activity_kind::accelerator_work)].load(std::memory_order_acquire)};
}

bool quiescence_tracker::wait_until_empty(std::chrono::steady_clock::time_point deadline)
{
  std::unique_lock<std::mutex> lock(wait_mutex);
  return wait_condition.wait_until(lock, deadline, [this] { return snapshot().empty(); });
}

void quiescence_tracker::release(activity_kind kind) noexcept
{
  const auto index = static_cast<std::size_t>(kind);
  if (index >= count) {
    return;
  }
  const uint32_t previous = active[index].fetch_sub(1, std::memory_order_release);
  if (previous == 1 && !accepting.load(std::memory_order_relaxed)) {
    wait_condition.notify_all();
  }
}
