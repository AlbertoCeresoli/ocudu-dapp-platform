// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear
//
// Free helpers for class_c_process_supervisor (split TU — no behavior change).

#include "class_c_process_supervisor_support.h"
#include "ocudu/dapp/process/v1/cuda_export.h"
#include "ocudu/dapp/process/v1/shared_ring.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <iterator>
#include <limits>
#include <poll.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace ocudu::dapp::class_c_process_detail {

bool valid_duration(std::chrono::milliseconds value) noexcept
{
  return value.count() > 0 && value <= std::chrono::minutes(1);
}

bool apply_limit(pid_t process, decltype(RLIMIT_CORE) resource, rlim_t value, const char* name, std::string& failure)
{
  const rlimit limit{value, value};
  if (::prlimit(process, resource, &limit, nullptr) == 0) {
    return true;
  }
  failure = std::string("unable to apply ") + name + ": " + std::strerror(errno);
  return false;
}

process_security_state read_process_security_state(pid_t process) noexcept
{
  char      path[64];
  const int length = std::snprintf(path, sizeof(path), "/proc/%d/status", process);
  if (length <= 0 || static_cast<std::size_t>(length) >= sizeof(path)) {
    return {};
  }
  unique_fd descriptor(::open(path, O_RDONLY | O_CLOEXEC));
  if (!descriptor.is_open()) {
    return {};
  }
  std::array<char, 8192> status{};
  const ssize_t          count = ::read(descriptor.value(), status.data(), status.size() - 1);
  if (count <= 0) {
    return {};
  }
  return {std::strstr(status.data(), "NoNewPrivs:\t1") != nullptr,
          std::strstr(status.data(), "Seccomp:\t2") != nullptr};
}

