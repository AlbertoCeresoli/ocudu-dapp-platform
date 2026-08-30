// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "ocudu/phy/upper/diagnostics/phy_diagnostics.h"
#include "ocudu/phy/upper/signal_processors/srs/srs_estimator_factory.h"
#include <memory>

namespace ocudu {

/// Wraps an SRS estimator factory so every admitted estimate publishes the
/// per-subcarrier least-squares channel response (one diagnostic per receive
/// port / transmit port pair, TA phase ramp removed) recomputed from the
/// resource grid. Works over the host and CUDA estimators; the estimator result
/// is returned unchanged.
std::shared_ptr<srs_estimator_factory>
create_srs_diagnostic_decorator_factory(std::shared_ptr<srs_estimator_factory> factory,
                                        std::shared_ptr<phy_diagnostic_sink>   sink);

} // namespace ocudu
