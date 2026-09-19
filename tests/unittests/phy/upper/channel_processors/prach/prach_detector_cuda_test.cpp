// SPDX-FileCopyrightText: Copyright (C) 2021-2026 DeepSig Inc
// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "channel_processors/prach/prach_detector_cuda_impl.h"
#include "phy_acceleration_prach_buffer_factory.h"
#include "ocudu/phy/support/support_factories.h"
#include "ocudu/phy/upper/channel_processors/prach/factories.h"
#include "ocudu/phy/upper/channel_processors/prach/prach_detector.h"
#include "ocudu/phy/upper/channel_processors/prach/prach_generator.h"
#include "ocudu/ran/prach/prach_preamble_information.h"
#include <algorithm>
#include <chrono>
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

/// \brief Runs \c detector against \c buffer with the accelerated path guaranteed to be in use.
///
/// \brief Returns the accelerated detector behind a base class reference.
///
/// The detectors these tests compare come from the factory in its forced mode, which either yields
/// the accelerated detector or reports a fatal error, so the type is known. The project builds
/// without RTTI, hence the static cast.
prach_detector_cuda_impl& as_accelerated(prach_detector& detector)
{
  return static_cast<prach_detector_cuda_impl&>(detector);
}

/// The first occasion of a workload shape is served by the host fallback while the backend is prepared on a helper
/// thread, so a test that wants to compare the accelerated result has to prime the detector and wait.
prach_detection_result
detect_accelerated(prach_detector& detector, const prach_buffer& buffer, const prach_detector::configuration& config)
{
  prach_detector_cuda_impl& cuda = as_accelerated(detector);
  detector.detect(buffer, config);
  EXPECT_TRUE(cuda.wait_for_warmup(std::chrono::seconds(60)));
  return detector.detect(buffer, config);
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
  prach_detection_result        gpu    = detect_accelerated(*gpu_detector, *buffer, config);

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
  prach_detection_result        gpu    = detect_accelerated(*gpu_detector, *gpu_buffer, config);

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
    prach_detection_result gpu = detect_accelerated(*gpu_detector, *buffer, config);
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
  prach_detection_result        gpu    = detect_accelerated(*gpu_detector, *buffer, config);

  EXPECT_TRUE(cpu.preambles.empty());
  EXPECT_TRUE(gpu.preambles.empty());
}

/// The accelerated detector prepares its backend for a workload shape on a helper thread and hands the occasion
/// that triggered it to the host detector. These tests cover that hand-over: that the first occasion is answered
/// correctly without waiting for a kernel to compile, that the shape prepared is the one the cell actually asked
/// for, and that a detector can be destroyed while a preparation is still running.
namespace {

/// Builds an accelerated detector and a host detector configured identically.
struct detector_pair {
  std::unique_ptr<prach_detector> cpu;
  std::unique_ptr<prach_detector> gpu;
};

detector_pair make_detectors()
{
  std::shared_ptr<dft_processor_factory> dft_factory = create_dft_processor_factory_fftw_fast();
  EXPECT_NE(dft_factory, nullptr);
  std::shared_ptr<prach_generator_factory> generator_factory = create_prach_generator_factory_sw();
  EXPECT_NE(generator_factory, nullptr);

  return {create_prach_detector_factory_sw(dft_factory, generator_factory)->create(),
          create_prach_detector_factory_accelerated(dft_factory, generator_factory, {}, "enabled")->create()};
}

} // namespace

TEST(prach_detector_cuda_warmup, first_occasion_of_a_new_shape_is_served_by_the_host_detector)
{
  if (!is_prach_detector_acceleration_available()) {
    GTEST_SKIP() << "CUDA PRACH detector is not available.";
  }

  test_case                     test{prach_format_type::zero, prach_subcarrier_spacing::kHz1_25, 0, 1, 12, 8};
  std::unique_ptr<prach_buffer> buffer = create_test_buffer(test);
  ASSERT_NE(buffer, nullptr);
  fill_detectable_preamble(*buffer, test);

  detector_pair                 detectors = make_detectors();
  prach_detector::configuration config    = make_detector_config(test);
  prach_detector_cuda_impl&     cuda      = as_accelerated(*detectors.gpu);
  ASSERT_FALSE(cuda.is_warm_for(config));

  // Nothing is prepared yet, so this occasion runs on the host detector: not merely close to it, the very same
  // computation. That is what keeps it off the kernel compile that the helper thread is starting behind it.
  prach_detection_result first = detectors.gpu->detect(*buffer, config);
  prach_detection_result host  = detectors.cpu->detect(*buffer, config);

  const auto* first_preamble = find_preamble(first, test.target_preamble);
  const auto* host_preamble  = find_preamble(host, test.target_preamble);
  ASSERT_NE(first_preamble, nullptr);
  ASSERT_NE(host_preamble, nullptr);
  EXPECT_EQ(first.preambles.size(), host.preambles.size());
  EXPECT_FLOAT_EQ(first_preamble->detection_metric, host_preamble->detection_metric);
  EXPECT_FLOAT_EQ(first.rssi_dB, host.rssi_dB);
}

