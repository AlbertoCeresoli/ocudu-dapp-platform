// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "lib/scheduler/config/cell_configuration.h"
#include "lib/scheduler/logging/cell_metrics_handler.h"
#include "tests/test_doubles/scheduler/scheduler_config_helper.h"
#include "tests/test_doubles/utils/test_rng.h"
#include "tests/unittests/scheduler/test_utils/config_generators.h"
#include "ocudu/scheduler/config/scheduler_expert_config_factory.h"
#include "ocudu/scheduler/result/sched_result.h"
#include <gtest/gtest.h>

using namespace ocudu;

class test_scheduler_cell_metrics_notifier : public scheduler_cell_metrics_notifier
{
public:
  unsigned                    period_slots = 1000;
  mutable slot_point_extended next_sl_report;
  scheduler_cell_metrics      last_report;

  scheduler_cell_metrics& get_next() override { return last_report; }

  void commit(scheduler_cell_metrics& ptr) override {}

  bool is_sched_report_required(slot_point_extended sl_tx) const override
  {
    if (not next_sl_report.valid()) {
      next_sl_report = sl_tx + period_slots - (sl_tx.count() % period_slots) - 1;
    }
    if (sl_tx >= next_sl_report) {
      next_sl_report += period_slots;
      return true;
    }
    return false;
  }
};

class test_scheduler_ue_metrics_notifier : public scheduler_metrics_notifier
{
public:
  scheduler_cell_metrics last_report;

  void report_metrics(const scheduler_cell_metrics& metrics) override { last_report = metrics; }
};

class scheduler_metrics_handler_tester : public ::testing::Test
{
protected:
  scheduler_metrics_handler_tester(
      std::chrono::milliseconds period = std::chrono::milliseconds{test_rng::uniform_int<unsigned>(2, 100)}) :
    report_period(period),
    cfg_mng{config_helpers::make_default_scheduler_expert_config()},
    cell_cfg(*cfg_mng.add_cell(sched_config_helper::make_default_sched_cell_configuration_request())),
    metrics(cell_cfg, sched_cell_configuration_request_message::metrics_config{&metrics_notif})
  {
    metrics_notif.period_slots = report_period.count() * get_nof_slots_per_subframe(cell_cfg.scs_common());
    metrics.handle_ue_creation(test_ue_index, to_rnti(0x4601), pci_t{0});
  }

  void run_slot(const sched_result& sched_res, std::chrono::microseconds latency = std::chrono::microseconds{0})
  {
    metrics.push_result(next_sl_tx, sched_res, latency);
    ++next_sl_tx;
    slot_count++;
  }

  void get_next_metric()
  {
    metrics_notif.last_report = {};
    sched_result sched_res;
    sched_res.dl.nof_dl_symbols = 14;
    sched_res.ul.nof_ul_symbols = 14;
    while (metrics_notif.last_report.ue_metrics.empty()) {
      run_slot(sched_res);
    }
  }

  std::chrono::milliseconds               report_period;
  test_scheduler_cell_metrics_notifier    metrics_notif;
  test_helpers::test_sched_config_manager cfg_mng;
  const cell_configuration&               cell_cfg;
  cell_metrics_handler                    metrics;
  du_ue_index_t test_ue_index = to_du_ue_index(test_rng::uniform_int<unsigned>(0, MAX_NOF_DU_UES - 1));

  slot_point_extended next_sl_tx{
      subcarrier_spacing::kHz15,
      test_rng::uniform_int<unsigned>(0, NOF_HYPER_SFNS * NOF_SFNS * NOF_SUBFRAMES_PER_FRAME - 1)};
  unsigned slot_count = 0;
};

