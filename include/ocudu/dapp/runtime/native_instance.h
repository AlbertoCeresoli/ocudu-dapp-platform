// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "guarded_call.h"
#include "instance_runtime.h"
#include "lifecycle.h"
#include "module_loader.h"
#include "native_worker.h"
#include "quiescence_tracker.h"
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace ocudu::dapp {

class native_instance;
class incident_store;
struct native_worker_incident_source;

struct instance_create_result {
  instance_operation_result        operation;
  std::unique_ptr<native_instance> instance;
  explicit operator bool() const noexcept { return static_cast<bool>(operation) && instance != nullptr; }
};

/// Serialized management owner for one in-process module instance.
class native_instance : public instance_runtime
{
public:
  static instance_create_result create(std::shared_ptr<native_module_library> library,
                                       const ocudu_dapp_host_api_v1&          host_api,
                                       const void*                            create_args = nullptr,
                                       uint32_t selected_backend                          = OCUDU_DAPP_BACKEND_HOST_V1);

  ~native_instance() override;
  native_instance(const native_instance&)            = delete;
  native_instance& operator=(const native_instance&) = delete;

  instance_operation_result prepare(const void* typed_config, const void* prepare_args) override;
  instance_operation_result arm(const void* admitted_shapes) override;
  instance_operation_result enter_shadow() override;
  instance_operation_result activate() override;
  instance_operation_result drain(std::chrono::steady_clock::time_point deadline) override;
  instance_operation_result retry_quiescence(std::chrono::steady_clock::time_point deadline) override;
  instance_operation_result unload() override;
  instance_operation_result reset_breakers() override;

  uint64_t                 management_capabilities() const override;
  instance_health_snapshot health() const override;
  management_call_result   describe_config(ocudu_dapp_config_descriptor_v1* descriptors, uint32_t capacity) override;
  management_call_result   get_config(ocudu_dapp_config_entry_v1* entries, uint32_t capacity) override;
  management_call_result   set_config(const ocudu_dapp_config_update_v1& update) override;
  management_call_result   describe_metrics(ocudu_dapp_metric_descriptor_v1* descriptors, uint32_t capacity) override;
  management_call_result   read_metrics(ocudu_dapp_metric_sample_v1* samples, uint32_t capacity) override;
  management_call_result   manage_model(const ocudu_dapp_model_request_v1& request) override;
  native_worker_create_result create_worker(uint32_t interface_id, uint32_t worker_id);
  uint32_t                    backend_id() const noexcept { return selected_backend_id; }

  activity_token try_enter(activity_kind kind = activity_kind::invocation) noexcept
  {
    return activity->try_acquire(kind);
  }
  bool                accepts_activity() const noexcept { return activity->admission_open(); }
  quiescence_snapshot outstanding_activity() const noexcept { return activity->snapshot(); }

  ocudu_dapp_lifecycle_state_v1 state() const override;
  bool                          try_pop_incident(incident_record& record) noexcept;
  uint64_t                      dropped_incidents() const noexcept;
  std::size_t                   drain_incidents(incident_store& destination) override;
  void set_incident_identity(const uint64_t instance_id[2], const uint64_t package_id[2], uint64_t generation) override;

private:
  friend class native_worker;
  native_instance(std::shared_ptr<native_module_library> library_,
                  const ocudu_dapp_host_api_v1&          host_api_,
                  uint32_t                               selected_backend_);

  instance_operation_result call_locked(incident_phase                               phase,
                                        const char*                                  operation,
                                        bool                                         cleanup,
                                        const std::function<ocudu_dapp_status_v1()>& callback);
  instance_operation_result result(ocudu_dapp_status_v1 status, const char* detail = nullptr) const;
  instance_operation_result quarantine(incident_cause cause, ocudu_dapp_status_v1 status, const char* detail);

  mutable std::mutex                                          management_mutex;
  std::shared_ptr<native_module_library>                      library;
  ocudu_dapp_module_v1                                        module{};
  ocudu_dapp_host_api_v1                                      host_api{};
  void*                                                       module_instance{};
  lifecycle                                                   lifecycle_state;
  std::shared_ptr<quiescence_tracker>                         activity;
  circuit_breaker                                             lifecycle_breaker;
  incident_ring<64>                                           incidents;
  incident_context                                            incident_identity;
  bool                                                        started{};
  bool                                                        destroy_attempted{};
  const uint32_t                                              selected_backend_id;
  mutable std::mutex                                          worker_sources_mutex;
  std::vector<std::shared_ptr<native_worker_incident_source>> worker_sources;
};

/// Number of intentionally mapped libraries retained after unsafe owner destruction.
std::size_t quarantined_native_library_count();

} // namespace ocudu::dapp
