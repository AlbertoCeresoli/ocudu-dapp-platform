// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "../../../../lib/ofh/receiver/ofh_data_flow_uplane_uplink_data_impl.h"
#include "../ofh_uplane_rx_symbol_notifier_test_doubles.h"
#include "helpers.h"
#include "ocudu/ocudulog/ocudulog.h"
#include <gtest/gtest.h>

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

  bool supports_resource_grid_decompression() const override { return direct_decompression_enabled; }

  bool decode_with_compressed_iq(uplane_message_decoder_results& results, span<const uint8_t> message) override
  {
    ++compressed_decode_count;
    results = spy_results;
    return compressed_decode_succeeds;
  }

  void set_results(const uplane_message_decoder_results& results) { spy_results = results; }

  bool     direct_decompression_enabled = false;
  bool     compressed_decode_succeeds    = true;
  unsigned host_decode_count             = 0;
  unsigned compressed_decode_count       = 0;
};

} // namespace

class data_flow_uplane_uplink_data_impl_fixture : public ::testing::Test
{
protected:
  const static_vector<unsigned, MAX_NOF_SUPPORTED_EAXC> ul_eaxc = {5, 1, 2, 3};
  slot_point                                            slot;
  unsigned                                              symbol   = 0;
  unsigned                                              nof_prbs = 51;
  unsigned                                              sector   = 0;
  unsigned                                              eaxc     = 5;
  resource_grid_writer_bool_spy                         rg_writer;
  resource_grid_reader_spy                              rg_reader;
  resource_grid_spy                                     grid;
  shared_resource_grid_spy                              shared_grid;
  uplane_rx_symbol_notifier_spy*                        notifier;
  std::shared_ptr<uplink_cplane_context_repository>     ul_cplane_context_repo_ptr =
      std::make_shared<uplink_cplane_context_repository>(1);
  std::shared_ptr<uplink_context_repository> ul_context_repo = std::make_shared<uplink_context_repository>(1);
  uplane_message_decoder_spy*                uplane_decoder;
  data_flow_uplane_uplink_data_impl          data_flow;

public:
  data_flow_uplane_uplink_data_impl_fixture() :
    slot(0, 0, 1),
    rg_writer(nof_prbs),
    rg_reader(rg_writer.get_nof_ports(), rg_writer.get_nof_symbols(), nof_prbs),
    grid(rg_reader, rg_writer),
    shared_grid(grid),
    data_flow(get_config(), get_dependencies())
  {
    ul_cplane_context context;
    context.filter_index = ocudu::ofh::filter_index_type::standard_channel_filter;
    context.start_symbol = 0;
    context.prb_start    = 0;
    context.nof_prb      = nof_prbs;
    context.nof_symbols  = 14;

    // Fill the contexts
    ul_cplane_context_repo_ptr->add(slot, eaxc, context);
    ul_context_repo->add({slot, sector},
                         shared_grid.get_grid(),
                         {context.start_symbol, context.nof_symbols},
                         ocudulog::fetch_basic_logger("TEST"));
    ul_context_repo->process_pending_contexts();
  }

  data_flow_uplane_uplink_data_impl_config get_config()
  {
    data_flow_uplane_uplink_data_impl_config config{};
    config.ul_eaxc             = ul_eaxc;
    config.sector              = sector;
    config.are_metrics_enabled = false;

    return config;
  }

  data_flow_uplane_uplink_data_impl_dependencies get_dependencies()
  {
    data_flow_uplane_uplink_data_impl_dependencies dependencies;

    dependencies.logger                 = &ocudulog::fetch_basic_logger("TEST");
    dependencies.ul_cplane_context_repo = ul_cplane_context_repo_ptr;
    dependencies.ul_context_repo        = ul_context_repo;

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
    deco_results.params.filter_index  = filter_index_type::standard_channel_filter;
    deco_results.params.symbol_id     = 0;
    auto& section                     = deco_results.sections.emplace_back();
    section.start_prb                 = 0;
    section.nof_prbs                  = nof_prbs;
    section.use_current_symbol_number = true;
    section.is_every_rb_used          = true;
    section.iq_samples.resize(MAX_NOF_SUBCARRIERS);

    return deco_results;
  }
};

