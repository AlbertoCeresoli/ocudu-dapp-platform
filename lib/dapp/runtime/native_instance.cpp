// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/runtime/native_instance.h"
#include "native_worker_internal.h"
#include "ocudu/dapp/runtime/incident_store.h"
#include <algorithm>
#include <vector>

using namespace ocudu::dapp;

namespace {

std::mutex quarantine_mutex;
struct quarantine_entry {
  std::shared_ptr<native_module_library> library;
  std::shared_ptr<quiescence_tracker>    activity;
  std::shared_ptr<native_control_context> control_context;
};
std::vector<quarantine_entry> resident_quarantine;

void retain_until_process_exit(std::shared_ptr<native_module_library> library,
                               std::shared_ptr<quiescence_tracker>    activity,
                               std::shared_ptr<native_control_context> control_context)
{
  std::lock_guard<std::mutex> lock(quarantine_mutex);
  resident_quarantine.push_back({std::move(library), std::move(activity), std::move(control_context)});
}

uint64_t deadline_to_ns(std::chrono::steady_clock::time_point deadline)
{
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(deadline.time_since_epoch()).count());
}

} // namespace

native_instance::native_instance(std::shared_ptr<native_module_library> library_,
                                 const ocudu_dapp_host_api_v1&          host_api_,
                                 uint32_t                               selected_backend_) :
  library(std::move(library_)),
  module(library->descriptor()),
  host_api(host_api_),
  activity(std::make_shared<quiescence_tracker>()),
  selected_backend_id(selected_backend_)
{
}

native_instance::~native_instance()
{
  activity->close_admission();
  if (library != nullptr && lifecycle_state.state() != OCUDU_DAPP_STATE_UNLOADED_V1) {
    retain_until_process_exit(std::move(library), std::move(activity), std::move(control_context));
  }
}

instance_create_result native_instance::create(std::shared_ptr<native_module_library> library,
                                               const ocudu_dapp_host_api_v1&          host_api,
                                               const void*                            create_args,
                                               uint32_t                               selected_backend)
{
  if (library == nullptr || host_api.struct_size < sizeof(ocudu_dapp_host_api_v1) ||
      host_api.abi_major != OCUDU_DAPP_ABI_MAJOR_V1 || selected_backend > OCUDU_DAPP_BACKEND_SYCL_V1) {
    return {{OCUDU_DAPP_INVALID_INPUT_V1, OCUDU_DAPP_STATE_ERROR_V1, "invalid library or host API"}, nullptr};
  }

  auto owner = std::unique_ptr<native_instance>(new native_instance(std::move(library), host_api, selected_backend));
  incident_context context{};
  context.phase = incident_phase::create;
  auto guarded =
      guarded_call([&] { return owner->module.create(&owner->host_api, create_args, &owner->module_instance); },
                   owner->lifecycle_breaker,
                   owner->incidents,
                   context);
  if (guarded.status != OCUDU_DAPP_OK_V1 || owner->module_instance == nullptr) {
    return {{guarded.status == OCUDU_DAPP_OK_V1 ? OCUDU_DAPP_INVALID_OUTPUT_V1 : guarded.status,
             OCUDU_DAPP_STATE_ERROR_V1,
             "module create failed or returned a null instance"},
            nullptr};
  }
  owner->lifecycle_state.transition_to(OCUDU_DAPP_STATE_LOADED_V1);
  return {{OCUDU_DAPP_OK_V1, OCUDU_DAPP_STATE_LOADED_V1, {}}, std::move(owner)};
}

instance_operation_result native_instance::prepare(const void* typed_config, const void* prepare_args)
{
  std::lock_guard<std::mutex> lock(management_mutex);
  if (lifecycle_state.state() != OCUDU_DAPP_STATE_LOADED_V1) {
    return result(OCUDU_DAPP_INVALID_INPUT_V1, "prepare requires loaded state");
  }
  auto configured = call_locked(
      incident_phase::configure, "configure", false, [&] { return module.configure(module_instance, typed_config); });
  if (!configured) {
    lifecycle_state.fail();
    return result(configured.status, "module configuration failed");
  }
  auto prepared = call_locked(
      incident_phase::prepare, "prepare", false, [&] { return module.prepare(module_instance, prepare_args); });
  if (!prepared) {
    lifecycle_state.fail();
    return result(prepared.status, "module preparation failed");
  }
  lifecycle_state.transition_to(OCUDU_DAPP_STATE_PREPARED_V1);
  return result(OCUDU_DAPP_OK_V1);
}

