// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/e3/aper_codec.h"
#include "ocudu/dapp/e3/e3_connection_registry.h"
#include "ocudu/dapp/e3/e3ap_codec.h"
#include "ocudu/dapp/e3/e3ap_sctp_server.h"
#include "ocudu/dapp/e3/embedded_agent.h"
#include "ocudu/dapp/e3/management_service.h"
#include "ocudu/dapp/e3/telemetry_publisher.h"
#include "ocudu/dapp/runtime/instance_manager.h"
#include <arpa/inet.h>
#include <chrono>
#include <complex>
#include <cstring>
#include <gtest/gtest.h>
#include <map>
#include <netinet/in.h>
#include <netinet/sctp.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

using namespace ocudu;
using namespace ocudu::dapp;
using namespace ocudu::dapp::e3;

namespace {

/// Minimal blocking E3AP client over a one-to-one SCTP association.
class test_client
{
public:
  explicit test_client(uint16_t port)
  {
    descriptor = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_SCTP);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port   = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    connected = descriptor >= 0 && ::connect(descriptor, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
    timeval timeout{2, 0};
    ::setsockopt(descriptor, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  }
  ~test_client()
  {
    if (descriptor >= 0) {
      ::close(descriptor);
    }
  }

  bool ok() const { return connected; }

  bool send(const e3ap_pdu& pdu)
  {
    auto encoded = e3ap_codec::encode(pdu);
    if (!encoded) {
      return false;
    }
    return ::send(descriptor, encoded.value->data(), encoded.value->size(), MSG_NOSIGNAL) ==
           static_cast<ssize_t>(encoded.value->size());
  }

  std::optional<e3ap_pdu> receive()
  {
    std::vector<uint8_t> message;
    std::vector<uint8_t> chunk(64 * 1024);
    while (true) {
      iovec  vector{chunk.data(), chunk.size()};
      msghdr header{};
      header.msg_iov         = &vector;
      header.msg_iovlen      = 1;
      const ssize_t received = ::recvmsg(descriptor, &header, 0);
      if (received <= 0) {
        return std::nullopt;
      }
      if ((header.msg_flags & MSG_NOTIFICATION) != 0) {
        continue;
      }
      message.insert(message.end(), chunk.begin(), chunk.begin() + received);
      if ((header.msg_flags & MSG_EOR) != 0) {
        break;
      }
    }
    auto decoded = e3ap_codec::decode(message);
    if (!decoded) {
      ADD_FAILURE() << decoded.detail;
      return std::nullopt;
    }
    return *decoded.value;
  }

  template <typename T>
  std::optional<T> receive_as()
  {
    while (auto pdu = receive()) {
      if (auto* body = std::get_if<T>(&pdu->message)) {
        return *body;
      }
    }
    return std::nullopt;
  }

  /// Completes setup and returns the assigned dApp identifier.
  std::optional<uint16_t> setup()
  {
    e3ap_pdu pdu;
    pdu.id = 1;
    e3ap_setup_request request;
    request.dapp_name    = "test-dapp";
    request.dapp_version = "0.0.1";
    request.vendor       = "DeepSig";
    pdu.message          = request;
    if (!send(pdu)) {
      return std::nullopt;
    }
    auto response = receive_as<e3ap_setup_response>();
    if (!response || response->response_code != e3ap_response_code::positive) {
      return std::nullopt;
    }
    last_setup = *response;
    return response->dapp_identifier;
  }

  e3ap_setup_response last_setup;

private:
  int  descriptor{-1};
  bool connected{};
};

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

std::shared_ptr<portable_data_store> make_store()
{
  portable_data_store_config config;
  config.maximum_subscriptions  = 16;
  config.maximum_retained_bytes = 64 * 1024 * 1024;
  config.maximum_object_bytes   = 4 * 1024 * 1024;
  config.maximum_metadata_bytes = 64 * 1024;
  config.minimum_chunk_bytes    = 4 * 1024;
  config.maximum_chunk_bytes    = 64 * 1024;
  config.minimum_lease_us       = 100'000;
  config.maximum_lease_us       = 10'000'000;
  auto created                  = portable_data_store::create(config);
  EXPECT_TRUE(created.first) << created.second;
  return created.first;
}

e3ap_sctp_server_config test_server_config()
{
  e3ap_sctp_server_config config;
  config.transport.port         = 0;
  config.transport.idle_timeout = std::chrono::seconds(5);
  config.transport.timer_period = std::chrono::milliseconds(2);
  config.setup_timeout          = std::chrono::milliseconds(500);
  config.lease_us               = 2'000'000;
  config.maximum_payload_bytes  = 4 * 1024 * 1024;
  return config;
}

struct fixture {
  fixture() :
    manager(make_manager()),
    agent(*manager, 8, 32),
    store(make_store()),
    connections(std::make_shared<e3_connection_registry>()),
    service(agent, {}, store, connections)
  {
    telemetry_publisher_config publisher_config;
    publisher_config.store                          = store;
    publisher_config.pusch_symbols_minimum_interval = std::chrono::microseconds(0);
    publisher_config.pusch_crc_minimum_interval     = std::chrono::microseconds(0);
    auto created                                    = telemetry_publisher::create(publisher_config);
    publisher                                       = created.first;
  }

