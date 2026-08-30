// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "flexible_o_du_factory.h"
#ifdef OCUDU_FLEXIBLE_DU_DAPP
#include "apps/services/dapp/dapp_metrics_json_fanout.h"
#include "apps/services/dapp/dapp_service.h"
#include "ocudu/dapp/abi/v1/memory.h"
#include "ocudu/dapp/e3/management_service.h"
#include "ocudu/phy/upper/channel_processors/pusch/factories.h"
#include "ocudu/ran/band_helper.h"
#include "ocudu/ran/bs_channel_bandwidth.h"
#include "ocudu/ran/subcarrier_spacing.h"
#endif
#include "apps/helpers/e2/e2_metric_connector_manager.h"
#include "apps/helpers/ntn/ntn_config_translators.h"
#include "apps/services/worker_manager/worker_manager.h"
#include "apps/units/flexible_o_du/flexible_o_du_commands.h"
#include "apps/units/flexible_o_du/o_du_high/du_high/du_high_config_translators.h"
#include "apps/units/flexible_o_du/o_du_high/o_du_high_unit_factory.h"
#include "apps/units/flexible_o_du/o_du_low/o_du_low_unit_factory.h"
#include "commands/ntn_config_update_remote_command.h"
#include "flexible_o_du_impl.h"
#include "flexible_o_du_ntn_configuration_manager_factory.h"
#include "metrics/flexible_o_du_metrics_builder.h"
#include "ocudu/du/du_high/du_high.h"
#include "ocudu/du/du_high/du_high_clock_controller.h"
#include "ocudu/du/o_du_factory.h"
#include "ocudu/e2/e2_du_metrics_connector.h"
#include "ocudu/fapi_adaptor/mac/mac_fapi_fastpath_adaptor.h"
#include "ocudu/fapi_adaptor/mac/mac_fapi_sector_fastpath_adaptor.h"
#include "ocudu/fapi_adaptor/mac/p5/mac_fapi_p5_sector_fastpath_adaptor.h"
#include "ocudu/fapi_adaptor/mac/p7/mac_fapi_p7_sector_fastpath_adaptor.h"
#include "ocudu/fapi_adaptor/phy/p5/phy_fapi_p5_sector_adaptor.h"
#include "ocudu/fapi_adaptor/phy/p7/phy_fapi_p7_sector_fastpath_adaptor.h"
#include "ocudu/fapi_adaptor/phy/phy_fapi_fastpath_adaptor.h"
#include "ocudu/fapi_adaptor/phy/phy_fapi_sector_fastpath_adaptor.h"
#include "ocudu/ntn/ntn_configuration_manager_config.h"
#include "fmt/format.h"
#include <algorithm>
#include <numeric>

using namespace ocudu;

#ifdef OCUDU_FLEXIBLE_DU_DAPP
static app_services::dapp_service_config
derive_dapp_service_config(app_services::dapp_service_config               config,
                           span<const o_du_low_unit_config::du_low_config> low_cells,
                           span<const odu::du_cell_config>                 high_cells,
                           const upper_phy_execution_configuration&        execution,
                           bool                                            pusch_acceleration_enabled)
{
  config.cells.clear();
  config.cells.reserve(low_cells.size());
  for (uint32_t cell_id = 0; cell_id != low_cells.size(); ++cell_id) {
    const unsigned nof_slots_per_frame =
        get_nof_slots_per_subframe(low_cells[cell_id].scs_common) * NOF_SUBFRAMES_PER_FRAME;
    app_services::dapp_service_cell_config cell;
    cell.cell_id                    = cell_id;
    cell.nof_uplink_producer_shards = nof_slots_per_frame + 1;
    cell.nof_ul_prbs =
        static_cast<uint16_t>(high_cells[cell_id].ran.ul_cfg_common.init_ul_bwp.generic_params.crbs.length());
    // The uplink carrier center is the RF placement reported on E3 subscription
    // records; a single-cell RU window is centered on the cell, so the offset is zero.
    cell.rf_center_frequency_hz =
        static_cast<uint64_t>(band_helper::nr_arfcn_to_freq(high_cells[cell_id].ran.ul_carrier.arfcn_f_ref));
    config.cells.push_back(cell);
  }
  config.nof_pusch_lanes = 2 * execution.pusch_executor.max_concurrency;
  // One SRS estimator is created per uplink producer shard (nof_ul_rg plus the pool's
  // default processor), not per executor lane: the upper PHY only inserts a pooling SRS
  // factory when srs_executor.max_concurrency > 1, so at concurrency 1 the dApp
  // decorator is asked for one estimator per shard and a concurrency-sized lane budget
  // is exhausted after the first. Size the lanes by the shard count, and never below
  // executor concurrency.
  const uint32_t max_uplink_producer_shards =
      std::accumulate(config.cells.begin(), config.cells.end(), 0U, [](uint32_t acc, const auto& cell) {
        return std::max(acc, cell.nof_uplink_producer_shards);
      });
  config.nof_srs_lanes = std::max<uint32_t>(execution.srs_executor.max_concurrency, max_uplink_producer_shards);
  config.nof_scheduler_lanes =
      std::accumulate(high_cells.begin(), high_cells.end(), 0U, [](unsigned count, const odu::du_cell_config& cell) {
        return count + cell.rrm_policy_members.size() + 2;
      });
  config.backend_mask = uint64_t{1} << OCUDU_DAPP_BACKEND_HOST_V1;
  if (pusch_acceleration_enabled && is_pusch_demodulator_acceleration_available()) {
    config.backend_mask |= uint64_t{1} << OCUDU_DAPP_BACKEND_CUDA_V1;
  }
  return config;
}
#endif

