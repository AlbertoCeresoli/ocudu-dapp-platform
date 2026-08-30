// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#ifndef OCUDU_DAPP_ABI_V1_METRIC_CATALOG_H
#define OCUDU_DAPP_ABI_V1_METRIC_CATALOG_H

/// Canonical cross-vendor metric names. A module may add names in its own
/// namespace, but must use these names and semantics for equivalent values.
#define OCUDU_DAPP_METRIC_INVOKE_COUNT_V1 "dapp.invoke.count"
#define OCUDU_DAPP_METRIC_INVOKE_LATENCY_NS_V1 "dapp.invoke.latency_ns"
#define OCUDU_DAPP_METRIC_FALLBACK_COUNT_V1 "dapp.fallback.count"
#define OCUDU_DAPP_METRIC_BREAKER_OPEN_V1 "dapp.breaker.open"
#define OCUDU_DAPP_METRIC_INPUT_MAX_LAYERS_V1 "dapp.input.max_layers"

#define OCUDU_DAPP_METRIC_HOST_LIFECYCLE_STATE_V1 "host.lifecycle_state"
#define OCUDU_DAPP_METRIC_HOST_BREAKER_STATE_V1 "host.breaker_state"
#define OCUDU_DAPP_METRIC_HOST_BREAKER_FAILURES_V1 "host.breaker_failures"
#define OCUDU_DAPP_METRIC_HOST_ACTIVITY_INVOCATIONS_V1 "host.activity.invocations"
#define OCUDU_DAPP_METRIC_HOST_ACTIVITY_LEASES_V1 "host.activity.leases"
#define OCUDU_DAPP_METRIC_HOST_ACTIVITY_CALLBACKS_V1 "host.activity.callbacks"
#define OCUDU_DAPP_METRIC_HOST_ACTIVITY_ACCELERATOR_V1 "host.activity.accelerator"
/// Bits 0..31 correspond to OCUDU DU cell identifiers 0..31.
#define OCUDU_DAPP_HOST_INPUT_CELL_BITMAP_BITS_V1 32U
#define OCUDU_DAPP_METRIC_HOST_INPUT_CELL_BITMAP_V1 "host.input.cell_bitmap"
/// Native worker contexts bound to this instance; zero for process placement.
#define OCUDU_DAPP_METRIC_HOST_NATIVE_WORKERS_V1 "host.native_workers"
/// Bound native worker contexts that have observed at least one valid DU input.
#define OCUDU_DAPP_METRIC_HOST_NATIVE_WORKERS_ACTIVE_V1 "host.native_workers_active"
#define OCUDU_DAPP_METRIC_HOST_INCIDENTS_DROPPED_V1 "host.incidents_dropped"

#define OCUDU_DAPP_METRIC_CLASS_A_ADMISSION_CLOSED_V1 "class_a.fallback.admission_closed"
#define OCUDU_DAPP_METRIC_CLASS_A_BREAKER_OPEN_V1 "class_a.fallback.breaker_open"
#define OCUDU_DAPP_METRIC_CLASS_A_OVERLAPPED_HOOK_V1 "class_a.fallback.overlapped_hook"
#define OCUDU_DAPP_METRIC_CLASS_A_UNSUPPORTED_METADATA_V1 "class_a.fallback.unsupported.metadata"
#define OCUDU_DAPP_METRIC_CLASS_A_UNSUPPORTED_LAYERS_V1 "class_a.fallback.unsupported.layers"
#define OCUDU_DAPP_METRIC_CLASS_A_UNSUPPORTED_RX_PORTS_V1 "class_a.fallback.unsupported.rx_ports"
#define OCUDU_DAPP_METRIC_CLASS_A_UNSUPPORTED_ALLOCATION_V1 "class_a.fallback.unsupported.allocation"
#define OCUDU_DAPP_METRIC_CLASS_A_UNSUPPORTED_MODULATION_V1 "class_a.fallback.unsupported.modulation"
#define OCUDU_DAPP_METRIC_CLASS_A_UNSUPPORTED_DMRS_V1 "class_a.fallback.unsupported.dmrs"
#define OCUDU_DAPP_METRIC_CLASS_A_UNSUPPORTED_TP_V1 "class_a.fallback.unsupported.transform_precoding"
#define OCUDU_DAPP_METRIC_CLASS_A_UNSUPPORTED_DC_V1 "class_a.fallback.unsupported.dc_overlap"
#define OCUDU_DAPP_METRIC_CLASS_A_MODULE_BYPASS_V1 "class_a.fallback.module_bypass"
#define OCUDU_DAPP_METRIC_CLASS_A_MODULE_UNSUPPORTED_V1 "class_a.fallback.module_unsupported"
#define OCUDU_DAPP_METRIC_CLASS_A_INVALID_INPUT_V1 "class_a.fallback.invalid_input"
#define OCUDU_DAPP_METRIC_CLASS_A_INVALID_OUTPUT_V1 "class_a.fallback.invalid_output"
#define OCUDU_DAPP_METRIC_CLASS_A_DEADLINE_V1 "class_a.fallback.deadline"
#define OCUDU_DAPP_METRIC_CLASS_A_EXCEPTION_V1 "class_a.fallback.exception"
#define OCUDU_DAPP_METRIC_CLASS_A_DEVICE_V1 "class_a.fallback.device"
#define OCUDU_DAPP_METRIC_CLASS_A_RESOURCE_V1 "class_a.fallback.resource"
#define OCUDU_DAPP_METRIC_CLASS_A_RETURNED_ERROR_V1 "class_a.fallback.returned_error"
#define OCUDU_DAPP_METRIC_CLASS_A_COMPLETION_DEADLINE_MISSES_V1 "class_a.completion.deadline_misses"

