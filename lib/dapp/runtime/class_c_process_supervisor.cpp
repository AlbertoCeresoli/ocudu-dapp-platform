// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/runtime/class_c_process_supervisor.h"
#include "class_c_process_supervisor_support.h"
#include "ocudu/dapp/ipc/seqpacket_channel.h"
#include "ocudu/dapp/process/v1/cuda_export.h"
#include "ocudu/dapp/process/v1/shared_ring.h"
#include "ocudu/support/io/unique_fd.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <iterator>
#include <limits>
#include <mutex>
#include <new>
#include <optional>
#include <poll.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

using namespace ocudu;
using namespace ocudu::dapp;
using namespace ocudu::dapp::class_c_process_detail;

class class_c_process_supervisor::implementation
{
public:
  explicit implementation(class_c_process_supervisor_config config_) : config(std::move(config_))
  {
    if (config.environment.empty()) {
      config.environment = {"PATH=/usr/bin:/bin", "LANG=C"};
    }
    set_routes_active(false);
  }

  ~implementation()
  {
    stop();
    if (monitor.joinable()) {
      pid_t process;
      {
        std::lock_guard<std::mutex> lock(mutex);
        process = child_pid;
      }
      signal_process_group(process, SIGKILL);
      monitor.join();
    }
  }

  class_c_process_operation_result start() noexcept
  {
    std::lock_guard<std::mutex> operation_lock(management_mutex);
    bool                        join_previous;
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (current != class_c_process_state::stopped && current != class_c_process_state::failed) {
        return {OCUDU_DAPP_CONFLICT_V1, "process supervisor is already active"};
      }
      join_previous = monitor.joinable();
    }
    if (join_previous) {
      monitor.join();
    }
    {
      std::lock_guard<std::mutex> lock(mutex);
      stop_requested   = false;
      restart_attempts = 0;
      current          = class_c_process_state::starting;
      last_detail.clear();
      changed.notify_all();
    }