TEST_F(data_flow_uplane_uplink_data_impl_fixture, valid_message_containing_all_symbols_must_be_notified)
{
  uplane_message_decoder_results deco_results = build_valid_decoder_results();
  uplane_decoder->set_results(deco_results);
  data_flow.decode_type1_message(eaxc, {});

  ASSERT_TRUE(notifier->has_new_uplink_symbol_function_been_called());
  ASSERT_FALSE(notifier->has_new_prach_function_been_called());
  ASSERT_EQ(nof_prbs, rg_writer.get_nof_prbs_written());
}

TEST_F(data_flow_uplane_uplink_data_impl_fixture, expired_cuda_packet_is_not_decoded_on_host)
{
  uplane_message_decoder_results deco_results = build_valid_decoder_results();
  deco_results.sections.front().compressed_iq_data = span<const uint8_t>(reinterpret_cast<const uint8_t*>("x"), 1);
  uplane_decoder->set_results(deco_results);
  uplane_decoder->direct_decompression_enabled = true;
  ul_context_repo->clear();

  data_flow.decode_type1_message(eaxc, {});

  ASSERT_EQ(1U, uplane_decoder->compressed_decode_count);
  ASSERT_EQ(0U, uplane_decoder->host_decode_count);
  ASSERT_FALSE(notifier->has_new_uplink_symbol_function_been_called());
  rx_data_flow_perf_metrics metrics{};
  data_flow.get_metrics_collector().collect_metrics(metrics);
  ASSERT_EQ(1U, metrics.direct_decompression.nof_missing_contexts);
}

TEST_F(data_flow_uplane_uplink_data_impl_fixture, unavailable_device_grid_falls_back_before_writing)
{
  uplane_message_decoder_results deco_results = build_valid_decoder_results();
  deco_results.sections.front().compressed_iq_data = span<const uint8_t>(reinterpret_cast<const uint8_t*>("x"), 1);
  uplane_decoder->set_results(deco_results);
  uplane_decoder->direct_decompression_enabled = true;

  data_flow.decode_type1_message(eaxc, {});

  ASSERT_EQ(1U, uplane_decoder->compressed_decode_count);
  ASSERT_EQ(1U, uplane_decoder->host_decode_count);
  ASSERT_TRUE(notifier->has_new_uplink_symbol_function_been_called());
  ASSERT_EQ(nof_prbs, rg_writer.get_nof_prbs_written());
  rx_data_flow_perf_metrics metrics{};
  data_flow.get_metrics_collector().collect_metrics(metrics);
  ASSERT_EQ(1U, metrics.direct_decompression.nof_unsupported_destinations);
}

TEST_F(data_flow_uplane_uplink_data_impl_fixture, required_direct_decompression_does_not_fallback_for_host_grid)
{
  auto config               = get_config();
  config.host_decompression = host_decompression_policy::forbid;
  data_flow_uplane_uplink_data_impl strict_data_flow(config, get_dependencies());

  uplane_message_decoder_results deco_results = build_valid_decoder_results();
  deco_results.sections.front().compressed_iq_data = span<const uint8_t>(reinterpret_cast<const uint8_t*>("x"), 1);
  uplane_decoder->set_results(deco_results);
  uplane_decoder->direct_decompression_enabled = true;

  strict_data_flow.decode_type1_message(eaxc, {});

  ASSERT_EQ(1U, uplane_decoder->compressed_decode_count);
  ASSERT_EQ(0U, uplane_decoder->host_decode_count);
  ASSERT_FALSE(notifier->has_new_uplink_symbol_function_been_called());
  ASSERT_FALSE(rg_writer.has_grid_been_written());
  rx_data_flow_perf_metrics metrics{};
  strict_data_flow.get_metrics_collector().collect_metrics(metrics);
  ASSERT_EQ(1U, metrics.direct_decompression.nof_unsupported_destinations);
  ASSERT_EQ(1U, metrics.nof_dropped_messages);
}

