// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "artifact_verifier.h"
#include "class_c_process_endpoint.h"
#include "ocudu/adt/span.h"
#include "ocudu/dapp/abi/v1/base.h"
#include "ocudu/dapp/process/v1/control.h"
#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace ocudu::dapp {

enum class class_c_process_result_capability : uint32_t {
  spectrum_map = UINT32_C(1) << 0U,
  quiet_intent = UINT32_C(1) << 1U,
  srs_preview  = UINT32_C(1) << 2U
};

constexpr uint32_t operator|(class_c_process_result_capability left, class_c_process_result_capability right) noexcept
{
  return static_cast<uint32_t>(left) | static_cast<uint32_t>(right);
}

struct class_c_process_result_source {
  uint64_t instance_id[2]{};
  uint64_t app_generation{};
  uint64_t route_generation{};
  uint64_t route_id{};
  uint32_t interface_id{};
};

class class_c_process_result_sink
{
public:
  virtual ~class_c_process_result_sink() = default;

  virtual ocudu_dapp_status_v1 publish(const class_c_process_result_source&         source,
                                       const ocudu_dapp_process_spectrum_result_v1& result)                  = 0;
  virtual ocudu_dapp_status_v1 publish(const class_c_process_result_source&             source,
                                       const ocudu_dapp_process_quiet_intent_result_v1& result)              = 0;
  virtual ocudu_dapp_status_v1 publish(const class_c_process_result_source&         source,
                                       const ocudu_dapp_process_srs_isac_result_v1& result)                  = 0;
  virtual ocudu_dapp_status_v1 publication_changed(const class_c_process_result_source& source, bool active) = 0;
};

struct class_c_process_route_binding {
  ocudu_dapp_route_offer_v1                 offer{};
  std::shared_ptr<class_c_process_endpoint> endpoint;
  /// UINT32_MAX preserves the original single-interface test/embedding API and
  /// is normalized to that interface's supported result set at construction.
  uint32_t result_capabilities{UINT32_MAX};
};

