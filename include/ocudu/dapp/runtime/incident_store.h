// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "incident.h"
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <vector>

namespace ocudu::dapp {

struct incident_filter {
  std::optional<std::array<uint64_t, 2>> instance_id;
  std::optional<incident_cause>          cause;
  std::optional<incident_phase>          phase;
  std::optional<uint64_t>                after_incident_id;
  std::size_t                            maximum_records{1024};
};

/// Bounded, management-thread incident history. No hot callback writes here directly.
class incident_store
{
public:
  explicit incident_store(std::size_t capacity = 4096, uint64_t deduplication_window_ns = 1'000'000'000ULL);

  void ingest(const incident_record& incident);

  template <std::size_t Capacity>
  std::size_t drain(incident_ring<Capacity>& source)
  {
    std::size_t     count = 0;
    incident_record record;
    while (source.try_pop(record)) {
      ingest(record);
      ++count;
    }
    account_source_drops(source.take_unreported_drops());
    return count;
  }

  std::vector<incident_record>   query(const incident_filter& filter = {}) const;
  std::optional<incident_record> get(uint64_t incident_id) const;

  /// Clears queryable history through the acknowledged ID; cumulative health remains unchanged.
  std::size_t clear_through(uint64_t acknowledged_incident_id);

  incident_health_summary health() const;
  std::size_t             size() const;
  /// Accounts records dropped by an external bounded incident source.
  void account_source_drops(uint64_t dropped_delta);

private:
  bool same_recurrence(const incident_record& left, const incident_record& right) const noexcept;
  void increment_health(const incident_record& incident) noexcept;

  const std::size_t           maximum_records;
  const uint64_t              dedup_window_ns;
  mutable std::mutex          mutex;
  std::deque<incident_record> records;
  incident_health_summary     cumulative;
};

} // namespace ocudu::dapp
