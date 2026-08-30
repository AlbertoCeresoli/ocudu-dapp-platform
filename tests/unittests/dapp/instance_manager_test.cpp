// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "fixtures/fixture_common.h"
#include "ocudu/dapp/abi/v1/metric_catalog.h"
#include "ocudu/dapp/runtime/instance_manager.h"
#include "ocudu/dapp/use_cases/v1/channel_estimator.h"
#include "ocudu/dapp/use_cases/v1/scheduler.h"
#include <algorithm>
#include <cstring>
#include <gtest/gtest.h>

using namespace ocudu::dapp;
using namespace ocudu::dapp::management;

namespace {

identifier id(uint64_t value)
{
  identifier result;
  std::memcpy(result.bytes.data() + 8, &value, sizeof(value));
  return result;
}

ocudu_dapp_host_info_v1 host_info()
{
  ocudu_dapp_host_info_v1 host{};
  host.struct_size = sizeof(host);
  host.abi_major   = OCUDU_DAPP_ABI_MAJOR_V1;
  host.abi_minor   = OCUDU_DAPP_ABI_MINOR_V1;
  return host;
}

ocudu_dapp_host_api_v1 host_api()
{
  ocudu_dapp_host_api_v1 api{};
  api.struct_size = sizeof(api);
  api.abi_major   = OCUDU_DAPP_ABI_MAJOR_V1;
  api.abi_minor   = OCUDU_DAPP_ABI_MINOR_V1;
  return api;
}

package_record valid_package(identifier package_id = id(1))
{
  artifact_verifier verifier(DAPP_TEST_CATALOG);
  auto              inspected = verifier.inspect(DAPP_VALID_FIXTURE);
  EXPECT_TRUE(inspected) << inspected.detail;
  package_record package;
  package.package_id              = package_id;
  package.global_name             = "org.deepsig.ocudu.test.valid";
  package.vendor                  = "DeepSig";
  package.version                 = "1.0.0";
  package.canonical_artifact_path = DAPP_VALID_FIXTURE;
  package.content_digest          = inspected.digest;
  package.backends                = {backend::cpu};
  return package;
}

package_record class_a_package()
{
  artifact_verifier verifier(DAPP_TEST_CATALOG);
  auto              inspected = verifier.inspect(DAPP_CLASS_A_FIXTURE);
  EXPECT_TRUE(inspected) << inspected.detail;
  package_record package;
  package.package_id              = id(2);
  package.global_name             = "org.deepsig.ocudu.test.class-a";
  package.vendor                  = "DeepSig";
  package.version                 = "1.0.0";
  package.canonical_artifact_path = DAPP_CLASS_A_FIXTURE;
  package.content_digest          = inspected.digest;
  package.backends                = {backend::cpu};
  // Catalog interface list is the admission view used by L1-path compatibility checks.
  // Must match fixtures/class_a.cpp (including pusch_profile) so module/manifest comparison passes.
  interface_record iface{};
  iface.interface_id    = OCUDU_DAPP_INTERFACE_CHANNEL_ESTIMATOR_V1;
  iface.interface_major = 1;
  iface.interface_minor = 0;
  iface.dapp_class      = OCUDU_DAPP_CLASS_A_INLINE_V1;
  iface.placement_mask  = 1U << (OCUDU_DAPP_PLACEMENT_IN_PROCESS_V1 - 1);
  iface.threading       = OCUDU_DAPP_THREADING_PER_WORKER_V1;
  iface.capability_bits = 0;
  iface.pusch           = interface_record::pusch_profile{/*max_nof_prb*/ 52,
                                                /*max_nof_rx_ports*/ 4,
                                                /*max_nof_layers*/ 1,
                                                /*modulation_mask*/ OCUDU_DAPP_PUSCH_MODULATION_QPSK_V1,
                                                /*dmrs_type_mask*/ OCUDU_DAPP_PUSCH_DMRS_TYPE1_V1,
                                                /*flags*/ 0};
  package.interfaces.push_back(iface);
  return package;
}

package_record class_b_package()
{
  artifact_verifier verifier(DAPP_TEST_CATALOG);
  auto              inspected = verifier.inspect(DAPP_CLASS_B_FIXTURE);
  EXPECT_TRUE(inspected) << inspected.detail;
  package_record package;
  package.package_id              = id(3);
  package.global_name             = "org.deepsig.ocudu.test.class-b";
  package.vendor                  = "DeepSig";
  package.version                 = "1.0.0";
  package.canonical_artifact_path = DAPP_CLASS_B_FIXTURE;
  package.content_digest          = inspected.digest;
  package.backends                = {backend::cpu, backend::cuda};
  package.interfaces.push_back({OCUDU_DAPP_INTERFACE_SCHEDULER_V1,
                                1,
                                0,
                                OCUDU_DAPP_CLASS_B_DEADLINE_V1,
                                1U << (OCUDU_DAPP_PLACEMENT_IN_PROCESS_V1 - 1),
                                OCUDU_DAPP_THREADING_PER_WORKER_V1,
                                0});
  return package;
}

std::unique_ptr<instance_manager> manager()
{
  return std::make_unique<instance_manager>(
      DAPP_TEST_CATALOG, host_info(), host_api(), 32, nullptr, DAPP_PREFLIGHT_HELPER, std::chrono::seconds(1));
}

operation_result load_one(instance_manager& manager, fixture_control& control, identifier transaction = id(100))
{
  load_request request;
  request.transaction_id      = transaction;
  request.package_id          = id(1);
  request.selected_backend    = backend::cpu;
  request.payload_fingerprint = 0xabc;
  request.create_args         = &control;
  return manager.load(request);
}

operation_result control(instance_manager& manager,
                         identifier        instance_id,
                         uint64_t          transaction,
                         lifecycle_action  action,
                         uint64_t          generation,
                         const void*       primary   = nullptr,
                         const void*       secondary = nullptr)
{
  lifecycle_request request;
  request.transaction_id      = id(transaction);
  request.instance_id         = instance_id;
  request.action              = action;
  request.expected_generation = generation;
  request.timeout_us          = 100'000;
  request.payload_fingerprint = transaction * 17;
  request.primary_payload     = primary;
  request.secondary_payload   = secondary;
  return manager.control(request);
}

} // namespace