#define OCUDU_DAPP_METRIC_CLASS_B_INVOCATIONS_V1 "class_b.policy.invocations"
#define OCUDU_DAPP_METRIC_CLASS_B_CONVENTIONAL_TIME_NS_V1 "class_b.policy.conventional_time_ns"
#define OCUDU_DAPP_METRIC_CLASS_B_FEATURE_TIME_NS_V1 "class_b.policy.feature_time_ns"
#define OCUDU_DAPP_METRIC_CLASS_B_DAPP_TIME_NS_V1 "class_b.policy.dapp_time_ns"
#define OCUDU_DAPP_METRIC_CLASS_B_VALIDATION_TIME_NS_V1 "class_b.policy.validation_time_ns"
#define OCUDU_DAPP_METRIC_CLASS_B_COMMIT_TIME_NS_V1 "class_b.policy.commit_time_ns"
#define OCUDU_DAPP_METRIC_CLASS_B_TOTAL_TIME_NS_V1 "class_b.policy.total_time_ns"
#define OCUDU_DAPP_METRIC_CLASS_B_ACCEPTED_V1 "class_b.policy.accepted"
#define OCUDU_DAPP_METRIC_CLASS_B_EVALUATION_ACCEPTED_V1 "class_b.policy.evaluation_accepted"
#define OCUDU_DAPP_METRIC_CLASS_B_FALLBACKS_V1 "class_b.policy.fallbacks"
#define OCUDU_DAPP_METRIC_CLASS_B_VALIDATION_REJECTIONS_V1 "class_b.policy.validation_rejections"
#define OCUDU_DAPP_METRIC_CLASS_B_DEADLINE_MISSES_V1 "class_b.policy.deadline_misses"

#define OCUDU_DAPP_METRIC_PROCESS_SUPERVISOR_STATE_V1 "process.supervisor_state"
#define OCUDU_DAPP_METRIC_PROCESS_PID_V1 "process.pid"
#define OCUDU_DAPP_METRIC_PROCESS_PUBLICATION_ENABLED_V1 "process.publication_enabled"
#define OCUDU_DAPP_METRIC_PROCESS_ROUTE_GENERATION_V1 "process.route_generation"
#define OCUDU_DAPP_METRIC_PROCESS_SUCCESSFUL_STARTS_V1 "process.successful_starts"
#define OCUDU_DAPP_METRIC_PROCESS_SUCCESSFUL_RESTARTS_V1 "process.successful_restarts"
#define OCUDU_DAPP_METRIC_PROCESS_EXITS_V1 "process.exits"
#define OCUDU_DAPP_METRIC_PROCESS_START_FAILURES_V1 "process.start_failures"
#define OCUDU_DAPP_METRIC_PROCESS_FORCED_KILLS_V1 "process.forced_kills"
#define OCUDU_DAPP_METRIC_PROCESS_LAST_WAIT_STATUS_V1 "process.last_wait_status"
#define OCUDU_DAPP_METRIC_PROCESS_PUBLICATION_REQUESTED_V1 "process.publication_requested"
#define OCUDU_DAPP_METRIC_PROCESS_RESULTS_ACCEPTED_V1 "process.results_accepted"
#define OCUDU_DAPP_METRIC_PROCESS_RESULTS_BUSY_V1 "process.results_busy"
#define OCUDU_DAPP_METRIC_PROCESS_RESULTS_REJECTED_V1 "process.results_rejected"
#define OCUDU_DAPP_METRIC_PROCESS_RESULT_PROTOCOL_FAILURES_V1 "process.result_protocol_failures"
#define OCUDU_DAPP_METRIC_PROCESS_HEARTBEAT_REQUESTS_V1 "process.heartbeat_requests"
#define OCUDU_DAPP_METRIC_PROCESS_HEARTBEAT_RESPONSES_V1 "process.heartbeat_responses"
#define OCUDU_DAPP_METRIC_PROCESS_HEARTBEAT_LOSSES_V1 "process.heartbeat_losses"
#define OCUDU_DAPP_METRIC_PROCESS_LAST_HEARTBEAT_LATENCY_NS_V1 "process.last_heartbeat_latency_ns"
#define OCUDU_DAPP_METRIC_PROCESS_MANAGEMENT_RESPONSES_V1 "process.management_responses"
#define OCUDU_DAPP_METRIC_PROCESS_MANAGEMENT_REPLAYS_V1 "process.management_replays"
#define OCUDU_DAPP_METRIC_PROCESS_MANAGEMENT_TIMEOUTS_V1 "process.management_timeouts"

