// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "ocudu_scheduler_adapter.h"
#include "ocudu/ran/cyclic_prefix.h"
#include "ocudu/ran/srs/srs_bandwidth_configuration.h"
#include "ocudu/ran/srs/srs_information.h"
#include "ocudu/ran/srs/srs_resource_configuration.h"
#include "ocudu/scheduler/result/sched_result.h"
#include "ocudu/scheduler/scheduler_factory.h"
#include "ocudu/support/async/async_timer.h"
#include "ocudu/support/executors/execute_until_success.h"

using namespace ocudu;

/// Convert a UE creation request for the MAC into a UE creation request of the scheduler.
static sched_ue_creation_request_message make_scheduler_ue_creation_request(const mac_ue_create_request& request)
{
  sched_ue_creation_request_message ret{};
  ret.ue_index           = request.ue_index;
  ret.crnti              = request.crnti;
  ret.starts_in_fallback = request.initial_fallback;
  ret.ul_ccch_slot_rx    = request.ul_ccch_slot_rx;
  ret.cfra_enabled       = request.cfra_preamble_index.has_value();
  ret.cfg                = request.sched_cfg;
  return ret;
}

/// Convert a UE reconfiguration request for the MAC into a UE reconfiguration request of the scheduler.
static sched_ue_reconfiguration_message
make_scheduler_ue_reconfiguration_request(const mac_ue_reconfiguration_request& request)
{
  sched_ue_reconfiguration_message ret{};
  ret.ue_index = request.ue_index;
  ret.crnti    = request.crnti;
  ret.cfg      = request.sched_cfg;
  ret.cs_rnti  = request.assigned_cs_rnti;
  return ret;
}

/// First resource of the first periodic SRS resource set of a UE cell configuration, if any.
static const srs_config::srs_resource* find_periodic_srs_resource(const ue_cell_config& cell)
{
  if (not cell.serv_cell_cfg.ul_config.has_value() or not cell.serv_cell_cfg.ul_config->init_ul_bwp.srs_cfg.has_value()) {
    return nullptr;
  }
  const srs_config& srs = *cell.serv_cell_cfg.ul_config->init_ul_bwp.srs_cfg;
  for (const auto& res_set : srs.srs_res_set_list) {
    if (not std::holds_alternative<srs_config::srs_resource_set::periodic_resource_type>(res_set.res_type)) {
      continue;
    }
    for (const auto res_id : res_set.srs_res_id_list) {
      for (const auto& res : srs.srs_res_list) {
        if (res.id.ue_res_id == res_id and res.periodicity_and_offset.has_value()) {
          return &res;
        }
      }
    }
  }
  return nullptr;
}

