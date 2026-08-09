// SPDX-FileCopyrightText: Copyright (C) 2021-2026 DeepSig Inc
// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "iq_compression_cuda.h"
#include "ofh_compression.h"
#include "phy_acceleration_resource_grid_factory.h"
#include "ocudu/ocudulog/ocudulog.h"
#include "ocudu/ofh/compression/compression_factory.h"
#include "ocudu/ofh/compression/compression_properties.h"
#include "ocudu/phy/support/resource_grid_reader.h"
#include "gtest/gtest.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cuda_runtime.h>
#include <random>
#include <thread>

using namespace ocudu;

namespace {

std::vector<cbf16_t> generate_iq(unsigned nof_prbs)
{
  std::mt19937                          rgen(0);
  std::uniform_real_distribution<float> dist(-0.75F, 0.75F);

  std::vector<cbf16_t> data(nof_prbs * NOF_SUBCARRIERS_PER_RB);
  for (cbf16_t& sample : data) {
    sample = cbf16_t(dist(rgen), dist(rgen));
  }
  return data;
}

std::vector<cbf16_t> generate_golden_iq_prb()
{
  static constexpr std::array<float, NOF_SUBCARRIERS_PER_RB> re = {
      0.5F, -0.5F, 0.25F, -0.25F, 0.125F, -0.125F, 0.0625F, -0.0625F, 0.0F, 0.375F, -0.375F, 0.03125F};
  static constexpr std::array<float, NOF_SUBCARRIERS_PER_RB> im = {
      -0.25F, 0.25F, -0.5F, 0.5F, -0.0625F, 0.0625F, -0.125F, 0.125F, 0.0F, -0.03125F, 0.03125F, 0.375F};

  std::vector<cbf16_t> data(NOF_SUBCARRIERS_PER_RB);
  for (unsigned i = 0; i != NOF_SUBCARRIERS_PER_RB; ++i) {
    data[i] = cbf16_t(re[i], im[i]);
  }
  return data;
}

uint32_t bit_mask(unsigned width)
{
  return (width == 16U) ? 0xffffU : ((1U << width) - 1U);
}

void append_msb_first_bits(std::vector<uint8_t>& out, unsigned& bit_pos, uint16_t value, unsigned width)
{
  for (unsigned i = 0; i != width; ++i) {
    if ((value & (1U << (width - 1U - i))) != 0) {
      out[bit_pos / 8U] |= static_cast<uint8_t>(1U << (7U - (bit_pos % 8U)));
    }
    ++bit_pos;
  }
}

std::vector<int16_t> quantize_iq_samples(span<const cbf16_t> iq_data, unsigned quantization_width, float iq_scale)
{
  const float          gain = static_cast<float>((1U << (quantization_width - 1U)) - 1U) * iq_scale;
  std::vector<int16_t> quantized(iq_data.size() * 2U);

  for (unsigned i = 0; i != iq_data.size(); ++i) {
    quantized[2U * i]      = static_cast<int16_t>(std::round(to_float(iq_data[i].real) * gain));
    quantized[2U * i + 1U] = static_cast<int16_t>(std::round(to_float(iq_data[i].imag) * gain));
  }

  return quantized;
}

std::vector<uint8_t> pack_samples_msb_first(span<const int16_t> samples, unsigned data_width, unsigned prefix_bytes = 0)
{
  std::vector<uint8_t> packed(prefix_bytes + ((samples.size() * data_width + 7U) / 8U), 0U);
  unsigned             bit_pos = prefix_bytes * 8U;
  for (int16_t sample : samples) {
    append_msb_first_bits(packed, bit_pos, static_cast<uint16_t>(sample) & bit_mask(data_width), data_width);
  }
  return packed;
}

unsigned determine_bfp_exponent_independent(unsigned max_abs, unsigned data_width)
{
  const unsigned max_shift       = ofh::MAX_IQ_WIDTH - data_width;
  unsigned       lz_without_sign = max_shift;

  if ((max_abs > 0U) && (max_shift > 0U)) {
    unsigned bits = 0;
    for (unsigned value = max_abs; value != 0U; value >>= 1U) {
      ++bits;
    }
    lz_without_sign = 15U - bits;
  }

  return max_shift - std::min(max_shift, lz_without_sign);
}

std::vector<uint8_t> expected_none_prb(span<const cbf16_t> iq_data, unsigned data_width, float iq_scale)
{
  return pack_samples_msb_first(quantize_iq_samples(iq_data, data_width, iq_scale), data_width);
}

std::vector<uint8_t> expected_bfp_prb(span<const cbf16_t> iq_data, unsigned data_width, float iq_scale)
{
  std::vector<int16_t> quantized = quantize_iq_samples(iq_data, ofh::Q_BIT_WIDTH, iq_scale);

  const auto [min_it, max_it] = std::minmax_element(quantized.begin(), quantized.end());
  unsigned max_abs            = static_cast<unsigned>(std::max(std::abs(*max_it), std::abs(*min_it) - 1));
  unsigned exponent           = determine_bfp_exponent_independent(max_abs, data_width);

  std::vector<int16_t> compressed(quantized.size());
  std::transform(quantized.begin(), quantized.end(), compressed.begin(), [exponent](int16_t sample) {
    return static_cast<int16_t>(sample >> exponent);
  });

  std::vector<uint8_t> packed = pack_samples_msb_first(compressed, data_width, 1U);
  packed[0]                   = static_cast<uint8_t>(exponent);
  return packed;
}

class ofh_iq_compression_cuda_test : public ::testing::TestWithParam<ofh::compression_type>
{
protected:
  ocudulog::basic_logger& logger = ocudulog::fetch_basic_logger("TEST", false);

  void SetUp() override { logger.set_level(ocudulog::basic_levels::none); }
};

} // namespace

TEST_P(ofh_iq_compression_cuda_test, cuda_matches_cpu_reference)
{
  static constexpr unsigned nof_prbs = 273;
  static constexpr float    iq_scale = 0.27F;

  ofh::compression_type type = GetParam();
  auto                  data = generate_iq(nof_prbs);

  std::unique_ptr<ofh::iq_compressor>   cpu_compressor   = ofh::create_iq_compressor(type, logger, iq_scale, "generic");
  std::unique_ptr<ofh::iq_decompressor> cpu_decompressor = ofh::create_iq_decompressor(type, logger, "generic");
  std::unique_ptr<ofh::iq_compressor>   cuda_compressor  = ofh::create_iq_compressor(type, logger, iq_scale, "cuda");
  std::unique_ptr<ofh::iq_decompressor> cuda_decompressor = ofh::create_iq_decompressor(type, logger, "cuda");

  for (unsigned data_width : {8U, 9U, 10U, 12U, 14U, 16U}) {
    ofh::ru_compression_params params;
    params.type       = type;
    params.data_width = data_width;

    unsigned             prb_size = ofh::get_compressed_prb_size(params).value();
    std::vector<uint8_t> cpu_compressed(nof_prbs * prb_size);
    std::vector<uint8_t> cuda_compressed(nof_prbs * prb_size);
    std::vector<cbf16_t> cpu_decompressed(data.size());
    std::vector<cbf16_t> cuda_decompressed(data.size());

    cpu_compressor->compress(cpu_compressed, data, params);
    cuda_compressor->compress(cuda_compressed, data, params);
    ASSERT_EQ(cpu_compressed, cuda_compressed)
        << "compression type=" << ofh::to_string(type) << " width=" << data_width;

    cpu_decompressor->decompress(cpu_decompressed, cpu_compressed, params);
    cuda_decompressor->decompress(cuda_decompressed, cpu_compressed, params);
    ASSERT_EQ(cpu_decompressed, cuda_decompressed)
        << "decompression type=" << ofh::to_string(type) << " width=" << data_width;
  }
}

