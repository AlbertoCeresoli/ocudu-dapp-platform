// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/e3/aper_codec.h"
#include "ocudu/dapp/e3/embedded_agent.h"
#include "ocudu/dapp/e3/management_service.h"
#include "ocudu/dapp/e3/portable_data_store.h"
#include "ocudu/scheduler/dapp/quiet_reservation.h"
#include <chrono>
#include <cstring>
#include <gtest/gtest.h>
#include <limits>
#include <random>

using namespace ocudu::dapp;
using namespace ocudu::dapp::e3;
using namespace ocudu::dapp::management;

namespace {

identifier test_id(uint8_t first)
{
  identifier result;
  for (std::size_t index = 0; index != result.bytes.size(); ++index) {
    result.bytes[index] = static_cast<uint8_t>(first + index);
  }
  return result;
}

std::unique_ptr<instance_manager> make_manager()
{
  ocudu_dapp_host_info_v1 host{};
  host.struct_size = sizeof(host);
  host.abi_major   = OCUDU_DAPP_ABI_MAJOR_V1;
  ocudu_dapp_host_api_v1 api{};
  api.struct_size = sizeof(api);
  api.abi_major   = OCUDU_DAPP_ABI_MAJOR_V1;
  return std::make_unique<instance_manager>(DAPP_TEST_CATALOG, host, api, 32);
}

std::vector<uint8_t> encode_request(const management_request& request)
{
  auto encoded = aper_codec::encode(request);
  EXPECT_TRUE(encoded) << encoded.detail;
  return encoded.value.value_or(std::vector<uint8_t>{});
}

std::vector<uint8_t> encode_response(const management_response& response)
{
  auto encoded = aper_codec::encode(response);
  EXPECT_TRUE(encoded) << encoded.detail;
  return encoded.value.value_or(std::vector<uint8_t>{});
}

std::string to_hex(const std::vector<uint8_t>& bytes)
{
  static constexpr char digits[] = "0123456789abcdef";
  std::string           result(bytes.size() * 2, '0');
  for (std::size_t index = 0; index != bytes.size(); ++index) {
    result[2 * index]     = digits[bytes[index] >> 4U];
    result[2 * index + 1] = digits[bytes[index] & 0xfU];
  }
  return result;
}

class test_ran_control_handler final : public ran_control_handler
{
public:
  bool request_ue_release(uint16_t serving_pci, uint16_t rnti) noexcept override
  {
    ++requests;
    last_pci  = serving_pci;
    last_rnti = rnti;
    return accept;
  }

  bool request_handover(uint16_t serving_pci, uint16_t rnti, uint16_t target_pci) noexcept override
  {
    ++handovers;
    last_pci    = serving_pci;
    last_rnti   = rnti;
    last_target = target_pci;
    return accept;
  }
  bool set_tx_gain(uint8_t port_id, double gain_db) noexcept override
  {
    last_port = port_id;
    last_gain = gain_db;
    ++gain_sets;
    return accept;
  }
  bool set_rx_gain(uint8_t port_id, double gain_db) noexcept override { return set_tx_gain(port_id, gain_db); }
  std::vector<cell_descriptor> cells() const noexcept override
  {
    cell_descriptor cell;
    cell.cell_id         = 0;
    cell.pci             = 2;
    cell.plmn            = "00101";
    cell.tac             = 7;
    cell.band            = 78;
    cell.dl_arfcn        = 627340;
    cell.ul_arfcn        = 627340;
    cell.dl_center_hz    = 3410100000ULL;
    cell.ul_center_hz    = 3410100000ULL;
    cell.bandwidth_mhz   = 20;
    cell.nof_prbs        = 51;
    cell.scs_khz         = 30;
    cell.nof_tx_antennas = 1;
    cell.nof_rx_antennas = 1;
    cell.tdd             = cell_tdd_pattern{10, 7, 6, 2, 4};
    cell.prach           = cell_prach_config{1, 1, 0, 64, 4};
    cell.rx_gain_db      = 60.0;
    return {cell};
  }
  ran_control_capabilities capabilities() const noexcept override { return {true, true, true, true}; }

  bool     accept{true};
  unsigned requests{};
  unsigned handovers{};
  unsigned gain_sets{};
  uint16_t last_pci{};
  uint16_t last_rnti{};
  uint16_t last_target{};
  uint8_t  last_port{};
  double   last_gain{};
};

} // namespace

TEST(dapp_e3_aper_codec, matches_external_aper_inventory_vector)
{
  inventory_query query;
  query.header.transaction_id         = test_id(0);
  const std::vector<uint8_t> expected = {
      0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x00};
  EXPECT_EQ(encode_request(query), expected);

  auto decoded = aper_codec::decode_request(expected);
  ASSERT_TRUE(decoded) << decoded.detail;
  const auto& roundtrip = std::get<inventory_query>(*decoded.value);
  EXPECT_EQ(roundtrip.header.transaction_id, query.header.transaction_id);
  EXPECT_TRUE(roundtrip.include_packages);
  EXPECT_TRUE(roundtrip.include_instances);
}

