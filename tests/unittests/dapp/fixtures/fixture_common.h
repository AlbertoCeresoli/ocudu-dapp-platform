// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "ocudu/dapp/abi/v1/module.h"
#include <atomic>
#include <stdexcept>

struct fixture_control {
  ocudu_dapp_status_v1              configure_status{OCUDU_DAPP_OK_V1};
  ocudu_dapp_status_v1              prepare_status{OCUDU_DAPP_OK_V1};
  ocudu_dapp_status_v1              warmup_status{OCUDU_DAPP_OK_V1};
  ocudu_dapp_status_v1              start_status{OCUDU_DAPP_OK_V1};
  ocudu_dapp_status_v1              stop_status{OCUDU_DAPP_OK_V1};
  ocudu_dapp_status_v1              quiesce_status{OCUDU_DAPP_OK_V1};
  bool                              throw_create{};
  bool                              throw_stop{};
  bool                              throw_quiesce{};
  bool                              throw_destroy{};
  uint32_t                          creates{};
  uint32_t                          configures{};
  uint32_t                          prepares{};
  uint32_t                          warmups{};
  uint32_t                          starts{};
  uint32_t                          stops{};
  uint32_t                          quiesces{};
  uint32_t                          destroys{};
  uint64_t                          config_generation{1};
  uint64_t                          model_generation{1};
  uint32_t                          management_calls{};
  bool                              throw_management{};
  std::atomic<uint32_t>             data_worker_creates{};
  std::atomic<uint32_t>             data_worker_destroys{};
  std::atomic<uint32_t>             data_invocations{};
  std::atomic<uint32_t>             advance_plan_calls{};
  std::atomic<uint32_t>             advance_plan_spans{};
  std::atomic<uint32_t>             active_data_invocations{};
  std::atomic<uint32_t>             maximum_active_data_invocations{};
  std::atomic<bool>                 block_data_invocations{};
  std::atomic<bool>                 throw_data_invocation{};
  std::atomic<ocudu_dapp_status_v1> data_status{OCUDU_DAPP_OK_V1};
  std::atomic<bool>                 invalid_data_output{};
  std::atomic<bool>                 replace_data_output{};
  std::atomic<bool>                 publish_interference{};
  std::atomic<bool>                 publish_quiet_intent{};
  std::atomic<bool>                 publish_prb_avoidance{};
  std::atomic<bool>                 publish_srs_preview{};
  std::atomic<bool>                 throw_after_prepared_output{};
  std::atomic<uint64_t>             monotonic_ns{1};
  ocudu_dapp_host_api_v1            host_api{};
};

inline void fixture_maybe_throw(fixture_control* control)
{
  ++control->management_calls;
  if (control->throw_management) {
    throw std::runtime_error("fixture management exception");
  }
}

inline ocudu_dapp_status_v1 fixture_describe_config(void*                            instance,
                                                    ocudu_dapp_config_descriptor_v1* descriptors,
                                                    uint32_t                         capacity,
                                                    uint32_t*                        count)
{
  auto* control = static_cast<fixture_control*>(instance);
  fixture_maybe_throw(control);
  *count = 1;
  if (descriptors == nullptr || capacity == 0) {
    return OCUDU_DAPP_OK_V1;
  }
  descriptors[0]             = {};
  descriptors[0].struct_size = sizeof(descriptors[0]);
  descriptors[0].key         = "threshold";
  descriptors[0].description = "test threshold";
  descriptors[0].value_kind  = OCUDU_DAPP_VALUE_UINT64_V1;
  return OCUDU_DAPP_OK_V1;
}

inline ocudu_dapp_status_v1 fixture_get_config(void*                       instance,
                                               ocudu_dapp_config_entry_v1* entries,
                                               uint32_t                    capacity,
                                               uint32_t*                   count,
                                               uint64_t*                   generation)
{
  auto* control = static_cast<fixture_control*>(instance);
  fixture_maybe_throw(control);
  *count      = 1;
  *generation = control->config_generation;
  if (entries != nullptr && capacity != 0) {
    entries[0]                          = {};
    entries[0].key                      = "threshold";
    entries[0].value.kind               = OCUDU_DAPP_VALUE_UINT64_V1;
    entries[0].value.value.uint64_value = 7;
  }
  return OCUDU_DAPP_OK_V1;
}

inline ocudu_dapp_status_v1
fixture_set_config(void* instance, const ocudu_dapp_config_update_v1* update, uint64_t* generation)
{
  auto* control = static_cast<fixture_control*>(instance);
  fixture_maybe_throw(control);
  if (update->expected_generation != control->config_generation) {
    return OCUDU_DAPP_CONFLICT_V1;
  }
  if (!update->dry_run) {
    ++control->config_generation;
  }
  *generation = control->config_generation;
  return OCUDU_DAPP_OK_V1;
}

inline ocudu_dapp_status_v1 fixture_describe_metrics(void*                            instance,
                                                     ocudu_dapp_metric_descriptor_v1* descriptors,
                                                     uint32_t                         capacity,
                                                     uint32_t*                        count)
{
  auto* control = static_cast<fixture_control*>(instance);
  fixture_maybe_throw(control);
  *count = 1;
  if (descriptors != nullptr && capacity != 0) {
    descriptors[0]             = {};
    descriptors[0].struct_size = sizeof(descriptors[0]);
    descriptors[0].name        = "fixture.calls";
    descriptors[0].unit        = "calls";
    descriptors[0].value_kind  = OCUDU_DAPP_VALUE_UINT64_V1;
  }
  return OCUDU_DAPP_OK_V1;
}