TEST(prach_detector_cuda_warmup, occasions_after_the_warm_up_run_on_the_accelerated_path)
{
  if (!is_prach_detector_acceleration_available()) {
    GTEST_SKIP() << "CUDA PRACH detector is not available.";
  }

  test_case                     test{prach_format_type::zero, prach_subcarrier_spacing::kHz1_25, 0, 1, 12, 8};
  std::unique_ptr<prach_buffer> buffer = create_test_buffer(test);
  ASSERT_NE(buffer, nullptr);
  fill_detectable_preamble(*buffer, test);

  detector_pair                 detectors = make_detectors();
  prach_detector::configuration config    = make_detector_config(test);

  prach_detector_cuda_impl& cuda = as_accelerated(*detectors.gpu);
  detectors.gpu->detect(*buffer, config);
  ASSERT_TRUE(cuda.wait_for_warmup(std::chrono::seconds(60)));
  // Without this the comparison below would pass just as well on the fallback, which is the thing it is meant to
  // be distinguishing itself from.
  ASSERT_TRUE(cuda.is_warm_for(config));

  prach_detection_result accelerated = detectors.gpu->detect(*buffer, config);
  prach_detection_result host        = detectors.cpu->detect(*buffer, config);

  const auto* accelerated_preamble = find_preamble(accelerated, test.target_preamble);
  const auto* host_preamble        = find_preamble(host, test.target_preamble);
  ASSERT_NE(accelerated_preamble, nullptr);
  ASSERT_NE(host_preamble, nullptr);
  EXPECT_EQ(accelerated.preambles.size(), host.preambles.size());
  EXPECT_NEAR(accelerated_preamble->time_advance.to_seconds(), host_preamble->time_advance.to_seconds(), 1e-9);
  EXPECT_NEAR(accelerated_preamble->detection_metric,
              host_preamble->detection_metric,
              std::max(0.25F, 1.0e-3F * std::abs(host_preamble->detection_metric)));
}