TEST(dapp_e3_aper_codec, roundtrips_typed_mutation_requests)
{
  config_set config;
  config.header.transaction_id               = test_id(1);
  config.header.instance_id                  = test_id(2);
  config.header.expected_instance_generation = 7;
  config.header.dry_run                      = true;
  config.expected_config_generation          = 9;
  config.boundary                            = OCUDU_DAPP_CONFIG_BOUNDARY_SLOT_V1;
  config.entries                             = {{"enabled", true},
                                                {"offset", int64_t{-17}},
                                                {"limit", uint64_t{42}},
                                                {"gain", 1.25},
                                                {"model", std::string("small")},
                                                {"digest", std::vector<uint8_t>{1, 2, 3, 4}}};

  auto              encoded = encode_request(config);
  const std::string golden =
      "340102030405060708090a0b0c0d0e0f1002030405060708090a0b0c0d0e0f101100000000000000078c000000000000"
      "000940000606656e61626c656408286f666673657410ffffffffffffffef046c696d697420000000000000002a03676169"
      "6e300380fe05046d6f64656c400005736d616c6c05646967657374500401020304";
  EXPECT_EQ(to_hex(encoded), golden);
  auto decoded = aper_codec::decode_request(encoded);
  ASSERT_TRUE(decoded) << decoded.detail;
  const auto& roundtrip = std::get<config_set>(*decoded.value);
  EXPECT_EQ(roundtrip.header.transaction_id, config.header.transaction_id);
  EXPECT_EQ(roundtrip.header.instance_id, config.header.instance_id);
  EXPECT_EQ(roundtrip.header.expected_instance_generation, 7U);
  EXPECT_TRUE(roundtrip.header.dry_run);
  EXPECT_EQ(roundtrip.expected_config_generation, 9U);
  EXPECT_EQ(roundtrip.boundary, OCUDU_DAPP_CONFIG_BOUNDARY_SLOT_V1);
  ASSERT_EQ(roundtrip.entries.size(), config.entries.size());
  for (std::size_t index = 0; index != config.entries.size(); ++index) {
    EXPECT_EQ(roundtrip.entries[index].key, config.entries[index].key);
    EXPECT_EQ(roundtrip.entries[index].entry_value, config.entries[index].entry_value);
  }

  model_control model;
  model.header.transaction_id               = test_id(3);
  model.header.instance_id                  = test_id(4);
  model.header.expected_instance_generation = 11;
  model.expected_model_generation           = 12;
  model.action                              = OCUDU_DAPP_MODEL_STAGE_V1;
  model.model_id                            = "receiver";
  model.model_version                       = "2.1";
  model.artifact                            = {5, 6, 7};
  decoded                                   = aper_codec::decode_request(encode_request(model));
  ASSERT_TRUE(decoded) << decoded.detail;
  const auto& decoded_model = std::get<model_control>(*decoded.value);
  EXPECT_EQ(decoded_model.model_id, model.model_id);
  EXPECT_EQ(decoded_model.model_version, model.model_version);
  EXPECT_EQ(decoded_model.artifact, model.artifact);
  EXPECT_EQ(decoded_model.expected_model_generation, 12U);
}

TEST(dapp_e3_aper_codec, roundtrips_and_dispatches_bounded_ue_release)
{
  ran_control request;
  request.header.transaction_id = test_id(140);
  request.serving_pci           = 1;
  request.rnti                  = 0x4601;
  auto decoded                  = aper_codec::decode_request(encode_request(request));
  ASSERT_TRUE(decoded) << decoded.detail;
  const auto& roundtrip = std::get<ran_control>(*decoded.value);
  EXPECT_EQ(roundtrip.serving_pci, 1U);
  EXPECT_EQ(roundtrip.rnti, 0x4601U);

  auto               manager = make_manager();
  embedded_agent     agent(*manager, 4, 16);
  auto               authority = std::make_shared<test_ran_control_handler>();
  management_service service(agent, {}, {}, {}, authority);

  auto response = service.handle(roundtrip);
  EXPECT_EQ(response.code, result_code::accepted);
  ASSERT_TRUE(response.result);
  EXPECT_EQ(response.result->code, result_code::accepted);
  EXPECT_EQ(authority->requests, 1U);
  EXPECT_EQ(authority->last_pci, 1U);
  EXPECT_EQ(authority->last_rnti, 0x4601U);

  request.header.dry_run = true;
  response               = service.handle(request);
  EXPECT_EQ(response.code, result_code::dry_run_ok);
  EXPECT_EQ(authority->requests, 1U) << "dry-run must not dispatch the mutation";

  authority->accept      = false;
  request.header.dry_run = false;
  response               = service.handle(request);
  EXPECT_EQ(response.code, result_code::resource_exhausted);
}

TEST(dapp_e3_aper_codec, roundtrips_full_width_integer_values_without_target_long_arithmetic)
{
  config_set config;
  config.header.transaction_id      = test_id(80);
  config.header.instance_id         = test_id(81);
  config.expected_config_generation = std::numeric_limits<uint64_t>::max();
  config.entries                    = {{"minimum", std::numeric_limits<int64_t>::min()},
                                       {"maximum", std::numeric_limits<int64_t>::max()},
                                       {"unsigned", std::numeric_limits<uint64_t>::max()}};

  auto decoded = aper_codec::decode_request(encode_request(config));
  ASSERT_TRUE(decoded) << decoded.detail;
  const auto& roundtrip = std::get<config_set>(*decoded.value);
  EXPECT_EQ(roundtrip.expected_config_generation, std::numeric_limits<uint64_t>::max());
  ASSERT_EQ(roundtrip.entries.size(), config.entries.size());
  for (std::size_t index = 0; index != config.entries.size(); ++index) {
    EXPECT_EQ(roundtrip.entries[index].entry_value, config.entries[index].entry_value);
  }
}

