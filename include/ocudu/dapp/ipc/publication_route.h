// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "ocudu/dapp/ipc/shared_spsc_ring.h"
#include "ocudu/support/io/unique_fd.h"
#include <chrono>
#include <memory>
#include <string>

namespace ocudu::dapp::ipc {

enum class publish_result : uint8_t { published, full, invalid_payload, notification_failed };
using publication_commit_callback = void (*)(void*) noexcept;

struct route_capabilities {
  ocudu::unique_fd ring;
  ocudu::unique_fd notification;
  ocudu::unique_fd completion;
  ocudu::unique_fd accelerator_export;
  explicit operator bool() const noexcept { return ring.is_open() && notification.is_open() && completion.is_open(); }
};

class publication_producer;
class publication_consumer;

struct producer_open_result {
  std::unique_ptr<publication_producer> producer;
  std::string                           detail;
  explicit                              operator bool() const noexcept { return producer != nullptr; }
};

struct consumer_open_result {
  std::unique_ptr<publication_consumer> consumer;
  std::string                           detail;
  explicit                              operator bool() const noexcept { return consumer != nullptr; }
};

class publication_producer
{
public:
  static producer_open_result create(uint32_t slots, uint32_t payload_capacity);

  route_capabilities export_capabilities() const noexcept;
  publish_result     publish(ocudu::span<const uint8_t> payload, uint32_t flags = 0) noexcept;
  publish_result     publish(ocudu::span<const payload_fragment> fragments, uint32_t flags = 0) noexcept;
  publish_result     publish(ocudu::span<const payload_fragment> fragments,
                             uint32_t                            flags,
                             publication_commit_callback         committed,
                             void*                               committed_context) noexcept;
  /// Publishes after the caller has reclaimed completed entries and retired
  /// any resources associated with them.
  publish_result publish_external(ocudu::span<const payload_fragment> fragments,
                                  uint32_t                            flags,
                                  publication_commit_callback         committed,
                                  void*                               committed_context) noexcept;
  bool           reset_after_consumer_restart() noexcept;
  uint64_t       reclaim_completed() noexcept { return ring->reclaim_completed(); }
  uint32_t       slot_count() const noexcept { return ring->slot_count(); }
  uint32_t       payload_capacity() const noexcept { return ring->payload_capacity(); }
  uint64_t       route_epoch() const noexcept { return ring->epoch(); }

private:
  publish_result publish_impl(ocudu::span<const payload_fragment> fragments,
                              uint32_t                            flags,
                              publication_commit_callback         committed,
                              void*                               committed_context,
                              bool                                reclaim) noexcept;
  publication_producer(std::unique_ptr<shared_spsc_ring> ring_, ocudu::unique_fd notification_) :
    ring(std::move(ring_)), notification(std::move(notification_))
  {
  }

  std::unique_ptr<shared_spsc_ring> ring;
  ocudu::unique_fd                  notification;
};

class publication_consumer
{
public:
  static consumer_open_result attach(int ring_fd, int notification_fd, int completion_fd);

  bool                                        wait(std::chrono::milliseconds timeout) noexcept;
  uint64_t                                    drain_notifications() noexcept;
  std::optional<shared_spsc_ring::read_lease> try_acquire() noexcept { return ring->try_acquire(); }

private:
  publication_consumer(std::unique_ptr<shared_spsc_ring> ring_, ocudu::unique_fd notification_) :
    ring(std::move(ring_)), notification(std::move(notification_))
  {
  }

  std::unique_ptr<shared_spsc_ring> ring;
  ocudu::unique_fd                  notification;
};

const char* to_string(publish_result result) noexcept;

} // namespace ocudu::dapp::ipc