/// Fills the TS 38.331 resource and its TS 38.211 mapping of transmit port 0 into an SRS configuration event.
static void fill_srs_resource(scheduler_dapp_srs_config_event& event,
                              const srs_config::srs_resource& res,
                              unsigned                        nof_symbols_per_slot)
{
  event.srs_configured            = true;
  event.resource_id               = static_cast<uint8_t>(res.id.ue_res_id);
  event.nof_ports                 = static_cast<uint8_t>(res.nof_ports);
  event.comb_size                 = static_cast<uint8_t>(res.tx_comb.size);
  event.comb_offset               = res.tx_comb.tx_comb_offset;
  event.cyclic_shift              = res.tx_comb.tx_comb_cyclic_shift;
  event.start_position            = res.res_mapping.start_pos;
  event.nof_symbols               = static_cast<uint8_t>(res.res_mapping.nof_symb);
  event.repetition_factor         = static_cast<uint8_t>(res.res_mapping.rept_factor);
  event.freq_domain_position      = res.freq_domain_pos;
  event.freq_domain_shift         = res.freq_domain_shift;
  event.c_srs                     = res.freq_hop.c_srs;
  event.b_srs                     = res.freq_hop.b_srs;
  event.b_hop                     = res.freq_hop.b_hop;
  event.group_or_sequence_hopping = static_cast<uint8_t>(res.grp_or_seq_hop);
  event.sequence_id               = res.sequence_id;
  event.period_slots              = static_cast<uint16_t>(res.periodicity_and_offset->period);
  event.offset_slots              = res.periodicity_and_offset->offset;
  event.start_symbol              = static_cast<uint8_t>(nof_symbols_per_slot - res.res_mapping.start_pos - 1);

  // get_srs_information() asserts on hopping and on an invalid (C_SRS, B_SRS) pair: check both first.
  if (res.grp_or_seq_hop != srs_group_or_sequence_hopping::neither or res.freq_hop.b_hop < res.freq_hop.b_srs or
      not srs_configuration_get(res.freq_hop.c_srs, res.freq_hop.b_srs).has_value()) {
    return;
  }
  srs_resource_configuration phy_res;
  phy_res.nof_antenna_ports   = static_cast<srs_resource_configuration::one_two_four_enum>(res.nof_ports);
  phy_res.nof_symbols         = res.res_mapping.nof_symb;
  phy_res.start_symbol        = event.start_symbol;
  phy_res.configuration_index = res.freq_hop.c_srs;
  phy_res.sequence_id         = res.sequence_id;
  phy_res.bandwidth_index     = res.freq_hop.b_srs;
  phy_res.comb_size           = res.tx_comb.size;
  phy_res.comb_offset         = res.tx_comb.tx_comb_offset;
  phy_res.cyclic_shift        = res.tx_comb.tx_comb_cyclic_shift;
  phy_res.freq_position       = res.freq_domain_pos;
  phy_res.freq_shift          = res.freq_domain_shift;
  phy_res.freq_hopping        = res.freq_hop.b_hop;
  phy_res.hopping             = res.grp_or_seq_hop;
  if (not phy_res.is_valid()) {
    return;
  }
  const srs_information info       = get_srs_information(phy_res, 0);
  event.derived_valid              = true;
  event.cyclic_shift_maximum       = static_cast<uint8_t>(info.n_cs_max);
  event.sequence_length            = static_cast<uint16_t>(info.sequence_length);
  event.mapping_initial_subcarrier = static_cast<uint16_t>(info.mapping_initial_subcarrier);
  event.sequence_group             = static_cast<uint16_t>(info.sequence_group);
  event.sequence_number            = static_cast<uint16_t>(info.sequence_number);
}

void ocudu_scheduler_adapter::publish_dapp_srs_config(scheduler_dapp_srs_config_event::kind event,
                                                      du_ue_index_t                         ue_index,
                                                      du_cell_index_t                       cell_index,
                                                      rnti_t                                rnti,
                                                      const sched_ue_config_request*        cfg)
{
  if (not dapp_hooks or ue_index >= MAX_NOF_DU_UES or cell_index >= MAX_NOF_DU_CELLS or
      not dapp_cells[cell_index].has_value()) {
    return;
  }
  const dapp_cell_context& cell = *dapp_cells[cell_index];
  auto&                    last = dapp_ue_srs[ue_index];

  scheduler_dapp_srs_config_event result;
  if (cfg == nullptr and last.has_value() and event != scheduler_dapp_srs_config_event::kind::removed) {
    // Unchanged configuration: repeat the last one under the new event.
    result = *last;
  } else {
    result.cell_id      = static_cast<uint32_t>(cell_index);
    result.rnti         = to_value(rnti);
    result.ue_index     = static_cast<uint16_t>(ue_index);
    result.numerology   = cell.numerology;
    result.n_ta_offset  = cell.n_ta_offset;
    result.bwp_start_rb = cell.bwp_start_rb;
    result.bwp_size_rb  = cell.bwp_size_rb;
    if (cfg != nullptr and cfg->cells.has_value() and not cfg->cells->empty()) {
      if (const auto* res = find_periodic_srs_resource(cfg->cells->front())) {
        fill_srs_resource(result, *res, cell.nof_symbols_per_slot);
      }
    }
  }
  result.event = event;
  if (event == scheduler_dapp_srs_config_event::kind::removed) {
    last.reset();
  } else {
    last = result;
  }
  dapp_hooks->publish_srs_config(result);
}

