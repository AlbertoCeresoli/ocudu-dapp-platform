// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "dapp_service.h"
#include <yaml-cpp/yaml.h>

namespace ocudu::app_services {

/// Writes only operator-controlled fields; DU topology is always derived at construction.
void fill_dapp_service_config_in_yaml_schema(YAML::Node& node, const std::optional<dapp_service_config>& config);

} // namespace ocudu::app_services