TEST(prach_detector_cuda_warmup, a_second_workload_shape_is_prepared_on_its_own_terms)
{
  if (!is_prach_detector_acceleration_available()) {
    GTEST_SKIP() << "CUDA PRACH detector is not available.";
  }

  // Two shapes the same cell can ask for. A zero correlation zone lets preambles share a root sequence, so the two
  // need a different number of correlations and the backend cannot serve both from one prepared plan. This is what
  // a warm-up that guesses the shape from a partial configuration gets wrong: it prepares one of them and the cell
  // then pays for the other on the slot path.
  test_case zone_zero{prach_format_type::zero, prach_subcarrier_spacing::kHz1_25, 0, 1, 12, 8};
  test_case zone_five{prach_format_type::zero, prach_subcarrier_spacing::kHz1_25, 5, 1, 3, 4};

  std::unique_ptr<prach_buffer> zone_zero_buffer = create_test_buffer(zone_zero);
  std::unique_ptr<prach_buffer> zone_five_buffer = create_test_buffer(zone_five);
  ASSERT_NE(zone_zero_buffer, nullptr);
  ASSERT_NE(zone_five_buffer, nullptr);
  fill_detectable_preamble(*zone_zero_buffer, zone_zero);
  fill_detectable_preamble(*zone_five_buffer, zone_five);

  detector_pair             detectors = make_detectors();
  prach_detector_cuda_impl& cuda      = as_accelerated(*detectors.gpu);

  // Settle on the first shape.
  prach_detector::configuration zone_zero_config = make_detector_config(zone_zero);
  detectors.gpu->detect(*zone_zero_buffer, zone_zero_config);
  ASSERT_TRUE(cuda.wait_for_warmup(std::chrono::seconds(60)));
  ASSERT_TRUE(cuda.is_warm_for(zone_zero_config));

  // The second shape has not been prepared, so its first occasion goes to the host detector even though the
  // detector is warm for something else. Being warm for one zone and not the other is exactly what a warm-up
  // that guesses the zone before the cell runs cannot get right.
  prach_detector::configuration zone_five_config = make_detector_config(zone_five);
  ASSERT_FALSE(cuda.is_warm_for(zone_five_config));
  prach_detection_result first          = detectors.gpu->detect(*zone_five_buffer, zone_five_config);
  prach_detection_result host           = detectors.cpu->detect(*zone_five_buffer, zone_five_config);
  const auto*            first_preamble = find_preamble(first, zone_five.target_preamble);
  const auto*            host_preamble  = find_preamble(host, zone_five.target_preamble);
  ASSERT_NE(first_preamble, nullptr);
  ASSERT_NE(host_preamble, nullptr);
  EXPECT_FLOAT_EQ(first_preamble->detection_metric, host_preamble->detection_metric);

  // Once prepared for it, the second shape runs accelerated and still finds the preamble.
  ASSERT_TRUE(cuda.wait_for_warmup(std::chrono::seconds(60)));
  ASSERT_TRUE(cuda.is_warm_for(zone_five_config));
  EXPECT_FALSE(cuda.is_warm_for(zone_zero_config)) << "the backend holds one plan at a time";
  prach_detection_result accelerated          = detectors.gpu->detect(*zone_five_buffer, zone_five_config);
  const auto*            accelerated_preamble = find_preamble(accelerated, zone_five.target_preamble);
  ASSERT_NE(accelerated_preamble, nullptr);
  EXPECT_NEAR(accelerated_preamble->detection_metric,
              host_preamble->detection_metric,
              std::max(0.25F, 1.0e-3F * std::abs(host_preamble->detection_metric)));
}

TEST(prach_detector_cuda_warmup, detector_can_be_destroyed_while_a_warm_up_is_in_flight)
{
  if (!is_prach_detector_acceleration_available()) {
    GTEST_SKIP() << "CUDA PRACH detector is not available.";
  }

  test_case                     test{prach_format_type::zero, prach_subcarrier_spacing::kHz1_25, 0, 1, 12, 8};
  std::unique_ptr<prach_buffer> buffer = create_test_buffer(test);
  ASSERT_NE(buffer, nullptr);
  fill_detectable_preamble(*buffer, test);

  // A cell that is torn down right after it starts destroys the detector with the helper thread still preparing the
  // backend. The destructor has to retire that thread before it releases the stream and the handle it is using.
  detector_pair detectors = make_detectors();
  detectors.gpu->detect(*buffer, make_detector_config(test));
  detectors.gpu.reset();
}

TEST(prach_detector_cuda_warmup, warm_up_can_be_turned_off)
{
  if (!is_prach_detector_acceleration_available()) {
    GTEST_SKIP() << "CUDA PRACH detector is not available.";
  }

  test_case                     test{prach_format_type::zero, prach_subcarrier_spacing::kHz1_25, 0, 1, 12, 8};
  std::unique_ptr<prach_buffer> buffer = create_test_buffer(test);
  ASSERT_NE(buffer, nullptr);
  fill_detectable_preamble(*buffer, test);

  // With the helper thread disabled the slot path prepares the backend itself, as it did before the warm-up
  // existed: the very first occasion runs accelerated, and pays for the kernel compile to get there.
  ASSERT_EQ(::setenv("OCUDU_PRACH_ACCELERATION_ASYNC_WARMUP", "0", 1), 0);
  detector_pair detectors = make_detectors();
  ASSERT_EQ(::unsetenv("OCUDU_PRACH_ACCELERATION_ASYNC_WARMUP"), 0);

  prach_detector::configuration config = make_detector_config(test);
  prach_detection_result        first  = detectors.gpu->detect(*buffer, config);
  prach_detection_result        host   = detectors.cpu->detect(*buffer, config);

  const auto* first_preamble = find_preamble(first, test.target_preamble);
  const auto* host_preamble  = find_preamble(host, test.target_preamble);
  ASSERT_NE(first_preamble, nullptr);
  ASSERT_NE(host_preamble, nullptr);
  EXPECT_NEAR(first_preamble->detection_metric,
              host_preamble->detection_metric,
              std::max(0.25F, 1.0e-3F * std::abs(host_preamble->detection_metric)));
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
