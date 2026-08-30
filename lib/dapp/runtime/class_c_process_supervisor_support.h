// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear
//
// Free helpers for class_c_process_supervisor. Internal to lib/dapp.

#pragma once

#include "ocudu/adt/span.h"
#include "ocudu/dapp/ipc/seqpacket_channel.h"
#include "ocudu/dapp/runtime/class_c_process_supervisor.h"
#include "ocudu/support/io/unique_fd.h"
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <spawn.h>
#include <string>
#include <sys/resource.h>
#include <sys/types.h>
#include <vector>

namespace ocudu::dapp::class_c_process_detail {

constexpr int         worker_control_fd         = 3;
constexpr int         spawn_source_fd_minimum   = 64;
constexpr std::size_t maximum_arguments         = 32;
constexpr std::size_t maximum_environment       = 64;
constexpr std::size_t maximum_string_bytes      = 4096;
constexpr uint32_t    maximum_protocol_failures = 8;
constexpr std::size_t maximum_replay_operations = 64;
constexpr std::size_t maximum_replay_bytes      = 4 * 1024 * 1024;

enum class result_frame_status : uint8_t { processed, rejected, protocol_error, fatal_protocol_error };

class spawn_actions
{
public:
  spawn_actions() noexcept : valid(posix_spawn_file_actions_init(&value) == 0) {}
  ~spawn_actions()
  {
    if (valid) {
      posix_spawn_file_actions_destroy(&value);
    }
  }
  spawn_actions(const spawn_actions&)                         = delete;
  spawn_actions&              operator=(const spawn_actions&) = delete;
  posix_spawn_file_actions_t* get() noexcept { return valid ? &value : nullptr; }

private:
  posix_spawn_file_actions_t value{};
  bool                       valid{};
};

class spawn_attributes
{
public:
  spawn_attributes() noexcept : valid(posix_spawnattr_init(&value) == 0) {}
  ~spawn_attributes()
  {
    if (valid) {
      posix_spawnattr_destroy(&value);
    }
  }
  spawn_attributes(const spawn_attributes&)             = delete;
  spawn_attributes&  operator=(const spawn_attributes&) = delete;
  posix_spawnattr_t* get() noexcept { return valid ? &value : nullptr; }

private:
  posix_spawnattr_t value{};
  bool              valid{};
};

bool valid_duration(std::chrono::milliseconds value) noexcept;
bool apply_limit(pid_t process, decltype(RLIMIT_CORE) resource, rlim_t value, const char* name, std::string& failure);
struct process_security_state {
  bool no_new_privileges{};
  bool seccomp_filter{};
};

process_security_state read_process_security_state(pid_t process) noexcept;
bool                   wait_for_process_security(pid_t                     process,
                                                 bool                      require_seccomp_filter,
                                                 std::chrono::milliseconds timeout,
                                                 std::string&              failure) noexcept;
uint64_t               monotonic_time_ns() noexcept;
bool valid_strings(const std::vector<std::string>& values, std::size_t maximum, bool environment) noexcept;
bool all_zero(const uint64_t* values, std::size_t count) noexcept;

template <std::size_t N>
bool terminated(const char (&value)[N]) noexcept
{
  return std::find(std::begin(value), std::end(value), '\0') != std::end(value);
}

bool        valid_management_operation(uint32_t operation) noexcept;
bool        valid_management_request(const ocudu_dapp_process_management_request_v1& request,
                                     std::size_t                                     artifact_size) noexcept;
bool        valid_management_response(const ocudu_dapp_process_management_response_v1& response,
                                      const ocudu_dapp_process_management_request_v1&  request) noexcept;
unique_fd   make_model_descriptor(span<const uint8_t> artifact) noexcept;
bool        valid_route_flags(uint32_t interface_id, uint32_t flags) noexcept;
bool        valid_route_transport(const ocudu_dapp_route_offer_v1& offer) noexcept;
uint64_t    route_bundle_id(const std::vector<ocudu_dapp_process_interface_binding_v1>& interfaces,
                            uint64_t                                                    generation) noexcept;
uint32_t    allowed_result_capabilities(uint32_t interface_id) noexcept;
std::string validate_config(const class_c_process_supervisor_config& config);
bool        wait_process(pid_t pid, int& status) noexcept;
bool        signal_process_group(pid_t pid, int signal) noexcept;
void        set_failure(std::string& destination, const char* detail) noexcept;
bool        wait_readable(int descriptor, std::chrono::milliseconds timeout) noexcept;

} // namespace ocudu::dapp::class_c_process_detail
