// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/e3/embedded_agent.h"
#include "ocudu/dapp/e3/local_connector.h"
#include "ocudu/dapp/e3/management_service.h"
#include "ocudu/scheduler/dapp/quiet_reservation.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <gtest/gtest.h>
#include <limits>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

using namespace ocudu::dapp;
using namespace ocudu::dapp::e3;
using namespace ocudu::dapp::management;

namespace {

identifier test_id(uint8_t value)
{
  identifier result;
  result.bytes[15] = value;
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

class temporary_endpoint
{
public:
  temporary_endpoint()
  {
    std::array<char, 64> pattern{};
    std::strcpy(pattern.data(), "/tmp/ocudu-e3-test-XXXXXX");
    char* created = ::mkdtemp(pattern.data());
    if (created != nullptr) {
      directory = created;
      path      = directory + "/management.sock";
    }
  }

  ~temporary_endpoint()
  {
    if (!path.empty()) {
      static_cast<void>(::unlink(path.c_str()));
    }
    if (!directory.empty()) {
      static_cast<void>(::rmdir(directory.c_str()));
    }
  }

  std::string directory;
  std::string path;
};

inventory_query inventory(uint8_t transaction)
{
  inventory_query result;
  result.header.transaction_id = test_id(transaction);
  return result;
}

template <typename Predicate>
local_server_metrics wait_for_metrics(local_management_server& server, Predicate&& ready)
{
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
  auto       metrics  = server.metrics();
  while (!ready(metrics) && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    metrics = server.metrics();
  }
  return metrics;
}

} // namespace

TEST(dapp_e3_local_connector, serves_credentialed_aper_requests_and_owns_its_path)
{
  temporary_endpoint endpoint;
  ASSERT_FALSE(endpoint.path.empty());
  auto               manager = make_manager();
  embedded_agent     agent(*manager, 8, 32);
  management_service service(agent);
  auto created = local_management_server::create(service, {endpoint.path, ::geteuid(), std::chrono::seconds(1)});
  ASSERT_TRUE(created) << created.detail;

  auto duplicate = local_management_server::create(service, {endpoint.path, ::geteuid(), std::chrono::seconds(1)});
  EXPECT_FALSE(duplicate);

  local_management_client client(endpoint.path);
  auto                    response = client.call(inventory(1));
  ASSERT_TRUE(response) << response.detail;
  EXPECT_EQ(response.value->code, result_code::ok);
  EXPECT_EQ(response.value->transaction_id, test_id(1));
  EXPECT_TRUE(std::holds_alternative<inventory_result>(response.value->payload));
  const auto metrics =
      wait_for_metrics(*created.server, [](const auto& value) { return value.completed_requests == 1; });
  EXPECT_EQ(metrics.completed_requests, 1U);

  created.server.reset();
  EXPECT_NE(::access(endpoint.path.c_str(), F_OK), 0);
}

TEST(dapp_e3_local_connector, rejects_bad_payloads_and_continues_serving)
{
  temporary_endpoint endpoint;
  auto               manager = make_manager();
  embedded_agent     agent(*manager, 8, 32);
  management_service service(agent);
  auto created = local_management_server::create(service, {endpoint.path, ::geteuid(), std::chrono::seconds(1)});
  ASSERT_TRUE(created) << created.detail;

  int descriptor = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
  ASSERT_GE(descriptor, 0);
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, endpoint.path.c_str(), endpoint.path.size() + 1);
  ASSERT_EQ(::connect(descriptor, reinterpret_cast<const sockaddr*>(&address), sizeof(address)), 0);
  const uint8_t malformed[] = {0xff, 0x00, 0x01};
  EXPECT_EQ(::send(descriptor, malformed, sizeof(malformed), MSG_NOSIGNAL), sizeof(malformed));
  ::close(descriptor);

  local_management_client client(endpoint.path);
  auto                    response = client.call(inventory(2));
  ASSERT_TRUE(response) << response.detail;
  EXPECT_EQ(response.value->code, result_code::ok);
  auto metrics = wait_for_metrics(*created.server, [](const auto& value) {
    return value.malformed_requests == 1 && value.completed_requests == 1;
  });
  EXPECT_EQ(metrics.malformed_requests, 1U);
  EXPECT_EQ(metrics.completed_requests, 1U);
}