TEST_P(ofh_iq_compression_cuda_test, cuda_matches_cpu_reference_large_batch)
{
  // Large enough to select the RE-parallel decompression and warp compression kernels (see
  // DECOMPRESS_*_RE_PARALLEL_PRB_THRESHOLD) that slot-sized symbol batches use in production.
  // The CPU reference is sized for single-symbol calls, so it processes the batch in chunks
  // while the CUDA implementation handles the whole batch in one call.
  static constexpr unsigned chunk_prbs = 273;
  static constexpr unsigned nof_chunks = 16;
  static constexpr unsigned nof_prbs   = chunk_prbs * nof_chunks;
  static constexpr float    iq_scale   = 0.27F;

  ofh::compression_type type = GetParam();
  auto                  data = generate_iq(nof_prbs);

  std::unique_ptr<ofh::iq_compressor>   cpu_compressor   = ofh::create_iq_compressor(type, logger, iq_scale, "generic");
  std::unique_ptr<ofh::iq_decompressor> cpu_decompressor = ofh::create_iq_decompressor(type, logger, "generic");
  std::unique_ptr<ofh::iq_compressor>   cuda_compressor  = ofh::create_iq_compressor(type, logger, iq_scale, "cuda");
  std::unique_ptr<ofh::iq_decompressor> cuda_decompressor = ofh::create_iq_decompressor(type, logger, "cuda");

  for (unsigned data_width : {8U, 9U, 10U, 12U, 14U, 16U}) {
    ofh::ru_compression_params params;
    params.type       = type;
    params.data_width = data_width;

    unsigned             prb_size = ofh::get_compressed_prb_size(params).value();
    std::vector<uint8_t> cpu_compressed(nof_prbs * prb_size);
    std::vector<uint8_t> cuda_compressed(nof_prbs * prb_size);
    std::vector<cbf16_t> cpu_decompressed(data.size());
    std::vector<cbf16_t> cuda_decompressed(data.size());

    for (unsigned chunk = 0; chunk != nof_chunks; ++chunk) {
      span<const cbf16_t> chunk_data(data.data() + chunk * chunk_prbs * NOF_SUBCARRIERS_PER_RB,
                                     chunk_prbs * NOF_SUBCARRIERS_PER_RB);
      span<uint8_t>       chunk_out(cpu_compressed.data() + chunk * chunk_prbs * prb_size, chunk_prbs * prb_size);
      cpu_compressor->compress(chunk_out, chunk_data, params);
    }
    cuda_compressor->compress(cuda_compressed, data, params);
    ASSERT_EQ(cpu_compressed, cuda_compressed)
        << "compression type=" << ofh::to_string(type) << " width=" << data_width;

    for (unsigned chunk = 0; chunk != nof_chunks; ++chunk) {
      span<const uint8_t> chunk_in(cpu_compressed.data() + chunk * chunk_prbs * prb_size, chunk_prbs * prb_size);
      span<cbf16_t>       chunk_out(cpu_decompressed.data() + chunk * chunk_prbs * NOF_SUBCARRIERS_PER_RB,
                              chunk_prbs * NOF_SUBCARRIERS_PER_RB);
      cpu_decompressor->decompress(chunk_out, chunk_in, params);
    }
    cuda_decompressor->decompress(cuda_decompressed, cpu_compressed, params);
    ASSERT_EQ(cpu_decompressed, cuda_decompressed)
        << "decompression type=" << ofh::to_string(type) << " width=" << data_width;
  }
}

INSTANTIATE_TEST_SUITE_P(ofh_iq_compression_cuda,
                         ofh_iq_compression_cuda_test,
                         ::testing::Values(ofh::compression_type::none, ofh::compression_type::BFP));

TEST(ofh_iq_compression_cuda_golden_test, none_12bit_matches_independent_byte_layout)
{
  static constexpr unsigned data_width = 12;
  static constexpr float    iq_scale   = 1.0F;

  ocudulog::basic_logger& logger = ocudulog::fetch_basic_logger("TEST", false);
  logger.set_level(ocudulog::basic_levels::none);

  std::vector<cbf16_t> data = generate_golden_iq_prb();
  std::vector<uint8_t> expected =
      expected_none_prb(span<const cbf16_t>(data.data(), data.size()), data_width, iq_scale);
  ofh::ru_compression_params params{ofh::compression_type::none, data_width};
  ASSERT_EQ(expected.size(), ofh::get_compressed_prb_size(params).value());

  std::unique_ptr<ofh::iq_compressor> cpu_compressor =
      ofh::create_iq_compressor(ofh::compression_type::none, logger, iq_scale, "generic");
  std::unique_ptr<ofh::iq_compressor> cuda_compressor =
      ofh::create_iq_compressor(ofh::compression_type::none, logger, iq_scale, "cuda");
  ASSERT_NE(cpu_compressor, nullptr);
  ASSERT_NE(cuda_compressor, nullptr);

  std::vector<uint8_t> cpu_compressed(expected.size());
  std::vector<uint8_t> cuda_compressed(expected.size());
  cpu_compressor->compress(cpu_compressed, data, params);
  cuda_compressor->compress(cuda_compressed, data, params);
  EXPECT_EQ(cpu_compressed, expected);
  EXPECT_EQ(cuda_compressed, expected);

  ocudu_ofh_compression_handle_t* handle = nullptr;
  ASSERT_NE(ocudu_ofh_compression_create(&handle), 0);
  std::vector<uint8_t> c_api_compressed(expected.size());
  ASSERT_NE(ocudu_ofh_compress(
                handle, OCUDU_OFH_COMPRESSION_TYPE_NONE, c_api_compressed.data(), data.data(), 1, data_width, iq_scale),
            0);
  EXPECT_EQ(c_api_compressed, expected);
  ocudu_ofh_compression_destroy(handle);
}

TEST(ofh_iq_compression_cuda_golden_test, bfp_9bit_matches_independent_exponent_and_byte_layout)
{
  static constexpr unsigned data_width = 9;
  static constexpr float    iq_scale   = 1.0F;

  ocudulog::basic_logger& logger = ocudulog::fetch_basic_logger("TEST", false);
  logger.set_level(ocudulog::basic_levels::none);

  std::vector<cbf16_t> data     = generate_golden_iq_prb();
  std::vector<uint8_t> expected = expected_bfp_prb(span<const cbf16_t>(data.data(), data.size()), data_width, iq_scale);
  ofh::ru_compression_params params{ofh::compression_type::BFP, data_width};
  ASSERT_EQ(expected.size(), ofh::get_compressed_prb_size(params).value());
  ASSERT_EQ(expected.front(), 7U);

  std::unique_ptr<ofh::iq_compressor> cpu_compressor =
      ofh::create_iq_compressor(ofh::compression_type::BFP, logger, iq_scale, "generic");
  std::unique_ptr<ofh::iq_compressor> cuda_compressor =
      ofh::create_iq_compressor(ofh::compression_type::BFP, logger, iq_scale, "cuda");
  ASSERT_NE(cpu_compressor, nullptr);
  ASSERT_NE(cuda_compressor, nullptr);

  std::vector<uint8_t> cpu_compressed(expected.size());
  std::vector<uint8_t> cuda_compressed(expected.size());
  cpu_compressor->compress(cpu_compressed, data, params);
  cuda_compressor->compress(cuda_compressed, data, params);
  EXPECT_EQ(cpu_compressed, expected);
  EXPECT_EQ(cuda_compressed, expected);

  ocudu_ofh_compression_handle_t* handle = nullptr;
  ASSERT_NE(ocudu_ofh_compression_create(&handle), 0);
  std::vector<uint8_t> c_api_compressed(expected.size());
  ASSERT_NE(ocudu_ofh_compress(
                handle, OCUDU_OFH_COMPRESSION_TYPE_BFP, c_api_compressed.data(), data.data(), 1, data_width, iq_scale),
            0);
  EXPECT_EQ(c_api_compressed, expected);
  ocudu_ofh_compression_destroy(handle);
}

