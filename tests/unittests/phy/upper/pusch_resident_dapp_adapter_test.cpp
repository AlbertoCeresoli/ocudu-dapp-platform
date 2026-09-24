// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "../support/pusch_test_grid.h"
#include "../support/resource_grid_test_doubles.h"
#include "apps/services/dapp/dapp_service.h"
#include "ocudu/dapp/abi/v1/metric_catalog.h"
#include "ocudu/dapp/runtime/artifact_verifier.h"
#include "ocudu/dapp/runtime/class_c_process_supervisor.h"
#include "ocudu/dapp/runtime/instance_manager.h"
#include "ocudu/dapp/runtime/module_loader.h"
#include "ocudu/dapp/runtime/native_hook_point.h"
#include "ocudu/dapp/runtime/native_instance.h"
#include "ocudu/phy/generic_functions/generic_functions_factories.h"
#include "ocudu/phy/generic_functions/transform_precoding/transform_precoding_factories.h"
#include "ocudu/phy/upper/channel_modulation/channel_modulation_factories.h"
#include "ocudu/phy/upper/channel_processors/pusch/factories.h"
#include "ocudu/phy/upper/channel_processors/pusch/pusch_codeword_buffer.h"
#include "ocudu/phy/upper/channel_processors/pusch/pusch_demodulator_notifier.h"
#include "ocudu/phy/upper/dapp/pusch_dapp_channel_estimator.h"
#include "ocudu/phy/upper/dapp/pusch_dapp_demodulator.h"
#include "ocudu/phy/upper/dapp/pusch_resident_dapp.h"
#include "ocudu/phy/upper/dapp/upper_phy_dapp_hook_provider.h"
#include "ocudu/phy/upper/equalization/equalization_factories.h"
#include "ocudu/phy/upper/phy_tap/phy_tap.h"
#include "ocudu/phy/upper/sequence_generators/sequence_generator_factories.h"
#include "ocudu/phy/upper/signal_processors/srs/srs_estimator_configuration.h"
#include "ocudu/phy/upper/signal_processors/srs/srs_estimator_factory.h"
#include "ocudu/phy/upper/signal_processors/srs/srs_estimator_result.h"
#include "ocudu/ran/srs/srs_information.h"
#include "ocudu/scheduler/dapp/scheduler_dapp_context.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <cuda_runtime.h>
#include <filesystem>
#include <gtest/gtest.h>
#include <mutex>
#include <optional>
#include <signal.h>
#include <thread>

using namespace ocudu;
using namespace std::chrono_literals;

namespace {

class target_spy final : public pusch_dapp_demodulator_target
{
public:
  pusch_dapp_demodulator_mode active_mode() const noexcept override { return mode; }
  uint32_t                    lane_count() const noexcept override { return 1; }
  uint32_t                    worker_id(uint32_t, pusch_dapp_demodulator_mode) const noexcept override { return 41; }
  uint32_t                    backend_id(pusch_dapp_demodulator_mode) const noexcept override { return backend; }

  dapp::native_invocation_result
  invoke(uint32_t, const ocudu_dapp_equalizer_input_v1& input, ocudu_dapp_equalizer_output_v1& output) noexcept override
  {
    ++calls;
    invocation = input.invocation;
    grid       = input.rx_grid;
    pusch      = *static_cast<const ocudu_dapp_pusch_metadata_v1*>(input.pusch.metadata.data);
    dmrs       = *static_cast<const ocudu_dapp_dmrs_metadata_v1*>(input.dmrs.metadata.data);
    equalized  = output.equalized_symbols;
    eq_noise   = output.post_equalization_noise;
    return {OCUDU_DAPP_OK_V1, false};
  }

  void report_fallback(uint32_t,
                       pusch_dapp_demodulator_mode   selected,
                       dapp::class_a_fallback_reason reason) noexcept override
  {
    fallback_reasons.emplace_back(selected, reason);
  }

  void report_completion_deadline(uint32_t,
                                  pusch_dapp_demodulator_mode     selected,
                                  const ocudu_dapp_invocation_v1& completed,
                                  uint64_t                        observed_latency_ns,
                                  uint64_t                        deadline_ns) noexcept override
  {
    completion_deadlines.push_back({selected, completed.invocation_id, observed_latency_ns, deadline_ns});
  }

  dapp::native_invocation_result
  invoke(uint32_t, const ocudu_dapp_receiver_input_v1& input, ocudu_dapp_receiver_output_v1& output) noexcept override
  {
    ++calls;
    invocation  = input.invocation;
    grid        = input.rx_grid;
    pusch       = *static_cast<const ocudu_dapp_pusch_metadata_v1*>(input.pusch.metadata.data);
    dmrs        = *static_cast<const ocudu_dapp_dmrs_metadata_v1*>(input.dmrs.metadata.data);
    llrs        = output.llrs;
    auto stream = reinterpret_cast<cudaStream_t>(static_cast<uintptr_t>(input.invocation.execution_stream));
    if (cudaMemsetAsync(reinterpret_cast<void*>(output.llrs.native_handle), 0, output.llrs.byte_length, stream) !=
        cudaSuccess) {
      return {OCUDU_DAPP_INTERNAL_ERROR_V1, true};
    }
    if (output.metrics.metadata_schema_id == OCUDU_DAPP_METADATA_RECEIVER_METRICS_V1 &&
        output.metrics.metadata.data != nullptr) {
      const auto* metrics = static_cast<const ocudu_dapp_receiver_metrics_v1*>(output.metrics.metadata.data);
      eq_noise            = metrics->post_equalization_noise;
      if (cudaMemsetAsync(reinterpret_cast<void*>(eq_noise.native_handle), 0, eq_noise.byte_length, stream) !=
          cudaSuccess) {
        return {OCUDU_DAPP_INTERNAL_ERROR_V1, true};
      }
      if (publish_receiver_metrics) {
        output.reserved0 |= OCUDU_DAPP_RECEIVER_METRICS_VALID_V1;
      }
    }
    return {OCUDU_DAPP_OK_V1, false};
  }

  pusch_dapp_demodulator_mode  mode{pusch_dapp_demodulator_mode::receiver};
  uint32_t                     backend{OCUDU_DAPP_BACKEND_CUDA_V1};
  bool                         publish_receiver_metrics{true};
  unsigned                     calls{};
  ocudu_dapp_invocation_v1     invocation{};
  ocudu_dapp_tensor_view_v1    grid{};
  ocudu_dapp_pusch_metadata_v1 pusch{};
  ocudu_dapp_dmrs_metadata_v1  dmrs{};
  ocudu_dapp_tensor_view_v1    llrs{};
  ocudu_dapp_tensor_view_v1    equalized{};
  ocudu_dapp_tensor_view_v1    eq_noise{};
  std::vector<std::pair<pusch_dapp_demodulator_mode, dapp::class_a_fallback_reason>> fallback_reasons;
  struct completion_deadline {
    pusch_dapp_demodulator_mode mode;
    uint64_t                    invocation_id;
    uint64_t                    observed_latency_ns;
    uint64_t                    deadline_ns;
  };
  std::vector<completion_deadline> completion_deadlines;
};

class channel_target_spy final : public pusch_dapp_channel_estimator_target
{
public:
  bool     active() const noexcept override { return enabled; }
  uint32_t lane_count() const noexcept override { return 1; }
  uint32_t worker_id(uint32_t) const noexcept override { return 51; }
  uint32_t backend_id() const noexcept override { return OCUDU_DAPP_BACKEND_CUDA_V1; }

  dapp::native_invocation_result invoke(uint32_t,
                                        const ocudu_dapp_channel_estimator_input_v1& input,
                                        ocudu_dapp_channel_estimator_output_v1&      output) noexcept override
  {
    ++calls;
    invocation = input.invocation;
    pusch      = *static_cast<const ocudu_dapp_pusch_metadata_v1*>(input.pusch.metadata.data);
    channel    = output.channel_estimates;
    noise      = output.noise_variances;
    std::copy(std::begin(output.reserved), std::end(output.reserved), output_reserved.begin());
    auto stream = reinterpret_cast<cudaStream_t>(static_cast<uintptr_t>(input.invocation.execution_stream));
    if (cudaMemsetAsync(reinterpret_cast<void*>(channel.native_handle), fill_byte, channel.byte_length, stream) !=
            cudaSuccess ||
        cudaMemsetAsync(reinterpret_cast<void*>(noise.native_handle), fill_byte, noise.byte_length, stream) !=
            cudaSuccess) {
      return {OCUDU_DAPP_INTERNAL_ERROR_V1, true};
    }
    if (publish_scheduler_noise) {
      if (output.reserved[OCUDU_DAPP_CHANNEL_ESTIMATOR_SCHED_NOISE_REQUEST_INDEX_V1] !=
              OCUDU_DAPP_CHANNEL_ESTIMATOR_SCHED_NOISE_REQUEST_V1 ||
          output.reserved[OCUDU_DAPP_CHANNEL_ESTIMATOR_SCHED_NOISE_HANDLE_INDEX_V1] == 0 ||
          output.reserved[OCUDU_DAPP_CHANNEL_ESTIMATOR_SCHED_NOISE_BYTES_INDEX_V1] < sizeof(float) ||
          cudaMemsetAsync(
              reinterpret_cast<void*>(output.reserved[OCUDU_DAPP_CHANNEL_ESTIMATOR_SCHED_NOISE_HANDLE_INDEX_V1]),
              scheduler_fill_byte,
              output.reserved[OCUDU_DAPP_CHANNEL_ESTIMATOR_SCHED_NOISE_BYTES_INDEX_V1],
              stream) != cudaSuccess) {
        return {OCUDU_DAPP_INVALID_INPUT_V1, true};
      }
      output.reserved[OCUDU_DAPP_CHANNEL_ESTIMATOR_SCHED_NOISE_RESPONSE_INDEX_V1] =
          OCUDU_DAPP_CHANNEL_ESTIMATOR_SCHED_NOISE_RESPONSE_V1;
      output_reserved[OCUDU_DAPP_CHANNEL_ESTIMATOR_SCHED_NOISE_RESPONSE_INDEX_V1] =
          OCUDU_DAPP_CHANNEL_ESTIMATOR_SCHED_NOISE_RESPONSE_V1;
    }
    return result;
  }

  void report_fallback(uint32_t, dapp::class_a_fallback_reason reason) noexcept override
  {
    fallback_reasons.push_back(reason);
  }

  void report_completion_deadline(uint32_t,
                                  const ocudu_dapp_invocation_v1& completed,
                                  uint64_t                        observed_latency_ns,
                                  uint64_t                        deadline_ns) noexcept override
  {
    completion_deadlines.push_back({completed.invocation_id, observed_latency_ns, deadline_ns});
  }

  bool                                       enabled{true};
  unsigned                                   calls{};
  ocudu_dapp_invocation_v1                   invocation{};
  ocudu_dapp_pusch_metadata_v1               pusch{};
  ocudu_dapp_tensor_view_v1                  channel{};
  ocudu_dapp_tensor_view_v1                  noise{};
  std::array<uint64_t, 4>                    output_reserved{};
  dapp::native_invocation_result             result{OCUDU_DAPP_OK_V1, false};
  int                                        fill_byte{};
  int                                        scheduler_fill_byte{};
  bool                                       publish_scheduler_noise{};
  std::vector<dapp::class_a_fallback_reason> fallback_reasons;
  struct completion_deadline {
    uint64_t invocation_id;
    uint64_t observed_latency_ns;
    uint64_t deadline_ns;
  };
  std::vector<completion_deadline> completion_deadlines;
};

class resident_receiver_spy final : public pusch_resident_dapp
{
public:
  bool bind_control_context(std::shared_ptr<dapp::native_control_context> context) noexcept override
  {
    control_context = std::move(context);
    return bool(control_context);
  }
  std::shared_ptr<dapp::native_control_context> control_context;
  pusch_resident_dapp_mode mode() const noexcept override { return pusch_resident_dapp_mode::receiver; }

  dapp::native_invocation_result invoke_equalizer(const pusch_resident_dapp_buffers&,
                                                  const pusch_demodulator::configuration&) noexcept override
  {
    return {OCUDU_DAPP_BYPASS_V1, true};
  }

  dapp::native_invocation_result invoke_receiver(const pusch_resident_dapp_buffers& buffers,
                                                 const pusch_demodulator::configuration&) noexcept override
  {
    ++calls;
    last        = buffers;
    auto stream = reinterpret_cast<cudaStream_t>(static_cast<uintptr_t>(buffers.execution_stream));
    if (cudaMemsetAsync(buffers.llrs_f16, fill_byte, 2 * buffers.nof_data_re * sizeof(uint16_t), stream) !=
        cudaSuccess) {
      return {OCUDU_DAPP_INTERNAL_ERROR_V1, true};
    }
    return result;
  }

  void on_completion(void* token) noexcept override
  {
    ++completions;
    completion_token = token;
  }

  unsigned                       calls{};
  unsigned                       completions{};
  void*                          completion_token{};
  pusch_resident_dapp_buffers    last{};
  dapp::native_invocation_result result{OCUDU_DAPP_OK_V1, false};
  int                            fill_byte{};
};

class resident_equalizer_spy final : public pusch_resident_dapp
{
public:
  bool bind_control_context(std::shared_ptr<dapp::native_control_context> context) noexcept override
  {
    control_context = std::move(context);
    return bool(control_context);
  }
  std::shared_ptr<dapp::native_control_context> control_context;
  pusch_resident_dapp_mode mode() const noexcept override { return pusch_resident_dapp_mode::equalizer; }

  dapp::native_invocation_result invoke_equalizer(const pusch_resident_dapp_buffers& buffers,
                                                  const pusch_demodulator::configuration&) noexcept override
  {
    ++calls;
    last        = buffers;
    auto stream = reinterpret_cast<cudaStream_t>(static_cast<uintptr_t>(buffers.execution_stream));
    if (cudaMemsetAsync(buffers.equalized_cf32, fill_byte, buffers.nof_data_re * sizeof(float2), stream) !=
            cudaSuccess ||
        cudaMemsetAsync(buffers.post_eq_noise_f32, fill_byte, buffers.nof_data_re * sizeof(float), stream) !=
            cudaSuccess) {
      return {OCUDU_DAPP_INTERNAL_ERROR_V1, true};
    }
    return result;
  }

  dapp::native_invocation_result invoke_receiver(const pusch_resident_dapp_buffers&,
                                                 const pusch_demodulator::configuration&) noexcept override
  {
    return {OCUDU_DAPP_BYPASS_V1, true};
  }

  void on_completion(void* token) noexcept override
  {
    ++completions;
    completion_token = token;
  }

  unsigned                       calls{};
  unsigned                       completions{};
  void*                          completion_token{};
  pusch_resident_dapp_buffers    last{};
  dapp::native_invocation_result result{OCUDU_DAPP_OK_V1, false};
  int                            fill_byte{};
};

class empty_results final : public dmrs_pusch_estimator_results
{
public:
  float                           get_noise_variance(unsigned) const override { return 0; }
  float                           get_rsrp(unsigned, unsigned = 0) const override { return 0; }
  static_vector<float, MAX_PORTS> get_rsrp_all_ports(unsigned = 0) const override { return {}; }
  float                           get_epre(unsigned) const override { return 0; }
  float                           get_snr(unsigned) const override { return 0; }
  float                           get_layer_average_snr(unsigned = 0) const override { return 0; }
  phy_time_unit                   get_time_alignment(unsigned) const override { return {}; }
  std::optional<float>            get_cfo_Hz(unsigned) const override { return std::nullopt; }
  void                            get_symbol_ch_estimate(span<cbf16_t>, unsigned, unsigned, unsigned) const override {}
  void                            get_symbol_ch_estimate(span<cbf16_t>,
                                                         unsigned,
                                                         unsigned,
                                                         unsigned,
                                                         const bounded_bitset<MAX_NOF_SUBCARRIERS>&) const override
  {
  }
  void get_channel_state_information(channel_state_information&) const override {}
};

class codeword_spy final : public pusch_codeword_buffer
{
public:
  span<log_likelihood_ratio> get_next_block_view(unsigned block_size) override
  {
    scratch.resize(block_size);
    return scratch;
  }
  void on_new_block(span<const log_likelihood_ratio>, const bit_buffer&) override { ++blocks; }
  void on_end_codeword() override { ++end_calls; }

