// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "ocudu/dapp/management/types.h"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

/// Per-UE routing of Class-A PUSCH grants: value types shared by the host route table
/// (ocudu/dapp/runtime/pusch_route_table.h) and E3 management clients (puschRouting).
namespace ocudu::dapp {

/// Entries a route table holds (a full table stays below one 16 KiB APER open-type fragment).
inline constexpr std::size_t pusch_route_max_entries = 512;
/// Hook points one Class-A PUSCH hook can hold.
inline constexpr uint8_t pusch_route_max_slots_per_hook = 8;

/// Class-A PUSCH hooks a route can select (channel estimator, equalizer, receiver).
enum class pusch_route_hook : uint8_t { channel_estimator, equalizer, receiver };
inline constexpr std::size_t nof_pusch_route_hooks = 3;

/// Where an operator sends a UE: the host default, the stock receiver, or one Class-A instance.
struct pusch_route_target {
  enum class kind : uint8_t { primary, conventional, instance };

  kind                   type = kind::primary;
  management::identifier instance_id{};

  bool operator==(const pusch_route_target& other) const noexcept
  {
    return type == other.type && (type != kind::instance || instance_id == other.instance_id);
  }
};

/// One configured UE. Without a cell index the entry applies to that RNTI in every cell of the DU.
struct pusch_route_entry {
  std::optional<uint16_t> cell_index;
  uint16_t                rnti = 0;
  pusch_route_target      target;
};

/// A Class-A instance currently bound to a routable hook point.
struct pusch_route_binding {
  management::identifier instance_id{};
  pusch_route_hook       hook = pusch_route_hook::receiver;
  uint8_t                slot = 0;
};

/// Control-plane view of the table.
struct pusch_route_state {
  uint64_t                         generation     = 0;
  uint8_t                          slots_per_hook = 1;
  pusch_route_target               default_target;
  std::vector<pusch_route_entry>   entries;
  std::vector<pusch_route_binding> bindings;
  /// Grants whose entry named an instance that was not bound at the time; they used the stock receiver.
  uint64_t unavailable_lookups = 0;
};

} // namespace ocudu::dapp
