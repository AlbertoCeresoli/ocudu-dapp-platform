// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "fixture_common.h"
#include "ocudu/dapp/use_cases/v1/srs_channel_estimator.h"
#include <complex>
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

/// Wideband least squares without time alignment: enough to see the host deliver the tensors it promises.
ocudu_dapp_status_v1 invoke(void*                                            context,
                            const ocudu_dapp_srs_channel_estimator_input_v1* input,
                            ocudu_dapp_srs_channel_estimator_output_v1*      output)
{
  auto* worker = static_cast<worker_context*>(context);
  if (worker == nullptr || input == nullptr || output == nullptr || input->invocation.worker_id != worker->worker_id) {
    return OCUDU_DAPP_INVALID_INPUT_V1;
  }
  auto& control = *worker->control;
  control.data_invocations.fetch_add(1, std::memory_order_relaxed);
  const auto status = control.data_status.load(std::memory_order_relaxed);
  if (status != OCUDU_DAPP_OK_V1) {
    return status;
  }
  if (control.replace_data_output.load(std::memory_order_relaxed)) {
    ++output->channel_matrix.allocation_id;
    return OCUDU_DAPP_OK_V1;
  }
  const auto& srs = *static_cast<const ocudu_dapp_srs_channel_estimator_metadata_v1*>(input->srs.metadata.data);
  const auto* rx  = reinterpret_cast<const std::complex<float>*>(input->rx_symbols.native_handle);
  const auto* ref = reinterpret_cast<const std::complex<float>*>(input->reference_symbols.native_handle);
  auto*       h   = reinterpret_cast<std::complex<float>*>(output->channel_matrix.native_handle);
  auto*       m   = reinterpret_cast<float*>(output->measurements.native_handle);
  for (uint32_t port = 0; port != srs.nof_rx_ports; ++port) {
    for (uint32_t tx = 0; tx != srs.nof_tx_ports; ++tx) {
      std::complex<float> sum;
      for (uint32_t n = 0; n != srs.sequence_length; ++n) {
        sum += rx[(static_cast<size_t>(srs.pilot_set[tx]) * srs.nof_rx_ports + port) * srs.nof_symbols *
                      srs.sequence_length +
                  n] *
               std::conj(ref[tx * srs.sequence_length + n]);
      }
      h[port * srs.nof_tx_ports + tx] = sum / static_cast<float>(srs.sequence_length);
    }
  }
  for (uint32_t index = 0; index != OCUDU_DAPP_SRS_MEASUREMENT_COUNT_V1; ++index) {
    m[index] = static_cast<float>(index);
  }
  return OCUDU_DAPP_OK_V1;
}

const ocudu_dapp_worker_api_v1                      worker_api    = {sizeof(ocudu_dapp_worker_api_v1),
                                                                     OCUDU_DAPP_ABI_MAJOR_V1,
                                                                     OCUDU_DAPP_ABI_MINOR_V1,
                                                                     create_worker,
                                                                     destroy_worker,
                                                                     {}};
const ocudu_dapp_srs_channel_estimator_interface_v1 srs_estimator = {worker_api, invoke, {}};
/// Two rx ports, one tx port, comb 4 only.
const ocudu_dapp_srs_profile_v1          profile   = {sizeof(ocudu_dapp_srs_profile_v1),
                                                      OCUDU_DAPP_ABI_MAJOR_V1,
                                                      OCUDU_DAPP_ABI_MINOR_V1,
                                                      OCUDU_DAPP_SRS_PROFILE_V1,
                                                      2,
                                                      1,
                                                      OCUDU_DAPP_SRS_MAX_SEQUENCE_LENGTH_V1,
                                                      OCUDU_DAPP_SRS_COMB_4_V1,
                                                      0,
                                                      {}};
const ocudu_dapp_interface_descriptor_v1 interface = {sizeof(ocudu_dapp_interface_descriptor_v1),
                                                      OCUDU_DAPP_ABI_MAJOR_V1,
                                                      OCUDU_DAPP_ABI_MINOR_V1,
                                                      OCUDU_DAPP_INTERFACE_SRS_CHANNEL_ESTIMATOR_V1,
                                                      1,
                                                      0,
                                                      OCUDU_DAPP_CLASS_A_INLINE_V1,
                                                      1U << (OCUDU_DAPP_PLACEMENT_IN_PROCESS_V1 - 1),
                                                      OCUDU_DAPP_THREADING_PER_WORKER_V1,
                                                      0,
                                                      &srs_estimator,
                                                      0,
                                                      &profile,
                                                      {}};

} // namespace

extern "C" OCUDU_DAPP_EXPORT_V1 ocudu_dapp_status_v1 ocudu_dapp_query_v1(const ocudu_dapp_host_info_v1*,
                                                                         ocudu_dapp_module_v1* module)
{
  populate_valid_module(module);
  module->global_name    = "org.deepsig.ocudu.test.class-a-srs";
  module->nof_interfaces = 1;
  module->interfaces     = &interface;
  return OCUDU_DAPP_OK_V1;
}