  std::vector<log_likelihood_ratio> scratch;
  unsigned                          blocks{};
  unsigned                          end_calls{};
};

class notifier_spy final : public pusch_demodulator_notifier
{
public:
  void     on_provisional_stats(unsigned, const demodulation_stats&) override {}
  void     on_end_stats(const demodulation_stats&) override { ++end_calls; }
  unsigned end_calls{};
};

pusch_demodulator::configuration make_config()
{
  pusch_demodulator::configuration config{};
  config.cell_id = 7;
  config.rnti    = to_rnti(0x1234);
  config.rb_mask = crb_bitmap(1);
  config.rb_mask.set(0);
  config.modulation         = modulation_scheme::QPSK;
  config.start_symbol_index = 2;
  config.nof_symbols        = 2;
  config.dmrs_symb_pos      = symbol_slot_mask(MAX_NSYMB_PER_SLOT);
  config.dmrs_symb_pos.set(2);
  config.dmrs_type                   = dmrs_config_type::type1;
  config.nof_cdm_groups_without_data = 1;
  config.n_id                        = 17;
  config.dmrs_scrambling_id          = 19;
  config.dmrs_scaling                = std::sqrt(2.0F);
  config.c_prefix                    = cyclic_prefix::NORMAL;
  config.nof_tx_layers               = 1;
  config.slot                        = slot_point{1, 22};
  config.rx_ports.push_back(0);
  return config;
}

std::unique_ptr<pusch_demodulator> make_accelerated_demodulator(unsigned nof_prb)
{
  pusch_demodulator_factory_accelerated_configuration factory_config;
  factory_config.equalizer_factory    = create_channel_equalizer_generic_factory();
  factory_config.precoder_factory     = create_dft_transform_precoder_factory(create_dft_processor_factory(), nof_prb);
  factory_config.demodulation_factory = create_demodulation_mapper_factory();
  factory_config.prg_factory          = create_pseudo_random_generator_sw_factory();
  factory_config.max_nof_prb          = nof_prb;
  return create_pusch_demodulator_factory_accelerated(factory_config)->create();
}

TEST(pusch_resident_dapp_adapter, publishes_cuda_views_and_pinned_dmrs_on_the_inherited_stream)
{
  auto target = std::make_shared<target_spy>();
  auto dapp   = create_pusch_resident_dapp(target,
                                           {},
                                         0,
                                         create_pseudo_random_generator_sw_factory()->create(),
                                         1,
                                         1,
                                         1,
                                         std::chrono::microseconds(150),
                                         1.0e-6F);
  ASSERT_TRUE(dapp);

  cudaStream_t stream{};
  ASSERT_EQ(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking), cudaSuccess);
  uint32_t* device_grid{};
  uint32_t* device_indices{};
  uint16_t* device_llrs{};
  float*    device_noise{};
  ASSERT_EQ(cudaMalloc(&device_grid, MAX_NSYMB_PER_SLOT * NOF_SUBCARRIERS_PER_RB * sizeof(uint32_t)), cudaSuccess);
  ASSERT_EQ(cudaMalloc(&device_indices, 18 * sizeof(uint32_t)), cudaSuccess);
  ASSERT_EQ(cudaMalloc(&device_llrs, 36 * sizeof(uint16_t)), cudaSuccess);
  ASSERT_EQ(cudaMalloc(&device_noise, 18 * sizeof(float)), cudaSuccess);

  const auto result = dapp->invoke_receiver({.grid_cbf16           = device_grid,
                                             .data_re_indices      = device_indices,
                                             .post_eq_noise_f32    = device_noise,
                                             .llrs_f16             = device_llrs,
                                             .nof_grid_ports       = 1,
                                             .nof_grid_symbols     = MAX_NSYMB_PER_SLOT,
                                             .nof_grid_subcarriers = NOF_SUBCARRIERS_PER_RB,
                                             .nof_data_re          = 18,
                                             .device_ordinal       = 0,
                                             .execution_stream     = reinterpret_cast<uintptr_t>(stream)},
                                            make_config());
  ASSERT_EQ(result.status, OCUDU_DAPP_OK_V1);
  EXPECT_FALSE(result.invoke_fallback);
  ASSERT_EQ(target->calls, 1U);
  EXPECT_EQ(target->invocation.worker_id, 41U);
  EXPECT_EQ(target->invocation.execution_stream, reinterpret_cast<uintptr_t>(stream));
  EXPECT_EQ(target->grid.domain, OCUDU_DAPP_MEM_ACCELERATOR_DEVICE_V1);
  EXPECT_EQ(target->grid.element_type, OCUDU_DAPP_ELEMENT_CBF16_V1);
  EXPECT_EQ(target->pusch.data_re_indices.native_handle, reinterpret_cast<uintptr_t>(device_indices));
  EXPECT_EQ(target->pusch.data_re_indices.layout_id, OCUDU_DAPP_LAYOUT_PUSCH_DATA_RE_INDICES_U32_V1);
  EXPECT_EQ(target->pusch.reserved[OCUDU_DAPP_PUSCH_RESERVED_RNTI_V1], to_value(make_config().rnti));
  EXPECT_EQ(target->pusch.reserved[OCUDU_DAPP_PUSCH_RESERVED_SUBCARRIER_SPACING_HZ_V1], 30000U);
  EXPECT_EQ(target->pusch.reserved[OCUDU_DAPP_PUSCH_RESERVED_GRID_NOF_SUBCARRIERS_V1], NOF_SUBCARRIERS_PER_RB);
  EXPECT_EQ(target->pusch.flags, OCUDU_DAPP_PUSCH_DATA_RE_SELECTION_COMPLETE_V1);
  EXPECT_EQ(target->llrs.native_handle, reinterpret_cast<uintptr_t>(device_llrs));
  EXPECT_EQ(target->llrs.layout_id, OCUDU_DAPP_LAYOUT_PUSCH_LLR_DATA_RE_LAYER_BIT_V1);
  EXPECT_EQ(target->eq_noise.native_handle, reinterpret_cast<uintptr_t>(device_noise));
  EXPECT_EQ(target->eq_noise.layout_id, OCUDU_DAPP_LAYOUT_PUSCH_POST_EQ_NOISE_DATA_RE_LAYER_V1);
  EXPECT_TRUE(dapp->receiver_post_eq_noise_valid());
  EXPECT_EQ(target->dmrs.nof_pilots, 6U);
  EXPECT_EQ(target->dmrs.flags,
            OCUDU_DAPP_DMRS_REFERENCE_SYMBOLS_LAYER_COMPLETE_V1 | OCUDU_DAPP_DMRS_COORDINATES_SHARED_ACROSS_LAYERS_V1);

  cf_t     first_reference{};
  uint16_t first_coordinate[2]{};
  ASSERT_EQ(cudaMemcpyAsync(&first_reference,
                            reinterpret_cast<const void*>(target->dmrs.reference_symbols.native_handle),
                            sizeof(first_reference),
                            cudaMemcpyDeviceToHost,
                            stream),
            cudaSuccess);
  ASSERT_EQ(cudaMemcpyAsync(first_coordinate,
                            reinterpret_cast<const void*>(target->dmrs.coordinates.native_handle),
                            sizeof(first_coordinate),
                            cudaMemcpyDeviceToHost,
                            stream),
            cudaSuccess);
  ASSERT_EQ(cudaStreamSynchronize(stream), cudaSuccess);
  EXPECT_NEAR(std::abs(first_reference), std::sqrt(2.0F), 1.0e-5F);
  EXPECT_EQ(first_coordinate[0], 2U);
  EXPECT_EQ(first_coordinate[1], 0U);

  cudaFree(device_noise);
  cudaFree(device_llrs);
  cudaFree(device_indices);
  cudaFree(device_grid);
  cudaStreamDestroy(stream);
}

TEST(pusch_resident_dapp_adapter, receiver_metric_validity_is_reset_for_every_invocation)
{
  auto target = std::make_shared<target_spy>();
  auto dapp   = create_pusch_resident_dapp(target,
                                           {},
                                         0,
                                         create_pseudo_random_generator_sw_factory()->create(),
                                         1,
                                         1,
                                         1,
                                         std::chrono::microseconds(150),
                                         1.0e-6F);
  ASSERT_TRUE(dapp);

  cudaStream_t stream{};
  ASSERT_EQ(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking), cudaSuccess);
  uint32_t* device_grid{};
  uint32_t* device_indices{};
  uint16_t* device_llrs{};
  float*    device_noise{};
  ASSERT_EQ(cudaMalloc(&device_grid, MAX_NSYMB_PER_SLOT * NOF_SUBCARRIERS_PER_RB * sizeof(uint32_t)), cudaSuccess);
  ASSERT_EQ(cudaMalloc(&device_indices, 18 * sizeof(uint32_t)), cudaSuccess);
  ASSERT_EQ(cudaMalloc(&device_llrs, 36 * sizeof(uint16_t)), cudaSuccess);
  ASSERT_EQ(cudaMalloc(&device_noise, 18 * sizeof(float)), cudaSuccess);
  const pusch_resident_dapp_buffers buffers{.grid_cbf16           = device_grid,
                                            .data_re_indices      = device_indices,
                                            .post_eq_noise_f32    = device_noise,
                                            .llrs_f16             = device_llrs,
                                            .nof_grid_ports       = 1,
                                            .nof_grid_symbols     = MAX_NSYMB_PER_SLOT,
                                            .nof_grid_subcarriers = NOF_SUBCARRIERS_PER_RB,
                                            .nof_data_re          = 18,
                                            .device_ordinal       = 0,
                                            .execution_stream     = reinterpret_cast<uintptr_t>(stream)};

  ASSERT_EQ(dapp->invoke_receiver(buffers, make_config()).status, OCUDU_DAPP_OK_V1);
  EXPECT_TRUE(dapp->receiver_post_eq_noise_valid());
  target->publish_receiver_metrics = false;
  ASSERT_EQ(dapp->invoke_receiver(buffers, make_config()).status, OCUDU_DAPP_OK_V1);
  EXPECT_FALSE(dapp->receiver_post_eq_noise_valid());

  cudaFree(device_noise);
  cudaFree(device_llrs);
  cudaFree(device_indices);
  cudaFree(device_grid);
  cudaStreamDestroy(stream);
}

TEST(pusch_resident_dapp_adapter, reports_async_output_deadline_without_claiming_current_call_fallback)
{
  auto target = std::make_shared<target_spy>();
  auto dapp   = create_pusch_resident_dapp(
      target, {}, 0, create_pseudo_random_generator_sw_factory()->create(), 1, 1, 1, 1ns, 1.0e-6F);
  ASSERT_TRUE(dapp);
  EXPECT_FALSE(dapp->diagnostic_timing(0).valid);
  dapp->enable_diagnostic_timing(true);

  cudaStream_t stream{};
  ASSERT_EQ(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking), cudaSuccess);
  uint32_t* device_grid{};
  uint32_t* device_indices{};
  uint16_t* device_llrs{};
  ASSERT_EQ(cudaMalloc(&device_grid, MAX_NSYMB_PER_SLOT * NOF_SUBCARRIERS_PER_RB * sizeof(uint32_t)), cudaSuccess);
  ASSERT_EQ(cudaMalloc(&device_indices, 18 * sizeof(uint32_t)), cudaSuccess);
  ASSERT_EQ(cudaMalloc(&device_llrs, 36 * sizeof(uint16_t)), cudaSuccess);

  const auto invoked = dapp->invoke_receiver({.grid_cbf16           = device_grid,
                                              .data_re_indices      = device_indices,
                                              .llrs_f16             = device_llrs,
                                              .nof_grid_ports       = 1,
                                              .nof_grid_symbols     = MAX_NSYMB_PER_SLOT,
                                              .nof_grid_subcarriers = NOF_SUBCARRIERS_PER_RB,
                                              .nof_data_re          = 18,
                                              .device_ordinal       = 0,
                                              .completion_slot      = 0,
                                              .execution_stream     = reinterpret_cast<uintptr_t>(stream)},
                                             make_config());
  ASSERT_EQ(invoked.status, OCUDU_DAPP_OK_V1);
  EXPECT_FALSE(invoked.invoke_fallback);
  EXPECT_FALSE(dapp->diagnostic_timing(0).valid);
  ASSERT_TRUE(dapp->on_inline_output_enqueued(0, true));
  const auto timing = dapp->diagnostic_timing(0);
  ASSERT_TRUE(timing.valid);
  EXPECT_EQ(timing.completion_slot, 0U);
  ASSERT_NE(timing.module_begin_token, nullptr);
  ASSERT_NE(timing.module_end_token, nullptr);
  ASSERT_NE(timing.inline_end_token, nullptr);
  ASSERT_EQ(cudaStreamSynchronize(stream), cudaSuccess);
  float module_ms{};
  float continuation_ms{};
  ASSERT_EQ(cudaEventElapsedTime(&module_ms,
                                 static_cast<cudaEvent_t>(timing.module_begin_token),
                                 static_cast<cudaEvent_t>(timing.module_end_token)),
            cudaSuccess);
  ASSERT_EQ(cudaEventElapsedTime(&continuation_ms,
                                 static_cast<cudaEvent_t>(timing.module_end_token),
                                 static_cast<cudaEvent_t>(timing.inline_end_token)),
            cudaSuccess);
  EXPECT_GE(module_ms, 0.0F);
  EXPECT_GE(continuation_ms, 0.0F);
  dapp->observe_inline_completion(0);
  ASSERT_EQ(target->completion_deadlines.size(), 1U);
  EXPECT_EQ(target->completion_deadlines.front().mode, pusch_dapp_demodulator_mode::receiver);
  EXPECT_EQ(target->completion_deadlines.front().invocation_id, target->invocation.invocation_id);
  EXPECT_GT(target->completion_deadlines.front().observed_latency_ns, target->completion_deadlines.front().deadline_ns);
  EXPECT_EQ(target->completion_deadlines.front().deadline_ns, 1U);
  EXPECT_TRUE(target->fallback_reasons.empty());

  dapp->on_completion(nullptr);
  cudaFree(device_llrs);
  cudaFree(device_indices);
  cudaFree(device_grid);
  cudaStreamDestroy(stream);
}

TEST(pusch_resident_dapp_adapter, applies_the_independent_channel_estimator_deadline)
{
  auto demod_target   = std::make_shared<target_spy>();
  demod_target->mode  = pusch_dapp_demodulator_mode::inactive;
  auto channel_target = std::make_shared<channel_target_spy>();
  auto dapp           = create_pusch_resident_dapp(demod_target,
                                         channel_target,
                                         0,
                                         create_pseudo_random_generator_sw_factory()->create(),
                                         1,
                                         1,
                                         1,
                                         1s,
                                         1.0e-6F,
                                         1ns);
  ASSERT_TRUE(dapp);

  cudaStream_t stream{};
  ASSERT_EQ(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking), cudaSuccess);
  uint32_t* device_grid{};
  uint32_t* device_indices{};
  ASSERT_EQ(cudaMalloc(&device_grid, MAX_NSYMB_PER_SLOT * NOF_SUBCARRIERS_PER_RB * sizeof(uint32_t)), cudaSuccess);
  ASSERT_EQ(cudaMalloc(&device_indices, 18 * sizeof(uint32_t)), cudaSuccess);

  const auto invoked = dapp->invoke_channel_estimator({.grid_cbf16           = device_grid,
                                                       .data_re_indices      = device_indices,
                                                       .nof_grid_ports       = 1,
                                                       .nof_grid_symbols     = MAX_NSYMB_PER_SLOT,
                                                       .nof_grid_subcarriers = NOF_SUBCARRIERS_PER_RB,
                                                       .nof_data_re          = 18,
                                                       .device_ordinal       = 0,
                                                       .completion_slot      = 0,
                                                       .execution_stream     = reinterpret_cast<uintptr_t>(stream)},
                                                      make_config());
  ASSERT_EQ(invoked.status, OCUDU_DAPP_OK_V1);
  EXPECT_FALSE(invoked.invoke_fallback);
  ASSERT_TRUE(dapp->on_inline_output_enqueued(0, true));
  ASSERT_EQ(cudaStreamSynchronize(stream), cudaSuccess);
  dapp->observe_inline_completion(0);
  ASSERT_EQ(channel_target->completion_deadlines.size(), 1U);
  EXPECT_EQ(channel_target->completion_deadlines.front().invocation_id, channel_target->invocation.invocation_id);
  EXPECT_GT(channel_target->completion_deadlines.front().observed_latency_ns,
            channel_target->completion_deadlines.front().deadline_ns);
  EXPECT_EQ(channel_target->completion_deadlines.front().deadline_ns, 1U);
  EXPECT_TRUE(demod_target->completion_deadlines.empty());

  dapp->on_completion(nullptr);
  cudaFree(device_indices);
  cudaFree(device_grid);
  cudaStreamDestroy(stream);
}

TEST(pusch_resident_dapp_adapter, publishes_two_layer_dmrs_occ_and_compact_output_shapes)
{
  auto target = std::make_shared<target_spy>();
  auto dapp   = create_pusch_resident_dapp(
      target, {}, 0, create_pseudo_random_generator_sw_factory()->create(), 1, 2, 2, 150us, 1.0e-6F);
  ASSERT_TRUE(dapp);

  cudaStream_t stream{};
  ASSERT_EQ(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking), cudaSuccess);
  uint32_t* device_grid{};
  uint32_t* device_indices{};
  uint16_t* device_llrs{};
  ASSERT_EQ(cudaMalloc(&device_grid, 2 * MAX_NSYMB_PER_SLOT * NOF_SUBCARRIERS_PER_RB * sizeof(uint32_t)), cudaSuccess);
  ASSERT_EQ(cudaMalloc(&device_indices, 18 * sizeof(uint32_t)), cudaSuccess);
  ASSERT_EQ(cudaMalloc(&device_llrs, 2 * 2 * 18 * sizeof(uint16_t)), cudaSuccess);

  auto config          = make_config();
  config.nof_tx_layers = 2;
  config.rx_ports.push_back(1);
  const auto result = dapp->invoke_receiver({.grid_cbf16           = device_grid,
                                             .data_re_indices      = device_indices,
                                             .llrs_f16             = device_llrs,
                                             .nof_grid_ports       = 2,
                                             .nof_grid_symbols     = MAX_NSYMB_PER_SLOT,
                                             .nof_grid_subcarriers = NOF_SUBCARRIERS_PER_RB,
                                             .nof_data_re          = 18,
                                             .device_ordinal       = 0,
                                             .execution_stream     = reinterpret_cast<uintptr_t>(stream)},
                                            config);
  ASSERT_EQ(result.status, OCUDU_DAPP_OK_V1);
  ASSERT_EQ(target->calls, 1U);
  EXPECT_EQ(target->pusch.nof_layers, 2U);
  EXPECT_EQ(target->dmrs.reference_symbols.dimensions[0], 2U);
  EXPECT_EQ(target->dmrs.reference_symbols.dimensions[1], 6U);
  EXPECT_EQ(target->llrs.dimensions[0], 18U);
  EXPECT_EQ(target->llrs.dimensions[1], 2U);
  EXPECT_EQ(target->llrs.dimensions[2], 2U);

  std::array<cf_t, 12> references{};
  ASSERT_EQ(cudaMemcpyAsync(references.data(),
                            reinterpret_cast<const void*>(target->dmrs.reference_symbols.native_handle),
                            sizeof(references),
                            cudaMemcpyDeviceToHost,
                            stream),
            cudaSuccess);
  ASSERT_EQ(cudaStreamSynchronize(stream), cudaSuccess);
  for (unsigned pilot = 0; pilot != 6; ++pilot) {
    const cf_t expected = references[pilot] * (pilot % 2 == 0 ? 1.0F : -1.0F);
    EXPECT_NEAR(std::abs(references[6 + pilot] - expected), 0.0F, 1.0e-5F);
  }

  cudaFree(device_llrs);
  cudaFree(device_indices);
  cudaFree(device_grid);
  cudaStreamDestroy(stream);
}