TEST(dapp_instance_manager, replace_package_requires_no_resident_instance)
{
  auto manager = ::manager();
  auto package = valid_package();
  ASSERT_TRUE(manager->register_package(package));

  fixture_control fixture;
  const auto      loaded = load_one(*manager, fixture);
  ASSERT_TRUE(loaded) << loaded.detail;

  auto again = valid_package();
  again.content_digest[0] ^= 0x01;
  const auto blocked = manager->replace_package(again);
  EXPECT_EQ(blocked.code, result_code::conflict);
  EXPECT_NE(blocked.detail.find("unload"), std::string::npos);

  ASSERT_TRUE(control(*manager, *loaded.instance_id, 110, lifecycle_action::unload, loaded.instance_generation));
  // Same identity, same bytes: replace is a no-op even after unload.
  EXPECT_TRUE(manager->replace_package(valid_package()));
}

TEST(dapp_instance_manager, package_registration_requires_exact_approved_digest)
{
  auto manager = ::manager();
  auto package = valid_package();
  package.content_digest[0] ^= 0xff;
  EXPECT_EQ(manager->register_package(package).code, result_code::verification_failed);
  EXPECT_TRUE(manager->inventory().packages.empty());

  package = valid_package();
  EXPECT_TRUE(manager->register_package(package));
  auto inventory = manager->inventory();
  ASSERT_EQ(inventory.packages.size(), 1U);
  EXPECT_EQ(inventory.packages[0].package_id, id(1));
}

