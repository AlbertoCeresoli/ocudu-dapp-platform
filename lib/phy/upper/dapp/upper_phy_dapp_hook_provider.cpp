// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/use_cases/v1/spectrum.h"
#include "ocudu/dapp/use_cases/v1/srs_isac.h"
#include "ocudu/phy/support/resource_grid_reader.h"
#include "ocudu/phy/upper/channel_processors/pusch/factories.h"
#include "ocudu/phy/upper/dapp/pusch_dapp_channel_estimator.h"
#include "ocudu/phy/upper/dapp/pusch_dapp_demodulator.h"
#include "ocudu/phy/upper/dapp/srs_dapp_channel_estimator.h"
#include "ocudu/phy/upper/dapp/upper_phy_dapp_data_router.h"
#include "ocudu/phy/upper/phy_tap/phy_tap.h"
#include "ocudu/phy/upper/sequence_generators/sequence_generator_factories.h"
#include "ocudu/phy/upper/signal_processors/pusch/factories.h"
#include "ocudu/phy/upper/signal_processors/srs/srs_estimator_configuration.h"
#include "ocudu/phy/upper/signal_processors/srs/srs_estimator_factory.h"
#include "ocudu/phy/upper/signal_processors/srs/srs_estimator_result.h"
#include "ocudu/ran/resource_block.h"
#include "ocudu/ran/srs/srs_information.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <variant>

using namespace ocudu;

namespace {

class spectrum_tap final : public phy_tap
{
public:
  spectrum_tap(upper_phy_dapp_producer_context             context_,
               std::shared_ptr<upper_phy_dapp_data_router> router_,
               std::unique_ptr<phy_tap>                    fallback_) :
    context(context_), router(std::move(router_)), fallback(std::move(fallback_))
  {
  }

  void handle_ul_symbol(resource_grid_writer&                                     grid_writer,
                        const resource_grid_reader&                               grid_reader,
                        slot_point                                                slot,
                        unsigned                                                  symbol,
                        span<const uplink_pdu_slot_repository::pusch_pdu>         pusch_pdus,
                        span<const uplink_pdu_slot_repository::pucch_pdu>         pucch_pdus,
                        span<const pucch_processor::format1_common_configuration> pucch_f1_pdus,
                        span<const uplink_pdu_slot_repository::srs_pdu>           srs_pdus) override
  {
    // Stage the host snapshot only when somebody will actually read the grid: a
    // wrapped third-party tap, or an active spectrum route. Staging a CUDA-visible
    // grid takes the grid-ready mutex and can spin on device work, so it must not
    // run on the UL-PHY thread when no consumer exists.
    const bool route_active = router->is_enabled(OCUDU_DAPP_INTERFACE_SPECTRUM_V1, context);
    if (fallback != nullptr || route_active) {
      (void)grid_reader.stage_owned_snapshot_from_host();
    }
    if (fallback) {
      fallback->handle_ul_symbol(
          grid_writer, grid_reader, slot, symbol, pusch_pdus, pucch_pdus, pucch_f1_pdus, srs_pdus);
    }
    if (!route_active) {
      tracking = false;
      return;
    }
    const unsigned nof_symbols = grid_reader.get_nof_symbols();
    if (!tracking || tracked_slot != slot) {
      begin_slot(grid_reader, slot);
    }
    if (symbol < 16) {
      observed_symbols |= uint16_t{1} << symbol;
    }
    for (const auto& pdu : pusch_pdus) {
      add_pusch(pdu.pdu);
    }
    for (const auto& pdu : pucch_pdus) {
      std::visit([this](const auto& config) { add_pucch(config); }, pdu.config);
    }
    for (const auto& config : pucch_f1_pdus) {
      add_pucch(config);
    }
    for (const auto& pdu : srs_pdus) {
      add_srs(pdu.config);
    }
    // Always publish once the last OFDM symbol of the UL slot arrives — even when
    // there are zero PUSCH/PUCCH/SRS PDUs (empty cell / noise-only sensing).
    // Plan is "complete" only if we observed every symbol of the slot.
    if (nof_symbols != 0 && symbol == nof_symbols - 1) {
      const uint16_t expected = nof_symbols >= 16 ? UINT16_MAX : static_cast<uint16_t>((1U << nof_symbols) - 1U);
      const bool     complete = plan_valid && observed_symbols == expected;
      router->publish_spectrum(
          context, grid_reader, slot, {{plan_cells.data(), plan_size}, plan_symbols, plan_prbs, complete});
      tracking = false;
    }
  }

