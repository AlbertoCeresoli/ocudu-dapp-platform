// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/ipc/publication_route.h"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>

using namespace ocudu::dapp::ipc;

namespace {

std::string error_text(const char* operation)
{
  return std::string(operation) + ": " + std::strerror(errno);
}

ocudu::unique_fd duplicate(int fd) noexcept
{
  return ocudu::unique_fd(::fcntl(fd, F_DUPFD_CLOEXEC, 0));
}

} // namespace

producer_open_result publication_producer::create(uint32_t slots, uint32_t payload_capacity)
{
  auto created = shared_spsc_ring::create(slots, payload_capacity);
  if (!created) {
    return {nullptr, created.detail};
  }
  ocudu::unique_fd notification(::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK));
  if (!notification.is_open()) {
    return {nullptr, error_text("create publication eventfd")};
  }
  return {
      std::unique_ptr<publication_producer>(new publication_producer(std::move(created.ring), std::move(notification))),
      {}};
}

route_capabilities publication_producer::export_capabilities() const noexcept
{
  return {ocudu::unique_fd(ring->export_fd()),
          duplicate(notification.value()),
          ocudu::unique_fd(ring->export_completion_fd())};
}

publish_result publication_producer::publish(ocudu::span<const uint8_t> payload, uint32_t flags) noexcept
{
  const payload_fragment fragment{payload, 0};
  return publish(ocudu::span<const payload_fragment>(&fragment, 1), flags);
}

publish_result publication_producer::publish(ocudu::span<const payload_fragment> fragments, uint32_t flags) noexcept
{
  return publish(fragments, flags, nullptr, nullptr);
}

publish_result publication_producer::publish(ocudu::span<const payload_fragment> fragments,
                                             uint32_t                            flags,
                                             publication_commit_callback         committed,
                                             void*                               committed_context) noexcept
{
  return publish_impl(fragments, flags, committed, committed_context, true);
}

publish_result publication_producer::publish_external(ocudu::span<const payload_fragment> fragments,
                                                      uint32_t                            flags,
                                                      publication_commit_callback         committed,
                                                      void*                               committed_context) noexcept
{
  return publish_impl(fragments, flags, committed, committed_context, false);
}

publish_result publication_producer::publish_impl(ocudu::span<const payload_fragment> fragments,
                                                  uint32_t                            flags,
                                                  publication_commit_callback         committed,
                                                  void*                               committed_context,
                                                  bool                                reclaim) noexcept
{
  uint64_t payload_size = 0;
  for (const payload_fragment& fragment : fragments) {
    const uint64_t capacity = ring->payload_capacity();
    if (payload_size > capacity || fragment.bytes.size() > capacity - payload_size) {
      return publish_result::invalid_payload;
    }
    payload_size += fragment.bytes.size();
    if (fragment.trailing_zeros > capacity - payload_size) {
      return publish_result::invalid_payload;
    }
    payload_size += fragment.trailing_zeros;
  }
  if (payload_size > ring->payload_capacity()) {
    return publish_result::invalid_payload;
  }
  const bool published =
      reclaim ? ring->try_publish(fragments, flags) : ring->try_publish_after_reclaim(fragments, flags);
  if (!published) {
    return publish_result::full;
  }
  if (committed != nullptr) {
    committed(committed_context);
  }
  const uint64_t increment = 1;
  const ssize_t  written   = ::write(notification.value(), &increment, sizeof(increment));
  if (written == sizeof(increment) || (written < 0 && errno == EAGAIN)) {
    return publish_result::published;
  }
  return publish_result::notification_failed;
}

bool publication_producer::reset_after_consumer_restart() noexcept
{
  uint64_t ignored = 0;
  while (::read(notification.value(), &ignored, sizeof(ignored)) == sizeof(ignored)) {
  }
  return ring->reset_after_consumer_restart();
}

consumer_open_result publication_consumer::attach(int ring_fd, int notification_fd, int completion_fd)
{
  auto attached = shared_spsc_ring::attach(ring_fd, completion_fd, endpoint_role::consumer);
  if (!attached) {
    return {nullptr, attached.detail};
  }
  auto owned_notification = duplicate(notification_fd);
  if (!owned_notification.is_open()) {
    return {nullptr, error_text("duplicate publication eventfd")};
  }
  return {std::unique_ptr<publication_consumer>(
              new publication_consumer(std::move(attached.ring), std::move(owned_notification))),
          {}};
}

bool publication_consumer::wait(std::chrono::milliseconds timeout) noexcept
{
  pollfd     descriptor{notification.value(), POLLIN, 0};
  const auto bounded = std::chrono::milliseconds(
      std::min<int64_t>(std::max<int64_t>(timeout.count(), 0), std::numeric_limits<int>::max()));
  const auto deadline  = std::chrono::steady_clock::now() + bounded;
  auto       remaining = bounded;
  do {
    const int result = ::poll(&descriptor, 1, static_cast<int>(remaining.count()));
    if (result >= 0) {
      return result == 1 && (descriptor.revents & POLLIN) != 0;
    }
    if (errno != EINTR) {
      return false;
    }
    remaining =
        std::max(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()),
                 std::chrono::milliseconds::zero());
  } while (remaining.count() != 0);
  return false;
}

uint64_t publication_consumer::drain_notifications() noexcept
{
  uint64_t total = 0;
  uint64_t value = 0;
  while (::read(notification.value(), &value, sizeof(value)) == sizeof(value)) {
    total += value;
  }
  return total;
}

const char* ocudu::dapp::ipc::to_string(publish_result result) noexcept
{
  static constexpr const char* names[] = {"published", "full", "invalid payload", "notification failed"};
  const auto                   index   = static_cast<unsigned>(result);
  return index < sizeof(names) / sizeof(names[0]) ? names[index] : "unknown publish result";
}
