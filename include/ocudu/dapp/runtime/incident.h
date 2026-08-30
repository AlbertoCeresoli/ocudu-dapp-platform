// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "ocudu/dapp/management/incidents.h"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace ocudu::dapp {

class incident_sink
{
public:
  virtual ~incident_sink()                                        = default;
  virtual bool try_push(const incident_record& incident) noexcept = 0;
};

/// Single-producer/single-consumer bounded ring. Capacity must be a power of two.
template <std::size_t Capacity>
class incident_ring final : public incident_sink
{
  static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0, "capacity must be a power of two");

public:
  bool try_push(const incident_record& incident) noexcept override
  {
    const std::size_t current_head = head.load(std::memory_order_relaxed);
    if (current_head - tail.load(std::memory_order_acquire) == Capacity) {
      dropped_count.fetch_add(1, std::memory_order_relaxed);
      unreported_drops.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    entries[current_head & (Capacity - 1)] = incident;
    head.store(current_head + 1, std::memory_order_release);
    return true;
  }

  bool try_pop(incident_record& incident) noexcept
  {
    const std::size_t current_tail = tail.load(std::memory_order_relaxed);
    if (current_tail == head.load(std::memory_order_acquire)) {
      return false;
    }
    incident = entries[current_tail & (Capacity - 1)];
    tail.store(current_tail + 1, std::memory_order_release);
    return true;
  }

  uint64_t dropped() const noexcept { return dropped_count.load(std::memory_order_relaxed); }
  uint64_t take_unreported_drops() noexcept { return unreported_drops.exchange(0, std::memory_order_acq_rel); }

private:
  alignas(64) std::array<incident_record, Capacity> entries{};
  alignas(64) std::atomic<std::size_t> head{0};
  alignas(64) std::atomic<std::size_t> tail{0};
  std::atomic<uint64_t> dropped_count{0};
  std::atomic<uint64_t> unreported_drops{0};
};

incident_record make_incident(const incident_context& context,
                              incident_cause          cause,
                              incident_action         action,
                              ocudu_dapp_status_v1    status,
                              const char*             message,
                              bool                    breaker_open) noexcept;

} // namespace ocudu::dapp
