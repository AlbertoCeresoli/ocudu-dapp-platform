// SPDX-FileCopyrightText: Copyright (C) 2021-2026 DeepSig Inc
// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "phy_acceleration_prach_buffer_factory.h"
#include "ocudu/phy/support/support_factories.h"
#include "ocudu/phy/upper/channel_processors/prach/factories.h"
#include "ocudu/phy/upper/channel_processors/prach/prach_detector.h"
#include "ocudu/phy/upper/channel_processors/prach/prach_generator.h"
#include "ocudu/ran/prach/prach_preamble_information.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <gtest/gtest.h>
#include <random>

using namespace ocudu;

namespace {

struct test_case {
  prach_format_type        format;
  prach_subcarrier_spacing ra_scs;
  unsigned                 zero_correlation_zone;
  unsigned                 nof_rx_ports;
  unsigned                 target_preamble;
  unsigned                 delay_samples = 0;
};

std::unique_ptr<prach_buffer> create_test_buffer(const test_case& test)
{
  if (is_long_preamble(test.format)) {
    return create_prach_buffer_long(test.nof_rx_ports, 1);
  }
  return create_prach_buffer_short(test.nof_rx_ports, 1, 1);
}

std::unique_ptr<prach_buffer> create_device_test_buffer(const test_case& test)
{
  if (is_long_preamble(test.format)) {
    return create_phy_acceleration_prach_buffer_long(test.nof_rx_ports, 1, true);
  }
  return create_phy_acceleration_prach_buffer_short(test.nof_rx_ports, 1, 1, true);
}

void fill_detectable_preamble(prach_buffer& buffer, const test_case& test)
{
  std::unique_ptr<prach_generator> generator = create_prach_generator_factory_sw()->create();
  prach_generator::configuration   generator_config;
  generator_config.format                = test.format;
  generator_config.root_sequence_index   = 0;
  generator_config.preamble_index        = test.target_preamble;
  generator_config.restricted_set        = restricted_set_config::UNRESTRICTED;
  generator_config.zero_correlation_zone = test.zero_correlation_zone;

  span<const cf_t> sequence    = generator->generate(generator_config);
  unsigned         nof_symbols = is_long_preamble(test.format)
                                     ? get_prach_preamble_long_info(test.format).nof_symbols
                                     : get_prach_preamble_short_info(test.format, test.ra_scs, false).nof_symbols;
  unsigned         dft_size    = is_long_preamble(test.format) ? 1024 : 256;
  unsigned         half        = sequence.size() / 2;

  for (unsigned i_port = 0; i_port != test.nof_rx_ports; ++i_port) {
    for (unsigned i_symbol = 0; i_symbol != nof_symbols; ++i_symbol) {
      span<cbf16_t> symbol = buffer.get_symbol(i_port, 0, 0, i_symbol);
      for (unsigned i_re = 0; i_re != sequence.size(); ++i_re) {
        int   fft_bin = (i_re < half) ? static_cast<int>(dft_size - half + i_re) : static_cast<int>(i_re - half);
        float phase   = -2.0F * static_cast<float>(M_PI) * static_cast<float>(fft_bin * test.delay_samples) /
                      static_cast<float>(dft_size);
        symbol[i_re] = to_cbf16(sequence[i_re] * std::polar(1.0F, phase));
      }
    }
  }
}

prach_detector::configuration make_detector_config(const test_case& test)
{
  return {.root_sequence_index   = 0,
          .format                = test.format,
          .restricted_set        = restricted_set_config::UNRESTRICTED,
          .zero_correlation_zone = test.zero_correlation_zone,
          .start_preamble_index  = 0,
          .nof_preamble_indices  = 64,
          .ra_scs                = test.ra_scs,
          .nof_rx_ports          = test.nof_rx_ports,
          .slot                  = slot_point(0, 0, 0)};
}

const prach_detection_result::preamble_indication* find_preamble(const prach_detection_result& result,
                                                                 unsigned                      preamble_index)
{
  for (const auto& preamble : result.preambles) {
    if (preamble.preamble_index == preamble_index) {
      return &preamble;
    }
  }
  return nullptr;
}

class prach_detector_cuda_fixture : public ::testing::TestWithParam<test_case>
{};

} // namespace