instance_operation_result native_instance::arm(const void* admitted_shapes)
{
  std::lock_guard<std::mutex> lock(management_mutex);
  const auto current = lifecycle_state.state();
  // Allow re-arm from inactive without unload/destroy. Hot unload of CUDA Class-A
  // packages has SIGSEGV'd the gNB (destroy path / CUDA static teardown). Lifecycle
  // matrix already permits inactive→armed; keep prepare-only path for first arm.
  if (current != OCUDU_DAPP_STATE_PREPARED_V1 && current != OCUDU_DAPP_STATE_INACTIVE_V1) {
    return result(OCUDU_DAPP_INVALID_INPUT_V1, "arm requires prepared or inactive state");
  }
  if (current == OCUDU_DAPP_STATE_PREPARED_V1 && module.warmup != nullptr) {
    auto warmed = call_locked(
        incident_phase::warmup, "warmup", false, [&] { return module.warmup(module_instance, admitted_shapes); });
    if (!warmed) {
      lifecycle_state.fail();
      return result(warmed.status, "module warmup failed");
    }
  }
  if (!lifecycle_state.transition_to(OCUDU_DAPP_STATE_ARMED_V1)) {
    return result(OCUDU_DAPP_INTERNAL_ERROR_V1, "failed to enter armed state");
  }
  return result(OCUDU_DAPP_OK_V1);
}

instance_operation_result native_instance::enter_shadow()
{
  std::lock_guard<std::mutex> lock(management_mutex);
  if (lifecycle_state.state() != OCUDU_DAPP_STATE_ARMED_V1) {
    return result(OCUDU_DAPP_INVALID_INPUT_V1, "shadow requires armed state");
  }
  auto start_result =
      call_locked(incident_phase::warmup, "start", false, [&] { return module.start(module_instance); });
  if (!start_result) {
    lifecycle_state.fail();
    return result(start_result.status, "module start failed");
  }
  lifecycle_state.transition_to(OCUDU_DAPP_STATE_SHADOW_V1);
  started = true;
  activity->open_admission();
  return result(OCUDU_DAPP_OK_V1);
}

instance_operation_result native_instance::activate()
{
  std::lock_guard<std::mutex> lock(management_mutex);
  if (!lifecycle_state.transition_to(OCUDU_DAPP_STATE_ACTIVE_V1)) {
    return result(OCUDU_DAPP_INVALID_INPUT_V1, "activate requires shadow state");
  }
  return result(OCUDU_DAPP_OK_V1);
}

instance_operation_result native_instance::drain(std::chrono::steady_clock::time_point deadline)
{
  std::lock_guard<std::mutex> lock(management_mutex);
  const auto                  current = lifecycle_state.state();
  if (current != OCUDU_DAPP_STATE_ACTIVE_V1 && current != OCUDU_DAPP_STATE_SHADOW_V1 &&
      current != OCUDU_DAPP_STATE_DEGRADED_V1 && current != OCUDU_DAPP_STATE_ERROR_V1) {
    return result(OCUDU_DAPP_INVALID_INPUT_V1, "drain requires a callable or failed state");
  }
  if (!lifecycle_state.transition_to(OCUDU_DAPP_STATE_DRAINING_V1)) {
    return result(OCUDU_DAPP_INTERNAL_ERROR_V1, "failed to enter draining state");
  }
  activity->close_admission();

  instance_operation_result stopped = result(OCUDU_DAPP_OK_V1);
  if (started) {
    stopped = call_locked(incident_phase::stop, "stop", true, [&] { return module.stop(module_instance); });
  }
  if (!activity->wait_until_empty(deadline)) {
    return quarantine(incident_cause::quiescence_timeout,
                      OCUDU_DAPP_DEADLINE_V1,
                      "host-owned activity did not quiesce before deadline");
  }
  const auto quiesced = call_locked(incident_phase::quiesce, "quiesce", true, [&] {
    return module.quiesce(module_instance, deadline_to_ns(deadline));
  });
  if (!stopped) {
    return quarantine(incident_cause::returned_error, stopped.status, "module stop did not succeed");
  }
  if (!quiesced) {
    return quarantine(incident_cause::returned_error, quiesced.status, "module quiesce did not succeed");
  }
  started = false;
  lifecycle_state.transition_to(OCUDU_DAPP_STATE_INACTIVE_V1);
  return result(OCUDU_DAPP_OK_V1);
}