bool wait_for_process_security(pid_t                     process,
                               bool                      require_seccomp_filter,
                               std::chrono::milliseconds timeout,
                               std::string&              failure) noexcept
{
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  do {
    const auto state = read_process_security_state(process);
    if (state.no_new_privileges && (!require_seccomp_filter || state.seccomp_filter)) {
      return true;
    }
    if (::kill(process, 0) != 0 && errno == ESRCH) {
      failure = "process exited before applying the required security profile";
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  } while (std::chrono::steady_clock::now() < deadline);
  const auto state = read_process_security_state(process);
  failure          = !state.no_new_privileges ? "process did not apply no_new_privs before route admission"
                                              : "process did not apply a seccomp filter before route admission";
  return false;
}

uint64_t monotonic_time_ns() noexcept
{
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

bool valid_strings(const std::vector<std::string>& values, std::size_t maximum, bool environment) noexcept
{
  if (values.size() > maximum) {
    return false;
  }
  for (const auto& value : values) {
    if (value.empty() || value.size() > maximum_string_bytes || value.find('\0') != std::string::npos ||
        (environment && (value.front() == '=' || value.find('=') == std::string::npos)) ||
        (!environment && value.rfind("--control-fd=", 0) == 0)) {
      return false;
    }
  }
  return true;
}

bool all_zero(const uint64_t* values, std::size_t count) noexcept
{
  return std::all_of(values, values + count, [](uint64_t value) { return value == 0; });
}

bool valid_management_operation(uint32_t operation) noexcept
{
  return operation >= OCUDU_DAPP_PROCESS_MANAGEMENT_DESCRIBE_CONFIG_V1 &&
         operation <= OCUDU_DAPP_PROCESS_MANAGEMENT_MODEL_V1;
}

bool valid_management_request(const ocudu_dapp_process_management_request_v1& request,
                              std::size_t                                     artifact_size) noexcept
{
  if (request.struct_size != sizeof(request) || !valid_management_operation(request.operation) ||
      request.route_generation != 0 || request.request_id != 0 ||
      request.entry_count > OCUDU_DAPP_PROCESS_MANAGEMENT_MAX_ENTRIES_V1 ||
      (request.flags & ~OCUDU_DAPP_PROCESS_MANAGEMENT_DRY_RUN_V1) != 0 ||
      !all_zero(request.reserved, std::size(request.reserved))) {
    return false;
  }
  const bool model = request.operation == OCUDU_DAPP_PROCESS_MANAGEMENT_MODEL_V1;
  if (model) {
    return request.entry_count == 0 && request.item_offset == 0 &&
           request.boundary_or_action >= OCUDU_DAPP_MODEL_STAGE_V1 &&
           request.boundary_or_action <= OCUDU_DAPP_MODEL_RETIRE_V1 && request.expected_generation != 0 &&
           terminated(request.model_id) && request.model_id[0] != '\0' && terminated(request.model_version) &&
           request.artifact_size == artifact_size && artifact_size <= 1024 * 1024;
  }
  if (artifact_size != 0 || request.artifact_size != 0 || request.model_id[0] != '\0' ||
      request.model_version[0] != '\0') {
    return false;
  }
  if (request.operation == OCUDU_DAPP_PROCESS_MANAGEMENT_SET_CONFIG_V1) {
    if (request.item_offset != 0 || request.expected_generation == 0 || request.entry_count == 0 ||
        request.boundary_or_action < OCUDU_DAPP_CONFIG_BOUNDARY_IMMEDIATE_V1 ||
        request.boundary_or_action > OCUDU_DAPP_CONFIG_BOUNDARY_RESTART_V1) {
      return false;
    }
    for (uint32_t index = 0; index != request.entry_count; ++index) {
      if (!terminated(request.entries[index].key) || request.entries[index].key[0] == '\0') {
        return false;
      }
    }
  } else if (request.entry_count != 0 || request.expected_generation != 0 || request.boundary_or_action != 0) {
    return false;
  }
  return true;
}

bool valid_management_response(const ocudu_dapp_process_management_response_v1& response,
                               const ocudu_dapp_process_management_request_v1&  request) noexcept
{
  const auto valid_value = [](const ocudu_dapp_process_value_v1& value) {
    if (value.size > OCUDU_DAPP_PROCESS_MANAGEMENT_VALUE_BYTES_V1 ||
        !all_zero(value.reserved, std::size(value.reserved))) {
      return false;
    }
    if (value.kind == 0) {
      return value.size == 0;
    }
    if (value.kind < OCUDU_DAPP_VALUE_BOOL_V1 || value.kind > OCUDU_DAPP_VALUE_BYTES_V1) {
      return false;
    }
    return (value.kind == OCUDU_DAPP_VALUE_STRING_V1 || value.kind == OCUDU_DAPP_VALUE_BYTES_V1) || value.size == 0;
  };
  if (response.struct_size != sizeof(response) || response.operation != request.operation ||
      response.status < OCUDU_DAPP_OK_V1 || response.status > OCUDU_DAPP_CONFLICT_V1 ||
      response.count > OCUDU_DAPP_PROCESS_MANAGEMENT_MAX_ENTRIES_V1 ||
      response.route_generation != request.route_generation || response.request_id != request.request_id ||
      (response.flags & ~OCUDU_DAPP_PROCESS_MANAGEMENT_MORE_V1) != 0 || response.reserved0 != 0 ||
      !terminated(response.detail) || !all_zero(response.reserved, std::size(response.reserved))) {
    return false;
  }
  if (response.status != OCUDU_DAPP_OK_V1) {
    return response.flags == 0;
  }
  if (request.operation == OCUDU_DAPP_PROCESS_MANAGEMENT_DESCRIBE_CONFIG_V1) {
    if (response.resulting_generation != 0 || request.item_offset >= response.count) {
      return response.count == 0 && request.item_offset == 0 && response.flags == 0;
    }
    const auto& item = response.payload.descriptor;
    return terminated(item.key) && item.key[0] != '\0' && terminated(item.description) &&
           item.value_kind >= OCUDU_DAPP_VALUE_BOOL_V1 && item.value_kind <= OCUDU_DAPP_VALUE_BYTES_V1 &&
           valid_value(item.default_value) && valid_value(item.minimum_value) && valid_value(item.maximum_value) &&
           all_zero(item.reserved, std::size(item.reserved)) &&
           ((response.flags & OCUDU_DAPP_PROCESS_MANAGEMENT_MORE_V1) != 0) ==
               (request.item_offset + 1 < response.count);
  }
  if (request.operation == OCUDU_DAPP_PROCESS_MANAGEMENT_GET_CONFIG_V1) {
    if (response.resulting_generation == 0 || response.flags != 0) {
      return false;
    }
    for (uint32_t index = 0; index != response.count; ++index) {
      const auto& item = response.payload.entries[index];
      if (!terminated(item.key) || item.key[0] == '\0' || !valid_value(item.value)) {
        return false;
      }
    }
    return true;
  }
  return response.count == 0 && response.resulting_generation != 0 && response.flags == 0;
}

unique_fd make_model_descriptor(span<const uint8_t> artifact) noexcept
{
  if (artifact.empty()) {
    return {};
  }
  const int writable = ::memfd_create("ocudu-dapp-model", MFD_CLOEXEC | MFD_ALLOW_SEALING);
  if (writable < 0) {
    return {};
  }
  unique_fd owner(writable);
  if (::ftruncate(writable, artifact.size()) != 0) {
    return {};
  }
  std::size_t offset = 0;
  while (offset != artifact.size()) {
    const ssize_t written = ::pwrite(writable, artifact.data() + offset, artifact.size() - offset, offset);
    if (written <= 0) {
      return {};
    }
    offset += static_cast<std::size_t>(written);
  }
  if (::fcntl(writable, F_ADD_SEALS, F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL) != 0) {
    return {};
  }
  char      path[64];
  const int length = std::snprintf(path, sizeof(path), "/proc/self/fd/%d", writable);
  return length > 0 && static_cast<std::size_t>(length) < sizeof(path) ? unique_fd(::open(path, O_RDONLY | O_CLOEXEC))
                                                                       : unique_fd{};
}

bool valid_route_flags(uint32_t interface_id, uint32_t flags) noexcept
{
  return flags == 0 ||
         (interface_id == OCUDU_DAPP_INTERFACE_SPECTRUM_V1 && flags == OCUDU_DAPP_ROUTE_FLAG_UL_RESOURCE_PLAN_V1);
}

bool valid_route_transport(const ocudu_dapp_route_offer_v1& offer) noexcept
{
  if (offer.reserved0 != 0 || offer.reserved != 0) {
    return false;
  }
  if (offer.transport_kind == OCUDU_DAPP_ROUTE_TRANSPORT_CPU_SHARED_RING_V1) {
    return offer.backend_id == 0 && offer.export_slot_count == 0 && offer.export_slot_capacity == 0;
  }
  return offer.transport_kind == OCUDU_DAPP_ROUTE_TRANSPORT_CUDA_IPC_EXPORT_V1 &&
         (offer.interface_id == OCUDU_DAPP_INTERFACE_SPECTRUM_V1 ||
          offer.interface_id == OCUDU_DAPP_INTERFACE_SRS_ISAC_V1) &&
         offer.flags == 0 && offer.backend_id == OCUDU_DAPP_BACKEND_CUDA_V1 && offer.export_slot_count >= 2 &&
         offer.export_slot_count <= OCUDU_DAPP_CUDA_EXPORT_MAX_SLOTS_V1 && offer.export_slot_capacity != 0 &&
         offer.export_slot_capacity <= OCUDU_DAPP_SHARED_RING_MAX_BYTES_V1;
}

uint64_t route_bundle_id(const std::vector<ocudu_dapp_process_interface_binding_v1>& interfaces,
                         uint64_t                                                    generation) noexcept
{
  uint64_t   value = UINT64_C(1469598103934665603);
  const auto mix   = [&value](uint64_t item) {
    value ^= item;
    value *= UINT64_C(1099511628211);
  };
  mix(generation);
  for (const auto& binding : interfaces) {
    mix(binding.interface_id);
    mix(binding.route_count);
    mix(binding.requirement);
    mix(binding.state);
  }
  return value != 0 ? value : UINT64_C(1);
}

uint32_t allowed_result_capabilities(uint32_t interface_id) noexcept
{
  if (interface_id == OCUDU_DAPP_INTERFACE_SPECTRUM_V1) {
    return class_c_process_result_capability::spectrum_map | class_c_process_result_capability::quiet_intent;
  }
  return interface_id == OCUDU_DAPP_INTERFACE_SRS_ISAC_V1
             ? static_cast<uint32_t>(class_c_process_result_capability::srs_preview)
             : 0U;
}

std::string validate_config(const class_c_process_supervisor_config& config)
{
  if (!config.executable) {
    return "missing approved process executable";
  }
  if (!valid_duration(config.startup_timeout) || !valid_duration(config.stop_timeout) ||
      !valid_duration(config.restart_backoff) || !valid_duration(config.maximum_restart_backoff) ||
      config.restart_budget_reset_after.count() < 0 || !valid_duration(config.heartbeat_period) ||
      !valid_duration(config.heartbeat_timeout) || !valid_duration(config.management_timeout) ||
      config.maximum_open_files < 16 || config.maximum_open_files > 1048576 ||
      config.maximum_open_files <
          worker_control_fd + 1 + config.routes.size() * OCUDU_DAPP_ROUTE_DESCRIPTOR_MAX_COUNT_V1 ||
      config.maximum_core_file_bytes > std::numeric_limits<rlim_t>::max() ||
      (config.maximum_address_space_bytes != 0 &&
       (config.maximum_address_space_bytes < 64 * 1024 * 1024 ||
        config.maximum_address_space_bytes > std::numeric_limits<rlim_t>::max())) ||
      config.maximum_restart_attempts > 100) {
    return "invalid process timing, resource, or restart bound";
  }
  if (!valid_strings(config.arguments, maximum_arguments, false) ||
      !valid_strings(config.environment, maximum_environment, true)) {
    return "invalid process arguments or environment";
  }
  if (config.results_prohibited && config.result_sink != nullptr) {
    return "observer-only process cannot have a result sink";
  }
  if (config.interfaces.empty() || config.interfaces.size() > OCUDU_DAPP_PROCESS_MAX_INTERFACES_V1 ||
      config.routes.empty() || config.routes.size() > OCUDU_DAPP_MAX_PRODUCER_SHARDS_V1) {
    return "invalid process route count";
  }
  std::size_t expected_route_count = 0;
  for (std::size_t index = 0; index != config.interfaces.size(); ++index) {
    const auto& binding     = config.interfaces[index];
    const bool  required    = binding.requirement == OCUDU_DAPP_PROCESS_BINDING_REQUIRED_V1;
    const bool  optional    = binding.requirement == OCUDU_DAPP_PROCESS_BINDING_OPTIONAL_V1;
    const bool  bound       = binding.state == OCUDU_DAPP_PROCESS_BINDING_BOUND_V1;
    const bool  unavailable = binding.state == OCUDU_DAPP_PROCESS_BINDING_UNAVAILABLE_V1;
    if (binding.interface_id == 0 || (!required && !optional) || (!bound && !unavailable) || (required && !bound) ||
        (bound && binding.route_count == 0) || (unavailable && (!optional || binding.route_count != 0))) {
      return "invalid process interface binding";
    }
    if (std::any_of(config.interfaces.begin(), config.interfaces.begin() + index, [&](const auto& previous) {
          return previous.interface_id == binding.interface_id;
        })) {
      return "duplicate process interface binding";
    }
    expected_route_count += binding.route_count;
  }
  if (expected_route_count != config.routes.size()) {
    return "process binding route count mismatch";
  }
  std::array<std::array<bool, OCUDU_DAPP_MAX_PRODUCER_SHARDS_V1>, OCUDU_DAPP_PROCESS_MAX_INTERFACES_V1> seen_shards{};
  for (std::size_t index = 0; index != config.routes.size(); ++index) {
    const auto& binding  = config.routes[index];
    const auto& offer    = binding.offer;
    const auto interface = std::find_if(config.interfaces.begin(), config.interfaces.end(), [&](const auto& candidate) {
      return candidate.interface_id == offer.interface_id;
    });
    const std::size_t interface_index = static_cast<std::size_t>(interface - config.interfaces.begin());
    const uint32_t    allowed_results = allowed_result_capabilities(offer.interface_id);
    if (interface == config.interfaces.end() || interface->state != OCUDU_DAPP_PROCESS_BINDING_BOUND_V1 ||
        !binding.endpoint || binding.endpoint->interface_id() != offer.interface_id ||
        offer.struct_size != sizeof(offer) || offer.interface_id == 0 || offer.metadata_schema_id == 0 ||
        offer.metadata_schema_version == 0 || offer.route_id == 0 ||
        !valid_route_flags(offer.interface_id, offer.flags) || offer.producer_shard_count != interface->route_count ||
        offer.producer_shard_id >= interface->route_count || seen_shards[interface_index][offer.producer_shard_id] ||
        !valid_route_transport(offer) || (binding.result_capabilities & ~allowed_results) != 0) {
      return "inconsistent process route set";
    }
    seen_shards[interface_index][offer.producer_shard_id] = true;
    for (std::size_t previous = 0; previous != index; ++previous) {
      if (config.routes[previous].offer.route_id == offer.route_id ||
          config.routes[previous].endpoint.get() == binding.endpoint.get()) {
        return "duplicate process route identity or endpoint";
      }
      if (config.routes[previous].offer.interface_id == offer.interface_id &&
          (config.routes[previous].offer.metadata_schema_id != offer.metadata_schema_id ||
           config.routes[previous].offer.metadata_schema_version != offer.metadata_schema_version)) {
        return "inconsistent process route set";
      }
    }
  }
  return {};
}

bool wait_process(pid_t pid, int& status) noexcept
{
  for (;;) {
    const pid_t result = ::waitpid(pid, &status, 0);
    if (result == pid) {
      return true;
    }
    if (result < 0 && errno == EINTR) {
      continue;
    }
    return false;
  }
}

bool signal_process_group(pid_t pid, int signal) noexcept
{
  if (pid <= 0) {
    return false;
  }
  return ::kill(-pid, signal) == 0 || ::kill(pid, signal) == 0;
}

void set_failure(std::string& destination, const char* detail) noexcept
{
  try {
    destination = detail;
  } catch (...) {
    destination.clear();
  }
}

bool wait_readable(int descriptor, std::chrono::milliseconds timeout) noexcept
{
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  pollfd     item{descriptor, POLLIN, 0};
  for (;;) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      return false;
    }
    const auto remaining =
        std::max(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now), std::chrono::milliseconds(1));
    const int result = ::poll(&item, 1, static_cast<int>(remaining.count()));
    if (result > 0) {
      return (item.revents & (POLLIN | POLLHUP | POLLERR)) != 0;
    }
    if (result == 0) {
      return false;
    }
    if (errno != EINTR) {
      return false;
    }
  }
}

} // namespace ocudu::dapp::class_c_process_detail
