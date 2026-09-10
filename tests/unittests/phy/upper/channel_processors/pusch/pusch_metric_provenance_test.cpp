// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "pusch_demodulation_metrics.h"
#include "pusch_processor_notifier_adaptor.h"
#include "pusch_sinr_readback.h"
#include <gtest/gtest.h>
#include <limits>

using namespace ocudu;

namespace {
using sinr_type         = channel_state_information::sinr_type;
constexpr float missing = -std::numeric_limits<float>::infinity();

class result_sink : public pusch_processor_result_notifier, public pusch_processor_csi_part1_feedback
{
public:
  pusch_processor_result_data last;
  unsigned                    calls = 0;
  void                        on_sch(const pusch_processor_result_data& data) override
  {
    last = data;
    ++calls;
  }
  void on_uci(const pusch_processor_result_control&) override {}
  void on_csi_part1(const uci_payload_type&) override {}
};

pusch_processor_result_data report(sinr_type                                             type,
                                   const pusch_demodulator_notifier::demodulation_stats& stats,
                                   bool                                                  provisional,
                                   std::optional<float>                                  prior_ch_est = std::nullopt)
{
  result_sink                      sink;
  pusch_processor_notifier_adaptor adaptor;
  channel_state_information        csi(type);
  if (prior_ch_est) {
    csi.set_sinr_dB(sinr_type::channel_estimator, *prior_ch_est);
  }
  adaptor.new_transmission(sink, sink, csi);
  if (provisional) {
    adaptor.get_demodulator_notifier().on_provisional_stats(3, stats);
  } else {
    adaptor.get_demodulator_notifier().on_end_stats(stats);
  }
  pusch_decoder_result decoded;
  decoded.tb_crc_ok = true;
  adaptor.get_sch_data_notifier().on_sch_data(decoded);
  EXPECT_EQ(sink.calls, 1U);
  EXPECT_TRUE(sink.last.data.tb_crc_ok);
  return sink.last;
}
} // namespace

TEST(pusch_metric_provenance, post_eq_does_not_overwrite_or_fabricate_channel_estimator)
{
  pusch_demodulator_notifier::demodulation_stats stats;
  stats.sinr_dB = 25.0F;
  for (bool provisional : {false, true}) {
    EXPECT_EQ(report(sinr_type::channel_estimator, stats, provisional, 7.0F).csi.get_sinr_dB(), 7.0F);
    EXPECT_FALSE(report(sinr_type::channel_estimator, stats, provisional).csi.get_sinr_dB());
    EXPECT_EQ(report(sinr_type::post_equalization, stats, provisional, 7.0F).csi.get_sinr_dB(), 25.0F);
  }
}

TEST(pusch_metric_provenance, independent_sources_survive_both_notification_forms)
{
  const auto stats = make_pusch_e2e_demodulation_stats(11.0F, 4.0F, -23.0F, -25.0F, 1.0e-6F, 81.0F, 0.1F);
  for (bool provisional : {false, true}) {
    EXPECT_EQ(report(sinr_type::channel_estimator, stats, provisional, 7.0F).csi.get_sinr_dB(), 4.0F);
    EXPECT_EQ(report(sinr_type::post_equalization, stats, provisional).csi.get_sinr_dB(), 11.0F);
    const auto result = report(sinr_type::evm, stats, provisional);
    ASSERT_TRUE(result.csi.get_sinr_dB());
    EXPECT_NEAR(*result.csi.get_sinr_dB(), 16.3F, 1.0e-5F);
    EXPECT_EQ(result.csi.get_epre_dB(), -23.0F);
    EXPECT_EQ(result.csi.get_rsrp_dB(), -25.0F);
    EXPECT_EQ(result.csi.get_cfo_Hz(), 81.0F);
    EXPECT_EQ(result.csi.get_total_evm(), 0.1F);
    ASSERT_TRUE(result.csi.get_time_alignment());
    EXPECT_EQ(*result.csi.get_time_alignment(), phy_time_unit::from_seconds(static_cast<double>(1.0e-6F)));
  }
}

