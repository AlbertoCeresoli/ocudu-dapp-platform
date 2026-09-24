// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "upper_phy_dapp_hook_provider.h"
#include "ocudu/adt/span.h"
#include "ocudu/dapp/use_cases/v1/spectrum.h"
#include "ocudu/dapp/use_cases/v1/srs_isac.h"
#include "ocudu/phy/upper/dapp/resource_grid_tensor_adapter.h"
#include <memory>
#include <string>
#include <vector>

namespace ocudu {

class resource_grid_reader;
class pusch_dapp_channel_estimator_target;
class pusch_dapp_demodulator_target;
class srs_dapp_channel_estimator_target;
struct srs_estimator_configuration;
struct srs_estimator_result;
class upper_phy_dapp_hook_provider;
class upper_phy_dapp_data_router;
class slot_point;

namespace dapp {
class native_hook_point;
}

inline constexpr uint32_t upper_phy_dapp_max_route_bindings = 4096;

/// Coarse committed UL-use plan accompanying one received slot. Only complete plans are exported to dApps.
struct upper_phy_dapp_expected_ul_plan {
  span<const uint8_t> cells;
  uint32_t            nof_symbols{};
  uint32_t            nof_prbs{};
  bool                complete{};
};

/// One already-admitted destination for a Class-C producer product.
class upper_phy_dapp_publication_target
{
public:
  virtual ~upper_phy_dapp_publication_target() = default;

  virtual uint32_t interface_id() const noexcept = 0;
  virtual bool     active() const noexcept       = 0;
  /// Cheap per-symbol gate check: may report true transiently while the target is
  /// binding or draining. Publish-time admission still uses active(). Default
  /// falls back to active() for implementations without a cheaper signal.
  virtual bool bound() const noexcept { return active(); }
  /// True when this target requires the spectrum grid in its selected accelerator domain.
  virtual bool uses_accelerator_spectrum() const noexcept { return false; }
  /// Publishes a spectrum grid on a target-owned execution lane. The target records grid completion before returning.
  virtual void publish_accelerator_spectrum(const ocudu_dapp_spectrum_input_v1&,
                                            const resource_grid_reader&,
                                            const dapp::resource_grid_lease&) noexcept
  {
  }
  /// True when this target requires SRS channel estimates in its selected accelerator domain.
  virtual bool uses_accelerator_srs() const noexcept { return false; }
  /// Derives and publishes SRS channel estimates on a target-owned execution lane.
  virtual void publish_accelerator_srs(const ocudu_dapp_srs_isac_input_v1&,
                                       const resource_grid_reader&,
                                       const srs_estimator_configuration&,
                                       const dapp::resource_grid_lease&) noexcept
  {
  }
  virtual void publish(const ocudu_dapp_spectrum_input_v1&) noexcept {}
  virtual void publish(const ocudu_dapp_ul_resource_plan_v1&) noexcept {}
  virtual void publish(const ocudu_dapp_srs_isac_input_v1&) noexcept {}
};

struct upper_phy_dapp_route_binding {
  upper_phy_dapp_producer_context                    producer;
  std::shared_ptr<upper_phy_dapp_publication_target> target;
  /// SRS estimator pools reuse one concurrency lane across cells and resource-grid shards.
  /// When set, this binding is the fallback for every SRS producer using producer.worker_lane.
  bool all_srs_producers_on_worker_lane = false;
};

struct upper_phy_dapp_data_router_create_result {
  ocudu_dapp_status_v1                        status{OCUDU_DAPP_INTERNAL_ERROR_V1};
  std::string                                 detail;
  std::shared_ptr<upper_phy_dapp_data_router> router;
  explicit operator bool() const noexcept { return status == OCUDU_DAPP_OK_V1 && router != nullptr; }
};

/// Placement-neutral sink for upper-PHY observation products.
class upper_phy_dapp_data_router
{
public:
  virtual ~upper_phy_dapp_data_router() = default;

  /// Fast subscription predicate. Implementations must not allocate, block or throw.
  virtual bool is_enabled(uint32_t interface_id, upper_phy_dapp_producer_context context) const noexcept = 0;

  /// Publishes one complete received uplink slot. Implementations preserve the caller's grid lifetime.
  virtual void publish_spectrum(upper_phy_dapp_producer_context context,
                                const resource_grid_reader&     grid,
                                slot_point                      slot,
                                upper_phy_dapp_expected_ul_plan plan) noexcept = 0;

  /// Publishes one completed SRS observation without changing the conventional estimator result.
  virtual void publish_srs_isac(upper_phy_dapp_producer_context    context,
                                const resource_grid_reader&        grid,
                                const srs_estimator_configuration& config,
                                const srs_estimator_result&        result) noexcept = 0;
};

/// Creates the concrete OCUDU factory decorator used by DU-low construction.
/// \param srs_channel_estimator Class-A SRS channel-estimator target; it takes one of the \p nof_srs_lanes lanes
///                              per SRS estimator, like the Class-C publication.
/// \param nof_srs_lanes Number of SRS publication lanes registered with the router. The SRS
///                       estimator decorator hands out one lane per estimator instance, so this
///                       bounds how many estimators the upper PHY may build. It is not the SRS
///                       executor concurrency: the upper PHY only inserts a pooling SRS factory
///                       when concurrency > 1, and at concurrency 1 it builds one estimator per
///                       uplink resource grid.
std::shared_ptr<upper_phy_dapp_hook_provider>
create_upper_phy_dapp_hook_provider(std::shared_ptr<upper_phy_dapp_data_router>          router,
                                    std::shared_ptr<pusch_dapp_channel_estimator_target> channel_estimator     = {},
                                    std::shared_ptr<pusch_dapp_demodulator_target>       demodulator           = {},
                                    uint32_t                                             nof_srs_lanes         = 0,
                                    std::shared_ptr<srs_dapp_channel_estimator_target>   srs_channel_estimator = {});

/// Builds an immutable, bounded fanout table and preallocates all SRS reference scratch.
upper_phy_dapp_data_router_create_result
create_upper_phy_dapp_data_router(std::vector<upper_phy_dapp_route_binding> bindings) noexcept;

/// Adapts one activated native hook lane to the placement-neutral publication target.
std::shared_ptr<upper_phy_dapp_publication_target>
create_native_upper_phy_dapp_target(std::shared_ptr<dapp::native_hook_point> point, uint32_t lane) noexcept;

} // namespace ocudu