TEST(ofh_iq_compression_cuda_c_api_test, rejects_invalid_arguments_and_requests)
{
  std::vector<cbf16_t> input_cbf16 = generate_iq(1);
  std::vector<cbf16_t> output_cbf16(input_cbf16.size());
  std::vector<uint8_t> bytes(64);

  EXPECT_EQ(ocudu_ofh_compression_create(nullptr), 0);
  EXPECT_EQ(ocudu_ofh_compression_get_stream(nullptr), nullptr);
  EXPECT_EQ(ocudu_ofh_compression_synchronize(nullptr), 0);
  ocudu_ofh_compression_error_info_t error_info = {};
  EXPECT_EQ(ocudu_ofh_compression_get_last_error(nullptr, &error_info), 0);
  ocudu_ofh_compression_destroy(nullptr);

  ocudu_ofh_compression_handle_t* handle = nullptr;
  if (ocudu_ofh_compression_create(&handle) == 0) {
    GTEST_SKIP() << "CUDA OFH compression backend is not available.";
  }
  ASSERT_NE(handle, nullptr);

  EXPECT_EQ(ocudu_ofh_compress(nullptr, OCUDU_OFH_COMPRESSION_TYPE_BFP, bytes.data(), input_cbf16.data(), 1, 9, 0.27F),
            0);
  EXPECT_EQ(ocudu_ofh_compress(handle, OCUDU_OFH_COMPRESSION_TYPE_BFP, nullptr, input_cbf16.data(), 1, 9, 0.27F), 0);
  EXPECT_EQ(ocudu_ofh_compress(handle, OCUDU_OFH_COMPRESSION_TYPE_BFP, bytes.data(), nullptr, 1, 9, 0.27F), 0);
  EXPECT_EQ(ocudu_ofh_compress(handle, 99, bytes.data(), input_cbf16.data(), 1, 9, 0.27F), 0);
  EXPECT_EQ(ocudu_ofh_compress(handle, OCUDU_OFH_COMPRESSION_TYPE_BFP, bytes.data(), input_cbf16.data(), 1, 0, 0.27F),
            0);
  EXPECT_EQ(ocudu_ofh_compress(handle, OCUDU_OFH_COMPRESSION_TYPE_BFP, bytes.data(), input_cbf16.data(), 1, 17, 0.27F),
            0);
  EXPECT_EQ(ocudu_ofh_decompress(handle, OCUDU_OFH_COMPRESSION_TYPE_BFP, output_cbf16.data(), bytes.data(), 1, 17), 0);
  ASSERT_NE(ocudu_ofh_compression_get_last_error(handle, &error_info), 0);
  EXPECT_EQ(error_info.cuda_error_code, static_cast<int>(cudaErrorInvalidValue));
  EXPECT_STREQ(error_info.operation, "validate decompression request");
  EXPECT_STREQ(error_info.error_name, cudaGetErrorName(cudaErrorInvalidValue));
  EXPECT_STREQ(error_info.error_description, cudaGetErrorString(cudaErrorInvalidValue));

  EXPECT_EQ(ocudu_ofh_compress_device_grid(handle,
                                           OCUDU_OFH_COMPRESSION_TYPE_BFP,
                                           bytes.data(),
                                           input_cbf16.data(),
                                           1,
                                           NOF_SUBCARRIERS_PER_RB,
                                           0,
                                           1,
                                           0,
                                           1,
                                           9,
                                           0.27F),
            0);
  EXPECT_EQ(ocudu_ofh_compress_device_grid(handle,
                                           OCUDU_OFH_COMPRESSION_TYPE_BFP,
                                           bytes.data(),
                                           input_cbf16.data(),
                                           1,
                                           NOF_SUBCARRIERS_PER_RB + 1,
                                           0,
                                           0,
                                           0,
                                           1,
                                           9,
                                           0.27F),
            0);
  EXPECT_EQ(ocudu_ofh_compress_device_grid_ports(handle,
                                                 OCUDU_OFH_COMPRESSION_TYPE_BFP,
                                                 bytes.data(),
                                                 bytes.size(),
                                                 input_cbf16.data(),
                                                 1,
                                                 NOF_SUBCARRIERS_PER_RB,
                                                 0,
                                                 0,
                                                 0,
                                                 0,
                                                 1,
                                                 9,
                                                 0.27F),
            0);

  std::array<void*, 2> host_buffers = {bytes.data(), nullptr};
  EXPECT_EQ(ocudu_ofh_compress_device_grid_ports_to_host_buffers(handle,
                                                                 OCUDU_OFH_COMPRESSION_TYPE_BFP,
                                                                 host_buffers.data(),
                                                                 host_buffers.size(),
                                                                 bytes.size(),
                                                                 input_cbf16.data(),
                                                                 1,
                                                                 NOF_SUBCARRIERS_PER_RB,
                                                                 0,
                                                                 2,
                                                                 0,
                                                                 0,
                                                                 1,
                                                                 9,
                                                                 0.27F),
            0);

  EXPECT_EQ(ocudu_ofh_compress_device_grid_symbol_batch(handle,
                                                        OCUDU_OFH_COMPRESSION_TYPE_BFP,
                                                        bytes.data(),
                                                        bytes.size(),
                                                        bytes.size(),
                                                        input_cbf16.data(),
                                                        1,
                                                        NOF_SUBCARRIERS_PER_RB,
                                                        0,
                                                        1,
                                                        1,
                                                        1,
                                                        0,
                                                        1,
                                                        9,
                                                        0.27F),
            0);

  ocudu_ofh_compression_destroy(handle);
}

TEST(ofh_iq_compression_cuda_c_api_test, accepts_zero_length_noop_requests)
{
  std::vector<cbf16_t> input_cbf16 = generate_iq(1);
  std::vector<cbf16_t> output_cbf16(input_cbf16.size());
  std::vector<uint8_t> bytes(64);

  ocudu_ofh_compression_handle_t* handle = nullptr;
  if (ocudu_ofh_compression_create(&handle) == 0) {
    GTEST_SKIP() << "CUDA OFH compression backend is not available.";
  }
  ASSERT_NE(handle, nullptr);

  EXPECT_NE(ocudu_ofh_compress(handle, OCUDU_OFH_COMPRESSION_TYPE_BFP, bytes.data(), input_cbf16.data(), 0, 9, 0.27F),
            0);
  EXPECT_NE(ocudu_ofh_decompress(handle, OCUDU_OFH_COMPRESSION_TYPE_BFP, output_cbf16.data(), bytes.data(), 0, 9), 0);
  EXPECT_NE(ocudu_ofh_compress_device_grid(handle,
                                           OCUDU_OFH_COMPRESSION_TYPE_BFP,
                                           bytes.data(),
                                           input_cbf16.data(),
                                           1,
                                           NOF_SUBCARRIERS_PER_RB,
                                           0,
                                           0,
                                           0,
                                           0,
                                           9,
                                           0.27F),
            0);
  EXPECT_NE(ocudu_ofh_decompress_to_device_grid_async(handle,
                                                      OCUDU_OFH_COMPRESSION_TYPE_BFP,
                                                      output_cbf16.data(),
                                                      bytes.data(),
                                                      1,
                                                      NOF_SUBCARRIERS_PER_RB,
                                                      0,
                                                      0,
                                                      0,
                                                      0,
                                                      9),
            0);
  EXPECT_NE(ocudu_ofh_decompress_to_device_prach_buffer_async(
                handle, OCUDU_OFH_COMPRESSION_TYPE_BFP, output_cbf16.data(), 0, bytes.data(), 0, 0, 1, 9),
            0);

  ocudu_ofh_compression_destroy(handle);
}

TEST(ofh_iq_compression_cuda_c_api_test, async_prach_decompression_snapshots_transient_none16_input)
{
  static constexpr unsigned nof_prbs   = 1;
  static constexpr unsigned data_width = 16;

  ocudulog::basic_logger& logger = ocudulog::fetch_basic_logger("TEST", false);
  logger.set_level(ocudulog::basic_levels::none);
  ofh::ru_compression_params params{ofh::compression_type::none, data_width};
  std::vector<cbf16_t>       source = generate_golden_iq_prb();
  std::vector<uint8_t>       compressed(ofh::get_compressed_prb_size(params).value() * nof_prbs);
  std::vector<cbf16_t>       expected(source.size());
  ofh::create_iq_compressor(ofh::compression_type::none, logger, 1.0F, "generic")->compress(compressed, source, params);
  ofh::create_iq_decompressor(ofh::compression_type::none, logger, "generic")->decompress(expected, compressed, params);

  ocudu_ofh_compression_handle_t* handle = nullptr;
  if (ocudu_ofh_compression_create(&handle) == 0) {
    GTEST_SKIP() << "CUDA OFH compression backend is not available.";
  }

  cbf16_t* output = nullptr;
  ASSERT_EQ(cudaMallocManaged(&output, expected.size() * sizeof(cbf16_t)), cudaSuccess);
  std::fill(output, output + expected.size(), cbf16_t{0.0F, 0.0F});

  // Prime the device buffers and every pinned staging slot before blocking
  // the stream. CUDA allocation APIs may synchronize outstanding work; this
  // test is intended to isolate caller-buffer lifetime after enqueue, not
  // allocation behavior behind a deliberately blocked host callback.
  for (unsigned slot = 0; slot != 4; ++slot) {
    ASSERT_NE(ocudu_ofh_decompress_to_device_prach_buffer_async(handle,
                                                                OCUDU_OFH_COMPRESSION_TYPE_NONE,
                                                                output,
                                                                0,
                                                                compressed.data(),
                                                                0,
                                                                expected.size(),
                                                                nof_prbs,
                                                                data_width),
              0);
    ASSERT_NE(ocudu_ofh_compression_synchronize(handle), 0);
  }
  std::fill(output, output + expected.size(), cbf16_t{0.0F, 0.0F});

  struct callback_gate {
    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};
  } gate;
  cudaStream_t stream = static_cast<cudaStream_t>(ocudu_ofh_compression_get_stream(handle));
  ASSERT_NE(stream, nullptr);
  ASSERT_EQ(cudaLaunchHostFunc(
                stream,
                [](void* data) {
                  auto& callback = *static_cast<callback_gate*>(data);
                  callback.entered.store(true, std::memory_order_release);
                  while (!callback.release.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                  }
                },
                &gate),
            cudaSuccess);
  while (!gate.entered.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }

  std::atomic<int>  enqueue_status{0};
  std::atomic<bool> enqueue_finished{false};
  std::thread       enqueue_thread([&]() {
    enqueue_status.store(ocudu_ofh_decompress_to_device_prach_buffer_async(handle,
                                                                           OCUDU_OFH_COMPRESSION_TYPE_NONE,
                                                                           output,
                                                                           0,
                                                                           compressed.data(),
                                                                           0,
                                                                           expected.size(),
                                                                           nof_prbs,
                                                                           data_width),
                         std::memory_order_release);
    enqueue_finished.store(true, std::memory_order_release);
  });

  // Model the O-RAN receiver returning and immediately recycling its packet storage while CUDA work is queued.
  while (!enqueue_finished.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
  std::fill(compressed.begin(), compressed.end(), 0U);
  gate.release.store(true, std::memory_order_release);
  enqueue_thread.join();

  ASSERT_NE(enqueue_status.load(std::memory_order_acquire), 0);
  ASSERT_NE(ocudu_ofh_compression_synchronize(handle), 0);
  EXPECT_TRUE(std::equal(expected.begin(), expected.end(), output));

  cudaFree(output);
  ocudu_ofh_compression_destroy(handle);
}

