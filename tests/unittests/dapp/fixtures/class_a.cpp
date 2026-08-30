// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "fixture_common.h"
#include "ocudu/dapp/use_cases/v1/channel_estimator.h"
#include <new>
#include <stdexcept>
#include <thread>

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

ocudu_dapp_status_v1 invoke(void*                                        context,
                            const ocudu_dapp_channel_estimator_input_v1* input,
                            ocudu_dapp_channel_estimator_output_v1*      output)
{
  auto* worker = static_cast<worker_context*>(context);
  if (worker == nullptr || input == nullptr || output == nullptr || input->invocation.worker_id != worker->worker_id) {
    return OCUDU_DAPP_INVALID_INPUT_V1;
  }
  auto& control = *worker->control;
  control.data_invocations.fetch_add(1, std::memory_order_relaxed);
  const uint32_t active  = control.active_data_invocations.fetch_add(1, std::memory_order_acq_rel) + 1;
  uint32_t       maximum = control.maximum_active_data_invocations.load(std::memory_order_relaxed);
  while (maximum < active && !control.maximum_active_data_invocations.compare_exchange_weak(
                                 maximum, active, std::memory_order_relaxed, std::memory_order_relaxed)) {
  }
  while (control.block_data_invocations.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
  control.active_data_invocations.fetch_sub(1, std::memory_order_acq_rel);
  if (control.throw_data_invocation.load(std::memory_order_relaxed)) {
    throw std::runtime_error("fixture Class-A invocation exception");
  }
  const auto status = control.data_status.load(std::memory_order_relaxed);
  if (status != OCUDU_DAPP_OK_V1) {
    return status;
  }
  if (control.invalid_data_output.load(std::memory_order_relaxed)) {
    *output = {};
    return OCUDU_DAPP_OK_V1;
  }
  if (control.replace_data_output.load(std::memory_order_relaxed)) {
    ++output->channel_estimates.allocation_id;
  }
  return OCUDU_DAPP_OK_V1;
}

const ocudu_dapp_worker_api_v1                  worker_api        = {sizeof(ocudu_dapp_worker_api_v1),
                                                                     OCUDU_DAPP_ABI_MAJOR_V1,
                                                                     OCUDU_DAPP_ABI_MINOR_V1,
                                                                     create_worker,
                                                                     destroy_worker,
                                                                     {}};
const ocudu_dapp_channel_estimator_interface_v1 channel_estimator = {worker_api, invoke, {}};
const ocudu_dapp_pusch_profile_v1               profile           = {sizeof(ocudu_dapp_pusch_profile_v1),
                                                                     OCUDU_DAPP_ABI_MAJOR_V1,
                                                                     OCUDU_DAPP_ABI_MINOR_V1,
                                                                     OCUDU_DAPP_PUSCH_PROFILE_V1,
                                                                     52,
                                                                     4,
                                                                     1,
                                                                     OCUDU_DAPP_PUSCH_MODULATION_QPSK_V1,
                                                                     OCUDU_DAPP_PUSCH_DMRS_TYPE1_V1,
                                                                     0,
                                                                     0,
                                                                     {}};
const ocudu_dapp_interface_descriptor_v1        interface         = {sizeof(ocudu_dapp_interface_descriptor_v1),
                                                                     OCUDU_DAPP_ABI_MAJOR_V1,
                                                                     OCUDU_DAPP_ABI_MINOR_V1,
                                                                     OCUDU_DAPP_INTERFACE_CHANNEL_ESTIMATOR_V1,
                                                                     1,
                                                                     0,
                                                                     OCUDU_DAPP_CLASS_A_INLINE_V1,
                                                                     1U << (OCUDU_DAPP_PLACEMENT_IN_PROCESS_V1 - 1),
                                                                     OCUDU_DAPP_THREADING_PER_WORKER_V1,
                                                                     0,
                                                                     &channel_estimator,
                                                                     0,
                                                                     &profile,
                                                                     {}};

} // namespace

extern "C" OCUDU_DAPP_EXPORT_V1 ocudu_dapp_status_v1 ocudu_dapp_query_v1(const ocudu_dapp_host_info_v1*,
                                                                         ocudu_dapp_module_v1* module)
{
  populate_valid_module(module);
  module->global_name    = "org.deepsig.ocudu.test.class-a";
  module->nof_interfaces = 1;
  module->interfaces     = &interface;
  return OCUDU_DAPP_OK_V1;
}