TEST(dapp_instance_manager, rejects_a_bad_native_query_before_loading_it_in_the_host)
{
  auto              owner = ::manager();
  artifact_verifier verifier(DAPP_TEST_CATALOG);
  auto              inspected = verifier.inspect(DAPP_THROWING_QUERY_FIXTURE);
  ASSERT_TRUE(inspected) << inspected.detail;

  auto package                    = valid_package();
  package.package_id              = id(9);
  package.canonical_artifact_path = DAPP_THROWING_QUERY_FIXTURE;
  package.content_digest          = inspected.digest;
  ASSERT_TRUE(owner->register_package(package));

  load_request request;
  request.transaction_id      = id(901);
  request.package_id          = package.package_id;
  request.selected_backend    = backend::cpu;
  request.payload_fingerprint = 0x901;
  const auto result           = owner->load(request);
  EXPECT_EQ(result.code, result_code::load_failed);
  EXPECT_NE(result.detail.find("query threw (exit 14)"), std::string::npos);
  EXPECT_TRUE(owner->inventory().instances.empty());
}

TEST(dapp_instance_manager, dry_run_is_idempotent_and_transaction_reuse_with_new_request_is_rejected)
{
  auto manager = ::manager();
  ASSERT_TRUE(manager->register_package(valid_package()));
  load_request request;
  request.transaction_id = id(10);
  request.package_id     = id(1);
  request.dry_run        = true;
  auto first             = manager->load(request);
  EXPECT_EQ(first.code, result_code::dry_run_ok);
  EXPECT_EQ(manager->inventory().instances.size(), 0U);
  EXPECT_EQ(manager->load(request).code, result_code::dry_run_ok);

  request.selected_backend = backend::cuda;
  EXPECT_EQ(manager->load(request).code, result_code::transaction_mismatch);
}

TEST(dapp_instance_manager, load_rejects_a_manifest_backend_missing_from_the_host)
{
  auto host         = host_info();
  host.backend_mask = uint64_t{1} << OCUDU_DAPP_BACKEND_HOST_V1;
  instance_manager manager(DAPP_TEST_CATALOG, host, host_api(), 32);
  auto             package = valid_package();
  package.backends         = {backend::cuda};
  ASSERT_TRUE(manager.register_package(package));

  load_request request;
  request.transaction_id   = id(11);
  request.package_id       = id(1);
  request.selected_backend = backend::cuda;
  request.dry_run          = true;
  EXPECT_EQ(manager.load(request).code, result_code::incompatible);
}

TEST(dapp_instance_manager, load_rejects_class_a_cpu_when_accelerated_l1_path_is_active)
{
  auto host         = host_info();
  host.backend_mask = (uint64_t{1} << OCUDU_DAPP_BACKEND_HOST_V1) | (uint64_t{1} << OCUDU_DAPP_BACKEND_CUDA_V1);
  instance_manager manager(DAPP_TEST_CATALOG, host, host_api(), 32);
  ASSERT_TRUE(manager.register_package(class_a_package()));

  load_request request;
  request.transaction_id   = id(13);
  request.package_id       = id(2);
  request.selected_backend = backend::cpu;
  request.dry_run          = true;
  const auto result        = manager.load(request);
  EXPECT_EQ(result.code, result_code::incompatible);
  EXPECT_NE(result.detail.find("accelerated L1 path"), std::string::npos) << result.detail;
}

TEST(dapp_instance_manager, load_rejects_class_a_cuda_when_only_host_l1_path_is_active)
{
  auto host         = host_info();
  host.backend_mask = uint64_t{1} << OCUDU_DAPP_BACKEND_HOST_V1;
  instance_manager manager(DAPP_TEST_CATALOG, host, host_api(), 32);
  auto             package = class_a_package();
  package.backends         = {backend::cuda};
  ASSERT_TRUE(manager.register_package(package));

  load_request request;
  request.transaction_id   = id(14);
  request.package_id       = id(2);
  request.selected_backend = backend::cuda;
  request.dry_run          = true;
  const auto result        = manager.load(request);
  EXPECT_EQ(result.code, result_code::incompatible);
  // Either backend_mask gate or Class-A L1 path gate — both are correct.
  EXPECT_FALSE(result.detail.empty()) << result.detail;
}

TEST(dapp_instance_manager, dry_run_accepts_a_cuda_class_b_scheduler_with_host_record_hooks)
{
  auto manager = ::manager();
  ASSERT_TRUE(manager->register_package(class_b_package()));

  load_request request;
  request.transaction_id   = id(12);
  request.package_id       = id(3);
  request.selected_backend = backend::cuda;
  request.dry_run          = true;
  const auto result        = manager->load(request);
  EXPECT_EQ(result.code, result_code::dry_run_ok) << result.detail;
  EXPECT_TRUE(manager->inventory().instances.empty());
}

