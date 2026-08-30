// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "management_protocol.h"
#include <algorithm>
#include <map>
#include <mutex>

namespace ocudu::dapp::e3 {

/// Bounded management-plane mirror of downstream E3AP associations.
///
/// Updates originate only on the SCTP server thread. Inventory queries may run
/// on another control thread, so snapshots take a short mutex; no radio-path
/// producer references this registry.
class e3_connection_registry
{
public:
  static constexpr std::size_t maximum_connections = 64;

  bool upsert(e3_connection_record record) noexcept
  {
    try {
      record.peer_address.resize(std::min<std::size_t>(record.peer_address.size(), 128));
      record.dapp_name.resize(std::min<std::size_t>(record.dapp_name.size(), 128));
      record.subscriptions.resize(std::min<std::size_t>(record.subscriptions.size(), 64));
      for (auto& subscription : record.subscriptions) {
        subscription.telemetry_identifiers.resize(std::min<std::size_t>(subscription.telemetry_identifiers.size(), 64));
      }
      std::lock_guard lock(mutex);
      auto            found = records.find(record.association_id);
      if (found == records.end() && records.size() >= maximum_connections) {
        return false;
      }
      records[record.association_id] = std::move(record);
      return true;
    } catch (...) {
      return false;
    }
  }

  void erase(uint64_t association_id) noexcept
  {
    try {
      std::lock_guard lock(mutex);
      records.erase(association_id);
    } catch (...) {
    }
  }

  std::vector<e3_connection_record> snapshot() const
  {
    std::lock_guard                   lock(mutex);
    std::vector<e3_connection_record> result;
    result.reserve(records.size());
    for (const auto& [id, record] : records) {
      static_cast<void>(id);
      result.push_back(record);
    }
    return result;
  }

private:
  mutable std::mutex                       mutex;
  std::map<uint64_t, e3_connection_record> records;
};

} // namespace ocudu::dapp::e3
