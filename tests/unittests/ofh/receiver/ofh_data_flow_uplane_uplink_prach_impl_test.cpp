// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "../../../../lib/ofh/receiver/ofh_data_flow_uplane_uplink_prach_impl.h"
#include "../ofh_uplane_rx_symbol_notifier_test_doubles.h"
#include "helpers.h"
#include "ocudu/ocudulog/ocudulog.h"
#include "ocudu/ofh/serdes/ofh_message_decoder_properties.h"
#include "ocudu/ofh/serdes/ofh_uplane_message_decoder.h"
#include <gtest/gtest.h>
#include <tuple>

using namespace ocudu;
using namespace ofh;
using namespace ofh::testing;

namespace {
class uplane_message_decoder_spy : public uplane_message_decoder
{
  uplane_message_decoder_results spy_results;

public:
  bool decode(uplane_message_decoder_results& results, span<const uint8_t> message) override
  {
    ++host_decode_count;
    results = spy_results;

    return true;
  }

  bool supports_prach_buffer_decompression() const override { return direct_decompression_enabled; }

  bool decode_with_compressed_iq(uplane_message_decoder_results& results, span<const uint8_t> message) override
  {
    ++compressed_decode_count;
    results = spy_results;
    return compressed_decode_succeeds;
  }

  void set_results(const uplane_message_decoder_results& results) { spy_results = results; }

  bool     direct_decompression_enabled = false;
  bool     compressed_decode_succeeds   = true;
  unsigned host_decode_count            = 0;
  unsigned compressed_decode_count      = 0;
};

} // namespace

