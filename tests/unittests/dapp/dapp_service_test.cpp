// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "../phy/support/resource_grid_test_doubles.h"
#include "apps/services/dapp/dapp_service.h"
#include "ocudu/dapp/ipc/class_c_metadata.h"
#include "ocudu/dapp/runtime/native_hook_registry.h"
#include "ocudu/dapp/use_cases/v1/channel_estimator.h"
#include "ocudu/dapp/use_cases/v1/equalizer.h"
#include "ocudu/dapp/use_cases/v1/receiver.h"
#include "ocudu/dapp/use_cases/v1/scheduler.h"
#include "ocudu/dapp/use_cases/v1/spectrum.h"
#include "ocudu/dapp/use_cases/v1/srs_isac.h"
#include "ocudu/phy/upper/dapp/upper_phy_dapp_hook_provider.h"
#include "ocudu/phy/upper/phy_tap/phy_tap.h"
#include "ocudu/ran/du_cell_index.h"
#include <array>
#include <cstring>
#include <gtest/gtest.h>
#include <unistd.h>

#ifdef OCUDU_DAPP_TEST_LOCAL_E3
#include "ocudu/dapp/e3/local_client.h"
#endif

using namespace ocudu;
using namespace ocudu::app_services;

namespace {

dapp_service_config service_config()
{
  dapp_service_config result;
  result.catalog_root                            = DAPP_TEST_CATALOG;
  result.native_preflight_helper                 = DAPP_PREFLIGHT_HELPER;
  result.package_signatures.mode                 = dapp::package_signature_mode::allow_unsigned;
  result.package_signatures.minimum_policy_epoch = 0;
  result.cells                                   = {{3, 4, 52}, {8, 2, 24}};
  result.nof_pusch_lanes                         = 6;
  result.nof_srs_lanes                           = 3;
  result.nof_scheduler_lanes                     = 5;
  result.maximum_transactions                    = 32;
  result.maximum_queued_jobs                     = 8;
  result.maximum_retained_jobs                   = 32;
  return result;
}

#ifdef OCUDU_DAPP_TEST_LOCAL_E3
class temporary_endpoint
{
public:
  temporary_endpoint()
  {
    std::array<char, 64> pattern{};
    std::strcpy(pattern.data(), "/tmp/ocudu-dapp-service-XXXXXX");
    if (char* created = ::mkdtemp(pattern.data()); created != nullptr) {
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

class contiguous_e3_reader final : public resource_grid_reader_spy
{
public:
  contiguous_e3_reader() :
    resource_grid_reader_spy(1, 14, 1), storage(14 * NOF_SUBCARRIERS_PER_RB, to_cbf16(cf_t{0.5F, -0.25F}))
  {
  }
  span<const cbf16_t> get_contiguous_view() const override { return storage; }
  span<const cbf16_t> view() const { return storage; }

private:
  std::vector<cbf16_t> storage;
};
#endif

} // namespace

TEST(dapp_service, constructs_one_bounded_hook_topology_for_all_six_interfaces)
{
  auto created = dapp_service::create(service_config());
  ASSERT_TRUE(created) << created.detail;
  EXPECT_NE(created.service->upper_phy_hooks(), nullptr);
  EXPECT_NE(created.service->scheduler_hooks(), nullptr);
  EXPECT_NE(created.service->interference_cache(), nullptr);

  auto registry = created.service->native_hooks();
  ASSERT_NE(registry, nullptr);
  EXPECT_EQ(registry->find(OCUDU_DAPP_INTERFACE_SPECTRUM_V1)->lane_count(), 6U);
  EXPECT_EQ(registry->find(OCUDU_DAPP_INTERFACE_SRS_ISAC_V1)->lane_count(), 3U);
  EXPECT_EQ(registry->find(OCUDU_DAPP_INTERFACE_CHANNEL_ESTIMATOR_V1)->lane_count(), 6U);
  EXPECT_EQ(registry->find(OCUDU_DAPP_INTERFACE_EQUALIZER_V1)->lane_count(), 6U);
  EXPECT_EQ(registry->find(OCUDU_DAPP_INTERFACE_RECEIVER_V1)->lane_count(), 6U);
  EXPECT_EQ(registry->find(OCUDU_DAPP_INTERFACE_SCHEDULER_V1)->lane_count(), 5U);
#ifdef OCUDU_DAPP_TEST_E3
  EXPECT_TRUE(created.service->has_embedded_e3());
#else
  EXPECT_FALSE(created.service->has_embedded_e3());
#endif
  EXPECT_FALSE(created.service->has_local_e3());
  created.service->stop();
  created.service->stop();
}

TEST(dapp_service, pusch_route_slots_multiply_only_the_class_a_pusch_hooks)
{
  auto config              = service_config();
  config.pusch_route_slots = 3;
  auto created             = dapp_service::create(std::move(config));
  ASSERT_TRUE(created) << created.detail;
  auto registry = created.service->native_hooks();
  ASSERT_NE(registry, nullptr);
  for (uint32_t interface_id : {OCUDU_DAPP_INTERFACE_CHANNEL_ESTIMATOR_V1,
                                OCUDU_DAPP_INTERFACE_EQUALIZER_V1,
                                OCUDU_DAPP_INTERFACE_RECEIVER_V1}) {
    const auto slots = registry->find_all(interface_id);
    ASSERT_EQ(slots.size(), 3U);
    for (const auto& point : slots) {
      EXPECT_EQ(point->lane_count(), 6U);
    }
  }
  for (uint32_t interface_id :
       {OCUDU_DAPP_INTERFACE_SPECTRUM_V1, OCUDU_DAPP_INTERFACE_SRS_ISAC_V1, OCUDU_DAPP_INTERFACE_SCHEDULER_V1}) {
    EXPECT_EQ(registry->find_all(interface_id).size(), 1U);
  }
  created.service->stop();

  for (unsigned invalid : {0U, 9U}) {
    auto rejected              = service_config();
    rejected.pusch_route_slots = invalid;
    EXPECT_FALSE(dapp_service::create(std::move(rejected)));
  }
}

TEST(dapp_service, rejects_ambiguous_or_unbounded_topologies_before_construction)
{
  auto config = service_config();
  config.cells.push_back({3, 1});
  EXPECT_FALSE(dapp_service::create(config));

  config                                          = service_config();
  config.cells.front().nof_uplink_producer_shards = 4096;
  EXPECT_FALSE(dapp_service::create(config));

  config                       = service_config();
  config.cells.front().cell_id = MAX_NOF_DU_CELLS;
  EXPECT_FALSE(dapp_service::create(config));

  config                 = service_config();
  config.nof_pusch_lanes = 0;
  EXPECT_FALSE(dapp_service::create(config));

  config                    = service_config();
  config.scheduler_deadline = std::chrono::nanoseconds::zero();
  EXPECT_FALSE(dapp_service::create(config));

  config                    = service_config();
  config.scheduler_deadline = std::chrono::seconds(2);
  EXPECT_FALSE(dapp_service::create(config));
}

#ifdef OCUDU_DAPP_TEST_LOCAL_E3
TEST(dapp_service, owns_a_credentialed_local_e3_endpoint_over_the_same_instance_manager)
{
  temporary_endpoint endpoint;
  ASSERT_FALSE(endpoint.path.empty());
  auto config     = service_config();
  config.local_e3 = dapp_local_e3_config{endpoint.path, ::geteuid(), std::chrono::seconds(1)};
  auto created    = dapp_service::create(std::move(config));
  ASSERT_TRUE(created) << created.detail;
  EXPECT_TRUE(created.service->has_embedded_e3());
  EXPECT_TRUE(created.service->has_local_e3());

  dapp::e3::inventory_query query;
  query.header.transaction_id.bytes[15] = 1;
  dapp::e3::local_management_client client(endpoint.path);
  auto                              response = client.call(query);
  ASSERT_TRUE(response) << response.detail;
  EXPECT_EQ(response.value->code, dapp::management::result_code::ok);
  EXPECT_TRUE(std::holds_alternative<dapp::e3::inventory_result>(response.value->payload));
}

TEST(dapp_service, carries_one_sampled_spectrum_snapshot_through_the_portable_e3_v1_profile)
{
  temporary_endpoint endpoint;
  ASSERT_FALSE(endpoint.path.empty());
  auto config             = service_config();
  config.local_e3         = dapp_local_e3_config{endpoint.path, ::geteuid(), std::chrono::seconds(1)};
  config.portable_e3_data = dapp_portable_e3_data_config{};
  auto created            = dapp_service::create(std::move(config));
  ASSERT_TRUE(created) << created.detail;

  dapp::e3::data_subscription_control subscribe;
  subscribe.header.transaction_id.bytes[15] = 10;
  subscribe.action                          = dapp::e3::data_subscription_action::create;
  dapp::management::identifier subscription_id;
  subscription_id.bytes[0]  = 0xe3;
  subscription_id.bytes[15] = 1;
  subscribe.subscription_id = subscription_id;
  subscribe.interface_id    = OCUDU_DAPP_INTERFACE_SPECTRUM_V1;
  subscribe.cell_id         = 3;
  subscribe.sample_every_n  = 2;
  dapp::e3::local_management_client client(endpoint.path);
  auto                              response = client.call(subscribe);
  ASSERT_TRUE(response) << response.detail;
  ASSERT_EQ(response.value->code, dapp::management::result_code::ok);
  ASSERT_TRUE(std::holds_alternative<dapp::e3::data_subscription_result>(response.value->payload));

  auto tap = created.service->upper_phy_hooks()->wrap_phy_tap({3, 0, 0}, nullptr);
  ASSERT_NE(tap, nullptr);
  contiguous_e3_reader reader;
  tap->handle_quiet_grid(reader, slot_point{1, 7});
  tap->handle_quiet_grid(reader, slot_point{1, 8});

  dapp::e3::data_poll_query poll;
  poll.header.transaction_id.bytes[15] = 11;
  poll.subscription_id                 = subscription_id;
  response                             = client.call(poll);
  ASSERT_TRUE(response) << response.detail;
  ASSERT_EQ(response.value->code, dapp::management::result_code::ok);
  const auto& publications = std::get<dapp::e3::data_publication_result>(response.value->payload);
  ASSERT_EQ(publications.chunks.size(), 1U);
  EXPECT_EQ(publications.chunks[0].interface_id, OCUDU_DAPP_INTERFACE_SPECTRUM_V1);
  EXPECT_EQ(publications.chunks[0].cell_id, 3U);
  EXPECT_EQ(publications.chunks[0].publication_sequence, 1U);
  EXPECT_EQ(dapp::ipc::validate_class_c_metadata(publications.chunks[0].metadata), dapp::ipc::metadata_error::none);
  ASSERT_EQ(publications.chunks[0].tensor_chunk.size(), reader.view().size_bytes());
  EXPECT_EQ(std::memcmp(publications.chunks[0].tensor_chunk.data(), reader.view().data(), reader.view().size_bytes()),
            0);
  ASSERT_TRUE(publications.subscription);
  EXPECT_EQ(publications.subscription->published_objects, 1U);
  EXPECT_EQ(publications.subscription->sampled_objects, 1U);
  EXPECT_EQ(publications.subscription->published_tensor_bytes, reader.view().size_bytes());

  dapp::e3::data_subscription_control remove;
  remove.header.transaction_id.bytes[15] = 12;
  remove.action                          = dapp::e3::data_subscription_action::remove;
  remove.subscription_id                 = subscription_id;
  response                               = client.call(remove);
  ASSERT_TRUE(response) << response.detail;
  EXPECT_EQ(response.value->code, dapp::management::result_code::ok);
}
#endif