  void handle_prach_window(prach_buffer& buffer, const prach_buffer_context& context_) override
  {
    if (fallback) {
      fallback->handle_prach_window(buffer, context_);
    }
  }

  // Path used when upper PHY has a receive-request but only quiet/noise content
  // (or alternative processor for slots without PDU repos). Still publishes a
  // full-empty expected-use plan so Class-C spectrum sees every UL slot.
  void handle_quiet_grid(const resource_grid_reader& grid_reader, slot_point slot) override
  {
    if (fallback) {
      fallback->handle_quiet_grid(grid_reader, slot);
    }
    if (router->is_enabled(OCUDU_DAPP_INTERFACE_SPECTRUM_V1, context)) {
      begin_slot(grid_reader, slot);
      router->publish_spectrum(
          context, grid_reader, slot, {{plan_cells.data(), plan_size}, plan_symbols, plan_prbs, plan_valid});
      tracking = false;
    }
  }

private:
  void begin_slot(const resource_grid_reader& grid, slot_point slot) noexcept
  {
    tracking         = true;
    tracked_slot     = slot;
    observed_symbols = 0;
    plan_symbols     = grid.get_nof_symbols();
    plan_prbs        = grid.get_nof_subc() / NOF_SUBCARRIERS_PER_RB;
    plan_valid       = plan_symbols != 0 && plan_symbols <= 14 && plan_prbs != 0 &&
                 plan_prbs <= OCUDU_DAPP_SPECTRUM_MAX_PRBS_V1 && grid.get_nof_subc() % NOF_SUBCARRIERS_PER_RB == 0;
    plan_size = plan_valid ? plan_symbols * plan_prbs : 0;
    std::fill_n(plan_cells.begin(), plan_size, static_cast<uint8_t>(OCUDU_DAPP_UL_USE_PLANNED_EMPTY_V1));
  }

  void mark(unsigned symbol, unsigned first_prb, unsigned nof_prbs, uint8_t category) noexcept
  {
    if (!plan_valid || symbol >= plan_symbols || first_prb >= plan_prbs) {
      return;
    }
    const unsigned stop = std::min(plan_prbs, first_prb + nof_prbs);
    for (unsigned prb = first_prb; prb != stop; ++prb) {
      uint8_t& current = plan_cells[symbol * plan_prbs + prb];
      if (current == OCUDU_DAPP_UL_USE_PLANNED_EMPTY_V1 || current == category) {
        current = category;
      } else if ((current == OCUDU_DAPP_UL_USE_PUSCH_V1 && category == OCUDU_DAPP_UL_USE_REFERENCE_SIGNAL_V1) ||
                 (current == OCUDU_DAPP_UL_USE_REFERENCE_SIGNAL_V1 && category == OCUDU_DAPP_UL_USE_PUSCH_V1)) {
        current = OCUDU_DAPP_UL_USE_REFERENCE_SIGNAL_V1;
      } else {
        current = OCUDU_DAPP_UL_USE_UNKNOWN_V1;
      }
    }
  }

  void add_pusch(const pusch_processor::pdu_t& config)
  {
    const auto prbs = config.freq_alloc.get_crb_indices(config.bwp_start_rb, config.bwp_size_rb);
    for (unsigned symbol = config.start_symbol_index; symbol != config.start_symbol_index + config.nof_symbols;
         ++symbol) {
      const uint8_t category =
          config.dmrs_symbol_mask.test(symbol) ? OCUDU_DAPP_UL_USE_REFERENCE_SIGNAL_V1 : OCUDU_DAPP_UL_USE_PUSCH_V1;
      for (uint16_t prb : prbs) {
        mark(symbol, prb, 1, category);
      }
    }
  }

  void add_pucch_allocation(unsigned                       bwp_start,
                            unsigned                       first_prb,
                            unsigned                       nof_prbs,
                            const std::optional<unsigned>& second_hop,
                            unsigned                       first_symbol,
                            unsigned                       nof_symbols) noexcept
  {
    const unsigned second_hop_start = first_symbol + nof_symbols / 2;
    for (unsigned symbol = first_symbol; symbol != first_symbol + nof_symbols; ++symbol) {
      const unsigned relative_prb = second_hop && symbol >= second_hop_start ? *second_hop : first_prb;
      mark(symbol, bwp_start + relative_prb, nof_prbs, OCUDU_DAPP_UL_USE_PUCCH_V1);
    }
  }

