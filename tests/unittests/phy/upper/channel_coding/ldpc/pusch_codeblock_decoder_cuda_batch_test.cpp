/*
 *
 * Copyright 2021-2026 Software Radio Systems Limited
 *
 * By using this file, you agree to the terms and conditions set
 * forth in the LICENSE file which can be found at the top level of
 * the distribution.
 *
 */

/// \file
/// \brief HARQ soft-buffer export of the CUDA batch PUSCH codeblock decoder.
///
/// A GPU-resident first transmission keeps its rate-dematched LLRs on the device. A retransmission of the same
/// transport block is decoded on the host path and soft-combines with the host rate-matching buffer, so the resident
/// decode must be able to seed that buffer. These tests check that export_resident_softbits() reproduces what the host
/// rate dematcher would have written (layout, scale, saturation) so that the subsequent combination is exact.

#include "pusch_codeblock_decoder_cuda_batch.h"
#include "ocudu/phy/upper/channel_coding/channel_coding_factories.h"
#include "ocudu/phy/upper/channel_coding/ldpc/ldpc.h"
#include "ocudu/phy/upper/codeblock_metadata.h"
#include "ocudu/phy/upper/log_likelihood_ratio.h"
#include <cuda_fp16.h>
#include <cuda_runtime.h>
#include <gtest/gtest.h>
#include <random>
#include <vector>

using namespace ocudu;

namespace {

struct export_test_params {
  ldpc_base_graph_type bg;
  unsigned             Z;
  modulation_scheme    mod;
  /// Rate-matched length as a fraction (percent) of the short codeword length.
  unsigned E_percent;
};

std::ostream& operator<<(std::ostream& os, const export_test_params& p)
{
  return os << "BG" << ((p.bg == ldpc_base_graph_type::BG1) ? 1 : 2) << "_Z" << p.Z << "_Qm"
            << get_bits_per_symbol(p.mod) << "_E" << p.E_percent;
}

class PuschCodeblockDecoderCudaBatchExportTest : public ::testing::TestWithParam<export_test_params>
{
protected:
  static std::unique_ptr<pusch_codeblock_decoder_cuda_batch> make_decoder(unsigned max_prb = 51)
  {
    auto dematcher_factory = create_ldpc_rate_dematcher_factory_sw("auto");
    auto crc_factory       = create_crc_calculator_factory_sw("auto");
    if (!dematcher_factory || !crc_factory) {
      return nullptr;
    }
    pusch_codeblock_decoder_cuda_batch::sch_crc crcs;
    crcs.crc16  = crc_factory->create(crc_generator_poly::CRC16);
    crcs.crc24A = crc_factory->create(crc_generator_poly::CRC24A);
    crcs.crc24B = crc_factory->create(crc_generator_poly::CRC24B);
    // Every existing export case fits a 51-PRB SISO carrier; preserve all BG/Z/Qm/E cases.
    return std::make_unique<pusch_codeblock_decoder_cuda_batch>(dematcher_factory->create(), std::move(crcs), 0,
                                                               max_prb, 1);
  }

  /// Uploads host int8 LLRs as device fp16 LLRs on the demodulator scale (host = 4 x device).
  static void* upload_as_half(const std::vector<log_likelihood_ratio>& llrs)
  {
    std::vector<__half> half(llrs.size());
    for (size_t i = 0; i != llrs.size(); ++i) {
      half[i] = __float2half(static_cast<float>(llrs[i].to_int()) / 4.0F);
    }
    void* device = nullptr;
    if (cudaMalloc(&device, half.size() * sizeof(__half)) != cudaSuccess) {
      return nullptr;
    }
    if (cudaMemcpy(device, half.data(), half.size() * sizeof(__half), cudaMemcpyHostToDevice) != cudaSuccess) {
      cudaFree(device);
      return nullptr;
    }
    return device;
  }
};

} // namespace