TEST(dapp_instance_manager, complete_lifecycle_uses_expected_generations_and_stable_inventory)
{
  auto manager = ::manager();
  ASSERT_TRUE(manager->register_package(valid_package()));
  fixture_control fixture;
  auto            loaded = load_one(*manager, fixture);
  ASSERT_TRUE(loaded) << loaded.detail;
  ASSERT_TRUE(loaded.instance_id);
  const identifier instance_id = *loaded.instance_id;
  EXPECT_EQ(loaded.instance_generation, 1U);

  EXPECT_TRUE(control(*manager, instance_id, 101, lifecycle_action::prepare, 1));
  EXPECT_TRUE(control(*manager, instance_id, 102, lifecycle_action::arm, 2));
  EXPECT_TRUE(control(*manager, instance_id, 103, lifecycle_action::enter_shadow, 3));
  EXPECT_TRUE(control(*manager, instance_id, 104, lifecycle_action::activate, 4));
  auto stale = control(*manager, instance_id, 105, lifecycle_action::deactivate, 4);
  EXPECT_EQ(stale.code, result_code::conflict);
  EXPECT_TRUE(control(*manager, instance_id, 106, lifecycle_action::deactivate, 5));
  auto unloaded = control(*manager, instance_id, 107, lifecycle_action::unload, 6);
  ASSERT_TRUE(unloaded) << unloaded.detail;
  EXPECT_EQ(unloaded.instance_generation, 7U);
  auto record = manager->instance(instance_id);
  ASSERT_TRUE(record);
  EXPECT_EQ(record->state, OCUDU_DAPP_STATE_UNLOADED_V1);
  EXPECT_EQ(record->interfaces.size(), 1U);
  EXPECT_EQ(record->interfaces[0].dapp_class, OCUDU_DAPP_CLASS_C_OPEN_LOOP_V1);
}

TEST(dapp_instance_manager, repeated_transaction_returns_original_result_without_duplicate_instance)
{
  auto manager = ::manager();
  ASSERT_TRUE(manager->register_package(valid_package()));
  fixture_control fixture;
  auto            first  = load_one(*manager, fixture, id(200));
  auto            second = load_one(*manager, fixture, id(200));
  ASSERT_TRUE(first && second);
  EXPECT_EQ(first.instance_id, second.instance_id);
  EXPECT_EQ(manager->inventory().instances.size(), 1U);
  ASSERT_TRUE(first.instance_id);
  EXPECT_TRUE(control(*manager, *first.instance_id, 201, lifecycle_action::unload, 1));
}

TEST(dapp_instance_manager, manifest_identity_must_match_loaded_module)
{
  auto manager        = ::manager();
  auto package        = valid_package();
  package.global_name = "org.deepsig.wrong-name";
  ASSERT_TRUE(manager->register_package(package));
  fixture_control fixture;
  auto            result = load_one(*manager, fixture);
  EXPECT_EQ(result.code, result_code::incompatible);
  EXPECT_TRUE(manager->inventory().instances.empty());
}

TEST(dapp_instance_manager, manifest_interfaces_must_match_loaded_module)
{
  auto manager = ::manager();
  auto package = valid_package();
  package.interfaces.push_back({0x1234,
                                1,
                                1,
                                OCUDU_DAPP_CLASS_C_OPEN_LOOP_V1,
                                1U << (OCUDU_DAPP_PLACEMENT_IN_PROCESS_V1 - 1),
                                OCUDU_DAPP_THREADING_REENTRANT_SHARED_V1,
                                0});
  ASSERT_TRUE(manager->register_package(package));
  fixture_control fixture;
  auto            result = load_one(*manager, fixture);
  EXPECT_EQ(result.code, result_code::incompatible);
  EXPECT_TRUE(manager->inventory().instances.empty());
}