TEST_P(ofh_iq_compression_cuda_test, cuda_device_grid_path_matches_cpu_reference)
{
  static constexpr unsigned grid_nof_prbs = 273;
  static constexpr unsigned nof_prbs      = 64;
  static constexpr unsigned start_prb     = 37;
  static constexpr unsigned nof_symbols   = 14;
  static constexpr unsigned nof_ports     = 4;
  static constexpr unsigned port          = 2;
  static constexpr unsigned symbol        = 9;
  static constexpr float    iq_scale      = 0.27F;

  ofh::compression_type type = GetParam();
  auto                  data = generate_iq(nof_prbs);

  cbf16_t* device_grid = nullptr;
  ASSERT_EQ(cudaMallocManaged(&device_grid,
                              static_cast<size_t>(nof_ports) * nof_symbols * grid_nof_prbs * NOF_SUBCARRIERS_PER_RB *
                                  sizeof(cbf16_t)),
            cudaSuccess);
  cbf16_t* output_grid = nullptr;
  ASSERT_EQ(cudaMallocManaged(&output_grid,
                              static_cast<size_t>(nof_ports) * nof_symbols * grid_nof_prbs * NOF_SUBCARRIERS_PER_RB *
                                  sizeof(cbf16_t)),
            cudaSuccess);

  const size_t grid_offset =
      (static_cast<size_t>(port) * nof_symbols + symbol) * grid_nof_prbs * NOF_SUBCARRIERS_PER_RB +
      start_prb * NOF_SUBCARRIERS_PER_RB;
  std::fill(device_grid,
            device_grid + static_cast<size_t>(nof_ports) * nof_symbols * grid_nof_prbs * NOF_SUBCARRIERS_PER_RB,
            cbf16_t{0.0F, 0.0F});
  std::fill(output_grid,
            output_grid + static_cast<size_t>(nof_ports) * nof_symbols * grid_nof_prbs * NOF_SUBCARRIERS_PER_RB,
            cbf16_t{0.0F, 0.0F});
  std::copy(data.begin(), data.end(), device_grid + grid_offset);

  std::unique_ptr<ofh::iq_compressor>   cpu_compressor   = ofh::create_iq_compressor(type, logger, iq_scale, "generic");
  std::unique_ptr<ofh::iq_decompressor> cpu_decompressor = ofh::create_iq_decompressor(type, logger, "generic");

  ocudu_ofh_compression_handle_t* handle = nullptr;
  ASSERT_NE(ocudu_ofh_compression_create(&handle), 0);

  for (unsigned data_width : {8U, 9U, 10U, 12U, 14U, 16U}) {
    ofh::ru_compression_params params;
    params.type       = type;
    params.data_width = data_width;

    unsigned             prb_size = ofh::get_compressed_prb_size(params).value();
    std::vector<uint8_t> cpu_compressed(nof_prbs * prb_size);
    std::vector<uint8_t> cuda_compressed(nof_prbs * prb_size);
    std::vector<cbf16_t> cpu_decompressed(data.size());

    cpu_compressor->compress(cpu_compressed, data, params);
    ASSERT_NE(ocudu_ofh_compress_device_grid(handle,
                                             (type == ofh::compression_type::BFP) ? OCUDU_OFH_COMPRESSION_TYPE_BFP
                                                                                  : OCUDU_OFH_COMPRESSION_TYPE_NONE,
                                             cuda_compressed.data(),
                                             device_grid,
                                             nof_symbols,
                                             grid_nof_prbs * NOF_SUBCARRIERS_PER_RB,
                                             port,
                                             symbol,
                                             start_prb,
                                             nof_prbs,
                                             data_width,
                                             iq_scale),
              0);
    ASSERT_EQ(cpu_compressed, cuda_compressed)
        << "compression type=" << ofh::to_string(type) << " width=" << data_width;

    cpu_decompressor->decompress(cpu_decompressed, cpu_compressed, params);
    std::fill(output_grid,
              output_grid + static_cast<size_t>(nof_ports) * nof_symbols * grid_nof_prbs * NOF_SUBCARRIERS_PER_RB,
              cbf16_t{0.0F, 0.0F});
    ASSERT_NE(ocudu_ofh_decompress_to_device_grid(handle,
                                                  (type == ofh::compression_type::BFP)
                                                      ? OCUDU_OFH_COMPRESSION_TYPE_BFP
                                                      : OCUDU_OFH_COMPRESSION_TYPE_NONE,
                                                  output_grid,
                                                  cpu_compressed.data(),
                                                  nof_symbols,
                                                  grid_nof_prbs * NOF_SUBCARRIERS_PER_RB,
                                                  port,
                                                  symbol,
                                                  start_prb,
                                                  nof_prbs,
                                                  data_width),
              0);
    ASSERT_TRUE(std::equal(cpu_decompressed.begin(), cpu_decompressed.end(), output_grid + grid_offset));

    std::fill(output_grid,
              output_grid + static_cast<size_t>(nof_ports) * nof_symbols * grid_nof_prbs * NOF_SUBCARRIERS_PER_RB,
              cbf16_t{0.0F, 0.0F});
    ASSERT_NE(ocudu_ofh_decompress_to_device_grid_async(handle,
                                                        (type == ofh::compression_type::BFP)
                                                            ? OCUDU_OFH_COMPRESSION_TYPE_BFP
                                                            : OCUDU_OFH_COMPRESSION_TYPE_NONE,
                                                        output_grid,
                                                        cpu_compressed.data(),
                                                        nof_symbols,
                                                        grid_nof_prbs * NOF_SUBCARRIERS_PER_RB,
                                                        port,
                                                        symbol,
                                                        start_prb,
                                                        nof_prbs,
                                                        data_width),
              0);
    ASSERT_NE(ocudu_ofh_compression_synchronize(handle), 0);
    ASSERT_TRUE(std::equal(cpu_decompressed.begin(), cpu_decompressed.end(), output_grid + grid_offset));

    std::fill(output_grid,
              output_grid + static_cast<size_t>(nof_ports) * nof_symbols * grid_nof_prbs * NOF_SUBCARRIERS_PER_RB,
              cbf16_t{0.0F, 0.0F});
    ASSERT_NE(ocudu_ofh_decompress_to_device_prach_buffer_async(handle,
                                                                (type == ofh::compression_type::BFP)
                                                                    ? OCUDU_OFH_COMPRESSION_TYPE_BFP
                                                                    : OCUDU_OFH_COMPRESSION_TYPE_NONE,
                                                                output_grid,
                                                                grid_offset,
                                                                cpu_compressed.data(),
                                                                0,
                                                                cpu_decompressed.size(),
                                                                nof_prbs,
                                                                data_width),
              0);
    ASSERT_NE(ocudu_ofh_compression_synchronize(handle), 0);
    ASSERT_TRUE(std::equal(cpu_decompressed.begin(), cpu_decompressed.end(), output_grid + grid_offset));
  }

  ocudu_ofh_compression_destroy(handle);
  cudaFree(output_grid);
  cudaFree(device_grid);
}