#define OCUDU_DAPP_METRIC_CLASS_C_ROUTE_PUBLICATIONS_V1 "class_c.route.publications"
#define OCUDU_DAPP_METRIC_CLASS_C_ROUTE_PAYLOAD_BYTES_V1 "class_c.route.payload_bytes"
#define OCUDU_DAPP_METRIC_CLASS_C_ROUTE_BACKPRESSURE_DROPS_V1 "class_c.route.drops.backpressure"
#define OCUDU_DAPP_METRIC_CLASS_C_ROUTE_INVALID_DROPS_V1 "class_c.route.drops.invalid"
#define OCUDU_DAPP_METRIC_CLASS_C_ROUTE_ENCODE_FAILURES_V1 "class_c.route.failures.encode"
#define OCUDU_DAPP_METRIC_CLASS_C_ROUTE_NOTIFICATION_FAILURES_V1 "class_c.route.failures.notification"
#define OCUDU_DAPP_METRIC_CLASS_C_ROUTE_INACTIVE_BYPASSES_V1 "class_c.route.bypass.inactive"
#define OCUDU_DAPP_METRIC_CLASS_C_ROUTE_CONCURRENT_DROPS_V1 "class_c.route.drops.concurrent"

/// Tensor bytes copied after producer readiness into the CPU process snapshot.
#define OCUDU_DAPP_METRIC_CLASS_C_COPY_SNAPSHOT_COUNT_V1 "class_c.copy.snapshot.count"
#define OCUDU_DAPP_METRIC_CLASS_C_COPY_SNAPSHOT_BYTES_V1 "class_c.copy.snapshot.bytes"
/// Publications consuming a producer allocation under a lifetime lease.
#define OCUDU_DAPP_METRIC_CLASS_C_COPY_LEASE_COUNT_V1 "class_c.copy.lease.count"
/// Tensor bytes copied into a dedicated accelerator export allocation.
#define OCUDU_DAPP_METRIC_CLASS_C_COPY_EXPORT_COUNT_V1 "class_c.copy.export.count"
#define OCUDU_DAPP_METRIC_CLASS_C_COPY_EXPORT_BYTES_V1 "class_c.copy.export.bytes"
/// Fixed accelerator export-pool pressure and reservation failures.
#define OCUDU_DAPP_METRIC_CLASS_C_EXPORT_SLOTS_CAPACITY_V1 "class_c.export_slots.capacity"
#define OCUDU_DAPP_METRIC_CLASS_C_EXPORT_SLOTS_IN_USE_V1 "class_c.export_slots.in_use"
#define OCUDU_DAPP_METRIC_CLASS_C_EXPORT_SLOTS_HIGH_WATERMARK_V1 "class_c.export_slots.high_watermark"
#define OCUDU_DAPP_METRIC_CLASS_C_EXPORT_SLOT_RESERVATION_FAILURES_V1 "class_c.export_slots.reservation_failures"
/// Tensor bytes materialized into a portable E3 payload.
#define OCUDU_DAPP_METRIC_CLASS_C_COPY_E3_PAYLOAD_COUNT_V1 "class_c.copy.e3_payload.count"
#define OCUDU_DAPP_METRIC_CLASS_C_COPY_E3_PAYLOAD_BYTES_V1 "class_c.copy.e3_payload.bytes"

#endif