TEST(dapp_e3_aper_codec, roundtrips_portable_data_subscription_and_chunk_payloads)
{
  data_subscription_control subscribe;
  subscribe.header.transaction_id = test_id(90);
  subscribe.action                = data_subscription_action::create;
  subscribe.subscription_id       = test_id(91);
  subscribe.interface_id          = OCUDU_DAPP_INTERFACE_SPECTRUM_V1;
  subscribe.cell_id               = 3;
  subscribe.sample_every_n        = 4;
  subscribe.queue_depth_chunks    = 17;
  subscribe.maximum_chunk_bytes   = 4096;
  subscribe.lease_us              = 500'000;
  subscribe.drop_policy           = portable_drop_policy::drop_oldest;

  auto decoded = aper_codec::decode_request(encode_request(subscribe));
  ASSERT_TRUE(decoded) << decoded.detail;
  const auto& request = std::get<data_subscription_control>(*decoded.value);
  EXPECT_EQ(request.header.transaction_id, subscribe.header.transaction_id);
  EXPECT_EQ(request.action, data_subscription_action::create);
  EXPECT_EQ(request.subscription_id, subscribe.subscription_id);
  EXPECT_EQ(request.interface_id, subscribe.interface_id);
  EXPECT_EQ(request.cell_id, subscribe.cell_id);
  EXPECT_EQ(request.sample_every_n, 4U);
  EXPECT_EQ(request.queue_depth_chunks, 17U);
  EXPECT_EQ(request.maximum_chunk_bytes, 4096U);
  EXPECT_EQ(request.lease_us, 500'000U);
  EXPECT_EQ(request.drop_policy, portable_drop_policy::drop_oldest);

  data_poll_query poll;
  poll.header.transaction_id = test_id(92);
  poll.subscription_id       = test_id(91);
  poll.maximum_chunks        = 7;
  decoded                    = aper_codec::decode_request(encode_request(poll));
  ASSERT_TRUE(decoded) << decoded.detail;
  const auto& decoded_poll = std::get<data_poll_query>(*decoded.value);
  EXPECT_EQ(decoded_poll.subscription_id, poll.subscription_id);
  EXPECT_EQ(decoded_poll.maximum_chunks, 7U);

  portable_subscription_record subscription;
  subscription.request.subscription_id     = test_id(91);
  subscription.request.interface_id        = OCUDU_DAPP_INTERFACE_SPECTRUM_V1;
  subscription.request.cell_id             = 3;
  subscription.request.sample_every_n      = 4;
  subscription.request.queue_depth_chunks  = 17;
  subscription.request.maximum_chunk_bytes = 4096;
  subscription.request.lease_us            = 500'000;
  subscription.request.drop_policy         = portable_drop_policy::drop_oldest;
  subscription.lease_expiry_ns             = std::numeric_limits<uint64_t>::max();
  subscription.queued_chunks               = 2;
  subscription.published_objects           = 9;
  subscription.published_tensor_bytes      = std::numeric_limits<uint64_t>::max();
  subscription.dropped_objects             = 3;
  subscription.busy_drops                  = 2;
  subscription.sampled_objects             = 11;
  subscription.active                      = true;

  portable_publication_chunk chunk;
  chunk.subscription_id      = subscription.request.subscription_id;
  chunk.interface_id         = subscription.request.interface_id;
  chunk.cell_id              = 3;
  chunk.publication_sequence = std::numeric_limits<uint64_t>::max();
  chunk.route_epoch          = 17;
  chunk.chunk_index          = 0;
  chunk.chunk_count          = 2;
  chunk.tensor_size          = 6144;
  chunk.dropped_objects      = 3;
  chunk.metadata             = {1, 2, 3, 4};
  chunk.tensor_chunk         = std::vector<uint8_t>(4096, 0x5a);

  management_response response;
  response.transaction_id = poll.header.transaction_id;
  response.code           = result_code::ok;
  response.payload        = data_publication_result{{chunk}, subscription};
  auto decoded_response   = aper_codec::decode_response(encode_response(response));
  ASSERT_TRUE(decoded_response) << decoded_response.detail;
  const auto& publications = std::get<data_publication_result>(decoded_response.value->payload);
  ASSERT_EQ(publications.chunks.size(), 1U);
  EXPECT_EQ(publications.chunks[0].subscription_id, chunk.subscription_id);
  EXPECT_EQ(publications.chunks[0].publication_sequence, chunk.publication_sequence);
  EXPECT_EQ(publications.chunks[0].metadata, chunk.metadata);
  EXPECT_EQ(publications.chunks[0].tensor_chunk, chunk.tensor_chunk);
  ASSERT_TRUE(publications.subscription);
  EXPECT_EQ(publications.subscription->published_tensor_bytes, std::numeric_limits<uint64_t>::max());
  EXPECT_EQ(publications.subscription->request.drop_policy, portable_drop_policy::drop_oldest);

  response.payload = data_subscription_result{{subscription}};
  decoded_response = aper_codec::decode_response(encode_response(response));
  ASSERT_TRUE(decoded_response) << decoded_response.detail;
  const auto& subscriptions = std::get<data_subscription_result>(decoded_response.value->payload);
  ASSERT_EQ(subscriptions.subscriptions.size(), 1U);
  EXPECT_EQ(subscriptions.subscriptions[0].request.subscription_id, subscription.request.subscription_id);
  EXPECT_TRUE(subscriptions.subscriptions[0].active);
}

TEST(dapp_e3_aper_codec, roundtrips_inventory_and_management_payloads)
{
  inventory_snapshot inventory;
  inventory.generation = 15;
  package_record package;
  package.package_id     = test_id(10);
  package.global_name    = "org.deepsig.reference";
  package.vendor         = "DeepSig";
  package.version        = "1.0";
  package.placement_mask = OCUDU_DAPP_PLACEMENT_IN_PROCESS_V1 | OCUDU_DAPP_PLACEMENT_OUT_OF_PROCESS_V1;
  package.backends       = {backend::cpu, backend::cuda};
  package.interfaces.push_back(
      {100, 1, 0, OCUDU_DAPP_CLASS_C_OPEN_LOOP_V1, package.placement_mask, OCUDU_DAPP_THREADING_PER_WORKER_V1, 0x1234});
  package.interfaces.back().requirement = binding_requirement::optional;
  interface_record class_a{OCUDU_DAPP_INTERFACE_RECEIVER_V1,
                           1,
                           0,
                           OCUDU_DAPP_CLASS_A_INLINE_V1,
                           OCUDU_DAPP_PLACEMENT_IN_PROCESS_V1,
                           OCUDU_DAPP_THREADING_PER_WORKER_V1,
                           OCUDU_DAPP_PUSCH_CAP_RESIDENT_ACCELERATOR_V1};
  class_a.pusch = interface_record::pusch_profile{275, 64, 2, 15, 3, 0};
  package.interfaces.push_back(class_a);
  package.compatible = true;
  std::fill(package.content_digest.begin(), package.content_digest.end(), 0x5a);
  package.signature_status = package_signature_status::verified;
  std::fill(package.signing_key_id.begin(), package.signing_key_id.end(), 0x6b);
  package.signature_sequence      = 17;
  package.trust_policy_epoch      = 9;
  package.manifest_schema_version = 1;
  std::fill(package.sbom_digest.begin(), package.sbom_digest.end(), 0x7c);
  package.sbom_verified = true;
  inventory.packages.push_back(package);

  instance_record instance;
  instance.instance_id             = test_id(30);
  instance.package_id              = package.package_id;
  instance.global_name             = package.global_name;
  instance.vendor                  = package.vendor;
  instance.version                 = package.version;
  instance.placement               = OCUDU_DAPP_PLACEMENT_OUT_OF_PROCESS_V1;
  instance.selected_backend        = backend::cpu;
  instance.state                   = OCUDU_DAPP_STATE_ACTIVE_V1;
  instance.generation              = 22;
  instance.interfaces              = package.interfaces;
  instance.interfaces[0].state     = binding_state::unavailable;
  instance.interfaces[1].state     = binding_state::bound;
  instance.management_capabilities = 7;
  instance.last_incident_id        = 99;
  inventory.instances.push_back(instance);

  management_response response;
  response.transaction_id = test_id(50);
  response.code           = result_code::ok;
  inventory_result inventory_payload;
  inventory_payload.inventory = inventory;
  e3_connection_record connection;
  connection.association_id   = 17;
  connection.opened_ns        = 18;
  connection.peer_address     = "127.0.0.1";
  connection.setup_complete   = true;
  connection.dapp_id          = 3;
  connection.dapp_name        = "dashboard";
  connection.control_requests = 4;
  connection.subscriptions.push_back({5, 1, {1, 2, 5}});
  inventory_payload.connections.push_back(connection);
  response.payload = inventory_payload;
  auto decoded     = aper_codec::decode_response(encode_response(response));
  ASSERT_TRUE(decoded) << decoded.detail;
  const auto& decoded_payload   = std::get<inventory_result>(decoded.value->payload);
  const auto& decoded_inventory = decoded_payload.inventory;
  ASSERT_EQ(decoded_inventory.packages.size(), 1U);
  ASSERT_EQ(decoded_inventory.instances.size(), 1U);
  EXPECT_EQ(decoded_inventory.generation, inventory.generation);
  EXPECT_EQ(decoded_inventory.packages[0].content_digest, package.content_digest);
  EXPECT_EQ(decoded_inventory.packages[0].signature_status, package_signature_status::verified);
  EXPECT_EQ(decoded_inventory.packages[0].signing_key_id, package.signing_key_id);
  EXPECT_EQ(decoded_inventory.packages[0].signature_sequence, 17U);
  EXPECT_EQ(decoded_inventory.packages[0].trust_policy_epoch, 9U);
  EXPECT_EQ(decoded_inventory.packages[0].manifest_schema_version, 1U);
  EXPECT_EQ(decoded_inventory.packages[0].sbom_digest, package.sbom_digest);
  EXPECT_TRUE(decoded_inventory.packages[0].sbom_verified);
  EXPECT_EQ(decoded_inventory.packages[0].interfaces[0].capability_bits, 0x1234U);
  EXPECT_EQ(decoded_inventory.packages[0].interfaces[0].requirement, binding_requirement::optional);
  EXPECT_EQ(decoded_inventory.packages[0].interfaces[0].state, binding_state::unbound);
  ASSERT_TRUE(decoded_inventory.packages[0].interfaces[1].pusch);
  EXPECT_EQ(decoded_inventory.packages[0].interfaces[1].pusch->max_nof_layers, 2U);
  EXPECT_EQ(decoded_inventory.instances[0].state, OCUDU_DAPP_STATE_ACTIVE_V1);
  EXPECT_EQ(decoded_inventory.instances[0].interfaces[0].requirement, binding_requirement::optional);
  EXPECT_EQ(decoded_inventory.instances[0].interfaces[0].state, binding_state::unavailable);
  EXPECT_EQ(decoded_inventory.instances[0].interfaces[1].state, binding_state::bound);
  EXPECT_EQ(decoded_inventory.instances[0].last_incident_id, 99U);
  ASSERT_EQ(decoded_payload.connections.size(), 1U);
  EXPECT_EQ(decoded_payload.connections[0].association_id, 17U);
  EXPECT_EQ(decoded_payload.connections[0].peer_address, "127.0.0.1");
  EXPECT_EQ(decoded_payload.connections[0].dapp_name, "dashboard");
  ASSERT_EQ(decoded_payload.connections[0].subscriptions.size(), 1U);
  EXPECT_EQ(decoded_payload.connections[0].subscriptions[0].telemetry_identifiers, (std::vector<uint16_t>{1, 2, 5}));

  config_result config;
  config.generation = 8;
  config.descriptors.push_back({"threshold", "Decision threshold", 3, OCUDU_DAPP_VALUE_DOUBLE_V1, 0.5, 0.0, 1.0});
  config.entries.push_back({"threshold", 0.75});
  response.payload = config;
  decoded          = aper_codec::decode_response(encode_response(response));
  ASSERT_TRUE(decoded) << decoded.detail;
  const auto& decoded_config = std::get<config_result>(decoded.value->payload);
  EXPECT_EQ(decoded_config.generation, 8U);
  ASSERT_EQ(decoded_config.descriptors.size(), 1U);
  EXPECT_EQ(decoded_config.descriptors[0].flags, 3U);
  EXPECT_EQ(std::get<double>(decoded_config.entries[0].entry_value), 0.75);

  metric_result metrics;
  metrics.descriptors.push_back({"latency", "Invocation latency", "ns", OCUDU_DAPP_VALUE_UINT64_V1, 1});
  metrics.samples.push_back({"latency", uint64_t{123}, 456});
  response.payload = metrics;
  decoded          = aper_codec::decode_response(encode_response(response));
  ASSERT_TRUE(decoded) << decoded.detail;
  const auto& decoded_metrics = std::get<metric_result>(decoded.value->payload);
  EXPECT_EQ(decoded_metrics.samples[0].monotonic_timestamp_ns, 456U);
  EXPECT_EQ(std::get<uint64_t>(decoded_metrics.samples[0].sample_value), 123U);
}

TEST(dapp_e3_aper_codec, roundtrips_incident_health_and_jobs)
{
  management_response response;
  response.transaction_id = test_id(60);
  response.code           = result_code::ok;

  incident_record incident;
  incident.incident_id                 = 7;
  incident.monotonic_ns                = 8;
  incident.context.instance_id[0]      = 9;
  incident.context.package_id[0]       = 10;
  incident.context.app_generation      = 11;
  incident.context.invocation_sequence = 12;
  incident.context.phase               = incident_phase::invoke;
  incident.cause                       = incident_cause::standard_exception;
  incident.action                      = incident_action::disabled;
  incident.native_status               = -4;
  incident.recurrence_count            = 3;
  incident.exception_type_hash         = 13;
  incident.observed_latency_ns         = 14;
  incident.deadline_ns                 = 15;
  incident.fallback_succeeded          = true;
  incident.breaker_open                = true;
  const char* message                  = "fixture failure";
  incident.message_size                = std::strlen(message);
  std::memcpy(incident.sanitized_message.data(), message, incident.message_size);
  auto policy_incident        = incident;
  policy_incident.incident_id = 8;
  policy_incident.cause       = incident_cause::policy_rejection;
  response.payload            = incident_result{{incident, policy_incident}, 4};
  auto decoded                = aper_codec::decode_response(encode_response(response));
  ASSERT_TRUE(decoded) << decoded.detail;
  const auto& incidents = std::get<incident_result>(decoded.value->payload);
  ASSERT_EQ(incidents.incidents.size(), 2U);
  EXPECT_EQ(incidents.cleared_count, 4U);
  EXPECT_EQ(incidents.incidents[0].cause, incident_cause::standard_exception);
  EXPECT_EQ(incidents.incidents[0].message_size, incident.message_size);
  EXPECT_EQ(incidents.incidents[0].context.invocation_sequence, 12U);
  EXPECT_EQ(incidents.incidents[0].observed_latency_ns, 14U);
  EXPECT_TRUE(incidents.incidents[0].fallback_succeeded);
  EXPECT_TRUE(incidents.incidents[0].breaker_open);
  EXPECT_EQ(incidents.incidents[1].cause, incident_cause::policy_rejection);

  incident_health_summary health;
  health.total               = 10;
  health.standard_exceptions = 2;
  health.deadline_misses     = 3;
  health.last_incident_id    = 9;
  response.payload           = health;
  decoded                    = aper_codec::decode_response(encode_response(response));
  ASSERT_TRUE(decoded) << decoded.detail;
  EXPECT_EQ(std::get<incident_health_summary>(decoded.value->payload).deadline_misses, 3U);

  operation_result operation;
  operation.code                = result_code::lifecycle_failed;
  operation.instance_id         = test_id(70);
  operation.instance_generation = 5;
  operation.state               = OCUDU_DAPP_STATE_DEGRADED_V1;
  operation.detail              = "contained failure";
  job_record job;
  job.job_id                 = test_id(80);
  job.transaction_id         = test_id(90);
  job.state                  = job_state::failed;
  job.progress_percent       = 100;
  job.submitted_monotonic_ns = 1;
  job.started_monotonic_ns   = 2;
  job.completed_monotonic_ns = 3;
  job.result                 = operation;
  job.detail                 = "job detail";
  response.payload           = job_result{{job}};
  decoded                    = aper_codec::decode_response(encode_response(response));
  ASSERT_TRUE(decoded) << decoded.detail;
  const auto& jobs = std::get<job_result>(decoded.value->payload).jobs;
  ASSERT_EQ(jobs.size(), 1U);
  ASSERT_TRUE(jobs[0].result);
  EXPECT_EQ(jobs[0].result->state, OCUDU_DAPP_STATE_DEGRADED_V1);
  EXPECT_EQ(jobs[0].result->detail, operation.detail);
  EXPECT_EQ(jobs[0].detail, job.detail);
}

TEST(dapp_e3_aper_codec, roundtrips_bounded_quiet_reservation_query_and_result)
{
  quiet_reservation_query query;
  query.header.transaction_id = test_id(31);
  query.header.instance_id    = test_id(32);
  query.cell_id               = 2;
  query.maximum_records       = 7;
  auto decoded_request        = aper_codec::decode_request(encode_request(query));
  ASSERT_TRUE(decoded_request) << decoded_request.detail;
  const auto& decoded_query = std::get<quiet_reservation_query>(*decoded_request.value);
  EXPECT_EQ(decoded_query.header.instance_id, query.header.instance_id);
  EXPECT_EQ(decoded_query.cell_id, 2U);
  EXPECT_EQ(decoded_query.maximum_records, 7U);

  quiet_reservation_record record;
  record.cell_id                     = 2;
  auto& reservation                  = record.reservation;
  reservation.struct_size            = sizeof(reservation);
  reservation.abi_major              = OCUDU_DAPP_ABI_MAJOR_V1;
  reservation.request_id[0]          = 1;
  reservation.reservation_id[1]      = 2;
  reservation.reservation_generation = 3;
  const auto source                  = test_id(32);
  std::memcpy(reservation.request_source_instance_id, source.bytes.data(), source.bytes.size());
  reservation.request_source_generation = 4;
  reservation.request_source_sequence   = 5;
  reservation.start                     = {6, 7, 1, 0};
  reservation.duration_slots            = 2;
  reservation.repeat_period_slots       = 0;
  reservation.repeat_count              = 1;
  reservation.first_prb                 = 8;
  reservation.nof_prbs                  = 3;
  reservation.achieved_level            = OCUDU_DAPP_QUIET_NO_NEW_PUSCH_V1;
  reservation.status                    = OCUDU_DAPP_RESERVATION_CLAMPED_V1;
  reservation.reason                    = OCUDU_DAPP_QUIET_REASON_CAPABILITY_V1;
  reservation.budget_class              = 1;
  reservation.confidence_q16            = 1U << 15U;
  reservation.expiry_monotonic_ns       = 99;

  management_response response;
  response.transaction_id = query.header.transaction_id;
  response.code           = result_code::ok;
  response.payload        = quiet_reservation_result{{record}, true};
  auto decoded_response   = aper_codec::decode_response(encode_response(response));
  ASSERT_TRUE(decoded_response) << decoded_response.detail;
  const auto& result = std::get<quiet_reservation_result>(decoded_response.value->payload);
  ASSERT_EQ(result.reservations.size(), 1U);
  EXPECT_TRUE(result.truncated);
  EXPECT_EQ(result.reservations[0].cell_id, 2U);
  EXPECT_EQ(result.reservations[0].reservation.request_source_sequence, 5U);
  EXPECT_EQ(result.reservations[0].reservation.start.slot, 7U);
  EXPECT_EQ(result.reservations[0].reservation.status, OCUDU_DAPP_RESERVATION_CLAMPED_V1);
  EXPECT_EQ(result.reservations[0].reservation.reason, OCUDU_DAPP_QUIET_REASON_CAPABILITY_V1);
}

TEST(dapp_e3_aper_codec, dispatches_decoded_requests_through_embedded_authority)
{
  auto               manager = make_manager();
  embedded_agent     agent(*manager, 4, 16);
  management_service service(agent);
  inventory_query    query;
  query.header.transaction_id = test_id(100);

  auto decoded_request = aper_codec::decode_request(encode_request(query));
  ASSERT_TRUE(decoded_request) << decoded_request.detail;
  auto response         = service.handle(*decoded_request.value);
  auto decoded_response = aper_codec::decode_response(encode_response(response));
  ASSERT_TRUE(decoded_response) << decoded_response.detail;
  EXPECT_EQ(decoded_response.value->transaction_id, query.header.transaction_id);
  EXPECT_EQ(decoded_response.value->code, result_code::ok);
  EXPECT_TRUE(std::holds_alternative<inventory_result>(decoded_response.value->payload));
}

TEST(dapp_e3_aper_codec, queries_authoritative_quiet_reservations_through_management_service)
{
  auto                    manager = make_manager();
  embedded_agent          agent(*manager, 4, 16);
  std::array<uint16_t, 2> widths{52, 24};
  auto                    reservations = ocudu::create_quiet_reservation_manager(widths);
  ASSERT_NE(reservations, nullptr);

  const uint64_t now = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
          .count());
  ocudu::quiet_reservation_request request{};
  request.intent.struct_size         = sizeof(request.intent);
  request.intent.abi_major           = OCUDU_DAPP_ABI_MAJOR_V1;
  request.intent.request_id[0]       = 4;
  request.intent.earliest_start      = {10, 8, 0, 0};
  request.intent.latest_start        = request.intent.earliest_start;
  request.intent.duration_slots      = 1;
  request.intent.repeat_period_slots = 0;
  request.intent.repeat_count        = 1;
  request.intent.first_prb           = 3;
  request.intent.nof_prbs            = 2;
  request.intent.confidence_q16      = 1U << 15U;
  request.intent.expiry_monotonic_ns = now + 1'000'000'000;
  const auto source                  = test_id(45);
  std::memcpy(request.source_instance_id.data(), source.bytes.data(), source.bytes.size());
  request.source_generation    = 2;
  request.source_sequence      = 3;
  request.cell_id              = 1;
  request.observed_slot        = ocudu::slot_point{0, 10, 4};
  request.publish_monotonic_ns = now;
  ASSERT_EQ(reservations->submit(request).result, ocudu::quiet_reservation_result::accepted);

  management_service      service(agent, reservations);
  quiet_reservation_query query;
  query.header.transaction_id = test_id(46);
  query.header.instance_id    = source;
  query.cell_id               = 1;
  auto response               = service.handle(query);
  ASSERT_EQ(response.code, result_code::ok);
  const auto& result = std::get<quiet_reservation_result>(response.payload);
  ASSERT_EQ(result.reservations.size(), 1U);
  EXPECT_EQ(result.reservations[0].reservation.request_id[0], 4U);

  query.header.instance_id = test_id(47);
  response                 = service.handle(query);
  EXPECT_TRUE(std::get<quiet_reservation_result>(response.payload).reservations.empty());

  management_service unavailable(agent);
  EXPECT_EQ(unavailable.handle(query).code, result_code::incompatible);
}