instance_operation_result native_instance::retry_quiescence(std::chrono::steady_clock::time_point deadline)
{
  std::lock_guard<std::mutex> lock(management_mutex);
  if (lifecycle_state.state() != OCUDU_DAPP_STATE_QUARANTINED_RESIDENT_V1 || destroy_attempted) {
    return result(OCUDU_DAPP_INVALID_INPUT_V1, "retry requires quiescence quarantine before destroy");
  }
  activity->close_admission();
  if (!activity->wait_until_empty(deadline)) {
    return result(OCUDU_DAPP_DEADLINE_V1, "activity remains outstanding");
  }
  if (started) {
    auto stopped = call_locked(incident_phase::stop, "stop", true, [&] { return module.stop(module_instance); });
    if (!stopped) {
      return result(stopped.status, "module still not stopped");
    }
  }
  auto quiesced = call_locked(incident_phase::quiesce, "quiesce", true, [&] {
    return module.quiesce(module_instance, deadline_to_ns(deadline));
  });
  if (!quiesced) {
    return result(quiesced.status, "module still not quiescent");
  }
  started = false;
  lifecycle_state.transition_to(OCUDU_DAPP_STATE_INACTIVE_V1);
  return result(OCUDU_DAPP_OK_V1);
}

instance_operation_result native_instance::unload()
{
  std::lock_guard<std::mutex> lock(management_mutex);
  const auto                  current = lifecycle_state.state();
  if ((current != OCUDU_DAPP_STATE_INACTIVE_V1 && current != OCUDU_DAPP_STATE_LOADED_V1) ||
      !activity->snapshot().empty() || module_instance == nullptr || started) {
    return result(OCUDU_DAPP_INVALID_INPUT_V1, "unload requires loaded-unused or proven inactive instance");
  }
  destroy_attempted = true;
  auto destroyed    = call_locked(incident_phase::unload, "destroy", true, [&] {
    module.destroy(module_instance);
    return OCUDU_DAPP_OK_V1;
  });
  if (!destroyed) {
    lifecycle_state.transition_to(OCUDU_DAPP_STATE_QUARANTINED_RESIDENT_V1);
    return result(destroyed.status, "destroy threw; module remains resident");
  }
  module_instance = nullptr;
  lifecycle_state.transition_to(OCUDU_DAPP_STATE_UNLOADED_V1);
  module.management = nullptr;
  library.reset();
  control_context.reset();
  return result(OCUDU_DAPP_OK_V1);
}

uint64_t native_instance::management_capabilities() const
{
  std::lock_guard<std::mutex> lock(management_mutex);
  return module.management != nullptr ? module.management->capability_bits : 0;
}