TEST_F(scheduler_metrics_handler_tester, metrics_sent_with_defined_periodicity)
{
  unsigned nof_reports = test_rng::uniform_int<unsigned>(1, 10);
  ASSERT_TRUE(metrics_notif.last_report.ue_metrics.empty());

  // Discard first report, as it may have not enough slots.
  get_next_metric();
  ASSERT_LE(metrics_notif.last_report.nof_slots, report_period.count());
  unsigned last_slot_count = slot_count;

  for (unsigned i = 0; i != nof_reports; ++i) {
    get_next_metric();
    ASSERT_EQ(metrics_notif.last_report.ue_metrics.size(), 1);
    ASSERT_EQ(metrics_notif.last_report.nof_slots, report_period.count());
    ASSERT_EQ(slot_count - last_slot_count, report_period.count() * (i + 1));

    ASSERT_EQ(metrics_notif.last_report.ue_metrics[0].rnti, to_rnti(0x4601));
  }
}

TEST_F(scheduler_metrics_handler_tester, when_ue_metrics_disabled_then_only_cell_metrics_are_reported)
{
  test_scheduler_cell_metrics_notifier cell_only_notif;
  cell_only_notif.period_slots = report_period.count() * get_nof_slots_per_subframe(cell_cfg.scs_common());
  cell_metrics_handler cell_only_metrics(
      cell_cfg, sched_cell_configuration_request_message::metrics_config{&cell_only_notif, 64, false});
  cell_only_metrics.handle_ue_creation(test_ue_index, to_rnti(0x4601), pci_t{0});

  sched_result sched_res;
  sched_res.dl.nof_dl_symbols = 14;
  sched_res.ul.nof_ul_symbols = 14;

  // Drive enough slots to span at least two report periods.
  slot_point_extended sl = next_sl_tx;
  for (unsigned i = 0, e = 2 * cell_only_notif.period_slots; i != e; ++i) {
    cell_only_metrics.push_result(sl, sched_res, std::chrono::microseconds{0});
    ++sl;
  }

  ASSERT_GT(cell_only_notif.last_report.nof_slots, 0) << "Cell metrics must still be reported";
  ASSERT_FALSE(cell_only_notif.last_report.ue_metrics.empty()) << "UE metrics must still be computed";
  ASSERT_FALSE(cell_only_notif.last_report.report_ue_metrics) << "Per-UE reporting flag must be disabled";
}

TEST_F(scheduler_metrics_handler_tester, when_no_events_took_place_then_metrics_are_zero)
{
  this->get_next_metric();
  scheduler_ue_metrics ue_metrics = metrics_notif.last_report.ue_metrics[0];

  ASSERT_EQ(ue_metrics.rnti, to_rnti(0x4601));
  ASSERT_EQ(ue_metrics.dl_brate_kbps, 0);
  ASSERT_EQ(ue_metrics.ul_brate_kbps, 0);
  ASSERT_EQ(ue_metrics.dl_mcs, 0);
  ASSERT_EQ(ue_metrics.ul_mcs, 0);
  ASSERT_EQ(ue_metrics.dl_nof_ok, 0);
  ASSERT_EQ(ue_metrics.dl_nof_nok, 0);
  ASSERT_EQ(ue_metrics.ul_nof_ok, 0);
  ASSERT_EQ(ue_metrics.ul_nof_nok, 0);
  ASSERT_EQ(ue_metrics.pusch_snr_db, 0);
  ASSERT_EQ(ue_metrics.pusch_rsrp_db, -std::numeric_limits<float>::infinity());
  ASSERT_EQ(ue_metrics.pucch_snr_db, 0);
  ASSERT_EQ(ue_metrics.bsr, 0);

  // Check that the CQI and RI statistics have no observations.
  ASSERT_EQ(ue_metrics.cqi_stats.get_nof_observations(), 0);
  ASSERT_EQ(ue_metrics.dl_ri_stats.get_nof_observations(), 0);
  ASSERT_EQ(ue_metrics.ul_ri_stats.get_nof_observations(), 0);
}