    try {
      monitor = std::thread([this] {
        std::string failure;
        if (!spawn_generation_contained(failure)) {
          std::lock_guard<std::mutex> lock(mutex);
          ++start_failures;
          current     = class_c_process_state::failed;
          last_detail = std::move(failure);
          changed.notify_all();
          return;
        }
        monitor_process();
      });
    } catch (...) {
      std::lock_guard<std::mutex> lock(mutex);
      ++start_failures;
      current     = class_c_process_state::failed;
      last_detail = "unable to create process monitor thread";
      changed.notify_all();
      return {OCUDU_DAPP_RESOURCE_V1, last_detail};
    }
    std::unique_lock<std::mutex> lock(mutex);
    changed.wait(lock, [this] { return current != class_c_process_state::starting; });
    if (current == class_c_process_state::failed) {
      return {OCUDU_DAPP_INTERNAL_ERROR_V1, last_detail};
    }
    return {OCUDU_DAPP_OK_V1, {}};
  }

  class_c_process_operation_result stop() noexcept
  {
    std::lock_guard<std::mutex> operation_lock(management_mutex);
    pid_t                       process = -1;
    {
      std::lock_guard<std::mutex> gate(result_mutex);
      std::lock_guard<std::mutex> lock(mutex);
      if (current == class_c_process_state::stopped && !monitor.joinable()) {
        publication_requested = false;
        set_routes_active(false);
        return {OCUDU_DAPP_OK_V1, {}};
      }
      stop_requested        = true;
      current               = class_c_process_state::stopping;
      process               = child_pid;
      publication_requested = false;
      set_routes_active(false);
    }
    changed.notify_all();
    signal_process_group(process, SIGTERM);

    bool reaped = false;
    {
      std::unique_lock<std::mutex> lock(mutex);
      reaped = changed.wait_for(lock, config.stop_timeout, [this] { return child_pid <= 0; });
      if (!reaped && child_pid > 0) {
        process = child_pid;
      }
    }
    if (!reaped) {
      if (signal_process_group(process, SIGKILL)) {
        std::lock_guard<std::mutex> lock(mutex);
        ++forced_kills;
      }
      std::unique_lock<std::mutex> lock(mutex);
      reaped = changed.wait_for(lock, config.stop_timeout, [this] { return child_pid <= 0; });
    }
    if (!reaped) {
      std::lock_guard<std::mutex> lock(mutex);
      current     = class_c_process_state::failed;
      last_detail = "Class-C worker did not exit after SIGKILL";
      changed.notify_all();
      return {OCUDU_DAPP_DEADLINE_V1, last_detail};
    }
    if (monitor.joinable()) {
      monitor.join();
    }
    {
      std::lock_guard<std::mutex> lock(mutex);
      child_pid = -1;
      control.reset();
      current = class_c_process_state::stopped;
      changed.notify_all();
    }
    return {OCUDU_DAPP_OK_V1, {}};
  }

  class_c_process_snapshot snapshot() const
  {
    std::lock_guard<std::mutex>      lock(mutex);
    class_c_process_endpoint_metrics route_metrics;
    for (const auto& route : config.routes) {
      const auto value = route.endpoint->snapshot_metrics();
      route_metrics.observed_cell_bitmap |= value.observed_cell_bitmap;
      route_metrics.publications += value.publications;
      route_metrics.payload_bytes += value.payload_bytes;
      route_metrics.backpressure_drops += value.backpressure_drops;
      route_metrics.invalid_drops += value.invalid_drops;
      route_metrics.encode_failures += value.encode_failures;
      route_metrics.notification_failures += value.notification_failures;
      route_metrics.inactive_bypasses += value.inactive_bypasses;
      route_metrics.concurrent_drops += value.concurrent_drops;
      route_metrics.snapshot_copy_operations += value.snapshot_copy_operations;
      route_metrics.snapshot_copy_bytes += value.snapshot_copy_bytes;
      route_metrics.lease_operations += value.lease_operations;
      route_metrics.export_copy_operations += value.export_copy_operations;
      route_metrics.export_copy_bytes += value.export_copy_bytes;
      route_metrics.export_slots_capacity += value.export_slots_capacity;
      route_metrics.export_slots_in_use += value.export_slots_in_use;
      route_metrics.export_slots_high_watermark += value.export_slots_high_watermark;
      route_metrics.export_slot_reservation_failures += value.export_slot_reservation_failures;
      route_metrics.e3_payload_copy_operations += value.e3_payload_copy_operations;
      route_metrics.e3_payload_copy_bytes += value.e3_payload_copy_bytes;
    }
    return {current,
            child_pid,
            routes_active,
            publication_requested,
            generation,
            starts,
            restarts,
            exits,
            start_failures,
            forced_kills,
            accepted_results,
            busy_results,
            rejected_results,
            result_protocol_failures,
            heartbeat_requests,
            heartbeat_responses,
            heartbeat_losses,
            last_heartbeat_latency_ns,
            management_responses,
            management_replays,
            management_timeouts,
            route_metrics,
            last_status,
            last_detail};
  }

  void set_result_identity(const uint64_t instance_id[2], uint64_t app_generation) noexcept
  {
    std::lock_guard<std::mutex> lock(mutex);
    result_instance_id[0] = instance_id != nullptr ? instance_id[0] : 0;
    result_instance_id[1] = instance_id != nullptr ? instance_id[1] : 0;
    result_app_generation = app_generation;
  }

  class_c_process_operation_result set_publication_enabled(bool enabled) noexcept
  {
    std::lock_guard<std::mutex>                  operation_lock(management_mutex);
    std::lock_guard<std::mutex>                  gate(result_mutex);
    std::shared_ptr<class_c_process_result_sink> sink;
    class_c_process_result_source                source;
    bool                                         notify = false;
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (enabled && (current == class_c_process_state::stopping || current == class_c_process_state::failed)) {
        return {OCUDU_DAPP_CONFLICT_V1, "failed or stopping process supervisor cannot enable publication"};
      }
      publication_requested = enabled;
      if (!enabled) {
        set_routes_active(false);
      }
      notify = current == class_c_process_state::running && config.result_sink != nullptr;
      if (notify) {
        sink                    = config.result_sink;
        source.instance_id[0]   = result_instance_id[0];
        source.instance_id[1]   = result_instance_id[1];
        source.app_generation   = result_app_generation;
        source.route_generation = generation;
        const auto& route       = result_lifecycle_route();
        source.route_id         = route.offer.route_id;
        source.interface_id     = route.offer.interface_id;
      }
      if (!notify && enabled) {
        set_routes_active(current == class_c_process_state::running);
      }
      changed.notify_all();
    }
    if (notify) {
      // Arm the result path before the sink callback: the callback is what
      // starts publications flowing to the worker, and a warm worker can
      // answer within microseconds. Enabling routes afterwards left a window
      // where that first result was counted as rejected. On callback failure
      // no publication can have been produced yet, so rolling back is safe.
      if (enabled) {
        std::lock_guard<std::mutex> lock(mutex);
        if (current != class_c_process_state::running || !publication_requested) {
          return {OCUDU_DAPP_CONFLICT_V1, "process state changed while enabling result publication"};
        }
        set_routes_active(true);
      }
      ocudu_dapp_status_v1 status = OCUDU_DAPP_EXCEPTION_V1;
      try {
        status = sink->publication_changed(source, enabled);
      } catch (...) {
      }
      if (status != OCUDU_DAPP_OK_V1) {
        if (enabled) {
          std::lock_guard<std::mutex> lock(mutex);
          publication_requested = false;
          set_routes_active(false);
        }
        return {status,
                enabled && status == OCUDU_DAPP_CONFLICT_V1
                    ? "Class-C result authority activation failed because another authority is active"
                    : "Class-C result lifecycle callback failed"};
      }
    }
    changed.notify_all();
    return {OCUDU_DAPP_OK_V1, {}};
  }

  class_c_process_management_result manage(ocudu_dapp_process_management_request_v1 request,
                                           span<const uint8_t>                      artifact) noexcept
  {
    try {
      std::lock_guard<std::mutex> command_lock(command_mutex);
      if (!valid_management_request(request, artifact.size())) {
        return {{OCUDU_DAPP_INVALID_INPUT_V1, "invalid Class-C process management request"}, {}};
      }
      const bool mutating = (request.operation == OCUDU_DAPP_PROCESS_MANAGEMENT_SET_CONFIG_V1 ||
                             request.operation == OCUDU_DAPP_PROCESS_MANAGEMENT_MODEL_V1) &&
                            (request.flags & OCUDU_DAPP_PROCESS_MANAGEMENT_DRY_RUN_V1) == 0;
      if (mutating &&
          (replay_log.size() == maximum_replay_operations || replay_bytes > maximum_replay_bytes - artifact.size())) {
        return {{OCUDU_DAPP_RESOURCE_V1, "Class-C management replay log is full"}, {}};
      }

      ipc::seqpacket_channel* channel;
      pid_t                   process;
      {
        std::lock_guard<std::mutex> lock(mutex);
        if (current != class_c_process_state::running || child_pid <= 0 || control == nullptr) {
          return {{OCUDU_DAPP_CONFLICT_V1, "Class-C worker is not running"}, {}};
        }
        process                  = child_pid;
        channel                  = control.get();
        request.route_generation = generation;
        request.request_id       = next_management_request_id();
      }
      {
        std::lock_guard<std::mutex> lock(response_mutex);
        response_waiting    = true;
        response_generation = request.route_generation;
        response_request_id = request.request_id;
        pending_response.reset();
      }
      if (!send_management(*channel, request, artifact)) {
        clear_response_wait();
        signal_process_group(process, SIGKILL);
        return {{OCUDU_DAPP_INTERNAL_ERROR_V1, "unable to send Class-C management request"}, {}};
      }

      std::optional<ocudu_dapp_process_management_response_v1> received;
      {
        std::unique_lock<std::mutex> lock(response_mutex);
        if (response_changed.wait_for(
                lock, config.management_timeout, [&] { return pending_response.has_value() || !response_waiting; })) {
          received = pending_response;
        }
        response_waiting = false;
        pending_response.reset();
      }
      if (!received) {
        {
          std::lock_guard<std::mutex> gate(result_mutex);
          std::lock_guard<std::mutex> lock(mutex);
          if (child_pid == process && generation == request.route_generation) {
            ++management_timeouts;
            last_detail = "Class-C worker management request timed out";
            set_routes_active(false);
          }
        }
        signal_process_group(process, SIGKILL);
        return {{OCUDU_DAPP_DEADLINE_V1, "Class-C management request timed out; worker restart requested"}, {}};
      }
      if (!valid_management_response(*received, request)) {
        {
          std::lock_guard<std::mutex> lock(mutex);
          ++result_protocol_failures;
        }
        signal_process_group(process, SIGKILL);
        return {{OCUDU_DAPP_INVALID_OUTPUT_V1, "invalid Class-C management response"}, {}};
      }
      {
        std::lock_guard<std::mutex> lock(mutex);
        ++management_responses;
      }
      if (mutating && received->status == OCUDU_DAPP_OK_V1) {
        try {
          replay_log.push_back({request, std::vector<uint8_t>(artifact.begin(), artifact.end())});
          replay_bytes += artifact.size();
        } catch (const std::bad_alloc&) {
          signal_process_group(process, SIGKILL);
          return {{OCUDU_DAPP_RESOURCE_V1, "unable to retain Class-C management state for restart"}, {}};
        } catch (...) {
          signal_process_group(process, SIGKILL);
          return {{OCUDU_DAPP_INTERNAL_ERROR_V1, "unable to retain Class-C management state for restart"}, {}};
        }
      }
      const auto status = static_cast<ocudu_dapp_status_v1>(received->status);
      return {{status, {}}, *received};
    } catch (const std::bad_alloc&) {
      return {{OCUDU_DAPP_RESOURCE_V1, "unable to allocate Class-C management state"}, {}};
    } catch (...) {
      return {{OCUDU_DAPP_INTERNAL_ERROR_V1, "unknown Class-C management failure"}, {}};
    }
  }

  bool wait_for_state(class_c_process_state expected, std::chrono::milliseconds timeout) const noexcept
  {
    if (timeout.count() < 0) {
      return false;
    }
    std::unique_lock<std::mutex> lock(mutex);
    return changed.wait_for(lock, timeout, [this, expected] { return current == expected; });
  }