TEST(dapp_instance_manager, lifecycle_incident_is_attributed_to_generated_instance_identity)
{
  auto manager = ::manager();
  ASSERT_TRUE(manager->register_package(valid_package()));
  fixture_control fixture;
  fixture.prepare_status = OCUDU_DAPP_RESOURCE_V1;
  auto loaded            = load_one(*manager, fixture);
  ASSERT_TRUE(loaded && loaded.instance_id);
  auto prepared = control(*manager, *loaded.instance_id, 301, lifecycle_action::prepare, 1);
  EXPECT_EQ(prepared.code, result_code::lifecycle_failed);
  auto record = manager->instance(*loaded.instance_id);
  ASSERT_TRUE(record);
  EXPECT_EQ(record->state, OCUDU_DAPP_STATE_ERROR_V1);

  incident_filter         filter;
  std::array<uint64_t, 2> identity_words{};
  std::memcpy(identity_words.data(), loaded.instance_id->bytes.data(), loaded.instance_id->bytes.size());
  filter.instance_id = identity_words;
  auto incidents     = manager->incidents().query(filter);
  ASSERT_FALSE(incidents.empty());
  EXPECT_EQ(incidents.back().cause, incident_cause::resource_exhausted);

  EXPECT_TRUE(control(*manager, *loaded.instance_id, 302, lifecycle_action::deactivate, 2));
  EXPECT_TRUE(control(*manager, *loaded.instance_id, 303, lifecycle_action::unload, 3));
}