TEST_P(ofh_iq_compression_cuda_test, cuda_device_grid_port_batch_path_matches_cpu_reference)
{
  static constexpr unsigned grid_nof_prbs = 273;
  static constexpr unsigned nof_prbs      = 64;
  static constexpr unsigned start_prb     = 37;
  static constexpr unsigned nof_symbols   = 14;
  static constexpr unsigned nof_ports     = 4;
  static constexpr unsigned symbol        = 9;
  static constexpr float    iq_scale      = 0.27F;

  ofh::compression_type type = GetParam();

  std::array<std::vector<cbf16_t>, nof_ports> data;
  for (unsigned port = 0; port != nof_ports; ++port) {
    data[port] = generate_iq(nof_prbs);
    for (cbf16_t& sample : data[port]) {
      sample = cbf16_t(to_float(sample.real) * (1.0F - 0.05F * port), to_float(sample.imag) * (1.0F + 0.03F * port));
    }
  }

  cbf16_t* device_grid = nullptr;
  ASSERT_EQ(cudaMallocManaged(&device_grid,
                              static_cast<size_t>(nof_ports) * nof_symbols * grid_nof_prbs * NOF_SUBCARRIERS_PER_RB *
                                  sizeof(cbf16_t)),
            cudaSuccess);
  cbf16_t* output_grid = nullptr;
  ASSERT_EQ(cudaMallocManaged(&output_grid,
                              static_cast<size_t>(nof_ports) * nof_symbols * grid_nof_prbs * NOF_SUBCARRIERS_PER_RB *
                                  sizeof(cbf16_t)),
            cudaSuccess);

  const size_t grid_size = static_cast<size_t>(nof_ports) * nof_symbols * grid_nof_prbs * NOF_SUBCARRIERS_PER_RB;
  std::fill(device_grid, device_grid + grid_size, cbf16_t{0.0F, 0.0F});
  std::fill(output_grid, output_grid + grid_size, cbf16_t{0.0F, 0.0F});
  for (unsigned port = 0; port != nof_ports; ++port) {
    const size_t grid_offset =
        (static_cast<size_t>(port) * nof_symbols + symbol) * grid_nof_prbs * NOF_SUBCARRIERS_PER_RB +
        start_prb * NOF_SUBCARRIERS_PER_RB;
    std::copy(data[port].begin(), data[port].end(), device_grid + grid_offset);
  }

  std::unique_ptr<ofh::iq_compressor>   cpu_compressor   = ofh::create_iq_compressor(type, logger, iq_scale, "generic");
  std::unique_ptr<ofh::iq_decompressor> cpu_decompressor = ofh::create_iq_decompressor(type, logger, "generic");

  ocudu_ofh_compression_handle_t* handle = nullptr;
  ASSERT_NE(ocudu_ofh_compression_create(&handle), 0);

  for (unsigned data_width : {8U, 9U, 10U, 12U, 14U, 16U}) {
    ofh::ru_compression_params params;
    params.type       = type;
    params.data_width = data_width;

    unsigned                                    prb_size    = ofh::get_compressed_prb_size(params).value();
    unsigned                                    port_stride = nof_prbs * prb_size;
    std::vector<uint8_t>                        cuda_compressed(nof_ports * port_stride);
    std::array<std::vector<uint8_t>, nof_ports> host_buffer_compressed;
    std::array<void*, nof_ports>                host_buffers = {};
    for (unsigned port = 0; port != nof_ports; ++port) {
      host_buffer_compressed[port].resize(port_stride);
      host_buffers[port] = host_buffer_compressed[port].data();
    }
    std::vector<uint8_t> device_cuda_compressed(nof_ports * port_stride);
    uint8_t*             device_compressed = nullptr;
    ASSERT_EQ(cudaMalloc(&device_compressed, device_cuda_compressed.size()), cudaSuccess);

    ASSERT_NE(ocudu_ofh_compress_device_grid_ports(handle,
                                                   (type == ofh::compression_type::BFP)
                                                       ? OCUDU_OFH_COMPRESSION_TYPE_BFP
                                                       : OCUDU_OFH_COMPRESSION_TYPE_NONE,
                                                   cuda_compressed.data(),
                                                   port_stride,
                                                   device_grid,
                                                   nof_symbols,
                                                   grid_nof_prbs * NOF_SUBCARRIERS_PER_RB,
                                                   0,
                                                   nof_ports,
                                                   symbol,
                                                   start_prb,
                                                   nof_prbs,
                                                   data_width,
                                                   iq_scale),
              0);
    ASSERT_NE(ocudu_ofh_compress_device_grid_ports_to_host_buffers(handle,
                                                                   (type == ofh::compression_type::BFP)
                                                                       ? OCUDU_OFH_COMPRESSION_TYPE_BFP
                                                                       : OCUDU_OFH_COMPRESSION_TYPE_NONE,
                                                                   host_buffers.data(),
                                                                   host_buffers.size(),
                                                                   port_stride,
                                                                   device_grid,
                                                                   nof_symbols,
                                                                   grid_nof_prbs * NOF_SUBCARRIERS_PER_RB,
                                                                   0,
                                                                   nof_ports,
                                                                   symbol,
                                                                   start_prb,
                                                                   nof_prbs,
                                                                   data_width,
                                                                   iq_scale),
              0);
    for (unsigned port = 0; port != nof_ports; ++port) {
      ASSERT_TRUE(std::equal(host_buffer_compressed[port].begin(),
                             host_buffer_compressed[port].end(),
                             cuda_compressed.data() + static_cast<size_t>(port) * port_stride))
          << "host-buffer compression type=" << ofh::to_string(type) << " width=" << data_width << " port=" << port;
    }
    ASSERT_NE(ocudu_ofh_compress_device_grid_ports_to_device(handle,
                                                             (type == ofh::compression_type::BFP)
                                                                 ? OCUDU_OFH_COMPRESSION_TYPE_BFP
                                                                 : OCUDU_OFH_COMPRESSION_TYPE_NONE,
                                                             device_compressed,
                                                             port_stride,
                                                             device_grid,
                                                             nof_symbols,
                                                             grid_nof_prbs * NOF_SUBCARRIERS_PER_RB,
                                                             0,
                                                             nof_ports,
                                                             symbol,
                                                             start_prb,
                                                             nof_prbs,
                                                             data_width,
                                                             iq_scale),
              0);
    ASSERT_EQ(
        cudaMemcpy(
            device_cuda_compressed.data(), device_compressed, device_cuda_compressed.size(), cudaMemcpyDeviceToHost),
        cudaSuccess);
    ASSERT_EQ(cuda_compressed, device_cuda_compressed)
        << "device compression type=" << ofh::to_string(type) << " width=" << data_width;

    ASSERT_EQ(cudaMemset(device_compressed, 0, device_cuda_compressed.size()), cudaSuccess);
    ASSERT_EQ(cudaDeviceSynchronize(), cudaSuccess);
    ASSERT_NE(ocudu_ofh_compress_device_grid_ports_to_device_async(handle,
                                                                   (type == ofh::compression_type::BFP)
                                                                       ? OCUDU_OFH_COMPRESSION_TYPE_BFP
                                                                       : OCUDU_OFH_COMPRESSION_TYPE_NONE,
                                                                   device_compressed,
                                                                   port_stride,
                                                                   device_grid,
                                                                   nof_symbols,
                                                                   grid_nof_prbs * NOF_SUBCARRIERS_PER_RB,
                                                                   0,
                                                                   nof_ports,
                                                                   symbol,
                                                                   start_prb,
                                                                   nof_prbs,
                                                                   data_width,
                                                                   iq_scale),
              0);
    ASSERT_NE(ocudu_ofh_compression_synchronize(handle), 0);
    std::vector<uint8_t> async_device_cuda_compressed(device_cuda_compressed.size());
    ASSERT_EQ(cudaMemcpy(async_device_cuda_compressed.data(),
                         device_compressed,
                         async_device_cuda_compressed.size(),
                         cudaMemcpyDeviceToHost),
              cudaSuccess);
    ASSERT_EQ(cuda_compressed, async_device_cuda_compressed)
        << "async device compression type=" << ofh::to_string(type) << " width=" << data_width;

    for (unsigned port = 0; port != nof_ports; ++port) {
      std::vector<uint8_t> cpu_compressed(port_stride);
      cpu_compressor->compress(cpu_compressed, data[port], params);
      ASSERT_TRUE(std::equal(cpu_compressed.begin(),
                             cpu_compressed.end(),
                             cuda_compressed.data() + static_cast<size_t>(port) * port_stride))
          << "compression type=" << ofh::to_string(type) << " width=" << data_width << " port=" << port;
    }

    std::fill(output_grid, output_grid + grid_size, cbf16_t{0.0F, 0.0F});
    ASSERT_NE(ocudu_ofh_decompress_to_device_grid_ports(handle,
                                                        (type == ofh::compression_type::BFP)
                                                            ? OCUDU_OFH_COMPRESSION_TYPE_BFP
                                                            : OCUDU_OFH_COMPRESSION_TYPE_NONE,
                                                        output_grid,
                                                        cuda_compressed.data(),
                                                        port_stride,
                                                        nof_symbols,
                                                        grid_nof_prbs * NOF_SUBCARRIERS_PER_RB,
                                                        0,
                                                        nof_ports,
                                                        symbol,
                                                        start_prb,
                                                        nof_prbs,
                                                        data_width),
              0);

    for (unsigned port = 0; port != nof_ports; ++port) {
      std::vector<cbf16_t> cpu_decompressed(data[port].size());
      cpu_decompressor->decompress(
          cpu_decompressed, span<const uint8_t>(cuda_compressed.data() + port * port_stride, port_stride), params);
      const size_t grid_offset =
          (static_cast<size_t>(port) * nof_symbols + symbol) * grid_nof_prbs * NOF_SUBCARRIERS_PER_RB +
          start_prb * NOF_SUBCARRIERS_PER_RB;
      ASSERT_TRUE(std::equal(cpu_decompressed.begin(), cpu_decompressed.end(), output_grid + grid_offset))
          << "decompression type=" << ofh::to_string(type) << " width=" << data_width << " port=" << port;
    }

    std::fill(output_grid, output_grid + grid_size, cbf16_t{0.0F, 0.0F});
    ASSERT_NE(ocudu_ofh_decompress_device_bytes_to_device_grid_ports(handle,
                                                                     (type == ofh::compression_type::BFP)
                                                                         ? OCUDU_OFH_COMPRESSION_TYPE_BFP
                                                                         : OCUDU_OFH_COMPRESSION_TYPE_NONE,
                                                                     output_grid,
                                                                     device_compressed,
                                                                     port_stride,
                                                                     nof_symbols,
                                                                     grid_nof_prbs * NOF_SUBCARRIERS_PER_RB,
                                                                     0,
                                                                     nof_ports,
                                                                     symbol,
                                                                     start_prb,
                                                                     nof_prbs,
                                                                     data_width),
              0);

    for (unsigned port = 0; port != nof_ports; ++port) {
      std::vector<cbf16_t> cpu_decompressed(data[port].size());
      cpu_decompressor->decompress(
          cpu_decompressed, span<const uint8_t>(cuda_compressed.data() + port * port_stride, port_stride), params);
      const size_t grid_offset =
          (static_cast<size_t>(port) * nof_symbols + symbol) * grid_nof_prbs * NOF_SUBCARRIERS_PER_RB +
          start_prb * NOF_SUBCARRIERS_PER_RB;
      ASSERT_TRUE(std::equal(cpu_decompressed.begin(), cpu_decompressed.end(), output_grid + grid_offset))
          << "device decompression type=" << ofh::to_string(type) << " width=" << data_width << " port=" << port;
    }

    cudaFree(device_compressed);
  }

  ocudu_ofh_compression_destroy(handle);
  cudaFree(output_grid);
  cudaFree(device_grid);
}