  void add_pucch(const pucch_processor::format0_configuration& config) noexcept
  {
    add_pucch_allocation(config.bwp_start_rb,
                         config.starting_prb,
                         1,
                         config.second_hop_prb,
                         config.start_symbol_index,
                         config.nof_symbols);
  }

  void add_pucch(const pucch_processor::format1_configuration& config) noexcept
  {
    add_pucch_allocation(config.bwp_start_rb,
                         config.starting_prb,
                         1,
                         config.second_hop_prb,
                         config.start_symbol_index,
                         config.nof_symbols);
  }

  void add_pucch(const pucch_processor::format1_common_configuration& config) noexcept
  {
    add_pucch_allocation(config.bwp_start_rb,
                         config.starting_prb,
                         1,
                         config.second_hop_prb,
                         config.start_symbol_index,
                         config.nof_symbols);
  }

  void add_pucch(const pucch_processor::format2_configuration& config) noexcept
  {
    add_pucch_allocation(config.bwp_start_rb,
                         config.prbs.start(),
                         config.prbs.length(),
                         config.second_hop_prb,
                         config.start_symbol_index,
                         config.nof_symbols);
  }

  void add_pucch(const pucch_processor::format3_configuration& config) noexcept
  {
    add_pucch_allocation(config.bwp_start_rb,
                         config.prbs.start(),
                         config.prbs.length(),
                         config.second_hop_prb,
                         config.start_symbol_index,
                         config.nof_symbols);
  }

  void add_pucch(const pucch_processor::format4_configuration& config) noexcept
  {
    add_pucch_allocation(config.bwp_start_rb,
                         config.starting_prb,
                         1,
                         config.second_hop_prb,
                         config.start_symbol_index,
                         config.nof_symbols);
  }

  void add_srs(const srs_estimator_configuration& config)
  {
    const srs_information info         = get_srs_information(config.resource, 0);
    const unsigned        first_symbol = config.resource.start_symbol.value();
    const unsigned        nof_symbols  = static_cast<unsigned>(config.resource.nof_symbols);
    const unsigned        first_prb    = info.mapping_initial_subcarrier / NOF_SUBCARRIERS_PER_RB;
    const unsigned        last_subcarrier =
        info.mapping_initial_subcarrier + (info.sequence_length == 0 ? 0 : (info.sequence_length - 1) * info.comb_size);
    const unsigned nof_prbs = last_subcarrier / NOF_SUBCARRIERS_PER_RB - first_prb + 1;
    for (unsigned symbol = first_symbol; symbol != first_symbol + nof_symbols; ++symbol) {
      mark(symbol, first_prb, nof_prbs, OCUDU_DAPP_UL_USE_SRS_V1);
    }
  }

  static constexpr std::size_t                max_plan_cells = 14 * OCUDU_DAPP_SPECTRUM_MAX_PRBS_V1;
  upper_phy_dapp_producer_context             context;
  std::shared_ptr<upper_phy_dapp_data_router> router;
  std::unique_ptr<phy_tap>                    fallback;
  std::array<uint8_t, max_plan_cells>         plan_cells{};
  slot_point                                  tracked_slot;
  uint32_t                                    plan_symbols{};
  uint32_t                                    plan_prbs{};
  uint32_t                                    plan_size{};
  uint16_t                                    observed_symbols{};
  bool                                        tracking{};
  bool                                        plan_valid{};
};

class srs_estimator_decorator final : public srs_estimator
{
public:
  srs_estimator_decorator(std::unique_ptr<srs_estimator>              fallback_,
                          std::shared_ptr<upper_phy_dapp_data_router> router_,
                          uint32_t                                    worker_lane_) :
    fallback(std::move(fallback_)), router(std::move(router_)), worker_lane(worker_lane_)
  {
  }

