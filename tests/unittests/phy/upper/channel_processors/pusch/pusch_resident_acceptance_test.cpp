// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

// Links the actual ENABLE_CUDA decoder control flow to a CPU-only resident backend.
// No CUDA runtime, driver, context, stream or kernel is used by this test.
#include "../../rx_buffer_test_doubles.h"
#include "crc_calculator_generic_impl.h"
#include "ldpc_segmenter_rx_impl.h"
#include "phy/metrics/phy_metrics_pusch_processor_decorator.h"
#include "phy/upper/metrics/aggregators/pusch_processor_metrics_aggregator.h"
#include "phy_to_fapi_results_event_fastpath_translator.h"
#include "pusch_decoder_impl.h"
#include "pusch_processor_notifier_adaptor.h"
#include "ocudu/phy/upper/channel_coding/ldpc/ldpc.h"
#include "ocudu/phy/upper/channel_processors/pusch/pusch_decoder_notifier.h"
#include "ocudu/phy/upper/channel_processors/pusch/pusch_decoder_result.h"
#include "ocudu/ran/sch/sch_segmentation.h"
#include <gtest/gtest.h>

using namespace ocudu;

namespace {
struct trace {
  bool                                   fenced               = false;
  bool                                   accepted             = true;
  bool                                   crc_ok               = true;
  unsigned                               validations          = 0;
  unsigned                               exports              = 0;
  unsigned                               decodes              = 0;
  unsigned                               notifications        = 0;
  unsigned                               completion_callbacks = 0;
  std::array<unsigned, MAX_NOF_SEGMENTS> cb_lengths{};
};

class resident_fake final : public pusch_resident_codeblock_decoder
{
public:
  explicit resident_fake(trace& state_) : state(state_) {}
  std::vector<codeblock_result> decode_batch(span<const described_rx_codeblock>,
                                             span<bit_buffer>,
                                             span<span<log_likelihood_ratio>>,
                                             bool,
                                             crc_generator_poly,
                                             bool,
                                             unsigned) override
  {
    ADD_FAILURE() << "Unexpected host decode";
    return {};
  }
  void rate_dematch_only(span<const log_likelihood_ratio>,
                         span<log_likelihood_ratio>,
                         bool,
                         const codeblock_metadata&) override
  {
    ADD_FAILURE() << "Unexpected host dematching";
  }
  bool export_resident_softbits(span<span<log_likelihood_ratio>> buffers) override
  {
    EXPECT_TRUE(state.fenced);
    EXPECT_EQ(state.completion_callbacks, 1U);
    ++state.exports;
    for (auto buffer : buffers) {
      std::fill(buffer.begin(), buffer.end(), log_likelihood_ratio(12));
    }
    return true;
  }
  tb_decode_result decode_resident_softbits(void*,
                                            size_t,
                                            const scrambling_config*,
                                            const codeblock_metadata::tb_common_metadata&,
                                            span<const codeblock_metadata::cb_specific_metadata> metadata,
                                            span<uint8_t>                                        output,
                                            crc_generator_poly,
                                            unsigned,
                                            void*,
                                            int) override
  {
    EXPECT_EQ(state.validations, 0U);
    EXPECT_EQ(state.notifications, 0U);
    ++state.decodes;
    for (unsigned i = 0; i != metadata.size(); ++i) {
      state.cb_lengths[i] = metadata[i].full_length;
    }
    std::fill(output.begin(), output.end(), 0xa5);
    state.fenced = true;
    return {state.crc_ok, state.crc_ok, 3};
  }
  const pusch_resident_codeblock_decoder_timing_stats& get_last_timing_stats() const override { return timing; }
  void                                                 set_demod_timing(float, float) override {}
  void                                                 set_timing_enabled(bool) override {}
  void                                                 set_ldpc_decoder_algorithm(const std::string&) override {}

private:
  trace&                                        state;
  pusch_resident_codeblock_decoder_timing_stats timing;
};

class buffer_spy final : public rx_buffer_spy
{
public:
  explicit buffer_spy(unsigned cbs) : rx_buffer_spy(25344, cbs) {}
  void reset_codeblocks_crc() override
  {
    auto values = get_codeblocks_crc();
    std::fill(values.begin(), values.end(), false);
  }
};

class decoded_sink final : public pusch_decoder_notifier
{
public:
  explicit decoded_sink(trace& state_) : state(state_) {}
  void on_sch_data(const pusch_decoder_result& result) override
  {
    EXPECT_TRUE(state.fenced);
    ++state.notifications;
    last = result;
  }
  pusch_decoder_result last;

private:
  trace& state;
};

bool accept(void* context, const resident_softbit_buffer& input) noexcept
{
  auto& state = *static_cast<trace*>(context);
  EXPECT_TRUE(state.fenced);
  EXPECT_EQ(state.exports, 0U);
  EXPECT_EQ(state.notifications, 0U);
  EXPECT_EQ(input.buffer_index, 17);
  EXPECT_EQ(input.rnti, 0x4817);
  ++state.validations;
  return state.accepted;
}

struct case_config {
  modulation_scheme qm;
  unsigned          bytes;
  bool              accepted;
  bool              crc_ok;
};

class resident_acceptance : public ::testing::TestWithParam<case_config>
{};

TEST_P(resident_acceptance, gates_real_decoder_harq_crc_payload_and_notification)
{
  const auto cfg = GetParam();
  trace      state;
  state.accepted = cfg.accepted;
  state.crc_ok   = cfg.crc_ok;
  decoded_sink                 sink(state);
  const unsigned               cbs = compute_nof_codeblocks(units::bits(cfg.bytes * 8), ldpc_base_graph_type::BG2);
  buffer_spy                   buffer(cbs);
  std::vector<uint8_t>         payload(cfg.bytes, 0x77);
  pusch_decoder_impl           decoder(std::make_unique<ldpc_segmenter_rx_impl>(),
                             nullptr,
                             std::make_unique<resident_fake>(state),
                                       {std::make_unique<crc_calculator_generic_impl>(crc_generator_poly::CRC16),
                                        std::make_unique<crc_calculator_generic_impl>(crc_generator_poly::CRC24A),
                                        std::make_unique<crc_calculator_generic_impl>(crc_generator_poly::CRC24B)},
                             nullptr,
                             51,
                             1);
  pusch_decoder::configuration settings;
  settings.base_graph = ldpc_base_graph_type::BG2;
  settings.mod        = cfg.qm;
  settings.nof_layers = 1;
  decoder.new_data(payload, unique_rx_buffer(buffer), sink, settings);
  decoder.set_nof_softbits(units::bits(18432));
  decoder.enable_resident_decode();
  for (unsigned i = 0; i != cbs; ++i) {
    auto bits = buffer.get_codeblock_soft_bits(i, 25344);
    std::fill(bits.begin(), bits.end(), log_likelihood_ratio(7));
  }
  decoder.set_pre_join_callback([&](bool accepted) {
    EXPECT_EQ(accepted, cfg.accepted);
    EXPECT_EQ(state.validations, 1U);
    EXPECT_EQ(state.exports, 0U);
    EXPECT_EQ(state.notifications, 0U);
    ++state.completion_callbacks;
  });
  resident_softbit_buffer input;
  input.data                = &state; // Opaque fake: the backend never dereferences a device address.
  input.execution_context   = &state;
  input.nof_softbits        = 18432;
  input.valid               = true;
  input.buffer_index        = 17;
  input.rnti                = 0x4817;
  input.acceptance_required = true;
  input.completion_context  = &state;
  input.accept_completion   = accept;
  ASSERT_TRUE(decoder.try_decode_resident(input));
  EXPECT_EQ(state.validations, 1U);
  EXPECT_EQ(state.completion_callbacks, 1U);
  EXPECT_EQ(state.notifications, 1U);
  EXPECT_EQ(state.exports, cfg.accepted && !cfg.crc_ok ? 1U : 0U);
  EXPECT_EQ(sink.last.input_valid, cfg.accepted);
  EXPECT_EQ(sink.last.tb_crc_ok, cfg.accepted && cfg.crc_ok);
  EXPECT_EQ(sink.last.nof_codeblocks_total, cbs);
  if (!cfg.accepted) {
    EXPECT_TRUE(std::all_of(payload.begin(), payload.end(), [](auto v) { return v == 0; }));
    for (unsigned i = 0; i != cbs; ++i) {
      EXPECT_FALSE(buffer.get_codeblocks_crc()[i]);
      // Every CB's actual mother-code footprint is erased, not the unused capacity tail.
      ASSERT_GT(state.cb_lengths[i], 0U);
      auto values = buffer.get_codeblock_soft_bits(i, state.cb_lengths[i]);
      EXPECT_TRUE(std::all_of(values.begin(), values.end(), [](auto v) { return v == log_likelihood_ratio(0); }));
      if (state.cb_lengths[i] < 25344) {
        EXPECT_EQ(buffer.get_codeblock_soft_bits(i, 25344)[state.cb_lengths[i]], log_likelihood_ratio(7));
      }
    }
  } else {
    EXPECT_TRUE(std::all_of(payload.begin(), payload.end(), [](auto v) { return v == 0xa5; }));
  }
}

std::vector<case_config> cases()
{
  std::vector<case_config> result;
  for (auto qm :
       {modulation_scheme::QPSK, modulation_scheme::QAM16, modulation_scheme::QAM64, modulation_scheme::QAM256}) {
    for (unsigned bytes : {100U, 1500U}) {
      for (bool accepted : {false, true}) {
        for (bool crc_ok : {false, true}) {
          result.push_back({qm, bytes, accepted, crc_ok});
        }
      }
    }
  }
  return result;
}
INSTANTIATE_TEST_SUITE_P(all_qam_single_and_multi_cb, resident_acceptance, ::testing::ValuesIn(cases()));

std::unique_ptr<pusch_decoder_impl> make_discard_decoder(trace& state)
{
  return std::make_unique<pusch_decoder_impl>(
      std::make_unique<ldpc_segmenter_rx_impl>(),
      nullptr,
      std::make_unique<resident_fake>(state),
      pusch_decoder_impl::sch_crc{std::make_unique<crc_calculator_generic_impl>(crc_generator_poly::CRC16),
                                  std::make_unique<crc_calculator_generic_impl>(crc_generator_poly::CRC24A),
                                  std::make_unique<crc_calculator_generic_impl>(crc_generator_poly::CRC24B)},
      nullptr,
      51,
      1);
}

TEST(resident_predecode_discard, all_qam_codeblocks_preserve_prior_harq_without_decode)
{
  for (auto qm :
       {modulation_scheme::QPSK, modulation_scheme::QAM16, modulation_scheme::QAM64, modulation_scheme::QAM256}) {
    for (unsigned bytes : {100U, 1500U}) {
      for (bool segmented : {false, true}) {
        trace state;
        // Producer fencing is a caller prerequisite. The decoder must not invoke its resident backend here.
        state.fenced = true;
        decoded_sink                 sink(state);
        buffer_spy                   buffer(compute_nof_codeblocks(units::bits(bytes * 8), ldpc_base_graph_type::BG2));
        std::vector<uint8_t>         payload(bytes, 0x77);
        auto                         decoder = make_discard_decoder(state);
        pusch_decoder::configuration settings;
        settings.base_graph = ldpc_base_graph_type::BG2;
        settings.mod        = qm;
        settings.nof_layers = 1;
        settings.new_data   = false;
        for (unsigned cb = 0; cb != buffer.get_nof_codeblocks(); ++cb) {
          auto bits = buffer.get_codeblock_soft_bits(cb, 25344);
          std::fill(bits.begin(), bits.end(), log_likelihood_ratio(7));
          buffer.get_codeblocks_crc()[cb] = true;
        }
        decoder->new_data(payload, unique_rx_buffer(buffer), sink, settings);
        if (segmented) {
          decoder->set_nof_softbits(units::bits(18432));
        }
        decoder->set_pre_join_callback([](bool) { ADD_FAILURE() << "No post-decode callback before decode"; });
        ASSERT_TRUE(decoder->discard_unsubmitted_input());
        EXPECT_FALSE(decoder->discard_unsubmitted_input());
        EXPECT_EQ(state.notifications, 1U);
        EXPECT_EQ(state.decodes, 0U);
        EXPECT_EQ(state.exports, 0U);
        EXPECT_EQ(state.validations, 0U);
        EXPECT_FALSE(sink.last.input_valid);
        EXPECT_FALSE(sink.last.tb_crc_ok);
        EXPECT_EQ(sink.last.nof_codeblocks_total, 0U);
        EXPECT_TRUE(std::all_of(payload.begin(), payload.end(), [](auto v) { return v == 0; }));
        for (unsigned cb = 0; cb != buffer.get_nof_codeblocks(); ++cb) {
          EXPECT_TRUE(buffer.get_codeblocks_crc()[cb]);
          auto bits = buffer.get_codeblock_soft_bits(cb, 25344);
          EXPECT_TRUE(std::all_of(bits.begin(), bits.end(), [](auto v) { return v == log_likelihood_ratio(7); }));
        }
        // The same decoder accepts the next grant without a stuck collecting/decoding state.
        decoder->new_data(payload, unique_rx_buffer(buffer), sink, settings);
        ASSERT_TRUE(decoder->discard_unsubmitted_input());
        EXPECT_EQ(state.notifications, 2U);
      }
    }
  }
}

TEST(resident_predecode_discard, refuses_partial_host_input_without_mutation)
{
  trace                        state;
  decoded_sink                 sink(state);
  buffer_spy                   buffer(1);
  std::vector<uint8_t>         payload(100, 0x77);
  auto                         decoder = make_discard_decoder(state);
  pusch_decoder::configuration settings;
  settings.base_graph                       = ldpc_base_graph_type::BG2;
  settings.mod                              = modulation_scheme::QPSK;
  settings.nof_layers                       = 1;
  auto&                               input = decoder->new_data(payload, unique_rx_buffer(buffer), sink, settings);
  std::array<log_likelihood_ratio, 1> llrs{log_likelihood_ratio(1)};
  input.on_new_softbits(llrs);
  EXPECT_FALSE(decoder->discard_unsubmitted_input());
  EXPECT_EQ(state.notifications, 0U);
  EXPECT_EQ(state.decodes, 0U);
  EXPECT_TRUE(std::all_of(payload.begin(), payload.end(), [](auto v) { return v == 0x77; }));
}

TEST(resident_predecode_discard, refuses_idle_decoder)
{
  trace state;
  auto  decoder = make_discard_decoder(state);
  EXPECT_FALSE(decoder->discard_unsubmitted_input());
  EXPECT_EQ(state.notifications, 0U);
}

TEST(resident_acceptance_contract, missing_or_ambiguous_required_check_rejects)
{
  resident_softbit_buffer input;
  EXPECT_TRUE(input.accept_completed_input());
  input.acceptance_required = true;
  EXPECT_FALSE(input.accept_completed_input());
  trace state;
  input.completion_context = &state;
  EXPECT_FALSE(input.accept_completed_input());
  input.accept_completion  = accept;
  input.completion_context = nullptr;
  EXPECT_FALSE(input.accept_completed_input());
  input.acceptance_required = false;
  EXPECT_FALSE(input.accept_completed_input());
  input.accept_completion  = nullptr;
  input.completion_context = &state;
  EXPECT_FALSE(input.accept_completed_input());
  input.completion_context = nullptr;
  input.completion_generation = 1;
  EXPECT_FALSE(input.accept_completed_input());
}

TEST(resident_acceptance_contract, callback_storage_is_bounded_and_consumed_on_move)
{
  std::array<unsigned, 24> capture{};
  capture[23]                                       = 42;
  unsigned                                    value = 0;
  pusch_decoder::resident_completion_callback callback(
      [capture, &value](bool accepted) { value = accepted ? capture[23] : 0; });
  EXPECT_TRUE(callback.is_in_small_buffer());
  auto moved = std::move(callback);
  EXPECT_TRUE(callback.is_empty());
  EXPECT_TRUE(moved.is_in_small_buffer());
  moved(true);
  EXPECT_EQ(value, 42U);
}

class processor_sink final : public pusch_processor_result_notifier, public pusch_processor_csi_part1_feedback
{
public:
  void on_sch(const pusch_processor_result_data& result) override { data = result; }
  void on_uci(const pusch_processor_result_control& result) override
  {
    control = result;
    ++uci_calls;
  }
  void                           on_csi_part1(const uci_payload_type&) override { ++feedback_calls; }
  pusch_processor_result_data    data;
  pusch_processor_result_control control;
  unsigned                       uci_calls = 0, feedback_calls = 0;
};

TEST(resident_acceptance_notifier, rejection_discards_csi_and_settles_uci_without_candidate_payload)
{
  processor_sink                   sink;
  pusch_processor_notifier_adaptor adaptor;
  channel_state_information        csi;
  csi.set_sinr_dB(channel_state_information::sinr_type::post_equalization, 33);
  adaptor.new_transmission(sink, sink, csi);
  adaptor.get_harq_ack_notifier();
  adaptor.get_csi_part1_notifier();
  adaptor.discard_resident_outputs(2, 4);
  EXPECT_EQ(sink.uci_calls, 1U);
  EXPECT_EQ(sink.feedback_calls, 0U);
  EXPECT_EQ(sink.control.harq_ack.status, uci_status::unknown);
  EXPECT_EQ(sink.control.harq_ack.payload.size(), 2U);
  EXPECT_EQ(sink.control.csi_part1.status, uci_status::unknown);
  EXPECT_EQ(sink.control.csi_part1.payload.size(), 4U);
  EXPECT_FALSE(sink.control.csi.get_sinr_dB());
  pusch_decoder_result invalid;
  invalid.input_valid = false;
  invalid.tb_crc_ok   = true; // Defensive rejection even if a producer contradicts the validity bit.
  adaptor.get_sch_data_notifier().on_sch_data(invalid);
  EXPECT_FALSE(sink.data.data.tb_crc_ok);
  EXPECT_FALSE(sink.data.csi.get_sinr_dB());
  adaptor.discard_resident_outputs(2, 4);
  EXPECT_EQ(sink.uci_calls, 1U);
}

class fapi_sink final : public fapi::p7_indications_notifier
{
public:
  void     on_crc_indication(const fapi::crc_indication&) override { ++crc; }
  void     on_rx_data_indication(const fapi::rx_data_indication&) override { ++data; }
  void     on_uci_indication(const fapi::uci_indication&) override {}
  void     on_srs_indication(const fapi::srs_indication&) override {}
  void     on_rach_indication(const fapi::rach_indication&) override {}
  unsigned crc = 0, data = 0;
};

TEST(resident_acceptance_fapi, software_discard_is_not_a_radio_crc_or_payload_indication)
{
  fapi_sink                                                   sink;
  fapi_adaptor::phy_to_fapi_results_event_fastpath_translator translator({0, 0, 0},
                                                                         {ocudulog::fetch_basic_logger("TEST")});
  translator.set_p7_indications_notifier(sink);
  auto result = ul_pusch_results_data::create_discarded(to_rnti(0x4817), slot_point(1, 0), to_harq_id(0));
  result.decoder_result.input_valid = false;
  result.decoder_result.tb_crc_ok   = true;
  translator.on_new_pusch_results_data(result);
  EXPECT_EQ(sink.crc, 0U);
  EXPECT_EQ(sink.data, 0U);
  result.decoder_result.input_valid = true;
  result.decoder_result.tb_crc_ok   = false;
  translator.on_new_pusch_results_data(result);
  EXPECT_EQ(sink.crc, 1U);
  EXPECT_EQ(sink.data, 1U);
}

TEST(resident_acceptance_metrics, software_cost_is_counted_without_polluting_radio_bler_or_sinr)
{
  pusch_processor_metrics_aggregator aggregator;
  auto&                              notifier = static_cast<pusch_processor_metric_notifier&>(aggregator);
  pusch_processor_metrics            metric{};
  metric.slot           = slot_point(1, 0);
  metric.tbs            = units::bytes(100);
  metric.elapsed_data   = std::chrono::microseconds(100);
  metric.elapsed_return = std::chrono::microseconds(10);
  metric.crc_ok         = true;
  metric.sinr_dB        = 20;
  metric.evm            = .1F;
  notifier.on_new_metric(metric);
  metric.input_valid = false;
  metric.crc_ok      = false;
  metric.sinr_dB     = 99;
  notifier.on_new_metric(metric);
  EXPECT_EQ(aggregator.get_decoding_bler(), 0);
  EXPECT_EQ(aggregator.get_nof_software_discards(), 1U);
  EXPECT_EQ(aggregator.get_total_time(), std::chrono::microseconds(200));
  EXPECT_EQ(aggregator.get_ch_est_sinr(), 20);
  metric.input_valid = true;
  notifier.on_new_metric(metric);
  EXPECT_DOUBLE_EQ(aggregator.get_decoding_bler(), .5);
  aggregator.reset();
  EXPECT_EQ(aggregator.get_nof_software_discards(), 0U);
  EXPECT_EQ(aggregator.get_decoding_bler(), 0);
}
} // namespace