private:
  struct replay_operation {
    ocudu_dapp_process_management_request_v1 request{};
    std::vector<uint8_t>                     artifact;
  };

  uint64_t next_management_request_id() noexcept
  {
    ++management_request_id;
    if (management_request_id == 0) {
      ++management_request_id;
    }
    return management_request_id;
  }

  void clear_response_wait() noexcept
  {
    std::lock_guard<std::mutex> lock(response_mutex);
    response_waiting = false;
    pending_response.reset();
    response_changed.notify_all();
  }

  bool send_management(ipc::seqpacket_channel&                         channel,
                       const ocudu_dapp_process_management_request_v1& request,
                       span<const uint8_t>                             artifact) noexcept
  {
    unique_fd descriptor = make_model_descriptor(artifact);
    if (!artifact.empty() && !descriptor.is_open()) {
      return false;
    }
    const std::array<int, 1>    descriptors{descriptor.value()};
    std::lock_guard<std::mutex> lock(send_mutex);
    return channel.send(OCUDU_DAPP_CONTROL_MANAGEMENT_REQUEST_V1,
                        request.route_generation,
                        {reinterpret_cast<const uint8_t*>(&request), sizeof(request)},
                        artifact.empty() ? span<const int>{} : span<const int>{descriptors});
  }

  bool replay_management(ipc::seqpacket_channel& channel, uint64_t route_generation, std::string& failure) noexcept
  {
    std::lock_guard<std::mutex> command_lock(command_mutex);
    for (const auto& retained : replay_log) {
      auto request             = retained.request;
      request.route_generation = route_generation;
      request.request_id       = next_management_request_id();
      if (!send_management(channel, request, retained.artifact)) {
        set_failure(failure, "unable to replay Class-C management state");
        return false;
      }
      if (!wait_readable(channel.fd(), config.management_timeout)) {
        set_failure(failure, "Class-C management replay timed out");
        return false;
      }
      auto received = channel.receive();
      if (!received || received.packet->type != OCUDU_DAPP_CONTROL_MANAGEMENT_RESPONSE_V1 ||
          received.packet->sequence != route_generation || !received.packet->descriptors.empty() ||
          received.packet->payload.size() != sizeof(ocudu_dapp_process_management_response_v1)) {
        set_failure(failure, "invalid Class-C management replay response");
        return false;
      }
      ocudu_dapp_process_management_response_v1 response{};
      std::memcpy(&response, received.packet->payload.data(), sizeof(response));
      if (!valid_management_response(response, request) || response.status != OCUDU_DAPP_OK_V1) {
        set_failure(failure, "Class-C worker rejected retained management state");
        return false;
      }
      std::lock_guard<std::mutex> lock(mutex);
      ++management_replays;
    }
    return true;
  }

  void set_routes_active(bool active) noexcept
  {
    for (const auto& route : config.routes) {
      route.endpoint->set_active(active);
    }
    routes_active = active;
  }

  const class_c_process_route_binding& result_lifecycle_route() const noexcept
  {
    const auto spectrum = std::find_if(config.routes.begin(), config.routes.end(), [](const auto& route) {
      return route.offer.interface_id == OCUDU_DAPP_INTERFACE_SPECTRUM_V1 && route.result_capabilities != 0;
    });
    return spectrum != config.routes.end() ? *spectrum : config.routes.front();
  }

  bool reset_routes(std::chrono::steady_clock::time_point deadline) noexcept
  {
    std::array<bool, OCUDU_DAPP_MAX_PRODUCER_SHARDS_V1> reset{};
    std::size_t                                         remaining = config.routes.size();
    while (remaining != 0) {
      for (std::size_t index = 0; index != config.routes.size(); ++index) {
        if (!reset[index] && config.routes[index].endpoint->reset_after_consumer_restart()) {
          reset[index] = true;
          --remaining;
        }
      }
      if (remaining == 0) {
        return true;
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        return false;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
  }

  bool spawn_generation_contained(std::string& failure) noexcept
  {
    try {
      return spawn_generation(failure);
    } catch (const std::bad_alloc&) {
      set_failure(failure, "resource exhausted while starting Class-C worker");
    } catch (...) {
      set_failure(failure, "unknown failure while starting Class-C worker");
    }
    return false;
  }

  bool spawn_generation(std::string& failure)
  {
    auto channels = ipc::seqpacket_channel::create_pair();
    if (!channels) {
      failure = std::move(channels.detail);
      return false;
    }
    unique_fd executable_source(
        ::fcntl(config.executable->native_descriptor(), F_DUPFD_CLOEXEC, spawn_source_fd_minimum));
    unique_fd        control_source(::fcntl(channels.second->fd(), F_DUPFD_CLOEXEC, spawn_source_fd_minimum));
    spawn_actions    actions;
    spawn_attributes attributes;
    if (!executable_source.is_open() || !control_source.is_open() || actions.get() == nullptr ||
        attributes.get() == nullptr ||
        posix_spawn_file_actions_adddup2(actions.get(), control_source.value(), worker_control_fd) != 0) {
      failure = "unable to prepare process spawn descriptors";
      return false;
    }

    sigset_t empty_signals;
    sigset_t default_signals;
    sigemptyset(&empty_signals);
    sigemptyset(&default_signals);
    sigaddset(&default_signals, SIGTERM);
    sigaddset(&default_signals, SIGINT);
    sigaddset(&default_signals, SIGHUP);
    sigaddset(&default_signals, SIGPIPE);
    sigaddset(&default_signals, SIGCHLD);
    short flags = POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF;
    if (posix_spawnattr_setpgroup(attributes.get(), 0) != 0 ||
        posix_spawnattr_setsigmask(attributes.get(), &empty_signals) != 0 ||
        posix_spawnattr_setsigdefault(attributes.get(), &default_signals) != 0 ||
        posix_spawnattr_setflags(attributes.get(), flags) != 0) {
      failure = "unable to prepare process spawn attributes";
      return false;
    }

    uint64_t next_generation;
    {
      std::lock_guard<std::mutex> lock(mutex);
      next_generation = ++generation;
      if (next_generation == 0) {
        next_generation = ++generation;
      }
      current = class_c_process_state::starting;
      changed.notify_all();
    }
    std::string              executable_path  = "/proc/self/fd/" + std::to_string(executable_source.value());
    std::string              control_argument = "--control-fd=" + std::to_string(worker_control_fd);
    std::vector<std::string> argument_storage;
    argument_storage.reserve(config.arguments.size() + 2);
    argument_storage.push_back(config.executable->canonical_path());
    argument_storage.push_back(std::move(control_argument));
    argument_storage.insert(argument_storage.end(), config.arguments.begin(), config.arguments.end());
    std::vector<char*> arguments;
    arguments.reserve(argument_storage.size() + 1);
    for (auto& value : argument_storage) {
      arguments.push_back(value.data());
    }
    arguments.push_back(nullptr);
    std::vector<char*> environment;
    environment.reserve(config.environment.size() + 1);
    for (auto& value : config.environment) {
      environment.push_back(value.data());
    }
    environment.push_back(nullptr);

    pid_t     process = -1;
    const int result  = ::posix_spawn(
        &process, executable_path.c_str(), actions.get(), attributes.get(), arguments.data(), environment.data());
    if (result != 0 || process <= 0) {
      failure = std::string("posix_spawn failed: ") + std::strerror(result);
      return false;
    }
    try {
      channels.first->require_peer(process, ::geteuid());
      channels.second.reset();
      {
        std::lock_guard<std::mutex> lock(mutex);
        child_pid = process;
        changed.notify_all();
      }

      if (!apply_limit(process, RLIMIT_CORE, config.maximum_core_file_bytes, "RLIMIT_CORE", failure) ||
          !apply_limit(process, RLIMIT_NOFILE, config.maximum_open_files, "RLIMIT_NOFILE", failure) ||
          (config.maximum_address_space_bytes != 0 &&
           !apply_limit(process, RLIMIT_AS, config.maximum_address_space_bytes, "RLIMIT_AS", failure)) ||
          !wait_for_process_security(process, config.require_seccomp_filter, config.startup_timeout, failure) ||
          !send_routes(*channels.first, next_generation, failure) ||
          !receive_ready(*channels.first, next_generation, failure) ||
          !replay_management(*channels.first, next_generation, failure)) {
        terminate_spawn(process);
        return false;
      }
      bool started = false;
      {
        std::lock_guard<std::mutex>                  gate(result_mutex);
        std::shared_ptr<class_c_process_result_sink> sink;
        class_c_process_result_source                source;
        bool                                         notify = false;
        {
          std::lock_guard<std::mutex> lock(mutex);
          if (stop_requested) {
            failure = "process start cancelled";
          } else {
            control = std::move(channels.first);
            set_routes_active(false);
            child_pid = process;
            ++starts;
            current = class_c_process_state::running;
            last_detail.clear();
            notify = publication_requested && config.result_sink != nullptr;
            if (notify) {
              sink                    = config.result_sink;
              source.instance_id[0]   = result_instance_id[0];
              source.instance_id[1]   = result_instance_id[1];
              source.app_generation   = result_app_generation;
              source.route_generation = generation;
              const auto& route       = result_lifecycle_route();
              source.route_id         = route.offer.route_id;
              source.interface_id     = route.offer.interface_id;
            } else {
              set_routes_active(publication_requested);
              started = true;
            }
            changed.notify_all();
          }
        }
        if (notify) {
          ocudu_dapp_status_v1 status = OCUDU_DAPP_EXCEPTION_V1;
          try {
            status = sink->publication_changed(source, true);
          } catch (...) {
          }
          std::lock_guard<std::mutex> lock(mutex);
          if (status == OCUDU_DAPP_OK_V1 && !stop_requested && current == class_c_process_state::running) {
            set_routes_active(true);
            started = true;
          } else {
            failure = "Class-C result authority activation failed because another authority is active";
          }
          changed.notify_all();
        }
      }
      if (started) {
        return true;
      }
      terminate_spawn(process);
      return false;
    } catch (...) {
      terminate_spawn(process);
      throw;
    }
  }

  bool send_routes(ipc::seqpacket_channel& channel, uint64_t route_generation, std::string& failure)
  {
    if (config.interfaces.size() > 1) {
      ocudu_dapp_route_bundle_v1 bundle{};
      bundle.struct_size      = sizeof(bundle);
      bundle.interface_count  = static_cast<uint16_t>(config.interfaces.size());
      bundle.route_count      = static_cast<uint16_t>(config.routes.size());
      bundle.route_generation = route_generation;
      bundle.bundle_id        = route_bundle_id(config.interfaces, route_generation);
      std::copy(config.interfaces.begin(), config.interfaces.end(), std::begin(bundle.interfaces));
      if (!channel.send(OCUDU_DAPP_CONTROL_ROUTE_BUNDLE_V1,
                        route_generation,
                        {reinterpret_cast<const uint8_t*>(&bundle), sizeof(bundle)})) {
        failure = "unable to send process route bundle";
        return false;
      }
    }
    for (const auto& route : config.routes) {
      auto capabilities = route.endpoint->export_capabilities();
      if (!capabilities) {
        failure = "unable to export process route capabilities";
        return false;
      }
      std::array<int, OCUDU_DAPP_ROUTE_DESCRIPTOR_MAX_COUNT_V1> descriptors{
          capabilities.ring.value(), capabilities.notification.value(), capabilities.completion.value(), -1};
      std::size_t descriptor_count = OCUDU_DAPP_ROUTE_DESCRIPTOR_COUNT_V1;
      if (route.offer.transport_kind == OCUDU_DAPP_ROUTE_TRANSPORT_CUDA_IPC_EXPORT_V1) {
        if (!capabilities.accelerator_export.is_open()) {
          failure = "missing CUDA export route capability";
          return false;
        }
        descriptors[OCUDU_DAPP_ROUTE_DESCRIPTOR_ACCELERATOR_EXPORT_V1] = capabilities.accelerator_export.value();
        descriptor_count                                               = OCUDU_DAPP_ROUTE_DESCRIPTOR_CUDA_COUNT_V1;
      } else if (capabilities.accelerator_export.is_open()) {
        failure = "unexpected accelerator export route capability";
        return false;
      }
      if (!channel.send(OCUDU_DAPP_CONTROL_ROUTE_OFFER_V1,
                        route_generation,
                        {reinterpret_cast<const uint8_t*>(&route.offer), sizeof(route.offer)},
                        {descriptors.data(), descriptor_count})) {
        failure = "unable to send process route offer";
        return false;
      }
    }
    return true;
  }

  bool receive_ready(ipc::seqpacket_channel& channel, uint64_t route_generation, std::string& failure)
  {
    if (!wait_readable(channel.fd(), config.startup_timeout)) {
      failure = "process route readiness timed out";
      return false;
    }
    auto received = channel.receive();
    if (!received) {
      failure = received.peer_closed ? "process closed before route readiness" : std::move(received.detail);
      return false;
    }
    if (received.packet->sequence != route_generation || !received.packet->descriptors.empty()) {
      failure = "invalid process route readiness frame";
      return false;
    }
    const bool cuda_routes = std::any_of(config.routes.begin(), config.routes.end(), [](const auto& route) {
      return route.offer.transport_kind == OCUDU_DAPP_ROUTE_TRANSPORT_CUDA_IPC_EXPORT_V1;
    });
    uint32_t   required =
        OCUDU_DAPP_PROCESS_REQUIRED_CAPABILITIES_V1 | (cuda_routes ? OCUDU_DAPP_PROCESS_CAP_CUDA_IPC_V1 : 0U);
    if (config.interfaces.size() > 1) {
      if (received.packet->type != OCUDU_DAPP_CONTROL_ROUTE_BUNDLE_READY_V1 ||
          received.packet->payload.size() != sizeof(ocudu_dapp_route_bundle_ready_v1)) {
        failure = "invalid process route bundle readiness frame";
        return false;
      }
      ocudu_dapp_route_bundle_ready_v1 ready{};
      std::memcpy(&ready, received.packet->payload.data(), sizeof(ready));
      required |= OCUDU_DAPP_PROCESS_CAP_MULTI_INTERFACE_V1;
      if (ready.struct_size != sizeof(ready) || ready.interface_count != config.interfaces.size() ||
          ready.route_count != config.routes.size() || ready.flags != required || ready.reserved0 != 0 ||
          ready.route_generation != route_generation ||
          ready.bundle_id != route_bundle_id(config.interfaces, route_generation) ||
          !all_zero(ready.reserved, std::size(ready.reserved))) {
        failure = "invalid process route bundle readiness payload";
        return false;
      }
      return true;
    }
    if (received.packet->type != OCUDU_DAPP_CONTROL_ROUTE_READY_V1 ||
        received.packet->payload.size() != sizeof(ocudu_dapp_route_ready_v1)) {
      failure = "invalid process route readiness frame";
      return false;
    }
    ocudu_dapp_route_ready_v1 ready{};
    std::memcpy(&ready, received.packet->payload.data(), sizeof(ready));
    const auto& expected = config.routes.front().offer;
    if (ready.struct_size != sizeof(ready) || ready.interface_id != expected.interface_id ||
        ready.route_generation != route_generation || ready.producer_shard_count != config.routes.size() ||
        ready.reserved0 != 0 || ready.flags != required || ready.reserved != 0) {
      failure = "invalid process route readiness payload";
      return false;
    }
    return true;
  }

  void terminate_spawn(pid_t process) noexcept
  {
    if (process <= 0) {
      return;
    }
    const bool signalled = signal_process_group(process, SIGKILL);
    int        status    = 0;
    wait_process(process, status);
    std::lock_guard<std::mutex> lock(mutex);
    set_routes_active(false);
    if (signalled) {
      ++forced_kills;
    }
    last_status = status;
    child_pid   = -1;
    control.reset();
    changed.notify_all();
  }

  void request_stop_and_reap() noexcept
  {
    pid_t process;
    {
      std::lock_guard<std::mutex> lock(mutex);
      stop_requested        = true;
      process               = child_pid;
      publication_requested = false;
      set_routes_active(false);
    }
    if (process > 0) {
      terminate_spawn(process);
    }
  }

  /// Exponential backoff with jitter: base * 2^attempts, capped, +-25 %. The
  /// jitter source does not need cryptographic quality; it only decorrelates
  /// concurrent supervisors restarting after a shared fault.
  std::chrono::milliseconds restart_delay(uint32_t attempts) noexcept
  {
    const uint64_t base   = static_cast<uint64_t>(config.restart_backoff.count());
    const uint64_t cap    = std::max<uint64_t>(base, static_cast<uint64_t>(config.maximum_restart_backoff.count()));
    const uint64_t scaled = attempts >= 32 ? cap : std::min<uint64_t>(cap, base << attempts);
    jitter_state          = jitter_state * 6364136223846793005ULL + 1442695040888963407ULL;
    const uint64_t jitter = scaled / 4;
    const uint64_t delay  = jitter == 0 ? scaled : scaled - jitter + (jitter_state >> 33) % (2 * jitter + 1);
    return std::chrono::milliseconds(delay);
  }

  void monitor_process() noexcept
  {
    for (;;) {
      pid_t      process;
      uint64_t   process_generation;
      const auto generation_started = std::chrono::steady_clock::now();
      {
        std::lock_guard<std::mutex> lock(mutex);
        process            = child_pid;
        process_generation = generation;
      }
      std::thread result_receiver;
      try {
        result_receiver =
            std::thread([this, process, process_generation] { receive_results(process, process_generation); });
      } catch (...) {
        {
          std::lock_guard<std::mutex> lock(mutex);
          ++start_failures;
          last_detail = "unable to create Class-C result receiver thread";
        }
        signal_process_group(process, SIGKILL);
      }
      int status = 0;
      if (process <= 0 || !wait_process(process, status)) {
        signal_process_group(process, SIGKILL);
        if (result_receiver.joinable()) {
          result_receiver.join();
        }
        clear_response_wait();
        std::lock_guard<std::mutex> lock(mutex);
        set_routes_active(false);
        child_pid   = -1;
        current     = stop_requested ? class_c_process_state::stopped : class_c_process_state::failed;
        last_detail = stop_requested ? std::string{} : "waitpid failed while supervising process";
        changed.notify_all();
        return;
      }
      {
        std::lock_guard<std::mutex>                  gate(result_mutex);
        std::shared_ptr<class_c_process_result_sink> sink;
        class_c_process_result_source                source;
        bool                                         notify = false;
        {
          std::lock_guard<std::mutex> lock(mutex);
          notify = routes_active && config.result_sink != nullptr;
          if (notify) {
            sink                    = config.result_sink;
            source.instance_id[0]   = result_instance_id[0];
            source.instance_id[1]   = result_instance_id[1];
            source.app_generation   = result_app_generation;
            source.route_generation = generation;
            const auto& route       = result_lifecycle_route();
            source.route_id         = route.offer.route_id;
            source.interface_id     = route.offer.interface_id;
          }
          set_routes_active(false);
        }
        if (notify) {
          try {
            static_cast<void>(sink->publication_changed(source, false));
          } catch (...) {
          }
        }
      }
      if (result_receiver.joinable()) {
        result_receiver.join();
      }
      clear_response_wait();
      {
        std::lock_guard<std::mutex> lock(mutex);
        child_pid = -1;
        control.reset();
        ++exits;
        last_status = status;
        if (stop_requested) {
          current = class_c_process_state::stopped;
          changed.notify_all();
          return;
        }
        // A generation that ran healthily for the configured window earns the
        // budget back: an occasional crash over months must stay
        // distinguishable from a crash loop.
        if (config.restart_budget_reset_after.count() > 0 &&
            std::chrono::steady_clock::now() - generation_started >= config.restart_budget_reset_after) {
          restart_attempts = 0;
        }
        current     = class_c_process_state::backoff;
        last_detail = "Class-C worker exited";
        changed.notify_all();
      }

      bool restarted = false;
      while (!restarted) {
        {
          std::unique_lock<std::mutex> lock(mutex);
          if (restart_attempts >= config.maximum_restart_attempts) {
            current     = class_c_process_state::failed;
            last_detail = "Class-C worker restart limit reached";
            changed.notify_all();
            return;
          }
          if (changed.wait_for(lock, restart_delay(restart_attempts), [this] { return stop_requested; })) {
            current = class_c_process_state::stopped;
            changed.notify_all();
            return;
          }
          ++restart_attempts;
        }
        if (!reset_routes(std::chrono::steady_clock::now() + config.stop_timeout)) {
          std::lock_guard<std::mutex> lock(mutex);
          current     = class_c_process_state::failed;
          last_detail = "Class-C route quiescence timed out before restart";
          changed.notify_all();
          return;
        }
        std::string failure;
        if (spawn_generation_contained(failure)) {
          std::lock_guard<std::mutex> lock(mutex);
          ++restarts;
          restarted = true;
        } else {
          std::lock_guard<std::mutex> lock(mutex);
          ++start_failures;
          if (stop_requested) {
            current = class_c_process_state::stopped;
            changed.notify_all();
            return;
          }
          current     = class_c_process_state::backoff;
          last_detail = std::move(failure);
          changed.notify_all();
        }
      }
    }
  }

  void receive_results(pid_t process, uint64_t process_generation) noexcept
  {
    uint32_t consecutive_failures = 0;
    uint64_t challenge            = 0;
    uint64_t requested_at_ns      = 0;
    bool     outstanding          = false;
    auto     next_request         = std::chrono::steady_clock::now() + config.heartbeat_period;
    auto     response_deadline    = next_request;
    for (;;) {
      ipc::seqpacket_channel* channel;
      {
        std::lock_guard<std::mutex> lock(mutex);
        if (child_pid != process || generation != process_generation || control == nullptr) {
          return;
        }
        channel = control.get();
      }
      // Heartbeat issuance and the timeout check run on every iteration, not
      // only when the poll times out: a worker that keeps the control socket
      // continuously readable must not be able to starve its own challenges.
      const auto now = std::chrono::steady_clock::now();
      if (outstanding && now >= response_deadline) {
        {
          std::lock_guard<std::mutex> gate(result_mutex);
          std::lock_guard<std::mutex> lock(mutex);
          if (child_pid != process || generation != process_generation || stop_requested)
            return;
          ++heartbeat_losses;
          last_detail = "Class-C worker heartbeat timed out";
          set_routes_active(false);
        }
        signal_process_group(process, SIGKILL);
        return;
      }
      if (!outstanding && now >= next_request) {
        ++challenge;
        if (challenge == 0)
          ++challenge;
        requested_at_ns = monotonic_time_ns();
        ocudu_dapp_process_heartbeat_v1 heartbeat{};
        heartbeat.struct_size       = sizeof(heartbeat);
        heartbeat.route_generation  = process_generation;
        heartbeat.challenge         = challenge;
        heartbeat.host_monotonic_ns = requested_at_ns;
        {
          std::lock_guard<std::mutex> send_lock(send_mutex);
          if (!channel->send(OCUDU_DAPP_CONTROL_HEARTBEAT_REQUEST_V1,
                             process_generation,
                             {reinterpret_cast<const uint8_t*>(&heartbeat), sizeof(heartbeat)})) {
            signal_process_group(process, SIGKILL);
            return;
          }
        }
        {
          std::lock_guard<std::mutex> lock(mutex);
          ++heartbeat_requests;
        }
        outstanding       = true;
        response_deadline = now + config.heartbeat_timeout;
      }
      const auto deadline = outstanding ? response_deadline : next_request;
      const auto wait =
          std::max(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now), std::chrono::milliseconds(1));
      if (!wait_readable(channel->fd(), wait)) {
        continue;
      }
      auto received = channel->receive();
      if (!received) {
        if (received.peer_closed) {
          return;
        }
        {
          std::lock_guard<std::mutex> lock(mutex);
          ++result_protocol_failures;
        }
        if (++consecutive_failures == maximum_protocol_failures) {
          signal_process_group(process, SIGKILL);
          return;
        }
        continue;
      }
      if (received.packet->type == OCUDU_DAPP_CONTROL_HEARTBEAT_RESPONSE_V1) {
        ocudu_dapp_process_heartbeat_v1 response{};
        bool valid_response = outstanding && received.packet->sequence == process_generation &&
                              received.packet->descriptors.empty() &&
                              received.packet->payload.size() == sizeof(response);
        if (valid_response) {
          std::memcpy(&response, received.packet->payload.data(), sizeof(response));
          valid_response = response.struct_size == sizeof(response) && response.flags == 0 &&
                           response.route_generation == process_generation && response.challenge == challenge &&
                           response.host_monotonic_ns == requested_at_ns &&
                           all_zero(response.reserved, std::size(response.reserved));
        }
        if (valid_response) {
          const uint64_t latency = monotonic_time_ns() - requested_at_ns;
          {
            std::lock_guard<std::mutex> lock(mutex);
            ++heartbeat_responses;
            last_heartbeat_latency_ns = latency;
          }
          consecutive_failures = 0;
          outstanding          = false;
          next_request         = std::chrono::steady_clock::now() + config.heartbeat_period;
          continue;
        }
        {
          std::lock_guard<std::mutex> lock(mutex);
          ++result_protocol_failures;
        }
        ++consecutive_failures;
        if (consecutive_failures == maximum_protocol_failures) {
          signal_process_group(process, SIGKILL);
          return;
        }
        continue;
      }
      if (received.packet->type == OCUDU_DAPP_CONTROL_MANAGEMENT_RESPONSE_V1) {
        ocudu_dapp_process_management_response_v1 management{};
        const bool framed = received.packet->sequence == process_generation && received.packet->descriptors.empty() &&
                            received.packet->payload.size() == sizeof(management);
        if (framed) {
          std::memcpy(&management, received.packet->payload.data(), sizeof(management));
        }
        bool delivered = false;
        if (framed) {
          std::lock_guard<std::mutex> lock(response_mutex);
          if (response_waiting && response_generation == process_generation &&
              response_request_id == management.request_id) {
            pending_response = management;
            response_waiting = false;
            delivered        = true;
            response_changed.notify_all();
          }
        }
        if (!delivered) {
          std::lock_guard<std::mutex> lock(mutex);
          ++result_protocol_failures;
          if (++consecutive_failures == maximum_protocol_failures) {
            signal_process_group(process, SIGKILL);
            return;
          }
        } else {
          consecutive_failures = 0;
        }
        continue;
      }
      const auto status = handle_result(*received.packet, process, process_generation);
      if (status == result_frame_status::fatal_protocol_error) {
        {
          std::lock_guard<std::mutex> lock(mutex);
          ++result_protocol_failures;
          last_detail = "Class-C worker emitted a result outside its declared capability";
          set_routes_active(false);
        }
        signal_process_group(process, SIGKILL);
        return;
      }
      if (status == result_frame_status::protocol_error) {
        {
          std::lock_guard<std::mutex> lock(mutex);
          ++result_protocol_failures;
        }
        if (++consecutive_failures == maximum_protocol_failures) {
          signal_process_group(process, SIGKILL);
          return;
        }
      } else if (status == result_frame_status::rejected) {
        if (++consecutive_failures == maximum_protocol_failures) {
          signal_process_group(process, SIGKILL);
          return;
        }
      } else {
        consecutive_failures = 0;
      }
    }
  }

  result_frame_status
  handle_result(const ipc::control_packet& packet, pid_t process, uint64_t process_generation) noexcept
  {
    if (packet.sequence != process_generation || !packet.descriptors.empty()) {
      return result_frame_status::protocol_error;
    }
    if (packet.type == OCUDU_DAPP_CONTROL_SPECTRUM_RESULT_V1 &&
        packet.payload.size() == sizeof(ocudu_dapp_process_spectrum_result_v1)) {
      ocudu_dapp_process_spectrum_result_v1 result{};
      std::memcpy(&result, packet.payload.data(), sizeof(result));
      // Range checks live here at the trust boundary, not only in the sink:
      // class_c_process_result_sink is a public interface, and a second
      // implementation must not inherit a buffer overflow by forgetting one of
      // these clauses. The sink's own checks remain as redundant defence.
      if (result.struct_size != sizeof(result) || result.schema_id != OCUDU_DAPP_CONTEXT_INTERFERENCE_MAP_V1 ||
          result.route_id == 0 || result.source_grid_sequence == 0 || result.nof_prbs == 0 ||
          result.nof_prbs > OCUDU_DAPP_SPECTRUM_MAX_PRBS_V1 ||
          result.first_prb > OCUDU_DAPP_SPECTRUM_MAX_PRBS_V1 - result.nof_prbs || result.confidence_q16 > (1U << 16U) ||
          std::any_of(
              std::begin(result.reserved0), std::end(result.reserved0), [](uint8_t value) { return value != 0; }) ||
          !all_zero(result.reserved, std::size(result.reserved))) {
        reject_result();
        return result_frame_status::rejected;
      }
      return dispatch_result(result,
                             result.route_id,
                             OCUDU_DAPP_INTERFACE_SPECTRUM_V1,
                             class_c_process_result_capability::spectrum_map,
                             process,
                             process_generation,
                             [&](class_c_process_endpoint& endpoint) {
                               return endpoint.consume_result_source(result.source_grid_sequence,
                                                                     result.source_plan_generation,
                                                                     result.cell_id,
                                                                     result.created);
                             });
    }
    if (packet.type == OCUDU_DAPP_CONTROL_SRS_ISAC_RESULT_V1 &&
        packet.payload.size() == sizeof(ocudu_dapp_process_srs_isac_result_v1)) {
      ocudu_dapp_process_srs_isac_result_v1 result{};
      std::memcpy(&result, packet.payload.data(), sizeof(result));
      const auto& preview = result.preview;
      // As above: array-bounding fields are validated at the trust boundary.
      if (result.struct_size != sizeof(result) || result.schema_id != OCUDU_DAPP_REPORT_SRS_CIR_PREVIEW_V1 ||
          result.route_id == 0 || preview.source_srs_sequence == 0 || preview.tap_count == 0 ||
          preview.tap_count > OCUDU_DAPP_SRS_ISAC_MAX_CIR_TAPS_V1 || preview.peak_delay_bin >= preview.tap_count ||
          !all_zero(result.reserved, std::size(result.reserved))) {
        reject_result();
        return result_frame_status::rejected;
      }
      return dispatch_result(result,
                             result.route_id,
                             OCUDU_DAPP_INTERFACE_SRS_ISAC_V1,
                             class_c_process_result_capability::srs_preview,
                             process,
                             process_generation,
                             [&](class_c_process_endpoint& endpoint) {
                               return endpoint.consume_srs_result_source(preview.source_srs_sequence,
                                                                         preview.srs_resource_id,
                                                                         preview.srs_config_id,
                                                                         preview.context.cell_id,
                                                                         preview.context.created);
                             });
    }
    if (packet.type == OCUDU_DAPP_CONTROL_QUIET_INTENT_RESULT_V1 &&
        packet.payload.size() == sizeof(ocudu_dapp_process_quiet_intent_result_v1)) {
      ocudu_dapp_process_quiet_intent_result_v1 result{};
      std::memcpy(&result, packet.payload.data(), sizeof(result));
      if (result.struct_size != sizeof(result) ||
          result.schema_id != OCUDU_DAPP_SPECTRUM_OUTPUT_QUIET_PERIOD_INTENT_V1 || result.route_id == 0 ||
          result.source_grid_sequence == 0 || result.reserved0 != 0 ||
          !all_zero(result.reserved, std::size(result.reserved))) {
        reject_result();
        return result_frame_status::rejected;
      }
      return dispatch_result(result,
                             result.route_id,
                             OCUDU_DAPP_INTERFACE_SPECTRUM_V1,
                             class_c_process_result_capability::quiet_intent,
                             process,
                             process_generation,
                             [&](class_c_process_endpoint& endpoint) {
                               return endpoint.consume_quiet_intent_source(result.source_grid_sequence,
                                                                           result.source_plan_generation,
                                                                           result.cell_id,
                                                                           result.created);
                             });
    }
    return result_frame_status::protocol_error;
  }

  void reject_result() noexcept
  {
    std::lock_guard<std::mutex> lock(mutex);
    ++rejected_results;
  }

  template <typename Result, typename Consume>
  result_frame_status dispatch_result(const Result&                     result,
                                      uint64_t                          route_id,
                                      uint32_t                          interface_id,
                                      class_c_process_result_capability required_capability,
                                      pid_t                             process,
                                      uint64_t                          process_generation,
                                      Consume&&                         consume) noexcept
  {
    std::lock_guard<std::mutex>                  gate(result_mutex);
    std::shared_ptr<class_c_process_endpoint>    endpoint;
    std::shared_ptr<class_c_process_result_sink> sink;
    class_c_process_result_source                source;
    {
      std::lock_guard<std::mutex> lock(mutex);
      auto route = std::find_if(config.routes.begin(), config.routes.end(), [&](const auto& candidate) {
        return candidate.offer.route_id == route_id;
      });
      if (config.results_prohibited ||
          (route != config.routes.end() &&
           (route->result_capabilities & static_cast<uint32_t>(required_capability)) == 0)) {
        return result_frame_status::fatal_protocol_error;
      }
      if (child_pid != process || generation != process_generation || current != class_c_process_state::running ||
          !routes_active || route == config.routes.end() || route->offer.interface_id != interface_id ||
          config.result_sink == nullptr || result_app_generation == 0 ||
          (result_instance_id[0] == 0 && result_instance_id[1] == 0)) {
        ++rejected_results;
        return result_frame_status::rejected;
      }
      endpoint                = route->endpoint;
      sink                    = config.result_sink;
      source.instance_id[0]   = result_instance_id[0];
      source.instance_id[1]   = result_instance_id[1];
      source.app_generation   = result_app_generation;
      source.route_generation = process_generation;
      source.route_id         = route_id;
      source.interface_id     = route->offer.interface_id;
    }
    if (!consume(*endpoint)) {
      reject_result();
      return result_frame_status::rejected;
    }

    ocudu_dapp_status_v1 status = OCUDU_DAPP_EXCEPTION_V1;
    try {
      status = sink->publish(source, result);
    } catch (...) {
    }
    std::lock_guard<std::mutex> lock(mutex);
    if (status == OCUDU_DAPP_OK_V1) {
      ++accepted_results;
    } else if (status == OCUDU_DAPP_BYPASS_V1) {
      ++busy_results;
    } else {
      ++rejected_results;
      return result_frame_status::rejected;
    }
    return result_frame_status::processed;
  }

  class_c_process_supervisor_config                        config;
  mutable std::mutex                                       management_mutex;
  mutable std::mutex                                       command_mutex;
  mutable std::mutex                                       send_mutex;
  mutable std::mutex                                       response_mutex;
  std::condition_variable                                  response_changed;
  mutable std::mutex                                       mutex;
  mutable std::condition_variable                          changed;
  mutable std::mutex                                       result_mutex;
  std::thread                                              monitor;
  std::unique_ptr<ipc::seqpacket_channel>                  control;
  class_c_process_state                                    current{class_c_process_state::stopped};
  pid_t                                                    child_pid{-1};
  bool                                                     stop_requested{};
  bool                                                     publication_requested{};
  bool                                                     routes_active{};
  uint32_t                                                 restart_attempts{};
  uint64_t                                                 jitter_state{0x9E3779B97F4A7C15ULL};
  uint64_t                                                 generation{};
  uint64_t                                                 starts{};
  uint64_t                                                 restarts{};
  uint64_t                                                 exits{};
  uint64_t                                                 start_failures{};
  uint64_t                                                 forced_kills{};
  uint64_t                                                 accepted_results{};
  uint64_t                                                 busy_results{};
  uint64_t                                                 rejected_results{};
  uint64_t                                                 result_protocol_failures{};
  uint64_t                                                 heartbeat_requests{};
  uint64_t                                                 heartbeat_responses{};
  uint64_t                                                 heartbeat_losses{};
  uint64_t                                                 last_heartbeat_latency_ns{};
  uint64_t                                                 management_responses{};
  uint64_t                                                 management_replays{};
  uint64_t                                                 management_timeouts{};
  uint64_t                                                 management_request_id{};
  bool                                                     response_waiting{};
  uint64_t                                                 response_generation{};
  uint64_t                                                 response_request_id{};
  std::optional<ocudu_dapp_process_management_response_v1> pending_response;
  std::vector<replay_operation>                            replay_log;
  std::size_t                                              replay_bytes{};
  uint64_t                                                 result_instance_id[2]{};
  uint64_t                                                 result_app_generation{};
  int32_t                                                  last_status{};
  std::string                                              last_detail;
};