TEST(dapp_instance_manager, management_queries_return_bounded_host_owned_snapshots)
{
  auto manager = ::manager();
  ASSERT_TRUE(manager->register_package(valid_package()));
  fixture_control fixture;
  auto            loaded = load_one(*manager, fixture);
  ASSERT_TRUE(loaded && loaded.instance_id);

  auto record = manager->instance(*loaded.instance_id);
  ASSERT_TRUE(record);
  EXPECT_EQ(record->management_capabilities,
            OCUDU_DAPP_MANAGEMENT_CONFIG_V1 | OCUDU_DAPP_MANAGEMENT_METRICS_V1 | OCUDU_DAPP_MANAGEMENT_MODELS_V1);

  auto schema = manager->config_schema(*loaded.instance_id);
  ASSERT_TRUE(schema);
  ASSERT_EQ(schema.value.size(), 1U);
  EXPECT_EQ(schema.value[0].key, "threshold");
  EXPECT_EQ(schema.value[0].value_kind, OCUDU_DAPP_VALUE_UINT64_V1);

  auto config = manager->config(*loaded.instance_id);
  ASSERT_TRUE(config);
  ASSERT_EQ(config.value.entries.size(), 1U);
  EXPECT_EQ(config.value.generation, 1U);
  EXPECT_EQ(std::get<uint64_t>(config.value.entries[0].entry_value), 7U);

  auto metric_schema = manager->metric_schema(*loaded.instance_id);
  ASSERT_TRUE(metric_schema);
  EXPECT_EQ(metric_schema.value.size(), 12U);
  const auto lifecycle_metric = std::find_if(metric_schema.value.begin(),
                                             metric_schema.value.end(),
                                             [](const auto& item) { return item.name == "host.lifecycle_state"; });
  ASSERT_NE(lifecycle_metric, metric_schema.value.end());
  EXPECT_EQ(lifecycle_metric->flags, OCUDU_DAPP_METRIC_GAUGE_V1);
  const auto dropped_metric = std::find_if(metric_schema.value.begin(),
                                           metric_schema.value.end(),
                                           [](const auto& item) { return item.name == "host.incidents_dropped"; });
  ASSERT_NE(dropped_metric, metric_schema.value.end());
  EXPECT_EQ(dropped_metric->flags, OCUDU_DAPP_METRIC_COUNTER_V1);
  const auto cell_bitmap_metric =
      std::find_if(metric_schema.value.begin(), metric_schema.value.end(), [](const auto& item) {
        return item.name == OCUDU_DAPP_METRIC_HOST_INPUT_CELL_BITMAP_V1;
      });
  ASSERT_NE(cell_bitmap_metric, metric_schema.value.end());
  EXPECT_EQ(cell_bitmap_metric->value_kind, OCUDU_DAPP_VALUE_UINT64_V1);
  EXPECT_EQ(cell_bitmap_metric->flags, OCUDU_DAPP_METRIC_GAUGE_V1);
  const auto native_workers_metric =
      std::find_if(metric_schema.value.begin(), metric_schema.value.end(), [](const auto& item) {
        return item.name == OCUDU_DAPP_METRIC_HOST_NATIVE_WORKERS_V1;
      });
  ASSERT_NE(native_workers_metric, metric_schema.value.end());
  EXPECT_EQ(native_workers_metric->value_kind, OCUDU_DAPP_VALUE_UINT64_V1);
  EXPECT_EQ(native_workers_metric->flags, OCUDU_DAPP_METRIC_GAUGE_V1);
  const auto active_native_workers_metric =
      std::find_if(metric_schema.value.begin(), metric_schema.value.end(), [](const auto& item) {
        return item.name == OCUDU_DAPP_METRIC_HOST_NATIVE_WORKERS_ACTIVE_V1;
      });
  ASSERT_NE(active_native_workers_metric, metric_schema.value.end());
  EXPECT_EQ(active_native_workers_metric->value_kind, OCUDU_DAPP_VALUE_UINT64_V1);
  EXPECT_EQ(active_native_workers_metric->flags, OCUDU_DAPP_METRIC_GAUGE_V1);
  EXPECT_NE(std::find_if(metric_schema.value.begin(),
                         metric_schema.value.end(),
                         [](const auto& item) { return item.name == "fixture.calls"; }),
            metric_schema.value.end());
  auto metrics = manager->metrics(*loaded.instance_id);
  ASSERT_TRUE(metrics);
  EXPECT_EQ(metrics.value.size(), 12U);
  const auto cell_bitmap = std::find_if(metrics.value.begin(), metrics.value.end(), [](const auto& item) {
    return item.name == OCUDU_DAPP_METRIC_HOST_INPUT_CELL_BITMAP_V1;
  });
  ASSERT_NE(cell_bitmap, metrics.value.end());
  EXPECT_EQ(std::get<uint64_t>(cell_bitmap->sample_value), 0U);
  const auto native_workers = std::find_if(metrics.value.begin(), metrics.value.end(), [](const auto& item) {
    return item.name == OCUDU_DAPP_METRIC_HOST_NATIVE_WORKERS_V1;
  });
  ASSERT_NE(native_workers, metrics.value.end());
  EXPECT_EQ(std::get<uint64_t>(native_workers->sample_value), 0U);
  const auto active_native_workers = std::find_if(metrics.value.begin(), metrics.value.end(), [](const auto& item) {
    return item.name == OCUDU_DAPP_METRIC_HOST_NATIVE_WORKERS_ACTIVE_V1;
  });
  ASSERT_NE(active_native_workers, metrics.value.end());
  EXPECT_EQ(std::get<uint64_t>(active_native_workers->sample_value), 0U);
  EXPECT_NE(std::find_if(metrics.value.begin(),
                         metrics.value.end(),
                         [](const auto& item) { return item.name == "fixture.calls"; }),
            metrics.value.end());

  ASSERT_TRUE(control(*manager, *loaded.instance_id, 501, lifecycle_action::unload, 1));
  EXPECT_EQ(schema.value[0].key, "threshold");
  EXPECT_EQ(manager->config(*loaded.instance_id).code, result_code::incompatible);
}

