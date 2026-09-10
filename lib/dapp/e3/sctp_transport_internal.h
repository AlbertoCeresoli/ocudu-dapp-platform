// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <netinet/in.h>
#include <netinet/sctp.h>
#include <optional>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>

// Kernel UAPI flag for SCTP event notifications delivered through recvmsg.
#ifndef MSG_NOTIFICATION
#define MSG_NOTIFICATION 0x8000
#endif

namespace ocudu::dapp::e3::sctp_detail {

inline std::string error_text(const char* operation)
{
  return std::string(operation) + ": " + std::strerror(errno);
}

inline uint64_t monotonic_ns() noexcept
{
  const auto value =
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
  return value > 0 ? static_cast<uint64_t>(value) : 1;
}

/// Parses a literal IPv4 or IPv6 address plus port into a socket address.
inline bool
make_address(const std::string& address, uint16_t port, sockaddr_storage& out, socklen_t& length, std::string& detail)
{
  out = {};
  sockaddr_in  v4{};
  sockaddr_in6 v6{};
  if (::inet_pton(AF_INET, address.c_str(), &v4.sin_addr) == 1) {
    v4.sin_family = AF_INET;
    v4.sin_port   = htons(port);
    std::memcpy(&out, &v4, sizeof(v4));
    length = sizeof(v4);
    return true;
  }
  if (::inet_pton(AF_INET6, address.c_str(), &v6.sin6_addr) == 1) {
    v6.sin6_family = AF_INET6;
    v6.sin6_port   = htons(port);
    std::memcpy(&out, &v6, sizeof(v6));
    length = sizeof(v6);
    return true;
  }
  detail = "SCTP address must be a literal IPv4 or IPv6 address";
  return false;
}

/// Renders the address part of a peer socket address as text.
inline std::string address_text(const sockaddr_storage& address)
{
  char buffer[INET6_ADDRSTRLEN] = {};
  if (address.ss_family == AF_INET) {
    const auto* v4 = reinterpret_cast<const sockaddr_in*>(&address);
    if (::inet_ntop(AF_INET, &v4->sin_addr, buffer, sizeof(buffer)) != nullptr) {
      return buffer;
    }
  } else if (address.ss_family == AF_INET6) {
    const auto* v6 = reinterpret_cast<const sockaddr_in6*>(&address);
    if (::inet_ntop(AF_INET6, &v6->sin6_addr, buffer, sizeof(buffer)) != nullptr) {
      return buffer;
    }
  }
  return {};
}

inline std::optional<uint16_t> bound_port(int descriptor)
{
  sockaddr_storage local{};
  socklen_t        length = sizeof(local);
  if (::getsockname(descriptor, reinterpret_cast<sockaddr*>(&local), &length) != 0) {
    return std::nullopt;
  }
  if (local.ss_family == AF_INET) {
    return ntohs(reinterpret_cast<const sockaddr_in*>(&local)->sin_port);
  }
  if (local.ss_family == AF_INET6) {
    return ntohs(reinterpret_cast<const sockaddr_in6*>(&local)->sin6_port);
  }
  return std::nullopt;
}

/// Best-effort socket-buffer sizing. The kernel clamps the request to
/// net.core.wmem_max / rmem_max; callers read the effective size back and
/// budget their message sizes accordingly.
inline void request_socket_buffers(int descriptor, std::size_t bytes)
{
  const int request = static_cast<int>(std::min<std::size_t>(bytes, static_cast<std::size_t>(INT32_MAX / 2)));
  ::setsockopt(descriptor, SOL_SOCKET, SO_SNDBUF, &request, sizeof(request));
  ::setsockopt(descriptor, SOL_SOCKET, SO_RCVBUF, &request, sizeof(request));
}

inline std::size_t effective_send_buffer(int descriptor)
{
  int       value = 0;
  socklen_t size  = sizeof(value);
  if (::getsockopt(descriptor, SOL_SOCKET, SO_SNDBUF, &value, &size) != 0 || value <= 0) {
    return 0;
  }
  return static_cast<std::size_t>(value);
}

inline void set_nodelay(int descriptor)
{
  const int enabled = 1;
  ::setsockopt(descriptor, IPPROTO_SCTP, SCTP_NODELAY, &enabled, sizeof(enabled));
}

/// Disables the delayed-SACK timer (200 ms by default on Linux) and asks for
/// a SACK on every received chunk.
///
/// Measured on Linux 6.11 loopback with 1.7 MB objects in 64 KiB fragments:
/// with the kernel defaults (16 KiB send buffer, receive window fixed from the
/// receive buffer at association setup, delayed SACK) a transfer takes 5-16 s
/// because at most one chunk is in flight and every one waits for the 200 ms
/// SACK timer. Presetting both sockets' buffers *before* the association is
/// established (listener before listen(), client before connect()) brings it
/// to ~1.3 ms; disabling delayed SACK on top of that brings it to ~1.0 ms.
/// Enlarging buffers after the association exists does not help, and delayed
/// SACK off alone does not help. Both peers must do this; see
/// docs/dapp/e3_wire.md, "Socket options".
/// Set on the listener so accepted associations inherit it, and again on each
/// accepted socket for kernels that do not propagate the endpoint default.
inline void disable_delayed_sack(int descriptor)
{
  sctp_sack_info info{};
  info.sack_assoc_id = 0;
  info.sack_delay    = 0;
  info.sack_freq     = 1;
  ::setsockopt(descriptor, IPPROTO_SCTP, SCTP_DELAYED_SACK, &info, sizeof(info));
}

inline void request_streams(int descriptor, uint16_t nof_streams)
{
  sctp_initmsg init{};
  init.sinit_num_ostreams  = nof_streams;
  init.sinit_max_instreams = nof_streams;
  ::setsockopt(descriptor, IPPROTO_SCTP, SCTP_INITMSG, &init, sizeof(init));
}

/// Requests per-delivery sctp_rcvinfo ancillary data so inbound payload
/// protocol identifiers can be validated. Best-effort: without it deliveries
/// simply carry no info and PPID validation is skipped.
inline void enable_receive_info(int descriptor)
{
  const int enabled = 1;
  ::setsockopt(descriptor, IPPROTO_SCTP, SCTP_RECVRCVINFO, &enabled, sizeof(enabled));
}

/// Sends one complete message on one SCTP stream with the given payload
/// protocol identifier. Returns the byte count sent, zero on EAGAIN, or -1.
inline ssize_t
send_message(int descriptor, const uint8_t* data, std::size_t size, uint16_t stream, uint32_t ppid) noexcept
{
  iovec  vector{const_cast<uint8_t*>(data), size};
  msghdr header{};
  header.msg_iov                                                  = &vector;
  header.msg_iovlen                                               = 1;
  alignas(cmsghdr) char control[CMSG_SPACE(sizeof(sctp_sndinfo))] = {};
  header.msg_control                                              = control;
  header.msg_controllen                                           = sizeof(control);
  cmsghdr* cmsg                                                   = CMSG_FIRSTHDR(&header);
  cmsg->cmsg_level                                                = IPPROTO_SCTP;
  cmsg->cmsg_type                                                 = SCTP_SNDINFO;
  cmsg->cmsg_len                                                  = CMSG_LEN(sizeof(sctp_sndinfo));
  sctp_sndinfo info{};
  info.snd_sid  = stream;
  info.snd_ppid = htonl(ppid);
  std::memcpy(CMSG_DATA(cmsg), &info, sizeof(info));
  while (true) {
    const ssize_t sent = ::sendmsg(descriptor, &header, MSG_NOSIGNAL | MSG_DONTWAIT);
    if (sent >= 0) {
      return sent;
    }
    if (errno == EINTR) {
      continue;
    }
    return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -1;
  }
}

enum class receive_status : uint8_t { partial, complete, would_block, closed, error, notification };

/// Delivery metadata read from sctp_rcvinfo ancillary data, when present.
struct receive_info {
  bool     valid{};
  uint16_t stream{};
  uint32_t ppid{};
};

/// Reads one delivery from a non-blocking SCTP socket. Messages larger than
/// one read arrive as several partial deliveries; MSG_EOR marks the last.
/// Truncated ancillary data (MSG_CTRUNC) is treated as a transport error.
inline receive_status
receive_part(int descriptor, uint8_t* buffer, std::size_t capacity, std::size_t& received, receive_info& info)
{
  iovec  vector{buffer, capacity};
  msghdr header{};
  header.msg_iov                                                  = &vector;
  header.msg_iovlen                                               = 1;
  alignas(cmsghdr) char control[CMSG_SPACE(sizeof(sctp_rcvinfo))] = {};
  header.msg_control                                              = control;
  header.msg_controllen                                           = sizeof(control);
  while (true) {
    const ssize_t count = ::recvmsg(descriptor, &header, MSG_DONTWAIT);
    if (count == 0) {
      return receive_status::closed;
    }
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      return errno == EAGAIN || errno == EWOULDBLOCK ? receive_status::would_block : receive_status::error;
    }
    received = static_cast<std::size_t>(count);
    if ((header.msg_flags & MSG_CTRUNC) != 0) {
      return receive_status::error;
    }
    for (cmsghdr* cmsg = CMSG_FIRSTHDR(&header); cmsg != nullptr; cmsg = CMSG_NXTHDR(&header, cmsg)) {
      if (cmsg->cmsg_level == IPPROTO_SCTP && cmsg->cmsg_type == SCTP_RCVINFO &&
          cmsg->cmsg_len >= CMSG_LEN(sizeof(sctp_rcvinfo))) {
        sctp_rcvinfo rcvinfo{};
        std::memcpy(&rcvinfo, CMSG_DATA(cmsg), sizeof(rcvinfo));
        info.valid  = true;
        info.stream = rcvinfo.rcv_sid;
        info.ppid   = ntohl(rcvinfo.rcv_ppid);
      }
    }
    if ((header.msg_flags & MSG_NOTIFICATION) != 0) {
      return receive_status::notification;
    }
    return (header.msg_flags & MSG_EOR) != 0 ? receive_status::complete : receive_status::partial;
  }
}

} // namespace ocudu::dapp::e3::sctp_detail
