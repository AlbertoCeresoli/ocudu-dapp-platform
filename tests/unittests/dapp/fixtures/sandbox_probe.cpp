// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "fixture_common.h"
#include <cstdlib>
#include <fcntl.h>

extern "C" OCUDU_DAPP_EXPORT_V1 ocudu_dapp_status_v1 ocudu_dapp_query_v1(const ocudu_dapp_host_info_v1*,
                                                                         ocudu_dapp_module_v1* module)
{
  if (std::getenv("OCUDU_PREFLIGHT_TEST_SECRET") != nullptr) {
    return OCUDU_DAPP_INVALID_INPUT_V1;
  }
  for (int descriptor = 4; descriptor != 256; ++descriptor) {
    if (::fcntl(descriptor, F_GETFD) >= 0) {
      return OCUDU_DAPP_INVALID_INPUT_V1;
    }
  }
  populate_valid_module(module);
  return OCUDU_DAPP_OK_V1;
}
