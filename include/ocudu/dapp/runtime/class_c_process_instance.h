// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "class_c_process_supervisor.h"
#include "incident.h"
#include "instance_runtime.h"
#include "lifecycle.h"
#include <array>
#include <memory>
#include <mutex>

namespace ocudu::dapp {

class class_c_process_instance;

struct class_c_process_instance_create_result {
  instance_operation_result                 operation;
  std::unique_ptr<class_c_process_instance> instance;
  explicit                                  operator bool() const noexcept { return operation && instance != nullptr; }
};

/// Common lifecycle and management owner for one supervised Class-C executable.
class class_c_process_instance final : public instance_runtime
{
public:
  static class_c_process_instance_create_result create(std::unique_ptr<class_c_process_supervisor> supervisor) noexcept;

  ~class_c_process_instance() override;
  class_c_process_instance(const class_c_process_instance&)            = delete;
  class_c_process_instance& operator=(const class_c_process_instance&) = delete;

  instance_operation_result prepare(const void* typed_config, const void* prepare_args) override;
  instance_operation_result arm(const void* admitted_shapes) override;
  instance_operation_result enter_shadow() override;
  instance_operation_result activate() override;
  instance_operation_result drain(std::chrono::steady_clock::time_point deadline) override;
  instance_operation_result retry_quiescence(std::chrono::steady_clock::time_point deadline) override;
  instance_operation_result unload() override;

  uint64_t                 management_capabilities() const override;
  instance_health_snapshot health() const override;
  management_call_result   describe_config(ocudu_dapp_config_descriptor_v1* descriptors, uint32_t capacity) override;
  management_call_result   get_config(ocudu_dapp_config_entry_v1* entries, uint32_t capacity) override;
  management_call_result   set_config(const ocudu_dapp_config_update_v1& update) override;
  management_call_result   describe_metrics(ocudu_dapp_metric_descriptor_v1* descriptors, uint32_t capacity) override;
  management_call_result   read_metrics(ocudu_dapp_metric_sample_v1* samples, uint32_t capacity) override;
  management_call_result   manage_model(const ocudu_dapp_model_request_v1& request) override;

  ocudu_dapp_lifecycle_state_v1 state() const override;
  std::size_t                   drain_incidents(incident_store& destination) override;
  void set_incident_identity(const uint64_t instance_id[2], const uint64_t package_id[2], uint64_t generation) override;

private:
  explicit class_c_process_instance(std::unique_ptr<class_c_process_supervisor> supervisor_) noexcept;

  instance_operation_result result(ocudu_dapp_status_v1 status, const char* detail = nullptr) const;
  void                      refresh_locked() const;
  void                      synchronize_counters_locked() const;
  void                      record_incident(incident_phase       phase,
                                            incident_cause       cause,
                                            incident_action      action,
                                            ocudu_dapp_status_v1 status,
                                            const char*          detail,
                                            bool                 breaker_open) const noexcept;

  mutable std::mutex                          management_mutex;
  std::unique_ptr<class_c_process_supervisor> supervisor;
  mutable lifecycle                           lifecycle_state;
  mutable incident_ring<64>                   incidents;
  mutable incident_context                    incident_identity;
  mutable uint64_t                            observed_exits{};
  mutable uint64_t                            observed_restarts{};
  mutable uint64_t                            observed_rejected_results{};
  mutable uint64_t                            observed_protocol_failures{};
  mutable uint64_t                            observed_heartbeat_losses{};
  mutable uint64_t                            observed_management_timeouts{};
  std::array<ocudu_dapp_process_config_descriptor_v1, OCUDU_DAPP_PROCESS_MANAGEMENT_MAX_ENTRIES_V1>
                                                                                               descriptor_storage{};
  std::array<ocudu_dapp_process_config_entry_v1, OCUDU_DAPP_PROCESS_MANAGEMENT_MAX_ENTRIES_V1> config_storage{};
};

} // namespace ocudu::dapp
