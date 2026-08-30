// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/ipc/seqpacket_channel.h"
#include <array>
#include <cerrno>
#include <cstring>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

using namespace ocudu::dapp::ipc;

namespace {

using wire_header = ocudu_dapp_control_header_v1;
static_assert(sizeof(wire_header) == 24, "unexpected local control header layout");

std::string error_text(const char* operation)
{
  return std::string(operation) + ": " + std::strerror(errno);
}

} // namespace

channel_pair_result seqpacket_channel::create_pair()
{
  int descriptors[2];
  if (::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, descriptors) != 0) {
    return {nullptr, nullptr, error_text("create seqpacket pair")};
  }
  int enabled = 1;
  if (::setsockopt(descriptors[0], SOL_SOCKET, SO_PASSCRED, &enabled, sizeof(enabled)) != 0 ||
      ::setsockopt(descriptors[1], SOL_SOCKET, SO_PASSCRED, &enabled, sizeof(enabled)) != 0) {
    const auto detail = error_text("enable seqpacket credentials");
    ::close(descriptors[0]);
    ::close(descriptors[1]);
    return {nullptr, nullptr, detail};
  }
  // Bounded blocking on both ends: a peer that stops draining its socket must
  // surface as a protocol failure, not wedge the caller in send()/receive()
  // indefinitely (send_routes and the heartbeat path hold supervisor mutexes
  // across send()). EAGAIN after the timeout maps to the existing failure paths.
  const timeval channel_timeout{5, 0};
  if (::setsockopt(descriptors[0], SOL_SOCKET, SO_SNDTIMEO, &channel_timeout, sizeof(channel_timeout)) != 0 ||
      ::setsockopt(descriptors[1], SOL_SOCKET, SO_SNDTIMEO, &channel_timeout, sizeof(channel_timeout)) != 0 ||
      ::setsockopt(descriptors[0], SOL_SOCKET, SO_RCVTIMEO, &channel_timeout, sizeof(channel_timeout)) != 0 ||
      ::setsockopt(descriptors[1], SOL_SOCKET, SO_RCVTIMEO, &channel_timeout, sizeof(channel_timeout)) != 0) {
    const auto detail = error_text("bound seqpacket blocking");
    ::close(descriptors[0]);
    ::close(descriptors[1]);
    return {nullptr, nullptr, detail};
  }
  auto first          = std::unique_ptr<seqpacket_channel>(new seqpacket_channel(ocudu::unique_fd(descriptors[0])));
  auto second         = std::unique_ptr<seqpacket_channel>(new seqpacket_channel(ocudu::unique_fd(descriptors[1])));
  first->expected_uid = second->expected_uid = ::geteuid();
  return {std::move(first), std::move(second), {}};
}

void seqpacket_channel::require_peer(pid_t pid, uid_t uid) noexcept
{
  expected_pid = pid;
  expected_uid = uid;
}

bool seqpacket_channel::send(uint16_t                   type,
                             uint64_t                   sequence,
                             ocudu::span<const uint8_t> payload,
                             ocudu::span<const int>     descriptors) noexcept
{
  if (type == 0 || payload.size() > maximum_control_payload || descriptors.size() > maximum_control_fds) {
    return false;
  }
  wire_header          header{OCUDU_DAPP_CONTROL_MAGIC_V1,
                     OCUDU_DAPP_CONTROL_VERSION_V1,
                     type,
                     static_cast<uint32_t>(payload.size()),
                     static_cast<uint16_t>(descriptors.size()),
                     0,
                     sequence};
  std::array<iovec, 2> vectors{{{&header, sizeof(header)}, {const_cast<uint8_t*>(payload.data()), payload.size()}}};
  alignas(cmsghdr) std::array<uint8_t, CMSG_SPACE(maximum_control_fds * sizeof(int))> control{};
  msghdr                                                                              message{};
  message.msg_iov    = vectors.data();
  message.msg_iovlen = payload.empty() ? 1 : vectors.size();
  if (!descriptors.empty()) {
    message.msg_control    = control.data();
    message.msg_controllen = CMSG_SPACE(descriptors.size() * sizeof(int));
    auto* rights           = CMSG_FIRSTHDR(&message);
    rights->cmsg_level     = SOL_SOCKET;
    rights->cmsg_type      = SCM_RIGHTS;
    rights->cmsg_len       = CMSG_LEN(descriptors.size() * sizeof(int));
    std::memcpy(CMSG_DATA(rights), descriptors.data(), descriptors.size() * sizeof(int));
  }
  const auto sent = ::sendmsg(socket.value(), &message, MSG_NOSIGNAL);
  return sent == static_cast<ssize_t>(sizeof(header) + payload.size());
}

