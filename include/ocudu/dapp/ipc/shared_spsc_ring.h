// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "ocudu/adt/span.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace ocudu::dapp::ipc {

enum class endpoint_role : uint8_t { producer, consumer };

/// One source span followed by an optional zero-filled gap in a ring payload.
struct payload_fragment {
  ocudu::span<const uint8_t> bytes;
  uint32_t                   trailing_zeros{};
};

class shared_spsc_ring;

struct ring_open_result {
  std::unique_ptr<shared_spsc_ring> ring;
  std::string                       detail;
  explicit                          operator bool() const noexcept { return ring != nullptr; }
};

class shared_spsc_ring
{
  struct mapping;

public:
  // Exactly one producer shard and one consumer own a ring. Concurrent PHY
  // workers require distinct routes; sharing a producer endpoint is invalid.
  class read_lease
  {
  public:
    read_lease() noexcept = default;
    read_lease(read_lease&& other) noexcept;
    read_lease& operator=(read_lease&& other) noexcept;
    ~read_lease();

    read_lease(const read_lease&)            = delete;
    read_lease& operator=(const read_lease&) = delete;

    ocudu::span<const uint8_t> payload() const noexcept { return bytes; }
    uint32_t                   flags() const noexcept { return entry_flags; }
    uint64_t                   sequence() const noexcept { return entry_sequence; }
    explicit                   operator bool() const noexcept { return owner != nullptr; }
    void                       reset() noexcept;

  private:
    friend class shared_spsc_ring;
    read_lease(std::shared_ptr<mapping>   owner_,
               ocudu::span<const uint8_t> bytes_,
               uint32_t                   flags_,
               uint64_t                   sequence_,
               uint64_t                   epoch_) noexcept;

    std::shared_ptr<mapping>   owner;
    ocudu::span<const uint8_t> bytes;
    uint32_t                   entry_flags{};
    uint64_t                   entry_sequence{};
    uint64_t                   acquired_epoch{};
  };

  static ring_open_result create(uint32_t slots, uint32_t payload_capacity);
  static ring_open_result attach(int data_fd, int completion_fd, endpoint_role role);

  shared_spsc_ring(shared_spsc_ring&&) noexcept            = default;
  shared_spsc_ring& operator=(shared_spsc_ring&&) noexcept = default;
  ~shared_spsc_ring()                                      = default;

  shared_spsc_ring(const shared_spsc_ring&)            = delete;
  shared_spsc_ring& operator=(const shared_spsc_ring&) = delete;

  int      export_fd() const noexcept;
  int      export_completion_fd() const noexcept;
  uint32_t slot_count() const noexcept;
  uint32_t payload_capacity() const noexcept;
  uint64_t epoch() const noexcept;

  /// Reclaims consumer-completed entries and returns the number newly
  /// released. The SPSC producer may use this to retire associated external
  /// resources in the same FIFO order as ring leases.
  uint64_t reclaim_completed() noexcept;

  bool try_publish(ocudu::span<const uint8_t> payload, uint32_t flags = 0) noexcept;
  bool try_publish(ocudu::span<const payload_fragment> fragments, uint32_t flags = 0) noexcept;
  /// Publishes without consuming completion notifications. The producer must
  /// first call reclaim_completed() and retire matching external resources.
  bool try_publish_after_reclaim(ocudu::span<const payload_fragment> fragments, uint32_t flags = 0) noexcept;
  std::optional<read_lease> try_acquire() noexcept;

  /// Requires external proof that the old consumer process is no longer alive.
  bool reset_after_consumer_restart() noexcept;

private:
  static ring_open_result map_fd(int data_fd, int completion_fd, endpoint_role role);
  bool try_publish_impl(ocudu::span<const payload_fragment> fragments, uint32_t flags, bool reclaim) noexcept;
  shared_spsc_ring(std::shared_ptr<mapping> mapping_, endpoint_role role_) : mapped(std::move(mapping_)), role(role_) {}

  std::shared_ptr<mapping> mapped;
  endpoint_role            role;
};

} // namespace ocudu::dapp::ipc
