// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "local_client.h"
#include <atomic>
#include <chrono>
#include <memory>
#include <sys/types.h>

namespace ocudu::dapp::e3 {

class management_service;

struct local_server_config {
  std::string               socket_path;
  std::optional<uid_t>      allowed_uid;
  std::chrono::milliseconds request_timeout{1000};
};

struct local_server_metrics {
  uint64_t accepted_connections{};
  uint64_t rejected_credentials{};
  uint64_t malformed_requests{};
  uint64_t completed_requests{};
  uint64_t transport_failures{};
};

class local_management_server;

struct local_server_create_result {
  std::unique_ptr<local_management_server> server;
  std::string                              detail;
  explicit                                 operator bool() const noexcept { return server != nullptr; }
};

/// Credentialed same-host carrier for management service-model APER payloads.
/// It is not a replacement for E3AP and never transports Class-C bulk tensors.
class local_management_server
{
public:
  static local_server_create_result create(management_service& service, local_server_config config) noexcept;

  ~local_management_server();
  local_management_server(const local_management_server&)            = delete;
  local_management_server& operator=(const local_management_server&) = delete;

  void                 stop() noexcept;
  local_server_metrics metrics() const noexcept;

private:
  struct implementation;
  explicit local_management_server(std::unique_ptr<implementation> state_);

  std::unique_ptr<implementation> state;
};

} // namespace ocudu::dapp::e3