instance_health_snapshot native_instance::health() const
{
  std::lock_guard<std::mutex> lock(management_mutex);
  breaker_state               breaker           = lifecycle_breaker.state();
  uint32_t                    failures          = lifecycle_breaker.consecutive_failures();
  uint64_t                    dropped           = incidents.dropped();
  uint32_t                    input_cell_bitmap = 0;
  uint32_t                    native_workers    = 0;
  uint32_t                    active_workers    = 0;
  class_a_fallback_snapshot   class_a_fallbacks;
  class_a_completion_snapshot class_a_completion;
  class_b_policy_snapshot     class_b_policy;
  {
    std::lock_guard<std::mutex> worker_lock(worker_sources_mutex);
    native_workers = static_cast<uint32_t>(worker_sources.size());
    for (const auto& source : worker_sources) {
      if (source->breaker.state() == breaker_state::open) {
        breaker = breaker_state::open;
      }
      failures = std::max(failures, source->breaker.consecutive_failures());
      dropped += source->incidents.dropped();
      const uint32_t observed_cells = source->observed_cells();
      input_cell_bitmap |= observed_cells;
      active_workers += observed_cells != 0;
      class_a_fallbacks += source->class_a_snapshot();
      class_a_completion += source->class_a_completion();
      class_b_policy += source->class_b_snapshot();
    }
  }
  return {lifecycle_state.state(),
          breaker,
          failures,
          activity->snapshot(),
          dropped,
          input_cell_bitmap,
          native_workers,
          active_workers,
          class_a_fallbacks,
          class_a_completion,
          class_b_policy};
}

management_call_result native_instance::describe_config(ocudu_dapp_config_descriptor_v1* descriptors, uint32_t capacity)
{
  std::lock_guard<std::mutex> lock(management_mutex);
  uint32_t                    count = 0;
  if (module.management == nullptr || (module.management->capability_bits & OCUDU_DAPP_MANAGEMENT_CONFIG_V1) == 0) {
    return {result(OCUDU_DAPP_UNSUPPORTED_V1, "configuration management is not supported"), 0, 0};
  }
  auto called = call_locked(incident_phase::report, "describe config", false, [&] {
    return module.management->describe_config(module_instance, descriptors, capacity, &count);
  });
  if (called && count > capacity && descriptors != nullptr) {
    return {result(OCUDU_DAPP_INVALID_OUTPUT_V1, "configuration descriptor count exceeds capacity"), count, 0};
  }
  return {called, count, 0};
}

management_call_result native_instance::get_config(ocudu_dapp_config_entry_v1* entries, uint32_t capacity)
{
  std::lock_guard<std::mutex> lock(management_mutex);
  uint32_t                    count      = 0;
  uint64_t                    generation = 0;
  if (module.management == nullptr || (module.management->capability_bits & OCUDU_DAPP_MANAGEMENT_CONFIG_V1) == 0) {
    return {result(OCUDU_DAPP_UNSUPPORTED_V1, "configuration management is not supported"), 0, 0};
  }
  auto called = call_locked(incident_phase::report, "get config", false, [&] {
    return module.management->get_config(module_instance, entries, capacity, &count, &generation);
  });
  if (called && ((count > capacity && entries != nullptr) || generation == 0)) {
    return {result(OCUDU_DAPP_INVALID_OUTPUT_V1, "invalid configuration snapshot"), count, generation};
  }
  return {called, count, generation};
}

management_call_result native_instance::set_config(const ocudu_dapp_config_update_v1& update)
{
  std::lock_guard<std::mutex> lock(management_mutex);
  uint64_t                    generation = 0;
  if (module.management == nullptr || (module.management->capability_bits & OCUDU_DAPP_MANAGEMENT_CONFIG_V1) == 0) {
    return {result(OCUDU_DAPP_UNSUPPORTED_V1, "configuration management is not supported"), 0, 0};
  }
  if (update.struct_size < sizeof(ocudu_dapp_config_update_v1) || update.expected_generation == 0 ||
      (update.nof_entries != 0 && update.entries == nullptr) ||
      update.boundary < OCUDU_DAPP_CONFIG_BOUNDARY_IMMEDIATE_V1 ||
      update.boundary > OCUDU_DAPP_CONFIG_BOUNDARY_RESTART_V1) {
    return {result(OCUDU_DAPP_INVALID_INPUT_V1, "invalid configuration update"), 0, 0};
  }
  auto called = call_locked(incident_phase::configure, "set config", false, [&] {
    return module.management->set_config(module_instance, &update, &generation);
  });
  if (called && generation == 0) {
    return {result(OCUDU_DAPP_INVALID_OUTPUT_V1, "configuration update returned no generation"), 0, 0};
  }
  return {called, 0, generation};
}