class data_flow_uplane_uplink_prach_impl_fixture
  : public ::testing::TestWithParam<std::tuple<prach_format_type, bool, bool>>
{
protected:
  const static_vector<unsigned, MAX_NOF_SUPPORTED_EAXC> ul_eaxc = {5, 1, 2, 3};
  std::tuple<prach_format_type, bool, bool>             params  = GetParam();
  slot_point                                            slot;
  unsigned                                              symbol                    = 0;
  unsigned                                              sector                    = 0;
  unsigned                                              eaxc                      = 5;
  bool                                                  is_cplane_enabled         = std::get<1>(params);
  bool                                                  ignore_prach_start_symbol = std::get<2>(params);
  prach_format_type                                     format                    = std::get<0>(params);

  std::unique_ptr<prach_buffer_pool>                prach_pool;
  shared_prach_buffer                               buffer;
  prach_buffer_context                              buffer_context;
  unsigned                                          preamble_length;
  unsigned                                          nof_symbols;
  message_decoder_results                           results;
  std::shared_ptr<prach_context_repository>         repo = std::make_shared<prach_context_repository>(1);
  uplane_rx_symbol_notifier_spy*                    notifier;
  std::shared_ptr<uplink_cplane_context_repository> ul_cplane_context_repo_ptr =
      std::make_shared<uplink_cplane_context_repository>(1);
  std::shared_ptr<prach_context_repository> prach_context_repo = std::make_shared<prach_context_repository>(1);
  uplane_message_decoder_spy*               uplane_decoder;
  data_flow_uplane_uplink_prach_impl        data_flow;

public:
  data_flow_uplane_uplink_prach_impl_fixture() :
    slot(0, 0, 1),
    prach_pool(create_prach_buffer_pool(format)),
    preamble_length(is_long_preamble(format) ? 839 : 139),
    nof_symbols(get_preamble_duration(format) == 0 ? 1U : get_preamble_duration(format)),
    data_flow(get_config(), get_dependencies())
  {
    buffer_context.slot             = slot;
    buffer_context.format           = format;
    buffer_context.ports            = {0};
    buffer_context.nof_td_occasions = 1;
    buffer_context.nof_fd_occasions = 1;
    buffer_context.pusch_scs        = ocudu::subcarrier_spacing::kHz30;
    buffer_context.start_symbol     = 0;

    buffer = prach_pool->get();

    repo->add(buffer_context, buffer.clone(), ocudulog::fetch_basic_logger("TEST"), std::nullopt);
    repo->process_pending_contexts();

    results.uplane_results.params.slot      = slot;
    results.uplane_results.params.symbol_id = 0;
    results.eaxc                            = 4;
    auto& section                           = results.uplane_results.sections.emplace_back();
    section.iq_samples.resize(MAX_NOF_SUBCARRIERS);

    ul_cplane_context context;
    context.filter_index = ocudu::ofh::filter_index_type::ul_prach_preamble_1p25khz;
    context.start_symbol = 0;
    context.prb_start    = 0;
    context.nof_prb      = 273;
    context.nof_symbols  = nof_symbols;

    // Fill the contexts
    ul_cplane_context_repo_ptr->add(slot, eaxc, context);
    prach_context_repo->add(buffer_context, buffer.clone(), ocudulog::fetch_basic_logger("TEST"), std::nullopt);
    prach_context_repo->process_pending_contexts();
  }

  data_flow_uplane_uplink_prach_impl_config get_config()
  {
    data_flow_uplane_uplink_prach_impl_config config{};
    config.prach_eaxcs               = ul_eaxc;
    config.sector                    = sector;
    config.is_prach_cplane_enabled   = is_cplane_enabled;
    config.ignore_prach_start_symbol = ignore_prach_start_symbol;
    config.are_metrics_enabled       = false;

    return config;
  }

  data_flow_uplane_uplink_prach_impl_dependencies get_dependencies()
  {
    data_flow_uplane_uplink_prach_impl_dependencies dependencies;

    dependencies.logger                    = &ocudulog::fetch_basic_logger("TEST");
    dependencies.prach_cplane_context_repo = ul_cplane_context_repo_ptr;
    dependencies.prach_context_repo        = prach_context_repo;

    {
      auto temp             = std::make_shared<uplane_rx_symbol_notifier_spy>();
      notifier              = temp.get();
      dependencies.notifier = std::move(temp);
    }
    {
      auto temp                   = std::make_unique<uplane_message_decoder_spy>();
      uplane_decoder              = temp.get();
      dependencies.uplane_decoder = std::move(temp);
    }

    return dependencies;
  }

  uplane_message_decoder_results build_valid_decoder_results()
  {
    uplane_message_decoder_results deco_results;
    deco_results.params.slot          = slot;
    deco_results.params.direction     = data_direction::uplink;
    deco_results.params.filter_index  = filter_index_type::ul_prach_preamble_1p25khz;
    deco_results.params.symbol_id     = 0;
    auto& section                     = deco_results.sections.emplace_back();
    section.start_prb                 = 0;
    section.nof_prbs                  = 273;
    section.use_current_symbol_number = true;
    section.is_every_rb_used          = true;
    section.iq_samples.resize(MAX_NOF_SUBCARRIERS);

    return deco_results;
  }
};

INSTANTIATE_TEST_SUITE_P(prach_format,
                         data_flow_uplane_uplink_prach_impl_fixture,
                         ::testing::Combine(::testing::Values(prach_format_type::zero, prach_format_type::B4),
                                            ::testing::Values(true, false),
                                            ::testing::Values(true, false)));

TEST_P(data_flow_uplane_uplink_prach_impl_fixture, valid_message_containing_all_symbols_must_be_notified)
{
  for (unsigned i = 0, e = nof_symbols; i != e; ++i) {
    uplane_message_decoder_results deco_results = build_valid_decoder_results();
    deco_results.params.symbol_id               = i;
    uplane_decoder->set_results(deco_results);

    data_flow.decode_type1_message(eaxc, {});
  }

  ASSERT_FALSE(notifier->has_new_uplink_symbol_function_been_called());
  ASSERT_TRUE(notifier->has_new_prach_function_been_called());
}

