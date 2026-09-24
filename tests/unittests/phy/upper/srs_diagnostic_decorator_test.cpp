// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "../../../../lib/phy/upper/diagnostics/srs_diagnostic_decorator.h"
#include "srs_dapp_test_occasions.h"
#include "ocudu/phy/generic_functions/generic_functions_factories.h"
#include "ocudu/phy/support/time_alignment_estimator/time_alignment_estimator_factories.h"
#include "ocudu/phy/upper/diagnostics/phy_diagnostics.h"
#include "ocudu/phy/upper/signal_processors/srs/srs_estimator_factory.h"
#include "ocudu/phy/upper/signal_processors/srs/srs_estimator_result.h"
#include <gtest/gtest.h>
#include <vector>

using namespace ocudu;
using namespace ocudu::srs_dapp_test;

namespace {

/// Keeps a copy of every SRS channel event.
class recording_sink final : public phy_diagnostic_sink
{
public:
  struct event {
    srs_channel_diagnostic header;
    std::vector<cf_t>      coefficients;
    std::vector<cf_t>      pilots;
    std::vector<cf_t>      reference;
  };

  bool should_collect(phy_diagnostic_stream stream, uint16_t) override
  {
    return stream == phy_diagnostic_stream::srs_channel;
  }
  bool is_collect_due(phy_diagnostic_stream stream, uint16_t rnti) override { return should_collect(stream, rnti); }
  void on_pusch_crc(const pusch_crc_diagnostic&) override {}
  void on_pusch_symbols(const pusch_symbols_diagnostic&) override {}
  void on_pusch_dmrs_channel(const pusch_dmrs_channel_diagnostic&) override {}
  void on_srs_channel(const srs_channel_diagnostic& diagnostic) override
  {
    events.push_back({diagnostic,
                      {diagnostic.h_coefficients.begin(), diagnostic.h_coefficients.end()},
                      {diagnostic.received_pilots.begin(), diagnostic.received_pilots.end()},
                      {diagnostic.reference_symbols.begin(), diagnostic.reference_symbols.end()}});
    events.back().header.h_coefficients    = {};
    events.back().header.received_pilots   = {};
    events.back().header.reference_symbols = {};
  }

  std::vector<event> events;
};

TEST(srs_diagnostic_decorator, publishes_the_dapp_input_behind_every_port_pair_estimate)
{
  auto dft = create_dft_processor_factory_fftw_slow();
  if (!dft) {
    dft = create_dft_processor_factory_generic();
  }
  auto sink    = std::make_shared<recording_sink>();
  auto factory = create_srs_diagnostic_decorator_factory(
      create_srs_estimator_generic_factory(create_low_papr_sequence_generator_sw_factory(),
                                           create_time_alignment_estimator_dft_factory(dft),
                                           MAX_NOF_PRBS,
                                           "disabled"),
      sink);
  auto estimator = factory->create();
  ASSERT_NE(estimator, nullptr);

  const occasion_profile profile{
      .nof_rx_ports = 2, .nof_tx_ports = 2, .nof_symbols = srs_nof_symbols::n2, .start_symbol = 12, .delay_s = 0.1e-6};
  const auto config = make_configuration(profile);
  auto       grid   = make_grid(profile);
  ASSERT_TRUE(write_occasion(*grid, profile, config));
  const auto result = estimator->estimate(grid->get_reader(), config);
  ASSERT_EQ(sink->events.size(), 4U);

  auto              generator = create_low_papr_sequence_generator_sw_factory()->create();
  const auto        scs_hz    = 30e3;
  std::vector<cf_t> expected;
  for (const auto& event : sink->events) {
    const unsigned        rx     = event.header.rx_port_index;
    const unsigned        tx     = event.header.tx_port_index;
    const srs_information info   = get_srs_information(config.resource, tx);
    const unsigned        length = info.sequence_length;
    SCOPED_TRACE("rx " + std::to_string(rx) + " tx " + std::to_string(tx));
    ASSERT_EQ(event.coefficients.size(), length);
    ASSERT_EQ(event.pilots.size(), 2U * length);
    ASSERT_EQ(event.reference.size(), length);
    EXPECT_EQ(event.header.mapping_initial_subcarrier, info.mapping_initial_subcarrier);
    EXPECT_EQ(event.header.n_cs, info.n_cs);

    // The pilots are the grid at the port's comb subcarriers, one row per symbol.
    expected.resize(length);
    for (unsigned symbol = 0; symbol != 2; ++symbol) {
      grid->get_reader().get(expected, rx, 12 + symbol, info.mapping_initial_subcarrier, info.comb_size);
      for (unsigned n = 0; n != length; ++n) {
        ASSERT_EQ(event.pilots[symbol * length + n], expected[n]);
      }
    }
    // The reference is the port's sequence with its cyclic shift.
    generator->generate(expected, info.sequence_group, info.sequence_number, info.n_cs, info.n_cs_max);
    for (unsigned n = 0; n != length; ++n) {
      ASSERT_EQ(event.reference[n], expected[n]);
    }
    // The estimate is their symbol-averaged product with the measured time-alignment ramp removed.
    const double step = 2.0 * M_PI * result.time_alignment.time_alignment * scs_hz * info.comb_size;
    for (unsigned n = 0; n != length; ++n) {
      const cf_t average = (event.pilots[n] + event.pilots[length + n]) * 0.5F;
      const cf_t ramp =
          std::polar(1.0F, static_cast<float>(n * step + step * info.mapping_initial_subcarrier / info.comb_size));
      EXPECT_LT(std::abs(event.coefficients[n] - average * std::conj(event.reference[n]) * ramp), 1e-4F);
    }
  }
}

TEST(srs_diagnostic_decorator, interleaved_ports_report_their_own_comb_subcarriers)
{
  auto dft = create_dft_processor_factory_fftw_slow();
  if (!dft) {
    dft = create_dft_processor_factory_generic();
  }
  auto sink      = std::make_shared<recording_sink>();
  auto estimator = create_srs_diagnostic_decorator_factory(
                       create_srs_estimator_generic_factory(create_low_papr_sequence_generator_sw_factory(),
                                                            create_time_alignment_estimator_dft_factory(dft),
                                                            MAX_NOF_PRBS,
                                                            "disabled"),
                       sink)
                       ->create();
  const occasion_profile profile{.nof_rx_ports = 1, .nof_tx_ports = 4, .cyclic_shift = 8};
  const auto             config = make_configuration(profile);
  auto                   grid   = make_grid(profile);
  ASSERT_TRUE(write_occasion(*grid, profile, config));
  estimator->estimate(grid->get_reader(), config);
  ASSERT_EQ(sink->events.size(), 4U);
  EXPECT_EQ(sink->events[0].header.mapping_initial_subcarrier, sink->events[2].header.mapping_initial_subcarrier);
  EXPECT_EQ(sink->events[1].header.mapping_initial_subcarrier, sink->events[3].header.mapping_initial_subcarrier);
  EXPECT_NE(sink->events[0].header.mapping_initial_subcarrier, sink->events[1].header.mapping_initial_subcarrier);
}

} // namespace