static fapi::carrier_config generate_carrier_config_tlv(const odu::du_cell_config& du_cell)
{
  const subcarrier_spacing scs_common       = du_cell.ran.dl_cfg_common.init_dl_bwp.generic_params.scs;
  unsigned                 grid_size_bw_prb = band_helper::get_n_rbs_from_bw(
      du_cell.ran.dl_carrier.carrier_bw, scs_common, band_helper::get_freq_range(du_cell.ran.dl_carrier.band));

  fapi::carrier_config fapi_config = {};

  fapi_config.dl_grid_size = grid_size_bw_prb;
  fapi_config.ul_grid_size = grid_size_bw_prb;

  // Number of transmit and receive antenna ports.
  fapi_config.num_tx_ant = du_cell.ran.dl_carrier.nof_ant;
  fapi_config.num_rx_ant = du_cell.ran.ul_carrier.nof_ant;

  return fapi_config;
}

static o_du_low_unit_config generate_o_du_low_config(const du_low_unit_config&            du_low_unit_cfg,
                                                     float                                rx_gain_dB,
                                                     span<const odu::du_cell_config>      cells,
                                                     span<const du_high_unit_cell_config> du_hi_cells)
{
  o_du_low_unit_config odu_low_cfg = {du_low_unit_cfg, {}, {}};

  for (unsigned i = 0, e = cells.size(); i != e; ++i) {
    const auto&              cell       = cells[i];
    const auto&              du_hi_cell = du_hi_cells[i];
    const subcarrier_spacing scs_common = cell.ran.dl_cfg_common.init_dl_bwp.generic_params.scs;

    fapi_adaptor::phy_fapi_p5_sector_fastpath_adaptor_config p5_cfg{.sector_id = i};

    fapi_adaptor::phy_fapi_p7_sector_fastpath_adaptor_config p7_cfg = {
        .sector_id                     = i,
        .nof_slots_request_headroom    = du_low_unit_cfg.expert_phy_cfg.nof_slots_request_headroom,
        .allow_request_on_empty_ul_tti = du_low_unit_cfg.expert_phy_cfg.allow_request_on_empty_uplink_slot,
        .scs                           = scs_common,
        .scs_common                    = scs_common,
        .carrier_cfg                   = generate_carrier_config_tlv(cell),
        .prach_cfg                     = *cell.ran.ul_cfg_common.init_ul_bwp.rach_cfg_common,
        .prach_ports                   = du_hi_cell.cell.prach_cfg.ports,
        // The SDR Radio Unit gain is known to this application unit, so it is subtracted from the configured
        // '--dbfs_to_dbm_conversion_factor' here. For Open Fronthaul, the receive gain is applied by the externalis
        // applied (rx_gain_dB is 0).
        .dbfs_to_dbm_conversion_factor = du_low_unit_cfg.power_calibration.dbfs_to_dbm_conversion_factor - rx_gain_dB,
        .db_to_dbfs_conversion_factor  = du_low_unit_cfg.power_calibration.db_to_dbfs_conversion_factor};

    odu_low_cfg.fapi_cfg.sectors.push_back({.p5_config = p5_cfg, .p7_config = p7_cfg});

    report_error_if_not(cell.ran.ul_cfg_common.init_ul_bwp.rach_cfg_common,
                        "RACH configuration for the cell is not present");

    auto&   du_low_cell    = odu_low_cfg.cells.emplace_back();
    nr_band band           = cell.ran.dl_carrier.band;
    du_low_cell.duplex     = band_helper::get_duplex_mode(band);
    du_low_cell.freq_range = band_helper::get_freq_range(band);
    du_low_cell.bw_rb =
        band_helper::get_n_rbs_from_bw(cell.ran.dl_carrier.carrier_bw, scs_common, du_low_cell.freq_range);
    du_low_cell.nof_rx_antennas = cell.ran.ul_carrier.nof_ant;
    du_low_cell.nof_tx_antennas = cell.ran.dl_carrier.nof_ant;
    du_low_cell.prach_ports     = du_hi_cell.cell.prach_cfg.ports;
    du_low_cell.scs_common      = scs_common;
    du_low_cell.prach_config_index =
        cell.ran.ul_cfg_common.init_ul_bwp.rach_cfg_common->rach_cfg_generic.prach_config_index;
    du_low_cell.max_puschs_per_slot  = du_hi_cell.cell.pusch_cfg.max_puschs_per_slot;
    du_low_cell.pusch_max_nof_layers = cell.ran.init_bwp.pusch.max_nof_layers;
    du_low_cell.tdd_pattern          = cell.ran.tdd_cfg;
    if (du_hi_cell.cell.ntn_cfg && du_hi_cell.cell.ntn_cfg->serving) {
      du_low_cell.ntn_cs_koffset = du_hi_cell.cell.ntn_cfg->serving->cell_specific_koffset;
    }
  }

  return odu_low_cfg;
}