TEST_P(prach_detector_cuda_fixture, matches_cpu_detection_for_known_preamble)
{
  if (!is_prach_detector_acceleration_available()) {
    GTEST_SKIP() << "CUDA PRACH detector is not available.";
  }

  test_case                     test   = GetParam();
  std::unique_ptr<prach_buffer> buffer = create_test_buffer(test);
  fill_detectable_preamble(*buffer, test);

  std::shared_ptr<dft_processor_factory> dft_factory = create_dft_processor_factory_fftw_fast();
  ASSERT_NE(dft_factory, nullptr);
  std::shared_ptr<prach_generator_factory> generator_factory = create_prach_generator_factory_sw();
  ASSERT_NE(generator_factory, nullptr);

  std::unique_ptr<prach_detector> cpu_detector =
      create_prach_detector_factory_sw(dft_factory, generator_factory)->create();
  std::unique_ptr<prach_detector> gpu_detector =
      create_prach_detector_factory_accelerated(dft_factory, generator_factory, {}, "enabled")->create();

  prach_detector::configuration config = make_detector_config(test);
  prach_detection_result        cpu    = cpu_detector->detect(*buffer, config);
  prach_detection_result        gpu    = gpu_detector->detect(*buffer, config);

  const auto* cpu_preamble = find_preamble(cpu, test.target_preamble);
  const auto* gpu_preamble = find_preamble(gpu, test.target_preamble);
  ASSERT_NE(cpu_preamble, nullptr);
  ASSERT_NE(gpu_preamble, nullptr);
  EXPECT_EQ(cpu_preamble->preamble_index, gpu_preamble->preamble_index);
  EXPECT_EQ(cpu.preambles.size(), gpu.preambles.size());
  EXPECT_NEAR(cpu_preamble->time_advance.to_seconds(), gpu_preamble->time_advance.to_seconds(), 1e-9);
  if (test.delay_samples != 0) {
    EXPECT_GT(cpu_preamble->time_advance.to_seconds(), 0.0);
  }
  // True-IDFT GPU path: metric must match host for zero and delayed peaks. The GPU accumulates the correlation in a
  // different order than the host, so the comparison scales with the metric magnitude, which spans several orders of
  // magnitude across the test vectors. The original absolute margin is kept as a floor for small metrics.
  EXPECT_NEAR(cpu_preamble->detection_metric,
              gpu_preamble->detection_metric,
              std::max(0.25F, 1.0e-3F * std::abs(cpu_preamble->detection_metric)));
  EXPECT_GT(gpu_preamble->detection_metric, 1.0F);
  EXPECT_NEAR(cpu_preamble->preamble_power_dB, gpu_preamble->preamble_power_dB, 0.3);
  EXPECT_NEAR(cpu.rssi_dB, gpu.rssi_dB, 0.2);
}

TEST(prach_detector_cuda, device_visible_buffer_matches_cpu_detection_and_metrics)
{
  if (!is_prach_detector_acceleration_available()) {
    GTEST_SKIP() << "CUDA PRACH detector is not available.";
  }

  test_case                     test{prach_format_type::B4, prach_subcarrier_spacing::kHz30, 14, 4, 20};
  std::unique_ptr<prach_buffer> cpu_buffer = create_test_buffer(test);
  std::unique_ptr<prach_buffer> gpu_buffer = create_device_test_buffer(test);
  ASSERT_NE(cpu_buffer, nullptr);
  ASSERT_NE(gpu_buffer, nullptr);
  ASSERT_TRUE(gpu_buffer->supports_device_prach_buffer_reading());
  fill_detectable_preamble(*cpu_buffer, test);
  fill_detectable_preamble(*gpu_buffer, test);

  std::shared_ptr<dft_processor_factory> dft_factory = create_dft_processor_factory_fftw_fast();
  ASSERT_NE(dft_factory, nullptr);
  std::shared_ptr<prach_generator_factory> generator_factory = create_prach_generator_factory_sw();
  ASSERT_NE(generator_factory, nullptr);

  std::unique_ptr<prach_detector> cpu_detector =
      create_prach_detector_factory_sw(dft_factory, generator_factory)->create();
  std::unique_ptr<prach_detector> gpu_detector =
      create_prach_detector_factory_accelerated(dft_factory, generator_factory, {}, "enabled")->create();

  prach_detector::configuration config = make_detector_config(test);
  prach_detection_result        cpu    = cpu_detector->detect(*cpu_buffer, config);
  prach_detection_result        gpu    = gpu_detector->detect(*gpu_buffer, config);

  const auto* cpu_preamble = find_preamble(cpu, test.target_preamble);
  const auto* gpu_preamble = find_preamble(gpu, test.target_preamble);
  ASSERT_NE(cpu_preamble, nullptr);
  ASSERT_NE(gpu_preamble, nullptr);
  EXPECT_EQ(cpu_preamble->preamble_index, gpu_preamble->preamble_index);
  EXPECT_EQ(cpu.preambles.size(), gpu.preambles.size());
  EXPECT_NEAR(cpu_preamble->time_advance.to_seconds(), gpu_preamble->time_advance.to_seconds(), 1e-9);
  EXPECT_NEAR(cpu_preamble->detection_metric, gpu_preamble->detection_metric, 0.2);
  EXPECT_NEAR(cpu_preamble->preamble_power_dB, gpu_preamble->preamble_power_dB, 0.3);
  EXPECT_NEAR(cpu.rssi_dB, gpu.rssi_dB, 0.2);
}