TEST_F(data_flow_uplane_uplink_data_impl_fixture, required_direct_decompression_drops_decode_failure)
{
  auto config               = get_config();
  config.host_decompression = host_decompression_policy::forbid;
  data_flow_uplane_uplink_data_impl strict_data_flow(config, get_dependencies());

  uplane_decoder->direct_decompression_enabled = true;
  uplane_decoder->compressed_decode_succeeds    = false;

  strict_data_flow.decode_type1_message(eaxc, {});

  ASSERT_EQ(1U, uplane_decoder->compressed_decode_count);
  ASSERT_EQ(0U, uplane_decoder->host_decode_count);
  ASSERT_FALSE(notifier->has_new_uplink_symbol_function_been_called());
  rx_data_flow_perf_metrics metrics{};
  strict_data_flow.get_metrics_collector().collect_metrics(metrics);
  ASSERT_EQ(1U, metrics.direct_decompression.nof_invalid_inputs);
  ASSERT_EQ(1U, metrics.nof_dropped_messages);
}

TEST_F(data_flow_uplane_uplink_data_impl_fixture, invalid_filter_index_does_not_write_grid)
{
  uplane_message_decoder_results deco_results = build_valid_decoder_results();
  deco_results.params.filter_index            = ocudu::ofh::filter_index_type::ul_prach_preamble_1p25khz;
  uplane_decoder->set_results(deco_results);
  data_flow.decode_type1_message(eaxc, {});

  ASSERT_FALSE(notifier->has_new_uplink_symbol_function_been_called());
  ASSERT_FALSE(notifier->has_new_prach_function_been_called());
  ASSERT_FALSE(rg_writer.has_grid_been_written());
}

TEST_F(data_flow_uplane_uplink_data_impl_fixture, invalid_prb_range_does_not_write_grid)
{
  uplane_message_decoder_results deco_results = build_valid_decoder_results();
  deco_results.sections.front().start_prb     = 51;
  uplane_decoder->set_results(deco_results);
  data_flow.decode_type1_message(eaxc, {});

  ASSERT_FALSE(notifier->has_new_uplink_symbol_function_been_called());
  ASSERT_FALSE(notifier->has_new_prach_function_been_called());
  ASSERT_FALSE(rg_writer.has_grid_been_written());
}

TEST_F(data_flow_uplane_uplink_data_impl_fixture, partial_prb_falling_outside_cplane_prbs_does_not_write_grid)
{
  uplane_message_decoder_results deco_results = build_valid_decoder_results();
  deco_results.sections.front().start_prb     = 41;
  deco_results.sections.front().nof_prbs      = 51;
  uplane_decoder->set_results(deco_results);
  data_flow.decode_type1_message(eaxc, {});

  ASSERT_FALSE(notifier->has_new_uplink_symbol_function_been_called());
  ASSERT_FALSE(notifier->has_new_prach_function_been_called());
  ASSERT_FALSE(rg_writer.has_grid_been_written());
}

TEST_F(data_flow_uplane_uplink_data_impl_fixture, partial_prb_inside_cplane_prbs_write_grid)
{
  uplane_message_decoder_results deco_results = build_valid_decoder_results();
  unsigned                       num_prbs     = 23;
  deco_results.sections.front().start_prb     = 10;
  deco_results.sections.front().nof_prbs      = num_prbs;
  uplane_decoder->set_results(deco_results);
  data_flow.decode_type1_message(eaxc, {});

  ASSERT_FALSE(notifier->has_new_uplink_symbol_function_been_called());
  ASSERT_FALSE(notifier->has_new_prach_function_been_called());
  ASSERT_TRUE(rg_writer.has_grid_been_written());
  ASSERT_EQ(num_prbs, rg_writer.get_nof_prbs_written());
}