TEST(pusch_metric_provenance, missing_sources_never_become_high_sinr_or_cross_source_fallback)
{
  for (float absent : {missing, -missing, std::numeric_limits<float>::quiet_NaN()}) {
    const auto stats = make_pusch_e2e_demodulation_stats(absent, absent, absent, absent, absent, absent, 0.01F);
    EXPECT_FALSE(stats.sinr_dB);
    EXPECT_FALSE(stats.channel_estimator_sinr_dB);
    EXPECT_FALSE(stats.epre_dB);
    EXPECT_FALSE(stats.rsrp_dB);
    EXPECT_FALSE(stats.time_alignment_s);
    EXPECT_FALSE(stats.cfo_Hz);
    for (bool provisional : {false, true}) {
      EXPECT_FALSE(report(sinr_type::channel_estimator, stats, provisional).csi.get_sinr_dB());
      EXPECT_FALSE(report(sinr_type::post_equalization, stats, provisional).csi.get_sinr_dB());
    }
  }
  for (float evm : {missing, -missing, std::numeric_limits<float>::quiet_NaN(), 0.0F, -0.1F}) {
    EXPECT_FALSE(make_pusch_e2e_demodulation_stats(3, 4, 5, 6, 0, 0, evm).evm);
  }
  const auto pilot_only = make_pusch_e2e_demodulation_stats(missing, 12, missing, missing, missing, missing, missing);
  EXPECT_FALSE(pilot_only.sinr_dB);
  EXPECT_EQ(pilot_only.channel_estimator_sinr_dB, 12.0F);
}

TEST(pusch_metric_provenance, new_grant_clears_previous_demodulation_metrics)
{
  result_sink                      sink;
  pusch_processor_notifier_adaptor adaptor;
  for (auto type : {sinr_type::channel_estimator, sinr_type::post_equalization, sinr_type::evm}) {
    adaptor.new_transmission(sink, sink, channel_state_information(type));
    adaptor.get_demodulator_notifier().on_end_stats(make_pusch_e2e_demodulation_stats(11, 4, -23, -25, 0, 81, 0.1F));
    adaptor.get_sch_data_notifier().on_sch_data({});
    ASSERT_TRUE(sink.last.csi.get_sinr_dB());
    adaptor.new_transmission(sink, sink, channel_state_information(type));
    adaptor.get_demodulator_notifier().on_end_stats({});
    adaptor.get_sch_data_notifier().on_sch_data({});
    EXPECT_FALSE(sink.last.csi.get_sinr_dB());
    EXPECT_FALSE(sink.last.csi.get_epre_dB());
    EXPECT_FALSE(sink.last.csi.get_rsrp_dB());
    EXPECT_FALSE(sink.last.csi.get_time_alignment());
    EXPECT_FALSE(sink.last.csi.get_cfo_Hz());
    EXPECT_FALSE(sink.last.csi.get_total_evm());
  }
}

TEST(pusch_metric_provenance, post_eq_accumulator_rejects_invalid_measurements)
{
  EXPECT_FLOAT_EQ(detail::pusch_post_eq_sinr_from_accumulator(1.0F, 100), 20.0F);
  EXPECT_FLOAT_EQ(detail::pusch_post_eq_sinr_from_accumulator(100.0F, 1), -20.0F);
  EXPECT_EQ(detail::pusch_post_eq_sinr_from_accumulator(1.0F, 0), missing);
  for (float invalid : {0.0F, -1.0F, missing, -missing, std::numeric_limits<float>::quiet_NaN()}) {
    EXPECT_EQ(detail::pusch_post_eq_sinr_from_accumulator(invalid, 100), missing);
  }
  EXPECT_EQ(detail::pusch_post_eq_sinr_from_accumulator(std::numeric_limits<float>::denorm_min(), 100), missing);
}