  srs_estimator_result estimate(const resource_grid_reader& grid, const srs_estimator_configuration& config) override
  {
    srs_estimator_result                  result = fallback->estimate(grid, config);
    const upper_phy_dapp_producer_context context{config.cell_id, config.producer_shard, worker_lane};
    if (router->is_enabled(OCUDU_DAPP_INTERFACE_SRS_ISAC_V1, context)) {
      router->publish_srs_isac(context, grid, config, result);
    }
    return result;
  }

private:
  std::unique_ptr<srs_estimator>              fallback;
  std::shared_ptr<upper_phy_dapp_data_router> router;
  uint32_t                                    worker_lane;
};

class srs_estimator_factory_decorator final : public srs_estimator_factory
{
public:
  srs_estimator_factory_decorator(std::shared_ptr<srs_estimator_factory>      fallback_,
                                  std::shared_ptr<upper_phy_dapp_data_router> router_,
                                  unsigned                                    nof_lanes_) :
    fallback(std::move(fallback_)), router(std::move(router_)), nof_lanes(nof_lanes_)
  {
  }

  std::unique_ptr<srs_estimator> create() override
  {
    const uint32_t lane = take_lane();
    if (lane >= nof_lanes) {
      return {};
    }
    auto estimator = fallback->create();
    return estimator ? std::make_unique<srs_estimator_decorator>(std::move(estimator), router, lane) : nullptr;
  }

  std::unique_ptr<srs_estimator> create(ocudulog::basic_logger& logger) override
  {
    const uint32_t lane = take_lane();
    if (lane >= nof_lanes) {
      return {};
    }
    auto estimator = fallback->create(logger);
    return estimator ? std::make_unique<srs_estimator_decorator>(std::move(estimator), router, lane) : nullptr;
  }

  std::unique_ptr<srs_estimator_configuration_validator> create_validator() override
  {
    return fallback->create_validator();
  }

private:
  uint32_t take_lane() noexcept { return next_lane.fetch_add(1, std::memory_order_relaxed); }

  std::shared_ptr<srs_estimator_factory>      fallback;
  std::shared_ptr<upper_phy_dapp_data_router> router;
  const uint32_t                              nof_lanes;
  std::atomic<uint32_t>                       next_lane{};
};

/// Host channel estimator only while no demodulator dApp serves the grant.
///
/// With routing, the pair (channel estimator, demodulator) is chosen per grant: route() returns a
/// prebuilt gate over the two targets' views of the same route, so a UE routed to a channel
/// estimator slot sees an inactive demodulator and the estimator runs, while a UE routed to an
/// equalizer or receiver keeps the conventional estimator.
class channel_estimator_target_gate final : public pusch_dapp_channel_estimator_target
{
public:
  channel_estimator_target_gate(std::shared_ptr<pusch_dapp_channel_estimator_target> channel_estimator_,
                                std::shared_ptr<pusch_dapp_demodulator_target>       demodulator_) :
    owned_channel_estimator(std::move(channel_estimator_)),
    owned_demodulator(std::move(demodulator_)),
    channel_estimator(*owned_channel_estimator),
    demodulator(*owned_demodulator)
  {
    views.reserve(nof_route_views);
    for (std::size_t index = 0; index != nof_route_views; ++index) {
      const auto selected = route_of(index);
      views.push_back(std::make_unique<channel_estimator_target_gate>(channel_estimator.route(selected),
                                                                      demodulator.route(selected)));
    }
  }

  channel_estimator_target_gate(pusch_dapp_channel_estimator_target& channel_estimator_,
                                pusch_dapp_demodulator_target&       demodulator_) :
    channel_estimator(channel_estimator_), demodulator(demodulator_)
  {
  }

  dapp::pusch_route resolve_route(uint32_t cell_index, uint16_t rnti) const noexcept override
  {
    return channel_estimator.resolve_route(cell_index, rnti);
  }
  pusch_dapp_channel_estimator_target& route(const dapp::pusch_route& selected) noexcept override
  {
    const std::size_t index = index_of(selected);
    return views.empty() || index >= views.size() ? *this : *views[index];
  }

  bool active() const noexcept override
  {
    return channel_estimator.active() && channel_estimator.backend_id() == OCUDU_DAPP_BACKEND_HOST_V1 &&
           demodulator.active_mode() == pusch_dapp_demodulator_mode::inactive;
  }
  uint32_t lane_count() const noexcept override { return channel_estimator.lane_count(); }
  uint32_t worker_id(uint32_t lane) const noexcept override { return channel_estimator.worker_id(lane); }
  uint32_t backend_id() const noexcept override { return channel_estimator.backend_id(); }
  bool set_completion_barrier(uint32_t lane, std::shared_ptr<dapp::native_completion_barrier> barrier) noexcept override
  {
    return channel_estimator.set_completion_barrier(lane, std::move(barrier));
  }

