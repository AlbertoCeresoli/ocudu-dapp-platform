// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
#include "pusch_decoder_capacity.h"
#include <gtest/gtest.h>
#include <limits>

using namespace ocudu;

TEST(pusch_decoder_capacity, configured_geometry_not_independent_maximum_cross_product)
{
  auto small = make_pusch_decoder_capacity(4, 1);
  ASSERT_TRUE(small.has_value());
  EXPECT_EQ(small->codeword_llrs, 5248U);
  EXPECT_EQ(small->codeblocks, 2U);
  EXPECT_EQ(small->tb_bytes, 659U);
  EXPECT_EQ(small->codeword_llrs * sizeof(float), 20992U);
  auto sector = make_pusch_decoder_capacity(51, 1);
  ASSERT_TRUE(sector.has_value());
  EXPECT_EQ(sector->codeword_llrs, 66912U);
  EXPECT_EQ(sector->codeblocks, 18U);
  auto largest = make_pusch_decoder_capacity(MAX_NOF_PRBS, 4);
  ASSERT_TRUE(largest.has_value());
  EXPECT_EQ(largest->codeword_llrs, MAX_NOF_PRBS * 164U * 4U * 8U);
  EXPECT_LT(largest->codeword_llrs, MAX_NOF_SEGMENTS * 156000U);
  EXPECT_LE(largest->codeblocks, MAX_NOF_SEGMENTS);
}

TEST(pusch_decoder_capacity, invalid_dimensions_reject_before_multiplication)
{
  for (unsigned invalid : {0U, static_cast<unsigned>(MAX_NOF_PRBS + 1), std::numeric_limits<unsigned>::max()}) {
    EXPECT_FALSE(make_pusch_decoder_capacity(invalid, 1));
  }
  for (unsigned invalid : {0U, 5U, std::numeric_limits<unsigned>::max()}) {
    EXPECT_FALSE(make_pusch_decoder_capacity(51, invalid));
  }
}

TEST(pusch_decoder_capacity, every_prb_and_layer_covers_both_segmentation_graphs)
{
  for (unsigned layers = 1; layers <= 4; ++layers) {
    unsigned previous_bits = 0;
    unsigned previous_cbs  = 0;
    for (unsigned prb = 1; prb <= MAX_NOF_PRBS; ++prb) {
      auto capacity = make_pusch_decoder_capacity(prb, layers);
      ASSERT_TRUE(capacity);
      EXPECT_GT(capacity->codeword_llrs, previous_bits);
      EXPECT_GE(capacity->codeblocks, previous_cbs);
      EXPECT_EQ(capacity->codeword_llrs, prb * 164U * layers * 8U);
      // Independent conservative segment-payload inequality, including TB CRC.
      EXPECT_GE(capacity->codeblocks * (3840U - 24U), capacity->codeword_llrs + 24U);
      EXPECT_GE(capacity->tb_bytes * 8U, capacity->codeword_llrs + 24U);
      for (auto bg : {ldpc_base_graph_type::BG1, ldpc_base_graph_type::BG2}) {
        for (unsigned tbs : {24U, 256U, 3824U, 3840U, 8424U, 8448U, capacity->codeword_llrs - 24,
                             capacity->codeword_llrs}) {
          if (tbs <= capacity->codeword_llrs) {
            EXPECT_LE(compute_nof_codeblocks(units::bits(tbs), bg), capacity->codeblocks);
          }
        }
      }
      previous_bits = capacity->codeword_llrs;
      previous_cbs  = capacity->codeblocks;
    }
  }
}

TEST(pusch_decoder_capacity, changing_grants_fit_and_out_of_capacity_totals_reject)
{
  auto capacity = *make_pusch_decoder_capacity(51, 1);
  for (unsigned prb = 1; prb <= 51; ++prb) {
    for (unsigned qm : {2U, 4U, 6U, 8U}) {
      unsigned bits = prb * 164U * qm;
      EXPECT_TRUE(capacity.accepts(compute_nof_codeblocks(units::bits(bits), ldpc_base_graph_type::BG2), bits));
    }
  }
  EXPECT_FALSE(capacity.accepts(0, 100));
  EXPECT_FALSE(capacity.accepts(1, 0));
  EXPECT_FALSE(capacity.accepts(capacity.codeblocks + 1, 100));
  EXPECT_FALSE(capacity.accepts(1, capacity.codeword_llrs + 1));
  EXPECT_FALSE(capacity.accepts(1, 100, capacity.tb_bytes + 1));
  EXPECT_FALSE(capacity.accepts(1, std::numeric_limits<std::size_t>::max()));
  EXPECT_FALSE(capacity.accepts(1, 100, std::numeric_limits<std::size_t>::max()));
}
