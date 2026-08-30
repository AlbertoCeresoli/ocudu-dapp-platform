// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/abi/v1/module.h"
#include <unistd.h>

extern "C" OCUDU_DAPP_EXPORT_V1 ocudu_dapp_status_v1 ocudu_dapp_query_v1(const ocudu_dapp_host_info_v1*,
                                                                         ocudu_dapp_module_v1*)
{
  while (true) {
    ::pause();
  }
}
