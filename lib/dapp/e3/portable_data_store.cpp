// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/e3/portable_data_store.h"
#include "ocudu/dapp/e3/telemetry_types.h"
#include "ocudu/dapp/use_cases/v1/spectrum.h"
#include "ocudu/dapp/use_cases/v1/srs_isac.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <utility>

using namespace ocudu::dapp::e3;

namespace {

uint64_t monotonic_ns() noexcept
{
  const auto value =
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
  return value > 0 ? static_cast<uint64_t>(value) : 1;
}

constexpr std::size_t nof_class_c_interfaces = 2;
constexpr std::size_t nof_telemetry_streams  = telemetry_stream_pusch_grid - telemetry_stream_pusch_symbols +
                                              1; // derived: adding a stream cannot leave the tables short
constexpr std::size_t nof_store_interfaces = nof_class_c_interfaces + nof_telemetry_streams;

/// Class-C v1 interfaces occupy the first slots; telemetry streams follow in
/// identifier order so every admitted interface has one active counter.
std::optional<std::size_t> interface_index(uint32_t interface_id) noexcept
{
  if (interface_id == OCUDU_DAPP_INTERFACE_SPECTRUM_V1) {
    return 0;
  }
  if (interface_id == OCUDU_DAPP_INTERFACE_SRS_ISAC_V1) {
    return 1;
  }
  if (is_telemetry_stream(interface_id)) {
    return nof_class_c_interfaces + (interface_id - telemetry_stream_pusch_symbols);
  }
  return std::nullopt;
}

bool same_request(const portable_subscription_request& left, const portable_subscription_request& right) noexcept
{
  return left.subscription_id == right.subscription_id && left.interface_id == right.interface_id &&
         left.cell_id == right.cell_id && left.sample_every_n == right.sample_every_n &&
         left.queue_depth_chunks == right.queue_depth_chunks && left.maximum_chunk_bytes == right.maximum_chunk_bytes &&
         left.lease_us == right.lease_us && left.drop_policy == right.drop_policy;
}

} // namespace

struct portable_data_store::implementation {
  struct queued_chunk {
    uint32_t             interface_id{};
    uint32_t             cell_id{};
    uint64_t             publication_sequence{};
    uint64_t             route_epoch{};
    uint32_t             chunk_index{};
    uint32_t             chunk_count{};
    uint64_t             tensor_size{};
    uint64_t             dropped_objects{};
    uint32_t             metadata_size{};
    uint32_t             tensor_chunk_size{};
    std::vector<uint8_t> storage;
  };

  struct subscription {
    subscription(portable_subscription_request request_, uint32_t metadata_capacity, uint64_t expiry) :
      request(std::move(request_)), lease_expiry_ns(expiry), queue(request.queue_depth_chunks)
    {
      const std::size_t slot_bytes = static_cast<std::size_t>(metadata_capacity) + request.maximum_chunk_bytes;
      for (auto& chunk : queue) {
        chunk.storage.resize(slot_bytes);
      }
    }

    portable_subscription_record snapshot() const noexcept
    {
      const uint64_t expiry = lease_expiry_ns.load(std::memory_order_acquire);
      return {request,
              expiry,
              queued_chunks.load(std::memory_order_relaxed),
              published_objects.load(std::memory_order_relaxed),
              published_tensor_bytes.load(std::memory_order_relaxed),
              dropped_objects.load(std::memory_order_relaxed),
              busy_drops.load(std::memory_order_relaxed),
              sampled_objects.load(std::memory_order_relaxed),
              expiry > monotonic_ns()};
    }

    portable_subscription_request request;
    std::atomic<uint64_t>         lease_expiry_ns{};
    std::atomic<uint64_t>         observed_objects{};
    std::atomic<uint64_t>         queued_chunks{};
    std::atomic<uint64_t>         published_objects{};
    std::atomic<uint64_t>         published_tensor_bytes{};
    std::atomic<uint64_t>         dropped_objects{};
    std::atomic<uint64_t>         busy_drops{};
    std::atomic<uint64_t>         sampled_objects{};
    std::mutex                    queue_mutex;
    std::vector<queued_chunk>     queue;
    std::size_t                   head{};
    std::size_t                   tail{};
    std::size_t                   count{};
  };

