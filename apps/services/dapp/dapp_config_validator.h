// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

namespace ocudu::app_services {

struct dapp_service_config;

/// Validates operator-controlled fields before topology is derived by the DU factory.
bool validate_dapp_service_config(const dapp_service_config& config);

} // namespace ocudu::app_services
