// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/e3/local_client.h"
#include "local_transport_internal.h"
#include <exception>
#include <sys/socket.h>
#include <unistd.h>

using namespace ocudu::dapp::e3;
using namespace ocudu::dapp::e3::detail;

namespace {

class owned_fd
{
public:
  explicit owned_fd(int value_) noexcept : descriptor(value_) {}
  ~owned_fd()
  {
    if (descriptor >= 0) {
      static_cast<void>(::close(descriptor));
    }
  }

  owned_fd(const owned_fd&)            = delete;
  owned_fd& operator=(const owned_fd&) = delete;

  bool is_open() const noexcept { return descriptor >= 0; }
  int  value() const noexcept { return descriptor; }

private:
  int descriptor;
};

} // namespace

codec_result<management_response> local_management_client::call(const management_request& request,
                                                                std::chrono::milliseconds timeout) const noexcept
{
  try {
    sockaddr_un address{};
    std::string detail;
    if (!make_address(socket_path, address, detail) || timeout.count() <= 0) {
      return {std::nullopt, detail.empty() ? "local management timeout must be positive" : std::move(detail)};
    }
    auto encoded = aper_codec::encode(request);
    if (!encoded) {
      return {std::nullopt, std::move(encoded.detail)};
    }
    owned_fd socket(::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0));
    if (!socket.is_open()) {
      return {std::nullopt, error_text("create local management client socket")};
    }
    if (!set_timeout(socket.value(), timeout)) {
      return {std::nullopt, error_text("set local management client timeout")};
    }
    if (::connect(socket.value(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
      return {std::nullopt, error_text("connect local management socket")};
    }
    ucred       credentials{};
    socklen_t   credential_size = sizeof(credentials);
    const uid_t required_uid    = expected_server_uid.value_or(::geteuid());
    if (::getsockopt(socket.value(), SOL_SOCKET, SO_PEERCRED, &credentials, &credential_size) != 0 ||
        credential_size != sizeof(credentials) || credentials.uid != required_uid) {
      return {std::nullopt, "local management server credentials rejected"};
    }
    if (!send_message(socket.value(), *encoded.value)) {
      return {std::nullopt, error_text("send local management request")};
    }
    auto received = receive_message(socket.value());
    if (!received) {
      return {std::nullopt, std::move(received.detail)};
    }
    auto response = aper_codec::decode_response(*received.value);
    if (!response) {
      return response;
    }
    if (response.value->transaction_id != header(request).transaction_id) {
      return {std::nullopt, "management response transaction ID mismatch"};
    }
    return response;
  } catch (const std::exception& exception) {
    return {std::nullopt, exception.what()};
  } catch (...) {
    return {std::nullopt, "unknown local management client failure"};
  }
}