TEST_P(data_flow_uplane_uplink_prach_impl_fixture, expired_cuda_packet_is_not_decoded_on_host)
{
  uplane_message_decoder_results deco_results      = build_valid_decoder_results();
  deco_results.sections.front().compressed_iq_data = span<const uint8_t>(reinterpret_cast<const uint8_t*>("x"), 1);
  uplane_decoder->set_results(deco_results);
  uplane_decoder->direct_decompression_enabled = true;
  prach_context_repo->clear();

  data_flow.decode_type1_message(eaxc, {});

  ASSERT_EQ(1U, uplane_decoder->compressed_decode_count);
  ASSERT_EQ(0U, uplane_decoder->host_decode_count);
  ASSERT_FALSE(notifier->has_new_prach_function_been_called());
  rx_data_flow_perf_metrics metrics{};
  data_flow.get_metrics_collector().collect_metrics(metrics);
  ASSERT_EQ(1U, metrics.direct_decompression.nof_missing_contexts);
}

TEST_P(data_flow_uplane_uplink_prach_impl_fixture, required_direct_decompression_does_not_fallback_for_host_buffer)
{
  auto config               = get_config();
  config.host_decompression = host_decompression_policy::forbid;
  data_flow_uplane_uplink_prach_impl strict_data_flow(config, get_dependencies());

  uplane_message_decoder_results deco_results      = build_valid_decoder_results();
  deco_results.sections.front().compressed_iq_data = span<const uint8_t>(reinterpret_cast<const uint8_t*>("x"), 1);
  uplane_decoder->set_results(deco_results);
  uplane_decoder->direct_decompression_enabled = true;

  strict_data_flow.decode_type1_message(eaxc, {});

  ASSERT_EQ(1U, uplane_decoder->compressed_decode_count);
  ASSERT_EQ(0U, uplane_decoder->host_decode_count);
  ASSERT_FALSE(notifier->has_new_prach_function_been_called());
  rx_data_flow_perf_metrics metrics{};
  strict_data_flow.get_metrics_collector().collect_metrics(metrics);
  ASSERT_EQ(1U, metrics.direct_decompression.nof_unsupported_destinations);
  ASSERT_EQ(1U, metrics.nof_dropped_messages);
}

TEST_P(data_flow_uplane_uplink_prach_impl_fixture, required_direct_decompression_drops_decode_failure)
{
  auto config               = get_config();
  config.host_decompression = host_decompression_policy::forbid;
  data_flow_uplane_uplink_prach_impl strict_data_flow(config, get_dependencies());

  uplane_decoder->direct_decompression_enabled = true;
  uplane_decoder->compressed_decode_succeeds   = false;

  strict_data_flow.decode_type1_message(eaxc, {});

  ASSERT_EQ(1U, uplane_decoder->compressed_decode_count);
  ASSERT_EQ(0U, uplane_decoder->host_decode_count);
  ASSERT_FALSE(notifier->has_new_prach_function_been_called());
  rx_data_flow_perf_metrics metrics{};
  strict_data_flow.get_metrics_collector().collect_metrics(metrics);
  ASSERT_EQ(1U, metrics.direct_decompression.nof_invalid_inputs);
  ASSERT_EQ(1U, metrics.nof_dropped_messages);
}

TEST_P(data_flow_uplane_uplink_prach_impl_fixture, invalid_filter_index_does_not_write_buffer)
{
  uplane_message_decoder_results deco_results = build_valid_decoder_results();
  deco_results.params.filter_index            = ocudu::ofh::filter_index_type::standard_channel_filter;
  uplane_decoder->set_results(deco_results);
  data_flow.decode_type1_message(eaxc, {});

  ASSERT_FALSE(notifier->has_new_uplink_symbol_function_been_called());
  ASSERT_FALSE(notifier->has_new_prach_function_been_called());
}

TEST_P(data_flow_uplane_uplink_prach_impl_fixture, prbs_outside_prach_range_does_not_notify)
{
  for (unsigned i = 0, e = nof_symbols; i != e; ++i) {
    uplane_message_decoder_results deco_results = build_valid_decoder_results();
    deco_results.params.symbol_id               = i;
    deco_results.sections.front().start_prb     = 272;
    deco_results.sections.front().nof_prbs      = 1;
    uplane_decoder->set_results(deco_results);

    data_flow.decode_type1_message(eaxc, {});
  }

  ASSERT_FALSE(notifier->has_new_uplink_symbol_function_been_called());
  ASSERT_FALSE(notifier->has_new_prach_function_been_called());
}