ocudu_scheduler_adapter::ocudu_scheduler_adapter(const ocudu_mac_sched_config& params, rnti_manager& rnti_mng_) :
  rnti_mng(rnti_mng_),
  rlf_handler(params.mac_cfg, params.ctrl_exec),
  ctrl_exec(params.ctrl_exec),
  timers(params.timers),
  logger(ocudulog::fetch_basic_logger("MAC")),
  notifier(*this),
  sched_impl(create_scheduler(scheduler_config{params.sched_cfg, notifier, params.dapp_hooks})),
  rach_handler(*sched_impl, rnti_mng, logger),
  pos_handler(create_positioning_handler(*sched_impl, params.ctrl_exec, params.timers, logger)),
  dapp_hooks(params.dapp_hooks)
{
  ocudu_assert(last_slot_point.is_lock_free(), "slot point is not lock free");
  ocudu_assert(last_slot_tp.is_lock_free(), "slot time_point is not lock free");
}

void ocudu_scheduler_adapter::add_cell(const mac_scheduler_cell_creation_request& msg)
{
  // Setup UCI decoder for new cell.
  cell_handlers.emplace(msg.cell_params.cell_index, *this, msg.cell_params.sched_req);

  if (msg.cell_params.cell_index < MAX_NOF_DU_CELLS) {
    const ran_cell_config&   ran = msg.cell_params.sched_req.ran;
    const bwp_configuration& bwp = ran.ul_cfg_common.init_ul_bwp.generic_params;
    dapp_cells[msg.cell_params.cell_index] =
        dapp_cell_context{static_cast<uint8_t>(to_numerology_value(bwp.scs)),
                          static_cast<uint16_t>(ran.ta_offset),
                          static_cast<uint16_t>(bwp.crbs.start()),
                          static_cast<uint16_t>(bwp.crbs.length()),
                          get_nsymb_per_slot(bwp.cp)};
  }

  // Forward cell configuration to scheduler.
  auto sched_req                      = msg.cell_params.sched_req;
  sched_req.metrics                   = {msg.metric_notifier};
  sched_req.metrics.report_ue_metrics = msg.report_ue_metrics;
  sched_impl->handle_cell_configuration_request(sched_req);
}

void ocudu_scheduler_adapter::remove_cell(du_cell_index_t cell_index)
{
  if (not cell_handlers.contains(cell_index)) {
    return;
  }

  // Request cell removal from scheduler.
  sched_impl->handle_cell_removal_request(cell_index);

  // Remove cell from cell handlers.
  cell_handlers.erase(cell_index);
}

async_task<bool> ocudu_scheduler_adapter::handle_ue_creation_request(const mac_ue_create_request& msg)
{
  return launch_async([this, msg](coro_context<async_task<bool>>& ctx) {
    CORO_BEGIN(ctx);

    // Add UE to RLF handler.
    rlf_handler.add_ue(msg.ue_index, *msg.rlf_notifier);

    // In case of CFRA, store the RA preamble.
    if (msg.cfra_preamble_index.has_value()) {
      if (not cell_handlers[msg.cell_index].get_rach_handler().handle_cfra_allocation(
              msg.cfra_preamble_index.value(), msg.ue_index, msg.crnti)) {
        CORO_EARLY_RETURN(false);
      }
    }

    // Create UE in the Scheduler.
    sched_impl->handle_ue_creation_request(make_scheduler_ue_creation_request(msg));

    // Await Scheduler notification that UE was added.
    CORO_AWAIT_VALUE(bool res, sched_cfg_notif_map[msg.ue_index].ue_config_ready);
    sched_cfg_notif_map[msg.ue_index].ue_config_ready.reset();

    if (res) {
      publish_dapp_srs_config(
          scheduler_dapp_srs_config_event::kind::created, msg.ue_index, msg.cell_index, msg.crnti, &msg.sched_cfg);
    }

    CORO_RETURN(res);
  });
}