receive_result seqpacket_channel::receive() noexcept
try {
  std::array<uint8_t, sizeof(wire_header) + maximum_control_payload> data{};
  alignas(cmsghdr) std::array<uint8_t, CMSG_SPACE(maximum_control_fds * sizeof(int)) + CMSG_SPACE(sizeof(ucred))>
         control{};
  iovec  vector{data.data(), data.size()};
  msghdr message{};
  message.msg_iov        = &vector;
  message.msg_iovlen     = 1;
  message.msg_control    = control.data();
  message.msg_controllen = control.size();
  ssize_t received;
  do {
    received = ::recvmsg(socket.value(), &message, MSG_CMSG_CLOEXEC);
  } while (received < 0 && errno == EINTR);
  if (received == 0) {
    return {std::nullopt, {}, true};
  }
  if (received < 0) {
    if (errno == ECONNRESET || errno == ENOTCONN) {
      return {std::nullopt, {}, true};
    }
    return {std::nullopt, error_text("receive seqpacket"), false};
  }

  control_packet packet;
  // Reserve before touching any received descriptor so the emplace loop below
  // can never throw with raw fds still unwrapped (each fd is owned by a
  // unique_fd the moment it leaves the cmsg buffer).
  packet.descriptors.reserve(maximum_control_fds);
  bool have_credentials = false;
  for (auto* item = CMSG_FIRSTHDR(&message); item != nullptr; item = CMSG_NXTHDR(&message, item)) {
    if (item->cmsg_level != SOL_SOCKET) {
      continue;
    }
    if (item->cmsg_type == SCM_CREDENTIALS && item->cmsg_len == CMSG_LEN(sizeof(ucred))) {
      ucred credentials{};
      std::memcpy(&credentials, CMSG_DATA(item), sizeof(credentials));
      packet.sender    = {credentials.pid, credentials.uid, credentials.gid};
      have_credentials = true;
    } else if (item->cmsg_type == SCM_RIGHTS && item->cmsg_len >= CMSG_LEN(sizeof(int))) {
      const std::size_t count = (item->cmsg_len - CMSG_LEN(0)) / sizeof(int);
      if (count > maximum_control_fds) {
        return {std::nullopt, "too many received descriptors", false};
      }
      const auto* descriptors = reinterpret_cast<const int*>(CMSG_DATA(item));
      for (std::size_t i = 0; i != count; ++i) {
        packet.descriptors.emplace_back(descriptors[i]);
      }
    }
  }
  if ((message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0 || received < static_cast<ssize_t>(sizeof(wire_header)) ||
      !have_credentials) {
    return {std::nullopt, "truncated or unauthenticated control packet", false};
  }
  wire_header header{};
  std::memcpy(&header, data.data(), sizeof(header));
  if (header.magic != OCUDU_DAPP_CONTROL_MAGIC_V1 || header.version != OCUDU_DAPP_CONTROL_VERSION_V1 ||
      header.type == 0 || header.reserved != 0 || header.payload_size > maximum_control_payload ||
      header.descriptor_count > maximum_control_fds ||
      received != static_cast<ssize_t>(sizeof(header) + header.payload_size) ||
      header.descriptor_count != packet.descriptors.size()) {
    return {std::nullopt, "invalid control packet framing", false};
  }
  if (packet.sender.uid != expected_uid || (expected_pid && packet.sender.pid != *expected_pid)) {
    return {std::nullopt, "control packet peer credentials rejected", false};
  }
  packet.type     = header.type;
  packet.sequence = header.sequence;
  packet.payload.assign(data.begin() + sizeof(header), data.begin() + received);
  return {std::move(packet), {}, false};
} catch (...) {
  // Allocation sizes are bounded (<= 8 fds, <= 4 KiB payload) so this is only
  // reachable under genuine memory pressure. Report a degraded-mode protocol
  // failure instead of letting bad_alloc escape a noexcept function and abort
  // the gNB. Descriptors are wrapped in unique_fd as they leave the cmsg
  // buffer (capacity reserved up front), so unwinding closes them here.
  return {std::nullopt, {}, false};
}
