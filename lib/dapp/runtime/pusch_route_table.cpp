// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/runtime/pusch_route_table.h"
#include "ocudu/dapp/use_cases/v1/channel_estimator.h"
#include "ocudu/dapp/use_cases/v1/equalizer.h"
#include "ocudu/dapp/use_cases/v1/receiver.h"
#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <thread>

using namespace ocudu::dapp;

namespace {

/// Highest C-RNTI (TS 38.321 Table 7.1-1); larger values are reserved and never carry PUSCH data.
constexpr uint16_t max_c_rnti = 0xFFEFU;
/// A reader holds a snapshot only for two binary searches; this bound only guards against a
/// descheduled reader and is never reached in practice.
constexpr auto reader_drain_timeout = std::chrono::seconds(1);

pusch_route_result failure(management::result_code code, const char* detail)
{
  return {code, detail, 0};
}

} // namespace

std::optional<pusch_route_hook> ocudu::dapp::pusch_route_hook_for_interface(uint32_t interface_id) noexcept
{
  switch (interface_id) {
    case OCUDU_DAPP_INTERFACE_CHANNEL_ESTIMATOR_V1:
      return pusch_route_hook::channel_estimator;
    case OCUDU_DAPP_INTERFACE_EQUALIZER_V1:
      return pusch_route_hook::equalizer;
    case OCUDU_DAPP_INTERFACE_RECEIVER_V1:
      return pusch_route_hook::receiver;
    default:
      return std::nullopt;
  }
}

pusch_route_table::pusch_route_table(uint8_t slots_per_hook) : slots(slots_per_hook)
{
  if (slots == 0 || slots > max_slots_per_hook) {
    throw std::invalid_argument("PUSCH route table needs between 1 and 8 slots per hook");
  }
}

uint32_t pusch_route_table::key_of(const pusch_route_entry& entry) noexcept
{
  return (static_cast<uint32_t>(entry.cell_index.value_or(any_cell)) << 16U) | entry.rnti;
}

const pusch_route_table::compiled_entry* pusch_route_table::find(const snapshot& table, uint32_t key) const noexcept
{
  const auto* first = table.entries.data();
  const auto* last  = first + table.size;
  const auto* found =
      std::lower_bound(first, last, key, [](const compiled_entry& entry, uint32_t value) { return entry.key < value; });
  return found != last && found->key == key ? found : nullptr;
}

pusch_route pusch_route_table::resolve(uint32_t cell_index, uint16_t rnti) const noexcept
{
  // Pin one snapshot: a writer never rebuilds a buffer whose reader count is nonzero, and the
  // re-check rejects a buffer that stopped being current between the load and the increment.
  uint32_t index = current.load(std::memory_order_acquire);
  for (;;) {
    readers[index].fetch_add(1, std::memory_order_seq_cst);
    const uint32_t confirmed = current.load(std::memory_order_seq_cst);
    if (confirmed == index) {
      break;
    }
    readers[index].fetch_sub(1, std::memory_order_release);
    index = confirmed;
  }

  const snapshot&       table = buffers[index];
  const compiled_entry* entry = nullptr;
  if (table.size != 0) {
    if (cell_index < any_cell) {
      entry = find(table, (cell_index << 16U) | rnti);
    }
    if (entry == nullptr) {
      entry = find(table, (static_cast<uint32_t>(any_cell) << 16U) | rnti);
    }
  }
  const pusch_route route = entry != nullptr ? entry->route : table.default_route;
  const bool        stale = entry != nullptr ? entry->unavailable : table.default_unavailable;
  readers[index].fetch_sub(1, std::memory_order_release);

  if (stale) {
    unavailable.fetch_add(1, std::memory_order_relaxed);
  }
  return route;
}

pusch_route_result pusch_route_table::validate_locked(const pusch_route_target& target) const
{
  if (target.type != pusch_route_target::kind::instance) {
    return {};
  }
  if (target.instance_id.is_zero()) {
    return failure(management::result_code::invalid_request, "an instance route needs an instance identifier");
  }
  const bool bound = std::any_of(bindings.begin(), bindings.end(), [&](const pusch_route_binding& binding) {
    return binding.instance_id == target.instance_id;
  });
  return bound ? pusch_route_result{}
               : failure(management::result_code::not_found,
                         "the instance is not active on a channel estimator, equalizer or receiver hook");
}