static flexible_o_du_ru_config
generate_o_du_ru_config(span<const odu::du_cell_config> cells, unsigned max_processing_delay, unsigned prach_nof_ports)
{
  flexible_o_du_ru_config out_cfg;
  out_cfg.prach_nof_ports      = prach_nof_ports;
  out_cfg.max_processing_delay = max_processing_delay;

  for (const auto& cell : cells) {
    auto&                    out_cell   = out_cfg.cells.emplace_back();
    const subcarrier_spacing scs_common = cell.ran.dl_cfg_common.init_dl_bwp.generic_params.scs;
    out_cell.nof_tx_antennas            = cell.ran.dl_carrier.nof_ant;
    out_cell.nof_rx_antennas            = cell.ran.ul_carrier.nof_ant;
    out_cell.scs                        = scs_common;
    out_cell.dl_arfcn                   = cell.ran.dl_carrier.arfcn_f_ref.value();
    out_cell.ul_arfcn                   = cell.ran.ul_carrier.arfcn_f_ref.value();
    out_cell.tdd_config                 = cell.ran.tdd_cfg;
    out_cell.bw                         = cell.ran.dl_carrier.carrier_bw;
    out_cell.freq_range                 = band_helper::get_freq_range(cell.ran.dl_carrier.band);
    out_cell.cp                         = cell.ran.dl_cfg_common.init_dl_bwp.generic_params.cp;
  }

  return out_cfg;
}

/// Converts app-level ntn_config to library-level ntn_serving_cell_config. Returns std::nullopt for a TN-band cell
/// that only reports NTN neighbor cells. \p resolved_satellites must already contain an entry for the cell's
/// satellite_idx (resolved, including any inline satellite definitions, before this is called).
static std::optional<ocudu_ntn::ntn_serving_cell_config>
convert_ntn_config_to_serving_cell_config(const du_high_unit_cell_ntn_config&         cfg,
                                          span<const ocudu_ntn::ntn_satellite_config> resolved_satellites)
{
  if (!cfg.serving) {
    return std::nullopt;
  }
  const auto& serving = *cfg.serving;

  ocudu_ntn::ntn_serving_cell_config info = {};

  info.satellite_index = *serving.sat_ref.satellite_idx;

  // SIB19 fields exempt from valuetag.
  info.moving_reference_location = serving.moving_ref_location;
  if (serving.sat_ref.ta_info) {
    info.ta_common_offset = serving.sat_ref.ta_info->ta_common_offset;
  }
  info.epoch_time               = serving.epoch_time;
  info.ntn_ul_sync_validity_dur = serving.ntn_ul_sync_validity_dur;

  // SIB19 fields tracked by valuetag.
  info.reference_location    = serving.reference_location;
  info.distance_threshold    = serving.distance_threshold;
  info.t_service             = serving.t_service;
  info.cell_specific_koffset = serving.cell_specific_koffset;
  info.k_mac                 = serving.k_mac;
  info.polarization          = serving.polarization;
  info.ta_report             = serving.ta_report;

  // Metadata fields.
  info.epoch_sfn_offset = serving.epoch_sfn_offset;
  info.feeder_link_info = serving.feeder_link_info;

  info.use_state_vector = derive_use_state_vector(
      serving.use_state_vector, serving.sat_ref.ephemeris_info, info.satellite_index, resolved_satellites);

  return info;
}

