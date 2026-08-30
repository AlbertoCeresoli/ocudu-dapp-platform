// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/e3/e3dp_sctp_server.h"
#include "ocudu/dapp/e3/embedded_agent.h"
#include "ocudu/dapp/e3/fbs_codec.h"
#include "ocudu/dapp/e3/management_service.h"
#include "ocudu/dapp/e3/telemetry_publisher.h"
#include "ocudu/dapp/runtime/instance_manager.h"
#include "ocudu/dapp/use_cases/v1/spectrum.h"
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <netinet/sctp.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

using namespace ocudu;
using namespace ocudu::dapp;
using namespace ocudu::dapp::e3;

namespace {

/// Blocking data-profile client following the reference-client transport discipline.
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

  codec_result<management_response> call(const management_request& request)
  {
    auto encoded = fbs_codec::encode(request);
    if (!encoded) {
      return {std::nullopt, encoded.detail};
    }
    if (::send(descriptor, encoded.value->data(), encoded.value->size(), MSG_NOSIGNAL) !=
        static_cast<ssize_t>(encoded.value->size())) {
      return {std::nullopt, "send failed"};
    }
    std::vector<uint8_t> message;
    std::vector<uint8_t> chunk(64 * 1024);
    while (true) {
      iovec  vector{chunk.data(), chunk.size()};
      msghdr header{};
      header.msg_iov         = &vector;
      header.msg_iovlen      = 1;
      const ssize_t received = ::recvmsg(descriptor, &header, 0);
      if (received <= 0) {
        return {std::nullopt, "receive failed"};
      }
      if ((header.msg_flags & MSG_NOTIFICATION) != 0) {
        continue;
      }
      message.insert(message.end(), chunk.begin(), chunk.begin() + received);
      if ((header.msg_flags & MSG_EOR) != 0) {
        break;
      }
    }
    return fbs_codec::decode_response(message);
  }

  bool send_raw(const std::vector<uint8_t>& bytes)
  {
    return ::send(descriptor, bytes.data(), bytes.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(bytes.size());
  }

  std::optional<management_response> receive()
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
        last_error = received == 0 ? "peer closed" : std::strerror(errno);
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
    auto decoded = fbs_codec::decode_response(message);
    last_error   = decoded.detail;
    return decoded ? std::optional<management_response>(*decoded.value) : std::nullopt;
  }

  std::string last_error;

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
  config.maximum_subscriptions  = 8;
  config.maximum_retained_bytes = 64 * 1024 * 1024;
  config.maximum_object_bytes   = 4 * 1024 * 1024;
  config.maximum_metadata_bytes = 64 * 1024;
  config.minimum_chunk_bytes    = 1024;
  config.maximum_chunk_bytes    = 64 * 1024;
  config.minimum_lease_us       = 1;
  config.maximum_lease_us       = 60'000'000;
  auto created                  = portable_data_store::create(config);
  EXPECT_TRUE(created.first) << created.second;
  return created.first;
}

management::identifier test_id(uint8_t value)
{
  management::identifier result;
  result.bytes[0]  = 0x40;
  result.bytes[15] = value;
  return result;
}

} // namespace