bool pusch_route_table::compile_locked(snapshot& output) const noexcept
{
  const auto compile = [this](const pusch_route_target& target, pusch_route& route, bool& unavailable_route) {
    unavailable_route = false;
    switch (target.type) {
      case pusch_route_target::kind::primary:
        route = pusch_route::primary();
        return;
      case pusch_route_target::kind::conventional:
        route = pusch_route::conventional();
        return;
      case pusch_route_target::kind::instance:
        break;
    }
    const auto binding = std::find_if(bindings.begin(), bindings.end(), [&](const pusch_route_binding& candidate) {
      return candidate.instance_id == target.instance_id;
    });
    if (binding == bindings.end()) {
      route             = pusch_route::conventional();
      unavailable_route = true;
      return;
    }
    route = pusch_route::instance(binding->hook, binding->slot);
  };

  if (configured_entries.size() > max_entries) {
    return false;
  }
  compile(configured_default, output.default_route, output.default_unavailable);
  output.size = static_cast<uint32_t>(configured_entries.size());
  for (std::size_t index = 0; index != configured_entries.size(); ++index) {
    auto& compiled = output.entries[index];
    compiled.key   = key_of(configured_entries[index]);
    compile(configured_entries[index].target, compiled.route, compiled.unavailable);
  }
  return true;
}

bool pusch_route_table::wait_for_readers(uint32_t buffer) const noexcept
{
  const auto deadline = std::chrono::steady_clock::now() + reader_drain_timeout;
  while (readers[buffer].load(std::memory_order_seq_cst) != 0) {
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    std::this_thread::yield();
  }
  return true;
}

pusch_route_result pusch_route_table::validate_unique(std::vector<pusch_route_entry> entries) const
{
  std::sort(entries.begin(), entries.end(), [](const pusch_route_entry& lhs, const pusch_route_entry& rhs) {
    return key_of(lhs) < key_of(rhs);
  });
  const auto duplicate = std::adjacent_find(
      entries.begin(), entries.end(), [](const auto& lhs, const auto& rhs) { return key_of(lhs) == key_of(rhs); });
  if (duplicate != entries.end()) {
    return failure(management::result_code::invalid_request, "the table names one UE more than once");
  }
  return {management::result_code::ok, {}, generation};
}

pusch_route_result pusch_route_table::publish_locked(pusch_route_target             default_target,
                                                     std::vector<pusch_route_entry> entries)
{
  if (auto unique = validate_unique(entries); !unique) {
    return unique;
  }
  // Build the new configuration aside; commit it only once the snapshot is published.
  std::sort(entries.begin(), entries.end(), [](const pusch_route_entry& lhs, const pusch_route_entry& rhs) {
    return key_of(lhs) < key_of(rhs);
  });

  std::swap(configured_default, default_target);
  std::swap(configured_entries, entries);
  const uint32_t next = current.load(std::memory_order_relaxed) ^ 1U;
  if (!wait_for_readers(next) || !compile_locked(buffers[next])) {
    std::swap(configured_default, default_target);
    std::swap(configured_entries, entries);
    return failure(management::result_code::resource_exhausted, "the route snapshot could not be republished");
  }
  current.store(next, std::memory_order_seq_cst);
  return {management::result_code::ok, {}, ++generation};
}

pusch_route_result pusch_route_table::replace(const pusch_route_target&             default_target,
                                              const std::vector<pusch_route_entry>& entries,
                                              bool                                  validate_only)
{
  if (entries.size() > max_entries) {
    return failure(management::result_code::resource_exhausted, "the route table holds at most 512 UEs");
  }
  try {
    std::lock_guard<std::mutex> lock(mutex);
    if (auto checked = validate_locked(default_target); !checked) {
      return checked;
    }
    std::vector<pusch_route_entry> table;
    table.reserve(entries.size());
    for (const auto& entry : entries) {
      if (entry.rnti == 0 || entry.rnti > max_c_rnti || (entry.cell_index && *entry.cell_index >= any_cell)) {
        return failure(management::result_code::invalid_request, "an entry has an invalid RNTI or cell index");
      }
      if (auto checked = validate_locked(entry.target); !checked) {
        return checked;
      }
      // A primary entry is the default behaviour; storing it would only shadow the default.
      if (entry.target.type != pusch_route_target::kind::primary) {
        table.push_back(entry);
      }
    }
    if (validate_only) {
      return validate_unique(table);
    }
    return publish_locked(default_target, std::move(table));
  } catch (const std::bad_alloc&) {
    return failure(management::result_code::resource_exhausted, "unable to allocate the route table");
  }
}