TEST_F(scheduler_metrics_handler_tester, compute_nof_dl_oks_and_noks)
{
  unsigned nof_acks  = test_rng::uniform_int<unsigned>(1, 100);
  unsigned nof_nacks = test_rng::uniform_int<unsigned>(1, 100);

  for (unsigned i = 0; i != nof_acks; ++i) {
    metrics.handle_dl_harq_ack(test_ue_index, true, units::bytes{1});
  }
  for (unsigned i = 0; i != nof_nacks; ++i) {
    metrics.handle_dl_harq_ack(test_ue_index, false, units::bytes{1});
  }

  this->get_next_metric();
  scheduler_ue_metrics ue_metrics = metrics_notif.last_report.ue_metrics[0];
  ASSERT_EQ(ue_metrics.rnti, to_rnti(0x4601));
  ASSERT_EQ(ue_metrics.dl_nof_ok, nof_acks);
  ASSERT_EQ(ue_metrics.dl_nof_nok, nof_nacks);

  this->get_next_metric();
  ue_metrics = metrics_notif.last_report.ue_metrics[0];
  ASSERT_EQ(ue_metrics.rnti, to_rnti(0x4601));
  ASSERT_EQ(ue_metrics.dl_nof_ok, 0) << "Nof DL OKs not reset";
  ASSERT_EQ(ue_metrics.dl_nof_nok, 0) << "Nof DL NOKs not reset";
}

TEST_F(scheduler_metrics_handler_tester, compute_nof_ul_oks_and_noks)
{
  unsigned nof_acks  = test_rng::uniform_int<unsigned>(1, 100);
  unsigned nof_nacks = test_rng::uniform_int<unsigned>(1, 100);

  ul_crc_pdu_indication crc_pdu;
  crc_pdu.rnti           = to_rnti(0x4601);
  crc_pdu.ue_index       = test_ue_index;
  crc_pdu.tb_crc_success = true;
  for (unsigned i = 0; i != nof_acks; ++i) {
    metrics.handle_crc_indication(next_sl_tx.without_hyper_sfn() - 1, crc_pdu, units::bytes{1});
  }
  crc_pdu.tb_crc_success = false;
  for (unsigned i = 0; i != nof_nacks; ++i) {
    metrics.handle_crc_indication(next_sl_tx.without_hyper_sfn() - 1, crc_pdu, units::bytes{1});
  }

  this->get_next_metric();
  scheduler_ue_metrics ue_metrics = metrics_notif.last_report.ue_metrics[0];
  ASSERT_EQ(ue_metrics.rnti, to_rnti(0x4601));
  ASSERT_EQ(ue_metrics.pci, pci_t{0});
  ASSERT_EQ(ue_metrics.ul_nof_ok, nof_acks);
  ASSERT_EQ(ue_metrics.ul_nof_nok, nof_nacks);

  this->get_next_metric();
  ue_metrics = metrics_notif.last_report.ue_metrics[0];
  ASSERT_EQ(ue_metrics.rnti, to_rnti(0x4601));
  ASSERT_EQ(ue_metrics.ul_nof_ok, 0) << "Nof DL OKs not reset";
  ASSERT_EQ(ue_metrics.ul_nof_nok, 0) << "Nof DL NOKs not reset";
}

