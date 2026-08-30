// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "upper_phy_dapp_data_router.h"
#include "ocudu/dapp/ipc/class_c_publication_encoder.h"
#include "ocudu/dapp/runtime/class_c_process_endpoint.h"
#include <memory>
#include <string>

namespace ocudu {

struct upper_phy_dapp_process_target_config {
  uint32_t                          interface_id{};
  uint32_t                          backend_id{OCUDU_DAPP_BACKEND_HOST_V1};
  uint32_t                          route_flags{};
  uint32_t                          slot_count{};
  uint32_t                          payload_capacity{};
  dapp::ipc::class_c_encoder_config encoder;
};

/// One host-owned SPSC publication endpoint for an isolated Class-C worker.
class upper_phy_dapp_process_target : public upper_phy_dapp_publication_target, public dapp::class_c_process_endpoint
{
public:
  void                          set_active(bool active) noexcept override     = 0;
  dapp::ipc::route_capabilities export_capabilities() const noexcept override = 0;
  /// Requires external proof that the prior worker process is dead and the target is inactive.
  bool                                   reset_after_consumer_restart() noexcept override = 0;
  dapp::class_c_process_endpoint_metrics snapshot_metrics() const noexcept override       = 0;
};

struct upper_phy_dapp_process_target_create_result {
  ocudu_dapp_status_v1                           status{OCUDU_DAPP_INTERNAL_ERROR_V1};
  std::string                                    detail;
  std::shared_ptr<upper_phy_dapp_process_target> target;
  explicit operator bool() const noexcept { return status == OCUDU_DAPP_OK_V1 && target != nullptr; }
};

upper_phy_dapp_process_target_create_result
create_upper_phy_dapp_process_target(upper_phy_dapp_process_target_config config) noexcept;

} // namespace ocudu