  explicit implementation(portable_data_store_config config_) : config(config_), slots(config.maximum_subscriptions)
  {
    for (auto& value : fastest_requested_us) {
      value.store(portable_interval_unspecified, std::memory_order_relaxed);
    }
  }

  uint64_t retained_bytes(const portable_subscription_request& request) const noexcept
  {
    const uint64_t slot_bytes = static_cast<uint64_t>(config.maximum_metadata_bytes) + request.maximum_chunk_bytes;
    return request.queue_depth_chunks > std::numeric_limits<uint64_t>::max() / slot_bytes
               ? std::numeric_limits<uint64_t>::max()
               : request.queue_depth_chunks * slot_bytes;
  }

  bool valid(const portable_subscription_request& request) const noexcept
  {
    const auto index = interface_index(request.interface_id);
    return !request.subscription_id.is_zero() && index && (!request.cell_id || *request.cell_id < 32) &&
           request.sample_every_n != 0 && request.sample_every_n <= 65535 && request.queue_depth_chunks != 0 &&
           request.queue_depth_chunks <= 1024 && request.maximum_chunk_bytes >= config.minimum_chunk_bytes &&
           request.maximum_chunk_bytes <= config.maximum_chunk_bytes && request.lease_us >= config.minimum_lease_us &&
           request.lease_us <= config.maximum_lease_us;
    // A queue shorter than one maximum-size object is legal: publish_one drops
    // such objects as object_too_large instead of blocking, so small-object
    // streams can reserve proportionally less preallocated storage.
  }

  uint64_t expiry(uint64_t lease_us) const noexcept
  {
    const uint64_t now = monotonic_ns();
    return lease_us > (std::numeric_limits<uint64_t>::max() - now) / 1000U ? std::numeric_limits<uint64_t>::max()
                                                                           : now + lease_us * 1000U;
  }

  void renew(const std::shared_ptr<subscription>& item, uint64_t expiry_ns) noexcept
  {
    if (item->lease_expiry_ns.exchange(expiry_ns, std::memory_order_acq_rel) == 0) {
      const auto index = *interface_index(item->request.interface_id);
      active_counts[index].fetch_add(1, std::memory_order_release);
      refresh_demand(index);
    }
  }

  void deactivate(const std::shared_ptr<subscription>& item) noexcept
  {
    if (item->lease_expiry_ns.exchange(0, std::memory_order_acq_rel) != 0) {
      const auto index = *interface_index(item->request.interface_id);
      active_counts[index].fetch_sub(1, std::memory_order_release);
      refresh_demand(index);
    }
  }

  bool expire_if_due(const std::shared_ptr<subscription>& item, uint64_t now) noexcept
  {
    uint64_t observed = item->lease_expiry_ns.load(std::memory_order_acquire);
    while (observed != 0 && observed <= now) {
      if (item->lease_expiry_ns.compare_exchange_weak(
              observed, 0, std::memory_order_acq_rel, std::memory_order_acquire)) {
        const auto index = *interface_index(item->request.interface_id);
        active_counts[index].fetch_sub(1, std::memory_order_release);
        refresh_demand(index);
        return true;
      }
    }
    return observed == 0;
  }

  portable_subscription_result subscribe(const portable_subscription_request& request) noexcept
  {
    if (!valid(request)) {
      return {portable_subscription_code::invalid_request, {}, "invalid portable E3 subscription bounds"};
    }
    try {
      std::lock_guard<std::mutex> lock(control_mutex);
      std::size_t                 empty = slots.size();
      uint64_t                    retained{};
      for (std::size_t index = 0; index != slots.size(); ++index) {
        auto current = std::atomic_load_explicit(&slots[index], std::memory_order_acquire);
        if (!current) {
          if (empty == slots.size()) {
            empty = index;
          }
          continue;
        }
        const uint64_t now = monotonic_ns();
        if (current->lease_expiry_ns.load(std::memory_order_acquire) == 0 || expire_if_due(current, now)) {
          std::atomic_store_explicit(&slots[index], std::shared_ptr<subscription>{}, std::memory_order_release);
          if (empty == slots.size()) {
            empty = index;
          }
          continue;
        }
        retained = retained > std::numeric_limits<uint64_t>::max() - retained_bytes(current->request)
                       ? std::numeric_limits<uint64_t>::max()
                       : retained + retained_bytes(current->request);
        if (current->request.subscription_id == request.subscription_id) {
          if (!same_request(current->request, request)) {
            return {portable_subscription_code::duplicate, {}, "subscription ID names different parameters"};
          }
          renew(current, expiry(request.lease_us));
          return {portable_subscription_code::ok, {current->snapshot()}, {}};
        }
      }
      const uint64_t requested = retained_bytes(request);
      if (empty == slots.size() || retained > config.maximum_retained_bytes ||
          requested > config.maximum_retained_bytes - retained) {
        return {portable_subscription_code::resource_exhausted, {}, "portable E3 subscription storage exhausted"};
      }
      auto created = std::make_shared<subscription>(request, config.maximum_metadata_bytes, expiry(request.lease_us));
      std::atomic_store_explicit(&slots[empty], created, std::memory_order_release);
      const auto index = *interface_index(request.interface_id);
      active_counts[index].fetch_add(1, std::memory_order_release);
      refresh_demand(index);
      return {portable_subscription_code::ok, {created->snapshot()}, {}};
    } catch (const std::bad_alloc&) {
      return {portable_subscription_code::resource_exhausted, {}, "portable E3 queue allocation failed"};
    } catch (...) {
      return {portable_subscription_code::invalid_request, {}, "unknown portable E3 subscription failure"};
    }
  }