static ocudu_ntn::ntn_configuration_manager_config
generate_ntn_configuration_manager_config(const gnb_id_t&                          gnb_id,
                                          span<const du_high_unit_cell_config>     du_hi_cells,
                                          const std::vector<ntn_satellite_config>& ntn_satellites)
{
  ocudu_ntn::ntn_configuration_manager_config out_cfg = {};

  // Add globally-defined satellites first. Use user-defined satellite_idx as internal satellite_index.
  unsigned next_satellite_idx = add_global_ntn_satellites(ntn_satellites, out_cfg.satellites);

  // Resolve satellite_idx for every serving cell, sat-switch target and neighbor cell: reuse the global satellite
  // if satellite_idx is set, else create one inline (1-to-1). After this loop, satellite_idx is guaranteed set
  // wherever a satellite reference is present. Neighbor cells are resolved regardless of whether this is an
  // NTN serving cell or a TN-band cell that only reports NTN neighbor cells.
  std::vector<std::optional<du_high_unit_cell_ntn_config>> resolved_ntn_cfgs(du_hi_cells.size());
  for (unsigned phy_sector_idx = 0; phy_sector_idx != du_hi_cells.size(); ++phy_sector_idx) {
    const auto& cell_cfg = du_hi_cells[phy_sector_idx].cell;
    if (!cell_cfg.ntn_cfg) {
      continue;
    }
    du_high_unit_cell_ntn_config ntn_cfg = *cell_cfg.ntn_cfg;

    if (ntn_cfg.serving) {
      auto& serving = *ntn_cfg.serving;
      resolve_ntn_satellite_ref(serving.sat_ref,
                                out_cfg.satellites,
                                next_satellite_idx,
                                serving.sat_ref.ta_info,
                                fmt::format("cells[{}].ntn", phy_sector_idx));

      if (serving.sat_switch_with_resync) {
        auto& sat_sw = *serving.sat_switch_with_resync;
        resolve_ntn_satellite_ref(sat_sw.sat_ref,
                                  out_cfg.satellites,
                                  next_satellite_idx,
                                  std::nullopt,
                                  fmt::format("cells[{}].ntn.sat_switch_with_resync", phy_sector_idx));
      }
    }

    for (auto& ncell : ntn_cfg.ncells) {
      resolve_ntn_satellite_ref(ncell.sat_ref,
                                out_cfg.satellites,
                                next_satellite_idx,
                                ncell.sat_ref.ta_info,
                                fmt::format("cells[{}].ntn.ncells[pci={}]",
                                            phy_sector_idx,
                                            ncell.phys_cell_id ? static_cast<unsigned>(*ncell.phys_cell_id) : 0U));
    }

    resolved_ntn_cfgs[phy_sector_idx] = std::move(ntn_cfg);
  }

  // Build the cell configs from the resolved NTN configs (satellite_idx guaranteed set).
  for (unsigned phy_sector_idx = 0; phy_sector_idx != du_hi_cells.size(); ++phy_sector_idx) {
    if (!resolved_ntn_cfgs[phy_sector_idx]) {
      continue;
    }
    const auto& cell_cfg = du_hi_cells[phy_sector_idx].cell;
    const auto& ntn_cfg  = *resolved_ntn_cfgs[phy_sector_idx];

    // Build cell config.
    auto&                      out_cell = out_cfg.cells.emplace_back();
    expected<plmn_identity>    plmn     = plmn_identity::parse(cell_cfg.plmn);
    expected<nr_cell_identity> nci      = nr_cell_identity::create(gnb_id, cell_cfg.sector_id.value());
    if (not plmn) {
      report_error("Invalid PLMN: {}", cell_cfg.plmn);
    }
    if (not nci) {
      report_error("Invalid NR-NCI");
    }
    out_cell.sector_id      = phy_sector_idx;
    out_cell.nr_cgi.plmn_id = plmn.value();
    out_cell.nr_cgi.nci     = nci.value();
    out_cell.ntn_cfg        = convert_ntn_config_to_serving_cell_config(ntn_cfg, out_cfg.satellites);

    // Build sat-switch target satellite (if configured).
    if (ntn_cfg.serving && ntn_cfg.serving->sat_switch_with_resync) {
      const auto& sat_sw  = *ntn_cfg.serving->sat_switch_with_resync;
      out_cell.sat_switch = {*sat_sw.sat_ref.satellite_idx,
                             sat_sw.t_service_start,
                             sat_sw.ssb_time_offset_sf,
                             sat_sw.ntn_ul_sync_validity_dur,
                             sat_sw.cell_specific_koffset,
                             sat_sw.k_mac,
                             sat_sw.polarization,
                             sat_sw.ta_report,
                             derive_use_state_vector(sat_sw.use_state_vector,
                                                     sat_sw.sat_ref.ephemeris_info,
                                                     *sat_sw.sat_ref.satellite_idx,
                                                     out_cfg.satellites),
                             sat_sw.promote_to_serving,
                             sat_sw.promote_neighbors};
    }

    // Build neighbors' cell configs.
    for (const auto& ncell : ntn_cfg.ncells) {
      auto& nc_cfg                    = out_cell.ncells.emplace_back();
      nc_cfg.satellite_index          = *ncell.sat_ref.satellite_idx;
      nc_cfg.carrier_freq             = ncell.carrier_freq;
      nc_cfg.phys_cell_id             = ncell.phys_cell_id;
      nc_cfg.cell_specific_koffset    = ncell.cell_specific_koffset;
      nc_cfg.ntn_ul_sync_validity_dur = ncell.ntn_ul_sync_validity_dur;
      nc_cfg.k_mac                    = ncell.k_mac;
      nc_cfg.polarization             = ncell.polarization;
      nc_cfg.ta_report                = ncell.ta_report;
      nc_cfg.has_feeder_link          = ncell.has_feeder_link;
      nc_cfg.use_state_vector         = derive_use_state_vector(
          ncell.use_state_vector, ncell.sat_ref.ephemeris_info, nc_cfg.satellite_index, out_cfg.satellites);
    }

    // SIB19 Scheduling info.
    const auto& sib_cfg = cell_cfg.sib_cfg;
    for (unsigned i = 0, ie = sib_cfg.si_sched_info.size(); i != ie; ++i) {
      const auto& si_msg = sib_cfg.si_sched_info[i];
      for (unsigned j = 0, je = si_msg.sib_mapping_info.size(); j != je; ++j) {
        if (si_msg.sib_mapping_info[j] == 19) {
          out_cell.si_sched = ocudu_ntn::ntn_si_scheduling_info{
              i, si_msg.si_period_rf, sib_cfg.si_window_len_slots, si_msg.si_window_position.value()};
        }
      }
    }

    // Each NTN cell must configure exactly one of SIB19 scheduling info or an explicit update period.
    if (out_cell.si_sched.has_value() == out_cell.update_period.has_value()) {
      report_error("NTN cell={:#x} must configure exactly one of SIB19 scheduling info or an explicit update period",
                   out_cell.nr_cgi.nci);
    }
  }
  return out_cfg;
}