TEST(dapp_e3dp_sctp_server, serves_reference_client_subscription_and_poll_flow)
{
  auto                    manager = make_manager();
  embedded_agent          agent(*manager, 8, 32);
  auto                    store = make_store();
  management_service      service(agent, {}, store);
  e3dp_sctp_server_config config;
  config.transport.port         = 0;
  config.transport.timer_period = std::chrono::milliseconds(5);
  config.queue_depth_chunks     = 64;
  config.cells                  = {{0, 3'500'000'000ULL, -10'000'000}};
  auto server                   = e3dp_sctp_server::create(service, store, config);
  ASSERT_TRUE(server) << server.detail;
  ASSERT_NE(server.server->port(), 0U);

  test_client client(server.server->port());
  ASSERT_TRUE(client.ok());

  data_subscription_control create;
  create.header.transaction_id = test_id(1);
  create.action                = data_subscription_action::create;
  create.subscription_id       = test_id(0x40);
  create.interface_id          = telemetry_stream_pusch_crc;
  create.sample_every_n        = 1;
  create.lease_us              = 5'000'000;
  auto response                = client.call(create);
  ASSERT_TRUE(response) << response.detail;
  EXPECT_EQ(response.value->transaction_id, test_id(1));
  ASSERT_EQ(response.value->code, management::result_code::ok) << response.value->error_text;
  const auto& records = std::get<data_subscription_result>(response.value->payload).subscriptions;
  ASSERT_EQ(records.size(), 1U);
  EXPECT_EQ(records[0].request.queue_depth_chunks, 64U) << "queue depth is server-assigned";
  EXPECT_EQ(records[0].rf_center_frequency_hz, 3'500'000'000ULL);
  EXPECT_EQ(records[0].cell_frequency_offset_hz, -10'000'000);
  EXPECT_TRUE(store->has_active(telemetry_stream_pusch_crc));

  // Byte-identical create renews; a different parameter set conflicts.
  create.header.transaction_id = test_id(2);
  response                     = client.call(create);
  ASSERT_TRUE(response) << response.detail;
  EXPECT_EQ(response.value->code, management::result_code::ok);
  create.header.transaction_id = test_id(3);
  create.sample_every_n        = 2;
  response                     = client.call(create);
  ASSERT_TRUE(response) << response.detail;
  EXPECT_EQ(response.value->code, management::result_code::conflict);

  telemetry_publisher_config publisher_config;
  publisher_config.store                      = store;
  publisher_config.pusch_crc_minimum_interval = std::chrono::microseconds(0);
  auto publisher                              = telemetry_publisher::create(publisher_config).first;
  ASSERT_TRUE(publisher);
  ASSERT_TRUE(publisher->should_collect(phy_diagnostic_stream::pusch_crc, 0x4601));
  publisher->on_pusch_crc({0x4601, slot_point{1, 5, 2}, true});

  data_poll_query poll;
  poll.header.transaction_id = test_id(4);
  poll.subscription_id       = test_id(0x40);
  poll.maximum_chunks        = 8;
  response                   = client.call(poll);
  ASSERT_TRUE(response) << response.detail;
  ASSERT_EQ(response.value->code, management::result_code::ok) << response.value->error_text;
  const auto& publication = std::get<data_publication_result>(response.value->payload);
  ASSERT_EQ(publication.chunks.size(), 1U);
  EXPECT_EQ(publication.chunks[0].chunk_count, 1U);
  EXPECT_EQ(publication.chunks[0].metadata.size(), sizeof(pusch_crc_metadata_v1));
  EXPECT_EQ(publication.chunks[0].tensor_chunk.size(), 1U);
  ASSERT_TRUE(publication.subscription);
  EXPECT_EQ(publication.subscription->rf_center_frequency_hz, 3'500'000'000ULL);

  // An empty queue answers ok with zero chunks, and unknown ids answer notFound.
  poll.header.transaction_id = test_id(5);
  response                   = client.call(poll);
  ASSERT_TRUE(response) << response.detail;
  EXPECT_EQ(response.value->code, management::result_code::ok);
  EXPECT_TRUE(std::get<data_publication_result>(response.value->payload).chunks.empty());
  poll.header.transaction_id = test_id(6);
  poll.subscription_id       = test_id(0x41);
  response                   = client.call(poll);
  ASSERT_TRUE(response) << response.detail;
  EXPECT_EQ(response.value->code, management::result_code::not_found);

  data_subscription_control remove;
  remove.header.transaction_id = test_id(7);
  remove.action                = data_subscription_action::remove;
  remove.subscription_id       = test_id(0x40);
  response                     = client.call(remove);
  ASSERT_TRUE(response) << response.detail;
  EXPECT_EQ(response.value->code, management::result_code::ok);
  EXPECT_FALSE(store->has_active(telemetry_stream_pusch_crc));

  const auto metrics = server.server->metrics();
  EXPECT_EQ(metrics.subscription_controls, 4U);
  EXPECT_EQ(metrics.polls, 3U);
  EXPECT_EQ(metrics.malformed_requests, 0U);
}

TEST(dapp_e3dp_sctp_server, answers_semantic_errors_and_keeps_the_association_open)
{
  auto                    manager = make_manager();
  embedded_agent          agent(*manager, 8, 32);
  auto                    store = make_store();
  management_service      service(agent, {}, store);
  e3dp_sctp_server_config config;
  config.transport.port = 0;
  auto server           = e3dp_sctp_server::create(service, store, config);
  ASSERT_TRUE(server) << server.detail;
  test_client client(server.server->port());
  ASSERT_TRUE(client.ok());

  // A message whose size prefix lies about its length is a semantic error.
  ASSERT_TRUE(client.send_raw({0x10, 0x00, 0x00, 0x00, 0x45, 0x33, 0x44, 0x50}));
  auto reply = client.receive();
  ASSERT_TRUE(reply) << client.last_error;
  EXPECT_EQ(reply->code, management::result_code::invalid_request);

  // The association is still usable afterwards.
  data_subscription_control list;
  list.header.transaction_id = test_id(9);
  list.action                = data_subscription_action::list;
  auto response              = client.call(list);
  ASSERT_TRUE(response) << response.detail;
  EXPECT_EQ(response.value->transaction_id, test_id(9));
  EXPECT_EQ(response.value->code, management::result_code::ok);
  EXPECT_EQ(server.server->metrics().malformed_requests, 1U);
}

TEST(dapp_e3dp_sctp_server, reclaims_subscriptions_when_the_association_closes)
{
  auto                    manager = make_manager();
  embedded_agent          agent(*manager, 8, 32);
  auto                    store = make_store();
  management_service      service(agent, {}, store);
  e3dp_sctp_server_config config;
  config.transport.port                        = 0;
  config.transport.timer_period                = std::chrono::milliseconds(5);
  config.queue_depth_chunks                    = 64;
  config.maximum_subscriptions_per_association = 2;
  config.cells                                 = {{0, 3'500'000'000ULL, -10'000'000}};
  auto server                                  = e3dp_sctp_server::create(service, store, config);
  ASSERT_TRUE(server) << server.detail;

  {
    test_client first(server.server->port());
    ASSERT_TRUE(first.ok());
    data_subscription_control create;
    create.header.transaction_id = test_id(1);
    create.action                = data_subscription_action::create;
    create.subscription_id       = test_id(0x50);
    create.interface_id          = telemetry_stream_pusch_crc;
    create.sample_every_n        = 1;
    create.lease_us              = 5'000'000;
    auto response                = first.call(create);
    ASSERT_TRUE(response) << response.detail;
    ASSERT_EQ(response.value->code, management::result_code::ok) << response.value->error_text;
    create.header.transaction_id = test_id(2);
    create.subscription_id       = test_id(0x51);
    response                     = first.call(create);
    ASSERT_TRUE(response) << response.detail;
    ASSERT_EQ(response.value->code, management::result_code::ok);

    // The per-association bound refuses a third subscription outright.
    create.header.transaction_id = test_id(3);
    create.subscription_id       = test_id(0x52);
    response                     = first.call(create);
    ASSERT_TRUE(response) << response.detail;
    EXPECT_NE(response.value->code, management::result_code::ok);
    EXPECT_TRUE(store->has_active(telemetry_stream_pusch_crc));
  } // first association closes here

  // A fresh association observes the closed peer's subscriptions reclaimed:
  // the store frees the slots, and the same ids can be created again.
  test_client second(server.server->port());
  ASSERT_TRUE(second.ok());
  data_subscription_control list;
  list.header.transaction_id = test_id(4);
  list.action                = data_subscription_action::list;
  bool reclaimed             = false;
  for (int attempt = 0; attempt != 100 && !reclaimed; ++attempt) {
    auto response = second.call(list);
    ASSERT_TRUE(response) << response.detail;
    ASSERT_EQ(response.value->code, management::result_code::ok);
    reclaimed = std::get<data_subscription_result>(response.value->payload).subscriptions.empty();
    if (!reclaimed) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
  EXPECT_TRUE(reclaimed) << "closed association's subscriptions were not reclaimed";

  data_subscription_control create;
  create.header.transaction_id = test_id(5);
  create.action                = data_subscription_action::create;
  create.subscription_id       = test_id(0x50);
  create.interface_id          = telemetry_stream_pusch_crc;
  create.sample_every_n        = 1;
  create.lease_us              = 5'000'000;
  auto response                = second.call(create);
  ASSERT_TRUE(response) << response.detail;
  EXPECT_EQ(response.value->code, management::result_code::ok) << response.value->error_text;
}