  portable_subscription_result unsubscribe(const management::identifier& id) noexcept
  {
    if (id.is_zero()) {
      return {portable_subscription_code::invalid_request, {}, "subscription ID is required"};
    }
    try {
      std::lock_guard<std::mutex> lock(control_mutex);
      for (auto& slot : slots) {
        auto current = std::atomic_load_explicit(&slot, std::memory_order_acquire);
        if (current && current->request.subscription_id == id) {
          deactivate(current);
          const auto snapshot = current->snapshot();
          std::atomic_store_explicit(&slot, std::shared_ptr<subscription>{}, std::memory_order_release);
          return {portable_subscription_code::ok, {snapshot}, {}};
        }
      }
      return {portable_subscription_code::not_found, {}, "portable E3 subscription does not exist"};
    } catch (...) {
      return {portable_subscription_code::invalid_request, {}, "unknown portable E3 unsubscribe failure"};
    }
  }

  portable_subscription_result subscriptions() const noexcept
  {
    try {
      portable_subscription_result result{portable_subscription_code::ok, {}, {}};
      result.subscriptions.reserve(slots.size());
      for (const auto& slot : slots) {
        auto current = std::atomic_load_explicit(&slot, std::memory_order_acquire);
        if (current) {
          result.subscriptions.push_back(current->snapshot());
        }
      }
      return result;
    } catch (const std::bad_alloc&) {
      return {portable_subscription_code::resource_exhausted, {}, "portable E3 subscription snapshot failed"};
    } catch (...) {
      return {portable_subscription_code::invalid_request, {}, "unknown portable E3 subscription query failure"};
    }
  }

  std::shared_ptr<subscription> find(const management::identifier& id) const noexcept
  {
    for (const auto& slot : slots) {
      auto current = std::atomic_load_explicit(&slot, std::memory_order_acquire);
      if (current && current->request.subscription_id == id) {
        return current;
      }
    }
    return {};
  }

