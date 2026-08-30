// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "fixture_common.h"
#include "ocudu/dapp/use_cases/v1/scheduler.h"
#include <algorithm>
#include <new>
#include <stdexcept>

namespace {

struct worker_context {
  fixture_control* control;
  uint32_t         worker_id;
};

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

ocudu_dapp_status_v1
invoke(void* context, const ocudu_dapp_scheduler_input_v1* input, ocudu_dapp_scheduler_output_v1* output)
{
  auto* worker = static_cast<worker_context*>(context);
  if (worker == nullptr || input == nullptr || output == nullptr || input->invocation.worker_id != worker->worker_id) {
    return OCUDU_DAPP_INVALID_INPUT_V1;
  }
  auto& control = *worker->control;
  control.data_invocations.fetch_add(1, std::memory_order_relaxed);
  if (control.throw_data_invocation.load(std::memory_order_relaxed)) {
    throw std::runtime_error("fixture Class-B invocation exception");
  }
  if (control.invalid_data_output.load(std::memory_order_relaxed)) {
    *output = {};
    return OCUDU_DAPP_OK_V1;
  }
  if (control.replace_data_output.load(std::memory_order_relaxed)) {
    ++output->intent_capacity;
    return OCUDU_DAPP_OK_V1;
  }
  for (uint32_t index = 0; index != input->nof_candidates; ++index) {
    auto& intent        = output->intents[index];
    intent.struct_size  = sizeof(intent);
    intent.candidate_id = input->candidates[index].candidate_id;
    intent.flags        = OCUDU_DAPP_SCHEDULER_INTENT_PRIORITY_V1;
    intent.priority     = input->candidates[index].conventional_priority + 1.0;
  }
  if (control.publish_prb_avoidance.load(std::memory_order_relaxed) &&
      (input->allowed_output_flags & OCUDU_DAPP_SCHEDULER_ALLOW_AVOID_PRB_MASK_V1) != 0) {
    const auto* interference = reinterpret_cast<const uint8_t*>(input->optional_interference_mask.native_handle);
    auto*       avoidance    = reinterpret_cast<uint8_t*>(output->avoid_prb_mask.native_handle);
    if (interference == nullptr || avoidance == nullptr || input->cell_nof_prbs == 0 ||
        input->optional_interference_mask.dimensions[0] != input->cell_nof_prbs ||
        output->avoid_prb_mask.dimensions[0] != input->cell_nof_prbs) {
      return OCUDU_DAPP_INVALID_INPUT_V1;
    }
    std::transform(interference, interference + input->cell_nof_prbs, avoidance, [](uint8_t confidence) {
      return confidence >= 192 ? uint8_t{1} : uint8_t{0};
    });
    if (std::any_of(avoidance, avoidance + input->cell_nof_prbs, [](uint8_t value) { return value != 0; })) {
      output->flags |= OCUDU_DAPP_SCHEDULER_OUTPUT_AVOID_PRB_MASK_V1;
    }
  }
  output->nof_intents         = input->nof_candidates;
  output->decision_generation = input->candidate_generation;
  return OCUDU_DAPP_OK_V1;
}

const ocudu_dapp_worker_api_v1           worker_api = {sizeof(ocudu_dapp_worker_api_v1),
                                                       OCUDU_DAPP_ABI_MAJOR_V1,
                                                       OCUDU_DAPP_ABI_MINOR_V1,
                                                       create_worker,
                                                       destroy_worker,
                                                       {}};
const ocudu_dapp_scheduler_interface_v1  scheduler  = {worker_api, invoke, {}};
const ocudu_dapp_interface_descriptor_v1 interface  = {sizeof(ocudu_dapp_interface_descriptor_v1),
                                                       OCUDU_DAPP_ABI_MAJOR_V1,
                                                       OCUDU_DAPP_ABI_MINOR_V1,
                                                       OCUDU_DAPP_INTERFACE_SCHEDULER_V1,
                                                       1,
                                                       0,
                                                       OCUDU_DAPP_CLASS_B_DEADLINE_V1,
                                                       1U << (OCUDU_DAPP_PLACEMENT_IN_PROCESS_V1 - 1),
                                                       OCUDU_DAPP_THREADING_PER_WORKER_V1,
                                                       0,
                                                       &scheduler,
                                                       0,
                                                       {}};

} // namespace

extern "C" OCUDU_DAPP_EXPORT_V1 ocudu_dapp_status_v1 ocudu_dapp_query_v1(const ocudu_dapp_host_info_v1*,
                                                                         ocudu_dapp_module_v1* module)
{
  populate_valid_module(module);
  module->global_name    = "org.deepsig.ocudu.test.class-b";
  module->nof_interfaces = 1;
  module->interfaces     = &interface;
  return OCUDU_DAPP_OK_V1;
}
