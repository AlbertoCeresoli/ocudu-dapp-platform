// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "fixture_common.h"

extern "C" OCUDU_DAPP_EXPORT_V1 ocudu_dapp_status_v1 ocudu_dapp_query_v1(const ocudu_dapp_host_info_v1*,
                                                                         ocudu_dapp_module_v1* module)
{
  module->struct_size = sizeof(*module);
  module->abi_major   = OCUDU_DAPP_ABI_MAJOR_V1;
  module->abi_minor   = OCUDU_DAPP_ABI_MINOR_V1;
  return OCUDU_DAPP_OK_V1;
}