TEST(dapp_e3_aper_codec, manages_bounded_portable_data_subscriptions_through_the_shared_service)
{
  auto                      manager = make_manager();
  embedded_agent            agent(*manager, 4, 16);
  data_subscription_control query;
  query.header.transaction_id = test_id(70);
  query.action                = data_subscription_action::list;
  EXPECT_EQ(management_service(agent).handle(query).code, result_code::incompatible);

  portable_data_store_config config;
  config.maximum_subscriptions  = 1;
  config.maximum_retained_bytes = 1024 * 1024;
  config.maximum_object_bytes   = 1024;
  config.maximum_metadata_bytes = 1024;
  config.minimum_chunk_bytes    = 1024;
  config.maximum_chunk_bytes    = 1024;
  auto [store, detail]          = portable_data_store::create(config);
  ASSERT_NE(store, nullptr) << detail;
  management_service service(agent, {}, store);

  query.action              = data_subscription_action::create;
  query.subscription_id     = test_id(71);
  query.interface_id        = OCUDU_DAPP_INTERFACE_SPECTRUM_V1;
  query.queue_depth_chunks  = 1;
  query.maximum_chunk_bytes = 1024;
  auto response             = service.handle(query);
  ASSERT_EQ(response.code, result_code::ok);
  ASSERT_EQ(std::get<data_subscription_result>(response.payload).subscriptions.size(), 1U);

  query.cell_id = 1;
  EXPECT_EQ(service.handle(query).code, result_code::conflict);
  query                       = {};
  query.header.transaction_id = test_id(72);
  query.action                = data_subscription_action::list;
  response                    = service.handle(query);
  ASSERT_EQ(response.code, result_code::ok);
  EXPECT_EQ(std::get<data_subscription_result>(response.payload).subscriptions.size(), 1U);

  data_poll_query poll;
  poll.header.transaction_id = test_id(73);
  poll.subscription_id       = test_id(74);
  EXPECT_EQ(service.handle(poll).code, result_code::not_found);

  query.action              = data_subscription_action::remove;
  query.subscription_id     = test_id(71);
  query.interface_id        = 0;
  query.queue_depth_chunks  = 64;
  query.maximum_chunk_bytes = 64 * 1024;
  response                  = service.handle(query);
  ASSERT_EQ(response.code, result_code::ok);
  EXPECT_FALSE(store->has_active(OCUDU_DAPP_INTERFACE_SPECTRUM_V1));
  EXPECT_EQ(service.handle(query).code, result_code::not_found);
}