TEST_F(scheduler_metrics_handler_tester, classified_crc_and_exact_resource_metrics_reset_each_report)
{
  get_next_metric();
  ul_crc_pdu_indication crc{};
  crc.rnti     = to_rnti(0x4601);
  crc.ue_index = test_ue_index;
  const ul_crc_link_adaptation_info first{0, true, true, -6.0F, 6.0F, 0.02F};
  const ul_crc_link_adaptation_info retry{1, true, false, -6.0F, 6.0F, 0.02F};
  crc.tb_crc_success = false;
  metrics.handle_crc_indication(next_sl_tx.without_hyper_sfn() - 1, crc, units::bytes{100}, first);
  crc.tb_crc_success = true;
  metrics.handle_crc_indication(next_sl_tx.without_hyper_sfn() - 1, crc, units::bytes{100}, retry);
  sched_result res;
  res.dl.nof_dl_symbols   = 14;
  res.ul.nof_ul_symbols   = 14;
  auto& grant             = res.ul.puschs.emplace_back();
  grant.pusch_cfg.rnti    = crc.rnti;
  grant.pusch_cfg.rbs     = vrb_interval{2, 7};
  grant.pusch_cfg.symbols = ofdm_symbol_range{2, 14};
  grant.context.nof_retxs = 1;
  run_slot(res);
  get_next_metric();
  const auto& result = metrics_notif.last_report.ue_metrics[0];
  EXPECT_EQ(result.ul_first_tx_nok, 1U);
  EXPECT_EQ(result.ul_first_tx_ok, 0U);
  EXPECT_EQ(result.ul_retx_ok, 1U);
  EXPECT_EQ(result.ul_retx_nok, 0U);
  EXPECT_EQ(result.ul_mac_delivered_bytes, 100U);
  EXPECT_EQ(result.ul_prb_symbols, 60U);
  EXPECT_EQ(result.ul_retx_prb_symbols, 60U);
  EXPECT_EQ(result.ul_olla_eligible_nok, 1U);
  EXPECT_EQ(result.ul_olla_skipped, 1U);
  ASSERT_TRUE(result.ul_link_adaptation);
  EXPECT_EQ(result.ul_link_adaptation->offset_db, -6.0F);
  EXPECT_EQ(result.report_period, report_period);
  const uint64_t sequence = result.report_sequence;
  get_next_metric();
  const auto& empty = metrics_notif.last_report.ue_metrics[0];
  EXPECT_EQ(empty.ul_first_tx_nok, 0U);
  EXPECT_EQ(empty.ul_retx_ok, 0U);
  EXPECT_EQ(empty.ul_prb_symbols, 0U);
  EXPECT_EQ(empty.ul_olla_skipped, 0U);
  EXPECT_EQ(empty.ul_mac_delivered_bytes, 0U);
  EXPECT_EQ(empty.ul_retx_prb_symbols, 0U);
  EXPECT_EQ(empty.ul_olla_eligible_nok, 0U);
  EXPECT_EQ(empty.report_sequence, sequence + 1);
}

TEST_F(scheduler_metrics_handler_tester, phr_statistics_cover_every_report_and_reset_each_period)
{
  get_next_metric();
  const auto send_phr = [this](interval<int> ph) {
    ul_phr_indication_message phr_ind{};
    phr_ind.ue_index = test_ue_index;
    phr_ind.rnti     = to_rnti(0x4601);
    phr_ind.slot_rx  = next_sl_tx.without_hyper_sfn() - 1;
    phr_ind.phr.set_se_phr({.serv_cell_id = SERVING_PCELL_IDX,
                            .ph_type      = ph_field_type_t::type1,
                            .ph           = ph,
                            .p_cmax       = p_cmax_dbm_range{20, 21}});
    metrics.handle_ul_phr_indication(phr_ind);
  };
  send_phr({4, 5});
  send_phr({-6, -5});
  send_phr({-1, 0});
  get_next_metric();
  const auto& result = metrics_notif.last_report.ue_metrics[0];
  ASSERT_EQ(result.phr_stats.get_nof_observations(), 3U);
  EXPECT_FLOAT_EQ(result.phr_stats.get_mean(), (4.5F - 5.5F - 0.5F) / 3.0F);
  EXPECT_FLOAT_EQ(result.phr_stats.get_min(), -5.5F);
  EXPECT_FLOAT_EQ(result.phr_stats.get_max(), 4.5F);
  // The floor of the last interval's midpoint: [-1, 0) dB is -1, not 0.
  ASSERT_TRUE(result.last_phr.has_value());
  EXPECT_EQ(*result.last_phr, -1);

  // The statistics restart every period; the last PHR persists until the next one.
  get_next_metric();
  const auto& empty = metrics_notif.last_report.ue_metrics[0];
  EXPECT_EQ(empty.phr_stats.get_nof_observations(), 0U);
  ASSERT_TRUE(empty.last_phr.has_value());
  EXPECT_EQ(*empty.last_phr, -1);
}