pusch_route_result pusch_route_table::update(const std::vector<pusch_route_entry>& entries, bool validate_only)
{
  if (entries.size() > max_entries) {
    return failure(management::result_code::resource_exhausted, "the route table holds at most 512 UEs");
  }
  try {
    std::lock_guard<std::mutex>    lock(mutex);
    std::vector<pusch_route_entry> table = configured_entries;
    for (const auto& entry : entries) {
      if (entry.rnti == 0 || entry.rnti > max_c_rnti || (entry.cell_index && *entry.cell_index >= any_cell)) {
        return failure(management::result_code::invalid_request, "an entry has an invalid RNTI or cell index");
      }
      if (auto checked = validate_locked(entry.target); !checked) {
        return checked;
      }
      const uint32_t key      = key_of(entry);
      const auto     existing = std::find_if(
          table.begin(), table.end(), [key](const pusch_route_entry& candidate) { return key_of(candidate) == key; });
      if (entry.target.type == pusch_route_target::kind::primary) {
        if (existing != table.end()) {
          table.erase(existing);
        }
      } else if (existing != table.end()) {
        existing->target = entry.target;
      } else {
        table.push_back(entry);
      }
    }
    if (table.size() > max_entries) {
      return failure(management::result_code::resource_exhausted, "the route table holds at most 512 UEs");
    }
    if (validate_only) {
      return validate_unique(table);
    }
    return publish_locked(configured_default, std::move(table));
  } catch (const std::bad_alloc&) {
    return failure(management::result_code::resource_exhausted, "unable to allocate the route table");
  }
}

pusch_route_result pusch_route_table::clear()
{
  try {
    std::lock_guard<std::mutex> lock(mutex);
    return publish_locked({}, {});
  } catch (const std::bad_alloc&) {
    return failure(management::result_code::resource_exhausted, "unable to clear the route table");
  }
}

pusch_route_state pusch_route_table::state() const
{
  std::lock_guard<std::mutex> lock(mutex);
  pusch_route_state           view;
  view.generation          = generation;
  view.slots_per_hook      = slots;
  view.default_target      = configured_default;
  view.entries             = configured_entries;
  view.bindings            = bindings;
  view.unavailable_lookups = unavailable.load(std::memory_order_relaxed);
  return view;
}

bool pusch_route_table::bind(const management::identifier& instance_id, pusch_route_hook hook, uint8_t slot) noexcept
{
  if (instance_id.is_zero() || slot >= slots) {
    return false;
  }
  try {
    std::lock_guard<std::mutex> lock(mutex);
    const auto                  previous = bindings;
    bindings.erase(std::remove_if(bindings.begin(),
                                  bindings.end(),
                                  [&](const pusch_route_binding& binding) {
                                    return binding.instance_id == instance_id ||
                                           (binding.hook == hook && binding.slot == slot);
                                  }),
                   bindings.end());
    bindings.push_back({instance_id, hook, slot});
    if (!publish_locked(configured_default, configured_entries)) {
      bindings = previous;
      return false;
    }
    return true;
  } catch (...) {
    return false;
  }
}

void pusch_route_table::unbind(const management::identifier& instance_id) noexcept
{
  try {
    std::lock_guard<std::mutex> lock(mutex);
    const auto                  before = bindings.size();
    bindings.erase(
        std::remove_if(bindings.begin(),
                       bindings.end(),
                       [&](const pusch_route_binding& binding) { return binding.instance_id == instance_id; }),
        bindings.end());
    if (bindings.size() != before) {
      // If this republish fails, grants still routed to the freed slot find it empty and use the stock
      // receiver; the next bind of that slot republishes or is refused.
      static_cast<void>(publish_locked(configured_default, configured_entries));
    }
  } catch (...) {
  }
}