TEST(dapp_instance_manager, class_a_fallback_metrics_are_host_owned_and_e3_queryable)
{
  auto manager = ::manager();
  ASSERT_TRUE(manager->register_package(class_a_package()));
  fixture_control fixture;
  load_request    request;
  request.transaction_id   = id(601);
  request.package_id       = id(2);
  request.selected_backend = backend::cpu;
  request.create_args      = &fixture;
  auto loaded              = manager->load(request);
  ASSERT_TRUE(loaded && loaded.instance_id) << loaded.detail;

  auto schema = manager->metric_schema(*loaded.instance_id);
  ASSERT_TRUE(schema);
  const auto reason = std::find_if(schema.value.begin(), schema.value.end(), [](const auto& item) {
    return item.name == OCUDU_DAPP_METRIC_CLASS_A_UNSUPPORTED_LAYERS_V1;
  });
  ASSERT_NE(reason, schema.value.end());
  EXPECT_EQ(reason->flags, OCUDU_DAPP_METRIC_COUNTER_V1);
  const auto completion_deadline = std::find_if(schema.value.begin(), schema.value.end(), [](const auto& item) {
    return item.name == OCUDU_DAPP_METRIC_CLASS_A_COMPLETION_DEADLINE_MISSES_V1;
  });
  ASSERT_NE(completion_deadline, schema.value.end());
  EXPECT_EQ(completion_deadline->flags, OCUDU_DAPP_METRIC_COUNTER_V1);

  auto samples = manager->metrics(*loaded.instance_id);
  ASSERT_TRUE(samples);
  const auto sample = std::find_if(samples.value.begin(), samples.value.end(), [](const auto& item) {
    return item.name == OCUDU_DAPP_METRIC_CLASS_A_UNSUPPORTED_LAYERS_V1;
  });
  ASSERT_NE(sample, samples.value.end());
  EXPECT_EQ(std::get<uint64_t>(sample->sample_value), 0U);
  const auto completion_sample = std::find_if(samples.value.begin(), samples.value.end(), [](const auto& item) {
    return item.name == OCUDU_DAPP_METRIC_CLASS_A_COMPLETION_DEADLINE_MISSES_V1;
  });
  ASSERT_NE(completion_sample, samples.value.end());
  EXPECT_EQ(std::get<uint64_t>(completion_sample->sample_value), 0U);

  ASSERT_TRUE(control(*manager, *loaded.instance_id, 602, lifecycle_action::unload, 1));
}

TEST(dapp_instance_manager, class_b_boundary_metrics_are_host_owned_and_e3_queryable)
{
  auto hooks   = std::make_shared<native_hook_registry>();
  auto created = native_hook_point::create(OCUDU_DAPP_INTERFACE_SCHEDULER_V1, 40, 1);
  ASSERT_TRUE(created) << created.detail;
  std::shared_ptr<native_hook_point> point(std::move(created.point));
  const auto                         registered = hooks->register_point(point);
  ASSERT_TRUE(registered) << registered.detail;

  instance_manager manager(
      DAPP_TEST_CATALOG, host_info(), host_api(), 32, hooks, DAPP_PREFLIGHT_HELPER, std::chrono::seconds(1));
  ASSERT_TRUE(manager.register_package(class_b_package()));
  fixture_control fixture;
  load_request    request;
  request.transaction_id   = id(701);
  request.package_id       = id(3);
  request.selected_backend = backend::cpu;
  request.create_args      = &fixture;
  auto result              = manager.load(request);
  ASSERT_TRUE(result && result.instance_id) << result.detail;
  const identifier instance_id = *result.instance_id;
  uint64_t         transaction = 702;
  for (auto action :
       {lifecycle_action::prepare, lifecycle_action::arm, lifecycle_action::enter_shadow, lifecycle_action::activate}) {
    result = control(manager, instance_id, transaction++, action, result.instance_generation);
    ASSERT_TRUE(result) << result.detail;
  }

  class_b_policy_measurement measurement;
  measurement.conventional_time_ns = 11;
  measurement.feature_time_ns      = 13;
  measurement.dapp_time_ns         = 17;
  measurement.validation_time_ns   = 19;
  measurement.commit_time_ns       = 23;
  measurement.total_time_ns        = 72;
  measurement.outcome              = class_b_policy_outcome::accepted;
  point->record_class_b_policy(0, measurement);

  auto schema = manager.metric_schema(instance_id);
  ASSERT_TRUE(schema);
  auto samples = manager.metrics(instance_id);
  ASSERT_TRUE(samples);
  const auto expect_metric = [&](const char* name, uint64_t value) {
    EXPECT_NE(
        std::find_if(schema.value.begin(), schema.value.end(), [&](const auto& item) { return item.name == name; }),
        schema.value.end());
    const auto sample =
        std::find_if(samples.value.begin(), samples.value.end(), [&](const auto& item) { return item.name == name; });
    ASSERT_NE(sample, samples.value.end());
    EXPECT_EQ(std::get<uint64_t>(sample->sample_value), value);
  };
  expect_metric(OCUDU_DAPP_METRIC_CLASS_B_FEATURE_TIME_NS_V1, 13);
  expect_metric(OCUDU_DAPP_METRIC_CLASS_B_DAPP_TIME_NS_V1, 17);
  expect_metric(OCUDU_DAPP_METRIC_CLASS_B_VALIDATION_TIME_NS_V1, 19);
  expect_metric(OCUDU_DAPP_METRIC_CLASS_B_COMMIT_TIME_NS_V1, 23);
  expect_metric(OCUDU_DAPP_METRIC_CLASS_B_TOTAL_TIME_NS_V1, 72);
  expect_metric(OCUDU_DAPP_METRIC_CLASS_B_ACCEPTED_V1, 1);

  result = control(manager, instance_id, transaction++, lifecycle_action::deactivate, result.instance_generation);
  ASSERT_TRUE(result) << result.detail;
  EXPECT_TRUE(control(manager, instance_id, transaction, lifecycle_action::unload, result.instance_generation));
}