  portable_poll_result poll(const management::identifier& id, uint32_t maximum_chunks) noexcept
  {
    if (id.is_zero() || maximum_chunks == 0 || maximum_chunks > 8) {
      return {portable_subscription_code::invalid_request, {}, {}, "portable E3 poll bound is invalid"};
    }
    auto current = find(id);
    if (!current) {
      return {portable_subscription_code::not_found, {}, {}, "portable E3 subscription does not exist"};
    }
    const uint64_t now = monotonic_ns();
    if (current->lease_expiry_ns.load(std::memory_order_acquire) == 0 || expire_if_due(current, now)) {
      return {portable_subscription_code::not_found, {}, current->snapshot(), "portable E3 subscription expired"};
    }
    try {
      std::unique_lock<std::mutex> lock(current->queue_mutex, std::try_to_lock);
      if (!lock.owns_lock()) {
        return {portable_subscription_code::busy, {}, current->snapshot(), "portable E3 publication queue is busy"};
      }
      portable_poll_result result{portable_subscription_code::ok, {}, {}, {}};
      const std::size_t    count = std::min<std::size_t>(maximum_chunks, current->count);
      result.chunks.reserve(count);
      std::size_t read = current->tail;
      for (std::size_t index = 0; index != count; ++index) {
        const queued_chunk&        source = current->queue[read];
        portable_publication_chunk chunk;
        chunk.subscription_id      = current->request.subscription_id;
        chunk.interface_id         = source.interface_id;
        chunk.cell_id              = source.cell_id;
        chunk.publication_sequence = source.publication_sequence;
        chunk.route_epoch          = source.route_epoch;
        chunk.chunk_index          = source.chunk_index;
        chunk.chunk_count          = source.chunk_count;
        chunk.tensor_size          = source.tensor_size;
        chunk.dropped_objects      = source.dropped_objects;
        chunk.metadata.assign(source.storage.begin(), source.storage.begin() + source.metadata_size);
        chunk.tensor_chunk.assign(source.storage.begin() + source.metadata_size,
                                  source.storage.begin() + source.metadata_size + source.tensor_chunk_size);
        result.chunks.push_back(std::move(chunk));
        read = (read + 1) % current->queue.size();
      }
      {
        std::lock_guard<std::mutex> control_lock(control_mutex);
        const bool                  registered = std::any_of(slots.begin(), slots.end(), [&](const auto& slot) {
          return std::atomic_load_explicit(&slot, std::memory_order_acquire) == current;
        });
        if (!registered) {
          return {portable_subscription_code::not_found, {}, {}, "portable E3 subscription was removed"};
        }
        renew(current, expiry(current->request.lease_us));
      }
      current->tail = read;
      current->count -= count;
      current->queued_chunks.store(current->count, std::memory_order_relaxed);
      result.subscription = current->snapshot();
      return result;
    } catch (const std::bad_alloc&) {
      return {portable_subscription_code::resource_exhausted,
              {},
              current->snapshot(),
              "portable E3 poll response allocation failed"};
    } catch (...) {
      return {portable_subscription_code::invalid_request, {}, current->snapshot(), "unknown portable E3 poll failure"};
    }
  }

  void drop_oldest_object(subscription& item) noexcept
  {
    if (item.count == 0) {
      return;
    }
    const auto& first     = item.queue[item.tail];
    const auto  sequence  = first.publication_sequence;
    const auto  epoch     = first.route_epoch;
    const auto  interface = first.interface_id;
    const auto  cell      = first.cell_id;
    do {
      item.tail = (item.tail + 1) % item.queue.size();
      --item.count;
    } while (item.count != 0 && item.queue[item.tail].publication_sequence == sequence &&
             item.queue[item.tail].route_epoch == epoch && item.queue[item.tail].interface_id == interface &&
             item.queue[item.tail].cell_id == cell);
    item.dropped_objects.fetch_add(1, std::memory_order_relaxed);
  }

  void make_room(subscription& item, std::size_t chunks) noexcept
  {
    if (item.request.drop_policy == portable_drop_policy::coalesce_latest) {
      while (item.count != 0) {
        drop_oldest_object(item);
      }
      item.head = item.tail;
      return;
    }
    while (item.queue.size() - item.count < chunks && item.count != 0) {
      drop_oldest_object(item);
    }
  }

