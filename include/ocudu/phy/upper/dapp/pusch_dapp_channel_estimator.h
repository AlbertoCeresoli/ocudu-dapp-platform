// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "ocudu/dapp/runtime/native_completion_barrier.h"
#include "ocudu/dapp/runtime/native_worker.h"
#include "ocudu/dapp/runtime/pusch_route_table.h"
#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

namespace ocudu {

class dmrs_pusch_estimator_factory;
class low_papr_sequence_generator_factory;
class pseudo_random_generator_factory;

namespace dapp {
class native_hook_point;
}

/// Pre-bound Class-A channel-estimator target. Each lane is entered by at most one PUSCH dependency object.
class pusch_dapp_channel_estimator_target
{
public:
  virtual ~pusch_dapp_channel_estimator_target() = default;

  virtual bool     active() const noexcept                 = 0;
  virtual uint32_t lane_count() const noexcept             = 0;
  virtual uint32_t worker_id(uint32_t lane) const noexcept = 0;
  virtual uint32_t backend_id() const noexcept { return OCUDU_DAPP_BACKEND_HOST_V1; }
  virtual bool     set_completion_barrier(uint32_t, std::shared_ptr<dapp::native_completion_barrier>) noexcept
  {
    return true;
  }
  virtual dapp::native_invocation_result invoke(uint32_t                                     lane,
                                                const ocudu_dapp_channel_estimator_input_v1& input,
                                                ocudu_dapp_channel_estimator_output_v1&      output) noexcept = 0;
  virtual void                           report_invalid_output(uint32_t, const ocudu_dapp_invocation_v1&) noexcept {}
  virtual void report_completion_deadline(uint32_t, const ocudu_dapp_invocation_v1&, uint64_t, uint64_t) noexcept {}
  virtual void report_fallback(uint32_t, dapp::class_a_fallback_reason) noexcept {}

  /// Per-UE routing (see dapp::pusch_route_table): the route of one grant. A target without a route
  /// table always answers the primary route.
  virtual dapp::pusch_route resolve_route(uint32_t /*cell_index*/, uint16_t /*rnti*/) const noexcept
  {
    return dapp::pusch_route::primary();
  }
  /// The target serving a grant on \p route. The primary route is this target; a route to another
  /// hook or to the stock receiver is an inactive target, so the grant keeps the conventional estimator.
  virtual pusch_dapp_channel_estimator_target& route(const dapp::pusch_route& /*route*/) noexcept { return *this; }
};

struct pusch_dapp_channel_estimator_config {
  /// Total number of dependency objects, including the regular and UCI-only PUSCH pools.
  unsigned                 nof_lanes;
  unsigned                 max_nof_prb;
  unsigned                 max_nof_rx_ports;
  unsigned                 max_nof_layers;
  std::chrono::nanoseconds deadline{std::chrono::microseconds(100)};
  float                    noise_variance_floor{1.0e-6F};
};

/// Creates a CPU Class-A adapter with one fixed lane and preallocated workspace per estimator object.
std::shared_ptr<dmrs_pusch_estimator_factory>
create_pusch_dapp_channel_estimator_factory(std::shared_ptr<dmrs_pusch_estimator_factory>        fallback,
                                            std::shared_ptr<pusch_dapp_channel_estimator_target> target,
                                            std::shared_ptr<pseudo_random_generator_factory>     prg_factory,
                                            std::shared_ptr<low_papr_sequence_generator_factory> low_papr_factory,
                                            pusch_dapp_channel_estimator_config                  config) noexcept;

/// Adapts an activated native hook point to the upper-PHY channel-estimator target.
std::shared_ptr<pusch_dapp_channel_estimator_target>
create_native_pusch_dapp_channel_estimator_target(std::shared_ptr<dapp::native_hook_point> point) noexcept;

/// Adapts the route-table slots of the channel-estimator hook (slot order, slot 0 first) to one
/// routed target: the primary route is slot 0 and a routed UE is served by its instance's slot.
std::shared_ptr<pusch_dapp_channel_estimator_target>
create_native_pusch_dapp_channel_estimator_target(std::vector<std::shared_ptr<dapp::native_hook_point>> slots,
                                                  std::shared_ptr<const dapp::pusch_route_table>        table) noexcept;

} // namespace ocudu
