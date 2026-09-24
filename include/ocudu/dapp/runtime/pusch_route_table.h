// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "ocudu/dapp/management/pusch_routes.h"
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace ocudu::dapp {

/// Maps a native interface identifier onto its routable PUSCH hook; nullopt for any other interface.
std::optional<pusch_route_hook> pusch_route_hook_for_interface(uint32_t interface_id) noexcept;

/// Outcome of one per-grant lookup: which hook-point slot serves the grant.
///
/// \c primary keeps the behaviour of a host without routing: slot 0 of every hook, with the usual
/// hook exclusivity rules. \c conventional selects the stock receiver. \c instance selects exactly
/// one Class-A instance, identified by its hook and hook-point slot.
struct pusch_route {
  enum class kind : uint8_t { primary, conventional, instance };

  kind             type = kind::primary;
  pusch_route_hook hook = pusch_route_hook::receiver;
  uint8_t          slot = 0;

  static constexpr pusch_route primary() noexcept { return {}; }
  static constexpr pusch_route conventional() noexcept { return {kind::conventional, pusch_route_hook::receiver, 0}; }
  static constexpr pusch_route instance(pusch_route_hook hook_, uint8_t slot_) noexcept
  {
    return {kind::instance, hook_, slot_};
  }
  bool operator==(const pusch_route& other) const noexcept
  {
    return type == other.type && (type != kind::instance || (hook == other.hook && slot == other.slot));
  }
  bool operator!=(const pusch_route& other) const noexcept { return !(*this == other); }
};

struct pusch_route_result {
  management::result_code code = management::result_code::ok;
  std::string             detail;
  uint64_t                generation = 0;
  explicit                operator bool() const noexcept { return code == management::result_code::ok; }
};

/// Per-UE routing of Class-A PUSCH grants between resident dApp instances and the stock receiver.
///
/// Lookups never lock or allocate: a grant reads one of two fixed-size compiled snapshots under a
/// reader count. Writers are serialized, rebuild only the snapshot no reader holds, and publish it
/// with one store. Entries name instances, not slots: the hook registry reports bindings and every
/// change recompiles the snapshot, so an entry follows its instance across deactivation and
/// reactivation and never reaches another instance that later takes the same slot. An entry whose
/// instance is not bound selects the stock receiver.
class pusch_route_table
{
public:
  static constexpr std::size_t max_entries        = pusch_route_max_entries;
  static constexpr uint8_t     max_slots_per_hook = pusch_route_max_slots_per_hook;

  /// \param slots_per_hook Hook points per routable interface (1 keeps the single-owner topology).
  explicit pusch_route_table(uint8_t slots_per_hook);

  pusch_route_table(const pusch_route_table&)            = delete;
  pusch_route_table& operator=(const pusch_route_table&) = delete;

  uint8_t slots_per_hook() const noexcept { return slots; }

  /// Hot path: the route of one grant. Lock free, allocation free, bounded (two binary searches).
  pusch_route resolve(uint32_t cell_index, uint16_t rnti) const noexcept;

  /// Replaces the default and every entry in one step. With \p validate_only nothing is applied.
  pusch_route_result replace(const pusch_route_target&             default_target,
                             const std::vector<pusch_route_entry>& entries,
                             bool                                  validate_only = false);
  /// Inserts or overwrites the given entries; an entry targeting \c primary removes that UE's entry.
  pusch_route_result update(const std::vector<pusch_route_entry>& entries, bool validate_only = false);
  /// Removes every entry and resets the default to \c primary.
  pusch_route_result clear();
  pusch_route_state  state() const;

  /// Hook registry notifications, called with the registry's binding mutex held. A failed bind means
  /// the snapshot could not be republished; the registry must then refuse the activation, so a slot
  /// is never served under a stale route snapshot.
  bool bind(const management::identifier& instance_id, pusch_route_hook hook, uint8_t slot) noexcept;
  void unbind(const management::identifier& instance_id) noexcept;

private:
  static constexpr uint16_t any_cell = 0xFFFFU;

  struct compiled_entry {
    uint32_t    key = 0; ///< (cell << 16) | rnti; cell 0xFFFF matches every cell.
    pusch_route route;
    bool        unavailable = false;
  };
  struct snapshot {
    pusch_route                             default_route;
    bool                                    default_unavailable = false;
    uint32_t                                size                = 0;
    std::array<compiled_entry, max_entries> entries{};
  };

  pusch_route_result    publish_locked(pusch_route_target default_target, std::vector<pusch_route_entry> entries);
  pusch_route_result    validate_unique(std::vector<pusch_route_entry> entries) const;
  pusch_route_result    validate_locked(const pusch_route_target& target) const;
  bool                  compile_locked(snapshot& output) const noexcept;
  bool                  wait_for_readers(uint32_t buffer) const noexcept;
  static uint32_t       key_of(const pusch_route_entry& entry) noexcept;
  const compiled_entry* find(const snapshot& table, uint32_t key) const noexcept;

  const uint8_t slots;

  // Control plane, serialized by `mutex`.
  mutable std::mutex               mutex;
  pusch_route_target               configured_default;
  std::vector<pusch_route_entry>   configured_entries; ///< Sorted by key, unique.
  std::vector<pusch_route_binding> bindings;
  uint64_t                         generation = 0;

  // Data plane.
  std::array<snapshot, 2>                      buffers;
  std::atomic<uint32_t>                        current{0};
  mutable std::array<std::atomic<uint32_t>, 2> readers{};
  mutable std::atomic<uint64_t>                unavailable{0};
};

} // namespace ocudu::dapp