async_task<bool> ocudu_scheduler_adapter::handle_ue_reconfiguration_request(const mac_ue_reconfiguration_request& msg)
{
  return launch_async([this, msg](coro_context<async_task<bool>>& ctx) {
    CORO_BEGIN(ctx);

    // Reconfigure UE in the scheduler.
    sched_impl->handle_ue_reconfiguration_request(make_scheduler_ue_reconfiguration_request(msg));

    // Await Scheduler notification.
    CORO_AWAIT(sched_cfg_notif_map[msg.ue_index].ue_config_ready);
    sched_cfg_notif_map[msg.ue_index].ue_config_ready.reset();

    if (msg.ue_index < MAX_NOF_DU_UES and dapp_ue_srs[msg.ue_index].has_value()) {
      publish_dapp_srs_config(scheduler_dapp_srs_config_event::kind::reconfigured,
                              msg.ue_index,
                              static_cast<du_cell_index_t>(dapp_ue_srs[msg.ue_index]->cell_id),
                              msg.crnti,
                              msg.sched_cfg.cells.has_value() ? &msg.sched_cfg : nullptr);
    }

    CORO_RETURN(true);
  });
}

async_task<void> ocudu_scheduler_adapter::handle_ue_removal_request(const mac_ue_delete_request& msg)
{
  return launch_async([this, msg](coro_context<async_task<void>>& ctx) {
    CORO_BEGIN(ctx);

    if (msg.min_removal_delay.count() > 0) {
      // > Wait for the minimum removal delay before starting the actual UE removal.

      // Deactivate UE so it doesn't get new PDSCH/PUSCH grants during the wait period.
      sched_impl->handle_ue_deactivation_request(msg.ue_index);

      // Wait for the specified delay.
      CORO_AWAIT(async_wait_for(timers.create_unique_timer(ctrl_exec), msg.min_removal_delay));
    }

    // Remove UE from the scheduler.
    sched_impl->handle_ue_removal_request(msg.ue_index);

    // Await Scheduler notification.
    CORO_AWAIT(sched_cfg_notif_map[msg.ue_index].ue_config_ready);
    sched_cfg_notif_map[msg.ue_index].ue_config_ready.reset();

    publish_dapp_srs_config(
        scheduler_dapp_srs_config_event::kind::removed, msg.ue_index, msg.cell_index, msg.rnti, nullptr);

    // In case the UE was created via CFRA, remove the RA preamble.
    cell_handlers[msg.cell_index].get_rach_handler().handle_cfra_deallocation(msg.ue_index);

    // Remove UE from RLF handler.
    rlf_handler.rem_ue(msg.ue_index, msg.cell_index);

    CORO_RETURN();
  });
}

void ocudu_scheduler_adapter::handle_ue_config_applied(du_ue_index_t ue_index)
{
  // Free any CFRA preamble slot still held by this UE — the config being applied means
  // the random access procedure is complete and the slot can be reused.
  rach_handler.handle_cfra_deallocation(ue_index);

  // Notify scheduler that the UE confirmed the configuration.
  sched_impl->handle_ue_config_applied(ue_index);

  if (ue_index < MAX_NOF_DU_UES and dapp_ue_srs[ue_index].has_value()) {
    const scheduler_dapp_srs_config_event& last = *dapp_ue_srs[ue_index];
    publish_dapp_srs_config(scheduler_dapp_srs_config_event::kind::applied,
                            ue_index,
                            static_cast<du_cell_index_t>(last.cell_id),
                            to_rnti(last.rnti),
                            nullptr);
  }
}

/// Converts BSR MAC CE (See 38.321, 6.1.3.1) to Scheduler UL BSR event type.
static ul_bsr_lcg_report make_sched_lcg_report(const lcg_bsr_report& ul_mac_bsr, bsr_format bsr_format)
{
  return ul_bsr_lcg_report{ul_mac_bsr.lcg_id, buff_size_field_to_bytes(ul_mac_bsr.buffer_size, bsr_format)};
}

