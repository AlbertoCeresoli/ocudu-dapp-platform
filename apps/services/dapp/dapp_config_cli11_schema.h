// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "dapp_service.h"
#include "CLI/CLI11.hpp"

namespace ocudu::app_services {

/// Adds the optional dApp service section to an application configuration.
void configure_cli11_with_dapp_service_config_schema(CLI::App& app, std::optional<dapp_service_config>& config);

/// Removes optionals whose YAML/CLI sections were absent after parsing.
void autoderive_dapp_service_config_after_parsing(CLI::App& app, std::optional<dapp_service_config>& config);

} // namespace ocudu::app_services