management_call_result native_instance::describe_metrics(ocudu_dapp_metric_descriptor_v1* descriptors,
                                                         uint32_t                         capacity)
{
  std::lock_guard<std::mutex> lock(management_mutex);
  uint32_t                    count = 0;
  if (module.management == nullptr || (module.management->capability_bits & OCUDU_DAPP_MANAGEMENT_METRICS_V1) == 0) {
    return {result(OCUDU_DAPP_UNSUPPORTED_V1, "custom metrics are not supported"), 0, 0};
  }
  auto called = call_locked(incident_phase::report, "describe metrics", false, [&] {
    return module.management->describe_metrics(module_instance, descriptors, capacity, &count);
  });
  if (called && count > capacity && descriptors != nullptr) {
    return {result(OCUDU_DAPP_INVALID_OUTPUT_V1, "metric descriptor count exceeds capacity"), count, 0};
  }
  return {called, count, 0};
}

management_call_result native_instance::read_metrics(ocudu_dapp_metric_sample_v1* samples, uint32_t capacity)
{
  std::lock_guard<std::mutex> lock(management_mutex);
  uint32_t                    count = 0;
  if (module.management == nullptr || (module.management->capability_bits & OCUDU_DAPP_MANAGEMENT_METRICS_V1) == 0) {
    return {result(OCUDU_DAPP_UNSUPPORTED_V1, "custom metrics are not supported"), 0, 0};
  }
  auto called = call_locked(incident_phase::report, "read metrics", false, [&] {
    return module.management->read_metrics(module_instance, samples, capacity, &count);
  });
  if (called && count > capacity && samples != nullptr) {
    return {result(OCUDU_DAPP_INVALID_OUTPUT_V1, "metric sample count exceeds capacity"), count, 0};
  }
  return {called, count, 0};
}

management_call_result native_instance::manage_model(const ocudu_dapp_model_request_v1& request)
{
  std::lock_guard<std::mutex> lock(management_mutex);
  uint64_t                    generation = 0;
  if (module.management == nullptr || (module.management->capability_bits & OCUDU_DAPP_MANAGEMENT_MODELS_V1) == 0) {
    return {result(OCUDU_DAPP_UNSUPPORTED_V1, "model management is not supported"), 0, 0};
  }
  if (request.struct_size < sizeof(ocudu_dapp_model_request_v1) || request.expected_generation == 0 ||
      request.action < OCUDU_DAPP_MODEL_STAGE_V1 || request.action > OCUDU_DAPP_MODEL_RETIRE_V1 ||
      request.model_id == nullptr || request.model_id[0] == '\0') {
    return {result(OCUDU_DAPP_INVALID_INPUT_V1, "invalid model request"), 0, 0};
  }
  auto called = call_locked(incident_phase::configure, "manage model", false, [&] {
    return module.management->manage_model(module_instance, &request, &generation);
  });
  if (called && generation == 0) {
    return {result(OCUDU_DAPP_INVALID_OUTPUT_V1, "model operation returned no generation"), 0, 0};
  }
  return {called, 0, generation};
}

native_worker_create_result native_instance::create_worker(uint32_t interface_id, uint32_t worker_id,
                                                          std::shared_ptr<native_control_context> context)
{
  std::lock_guard<std::mutex> lock(management_mutex);
  if (lifecycle_state.state() != OCUDU_DAPP_STATE_SHADOW_V1 && lifecycle_state.state() != OCUDU_DAPP_STATE_ACTIVE_V1) {
    return {OCUDU_DAPP_INVALID_INPUT_V1, "worker creation requires shadow or active state", {}};
  }
  const auto* descriptor = std::find_if(module.interfaces, module.interfaces + module.nof_interfaces,
      [interface_id](const auto& item) { return item.interface_id == interface_id; });
  const bool needs_context = descriptor != module.interfaces + module.nof_interfaces &&
      interface_id == OCUDU_DAPP_INTERFACE_RECEIVER_V1 && selected_backend_id == OCUDU_DAPP_BACKEND_CUDA_V1 &&
      (descriptor->capability_bits & OCUDU_DAPP_RECEIVER_CAP_COMPLETION_V1);
  if (needs_context) {
    if (!context || !context->identity() || context->backend_id() != selected_backend_id)
      return {OCUDU_DAPP_UNSUPPORTED_V1, "completion receiver needs a retained host control context", {}};
    if (control_context && control_context->identity() != context->identity())
      return {OCUDU_DAPP_CONFLICT_V1, "one instance cannot mix CUDA contexts", {}};
    // Retain BEFORE creation: a failed module callback can leave private GPU allocations for quiescence.
    if (!control_context) control_context = std::move(context);
  }
  return native_worker::create(*this, interface_id, worker_id);
}

