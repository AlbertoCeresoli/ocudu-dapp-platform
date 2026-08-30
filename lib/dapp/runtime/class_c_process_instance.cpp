// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/runtime/class_c_process_instance.h"
#include "ocudu/dapp/abi/v1/metric_catalog.h"
#include "ocudu/dapp/runtime/incident_store.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <new>

using namespace ocudu::dapp;

namespace {

struct metric_definition {
  const char* name;
  const char* description;
  const char* unit;
  uint32_t    flags{OCUDU_DAPP_METRIC_COUNTER_V1};
};

constexpr std::array<metric_definition, 41> process_metrics{{
    {OCUDU_DAPP_METRIC_PROCESS_SUPERVISOR_STATE_V1,
     "Class-C process supervisor state",
     "state",
     OCUDU_DAPP_METRIC_GAUGE_V1},
    {OCUDU_DAPP_METRIC_PROCESS_PID_V1, "Current Class-C worker process identifier", "pid", OCUDU_DAPP_METRIC_GAUGE_V1},
    {OCUDU_DAPP_METRIC_PROCESS_PUBLICATION_ENABLED_V1,
     "Whether admitted producer endpoints are active",
     "boolean",
     OCUDU_DAPP_METRIC_GAUGE_V1},
    {OCUDU_DAPP_METRIC_PROCESS_ROUTE_GENERATION_V1,
     "Current process route generation",
     "generation",
     OCUDU_DAPP_METRIC_GAUGE_V1},
    {OCUDU_DAPP_METRIC_PROCESS_SUCCESSFUL_STARTS_V1, "Successful worker readiness handshakes", "starts"},
    {OCUDU_DAPP_METRIC_PROCESS_SUCCESSFUL_RESTARTS_V1, "Successful supervised worker restarts", "restarts"},
    {OCUDU_DAPP_METRIC_PROCESS_EXITS_V1, "Observed worker process exits", "exits"},
    {OCUDU_DAPP_METRIC_PROCESS_START_FAILURES_V1, "Failed worker start or readiness attempts", "failures"},
    {OCUDU_DAPP_METRIC_PROCESS_FORCED_KILLS_V1, "Worker processes sent SIGKILL", "kills"},
    {OCUDU_DAPP_METRIC_PROCESS_LAST_WAIT_STATUS_V1, "Last waitpid status value", "status", OCUDU_DAPP_METRIC_GAUGE_V1},
    {OCUDU_DAPP_METRIC_PROCESS_PUBLICATION_REQUESTED_V1,
     "Host lifecycle publication intent",
     "boolean",
     OCUDU_DAPP_METRIC_GAUGE_V1},
    {OCUDU_DAPP_METRIC_PROCESS_RESULTS_ACCEPTED_V1, "Validated Class-C results accepted by the host", "results"},
    {OCUDU_DAPP_METRIC_PROCESS_RESULTS_BUSY_V1,
     "Validated Class-C results dropped by bounded host backpressure",
     "results"},
    {OCUDU_DAPP_METRIC_PROCESS_RESULTS_REJECTED_V1,
     "Class-C results rejected by admission or schema validation",
     "results"},
    {OCUDU_DAPP_METRIC_PROCESS_RESULT_PROTOCOL_FAILURES_V1,
     "Malformed or unauthorized worker control frames",
     "failures"},
    {OCUDU_DAPP_METRIC_PROCESS_HEARTBEAT_REQUESTS_V1,
     "Authenticated heartbeat challenges sent by the host",
     "heartbeats"},
    {OCUDU_DAPP_METRIC_PROCESS_HEARTBEAT_RESPONSES_V1,
     "Valid heartbeat responses received from the worker",
     "heartbeats"},
    {OCUDU_DAPP_METRIC_PROCESS_HEARTBEAT_LOSSES_V1, "Heartbeat response deadlines missed by the worker", "failures"},
    {OCUDU_DAPP_METRIC_PROCESS_LAST_HEARTBEAT_LATENCY_NS_V1,
     "Most recent valid heartbeat round-trip latency",
     "nanoseconds",
     OCUDU_DAPP_METRIC_GAUGE_V1},
    {OCUDU_DAPP_METRIC_PROCESS_MANAGEMENT_RESPONSES_V1, "Validated configuration or model responses", "responses"},
    {OCUDU_DAPP_METRIC_PROCESS_MANAGEMENT_REPLAYS_V1,
     "Retained management mutations replayed after restart",
     "operations"},
    {OCUDU_DAPP_METRIC_PROCESS_MANAGEMENT_TIMEOUTS_V1, "Management requests that forced worker recovery", "failures"},
    {OCUDU_DAPP_METRIC_CLASS_C_ROUTE_PUBLICATIONS_V1, "Committed Class-C route publications", "publications"},
    {OCUDU_DAPP_METRIC_CLASS_C_ROUTE_PAYLOAD_BYTES_V1, "Complete record bytes committed to Class-C routes", "bytes"},
    {OCUDU_DAPP_METRIC_CLASS_C_ROUTE_BACKPRESSURE_DROPS_V1,
     "Class-C publications rejected because a bounded route was full",
     "publications"},
    {OCUDU_DAPP_METRIC_CLASS_C_ROUTE_INVALID_DROPS_V1,
     "Class-C publications rejected before commit as invalid",
     "publications"},
    {OCUDU_DAPP_METRIC_CLASS_C_ROUTE_ENCODE_FAILURES_V1, "Class-C metadata encoding failures", "failures"},
    {OCUDU_DAPP_METRIC_CLASS_C_ROUTE_NOTIFICATION_FAILURES_V1,
     "Committed publications whose consumer notification failed",
     "failures"},
    {OCUDU_DAPP_METRIC_CLASS_C_ROUTE_INACTIVE_BYPASSES_V1,
     "Class-C publications bypassed while the route was inactive",
     "publications"},
    {OCUDU_DAPP_METRIC_CLASS_C_ROUTE_CONCURRENT_DROPS_V1,
     "Class-C publications rejected by the SPSC single-producer guard",
     "publications"},
    {OCUDU_DAPP_METRIC_CLASS_C_COPY_SNAPSHOT_COUNT_V1,
     "Tensor snapshots copied into CPU process routes after producer readiness",
     "copies"},
    {OCUDU_DAPP_METRIC_CLASS_C_COPY_SNAPSHOT_BYTES_V1,
     "Tensor bytes copied into CPU process routes after producer readiness",
     "bytes"},
    {OCUDU_DAPP_METRIC_CLASS_C_COPY_LEASE_COUNT_V1,
     "Publications borrowing producer tensor storage without moving tensor bytes",
     "leases"},
    {OCUDU_DAPP_METRIC_CLASS_C_COPY_EXPORT_COUNT_V1,
     "Tensor snapshots copied into accelerator export allocations",
     "copies"},
    {OCUDU_DAPP_METRIC_CLASS_C_COPY_EXPORT_BYTES_V1,
     "Tensor bytes copied into accelerator export allocations",
     "bytes"},
    {OCUDU_DAPP_METRIC_CLASS_C_EXPORT_SLOTS_CAPACITY_V1,
     "Total fixed accelerator export slots across process routes",
     "slots",
     OCUDU_DAPP_METRIC_GAUGE_V1},
    {OCUDU_DAPP_METRIC_CLASS_C_EXPORT_SLOTS_IN_USE_V1,
     "Accelerator export slots currently retained by process routes",
     "slots",
     OCUDU_DAPP_METRIC_GAUGE_V1},
    {OCUDU_DAPP_METRIC_CLASS_C_EXPORT_SLOTS_HIGH_WATERMARK_V1,
     "Sum of per-route accelerator export-slot high-watermarks",
     "slots",
     OCUDU_DAPP_METRIC_GAUGE_V1},
    {OCUDU_DAPP_METRIC_CLASS_C_EXPORT_SLOT_RESERVATION_FAILURES_V1,
     "Accelerator export attempts rejected because every route slot was retained",
     "failures"},
    {OCUDU_DAPP_METRIC_CLASS_C_COPY_E3_PAYLOAD_COUNT_V1,
     "Tensor snapshots materialized into portable E3 payloads",
     "copies"},
    {OCUDU_DAPP_METRIC_CLASS_C_COPY_E3_PAYLOAD_BYTES_V1,
     "Tensor bytes materialized into portable E3 payloads",
     "bytes"},
}};

ocudu_dapp_value_v1 unsigned_value(uint64_t value) noexcept
{
  ocudu_dapp_value_v1 result{};
  result.kind               = OCUDU_DAPP_VALUE_UINT64_V1;
  result.value.uint64_value = value;
  return result;
}

uint64_t monotonic_time_ns() noexcept
{
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

template <std::size_t N>
bool copy_fixed(char (&destination)[N], const char* source) noexcept
{
  if (source == nullptr) {
    return false;
  }
  const std::size_t size = ::strnlen(source, N);
  if (size == 0 || size == N) {
    return false;
  }
  std::memcpy(destination, source, size);
  destination[size] = '\0';
  return true;
}

bool encode_value(ocudu_dapp_process_value_v1& destination, const ocudu_dapp_value_v1& source) noexcept
{
  destination      = {};
  destination.kind = source.kind;
  switch (source.kind) {
    case 0:
      return true;
    case OCUDU_DAPP_VALUE_BOOL_V1:
      destination.value.boolean = source.value.boolean;
      return source.value.boolean <= 1;
    case OCUDU_DAPP_VALUE_INT64_V1:
      destination.value.int64_value = source.value.int64_value;
      return true;
    case OCUDU_DAPP_VALUE_UINT64_V1:
      destination.value.uint64_value = source.value.uint64_value;
      return true;
    case OCUDU_DAPP_VALUE_DOUBLE_V1:
      destination.value.double_value = source.value.double_value;
      return true;
    case OCUDU_DAPP_VALUE_STRING_V1:
    case OCUDU_DAPP_VALUE_BYTES_V1:
      if (source.value.blob.size > OCUDU_DAPP_PROCESS_MANAGEMENT_VALUE_BYTES_V1 ||
          (source.value.blob.size != 0 && source.value.blob.data == nullptr)) {
        return false;
      }
      destination.size = source.value.blob.size;
      if (destination.size != 0) {
        std::memcpy(destination.value.bytes, source.value.blob.data, destination.size);
      }
      return true;
    default:
      return false;
  }
}

ocudu_dapp_value_v1 decode_value(const ocudu_dapp_process_value_v1& source) noexcept
{
  ocudu_dapp_value_v1 result{};
  result.kind = source.kind;
  switch (source.kind) {
    case OCUDU_DAPP_VALUE_BOOL_V1:
      result.value.boolean = source.value.boolean;
      break;
    case OCUDU_DAPP_VALUE_INT64_V1:
      result.value.int64_value = source.value.int64_value;
      break;
    case OCUDU_DAPP_VALUE_UINT64_V1:
      result.value.uint64_value = source.value.uint64_value;
      break;
    case OCUDU_DAPP_VALUE_DOUBLE_V1:
      result.value.double_value = source.value.double_value;
      break;
    case OCUDU_DAPP_VALUE_STRING_V1:
    case OCUDU_DAPP_VALUE_BYTES_V1:
      result.value.blob = {source.value.bytes, source.size, 0};
      break;
    default:
      break;
  }
  return result;
}

} // namespace

class_c_process_instance::class_c_process_instance(std::unique_ptr<class_c_process_supervisor> supervisor_) noexcept :
  supervisor(std::move(supervisor_))
{
  lifecycle_state.transition_to(OCUDU_DAPP_STATE_LOADED_V1);
}

class_c_process_instance::~class_c_process_instance() = default;

class_c_process_instance_create_result
class_c_process_instance::create(std::unique_ptr<class_c_process_supervisor> supervisor) noexcept
{
  if (supervisor == nullptr) {
    return {{OCUDU_DAPP_INVALID_INPUT_V1, OCUDU_DAPP_STATE_ERROR_V1, "missing Class-C process supervisor"}, {}};
  }
  try {
    auto instance = std::unique_ptr<class_c_process_instance>(new class_c_process_instance(std::move(supervisor)));
    return {{OCUDU_DAPP_OK_V1, OCUDU_DAPP_STATE_LOADED_V1, {}}, std::move(instance)};
  } catch (const std::bad_alloc&) {
    return {{OCUDU_DAPP_RESOURCE_V1, OCUDU_DAPP_STATE_ERROR_V1, "unable to allocate Class-C process instance"}, {}};
  } catch (...) {
    return {{OCUDU_DAPP_INTERNAL_ERROR_V1, OCUDU_DAPP_STATE_ERROR_V1, "unknown process instance creation failure"}, {}};
  }
}

instance_operation_result class_c_process_instance::prepare(const void* typed_config, const void* prepare_args)
{
  std::lock_guard<std::mutex> lock(management_mutex);
  refresh_locked();
  if (lifecycle_state.state() != OCUDU_DAPP_STATE_LOADED_V1 || typed_config != nullptr || prepare_args != nullptr) {
    return result(OCUDU_DAPP_INVALID_INPUT_V1, "process prepare requires loaded state and host-owned route config");
  }
  lifecycle_state.transition_to(OCUDU_DAPP_STATE_PREPARED_V1);
  return result(OCUDU_DAPP_OK_V1);
}

instance_operation_result class_c_process_instance::arm(const void* admitted_shapes)
{
  std::lock_guard<std::mutex> lock(management_mutex);
  refresh_locked();
  if (lifecycle_state.state() != OCUDU_DAPP_STATE_PREPARED_V1 || admitted_shapes != nullptr) {
    return result(OCUDU_DAPP_INVALID_INPUT_V1, "process arm requires prepared state and pre-admitted routes");
  }
  lifecycle_state.transition_to(OCUDU_DAPP_STATE_ARMED_V1);
  return result(OCUDU_DAPP_OK_V1);
}

instance_operation_result class_c_process_instance::enter_shadow()
{
  std::lock_guard<std::mutex> lock(management_mutex);
  refresh_locked();
  if (lifecycle_state.state() != OCUDU_DAPP_STATE_ARMED_V1) {
    return result(OCUDU_DAPP_INVALID_INPUT_V1, "process shadow requires armed state");
  }
  auto started = supervisor->start();
  if (!started) {
    lifecycle_state.fail();
    record_incident(incident_phase::warmup,
                    incident_cause::returned_error,
                    incident_action::disabled,
                    started.status,
                    started.detail.c_str(),
                    true);
    return result(started.status,
                  started.detail.empty() ? "Class-C worker start or readiness failed" : started.detail.c_str());
  }
  lifecycle_state.transition_to(OCUDU_DAPP_STATE_SHADOW_V1);
  return result(OCUDU_DAPP_OK_V1);
}

instance_operation_result class_c_process_instance::activate()
{
  std::lock_guard<std::mutex> lock(management_mutex);
  refresh_locked();
  if (lifecycle_state.state() != OCUDU_DAPP_STATE_SHADOW_V1) {
    return result(OCUDU_DAPP_INVALID_INPUT_V1, "process activate requires shadow state");
  }
  auto enabled = supervisor->set_publication_enabled(true);
  if (!enabled) {
    lifecycle_state.fail();
    record_incident(incident_phase::configure,
                    incident_cause::returned_error,
                    incident_action::disabled,
                    enabled.status,
                    enabled.detail.c_str(),
                    true);
    return result(enabled.status,
                  enabled.detail.empty() ? "Class-C publication activation failed" : enabled.detail.c_str());
  }
  lifecycle_state.transition_to(OCUDU_DAPP_STATE_ACTIVE_V1);
  return result(OCUDU_DAPP_OK_V1);
}

instance_operation_result class_c_process_instance::drain(std::chrono::steady_clock::time_point deadline)
{
  std::lock_guard<std::mutex> lock(management_mutex);
  refresh_locked();
  const auto current = lifecycle_state.state();
  if (current != OCUDU_DAPP_STATE_ACTIVE_V1 && current != OCUDU_DAPP_STATE_SHADOW_V1 &&
      current != OCUDU_DAPP_STATE_DEGRADED_V1 && current != OCUDU_DAPP_STATE_ERROR_V1) {
    return result(OCUDU_DAPP_INVALID_INPUT_V1, "process drain requires a running or failed state");
  }
  lifecycle_state.transition_to(OCUDU_DAPP_STATE_DRAINING_V1);
  supervisor->set_publication_enabled(false);
  auto stopped = supervisor->stop();
  synchronize_counters_locked();
  if (!stopped) {
    lifecycle_state.transition_to(OCUDU_DAPP_STATE_QUARANTINED_RESIDENT_V1);
    record_incident(incident_phase::stop,
                    stopped.status == OCUDU_DAPP_DEADLINE_V1 ? incident_cause::quiescence_timeout
                                                             : incident_cause::returned_error,
                    incident_action::quarantined,
                    stopped.status,
                    stopped.detail.c_str(),
                    true);
    return result(stopped.status, "Class-C worker did not stop cleanly");
  }
  lifecycle_state.transition_to(OCUDU_DAPP_STATE_INACTIVE_V1);
  const bool met_deadline = std::chrono::steady_clock::now() <= deadline;
  return result(met_deadline ? OCUDU_DAPP_OK_V1 : OCUDU_DAPP_DEADLINE_V1,
                met_deadline ? nullptr : "Class-C worker stopped after deadline");
}

instance_operation_result class_c_process_instance::retry_quiescence(std::chrono::steady_clock::time_point deadline)
{
  std::lock_guard<std::mutex> lock(management_mutex);
  if (lifecycle_state.state() != OCUDU_DAPP_STATE_QUARANTINED_RESIDENT_V1) {
    return result(OCUDU_DAPP_INVALID_INPUT_V1, "process retry requires quarantine state");
  }
  auto stopped = supervisor->stop();
  synchronize_counters_locked();
  if (!stopped || std::chrono::steady_clock::now() > deadline) {
    return result(stopped ? OCUDU_DAPP_DEADLINE_V1 : stopped.status,
                  stopped ? "Class-C worker stopped after retry deadline" : stopped.detail.c_str());
  }
  lifecycle_state.transition_to(OCUDU_DAPP_STATE_INACTIVE_V1);
  return result(OCUDU_DAPP_OK_V1);
}

instance_operation_result class_c_process_instance::unload()
{
  std::lock_guard<std::mutex> lock(management_mutex);
  refresh_locked();
  const auto current = lifecycle_state.state();
  if (current != OCUDU_DAPP_STATE_LOADED_V1 && current != OCUDU_DAPP_STATE_INACTIVE_V1) {
    return result(OCUDU_DAPP_INVALID_INPUT_V1, "process unload requires loaded-unused or inactive state");
  }
  auto stopped = supervisor->stop();
  synchronize_counters_locked();
  if (!stopped) {
    lifecycle_state.transition_to(OCUDU_DAPP_STATE_QUARANTINED_RESIDENT_V1);
    return result(stopped.status, "Class-C supervisor could not release its worker");
  }
  lifecycle_state.transition_to(OCUDU_DAPP_STATE_UNLOADED_V1);
  supervisor.reset();
  return result(OCUDU_DAPP_OK_V1);
}

uint64_t class_c_process_instance::management_capabilities() const
{
  return OCUDU_DAPP_MANAGEMENT_CONFIG_V1 | OCUDU_DAPP_MANAGEMENT_METRICS_V1 | OCUDU_DAPP_MANAGEMENT_MODELS_V1;
}

instance_health_snapshot class_c_process_instance::health() const
{
  std::lock_guard<std::mutex> lock(management_mutex);
  refresh_locked();
  const auto snapshot = supervisor != nullptr ? supervisor->snapshot() : class_c_process_snapshot{};
  return {lifecycle_state.state(),
          snapshot.state == class_c_process_state::failed ? breaker_state::open : breaker_state::closed,
          static_cast<uint32_t>(std::min<uint64_t>(snapshot.start_failures, UINT32_MAX)),
          {},
          incidents.dropped(),
          snapshot.route_metrics.observed_cell_bitmap};
}

management_call_result class_c_process_instance::describe_config(ocudu_dapp_config_descriptor_v1* descriptors,
                                                                 uint32_t                         capacity)
{
  std::lock_guard<std::mutex> lock(management_mutex);
  refresh_locked();
  if (supervisor == nullptr) {
    return {result(OCUDU_DAPP_CONFLICT_V1, "process instance is unloaded"), 0, 0};
  }
  ocudu_dapp_process_management_request_v1 request{};
  request.struct_size = sizeof(request);
  request.operation   = OCUDU_DAPP_PROCESS_MANAGEMENT_DESCRIBE_CONFIG_V1;
  auto managed        = supervisor->manage(request);
  if (!managed.operation) {
    return {result(managed.operation.status, managed.operation.detail.c_str()), 0, 0};
  }
  const uint32_t count = managed.response.count;
  if (count > OCUDU_DAPP_PROCESS_MANAGEMENT_MAX_ENTRIES_V1) {
    return {result(OCUDU_DAPP_RESOURCE_V1, "process config schema exceeds the v1 item bound"), count, 0};
  }
  if (descriptors == nullptr) {
    return {result(OCUDU_DAPP_OK_V1), count, 0};
  }
  if (capacity < count) {
    return {result(OCUDU_DAPP_RESOURCE_V1, "process config descriptor capacity is too small"), count, 0};
  }
  for (uint32_t index = 0; index != count; ++index) {
    request.item_offset = index;
    managed             = supervisor->manage(request);
    if (!managed.operation || managed.response.count != count) {
      return {
          result(managed.operation ? OCUDU_DAPP_CONFLICT_V1 : managed.operation.status,
                 managed.operation ? "process config schema changed during query" : managed.operation.detail.c_str()),
          count,
          0};
    }
    descriptor_storage[index]        = managed.response.payload.descriptor;
    const auto& wire                 = descriptor_storage[index];
    descriptors[index]               = {};
    descriptors[index].struct_size   = sizeof(descriptors[index]);
    descriptors[index].key           = wire.key;
    descriptors[index].description   = wire.description;
    descriptors[index].value_kind    = wire.value_kind;
    descriptors[index].flags         = wire.flags;
    descriptors[index].default_value = decode_value(wire.default_value);
    descriptors[index].minimum_value = decode_value(wire.minimum_value);
    descriptors[index].maximum_value = decode_value(wire.maximum_value);
  }
  return {result(OCUDU_DAPP_OK_V1), count, 0};
}

management_call_result class_c_process_instance::get_config(ocudu_dapp_config_entry_v1* entries, uint32_t capacity)
{
  std::lock_guard<std::mutex> lock(management_mutex);
  refresh_locked();
  if (supervisor == nullptr) {
    return {result(OCUDU_DAPP_CONFLICT_V1, "process instance is unloaded"), 0, 0};
  }
  ocudu_dapp_process_management_request_v1 request{};
  request.struct_size = sizeof(request);
  request.operation   = OCUDU_DAPP_PROCESS_MANAGEMENT_GET_CONFIG_V1;
  auto managed        = supervisor->manage(request);
  if (!managed.operation) {
    return {result(managed.operation.status, managed.operation.detail.c_str()), 0, 0};
  }
  const uint32_t count = managed.response.count;
  if (count > OCUDU_DAPP_PROCESS_MANAGEMENT_MAX_ENTRIES_V1) {
    return {result(OCUDU_DAPP_RESOURCE_V1, "process config exceeds the v1 item bound"), count, 0};
  }
  if (entries == nullptr) {
    return {result(OCUDU_DAPP_OK_V1), count, managed.response.resulting_generation};
  }
  if (capacity < count) {
    return {result(OCUDU_DAPP_RESOURCE_V1, "process config entry capacity is too small"), count, 0};
  }
  std::copy_n(managed.response.payload.entries, count, config_storage.begin());
  for (uint32_t index = 0; index != count; ++index) {
    entries[index]       = {};
    entries[index].key   = config_storage[index].key;
    entries[index].value = decode_value(config_storage[index].value);
  }
  return {result(OCUDU_DAPP_OK_V1), count, managed.response.resulting_generation};
}

management_call_result class_c_process_instance::set_config(const ocudu_dapp_config_update_v1& update)
{
  std::lock_guard<std::mutex> lock(management_mutex);
  refresh_locked();
  if (supervisor == nullptr || update.struct_size < sizeof(update) || update.nof_entries == 0 ||
      update.nof_entries > OCUDU_DAPP_PROCESS_MANAGEMENT_MAX_ENTRIES_V1 || update.entries == nullptr ||
      update.expected_generation == 0 || update.boundary < OCUDU_DAPP_CONFIG_BOUNDARY_IMMEDIATE_V1 ||
      update.boundary > OCUDU_DAPP_CONFIG_BOUNDARY_RESTART_V1) {
    return {result(OCUDU_DAPP_INVALID_INPUT_V1, "invalid process config update"), 0, 0};
  }
  ocudu_dapp_process_management_request_v1 request{};
  request.struct_size         = sizeof(request);
  request.operation           = OCUDU_DAPP_PROCESS_MANAGEMENT_SET_CONFIG_V1;
  request.expected_generation = update.expected_generation;
  request.boundary_or_action  = update.boundary;
  request.entry_count         = update.nof_entries;
  request.flags               = update.dry_run ? OCUDU_DAPP_PROCESS_MANAGEMENT_DRY_RUN_V1 : 0;
  for (uint32_t index = 0; index != update.nof_entries; ++index) {
    if (!copy_fixed(request.entries[index].key, update.entries[index].key) ||
        !encode_value(request.entries[index].value, update.entries[index].value)) {
      return {result(OCUDU_DAPP_INVALID_INPUT_V1, "invalid process config entry"), 0, 0};
    }
  }
  auto managed = supervisor->manage(request);
  return {result(managed.operation.status, managed.operation.detail.c_str()), 0, managed.response.resulting_generation};
}

management_call_result class_c_process_instance::describe_metrics(ocudu_dapp_metric_descriptor_v1* descriptors,
                                                                  uint32_t                         capacity)
{
  std::lock_guard<std::mutex> lock(management_mutex);
  refresh_locked();
  if (descriptors == nullptr) {
    return {result(OCUDU_DAPP_OK_V1), process_metrics.size(), 0};
  }
  if (capacity < process_metrics.size()) {
    return {
        result(OCUDU_DAPP_RESOURCE_V1, "process metric descriptor capacity is too small"), process_metrics.size(), 0};
  }
  for (std::size_t index = 0; index != process_metrics.size(); ++index) {
    descriptors[index]             = {};
    descriptors[index].struct_size = sizeof(descriptors[index]);
    descriptors[index].name        = process_metrics[index].name;
    descriptors[index].description = process_metrics[index].description;
    descriptors[index].unit        = process_metrics[index].unit;
    descriptors[index].value_kind  = OCUDU_DAPP_VALUE_UINT64_V1;
    descriptors[index].flags       = process_metrics[index].flags;
  }
  return {result(OCUDU_DAPP_OK_V1), process_metrics.size(), 0};
}

management_call_result class_c_process_instance::read_metrics(ocudu_dapp_metric_sample_v1* samples, uint32_t capacity)
{
  std::lock_guard<std::mutex> lock(management_mutex);
  refresh_locked();
  if (samples == nullptr) {
    return {result(OCUDU_DAPP_OK_V1), process_metrics.size(), 0};
  }
  if (capacity < process_metrics.size()) {
    return {result(OCUDU_DAPP_RESOURCE_V1, "process metric sample capacity is too small"), process_metrics.size(), 0};
  }
  const auto snapshot = supervisor != nullptr ? supervisor->snapshot() : class_c_process_snapshot{};
  const std::array<uint64_t, process_metrics.size()> values{{
      static_cast<uint64_t>(snapshot.state),
      snapshot.pid > 0 ? static_cast<uint64_t>(snapshot.pid) : 0,
      snapshot.publication_enabled,
      snapshot.route_generation,
      snapshot.successful_starts,
      snapshot.successful_restarts,
      snapshot.process_exits,
      snapshot.start_failures,
      snapshot.forced_kills,
      static_cast<uint32_t>(snapshot.last_wait_status),
      snapshot.publication_requested,
      snapshot.accepted_results,
      snapshot.busy_results,
      snapshot.rejected_results,
      snapshot.result_protocol_failures,
      snapshot.heartbeat_requests,
      snapshot.heartbeat_responses,
      snapshot.heartbeat_losses,
      snapshot.last_heartbeat_latency_ns,
      snapshot.management_responses,
      snapshot.management_replays,
      snapshot.management_timeouts,
      snapshot.route_metrics.publications,
      snapshot.route_metrics.payload_bytes,
      snapshot.route_metrics.backpressure_drops,
      snapshot.route_metrics.invalid_drops,
      snapshot.route_metrics.encode_failures,
      snapshot.route_metrics.notification_failures,
      snapshot.route_metrics.inactive_bypasses,
      snapshot.route_metrics.concurrent_drops,
      snapshot.route_metrics.snapshot_copy_operations,
      snapshot.route_metrics.snapshot_copy_bytes,
      snapshot.route_metrics.lease_operations,
      snapshot.route_metrics.export_copy_operations,
      snapshot.route_metrics.export_copy_bytes,
      snapshot.route_metrics.export_slots_capacity,
      snapshot.route_metrics.export_slots_in_use,
      snapshot.route_metrics.export_slots_high_watermark,
      snapshot.route_metrics.export_slot_reservation_failures,
      snapshot.route_metrics.e3_payload_copy_operations,
      snapshot.route_metrics.e3_payload_copy_bytes,
  }};
  const uint64_t                                     timestamp = monotonic_time_ns();
  for (std::size_t index = 0; index != process_metrics.size(); ++index) {
    samples[index]                        = {};
    samples[index].name                   = process_metrics[index].name;
    samples[index].value                  = unsigned_value(values[index]);
    samples[index].monotonic_timestamp_ns = timestamp;
  }
  return {result(OCUDU_DAPP_OK_V1), process_metrics.size(), 0};
}

management_call_result class_c_process_instance::manage_model(const ocudu_dapp_model_request_v1& model)
{
  std::lock_guard<std::mutex> lock(management_mutex);
  refresh_locked();
  if (supervisor == nullptr || model.struct_size < sizeof(model) || model.expected_generation == 0 ||
      model.action < OCUDU_DAPP_MODEL_STAGE_V1 || model.action > OCUDU_DAPP_MODEL_RETIRE_V1 ||
      model.artifact.size > 1024 * 1024 || (model.artifact.size != 0 && model.artifact.data == nullptr)) {
    return {result(OCUDU_DAPP_INVALID_INPUT_V1, "invalid process model request"), 0, 0};
  }
  ocudu_dapp_process_management_request_v1 request{};
  request.struct_size         = sizeof(request);
  request.operation           = OCUDU_DAPP_PROCESS_MANAGEMENT_MODEL_V1;
  request.expected_generation = model.expected_generation;
  request.boundary_or_action  = model.action;
  request.flags               = model.dry_run ? OCUDU_DAPP_PROCESS_MANAGEMENT_DRY_RUN_V1 : 0;
  request.artifact_size       = model.artifact.size;
  if (!copy_fixed(request.model_id, model.model_id) ||
      (model.model_version != nullptr && model.model_version[0] != '\0' &&
       !copy_fixed(request.model_version, model.model_version))) {
    return {result(OCUDU_DAPP_INVALID_INPUT_V1, "invalid process model identity"), 0, 0};
  }
  auto managed = supervisor->manage(
      request, {static_cast<const uint8_t*>(model.artifact.data), static_cast<std::size_t>(model.artifact.size)});
  return {result(managed.operation.status, managed.operation.detail.c_str()), 0, managed.response.resulting_generation};
}

ocudu_dapp_lifecycle_state_v1 class_c_process_instance::state() const
{
  std::lock_guard<std::mutex> lock(management_mutex);
  refresh_locked();
  return lifecycle_state.state();
}

std::size_t class_c_process_instance::drain_incidents(incident_store& destination)
{
  std::lock_guard<std::mutex> lock(management_mutex);
  refresh_locked();
  return destination.drain(incidents);
}

void class_c_process_instance::set_incident_identity(const uint64_t instance_id[2],
                                                     const uint64_t package_id[2],
                                                     uint64_t       generation)
{
  std::lock_guard<std::mutex> lock(management_mutex);
  incident_identity.instance_id[0] = instance_id[0];
  incident_identity.instance_id[1] = instance_id[1];
  incident_identity.package_id[0]  = package_id[0];
  incident_identity.package_id[1]  = package_id[1];
  incident_identity.app_generation = generation;
  if (supervisor != nullptr) {
    supervisor->set_result_identity(instance_id, generation);
  }
}

instance_operation_result class_c_process_instance::result(ocudu_dapp_status_v1 status, const char* detail) const
{
  return {status, lifecycle_state.state(), detail != nullptr ? detail : ""};
}

void class_c_process_instance::refresh_locked() const
{
  if (supervisor == nullptr) {
    return;
  }
  const auto snapshot = supervisor->snapshot();
  if (snapshot.state == class_c_process_state::running || snapshot.state == class_c_process_state::failed) {
    const auto new_restarts = snapshot.successful_restarts - observed_restarts;
    const auto new_exits    = snapshot.process_exits - observed_exits;
    for (uint64_t index = 0; index != new_exits; ++index) {
      const bool restarted = index < new_restarts;
      record_incident(incident_phase::report,
                      incident_cause::process_exit,
                      restarted ? incident_action::process_restarted : incident_action::disabled,
                      OCUDU_DAPP_INTERNAL_ERROR_V1,
                      restarted ? "Class-C worker exited and was restarted" : "Class-C worker exited",
                      !restarted);
    }
    observed_exits    = snapshot.process_exits;
    observed_restarts = snapshot.successful_restarts;
  }
  if (snapshot.rejected_results != observed_rejected_results) {
    record_incident(incident_phase::report,
                    incident_cause::invalid_output,
                    incident_action::observed,
                    OCUDU_DAPP_INVALID_OUTPUT_V1,
                    "Class-C result failed source or schema admission",
                    false);
    observed_rejected_results = snapshot.rejected_results;
  }
  if (snapshot.result_protocol_failures != observed_protocol_failures) {
    record_incident(incident_phase::report,
                    incident_cause::security_violation,
                    incident_action::observed,
                    OCUDU_DAPP_INVALID_INPUT_V1,
                    "Class-C result control protocol violation",
                    false);
    observed_protocol_failures = snapshot.result_protocol_failures;
  }
  if (snapshot.heartbeat_losses != observed_heartbeat_losses) {
    const auto new_losses = snapshot.heartbeat_losses - observed_heartbeat_losses;
    for (uint64_t index = 0; index != new_losses; ++index) {
      record_incident(incident_phase::report,
                      incident_cause::heartbeat_loss,
                      snapshot.state == class_c_process_state::running ? incident_action::process_restarted
                                                                       : incident_action::disabled,
                      OCUDU_DAPP_DEADLINE_V1,
                      "Class-C worker missed its authenticated heartbeat deadline",
                      snapshot.state != class_c_process_state::running);
    }
    observed_heartbeat_losses = snapshot.heartbeat_losses;
  }
  if (snapshot.management_timeouts != observed_management_timeouts) {
    const auto new_timeouts = snapshot.management_timeouts - observed_management_timeouts;
    for (uint64_t index = 0; index != new_timeouts; ++index) {
      record_incident(incident_phase::configure,
                      incident_cause::deadline_miss,
                      incident_action::process_restarted,
                      OCUDU_DAPP_DEADLINE_V1,
                      "Class-C management request timed out; publication was withdrawn and worker restart requested",
                      false);
    }
    observed_management_timeouts = snapshot.management_timeouts;
  }
  if (snapshot.state == class_c_process_state::failed) {
    if (lifecycle_state.state() == OCUDU_DAPP_STATE_ACTIVE_V1) {
      lifecycle_state.transition_to(OCUDU_DAPP_STATE_DEGRADED_V1);
    } else if (lifecycle_state.state() != OCUDU_DAPP_STATE_DRAINING_V1 &&
               lifecycle_state.state() != OCUDU_DAPP_STATE_INACTIVE_V1 &&
               lifecycle_state.state() != OCUDU_DAPP_STATE_UNLOADED_V1 &&
               lifecycle_state.state() != OCUDU_DAPP_STATE_QUARANTINED_RESIDENT_V1) {
      lifecycle_state.fail();
    }
  }
}

void class_c_process_instance::synchronize_counters_locked() const
{
  if (supervisor == nullptr) {
    return;
  }
  const auto snapshot          = supervisor->snapshot();
  observed_exits               = snapshot.process_exits;
  observed_restarts            = snapshot.successful_restarts;
  observed_rejected_results    = snapshot.rejected_results;
  observed_protocol_failures   = snapshot.result_protocol_failures;
  observed_heartbeat_losses    = snapshot.heartbeat_losses;
  observed_management_timeouts = snapshot.management_timeouts;
}

void class_c_process_instance::record_incident(incident_phase       phase,
                                               incident_cause       cause,
                                               incident_action      action,
                                               ocudu_dapp_status_v1 status,
                                               const char*          detail,
                                               bool                 breaker_open) const noexcept
{
  auto context  = incident_identity;
  context.phase = phase;
  incidents.try_push(make_incident(context, cause, action, status, detail, breaker_open));
}
