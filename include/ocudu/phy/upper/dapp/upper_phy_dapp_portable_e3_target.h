// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "upper_phy_dapp_data_router.h"
#include "ocudu/dapp/e3/portable_data_store.h"
#include "ocudu/dapp/ipc/class_c_publication_encoder.h"
#include <memory>
#include <string>

namespace ocudu {

struct upper_phy_dapp_portable_e3_target_config {
  uint32_t                                       interface_id{};
  uint64_t                                       route_epoch{};
  dapp::ipc::class_c_encoder_config              encoder;
  std::shared_ptr<dapp::e3::portable_data_store> store;
};

struct upper_phy_dapp_portable_e3_target_create_result {
  ocudu_dapp_status_v1                               status{OCUDU_DAPP_INTERNAL_ERROR_V1};
  std::string                                        detail;
  std::shared_ptr<upper_phy_dapp_publication_target> target;
  explicit operator bool() const noexcept { return status == OCUDU_DAPP_OK_V1 && target != nullptr; }
};

/// Creates a host-snapshot Class-C target for the bounded portable E3 payload profile.
upper_phy_dapp_portable_e3_target_create_result
create_upper_phy_dapp_portable_e3_target(upper_phy_dapp_portable_e3_target_config config) noexcept;

} // namespace ocudu