class_c_process_supervisor::class_c_process_supervisor(std::unique_ptr<implementation> state_) noexcept :
  state(std::move(state_))
{
}

class_c_process_supervisor::~class_c_process_supervisor() = default;

class_c_process_supervisor_create_result
class_c_process_supervisor::create(class_c_process_supervisor_config config) noexcept
{
  try {
    if (config.interfaces.empty() && !config.routes.empty()) {
      config.interfaces.push_back({config.routes.front().offer.interface_id,
                                   static_cast<uint16_t>(config.routes.size()),
                                   OCUDU_DAPP_PROCESS_BINDING_REQUIRED_V1,
                                   OCUDU_DAPP_PROCESS_BINDING_BOUND_V1});
    }
    for (auto& route : config.routes) {
      if (route.result_capabilities == UINT32_MAX) {
        route.result_capabilities = allowed_result_capabilities(route.offer.interface_id);
      }
    }
    const std::string error = validate_config(config);
    if (!error.empty()) {
      return {{OCUDU_DAPP_INVALID_INPUT_V1, error}, {}};
    }
    auto state = std::make_unique<implementation>(std::move(config));
    return {{OCUDU_DAPP_OK_V1, {}},
            std::unique_ptr<class_c_process_supervisor>(new class_c_process_supervisor(std::move(state)))};
  } catch (const std::bad_alloc&) {
    return {{OCUDU_DAPP_RESOURCE_V1, "unable to allocate Class-C process supervisor"}, {}};
  } catch (...) {
    return {{OCUDU_DAPP_INTERNAL_ERROR_V1, "unknown Class-C process supervisor construction failure"}, {}};
  }
}

