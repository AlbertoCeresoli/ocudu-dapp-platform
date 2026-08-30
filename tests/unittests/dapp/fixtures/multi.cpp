// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "fixture_common.h"
#include "ocudu/dapp/use_cases/v1/channel_estimator.h"
#include "ocudu/dapp/use_cases/v1/spectrum.h"
#include <new>
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
  auto* worker = new (std::nothrow) worker_context{static_cast<fixture_control*>(instance), worker_id};
  if (worker == nullptr) {
    return OCUDU_DAPP_RESOURCE_V1;
  }
  worker->control->data_worker_creates.fetch_add(1, std::memory_order_relaxed);
  *output = worker;
  return OCUDU_DAPP_OK_V1;
}

void destroy_worker(void*, void* context)
{
  auto* worker = static_cast<worker_context*>(context);
  worker->control->data_worker_destroys.fetch_add(1, std::memory_order_relaxed);
  delete worker;
}

ocudu_dapp_status_v1 invoke_channel_estimator(void*                                        context,
                                              const ocudu_dapp_channel_estimator_input_v1* input,
                                              ocudu_dapp_channel_estimator_output_v1*)
{
  auto* worker = static_cast<worker_context*>(context);
  if (worker == nullptr || input == nullptr || input->invocation.worker_id != worker->worker_id) {
    return OCUDU_DAPP_INVALID_INPUT_V1;
  }
  auto& control = *worker->control;
  control.data_invocations.fetch_add(1, std::memory_order_relaxed);
  control.active_data_invocations.fetch_add(1, std::memory_order_acq_rel);
  while (control.block_data_invocations.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
  control.active_data_invocations.fetch_sub(1, std::memory_order_acq_rel);
  return OCUDU_DAPP_OK_V1;
}

ocudu_dapp_status_v1 publish_spectrum(void* context, const ocudu_dapp_spectrum_input_v1* input)
{
  auto* worker = static_cast<worker_context*>(context);
  if (worker == nullptr || input == nullptr || input->invocation.worker_id != worker->worker_id) {
    return OCUDU_DAPP_INVALID_INPUT_V1;
  }
  worker->control->data_invocations.fetch_add(1, std::memory_order_relaxed);
  return OCUDU_DAPP_OK_V1;
}

const ocudu_dapp_worker_api_v1                  worker_api        = {sizeof(ocudu_dapp_worker_api_v1),
                                                                     OCUDU_DAPP_ABI_MAJOR_V1,
                                                                     OCUDU_DAPP_ABI_MINOR_V1,
                                                                     create_worker,
                                                                     destroy_worker,
                                                                     {}};
const ocudu_dapp_channel_estimator_interface_v1 channel_estimator = {worker_api, invoke_channel_estimator, {}};
const ocudu_dapp_spectrum_interface_v1          spectrum          = {worker_api, publish_spectrum, {}};
const ocudu_dapp_interface_descriptor_v1        interfaces[]      = {{sizeof(ocudu_dapp_interface_descriptor_v1),
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
                                                                      {}},
                                                                     {sizeof(ocudu_dapp_interface_descriptor_v1),
                                                                      OCUDU_DAPP_ABI_MAJOR_V1,
                                                                      OCUDU_DAPP_ABI_MINOR_V1,
                                                                      OCUDU_DAPP_INTERFACE_SPECTRUM_V1,
                                                                      1,
                                                                      0,
                                                                      OCUDU_DAPP_CLASS_C_OPEN_LOOP_V1,
                                                                      1U << (OCUDU_DAPP_PLACEMENT_IN_PROCESS_V1 - 1),
                                                                      OCUDU_DAPP_THREADING_PER_WORKER_V1,
                                                                      0,
                                                                      &spectrum,
                                                                      0,
                                                                      {}}};

} // namespace

extern "C" OCUDU_DAPP_EXPORT_V1 ocudu_dapp_status_v1 ocudu_dapp_query_v1(const ocudu_dapp_host_info_v1*,
                                                                         ocudu_dapp_module_v1* module)
{
  populate_valid_module(module);
  module->global_name    = "org.deepsig.ocudu.test.multi";
  module->nof_interfaces = sizeof(interfaces) / sizeof(interfaces[0]);
  module->interfaces     = interfaces;
  return OCUDU_DAPP_OK_V1;
}