TEST(pusch_resident_dapp_adapter, publishes_two_layer_channel_estimator_outputs)
{
  auto demod_target   = std::make_shared<target_spy>();
  demod_target->mode  = pusch_dapp_demodulator_mode::inactive;
  auto channel_target = std::make_shared<channel_target_spy>();
  auto dapp           = create_pusch_resident_dapp(
      demod_target, channel_target, 0, create_pseudo_random_generator_sw_factory()->create(), 1, 2, 2, 100us, 1.0e-6F);
  ASSERT_TRUE(dapp);
  auto config          = make_config();
  config.nof_tx_layers = 2;
  config.rx_ports.push_back(1);
  cudaStream_t stream{};
  ASSERT_EQ(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking), cudaSuccess);
  uint32_t* device_grid{};
  uint32_t* device_indices{};
  ASSERT_EQ(cudaMalloc(&device_grid, 2 * MAX_NSYMB_PER_SLOT * NOF_SUBCARRIERS_PER_RB * sizeof(uint32_t)), cudaSuccess);
  ASSERT_EQ(cudaMalloc(&device_indices, 18 * sizeof(uint32_t)), cudaSuccess);
  const auto result = dapp->invoke_channel_estimator({.grid_cbf16           = device_grid,
                                                      .data_re_indices      = device_indices,
                                                      .nof_grid_ports       = 2,
                                                      .nof_grid_symbols     = MAX_NSYMB_PER_SLOT,
                                                      .nof_grid_subcarriers = NOF_SUBCARRIERS_PER_RB,
                                                      .nof_data_re          = 18,
                                                      .device_ordinal       = 0,
                                                      .execution_stream     = reinterpret_cast<uintptr_t>(stream)},
                                                     config);
  EXPECT_EQ(result.status, OCUDU_DAPP_OK_V1);
  EXPECT_FALSE(result.invoke_fallback);
  ASSERT_EQ(channel_target->calls, 1U);
  EXPECT_EQ(channel_target->pusch.nof_layers, 2U);
  EXPECT_EQ(channel_target->channel.dimensions[1], 2U);
  EXPECT_EQ(channel_target->noise.dimensions[1], 2U);
  EXPECT_EQ(dapp->channel_scheduler_noise_f32(), nullptr);
  cudaFree(device_indices);
  cudaFree(device_grid);
  cudaStreamDestroy(stream);
}

TEST(pusch_resident_dapp_adapter, bypasses_unsupported_or_non_cuda_invocations_before_the_module)
{
  auto target = std::make_shared<target_spy>();
  auto dapp   = create_pusch_resident_dapp(target,
                                           {},
                                         0,
                                         create_pseudo_random_generator_sw_factory()->create(),
                                         1,
                                         1,
                                         1,
                                         std::chrono::microseconds(150),
                                         1.0e-6F);
  ASSERT_TRUE(dapp);
  auto config       = make_config();
  config.modulation = modulation_scheme::QAM16;
  auto result       = dapp->invoke_receiver({}, config);
  EXPECT_EQ(result.status, OCUDU_DAPP_UNSUPPORTED_V1);
  EXPECT_EQ(target->calls, 0U);
  EXPECT_EQ(target->fallback_reasons,
            (std::vector<std::pair<pusch_dapp_demodulator_mode, dapp::class_a_fallback_reason>>{
                {pusch_dapp_demodulator_mode::receiver, dapp::class_a_fallback_reason::unsupported_metadata}}));
  target->backend = OCUDU_DAPP_BACKEND_HOST_V1;
  result          = dapp->invoke_receiver({}, make_config());
  EXPECT_EQ(result.status, OCUDU_DAPP_BYPASS_V1);
  EXPECT_EQ(target->calls, 0U);
}

TEST(pusch_resident_dapp_adapter, publishes_compact_cuda_equalizer_outputs)
{
  auto target  = std::make_shared<target_spy>();
  target->mode = pusch_dapp_demodulator_mode::equalizer;
  auto dapp    = create_pusch_resident_dapp(target,
                                            {},
                                         0,
                                         create_pseudo_random_generator_sw_factory()->create(),
                                         1,
                                         1,
                                         1,
                                         std::chrono::microseconds(150),
                                         1.0e-6F);
  ASSERT_TRUE(dapp);

  cudaStream_t stream{};
  ASSERT_EQ(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking), cudaSuccess);
  uint32_t* device_grid{};
  uint32_t* device_indices{};
  float2*   device_equalized{};
  float*    device_noise{};
  ASSERT_EQ(cudaMalloc(&device_grid, MAX_NSYMB_PER_SLOT * NOF_SUBCARRIERS_PER_RB * sizeof(uint32_t)), cudaSuccess);
  ASSERT_EQ(cudaMalloc(&device_indices, 18 * sizeof(uint32_t)), cudaSuccess);
  ASSERT_EQ(cudaMalloc(&device_equalized, 18 * sizeof(float2)), cudaSuccess);
  ASSERT_EQ(cudaMalloc(&device_noise, 18 * sizeof(float)), cudaSuccess);

  const auto result = dapp->invoke_equalizer({.grid_cbf16           = device_grid,
                                              .data_re_indices      = device_indices,
                                              .equalized_cf32       = device_equalized,
                                              .post_eq_noise_f32    = device_noise,
                                              .nof_grid_ports       = 1,
                                              .nof_grid_symbols     = MAX_NSYMB_PER_SLOT,
                                              .nof_grid_subcarriers = NOF_SUBCARRIERS_PER_RB,
                                              .nof_data_re          = 18,
                                              .device_ordinal       = 0,
                                              .execution_stream     = reinterpret_cast<uintptr_t>(stream)},
                                             make_config());
  ASSERT_EQ(result.status, OCUDU_DAPP_OK_V1);
  EXPECT_EQ(target->calls, 1U);
  EXPECT_EQ(target->equalized.native_handle, reinterpret_cast<uintptr_t>(device_equalized));
  EXPECT_EQ(target->equalized.layout_id, OCUDU_DAPP_LAYOUT_PUSCH_EQUALIZED_DATA_RE_LAYER_V1);
  EXPECT_EQ(target->equalized.dimensions[0], 18U);
  EXPECT_EQ(target->eq_noise.native_handle, reinterpret_cast<uintptr_t>(device_noise));
  EXPECT_EQ(target->eq_noise.layout_id, OCUDU_DAPP_LAYOUT_PUSCH_POST_EQ_NOISE_DATA_RE_LAYER_V1);

  dapp->on_completion(nullptr);
  cudaFree(device_noise);
  cudaFree(device_equalized);
  cudaFree(device_indices);
  cudaFree(device_grid);
  cudaStreamDestroy(stream);
}

// Msg3 stamps RAPID on the PUSCH PDU (MAC context / FAPI optional rapid). Equalizer modules never
// use RAPID (scrambling is post-EQ), so the resident path must still invoke NeuralEq / Class-A EQ.
TEST(pusch_resident_dapp_adapter, equalizer_accepts_msg3_rapid_and_uci_pipeline_flags)
{
  auto target  = std::make_shared<target_spy>();
  target->mode = pusch_dapp_demodulator_mode::equalizer;
  auto dapp    = create_pusch_resident_dapp(target,
                                            {},
                                         0,
                                         create_pseudo_random_generator_sw_factory()->create(),
                                         1,
                                         1,
                                         1,
                                         std::chrono::microseconds(150),
                                         1.0e-6F);
  ASSERT_TRUE(dapp);

  cudaStream_t stream{};
  ASSERT_EQ(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking), cudaSuccess);
  uint32_t* device_grid{};
  uint32_t* device_indices{};
  float2*   device_equalized{};
  float*    device_noise{};
  ASSERT_EQ(cudaMalloc(&device_grid, MAX_NSYMB_PER_SLOT * NOF_SUBCARRIERS_PER_RB * sizeof(uint32_t)), cudaSuccess);
  ASSERT_EQ(cudaMalloc(&device_indices, 18 * sizeof(uint32_t)), cudaSuccess);
  ASSERT_EQ(cudaMalloc(&device_equalized, 18 * sizeof(float2)), cudaSuccess);
  ASSERT_EQ(cudaMalloc(&device_noise, 18 * sizeof(float)), cudaSuccess);

  auto config                            = make_config();
  config.n_rapid                         = 17; // Msg3-style RAPID stamp
  config.resident.host_uci_demux         = true;
  config.resident.device_uci_demux       = true;
  config.resident.sch_compaction.enabled = true;

  const auto result = dapp->invoke_equalizer({.grid_cbf16           = device_grid,
                                              .data_re_indices      = device_indices,
                                              .equalized_cf32       = device_equalized,
                                              .post_eq_noise_f32    = device_noise,
                                              .nof_grid_ports       = 1,
                                              .nof_grid_symbols     = MAX_NSYMB_PER_SLOT,
                                              .nof_grid_subcarriers = NOF_SUBCARRIERS_PER_RB,
                                              .nof_data_re          = 18,
                                              .device_ordinal       = 0,
                                              .execution_stream     = reinterpret_cast<uintptr_t>(stream)},
                                             config);
  EXPECT_EQ(result.status, OCUDU_DAPP_OK_V1);
  EXPECT_FALSE(result.invoke_fallback);
  EXPECT_EQ(target->calls, 1U);
  EXPECT_TRUE(target->fallback_reasons.empty());

  dapp->on_completion(nullptr);
  cudaFree(device_noise);
  cudaFree(device_equalized);
  cudaFree(device_indices);
  cudaFree(device_grid);
  cudaStreamDestroy(stream);
}

// Full-receiver modules still need RAPID in the ABI for MsgA/Msg3 descrambling, so keep rejecting.
TEST(pusch_resident_dapp_adapter, receiver_still_rejects_msg3_rapid)
{
  auto target  = std::make_shared<target_spy>();
  target->mode = pusch_dapp_demodulator_mode::receiver;
  auto dapp    = create_pusch_resident_dapp(target,
                                            {},
                                         0,
                                         create_pseudo_random_generator_sw_factory()->create(),
                                         1,
                                         1,
                                         1,
                                         std::chrono::microseconds(150),
                                         1.0e-6F);
  ASSERT_TRUE(dapp);

  cudaStream_t stream{};
  ASSERT_EQ(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking), cudaSuccess);
  uint32_t* device_grid{};
  uint32_t* device_indices{};
  uint16_t* device_llrs{};
  ASSERT_EQ(cudaMalloc(&device_grid, MAX_NSYMB_PER_SLOT * NOF_SUBCARRIERS_PER_RB * sizeof(uint32_t)), cudaSuccess);
  ASSERT_EQ(cudaMalloc(&device_indices, 18 * sizeof(uint32_t)), cudaSuccess);
  ASSERT_EQ(cudaMalloc(&device_llrs, 18 * 4 * sizeof(uint16_t)), cudaSuccess);

  auto config       = make_config();
  config.n_rapid    = 17;
  const auto result = dapp->invoke_receiver({.grid_cbf16           = device_grid,
                                             .data_re_indices      = device_indices,
                                             .llrs_f16             = device_llrs,
                                             .nof_grid_ports       = 1,
                                             .nof_grid_symbols     = MAX_NSYMB_PER_SLOT,
                                             .nof_grid_subcarriers = NOF_SUBCARRIERS_PER_RB,
                                             .nof_data_re          = 18,
                                             .device_ordinal       = 0,
                                             .execution_stream     = reinterpret_cast<uintptr_t>(stream)},
                                            config);
  EXPECT_EQ(result.status, OCUDU_DAPP_UNSUPPORTED_V1);
  EXPECT_TRUE(result.invoke_fallback);
  EXPECT_EQ(target->calls, 0U);
  EXPECT_EQ(target->fallback_reasons,
            (std::vector<std::pair<pusch_dapp_demodulator_mode, dapp::class_a_fallback_reason>>{
                {pusch_dapp_demodulator_mode::receiver, dapp::class_a_fallback_reason::unsupported_metadata}}));

  cudaFree(device_llrs);
  cudaFree(device_indices);
  cudaFree(device_grid);
  cudaStreamDestroy(stream);
}

namespace {

/// A receiver on the primary route, an equalizer on slot 1 of the equalizer hook, and the stock
/// receiver, selected per UE like the native routed target.
class resident_routing_target final : public pusch_dapp_demodulator_target
{
public:
  resident_routing_target()
  {
    primary.mode   = pusch_dapp_demodulator_mode::receiver;
    equalizer.mode = pusch_dapp_demodulator_mode::equalizer;
    stock.mode     = pusch_dapp_demodulator_mode::inactive;
  }

  dapp::pusch_route resolve_route(uint32_t, uint16_t rnti) const noexcept override
  {
    return rnti == 0x4601   ? dapp::pusch_route::instance(dapp::pusch_route_hook::equalizer, 1)
           : rnti == 0x4602 ? dapp::pusch_route::conventional()
                            : dapp::pusch_route::primary();
  }
  pusch_dapp_demodulator_target& route(const dapp::pusch_route& selected) noexcept override
  {
    if (selected.type == dapp::pusch_route::kind::instance) {
      return equalizer;
    }
    return selected.type == dapp::pusch_route::kind::conventional ? static_cast<pusch_dapp_demodulator_target&>(stock)
                                                                  : primary;
  }

  pusch_dapp_demodulator_mode active_mode() const noexcept override { return primary.active_mode(); }
  uint32_t                    lane_count() const noexcept override { return primary.lane_count(); }
  uint32_t                    worker_id(uint32_t lane, pusch_dapp_demodulator_mode mode) const noexcept override
  {
    return primary.worker_id(lane, mode);
  }
  dapp::native_invocation_result invoke(uint32_t                             lane,
                                        const ocudu_dapp_equalizer_input_v1& input,
                                        ocudu_dapp_equalizer_output_v1&      output) noexcept override
  {
    return primary.invoke(lane, input, output);
  }
  dapp::native_invocation_result invoke(uint32_t                            lane,
                                        const ocudu_dapp_receiver_input_v1& input,
                                        ocudu_dapp_receiver_output_v1&      output) noexcept override
  {
    return primary.invoke(lane, input, output);
  }

  target_spy primary;
  target_spy equalizer;
  target_spy stock;
};

} // namespace

TEST(pusch_resident_dapp_adapter, the_selected_route_decides_the_mode_until_the_next_grant)
{
  auto target = std::make_shared<resident_routing_target>();
  auto dapp   = create_pusch_resident_dapp(target,
                                         nullptr,
                                         0,
                                         create_pseudo_random_generator_sw_factory()->create(),
                                         1,
                                         1,
                                         1,
                                         std::chrono::microseconds(100),
                                         1.0e-6F);
  ASSERT_TRUE(dapp);
  EXPECT_EQ(dapp->mode(), pusch_resident_dapp_mode::receiver) << "no selection is the primary route";

  ASSERT_TRUE(dapp->begin_grant());
  dapp->select_route(dapp->resolve_route(0, 0x4601));
  EXPECT_EQ(dapp->mode(), pusch_resident_dapp_mode::equalizer);

  ASSERT_TRUE(dapp->begin_grant());
  EXPECT_EQ(dapp->mode(), pusch_resident_dapp_mode::receiver) << "every grant starts on the primary route";
  dapp->select_route(dapp->resolve_route(0, 0x4602));
  EXPECT_EQ(dapp->mode(), pusch_resident_dapp_mode::inactive) << "the stock receiver serves this UE";

  ASSERT_TRUE(dapp->begin_grant());
  dapp->select_route(dapp->resolve_route(0, 0x1234));
  EXPECT_EQ(dapp->mode(), pusch_resident_dapp_mode::receiver);
}