TEST(dapp_e3_aper_codec, rejects_truncated_and_semantically_unsupported_messages)
{
  inventory_query query;
  query.header.transaction_id = test_id(110);
  auto encoded                = encode_request(query);
  encoded.pop_back();
  EXPECT_FALSE(aper_codec::decode_request(encoded));

  lifecycle_control remote;
  remote.header.transaction_id = test_id(120);
  remote.action                = ocudu::dapp::e3::lifecycle_action::load;
  remote.package_id            = test_id(121);
  remote.placement             = static_cast<ocudu_dapp_placement_v1>(3);
  remote.selected_backend      = backend::cpu;
  EXPECT_FALSE(aper_codec::encode(remote));
}

TEST(dapp_e3_aper_codec, bounded_mutation_corpus_never_escapes_the_noexcept_decoder_boundary)
{
  inventory_query query;
  query.header.transaction_id = test_id(130);
  const auto seed             = encode_request(query);
  for (std::size_t index = 0; index != seed.size(); ++index) {
    for (uint8_t bit = 0; bit != 8; ++bit) {
      auto mutation = seed;
      mutation[index] ^= static_cast<uint8_t>(1U << bit);
      static_cast<void>(aper_codec::decode_request(mutation));
      static_cast<void>(aper_codec::decode_response(mutation));
    }
  }

  std::mt19937 random(0x0c0d0a99U);
  for (std::size_t size = 0; size != 512; ++size) {
    std::vector<uint8_t> bytes(size);
    for (auto& value : bytes) {
      value = static_cast<uint8_t>(random());
    }
    static_cast<void>(aper_codec::decode_request(bytes));
    static_cast<void>(aper_codec::decode_response(bytes));
  }
}