void ocudu_scheduler_adapter::handle_ul_bsr_indication(const mac_bsr_ce_info& bsr)
{
  // Prepare UL BSR indication for Scheduler
  ul_bsr_indication_message ul_bsr_ind{};
  ul_bsr_ind.cell_index = bsr.cell_index;
  ul_bsr_ind.ue_index   = bsr.ue_index;
  ul_bsr_ind.crnti      = bsr.rnti;
  ul_bsr_ind.type       = bsr.bsr_fmt;
  if (bsr.bsr_fmt == bsr_format::SHORT_BSR or bsr.bsr_fmt == bsr_format::SHORT_TRUNC_BSR) {
    const ul_bsr_lcg_report sched_bsr = make_sched_lcg_report(bsr.lcg_reports[0], bsr.bsr_fmt);

    if (sched_bsr.nof_bytes == 0) {
      // In case SBSR=0, assume all LCGs are 0.
      for (unsigned j = 0; j <= MAX_LCG_ID; ++j) {
        ul_bsr_ind.reported_lcgs.push_back(ul_bsr_lcg_report{uint_to_lcg_id(j), 0U});
      }
    } else {
      ul_bsr_ind.reported_lcgs.push_back(sched_bsr);
    }
  } else {
    // Long BSR or Long truncated BSR.
    for (const lcg_bsr_report& lb : bsr.lcg_reports) {
      ul_bsr_ind.reported_lcgs.push_back(make_sched_lcg_report(lb, bsr.bsr_fmt));
    }
  }

  // Send UL BSR indication to Scheduler.
  sched_impl->handle_ul_bsr_indication(ul_bsr_ind);
}

void ocudu_scheduler_adapter::handle_ul_sched_command(const mac_ul_scheduling_command& cmd)
{
  // Convert UL scheduling command into a small BSR indication. The SR indication is not used for this purpose
  // because the Scheduler relies on UCI signalling to know the current UE state.
  ul_bsr_indication_message bsr{};
  bsr.cell_index    = cmd.cell_index;
  bsr.ue_index      = cmd.ue_index;
  bsr.crnti         = cmd.rnti;
  bsr.type          = bsr_format::SHORT_BSR;
  bsr.reported_lcgs = {ul_bsr_lcg_report{uint_to_lcg_id(0), 8U}};

  // Send BSR indication to Scheduler.
  sched_impl->handle_ul_bsr_indication(bsr);
}

void ocudu_scheduler_adapter::handle_dl_mac_ce_indication(const mac_ce_scheduling_command& mac_ce)
{
  sched_impl->handle_dl_mac_ce_indication(dl_mac_ce_indication{mac_ce.ue_index, mac_ce.ce_lcid});
}

static slot_point chrono_to_slot_point(std::chrono::high_resolution_clock::time_point hol_toa,
                                       std::chrono::high_resolution_clock::time_point last_slot_tp,
                                       slot_point                                     last_slot_p)
{
  using namespace std::chrono;
  static constexpr microseconds half_system_frame_dur = milliseconds{10240 / 2};

  // Get delay between last slot indication time point and HOL ToA.
  microseconds hol_delay = duration_cast<microseconds>(last_slot_tp - hol_toa);

  // Bound delay to avoid negative values and slot wrap around ambiguity.
  hol_delay = std::min(std::max(hol_delay, microseconds{0}), half_system_frame_dur);

  // Convert usec to slots
  const int hol_delay_slots = hol_delay.count() * last_slot_p.nof_slots_per_subframe() / 1000;

  // Subtract the delay to the current slot.
  return last_slot_p - hol_delay_slots;
}

void ocudu_scheduler_adapter::handle_dl_buffer_state_update(const mac_dl_buffer_state_indication_message& mac_dl_bs_ind)
{
  using namespace std::chrono;

  // Forward DL buffer state indication to the scheduler.
  dl_buffer_state_indication_message bs{};
  bs.ue_index = mac_dl_bs_ind.ue_index;
  bs.lcid     = mac_dl_bs_ind.lcid;
  bs.bs       = mac_dl_bs_ind.bs;
  if (mac_dl_bs_ind.hol_toa.has_value()) {
    // Check if at least one slot indication has been processed.
    const high_resolution_clock::time_point sl_tp = last_slot_tp.load(std::memory_order_relaxed);
    if (sl_tp != high_resolution_clock::time_point{}) {
      // Convert HOL TOA from chrono time point to slots.
      bs.hol_toa = chrono_to_slot_point(
          mac_dl_bs_ind.hol_toa.value(), sl_tp, last_slot_point.load(std::memory_order_relaxed).without_hyper_sfn());
    }
  }

  // Forward DL buffer status.
  sched_impl->handle_dl_buffer_state_indication(bs);
}

