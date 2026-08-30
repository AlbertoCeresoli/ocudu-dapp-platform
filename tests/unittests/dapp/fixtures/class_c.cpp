// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "fixture_common.h"
#include "ocudu/dapp/use_cases/v1/coordination.h"
#include "ocudu/dapp/use_cases/v1/spectrum.h"
#include <array>
#include <new>
#include <stdexcept>

namespace {

struct worker_context {
  fixture_control* control;
  uint32_t         worker_id;
};

bool add_slots(const ocudu_dapp_radio_time_v1& input, uint32_t delta, ocudu_dapp_radio_time_v1& output) noexcept
{
  if (input.numerology > 4 || input.sfn >= 1024 || input.reserved != 0) {
    return false;
  }
  const uint32_t slots_per_frame = 10U << input.numerology;
  if (input.slot >= slots_per_frame) {
    return false;
  }
  const uint32_t cycle = 1024U * slots_per_frame;
  const uint32_t value = (static_cast<uint32_t>(input.sfn) * slots_per_frame + input.slot + delta) % cycle;
  output               = {static_cast<uint16_t>(value / slots_per_frame),
                          static_cast<uint16_t>(value % slots_per_frame),
                          input.numerology,
                          0};
  return true;
}

ocudu_dapp_status_v1 create_worker(void* instance, uint32_t worker_id, void** output)
{
  if (instance == nullptr || output == nullptr) {
    return OCUDU_DAPP_INVALID_INPUT_V1;
  }
  auto* control = static_cast<fixture_control*>(instance);
  auto* worker  = new (std::nothrow) worker_context{control, worker_id};
  if (worker == nullptr) {
    return OCUDU_DAPP_RESOURCE_V1;
  }
  control->data_worker_creates.fetch_add(1, std::memory_order_relaxed);
  *output = worker;
  return OCUDU_DAPP_OK_V1;
}

void destroy_worker(void*, void* context)
{
  auto* worker = static_cast<worker_context*>(context);
  worker->control->data_worker_destroys.fetch_add(1, std::memory_order_relaxed);
  delete worker;
}

ocudu_dapp_status_v1 publish(void* context, const ocudu_dapp_spectrum_input_v1* input)
{
  auto* worker = static_cast<worker_context*>(context);
  if (worker == nullptr || input == nullptr || input->invocation.worker_id != worker->worker_id) {
    return OCUDU_DAPP_INVALID_INPUT_V1;
  }
  worker->control->data_invocations.fetch_add(1, std::memory_order_relaxed);
  if (worker->control->throw_data_invocation.load(std::memory_order_relaxed)) {
    throw std::runtime_error("fixture Class-C invocation exception");
  }
  if (worker->control->publish_interference.load(std::memory_order_relaxed)) {
    std::array<uint8_t, 52> confidence{};
    confidence[7] = 255;
    ocudu_dapp_spectrum_interference_map_v1 output{};
    output.struct_size             = sizeof(output);
    output.abi_major               = OCUDU_DAPP_ABI_MAJOR_V1;
    output.abi_minor               = OCUDU_DAPP_ABI_MINOR_V1;
    output.context.struct_size     = sizeof(output.context);
    output.context.schema_id       = OCUDU_DAPP_CONTEXT_INTERFERENCE_MAP_V1;
    output.context.schema_version  = 1;
    output.context.source_sequence = input->source_sequence;
    output.context.cell_id         = input->invocation.cell_id;
    output.context.confidence_q16  = 1U << 16U;
    output.context.created         = input->invocation.radio_time;
    if (!add_slots(output.context.created, 1, output.context.valid_from) ||
        !add_slots(output.context.created, 4, output.context.valid_until)) {
      return OCUDU_DAPP_INVALID_INPUT_V1;
    }
    output.prb_confidence.struct_size           = sizeof(output.prb_confidence);
    output.prb_confidence.abi_major             = OCUDU_DAPP_ABI_MAJOR_V1;
    output.prb_confidence.abi_minor             = OCUDU_DAPP_ABI_MINOR_V1;
    output.prb_confidence.domain                = OCUDU_DAPP_MEM_HOST_V1;
    output.prb_confidence.backend_id            = OCUDU_DAPP_BACKEND_HOST_V1;
    output.prb_confidence.element_type          = OCUDU_DAPP_ELEMENT_U8_V1;
    output.prb_confidence.rank                  = 1;
    output.prb_confidence.access                = OCUDU_DAPP_ACCESS_READ_V1 | OCUDU_DAPP_ACCESS_HOST_VISIBLE_V1;
    output.prb_confidence.allocation_id         = input->source_sequence;
    output.prb_confidence.allocation_generation = input->source_sequence;
    output.prb_confidence.native_handle         = reinterpret_cast<uintptr_t>(confidence.data());
    output.prb_confidence.byte_length           = confidence.size();
    output.prb_confidence.dimensions[0]         = confidence.size();
    output.prb_confidence.byte_strides[0]       = sizeof(uint8_t);
    output.prb_confidence.layout_id             = OCUDU_DAPP_LAYOUT_PRB_INTERFERENCE_U8_V1;
    output.prb_confidence.layout_version        = 1;
    output.prb_confidence.lease_token           = input->source_sequence;
    output.source_grid_sequence                 = input->source_sequence;
    const auto* metadata = static_cast<const ocudu_dapp_spectrum_slot_metadata_v1*>(input->slot_metadata.metadata.data);
    output.source_plan_generation = metadata != nullptr ? metadata->source_plan_generation : 0;
    output.nof_prbs               = confidence.size();
    const auto& host              = worker->control->host_api;
    if (host.publish_prepared == nullptr) {
      return OCUDU_DAPP_UNSUPPORTED_V1;
    }
    const auto status = host.publish_prepared(host.host_context,
                                              OCUDU_DAPP_SPECTRUM_OUTPUT_INTERFERENCE_MAP_V1,
                                              OCUDU_DAPP_CONTEXT_INTERFERENCE_MAP_V1,
                                              &output,
                                              sizeof(output));
    if (status != OCUDU_DAPP_OK_V1) {
      return status;
    }
  }
  if (worker->control->publish_quiet_intent.load(std::memory_order_relaxed)) {
    const auto& host = worker->control->host_api;
    if (host.publish_prepared == nullptr || host.monotonic_ns == nullptr) {
      return OCUDU_DAPP_UNSUPPORTED_V1;
    }
    ocudu_dapp_quiet_period_intent_v1 intent{};
    intent.struct_size         = sizeof(intent);
    intent.abi_major           = OCUDU_DAPP_ABI_MAJOR_V1;
    intent.abi_minor           = OCUDU_DAPP_ABI_MINOR_V1;
    intent.request_id[0]       = input->source_sequence;
    intent.request_id[1]       = 0x5155494554;
    intent.duration_slots      = 1;
    intent.repeat_count        = 1;
    intent.first_prb           = 20;
    intent.nof_prbs            = 4;
    intent.requested_level     = OCUDU_DAPP_QUIET_NO_NEW_PUSCH_V1;
    intent.budget_class        = 1;
    intent.confidence_q16      = 1U << 15U;
    intent.expiry_monotonic_ns = host.monotonic_ns(host.host_context) + 500'000'000;
    if (!add_slots(input->invocation.radio_time, 4, intent.earliest_start)) {
      return OCUDU_DAPP_INVALID_INPUT_V1;
    }
    intent.latest_start = intent.earliest_start;
    const auto status   = host.publish_prepared(host.host_context,
                                              OCUDU_DAPP_SPECTRUM_OUTPUT_QUIET_PERIOD_INTENT_V1,
                                              OCUDU_DAPP_SPECTRUM_OUTPUT_QUIET_PERIOD_INTENT_V1,
                                              &intent,
                                              sizeof(intent));
    if (status != OCUDU_DAPP_OK_V1) {
      return status;
    }
  }
  if (worker->control->throw_after_prepared_output.load(std::memory_order_relaxed)) {
    throw std::runtime_error("fixture exception after prepared output");
  }
  return OCUDU_DAPP_OK_V1;
}

ocudu_dapp_status_v1 publish_plan(void* context, const ocudu_dapp_ul_resource_plan_v1* plan)
{
  auto* worker = static_cast<worker_context*>(context);
  if (worker == nullptr || plan == nullptr || plan->plan_generation == 0) {
    return OCUDU_DAPP_INVALID_INPUT_V1;
  }
  worker->control->data_invocations.fetch_add(1, std::memory_order_relaxed);
  worker->control->advance_plan_calls.fetch_add(1, std::memory_order_relaxed);
  worker->control->advance_plan_spans.store(plan->nof_spans, std::memory_order_relaxed);
  if (worker->control->throw_data_invocation.load(std::memory_order_relaxed)) {
    throw std::runtime_error("fixture Class-C plan exception");
  }
  return OCUDU_DAPP_OK_V1;
}

const ocudu_dapp_worker_api_v1           worker_api = {sizeof(ocudu_dapp_worker_api_v1),
                                                       OCUDU_DAPP_ABI_MAJOR_V1,
                                                       OCUDU_DAPP_ABI_MINOR_V1,
                                                       create_worker,
                                                       destroy_worker,
                                                       {}};
const ocudu_dapp_spectrum_interface_v1   spectrum   = {worker_api, publish, publish_plan, {}};
const ocudu_dapp_interface_descriptor_v1 interface  = {
    sizeof(ocudu_dapp_interface_descriptor_v1),
    OCUDU_DAPP_ABI_MAJOR_V1,
    OCUDU_DAPP_ABI_MINOR_V1,
    OCUDU_DAPP_INTERFACE_SPECTRUM_V1,
    1,
    0,
    OCUDU_DAPP_CLASS_C_OPEN_LOOP_V1,
    (1U << (OCUDU_DAPP_PLACEMENT_IN_PROCESS_V1 - 1)) | (1U << (OCUDU_DAPP_PLACEMENT_OUT_OF_PROCESS_V1 - 1)),
    OCUDU_DAPP_THREADING_PER_WORKER_V1,
    0,
    &spectrum,
    OCUDU_DAPP_SPECTRUM_CAP_INTERFERENCE_MAP_V1 | OCUDU_DAPP_SPECTRUM_CAP_QUIET_PERIOD_INTENT_V1 |
        OCUDU_DAPP_SPECTRUM_CAP_FUTURE_UL_PLAN_V1,
    {}};

} // namespace

extern "C" OCUDU_DAPP_EXPORT_V1 ocudu_dapp_status_v1 ocudu_dapp_query_v1(const ocudu_dapp_host_info_v1*,
                                                                         ocudu_dapp_module_v1* module)
{
  populate_valid_module(module);
  module->global_name    = "org.deepsig.ocudu.test.class-c";
  module->nof_interfaces = 1;
  module->interfaces     = &interface;
  return OCUDU_DAPP_OK_V1;
}