TEST_F(scheduler_metrics_handler_tester, compute_mcs)
{
  sch_mcs_index dl_mcs{test_rng::uniform_int<uint8_t>(1, 28)};
  sch_mcs_index ul_mcs{test_rng::uniform_int<uint8_t>(1, 28)};

  // Discard first report, as it may have not enough slots.
  get_next_metric();
  metrics_notif.last_report = {};

  sched_result res;
  res.dl.nof_dl_symbols = 14;
  res.ul.nof_ul_symbols = 14;
  dl_msg_alloc& dl_msg  = res.dl.ue_grants.emplace_back();
  dl_msg.pdsch_cfg.rnti = to_rnti(0x4601);
  auto&          cw     = dl_msg.pdsch_cfg.codewords.emplace_back();
  ul_sched_info& ul_msg = res.ul.puschs.emplace_back();
  ul_msg.pusch_cfg.rnti = to_rnti(0x4601);

  for (unsigned i = 0; i != report_period.count() and metrics_notif.last_report.nof_slots == 0; ++i) {
    cw.mcs_index               = dl_mcs;
    ul_msg.pusch_cfg.mcs_index = ul_mcs;
    run_slot(res);
  }

  scheduler_ue_metrics ue_metrics = metrics_notif.last_report.ue_metrics[0];
  ASSERT_EQ(ue_metrics.rnti, to_rnti(0x4601));
  ASSERT_EQ(ue_metrics.dl_mcs, dl_mcs);
  ASSERT_EQ(ue_metrics.ul_mcs, ul_mcs);

  this->get_next_metric();
  ue_metrics = metrics_notif.last_report.ue_metrics[0];
  ASSERT_EQ(ue_metrics.rnti, to_rnti(0x4601));
  ASSERT_EQ(ue_metrics.dl_mcs, 0) << "DL MCS not reset";
  ASSERT_EQ(ue_metrics.ul_mcs, 0) << "UL MCS not reset";
}

TEST_F(scheduler_metrics_handler_tester, mcs_and_sinr_spread_over_the_period)
{
  get_next_metric();
  sched_result res;
  res.dl.nof_dl_symbols = 14;
  res.ul.nof_ul_symbols = 14;
  dl_msg_alloc& dl_msg  = res.dl.ue_grants.emplace_back();
  dl_msg.pdsch_cfg.rnti = to_rnti(0x4601);
  auto&          cw     = dl_msg.pdsch_cfg.codewords.emplace_back();
  ul_sched_info& ul_msg = res.ul.puschs.emplace_back();
  ul_msg.pusch_cfg.rnti = to_rnti(0x4601);
  for (const unsigned mcs : {10U, 14U, 20U}) {
    cw.mcs_index               = sch_mcs_index{static_cast<uint8_t>(mcs)};
    ul_msg.pusch_cfg.mcs_index = sch_mcs_index{static_cast<uint8_t>(mcs + 1)};
    run_slot(res);
  }
  ul_crc_pdu_indication crc{};
  crc.rnti           = to_rnti(0x4601);
  crc.ue_index       = test_ue_index;
  crc.tb_crc_success = true;
  for (const float sinr : {12.5F, 30.0F, 21.0F}) {
    crc.ul_sinr_dB = sinr;
    metrics.handle_crc_indication(next_sl_tx.without_hyper_sfn() - 1, crc, units::bytes{10});
  }
  get_next_metric();
  const auto& result = metrics_notif.last_report.ue_metrics[0];
  ASSERT_EQ(result.dl_mcs_stats.get_nof_observations(), 3U);
  EXPECT_EQ(result.dl_mcs_stats.get_min(), 10U);
  EXPECT_EQ(result.dl_mcs_stats.get_max(), 20U);
  EXPECT_FLOAT_EQ(result.dl_mcs_stats.get_mean(), 44.0F / 3.0F);
  ASSERT_EQ(result.ul_mcs_stats.get_nof_observations(), 3U);
  EXPECT_EQ(result.ul_mcs_stats.get_min(), 11U);
  EXPECT_EQ(result.ul_mcs_stats.get_max(), 21U);
  ASSERT_EQ(result.pusch_snr_stats.get_nof_observations(), 3U);
  EXPECT_FLOAT_EQ(result.pusch_snr_stats.get_min(), 12.5F);
  EXPECT_FLOAT_EQ(result.pusch_snr_stats.get_max(), 30.0F);
  EXPECT_FLOAT_EQ(result.pusch_snr_stats.get_mean(), 63.5F / 3.0F);

  get_next_metric();
  const auto& empty = metrics_notif.last_report.ue_metrics[0];
  EXPECT_EQ(empty.dl_mcs_stats.get_nof_observations(), 0U);
  EXPECT_EQ(empty.ul_mcs_stats.get_nof_observations(), 0U);
  EXPECT_EQ(empty.pusch_snr_stats.get_nof_observations(), 0U);
}

