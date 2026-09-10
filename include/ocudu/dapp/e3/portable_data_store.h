// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "portable_data_types.h"
#include "ocudu/adt/span.h"
#include <memory>
#include <string>

namespace ocudu::dapp::e3 {

struct portable_data_store_config {
  uint32_t maximum_subscriptions{32};
  uint64_t maximum_retained_bytes{128ULL * 1024 * 1024};
  uint32_t maximum_object_bytes{4 * 1024 * 1024};
  uint32_t maximum_metadata_bytes{64 * 1024};
  uint32_t minimum_chunk_bytes{4 * 1024};
  uint32_t maximum_chunk_bytes{64 * 1024};
  uint64_t minimum_lease_us{100'000};
  uint64_t maximum_lease_us{10'000'000};
  /// Total metadata + tensor bytes copied across all subscribers by one
  /// producer call. Whole objects beyond this budget are dropped (full).
  uint64_t maximum_publication_copy_bytes{8ULL * 1024 * 1024};
};

struct portable_interface_demand {
  uint32_t active{};
  uint32_t unspecified{};
  uint64_t fastest_requested_us{portable_interval_unspecified};
};

struct portable_publication_source {
  uint32_t                   interface_id{};
  uint32_t                   cell_id{};
  uint64_t                   publication_sequence{};
  uint64_t                   route_epoch{};
  ocudu::span<const uint8_t> metadata;
  ocudu::span<const uint8_t> tensor;
};

enum class portable_publish_result : uint8_t {
  published,
  no_subscription,
  sampled,
  full,
  busy,
  invalid,
  object_too_large
};

/// Bounded low-rate Class-C publication store shared by E3 transport adapters.
/// Radio producers never allocate, wait for a lock, or invoke a codec here.
class portable_data_store
{
public:
  static std::pair<std::shared_ptr<portable_data_store>, std::string>
  create(portable_data_store_config config) noexcept;

  ~portable_data_store();
  portable_data_store(const portable_data_store&)            = delete;
  portable_data_store& operator=(const portable_data_store&) = delete;

  portable_subscription_result subscribe(const portable_subscription_request& request) noexcept;
  portable_subscription_result unsubscribe(const management::identifier& subscription_id) noexcept;
  portable_subscription_result subscriptions() const noexcept;
  portable_poll_result         poll(const management::identifier& subscription_id, uint32_t maximum_chunks) noexcept;

  /// Constant-time disabled-path predicate used by immutable radio routing.
  bool has_active(uint32_t interface_id) const noexcept;

  /// Active-subscriber demand for one interface: how many subscribers are live,
  /// how many left the capture interval unspecified, and the fastest interval
  /// requested. Reads cached atomics; never scans shared ownership on the producer.
  portable_interface_demand demand(uint32_t interface_id) const noexcept;

  /// Copies one already-encoded metadata/tensor snapshot into each admitted subscriber queue.
  portable_publish_result publish(const portable_publication_source& source) noexcept;

private:
  struct implementation;
  explicit portable_data_store(std::unique_ptr<implementation> state_);
  std::unique_ptr<implementation> state;
};

const char* to_string(portable_publish_result result) noexcept;

} // namespace ocudu::dapp::e3
