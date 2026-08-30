// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace ocudu::dapp {

enum class activity_kind : uint8_t { invocation, lease, callback, accelerator_work, count };

struct quiescence_snapshot {
  uint32_t invocations{};
  uint32_t leases{};
  uint32_t callbacks{};
  uint32_t accelerator_work{};
  bool     empty() const noexcept { return invocations == 0 && leases == 0 && callbacks == 0 && accelerator_work == 0; }
};

class quiescence_tracker;

class activity_token
{
public:
  activity_token() noexcept = default;
  activity_token(activity_token&& other) noexcept;
  activity_token& operator=(activity_token&& other) noexcept;
  ~activity_token();

  activity_token(const activity_token&)            = delete;
  activity_token& operator=(const activity_token&) = delete;

  explicit operator bool() const noexcept { return owner != nullptr; }
  void     reset() noexcept;

private:
  friend class quiescence_tracker;
  activity_token(quiescence_tracker* owner_, activity_kind kind_) noexcept : owner(owner_), kind(kind_) {}

  quiescence_tracker* owner{};
  activity_kind       kind{activity_kind::invocation};
};

class quiescence_tracker
{
public:
  activity_token try_acquire(activity_kind kind) noexcept;
  void           close_admission() noexcept;
  void           open_admission() noexcept;
  bool           admission_open() const noexcept { return accepting.load(std::memory_order_acquire); }

  quiescence_snapshot snapshot() const noexcept;
  bool                wait_until_empty(std::chrono::steady_clock::time_point deadline);

private:
  friend class activity_token;
  void release(activity_kind kind) noexcept;

  static constexpr std::size_t             count = static_cast<std::size_t>(activity_kind::count);
  std::array<std::atomic<uint32_t>, count> active{};
  std::atomic<bool>                        accepting{false};
  mutable std::mutex                       wait_mutex;
  std::condition_variable                  wait_condition;
};

} // namespace ocudu::dapp