struct class_c_process_supervisor_config {
  std::unique_ptr<approved_artifact> executable;
  std::vector<std::string>           arguments;
  std::vector<std::string>           environment;
  /// Manifest-ordered resolved bindings. Empty preserves the original
  /// single-interface process handshake and is inferred from routes.
  std::vector<ocudu_dapp_process_interface_binding_v1> interfaces;
  std::vector<class_c_process_route_binding>           routes;
  std::shared_ptr<class_c_process_result_sink>         result_sink;
  /// Treat every worker result as a fatal capability violation. Used by
  /// observer-only packages that are admitted to inputs but declare no output.
  bool results_prohibited{};
  /// Maximum time for the spawned worker to signal readiness.
  std::chrono::milliseconds startup_timeout{1000};
  /// Grace period after a stop request before the worker is killed.
  std::chrono::milliseconds stop_timeout{1000};
  /// Base delay between supervised restart attempts. Grows exponentially with
  /// consecutive attempts (with jitter) up to maximum_restart_backoff.
  std::chrono::milliseconds restart_backoff{100};
  /// Fixed supervisor policy (not an operator YAML knob).
  /// Ceiling for the exponentially growing restart backoff.
  std::chrono::milliseconds maximum_restart_backoff{5000};
  /// Healthy uptime after which the restart budget resets, so a worker that
  /// crashes occasionally over months is not permanently failed like a crash
  /// loop. Zero keeps the lifetime budget.
  std::chrono::milliseconds restart_budget_reset_after{60'000};
  /// Interval between authenticated heartbeat challenges.
  std::chrono::milliseconds heartbeat_period{1000};
  /// Maximum time for a heartbeat response before it counts as a loss.
  std::chrono::milliseconds heartbeat_timeout{3000};
  /// Maximum time for one configuration or model operation on the worker.
  std::chrono::milliseconds management_timeout{1000};
  /// Zero inherits the host address-space limit. A nonzero value becomes an
  /// irrevocable soft and hard RLIMIT_AS before route descriptors are sent.
  uint64_t maximum_address_space_bytes{};
  /// Maximum bytes in an isolated-worker CPU core. Zero disables worker core
  /// dumps. A nonzero value becomes an irrevocable soft and hard RLIMIT_CORE
  /// before route descriptors are sent.
  uint64_t maximum_core_file_bytes{};
  /// Require the worker to enter Linux seccomp filter mode before any route
  /// descriptor is transferred. Keep false only for explicitly trusted legacy
  /// workers that predate the restricted process profile.
  bool require_seccomp_filter{};
  /// Irrevocable soft and hard RLIMIT_NOFILE applied before route admission.
  uint32_t maximum_open_files{512};
  uint32_t maximum_restart_attempts{3};
};

enum class class_c_process_state : uint8_t { stopped, starting, running, backoff, stopping, failed };

struct class_c_process_snapshot {
  class_c_process_state            state{class_c_process_state::stopped};
  int64_t                          pid{-1};
  bool                             publication_enabled{};
  bool                             publication_requested{};
  uint64_t                         route_generation{};
  uint64_t                         successful_starts{};
  uint64_t                         successful_restarts{};
  uint64_t                         process_exits{};
  uint64_t                         start_failures{};
  uint64_t                         forced_kills{};
  uint64_t                         accepted_results{};
  uint64_t                         busy_results{};
  uint64_t                         rejected_results{};
  uint64_t                         result_protocol_failures{};
  uint64_t                         heartbeat_requests{};
  uint64_t                         heartbeat_responses{};
  uint64_t                         heartbeat_losses{};
  uint64_t                         last_heartbeat_latency_ns{};
  uint64_t                         management_responses{};
  uint64_t                         management_replays{};
  uint64_t                         management_timeouts{};
  class_c_process_endpoint_metrics route_metrics{};
  int32_t                          last_wait_status{};
  std::string                      detail;
};

struct class_c_process_operation_result {
  ocudu_dapp_status_v1 status{OCUDU_DAPP_INTERNAL_ERROR_V1};
  std::string          detail;
  explicit             operator bool() const noexcept { return status == OCUDU_DAPP_OK_V1; }
};

struct class_c_process_management_result {
  class_c_process_operation_result          operation;
  ocudu_dapp_process_management_response_v1 response{};
  explicit operator bool() const noexcept { return operation && response.status == OCUDU_DAPP_OK_V1; }
};

class class_c_process_supervisor;

struct class_c_process_supervisor_create_result {
  class_c_process_operation_result            operation;
  std::unique_ptr<class_c_process_supervisor> supervisor;
  explicit operator bool() const noexcept { return operation && supervisor != nullptr; }
};

/// Supervises one separately built Class-C executable and one complete route bundle.
class class_c_process_supervisor
{
public:
  static class_c_process_supervisor_create_result create(class_c_process_supervisor_config config) noexcept;

  ~class_c_process_supervisor();
  class_c_process_supervisor(const class_c_process_supervisor&)            = delete;
  class_c_process_supervisor& operator=(const class_c_process_supervisor&) = delete;

  class_c_process_operation_result  start() noexcept;
  class_c_process_operation_result  stop() noexcept;
  class_c_process_operation_result  set_publication_enabled(bool enabled) noexcept;
  class_c_process_management_result manage(ocudu_dapp_process_management_request_v1 request,
                                           span<const uint8_t>                      artifact = {}) noexcept;
  void                     set_result_identity(const uint64_t instance_id[2], uint64_t app_generation) noexcept;
  class_c_process_snapshot snapshot() const;
  bool wait_for_state(class_c_process_state state, std::chrono::milliseconds timeout) const noexcept;

private:
  class implementation;
  explicit class_c_process_supervisor(std::unique_ptr<implementation> state_) noexcept;
  std::unique_ptr<implementation> state;
};

const char* to_string(class_c_process_state state) noexcept;

} // namespace ocudu::dapp