TEST(pusch_resident_dapp_adapter, publishes_cuda_channel_estimator_outputs)
{
  auto demod_target                       = std::make_shared<target_spy>();
  demod_target->mode                      = pusch_dapp_demodulator_mode::inactive;
  auto channel_target                     = std::make_shared<channel_target_spy>();
  channel_target->publish_scheduler_noise = true;
  auto dapp                               = create_pusch_resident_dapp(demod_target,
                                         channel_target,
                                         0,
                                         create_pseudo_random_generator_sw_factory()->create(),
                                         1,
                                         1,
                                         1,
                                         std::chrono::microseconds(100),
                                         1.0e-6F);
  ASSERT_TRUE(dapp);
  EXPECT_EQ(dapp->mode(), pusch_resident_dapp_mode::channel_estimator);

  cudaStream_t stream{};
  ASSERT_EQ(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking), cudaSuccess);
  uint32_t* device_grid{};
  uint32_t* device_indices{};
  ASSERT_EQ(cudaMalloc(&device_grid, MAX_NSYMB_PER_SLOT * NOF_SUBCARRIERS_PER_RB * sizeof(uint32_t)), cudaSuccess);
  ASSERT_EQ(cudaMalloc(&device_indices, 18 * sizeof(uint32_t)), cudaSuccess);
  const auto result = dapp->invoke_channel_estimator({.grid_cbf16           = device_grid,
                                                      .data_re_indices      = device_indices,
                                                      .nof_grid_ports       = 1,
                                                      .nof_grid_symbols     = MAX_NSYMB_PER_SLOT,
                                                      .nof_grid_subcarriers = NOF_SUBCARRIERS_PER_RB,
                                                      .nof_data_re          = 18,
                                                      .device_ordinal       = 0,
                                                      .execution_stream     = reinterpret_cast<uintptr_t>(stream)},
                                                     make_config());
  ASSERT_EQ(result.status, OCUDU_DAPP_OK_V1);
  EXPECT_EQ(channel_target->calls, 1U);
  EXPECT_EQ(channel_target->channel.native_handle, reinterpret_cast<uintptr_t>(dapp->channel_estimates_cf32()));
  EXPECT_EQ(channel_target->channel.layout_id, OCUDU_DAPP_LAYOUT_PUSCH_CHANNEL_PORT_LAYER_SYMBOL_SUBCARRIER_V1);
  EXPECT_EQ(channel_target->noise.native_handle, reinterpret_cast<uintptr_t>(dapp->channel_noise_f32()));
  EXPECT_EQ(channel_target->output_reserved[OCUDU_DAPP_CHANNEL_ESTIMATOR_SCHED_NOISE_REQUEST_INDEX_V1],
            OCUDU_DAPP_CHANNEL_ESTIMATOR_SCHED_NOISE_REQUEST_V1);
  EXPECT_EQ(channel_target->output_reserved[OCUDU_DAPP_CHANNEL_ESTIMATOR_SCHED_NOISE_HANDLE_INDEX_V1],
            reinterpret_cast<uintptr_t>(dapp->channel_scheduler_noise_f32()));
  EXPECT_EQ(channel_target->output_reserved[OCUDU_DAPP_CHANNEL_ESTIMATOR_SCHED_NOISE_BYTES_INDEX_V1], sizeof(float));
  EXPECT_EQ(channel_target->output_reserved[OCUDU_DAPP_CHANNEL_ESTIMATOR_SCHED_NOISE_RESPONSE_INDEX_V1],
            OCUDU_DAPP_CHANNEL_ESTIMATOR_SCHED_NOISE_RESPONSE_V1);

  dapp->on_completion(nullptr);
  cudaFree(device_indices);
  cudaFree(device_grid);
  cudaStreamDestroy(stream);
}

TEST(pusch_resident_dapp_adapter, accelerated_receiver_uses_dapp_output_and_existing_resident_completion_handoff)
{
  constexpr unsigned                                  nof_prb = 25;
  pusch_demodulator_factory_accelerated_configuration factory_config;
  factory_config.equalizer_factory    = create_channel_equalizer_generic_factory();
  factory_config.precoder_factory     = create_dft_transform_precoder_factory(create_dft_processor_factory(), nof_prb);
  factory_config.demodulation_factory = create_demodulation_mapper_factory();
  factory_config.prg_factory          = create_pseudo_random_generator_sw_factory();
  factory_config.max_nof_prb          = nof_prb;
  auto factory                        = create_pusch_demodulator_factory_accelerated(factory_config);
  ASSERT_TRUE(factory);
  auto demodulator = factory->create();
  ASSERT_TRUE(demodulator);
  ASSERT_TRUE(demodulator->supports_resident_mode());
  auto dapp = std::make_shared<resident_receiver_spy>();
  ASSERT_TRUE(demodulator->install_resident_dapp(dapp));
  demodulator->enable_resident_mode();
  demodulator->enable_symbol_capture();

  auto config    = make_config();
  config.rb_mask = crb_bitmap(nof_prb);
  for (unsigned rb = 0; rb != nof_prb; ++rb) {
    config.rb_mask.set(rb);
  }
  resource_grid_reader_spy grid(1, MAX_NSYMB_PER_SLOT, nof_prb);
  empty_results            estimates;
  codeword_spy             codeword;
  notifier_spy             notifier;
  demodulator->demodulate(codeword, notifier, grid, estimates, config);

  ASSERT_EQ(dapp->calls, 1U);
  ASSERT_EQ(dapp->completions, 1U);
  EXPECT_EQ(dapp->last.nof_data_re, 18U * nof_prb);
  const resident_softbit_buffer resident = demodulator->get_resident_softbits();
  ASSERT_TRUE(resident.valid);
  ASSERT_NE(resident.data, nullptr);
  EXPECT_EQ(resident.nof_softbits, 36U * nof_prb);
  EXPECT_EQ(dapp->completion_token, resident.completion_token);
  ASSERT_EQ(cudaEventSynchronize(static_cast<cudaEvent_t>(resident.completion_token)), cudaSuccess);
  std::array<uint16_t, 4> llrs{};
  ASSERT_EQ(cudaMemcpy(llrs.data(), resident.data, sizeof(llrs), cudaMemcpyDeviceToHost), cudaSuccess);
  for (uint16_t value : llrs) {
    EXPECT_EQ(value & 0x7fffU, 0U);
  }
  // This full receiver intentionally returns LLRs only. A noise-reduction
  // extent is not evidence that d_symbols_ was filled for this invocation.
  demodulator->wait_captured_symbols();
  EXPECT_TRUE(demodulator->get_captured_symbols().empty());
  EXPECT_EQ(codeword.blocks, 0U);
  EXPECT_EQ(codeword.end_calls, 0U);
  demodulator->report_deferred_sinr(notifier);
  EXPECT_EQ(notifier.end_calls, 1U);
}

TEST(pusch_resident_dapp_adapter, accelerated_re_index_cache_tracks_exact_sparse_allocation_per_device_buffer)
{
  constexpr unsigned                                  nof_prb = 5;
  pusch_demodulator_factory_accelerated_configuration factory_config;
  factory_config.equalizer_factory    = create_channel_equalizer_generic_factory();
  factory_config.precoder_factory     = create_dft_transform_precoder_factory(create_dft_processor_factory(), nof_prb);
  factory_config.demodulation_factory = create_demodulation_mapper_factory();
  factory_config.prg_factory          = create_pseudo_random_generator_sw_factory();
  factory_config.max_nof_prb          = nof_prb;
  auto factory                        = create_pusch_demodulator_factory_accelerated(factory_config);
  ASSERT_TRUE(factory);
  auto demodulator = factory->create();
  ASSERT_TRUE(demodulator);
  auto dapp = std::make_shared<resident_receiver_spy>();
  ASSERT_TRUE(demodulator->install_resident_dapp(dapp));
  demodulator->enable_resident_mode();

  auto       config = make_config();
  crb_bitmap allocation_a(nof_prb);
  crb_bitmap allocation_b(nof_prb);
  for (unsigned rb : {0U, 1U, 4U}) {
    allocation_a.set(rb);
  }
  for (unsigned rb : {0U, 3U, 4U}) {
    allocation_b.set(rb);
  }
  ASSERT_EQ(allocation_a.count(), allocation_b.count());
  ASSERT_EQ(allocation_a.find_lowest(), allocation_b.find_lowest());
  ASSERT_EQ(allocation_a.find_highest(), allocation_b.find_highest());

  resource_grid_reader_spy grid(1, MAX_NSYMB_PER_SLOT, nof_prb);
  empty_results            estimates;
  codeword_spy             codeword;
  notifier_spy             notifier;
  auto                     capture_indices = [&](const crb_bitmap& allocation) {
    config.rb_mask = allocation;
    demodulator->demodulate(codeword, notifier, grid, estimates, config);
    const resident_softbit_buffer output = demodulator->get_resident_softbits();
    EXPECT_TRUE(output.valid);
    EXPECT_EQ(cudaEventSynchronize(static_cast<cudaEvent_t>(output.completion_token)), cudaSuccess);
    std::vector<uint32_t> indices(dapp->last.nof_data_re);
    EXPECT_EQ(cudaMemcpy(indices.data(),
                         dapp->last.data_re_indices,
                         indices.size() * sizeof(indices.front()),
                         cudaMemcpyDeviceToHost),
              cudaSuccess);
    return indices;
  };

  const auto first  = capture_indices(allocation_a); // Device buffer 0 stores allocation A.
  const auto second = capture_indices(allocation_b); // Host cache must distinguish B.
  const auto third  = capture_indices(allocation_b); // Device buffer 2 stores B.
  const auto fourth = capture_indices(allocation_b); // Device buffer 0 must replace A with B.
  EXPECT_EQ(dapp->calls, 4U);
  EXPECT_EQ(dapp->completions, 4U);
  EXPECT_NE(first, fourth);
  EXPECT_EQ(second, third);
  EXPECT_EQ(second, fourth);

  std::array<bool, nof_prb> seen{};
  for (uint32_t index : fourth) {
    const unsigned rb = (index % (nof_prb * NOF_SUBCARRIERS_PER_RB)) / NOF_SUBCARRIERS_PER_RB;
    ASSERT_LT(rb, seen.size());
    EXPECT_TRUE(allocation_b.test(rb));
    seen[rb] = true;
  }
  for (unsigned rb = 0; rb != nof_prb; ++rb) {
    EXPECT_EQ(seen[rb], allocation_b.test(rb));
  }
}

TEST(pusch_resident_dapp_adapter, accelerated_receiver_failure_matches_same_stream_conventional_fallback)
{
  constexpr unsigned nof_prb      = 25;
  auto               conventional = make_accelerated_demodulator(nof_prb);
  auto               guarded      = make_accelerated_demodulator(nof_prb);
  ASSERT_TRUE(conventional);
  ASSERT_TRUE(guarded);
  conventional->enable_resident_mode();
  guarded->enable_resident_mode();
  auto dapp       = std::make_shared<resident_receiver_spy>();
  dapp->fill_byte = 0x7e;
  dapp->result    = {OCUDU_DAPP_INVALID_OUTPUT_V1, true};
  ASSERT_TRUE(guarded->install_resident_dapp(dapp));

  auto config          = make_config();
  config.nof_tx_layers = 2;
  config.rx_ports.push_back(1);
  config.rb_mask = crb_bitmap(nof_prb);
  config.rb_mask.fill(0, nof_prb);
  resource_grid_reader_spy grid(2, MAX_NSYMB_PER_SLOT, nof_prb);
  test_support::populate_identity_pusch_grid(grid, config, nof_prb);
  empty_results estimates;
  codeword_spy  codeword;
  notifier_spy  notifier;
  conventional->demodulate(codeword, notifier, grid, estimates, config);
  guarded->demodulate(codeword, notifier, grid, estimates, config);

  const auto expected = conventional->get_resident_softbits();
  const auto actual   = guarded->get_resident_softbits();
  ASSERT_TRUE(expected.valid);
  ASSERT_TRUE(actual.valid);
  ASSERT_EQ(actual.nof_softbits, expected.nof_softbits);
  ASSERT_EQ(cudaEventSynchronize(static_cast<cudaEvent_t>(expected.completion_token)), cudaSuccess);
  ASSERT_EQ(cudaEventSynchronize(static_cast<cudaEvent_t>(actual.completion_token)), cudaSuccess);
  std::vector<uint16_t> expected_llrs(expected.nof_softbits);
  std::vector<uint16_t> actual_llrs(actual.nof_softbits);
  ASSERT_EQ(
      cudaMemcpy(expected_llrs.data(), expected.data, expected_llrs.size() * sizeof(uint16_t), cudaMemcpyDeviceToHost),
      cudaSuccess);
  ASSERT_EQ(cudaMemcpy(actual_llrs.data(), actual.data, actual_llrs.size() * sizeof(uint16_t), cudaMemcpyDeviceToHost),
            cudaSuccess);
  EXPECT_EQ(actual_llrs, expected_llrs);
  EXPECT_EQ(dapp->calls, 1U);
  EXPECT_EQ(dapp->completions, 1U);
  EXPECT_EQ(dapp->completion_token, actual.completion_token);
}

TEST(pusch_resident_dapp_adapter, accelerated_equalizer_rejoins_resident_demapping_and_decode_handoff)
{
  constexpr unsigned                                  nof_prb = 25;
  pusch_demodulator_factory_accelerated_configuration factory_config;
  factory_config.equalizer_factory    = create_channel_equalizer_generic_factory();
  factory_config.precoder_factory     = create_dft_transform_precoder_factory(create_dft_processor_factory(), nof_prb);
  factory_config.demodulation_factory = create_demodulation_mapper_factory();
  factory_config.prg_factory          = create_pseudo_random_generator_sw_factory();
  factory_config.max_nof_prb          = nof_prb;
  auto factory                        = create_pusch_demodulator_factory_accelerated(factory_config);
  ASSERT_TRUE(factory);
  auto demodulator = factory->create();
  ASSERT_TRUE(demodulator);
  auto dapp = std::make_shared<resident_equalizer_spy>();
  ASSERT_TRUE(demodulator->install_resident_dapp(dapp));
  demodulator->enable_resident_mode();

  auto config    = make_config();
  config.rb_mask = crb_bitmap(nof_prb);
  for (unsigned rb = 0; rb != nof_prb; ++rb) {
    config.rb_mask.set(rb);
  }
  resource_grid_reader_spy grid(1, MAX_NSYMB_PER_SLOT, nof_prb);
  empty_results            estimates;
  codeword_spy             codeword;
  notifier_spy             notifier;
  demodulator->demodulate(codeword, notifier, grid, estimates, config);

  ASSERT_EQ(dapp->calls, 1U);
  ASSERT_EQ(dapp->completions, 1U);
  EXPECT_NE(dapp->last.equalized_cf32, nullptr);
  EXPECT_NE(dapp->last.post_eq_noise_f32, nullptr);
  const resident_softbit_buffer resident = demodulator->get_resident_softbits();
  ASSERT_TRUE(resident.valid);
  EXPECT_EQ(dapp->completion_token, resident.completion_token);
  ASSERT_EQ(cudaEventSynchronize(static_cast<cudaEvent_t>(resident.completion_token)), cudaSuccess);
  EXPECT_EQ(codeword.blocks, 0U);
  EXPECT_EQ(codeword.end_calls, 0U);
}

TEST(pusch_resident_dapp_adapter, accelerated_equalizer_failure_matches_same_stream_conventional_fallback)
{
  constexpr unsigned nof_prb      = 25;
  auto               conventional = make_accelerated_demodulator(nof_prb);
  auto               guarded      = make_accelerated_demodulator(nof_prb);
  ASSERT_TRUE(conventional);
  ASSERT_TRUE(guarded);
  conventional->enable_resident_mode();
  guarded->enable_resident_mode();
  auto dapp       = std::make_shared<resident_equalizer_spy>();
  dapp->fill_byte = 0x7e;
  dapp->result    = {OCUDU_DAPP_INVALID_OUTPUT_V1, true};
  ASSERT_TRUE(guarded->install_resident_dapp(dapp));

  auto config    = make_config();
  config.rb_mask = crb_bitmap(nof_prb);
  config.rb_mask.fill(0, nof_prb);
  resource_grid_reader_spy grid(1, MAX_NSYMB_PER_SLOT, nof_prb);
  empty_results            estimates;
  codeword_spy             codeword;
  notifier_spy             notifier;
  conventional->demodulate(codeword, notifier, grid, estimates, config);
  guarded->demodulate(codeword, notifier, grid, estimates, config);

  const auto expected = conventional->get_resident_softbits();
  const auto actual   = guarded->get_resident_softbits();
  ASSERT_TRUE(expected.valid);
  ASSERT_TRUE(actual.valid);
  ASSERT_EQ(actual.nof_softbits, expected.nof_softbits);
  ASSERT_EQ(cudaEventSynchronize(static_cast<cudaEvent_t>(expected.completion_token)), cudaSuccess);
  ASSERT_EQ(cudaEventSynchronize(static_cast<cudaEvent_t>(actual.completion_token)), cudaSuccess);
  std::vector<uint16_t> expected_llrs(expected.nof_softbits);
  std::vector<uint16_t> actual_llrs(actual.nof_softbits);
  ASSERT_EQ(
      cudaMemcpy(expected_llrs.data(), expected.data, expected_llrs.size() * sizeof(uint16_t), cudaMemcpyDeviceToHost),
      cudaSuccess);
  ASSERT_EQ(cudaMemcpy(actual_llrs.data(), actual.data, actual_llrs.size() * sizeof(uint16_t), cudaMemcpyDeviceToHost),
            cudaSuccess);
  EXPECT_EQ(actual_llrs, expected_llrs);
  EXPECT_EQ(dapp->calls, 1U);
  EXPECT_EQ(dapp->completions, 1U);
}

TEST(pusch_resident_dapp_adapter, accelerated_channel_estimator_rejoins_resident_receiver_continuation)
{
  constexpr unsigned                                  nof_prb = 25;
  pusch_demodulator_factory_accelerated_configuration factory_config;
  factory_config.equalizer_factory    = create_channel_equalizer_generic_factory();
  factory_config.precoder_factory     = create_dft_transform_precoder_factory(create_dft_processor_factory(), nof_prb);
  factory_config.demodulation_factory = create_demodulation_mapper_factory();
  factory_config.prg_factory          = create_pseudo_random_generator_sw_factory();
  factory_config.max_nof_prb          = nof_prb;
  auto factory                        = create_pusch_demodulator_factory_accelerated(factory_config);
  ASSERT_TRUE(factory);
  auto demodulator = factory->create();
  ASSERT_TRUE(demodulator);

  auto demod_target                       = std::make_shared<target_spy>();
  demod_target->mode                      = pusch_dapp_demodulator_mode::inactive;
  auto channel_target                     = std::make_shared<channel_target_spy>();
  channel_target->publish_scheduler_noise = true;
  auto dapp                               = create_pusch_resident_dapp(demod_target,
                                         channel_target,
                                         0,
                                         create_pseudo_random_generator_sw_factory()->create(),
                                         nof_prb,
                                         1,
                                         1,
                                         std::chrono::microseconds(100),
                                         1.0e-6F);
  ASSERT_TRUE(dapp);
  ASSERT_TRUE(demodulator->install_resident_dapp(dapp));
  demodulator->enable_resident_mode();

  auto config    = make_config();
  config.rb_mask = crb_bitmap(nof_prb);
  for (unsigned rb = 0; rb != nof_prb; ++rb) {
    config.rb_mask.set(rb);
  }
  resource_grid_reader_spy grid(1, MAX_NSYMB_PER_SLOT, nof_prb);
  empty_results            estimates;
  codeword_spy             codeword;
  notifier_spy             notifier;
  demodulator->demodulate(codeword, notifier, grid, estimates, config);

  ASSERT_EQ(channel_target->calls, 1U);
  const resident_softbit_buffer resident = demodulator->get_resident_softbits();
  ASSERT_TRUE(resident.valid);
  ASSERT_NE(resident.completion_token, nullptr);
  ASSERT_EQ(cudaEventSynchronize(static_cast<cudaEvent_t>(resident.completion_token)), cudaSuccess);
  EXPECT_EQ(resident.nof_softbits, 36U * nof_prb);
  EXPECT_EQ(codeword.blocks, 0U);
  EXPECT_EQ(codeword.end_calls, 0U);
}

