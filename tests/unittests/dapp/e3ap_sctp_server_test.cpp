// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/adt/scope_exit.h"
#include "ocudu/dapp/e3/aper_codec.h"
#include "ocudu/dapp/e3/e3_connection_registry.h"
#include "ocudu/dapp/e3/e3ap_codec.h"
#include "ocudu/dapp/e3/e3ap_libe3_codec.h"
#include "ocudu/dapp/e3/e3ap_protobuf_codec.h"
#include "ocudu/dapp/e3/e3ap_v2_codec.h"
#include "ocudu/dapp/e3/e3ap_sctp_server.h"
#include "ocudu/dapp/e3/embedded_agent.h"
#include "ocudu/dapp/e3/management_service.h"
#include "ocudu/dapp/e3/shared_region.h"
#include "ocudu/dapp/e3/shared_region_layout.h"
#include "ocudu/dapp/e3/telemetry_publisher.h"
#include "ocudu/dapp/runtime/instance_manager.h"
#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <complex>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <gtest/gtest.h>
#include <map>
#include <mutex>
#include <netinet/in.h>
#include <netinet/sctp.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

using namespace ocudu;
using namespace ocudu::dapp;
using namespace ocudu::dapp::e3;

namespace {

/// What a test client speaks. ocudu_aper is E3AP-OCUDU-V2 APER with the OCUDU profile; libe3_aper
/// the same module without additions (what libe3 sends).
enum class client_wire { ocudu_aper, libe3_aper, libe3_protobuf, ocudu_protobuf };

/// Minimal blocking E3AP client over a one-to-one SCTP association.
class test_client
{
public:
  explicit test_client(uint16_t port, bool libe3_ = false) :
    test_client(port, libe3_ ? client_wire::libe3_aper : client_wire::ocudu_aper)
  {
  }
  test_client(uint16_t port, client_wire wire_) : wire(wire_)
  {
    descriptor = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_SCTP);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port   = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    // The receiver of bulk indications must acknowledge every chunk: with the
    // kernel's 200 ms delayed-SACK default the agent's sender stalls on every
    // multi-fragment object. Production clients (python/ocudu_e3/sctp.py) set
    // the same option.
    sctp_sack_info sack{};
    sack.sack_delay = 0;
    sack.sack_freq  = 1;
    ::setsockopt(descriptor, IPPROTO_SCTP, SCTP_DELAYED_SACK, &sack, sizeof(sack));
    // Buffers must be preset before connect(): the receive window is fixed
    // from the receive buffer at association setup, and the 16 KiB default
    // send buffer keeps one chunk in flight.
    const int buffer_bytes = 8 * 1024 * 1024;
    ::setsockopt(descriptor, SOL_SOCKET, SO_RCVBUF, &buffer_bytes, sizeof(buffer_bytes));
    ::setsockopt(descriptor, SOL_SOCKET, SO_SNDBUF, &buffer_bytes, sizeof(buffer_bytes));
    connected = descriptor >= 0 && ::connect(descriptor, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
    // Longer than SCTP's initial retransmission timeout (3 s): the first
    // message of a fresh association occasionally arrives only on
    // retransmission (see test_server_config).
    timeval timeout{5, 0};
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
    auto encoded = wire == client_wire::ocudu_aper    ? e3ap_v2_codec::encode(pdu, e3ap_profile::ocudu)
                   : wire == client_wire::libe3_aper     ? e3ap_libe3_codec::encode(pdu)
                   : wire == client_wire::libe3_protobuf ? e3ap_protobuf_codec::encode(pdu, e3ap_profile::libe3)
                                                         : e3ap_protobuf_codec::encode(pdu, e3ap_profile::ocudu);
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
    auto decoded = wire == client_wire::ocudu_aper  ? e3ap_v2_codec::decode(message, e3ap_profile::ocudu)
                   : wire == client_wire::libe3_aper   ? e3ap_libe3_codec::decode(message)
                   : wire == client_wire::libe3_protobuf
                       ? e3ap_protobuf_codec::decode(message, e3ap_profile::libe3)
                       : e3ap_protobuf_codec::decode(message, e3ap_profile::ocudu);
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
  std::optional<uint16_t> setup(bool stamp = true)
  {
    e3ap_pdu pdu;
    pdu.id = 1;
    if (stamp) {
      pdu.timestamp_ns =
          std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
              .count();
    }
    e3ap_setup_request request;
    request.dapp_name    = "test-dapp";
    request.dapp_version = "0.0.1";
    request.vendor       = "DeepSig";
    if (ocudu_profile()) {
      request.ocudu_profile = e3ap_ocudu_profile_version;
    }
    pdu.message = request;
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

  /// The client runs the OCUDU procedures (and asks for them at setup).
  bool ocudu_profile() const
  {
    return wire == client_wire::ocudu_aper || wire == client_wire::ocudu_protobuf;
  }

private:
  int  descriptor{-1};
  bool connected{};
  client_wire wire{client_wire::ocudu_aper};
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
  // The production default: a setup request that lands while accept() moves
  // the association off the listener can go unacknowledged in the kernel and
  // arrive only on retransmission, rto_initial (3 s) later; a shorter timeout
  // closes that client before its setup is read.
  config.setup_timeout          = std::chrono::milliseconds(5000);
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

// The default outlasts SCTP's initial retransmission timeout (3 s): a chunk of
// a fresh or closing association occasionally arrives only on retransmission.
template <typename Predicate>
bool wait_until(Predicate&& ready, std::chrono::milliseconds limit = std::chrono::seconds(5))
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

/// Publishes one PUSCH CRC record and retries until the store takes it. The store never blocks the
/// (real-time) producer: a publication that meets the agent's poll holding the subscription queue is
/// dropped as busy, which is right for a live stream but would lose a test's only record.
void publish_crc(telemetry_publisher& publisher, const pusch_crc_diagnostic& event)
{
  const uint64_t before = publisher.metrics().published;
  for (int attempt = 0; attempt != 200 && publisher.metrics().published == before; ++attempt) {
    publisher.on_pusch_crc(event);
    if (publisher.metrics().published == before) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
  ASSERT_GT(publisher.metrics().published, before) << "the store never accepted the record";
}

} // namespace

TEST(dapp_e3ap_sctp_server, rejects_unknown_protocol_and_profile_versions_before_allocating_an_id)
{
  for (auto wire :
       {client_wire::ocudu_aper, client_wire::libe3_aper, client_wire::ocudu_protobuf, client_wire::libe3_protobuf}) {
    SCOPED_TRACE(static_cast<int>(wire));
    fixture f;
    auto    server = e3ap_sctp_server::create(&f.service, f.store, test_server_config());
    ASSERT_TRUE(server) << server.detail;
    test_client peer(server.server->port(), wire);
    ASSERT_TRUE(peer.ok());
    e3ap_setup_request request;
    request.dapp_name    = "version-test";
    request.dapp_version = "1";
    request.vendor       = "test";
    if (peer.ocudu_profile()) {
      request.ocudu_profile = e3ap_ocudu_profile_version;
    }
    e3ap_pdu pdu;
    pdu.id             = 1;
    const auto refused = [&]() {
      pdu.message = request;
      EXPECT_TRUE(peer.send(pdu));
      auto response = peer.receive_as<e3ap_setup_response>();
      EXPECT_TRUE(response);
      if (response) {
        EXPECT_EQ(response->response_code, e3ap_response_code::negative);
        EXPECT_FALSE(response->dapp_identifier);
        EXPECT_FALSE(response->ocudu_profile);
        if (peer.ocudu_profile()) {
          EXPECT_EQ(response->cause, e3ap_cause::unsupported_protocol_version);
        }
      }
      ++pdu.id;
    };
    for (const auto* version : {"1.", "1.invalid", "1.0.1", "1.1.0", "2.0.0"}) {
      request.e3ap_protocol_version = version;
      refused();
    }
    request.e3ap_protocol_version = e3ap_version_string;
    if (peer.ocudu_profile()) {
      for (uint8_t profile : {2, 255}) {
        request.ocudu_profile = profile;
        refused();
      }
    }
    EXPECT_EQ(server.server->metrics().setups_completed, 0U);
    EXPECT_EQ(server.server->metrics().setups_rejected, pdu.id - 1);
    // A correction on the bound encoding is allowed; only this setup owns an ID.
    ASSERT_TRUE(peer.setup());
    EXPECT_EQ(server.server->metrics().setups_completed, 1U);
  }
}

TEST(dapp_e3ap_sctp_server, normalizes_periodicity_units_before_applying_defaults_and_hard_floors)
{
  for (auto wire :
       {client_wire::ocudu_aper, client_wire::libe3_aper, client_wire::ocudu_protobuf, client_wire::libe3_protobuf}) {
    SCOPED_TRACE(static_cast<int>(wire));
    fixture f;
    auto    config = test_server_config();
    config.streams = default_telemetry_streams();
    for (auto& stream : config.streams) {
      if (stream.telemetry_id == telemetry_id_pusch_crc) {
        stream.minimum_interval_us    = 700;
        stream.hard_floor_interval_us = 0;
      }
    }
    auto server = e3ap_sctp_server::create(&f.service, f.store, config);
    ASSERT_TRUE(server);
    test_client peer(server.server->port(), wire);
    auto        id = peer.setup();
    ASSERT_TRUE(id);
    e3ap_subscription_request request;
    request.dapp_identifier         = *id;
    request.ran_function_identifier = e3ap_ran_function_telemetry;
    request.telemetry_identifiers   = {telemetry_id_pusch_crc};
    e3ap_pdu pdu;
    pdu.id = 10;
    for (auto interval : {std::optional<uint32_t>{},
                          std::optional<uint32_t>{0},
                          std::optional<uint32_t>{1},
                          std::optional<uint32_t>{1000}}) {
      request.periodicity = interval;
      pdu.message         = request;
      ASSERT_TRUE(peer.send(pdu));
      auto response = peer.receive_as<e3ap_subscription_response>();
      ASSERT_TRUE(response && response->subscription_id);
      ASSERT_EQ(response->response_code, e3ap_response_code::positive);
      const uint64_t expected = interval ? uint64_t{*interval} * (peer.ocudu_profile() ? 1000 : 1) : 700;
      EXPECT_EQ(f.store->demand(telemetry_stream_pusch_crc).fastest_requested_us, expected);
      pdu.message = e3ap_subscription_delete{*id, *response->subscription_id};
      ASSERT_TRUE(peer.send(pdu));
      if (peer.ocudu_profile()) {
        ASSERT_TRUE(peer.receive_as<e3ap_message_ack>());
      } else {
        ASSERT_TRUE(peer.receive_as<e3ap_subscription_response>());
      }
      ++pdu.id;
    }
    // Prove this reaches emission filtering, not just the store's demand field:
    // libe3 1000 means 1 ms, not 1 s. 20 ms spaced records must both be emitted.
    if (!peer.ocudu_profile()) {
      pdu.message = request;
      ASSERT_TRUE(peer.send(pdu));
      ASSERT_TRUE(peer.receive_as<e3ap_subscription_response>());
      publish_crc(*f.publisher, {0x4601, slot_point{1, 20, 3}, true});
      ASSERT_TRUE(peer.receive_as<e3ap_indication_message>());
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      publish_crc(*f.publisher, {0x4601, slot_point{1, 20, 4}, true});
      ASSERT_TRUE(
          wait_until([&] { return server.server->metrics().indications_sent == 2; }, std::chrono::milliseconds(500)));
      ASSERT_TRUE(peer.receive_as<e3ap_indication_message>());
    }
  }
}

TEST(dapp_e3ap_sctp_server, rejected_setup_does_not_pin_the_proposed_profile)
{
  for (auto wire : {client_wire::ocudu_aper, client_wire::ocudu_protobuf}) {
    fixture f;
    auto    server = e3ap_sctp_server::create(&f.service, f.store, test_server_config());
    ASSERT_TRUE(server);
    test_client        peer(server.server->port(), wire);
    e3ap_setup_request request;
    request.dapp_name     = "retry";
    request.dapp_version  = "1";
    request.vendor        = "test";
    request.ocudu_profile = 2;
    e3ap_pdu pdu;
    pdu.id      = 1;
    pdu.message = request;
    ASSERT_TRUE(peer.send(pdu));
    auto response = peer.receive_as<e3ap_setup_response>();
    ASSERT_TRUE(response);
    EXPECT_EQ(response->response_code, e3ap_response_code::negative);
    request.ocudu_profile.reset(); // retry as libe3, not OCUDU profile 1
    pdu.id      = 2;
    pdu.message = request;
    ASSERT_TRUE(peer.send(pdu));
    response = peer.receive_as<e3ap_setup_response>();
    ASSERT_TRUE(response && response->dapp_identifier);
    EXPECT_EQ(response->response_code, e3ap_response_code::positive);
    EXPECT_FALSE(response->ocudu_profile);
    EXPECT_LE(*response->dapp_identifier, e3ap_libe3_codec::dapp_identifier_pool);
    EXPECT_EQ(server.server->metrics().libe3_setups_completed, 1U);
  }
}

TEST(dapp_e3ap_sctp_server, hard_floor_and_millisecond_overrides_survive_store_lease_recovery)
{
  for (auto wire :
       {client_wire::ocudu_aper, client_wire::libe3_aper, client_wire::ocudu_protobuf, client_wire::libe3_protobuf}) {
    SCOPED_TRACE(static_cast<int>(wire));
    fixture f;
    auto    config = test_server_config();
    config.streams = default_telemetry_streams();
    for (auto& stream : config.streams) {
      if (stream.telemetry_id == telemetry_id_pusch_crc) {
        stream.hard_floor_interval_us = 1500;
      }
    }
    auto server = e3ap_sctp_server::create(&f.service, f.store, config);
    ASSERT_TRUE(server);
    test_client peer(server.server->port(), wire);
    auto        id = peer.setup();
    ASSERT_TRUE(id);
    e3ap_subscription_request request;
    request.dapp_identifier         = *id;
    request.ran_function_identifier = e3ap_ran_function_telemetry;
    request.telemetry_identifiers   = {telemetry_id_pusch_crc};
    request.periodicity             = 1;
    if (peer.ocudu_profile()) {
      telemetry_subscription_config stream;
      stream.telemetry_id        = telemetry_id_pusch_crc;
      stream.minimum_interval_ms = 5;
      stream.cell_id             = 3;
      stream.sample_every_n      = 2;
      stream.drop_policy         = telemetry_drop_policy::drop_newest;
      telemetry_subscription_data data;
      data.configs.push_back(stream);
      auto encoded = e3ap_codec::encode(data);
      ASSERT_TRUE(encoded);
      request.subscription_data = *encoded.value;
    }
    e3ap_pdu pdu;
    pdu.id      = 2;
    pdu.message = request;
    ASSERT_TRUE(peer.send(pdu));
    ASSERT_TRUE(peer.receive_as<e3ap_subscription_response>());
    const uint64_t expected_us = peer.ocudu_profile() ? 5000 : 1500;
    EXPECT_EQ(f.store->demand(telemetry_stream_pusch_crc).fastest_requested_us, expected_us);
    auto before = f.store->subscriptions();
    ASSERT_EQ(before.subscriptions.size(), 1U);
    // Simulate a lost store lease while leaving the protocol subscription valid.
    ASSERT_TRUE(f.store->unsubscribe(before.subscriptions[0].request.subscription_id));
    ASSERT_TRUE(wait_until([&] { return f.store->demand(telemetry_stream_pusch_crc).active == 1; }));
    auto after = f.store->subscriptions();
    ASSERT_EQ(after.subscriptions.size(), 1U);
    const auto& restored = after.subscriptions[0].request;
    EXPECT_EQ(restored.requested_interval_us, expected_us);
    EXPECT_EQ(restored.cell_id, before.subscriptions[0].request.cell_id);
    EXPECT_EQ(restored.sample_every_n, before.subscriptions[0].request.sample_every_n);
    EXPECT_EQ(restored.drop_policy, before.subscriptions[0].request.drop_policy);
  }
}

TEST(dapp_e3ap_sctp_server, subscription_expiry_reclaims_shared_memory_and_admission_budget)
{
  fixture f;
  auto    config                        = test_server_config();
  config.maximum_payload_bytes          = 65536;
  config.shared_region_rows             = 1;
  config.maximum_shared_region_bytes    = 65664;
  config.maximum_subscriptions_per_dapp = 1;
  auto server                           = e3ap_sctp_server::create(&f.service, f.store, config, f.connections);
  ASSERT_TRUE(server);
  test_client peer(server.server->port());
  auto        id = peer.setup();
  ASSERT_TRUE(id);
  e3ap_subscription_request request;
  request.dapp_identifier         = *id;
  request.ran_function_identifier = e3ap_ran_function_telemetry;
  request.telemetry_identifiers   = {telemetry_id_pusch_crc};
  request.subscription_time_s     = 1;
  request.delivery_mode           = e3ap_delivery_mode::by_reference;
  e3ap_pdu pdu;
  pdu.id      = 2;
  pdu.message = request;
  ASSERT_TRUE(peer.send(pdu));
  auto first = peer.receive_as<e3ap_subscription_response>();
  ASSERT_TRUE(first && first->shared_region);
  ASSERT_TRUE(wait_until([&] { return server.server->metrics().subscriptions_expired == 1; }));
  const int descriptor = ::shm_open(first->shared_region->region_name.c_str(), O_RDONLY, 0);
  EXPECT_EQ(descriptor, -1);
  if (descriptor >= 0) {
    ::close(descriptor);
  } else {
    EXPECT_EQ(errno, ENOENT);
  }
  EXPECT_TRUE(f.store->subscriptions().subscriptions.empty());
  request.subscription_time_s = 0;
  pdu.id                      = 3;
  pdu.message                 = request;
  ASSERT_TRUE(peer.send(pdu));
  auto second = peer.receive_as<e3ap_subscription_response>();
  ASSERT_TRUE(second && second->shared_region);
  EXPECT_EQ(second->response_code, e3ap_response_code::positive);
  EXPECT_NE(second->shared_region->region_name, first->shared_region->region_name);
}

TEST(dapp_e3ap_sctp_server, finite_subscription_expires_despite_continuous_store_lease_renewal)
{
  for (auto wire :
       {client_wire::ocudu_aper, client_wire::libe3_aper, client_wire::ocudu_protobuf, client_wire::libe3_protobuf}) {
    SCOPED_TRACE(static_cast<int>(wire));
    fixture f;
    auto    config  = test_server_config();
    config.lease_us = 100'000; // renewed by idle drains many times before the 1 s protocol lifetime
    auto server     = e3ap_sctp_server::create(&f.service, f.store, config, f.connections);
    ASSERT_TRUE(server);
    test_client peer(server.server->port(), wire);
    auto        id = peer.setup();
    ASSERT_TRUE(id);
    e3ap_subscription_request request;
    request.dapp_identifier         = *id;
    request.ran_function_identifier = e3ap_ran_function_telemetry;
    request.telemetry_identifiers   = {telemetry_id_pusch_crc};
    request.subscription_time_s     = 1;
    e3ap_pdu pdu;
    pdu.id      = 2;
    pdu.message = request;
    ASSERT_TRUE(peer.send(pdu));
    auto granted = peer.receive_as<e3ap_subscription_response>();
    ASSERT_TRUE(granted && granted->subscription_id);
    // Traffic and store lease renewal must not extend the subscription duration.
    for (unsigned index = 0; index != 3; ++index) {
      publish_crc(*f.publisher, {0x4601, slot_point{1, 20, 3}, true});
      ASSERT_TRUE(peer.receive_as<e3ap_indication_message>());
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    EXPECT_EQ(server.server->metrics().subscriptions_expired, 0U);
    ASSERT_TRUE(
        wait_until([&] { return server.server->metrics().subscriptions_expired == 1; }, std::chrono::seconds(3)));
    EXPECT_EQ(server.server->metrics().live_subscriptions, 0U);
    EXPECT_EQ(server.server->metrics().subscriptions_removed, 1U);
    EXPECT_TRUE(f.store->subscriptions().subscriptions.empty());
    EXPECT_FALSE(f.store->has_active(telemetry_stream_pusch_crc));
    ASSERT_TRUE(wait_until([&] {
      auto records = f.connections->snapshot();
      return records.size() == 1 && records[0].subscriptions.empty();
    }));
    // Deleting an expired subscription must finish in the peer's procedure dialect.
    pdu.id      = 3;
    pdu.message = e3ap_subscription_delete{*id, *granted->subscription_id};
    ASSERT_TRUE(peer.send(pdu));
    if (peer.ocudu_profile()) {
      auto response = peer.receive_as<e3ap_message_ack>();
      ASSERT_TRUE(response);
      EXPECT_EQ(response->cause, e3ap_cause::not_found);
    } else {
      auto response = peer.receive_as<e3ap_subscription_response>();
      ASSERT_TRUE(response);
      EXPECT_EQ(response->response_code, e3ap_response_code::negative);
      EXPECT_EQ(response->subscription_id, granted->subscription_id);
    }
  }
}

TEST(dapp_e3ap_sctp_server, quiet_subscription_survives_idle_until_expiry_or_peer_disconnect)
{
  for (auto duration : {std::optional<uint32_t>{}, std::optional<uint32_t>{0}, std::optional<uint32_t>{1}}) {
    fixture f;
    auto    config                = test_server_config();
    config.transport.idle_timeout = std::chrono::milliseconds(200);
    auto server                   = e3ap_sctp_server::create(&f.service, f.store, config);
    ASSERT_TRUE(server);
    {
      test_client peer(server.server->port());
      auto        id = peer.setup();
      ASSERT_TRUE(id);
      e3ap_subscription_request request;
      request.dapp_identifier         = *id;
      request.ran_function_identifier = e3ap_ran_function_telemetry;
      request.telemetry_identifiers   = {telemetry_id_pusch_crc};
      request.subscription_time_s     = duration;
      e3ap_pdu pdu;
      pdu.id      = 2;
      pdu.message = request;
      ASSERT_TRUE(peer.send(pdu));
      ASSERT_TRUE(peer.receive_as<e3ap_subscription_response>());
      std::this_thread::sleep_for(std::chrono::milliseconds(400));
      EXPECT_EQ(server.server->metrics().transport.live_associations, 1U);
      EXPECT_EQ(server.server->metrics().transport.idle_closures, 0U);
      EXPECT_EQ(server.server->metrics().live_subscriptions, 1U);
      if (duration.value_or(0) != 0) {
        ASSERT_TRUE(wait_until([&] { return server.server->metrics().transport.live_associations == 0; }));
        EXPECT_EQ(server.server->metrics().subscriptions_expired, 1U);
        EXPECT_EQ(server.server->metrics().transport.idle_closures, 1U);
      }
    } // A real disconnect still reclaims an unlimited quiet subscription.
    ASSERT_TRUE(wait_until([&] { return server.server->metrics().live_subscriptions == 0; }));
    EXPECT_FALSE(f.store->has_active(telemetry_stream_pusch_crc));
  }
}

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
  // [srs-sharing] 8 upstream streams + srs.config + ta.command.
  EXPECT_EQ(telemetry.telemetry_identifiers.size(), 10U);
  auto catalog = e3ap_codec::decode_telemetry_function(telemetry.ran_function_data);
  ASSERT_TRUE(catalog) << catalog.detail;
  EXPECT_EQ(catalog.value->streams.size(), 10U);
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
  publish_crc(*f.publisher, {0x4601, slot_point{1, 20, 3}, true});

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
  setup_pdu.message          = setup_request;
  ASSERT_TRUE(survivor.send(setup_pdu));
  auto       setup_response = survivor.receive();
  const auto m              = server.server->metrics();
  ASSERT_TRUE(setup_response) << "no setup response: association dropped, slot was not reclaimed; accepted="
                              << m.transport.accepted_associations << " rejected=" << m.transport.rejected_associations
                              << " closed=" << m.transport.closed_associations
                              << " live=" << m.transport.live_associations
                              << " failures=" << m.transport.transport_failures << " setups=" << m.setups_completed
                              << " setups_rejected=" << m.setups_rejected << " protocol_errors=" << m.protocol_errors
                              << " live_subs=" << m.live_subscriptions;
  const auto* positive = std::get_if<e3ap_setup_response>(&setup_response->message);
  ASSERT_NE(positive, nullptr);
  EXPECT_EQ(positive->response_code, e3ap_response_code::positive) << positive->detail;
  EXPECT_EQ(server.server->metrics().transport.rejected_associations, 0U);
}

TEST(dapp_e3ap_sctp_server, stamps_outbound_pdus_with_the_realtime_clock)
{
  fixture f;
  auto    server = e3ap_sctp_server::create(&f.service, f.store, test_server_config(), f.connections);
  ASSERT_TRUE(server) << server.detail;
  test_client client(server.server->port());
  ASSERT_TRUE(client.ok());
  e3ap_pdu pdu;
  pdu.id = 1;
  e3ap_setup_request request;
  request.dapp_name    = "ts-dapp";
  request.dapp_version = "1";
  request.vendor       = "DeepSig";
  pdu.message          = request;
  const auto before =
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
  pdu.timestamp_ns = before;
  ASSERT_TRUE(client.send(pdu));
  auto reply = client.receive();
  ASSERT_TRUE(reply);
  ASSERT_TRUE(std::holds_alternative<e3ap_setup_response>(reply->message));
  ASSERT_TRUE(reply->timestamp_ns) << "the agent must stamp every PDU to a peer that stamps";
  const auto after =
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
  EXPECT_GE(*reply->timestamp_ns, before);
  EXPECT_LE(*reply->timestamp_ns, after);

  // A peer built against the pre-timestamp module never stamps its setup and
  // cannot decode the field: the agent must answer it without timestamps.
  test_client legacy(server.server->port());
  ASSERT_TRUE(legacy.ok());
  e3ap_pdu legacy_pdu;
  legacy_pdu.id      = 1;
  legacy_pdu.message = request;
  ASSERT_TRUE(legacy.send(legacy_pdu));
  auto legacy_reply = legacy.receive();
  ASSERT_TRUE(legacy_reply);
  ASSERT_TRUE(std::holds_alternative<e3ap_setup_response>(legacy_reply->message));
  EXPECT_FALSE(legacy_reply->timestamp_ns) << "unstamped peers must get the pre-timestamp wire";
}

TEST(dapp_e3ap_sctp_server, hands_dapp_reports_to_the_handler_and_keeps_recent_ones)
{
  fixture f;
  auto    server = e3ap_sctp_server::create(&f.service, f.store, test_server_config(), f.connections);
  ASSERT_TRUE(server) << server.detail;
  std::mutex                          mutex;
  std::vector<e3ap_dapp_report_event> seen;
  server.server->set_dapp_report_handler([&](const e3ap_dapp_report_event& event) {
    std::lock_guard<std::mutex> lock(mutex);
    seen.push_back(event);
  });
  test_client client(server.server->port());
  ASSERT_TRUE(client.ok());
  auto dapp_id = client.setup();
  ASSERT_TRUE(dapp_id);

  e3ap_pdu pdu;
  pdu.id           = 9;
  pdu.timestamp_ns = 1756800000000000000LL;
  e3ap_dapp_report report;
  report.dapp_identifier         = *dapp_id;
  report.ran_function_identifier = 1; // a Spectrum-style service model
  report.report_data             = {0xAA, 0xBB, 0xCC};
  report.sequence_identifier     = 31; // mandatory (libe3's root field)
  pdu.message                    = report;
  ASSERT_TRUE(client.send(pdu));
  auto ack = client.receive_as<e3ap_message_ack>();
  ASSERT_TRUE(ack);
  EXPECT_EQ(ack->request_id, 9U);
  EXPECT_EQ(ack->response_code, e3ap_response_code::positive);
  ASSERT_TRUE(wait_until([&] {
    std::lock_guard<std::mutex> lock(mutex);
    return seen.size() == 1;
  }));
  {
    std::lock_guard<std::mutex> lock(mutex);
    EXPECT_EQ(seen[0].dapp_identifier, *dapp_id);
    EXPECT_EQ(seen[0].dapp_name, "test-dapp");
    EXPECT_EQ(seen[0].ran_function_identifier, 1U);
    EXPECT_EQ(seen[0].report_data, (std::vector<uint8_t>{0xAA, 0xBB, 0xCC}));
    ASSERT_TRUE(seen[0].timestamp_ns);
    EXPECT_EQ(*seen[0].timestamp_ns, 1756800000000000000LL);
  }
  const auto recent = server.server->recent_reports();
  ASSERT_EQ(recent.size(), 1U);
  EXPECT_EQ(recent[0].report_data.size(), 3U);
  EXPECT_EQ(server.server->metrics().reports_received, 1U);

  // A report that names another dApp is refused and not consumed.
  report.dapp_identifier = static_cast<uint16_t>(*dapp_id + 1);
  pdu.id                 = 10;
  pdu.message            = report;
  ASSERT_TRUE(client.send(pdu));
  ack = client.receive_as<e3ap_message_ack>();
  ASSERT_TRUE(ack);
  EXPECT_EQ(ack->response_code, e3ap_response_code::negative);
  EXPECT_EQ(server.server->metrics().reports_received, 1U);
}

TEST(dapp_e3ap_sctp_server, relays_xapp_control_actions_to_the_named_dapp)
{
  fixture f;
  auto    server = e3ap_sctp_server::create(&f.service, f.store, test_server_config(), f.connections);
  ASSERT_TRUE(server) << server.detail;
  test_client client(server.server->port());
  ASSERT_TRUE(client.ok());
  auto dapp_id = client.setup();
  ASSERT_TRUE(dapp_id);

  ASSERT_TRUE(server.server->send_xapp_control_action(*dapp_id, 1, {0x01, 0x02}));
  auto action = client.receive_as<e3ap_xapp_control_action>();
  ASSERT_TRUE(action) << "the xApp control action must reach the dApp on the procedure stream";
  EXPECT_EQ(action->dapp_identifier, *dapp_id);
  EXPECT_EQ(action->ran_function_identifier, 1U);
  EXPECT_EQ(action->xapp_control_data, (std::vector<uint8_t>{0x01, 0x02}));
  EXPECT_TRUE(wait_until([&] { return server.server->metrics().xapp_controls_sent == 1; }));

  // An unknown dApp identifier is counted as dropped, not delivered.
  ASSERT_TRUE(server.server->send_xapp_control_action(static_cast<uint16_t>(*dapp_id + 7), 1, {0x03}));
  EXPECT_TRUE(wait_until([&] { return server.server->metrics().xapp_controls_dropped == 1; }));
  EXPECT_FALSE(server.server->send_xapp_control_action(*dapp_id, 1, {}));
}

TEST(dapp_e3ap_sctp_server, delivers_large_tensors_by_reference_through_a_shared_region)
{
  fixture f;
  auto    config            = test_server_config();
  config.shared_region_rows = 2;
  auto server               = e3ap_sctp_server::create(&f.service, f.store, config, f.connections);
  ASSERT_TRUE(server) << server.detail;
  test_client client(server.server->port());
  ASSERT_TRUE(client.ok());
  auto dapp_id = client.setup();
  ASSERT_TRUE(dapp_id);

  e3ap_pdu pdu;
  pdu.id = 5;
  e3ap_subscription_request subscription;
  subscription.dapp_identifier            = *dapp_id;
  subscription.ran_function_identifier    = e3ap_ran_function_telemetry;
  subscription.telemetry_identifiers      = {telemetry_id_pusch_symbols, telemetry_id_pusch_crc};
  subscription.delivery_mode              = e3ap_delivery_mode::by_reference;
  subscription.minimum_by_reference_bytes = 1024;
  pdu.message                             = subscription;
  ASSERT_TRUE(client.send(pdu));
  auto response = client.receive_as<e3ap_subscription_response>();
  ASSERT_TRUE(response);
  ASSERT_EQ(response->response_code, e3ap_response_code::positive) << response->detail;
  ASSERT_TRUE(response->shared_region) << "by-reference subscriptions advertise their region";
  const auto region = *response->shared_region;
  EXPECT_EQ(region.memory_domain, e3ap_memory_domain::posix_shared);
  EXPECT_EQ(region.nof_rows, 2U);
  EXPECT_EQ(region.header_bytes, 64U);
  EXPECT_GE(region.row_bytes, 64U + 4 * 1024 * 1024);
  EXPECT_EQ(region.region_bytes, 64U + region.row_bytes * 2);

  // Map the region read-only exactly as an external dApp would.
  const int fd = ::shm_open(region.region_name.c_str(), O_RDONLY, 0);
  ASSERT_GE(fd, 0) << "region " << region.region_name << " must exist: " << std::strerror(errno);
  void* base = ::mmap(nullptr, region.region_bytes, PROT_READ, MAP_SHARED, fd, 0);
  ASSERT_NE(base, MAP_FAILED);
  ::close(fd);
  const auto* header = static_cast<const ocudu_e3_shared_region_header_v1*>(base);
  EXPECT_EQ(header->magic, OCUDU_E3_SHARED_REGION_MAGIC_V1);
  EXPECT_EQ(header->nof_rows, 2U);
  EXPECT_EQ(header->row_bytes, region.row_bytes);
  EXPECT_EQ(header->write_index, OCUDU_E3_SHARED_REGION_NO_ROW);

  EXPECT_TRUE(wait_until([&] { return f.store->has_active(telemetry_stream_pusch_symbols); }));
  std::vector<std::complex<float>> symbols(273 * 12 * 13);
  for (std::size_t index = 0; index != symbols.size(); ++index) {
    symbols[index] = {static_cast<float>(index % 89), -2.0F};
  }
  pusch_symbols_diagnostic event;
  event.rnti              = 0x4601;
  event.slot              = slot_point{1, 20, 3};
  event.nof_prb           = 273;
  event.nof_data_symbols  = 13;
  event.nof_layers        = 1;
  event.mod_bits          = 8;
  event.equalized_symbols = symbols;
  ASSERT_TRUE(f.publisher->should_collect(phy_diagnostic_stream::pusch_symbols, 0x4601));
  f.publisher->on_pusch_symbols(event);
  ASSERT_TRUE(f.publisher->should_collect(phy_diagnostic_stream::pusch_crc, 0x4601));
  publish_crc(*f.publisher, {0x4601, slot_point{1, 20, 3}, true});

  bool saw_symbols = false;
  bool saw_crc     = false;
  while (!(saw_symbols && saw_crc)) {
    auto indication = client.receive_as<e3ap_indication_message>();
    ASSERT_TRUE(indication) << "indication stream stalled";
    ASSERT_TRUE(indication->telemetry_identifier);
    if (*indication->telemetry_identifier == telemetry_id_pusch_crc) {
      // One byte: below the by-reference threshold, so it stays in band.
      EXPECT_FALSE(indication->by_reference);
      saw_crc = true;
      continue;
    }
    ASSERT_EQ(*indication->telemetry_identifier, telemetry_id_pusch_symbols);
    EXPECT_EQ(indication->fragment_count, 1U) << "the record without its tensor fits one fragment";
    ASSERT_TRUE(indication->by_reference) << "large tensors must be delivered by reference";
    const auto& descriptor = *indication->by_reference;
    EXPECT_EQ(descriptor.byte_length, symbols.size() * sizeof(std::complex<float>));
    ASSERT_TRUE(descriptor.element_type);
    EXPECT_EQ(*descriptor.element_type, e3ap_element_type::complex_fp32);
    ASSERT_TRUE(std::holds_alternative<e3ap_posix_shared_location>(descriptor.location));
    const auto& location = std::get<e3ap_posix_shared_location>(descriptor.location);
    EXPECT_EQ(location.region_name, region.region_name);
    EXPECT_EQ(location.row_index, 0U);
    EXPECT_EQ(location.byte_offset, 64U + 64U);
    // The record itself arrived with its bulk field empty.
    auto decoded = e3ap_codec::decode_telemetry(indication->protocol_data);
    ASSERT_TRUE(decoded) << decoded.detail;
    const auto& record = std::get<pusch_symbols_record>(decoded.value->object);
    EXPECT_EQ(record.rnti, 0x4601);
    EXPECT_TRUE(record.equalized_symbols.empty());
    // Read the row like a dApp: check the generation before and after the copy.
    const auto* bytes = static_cast<const uint8_t*>(base);
    const auto* row_header =
        reinterpret_cast<const ocudu_e3_shared_row_header_v1*>(bytes + 64 + region.row_bytes * location.row_index);
    EXPECT_EQ(__atomic_load_n(&row_header->generation, __ATOMIC_ACQUIRE), descriptor.generation);
    EXPECT_EQ(row_header->byte_length, descriptor.byte_length);
    EXPECT_EQ(row_header->telemetry_id, telemetry_id_pusch_symbols);
    EXPECT_EQ(row_header->sequence, decoded.value->sequence);
    std::vector<uint8_t> copy(bytes + location.byte_offset, bytes + location.byte_offset + descriptor.byte_length);
    EXPECT_EQ(__atomic_load_n(&row_header->generation, __ATOMIC_ACQUIRE), descriptor.generation);
    EXPECT_EQ(std::memcmp(copy.data(), symbols.data(), copy.size()), 0);
    EXPECT_EQ(header->write_index, 0U);
    EXPECT_EQ(header->generation, descriptor.generation);
    saw_symbols = true;
  }
  EXPECT_TRUE(wait_until([&] { return server.server->metrics().indications_by_reference == 1; }));
  ::munmap(base, region.region_bytes);

  // Deleting the subscription unlinks the region.
  e3ap_pdu remove;
  remove.id      = 6;
  remove.message = e3ap_subscription_delete{*dapp_id, *response->subscription_id};
  ASSERT_TRUE(client.send(remove));
  auto ack = client.receive_as<e3ap_message_ack>();
  ASSERT_TRUE(ack);
  EXPECT_EQ(ack->response_code, e3ap_response_code::positive);
  EXPECT_TRUE(wait_until([&] { return ::shm_open(region.region_name.c_str(), O_RDONLY, 0) < 0; }));
}

TEST(dapp_e3ap_sctp_server, streams_multi_megabyte_indications_without_delayed_sack_stalls)
{
  // With the kernel defaults (16 KiB send buffer, receive window fixed at
  // association setup, 200 ms delayed SACK) a ~1.7 MB object in 64 KiB
  // fragments takes seconds on loopback; with buffers preset on the listener
  // before listen() and on the client before connect() it completes in
  // milliseconds. This guards the socket options, not a precise latency.
  fixture f;
  auto    server = e3ap_sctp_server::create(&f.service, f.store, test_server_config(), f.connections);
  ASSERT_TRUE(server) << server.detail;
  test_client client(server.server->port());
  ASSERT_TRUE(client.ok());
  auto dapp_id = client.setup();
  ASSERT_TRUE(dapp_id);

  // Full rate: the stream's default capture floor would thin back-to-back objects.
  telemetry_subscription_data   data;
  telemetry_subscription_config full_rate;
  full_rate.telemetry_id        = telemetry_id_pusch_symbols;
  full_rate.minimum_interval_ms = 0;
  data.configs.push_back(full_rate);
  auto data_bytes = e3ap_codec::encode(data);
  ASSERT_TRUE(data_bytes) << data_bytes.detail;
  e3ap_pdu pdu;
  pdu.id = 5;
  e3ap_subscription_request subscription;
  subscription.dapp_identifier         = *dapp_id;
  subscription.ran_function_identifier = e3ap_ran_function_telemetry;
  subscription.telemetry_identifiers   = {telemetry_id_pusch_symbols};
  subscription.subscription_data       = *data_bytes.value;
  pdu.message                          = subscription;
  ASSERT_TRUE(client.send(pdu));
  auto response = client.receive_as<e3ap_subscription_response>();
  ASSERT_TRUE(response);
  ASSERT_EQ(response->response_code, e3ap_response_code::positive) << response->detail;
  EXPECT_TRUE(wait_until([&] { return f.store->has_active(telemetry_stream_pusch_symbols); }));

  // Three objects of ~1.7 MB each (273 PRB x 12 x 13 symbols x 4 layers).
  std::vector<std::complex<float>> symbols(273 * 12 * 13 * 4, {1.0F, 1.0F});
  pusch_symbols_diagnostic         event;
  event.rnti              = 0x4601;
  event.slot              = slot_point{1, 20, 3};
  event.nof_prb           = 273;
  event.nof_data_symbols  = 13;
  event.nof_layers        = 4;
  event.mod_bits          = 8;
  event.equalized_symbols = symbols;
  const auto started      = std::chrono::steady_clock::now();
  unsigned   received     = 0;
  for (unsigned object = 0; object != 3; ++object) {
    ASSERT_TRUE(f.publisher->should_collect(phy_diagnostic_stream::pusch_symbols, 0x4601));
    f.publisher->on_pusch_symbols(event);
    std::size_t bytes = 0;
    while (true) {
      auto indication = client.receive_as<e3ap_indication_message>();
      ASSERT_TRUE(indication) << "indication stream stalled after " << received << " objects";
      bytes += indication->protocol_data.size();
      if (static_cast<uint32_t>(indication->fragment_index) + 1 == indication->fragment_count) {
        break;
      }
    }
    EXPECT_GE(bytes, symbols.size() * sizeof(std::complex<float>));
    ++received;
  }
  const auto elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
  EXPECT_LT(elapsed.count(), 1500) << "three ~1.7 MB objects over loopback took " << elapsed.count()
                                   << " ms; the listener probably no longer presets its socket buffers";
}

namespace {

/// Publishes \p objects PUSCH-symbol objects of \p symbols_per_object samples
/// and returns the per-object latency (publish call to complete receipt,
/// including the client's copy out of the shared region for by-reference
/// delivery), sorted ascending.
std::vector<double> measure_delivery(fixture&     f,
                                     test_client& client,
                                     uint16_t     dapp_id,
                                     bool         by_reference,
                                     unsigned     objects,
                                     std::size_t  symbols_per_object)
{
  telemetry_subscription_data   data;
  telemetry_subscription_config full_rate;
  full_rate.telemetry_id        = telemetry_id_pusch_symbols;
  full_rate.minimum_interval_ms = 0;
  data.configs.push_back(full_rate);
  auto data_bytes = e3ap_codec::encode(data);
  EXPECT_TRUE(data_bytes) << data_bytes.detail;
  e3ap_pdu pdu;
  pdu.id = 5;
  e3ap_subscription_request subscription;
  subscription.dapp_identifier         = dapp_id;
  subscription.ran_function_identifier = e3ap_ran_function_telemetry;
  subscription.telemetry_identifiers   = {telemetry_id_pusch_symbols};
  subscription.subscription_data       = *data_bytes.value;
  if (by_reference) {
    subscription.delivery_mode              = e3ap_delivery_mode::by_reference;
    subscription.minimum_by_reference_bytes = 4096;
  }
  pdu.message = subscription;
  EXPECT_TRUE(client.send(pdu));
  auto response = client.receive_as<e3ap_subscription_response>();
  if (!response || response->response_code != e3ap_response_code::positive || !response->subscription_id) {
    ADD_FAILURE() << "benchmark subscription was not admitted";
    return {};
  }
  EXPECT_TRUE(wait_until([&] { return f.store->has_active(telemetry_stream_pusch_symbols); }));

  const uint8_t* region_base  = nullptr;
  std::size_t    region_bytes = 0;
  auto           cleanup      = make_scope_exit([&] {
    if (region_base != nullptr) {
      ::munmap(const_cast<uint8_t*>(region_base), region_bytes);
    }
    e3ap_pdu remove;
    remove.id      = 6;
    remove.message = e3ap_subscription_delete{dapp_id, *response->subscription_id};
    EXPECT_TRUE(client.send(remove));
    EXPECT_TRUE(client.receive_as<e3ap_message_ack>());
  });
  if (by_reference) {
    if (!response->shared_region) {
      ADD_FAILURE() << "missing shared region";
      return {};
    }
    const int fd = ::shm_open(response->shared_region->region_name.c_str(), O_RDONLY, 0);
    if (fd < 0) {
      ADD_FAILURE() << "cannot open shared region";
      return {};
    }
    region_bytes = response->shared_region->region_bytes;
    void* base   = ::mmap(nullptr, region_bytes, PROT_READ, MAP_SHARED, fd, 0);
    ::close(fd);
    if (base == MAP_FAILED) {
      ADD_FAILURE() << "cannot map shared region";
      return {};
    }
    region_base = static_cast<const uint8_t*>(base);
  }

  std::vector<std::complex<float>> symbols(symbols_per_object, {1.0F, -1.0F});
  pusch_symbols_diagnostic         event;
  event.rnti              = 0x4601;
  event.slot              = slot_point{1, 20, 3};
  event.nof_prb           = 273;
  event.nof_data_symbols  = 13;
  event.nof_layers        = 4;
  event.mod_bits          = 8;
  event.equalized_symbols = symbols;
  std::vector<double>  latencies;
  std::vector<uint8_t> copy;
  const uint64_t       drops_before = f.publisher->metrics().dropped;
  for (unsigned object = 0; object != objects; ++object) {
    EXPECT_TRUE(f.publisher->should_collect(phy_diagnostic_stream::pusch_symbols, 0x4601));
    const auto     started          = std::chrono::steady_clock::now();
    const uint64_t published_before = f.publisher->metrics().published;
    // Admission is deliberately lossy under contention. Retry only a dropped
    // publication; include that cost in latency and report the drop count.
    if (!wait_until([&] {
          f.publisher->on_pusch_symbols(event);
          return f.publisher->metrics().published > published_before;
        })) {
      ADD_FAILURE() << "publication admission stalled";
      return latencies;
    }
    std::size_t bytes = 0;
    while (true) {
      auto indication = client.receive_as<e3ap_indication_message>();
      if (!indication) {
        ADD_FAILURE() << "indication stream stalled";
        return latencies;
      }
      bytes += indication->protocol_data.size();
      if (indication->fragment_index + 1U == indication->fragment_count) {
        if (by_reference) {
          if (!indication->by_reference ||
              !std::holds_alternative<e3ap_posix_shared_location>(indication->by_reference->location)) {
            ADD_FAILURE() << "expected POSIX by-reference indication";
            return latencies;
          }
          const auto& location = std::get<e3ap_posix_shared_location>(indication->by_reference->location);
          copy.assign(region_base + location.byte_offset,
                      region_base + location.byte_offset + indication->by_reference->byte_length);
          const auto* row_header = reinterpret_cast<const ocudu_e3_shared_row_header_v1*>(
              region_base + 64 + response->shared_region->row_bytes * location.row_index);
          EXPECT_EQ(__atomic_load_n(&row_header->generation, __ATOMIC_ACQUIRE), indication->by_reference->generation);
          bytes += copy.size();
        }
        break;
      }
    }
    EXPECT_GE(bytes, symbols.size() * sizeof(std::complex<float>));
    latencies.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - started).count());
  }
  std::printf("[ admission ] by_reference=%d dropped_attempts=%llu\n",
              by_reference,
              static_cast<unsigned long long>(f.publisher->metrics().dropped - drops_before));
  std::sort(latencies.begin(), latencies.end());
  return latencies;
}

} // namespace

TEST(dapp_e3ap_sctp_server, by_reference_delivery_is_faster_than_in_band_for_large_objects)
{
  // 1.7 MB PUSCH-symbol objects (273 PRB x 12 x 13 symbols x 4 layers, cf32):
  // in band they travel as 27 APER fragments; by reference as one row copy plus
  // a descriptor. Prints the medians; asserts only the ordering.
  fixture f;
  auto    server = e3ap_sctp_server::create(&f.service, f.store, test_server_config(), f.connections);
  ASSERT_TRUE(server) << server.detail;
  test_client client(server.server->port());
  ASSERT_TRUE(client.ok());
  auto dapp_id = client.setup();
  ASSERT_TRUE(dapp_id);
  constexpr unsigned    objects = 40;
  constexpr std::size_t samples = 273 * 12 * 13 * 4;
  const auto            in_band = measure_delivery(f, client, *dapp_id, false, objects, samples);
  ASSERT_EQ(in_band.size(), objects);
  const auto by_ref = measure_delivery(f, client, *dapp_id, true, objects, samples);
  ASSERT_EQ(by_ref.size(), objects);
  const double mb = static_cast<double>(samples * sizeof(std::complex<float>)) / 1e6;
  std::printf("[ latency ] %.2f MB objects, %u each: in-band median %.0f us (p90 %.0f), by-reference median %.0f us "
              "(p90 %.0f)\n",
              mb,
              objects,
              in_band[objects / 2],
              in_band[objects * 9 / 10],
              by_ref[objects / 2],
              by_ref[objects * 9 / 10]);
  std::printf("[ throughput ] in-band %.0f MB/s, by-reference %.0f MB/s (single subscriber, median)\n",
              mb / (in_band[objects / 2] / 1e6),
              mb / (by_ref[objects / 2] / 1e6));
  EXPECT_LT(by_ref[objects / 2], in_band[objects / 2]);
}

TEST(dapp_e3ap_sctp_server, serves_every_encoding_and_profile_on_one_port)
{
  fixture f;
  auto    server = e3ap_sctp_server::create(&f.service, f.store, test_server_config(), f.connections);
  ASSERT_TRUE(server) << server.detail;

  // Four peers, four wire formats, one port: each association is bound by its setup request.
  test_client native(server.server->port(), client_wire::ocudu_aper);
  test_client libe3_aper(server.server->port(), client_wire::libe3_aper);
  test_client libe3_protobuf(server.server->port(), client_wire::libe3_protobuf);
  test_client ocudu_protobuf(server.server->port(), client_wire::ocudu_protobuf);
  for (auto* client : {&native, &libe3_aper, &libe3_protobuf, &ocudu_protobuf}) {
    ASSERT_TRUE(client->ok());
  }
  auto native_id         = native.setup();
  auto libe3_aper_id     = libe3_aper.setup();
  auto libe3_protobuf_id = libe3_protobuf.setup();
  auto ocudu_protobuf_id = ocudu_protobuf.setup();
  ASSERT_TRUE(native_id && libe3_aper_id && libe3_protobuf_id && ocudu_protobuf_id);
  EXPECT_TRUE(wait_until([&] { return server.server->metrics().setups_completed == 4; }));
  EXPECT_EQ(server.server->metrics().libe3_setups_completed, 2U);
  EXPECT_EQ(server.server->metrics().protobuf_setups_completed, 2U);
  // OCUDU-profile peers are told the OCUDU procedures run; libe3 peers never see the field.
  EXPECT_EQ(native.last_setup.ocudu_profile, std::optional<uint8_t>(e3ap_ocudu_profile_version));
  EXPECT_EQ(ocudu_protobuf.last_setup.ocudu_profile, std::optional<uint8_t>(e3ap_ocudu_profile_version));
  EXPECT_FALSE(libe3_aper.last_setup.ocudu_profile);
  EXPECT_FALSE(libe3_protobuf.last_setup.ocudu_profile);

  // The libe3 Protobuf peer runs the libe3 procedures: libe3 identifiers, no OCUDU fields.
  EXPECT_LE(*libe3_protobuf_id, e3ap_libe3_codec::dapp_identifier_pool);
  EXPECT_FALSE(libe3_protobuf.last_setup.maximum_payload_bytes) << "OCUDU fields do not reach a libe3 peer";
  ASSERT_EQ(libe3_protobuf.last_setup.ran_functions.size(), 2U);
  // The OCUDU-profile peer runs the native procedures over Protobuf.
  EXPECT_GT(*ocudu_protobuf_id, e3ap_libe3_codec::dapp_identifier_pool);
  EXPECT_EQ(ocudu_protobuf.last_setup.maximum_payload_bytes, native.last_setup.maximum_payload_bytes);
  EXPECT_EQ(ocudu_protobuf.last_setup.ran_instance.has_value(), native.last_setup.ran_instance.has_value());
  ASSERT_EQ(ocudu_protobuf.last_setup.ran_functions.size(), 2U);
  EXPECT_EQ(ocudu_protobuf.last_setup.ran_functions[0].ran_function_data,
            native.last_setup.ran_functions[0].ran_function_data);

  // Both Protobuf peers subscribe to the CRC stream and receive the same indication.
  e3ap_pdu pdu;
  pdu.id = 5;
  e3ap_subscription_request subscription;
  subscription.ran_function_identifier = e3ap_ran_function_telemetry;
  subscription.telemetry_identifiers   = {telemetry_id_pusch_crc};
  subscription.dapp_identifier         = *libe3_protobuf_id;
  pdu.message                          = subscription;
  ASSERT_TRUE(libe3_protobuf.send(pdu));
  subscription.dapp_identifier = *ocudu_protobuf_id;
  pdu.message                  = subscription;
  ASSERT_TRUE(ocudu_protobuf.send(pdu));
  auto libe3_granted = libe3_protobuf.receive_as<e3ap_subscription_response>();
  auto ocudu_granted = ocudu_protobuf.receive_as<e3ap_subscription_response>();
  ASSERT_TRUE(libe3_granted && ocudu_granted);
  EXPECT_EQ(libe3_granted->response_code, e3ap_response_code::positive);
  EXPECT_EQ(ocudu_granted->response_code, e3ap_response_code::positive);
  EXPECT_FALSE(libe3_granted->telemetry_identifiers) << "libe3's Protobuf SubscriptionResponse has no lists";
  EXPECT_EQ(ocudu_granted->telemetry_identifiers,
            std::optional<std::vector<uint16_t>>(std::vector<uint16_t>{telemetry_id_pusch_crc}));
  EXPECT_TRUE(wait_until([&] { return f.store->has_active(telemetry_stream_pusch_crc); }));
  ASSERT_TRUE(f.publisher->should_collect(phy_diagnostic_stream::pusch_crc, 0x4602));
  // Two subscribers: a record one of them meets busy is lost to it alone, so publish a short burst.
  for (int burst = 0; burst != 8; ++burst) {
    publish_crc(*f.publisher, {0x4602, slot_point{1, 21, 3}, true});
    std::this_thread::sleep_for(std::chrono::milliseconds(3));
  }
  auto libe3_indication = libe3_protobuf.receive_as<e3ap_indication_message>();
  auto ocudu_indication = ocudu_protobuf.receive_as<e3ap_indication_message>();
  ASSERT_TRUE(libe3_indication && ocudu_indication);
  for (const auto* indication : {&*libe3_indication, &*ocudu_indication}) {
    auto record = e3ap_codec::decode_telemetry(indication->protocol_data);
    ASSERT_TRUE(record) << record.detail;
    EXPECT_EQ(record.value->telemetry_id, telemetry_id_pusch_crc);
    EXPECT_TRUE(std::get<pusch_crc_record>(record.value->object).crc_ok);
  }
  EXPECT_FALSE(libe3_indication->subscription_id);
  EXPECT_EQ(ocudu_indication->subscription_id, ocudu_granted->subscription_id) << "OCUDU fields reach OCUDU peers";

  // A management control action: the libe3 peer gets one MessageAck with the outcome, the OCUDU
  // peer the ack and then the ControlResponse that libe3 cannot carry.
  inventory_query query;
  query.header.transaction_id.bytes[15] = 0x44;
  auto request_bytes                    = aper_codec::encode(management_request{query});
  ASSERT_TRUE(request_bytes) << request_bytes.detail;
  pdu.id = 11;
  e3ap_dapp_control_action action;
  action.ran_function_identifier = e3ap_ran_function_management;
  action.control_identifier      = e3ap_management_control_request;
  action.action_data             = *request_bytes.value;
  action.dapp_identifier         = *libe3_protobuf_id;
  pdu.message                    = action;
  ASSERT_TRUE(libe3_protobuf.send(pdu));
  auto libe3_ack = libe3_protobuf.receive_as<e3ap_message_ack>();
  ASSERT_TRUE(libe3_ack);
  EXPECT_EQ(libe3_ack->request_id, 11U);
  EXPECT_EQ(libe3_ack->response_code, e3ap_response_code::positive);
  action.dapp_identifier = *ocudu_protobuf_id;
  action.payload_bytes   = static_cast<uint32_t>(request_bytes.value->size());
  pdu.message            = action;
  ASSERT_TRUE(ocudu_protobuf.send(pdu));
  auto ocudu_ack = ocudu_protobuf.receive_as<e3ap_message_ack>();
  ASSERT_TRUE(ocudu_ack);
  EXPECT_EQ(ocudu_ack->response_code, e3ap_response_code::positive) << ocudu_ack->detail;
  auto outcome = ocudu_protobuf.receive_as<e3ap_control_response>();
  ASSERT_TRUE(outcome);
  EXPECT_EQ(outcome->request_id, 11U);
  EXPECT_EQ(outcome->response_code, e3ap_response_code::positive) << outcome->detail;
  EXPECT_TRUE(aper_codec::decode_response(outcome->control_outcome));

  // A report from each Protobuf peer reaches the agent.
  e3ap_dapp_report report;
  report.ran_function_identifier = e3ap_ran_function_telemetry;
  report.report_data             = {0x01};
  report.dapp_identifier         = *libe3_protobuf_id;
  report.sequence_identifier     = 77;
  pdu.id                         = 12;
  pdu.message                    = report;
  ASSERT_TRUE(libe3_protobuf.send(pdu));
  report.dapp_identifier     = *ocudu_protobuf_id;
  report.sequence_identifier = 78;
  pdu.message                = report;
  ASSERT_TRUE(ocudu_protobuf.send(pdu));
  EXPECT_TRUE(wait_until([&] { return server.server->metrics().reports_received == 2; }));

  // The APER peers were never affected.
  EXPECT_EQ(server.server->metrics().protocol_errors, 0U);
}

TEST(dapp_e3ap_sctp_server, serves_libe3_module_peers_on_the_same_port_beside_native_ones)
{
  fixture f;
  auto    server = e3ap_sctp_server::create(&f.service, f.store, test_server_config(), f.connections);
  ASSERT_TRUE(server) << server.detail;

  test_client native(server.server->port());
  ASSERT_TRUE(native.ok());
  ASSERT_TRUE(native.setup());

  // The libe3 peer sends the upstream E3AP 1.0.0 module: the agent binds the
  // association to that module at setup and answers in it from then on.
  test_client peer(server.server->port(), /*libe3=*/true);
  ASSERT_TRUE(peer.ok());
  auto dapp_id = peer.setup();
  ASSERT_TRUE(dapp_id);
  EXPECT_GE(*dapp_id, 1U);
  EXPECT_LE(*dapp_id, e3ap_libe3_codec::dapp_identifier_pool);
  EXPECT_EQ(peer.last_setup.ran_identifier, "ocudu-gnb");
  EXPECT_EQ(peer.last_setup.e3ap_protocol_version, std::optional<std::string>("1.0.0"));
  EXPECT_FALSE(peer.last_setup.cause.has_value()) << "OCUDU-only fields do not reach a libe3 peer";
  ASSERT_EQ(peer.last_setup.ran_functions.size(), 2U);
  EXPECT_EQ(peer.last_setup.ran_functions[0].ran_function_identifier, e3ap_ran_function_telemetry);
  EXPECT_EQ(peer.last_setup.ran_functions[0].telemetry_identifiers, (std::vector<uint16_t>{1, 2, 3, 4, 5, 6, 7, 8, 9, 10}));
  EXPECT_EQ(peer.last_setup.ran_functions[1].control_identifiers, std::vector<uint16_t>{1});
  auto catalog = e3ap_codec::decode_telemetry_function(peer.last_setup.ran_functions[0].ran_function_data);
  ASSERT_TRUE(catalog) << catalog.detail;
  EXPECT_TRUE(wait_until([&] { return server.server->metrics().libe3_setups_completed == 1; }));
  EXPECT_EQ(server.server->metrics().setups_completed, 2U);

  // A plain libe3 subscription (no OCUDU subscriptionData) to the CRC stream.
  e3ap_pdu pdu;
  pdu.id = 5;
  e3ap_subscription_request subscription;
  subscription.dapp_identifier         = *dapp_id;
  subscription.ran_function_identifier = e3ap_ran_function_telemetry;
  subscription.telemetry_identifiers   = {telemetry_id_pusch_crc};
  pdu.message                          = subscription;
  ASSERT_TRUE(peer.send(pdu));
  auto granted = peer.receive_as<e3ap_subscription_response>();
  ASSERT_TRUE(granted);
  EXPECT_EQ(granted->request_id, 5U);
  EXPECT_EQ(granted->response_code, e3ap_response_code::positive);
  ASSERT_TRUE(granted->subscription_id);
  EXPECT_LE(*granted->subscription_id, e3ap_libe3_codec::maximum_identifier);
  EXPECT_EQ(granted->telemetry_identifiers, std::optional<std::vector<uint16_t>>(std::vector<uint16_t>{2}));
  EXPECT_TRUE(wait_until([&] { return f.store->has_active(telemetry_stream_pusch_crc); }));

  ASSERT_TRUE(f.publisher->should_collect(phy_diagnostic_stream::pusch_crc, 0x4601));
  publish_crc(*f.publisher, {0x4601, slot_point{1, 20, 3}, true});
  auto indication = peer.receive_as<e3ap_indication_message>();
  ASSERT_TRUE(indication);
  EXPECT_EQ(indication->dapp_identifier, *dapp_id);
  EXPECT_EQ(indication->ran_function_identifier, e3ap_ran_function_telemetry);
  EXPECT_EQ(indication->fragment_count, 1U);
  auto record = e3ap_codec::decode_telemetry(indication->protocol_data);
  ASSERT_TRUE(record) << record.detail;
  EXPECT_EQ(record.value->telemetry_id, telemetry_id_pusch_crc);
  EXPECT_TRUE(std::get<pusch_crc_record>(record.value->object).crc_ok);

  // A management control action is answered with exactly one E3-MessageAck
  // carrying the outcome (libe3 has no control response), and a report is
  // acknowledged; the two acks arrive in order with nothing in between.
  inventory_query query;
  query.header.transaction_id.bytes[15] = 0x43;
  auto request_bytes                    = aper_codec::encode(management_request{query});
  ASSERT_TRUE(request_bytes) << request_bytes.detail;
  pdu.id = 11;
  e3ap_dapp_control_action action;
  action.dapp_identifier         = *dapp_id;
  action.ran_function_identifier = e3ap_ran_function_management;
  action.control_identifier      = e3ap_management_control_request;
  action.action_data             = *request_bytes.value;
  pdu.message                    = action;
  ASSERT_TRUE(peer.send(pdu));
  pdu.id = 12;
  e3ap_dapp_report report;
  report.dapp_identifier         = *dapp_id;
  report.ran_function_identifier = e3ap_ran_function_telemetry;
  report.report_data             = {0x01, 0x02};
  report.sequence_identifier     = 4242; // mandatory on the libe3 >= 0.2 wire
  pdu.message                    = report;
  ASSERT_TRUE(peer.send(pdu));
  auto first = peer.receive();
  ASSERT_TRUE(first);
  auto* control_ack = std::get_if<e3ap_message_ack>(&first->message);
  ASSERT_NE(control_ack, nullptr);
  EXPECT_EQ(control_ack->request_id, 11U);
  EXPECT_EQ(control_ack->response_code, e3ap_response_code::positive);
  auto second = peer.receive();
  ASSERT_TRUE(second);
  auto* report_ack = std::get_if<e3ap_message_ack>(&second->message);
  ASSERT_NE(report_ack, nullptr);
  EXPECT_EQ(report_ack->request_id, 12U);
  EXPECT_EQ(report_ack->response_code, e3ap_response_code::positive);
  EXPECT_TRUE(wait_until([&] { return server.server->metrics().reports_received == 1; }));
  EXPECT_EQ(server.server->metrics().control_requests, 1U);

  // libe3 confirms a subscription delete with a subscription response.
  pdu.id = 13;
  e3ap_subscription_delete remove;
  remove.dapp_identifier = *dapp_id;
  remove.subscription_id = *granted->subscription_id;
  pdu.message            = remove;
  ASSERT_TRUE(peer.send(pdu));
  auto confirmed = peer.receive_as<e3ap_subscription_response>();
  ASSERT_TRUE(confirmed);
  EXPECT_EQ(confirmed->request_id, 13U);
  EXPECT_EQ(confirmed->response_code, e3ap_response_code::positive);
  EXPECT_EQ(confirmed->subscription_id, granted->subscription_id);
  EXPECT_TRUE(wait_until([&] { return !f.store->has_active(telemetry_stream_pusch_crc); }));

  // The native peer was never affected.
  EXPECT_TRUE(native.setup() == std::nullopt) << "second setup on the native association is refused, not broken";

  pdu.id = 14;
  e3ap_release_message release;
  release.dapp_identifier = *dapp_id;
  pdu.message             = release;
  ASSERT_TRUE(peer.send(pdu));
  auto released = peer.receive_as<e3ap_message_ack>();
  ASSERT_TRUE(released);
  EXPECT_EQ(released->response_code, e3ap_response_code::positive);
  EXPECT_FALSE(peer.receive()) << "the agent closes the association after a release";
}

TEST(dapp_e3ap_sctp_server, drops_indications_the_libe3_module_cannot_carry)
{
  fixture f;
  auto    server = e3ap_sctp_server::create(&f.service, f.store, test_server_config(), f.connections);
  ASSERT_TRUE(server) << server.detail;
  test_client peer(server.server->port(), /*libe3=*/true);
  ASSERT_TRUE(peer.ok());
  auto dapp_id = peer.setup();
  ASSERT_TRUE(dapp_id);

  e3ap_pdu pdu;
  pdu.id = 5;
  e3ap_subscription_request subscription;
  subscription.dapp_identifier         = *dapp_id;
  subscription.ran_function_identifier = e3ap_ran_function_telemetry;
  subscription.telemetry_identifiers   = {telemetry_id_pusch_symbols, telemetry_id_pusch_crc};
  pdu.message                          = subscription;
  ASSERT_TRUE(peer.send(pdu));
  auto granted = peer.receive_as<e3ap_subscription_response>();
  ASSERT_TRUE(granted);
  EXPECT_EQ(granted->response_code, e3ap_response_code::positive);
  EXPECT_TRUE(wait_until([&] { return f.store->has_active(telemetry_stream_pusch_symbols); }));

  // A 273-PRB symbol object encodes to hundreds of KiB: far beyond libe3's
  // 32 KiB protocolData, so it is dropped and counted; the CRC record that
  // follows it still arrives.
  std::vector<std::complex<float>> symbols(273 * 12 * 13);
  pusch_symbols_diagnostic         event;
  event.rnti              = 0x4601;
  event.slot              = slot_point{1, 20, 3};
  event.nof_prb           = 273;
  event.nof_data_symbols  = 13;
  event.nof_layers        = 1;
  event.mod_bits          = 8;
  event.equalized_symbols = symbols;
  ASSERT_TRUE(f.publisher->should_collect(phy_diagnostic_stream::pusch_symbols, 0x4601));
  f.publisher->on_pusch_symbols(event);
  ASSERT_TRUE(f.publisher->should_collect(phy_diagnostic_stream::pusch_crc, 0x4601));
  publish_crc(*f.publisher, {0x4601, slot_point{1, 20, 3}, false});
  auto indication = peer.receive_as<e3ap_indication_message>();
  ASSERT_TRUE(indication);
  auto record = e3ap_codec::decode_telemetry(indication->protocol_data);
  ASSERT_TRUE(record) << record.detail;
  EXPECT_EQ(record.value->telemetry_id, telemetry_id_pusch_crc);
  EXPECT_TRUE(wait_until([&] { return server.server->metrics().libe3_indications_dropped == 1; }));
  EXPECT_EQ(server.server->metrics().indications_sent, 1U);
}

TEST(dapp_e3ap_sctp_server, native_and_libe3_identifiers_remain_disjoint_during_churn)
{
  fixture f;
  auto    server = e3ap_sctp_server::create(&f.service, f.store, test_server_config(), f.connections);
  ASSERT_TRUE(server);
  test_client foreign(server.server->port(), true);
  const auto  foreign_id = foreign.setup();
  ASSERT_TRUE(foreign_id);
  for (unsigned iteration = 0; iteration != 105; ++iteration) {
    SCOPED_TRACE(iteration);
    ASSERT_TRUE(wait_until([&] { return server.server->metrics().transport.live_associations == 1; }));
    test_client native(server.server->port());
    auto        id = native.setup();
    ASSERT_TRUE(id);
    EXPECT_GT(*id, e3ap_libe3_codec::dapp_identifier_pool);
    EXPECT_NE(*id, *foreign_id);
    EXPECT_FALSE(server.server->send_xapp_control_action(*id, 10, {0}, UINT64_MAX));
    ASSERT_TRUE(server.server->send_xapp_control_action(*id, 10, {1, 2, 3}));
    auto action = native.receive_as<e3ap_xapp_control_action>();
    ASSERT_TRUE(action);
    EXPECT_EQ(action->dapp_identifier, *id);
    EXPECT_EQ(action->xapp_control_data, (std::vector<uint8_t>{1, 2, 3}));
  }
  ASSERT_TRUE(server.server->send_xapp_control_action(*foreign_id, 10, {4, 5, 6}));
  auto action = foreign.receive_as<e3ap_xapp_control_action>();
  ASSERT_TRUE(action);
  EXPECT_EQ(action->xapp_control_data, (std::vector<uint8_t>{4, 5, 6}));
}

// wineslab/libe3#84, #96 and #100: reject invalid selections on both profiles,
// including a valid first stream followed by an invalid one (rollback coverage).
TEST(dapp_e3ap_sctp_server, rejects_unknown_subscription_ids_without_leaking_partial_admission)
{
  for (auto wire :
       {client_wire::ocudu_aper, client_wire::libe3_aper, client_wire::ocudu_protobuf, client_wire::libe3_protobuf}) {
    SCOPED_TRACE(static_cast<int>(wire));
    fixture f;
    auto    server = e3ap_sctp_server::create(&f.service, f.store, test_server_config());
    ASSERT_TRUE(server);
    test_client peer(server.server->port(), wire);
    auto        id = peer.setup();
    ASSERT_TRUE(id);
    e3ap_subscription_request valid;
    valid.dapp_identifier         = *id;
    valid.ran_function_identifier = e3ap_ran_function_telemetry;
    valid.telemetry_identifiers   = {telemetry_id_pusch_crc};
    for (unsigned scenario = 0; scenario != 5; ++scenario) {
      SCOPED_TRACE(scenario);
      auto request = valid;
      auto cause   = e3ap_cause::unsupported_identifier;
      switch (scenario) {
        case 0:
          request.ran_function_identifier = 999;
          cause                           = e3ap_cause::unsupported_ran_function;
          break;
        case 1:
          request.telemetry_identifiers.push_back(999);
          break;
        case 2:
          request.control_identifiers = {999};
          cause                       = e3ap_cause::invalid_request;
          break;
        case 3:
          request.telemetry_identifiers.push_back(telemetry_id_pusch_crc);
          break;
        case 4:
          request.dapp_identifier = *id == 1 ? 2 : 1;
          cause                   = e3ap_cause::invalid_request;
          break;
      }
      e3ap_pdu pdu;
      pdu.id      = 1001 + scenario; // request IDs are not restricted to the old 1..1000 range.
      pdu.message = request;
      ASSERT_TRUE(peer.send(pdu));
      auto reply = peer.receive_as<e3ap_subscription_response>();
      ASSERT_TRUE(reply);
      EXPECT_EQ(reply->request_id, pdu.id);
      EXPECT_EQ(reply->response_code, e3ap_response_code::negative);
      EXPECT_FALSE(reply->subscription_id);
      if (peer.ocudu_profile()) {
        EXPECT_EQ(reply->cause, cause);
        EXPECT_FALSE(reply->detail.empty());
      } else {
        EXPECT_FALSE(reply->cause);
        EXPECT_TRUE(reply->detail.empty());
      }
      EXPECT_EQ(server.server->metrics().live_subscriptions, 0U);
      EXPECT_FALSE(f.store->has_active(telemetry_stream_pusch_crc));
    }
    // Rejection must not poison the association or consume its admission capacity.
    e3ap_pdu pdu;
    pdu.id      = 1010;
    pdu.message = valid;
    ASSERT_TRUE(peer.send(pdu));
    auto accepted = peer.receive_as<e3ap_subscription_response>();
    ASSERT_TRUE(accepted);
    EXPECT_EQ(accepted->response_code, e3ap_response_code::positive);
    EXPECT_TRUE(f.store->has_active(telemetry_stream_pusch_crc));
  }
}

// wineslab/libe3#88 and #97: document OCUDU's bounded multi-subscription policy;
// matching this policy is not a claim that libe3's own RAN uses the same one.
TEST(dapp_e3ap_sctp_server, repeat_subscriptions_report_grants_and_preserve_existing_ids_at_capacity)
{
  for (auto wire :
       {client_wire::ocudu_aper, client_wire::libe3_aper, client_wire::ocudu_protobuf, client_wire::libe3_protobuf}) {
    SCOPED_TRACE(static_cast<int>(wire));
    fixture f;
    auto    config                        = test_server_config();
    config.maximum_subscriptions_per_dapp = 2;
    auto server                           = e3ap_sctp_server::create(&f.service, f.store, config);
    ASSERT_TRUE(server);
    test_client peer(server.server->port(), wire);
    auto        id = peer.setup();
    ASSERT_TRUE(id);
    e3ap_subscription_request request;
    request.dapp_identifier         = *id;
    request.ran_function_identifier = e3ap_ran_function_telemetry;
    request.telemetry_identifiers   = {telemetry_id_pusch_crc};
    e3ap_pdu pdu;
    pdu.id      = 2;
    pdu.message = request;
    std::vector<uint16_t> granted_ids;
    for (unsigned attempt = 0; attempt != 3; ++attempt) {
      ASSERT_TRUE(peer.send(pdu));
      auto reply = peer.receive_as<e3ap_subscription_response>();
      ASSERT_TRUE(reply);
      EXPECT_EQ(reply->request_id, pdu.id);
      if (attempt == 2) {
        EXPECT_EQ(reply->response_code, e3ap_response_code::negative);
        EXPECT_FALSE(reply->subscription_id);
        if (peer.ocudu_profile()) {
          EXPECT_EQ(reply->cause, e3ap_cause::resource_exhausted);
        }
      } else {
        ASSERT_EQ(reply->response_code, e3ap_response_code::positive);
        ASSERT_TRUE(reply->subscription_id);
        if (wire == client_wire::libe3_protobuf) {
          // Upstream Protobuf has no granted-list fields. This is a documented
          // #88 limitation, not permission to add OCUDU fields to that profile.
          EXPECT_FALSE(reply->telemetry_identifiers);
          EXPECT_FALSE(reply->control_identifiers);
        } else {
          EXPECT_EQ(reply->telemetry_identifiers, std::optional<std::vector<uint16_t>>(request.telemetry_identifiers));
          EXPECT_EQ(reply->control_identifiers, std::optional<std::vector<uint16_t>>(std::vector<uint16_t>{}));
        }
        granted_ids.push_back(*reply->subscription_id);
      }
      ++pdu.id;
    }
    ASSERT_EQ(granted_ids.size(), 2U);
    EXPECT_NE(granted_ids[0], granted_ids[1]);
    EXPECT_EQ(server.server->metrics().live_subscriptions, 2U);
    for (uint16_t subscription_id : granted_ids) {
      pdu.message = e3ap_subscription_delete{*id, subscription_id};
      ASSERT_TRUE(peer.send(pdu));
      if (peer.ocudu_profile()) {
        auto reply = peer.receive_as<e3ap_message_ack>();
        ASSERT_TRUE(reply);
        EXPECT_EQ(reply->response_code, e3ap_response_code::positive);
      } else {
        auto reply = peer.receive_as<e3ap_subscription_response>();
        ASSERT_TRUE(reply);
        EXPECT_EQ(reply->response_code, e3ap_response_code::positive);
        EXPECT_EQ(reply->subscription_id, subscription_id);
      }
      ++pdu.id;
    }
    EXPECT_EQ(server.server->metrics().live_subscriptions, 0U);
    EXPECT_FALSE(f.store->has_active(telemetry_stream_pusch_crc));
  }
}

TEST(dapp_e3ap_sctp_server, subscription_wrap_never_replaces_a_live_libe3_subscription)
{
  fixture f;
  auto    server = e3ap_sctp_server::create(&f.service, f.store, test_server_config(), f.connections);
  ASSERT_TRUE(server);
  test_client peer(server.server->port(), true);
  auto        id = peer.setup();
  ASSERT_TRUE(id);
  e3ap_subscription_request request;
  request.dapp_identifier         = *id;
  request.ran_function_identifier = e3ap_ran_function_telemetry;
  request.telemetry_identifiers   = {telemetry_id_pusch_crc};
  e3ap_pdu pdu;
  pdu.id      = 2;
  pdu.message = request;
  ASSERT_TRUE(peer.send(pdu));
  auto held = peer.receive_as<e3ap_subscription_response>();
  ASSERT_TRUE(held && held->subscription_id);
  for (unsigned iteration = 0; iteration != 105; ++iteration) {
    pdu.message = request;
    ASSERT_TRUE(peer.send(pdu));
    auto added = peer.receive_as<e3ap_subscription_response>();
    ASSERT_TRUE(added && added->subscription_id);
    ASSERT_EQ(added->response_code, e3ap_response_code::positive);
    EXPECT_NE(added->subscription_id, held->subscription_id);
    pdu.message = e3ap_subscription_delete{*id, *added->subscription_id};
    ASSERT_TRUE(peer.send(pdu));
    ASSERT_TRUE(peer.receive_as<e3ap_subscription_response>());
  }
  EXPECT_EQ(server.server->metrics().live_subscriptions, 1U);
  pdu.message = e3ap_subscription_delete{*id, *held->subscription_id};
  ASSERT_TRUE(peer.send(pdu));
  auto removed = peer.receive_as<e3ap_subscription_response>();
  ASSERT_TRUE(removed);
  EXPECT_EQ(removed->response_code, e3ap_response_code::positive);
}

TEST(dapp_e3ap_sctp_server, shared_region_budget_rejects_and_reclaims_without_store_leaks)
{
  fixture f;
  auto    config                     = test_server_config();
  config.maximum_payload_bytes       = 65536;
  config.shared_region_rows          = 1;
  config.maximum_shared_region_bytes = 65664;
  auto server                        = e3ap_sctp_server::create(&f.service, f.store, config, f.connections);
  ASSERT_TRUE(server);
  test_client peer(server.server->port());
  auto        id = peer.setup();
  ASSERT_TRUE(id);
  e3ap_subscription_request request;
  request.dapp_identifier         = *id;
  request.ran_function_identifier = e3ap_ran_function_telemetry;
  request.telemetry_identifiers   = {telemetry_id_pusch_crc};
  request.delivery_mode           = e3ap_delivery_mode::by_reference;
  e3ap_pdu pdu;
  pdu.id      = 3;
  pdu.message = request;
  ASSERT_TRUE(peer.send(pdu));
  auto first = peer.receive_as<e3ap_subscription_response>();
  ASSERT_TRUE(first && first->subscription_id && first->shared_region);
  ASSERT_TRUE(peer.send(pdu));
  auto rejected = peer.receive_as<e3ap_subscription_response>();
  ASSERT_TRUE(rejected);
  EXPECT_EQ(rejected->cause, e3ap_cause::resource_exhausted);
  EXPECT_EQ(f.store->subscriptions().subscriptions.size(), 1U);
  pdu.message = e3ap_subscription_delete{*id, *first->subscription_id};
  ASSERT_TRUE(peer.send(pdu));
  ASSERT_TRUE(peer.receive_as<e3ap_message_ack>());
  pdu.message = request;
  ASSERT_TRUE(peer.send(pdu));
  auto replacement = peer.receive_as<e3ap_subscription_response>();
  ASSERT_TRUE(replacement && replacement->shared_region);
  EXPECT_EQ(replacement->response_code, e3ap_response_code::positive);
}

TEST(dapp_e3ap_sctp_server, shared_region_creation_does_not_unlink_an_existing_owner)
{
  e3_shared_region_config config;
  config.name              = "/ocudu-e3-owner-test-" + std::to_string(::getpid());
  config.row_payload_bytes = 128;
  config.nof_rows          = 1;
  auto first               = e3_shared_region::create(config);
  ASSERT_TRUE(first.first) << first.second;
  const std::array<uint8_t, 4> payload{1, 2, 3, 4};
  auto                         written = first.first->write(1, 1, 1, 0, payload);
  ASSERT_TRUE(written);
  EXPECT_EQ(first.first->bytes_of(*written).size(), payload.size());
  auto invalid        = *written;
  invalid.byte_offset = UINT64_MAX;
  EXPECT_TRUE(first.first->bytes_of(invalid).empty());
  invalid            = *written;
  invalid.generation = 1;
  EXPECT_TRUE(first.first->bytes_of(invalid).empty());
  auto second = e3_shared_region::create(config);
  EXPECT_FALSE(second.first);
  int fd = ::shm_open(config.name.c_str(), O_RDONLY, 0);
  ASSERT_GE(fd, 0);
  ::close(fd);
  config.name = "/../bad";
  EXPECT_FALSE(e3_shared_region::create(config).first);
}