TEST_P(ofh_iq_compression_cuda_test, cuda_device_grid_symbol_batch_path_matches_cpu_reference)
{
  static constexpr unsigned grid_nof_prbs     = 273;
  static constexpr unsigned nof_prbs          = 48;
  static constexpr unsigned start_prb         = 19;
  static constexpr unsigned nof_grid_symbols  = 14;
  static constexpr unsigned first_symbol      = 3;
  static constexpr unsigned nof_batch_symbols = 5;
  static constexpr unsigned nof_ports         = 3;
  static constexpr float    iq_scale          = 0.27F;

  ofh::compression_type type = GetParam();

  std::vector<std::vector<cbf16_t>> data(nof_batch_symbols * nof_ports);
  for (unsigned symbol = 0; symbol != nof_batch_symbols; ++symbol) {
    for (unsigned port = 0; port != nof_ports; ++port) {
      std::vector<cbf16_t>& symbol_data = data[symbol * nof_ports + port];
      symbol_data                       = generate_iq(nof_prbs);
      for (cbf16_t& sample : symbol_data) {
        sample = cbf16_t(to_float(sample.real) * (1.0F - 0.04F * port + 0.01F * symbol),
                         to_float(sample.imag) * (1.0F + 0.02F * port - 0.01F * symbol));
      }
    }
  }

  cbf16_t* device_grid = nullptr;
  ASSERT_EQ(cudaMallocManaged(&device_grid,
                              static_cast<size_t>(nof_ports) * nof_grid_symbols * grid_nof_prbs *
                                  NOF_SUBCARRIERS_PER_RB * sizeof(cbf16_t)),
            cudaSuccess);
  cbf16_t* output_grid = nullptr;
  ASSERT_EQ(cudaMallocManaged(&output_grid,
                              static_cast<size_t>(nof_ports) * nof_grid_symbols * grid_nof_prbs *
                                  NOF_SUBCARRIERS_PER_RB * sizeof(cbf16_t)),
            cudaSuccess);

  const size_t grid_size = static_cast<size_t>(nof_ports) * nof_grid_symbols * grid_nof_prbs * NOF_SUBCARRIERS_PER_RB;
  std::fill(device_grid, device_grid + grid_size, cbf16_t{0.0F, 0.0F});
  std::fill(output_grid, output_grid + grid_size, cbf16_t{0.0F, 0.0F});
  for (unsigned symbol = 0; symbol != nof_batch_symbols; ++symbol) {
    for (unsigned port = 0; port != nof_ports; ++port) {
      const size_t grid_offset = (static_cast<size_t>(port) * nof_grid_symbols + first_symbol + symbol) *
                                     grid_nof_prbs * NOF_SUBCARRIERS_PER_RB +
                                 start_prb * NOF_SUBCARRIERS_PER_RB;
      const std::vector<cbf16_t>& symbol_data = data[symbol * nof_ports + port];
      std::copy(symbol_data.begin(), symbol_data.end(), device_grid + grid_offset);
    }
  }

  std::unique_ptr<ofh::iq_compressor>   cpu_compressor   = ofh::create_iq_compressor(type, logger, iq_scale, "generic");
  std::unique_ptr<ofh::iq_decompressor> cpu_decompressor = ofh::create_iq_decompressor(type, logger, "generic");

  ocudu_ofh_compression_handle_t* handle = nullptr;
  ASSERT_NE(ocudu_ofh_compression_create(&handle), 0);

  for (unsigned data_width : {8U, 9U, 10U, 12U, 14U, 16U}) {
    ofh::ru_compression_params params;
    params.type       = type;
    params.data_width = data_width;

    unsigned                          prb_size      = ofh::get_compressed_prb_size(params).value();
    unsigned                          port_stride   = nof_prbs * prb_size;
    unsigned                          symbol_stride = nof_ports * port_stride;
    std::vector<uint8_t>              cuda_compressed(static_cast<size_t>(nof_batch_symbols) * symbol_stride);
    std::vector<std::vector<uint8_t>> host_buffer_compressed(nof_batch_symbols * nof_ports);
    std::vector<void*>                host_buffers(nof_batch_symbols * nof_ports);
    for (unsigned symbol = 0; symbol != nof_batch_symbols; ++symbol) {
      for (unsigned port = 0; port != nof_ports; ++port) {
        const unsigned buffer_index = symbol * nof_ports + port;
        host_buffer_compressed[buffer_index].resize(port_stride);
        host_buffers[buffer_index] = host_buffer_compressed[buffer_index].data();
      }
    }
    std::vector<uint8_t> device_cuda_compressed(cuda_compressed.size());
    uint8_t*             device_compressed = nullptr;
    ASSERT_EQ(cudaMalloc(&device_compressed, cuda_compressed.size()), cudaSuccess);

    ASSERT_NE(ocudu_ofh_compress_device_grid_symbol_batch(handle,
                                                          (type == ofh::compression_type::BFP)
                                                              ? OCUDU_OFH_COMPRESSION_TYPE_BFP
                                                              : OCUDU_OFH_COMPRESSION_TYPE_NONE,
                                                          cuda_compressed.data(),
                                                          symbol_stride,
                                                          port_stride,
                                                          device_grid,
                                                          nof_grid_symbols,
                                                          grid_nof_prbs * NOF_SUBCARRIERS_PER_RB,
                                                          0,
                                                          nof_ports,
                                                          first_symbol,
                                                          nof_batch_symbols,
                                                          start_prb,
                                                          nof_prbs,
                                                          data_width,
                                                          iq_scale),
              0);
    ASSERT_NE(ocudu_ofh_compress_device_grid_symbol_batch_to_host_buffers(handle,
                                                                          (type == ofh::compression_type::BFP)
                                                                              ? OCUDU_OFH_COMPRESSION_TYPE_BFP
                                                                              : OCUDU_OFH_COMPRESSION_TYPE_NONE,
                                                                          host_buffers.data(),
                                                                          host_buffers.size(),
                                                                          port_stride,
                                                                          device_grid,
                                                                          nof_grid_symbols,
                                                                          grid_nof_prbs * NOF_SUBCARRIERS_PER_RB,
                                                                          0,
                                                                          nof_ports,
                                                                          first_symbol,
                                                                          nof_batch_symbols,
                                                                          start_prb,
                                                                          nof_prbs,
                                                                          data_width,
                                                                          iq_scale),
              0);
    for (unsigned symbol = 0; symbol != nof_batch_symbols; ++symbol) {
      for (unsigned port = 0; port != nof_ports; ++port) {
        const unsigned buffer_index = symbol * nof_ports + port;
        ASSERT_TRUE(std::equal(host_buffer_compressed[buffer_index].begin(),
                               host_buffer_compressed[buffer_index].end(),
                               cuda_compressed.data() + static_cast<size_t>(symbol) * symbol_stride +
                                   static_cast<size_t>(port) * port_stride))
            << "host-buffer symbol-batch compression type=" << ofh::to_string(type) << " width=" << data_width
            << " symbol=" << symbol << " port=" << port;
      }
    }
    ASSERT_NE(ocudu_ofh_compress_device_grid_symbol_batch_to_device(handle,
                                                                    (type == ofh::compression_type::BFP)
                                                                        ? OCUDU_OFH_COMPRESSION_TYPE_BFP
                                                                        : OCUDU_OFH_COMPRESSION_TYPE_NONE,
                                                                    device_compressed,
                                                                    symbol_stride,
                                                                    port_stride,
                                                                    device_grid,
                                                                    nof_grid_symbols,
                                                                    grid_nof_prbs * NOF_SUBCARRIERS_PER_RB,
                                                                    0,
                                                                    nof_ports,
                                                                    first_symbol,
                                                                    nof_batch_symbols,
                                                                    start_prb,
                                                                    nof_prbs,
                                                                    data_width,
                                                                    iq_scale),
              0);
    ASSERT_EQ(
        cudaMemcpy(
            device_cuda_compressed.data(), device_compressed, device_cuda_compressed.size(), cudaMemcpyDeviceToHost),
        cudaSuccess);
    ASSERT_EQ(cuda_compressed, device_cuda_compressed)
        << "device symbol-batch compression type=" << ofh::to_string(type) << " width=" << data_width;

    ASSERT_EQ(cudaMemset(device_compressed, 0, cuda_compressed.size()), cudaSuccess);
    ASSERT_EQ(cudaDeviceSynchronize(), cudaSuccess);
    ASSERT_NE(ocudu_ofh_compress_device_grid_symbol_batch_to_device_async(handle,
                                                                          (type == ofh::compression_type::BFP)
                                                                              ? OCUDU_OFH_COMPRESSION_TYPE_BFP
                                                                              : OCUDU_OFH_COMPRESSION_TYPE_NONE,
                                                                          device_compressed,
                                                                          symbol_stride,
                                                                          port_stride,
                                                                          device_grid,
                                                                          nof_grid_symbols,
                                                                          grid_nof_prbs * NOF_SUBCARRIERS_PER_RB,
                                                                          0,
                                                                          nof_ports,
                                                                          first_symbol,
                                                                          nof_batch_symbols,
                                                                          start_prb,
                                                                          nof_prbs,
                                                                          data_width,
                                                                          iq_scale),
              0);
    ASSERT_NE(ocudu_ofh_compression_synchronize(handle), 0);
    std::vector<uint8_t> async_device_cuda_compressed(cuda_compressed.size());
    ASSERT_EQ(cudaMemcpy(async_device_cuda_compressed.data(),
                         device_compressed,
                         async_device_cuda_compressed.size(),
                         cudaMemcpyDeviceToHost),
              cudaSuccess);
    ASSERT_EQ(cuda_compressed, async_device_cuda_compressed)
        << "async device symbol-batch compression type=" << ofh::to_string(type) << " width=" << data_width;

    for (unsigned symbol = 0; symbol != nof_batch_symbols; ++symbol) {
      for (unsigned port = 0; port != nof_ports; ++port) {
        std::vector<uint8_t> cpu_compressed(port_stride);
        cpu_compressor->compress(cpu_compressed, data[symbol * nof_ports + port], params);
        ASSERT_TRUE(std::equal(cpu_compressed.begin(),
                               cpu_compressed.end(),
                               cuda_compressed.data() + static_cast<size_t>(symbol) * symbol_stride +
                                   static_cast<size_t>(port) * port_stride))
            << "compression type=" << ofh::to_string(type) << " width=" << data_width << " symbol=" << symbol
            << " port=" << port;
      }
    }

    auto check_decompressed_grid = [&](const char* label) {
      for (unsigned symbol = 0; symbol != nof_batch_symbols; ++symbol) {
        for (unsigned port = 0; port != nof_ports; ++port) {
          std::vector<cbf16_t> cpu_decompressed(data[symbol * nof_ports + port].size());
          cpu_decompressor->decompress(cpu_decompressed,
                                       span<const uint8_t>(cuda_compressed.data() +
                                                               static_cast<size_t>(symbol) * symbol_stride +
                                                               static_cast<size_t>(port) * port_stride,
                                                           port_stride),
                                       params);
          const size_t grid_offset = (static_cast<size_t>(port) * nof_grid_symbols + first_symbol + symbol) *
                                         grid_nof_prbs * NOF_SUBCARRIERS_PER_RB +
                                     start_prb * NOF_SUBCARRIERS_PER_RB;
          ASSERT_TRUE(std::equal(cpu_decompressed.begin(), cpu_decompressed.end(), output_grid + grid_offset))
              << label << " type=" << ofh::to_string(type) << " width=" << data_width << " symbol=" << symbol
              << " port=" << port;
        }
      }
    };

    std::fill(output_grid, output_grid + grid_size, cbf16_t{0.0F, 0.0F});
    ASSERT_NE(ocudu_ofh_decompress_to_device_grid_symbol_batch(handle,
                                                               (type == ofh::compression_type::BFP)
                                                                   ? OCUDU_OFH_COMPRESSION_TYPE_BFP
                                                                   : OCUDU_OFH_COMPRESSION_TYPE_NONE,
                                                               output_grid,
                                                               cuda_compressed.data(),
                                                               symbol_stride,
                                                               port_stride,
                                                               nof_grid_symbols,
                                                               grid_nof_prbs * NOF_SUBCARRIERS_PER_RB,
                                                               0,
                                                               nof_ports,
                                                               first_symbol,
                                                               nof_batch_symbols,
                                                               start_prb,
                                                               nof_prbs,
                                                               data_width),
              0);
    check_decompressed_grid("symbol-batch decompression");

    std::fill(output_grid, output_grid + grid_size, cbf16_t{0.0F, 0.0F});
    ASSERT_NE(ocudu_ofh_decompress_device_bytes_to_device_grid_symbol_batch(handle,
                                                                            (type == ofh::compression_type::BFP)
                                                                                ? OCUDU_OFH_COMPRESSION_TYPE_BFP
                                                                                : OCUDU_OFH_COMPRESSION_TYPE_NONE,
                                                                            output_grid,
                                                                            device_compressed,
                                                                            symbol_stride,
                                                                            port_stride,
                                                                            nof_grid_symbols,
                                                                            grid_nof_prbs * NOF_SUBCARRIERS_PER_RB,
                                                                            0,
                                                                            nof_ports,
                                                                            first_symbol,
                                                                            nof_batch_symbols,
                                                                            start_prb,
                                                                            nof_prbs,
                                                                            data_width),
              0);
    check_decompressed_grid("device symbol-batch decompression");

    cudaFree(device_compressed);
  }

  ocudu_ofh_compression_destroy(handle);
  cudaFree(output_grid);
  cudaFree(device_grid);
}