  dapp::native_invocation_result invoke(uint32_t                                     lane,
                                        const ocudu_dapp_channel_estimator_input_v1& input,
                                        ocudu_dapp_channel_estimator_output_v1&      output) noexcept override
  {
    return channel_estimator.invoke(lane, input, output);
  }

  void report_invalid_output(uint32_t lane, const ocudu_dapp_invocation_v1& invocation) noexcept override
  {
    channel_estimator.report_invalid_output(lane, invocation);
  }
  void report_completion_deadline(uint32_t                        lane,
                                  const ocudu_dapp_invocation_v1& invocation,
                                  uint64_t                        observed_latency_ns,
                                  uint64_t                        deadline_ns) noexcept override
  {
    channel_estimator.report_completion_deadline(lane, invocation, observed_latency_ns, deadline_ns);
  }
  void report_fallback(uint32_t lane, dapp::class_a_fallback_reason reason) noexcept override
  {
    channel_estimator.report_fallback(lane, reason);
  }

private:
  static constexpr std::size_t nof_route_views =
      2 + dapp::nof_pusch_route_hooks * dapp::pusch_route_table::max_slots_per_hook;

  static std::size_t index_of(const dapp::pusch_route& selected) noexcept
  {
    switch (selected.type) {
      case dapp::pusch_route::kind::primary:
        return 0;
      case dapp::pusch_route::kind::conventional:
        return 1;
      case dapp::pusch_route::kind::instance:
        break;
    }
    return 2 + static_cast<std::size_t>(selected.hook) * dapp::pusch_route_table::max_slots_per_hook + selected.slot;
  }
  static dapp::pusch_route route_of(std::size_t index) noexcept
  {
    if (index == 0) {
      return dapp::pusch_route::primary();
    }
    if (index == 1) {
      return dapp::pusch_route::conventional();
    }
    const std::size_t routed = index - 2;
    return dapp::pusch_route::instance(
        static_cast<dapp::pusch_route_hook>(routed / dapp::pusch_route_table::max_slots_per_hook),
        static_cast<uint8_t>(routed % dapp::pusch_route_table::max_slots_per_hook));
  }

  std::shared_ptr<pusch_dapp_channel_estimator_target>        owned_channel_estimator;
  std::shared_ptr<pusch_dapp_demodulator_target>              owned_demodulator;
  pusch_dapp_channel_estimator_target&                        channel_estimator;
  pusch_dapp_demodulator_target&                              demodulator;
  std::vector<std::unique_ptr<channel_estimator_target_gate>> views;
};

class upper_phy_dapp_hook_provider_impl final : public upper_phy_dapp_hook_provider
{
public:
  upper_phy_dapp_hook_provider_impl(std::shared_ptr<upper_phy_dapp_data_router>          router_,
                                    std::shared_ptr<pusch_dapp_channel_estimator_target> channel_estimator_,
                                    std::shared_ptr<pusch_dapp_demodulator_target>       demodulator_,
                                    uint32_t                                             nof_srs_lanes_,
                                    std::shared_ptr<srs_dapp_channel_estimator_target>   srs_channel_estimator_) :
    router(std::move(router_)),
    channel_estimator(std::move(channel_estimator_)),
    demodulator(std::move(demodulator_)),
    srs_channel_estimator(std::move(srs_channel_estimator_)),
    nof_srs_lanes(nof_srs_lanes_)
  {
  }

  std::shared_ptr<dmrs_pusch_estimator_factory>
  decorate_pusch_channel_estimator(std::shared_ptr<dmrs_pusch_estimator_factory> fallback,
                                   upper_phy_dapp_pusch_factory_config           config) override
  {
    if (!channel_estimator) {
      return fallback;
    }
    std::shared_ptr<pusch_dapp_channel_estimator_target> selected_target = channel_estimator;
    if (demodulator) {
      selected_target = std::make_shared<channel_estimator_target_gate>(selected_target, demodulator);
    }
    return create_pusch_dapp_channel_estimator_factory(std::move(fallback),
                                                       std::move(selected_target),
                                                       create_pseudo_random_generator_sw_factory(),
                                                       create_low_papr_sequence_generator_sw_factory(),
                                                       {.nof_lanes        = config.nof_lanes,
                                                        .max_nof_prb      = config.max_nof_prb,
                                                        .max_nof_rx_ports = config.max_nof_rx_ports,
                                                        .max_nof_layers   = config.max_nof_layers,
                                                        .deadline         = config.channel_estimator_deadline});
  }