void ocudu_scheduler_adapter::handle_ul_phr_indication(const mac_phr_ce_info& phr)
{
  // Forward UL PHR indication to the scheduler.
  ul_phr_indication_message ind{};
  ind.cell_index = phr.cell_index;
  ind.ue_index   = phr.ue_index;
  ind.rnti       = phr.rnti;
  ind.slot_rx    = phr.slot_rx;
  ind.phr        = phr.phr;
  sched_impl->handle_ul_phr_indication(ind);
}

void ocudu_scheduler_adapter::handle_crnti_ce_indication(du_ue_index_t old_ue_index, du_cell_index_t cell_index)
{
  rlf_handler.handle_crnti_ce(old_ue_index);
  sched_impl->handle_crnti_ce_received(old_ue_index);
}

const sched_result& ocudu_scheduler_adapter::slot_indication(slot_point_extended slot_tx, du_cell_index_t cell_idx)
{
  using namespace std::chrono;

  // Mark start of the slot in time (estimate).
  if (last_slot_point.exchange(slot_tx, std::memory_order_relaxed) != slot_tx) {
    auto slot_tp = high_resolution_clock::now();
    last_slot_tp.store(slot_tp, std::memory_order_relaxed);
  }

  const sched_result& res = sched_impl->slot_indication(slot_tx, cell_idx);

  if (res.success) {
    // Store UCI PDUs for later decoding.
    cell_handlers[cell_idx].uci_decoder.store_uci(slot_tx.without_hyper_sfn(), res.ul.pucchs.unsorted(), res.ul.puschs);
  }

  return res;
}

void ocudu_scheduler_adapter::handle_error_indication(slot_point                         slot_tx,
                                                      du_cell_index_t                    cell_idx,
                                                      mac_cell_slot_handler::error_event event)
{
  scheduler_slot_handler::error_outcome sched_err;
  sched_err.pdcch_discarded           = event.pdcch_discarded;
  sched_err.pdsch_discarded           = event.pdsch_discarded;
  sched_err.pusch_and_pucch_discarded = event.pusch_and_pucch_discarded;
  sched_impl->handle_error_indication(slot_tx, cell_idx, sched_err);
}

void ocudu_scheduler_adapter::handle_si_change_indication(const si_scheduling_update_request& request)
{
  sched_impl->handle_si_update_request(request);
}

void ocudu_scheduler_adapter::handle_pws_broadcast_indication(du_cell_index_t         cell_idx,
                                                              unsigned                si_msg_idx,
                                                              std::optional<unsigned> nof_segments,
                                                              units::bytes            msg_len)
{
  sched_impl->handle_pws_broadcast_indication({cell_idx, si_msg_idx, nof_segments, msg_len});
}

void ocudu_scheduler_adapter::handle_slice_reconfiguration_request(const du_cell_slice_reconfig_request& req)
{
  // Update RRM policies in the scheduler.
  sched_impl->handle_slice_reconfiguration_request(req);
}

void ocudu_scheduler_adapter::sched_config_notif_adapter::on_ue_config_complete(du_ue_index_t ue_index,
                                                                                bool          ue_creation_result)
{
  ocudu_sanity_check(is_du_ue_index_valid(ue_index), "Invalid ue index={}", ue_index);

  // Remove continuation of task in ctrl executor.
  defer_until_success(parent.ctrl_exec, parent.timers, [this, ue_index, ue_creation_result]() {
    parent.sched_cfg_notif_map[ue_index].ue_config_ready.set(ue_creation_result);
  });
}

void ocudu_scheduler_adapter::sched_config_notif_adapter::on_ue_deletion_completed(du_ue_index_t ue_index)
{
  ocudu_sanity_check(is_du_ue_index_valid(ue_index), "Invalid ue index={}", ue_index);

  // Continuation of ue remove task dispatched to the ctrl executor.
  defer_until_success(parent.ctrl_exec, parent.timers, [this, ue_index]() {
    parent.sched_cfg_notif_map[ue_index].ue_config_ready.set(true);
  });
}