inline ocudu_dapp_status_v1
fixture_read_metrics(void* instance, ocudu_dapp_metric_sample_v1* samples, uint32_t capacity, uint32_t* count)
{
  auto* control = static_cast<fixture_control*>(instance);
  fixture_maybe_throw(control);
  *count = 1;
  if (samples != nullptr && capacity != 0) {
    samples[0]                          = {};
    samples[0].name                     = "fixture.calls";
    samples[0].value.kind               = OCUDU_DAPP_VALUE_UINT64_V1;
    samples[0].value.value.uint64_value = control->management_calls;
  }
  return OCUDU_DAPP_OK_V1;
}

inline ocudu_dapp_status_v1
fixture_manage_model(void* instance, const ocudu_dapp_model_request_v1* request, uint64_t* generation)
{
  auto* control = static_cast<fixture_control*>(instance);
  fixture_maybe_throw(control);
  if (request->expected_generation != control->model_generation) {
    return OCUDU_DAPP_CONFLICT_V1;
  }
  if (!request->dry_run) {
    ++control->model_generation;
  }
  *generation = control->model_generation;
  return OCUDU_DAPP_OK_V1;
}

inline ocudu_dapp_status_v1 fixture_create(const ocudu_dapp_host_api_v1* host, const void* args, void** instance)
{
  auto* control = static_cast<fixture_control*>(const_cast<void*>(args));
  if (control == nullptr) {
    static fixture_control default_control;
    control = &default_control;
  }
  ++control->creates;
  if (control->throw_create) {
    throw std::runtime_error("fixture create exception");
  }
  if (host != nullptr) {
    control->host_api = *host;
  }
  *instance = control;
  return OCUDU_DAPP_OK_V1;
}
inline ocudu_dapp_status_v1 fixture_configure(void* instance, const void*)
{
  auto* control = static_cast<fixture_control*>(instance);
  ++control->configures;
  return control->configure_status;
}
inline ocudu_dapp_status_v1 fixture_prepare(void* instance, const void*)
{
  auto* control = static_cast<fixture_control*>(instance);
  ++control->prepares;
  return control->prepare_status;
}
inline ocudu_dapp_status_v1 fixture_warmup(void* instance, const void*)
{
  auto* control = static_cast<fixture_control*>(instance);
  ++control->warmups;
  return control->warmup_status;
}
inline ocudu_dapp_status_v1 fixture_start(void* instance)
{
  auto* control = static_cast<fixture_control*>(instance);
  ++control->starts;
  return control->start_status;
}
inline ocudu_dapp_status_v1 fixture_stop(void* instance)
{
  auto* control = static_cast<fixture_control*>(instance);
  ++control->stops;
  if (control->throw_stop) {
    throw std::runtime_error("fixture stop exception");
  }
  return control->stop_status;
}
inline ocudu_dapp_status_v1 fixture_quiesce(void* instance, uint64_t)
{
  auto* control = static_cast<fixture_control*>(instance);
  ++control->quiesces;
  if (control->throw_quiesce) {
    throw std::runtime_error("fixture quiesce exception");
  }
  return control->quiesce_status;
}
inline void fixture_destroy(void* instance)
{
  auto* control = static_cast<fixture_control*>(instance);
  ++control->destroys;
  if (control->throw_destroy) {
    throw std::runtime_error("fixture destroy exception");
  }
}

inline void populate_valid_module(ocudu_dapp_module_v1* module)
{
  static const uint64_t                           interface_table = 0;
  static const ocudu_dapp_interface_descriptor_v1 interface       = {sizeof(ocudu_dapp_interface_descriptor_v1),
                                                                     OCUDU_DAPP_ABI_MAJOR_V1,
                                                                     OCUDU_DAPP_ABI_MINOR_V1,
                                                                     0x1234,
                                                                     1,
                                                                     0,
                                                                     OCUDU_DAPP_CLASS_C_OPEN_LOOP_V1,
                                                                     1u << (OCUDU_DAPP_PLACEMENT_IN_PROCESS_V1 - 1),
                                                                     OCUDU_DAPP_THREADING_REENTRANT_SHARED_V1,
                                                                     0,
                                                                     &interface_table,
                                                                     0,
                                                                     {}};
  static const ocudu_dapp_management_v1           management      = {
      sizeof(ocudu_dapp_management_v1),
      OCUDU_DAPP_MANAGEMENT_MAJOR_V1,
      OCUDU_DAPP_MANAGEMENT_MINOR_V1,
      OCUDU_DAPP_MANAGEMENT_CONFIG_V1 | OCUDU_DAPP_MANAGEMENT_METRICS_V1 | OCUDU_DAPP_MANAGEMENT_MODELS_V1,
      fixture_describe_config,
      fixture_get_config,
      fixture_set_config,
      fixture_describe_metrics,
      fixture_read_metrics,
      fixture_manage_model,
      {}};

  module->struct_size    = sizeof(*module);
  module->abi_major      = OCUDU_DAPP_ABI_MAJOR_V1;
  module->abi_minor      = OCUDU_DAPP_ABI_MINOR_V1;
  module->global_name    = "org.deepsig.ocudu.test.valid";
  module->vendor         = "DeepSig";
  module->version        = "1.0.0";
  module->nof_interfaces = 1;
  module->interfaces     = &interface;
  module->create         = fixture_create;
  module->configure      = fixture_configure;
  module->prepare        = fixture_prepare;
  module->warmup         = fixture_warmup;
  module->start          = fixture_start;
  module->stop           = fixture_stop;
  module->quiesce        = fixture_quiesce;
  module->destroy        = fixture_destroy;
  module->management     = &management;
}
