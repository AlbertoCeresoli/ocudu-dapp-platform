// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "fixture_common.h"
#include "ocudu/dapp/use_cases/v1/channel_estimator.h"

namespace {

ocudu_dapp_status_v1 create_worker(void* instance, uint32_t, void** output)
{
  *output = instance;
  return OCUDU_DAPP_OK_V1;
}

void destroy_worker(void*, void*) {}

ocudu_dapp_status_v1
invoke(void*, const ocudu_dapp_channel_estimator_input_v1*, ocudu_dapp_channel_estimator_output_v1*)
{
  throw std::runtime_error("preflight invalid-vector exception");
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
                                                                     1,
                                                                     1,
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
  module->nof_interfaces = 1;
  module->interfaces     = &interface;
  return OCUDU_DAPP_OK_V1;
}