  std::shared_ptr<pusch_demodulator_factory>
  decorate_pusch_demodulator(std::shared_ptr<pusch_demodulator_factory> fallback,
                             upper_phy_dapp_pusch_factory_config        config) override
  {
    if (!demodulator) {
      return fallback;
    }
    return create_pusch_dapp_demodulator_factory(std::move(fallback),
                                                 demodulator,
                                                 create_pseudo_random_generator_sw_factory(),
                                                 create_demodulation_mapper_factory(),
                                                 {.nof_lanes                  = config.nof_lanes,
                                                  .max_nof_prb                = config.max_nof_prb,
                                                  .max_nof_rx_ports           = config.max_nof_rx_ports,
                                                  .max_nof_layers             = config.max_nof_layers,
                                                  .demodulator_deadline       = config.demodulator_deadline,
                                                  .channel_estimator_deadline = config.channel_estimator_deadline},
                                                 channel_estimator);
  }

  std::shared_ptr<srs_estimator_factory> decorate_srs_estimator(std::shared_ptr<srs_estimator_factory> fallback,
                                                                unsigned                               nof_lanes,
                                                                unsigned max_nof_prb) override
  {
    // The caller only knows the SRS executor concurrency, which undercounts the estimators the
    // upper PHY builds when no pooling factory is inserted (concurrency 1). Every lane the
    // router registered is publishable, so take the larger of the two budgets.
    const unsigned lanes = std::max<unsigned>(nof_lanes, nof_srs_lanes);
    if (!fallback || (!router && !srs_channel_estimator)) {
      return fallback;
    }
    if (lanes == 0) {
      return nullptr;
    }
    if (srs_channel_estimator) {
      fallback = create_srs_dapp_channel_estimator_factory(
          std::move(fallback),
          srs_channel_estimator,
          create_low_papr_sequence_generator_sw_factory(),
          {.nof_lanes = srs_channel_estimator->lane_count(), .max_nof_prb = max_nof_prb});
      if (!fallback) {
        return nullptr;
      }
    }
    return router ? std::make_shared<srs_estimator_factory_decorator>(std::move(fallback), router, lanes) : fallback;
  }

  std::unique_ptr<phy_tap> wrap_phy_tap(upper_phy_dapp_producer_context context,
                                        std::unique_ptr<phy_tap>        fallback) override
  {
    if (!router) {
      return fallback;
    }
    return std::make_unique<spectrum_tap>(context, router, std::move(fallback));
  }

private:
  std::shared_ptr<upper_phy_dapp_data_router>          router;
  std::shared_ptr<pusch_dapp_channel_estimator_target> channel_estimator;
  std::shared_ptr<pusch_dapp_demodulator_target>       demodulator;
  std::shared_ptr<srs_dapp_channel_estimator_target>   srs_channel_estimator;
  uint32_t                                             nof_srs_lanes{};
};

} // namespace

std::shared_ptr<upper_phy_dapp_hook_provider>
ocudu::create_upper_phy_dapp_hook_provider(std::shared_ptr<upper_phy_dapp_data_router>          router,
                                           std::shared_ptr<pusch_dapp_channel_estimator_target> channel_estimator,
                                           std::shared_ptr<pusch_dapp_demodulator_target>       demodulator,
                                           uint32_t                                             nof_srs_lanes,
                                           std::shared_ptr<srs_dapp_channel_estimator_target>   srs_channel_estimator)
{
  return router || channel_estimator || demodulator || srs_channel_estimator
             ? std::make_shared<upper_phy_dapp_hook_provider_impl>(std::move(router),
                                                                   std::move(channel_estimator),
                                                                   std::move(demodulator),
                                                                   nof_srs_lanes,
                                                                   std::move(srs_channel_estimator))
             : nullptr;
}