TEST(pusch_resident_dapp_adapter, accelerated_channel_estimator_failure_matches_same_stream_conventional_fallback)
{
  constexpr unsigned nof_prb      = 25;
  auto               conventional = make_accelerated_demodulator(nof_prb);
  auto               guarded      = make_accelerated_demodulator(nof_prb);
  ASSERT_TRUE(conventional);
  ASSERT_TRUE(guarded);
  conventional->enable_resident_mode();
  guarded->enable_resident_mode();
  auto demod_target         = std::make_shared<target_spy>();
  demod_target->mode        = pusch_dapp_demodulator_mode::inactive;
  auto channel_target       = std::make_shared<channel_target_spy>();
  channel_target->fill_byte = 0x7e;
  channel_target->result    = {OCUDU_DAPP_INVALID_OUTPUT_V1, true};
  auto dapp                 = create_pusch_resident_dapp(demod_target,
                                         channel_target,
                                         0,
                                         create_pseudo_random_generator_sw_factory()->create(),
                                         nof_prb,
                                         2,
                                         2,
                                         100us,
                                         1.0e-6F);
  ASSERT_TRUE(dapp);
  ASSERT_TRUE(guarded->install_resident_dapp(dapp));

  auto config          = make_config();
  config.nof_tx_layers = 2;
  config.rx_ports.push_back(1);
  config.rb_mask = crb_bitmap(nof_prb);
  config.rb_mask.fill(0, nof_prb);
  resource_grid_reader_spy grid(2, MAX_NSYMB_PER_SLOT, nof_prb);
  test_support::populate_identity_pusch_grid(grid, config, nof_prb);
  empty_results estimates;
  codeword_spy  codeword;
  notifier_spy  notifier;
  conventional->demodulate(codeword, notifier, grid, estimates, config);
  guarded->demodulate(codeword, notifier, grid, estimates, config);

  const auto expected = conventional->get_resident_softbits();
  const auto actual   = guarded->get_resident_softbits();
  ASSERT_TRUE(expected.valid);
  ASSERT_TRUE(actual.valid);
  ASSERT_EQ(actual.nof_softbits, expected.nof_softbits);
  ASSERT_EQ(cudaEventSynchronize(static_cast<cudaEvent_t>(expected.completion_token)), cudaSuccess);
  ASSERT_EQ(cudaEventSynchronize(static_cast<cudaEvent_t>(actual.completion_token)), cudaSuccess);
  std::vector<uint16_t> expected_llrs(expected.nof_softbits);
  std::vector<uint16_t> actual_llrs(actual.nof_softbits);
  ASSERT_EQ(
      cudaMemcpy(expected_llrs.data(), expected.data, expected_llrs.size() * sizeof(uint16_t), cudaMemcpyDeviceToHost),
      cudaSuccess);
  ASSERT_EQ(cudaMemcpy(actual_llrs.data(), actual.data, actual_llrs.size() * sizeof(uint16_t), cudaMemcpyDeviceToHost),
            cudaSuccess);
  EXPECT_EQ(actual_llrs, expected_llrs);
  EXPECT_EQ(channel_target->calls, 1U);
}

#ifdef OCUDU_DAPP_REFERENCE_CUDA_DIR
uint16_t to_bfloat16(float value)
{
  uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  return static_cast<uint16_t>(bits >> 16U);
}

uint32_t pack_cbf16(cf_t value)
{
  return static_cast<uint32_t>(to_bfloat16(value.real())) | (static_cast<uint32_t>(to_bfloat16(value.imag())) << 16U);
}

class device_spectrum_grid final : public resource_grid_reader_spy
{
public:
  device_spectrum_grid() : resource_grid_reader_spy(1, 1, 2)
  {
    std::array<uint32_t, 24> samples{};
    uint32_t                 bits     = 0;
    const float              occupied = 2.0F;
    std::memcpy(&bits, &occupied, sizeof(bits));
    const uint32_t packed = (bits >> 16U) & UINT16_MAX;
    std::fill_n(samples.begin(), 12, packed);
    valid = cudaStreamCreateWithFlags(&producer, cudaStreamNonBlocking) == cudaSuccess &&
            cudaEventCreateWithFlags(&ready, cudaEventDisableTiming) == cudaSuccess &&
            cudaEventCreateWithFlags(&completion, cudaEventDisableTiming) == cudaSuccess &&
            cudaMalloc(reinterpret_cast<void**>(&device_grid), sizeof(samples)) == cudaSuccess &&
            cudaMemcpyAsync(device_grid, samples.data(), sizeof(samples), cudaMemcpyHostToDevice, producer) ==
                cudaSuccess &&
            cudaEventRecord(ready, producer) == cudaSuccess;
  }

  ~device_spectrum_grid() override
  {
    if (completion_recorded) {
      static_cast<void>(cudaEventSynchronize(completion));
    } else if (ready != nullptr) {
      static_cast<void>(cudaEventSynchronize(ready));
    }
    if (device_grid != nullptr) {
      static_cast<void>(cudaFree(device_grid));
    }
    if (completion != nullptr) {
      static_cast<void>(cudaEventDestroy(completion));
    }
    if (ready != nullptr) {
      static_cast<void>(cudaEventDestroy(ready));
    }
    if (producer != nullptr) {
      static_cast<void>(cudaStreamDestroy(producer));
    }
  }

  bool        supports_device_grid_reading() const override { return valid; }
  const void* get_device_grid_cbf16() const override { return device_grid; }
  void*       get_device_grid_ready_event() const override { return ready; }
  // Owned-snapshot contract: this fixture's buffer is a cudaMalloc allocation,
  // so it doubles as the Class-C-safe snapshot the adapters require.
  const void* get_owned_device_snapshot_cbf16() const override
  {
    ++snapshot_reads;
    return device_grid;
  }
  void* get_owned_device_snapshot_ready_event() const override { return ready; }
  bool  prepare_device_grid_reading(void* stream) const override
  {
    ++prepare_calls;
    return valid && stream != nullptr &&
           cudaStreamWaitEvent(static_cast<cudaStream_t>(stream), ready, 0) == cudaSuccess;
  }
  bool on_device_grid_reading_enqueued(void* stream) const override
  {
    ++finish_calls;
    completion_recorded =
        stream != nullptr && cudaEventRecord(completion, static_cast<cudaStream_t>(stream)) == cudaSuccess;
    return completion_recorded;
  }

  bool                valid{};
  mutable unsigned    prepare_calls{};
  mutable unsigned    snapshot_reads{};
  mutable unsigned    finish_calls{};
  mutable bool        completion_recorded{};
  uint32_t*           device_grid{};
  cudaStream_t        producer{};
  cudaEvent_t         ready{};
  mutable cudaEvent_t completion{};
};

class device_srs_grid final : public resource_grid_reader_spy
{
public:
  explicit device_srs_grid(const srs_estimator_configuration& config) : resource_grid_reader_spy(1, 14, 275)
  {
    const srs_information information = get_srs_information(config.resource, 0);
    auto                  factory     = create_low_papr_sequence_generator_sw_factory();
    auto                  generator   = factory ? factory->create() : nullptr;
    std::vector<cf_t>     sequence(information.sequence_length);
    std::vector<uint32_t> samples(get_nof_ports() * get_nof_symbols() * get_nof_subc());
    if (!generator) {
      return;
    }
    generator->generate(
        sequence, information.sequence_group, information.sequence_number, information.n_cs, information.n_cs_max);
    for (uint32_t frequency = 0; frequency != information.sequence_length; ++frequency) {
      const uint32_t subcarrier = information.mapping_initial_subcarrier + frequency * information.comb_size;
      const uint64_t index = static_cast<uint64_t>(config.resource.start_symbol.value()) * get_nof_subc() + subcarrier;
      samples[index]       = pack_cbf16(sequence[frequency]);
    }
    valid = cudaStreamCreateWithFlags(&producer, cudaStreamNonBlocking) == cudaSuccess &&
            cudaEventCreateWithFlags(&ready, cudaEventDisableTiming) == cudaSuccess &&
            cudaEventCreateWithFlags(&completion, cudaEventDisableTiming) == cudaSuccess &&
            cudaMalloc(reinterpret_cast<void**>(&device_grid), samples.size() * sizeof(samples[0])) == cudaSuccess &&
            cudaMemcpyAsync(
                device_grid, samples.data(), samples.size() * sizeof(samples[0]), cudaMemcpyHostToDevice, producer) ==
                cudaSuccess &&
            cudaEventRecord(ready, producer) == cudaSuccess;
  }

  ~device_srs_grid() override
  {
    if (completion_recorded) {
      static_cast<void>(cudaEventSynchronize(completion));
    } else if (ready != nullptr) {
      static_cast<void>(cudaEventSynchronize(ready));
    }
    if (device_grid != nullptr) {
      static_cast<void>(cudaFree(device_grid));
    }
    if (completion != nullptr) {
      static_cast<void>(cudaEventDestroy(completion));
    }
    if (ready != nullptr) {
      static_cast<void>(cudaEventDestroy(ready));
    }
    if (producer != nullptr) {
      static_cast<void>(cudaStreamDestroy(producer));
    }
  }

  bool        supports_device_grid_reading() const override { return valid; }
  const void* get_device_grid_cbf16() const override { return device_grid; }
  void*       get_device_grid_ready_event() const override { return ready; }
  // Owned-snapshot contract: this fixture's buffer is a cudaMalloc allocation,
  // so it doubles as the Class-C-safe snapshot the adapters require.
  const void* get_owned_device_snapshot_cbf16() const override
  {
    ++snapshot_reads;
    return device_grid;
  }
  void* get_owned_device_snapshot_ready_event() const override { return ready; }
  bool  prepare_device_grid_reading(void* stream) const override
  {
    ++prepare_calls;
    return valid && stream != nullptr &&
           cudaStreamWaitEvent(static_cast<cudaStream_t>(stream), ready, 0) == cudaSuccess;
  }
  bool on_device_grid_reading_enqueued(void* stream) const override
  {
    ++finish_calls;
    completion_recorded =
        stream != nullptr && cudaEventRecord(completion, static_cast<cudaStream_t>(stream)) == cudaSuccess;
    return completion_recorded;
  }

  bool                valid{};
  mutable unsigned    prepare_calls{};
  mutable unsigned    snapshot_reads{};
  mutable unsigned    finish_calls{};
  mutable bool        completion_recorded{};
  uint32_t*           device_grid{};
  cudaStream_t        producer{};
  cudaEvent_t         ready{};
  mutable cudaEvent_t completion{};
};

class fixed_srs_estimator final : public srs_estimator
{
public:
  srs_estimator_result estimate(const resource_grid_reader&, const srs_estimator_configuration&) override
  {
    srs_estimator_result result{};
    result.noise_variance = 0.25F;
    result.rsrp_dB        = -10.0F;
    result.epre_dB        = -8.0F;
    return result;
  }
};

class fixed_srs_validator final : public srs_estimator_configuration_validator
{
public:
  error_type<std::string> is_valid(const srs_estimator_configuration&) const override { return default_success_t{}; }
};

class fixed_srs_factory final : public srs_estimator_factory
{
public:
  std::unique_ptr<srs_estimator> create() override { return std::make_unique<fixed_srs_estimator>(); }
  std::unique_ptr<srs_estimator_configuration_validator> create_validator() override
  {
    return std::make_unique<fixed_srs_validator>();
  }
};

class capturing_srs_sink final : public app_services::dapp_srs_isac_report_sink
{
public:
  ocudu_dapp_status_v1 publish(const ocudu_dapp_srs_isac_cir_preview_v1& value) override
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (calls < reports.size()) {
      reports[calls] = value;
    }
    ++calls;
    return OCUDU_DAPP_OK_V1;
  }

  std::pair<unsigned, std::array<ocudu_dapp_srs_isac_cir_preview_v1, 4>> snapshot()
  {
    std::lock_guard<std::mutex> lock(mutex);
    return {calls, reports};
  }

private:
  std::mutex                                        mutex;
  unsigned                                          calls{};
  std::array<ocudu_dapp_srs_isac_cir_preview_v1, 4> reports{};
};

srs_estimator_configuration make_cuda_srs_config()
{
  srs_estimator_configuration config{};
  config.cell_id        = 0;
  config.producer_shard = 0;
  config.slot           = slot_point{0, 3};
  config.resource       = {.nof_antenna_ports   = srs_resource_configuration::one_two_four_enum::one,
                           .nof_symbols         = srs_nof_symbols::n1,
                           .start_symbol        = 13,
                           .configuration_index = 0,
                           .sequence_id         = 0,
                           .bandwidth_index     = 0,
                           .comb_size           = tx_comb_size::n2,
                           .comb_offset         = 0,
                           .cyclic_shift        = 0,
                           .freq_position       = 0,
                           .freq_shift          = 0,
                           .freq_hopping        = 0,
                           .hopping             = srs_group_or_sequence_hopping::neither,
                           .periodicity         = std::nullopt};
  config.ports          = {0};
  // Multi-UE ISAC attribution: the router drops SRS observations without a UE
  // context (rnti 0 would mix UEs into one Doppler ring).
  config.context = srs_context{0, to_rnti(0x4601)};
  return config;
}

ocudu::dapp::management::identifier transaction_id(uint64_t value)
{
  ocudu::dapp::management::identifier result;
  std::memcpy(result.bytes.data() + result.bytes.size() - sizeof(value), &value, sizeof(value));
  return result;
}

