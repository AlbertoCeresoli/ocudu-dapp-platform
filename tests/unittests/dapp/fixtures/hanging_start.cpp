// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "fixture_common.h"
#include <thread>

namespace {

ocudu_dapp_status_v1 hang_start(void*)
{
  for (;;) {
    std::this_thread::yield();
  }
}

} // namespace

extern "C" OCUDU_DAPP_EXPORT_V1 ocudu_dapp_status_v1 ocudu_dapp_query_v1(const ocudu_dapp_host_info_v1*,
                                                                         ocudu_dapp_module_v1* module)
{
  populate_valid_module(module);
  module->start = hang_start;
  return OCUDU_DAPP_OK_V1;
}
