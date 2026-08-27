// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

/// \file
/// \brief Runtime decision helpers for implementation-specific PUSCH acceleration paths.

#pragma once

#include "../../phy_acceleration_runtime_options.h"

namespace ocudu {

inline bool pusch_acceleration_device_uci_enabled()
{
  return phy_acceleration_env_flag_enabled("OCUDU_PUSCH_ENABLE_ACCELERATED_UCI");
}

} // namespace ocudu
