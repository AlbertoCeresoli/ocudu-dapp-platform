// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include <chrono>
#include <cstdint>
#include <memory>

namespace ocudu {

class dmrs_pusch_estimator_factory;
class phy_tap;
class pusch_dapp_channel_estimator_target;
class pusch_demodulator_factory;
class srs_estimator_factory;

struct upper_phy_dapp_producer_context {
  uint32_t cell_id;
  uint32_t producer_shard;
  /// Stable concurrent execution lane within the producer shard.
  uint32_t worker_lane = 0;
};

struct upper_phy_dapp_pusch_factory_config {
  /// Includes the regular and UCI-only concurrent dependency pools.
  unsigned nof_lanes;
  unsigned max_nof_prb;
  unsigned max_nof_rx_ports;
  unsigned max_nof_layers;
  /// Soft CUDA completion wall. The observed interval includes same-stream host consumer work.
  std::chrono::nanoseconds channel_estimator_deadline{std::chrono::microseconds(800)};
  std::chrono::nanoseconds demodulator_deadline{std::chrono::microseconds(2000)};
};

/// Optional construction-time integration between upper-PHY factories and the runtime dApp hook registry.
class upper_phy_dapp_hook_provider
{
public:
  virtual ~upper_phy_dapp_hook_provider() = default;

  /// Decorates each PUSCH channel-estimator lane while retaining the supplied conventional fallback.
  virtual std::shared_ptr<dmrs_pusch_estimator_factory>
  decorate_pusch_channel_estimator(std::shared_ptr<dmrs_pusch_estimator_factory> fallback,
                                   upper_phy_dapp_pusch_factory_config           config) = 0;

  /// Decorates each PUSCH demodulator lane for CE+EQ or receiver hooks while retaining conventional fallback.
  virtual std::shared_ptr<pusch_demodulator_factory>
  decorate_pusch_demodulator(std::shared_ptr<pusch_demodulator_factory> fallback,
                             upper_phy_dapp_pusch_factory_config        config) = 0;

  /// \brief Decorates every concurrent SRS estimator lane with the dApp SRS hooks.
  ///
  /// The Class-A SRS channel estimator, when present, replaces \p fallback for the occasions its module serves; the
  /// Class-C SRS publication then observes whichever estimate was produced. \p max_nof_prb is the uplink bandwidth.
  virtual std::shared_ptr<srs_estimator_factory>
  decorate_srs_estimator(std::shared_ptr<srs_estimator_factory> fallback, unsigned nof_lanes, unsigned max_nof_prb) = 0;

  /// Wraps one per-cell/per-shard legacy PHY tap with the dApp resource-grid publication adapter.
  virtual std::unique_ptr<phy_tap> wrap_phy_tap(upper_phy_dapp_producer_context context,
                                                std::unique_ptr<phy_tap>        fallback) = 0;
};

} // namespace ocudu
