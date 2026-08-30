// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "ocudu/dapp/abi/v1/memory.h"
#include "ocudu/phy/upper/dapp/resource_grid_tensor_adapter.h"
#include <memory>

namespace ocudu {

class resource_grid_reader;
struct srs_estimator_configuration;

namespace dapp {

struct srs_channel_estimate_tensor_result {
  ocudu_dapp_status_v1      status{OCUDU_DAPP_INTERNAL_ERROR_V1};
  ocudu_dapp_tensor_view_v1 tensor{};
  bool                      grid_lease_started{};
  explicit                  operator bool() const noexcept { return status == OCUDU_DAPP_OK_V1; }
};

/// Per-producer-lane CUDA SRS frequency-response builder.
class srs_channel_estimate_tensor_adapter
{
public:
  virtual ~srs_channel_estimate_tensor_adapter() = default;

  virtual srs_channel_estimate_tensor_result make(const resource_grid_reader&            grid,
                                                  const srs_estimator_configuration&     config,
                                                  const accelerator_resource_grid_lease& lease) noexcept = 0;
};

std::unique_ptr<srs_channel_estimate_tensor_adapter> create_srs_channel_estimate_tensor_adapter() noexcept;

} // namespace dapp
} // namespace ocudu
