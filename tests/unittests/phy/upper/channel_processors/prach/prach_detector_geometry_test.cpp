// SPDX-FileCopyrightText: Copyright (C) 2021-2026 DeepSig Inc
// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

/// \file
/// \brief Tests for the PRACH detector geometry, and in particular for the IDFT batch count.
///
/// The accelerated detector prepares its backend for the batch count a configuration asks for, and preparing it
/// compiles a kernel. Getting that count wrong is not a detection error - the results stay correct - it just means
/// the backend was prepared for a workload the cell never runs and has to compile again, on the slot path, when the
/// real one arrives. These tests pin down which parts of the configuration the count follows, so that a warm-up
/// cannot quietly prepare the wrong shape. No GPU is involved.
///
/// The zero correlation zone dominates: it is what lets preambles share a root sequence, so a cell that configures
/// a zone needs a small fraction of the root sequences that a cell without one does.

#include "channel_processors/prach/prach_detector_geometry.h"
#include <gtest/gtest.h>
#include <set>

using namespace ocudu;
using namespace ocudu::detail;

namespace {

/// Long-preamble configuration covering all 64 preambles, the shape a contention-based occasion asks for.
prach_detector::configuration make_long_config()
{
  return {.root_sequence_index   = 0,
          .format                = prach_format_type::zero,
          .restricted_set        = restricted_set_config::UNRESTRICTED,
          .zero_correlation_zone = 0,
          .start_preamble_index  = 0,
          .nof_preamble_indices  = 64,
          .ra_scs                = prach_subcarrier_spacing::kHz1_25,
          .nof_rx_ports          = 1,
          .slot                  = slot_point(0, 0, 0)};
}

prach_detector::configuration make_short_config()
{
  prach_detector::configuration config = make_long_config();
  config.format                        = prach_format_type::B4;
  config.ra_scs                        = prach_subcarrier_spacing::kHz30;
  return config;
}

unsigned nof_idft_batches(const prach_detector::configuration& config)
{
  return get_nof_idft_batches(config, get_prach_detector_geometry(config));
}

} // namespace

TEST(prach_detector_geometry, long_format_zcz_zero_uses_one_root_per_preamble)
{
  const prach_detector::configuration config   = make_long_config();
  const prach_detector_geometry       geometry = get_prach_detector_geometry(config);

  // No cyclic shift multiplexing: every preamble needs its own root sequence.
  EXPECT_EQ(geometry.n_cs, 0U);
  EXPECT_EQ(geometry.nof_shifts, 1U);
  EXPECT_EQ(geometry.nof_sequences, 64U);
  EXPECT_EQ(geometry.active_nof_sequences, 64U);
  EXPECT_EQ(geometry.dft_size, 1024U);
  EXPECT_EQ(geometry.sequence_length, prach_constants::LONG_SEQUENCE_LENGTH);
}

TEST(prach_detector_geometry, zero_correlation_zone_drives_the_batch_count)
{
  // The axis that matters most. Zone 0 is the largest shape there is - 64 root sequences - and every configured
  // zone needs a fraction of that, so a warm-up that assumes zone 0 prepares a plan no cell with a zone can use.
  for (const prach_detector::configuration& zone_zero : {make_long_config(), make_short_config()}) {
    for (unsigned zcz : {1U, 5U, 8U, 12U}) {
      prach_detector::configuration config = zone_zero;
      config.zero_correlation_zone         = zcz;
      EXPECT_LT(nof_idft_batches(config), nof_idft_batches(zone_zero))
          << "format " << to_string(config.format) << ", zero correlation zone " << zcz
          << " must not share a plan with zone 0";
    }
  }
}

TEST(prach_detector_geometry, symbol_combining_sets_format_two_apart)
{
  // Format 2 is the one long format whose symbols are not combined before the detection, so it correlates every
  // symbol separately and needs four times the batches of format 0 at the same zone.
  for (unsigned zcz : {0U, 1U, 8U}) {
    prach_detector::configuration format_zero = make_long_config();
    format_zero.zero_correlation_zone         = zcz;
    prach_detector::configuration format_two  = format_zero;
    format_two.format                         = prach_format_type::two;

    EXPECT_TRUE(get_prach_detector_geometry(format_zero).combine_symbols);
    EXPECT_FALSE(get_prach_detector_geometry(format_two).combine_symbols);
    EXPECT_GT(nof_idft_batches(format_two), nof_idft_batches(format_zero)) << "zero correlation zone " << zcz;
  }
}

TEST(prach_detector_geometry, long_format_three_uses_its_own_cyclic_shift_table)
{
  // Format 3 is the long format that runs at 5 kHz, which is a different cyclic shift table, so it needs a
  // different number of root sequences than the other long formats at the same zone.
  prach_detector::configuration format_zero  = make_long_config();
  format_zero.zero_correlation_zone          = 8;
  prach_detector::configuration format_three = format_zero;
  format_three.format                        = prach_format_type::three;
  format_three.ra_scs                        = prach_subcarrier_spacing::kHz5;

  EXPECT_NE(get_prach_detector_geometry(format_three).n_cs, get_prach_detector_geometry(format_zero).n_cs);
  EXPECT_NE(nof_idft_batches(format_three), nof_idft_batches(format_zero));
}

TEST(prach_detector_geometry, long_formats_do_not_all_share_one_plan)
{
  std::set<unsigned> batch_counts;
  for (prach_format_type format :
       {prach_format_type::zero, prach_format_type::one, prach_format_type::two, prach_format_type::three}) {
    prach_detector::configuration config = make_long_config();
    config.format                        = format;
    config.zero_correlation_zone         = 8;
    config.ra_scs =
        (format == prach_format_type::three) ? prach_subcarrier_spacing::kHz5 : prach_subcarrier_spacing::kHz1_25;
    batch_counts.insert(nof_idft_batches(config));
  }
  EXPECT_GT(batch_counts.size(), 1U) << "the long formats must not collapse onto a single plan";
}

TEST(prach_detector_geometry, batch_count_scales_with_receive_ports)
{
  prach_detector::configuration one_port = make_long_config();
  prach_detector::configuration four     = make_long_config();
  four.nof_rx_ports                      = 4;

  EXPECT_EQ(nof_idft_batches(four), 4U * nof_idft_batches(one_port));
}

TEST(prach_detector_geometry, contention_free_preamble_subset_shrinks_the_batch_count)
{
  // A contention-free occasion asks for a slice of the preamble space, which needs fewer root sequences than the
  // contention-based occasion next to it. The two are different workloads for the backend.
  prach_detector::configuration all_preambles = make_long_config();
  prach_detector::configuration subset        = make_long_config();
  subset.start_preamble_index                 = 8;
  subset.nof_preamble_indices                 = 8;

  EXPECT_LT(nof_idft_batches(subset), nof_idft_batches(all_preambles));
}

TEST(prach_detector_geometry, long_and_short_preambles_use_different_transform_sizes)
{
  EXPECT_EQ(get_prach_detector_geometry(make_long_config()).dft_size, 1024U);
  EXPECT_EQ(get_prach_detector_geometry(make_short_config()).dft_size, 256U);
}

TEST(prach_detector_geometry, identical_configurations_agree)
{
  // The warm-up and the live path derive the shape from the same configuration through this function, so equal
  // configurations must give an equal batch count for the prepared plan to be the one the cell then uses.
  EXPECT_EQ(nof_idft_batches(make_long_config()), nof_idft_batches(make_long_config()));
  EXPECT_EQ(nof_idft_batches(make_short_config()), nof_idft_batches(make_short_config()));
}