ocudu_dapp_lifecycle_state_v1 native_instance::state() const
{
  std::lock_guard<std::mutex> lock(management_mutex);
  return lifecycle_state.state();
}

instance_operation_result native_instance::call_locked(incident_phase phase,
                                                       const char*,
                                                       bool                                         cleanup,
                                                       const std::function<ocudu_dapp_status_v1()>& callback)
{
  incident_context context{};
  context            = incident_identity;
  context.phase      = phase;
  const auto guarded = guarded_call([&] {
    return control_context && phase != incident_phase::report ? control_context->call(callback) : callback();
  }, lifecycle_breaker, incidents, context, cleanup);
  return result(guarded.status);
}

instance_operation_result native_instance::reset_breakers()
{
  std::lock_guard<std::mutex> lock(management_mutex);
  lifecycle_breaker.force_close();
  std::lock_guard<std::mutex> worker_lock(worker_sources_mutex);
  for (const auto& source : worker_sources) {
    source->breaker.force_close();
  }
  return result(OCUDU_DAPP_OK_V1);
}

instance_operation_result native_instance::result(ocudu_dapp_status_v1 status, const char* detail) const
{
  return {status, lifecycle_state.state(), detail != nullptr ? detail : ""};
}

instance_operation_result
native_instance::quarantine(incident_cause cause, ocudu_dapp_status_v1 status, const char* detail)
{
  lifecycle_state.transition_to(OCUDU_DAPP_STATE_QUARANTINED_RESIDENT_V1);
  incident_context context{};
  context       = incident_identity;
  context.phase = incident_phase::quiesce;
  incidents.try_push(make_incident(context, cause, incident_action::quarantined, status, detail, true));
  lifecycle_breaker.force_open();
  return result(status, detail);
}

std::size_t ocudu::dapp::quarantined_native_library_count()
{
  std::lock_guard<std::mutex> lock(quarantine_mutex);
  return resident_quarantine.size();
}

std::size_t native_instance::drain_incidents(incident_store& destination)
{
  std::size_t                 count = destination.drain(incidents);
  std::lock_guard<std::mutex> lock(worker_sources_mutex);
  for (const auto& source : worker_sources) {
    count += destination.drain(source->incidents);
  }
  return count;
}

bool native_instance::try_pop_incident(incident_record& record) noexcept
{
  if (incidents.try_pop(record)) {
    return true;
  }
  std::lock_guard<std::mutex> lock(worker_sources_mutex);
  return std::any_of(worker_sources.begin(), worker_sources.end(), [&](const auto& source) {
    return source->incidents.try_pop(record);
  });
}

uint64_t native_instance::dropped_incidents() const noexcept
{
  uint64_t                    dropped = incidents.dropped();
  std::lock_guard<std::mutex> lock(worker_sources_mutex);
  for (const auto& source : worker_sources) {
    dropped += source->incidents.dropped();
  }
  return dropped;
}

void native_instance::set_incident_identity(const uint64_t instance_id[2],
                                            const uint64_t package_id[2],
                                            uint64_t       generation)
{
  std::lock_guard<std::mutex> lock(management_mutex);
  incident_identity.instance_id[0] = instance_id[0];
  incident_identity.instance_id[1] = instance_id[1];
  incident_identity.package_id[0]  = package_id[0];
  incident_identity.package_id[1]  = package_id[1];
  incident_identity.app_generation = generation;
  std::lock_guard<std::mutex> worker_lock(worker_sources_mutex);
  for (const auto& source : worker_sources) {
    source->app_generation.store(generation, std::memory_order_release);
  }
}