TEST_P(PuschCodeblockDecoderCudaBatchExportTest, ExportMatchesHostDematcherAndCombines)
{
  const export_test_params& params = GetParam();

  auto decoder = make_decoder();
  ASSERT_NE(decoder, nullptr);
  auto host_dematcher = create_ldpc_rate_dematcher_factory_sw("auto")->create();
  ASSERT_NE(host_dematcher, nullptr);

  // Single codeblock, no filler bits, RV 0, unlimited buffer.
  const unsigned bg_K        = (params.bg == ldpc_base_graph_type::BG1) ? 22 : 10;
  const unsigned K           = bg_K * params.Z;
  const unsigned N_full      = ldpc::compute_full_codeblock_size(params.bg, units::bits(K)).value();
  const unsigned N_short     = N_full - 2 * params.Z; // Codeword without the punctured systematic bits.
  const unsigned Qm          = get_bits_per_symbol(params.mod);
  unsigned       E           = (N_short * params.E_percent) / 100;
  E                          = (E / Qm) * Qm;
  const unsigned tb_crc_bits = (K - 24 > 3824) ? 24 : 16;
  const unsigned tbs_bits    = K - tb_crc_bits;
  ASSERT_EQ(tbs_bits % 8, 0U);

  codeblock_metadata cfg  = {};
  cfg.tb_common.base_graph   = params.bg;
  cfg.tb_common.lifting_size = static_cast<ldpc::lifting_size_t>(params.Z);
  cfg.tb_common.rv           = 0;
  cfg.tb_common.mod          = params.mod;
  cfg.tb_common.Nref         = 0;
  cfg.tb_common.cw_length    = E;
  cfg.cb_specific.full_length     = N_full;
  cfg.cb_specific.rm_length       = E;
  cfg.cb_specific.nof_filler_bits = 0;
  cfg.cb_specific.cw_offset       = 0;
  cfg.cb_specific.nof_crc_bits    = tb_crc_bits;

  // Random soft bits: the transport block is noise, so the resident decode fails and the export path is exercised.
  std::mt19937                                   rng(0x5eed + params.Z);
  std::uniform_int_distribution<int>             dist(-40, 40);
  std::vector<log_likelihood_ratio>              first(E);
  std::vector<log_likelihood_ratio>              second(E);
  for (unsigned i = 0; i != E; ++i) {
    first[i]  = log_likelihood_ratio(static_cast<int8_t>(dist(rng)));
    second[i] = log_likelihood_ratio(static_cast<int8_t>(dist(rng)));
  }

  // Host reference: what the CPU dematcher writes for a new transmission.
  std::vector<log_likelihood_ratio> host_buffer(N_full, log_likelihood_ratio(0));
  host_dematcher->rate_dematch(host_buffer, first, true, cfg);

  // Device: resident decode of the first transmission, then export into a buffer pre-filled with garbage.
  void* d_llrs = upload_as_half(first);
  ASSERT_NE(d_llrs, nullptr);
  std::vector<uint8_t>              tb(tbs_bits / 8);
  std::vector<codeblock_metadata::cb_specific_metadata> cb_specific = {cfg.cb_specific};
  auto result = decoder->decode_resident_softbits(d_llrs,
                                                  E,
                                                  nullptr,
                                                  cfg.tb_common,
                                                  cb_specific,
                                                  tb,
                                                  crc_generator_poly::CRC24A,
                                                  6,
                                                  nullptr,
                                                  -1);
  EXPECT_FALSE(result.tb_crc_ok) << "random soft bits are not expected to decode";

  // Out-of-envelope input must not expose a previous decode's resident softbits,
  // touch caller output, or submit work using the deliberately null device pointer.
  const auto capacity = *make_pusch_decoder_capacity(51, 1);
  std::vector<uint8_t> rejected_output(tb.size(), 0xA5);
  auto rejected = decoder->decode_resident_softbits(nullptr, capacity.codeword_llrs + 1, nullptr,
                                                    cfg.tb_common, cb_specific, rejected_output,
                                                    crc_generator_poly::CRC24A, 6, nullptr, -1);
  EXPECT_FALSE(rejected.tb_crc_ok);
  EXPECT_FALSE(rejected.all_cb_crcs_ok);
  EXPECT_EQ(rejected.nof_iterations, 0U);
  EXPECT_EQ(rejected_output, std::vector<uint8_t>(tb.size(), 0xA5));
  std::vector<log_likelihood_ratio> rejected_export(N_full, log_likelihood_ratio(77));
  std::vector<span<log_likelihood_ratio>> rejected_buffers = {rejected_export};
  EXPECT_FALSE(decoder->export_resident_softbits(rejected_buffers));
  EXPECT_EQ(rejected_export, std::vector<log_likelihood_ratio>(N_full, log_likelihood_ratio(77)));

  const auto expect_rejected = [&](size_t count,
                                   const std::vector<codeblock_metadata::cb_specific_metadata>& metadata,
                                   size_t bytes) {
    std::vector<uint8_t> output(bytes, 0xA5);
    auto invalid = decoder->decode_resident_softbits(nullptr, count, nullptr, cfg.tb_common, metadata, output,
                                                     crc_generator_poly::CRC24A, 6, nullptr, -1);
    EXPECT_FALSE(invalid.tb_crc_ok);
    EXPECT_FALSE(invalid.all_cb_crcs_ok);
    EXPECT_EQ(invalid.nof_iterations, 0U);
    EXPECT_EQ(output, std::vector<uint8_t>(bytes, 0xA5));
    EXPECT_FALSE(decoder->export_resident_softbits(rejected_buffers));
  };
  expect_rejected(E, {}, tb.size());
  expect_rejected(E - 1, cb_specific, tb.size());
  expect_rejected(E, cb_specific, capacity.tb_bytes);
  auto invalid_metadata = cb_specific;
  invalid_metadata[0].rm_length = 0;
  expect_rejected(E, invalid_metadata, tb.size());
  invalid_metadata[0].rm_length = capacity.codeword_llrs + 1;
  expect_rejected(capacity.codeword_llrs + 1, invalid_metadata, tb.size());
  invalid_metadata[0].rm_length = 8;
  invalid_metadata.resize(capacity.codeblocks + 1, invalid_metadata[0]);
  expect_rejected(invalid_metadata.size() * 8, invalid_metadata, tb.size());

  // Host-input admission also rejects before copying or dematching, while
  // retaining one failed result per requested CB (the caller indexes each).
  std::vector<described_rx_codeblock> described = {{first, cfg}};
  std::vector<bit_buffer> data_buffers = {bit_buffer::from_bytes(rejected_output)};
  const auto expect_host_rejected = [&]() {
    auto failures = decoder->decode_batch(described, data_buffers, rejected_buffers, true,
                                          crc_generator_poly::CRC24A, true, 6);
    ASSERT_EQ(failures.size(), described.size());
    for (const auto& failure : failures) {
      EXPECT_FALSE(failure.crc_ok);
      EXPECT_EQ(failure.nof_iterations, 0U);
    }
    EXPECT_EQ(rejected_output, std::vector<uint8_t>(tb.size(), 0xA5));
    EXPECT_EQ(rejected_export, std::vector<log_likelihood_ratio>(N_full, log_likelihood_ratio(77)));
  };
  described[0].second.cb_specific.rm_length = E + 1;
  expect_host_rejected();
  std::vector<log_likelihood_ratio> oversized(capacity.codeword_llrs + 1, log_likelihood_ratio(1));
  described[0].first = oversized;
  described[0].second.cb_specific.rm_length = oversized.size();
  expect_host_rejected();
  described[0].first = span<const log_likelihood_ratio>(first).first(8);
  described[0].second.cb_specific.rm_length = 8;
  described.resize(capacity.codeblocks + 1, described[0]);
  data_buffers.resize(described.size(), data_buffers[0]);
  rejected_buffers.resize(described.size(), rejected_buffers[0]);
  expect_host_rejected();

  // Recover on the same instance. The usual numerical export/HARQ checks below
  // now also prove a rejected grant did not corrupt the subsequent valid decode.
  result = decoder->decode_resident_softbits(d_llrs, E, nullptr, cfg.tb_common, cb_specific, tb,
                                             crc_generator_poly::CRC24A, 6, nullptr, -1);
  EXPECT_FALSE(result.tb_crc_ok);

  std::vector<log_likelihood_ratio> exported(N_full, log_likelihood_ratio(77));
  std::vector<span<log_likelihood_ratio>> buffers = {exported};
  ASSERT_TRUE(decoder->export_resident_softbits(buffers));
  // A second export must refuse: the decode buffers rotate.
  EXPECT_FALSE(decoder->export_resident_softbits(buffers));

  unsigned mismatches = 0;
  int      max_diff   = 0;
  for (unsigned i = 0; i != N_full; ++i) {
    const int diff = std::abs(exported[i].to_int() - host_buffer[i].to_int());
    max_diff       = std::max(max_diff, diff);
    if (diff > 1) {
      ++mismatches;
    }
  }
  EXPECT_EQ(mismatches, 0U) << "exported soft bits differ from the host dematcher by more than one LSB (max "
                            << max_diff << ")";

  // Retransmission: combining on the exported buffer must equal combining on the host buffer.
  host_dematcher->rate_dematch(host_buffer, second, false, cfg);
  host_dematcher->rate_dematch(exported, second, false, cfg);
  unsigned combine_mismatches = 0;
  for (unsigned i = 0; i != N_full; ++i) {
    if (std::abs(exported[i].to_int() - host_buffer[i].to_int()) > 1) {
      ++combine_mismatches;
    }
  }
  EXPECT_EQ(combine_mismatches, 0U);

  cudaFree(d_llrs);
}

INSTANTIATE_TEST_SUITE_P(PuschCodeblockDecoderCudaBatch,
                         PuschCodeblockDecoderCudaBatchExportTest,
                         ::testing::Values(export_test_params{ldpc_base_graph_type::BG1, 384, modulation_scheme::QPSK, 100},
                                           export_test_params{ldpc_base_graph_type::BG1, 384, modulation_scheme::QAM64, 60},
                                           export_test_params{ldpc_base_graph_type::BG1, 208, modulation_scheme::QAM16, 80},
                                           export_test_params{ldpc_base_graph_type::BG2, 384, modulation_scheme::QPSK, 100},
                                           export_test_params{ldpc_base_graph_type::BG2, 160, modulation_scheme::QAM256, 50}),
                         ::testing::PrintToStringParamName());
