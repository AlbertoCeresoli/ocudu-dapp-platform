// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "fixture_common.h"
#include "ocudu/dapp/use_cases/v1/srs_isac.h"
#include <new>

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

ocudu_dapp_status_v1 publish(void* context, const ocudu_dapp_srs_isac_input_v1* input)
{
  auto* worker = static_cast<worker_context*>(context);
  if (worker == nullptr || input == nullptr || input->invocation.worker_id != worker->worker_id) {
    return OCUDU_DAPP_INVALID_INPUT_V1;
  }
  worker->control->data_invocations.fetch_add(1, std::memory_order_relaxed);
  if (!worker->control->publish_srs_preview.load(std::memory_order_relaxed)) {
    return OCUDU_DAPP_OK_V1;
  }
  const auto* metadata = static_cast<const ocudu_dapp_srs_isac_metadata_v1*>(input->srs_metadata.metadata.data);
  const auto& host     = worker->control->host_api;
  if (metadata == nullptr || host.publish_prepared == nullptr) {
    return OCUDU_DAPP_INVALID_INPUT_V1;
  }

  ocudu_dapp_srs_isac_cir_preview_v1 output{};
  output.struct_size             = sizeof(output);
  output.abi_major               = OCUDU_DAPP_ABI_MAJOR_V1;
  output.abi_minor               = OCUDU_DAPP_ABI_MINOR_V1;
  output.context.struct_size     = sizeof(output.context);
  output.context.schema_id       = OCUDU_DAPP_REPORT_SRS_CIR_PREVIEW_V1;
  output.context.schema_version  = 1;
  output.context.source_sequence = input->source_sequence;
  output.context.cell_id         = input->invocation.cell_id;
  output.context.confidence_q16  = 1U << 16U;
  output.context.created         = input->invocation.radio_time;
  output.context.valid_from      = input->invocation.radio_time;
  output.context.valid_until     = input->invocation.radio_time;
  output.source_srs_sequence     = input->source_sequence;
  output.srs_resource_id         = metadata->srs_resource_id;
  output.srs_config_id           = metadata->srs_config_id;
  output.tap_count               = 2;
  output.peak_delay_bin          = 1;
  output.mean_channel_power      = 2.0F;
  output.noise_corrected_power   = 1.75F;
  output.peak_cir_power          = 1.0F;
  output.noise_variance          = metadata->noise_variance;
  output.tap_power[0]            = 0.25F;
  output.tap_power[1]            = 1.0F;
  return host.publish_prepared(host.host_context,
                               OCUDU_DAPP_SRS_ISAC_OUTPUT_CIR_PREVIEW_V1,
                               OCUDU_DAPP_REPORT_SRS_CIR_PREVIEW_V1,
                               &output,
                               sizeof(output));
}

const ocudu_dapp_worker_api_v1           worker_api = {sizeof(ocudu_dapp_worker_api_v1),
                                                       OCUDU_DAPP_ABI_MAJOR_V1,
                                                       OCUDU_DAPP_ABI_MINOR_V1,
                                                       create_worker,
                                                       destroy_worker,
                                                       {}};
const ocudu_dapp_srs_isac_interface_v1   srs        = {worker_api, publish, {}};
const ocudu_dapp_interface_descriptor_v1 interface  = {sizeof(ocudu_dapp_interface_descriptor_v1),
                                                       OCUDU_DAPP_ABI_MAJOR_V1,
                                                       OCUDU_DAPP_ABI_MINOR_V1,
                                                       OCUDU_DAPP_INTERFACE_SRS_ISAC_V1,
                                                       1,
                                                       0,
                                                       OCUDU_DAPP_CLASS_C_OPEN_LOOP_V1,
                                                       1U << (OCUDU_DAPP_PLACEMENT_IN_PROCESS_V1 - 1),
                                                       OCUDU_DAPP_THREADING_PER_WORKER_V1,
                                                       0,
                                                       &srs,
                                                       0,
                                                       {}};

} // namespace

extern "C" OCUDU_DAPP_EXPORT_V1 ocudu_dapp_status_v1 ocudu_dapp_query_v1(const ocudu_dapp_host_info_v1*,
                                                                         ocudu_dapp_module_v1* module)
{
  populate_valid_module(module);
  module->global_name    = "org.deepsig.ocudu.test.class-c-srs";
  module->nof_interfaces = 1;
  module->interfaces     = &interface;
  return OCUDU_DAPP_OK_V1;
}
