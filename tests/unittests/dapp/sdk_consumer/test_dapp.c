// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/abi/v1/module.h"
#include "ocudu/dapp/process/v1/control.h"
#include "ocudu/dapp/process/v1/publication_record.h"
#include "ocudu/dapp/process/v1/shared_ring.h"
#include "ocudu/dapp/use_cases/v1/pusch_metadata.h"
#include "ocudu/dapp/use_cases/v1/spectrum.h"

#if OCUDU_DAPP_SHARED_RING_HEADER_BYTES_V1 != 128
#error "Unexpected Class-C shared ring header size"
#endif

OCUDU_DAPP_EXPORT_V1 ocudu_dapp_status_v1 ocudu_dapp_query_v1(const ocudu_dapp_host_info_v1* host,
                                                              ocudu_dapp_module_v1*          module_out)
{
  (void)host;
  (void)module_out;
  return OCUDU_DAPP_UNSUPPORTED_V1;
}