TEST(dapp_e3_aper_codec, roundtrips_cell_info_and_dispatches_handover_and_gain_controls)
{
  auto               manager   = make_manager();
  embedded_agent     agent(*manager, 4, 16);
  auto               authority = std::make_shared<test_ran_control_handler>();
  management_service service(agent, {}, {}, {}, authority);

  // Cell info: request and payload round trip through APER.
  cell_info_query query;
  query.header.transaction_id = test_id(21);
  auto decoded                = aper_codec::decode_request(encode_request(query));
  ASSERT_TRUE(decoded) << decoded.detail;
  ASSERT_TRUE(std::holds_alternative<cell_info_query>(*decoded.value));
  auto response = service.handle(*decoded.value);
  EXPECT_EQ(response.code, result_code::ok);
  auto encoded_response = aper_codec::encode(response);
  ASSERT_TRUE(encoded_response) << encoded_response.detail;
  auto decoded_response = aper_codec::decode_response(*encoded_response.value);
  ASSERT_TRUE(decoded_response) << decoded_response.detail;
  const auto* info = std::get_if<cell_info_result>(&decoded_response.value->payload);
  ASSERT_NE(info, nullptr);
  ASSERT_EQ(info->cells.size(), 1U);
  EXPECT_EQ(info->cells[0].pci, 2U);
  EXPECT_EQ(info->cells[0].plmn, "00101");
  EXPECT_EQ(info->cells[0].band, 78U);
  EXPECT_EQ(info->cells[0].dl_center_hz, 3410100000ULL);
  ASSERT_TRUE(info->cells[0].tdd);
  EXPECT_EQ(info->cells[0].tdd->dl_slots, 7U);
  ASSERT_TRUE(info->cells[0].prach);
  EXPECT_EQ(info->cells[0].prach->total_preambles, 64U);
  ASSERT_TRUE(info->cells[0].rx_gain_db);
  EXPECT_DOUBLE_EQ(*info->cells[0].rx_gain_db, 60.0);
  EXPECT_FALSE(info->cells[0].tx_gain_db);
  EXPECT_TRUE(info->capabilities.handover);

  // Handover: extension fields survive APER and reach the authority.
  ran_control handover;
  handover.header.transaction_id = test_id(22);
  handover.action                = ran_control_action::handover_ue;
  handover.serving_pci           = 2;
  handover.rnti                  = 0x4601;
  handover.target_pci            = 5;
  decoded                        = aper_codec::decode_request(encode_request(handover));
  ASSERT_TRUE(decoded) << decoded.detail;
  response = service.handle(*decoded.value);
  EXPECT_EQ(response.code, result_code::accepted) << response.error_text;
  EXPECT_EQ(authority->handovers, 1U);
  EXPECT_EQ(authority->last_target, 5U);

  // Handover to the serving cell is rejected before reaching the authority.
  handover.target_pci = 2;
  response            = service.handle(handover);
  EXPECT_EQ(response.code, result_code::invalid_request);
  EXPECT_EQ(authority->handovers, 1U);

  // Gain control: rnti 0, port and gain carried; applied (not deferred) and reported as such.
  ran_control gain;
  gain.header.transaction_id = test_id(23);
  gain.action                = ran_control_action::set_rx_gain;
  gain.port_id               = 1;
  gain.gain_db               = 55.5;
  decoded                    = aper_codec::decode_request(encode_request(gain));
  ASSERT_TRUE(decoded) << decoded.detail;
  const auto& gain_back = std::get<ran_control>(*decoded.value);
  ASSERT_TRUE(gain_back.gain_db);
  EXPECT_DOUBLE_EQ(*gain_back.gain_db, 55.5);
  ASSERT_TRUE(gain_back.port_id);
  EXPECT_EQ(*gain_back.port_id, 1U);
  response = service.handle(gain_back);
  EXPECT_EQ(response.code, result_code::accepted) << response.error_text;
  EXPECT_EQ(authority->gain_sets, 1U);
  EXPECT_DOUBLE_EQ(authority->last_gain, 55.5);

  gain.gain_db = 500.0;
  response     = service.handle(gain);
  EXPECT_EQ(response.code, result_code::invalid_request);
  gain.gain_db        = 40.0;
  gain.header.dry_run = true;
  response            = service.handle(gain);
  EXPECT_EQ(response.code, result_code::dry_run_ok);
  EXPECT_EQ(authority->gain_sets, 1U) << "dry-run must not touch the radio";
}