o_du_unit flexible_o_du_factory::create_flexible_o_du(const o_du_unit_dependencies& dependencies)
{
  o_du_unit      o_du;
  const unsigned nof_cells  = config.odu_high_cfg.du_high_cfg.config.cells_cfg.size();
  o_du.e2_metric_connectors = std::make_unique<
      e2_metric_connector_manager<e2_du_metrics_connector, e2_du_metrics_notifier, e2_du_metrics_interface>>(nof_cells);

  const du_high_unit_config& du_hi = config.odu_high_cfg.du_high_cfg.config;
  const du_low_unit_config&  du_lo = config.du_low_cfg;

  auto du_cells = generate_du_cell_config(du_hi);

  std::vector<pci_t> pci_cell_mapper;

  for (const auto& cell : du_cells) {
    pci_cell_mapper.push_back(cell.ran.pci);
  }

  std::chrono::nanoseconds symbol_duration(
      static_cast<int64_t>(1e6 / (get_nsymb_per_slot(cyclic_prefix::NORMAL) *
                                  get_nof_slots_per_subframe(du_hi.cells_cfg.front().cell.common_scs))));

  std::vector<std::unique_ptr<app_services::toggle_stdout_metrics_app_command::metrics_subcommand>>
      ru_metrics_subcommands;
  // Create flexible O-DU metrics configuration.
  flexible_o_du_metrics_notifier* flexible_odu_metrics_notifier =
      build_flexible_o_du_metrics_config(o_du.metrics,
                                         ru_metrics_subcommands,
                                         *dependencies.metrics_notifier,
                                         dependencies.remote_metrics_gateway,
                                         config.ru_cfg.config,
                                         std::move(pci_cell_mapper),
                                         symbol_duration);

  // Create flexible O-DU implementation.
  auto du_impl = std::make_unique<flexible_o_du_impl>(nof_cells, flexible_odu_metrics_notifier);

  o_du_low_unit_config odu_low_cfg =
      generate_o_du_low_config(du_lo, config.ru_cfg.rx_gain_dB, du_cells, du_hi.cells_cfg);
#ifdef OCUDU_FLEXIBLE_DU_DAPP
  std::unique_ptr<app_services::dapp_service> dapp_service;
  if (config.dapp) {
    auto dapp_config =
        derive_dapp_service_config(*config.dapp,
                                   odu_low_cfg.cells,
                                   du_cells,
                                   dependencies.workers->get_du_low_executor_mapper().get_upper_phy_execution_config(),
                                   du_lo.expert_phy_cfg.pusch_acceleration_mode != "disabled");
    dapp_config.ran_control = dependencies.dapp_ran_control;
    auto created            = app_services::dapp_service::create(std::move(dapp_config));
    report_fatal_error_if_not(created, "Unable to create dApp service: {}", created.detail);
    dapp_service = std::move(created.service);
  }
#endif
  o_du_low_unit_dependencies odu_low_dependencies = {
      .rg_gateway                 = du_impl->get_upper_ru_dl_rg_adapter(),
      .rx_symbol_request_notifier = du_impl->get_upper_ru_ul_request_adapter(),
      .workers                    = dependencies.workers->get_du_low_executor_mapper(),
      .fapi_p5_executor           = dependencies.workers->get_cmd_line_executor(),
#ifdef OCUDU_FLEXIBLE_DU_DAPP
      .dapp_hooks      = dapp_service ? dapp_service->upper_phy_hooks() : nullptr,
      .phy_diagnostics = dapp_service ? dapp_service->phy_diagnostics() : nullptr
#endif
  };
#ifdef OCUDU_FLEXIBLE_DU_DAPP
  if (dapp_service && dependencies.dapp_metrics_fanout != nullptr) {
    dependencies.dapp_metrics_fanout->bind(dapp_service->metrics_json_sink());
  }
#endif
  o_du_low_unit_factory odu_low_factory(du_lo.hal_config);
  auto                  odu_lo_unit = odu_low_factory.create(odu_low_cfg, odu_low_dependencies);

  std::for_each(odu_lo_unit.metrics.begin(), odu_lo_unit.metrics.end(), [&](auto& e) {
    o_du.metrics.emplace_back(std::move(e));
  });

  o_du_high_unit_dependencies odu_hi_unit_dependencies = {dependencies.workers->get_du_high_executor_mapper(),
                                                          *dependencies.f1c_client_handler,
                                                          *dependencies.f1u_teid_allocator,
                                                          *dependencies.f1u_gw,
                                                          *dependencies.timer_ctrl,
                                                          *dependencies.mac_p,
                                                          *dependencies.rlc_p,
                                                          *dependencies.e2_client_handler,
                                                          *(o_du.e2_metric_connectors),
                                                          *dependencies.metrics_notifier,
                                                          dependencies.remote_metrics_gateway,
                                                          {}};
#ifdef OCUDU_FLEXIBLE_DU_DAPP
  if (dapp_service) {
    odu_hi_unit_dependencies.o_du_hi_dependencies.du_hi.dapp_hooks = dapp_service->scheduler_hooks();
  }
#endif

  // Adjust the dependencies.
  for (unsigned i = 0, e = du_cells.size(); i != e; ++i) {
    odu::o_du_high_sector_dependencies sector_dependencies = {
        .p5_gateway = odu_lo_unit.o_du_lo->get_phy_fapi_fastpath_adaptor()
                          .get_sector_adaptor(i)
                          .get_p5_sector_adaptor()
                          .get_p5_requests_gateway(),
        .p7_gateway = odu_lo_unit.o_du_lo->get_phy_fapi_fastpath_adaptor()
                          .get_sector_adaptor(i)
                          .get_p7_sector_adaptor()
                          .get_p7_requests_gateway(),
        .p7_last_req_notifier = odu_lo_unit.o_du_lo->get_phy_fapi_fastpath_adaptor()
                                    .get_sector_adaptor(i)
                                    .get_p7_sector_adaptor()
                                    .get_p7_last_request_notifier(),
        .timer_mng          = dependencies.timer_ctrl->get_timer_manager(),
        .fapi_ctrl_executor = dependencies.workers->get_cmd_line_executor(),
        .mac_ctrl_executor  = dependencies.workers->get_du_high_executor_mapper().du_control_executor(),
        .fapi_logger        = *dependencies.fapi_logger,
    };

    odu_hi_unit_dependencies.o_du_hi_dependencies.sectors.push_back(sector_dependencies);
  }

  o_du_high_unit odu_hi_unit = make_o_du_high_unit(config.odu_high_cfg, std::move(odu_hi_unit_dependencies));

  // Connect the adaptors.
  for (unsigned i = 0, e = du_cells.size(); i != e; ++i) {
    auto& p5_phy_adaptor =
        odu_lo_unit.o_du_lo->get_phy_fapi_fastpath_adaptor().get_sector_adaptor(i).get_p5_sector_adaptor();
    auto& p5_mac_adaptor =
        odu_hi_unit.o_du_hi->get_mac_fapi_fastpath_adaptor().get_sector_adaptor(i).get_p5_sector_fastpath_adaptor();

    p5_phy_adaptor.set_p5_responses_notifier(p5_mac_adaptor.get_p5_responses_notifier());
    p5_phy_adaptor.set_error_indication_notifier(p5_mac_adaptor.get_error_indication_notifier());

    auto& p7_phy_adaptor =
        odu_lo_unit.o_du_lo->get_phy_fapi_fastpath_adaptor().get_sector_adaptor(i).get_p7_sector_adaptor();
    auto& p7_mac_adaptor =
        odu_hi_unit.o_du_hi->get_mac_fapi_fastpath_adaptor().get_sector_adaptor(i).get_p7_sector_adaptor();

    // Connect P7 O-DU low with O-DU high.
    p7_phy_adaptor.set_p7_slot_indication_notifier(p7_mac_adaptor.get_p7_slot_indication_notifier());
    p7_phy_adaptor.set_error_indication_notifier(p7_mac_adaptor.get_error_indication_notifier());
    p7_phy_adaptor.set_p7_indications_notifier(p7_mac_adaptor.get_p7_indications_notifier());
  }

  std::for_each(odu_hi_unit.metrics.begin(), odu_hi_unit.metrics.end(), [&](auto& e) {
    o_du.metrics.emplace_back(std::move(e));
  });

  // Manage commands.
  o_du.commands = std::move(odu_hi_unit.commands);

  odu::o_du_dependencies odu_dependencies;
  odu_dependencies.odu_hi           = std::move(odu_hi_unit.o_du_hi);
  odu_dependencies.odu_lo           = std::move(odu_lo_unit.o_du_lo);
  odu_dependencies.metrics_notifier = &du_impl->get_o_du_metrics_notifier();

  auto odu_instance = make_o_du(std::move(odu_dependencies));
  report_error_if_not(odu_instance, "Invalid Distributed Unit");

  flexible_o_du_ru_config ru_config = generate_o_du_ru_config(
      du_cells, du_lo.expert_phy_cfg.max_processing_delay_slots, du_hi.cells_cfg.front().cell.prach_cfg.ports.size());
  flexible_o_du_ru_dependencies ru_dependencies{*dependencies.workers,
                                                du_impl->get_upper_ru_ul_adapter(),
                                                du_impl->get_upper_ru_timing_adapter(),
                                                du_impl->get_upper_ru_error_adapter()};

  std::unique_ptr<radio_unit> ru = create_radio_unit(ru_config, ru_dependencies);

  ocudu_assert(ru, "Invalid Radio Unit");

#ifdef OCUDU_FLEXIBLE_DU_DAPP
  // Describe the served cells and hand the RU gain controller to the E3 RAN
  // control authority so cell info and gain actions are answerable over E3.
  if (dependencies.dapp_ran_control) {
    std::vector<dapp::e3::cell_descriptor> descriptors;
    for (unsigned cell_index = 0; cell_index != du_cells.size() && cell_index != du_hi.cells_cfg.size(); ++cell_index) {
      const auto&               unit = du_hi.cells_cfg[cell_index].cell;
      const auto&               du   = du_cells[cell_index];
      dapp::e3::cell_descriptor cell;
      cell.cell_id          = static_cast<uint16_t>(cell_index);
      cell.pci              = static_cast<uint16_t>(unit.pci);
      cell.plmn             = unit.plmn;
      cell.tac              = static_cast<uint32_t>(unit.tac);
      cell.nr_cell_identity = du.nr_cgi.nci.value();
      cell.band             = nr_band_to_uint(du.ran.dl_carrier.band);
      cell.dl_arfcn         = du.ran.dl_carrier.arfcn_f_ref.value();
      cell.ul_arfcn         = du.ran.ul_carrier.arfcn_f_ref.value();
      cell.dl_center_hz     = static_cast<uint64_t>(band_helper::nr_arfcn_to_freq(du.ran.dl_carrier.arfcn_f_ref));
      cell.ul_center_hz     = static_cast<uint64_t>(band_helper::nr_arfcn_to_freq(du.ran.ul_carrier.arfcn_f_ref));
      cell.bandwidth_mhz    = static_cast<uint16_t>(bs_channel_bandwidth_to_MHz(du.ran.dl_carrier.carrier_bw));
      cell.nof_prbs         = static_cast<uint16_t>(du.ran.ul_cfg_common.init_ul_bwp.generic_params.crbs.length());
      cell.scs_khz          = static_cast<uint16_t>(scs_to_khz(unit.common_scs));
      cell.nof_tx_antennas  = static_cast<uint8_t>(unit.nof_antennas_dl);
      cell.nof_rx_antennas  = static_cast<uint8_t>(unit.nof_antennas_ul);
      cell.barred           = du.cell_barred;
      if (du.ran.tdd_cfg) {
        const auto& pattern = du.ran.tdd_cfg->pattern1;
        cell.tdd            = dapp::e3::cell_tdd_pattern{static_cast<uint16_t>(pattern.dl_ul_tx_period_nof_slots),
                                              static_cast<uint8_t>(pattern.nof_dl_slots),
                                              static_cast<uint8_t>(pattern.nof_dl_symbols),
                                              static_cast<uint8_t>(pattern.nof_ul_slots),
                                              static_cast<uint8_t>(pattern.nof_ul_symbols)};
      }
      if (du.ran.ul_cfg_common.init_ul_bwp.rach_cfg_common) {
        const auto& rach = *du.ran.ul_cfg_common.init_ul_bwp.rach_cfg_common;
        cell.prach       = dapp::e3::cell_prach_config{rach.rach_cfg_generic.prach_config_index,
                                                 rach.prach_root_seq_index,
                                                 rach.rach_cfg_generic.zero_correlation_zone_config,
                                                 rach.total_nof_ra_preambles,
                                                 rach.rach_cfg_generic.msg1_frequency_start};
      }
      cell.rx_gain_db = config.ru_cfg.rx_gain_dB;
      if (config.ru_cfg.srate_MHz) {
        cell.sample_rate_hz = static_cast<uint64_t>(*config.ru_cfg.srate_MHz * 1e6);
      }
      descriptors.push_back(std::move(cell));
    }
    dependencies.dapp_ran_control->set_cells(std::move(descriptors));
    dependencies.dapp_ran_control->attach_ru_gain_controller(ru->get_controller().get_gain_controller());
  }
#endif

  // Add RU metrics subcommands.
  for (auto& subcmd : ru_metrics_subcommands) {
    o_du.commands.cmdline.metrics_subcommands.emplace_back(std::move(subcmd));
  }

  // Add RU command-line commands.
  o_du.commands.cmdline.commands.push_back(std::make_unique<change_log_level_app_command>());

  // Create the RU gain commands.
  if (auto* controller = ru->get_controller().get_gain_controller()) {
    o_du.commands.cmdline.commands.push_back(std::make_unique<tx_gain_app_command>(*controller));
    o_du.commands.cmdline.commands.push_back(std::make_unique<rx_gain_app_command>(*controller));
  }

  // Create the RU CFO command.
  if (auto* controller = ru->get_controller().get_cfo_controller()) {
    o_du.commands.cmdline.commands.push_back(std::make_unique<cfo_app_command>(*controller));
  }

  // Create the RU transmit time offset command.
  if (auto* controller = ru->get_controller().get_tx_time_offset_controller()) {
    o_du.commands.cmdline.commands.push_back(std::make_unique<tx_time_offset_app_command>(*controller));
  }

  // Create the NTN Configuration Manager if at least one NTN cell is present.
  ocudu_ntn::ntn_configuration_manager_config ntn_manager_config =
      generate_ntn_configuration_manager_config(du_hi.gnb_id, du_hi.cells_cfg, du_hi.ntn_satellites);

  if (not ntn_manager_config.cells.empty()) {
    o_du.ntn_configurator_manager =
        create_ntn_configuration_manager(ntn_manager_config,
                                         odu_instance->get_o_du_high().get_du_high().get_du_configurator(),
                                         odu_instance->get_o_du_high().get_du_high().get_subframe_time_mapper(),
                                         ru->get_controller(),
                                         dependencies.timer_ctrl->get_timer_manager(),
                                         dependencies.workers->get_du_high_executor_mapper().du_control_executor());
    o_du.commands.remote.push_back(
        std::make_unique<ocudu_ntn::ntn_config_update_remote_command>(*o_du.ntn_configurator_manager));
  }

  // Configure the RU and DU in the dynamic DU.
#ifdef OCUDU_FLEXIBLE_DU_DAPP
  if (dapp_service) {
    du_impl->add_dapp_service(std::move(dapp_service));
  }
#endif
  du_impl->add_ru(std::move(ru));
  du_impl->add_du(std::move(odu_instance));

  o_du.unit = std::move(du_impl);

  announce_du_high_cells(du_hi);

  return o_du;
}