class scheduler_metrics_handler_300ms_tester : public scheduler_metrics_handler_tester
{
protected:
  scheduler_metrics_handler_300ms_tester() : scheduler_metrics_handler_tester(std::chrono::milliseconds{300}) {}

  void send_crcs(unsigned ok, unsigned nok)
  {
    ul_crc_pdu_indication crc{};
    crc.rnti     = to_rnti(0x4601);
    crc.ue_index = test_ue_index;
    for (unsigned i = 0; i != ok + nok; ++i) {
      crc.tb_crc_success = i < ok;
      metrics.handle_crc_indication(next_sl_tx.without_hyper_sfn() - 1, crc, units::bytes{10});
    }
  }

  void run_slots(unsigned count)
  {
    sched_result res;
    res.dl.nof_dl_symbols = 14;
    res.ul.nof_ul_symbols = 14;
    for (unsigned i = 0; i != count; ++i) {
      run_slot(res);
    }
  }
};

TEST_F(scheduler_metrics_handler_300ms_tester, bler_spread_over_100ms_windows_with_enough_transport_blocks)
{
  // 15 kHz SCS: one slot per millisecond, three 100 ms windows per report.
  get_next_metric();
  send_crcs(5, 5);
  run_slots(100);
  send_crcs(10, 0);
  run_slots(100);
  // Three transport blocks are too few for a window: they do not count.
  send_crcs(1, 2);
  get_next_metric();
  const auto& result = metrics_notif.last_report.ue_metrics[0];
  ASSERT_EQ(result.ul_window_bler_stats.get_nof_observations(), 2U);
  EXPECT_FLOAT_EQ(result.ul_window_bler_stats.get_min(), 0.0F);
  EXPECT_FLOAT_EQ(result.ul_window_bler_stats.get_max(), 0.5F);
  EXPECT_FLOAT_EQ(result.ul_window_bler_stats.get_mean(), 0.25F);
  EXPECT_EQ(result.dl_window_bler_stats.get_nof_observations(), 0U);

  // A sparse window keeps filling until it has enough transport blocks.
  send_crcs(0, 3);
  run_slots(100);
  send_crcs(3, 0);
  get_next_metric();
  const auto& sparse = metrics_notif.last_report.ue_metrics[0];
  ASSERT_EQ(sparse.ul_window_bler_stats.get_nof_observations(), 1U);
  EXPECT_FLOAT_EQ(sparse.ul_window_bler_stats.get_mean(), 0.5F);
}