TEST(dapp_e3_local_connector, enforces_kernel_peer_uid)
{
  temporary_endpoint endpoint;
  auto               manager = make_manager();
  embedded_agent     agent(*manager, 8, 32);
  management_service service(agent);
  const uid_t rejected_uid = ::geteuid() == std::numeric_limits<uid_t>::max() ? ::geteuid() - 1 : ::geteuid() + 1;
  auto        created =
      local_management_server::create(service, {endpoint.path, rejected_uid, std::chrono::milliseconds(100)});
  ASSERT_TRUE(created) << created.detail;

  local_management_client client(endpoint.path);
  EXPECT_FALSE(client.call(inventory(3), std::chrono::milliseconds(250)));
  const auto metrics =
      wait_for_metrics(*created.server, [](const auto& value) { return value.rejected_credentials == 1; });
  EXPECT_EQ(metrics.rejected_credentials, 1U);
  EXPECT_EQ(metrics.completed_requests, 0U);
}

TEST(dapp_e3_local_connector, carries_authoritative_quiet_reservation_snapshots)
{
  temporary_endpoint      endpoint;
  auto                    manager = make_manager();
  embedded_agent          agent(*manager, 8, 32);
  std::array<uint16_t, 1> widths{52};
  auto                    reservations = ocudu::create_quiet_reservation_manager(widths);
  ASSERT_NE(reservations, nullptr);
  management_service service(agent, reservations);
  auto created = local_management_server::create(service, {endpoint.path, ::geteuid(), std::chrono::seconds(1)});
  ASSERT_TRUE(created) << created.detail;

  const uint64_t now = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
          .count());
  ocudu::quiet_reservation_request request{};
  request.intent.struct_size         = sizeof(request.intent);
  request.intent.abi_major           = OCUDU_DAPP_ABI_MAJOR_V1;
  request.intent.request_id[0]       = 1;
  request.intent.earliest_start      = {1, 8, 0, 0};
  request.intent.latest_start        = request.intent.earliest_start;
  request.intent.duration_slots      = 1;
  request.intent.repeat_count        = 1;
  request.intent.first_prb           = 4;
  request.intent.nof_prbs            = 2;
  request.intent.confidence_q16      = 1U << 15U;
  request.intent.expiry_monotonic_ns = now + 1'000'000'000;
  const auto source                  = test_id(9);
  std::memcpy(request.source_instance_id.data(), source.bytes.data(), source.bytes.size());
  request.source_generation    = 1;
  request.source_sequence      = 2;
  request.observed_slot        = ocudu::slot_point{0, 1, 4};
  request.publish_monotonic_ns = now;
  ASSERT_EQ(reservations->submit(request).result, ocudu::quiet_reservation_result::accepted);

  quiet_reservation_query query;
  query.header.transaction_id = test_id(10);
  query.header.instance_id    = source;
  local_management_client client(endpoint.path);
  auto                    response = client.call(query);
  ASSERT_TRUE(response) << response.detail;
  ASSERT_EQ(response.value->code, result_code::ok);
  const auto& payload = std::get<quiet_reservation_result>(response.value->payload);
  ASSERT_EQ(payload.reservations.size(), 1U);
  EXPECT_EQ(payload.reservations[0].reservation.first_prb, 4U);
  const auto metrics =
      wait_for_metrics(*created.server, [](const auto& value) { return value.completed_requests == 1; });
  EXPECT_EQ(metrics.completed_requests, 1U);
}

TEST(dapp_e3_local_connector, serializes_concurrent_clients_without_crossing_transactions)
{
  temporary_endpoint endpoint;
  auto               manager = make_manager();
  embedded_agent     agent(*manager, 16, 64);
  management_service service(agent);
  auto created = local_management_server::create(service, {endpoint.path, ::geteuid(), std::chrono::seconds(2)});
  ASSERT_TRUE(created) << created.detail;

  constexpr std::size_t          count = 8;
  std::array<bool, count>        success{};
  std::array<std::thread, count> clients;
  for (std::size_t index = 0; index != count; ++index) {
    clients[index] = std::thread([&, index] {
      local_management_client client(endpoint.path);
      auto response  = client.call(inventory(static_cast<uint8_t>(index + 10)), std::chrono::seconds(2));
      success[index] = response && response.value->transaction_id == test_id(static_cast<uint8_t>(index + 10));
    });
  }
  for (auto& client : clients) {
    client.join();
  }
  EXPECT_TRUE(std::all_of(success.begin(), success.end(), [](bool value) { return value; }));
  const auto metrics =
      wait_for_metrics(*created.server, [](const auto& value) { return value.completed_requests == count; });
  EXPECT_EQ(metrics.completed_requests, count);
}
