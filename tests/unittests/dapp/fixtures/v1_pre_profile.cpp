// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/abi/v1/module.h"
#include "ocudu/dapp/use_cases/v1/spectrum.h"
#include <cstddef>

namespace {

static_assert(sizeof(ocudu_dapp_interface_descriptor_v1) == 104);
static_assert(offsetof(ocudu_dapp_interface_descriptor_v1, reserved) == 48);
static_assert(sizeof(ocudu_dapp_module_v1) == 248);
static_assert(offsetof(ocudu_dapp_module_v1, management) == 120);

struct instance_state {};

ocudu_dapp_status_v1 create(const ocudu_dapp_host_api_v1*, const void*, void** output) noexcept
{
  static instance_state instance;
  if (output == nullptr) {
    return OCUDU_DAPP_INVALID_INPUT_V1;
  }
  *output = &instance;
  return OCUDU_DAPP_OK_V1;
}

ocudu_dapp_status_v1 configure(void*, const void*) noexcept
{
  return OCUDU_DAPP_OK_V1;
}

ocudu_dapp_status_v1 prepare(void*, const void*) noexcept
{
  return OCUDU_DAPP_OK_V1;
}

ocudu_dapp_status_v1 warmup(void*, const void*) noexcept
{
  return OCUDU_DAPP_OK_V1;
}

ocudu_dapp_status_v1 start(void*) noexcept
{
  return OCUDU_DAPP_OK_V1;
}

ocudu_dapp_status_v1 stop(void*) noexcept
{
  return OCUDU_DAPP_OK_V1;
}

ocudu_dapp_status_v1 quiesce(void*, uint64_t) noexcept
{
  return OCUDU_DAPP_OK_V1;
}

void destroy(void*) noexcept {}

ocudu_dapp_status_v1 create_worker(void* instance, uint32_t, void** output) noexcept
{
  if (instance == nullptr || output == nullptr) {
    return OCUDU_DAPP_INVALID_INPUT_V1;
  }
  *output = instance;
  return OCUDU_DAPP_OK_V1;
}

void destroy_worker(void*, void*) noexcept {}

ocudu_dapp_status_v1 publish(void*, const ocudu_dapp_spectrum_input_v1*) noexcept
{
  return OCUDU_DAPP_INVALID_INPUT_V1;
}

const ocudu_dapp_spectrum_interface_v1 spectrum_api = {{sizeof(ocudu_dapp_worker_api_v1),
                                                        OCUDU_DAPP_ABI_MAJOR_V1,
                                                        OCUDU_DAPP_ABI_MINOR_V1,
                                                        create_worker,
                                                        destroy_worker,
                                                        {}},
                                                       publish,
                                                       nullptr,
                                                       {}};

const ocudu_dapp_interface_descriptor_v1 spectrum_descriptor = {sizeof(ocudu_dapp_interface_descriptor_v1),
                                                                OCUDU_DAPP_ABI_MAJOR_V1,
                                                                OCUDU_DAPP_ABI_MINOR_V1,
                                                                OCUDU_DAPP_INTERFACE_SPECTRUM_V1,
                                                                1,
                                                                0,
                                                                OCUDU_DAPP_CLASS_C_OPEN_LOOP_V1,
                                                                1U << (OCUDU_DAPP_PLACEMENT_IN_PROCESS_V1 - 1U),
                                                                OCUDU_DAPP_THREADING_PER_WORKER_V1,
                                                                0,
                                                                &spectrum_api,
                                                                0,
                                                                {}};

} // namespace

extern "C" OCUDU_DAPP_EXPORT_V1 ocudu_dapp_status_v1 ocudu_dapp_query_v1(const ocudu_dapp_host_info_v1* host,
                                                                         ocudu_dapp_module_v1*          module)
{
  if (host == nullptr || module == nullptr || host->abi_major != OCUDU_DAPP_ABI_MAJOR_V1 ||
      module->struct_size < sizeof(*module)) {
    return OCUDU_DAPP_INVALID_INPUT_V1;
  }
  *module                = {};
  module->struct_size    = sizeof(*module);
  module->abi_major      = OCUDU_DAPP_ABI_MAJOR_V1;
  module->abi_minor      = OCUDU_DAPP_ABI_MINOR_V1;
  module->global_name    = "org.deepsig.ocudu.test.v1-pre-profile";
  module->vendor         = "DeepSig";
  module->version        = "1.0.0-pre-profile";
  module->nof_interfaces = 1;
  module->interfaces     = &spectrum_descriptor;
  module->create         = create;
  module->configure      = configure;
  module->prepare        = prepare;
  module->warmup         = warmup;
  module->start          = start;
  module->stop           = stop;
  module->quiesce        = quiesce;
  module->destroy        = destroy;
  return OCUDU_DAPP_OK_V1;
}