/// Verifies that a failure of the accelerated backend degrades to the CPU implementation instead of terminating the
/// process. The failure is injected with the OCUDU_OFH_COMPRESSION_FORCE_FALLBACK test hook, which is read when the
/// compressor is created.
TEST_P(ofh_iq_compression_cuda_test, cuda_failure_falls_back_to_cpu)
{
  static constexpr unsigned nof_prbs = 273;
  static constexpr float    iq_scale = 0.27F;

  ofh::compression_type type = GetParam();
  auto                  data = generate_iq(nof_prbs);

  std::unique_ptr<ofh::iq_compressor>   cpu_compressor   = ofh::create_iq_compressor(type, logger, iq_scale, "generic");
  std::unique_ptr<ofh::iq_decompressor> cpu_decompressor = ofh::create_iq_decompressor(type, logger, "generic");

  ASSERT_EQ(::setenv("OCUDU_OFH_COMPRESSION_FORCE_FALLBACK", "1", 1), 0);
  std::unique_ptr<ofh::iq_compressor>   cuda_compressor   = ofh::create_iq_compressor(type, logger, iq_scale, "cuda");
  std::unique_ptr<ofh::iq_decompressor> cuda_decompressor = ofh::create_iq_decompressor(type, logger, "cuda");
  ASSERT_EQ(::unsetenv("OCUDU_OFH_COMPRESSION_FORCE_FALLBACK"), 0);

  ASSERT_NE(cuda_compressor, nullptr);
  ASSERT_NE(cuda_decompressor, nullptr);

  for (unsigned data_width : {9U, 12U}) {
    ofh::ru_compression_params params;
    params.type       = type;
    params.data_width = data_width;

    unsigned             prb_size = ofh::get_compressed_prb_size(params).value();
    std::vector<uint8_t> cpu_compressed(nof_prbs * prb_size);
    std::vector<uint8_t> fallback_compressed(nof_prbs * prb_size);
    std::vector<cbf16_t> cpu_decompressed(data.size());
    std::vector<cbf16_t> fallback_decompressed(data.size());

    cpu_compressor->compress(cpu_compressed, data, params);
    cuda_compressor->compress(fallback_compressed, data, params);
    ASSERT_EQ(cpu_compressed, fallback_compressed)
        << "fallback compression type=" << ofh::to_string(type) << " width=" << data_width;

    cpu_decompressor->decompress(cpu_decompressed, cpu_compressed, params);
    cuda_decompressor->decompress(fallback_decompressed, cpu_compressed, params);
    ASSERT_EQ(cpu_decompressed, fallback_decompressed)
        << "fallback decompression type=" << ofh::to_string(type) << " width=" << data_width;
  }

  // The accelerated path reported a failure for every call above, and none of them was fatal.
  auto& cuda_compressor_impl = static_cast<ofh::iq_compression_cuda&>(*cuda_compressor);
  EXPECT_EQ(cuda_compressor_impl.get_nof_acceleration_failures(), 2U);
  auto& cuda_decompressor_impl = static_cast<ofh::iq_compression_cuda&>(*cuda_decompressor);
  EXPECT_EQ(cuda_decompressor_impl.get_nof_acceleration_failures(), 2U);
}