TEST_F(scheduler_metrics_handler_tester, compute_bitrate)
{
  units::bytes dl_tbs{test_rng::uniform_int<unsigned>(1, 1000)};
  units::bytes ul_tbs{test_rng::uniform_int<unsigned>(1, 1000)};

  // Slot with bytes ACKed.
  metrics.handle_dl_harq_ack(test_ue_index, true, dl_tbs);
  ul_crc_pdu_indication crc_pdu;
  crc_pdu.rnti           = to_rnti(0x4601);
  crc_pdu.ue_index       = test_ue_index;
  crc_pdu.tb_crc_success = true;
  metrics.handle_crc_indication(next_sl_tx.without_hyper_sfn() - 1, crc_pdu, ul_tbs);

  this->get_next_metric();
  unsigned             msec_elapsed = metrics_notif.last_report.nof_slots * next_sl_tx.nof_slots_per_subframe();
  scheduler_ue_metrics ue_metrics   = metrics_notif.last_report.ue_metrics[0];
  ASSERT_EQ(ue_metrics.rnti, to_rnti(0x4601));
  ASSERT_EQ(ue_metrics.pci, pci_t{0});
  ASSERT_EQ(ue_metrics.dl_brate_kbps, static_cast<double>(dl_tbs.value() * 8) / msec_elapsed);
  ASSERT_EQ(ue_metrics.ul_brate_kbps, static_cast<double>(ul_tbs.value() * 8) / msec_elapsed);

  // Slot with no ACKs.
  this->get_next_metric();
  ue_metrics = metrics_notif.last_report.ue_metrics[0];
  ASSERT_EQ(ue_metrics.rnti, to_rnti(0x4601));
  ASSERT_EQ(ue_metrics.dl_brate_kbps, 0) << "DL bitrate not reset";
  ASSERT_EQ(ue_metrics.ul_brate_kbps, 0) << "UL bitrate not reset";

  // Slot with bytes NACKed.
  metrics.handle_dl_harq_ack(test_ue_index, false, dl_tbs);
  crc_pdu.rnti           = to_rnti(0x4601);
  crc_pdu.ue_index       = test_ue_index;
  crc_pdu.tb_crc_success = false;
  metrics.handle_crc_indication(next_sl_tx.without_hyper_sfn() - 1, crc_pdu, ul_tbs);

  this->get_next_metric();
  ue_metrics = metrics_notif.last_report.ue_metrics[0];
  ASSERT_EQ(ue_metrics.rnti, to_rnti(0x4601));
  ASSERT_EQ(ue_metrics.dl_brate_kbps, 0) << "NACKs should not count for bitrate";
  ASSERT_EQ(ue_metrics.ul_brate_kbps, 0) << "NACKs should not count for bitrate";
}

TEST_F(scheduler_metrics_handler_tester, compute_latency_metric)
{
  using usecs                = std::chrono::microseconds;
  std::vector<usecs> samples = {usecs{10}, usecs{50}, usecs{150}, usecs{300}, usecs{500}};
  samples.resize(report_period.count());

  // Discard first report, as it may have not enough slots.
  get_next_metric();
  metrics_notif.last_report = {};

  sched_result sched_res;
  sched_res.dl.nof_dl_symbols = 14;
  sched_res.ul.nof_ul_symbols = 14;

  for (unsigned i = 0; i != samples.size(); ++i) {
    ASSERT_TRUE(metrics_notif.last_report.ue_metrics.empty());
    this->run_slot(sched_res, samples[i]);
  }
  ASSERT_FALSE(metrics_notif.last_report.ue_metrics.empty());
  scheduler_cell_metrics cell_metrics = metrics_notif.last_report;

  unsigned expected_avg = std::accumulate(samples.begin(), samples.end(), usecs{0}).count() / report_period.count();
  ASSERT_EQ(cell_metrics.average_decision_latency.count(), expected_avg);

  for (unsigned i = 0; i != cell_metrics.latency_histogram.size(); ++i) {
    unsigned expected = std::count_if(samples.begin(), samples.end(), [i](usecs u) {
      unsigned bin = u.count() / scheduler_cell_metrics::nof_usec_per_bin;
      bin          = std::min(bin, scheduler_cell_metrics::latency_hist_bins - 1);
      return bin == i;
    });
    ASSERT_EQ(cell_metrics.latency_histogram[i], expected);
  }
}