void ocudu_scheduler_adapter::handle_paging_information(const paging_information& msg)
{
  // Convert MAC paging information to scheduler notification.
  sched_paging_information pg_info{};
  pg_info.paging_drx            = msg.paging_drx;
  pg_info.edrx                  = msg.edrx;
  pg_info.paging_identity       = msg.paging_identity;
  pg_info.paging_type_indicator = msg.paging_type_indicator;
  pg_info.ue_identity           = msg.ue_identity;
  pg_info.paging_cells.resize(msg.paging_cells.size());
  pg_info.paging_cells.assign(msg.paging_cells.begin(), msg.paging_cells.end());

  sched_impl->handle_paging_information(pg_info);
}

ocudu_scheduler_adapter::cell_handler::cell_handler(ocudu_scheduler_adapter&                        parent_,
                                                    const sched_cell_configuration_request_message& sched_cfg) :
  uci_decoder(sched_cfg, parent_.rnti_mng, parent_.rlf_handler),
  pos_handler(parent_.pos_handler->add_cell(sched_cfg.cell_index)),
  cell_idx(sched_cfg.cell_index),
  parent(parent_),
  rach_handler(parent.rach_handler.add_cell(sched_cfg))
{
}

ocudu_scheduler_adapter::cell_handler::~cell_handler()
{
  parent.rach_handler.rem_cell(cell_idx);
}

void ocudu_scheduler_adapter::cell_handler::handle_crc(const mac_crc_indication_message& msg)
{
  // Convert MAC CRC indication to OCUDU scheduler UL CRC indication.
  ul_crc_indication ind{};
  ind.cell_index = cell_idx;
  ind.sl_rx      = msg.sl_rx;
  ind.crcs.resize(msg.crcs.size());
  for (unsigned i = 0, size = msg.crcs.size(); i != size; ++i) {
    const mac_crc_pdu&     mac_pdu = msg.crcs[i];
    ul_crc_pdu_indication& pdu     = ind.crcs[i];
    pdu.rnti                       = mac_pdu.rnti;
    pdu.ue_index                   = parent.rnti_mng[mac_pdu.rnti];
    pdu.harq_id                    = to_harq_id(mac_pdu.harq_id);
    pdu.rapid                      = mac_pdu.rapid;
    pdu.tb_crc_success             = mac_pdu.tb_crc_success;
    pdu.ul_sinr_dB                 = mac_pdu.ul_sinr_dB;
    pdu.ul_rsrp_dBFS               = mac_pdu.ul_rsrp_dBFS;
    pdu.time_advance_offset        = mac_pdu.time_advance_offset;
  }

  // Forward CRC indication to the scheduler.
  parent.sched_impl->handle_crc_indication(ind);

  // Report to RLF handler the CRC result.
  for (const auto& crc : ind.crcs) {
    // If Msg3, ignore the CRC result.
    // Note: UE index is invalid for Msg3 CRCs because no UE has been allocated yet.
    if (crc.ue_index != INVALID_DU_UE_INDEX) {
      parent.rlf_handler.handle_crc(crc.ue_index, cell_idx, crc.tb_crc_success);
    }
  }
}

void ocudu_scheduler_adapter::cell_handler::handle_uci(const mac_uci_indication_message& msg)
{
  // Forward UCI indication to the scheduler.
  parent.sched_impl->handle_uci_indication(uci_decoder.decode_uci(msg));
}

void ocudu_scheduler_adapter::cell_handler::handle_srs(const mac_srs_indication_message& msg)
{
  srs_indication ind;
  ind.cell_index = cell_idx;
  ind.slot_rx    = msg.sl_rx;
  for (const auto& mac_pdu : msg.srss) {
    // Only add PDUs with normalized channel IQ matrix.
    if (const auto* matrix = std::get_if<mac_srs_pdu::normalized_channel_iq_matrix>(&mac_pdu.report)) {
      ind.srss.emplace_back(
          parent.rnti_mng[mac_pdu.rnti], mac_pdu.rnti, mac_pdu.time_advance_offset, matrix->channel_matrix);
    }
  }
  // Forward SRS indication to the scheduler.
  parent.sched_impl->handle_srs_indication(ind);

  // Forward SRS into positioning handler.
  pos_handler->handle_srs_indication(msg);
}
