// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "../../../../lib/ofh/receiver/ofh_uplane_iq_data_fragment_assembler.h"
#include "ocudu/ofh/compression/compression_properties.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <numeric>
#include <vector>

using namespace ocudu;
using namespace ofh;

namespace {

constexpr slot_point            test_slot = {1, 2, 3};
constexpr ru_compression_params bfp9      = {compression_type::BFP, 9};
constexpr unsigned              symbol    = 4;
constexpr unsigned              port      = 1;
constexpr unsigned              nof_prbs  = 51;

std::vector<uint8_t> make_payload(unsigned payload_nof_prbs)
{
  unsigned             nof_bytes = payload_nof_prbs * get_compressed_prb_size(bfp9).value();
  std::vector<uint8_t> payload(nof_bytes);
  std::iota(payload.begin(), payload.end(), uint8_t{0});
  return payload;
}

} // namespace

TEST(ofh_uplane_iq_data_fragment_assembler_test, assembles_in_order_fragments)
{
  uplane_iq_data_fragment_assembler assembler;
  std::vector<uint8_t>               payload  = make_payload(nof_prbs);
  unsigned                           prb_size = get_compressed_prb_size(bfp9).value();

  ASSERT_EQ(uplane_iq_data_fragment_assembler::result::incomplete,
            assembler.add_fragment(test_slot,
                                   symbol,
                                   port,
                                   nof_prbs,
                                   0,
                                   25,
                                   span<const uint8_t>(payload).first(25 * prb_size),
                                   bfp9));
  ASSERT_EQ(uplane_iq_data_fragment_assembler::result::complete,
            assembler.add_fragment(test_slot,
                                   symbol,
                                   port,
                                   nof_prbs,
                                   25,
                                   26,
                                   span<const uint8_t>(payload).subspan(25 * prb_size, 26 * prb_size),
                                   bfp9));
  ASSERT_TRUE(std::equal(payload.begin(), payload.end(), assembler.get_assembled_data().begin()));
}

TEST(ofh_uplane_iq_data_fragment_assembler_test, assembles_out_of_order_fragments)
{
  uplane_iq_data_fragment_assembler assembler;
  std::vector<uint8_t>               payload  = make_payload(nof_prbs);
  unsigned                           prb_size = get_compressed_prb_size(bfp9).value();

  ASSERT_EQ(uplane_iq_data_fragment_assembler::result::incomplete,
            assembler.add_fragment(test_slot,
                                   symbol,
                                   port,
                                   nof_prbs,
                                   25,
                                   26,
                                   span<const uint8_t>(payload).subspan(25 * prb_size, 26 * prb_size),
                                   bfp9));
  ASSERT_EQ(uplane_iq_data_fragment_assembler::result::complete,
            assembler.add_fragment(test_slot,
                                   symbol,
                                   port,
                                   nof_prbs,
                                   0,
                                   25,
                                   span<const uint8_t>(payload).first(25 * prb_size),
                                   bfp9));
  ASSERT_TRUE(std::equal(payload.begin(), payload.end(), assembler.get_assembled_data().begin()));
}

TEST(ofh_uplane_iq_data_fragment_assembler_test, rejects_overlapping_and_short_fragments)
{
  uplane_iq_data_fragment_assembler assembler;
  std::vector<uint8_t>               payload  = make_payload(nof_prbs);
  unsigned                           prb_size = get_compressed_prb_size(bfp9).value();

  ASSERT_EQ(uplane_iq_data_fragment_assembler::result::incomplete,
            assembler.add_fragment(test_slot,
                                   symbol,
                                   port,
                                   nof_prbs,
                                   0,
                                   25,
                                   span<const uint8_t>(payload).first(25 * prb_size),
                                   bfp9));
  ASSERT_EQ(uplane_iq_data_fragment_assembler::result::invalid,
            assembler.add_fragment(test_slot,
                                   symbol,
                                   port,
                                   nof_prbs,
                                   24,
                                   1,
                                   span<const uint8_t>(payload).subspan(24 * prb_size, prb_size),
                                   bfp9));
  ASSERT_EQ(uplane_iq_data_fragment_assembler::result::invalid,
            assembler.add_fragment(test_slot,
                                   symbol,
                                   port,
                                   nof_prbs,
                                   0,
                                   nof_prbs,
                                   span<const uint8_t>(payload).first(payload.size() - 1),
                                   bfp9));
}

TEST(ofh_uplane_iq_data_fragment_assembler_test, a_new_symbol_replaces_an_incomplete_ring_entry)
{
  uplane_iq_data_fragment_assembler assembler;
  std::vector<uint8_t>               payload  = make_payload(nof_prbs);
  unsigned                           prb_size = get_compressed_prb_size(bfp9).value();

  ASSERT_EQ(uplane_iq_data_fragment_assembler::result::incomplete,
            assembler.add_fragment(test_slot,
                                   symbol,
                                   port,
                                   nof_prbs,
                                   0,
                                   1,
                                   span<const uint8_t>(payload).first(prb_size),
                                   bfp9));
  ASSERT_EQ(uplane_iq_data_fragment_assembler::result::complete,
            assembler.add_fragment(slot_point{1, 2, 4}, symbol, port, nof_prbs, 0, nof_prbs, payload, bfp9));
  ASSERT_TRUE(std::equal(payload.begin(), payload.end(), assembler.get_assembled_data().begin()));
}