/// Verifies that a transient CUDA failure stays on the accelerated path by quarantining the failed handle and retrying
/// the operation once on a freshly created CUDA stream.
TEST_P(ofh_iq_compression_cuda_test, cuda_transient_failure_retries_on_fresh_handle)
{
  static constexpr unsigned nof_prbs = 106;
  static constexpr float    iq_scale = 0.27F;

  ofh::compression_type                 type = GetParam();
  std::vector<cbf16_t>                  data = generate_iq(nof_prbs);
  ofh::ru_compression_params            params{type, 9U};
  std::unique_ptr<ofh::iq_compressor>   cpu_compressor   = ofh::create_iq_compressor(type, logger, iq_scale, "generic");
  std::unique_ptr<ofh::iq_decompressor> cpu_decompressor = ofh::create_iq_decompressor(type, logger, "generic");

  ASSERT_EQ(::setenv("OCUDU_OFH_COMPRESSION_FORCE_FIRST_GPU_FAILURE", "1", 1), 0);
  std::unique_ptr<ofh::iq_compressor>   cuda_compressor   = ofh::create_iq_compressor(type, logger, iq_scale, "cuda");
  std::unique_ptr<ofh::iq_decompressor> cuda_decompressor = ofh::create_iq_decompressor(type, logger, "cuda");
  ASSERT_EQ(::unsetenv("OCUDU_OFH_COMPRESSION_FORCE_FIRST_GPU_FAILURE"), 0);

  ASSERT_NE(cuda_compressor, nullptr);
  ASSERT_NE(cuda_decompressor, nullptr);

  unsigned             prb_size = ofh::get_compressed_prb_size(params).value();
  std::vector<uint8_t> cpu_compressed(nof_prbs * prb_size);
  std::vector<uint8_t> cuda_compressed(nof_prbs * prb_size);
  std::vector<cbf16_t> cpu_decompressed(data.size());
  std::vector<cbf16_t> cuda_decompressed(data.size());

  cpu_compressor->compress(cpu_compressed, data, params);
  cuda_compressor->compress(cuda_compressed, data, params);
  ASSERT_EQ(cpu_compressed, cuda_compressed);

  cpu_decompressor->decompress(cpu_decompressed, cpu_compressed, params);
  cuda_decompressor->decompress(cuda_decompressed, cpu_compressed, params);
  ASSERT_EQ(cpu_decompressed, cuda_decompressed);

  auto& cuda_compressor_impl = static_cast<ofh::iq_compression_cuda&>(*cuda_compressor);
  EXPECT_EQ(cuda_compressor_impl.get_nof_acceleration_retries(), 1U);
  EXPECT_EQ(cuda_compressor_impl.get_nof_acceleration_failures(), 0U);
  auto& cuda_decompressor_impl = static_cast<ofh::iq_compression_cuda&>(*cuda_decompressor);
  EXPECT_EQ(cuda_decompressor_impl.get_nof_acceleration_retries(), 1U);
  EXPECT_EQ(cuda_decompressor_impl.get_nof_acceleration_failures(), 0U);
}

/// Verifies that direct device-grid decompression safely retries on a fresh handle without entering host
/// decompression.
TEST_P(ofh_iq_compression_cuda_test, cuda_direct_grid_transient_failure_retries_on_fresh_handle)
{
  static constexpr unsigned nof_prbs = 24;

  ofh::compression_type      type   = GetParam();
  std::vector<cbf16_t>       data   = generate_iq(nof_prbs);
  ofh::ru_compression_params params = {type, 9U};

  std::unique_ptr<ofh::iq_compressor> cpu_compressor =
      ofh::create_iq_compressor(type, logger, 1.0F, "generic");
  unsigned             prb_size = ofh::get_compressed_prb_size(params).value();
  std::vector<uint8_t> compressed(nof_prbs * prb_size);
  cpu_compressor->compress(compressed, data, params);

  ASSERT_EQ(::setenv("OCUDU_OFH_COMPRESSION_FORCE_FIRST_GPU_FAILURE", "1", 1), 0);
  std::unique_ptr<ofh::iq_decompressor> cuda_decompressor =
      ofh::create_iq_decompressor(type, logger, "cuda");
  ASSERT_EQ(::unsetenv("OCUDU_OFH_COMPRESSION_FORCE_FIRST_GPU_FAILURE"), 0);

  ASSERT_EQ(::setenv("OCUDU_UL_CUDA_VISIBLE_GRID", "managed", 1), 0);
  std::shared_ptr<resource_grid_factory> grid_factory = create_phy_acceleration_resource_grid_factory(
      create_resource_grid_factory(), phy_acceleration_resource_grid_direction::uplink, true);
  ASSERT_EQ(::unsetenv("OCUDU_UL_CUDA_VISIBLE_GRID"), 0);
  std::unique_ptr<resource_grid> grid = grid_factory->create(1, 1, nof_prbs * NOF_SUBCARRIERS_PER_RB);
  ASSERT_NE(grid, nullptr);
  ASSERT_TRUE(grid->get_writer().supports_device_grid_mapping());

  ofh::ru_compression_params invalid_params = params;
  invalid_params.data_width                 = 0;
  EXPECT_EQ(cuda_decompressor->decompress_to_resource_grid(
                grid->get_writer(), 0, 0, 0, nof_prbs, compressed, invalid_params),
            ofh::iq_decompression_result::invalid_input);
  EXPECT_EQ(cuda_decompressor->decompress_to_resource_grid(
                grid->get_writer(),
                0,
                0,
                0,
                nof_prbs,
                span<const uint8_t>(compressed).first(compressed.size() - 1),
                params),
            ofh::iq_decompression_result::invalid_input);

  ASSERT_EQ(cuda_decompressor->decompress_to_resource_grid(
                grid->get_writer(), 0, 0, 0, nof_prbs, compressed, params),
            ofh::iq_decompression_result::success_after_retry);

  std::vector<cbf16_t> expected(data.size());
  std::unique_ptr<ofh::iq_decompressor> cpu_decompressor = ofh::create_iq_decompressor(type, logger, "generic");
  cpu_decompressor->decompress(expected, compressed, params);
  span<const cbf16_t> actual = grid->get_reader().get_view(0, 0);
  ASSERT_EQ(expected.size(), actual.size());
  ASSERT_TRUE(std::equal(expected.begin(), expected.end(), actual.begin()));

  auto& cuda_decompressor_impl = static_cast<ofh::iq_compression_cuda&>(*cuda_decompressor);
  EXPECT_EQ(cuda_decompressor_impl.get_nof_acceleration_retries(), 1U);
  EXPECT_EQ(cuda_decompressor_impl.get_nof_acceleration_failures(), 0U);
}
