// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/e3/e3ap_codec.h"
#include "ocudu/dapp/e3/e3ap_v2_codec.h"
#include "ocudu/dapp/e3/telemetry_types.h"
#include <cmath>
#include <cstring>
#include <gtest/gtest.h>

using namespace ocudu::dapp;
using namespace ocudu::dapp::e3;

namespace {

template <typename T>
const T& body(const codec_result<e3ap_pdu>& decoded)
{
  return std::get<T>(decoded.value->message);
}

std::vector<uint8_t> samples(std::size_t count, float seed)
{
  std::vector<uint8_t> bytes(count * 2 * sizeof(float));
  for (std::size_t index = 0; index != count * 2; ++index) {
    const float value = seed + static_cast<float>(index) * 0.5F;
    std::memcpy(bytes.data() + index * sizeof(float), &value, sizeof(float));
  }
  return bytes;
}

} // namespace

TEST(dapp_e3ap_codec, setup_procedure_round_trips_with_ran_functions)
{
  e3ap_pdu request;
  request.id = 7;
  e3ap_setup_request setup;
  setup.dapp_name    = "example-dash";
  setup.dapp_version = "0.1.0";
  setup.vendor       = "DeepSig";
  request.message    = setup;
  auto encoded       = e3ap_v2_codec::encode(request, e3ap_profile::ocudu);
  ASSERT_TRUE(encoded) << encoded.detail;
  auto decoded = e3ap_v2_codec::decode(*encoded.value, e3ap_profile::ocudu);
  ASSERT_TRUE(decoded) << decoded.detail;
  EXPECT_EQ(decoded.value->id, 7U);
  EXPECT_EQ(body<e3ap_setup_request>(decoded).dapp_name, "example-dash");
  EXPECT_EQ(body<e3ap_setup_request>(decoded).e3ap_protocol_version, "1.0.0");

  telemetry_ran_function_data function;
  function.streams                           = default_telemetry_streams();
  function.streams[2].hard_floor_interval_us = 2'000;
  function.maximum_object_bytes              = 4 * 1024 * 1024;
  function.maximum_subscriptions             = 16;
  auto function_bytes                        = e3ap_codec::encode(function);
  ASSERT_TRUE(function_bytes) << function_bytes.detail;
  auto function_back = e3ap_codec::decode_telemetry_function(*function_bytes.value);
  ASSERT_TRUE(function_back) << function_back.detail;
  ASSERT_EQ(function_back.value->streams.size(), function.streams.size());
  EXPECT_EQ(function_back.value->streams[0].name, "pusch.symbols");
  EXPECT_EQ(function_back.value->streams[5].interface_id, 0x00010001U);
  EXPECT_TRUE(function_back.value->streams[5].class_c_portable);
  ASSERT_TRUE(function_back.value->streams[2].hard_floor_interval_us);
  EXPECT_EQ(*function_back.value->streams[2].hard_floor_interval_us, 2'000U);
  EXPECT_FALSE(function_back.value->streams[1].hard_floor_interval_us);

  e3ap_pdu response;
  response.id = 7;
  e3ap_setup_response setup_response;
  setup_response.request_id            = 7;
  setup_response.response_code         = e3ap_response_code::positive;
  setup_response.ran_identifier        = "ocudu-gnb";
  setup_response.dapp_identifier       = 3;
  setup_response.ran_functions_present = true;
  e3ap_ran_function_definition definition;
  definition.ran_function_identifier = e3ap_ran_function_telemetry;
  definition.telemetry_identifiers   = {1, 2, 3, 4, 5, 6, 7};
  definition.ran_function_data       = *function_bytes.value;
  definition.ran_function_name       = "OCUDU-TELEMETRY-SM";
  setup_response.ran_functions.push_back(definition);
  setup_response.maximum_payload_bytes  = 4 * 1024 * 1024;
  setup_response.maximum_fragment_bytes = 32768;
  response.message                      = setup_response;
  encoded                               = e3ap_v2_codec::encode(response, e3ap_profile::ocudu);
  ASSERT_TRUE(encoded) << encoded.detail;
  decoded = e3ap_v2_codec::decode(*encoded.value, e3ap_profile::ocudu);
  ASSERT_TRUE(decoded) << decoded.detail;
  const auto& back = body<e3ap_setup_response>(decoded);
  EXPECT_EQ(back.response_code, e3ap_response_code::positive);
  ASSERT_TRUE(back.dapp_identifier);
  EXPECT_EQ(*back.dapp_identifier, 3U);
  ASSERT_EQ(back.ran_functions.size(), 1U);
  EXPECT_EQ(back.ran_functions[0].telemetry_identifiers.size(), 7U);
  EXPECT_EQ(back.ran_functions[0].ran_function_data, *function_bytes.value);
  ASSERT_TRUE(back.maximum_fragment_bytes);
  EXPECT_EQ(*back.maximum_fragment_bytes, 32768U);
}

TEST(dapp_e3ap_codec, setup_request_matches_the_python_golden_vector)
{
  // python/tests/test_e3ap_client.py pins the same bytes, which are libe3's setup request (a setup
  // without OCUDU additions); a change here is a wire change.
  e3ap_pdu pdu;
  pdu.id = 1;
  e3ap_setup_request setup;
  setup.dapp_name    = "x";
  setup.dapp_version = "0";
  setup.vendor       = "v";
  pdu.message        = setup;
  auto encoded       = e3ap_v2_codec::encode(pdu, e3ap_profile::ocudu);
  ASSERT_TRUE(encoded) << encoded.detail;
  const std::vector<uint8_t> golden = {0x00, 0x00, 0x01, 0x00, 0x31, 0x2e, 0x30, 0x2e, 0x30, 0x00, 0x78, 0x00, 0x30, 0x00, 0x76};
  EXPECT_EQ(*encoded.value, golden);
}

TEST(dapp_e3ap_codec, subscription_and_control_messages_round_trip)
{
  telemetry_subscription_data   data;
  telemetry_subscription_config config;
  config.telemetry_id        = telemetry_id_pusch_symbols;
  config.cell_id             = 0;
  config.sample_every_n      = 4;
  config.drop_policy         = telemetry_drop_policy::drop_oldest;
  config.minimum_interval_ms = 100;
  config.rnti_filter         = {0x4601, 0x4602};
  data.configs.push_back(config);
  auto data_bytes = e3ap_codec::encode(data);
  ASSERT_TRUE(data_bytes) << data_bytes.detail;
  auto data_back = e3ap_codec::decode_subscription(*data_bytes.value);
  ASSERT_TRUE(data_back) << data_back.detail;
  ASSERT_EQ(data_back.value->configs.size(), 1U);
  EXPECT_EQ(data_back.value->configs[0].sample_every_n, 4U);
  EXPECT_EQ(data_back.value->configs[0].drop_policy, telemetry_drop_policy::drop_oldest);
  EXPECT_EQ(data_back.value->configs[0].rnti_filter.size(), 2U);

  e3ap_pdu pdu;
  pdu.id = 2;
  e3ap_subscription_request subscription;
  subscription.dapp_identifier         = 3;
  subscription.ran_function_identifier = e3ap_ran_function_telemetry;
  subscription.telemetry_identifiers   = {1, 2, 5};
  subscription.periodicity             = 50;
  subscription.subscription_data       = *data_bytes.value;
  pdu.message                          = subscription;
  auto encoded                         = e3ap_v2_codec::encode(pdu, e3ap_profile::ocudu);
  ASSERT_TRUE(encoded) << encoded.detail;
  auto decoded = e3ap_v2_codec::decode(*encoded.value, e3ap_profile::ocudu);
  ASSERT_TRUE(decoded) << decoded.detail;
  EXPECT_EQ(body<e3ap_subscription_request>(decoded).telemetry_identifiers.size(), 3U);
  EXPECT_EQ(body<e3ap_subscription_request>(decoded).subscription_data, *data_bytes.value);

  e3ap_dapp_control_action action;
  action.dapp_identifier         = 3;
  action.ran_function_identifier = e3ap_ran_function_management;
  action.control_identifier      = e3ap_management_control_request;
  action.action_data             = {1, 2, 3, 4};
  action.fragment_index          = 1;
  action.fragment_count          = 2;
  action.payload_bytes           = 8;
  pdu.message                    = action;
  encoded                        = e3ap_v2_codec::encode(pdu, e3ap_profile::ocudu);
  ASSERT_TRUE(encoded) << encoded.detail;
  decoded = e3ap_v2_codec::decode(*encoded.value, e3ap_profile::ocudu);
  ASSERT_TRUE(decoded) << decoded.detail;
  EXPECT_EQ(body<e3ap_dapp_control_action>(decoded).fragment_index, 1U);
  EXPECT_EQ(body<e3ap_dapp_control_action>(decoded).fragment_count, 2U);
  ASSERT_TRUE(body<e3ap_dapp_control_action>(decoded).payload_bytes);
  EXPECT_EQ(*body<e3ap_dapp_control_action>(decoded).payload_bytes, 8U);

  e3ap_control_response outcome;
  outcome.request_id              = 2;
  outcome.dapp_identifier         = 3;
  outcome.ran_function_identifier = e3ap_ran_function_management;
  outcome.control_identifier      = 1;
  outcome.response_code           = e3ap_response_code::negative;
  outcome.cause                   = e3ap_cause::invalid_request;
  outcome.detail                  = "rejected";
  pdu.message                     = outcome;
  encoded                         = e3ap_v2_codec::encode(pdu, e3ap_profile::ocudu);
  ASSERT_TRUE(encoded) << encoded.detail;
  decoded = e3ap_v2_codec::decode(*encoded.value, e3ap_profile::ocudu);
  ASSERT_TRUE(decoded) << decoded.detail;
  EXPECT_EQ(body<e3ap_control_response>(decoded).detail, "rejected");
  ASSERT_TRUE(body<e3ap_control_response>(decoded).cause);
  EXPECT_EQ(*body<e3ap_control_response>(decoded).cause, e3ap_cause::invalid_request);

  e3ap_message_ack ack;
  ack.request_id    = 9;
  ack.response_code = e3ap_response_code::positive;
  pdu.message       = ack;
  encoded           = e3ap_v2_codec::encode(pdu, e3ap_profile::ocudu);
  ASSERT_TRUE(encoded) << encoded.detail;
  decoded = e3ap_v2_codec::decode(*encoded.value, e3ap_profile::ocudu);
  ASSERT_TRUE(decoded) << decoded.detail;
  EXPECT_EQ(body<e3ap_message_ack>(decoded).request_id, 9U);
}

TEST(dapp_e3ap_codec, rejects_invalid_identifiers_and_malformed_pdus)
{
  e3ap_pdu pdu;
  pdu.id      = 0;
  pdu.message = e3ap_release_message{1};
  EXPECT_FALSE(e3ap_v2_codec::encode(pdu, e3ap_profile::ocudu));
  pdu.id      = 1;
  pdu.message = e3ap_release_message{0};
  EXPECT_FALSE(e3ap_v2_codec::encode(pdu, e3ap_profile::ocudu));

  e3ap_indication_message indication;
  indication.dapp_identifier         = 1;
  indication.ran_function_identifier = 10;
  indication.protocol_data           = std::vector<uint8_t>(e3ap_maximum_fragment_bytes + 1, 0x11);
  pdu.message                        = indication;
  EXPECT_FALSE(e3ap_v2_codec::encode(pdu, e3ap_profile::ocudu));

  const uint8_t garbage[] = {0xff, 0xfe, 0xfd, 0x00};
  EXPECT_FALSE(e3ap_v2_codec::decode(garbage, e3ap_profile::ocudu));
  EXPECT_FALSE(e3ap_v2_codec::decode({}, e3ap_profile::ocudu));
  EXPECT_FALSE(e3ap_codec::decode_telemetry(garbage));
}

TEST(dapp_e3ap_codec, telemetry_indications_round_trip_every_record_kind)
{
  telemetry_indication indication;
  indication.telemetry_id    = telemetry_id_pusch_symbols;
  indication.cell_id         = 0;
  indication.sequence        = 42;
  indication.route_epoch     = 17;
  indication.monotonic_ns    = 1'000;
  indication.dropped_objects = 1;
  pusch_symbols_record symbols;
  symbols.rnti              = 0x4601;
  symbols.radio_slot        = {512, 7, 1};
  symbols.nof_prb           = 106;
  symbols.nof_data_symbols  = 12;
  symbols.nof_layers        = 1;
  symbols.modulation_bits   = 6;
  symbols.sinr_db           = 21.5;
  symbols.evm               = 0.05;
  symbols.equalized_symbols = samples(106 * 12, 0.25F);
  pusch_grant_parameters grant;
  grant.harq_id              = 3;
  grant.n_id                 = 1;
  grant.tbs_bytes            = 1234;
  grant.rv                   = 2;
  grant.new_data             = false;
  grant.ldpc_base_graph      = 2;
  grant.tbs_lbrm_bytes       = 65536;
  grant.mcs_index            = 16;
  grant.mcs_table            = 1;
  grant.start_symbol         = 0;
  grant.nof_symbols          = 14;
  grant.dmrs_symbol_mask     = 0x0804;
  grant.dmrs_scrambling_id   = 1;
  grant.bwp_size_rb          = 106;
  grant.prb_mask[0]          = 0xff;
  grant.prb_mask[13]         = 0x03;
  grant.nof_rx_ports         = 1;
  grant.nof_layers           = 1;
  grant.dc_position          = 636;
  grant.nof_harq_ack_bits    = 2;
  grant.alpha_scaling        = 1.0;
  grant.beta_offset_harq_ack = 12.625;
  symbols.grant              = grant;
  symbols.crc_ok             = true;
  symbols.symbols_truncated  = false;
  indication.object          = symbols;
  auto encoded               = e3ap_codec::encode(indication);
  ASSERT_TRUE(encoded) << encoded.detail;
  auto decoded = e3ap_codec::decode_telemetry(*encoded.value);
  ASSERT_TRUE(decoded) << decoded.detail;
  const auto& back = std::get<pusch_symbols_record>(decoded.value->object);
  EXPECT_EQ(back.rnti, 0x4601);
  EXPECT_EQ(back.radio_slot.sfn, 512U);
  EXPECT_EQ(back.equalized_symbols, symbols.equalized_symbols);
  ASSERT_TRUE(back.sinr_db);
  EXPECT_DOUBLE_EQ(*back.sinr_db, 21.5);
  EXPECT_FALSE(back.epre_db);
  EXPECT_EQ(decoded.value->sequence, 42U);
  ASSERT_TRUE(back.grant);
  EXPECT_EQ(back.grant->tbs_bytes, 1234U);
  EXPECT_EQ(back.grant->ldpc_base_graph, 2U);
  EXPECT_FALSE(back.grant->new_data);
  ASSERT_TRUE(back.grant->mcs_index);
  EXPECT_EQ(*back.grant->mcs_index, 16U);
  ASSERT_TRUE(back.grant->mcs_table);
  EXPECT_EQ(*back.grant->mcs_table, 1U);
  EXPECT_EQ(back.grant->dmrs_symbol_mask, 0x0804U);
  EXPECT_EQ(back.grant->prb_mask, grant.prb_mask);
  ASSERT_TRUE(back.grant->dc_position);
  EXPECT_EQ(*back.grant->dc_position, 636U);
  EXPECT_DOUBLE_EQ(back.grant->beta_offset_harq_ack, 12.625);
  ASSERT_TRUE(back.crc_ok);
  EXPECT_TRUE(*back.crc_ok);
  EXPECT_FALSE(back.symbols_truncated);

  pusch_grid_record grid;
  grid.rnti               = 0x4601;
  grid.radio_slot         = {512, 7, 1};
  grid.nof_prb            = 8;
  grid.nof_rx_ports       = 2;
  grid.start_symbol       = 0;
  grid.nof_symbols        = 14;
  grid.grant              = grant;
  grid.resource_elements  = samples(8 * 12 * 14 * 2, 0.5F);
  indication.telemetry_id = telemetry_id_pusch_grid;
  indication.object       = grid;
  encoded                 = e3ap_codec::encode(indication);
  ASSERT_TRUE(encoded) << encoded.detail;
  decoded = e3ap_codec::decode_telemetry(*encoded.value);
  ASSERT_TRUE(decoded) << decoded.detail;
  const auto& grid_back = std::get<pusch_grid_record>(decoded.value->object);
  EXPECT_EQ(grid_back.nof_rx_ports, 2U);
  EXPECT_EQ(grid_back.grant.tbs_bytes, 1234U);
  EXPECT_EQ(grid_back.resource_elements, grid.resource_elements);

  pusch_crc_record crc{0x4601, {1, 2, 1}, true};
  indication.telemetry_id = telemetry_id_pusch_crc;
  indication.object       = crc;
  encoded                 = e3ap_codec::encode(indication);
  ASSERT_TRUE(encoded) << encoded.detail;
  decoded = e3ap_codec::decode_telemetry(*encoded.value);
  ASSERT_TRUE(decoded) << decoded.detail;
  EXPECT_TRUE(std::get<pusch_crc_record>(decoded.value->object).crc_ok);

  srs_channel_record srs;
  srs.rnti                = 0x4601;
  srs.nof_subcarriers     = 1;
  srs.nof_rx_ports        = 2;
  srs.nof_tx_ports        = 1;
  srs.rx_port_index       = 1;
  srs.rsrp_db             = -70.0;
  srs.comb_offset         = 2;
  srs.coefficients        = samples(1, 1.0F);
  indication.telemetry_id = telemetry_id_srs_channel;
  indication.object       = srs;
  encoded                 = e3ap_codec::encode(indication);
  ASSERT_TRUE(encoded) << encoded.detail;
  decoded = e3ap_codec::decode_telemetry(*encoded.value);
  ASSERT_TRUE(decoded) << decoded.detail;
  ASSERT_TRUE(std::get<srs_channel_record>(decoded.value->object).comb_offset);
  EXPECT_EQ(*std::get<srs_channel_record>(decoded.value->object).comb_offset, 2U);
  EXPECT_FALSE(std::get<srs_channel_record>(decoded.value->object).sequence_id);
  EXPECT_TRUE(std::get<srs_channel_record>(decoded.value->object).received_pilots.empty());
  srs.received_pilots   = samples(3, 0.5F);
  srs.reference_symbols = samples(1, -1.0F);
  indication.object     = srs;
  encoded               = e3ap_codec::encode(indication);
  ASSERT_TRUE(encoded) << encoded.detail;
  decoded = e3ap_codec::decode_telemetry(*encoded.value);
  ASSERT_TRUE(decoded) << decoded.detail;
  EXPECT_EQ(std::get<srs_channel_record>(decoded.value->object).received_pilots, srs.received_pilots);
  EXPECT_EQ(std::get<srs_channel_record>(decoded.value->object).reference_symbols, srs.reference_symbols);

  pusch_dmrs_channel_record dmrs;
  dmrs.rnti               = 0x4601;
  dmrs.nof_prb            = 4;
  dmrs.nof_subcarriers    = 24;
  dmrs.dmrs_type          = 1;
  dmrs.nof_rx_ports       = 1;
  dmrs.nof_tx_layers      = 1;
  dmrs.estimates          = samples(24, 2.0F);
  indication.telemetry_id = telemetry_id_pusch_dmrs_channel;
  indication.object       = dmrs;
  encoded                 = e3ap_codec::encode(indication);
  ASSERT_TRUE(encoded) << encoded.detail;
  decoded = e3ap_codec::decode_telemetry(*encoded.value);
  ASSERT_TRUE(decoded) << decoded.detail;
  EXPECT_EQ(std::get<pusch_dmrs_channel_record>(decoded.value->object).estimates.size(), 24U * 8U);

  gnb_metrics_json_record json;
  const std::string       text = "{\"cell\":1}";
  json.json.assign(text.begin(), text.end());
  indication.telemetry_id = telemetry_id_gnb_metrics_json;
  indication.object       = json;
  encoded                 = e3ap_codec::encode(indication);
  ASSERT_TRUE(encoded) << encoded.detail;
  decoded = e3ap_codec::decode_telemetry(*encoded.value);
  ASSERT_TRUE(decoded) << decoded.detail;
  EXPECT_EQ(std::get<gnb_metrics_json_record>(decoded.value->object).json, json.json);

  class_c_portable_record portable;
  portable.interface_id   = 0x00010001;
  portable.metadata       = {1, 2, 3};
  portable.tensor         = std::vector<uint8_t>(300'000, 0x5a);
  indication.telemetry_id = telemetry_id_class_c_spectrum;
  indication.object       = portable;
  encoded                 = e3ap_codec::encode(indication);
  ASSERT_TRUE(encoded) << encoded.detail;
  EXPECT_GT(encoded.value->size(), 300'000U);
  decoded = e3ap_codec::decode_telemetry(*encoded.value);
  ASSERT_TRUE(decoded) << decoded.detail;
  EXPECT_EQ(std::get<class_c_portable_record>(decoded.value->object).tensor.size(), 300'000U);

  const auto fragments = e3ap_codec::fragment(*encoded.value, e3ap_maximum_fragment_bytes);
  ASSERT_EQ(fragments.size(), (encoded.value->size() + e3ap_maximum_fragment_bytes - 1) / e3ap_maximum_fragment_bytes);
  std::size_t total = 0;
  for (const auto& fragment : fragments) {
    total += fragment.size();
  }
  EXPECT_EQ(total, encoded.value->size());
}

TEST(dapp_e3ap_codec, converts_packed_store_metadata_into_records)
{
  pusch_symbols_metadata_v1 metadata;
  metadata.header.monotonic_ns = 99;
  metadata.rnti                = 0x1234;
  metadata.radio_slot          = {10, 3, 1};
  metadata.nof_prb             = 12;
  metadata.nof_data_symbols    = 11;
  metadata.nof_layers          = 1;
  metadata.modulation_bits     = 4;
  metadata.sinr_db             = 12.5F;
  metadata.epre_db             = std::nanf("");
  const auto  tensor           = samples(12 * 12 * 11, 0.0F);
  const auto* bytes            = reinterpret_cast<const uint8_t*>(&metadata);
  auto        converted =
      e3ap_codec::from_publication(telemetry_stream_pusch_symbols, 0, 5, 7, 0, {bytes, sizeof(metadata)}, tensor);
  ASSERT_TRUE(converted) << converted.detail;
  EXPECT_EQ(converted.value->telemetry_id, telemetry_id_pusch_symbols);
  EXPECT_EQ(converted.value->monotonic_ns, 99U);
  const auto& record = std::get<pusch_symbols_record>(converted.value->object);
  EXPECT_EQ(record.rnti, 0x1234);
  ASSERT_TRUE(record.sinr_db);
  EXPECT_FLOAT_EQ(static_cast<float>(*record.sinr_db), 12.5F);
  EXPECT_FALSE(record.epre_db);
  EXPECT_EQ(record.equalized_symbols, tensor);

  // A header that names the wrong kind or a truncated record is rejected.
  metadata.header.kind = static_cast<uint16_t>(telemetry_kind::pusch_crc);
  EXPECT_FALSE(
      e3ap_codec::from_publication(telemetry_stream_pusch_symbols, 0, 5, 7, 0, {bytes, sizeof(metadata)}, tensor));
  metadata.header.kind = static_cast<uint16_t>(telemetry_kind::pusch_symbols);
  EXPECT_FALSE(
      e3ap_codec::from_publication(telemetry_stream_pusch_symbols, 0, 5, 7, 0, {bytes, sizeof(metadata) - 1}, tensor));

  // Class-C portable publications are forwarded verbatim.
  const std::vector<uint8_t> envelope = {0x4f, 0x43, 0x43, 0x33};
  auto                       portable = e3ap_codec::from_publication(0x00010002, 1, 9, 3, 2, envelope, tensor);
  ASSERT_TRUE(portable) << portable.detail;
  EXPECT_EQ(portable.value->telemetry_id, telemetry_id_class_c_srs_isac);
  EXPECT_EQ(std::get<class_c_portable_record>(portable.value->object).metadata, envelope);
  EXPECT_EQ(portable.value->dropped_objects, 2U);
}

TEST(dapp_e3ap_codec, envelope_timestamp_round_trips_and_matches_the_python_golden_vector)
{
  // The timestamp is libe3's root OPTIONAL field. python/tests/test_e3ap_client.py pins the same bytes.
  e3ap_pdu pdu;
  pdu.id           = 1;
  pdu.timestamp_ns = 1756800000000000000LL;
  e3ap_setup_request setup;
  setup.dapp_name    = "x";
  setup.dapp_version = "0";
  setup.vendor       = "v";
  pdu.message        = setup;
  auto encoded       = e3ap_v2_codec::encode(pdu, e3ap_profile::ocudu);
  ASSERT_TRUE(encoded) << encoded.detail;
  const std::vector<uint8_t> golden = {0x40, 0x00, 0x08, 0x18, 0x61, 0x68, 0x4a, 0xc5, 0x68, 0x00, 0x00, 0x01, 0x00, 0x31, 0x2e, 0x30, 0x2e, 0x30, 0x00, 0x78, 0x00, 0x30, 0x00, 0x76};
  EXPECT_EQ(*encoded.value, golden);
  auto decoded = e3ap_v2_codec::decode(*encoded.value, e3ap_profile::ocudu);
  ASSERT_TRUE(decoded) << decoded.detail;
  ASSERT_TRUE(decoded.value->timestamp_ns);
  EXPECT_EQ(*decoded.value->timestamp_ns, 1756800000000000000LL);

  // Without a timestamp the presence bit is clear and the rest is unchanged.
  pdu.timestamp_ns.reset();
  auto plain = e3ap_v2_codec::encode(pdu, e3ap_profile::ocudu);
  ASSERT_TRUE(plain) << plain.detail;
  const std::vector<uint8_t> plain_golden = {0x00, 0x00, 0x01, 0x00, 0x31, 0x2e, 0x30, 0x2e, 0x30, 0x00, 0x78, 0x00, 0x30, 0x00, 0x76};
  EXPECT_EQ(*plain.value, plain_golden);
  auto plain_decoded = e3ap_v2_codec::decode(*plain.value, e3ap_profile::ocudu);
  ASSERT_TRUE(plain_decoded) << plain_decoded.detail;
  EXPECT_FALSE(plain_decoded.value->timestamp_ns);
}

TEST(dapp_e3ap_codec, by_reference_descriptors_and_regions_round_trip)
{
  // Subscription request asks for by-reference delivery.
  e3ap_pdu pdu;
  pdu.id = 7;
  e3ap_subscription_request request;
  request.dapp_identifier            = 3;
  request.ran_function_identifier    = e3ap_ran_function_telemetry;
  request.telemetry_identifiers      = {telemetry_id_pusch_symbols};
  request.delivery_mode              = e3ap_delivery_mode::by_reference;
  request.minimum_by_reference_bytes = 4096;
  pdu.message                        = request;
  auto encoded                       = e3ap_v2_codec::encode(pdu, e3ap_profile::ocudu);
  ASSERT_TRUE(encoded) << encoded.detail;
  auto decoded = e3ap_v2_codec::decode(*encoded.value, e3ap_profile::ocudu);
  ASSERT_TRUE(decoded) << decoded.detail;
  const auto& back = std::get<e3ap_subscription_request>(decoded.value->message);
  ASSERT_TRUE(back.delivery_mode);
  EXPECT_EQ(*back.delivery_mode, e3ap_delivery_mode::by_reference);
  ASSERT_TRUE(back.minimum_by_reference_bytes);
  EXPECT_EQ(*back.minimum_by_reference_bytes, 4096U);

  // Subscription response advertises the region.
  e3ap_subscription_response response;
  response.request_id      = 7;
  response.dapp_identifier = 3;
  response.response_code   = e3ap_response_code::positive;
  response.subscription_id = 1;
  e3ap_shared_region region;
  region.region_name     = "/ocudu-e3-1234-3-1";
  region.region_bytes    = 64 + 4 * (64 + 4 * 1024 * 1024);
  region.row_bytes       = 64 + 4 * 1024 * 1024;
  region.nof_rows        = 4;
  response.shared_region = region;
  pdu.message            = response;
  encoded                = e3ap_v2_codec::encode(pdu, e3ap_profile::ocudu);
  ASSERT_TRUE(encoded) << encoded.detail;
  decoded = e3ap_v2_codec::decode(*encoded.value, e3ap_profile::ocudu);
  ASSERT_TRUE(decoded) << decoded.detail;
  const auto& response_back = std::get<e3ap_subscription_response>(decoded.value->message);
  ASSERT_TRUE(response_back.shared_region);
  EXPECT_EQ(response_back.shared_region->region_name, region.region_name);
  EXPECT_EQ(response_back.shared_region->region_bytes, region.region_bytes);
  EXPECT_EQ(response_back.shared_region->row_bytes, region.row_bytes);
  EXPECT_EQ(response_back.shared_region->nof_rows, 4U);
  EXPECT_EQ(response_back.shared_region->memory_domain, e3ap_memory_domain::posix_shared);

  // Indication carries a POSIX shared-region descriptor.
  e3ap_indication_message indication;
  indication.dapp_identifier         = 3;
  indication.ran_function_identifier = e3ap_ran_function_telemetry;
  indication.protocol_data           = {0x01, 0x02, 0x03};
  indication.subscription_id         = 1;
  indication.telemetry_identifier    = telemetry_id_pusch_symbols;
  indication.sequence                = 42;
  e3ap_tensor_descriptor descriptor;
  descriptor.memory_domain = e3ap_memory_domain::posix_shared;
  descriptor.location      = e3ap_posix_shared_location{"/ocudu-e3-1234-3-1", 2, 64 + 2 * (64 + 4 * 1024 * 1024) + 64};
  descriptor.byte_length   = 734208;
  descriptor.generation    = 10;
  descriptor.element_type  = e3ap_element_type::complex_fp32;
  descriptor.dimensions    = {4, 14, 3276};
  descriptor.byte_strides  = {14 * 3276 * 8, 3276 * 8, 8};
  descriptor.layout_id     = 2;
  descriptor.lease_expiry_ns = 123456789ULL;
  indication.by_reference    = descriptor;
  pdu.message                = indication;
  encoded                    = e3ap_v2_codec::encode(pdu, e3ap_profile::ocudu);
  ASSERT_TRUE(encoded) << encoded.detail;
  decoded = e3ap_v2_codec::decode(*encoded.value, e3ap_profile::ocudu);
  ASSERT_TRUE(decoded) << decoded.detail;
  const auto& indication_back = std::get<e3ap_indication_message>(decoded.value->message);
  ASSERT_TRUE(indication_back.by_reference);
  const auto& d = *indication_back.by_reference;
  EXPECT_EQ(d.memory_domain, e3ap_memory_domain::posix_shared);
  ASSERT_TRUE(std::holds_alternative<e3ap_posix_shared_location>(d.location));
  const auto& location = std::get<e3ap_posix_shared_location>(d.location);
  EXPECT_EQ(location.region_name, "/ocudu-e3-1234-3-1");
  EXPECT_EQ(location.row_index, 2U);
  EXPECT_EQ(location.byte_offset, 64U + 2 * (64 + 4 * 1024 * 1024) + 64);
  EXPECT_EQ(d.byte_length, 734208U);
  EXPECT_EQ(d.generation, 10U);
  ASSERT_TRUE(d.element_type);
  EXPECT_EQ(*d.element_type, e3ap_element_type::complex_fp32);
  EXPECT_EQ(d.dimensions, (std::vector<uint32_t>{4, 14, 3276}));
  EXPECT_EQ(d.byte_strides, (std::vector<uint64_t>{14 * 3276 * 8, 3276 * 8, 8}));
  ASSERT_TRUE(d.layout_id);
  EXPECT_EQ(*d.layout_id, 2U);
  ASSERT_TRUE(d.lease_expiry_ns);
  EXPECT_EQ(*d.lease_expiry_ns, 123456789ULL);

  // And an accelerator IPC descriptor for GPU consumers.
  e3ap_accelerator_ipc_location ipc;
  ipc.device_ordinal = 1;
  ipc.allocation_id  = 77;
  for (std::size_t index = 0; index != ipc.memory_handle.size(); ++index) {
    ipc.memory_handle[index] = static_cast<uint8_t>(index);
  }
  std::array<uint8_t, 64> event{};
  event.fill(0xEE);
  ipc.ready_event_handle   = event;
  ipc.byte_offset          = 4096;
  descriptor.memory_domain = e3ap_memory_domain::accelerator_ipc_export;
  descriptor.location      = ipc;
  descriptor.dimensions.clear();
  descriptor.byte_strides.clear();
  indication.by_reference = descriptor;
  pdu.message             = indication;
  encoded                 = e3ap_v2_codec::encode(pdu, e3ap_profile::ocudu);
  ASSERT_TRUE(encoded) << encoded.detail;
  decoded = e3ap_v2_codec::decode(*encoded.value, e3ap_profile::ocudu);
  ASSERT_TRUE(decoded) << decoded.detail;
  const auto& ipc_back = std::get<e3ap_indication_message>(decoded.value->message);
  ASSERT_TRUE(ipc_back.by_reference);
  EXPECT_EQ(ipc_back.by_reference->memory_domain, e3ap_memory_domain::accelerator_ipc_export);
  ASSERT_TRUE(std::holds_alternative<e3ap_accelerator_ipc_location>(ipc_back.by_reference->location));
  const auto& ipc_location = std::get<e3ap_accelerator_ipc_location>(ipc_back.by_reference->location);
  EXPECT_EQ(ipc_location.device_ordinal, 1U);
  EXPECT_EQ(ipc_location.allocation_id, 77U);
  EXPECT_EQ(ipc_location.memory_handle, ipc.memory_handle);
  ASSERT_TRUE(ipc_location.ready_event_handle);
  EXPECT_EQ(*ipc_location.ready_event_handle, event);
  EXPECT_EQ(ipc_location.byte_offset, 4096U);
  EXPECT_TRUE(ipc_back.by_reference->dimensions.empty());
}
