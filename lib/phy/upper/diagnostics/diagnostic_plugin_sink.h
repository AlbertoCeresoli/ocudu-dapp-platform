// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "ocudu/phy/upper/diagnostics/phy_diagnostics.h"
#include <memory>
#include <string>

namespace ocudu {

/// Creates a diagnostic sink from a runtime-loaded plugin.
std::shared_ptr<phy_diagnostic_sink> create_diagnostic_plugin_sink(const std::string& endpoint);

} // namespace ocudu