  std::unique_ptr<instance_manager>       manager;
  embedded_agent                          agent;
  std::shared_ptr<portable_data_store>    store;
  std::shared_ptr<e3_connection_registry> connections;
  management_service                      service;
  std::shared_ptr<telemetry_publisher>    publisher;
};

template <typename Predicate>
bool wait_until(Predicate&& ready, std::chrono::milliseconds limit = std::chrono::seconds(2))
{
  const auto deadline = std::chrono::steady_clock::now() + limit;
  while (std::chrono::steady_clock::now() < deadline) {
    if (ready()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return ready();
}

} // namespace

TEST(dapp_e3ap_sctp_server, completes_setup_and_advertises_ran_functions)
{
  fixture f;
  auto    server = e3ap_sctp_server::create(&f.service, f.store, test_server_config(), f.connections);
  ASSERT_TRUE(server) << server.detail;
  ASSERT_NE(server.server->port(), 0U);

  test_client client(server.server->port());
  ASSERT_TRUE(client.ok());
  auto dapp_id = client.setup();
  ASSERT_TRUE(dapp_id);
  EXPECT_EQ(client.last_setup.ran_identifier, "ocudu-gnb");
  ASSERT_EQ(client.last_setup.ran_functions.size(), 2U);
  const auto& telemetry = client.last_setup.ran_functions[0];
  EXPECT_EQ(telemetry.ran_function_identifier, e3ap_ran_function_telemetry);
  EXPECT_EQ(telemetry.telemetry_identifiers.size(), 8U);
  auto catalog = e3ap_codec::decode_telemetry_function(telemetry.ran_function_data);
  ASSERT_TRUE(catalog) << catalog.detail;
  EXPECT_EQ(catalog.value->streams.size(), 8U);
  const auto& management = client.last_setup.ran_functions[1];
  EXPECT_EQ(management.ran_function_identifier, e3ap_ran_function_management);
  EXPECT_EQ(management.control_identifiers, std::vector<uint16_t>{1});
  auto management_data = e3ap_codec::decode_management_function(management.ran_function_data);
  ASSERT_TRUE(management_data) << management_data.detail;
  EXPECT_EQ(management_data.value->schema_module, "E3SM-OCUDU-DAPP-MGMT-V1");

  // A second setup on the same association is refused without closing it.
  e3ap_pdu again;
  again.id = 2;
  e3ap_setup_request request;
  request.dapp_name    = "test-dapp";
  request.dapp_version = "0.0.1";
  request.vendor       = "DeepSig";
  again.message        = request;
  ASSERT_TRUE(client.send(again));
  auto refused = client.receive_as<e3ap_setup_response>();
  ASSERT_TRUE(refused);
  EXPECT_EQ(refused->response_code, e3ap_response_code::negative);
  ASSERT_TRUE(refused->cause);
  EXPECT_EQ(*refused->cause, e3ap_cause::setup_already_complete);
  EXPECT_TRUE(wait_until([&] { return server.server->metrics().setups_rejected == 1; }));
}

TEST(dapp_e3ap_sctp_server, streams_fragmented_telemetry_indications_to_subscribers)
{
  fixture f;
  auto    server = e3ap_sctp_server::create(&f.service, f.store, test_server_config(), f.connections);
  ASSERT_TRUE(server) << server.detail;
  test_client client(server.server->port());
  ASSERT_TRUE(client.ok());
  auto dapp_id = client.setup();
  ASSERT_TRUE(dapp_id);

  telemetry_subscription_data   data;
  telemetry_subscription_config config;
  config.telemetry_id = telemetry_id_pusch_symbols;
  config.rnti_filter  = {0x4601};
  data.configs.push_back(config);
  auto data_bytes = e3ap_codec::encode(data);
  ASSERT_TRUE(data_bytes) << data_bytes.detail;

  e3ap_pdu pdu;
  pdu.id = 5;
  e3ap_subscription_request subscription;
  subscription.dapp_identifier         = *dapp_id;
  subscription.ran_function_identifier = e3ap_ran_function_telemetry;
  subscription.telemetry_identifiers   = {telemetry_id_pusch_symbols, telemetry_id_pusch_crc};
  subscription.subscription_data       = *data_bytes.value;
  pdu.message                          = subscription;
  ASSERT_TRUE(client.send(pdu));
  auto response = client.receive_as<e3ap_subscription_response>();
  ASSERT_TRUE(response);
  EXPECT_EQ(response->request_id, 5U);
  ASSERT_EQ(response->response_code, e3ap_response_code::positive) << response->detail;
  ASSERT_TRUE(response->subscription_id);
  EXPECT_TRUE(wait_until([&] { return f.store->has_active(telemetry_stream_pusch_symbols); }));
  ASSERT_TRUE(wait_until([&] {
    const auto connections = f.connections->snapshot();
    return connections.size() == 1 && connections[0].subscriptions.size() == 1;
  }));
  const auto connection_snapshot = f.connections->snapshot();
  EXPECT_EQ(connection_snapshot[0].dapp_name, "test-dapp");
  EXPECT_EQ(connection_snapshot[0].subscriptions[0].telemetry_identifiers,
            (std::vector<uint16_t>{telemetry_id_pusch_symbols, telemetry_id_pusch_crc}));

  // A large PUSCH symbol vector must arrive as several fragments; a filtered
  // RNTI and a CRC record are published alongside it.
  std::vector<std::complex<float>> symbols(273 * 12 * 13);
  for (std::size_t index = 0; index != symbols.size(); ++index) {
    symbols[index] = {static_cast<float>(index % 97), 1.0F};
  }
  pusch_symbols_diagnostic event;
  event.rnti              = 0x4601;
  event.slot              = slot_point{1, 20, 3};
  event.nof_prb           = 273;
  event.nof_data_symbols  = 13;
  event.nof_layers        = 1;
  event.mod_bits          = 8;
  event.sinr_dB           = 25.0F;
  event.equalized_symbols = symbols;
  ASSERT_TRUE(f.publisher->should_collect(phy_diagnostic_stream::pusch_symbols, 0x4601));
  f.publisher->on_pusch_symbols(event);
  event.rnti = 0x4602; // filtered out by the subscription config
  ASSERT_TRUE(f.publisher->should_collect(phy_diagnostic_stream::pusch_symbols, 0x4602));
  f.publisher->on_pusch_symbols(event);
  ASSERT_TRUE(f.publisher->should_collect(phy_diagnostic_stream::pusch_crc, 0x4601));
  f.publisher->on_pusch_crc({0x4601, slot_point{1, 20, 3}, true});

  struct assembly {
    std::vector<uint8_t> bytes;
    uint32_t             next{};
    uint32_t             count{};
    bool                 complete{};
  };
  std::map<uint16_t, assembly>             assemblies;
  std::map<uint16_t, telemetry_indication> decoded_by_id;
  while (decoded_by_id.size() < 2) {
    auto indication = client.receive_as<e3ap_indication_message>();
    ASSERT_TRUE(indication) << "indication stream stalled";
    EXPECT_EQ(indication->dapp_identifier, *dapp_id);
    EXPECT_EQ(indication->ran_function_identifier, e3ap_ran_function_telemetry);
    ASSERT_TRUE(indication->telemetry_identifier);
    ASSERT_TRUE(indication->subscription_id);
    EXPECT_EQ(*indication->subscription_id, *response->subscription_id);
    auto& item = assemblies[*indication->telemetry_identifier];
    ASSERT_EQ(indication->fragment_index, item.next);
    item.count = indication->fragment_count;
    item.bytes.insert(item.bytes.end(), indication->protocol_data.begin(), indication->protocol_data.end());
    ++item.next;
    if (item.next != item.count) {
      continue;
    }
    ASSERT_TRUE(indication->payload_bytes);
    EXPECT_EQ(item.bytes.size(), *indication->payload_bytes);
    auto decoded = e3ap_codec::decode_telemetry(item.bytes);
    ASSERT_TRUE(decoded) << decoded.detail;
    EXPECT_EQ(decoded.value->telemetry_id, *indication->telemetry_identifier);
    decoded_by_id[decoded.value->telemetry_id] = *decoded.value;
    assemblies.erase(*indication->telemetry_identifier);
  }
  ASSERT_EQ(decoded_by_id.count(telemetry_id_pusch_symbols), 1U);
  ASSERT_EQ(decoded_by_id.count(telemetry_id_pusch_crc), 1U);
  const auto& record = std::get<pusch_symbols_record>(decoded_by_id[telemetry_id_pusch_symbols].object);
  EXPECT_EQ(record.rnti, 0x4601) << "the RNTI filter must drop 0x4602";
  EXPECT_EQ(record.nof_prb, 273U);
  EXPECT_EQ(record.equalized_symbols.size(), symbols.size() * sizeof(std::complex<float>));
  EXPECT_EQ(std::memcmp(record.equalized_symbols.data(), symbols.data(), record.equalized_symbols.size()), 0);
  EXPECT_TRUE(std::get<pusch_crc_record>(decoded_by_id[telemetry_id_pusch_crc].object).crc_ok);
  EXPECT_TRUE(wait_until([&] { return server.server->metrics().indications_filtered == 1; }));
  EXPECT_EQ(server.server->metrics().indications_sent, 2U);

  // Deleting the subscription releases the store slots and stops the stream.
  e3ap_pdu remove;
  remove.id      = 6;
  remove.message = e3ap_subscription_delete{*dapp_id, *response->subscription_id};
  ASSERT_TRUE(client.send(remove));
  auto ack = client.receive_as<e3ap_message_ack>();
  ASSERT_TRUE(ack);
  EXPECT_EQ(ack->request_id, 6U);
  EXPECT_EQ(ack->response_code, e3ap_response_code::positive);
  EXPECT_TRUE(wait_until([&] { return !f.store->has_active(telemetry_stream_pusch_symbols); }));
  EXPECT_EQ(server.server->metrics().live_subscriptions, 0U);
}

TEST(dapp_e3ap_sctp_server, carries_management_requests_as_control_actions)
{
  fixture f;
  auto    server = e3ap_sctp_server::create(&f.service, f.store, test_server_config(), f.connections);
  ASSERT_TRUE(server) << server.detail;
  test_client client(server.server->port());
  ASSERT_TRUE(client.ok());
  auto dapp_id = client.setup();
  ASSERT_TRUE(dapp_id);

  inventory_query query;
  query.header.transaction_id.bytes[15] = 0x42;
  auto request_bytes                    = aper_codec::encode(management_request{query});
  ASSERT_TRUE(request_bytes) << request_bytes.detail;

  e3ap_pdu pdu;
  pdu.id = 11;
  e3ap_dapp_control_action action;
  action.dapp_identifier         = *dapp_id;
  action.ran_function_identifier = e3ap_ran_function_management;
  action.control_identifier      = e3ap_management_control_request;
  action.action_data             = *request_bytes.value;
  action.payload_bytes           = static_cast<uint32_t>(request_bytes.value->size());
  pdu.message                    = action;
  ASSERT_TRUE(client.send(pdu));

  auto ack = client.receive_as<e3ap_message_ack>();
  ASSERT_TRUE(ack);
  EXPECT_EQ(ack->request_id, 11U);
  EXPECT_EQ(ack->response_code, e3ap_response_code::positive) << ack->detail;
  auto outcome = client.receive_as<e3ap_control_response>();
  ASSERT_TRUE(outcome);
  EXPECT_EQ(outcome->request_id, 11U);
  EXPECT_EQ(outcome->response_code, e3ap_response_code::positive) << outcome->detail;
  EXPECT_EQ(outcome->fragment_count, 1U);
  EXPECT_EQ(outcome->payload_bytes, outcome->control_outcome.size());
  auto response = aper_codec::decode_response(outcome->control_outcome);
  ASSERT_TRUE(response) << response.detail;
  EXPECT_EQ(response.value->transaction_id, query.header.transaction_id);
  EXPECT_EQ(response.value->code, management::result_code::ok);
  EXPECT_TRUE(std::holds_alternative<inventory_result>(response.value->payload));
  const auto& inventory = std::get<inventory_result>(response.value->payload);
  ASSERT_EQ(inventory.connections.size(), 1U);
  EXPECT_TRUE(inventory.connections[0].setup_complete);
  EXPECT_EQ(inventory.connections[0].dapp_name, "test-dapp");
  EXPECT_EQ(inventory.connections[0].control_requests, 1U);

  // Garbage in actionData is refused with a negative ack and no outcome.
  pdu.id             = 12;
  action.action_data = {0xff, 0xee, 0xdd};
  action.payload_bytes.reset();
  pdu.message = action;
  ASSERT_TRUE(client.send(pdu));
  ack = client.receive_as<e3ap_message_ack>();
  ASSERT_TRUE(ack);
  EXPECT_EQ(ack->request_id, 12U);
  EXPECT_EQ(ack->response_code, e3ap_response_code::negative);
  ASSERT_TRUE(ack->cause);
  EXPECT_EQ(*ack->cause, e3ap_cause::invalid_request);
  EXPECT_TRUE(wait_until([&] { return server.server->metrics().control_failures == 1; }));
}

TEST(dapp_e3ap_sctp_server, enforces_setup_before_procedures_and_releases_cleanly)
{
  fixture f;
  auto    server = e3ap_sctp_server::create(&f.service, f.store, test_server_config(), f.connections);
  ASSERT_TRUE(server) << server.detail;
  test_client client(server.server->port());
  ASSERT_TRUE(client.ok());

  e3ap_pdu pdu;
  pdu.id = 3;
  e3ap_subscription_request subscription;
  subscription.dapp_identifier         = 1;
  subscription.ran_function_identifier = e3ap_ran_function_telemetry;
  subscription.telemetry_identifiers   = {telemetry_id_pusch_crc};
  pdu.message                          = subscription;
  ASSERT_TRUE(client.send(pdu));
  auto refused = client.receive_as<e3ap_subscription_response>();
  ASSERT_TRUE(refused);
  EXPECT_EQ(refused->response_code, e3ap_response_code::negative);
  ASSERT_TRUE(refused->cause);
  EXPECT_EQ(*refused->cause, e3ap_cause::setup_required);

  auto dapp_id = client.setup();
  ASSERT_TRUE(dapp_id);
  pdu.id                             = 4;
  subscription.dapp_identifier       = *dapp_id;
  subscription.telemetry_identifiers = {999};
  pdu.message                        = subscription;
  ASSERT_TRUE(client.send(pdu));
  refused = client.receive_as<e3ap_subscription_response>();
  ASSERT_TRUE(refused);
  EXPECT_EQ(refused->response_code, e3ap_response_code::negative);
  ASSERT_TRUE(refused->cause);
  EXPECT_EQ(*refused->cause, e3ap_cause::unsupported_identifier);

  pdu.id                             = 5;
  subscription.telemetry_identifiers = {telemetry_id_pusch_crc};
  pdu.message                        = subscription;
  ASSERT_TRUE(client.send(pdu));
  auto accepted = client.receive_as<e3ap_subscription_response>();
  ASSERT_TRUE(accepted);
  ASSERT_EQ(accepted->response_code, e3ap_response_code::positive) << accepted->detail;
  EXPECT_TRUE(wait_until([&] { return f.store->has_active(telemetry_stream_pusch_crc); }));

  pdu.id      = 6;
  pdu.message = e3ap_release_message{*dapp_id};
  ASSERT_TRUE(client.send(pdu));
  auto ack = client.receive_as<e3ap_message_ack>();
  ASSERT_TRUE(ack);
  EXPECT_EQ(ack->response_code, e3ap_response_code::positive);
  EXPECT_TRUE(wait_until([&] { return !f.store->has_active(telemetry_stream_pusch_crc); }));
  EXPECT_TRUE(wait_until([&] { return server.server->metrics().transport.live_associations == 0; }));
}

TEST(dapp_e3ap_sctp_server, closes_associations_that_never_complete_setup_or_speak_garbage)
{
  fixture f;
  auto    config       = test_server_config();
  config.setup_timeout = std::chrono::milliseconds(100);
  auto server          = e3ap_sctp_server::create(&f.service, f.store, config, f.connections);
  ASSERT_TRUE(server) << server.detail;
  {
    test_client idle(server.server->port());
    ASSERT_TRUE(idle.ok());
    EXPECT_TRUE(wait_until([&] { return server.server->metrics().transport.closed_associations == 1; }));
  }
  {
    test_client noisy(server.server->port());
    ASSERT_TRUE(noisy.ok());
    ASSERT_TRUE(noisy.setup());
    // Anything that does not decode as an E3-PDU is answered once and the association closes.
    const std::vector<uint8_t> garbage = {0xde, 0xad, 0xbe, 0xef};
    int                        fd      = ::socket(AF_INET, SOCK_STREAM, IPPROTO_SCTP);
    sockaddr_in                address{};
    address.sin_family = AF_INET;
    address.sin_port   = htons(server.server->port());
    ::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    ASSERT_EQ(::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
    ASSERT_EQ(::send(fd, garbage.data(), garbage.size(), MSG_NOSIGNAL), static_cast<ssize_t>(garbage.size()));
    EXPECT_TRUE(wait_until([&] { return server.server->metrics().protocol_errors >= 1; }));
    ::close(fd);
  }
  EXPECT_TRUE(wait_until([&] { return server.server->metrics().transport.live_associations == 0; }));
}

TEST(dapp_e3ap_sctp_server, reclaims_the_slot_of_a_peer_that_vanishes_with_output_pending)
{
  // A subscriber that dies while indications are queued for it must not pin an
  // association slot: with a single slot, a second peer has to be accepted.
  fixture f;
  auto    config                        = test_server_config();
  config.transport.maximum_associations = 1;
  config.transport.close_grace          = std::chrono::milliseconds(200);
  auto server                           = e3ap_sctp_server::create(&f.service, f.store, config, f.connections);
  ASSERT_TRUE(server) << server.detail;
  {
    test_client victim(server.server->port());
    ASSERT_TRUE(victim.ok());
    auto dapp_id = victim.setup();
    ASSERT_TRUE(dapp_id);
    e3ap_pdu pdu;
    pdu.id = 9;
    e3ap_subscription_request subscription;
    subscription.dapp_identifier         = *dapp_id;
    subscription.ran_function_identifier = e3ap_ran_function_telemetry;
    subscription.telemetry_identifiers   = {telemetry_id_pusch_symbols};
    pdu.message                          = subscription;
    ASSERT_TRUE(victim.send(pdu));
    auto response = victim.receive_as<e3ap_subscription_response>();
    ASSERT_TRUE(response);
    ASSERT_EQ(response->response_code, e3ap_response_code::positive) << response->detail;
    ASSERT_TRUE(wait_until([&] { return f.store->has_active(telemetry_stream_pusch_symbols); }));

    // Queue far more than the peer will ever read, then let it vanish without a release.
    std::vector<std::complex<float>> symbols(273 * 12 * 13, {1.0F, 0.0F});
    pusch_symbols_diagnostic         event;
    event.rnti              = 0x4601;
    event.slot              = slot_point{1, 1, 1};
    event.nof_prb           = 273;
    event.nof_data_symbols  = 13;
    event.nof_layers        = 1;
    event.mod_bits          = 2;
    event.equalized_symbols = symbols;
    for (unsigned index = 0; index != 200; ++index) {
      f.publisher->on_pusch_symbols(event);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  } // victim closes its socket abruptly here
  ASSERT_TRUE(
      wait_until([&] { return server.server->metrics().transport.live_associations == 0; }, std::chrono::seconds(3)))
      << "dead subscriber kept its slot";
  EXPECT_FALSE(f.store->has_active(telemetry_stream_pusch_symbols)) << "orphaned subscription not removed";

  test_client survivor(server.server->port());
  ASSERT_TRUE(survivor.ok());
  e3ap_pdu setup_pdu;
  setup_pdu.id = 1;
  e3ap_setup_request setup_request;
  setup_request.dapp_name    = "survivor";
  setup_request.dapp_version = "0.0.1";
  setup_request.vendor       = "DeepSig";
  setup_pdu.message       = setup_request;
  ASSERT_TRUE(survivor.send(setup_pdu));
  auto setup_response = survivor.receive();
  const auto m = server.server->metrics();
  ASSERT_TRUE(setup_response) << "no setup response: association dropped, slot was not reclaimed; accepted="
                              << m.transport.accepted_associations << " rejected=" << m.transport.rejected_associations
                              << " closed=" << m.transport.closed_associations << " live=" << m.transport.live_associations
                              << " failures=" << m.transport.transport_failures << " setups=" << m.setups_completed
                              << " setups_rejected=" << m.setups_rejected << " protocol_errors=" << m.protocol_errors
                              << " live_subs=" << m.live_subscriptions;
  const auto* positive = std::get_if<e3ap_setup_response>(&setup_response->message);
  ASSERT_NE(positive, nullptr);
  EXPECT_EQ(positive->response_code, e3ap_response_code::positive) << positive->detail;
  EXPECT_EQ(server.server->metrics().transport.rejected_associations, 0U);
}
