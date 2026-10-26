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

/// [srs-sharing] Resource-grid region a subscriber asks for (not in upstream). The store delivers only the objects of
/// the slots where (sfn * slots_per_frame + slot) mod slot_period == slot_offset, and of each one only the box
/// [first_symbol, first_symbol + nof_symbols) x [first_prb, first_prb + nof_prbs) on every receive port. A dApp that
/// overhears one SRS resource sets it from that resource's period, offset, symbols and PRBs, so it receives about 5 %
/// of the grid at 1/period of the slots instead of every uplink slot in full. The slot rule is consistent across an
/// SFN wrap when slot_period divides 1024 * slots_per_frame, which holds for every SRS periodicity of TS 38.331.
struct portable_grid_region {
  uint32_t slot_period{1};
  uint32_t slot_offset{};
  uint8_t  first_symbol{};
  uint8_t  nof_symbols{14};
  uint16_t first_prb{};
  uint16_t nof_prbs{1};

  bool operator==(const portable_grid_region& other) const noexcept
  {
    return slot_period == other.slot_period && slot_offset == other.slot_offset &&
           first_symbol == other.first_symbol && nof_symbols == other.nof_symbols && first_prb == other.first_prb &&
           nof_prbs == other.nof_prbs;
  }
  bool operator!=(const portable_grid_region& other) const noexcept { return !(*this == other); }
};

/// [srs-sharing] Layout of a resource-grid tensor handed to the store, so that it can apply a portable_grid_region
/// without decoding the producer's metadata: contiguous [port][symbol][subcarrier] elements of element_bytes bytes,
/// observed in radio slot (sfn, slot) of the given numerology.
struct portable_grid_layout {
  uint16_t sfn{};
  uint16_t slot{};
  uint8_t  numerology{};
  uint32_t ports{};
  uint32_t symbols{};
  uint32_t subcarriers{};
  uint32_t element_bytes{};
};

/// [srs-sharing] Part of the source grid an object actually carries after a region crop: the tensor is
/// [port][nof_symbols][nof_subcarriers], starting at first_symbol / first_subcarrier of the source grid. The window is
/// the requested box clipped to the grid, so it can be smaller than requested on a narrower carrier.
struct portable_grid_window {
  uint8_t  first_symbol{};
  uint8_t  nof_symbols{};
  uint32_t first_subcarrier{};
  uint32_t nof_subcarriers{};
};

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
  /// [srs-sharing] Optional resource-grid region (Class-C Spectrum only); absent delivers whole grids, as upstream.
  std::optional<portable_grid_region> grid_region;
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
  /// [srs-sharing] Present when the subscription's grid region cropped the object; the same on every chunk.
  std::optional<portable_grid_window> grid_window;
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