TEST_F(scheduler_metrics_handler_tester, when_ue_is_removed_mid_period_then_its_last_metrics_are_still_reported)
{
  unsigned nof_acks  = test_rng::uniform_int<unsigned>(1, 100);
  unsigned nof_nacks = test_rng::uniform_int<unsigned>(1, 100);

  ul_crc_pdu_indication crc_pdu;
  crc_pdu.rnti     = to_rnti(0x4601);
  crc_pdu.ue_index = test_ue_index;

  crc_pdu.tb_crc_success = true;
  for (unsigned i = 0; i != nof_acks; ++i) {
    metrics.handle_crc_indication(next_sl_tx.without_hyper_sfn() - 1, crc_pdu, units::bytes{1});
  }
  crc_pdu.tb_crc_success = false;
  for (unsigned i = 0; i != nof_nacks; ++i) {
    metrics.handle_crc_indication(next_sl_tx.without_hyper_sfn() - 1, crc_pdu, units::bytes{1});
  }

  // Remove the UE before the periodic report is generated.
  metrics.handle_ue_deletion(test_ue_index);

  // The next report must still carry the metrics accumulated by the removed UE.
  this->get_next_metric();
  ASSERT_EQ(metrics_notif.last_report.ue_metrics.size(), 1);
  scheduler_ue_metrics ue_metrics = metrics_notif.last_report.ue_metrics[0];
  ASSERT_EQ(ue_metrics.rnti, to_rnti(0x4601));
  ASSERT_EQ(ue_metrics.ul_nof_ok, nof_acks);
  ASSERT_EQ(ue_metrics.ul_nof_nok, nof_nacks);

  // The removed UE must not be reported again on subsequent reports.
  metrics_notif.last_report = {};
  sched_result sched_res;
  sched_res.dl.nof_dl_symbols = 14;
  sched_res.ul.nof_ul_symbols = 14;
  for (unsigned i = 0; i != report_period.count() * next_sl_tx.nof_slots_per_subframe(); ++i) {
    run_slot(sched_res);
  }
  ASSERT_TRUE(metrics_notif.last_report.ue_metrics.empty());
}

TEST_F(scheduler_metrics_handler_tester, compute_error_indications)
{
  metrics.handle_error_indication();
  metrics.handle_error_indication();
  this->get_next_metric();
  ASSERT_EQ(metrics_notif.last_report.nof_error_indications, 2);
  this->get_next_metric();
  ASSERT_EQ(metrics_notif.last_report.nof_error_indications, 0);
}

TEST_F(scheduler_metrics_handler_tester, failed_pdcch_allocs_are_accumulated_per_dl_ul_and_common)
{
  // Discard first report, as it may have not enough slots.
  get_next_metric();
  metrics_notif.last_report = {};

  // Per-slot failures: two DL (one of which in a common SearchSpace) and one common UL.
  sched_result sched_res;
  sched_res.dl.nof_dl_symbols               = 14;
  sched_res.ul.nof_ul_symbols               = 14;
  sched_res.failed_attempts.dl_pdcch        = 2;
  sched_res.failed_attempts.common_dl_pdcch = 1;
  sched_res.failed_attempts.ul_pdcch        = 1;
  sched_res.failed_attempts.common_ul_pdcch = 1;

  for (unsigned i = 0; i != report_period.count() and metrics_notif.last_report.nof_slots == 0; ++i) {
    run_slot(sched_res);
  }

  const scheduler_cell_metrics& report = metrics_notif.last_report;
  ASSERT_GT(report.nof_slots, 0);
  ASSERT_EQ(report.failed_dl_pdcch, report.nof_slots * 2);
  ASSERT_EQ(report.failed_common_dl_pdcch, report.nof_slots * 1);
  ASSERT_EQ(report.failed_ul_pdcch, report.nof_slots * 1);
  ASSERT_EQ(report.failed_common_ul_pdcch, report.nof_slots * 1);

  // Counters reset on the next report.
  this->get_next_metric();
  ASSERT_EQ(metrics_notif.last_report.failed_dl_pdcch, 0);
  ASSERT_EQ(metrics_notif.last_report.failed_common_dl_pdcch, 0);
  ASSERT_EQ(metrics_notif.last_report.failed_ul_pdcch, 0);
  ASSERT_EQ(metrics_notif.last_report.failed_common_ul_pdcch, 0);
}
