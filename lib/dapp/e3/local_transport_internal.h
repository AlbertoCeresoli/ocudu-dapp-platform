// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "ocudu/dapp/e3/aper_codec.h"
#include <cerrno>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>

namespace ocudu::dapp::e3::detail {

inline std::string error_text(const char* operation)
{
  return std::string(operation) + ": " + std::strerror(errno);
}

inline bool make_address(const std::string& path, sockaddr_un& address, std::string& detail)
{
  if (path.empty() || path.front() != '/' || path.size() >= sizeof(address.sun_path)) {
    detail = "local management socket path must be absolute and fit sockaddr_un";
    return false;
  }
  address            = {};
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
  return true;
}

inline bool set_timeout(int descriptor, std::chrono::milliseconds timeout)
{
  if (timeout.count() <= 0) {
    return false;
  }
  timeval value{};
  value.tv_sec  = timeout.count() / 1000;
  value.tv_usec = (timeout.count() % 1000) * 1000;
  return ::setsockopt(descriptor, SOL_SOCKET, SO_RCVTIMEO, &value, sizeof(value)) == 0 &&
         ::setsockopt(descriptor, SOL_SOCKET, SO_SNDTIMEO, &value, sizeof(value)) == 0;
}

inline const request_header& header(const management_request& request)
{
  return std::visit([](const auto& operation) -> const request_header& { return operation.header; }, request);
}

inline codec_result<std::vector<uint8_t>> receive_message(int descriptor)
{
  std::vector<uint8_t> bytes(aper_codec::maximum_message_size + 1);
  const ssize_t        received = ::recv(descriptor, bytes.data(), bytes.size(), MSG_TRUNC);
  if (received <= 0) {
    return {std::nullopt, received == 0 ? "management peer closed without a response" : error_text("receive")};
  }
  if (static_cast<std::size_t>(received) > aper_codec::maximum_message_size) {
    return {std::nullopt, "management message exceeds the configured bound"};
  }
  bytes.resize(received);
  return {std::move(bytes), {}};
}

inline bool send_message(int descriptor, const std::vector<uint8_t>& bytes)
{
  return ::send(descriptor, bytes.data(), bytes.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(bytes.size());
}

} // namespace ocudu::dapp::e3::detail