ocudu::dapp::management::operation_result control_instance(ocudu::dapp::instance_manager&            manager,
                                                           ocudu::dapp::management::identifier       instance,
                                                           uint64_t                                  transaction,
                                                           ocudu::dapp::management::lifecycle_action action,
                                                           uint64_t                                  generation,
                                                           uint64_t timeout_us = 100'000);

TEST(pusch_resident_dapp_adapter, production_service_routes_a_cuda_visible_grid_to_native_spectrum)
{
  app_services::dapp_service_config config;
  config.catalog_root                            = OCUDU_DAPP_REFERENCE_CUDA_DIR;
  config.native_preflight_helper                 = OCUDU_DAPP_PREFLIGHT_HELPER;
  config.native_preflight_timeout                = 5s;
  config.package_signatures.mode                 = dapp::package_signature_mode::allow_unsigned;
  config.package_signatures.minimum_policy_epoch = 0;
  config.package_manifests                       = {"ref_spectrum_cuda.manifest.json"};
  config.cells                                   = {{0, 1, 2}};
  config.nof_pusch_lanes                         = 1;
  config.nof_srs_lanes                           = 1;
  config.nof_scheduler_lanes                     = 1;
  config.max_scheduler_ul_prbs_avoided           = 1;
  config.backend_mask = (uint64_t{1} << OCUDU_DAPP_BACKEND_HOST_V1) | (uint64_t{1} << OCUDU_DAPP_BACKEND_CUDA_V1);
  auto service        = app_services::dapp_service::create(std::move(config));
  ASSERT_TRUE(service) << service.detail;
  auto inventory = service.service->instances().inventory();
  ASSERT_EQ(inventory.packages.size(), 1U);

  uint64_t                       transaction = 9000;
  dapp::management::load_request load;
  load.transaction_id   = transaction_id(transaction++);
  load.package_id       = inventory.packages[0].package_id;
  load.placement        = OCUDU_DAPP_PLACEMENT_IN_PROCESS_V1;
  load.selected_backend = dapp::management::backend::cuda;
  auto loaded           = service.service->instances().load(load);
  ASSERT_TRUE(loaded) << loaded.detail;
  ASSERT_TRUE(loaded.instance_id);
  uint64_t generation = loaded.instance_generation;
  for (auto action : {dapp::management::lifecycle_action::prepare,
                      dapp::management::lifecycle_action::arm,
                      dapp::management::lifecycle_action::enter_shadow,
                      dapp::management::lifecycle_action::activate}) {
    auto result =
        control_instance(service.service->instances(), *loaded.instance_id, transaction++, action, generation);
    ASSERT_TRUE(result) << result.detail;
    generation = result.instance_generation;
  }

  auto tap = service.service->upper_phy_hooks()->wrap_phy_tap({0, 0, 0}, nullptr);
  ASSERT_NE(tap, nullptr);
  device_spectrum_grid grid;
  ASSERT_TRUE(grid.valid);
  const slot_point source_slot{0, 3};
  tap->handle_quiet_grid(grid, source_slot);
  ASSERT_EQ(grid.snapshot_reads, 1U);
  // The owned-snapshot contract needs no completion fence on the live reader
  // (live_hold is false), so the router must not call the finish hook.
  ASSERT_EQ(grid.finish_calls, 0U);

  auto interference = service.service->interference_cache()->acquire(
      0,
      2,
      source_slot + 1,
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
          .count());
  ASSERT_TRUE(interference);
  ASSERT_EQ(interference.mask().dimensions[0], 2U);
  const auto* confidence = reinterpret_cast<const uint8_t*>(interference.mask().native_handle);
  EXPECT_EQ(confidence[0], UINT8_MAX);
  EXPECT_EQ(confidence[1], 0U);

  auto result = control_instance(service.service->instances(),
                                 *loaded.instance_id,
                                 transaction++,
                                 dapp::management::lifecycle_action::deactivate,
                                 generation);
  ASSERT_TRUE(result) << result.detail;
  generation = result.instance_generation;
  result     = control_instance(service.service->instances(),
                            *loaded.instance_id,
                            transaction++,
                            dapp::management::lifecycle_action::unload,
                            generation);
  ASSERT_TRUE(result) << result.detail;
  service.service->stop();
}

TEST(pusch_resident_dapp_adapter, production_service_exports_a_cuda_spectrum_snapshot_to_an_isolated_worker)
{
  app_services::dapp_service_config config;
  config.catalog_root                            = OCUDU_DAPP_REFERENCE_CUDA_DIR;
  config.native_preflight_helper                 = OCUDU_DAPP_PREFLIGHT_HELPER;
  config.native_preflight_timeout                = 5s;
  config.package_signatures.mode                 = dapp::package_signature_mode::allow_unsigned;
  config.package_signatures.minimum_policy_epoch = 0;
  config.package_manifests                       = {"ref_spectrumd_cuda.manifest.json"};
  config.cells                                   = {{0, 1, 2}};
  config.nof_pusch_lanes                         = 1;
  config.nof_srs_lanes                           = 1;
  config.nof_scheduler_lanes                     = 1;
  config.max_scheduler_ul_prbs_avoided           = 1;
  config.class_c_process.stop_timeout            = 10s;
  config.backend_mask = (uint64_t{1} << OCUDU_DAPP_BACKEND_HOST_V1) | (uint64_t{1} << OCUDU_DAPP_BACKEND_CUDA_V1);
  auto service        = app_services::dapp_service::create(std::move(config));
  ASSERT_TRUE(service) << service.detail;
  auto inventory = service.service->instances().inventory();
  ASSERT_EQ(inventory.packages.size(), 1U);

  uint64_t                       transaction = 9050;
  dapp::management::load_request load;
  load.transaction_id   = transaction_id(transaction++);
  load.package_id       = inventory.packages[0].package_id;
  load.placement        = OCUDU_DAPP_PLACEMENT_OUT_OF_PROCESS_V1;
  load.selected_backend = dapp::management::backend::cuda;
  auto loaded           = service.service->instances().load(load);
  ASSERT_TRUE(loaded) << loaded.detail;
  ASSERT_TRUE(loaded.instance_id);
  uint64_t generation = loaded.instance_generation;
  for (auto action : {dapp::management::lifecycle_action::prepare,
                      dapp::management::lifecycle_action::arm,
                      dapp::management::lifecycle_action::enter_shadow,
                      dapp::management::lifecycle_action::activate}) {
    auto result =
        control_instance(service.service->instances(), *loaded.instance_id, transaction++, action, generation);
    ASSERT_TRUE(result) << result.detail;
    generation = result.instance_generation;
  }

  auto tap = service.service->upper_phy_hooks()->wrap_phy_tap({0, 0, 0}, nullptr);
  ASSERT_NE(tap, nullptr);
  device_spectrum_grid grid;
  ASSERT_TRUE(grid.valid);
  constexpr auto process_test_timeout = 15s;
  auto           metric_value         = [&](const char* name) {
    auto metrics = service.service->instances().metrics(*loaded.instance_id);
    if (!metrics) {
      ADD_FAILURE() << metrics.detail;
      return uint64_t{0};
    }
    auto found = std::find_if(
        metrics.value.begin(), metrics.value.end(), [&](const auto& metric) { return metric.name == name; });
    EXPECT_NE(found, metrics.value.end()) << name;
    return found != metrics.value.end() ? std::get<uint64_t>(found->sample_value) : 0;
  };
  auto publish_and_wait = [&](slot_point source_slot, uint64_t expected_results) {
    // Class-C observation is a stream, not guaranteed single-shot delivery: a
    // record published while the worker is still initializing (CUDA context
    // setup takes seconds) expires before consumption and is dropped by
    // design. Keep publishing fresh slots while waiting, as the live PHY does.
    slot_point published = source_slot;
    tap->handle_quiet_grid(grid, published);
    const auto                        deadline     = std::chrono::steady_clock::now() + process_test_timeout;
    auto                              next_publish = std::chrono::steady_clock::now() + 200ms;
    scheduler_dapp_interference_lease interference;
    do {
      interference = service.service->interference_cache()->acquire(
          0,
          2,
          published + 1,
          std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
              .count());
      if (interference) {
        break;
      }
      if (std::chrono::steady_clock::now() >= next_publish) {
        published = published + 20;
        tap->handle_quiet_grid(grid, published);
        next_publish = std::chrono::steady_clock::now() + 200ms;
      }
      std::this_thread::sleep_for(1ms);
    } while (std::chrono::steady_clock::now() < deadline);
    if (!interference) {
      ADD_FAILURE() << "state=" << metric_value(OCUDU_DAPP_METRIC_PROCESS_SUPERVISOR_STATE_V1)
                    << " publications=" << metric_value(OCUDU_DAPP_METRIC_CLASS_C_ROUTE_PUBLICATIONS_V1)
                    << " invalid=" << metric_value(OCUDU_DAPP_METRIC_CLASS_C_ROUTE_INVALID_DROPS_V1)
                    << " encode=" << metric_value(OCUDU_DAPP_METRIC_CLASS_C_ROUTE_ENCODE_FAILURES_V1)
                    << " backpressure=" << metric_value(OCUDU_DAPP_METRIC_CLASS_C_ROUTE_BACKPRESSURE_DROPS_V1)
                    << " inactive=" << metric_value(OCUDU_DAPP_METRIC_CLASS_C_ROUTE_INACTIVE_BYPASSES_V1)
                    << " protocol=" << metric_value(OCUDU_DAPP_METRIC_PROCESS_RESULT_PROTOCOL_FAILURES_V1)
                    << " accepted=" << metric_value(OCUDU_DAPP_METRIC_PROCESS_RESULTS_ACCEPTED_V1)
                    << " busy=" << metric_value(OCUDU_DAPP_METRIC_PROCESS_RESULTS_BUSY_V1)
                    << " rejected=" << metric_value(OCUDU_DAPP_METRIC_PROCESS_RESULTS_REJECTED_V1)
                    << " exits=" << metric_value(OCUDU_DAPP_METRIC_PROCESS_EXITS_V1)
                    << " restarts=" << metric_value(OCUDU_DAPP_METRIC_PROCESS_SUCCESSFUL_RESTARTS_V1);
      return false;
    }
    while (std::chrono::steady_clock::now() < deadline &&
           metric_value(OCUDU_DAPP_METRIC_PROCESS_RESULTS_ACCEPTED_V1) < expected_results) {
      std::this_thread::sleep_for(1ms);
    }
    EXPECT_GE(metric_value(OCUDU_DAPP_METRIC_PROCESS_RESULTS_ACCEPTED_V1), expected_results);
    EXPECT_EQ(interference.mask().dimensions[0], 2U);
    const auto* confidence = reinterpret_cast<const uint8_t*>(interference.mask().native_handle);
    EXPECT_EQ(confidence[0], UINT8_MAX);
    EXPECT_EQ(confidence[1], 0U);
    return true;
  };

  constexpr uint64_t publications_before_restart = 6;
  for (uint64_t index = 0; index != publications_before_restart; ++index) {
    ASSERT_TRUE(publish_and_wait(slot_point{1, 3 + 2000 * static_cast<unsigned>(index)}, index + 1));
  }
  // The hardened staging path may take an additional owned snapshot around a
  // worker respawn; the invariant is at-least-one-read-per-publication, not
  // exactly-one (duplicates are benign, missed publications are not).
  ASSERT_GE(grid.snapshot_reads, publications_before_restart);
  ASSERT_EQ(grid.finish_calls, 0U);

  const uint64_t original_pid = metric_value(OCUDU_DAPP_METRIC_PROCESS_PID_V1);
  ASSERT_GT(original_pid, 0U);
  ASSERT_EQ(::kill(static_cast<pid_t>(original_pid), SIGKILL), 0);
  const auto restart_deadline = std::chrono::steady_clock::now() + process_test_timeout;
  while (std::chrono::steady_clock::now() < restart_deadline &&
         (metric_value(OCUDU_DAPP_METRIC_PROCESS_SUCCESSFUL_RESTARTS_V1) == 0 ||
          metric_value(OCUDU_DAPP_METRIC_PROCESS_PID_V1) == original_pid ||
          metric_value(OCUDU_DAPP_METRIC_PROCESS_SUPERVISOR_STATE_V1) !=
              static_cast<uint64_t>(dapp::class_c_process_state::running))) {
    std::this_thread::sleep_for(10ms);
  }
  ASSERT_EQ(metric_value(OCUDU_DAPP_METRIC_PROCESS_SUCCESSFUL_RESTARTS_V1), 1U);
  ASSERT_NE(metric_value(OCUDU_DAPP_METRIC_PROCESS_PID_V1), original_pid);
  ASSERT_EQ(metric_value(OCUDU_DAPP_METRIC_PROCESS_SUPERVISOR_STATE_V1),
            static_cast<uint64_t>(dapp::class_c_process_state::running));
  ASSERT_TRUE(publish_and_wait(slot_point{1, 15000}, publications_before_restart + 1));

  constexpr uint64_t total_publications = publications_before_restart + 1;
  EXPECT_GE(grid.snapshot_reads, total_publications);
  EXPECT_EQ(grid.finish_calls, 0U);
  EXPECT_GE(metric_value(OCUDU_DAPP_METRIC_CLASS_C_COPY_EXPORT_COUNT_V1), total_publications);
  EXPECT_GE(metric_value(OCUDU_DAPP_METRIC_CLASS_C_COPY_EXPORT_BYTES_V1),
            total_publications * static_cast<uint64_t>(grid.get_nof_ports()) * grid.get_nof_symbols() *
                grid.get_nof_subc() * sizeof(cbf16_t));
  EXPECT_EQ(metric_value(OCUDU_DAPP_METRIC_HOST_INPUT_CELL_BITMAP_V1), 1U);
  EXPECT_EQ(metric_value(OCUDU_DAPP_METRIC_CLASS_C_COPY_SNAPSHOT_COUNT_V1), 0U);

  auto current = service.service->instances().instance(*loaded.instance_id);
  ASSERT_TRUE(current);
  generation = current->generation;

  auto result = control_instance(service.service->instances(),
                                 *loaded.instance_id,
                                 transaction++,
                                 dapp::management::lifecycle_action::deactivate,
                                 generation,
                                 15'000'000);
  ASSERT_TRUE(result) << result.detail;
  generation = result.instance_generation;
  result     = control_instance(service.service->instances(),
                            *loaded.instance_id,
                            transaction++,
                            dapp::management::lifecycle_action::unload,
                            generation);
  ASSERT_TRUE(result) << result.detail;
  service.service->stop();
}

TEST(pusch_resident_dapp_adapter, production_service_routes_cuda_srs_estimates_to_native_isac)
{
  auto                              sink = std::make_shared<capturing_srs_sink>();
  app_services::dapp_service_config config;
  config.catalog_root                            = OCUDU_DAPP_REFERENCE_CUDA_DIR;
  config.native_preflight_helper                 = OCUDU_DAPP_PREFLIGHT_HELPER;
  config.native_preflight_timeout                = 5s;
  config.package_signatures.mode                 = dapp::package_signature_mode::allow_unsigned;
  config.package_signatures.minimum_policy_epoch = 0;
  config.package_manifests                       = {"ref_srs_isac_cuda.manifest.json"};
  config.cells                                   = {{0, 1, 275}};
  config.nof_pusch_lanes                         = 1;
  config.nof_srs_lanes                           = 2;
  config.nof_scheduler_lanes                     = 1;
  config.max_scheduler_ul_prbs_avoided           = 1;
  config.backend_mask = (uint64_t{1} << OCUDU_DAPP_BACKEND_HOST_V1) | (uint64_t{1} << OCUDU_DAPP_BACKEND_CUDA_V1);
  config.srs_isac_report_sink = sink;
  auto service                = app_services::dapp_service::create(std::move(config));
  ASSERT_TRUE(service) << service.detail;
  auto inventory = service.service->instances().inventory();
  ASSERT_EQ(inventory.packages.size(), 1U);

  uint64_t                       transaction = 9100;
  dapp::management::load_request load;
  load.transaction_id   = transaction_id(transaction++);
  load.package_id       = inventory.packages[0].package_id;
  load.placement        = OCUDU_DAPP_PLACEMENT_IN_PROCESS_V1;
  load.selected_backend = dapp::management::backend::cuda;
  auto loaded           = service.service->instances().load(load);
  ASSERT_TRUE(loaded) << loaded.detail;
  ASSERT_TRUE(loaded.instance_id);
  uint64_t generation = loaded.instance_generation;
  for (auto action : {dapp::management::lifecycle_action::prepare,
                      dapp::management::lifecycle_action::arm,
                      dapp::management::lifecycle_action::enter_shadow,
                      dapp::management::lifecycle_action::activate}) {
    auto result =
        control_instance(service.service->instances(), *loaded.instance_id, transaction++, action, generation);
    ASSERT_TRUE(result) << result.detail;
    generation = result.instance_generation;
  }

  auto decorated =
      service.service->upper_phy_hooks()->decorate_srs_estimator(std::make_shared<fixed_srs_factory>(), 2, 52);
  ASSERT_NE(decorated, nullptr);
  auto estimator0 = decorated->create();
  auto estimator1 = decorated->create();
  ASSERT_NE(estimator0, nullptr);
  ASSERT_NE(estimator1, nullptr);
  auto            srs_config = make_cuda_srs_config();
  device_srs_grid grid0(srs_config);
  device_srs_grid grid1(srs_config);
  ASSERT_TRUE(grid0.valid);
  ASSERT_TRUE(grid1.valid);
  std::thread thread0([&] { static_cast<void>(estimator0->estimate(grid0, srs_config)); });
  std::thread thread1([&] { static_cast<void>(estimator1->estimate(grid1, srs_config)); });
  thread0.join();
  thread1.join();

  // Owned-snapshot contract: each publication acquires the snapshot once and
  // never takes a live hold on the reader.
  ASSERT_EQ(grid0.snapshot_reads, 1U);
  ASSERT_EQ(grid0.finish_calls, 0U);
  ASSERT_EQ(grid1.snapshot_reads, 1U);
  ASSERT_EQ(grid1.finish_calls, 0U);
  auto [calls, reports] = sink->snapshot();
  ASSERT_EQ(calls, 2U);
  if (reports[0].context.source_sequence > reports[1].context.source_sequence) {
    std::swap(reports[0], reports[1]);
  }
  const uint32_t expected_taps =
      std::min(get_srs_information(srs_config.resource, 0).sequence_length, OCUDU_DAPP_SRS_ISAC_MAX_CIR_TAPS_V1);
  for (uint32_t index = 0; index != calls; ++index) {
    const auto& report = reports[index];
    EXPECT_EQ(report.context.source_sequence, index + 1);
    EXPECT_EQ(report.source_srs_sequence, 1U);
    EXPECT_EQ(report.srs_config_id, 0U);
    EXPECT_EQ(report.tap_count, expected_taps);
    EXPECT_EQ(report.peak_delay_bin, 0U);
    EXPECT_GT(report.mean_channel_power, 0.9F);
    EXPECT_GT(report.tap_power[0], 0.9F);
  }

  auto result = control_instance(service.service->instances(),
                                 *loaded.instance_id,
                                 transaction++,
                                 dapp::management::lifecycle_action::deactivate,
                                 generation);
  ASSERT_TRUE(result) << result.detail;
  generation = result.instance_generation;
  result     = control_instance(service.service->instances(),
                            *loaded.instance_id,
                            transaction++,
                            dapp::management::lifecycle_action::unload,
                            generation);
  ASSERT_TRUE(result) << result.detail;
  service.service->stop();
}

TEST(pusch_resident_dapp_adapter, production_service_exports_cuda_srs_estimates_to_an_isolated_worker)
{
  auto                              sink = std::make_shared<capturing_srs_sink>();
  app_services::dapp_service_config config;
  config.catalog_root                            = OCUDU_DAPP_REFERENCE_CUDA_DIR;
  config.native_preflight_helper                 = OCUDU_DAPP_PREFLIGHT_HELPER;
  config.native_preflight_timeout                = 5s;
  config.package_signatures.mode                 = dapp::package_signature_mode::allow_unsigned;
  config.package_signatures.minimum_policy_epoch = 0;
  config.package_manifests                       = {"ref_srs_isacd_cuda.manifest.json"};
  config.cells                                   = {{0, 1, 275}};
  config.nof_pusch_lanes                         = 1;
  config.nof_srs_lanes                           = 1;
  config.nof_scheduler_lanes                     = 1;
  config.max_scheduler_ul_prbs_avoided           = 1;
  config.class_c_process.stop_timeout            = 10s;
  config.backend_mask = (uint64_t{1} << OCUDU_DAPP_BACKEND_HOST_V1) | (uint64_t{1} << OCUDU_DAPP_BACKEND_CUDA_V1);
  config.srs_isac_report_sink = sink;
  auto service                = app_services::dapp_service::create(std::move(config));
  ASSERT_TRUE(service) << service.detail;
  auto inventory = service.service->instances().inventory();
  ASSERT_EQ(inventory.packages.size(), 1U);

  uint64_t                       transaction = 9150;
  dapp::management::load_request load;
  load.transaction_id   = transaction_id(transaction++);
  load.package_id       = inventory.packages[0].package_id;
  load.placement        = OCUDU_DAPP_PLACEMENT_OUT_OF_PROCESS_V1;
  load.selected_backend = dapp::management::backend::cuda;
  auto loaded           = service.service->instances().load(load);
  ASSERT_TRUE(loaded) << loaded.detail;
  ASSERT_TRUE(loaded.instance_id);
  uint64_t generation = loaded.instance_generation;
  for (auto action : {dapp::management::lifecycle_action::prepare,
                      dapp::management::lifecycle_action::arm,
                      dapp::management::lifecycle_action::enter_shadow,
                      dapp::management::lifecycle_action::activate}) {
    auto result =
        control_instance(service.service->instances(), *loaded.instance_id, transaction++, action, generation);
    ASSERT_TRUE(result) << result.detail;
    generation = result.instance_generation;
  }

  auto metric_value = [&](const char* name) {
    auto metrics = service.service->instances().metrics(*loaded.instance_id);
    if (!metrics) {
      ADD_FAILURE() << metrics.detail;
      return uint64_t{0};
    }
    auto found = std::find_if(
        metrics.value.begin(), metrics.value.end(), [&](const auto& metric) { return metric.name == name; });
    EXPECT_NE(found, metrics.value.end()) << name;
    return found != metrics.value.end() ? std::get<uint64_t>(found->sample_value) : 0;
  };
  constexpr auto process_test_timeout = 15s;

  auto decorated =
      service.service->upper_phy_hooks()->decorate_srs_estimator(std::make_shared<fixed_srs_factory>(), 1, 52);
  ASSERT_NE(decorated, nullptr);
  auto estimator = decorated->create();
  ASSERT_NE(estimator, nullptr);
  auto            srs_config = make_cuda_srs_config();
  device_srs_grid grid(srs_config);
  ASSERT_TRUE(grid.valid);
  // Streaming contract: records published while the worker is still bringing
  // up its CUDA context expire and are dropped by design, so keep publishing
  // fresh observations while waiting (the router deduplicates repeats of the
  // same slot, so each retry advances the slot).
  auto wait_for_reports = [&](unsigned expected) {
    const auto deadline     = std::chrono::steady_clock::now() + process_test_timeout;
    auto       next_publish = std::chrono::steady_clock::now() + 200ms;
    while (std::chrono::steady_clock::now() < deadline && sink->snapshot().first < expected) {
      if (std::chrono::steady_clock::now() >= next_publish) {
        srs_config.slot = srs_config.slot + 20;
        static_cast<void>(estimator->estimate(grid, srs_config));
        next_publish = std::chrono::steady_clock::now() + 200ms;
      }
      std::this_thread::sleep_for(1ms);
    }
    EXPECT_GE(sink->snapshot().first, expected);
  };
  static_cast<void>(estimator->estimate(grid, srs_config));
  wait_for_reports(1);

  const uint64_t original_pid = metric_value(OCUDU_DAPP_METRIC_PROCESS_PID_V1);
  ASSERT_GT(original_pid, 0U);
  ASSERT_EQ(::kill(static_cast<pid_t>(original_pid), SIGKILL), 0);
  const auto restart_deadline = std::chrono::steady_clock::now() + process_test_timeout;
  while (std::chrono::steady_clock::now() < restart_deadline &&
         (metric_value(OCUDU_DAPP_METRIC_PROCESS_SUCCESSFUL_RESTARTS_V1) == 0 ||
          metric_value(OCUDU_DAPP_METRIC_PROCESS_PID_V1) == original_pid ||
          metric_value(OCUDU_DAPP_METRIC_PROCESS_SUPERVISOR_STATE_V1) !=
              static_cast<uint64_t>(dapp::class_c_process_state::running))) {
    std::this_thread::sleep_for(10ms);
  }
  ASSERT_EQ(metric_value(OCUDU_DAPP_METRIC_PROCESS_SUCCESSFUL_RESTARTS_V1), 1U);
  ASSERT_NE(metric_value(OCUDU_DAPP_METRIC_PROCESS_PID_V1), original_pid);
  ASSERT_EQ(metric_value(OCUDU_DAPP_METRIC_PROCESS_SUPERVISOR_STATE_V1),
            static_cast<uint64_t>(dapp::class_c_process_state::running));

  srs_config.slot = srs_config.slot + 40;
  static_cast<void>(estimator->estimate(grid, srs_config));
  const uint64_t reports_before_restart = sink->snapshot().first;
  wait_for_reports(static_cast<unsigned>(reports_before_restart) + 1);
  // The fixed estimator double produces the estimate on the host, so the
  // adapter stages it into the export pool itself and never needs to read the
  // device grid; the export-pool copy metrics below are the evidence that the
  // CUDA export path ran.
  EXPECT_EQ(grid.prepare_calls, 0U);
  const uint64_t estimate_bytes =
      static_cast<uint64_t>(get_srs_information(srs_config.resource, 0).sequence_length) * sizeof(cf_t);
  EXPECT_GE(metric_value(OCUDU_DAPP_METRIC_CLASS_C_COPY_EXPORT_COUNT_V1), 2U);
  EXPECT_GE(metric_value(OCUDU_DAPP_METRIC_CLASS_C_COPY_EXPORT_BYTES_V1), 2U * estimate_bytes);
  EXPECT_EQ(metric_value(OCUDU_DAPP_METRIC_HOST_INPUT_CELL_BITMAP_V1), 1U);
  EXPECT_EQ(metric_value(OCUDU_DAPP_METRIC_CLASS_C_EXPORT_SLOTS_CAPACITY_V1), 4U);
  // Streaming retries reclaim export slots lazily on the next publish, so a
  // couple of consumed-but-unreclaimed slots may still be counted in use.
  EXPECT_LE(metric_value(OCUDU_DAPP_METRIC_CLASS_C_EXPORT_SLOTS_IN_USE_V1), 4U);
  EXPECT_GE(metric_value(OCUDU_DAPP_METRIC_CLASS_C_EXPORT_SLOTS_HIGH_WATERMARK_V1), 1U);
  EXPECT_EQ(metric_value(OCUDU_DAPP_METRIC_CLASS_C_EXPORT_SLOT_RESERVATION_FAILURES_V1), 0U);
  EXPECT_EQ(metric_value(OCUDU_DAPP_METRIC_CLASS_C_COPY_SNAPSHOT_COUNT_V1), 0U);
  // Restart hardening can accept a benign duplicate result around respawn;
  // fewer than the published count would mean a lost result.
  EXPECT_GE(metric_value(OCUDU_DAPP_METRIC_PROCESS_RESULTS_ACCEPTED_V1), 2U);
  const auto [calls, reports] = sink->snapshot();
  ASSERT_GE(calls, 2U); // >=: respawn may deliver one extra benign report
  for (uint32_t index = 0; index != calls; ++index) {
    // Streaming retries make sequence numbers sparse: the route sequence and
    // the SRS estimate sequence advance independently, so require both to be
    // nonzero and strictly increasing rather than dense and equal.
    EXPECT_GT(reports[index].context.source_sequence, 0U);
    EXPECT_GT(reports[index].source_srs_sequence, 0U);
    if (index != 0) {
      EXPECT_GT(reports[index].context.source_sequence, reports[index - 1].context.source_sequence);
      EXPECT_GT(reports[index].source_srs_sequence, reports[index - 1].source_srs_sequence);
    }
    EXPECT_EQ(reports[index].srs_config_id, 0U);
    EXPECT_GT(reports[index].mean_channel_power, 0.9F);
    EXPECT_GT(reports[index].tap_power[0], 0.9F);
  }

  auto current = service.service->instances().instance(*loaded.instance_id);
  ASSERT_TRUE(current);
  generation  = current->generation;
  auto result = control_instance(service.service->instances(),
                                 *loaded.instance_id,
                                 transaction++,
                                 dapp::management::lifecycle_action::deactivate,
                                 generation,
                                 15'000'000);
  ASSERT_TRUE(result) << result.detail;
  generation = result.instance_generation;
  result     = control_instance(service.service->instances(),
                            *loaded.instance_id,
                            transaction++,
                            dapp::management::lifecycle_action::unload,
                            generation);
  ASSERT_TRUE(result) << result.detail;
  service.service->stop();
}

ocudu::dapp::management::operation_result control_instance(ocudu::dapp::instance_manager&            manager,
                                                           ocudu::dapp::management::identifier       instance,
                                                           uint64_t                                  transaction,
                                                           ocudu::dapp::management::lifecycle_action action,
                                                           uint64_t                                  generation,
                                                           uint64_t                                  timeout_us)
{
  ocudu::dapp::management::lifecycle_request request;
  request.transaction_id      = transaction_id(transaction);
  request.instance_id         = instance;
  request.action              = action;
  request.expected_generation = generation;
  request.timeout_us          = timeout_us;
  return manager.control(request);
}

void run_packaged_cuda_dapp(const char*       artifact_name,
                            uint32_t          interface_id,
                            modulation_scheme modulation        = modulation_scheme::QPSK,
                            unsigned          nof_layers        = 1,
                            bool              multi_symbol_dmrs = false)
{
  const std::filesystem::path catalog  = OCUDU_DAPP_REFERENCE_CUDA_DIR;
  const std::filesystem::path artifact = catalog / artifact_name;
  dapp::artifact_verifier     verifier(catalog);
  auto                        inspected = verifier.inspect(artifact);
  ASSERT_TRUE(inspected) << inspected.detail;
  auto approved = verifier.verify(artifact, inspected.digest);
  ASSERT_TRUE(approved) << approved.detail;

  ocudu_dapp_host_info_v1 host{};
  host.struct_size  = sizeof(host);
  host.abi_major    = OCUDU_DAPP_ABI_MAJOR_V1;
  host.abi_minor    = OCUDU_DAPP_ABI_MINOR_V1;
  host.backend_mask = uint64_t{1} << OCUDU_DAPP_BACKEND_CUDA_V1;
  auto loaded       = dapp::load_native_module(std::move(*approved.artifact), host);
  ASSERT_TRUE(loaded) << loaded.detail;
  ocudu_dapp_host_api_v1 host_api{};
  host_api.struct_size = sizeof(host_api);
  host_api.abi_major   = OCUDU_DAPP_ABI_MAJOR_V1;
  host_api.abi_minor   = OCUDU_DAPP_ABI_MINOR_V1;
  auto created         = dapp::native_instance::create(loaded.library, host_api, nullptr, OCUDU_DAPP_BACKEND_CUDA_V1);
  ASSERT_TRUE(created) << created.operation.detail;
  ASSERT_TRUE(created.instance->prepare(nullptr, nullptr));
  ASSERT_TRUE(created.instance->arm(nullptr));
  ASSERT_TRUE(created.instance->enter_shadow());
  ASSERT_TRUE(created.instance->activate());

  auto point_result = dapp::native_hook_point::create(interface_id, 41, 1);
  ASSERT_TRUE(point_result);
  auto point = std::shared_ptr<dapp::native_hook_point>(std::move(point_result.point));
  std::shared_ptr<dapp::native_hook_point>             passive_receiver;
  std::shared_ptr<pusch_dapp_channel_estimator_target> channel_target;
  std::shared_ptr<pusch_dapp_demodulator_target>       demodulator_target;
  if (interface_id == OCUDU_DAPP_INTERFACE_CHANNEL_ESTIMATOR_V1) {
    auto passive_result = dapp::native_hook_point::create(OCUDU_DAPP_INTERFACE_RECEIVER_V1, 141, 1);
    ASSERT_TRUE(passive_result);
    passive_receiver   = std::shared_ptr<dapp::native_hook_point>(std::move(passive_result.point));
    channel_target     = create_native_pusch_dapp_channel_estimator_target(point);
    demodulator_target = create_native_pusch_dapp_demodulator_target({}, passive_receiver);
  } else if (interface_id == OCUDU_DAPP_INTERFACE_EQUALIZER_V1) {
    demodulator_target = create_native_pusch_dapp_demodulator_target(point, {});
  } else {
    demodulator_target = create_native_pusch_dapp_demodulator_target({}, point);
  }
  ASSERT_TRUE(demodulator_target);

  constexpr unsigned                                  nof_prb = 25;
  pusch_demodulator_factory_accelerated_configuration factory_config;
  factory_config.equalizer_factory    = create_channel_equalizer_generic_factory();
  factory_config.precoder_factory     = create_dft_transform_precoder_factory(create_dft_processor_factory(), nof_prb);
  factory_config.demodulation_factory = create_demodulation_mapper_factory();
  factory_config.prg_factory          = create_pseudo_random_generator_sw_factory();
  factory_config.max_nof_prb          = nof_prb;
  auto factory                        = create_pusch_demodulator_factory_accelerated(factory_config);
  ASSERT_TRUE(factory);
  auto demodulator = factory->create();
  ASSERT_TRUE(demodulator);
  auto resident = create_pusch_resident_dapp(demodulator_target,
                                             channel_target,
                                             0,
                                             create_pseudo_random_generator_sw_factory()->create(),
                                             nof_prb,
                                             nof_layers,
                                             nof_layers,
                                             150us,
                                             1.0e-6F);
  ASSERT_TRUE(resident);
  ASSERT_TRUE(demodulator->install_resident_dapp(resident));
  demodulator->enable_resident_mode();
  ASSERT_EQ(point->activate(*created.instance), OCUDU_DAPP_OK_V1);

  auto config          = make_config();
  config.modulation    = modulation;
  config.nof_tx_layers = nof_layers;
  if (multi_symbol_dmrs) {
    config.nof_symbols = 4;
    config.dmrs_symb_pos.set(5);
  }
  for (unsigned port = 1; port != nof_layers; ++port) {
    config.rx_ports.push_back(port);
  }
  config.rb_mask = crb_bitmap(nof_prb);
  for (unsigned rb = 0; rb != nof_prb; ++rb) {
    config.rb_mask.set(rb);
  }
  resource_grid_reader_spy grid(nof_layers, MAX_NSYMB_PER_SLOT, nof_prb);
  test_support::populate_identity_pusch_grid(grid, config, nof_prb);
  empty_results estimates;
  codeword_spy  codeword;
  notifier_spy  notifier;
  demodulator->demodulate(codeword, notifier, grid, estimates, config);
  const resident_softbit_buffer output = demodulator->get_resident_softbits();
  ASSERT_TRUE(output.valid);
  ASSERT_EQ(cudaEventSynchronize(static_cast<cudaEvent_t>(output.completion_token)), cudaSuccess);
  std::array<uint16_t, 32> llrs{};
  ASSERT_GE(output.nof_softbits, llrs.size());
  ASSERT_EQ(cudaMemcpy(llrs.data(), output.data, sizeof(llrs), cudaMemcpyDeviceToHost), cudaSuccess);
  EXPECT_TRUE(std::all_of(
      llrs.begin(), llrs.end(), [](uint16_t value) { return (value & 0x7fffU) != 0 && (value & 0x7c00U) != 0x7c00U; }));

  std::array<ocudu_dapp_metric_sample_v1, 2> metrics{};
  const auto metric_result = created.instance->read_metrics(metrics.data(), metrics.size());
  ASSERT_TRUE(metric_result) << metric_result.operation.detail;
  ASSERT_EQ(metric_result.count, 2U);
  EXPECT_STREQ(metrics[0].name, OCUDU_DAPP_METRIC_INVOKE_COUNT_V1);
  EXPECT_EQ(metrics[0].value.value.uint64_value, 1U);
  EXPECT_STREQ(metrics[1].name, OCUDU_DAPP_METRIC_INPUT_MAX_LAYERS_V1);
  EXPECT_EQ(metrics[1].value.value.uint64_value, nof_layers);

  demodulator.reset();
  EXPECT_EQ(point->deactivate(std::chrono::steady_clock::now() + 100ms), OCUDU_DAPP_OK_V1);
  EXPECT_TRUE(created.instance->drain(std::chrono::steady_clock::now() + 100ms));
  EXPECT_TRUE(created.instance->unload());
  std::weak_ptr<pusch_resident_dapp> resident_lifetime = resident;
  resident.reset();
  EXPECT_TRUE(resident_lifetime.expired());
}

TEST(pusch_resident_dapp_adapter, packaged_channel_estimator_runs_through_the_accelerated_hook)
{
  run_packaged_cuda_dapp(
      "ref_channel_estimator_cuda.so", OCUDU_DAPP_INTERFACE_CHANNEL_ESTIMATOR_V1, modulation_scheme::QPSK, 1);
  run_packaged_cuda_dapp(
      "ref_channel_estimator_cuda.so", OCUDU_DAPP_INTERFACE_CHANNEL_ESTIMATOR_V1, modulation_scheme::QPSK, 2);
}

TEST(pusch_resident_dapp_adapter, packaged_equalizer_runs_through_the_accelerated_hook)
{
  run_packaged_cuda_dapp("ref_equalizer_cuda.so", OCUDU_DAPP_INTERFACE_EQUALIZER_V1);
}

TEST(pusch_resident_dapp_adapter, packaged_receiver_runs_all_qam_orders_through_the_accelerated_hook)
{
  for (modulation_scheme modulation :
       {modulation_scheme::QPSK, modulation_scheme::QAM16, modulation_scheme::QAM64, modulation_scheme::QAM256}) {
    run_packaged_cuda_dapp("ref_receiver_cuda.so", OCUDU_DAPP_INTERFACE_RECEIVER_V1, modulation);
  }
}

TEST(pusch_resident_dapp_adapter, packaged_equalizer_and_receiver_run_two_layers_resident)
{
  run_packaged_cuda_dapp("ref_equalizer_cuda.so", OCUDU_DAPP_INTERFACE_EQUALIZER_V1, modulation_scheme::QPSK, 2);
  run_packaged_cuda_dapp("ref_receiver_cuda.so", OCUDU_DAPP_INTERFACE_RECEIVER_V1, modulation_scheme::QPSK, 2);
}

TEST(pusch_resident_dapp_adapter, packaged_class_a_depths_run_multi_symbol_two_layer_dmrs)
{
  run_packaged_cuda_dapp(
      "ref_channel_estimator_cuda.so", OCUDU_DAPP_INTERFACE_CHANNEL_ESTIMATOR_V1, modulation_scheme::QPSK, 2, true);
  run_packaged_cuda_dapp("ref_equalizer_cuda.so", OCUDU_DAPP_INTERFACE_EQUALIZER_V1, modulation_scheme::QPSK, 2, true);
  run_packaged_cuda_dapp("ref_receiver_cuda.so", OCUDU_DAPP_INTERFACE_RECEIVER_V1, modulation_scheme::QPSK, 2, true);
}

void run_production_service_cuda_packages(const std::filesystem::path& catalog,
                                          std::vector<std::string>     manifests,
                                          const std::string&           expected_name_prefix)
{
  app_services::dapp_service_config service_config;
  service_config.catalog_root                            = catalog;
  service_config.native_preflight_helper                 = OCUDU_DAPP_PREFLIGHT_HELPER;
  service_config.native_preflight_timeout                = 5s;
  service_config.package_manifests                       = std::move(manifests);
  service_config.package_signatures.mode                 = dapp::package_signature_mode::allow_unsigned;
  service_config.package_signatures.minimum_policy_epoch = 0;
  service_config.cells                                   = {{0, 1, 25}};
  service_config.nof_pusch_lanes                         = 3;
  service_config.nof_srs_lanes                           = 1;
  service_config.nof_scheduler_lanes                     = 1;
  service_config.max_scheduler_ul_prbs_avoided           = 1;
  service_config.backend_mask =
      (uint64_t{1} << OCUDU_DAPP_BACKEND_HOST_V1) | (uint64_t{1} << OCUDU_DAPP_BACKEND_CUDA_V1);
  auto service = app_services::dapp_service::create(std::move(service_config));
  ASSERT_TRUE(service) << service.detail;

  constexpr unsigned                                  nof_prb = 25;
  pusch_demodulator_factory_accelerated_configuration factory_config;
  factory_config.equalizer_factory    = create_channel_equalizer_generic_factory();
  factory_config.precoder_factory     = create_dft_transform_precoder_factory(create_dft_processor_factory(), nof_prb);
  factory_config.demodulation_factory = create_demodulation_mapper_factory();
  factory_config.prg_factory          = create_pseudo_random_generator_sw_factory();
  factory_config.max_nof_prb          = nof_prb;
  auto fallback                       = create_pusch_demodulator_factory_accelerated(factory_config);
  ASSERT_TRUE(fallback);
  auto factory = service.service->upper_phy_hooks()->decorate_pusch_demodulator(fallback,
                                                                                {.nof_lanes                  = 3,
                                                                                 .max_nof_prb                = nof_prb,
                                                                                 .max_nof_rx_ports           = 2,
                                                                                 .max_nof_layers             = 2,
                                                                                 .channel_estimator_deadline = 100ms,
                                                                                 .demodulator_deadline       = 100ms});
  ASSERT_TRUE(factory);
  auto inventory = service.service->instances().inventory();
  ASSERT_EQ(inventory.packages.size(), 3U);
  uint64_t                               transaction = 1000;
  const std::array<modulation_scheme, 4> modulations = {
      modulation_scheme::QPSK, modulation_scheme::QAM16, modulation_scheme::QAM64, modulation_scheme::QAM256};
  constexpr std::array<unsigned, 2>                   layer_counts = {1, 2};
  std::array<std::optional<std::vector<uint16_t>>, 8> reference_llrs;
  std::array<std::string, reference_llrs.size()>      reference_names;
  for (const auto& package : inventory.packages) {
    auto demodulator = factory->create();
    ASSERT_TRUE(demodulator);
    demodulator->enable_resident_mode();
    EXPECT_EQ(package.global_name.rfind(expected_name_prefix, 0), 0U);
    dapp::management::load_request load;
    load.transaction_id   = transaction_id(transaction++);
    load.package_id       = package.package_id;
    load.placement        = OCUDU_DAPP_PLACEMENT_IN_PROCESS_V1;
    load.selected_backend = dapp::management::backend::cuda;
    auto loaded           = service.service->instances().load(load);
    ASSERT_TRUE(loaded) << package.global_name << ": " << loaded.detail;
    ASSERT_TRUE(loaded.instance_id);

    uint64_t generation = loaded.instance_generation;
    for (auto action : {dapp::management::lifecycle_action::prepare,
                        dapp::management::lifecycle_action::arm,
                        dapp::management::lifecycle_action::enter_shadow,
                        dapp::management::lifecycle_action::activate}) {
      auto result =
          control_instance(service.service->instances(), *loaded.instance_id, transaction++, action, generation);
      ASSERT_TRUE(result) << package.global_name << ": " << result.detail;
      generation = result.instance_generation;
    }

    // Layer-major order deliberately exercises a one-port 256-QAM to two-port QPSK
    // transition on the same accelerated lane. The unified-buffer RE-index offset
    // changes even though the radio allocation and DM-RS geometry do not.
    for (size_t layer_index = 0; layer_index != layer_counts.size(); ++layer_index) {
      for (size_t modulation_index = 0; modulation_index != modulations.size(); ++modulation_index) {
        const unsigned nof_layers      = layer_counts[layer_index];
        const size_t   reference_index = layer_index * modulations.size() + modulation_index;
        auto           config          = make_config();
        config.modulation              = modulations[modulation_index];
        config.nof_tx_layers           = nof_layers;
        if (nof_layers == 2) {
          config.rx_ports.push_back(1);
        }
        config.rb_mask = crb_bitmap(nof_prb);
        config.rb_mask.fill(0, nof_prb);
        resource_grid_reader_spy grid(nof_layers, MAX_NSYMB_PER_SLOT, nof_prb);
        test_support::populate_identity_pusch_grid(grid, config, nof_prb);
        empty_results estimates;
        codeword_spy  codeword;
        notifier_spy  notifier;
        demodulator->demodulate(codeword, notifier, grid, estimates, config);
        const resident_softbit_buffer output = demodulator->get_resident_softbits();
        ASSERT_TRUE(output.valid) << package.global_name;
        ASSERT_EQ(cudaEventSynchronize(static_cast<cudaEvent_t>(output.completion_token)), cudaSuccess);
        std::vector<uint16_t> llrs(output.nof_softbits);
        ASSERT_FALSE(llrs.empty());
        ASSERT_EQ(cudaMemcpy(llrs.data(), output.data, llrs.size() * sizeof(llrs.front()), cudaMemcpyDeviceToHost),
                  cudaSuccess);
        EXPECT_TRUE(std::all_of(llrs.begin(), llrs.end(), [](uint16_t value) { return (value & 0x7c00U) != 0x7c00U; }))
            << package.global_name << " layers " << nof_layers << " modulation "
            << static_cast<unsigned>(config.modulation);
        EXPECT_TRUE(std::any_of(llrs.begin(), llrs.end(), [](uint16_t value) { return (value & 0x7fffU) != 0; }))
            << package.global_name << " layers " << nof_layers << " modulation "
            << static_cast<unsigned>(config.modulation);
        if (reference_llrs[reference_index].has_value()) {
          ASSERT_EQ(llrs.size(), reference_llrs[reference_index]->size()) << package.global_name;
          size_t mismatches = 0;
          size_t first      = llrs.size();
          for (size_t index = 0; index != llrs.size(); ++index) {
            if ((llrs[index] >> 15U) != ((*reference_llrs[reference_index])[index] >> 15U)) {
              first = std::min(first, index);
              ++mismatches;
            }
          }
          EXPECT_EQ(mismatches, 0U) << reference_names[reference_index] << " versus " << package.global_name
                                    << " layers " << nof_layers << " modulation "
                                    << static_cast<unsigned>(config.modulation) << " first mismatch " << first;
        } else {
          reference_llrs[reference_index]  = std::move(llrs);
          reference_names[reference_index] = package.global_name;
        }
      }
    }

    auto metrics = service.service->instances().metrics(*loaded.instance_id);
    ASSERT_TRUE(metrics) << metrics.detail;
    auto invoked = std::find_if(metrics.value.begin(), metrics.value.end(), [](const auto& metric) {
      return metric.name == OCUDU_DAPP_METRIC_INVOKE_COUNT_V1;
    });
    ASSERT_NE(invoked, metrics.value.end());
    EXPECT_EQ(std::get<uint64_t>(invoked->sample_value), layer_counts.size() * modulations.size())
        << package.global_name;
    auto maximum_layers = std::find_if(metrics.value.begin(), metrics.value.end(), [](const auto& metric) {
      return metric.name == OCUDU_DAPP_METRIC_INPUT_MAX_LAYERS_V1;
    });
    ASSERT_NE(maximum_layers, metrics.value.end());
    EXPECT_EQ(std::get<uint64_t>(maximum_layers->sample_value), layer_counts.back()) << package.global_name;

    const auto current_inventory = service.service->instances().inventory();
    const auto current_instance =
        std::find_if(current_inventory.instances.begin(), current_inventory.instances.end(), [&](const auto& instance) {
          return instance.instance_id == *loaded.instance_id;
        });
    ASSERT_NE(current_instance, current_inventory.instances.end());
    generation = current_instance->generation;

    auto result = control_instance(service.service->instances(),
                                   *loaded.instance_id,
                                   transaction++,
                                   dapp::management::lifecycle_action::deactivate,
                                   generation);
    ASSERT_TRUE(result) << result.detail;
    generation = result.instance_generation;
    result     = control_instance(service.service->instances(),
                              *loaded.instance_id,
                              transaction++,
                              dapp::management::lifecycle_action::unload,
                              generation);
    ASSERT_TRUE(result) << result.detail;
    demodulator.reset();
  }
  service.service->stop();
}

TEST(pusch_resident_dapp_adapter, production_service_activates_all_packaged_class_a_cuda_depths)
{
  run_production_service_cuda_packages(OCUDU_DAPP_REFERENCE_CUDA_DIR,
                                       {"ref_channel_estimator_cuda.manifest.json",
                                        "ref_equalizer_cuda.manifest.json",
                                        "ref_receiver_cuda.manifest.json"},
                                       "org.ocudu.reference.");
}

#ifdef OCUDU_DAPP_SCAFFOLD_CUDA_DIR
TEST(pusch_resident_dapp_adapter, production_service_activates_fresh_external_class_a_scaffolds)
{
  run_production_service_cuda_packages(OCUDU_DAPP_SCAFFOLD_CUDA_DIR,
                                       {"ce/build/scaffold_ce_cuda.manifest.json",
                                        "equalizer/build/scaffold_equalizer_cuda.manifest.json",
                                        "receiver/build/scaffold_receiver_cuda.manifest.json"},
                                       "com.deepsig.scaffold.");
}
#endif

TEST(pusch_resident_dapp_adapter, all_packaged_class_a_depths_run_concurrently_on_two_pusch_lanes)
{
  app_services::dapp_service_config service_config;
  service_config.catalog_root                            = OCUDU_DAPP_REFERENCE_CUDA_DIR;
  service_config.native_preflight_helper                 = OCUDU_DAPP_PREFLIGHT_HELPER;
  service_config.native_preflight_timeout                = 5s;
  service_config.package_manifests                       = {"ref_channel_estimator_cuda.manifest.json",
                                                            "ref_equalizer_cuda.manifest.json",
                                                            "ref_receiver_cuda.manifest.json"};
  service_config.package_signatures.mode                 = dapp::package_signature_mode::allow_unsigned;
  service_config.package_signatures.minimum_policy_epoch = 0;
  service_config.cells                                   = {{0, 1, 25}};
  service_config.nof_pusch_lanes                         = 2;
  service_config.nof_srs_lanes                           = 1;
  service_config.nof_scheduler_lanes                     = 1;
  service_config.backend_mask                            = (uint64_t{1} << OCUDU_DAPP_BACKEND_CUDA_V1);
  auto service = app_services::dapp_service::create(std::move(service_config));
  ASSERT_TRUE(service) << service.detail;

  constexpr unsigned                                  nof_prb = 25;
  pusch_demodulator_factory_accelerated_configuration factory_config;
  factory_config.equalizer_factory    = create_channel_equalizer_generic_factory();
  factory_config.precoder_factory     = create_dft_transform_precoder_factory(create_dft_processor_factory(), nof_prb);
  factory_config.demodulation_factory = create_demodulation_mapper_factory();
  factory_config.prg_factory          = create_pseudo_random_generator_sw_factory();
  factory_config.max_nof_prb          = nof_prb;
  auto fallback                       = create_pusch_demodulator_factory_accelerated(factory_config);
  ASSERT_TRUE(fallback);
  auto factory = service.service->upper_phy_hooks()->decorate_pusch_demodulator(
      fallback, {.nof_lanes = 2, .max_nof_prb = nof_prb, .max_nof_rx_ports = 2, .max_nof_layers = 2});
  ASSERT_TRUE(factory);
  std::array<std::unique_ptr<pusch_demodulator>, 2> demodulators = {factory->create(), factory->create()};
  for (auto& demodulator : demodulators) {
    ASSERT_TRUE(demodulator);
    demodulator->enable_resident_mode();
  }

  auto     inventory   = service.service->instances().inventory();
  uint64_t transaction = 2000;
  ASSERT_EQ(inventory.packages.size(), 3U);
  for (const auto& package : inventory.packages) {
    const unsigned                 nof_layers = 2;
    dapp::management::load_request load;
    load.transaction_id   = transaction_id(transaction++);
    load.package_id       = package.package_id;
    load.placement        = OCUDU_DAPP_PLACEMENT_IN_PROCESS_V1;
    load.selected_backend = dapp::management::backend::cuda;
    auto loaded           = service.service->instances().load(load);
    ASSERT_TRUE(loaded) << package.global_name << ": " << loaded.detail;
    ASSERT_TRUE(loaded.instance_id);
    uint64_t generation = loaded.instance_generation;
    for (auto action : {dapp::management::lifecycle_action::prepare,
                        dapp::management::lifecycle_action::arm,
                        dapp::management::lifecycle_action::enter_shadow,
                        dapp::management::lifecycle_action::activate}) {
      auto result =
          control_instance(service.service->instances(), *loaded.instance_id, transaction++, action, generation);
      ASSERT_TRUE(result) << package.global_name << ": " << result.detail;
      generation = result.instance_generation;
    }

    std::array<std::thread, 2> workers;
    for (unsigned lane = 0; lane != workers.size(); ++lane) {
      workers[lane] = std::thread([&, lane] {
        auto config          = make_config();
        config.nof_tx_layers = nof_layers;
        if (nof_layers == 2) {
          config.rx_ports.push_back(1);
        }
        config.rb_mask = crb_bitmap(nof_prb);
        config.rb_mask.fill(0, nof_prb);
        resource_grid_reader_spy grid(nof_layers, MAX_NSYMB_PER_SLOT, nof_prb);
        test_support::populate_identity_pusch_grid(grid, config, nof_prb);
        empty_results estimates;
        codeword_spy  codeword;
        notifier_spy  notifier;
        demodulators[lane]->demodulate(codeword, notifier, grid, estimates, config);
      });
    }
    for (auto& worker : workers) {
      worker.join();
    }
    for (auto& demodulator : demodulators) {
      const auto output = demodulator->get_resident_softbits();
      ASSERT_TRUE(output.valid) << package.global_name;
      ASSERT_EQ(cudaEventSynchronize(static_cast<cudaEvent_t>(output.completion_token)), cudaSuccess);
    }

    auto metrics = service.service->instances().metrics(*loaded.instance_id);
    ASSERT_TRUE(metrics) << metrics.detail;
    auto invoked = std::find_if(metrics.value.begin(), metrics.value.end(), [](const auto& metric) {
      return metric.name == OCUDU_DAPP_METRIC_INVOKE_COUNT_V1;
    });
    ASSERT_NE(invoked, metrics.value.end());
    EXPECT_EQ(std::get<uint64_t>(invoked->sample_value), 2U) << package.global_name;
    auto maximum_layers = std::find_if(metrics.value.begin(), metrics.value.end(), [](const auto& metric) {
      return metric.name == OCUDU_DAPP_METRIC_INPUT_MAX_LAYERS_V1;
    });
    ASSERT_NE(maximum_layers, metrics.value.end());
    EXPECT_EQ(std::get<uint64_t>(maximum_layers->sample_value), nof_layers) << package.global_name;

    auto current_inventory = service.service->instances().inventory();
    auto current_instance =
        std::find_if(current_inventory.instances.begin(), current_inventory.instances.end(), [&](const auto& instance) {
          return instance.instance_id == *loaded.instance_id;
        });
    ASSERT_NE(current_instance, current_inventory.instances.end());
    generation  = current_instance->generation;
    auto result = control_instance(service.service->instances(),
                                   *loaded.instance_id,
                                   transaction++,
                                   dapp::management::lifecycle_action::deactivate,
                                   generation);
    ASSERT_TRUE(result) << result.detail;
    generation = result.instance_generation;
    result     = control_instance(service.service->instances(),
                              *loaded.instance_id,
                              transaction++,
                              dapp::management::lifecycle_action::unload,
                              generation);
    ASSERT_TRUE(result) << result.detail;
  }
  for (auto& demodulator : demodulators) {
    demodulator.reset();
  }
  service.service->stop();
}
#endif

} // namespace
