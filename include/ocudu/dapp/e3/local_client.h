// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "management_protocol.h"
#include "result.h"
#include <chrono>
#include <optional>
#include <string>
#include <sys/types.h>

namespace ocudu::dapp::e3 {

/// Thread-safe reusable C++ management client. Each call uses one bounded
/// request/response connection so a stale peer cannot poison later calls.
class local_management_client
{
public:
  explicit local_management_client(std::string socket_path_, std::optional<uid_t> expected_server_uid_ = std::nullopt) :
    socket_path(std::move(socket_path_)), expected_server_uid(expected_server_uid_)
  {
  }

  codec_result<management_response> call(const management_request& request,
                                         std::chrono::milliseconds timeout = std::chrono::milliseconds{
                                             1000}) const noexcept;

private:
  std::string          socket_path;
  std::optional<uid_t> expected_server_uid;
};

} // namespace ocudu::dapp::e3
