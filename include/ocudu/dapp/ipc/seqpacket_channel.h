// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "ocudu/adt/span.h"
#include "ocudu/dapp/process/v1/control.h"
#include "ocudu/support/io/unique_fd.h"
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <sys/types.h>
#include <vector>

namespace ocudu::dapp::ipc {

inline constexpr std::size_t maximum_control_payload = OCUDU_DAPP_CONTROL_MAX_PAYLOAD_V1;
inline constexpr std::size_t maximum_control_fds     = OCUDU_DAPP_CONTROL_MAX_DESCRIPTORS_V1;

/// Kernel-verified sender identity delivered with every packet (SO_PASSCRED /
/// SCM_CREDENTIALS).
struct peer_credentials {
  pid_t pid{-1};
  uid_t uid{};
  gid_t gid{};
};

/// One received control message: a nonzero packet type, the sender's sequence
/// number, its kernel-verified credentials, the payload, and any file
/// descriptors passed via SCM_RIGHTS (received close-on-exec).
struct control_packet {
  uint16_t                      type{};
  uint64_t                      sequence{};
  peer_credentials              sender;
  std::vector<uint8_t>          payload;
  std::vector<ocudu::unique_fd> descriptors;
};

/// Outcome of \ref seqpacket_channel::receive. \c peer_closed distinguishes an
/// orderly or reset peer shutdown from a transport error described in
/// \c detail.
struct receive_result {
  std::optional<control_packet> packet;
  std::string                   detail;
  bool                          peer_closed{};
  explicit                      operator bool() const noexcept { return packet.has_value(); }
};

class seqpacket_channel;

struct channel_pair_result {
  std::unique_ptr<seqpacket_channel> first;
  std::unique_ptr<seqpacket_channel> second;
  std::string                        detail;
  explicit                           operator bool() const noexcept { return first != nullptr && second != nullptr; }
};

/// Message-preserving AF_UNIX SOCK_SEQPACKET control channel used between the
/// supervisor and isolated Class-C workers. Every message carries a 24-byte
/// framing header plus a bounded payload (\ref maximum_control_payload) and up
/// to \ref maximum_control_fds passed descriptors; kernel credentials are
/// attached to each datagram.
class seqpacket_channel
{
public:
  /// Creates a connected channel pair with credential passing enabled; both
  /// ends initially expect the current effective UID as peer.
  static channel_pair_result create_pair();

  seqpacket_channel(seqpacket_channel&&) noexcept            = default;
  seqpacket_channel& operator=(seqpacket_channel&&) noexcept = default;
  ~seqpacket_channel()                                       = default;

  seqpacket_channel(const seqpacket_channel&)            = delete;
  seqpacket_channel& operator=(const seqpacket_channel&) = delete;

  /// Pins the expected peer: subsequent receives reject packets whose
  /// kernel-verified credentials do not match this PID and UID exactly.
  void require_peer(pid_t pid, uid_t uid) noexcept;
  /// Sends one framed message (type must be nonzero, payload and descriptor
  /// counts bounded). Non-blocking with respect to signals (MSG_NOSIGNAL);
  /// returns false on bound violations or short/failed writes.
  bool send(uint16_t                   type,
            uint64_t                   sequence,
            ocudu::span<const uint8_t> payload     = {},
            ocudu::span<const int>     descriptors = {}) noexcept;
  /// Blocks for one message; validates framing, bounds, and peer credentials.
  /// A closed or reset peer returns an empty result with \c peer_closed set.
  receive_result receive() noexcept;
  /// Underlying socket descriptor, e.g. for poll/epoll registration.
  int fd() const noexcept { return socket.value(); }

private:
  explicit seqpacket_channel(ocudu::unique_fd socket_) : socket(std::move(socket_)) {}

  ocudu::unique_fd     socket;
  std::optional<pid_t> expected_pid;
  uid_t                expected_uid{static_cast<uid_t>(-1)};
};

} // namespace ocudu::dapp::ipc