TEST(dapp_instance_manager, config_and_model_mutations_are_generation_safe_and_idempotent)
{
  auto manager = ::manager();
  ASSERT_TRUE(manager->register_package(valid_package()));
  fixture_control fixture;
  auto            loaded = load_one(*manager, fixture);
  ASSERT_TRUE(loaded && loaded.instance_id);

  config_set_request config_request;
  config_request.transaction_id               = id(601);
  config_request.instance_id                  = *loaded.instance_id;
  config_request.expected_instance_generation = 1;
  config_request.expected_config_generation   = 1;
  config_request.boundary                     = OCUDU_DAPP_CONFIG_BOUNDARY_INVOCATION_V1;
  config_request.entries.push_back({"threshold", uint64_t{9}});
  auto configured = manager->set_config(config_request);
  ASSERT_TRUE(configured) << configured.detail;
  EXPECT_EQ(configured.instance_generation, 2U);
  EXPECT_EQ(configured.resource_generation, 2U);
  EXPECT_EQ(manager->set_config(config_request).resource_generation, 2U);

  config_request.entries[0].entry_value = uint64_t{10};
  EXPECT_EQ(manager->set_config(config_request).code, result_code::transaction_mismatch);

  config_request.transaction_id               = id(602);
  config_request.expected_instance_generation = 2;
  config_request.expected_config_generation   = 1;
  EXPECT_EQ(manager->set_config(config_request).code, result_code::conflict);
  EXPECT_EQ(manager->instance(*loaded.instance_id)->generation, 3U);

  model_request model;
  model.transaction_id               = id(603);
  model.instance_id                  = *loaded.instance_id;
  model.expected_instance_generation = 3;
  model.expected_model_generation    = 1;
  model.action                       = OCUDU_DAPP_MODEL_ACTIVATE_V1;
  model.model_id                     = "fixture-model";
  model.model_version                = "1.0";
  auto modeled                       = manager->manage_model(model);
  ASSERT_TRUE(modeled) << modeled.detail;
  EXPECT_EQ(modeled.instance_generation, 4U);
  EXPECT_EQ(modeled.resource_generation, 2U);

  EXPECT_TRUE(control(*manager, *loaded.instance_id, 604, lifecycle_action::unload, 4));
}

TEST(dapp_instance_manager, management_exception_is_visible_through_query_and_incident_history)
{
  auto manager = ::manager();
  ASSERT_TRUE(manager->register_package(valid_package()));
  fixture_control fixture;
  auto            loaded = load_one(*manager, fixture);
  ASSERT_TRUE(loaded && loaded.instance_id);
  fixture.throw_management = true;

  auto queried = manager->config(*loaded.instance_id);
  EXPECT_EQ(queried.code, result_code::lifecycle_failed);
  EXPECT_FALSE(manager->incidents().query({}).empty());
  EXPECT_EQ(manager->instance(*loaded.instance_id)->generation, 2U);
  fixture.throw_management = false;
  EXPECT_TRUE(control(*manager, *loaded.instance_id, 701, lifecycle_action::unload, 2));
}
