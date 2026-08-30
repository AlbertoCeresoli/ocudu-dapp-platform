// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "ocudu/dapp/management/types.h"
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace ocudu::dapp::e3 {

/// Behavior when a subscription queue is full: drop the incoming object, drop
/// the oldest queued object, or replace the newest queued object so a slow
/// consumer always sees the freshest one.
enum class portable_drop_policy : uint8_t { drop_newest, drop_oldest, coalesce_latest };

/// Sentinel for a subscription that leaves the producer's default capture interval in force.
inline constexpr uint64_t portable_interval_unspecified = std::numeric_limits<uint64_t>::max();

/// One leased subscription on a store interface. Renewing is an identical
/// create; polling renews the lease.
struct portable_subscription_request {
  management::identifier subscription_id;
  /// Store interface (Class-C 0x0001xxxx or telemetry 0x0002xxxx).
  uint32_t interface_id{};
  /// Optional cell filter; absent admits every cell.
  std::optional<uint32_t> cell_id;
  /// Keep one object out of every N admitted (1 = all).
  uint32_t sample_every_n{1};
  /// Reserved queue depth in 64 KiB chunks.
  uint32_t queue_depth_chunks{64};
  /// Largest tensor chunk returned by a poll, in bytes.
  uint32_t maximum_chunk_bytes{64 * 1024};
  /// Liveness lease in microseconds; the subscription lapses when not polled
  /// or renewed within it.
  uint64_t             lease_us{1'000'000};
  portable_drop_policy drop_policy{portable_drop_policy::drop_newest};
  /// Capture interval this subscriber asks the producer for, in microseconds.
  /// The producer's effective per-RNTI floor follows the fastest active request
  /// (zero = every event) bounded below by its configured hard floor.
  uint64_t requested_interval_us{portable_interval_unspecified};
};

struct portable_subscription_record {
  portable_subscription_request request;
  uint64_t                      lease_expiry_ns{};
  uint64_t                      queued_chunks{};
  uint64_t                      published_objects{};
  uint64_t                      published_tensor_bytes{};
  uint64_t                      dropped_objects{};
  uint64_t                      busy_drops{};
  uint64_t                      sampled_objects{};
  bool                          active{};
  /// Absolute center frequency of the measurement window and the signed
  /// offset of the cell's center relative to it; zero when unconfigured.
  uint64_t rf_center_frequency_hz{};
  int64_t  cell_frequency_offset_hz{};
};

/// One chunk of a published object. Clients reassemble by
/// (route_epoch, publication_sequence) with contiguous chunk_index; chunk 0
/// carries the metadata, later chunks leave it empty.
struct portable_publication_chunk {
  management::identifier subscription_id;
  uint32_t               interface_id{};
  uint32_t               cell_id{};
  uint64_t               publication_sequence{};
  /// Producer-instance epoch; changes when the DU restarts.
  uint64_t route_epoch{};
  uint32_t chunk_index{};
  uint32_t chunk_count{};
  /// Complete tensor size in bytes (not this chunk's size).
  uint64_t tensor_size{};
  /// Cumulative drop counter of the subscription at publication time.
  uint64_t             dropped_objects{};
  std::vector<uint8_t> metadata;
  std::vector<uint8_t> tensor_chunk;
};

enum class portable_subscription_code : uint8_t { ok, invalid_request, duplicate, not_found, resource_exhausted, busy };

struct portable_subscription_result {
  portable_subscription_code                code{portable_subscription_code::invalid_request};
  std::vector<portable_subscription_record> subscriptions;
  std::string                               detail;
  explicit operator bool() const noexcept { return code == portable_subscription_code::ok; }
};

struct portable_poll_result {
  portable_subscription_code                  code{portable_subscription_code::invalid_request};
  std::vector<portable_publication_chunk>     chunks;
  std::optional<portable_subscription_record> subscription;
  std::string                                 detail;
  explicit operator bool() const noexcept { return code == portable_subscription_code::ok; }
};

const char* to_string(portable_subscription_code code) noexcept;

} // namespace ocudu::dapp::e3