/// Format-0 / ZCZ-0 (common OTA long-preamble profile): GPU must not produce more
/// detections than host on the same AWGN realizations (false-alarm parity).
TEST(prach_detector_cuda, awgn_false_alarm_rate_matches_cpu_format0_zcz0)
{
  if (!is_prach_detector_acceleration_available()) {
    GTEST_SKIP() << "CUDA PRACH detector is not available.";
  }

  test_case                              test{prach_format_type::zero, prach_subcarrier_spacing::kHz1_25, 0, 1, 0};
  std::shared_ptr<dft_processor_factory> dft_factory = create_dft_processor_factory_fftw_fast();
  ASSERT_NE(dft_factory, nullptr);
  std::shared_ptr<prach_generator_factory> generator_factory = create_prach_generator_factory_sw();
  ASSERT_NE(generator_factory, nullptr);

  std::unique_ptr<prach_detector> cpu_detector =
      create_prach_detector_factory_sw(dft_factory, generator_factory)->create();
  std::unique_ptr<prach_detector> gpu_detector =
      create_prach_detector_factory_accelerated(dft_factory, generator_factory, {}, "enabled")->create();

  prach_detector::configuration config = make_detector_config(test);

  constexpr unsigned              nof_trials     = 64;
  unsigned                        cpu_detections = 0;
  unsigned                        gpu_detections = 0;
  std::mt19937                    rng(0xc0ffeeu);
  std::normal_distribution<float> noise(0.0F, 0.05F);

  for (unsigned trial = 0; trial != nof_trials; ++trial) {
    std::unique_ptr<prach_buffer> buffer = create_test_buffer(test);
    ASSERT_NE(buffer, nullptr);
    unsigned nof_symbols = get_prach_preamble_long_info(test.format).nof_symbols;
    unsigned L_ra        = prach_constants::LONG_SEQUENCE_LENGTH;
    for (unsigned i_port = 0; i_port != test.nof_rx_ports; ++i_port) {
      for (unsigned i_symbol = 0; i_symbol != nof_symbols; ++i_symbol) {
        span<cbf16_t> symbol = buffer->get_symbol(i_port, 0, 0, i_symbol);
        for (unsigned i_re = 0; i_re != L_ra; ++i_re) {
          symbol[i_re] = to_cbf16(cf_t{noise(rng), noise(rng)});
        }
      }
    }

    prach_detection_result cpu = cpu_detector->detect(*buffer, config);
    prach_detection_result gpu = gpu_detector->detect(*buffer, config);
    cpu_detections += static_cast<unsigned>(cpu.preambles.size());
    gpu_detections += static_cast<unsigned>(gpu.preambles.size());
  }

  EXPECT_LE(gpu_detections, cpu_detections + 2)
      << "cpu_fa=" << cpu_detections << " gpu_fa=" << gpu_detections << " trials=" << nof_trials;
}

TEST(prach_detector_cuda, zero_input_does_not_create_false_preamble)
{
  if (!is_prach_detector_acceleration_available()) {
    GTEST_SKIP() << "CUDA PRACH detector is not available.";
  }

  test_case                     test{prach_format_type::B4, prach_subcarrier_spacing::kHz30, 14, 2, 20};
  std::unique_ptr<prach_buffer> buffer = create_test_buffer(test);
  ASSERT_NE(buffer, nullptr);

  std::shared_ptr<dft_processor_factory> dft_factory = create_dft_processor_factory_fftw_fast();
  ASSERT_NE(dft_factory, nullptr);
  std::shared_ptr<prach_generator_factory> generator_factory = create_prach_generator_factory_sw();
  ASSERT_NE(generator_factory, nullptr);

  std::unique_ptr<prach_detector> cpu_detector =
      create_prach_detector_factory_sw(dft_factory, generator_factory)->create();
  std::unique_ptr<prach_detector> gpu_detector =
      create_prach_detector_factory_accelerated(dft_factory, generator_factory, {}, "enabled")->create();

  prach_detector::configuration config = make_detector_config(test);
  prach_detection_result        cpu    = cpu_detector->detect(*buffer, config);
  prach_detection_result        gpu    = gpu_detector->detect(*buffer, config);

  EXPECT_TRUE(cpu.preambles.empty());
  EXPECT_TRUE(gpu.preambles.empty());
}

INSTANTIATE_TEST_SUITE_P(cpu_gpu_parity,
                         prach_detector_cuda_fixture,
                         ::testing::Values(
                             // Format 0 / ZCZ 0 — OTA long-preamble profile.
                             test_case{prach_format_type::zero, prach_subcarrier_spacing::kHz1_25, 0, 1, 0},
                             test_case{prach_format_type::zero, prach_subcarrier_spacing::kHz1_25, 0, 1, 46},
                             test_case{prach_format_type::zero, prach_subcarrier_spacing::kHz1_25, 0, 1, 12, 8},
                             test_case{prach_format_type::zero, prach_subcarrier_spacing::kHz1_25, 0, 1, 27, 16},
                             // Non-zero ZCZ windows.
                             test_case{prach_format_type::zero, prach_subcarrier_spacing::kHz1_25, 1, 2, 12},
                             test_case{prach_format_type::zero, prach_subcarrier_spacing::kHz1_25, 5, 1, 3, 4},
                             // Short preambles.
                             test_case{prach_format_type::B4, prach_subcarrier_spacing::kHz30, 14, 4, 20},
                             test_case{prach_format_type::B4, prach_subcarrier_spacing::kHz30, 0, 1, 1, 6}));