class_c_process_operation_result class_c_process_supervisor::start() noexcept
{
  return state->start();
}

class_c_process_operation_result class_c_process_supervisor::stop() noexcept
{
  return state->stop();
}

class_c_process_operation_result class_c_process_supervisor::set_publication_enabled(bool enabled) noexcept
{
  return state->set_publication_enabled(enabled);
}

class_c_process_management_result class_c_process_supervisor::manage(ocudu_dapp_process_management_request_v1 request,
                                                                     span<const uint8_t> artifact) noexcept
{
  return state->manage(request, artifact);
}

void class_c_process_supervisor::set_result_identity(const uint64_t instance_id[2], uint64_t app_generation) noexcept
{
  state->set_result_identity(instance_id, app_generation);
}

class_c_process_snapshot class_c_process_supervisor::snapshot() const
{
  return state->snapshot();
}

bool class_c_process_supervisor::wait_for_state(class_c_process_state     expected,
                                                std::chrono::milliseconds timeout) const noexcept
{
  return state->wait_for_state(expected, timeout);
}

const char* ocudu::dapp::to_string(class_c_process_state state) noexcept
{
  static constexpr const char* names[] = {"stopped", "starting", "running", "backoff", "stopping", "failed"};
  const auto                   index   = static_cast<unsigned>(state);
  return index < std::size(names) ? names[index] : "unknown";
}