  portable_publish_result publish_one(const std::shared_ptr<subscription>& item,
                                      const portable_publication_source&   source,
                                      uint64_t                             now) noexcept
  {
    if (item->lease_expiry_ns.load(std::memory_order_acquire) == 0) {
      return portable_publish_result::no_subscription;
    }
    if (expire_if_due(item, now)) {
      return portable_publish_result::no_subscription;
    }
    if (item->request.interface_id != source.interface_id ||
        (item->request.cell_id && *item->request.cell_id != source.cell_id)) {
      return portable_publish_result::no_subscription;
    }
    const uint64_t ordinal = item->observed_objects.fetch_add(1, std::memory_order_relaxed);
    if (ordinal % item->request.sample_every_n != 0) {
      item->sampled_objects.fetch_add(1, std::memory_order_relaxed);
      return portable_publish_result::sampled;
    }
    const std::size_t chunks =
        (source.tensor.size() + item->request.maximum_chunk_bytes - 1U) / item->request.maximum_chunk_bytes;
    if (chunks == 0 || chunks > item->queue.size()) {
      item->dropped_objects.fetch_add(1, std::memory_order_relaxed);
      return portable_publish_result::object_too_large;
    }
    std::unique_lock<std::mutex> lock(item->queue_mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
      item->busy_drops.fetch_add(1, std::memory_order_relaxed);
      item->dropped_objects.fetch_add(1, std::memory_order_relaxed);
      return portable_publish_result::busy;
    }
    if (item->lease_expiry_ns.load(std::memory_order_acquire) == 0) {
      return portable_publish_result::no_subscription;
    }
    if (item->queue.size() - item->count < chunks) {
      if (item->request.drop_policy == portable_drop_policy::drop_newest) {
        item->dropped_objects.fetch_add(1, std::memory_order_relaxed);
        return portable_publish_result::full;
      }
      make_room(*item, chunks);
    }
    const uint64_t dropped = item->dropped_objects.load(std::memory_order_relaxed);
    std::size_t    offset{};
    for (std::size_t chunk_index = 0; chunk_index != chunks; ++chunk_index) {
      queued_chunk& destination        = item->queue[item->head];
      destination.interface_id         = source.interface_id;
      destination.cell_id              = source.cell_id;
      destination.publication_sequence = source.publication_sequence;
      destination.route_epoch          = source.route_epoch;
      destination.chunk_index          = chunk_index;
      destination.chunk_count          = chunks;
      destination.tensor_size          = source.tensor.size();
      destination.dropped_objects      = dropped;
      destination.metadata_size        = chunk_index == 0 ? source.metadata.size() : 0;
      destination.tensor_chunk_size =
          std::min<std::size_t>(item->request.maximum_chunk_bytes, source.tensor.size() - offset);
      if (destination.metadata_size != 0) {
        std::memcpy(destination.storage.data(), source.metadata.data(), destination.metadata_size);
      }
      std::memcpy(destination.storage.data() + destination.metadata_size,
                  source.tensor.data() + offset,
                  destination.tensor_chunk_size);
      offset += destination.tensor_chunk_size;
      item->head = (item->head + 1) % item->queue.size();
      ++item->count;
    }
    item->queued_chunks.store(item->count, std::memory_order_relaxed);
    item->published_objects.fetch_add(1, std::memory_order_relaxed);
    item->published_tensor_bytes.fetch_add(source.tensor.size(), std::memory_order_relaxed);
    return portable_publish_result::published;
  }

  portable_publish_result publish(const portable_publication_source& source) noexcept
  {
    const auto index = interface_index(source.interface_id);
    if (!index || source.publication_sequence == 0 || source.route_epoch == 0 || source.metadata.empty() ||
        source.metadata.data() == nullptr || source.tensor.data() == nullptr ||
        source.metadata.size() > config.maximum_metadata_bytes || source.tensor.empty()) {
      return portable_publish_result::invalid;
    }
    if (source.tensor.size() > config.maximum_object_bytes) {
      return portable_publish_result::object_too_large;
    }
    portable_publish_result aggregate = portable_publish_result::no_subscription;
    const uint64_t          now       = monotonic_ns();
    for (const auto& slot : slots) {
      auto current = std::atomic_load_explicit(&slot, std::memory_order_acquire);
      if (!current) {
        continue;
      }
      const auto result = publish_one(current, source, now);
      if (result == portable_publish_result::published) {
        aggregate = result;
      } else if (aggregate != portable_publish_result::published &&
                 result != portable_publish_result::no_subscription && result != portable_publish_result::sampled) {
        aggregate = result;
      } else if (aggregate == portable_publish_result::no_subscription && result == portable_publish_result::sampled) {
        aggregate = result;
      }
    }
    return aggregate;
  }

  portable_data_store_config                              config;
  mutable std::mutex                                      control_mutex;
  std::vector<std::shared_ptr<subscription>>              slots;
  std::array<std::atomic<uint32_t>, nof_store_interfaces> active_counts{};
  /// Cached per-interface demand so the real-time gate (telemetry_publisher::should_collect)
  /// reads three atomics instead of scanning shared_ptr slots (whose atomic loads take a
  /// library mutex). Refreshed on every activation change, which only happens on control paths.
  std::array<std::atomic<uint32_t>, nof_store_interfaces> unspecified_counts{};
  std::array<std::atomic<uint64_t>, nof_store_interfaces> fastest_requested_us{};

