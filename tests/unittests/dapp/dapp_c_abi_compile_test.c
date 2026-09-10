// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/abi/v1/module.h"
#include "ocudu/dapp/use_cases/v1/channel_estimator.h"
#include "ocudu/dapp/use_cases/v1/equalizer.h"
#include "ocudu/dapp/use_cases/v1/pusch_metadata.h"
#include "ocudu/dapp/use_cases/v1/receiver.h"
#include "ocudu/dapp/use_cases/v1/scheduler.h"
#include "ocudu/dapp/use_cases/v1/spectrum.h"
#include "ocudu/dapp/use_cases/v1/srs_isac.h"

struct legacy_receiver_metrics_v1 {
  uint32_t                  struct_size;
  uint16_t                  abi_major;
  uint16_t                  abi_minor;
  ocudu_dapp_tensor_view_v1 post_equalization_noise;
  uint64_t                  reserved[4];
};
_Static_assert(offsetof(ocudu_dapp_receiver_metrics_v1, equalized_symbols) == sizeof(struct legacy_receiver_metrics_v1),
               "receiver symbol output must preserve the old metrics prefix");
_Static_assert(OCUDU_DAPP_RECEIVER_EQUALIZED_SYMBOLS_MIN_SIZE_V1 == sizeof(ocudu_dapp_receiver_metrics_v1),
               "symbol accessor must require the complete optional tensor");

uint32_t ocudu_dapp_c_abi_compile_test(void)
{
  return (uint32_t)(sizeof(ocudu_dapp_module_v1) + sizeof(ocudu_dapp_management_v1) +
                    sizeof(ocudu_dapp_config_update_v1) + sizeof(ocudu_dapp_spectrum_interface_v1) +
                    sizeof(ocudu_dapp_srs_isac_interface_v1) + sizeof(ocudu_dapp_channel_estimator_interface_v1) +
                    sizeof(ocudu_dapp_equalizer_interface_v1) + sizeof(ocudu_dapp_receiver_interface_v1) +
                    sizeof(ocudu_dapp_scheduler_interface_v1) + sizeof(ocudu_dapp_pusch_metadata_v1) +
                    sizeof(ocudu_dapp_dmrs_metadata_v1) + sizeof(ocudu_dapp_spectrum_slot_metadata_v1) +
                    sizeof(ocudu_dapp_srs_isac_metadata_v1));
}