  void refresh_demand(std::size_t index) noexcept
  {
    uint32_t unspecified = 0;
    uint64_t fastest     = portable_interval_unspecified;
    for (const auto& slot : slots) {
      auto current = std::atomic_load_explicit(&slot, std::memory_order_acquire);
      if (!current || current->lease_expiry_ns.load(std::memory_order_acquire) == 0) {
        continue;
      }
      const auto slot_index = interface_index(current->request.interface_id);
      if (!slot_index || *slot_index != index) {
        continue;
      }
      if (current->request.requested_interval_us == portable_interval_unspecified) {
        ++unspecified;
      } else {
        fastest = std::min(fastest, current->request.requested_interval_us);
      }
    }
    unspecified_counts[index].store(unspecified, std::memory_order_release);
    fastest_requested_us[index].store(fastest, std::memory_order_release);
  }
};

std::pair<std::shared_ptr<portable_data_store>, std::string>
portable_data_store::create(portable_data_store_config config) noexcept
{
  const bool valid = config.maximum_subscriptions != 0 && config.maximum_subscriptions <= 32 &&
                     config.maximum_retained_bytes >= 1024 * 1024 &&
                     config.maximum_retained_bytes <= 1024ULL * 1024 * 1024 && config.maximum_object_bytes != 0 &&
                     config.maximum_object_bytes <= 64 * 1024 * 1024 && config.maximum_metadata_bytes >= 1024 &&
                     config.maximum_metadata_bytes <= 1024 * 1024 && config.minimum_chunk_bytes >= 1024 &&
                     config.minimum_chunk_bytes <= config.maximum_chunk_bytes &&
                     config.maximum_chunk_bytes <= 1024 * 1024 && config.minimum_lease_us != 0 &&
                     config.minimum_lease_us <= config.maximum_lease_us && config.maximum_lease_us <= 60'000'000;
  if (!valid) {
    return {{}, "invalid portable E3 data-store bounds"};
  }
  try {
    return {std::shared_ptr<portable_data_store>(
                new portable_data_store(std::make_unique<implementation>(std::move(config)))),
            {}};
  } catch (const std::bad_alloc&) {
    return {{}, "unable to allocate portable E3 data store"};
  } catch (...) {
    return {{}, "unknown portable E3 data-store construction failure"};
  }
}

portable_data_store::portable_data_store(std::unique_ptr<implementation> state_) : state(std::move(state_)) {}
portable_data_store::~portable_data_store() = default;

portable_subscription_result portable_data_store::subscribe(const portable_subscription_request& request) noexcept
{
  return state->subscribe(request);
}

portable_subscription_result portable_data_store::unsubscribe(const management::identifier& id) noexcept
{
  return state->unsubscribe(id);
}

portable_subscription_result portable_data_store::subscriptions() const noexcept
{
  return state->subscriptions();
}

portable_poll_result portable_data_store::poll(const management::identifier& id, uint32_t maximum_chunks) noexcept
{
  return state->poll(id, maximum_chunks);
}

bool portable_data_store::has_active(uint32_t interface_id) const noexcept
{
  const auto index = interface_index(interface_id);
  return index && state->active_counts[*index].load(std::memory_order_acquire) != 0;
}

portable_interface_demand portable_data_store::demand(uint32_t interface_id) const noexcept
{
  // Real-time callers: three atomic loads, no slot scan, no library locks.
  portable_interface_demand result;
  const auto                index = interface_index(interface_id);
  if (!index) {
    return result;
  }
  result.active               = state->active_counts[*index].load(std::memory_order_acquire);
  result.unspecified          = state->unspecified_counts[*index].load(std::memory_order_acquire);
  result.fastest_requested_us = state->fastest_requested_us[*index].load(std::memory_order_acquire);
  return result;
}

portable_publish_result portable_data_store::publish(const portable_publication_source& source) noexcept
{
  return state->publish(source);
}

const char* ocudu::dapp::e3::to_string(portable_subscription_code code) noexcept
{
  static constexpr const char* values[] = {
      "ok", "invalid request", "duplicate", "not found", "resource exhausted", "busy"};
  const auto index = static_cast<unsigned>(code);
  return index < std::size(values) ? values[index] : "unknown portable subscription code";
}

const char* ocudu::dapp::e3::to_string(portable_publish_result result) noexcept
{
  static constexpr const char* values[] = {
      "published", "no subscription", "sampled", "full", "busy", "invalid", "object too large"};
  const auto index = static_cast<unsigned>(result);
  return index < std::size(values) ? values[index] : "unknown portable publication result";
}
