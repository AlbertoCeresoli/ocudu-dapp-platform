// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#ifdef OCUDU_DAPP_TEST_E3
#include "ocudu/dapp/e3/embedded_agent.h"
#endif
#include "ocudu/dapp/abi/v1/metric_catalog.h"
#include "ocudu/dapp/runtime/class_c_process_instance.h"
#include "ocudu/dapp/runtime/incident_store.h"
#include "ocudu/dapp/runtime/instance_manager.h"
#include "ocudu/dapp/use_cases/v1/spectrum.h"
#include "ocudu/dapp/use_cases/v1/srs_isac.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstring>
#include <gtest/gtest.h>
#include <memory>
#include <numeric>
#include <string_view>
#include <sys/resource.h>
#include <thread>
#include <utility>
#include <vector>

using namespace ocudu::dapp;
using namespace std::chrono_literals;

namespace {

class test_process_endpoint final : public class_c_process_endpoint
{
public:
  test_process_endpoint(std::unique_ptr<ipc::publication_producer> producer_, uint32_t interface_id_) :
    producer(std::move(producer_)), id(interface_id_)
  {
  }

  uint32_t                interface_id() const noexcept override { return id; }
  ipc::route_capabilities export_capabilities() const noexcept override { return producer->export_capabilities(); }
  void                    set_active(bool value) noexcept override { enabled.store(value, std::memory_order_release); }
  bool                    reset_after_consumer_restart() noexcept override
  {
    if (!producer->reset_after_consumer_restart()) {
      return false;
    }
    resets.fetch_add(1, std::memory_order_relaxed);
    return true;
  }

  class_c_process_endpoint_metrics snapshot_metrics() const noexcept override
  {
    class_c_process_endpoint_metrics result;
    result.observed_cell_bitmap = observed_cell_bitmap.load(std::memory_order_relaxed);
    return result;
  }

  bool consume_srs_result_source(uint64_t                        source_sequence,
                                 uint32_t                        srs_resource_id,
                                 uint32_t                        srs_config_id,
                                 uint32_t                        cell_id,
                                 const ocudu_dapp_radio_time_v1& created) noexcept override
  {
    bool expected = true;
    return source_sequence == 88 && srs_resource_id == 17 && srs_config_id == 19 && cell_id == 3 && created.sfn == 4 &&
           created.slot == 2 && result_admitted.compare_exchange_strong(expected, false, std::memory_order_acq_rel);
  }

  bool consume_result_source(uint64_t                        source_sequence,
                             uint64_t                        source_plan_generation,
                             uint32_t                        cell_id,
                             const ocudu_dapp_radio_time_v1& created) noexcept override
  {
    bool expected = true;
    return source_sequence == 77 && source_plan_generation == 0 && cell_id == 3 && created.sfn == 4 &&
           created.slot == 2 && result_admitted.compare_exchange_strong(expected, false, std::memory_order_acq_rel);
  }

  bool consume_quiet_intent_source(uint64_t                        source_sequence,
                                   uint64_t                        source_plan_generation,
                                   uint32_t                        cell_id,
                                   const ocudu_dapp_radio_time_v1& created) noexcept override
  {
    bool expected = true;
    return source_sequence == 77 && source_plan_generation == 0 && cell_id == 3 && created.sfn == 4 &&
           created.slot == 2 && quiet_admitted.compare_exchange_strong(expected, false, std::memory_order_acq_rel);
  }

  void admit_result() noexcept
  {
    result_admitted.store(true, std::memory_order_release);
    quiet_admitted.store(true, std::memory_order_release);
  }

  void observe_cell(uint32_t cell_id) noexcept
  {
    ASSERT_LT(cell_id, OCUDU_DAPP_HOST_INPUT_CELL_BITMAP_BITS_V1);
    observed_cell_bitmap.fetch_or(uint32_t{1} << cell_id, std::memory_order_relaxed);
  }

  bool     active() const noexcept { return enabled.load(std::memory_order_acquire); }
  uint64_t reset_count() const noexcept { return resets.load(std::memory_order_relaxed); }

private:
  std::unique_ptr<ipc::publication_producer> producer;
  uint32_t                                   id;
  std::atomic<bool>                          enabled{};
  std::atomic<uint64_t>                      resets{};
  std::atomic<uint32_t>                      observed_cell_bitmap{};
  std::atomic<bool>                          result_admitted{};
  std::atomic<bool>                          quiet_admitted{};
};

class test_result_sink final : public class_c_process_result_sink
{
public:
  ocudu_dapp_status_v1 publish(const class_c_process_result_source&         source,
                               const ocudu_dapp_process_spectrum_result_v1& result) override
  {
    std::lock_guard<std::mutex> lock(mutex);
    received_source = source;
    received_result = result;
    ++count;
    changed.notify_all();
    return OCUDU_DAPP_OK_V1;
  }

  ocudu_dapp_status_v1 publish(const class_c_process_result_source&         source,
                               const ocudu_dapp_process_srs_isac_result_v1& result) override
  {
    std::lock_guard<std::mutex> lock(mutex);
    received_source     = source;
    received_srs_result = result;
    ++count;
    changed.notify_all();
    return OCUDU_DAPP_OK_V1;
  }

  ocudu_dapp_status_v1 publish(const class_c_process_result_source&             source,
                               const ocudu_dapp_process_quiet_intent_result_v1& result) override
  {
    std::lock_guard<std::mutex> lock(mutex);
    received_source       = source;
    received_quiet_result = result;
    ++count;
    changed.notify_all();
    return OCUDU_DAPP_OK_V1;
  }

  ocudu_dapp_status_v1 publication_changed(const class_c_process_result_source&, bool) override
  {
    return OCUDU_DAPP_OK_V1;
  }

  bool wait(std::chrono::milliseconds timeout)
  {
    std::unique_lock<std::mutex> lock(mutex);
    return changed.wait_for(lock, timeout, [&] { return count != 0; });
  }

  std::mutex                                mutex;
  std::condition_variable                   changed;
  uint64_t                                  count{};
  class_c_process_result_source             received_source{};
  ocudu_dapp_process_spectrum_result_v1     received_result{};
  ocudu_dapp_process_srs_isac_result_v1     received_srs_result{};
  ocudu_dapp_process_quiet_intent_result_v1 received_quiet_result{};
};

struct configured_process {
  class_c_process_supervisor_config                   config;
  std::vector<std::shared_ptr<test_process_endpoint>> endpoints;
};

std::unique_ptr<approved_artifact> admit_worker()
{
  artifact_verifier verifier(DAPP_TEST_CATALOG);
  auto              inspected = verifier.inspect(DAPP_CLASS_C_PROCESS_WORKER);
  EXPECT_TRUE(inspected) << inspected.detail;
  if (!inspected) {
    return {};
  }
  auto verified = verifier.verify(DAPP_CLASS_C_PROCESS_WORKER, inspected.digest);
  EXPECT_TRUE(verified) << verified.detail;
  return std::move(verified.artifact);
}

configured_process make_config_with_artifact(std::unique_ptr<approved_artifact> executable,
                                             std::vector<std::string>           arguments = {},
                                             uint16_t                           shards    = 2,
                                             uint32_t interface_id = OCUDU_DAPP_INTERFACE_SPECTRUM_V1)
{
  configured_process result;
  result.config.executable               = std::move(executable);
  result.config.arguments                = std::move(arguments);
  result.config.startup_timeout          = 100ms;
  result.config.stop_timeout             = 100ms;
  result.config.restart_backoff          = 1ms;
  result.config.maximum_restart_attempts = 2;
  for (uint16_t shard = 0; shard != shards; ++shard) {
    auto producer = ipc::publication_producer::create(2, 64);
    EXPECT_TRUE(producer) << producer.detail;
    if (!producer) {
      continue;
    }
    auto endpoint = std::make_shared<test_process_endpoint>(std::move(producer.producer), interface_id);
    ocudu_dapp_route_offer_v1 offer{};
    offer.struct_size             = sizeof(offer);
    offer.interface_id            = interface_id;
    offer.metadata_schema_id      = 17;
    offer.metadata_schema_version = 1;
    offer.route_id                = 100 + shard;
    offer.producer_shard_id       = shard;
    offer.producer_shard_count    = shards;
    result.config.routes.push_back({offer, endpoint});
    result.endpoints.push_back(std::move(endpoint));
  }
  return result;
}

configured_process make_config(std::vector<std::string> arguments    = {},
                               uint16_t                 shards       = 2,
                               uint32_t                 interface_id = OCUDU_DAPP_INTERFACE_SPECTRUM_V1)
{
  return make_config_with_artifact(admit_worker(), std::move(arguments), shards, interface_id);
}

management::identifier make_identifier(uint8_t value)
{
  management::identifier result;
  result.bytes[0] = value;
  return result;
}

void expect_all_active(const configured_process& process, bool expected)
{
  for (const auto& endpoint : process.endpoints) {
    EXPECT_EQ(endpoint->active(), expected);
  }
}

} // namespace

TEST(dapp_class_c_process_instance, aggregates_route_cell_coverage_into_placement_neutral_health)
{
  auto configured = make_config({}, 2);
  ASSERT_EQ(configured.endpoints.size(), 2U);
  configured.endpoints[0]->observe_cell(1);
  configured.endpoints[1]->observe_cell(3);
  configured.endpoints[1]->observe_cell(1);
  auto supervised = class_c_process_supervisor::create(std::move(configured.config));
  ASSERT_TRUE(supervised) << supervised.operation.detail;
  auto created = class_c_process_instance::create(std::move(supervised.supervisor));
  ASSERT_TRUE(created) << created.operation.detail;

  EXPECT_EQ(created.instance->health().input_cell_bitmap, (uint32_t{1} << 1U) | (uint32_t{1} << 3U));
  EXPECT_TRUE(created.instance->unload());
}

TEST(dapp_class_c_process_supervisor, activates_only_acknowledged_route_set_and_stops_cleanly)
{
  auto configured = make_config();
  auto created    = class_c_process_supervisor::create(std::move(configured.config));
  ASSERT_TRUE(created) << created.operation.detail;
  expect_all_active(configured, false);

  auto started = created.supervisor->start();
  ASSERT_TRUE(started) << started.detail;
  expect_all_active(configured, false);
  auto ready = created.supervisor->snapshot();
  EXPECT_EQ(ready.state, class_c_process_state::running);
  EXPECT_FALSE(ready.publication_enabled);
  EXPECT_FALSE(ready.publication_requested);
  ASSERT_TRUE(created.supervisor->set_publication_enabled(true));
  expect_all_active(configured, true);
  auto running = created.supervisor->snapshot();
  EXPECT_EQ(running.state, class_c_process_state::running);
  EXPECT_GT(running.pid, 0);
  EXPECT_EQ(running.route_generation, 1U);
  EXPECT_EQ(running.successful_starts, 1U);
  EXPECT_TRUE(running.publication_enabled);
  EXPECT_TRUE(running.publication_requested);
  EXPECT_EQ(created.supervisor->start().status, OCUDU_DAPP_CONFLICT_V1);

  EXPECT_TRUE(created.supervisor->stop());
  expect_all_active(configured, false);
  auto stopped = created.supervisor->snapshot();
  EXPECT_EQ(stopped.state, class_c_process_state::stopped);
  EXPECT_EQ(stopped.pid, -1);
  EXPECT_EQ(stopped.process_exits, 1U);
  EXPECT_EQ(stopped.forced_kills, 0U);
  EXPECT_TRUE(created.supervisor->stop());
}

TEST(dapp_class_c_process_supervisor, admits_required_srs_with_unavailable_optional_future_dmrs_as_one_bundle)
{
  constexpr uint32_t future_dmrs_interface_id = 0x7f010002U;
  auto               configured               = make_config({"--multi-bundle"}, 1, OCUDU_DAPP_INTERFACE_SRS_ISAC_V1);
  configured.config.interfaces                = {
      {OCUDU_DAPP_INTERFACE_SRS_ISAC_V1,
                      1,
                      OCUDU_DAPP_PROCESS_BINDING_REQUIRED_V1,
                      OCUDU_DAPP_PROCESS_BINDING_BOUND_V1},
      {future_dmrs_interface_id, 0, OCUDU_DAPP_PROCESS_BINDING_OPTIONAL_V1, OCUDU_DAPP_PROCESS_BINDING_UNAVAILABLE_V1}};
  auto created = class_c_process_supervisor::create(std::move(configured.config));
  ASSERT_TRUE(created) << created.operation.detail;
  ASSERT_TRUE(created.supervisor->start()) << created.supervisor->snapshot().detail;
  ASSERT_TRUE(created.supervisor->set_publication_enabled(true));
  ASSERT_EQ(configured.endpoints.size(), 1U);
  EXPECT_TRUE(configured.endpoints.front()->active());
  const auto running = created.supervisor->snapshot();
  EXPECT_EQ(running.state, class_c_process_state::running);
  EXPECT_TRUE(running.publication_enabled);
  EXPECT_TRUE(created.supervisor->stop());
  EXPECT_FALSE(configured.endpoints.front()->active());

  auto invalid              = make_config({}, 1, OCUDU_DAPP_INTERFACE_SRS_ISAC_V1);
  invalid.config.interfaces = {
      {OCUDU_DAPP_INTERFACE_SRS_ISAC_V1,
       1,
       OCUDU_DAPP_PROCESS_BINDING_REQUIRED_V1,
       OCUDU_DAPP_PROCESS_BINDING_BOUND_V1},
      {future_dmrs_interface_id, 0, OCUDU_DAPP_PROCESS_BINDING_REQUIRED_V1, OCUDU_DAPP_PROCESS_BINDING_UNAVAILABLE_V1}};
  auto rejected = class_c_process_supervisor::create(std::move(invalid.config));
  EXPECT_FALSE(rejected);
  EXPECT_EQ(rejected.operation.detail, "invalid process interface binding");
}

TEST(dapp_class_c_process_supervisor, worker_outlives_a_short_lived_lifecycle_caller_thread)
{
  auto configured = make_config();
  auto created    = class_c_process_supervisor::create(std::move(configured.config));
  ASSERT_TRUE(created) << created.operation.detail;

  class_c_process_operation_result started;
  std::thread                      caller([&] { started = created.supervisor->start(); });
  caller.join();
  ASSERT_TRUE(started) << started.detail;
  const auto running = created.supervisor->snapshot();
  ASSERT_EQ(running.state, class_c_process_state::running);
  ASSERT_GT(running.pid, 0);
  EXPECT_EQ(::kill(running.pid, 0), 0);
  EXPECT_TRUE(created.supervisor->stop());
}

TEST(dapp_class_c_process_supervisor, resets_routes_only_after_exit_and_bounds_restarts)
{
  auto configured = make_config({"--exit-after-ready"});
  auto created    = class_c_process_supervisor::create(std::move(configured.config));
  ASSERT_TRUE(created) << created.operation.detail;
  ASSERT_TRUE(created.supervisor->set_publication_enabled(true));
  expect_all_active(configured, false);
  ASSERT_TRUE(created.supervisor->start());
  ASSERT_TRUE(created.supervisor->wait_for_state(class_c_process_state::failed, 2s));

  expect_all_active(configured, false);
  for (const auto& endpoint : configured.endpoints) {
    EXPECT_EQ(endpoint->reset_count(), 2U);
  }
  const auto failed = created.supervisor->snapshot();
  EXPECT_EQ(failed.route_generation, 3U);
  EXPECT_EQ(failed.successful_starts, 3U);
  EXPECT_EQ(failed.successful_restarts, 2U);
  EXPECT_EQ(failed.process_exits, 3U);
  EXPECT_EQ(failed.start_failures, 0U);
  EXPECT_EQ(failed.forced_kills, 0U);
  EXPECT_FALSE(failed.publication_enabled);
  EXPECT_TRUE(failed.publication_requested);
  EXPECT_EQ(failed.detail, "Class-C worker restart limit reached");
  EXPECT_EQ(created.supervisor->set_publication_enabled(true).status, OCUDU_DAPP_CONFLICT_V1);
  EXPECT_TRUE(created.supervisor->stop());
  EXPECT_FALSE(created.supervisor->snapshot().publication_requested);
}

TEST(dapp_class_c_process_supervisor, rejects_worker_that_does_not_acknowledge_routes)
{
  auto configured                   = make_config({"--no-ready"});
  configured.config.startup_timeout = 20ms;
  auto created                      = class_c_process_supervisor::create(std::move(configured.config));
  ASSERT_TRUE(created) << created.operation.detail;

  const auto started = created.supervisor->start();
  EXPECT_FALSE(started);
  EXPECT_EQ(started.status, OCUDU_DAPP_INTERNAL_ERROR_V1);
  EXPECT_EQ(started.detail, "process route readiness timed out");
  expect_all_active(configured, false);
  const auto failed = created.supervisor->snapshot();
  EXPECT_EQ(failed.state, class_c_process_state::failed);
  EXPECT_EQ(failed.successful_starts, 0U);
  EXPECT_EQ(failed.start_failures, 1U);
  EXPECT_EQ(failed.forced_kills, 1U);
}

TEST(dapp_class_c_process_supervisor, rejects_worker_without_the_required_heartbeat_capability)
{
  auto configured = make_config({"--no-heartbeat-capability"});
  auto created    = class_c_process_supervisor::create(std::move(configured.config));
  ASSERT_TRUE(created) << created.operation.detail;
  const auto started = created.supervisor->start();
  EXPECT_FALSE(started);
  EXPECT_EQ(started.detail, "invalid process route readiness payload");
  EXPECT_EQ(created.supervisor->snapshot().forced_kills, 1U);
}

TEST(dapp_class_c_process_supervisor, rejects_worker_without_the_required_parent_death_signal)
{
  auto configured = make_config({"--skip-parent-death-signal"});
  auto created    = class_c_process_supervisor::create(std::move(configured.config));
  ASSERT_TRUE(created);
  auto started = created.supervisor->start();
  EXPECT_FALSE(started);
  EXPECT_EQ(started.detail, "invalid process route readiness payload");
  EXPECT_EQ(created.supervisor->snapshot().forced_kills, 1U);
}

TEST(dapp_class_c_process_supervisor, applies_kernel_resource_bounds_before_admitting_routes)
{
  auto configured                           = make_config();
  configured.config.maximum_open_files      = 64;
  configured.config.maximum_core_file_bytes = 32 * 1024 * 1024;
#if !defined(__SANITIZE_ADDRESS__)
  configured.config.maximum_address_space_bytes = 512 * 1024 * 1024;
#endif
  auto created = class_c_process_supervisor::create(std::move(configured.config));
  ASSERT_TRUE(created) << created.operation.detail;
  ASSERT_TRUE(created.supervisor->start());

  const pid_t process = created.supervisor->snapshot().pid;
  ASSERT_GT(process, 0);
  rlimit limit{};
  ASSERT_EQ(::prlimit(process, RLIMIT_CORE, nullptr, &limit), 0);
  EXPECT_EQ(limit.rlim_cur, 32U * 1024U * 1024U);
  EXPECT_EQ(limit.rlim_max, 32U * 1024U * 1024U);
  ASSERT_EQ(::prlimit(process, RLIMIT_NOFILE, nullptr, &limit), 0);
  EXPECT_EQ(limit.rlim_cur, 64U);
  EXPECT_EQ(limit.rlim_max, 64U);
#if !defined(__SANITIZE_ADDRESS__)
  ASSERT_EQ(::prlimit(process, RLIMIT_AS, nullptr, &limit), 0);
  EXPECT_EQ(limit.rlim_cur, 512U * 1024U * 1024U);
  EXPECT_EQ(limit.rlim_max, 512U * 1024U * 1024U);
#endif
  EXPECT_TRUE(created.supervisor->stop());
}

TEST(dapp_class_c_process_supervisor, rejects_worker_that_does_not_apply_no_new_privileges)
{
  auto configured                   = make_config({"--skip-no-new-privileges"});
  configured.config.startup_timeout = 20ms;
  auto created                      = class_c_process_supervisor::create(std::move(configured.config));
  ASSERT_TRUE(created) << created.operation.detail;
  const auto started = created.supervisor->start();
  EXPECT_FALSE(started);
  EXPECT_EQ(started.detail, "process did not apply no_new_privs before route admission");
  EXPECT_EQ(created.supervisor->snapshot().forced_kills, 1U);
  expect_all_active(configured, false);
}

TEST(dapp_class_c_process_supervisor, requires_seccomp_filter_before_route_admission_when_configured)
{
  auto missing                          = make_config();
  missing.config.require_seccomp_filter = true;
  missing.config.startup_timeout        = 20ms;
  auto rejected                         = class_c_process_supervisor::create(std::move(missing.config));
  ASSERT_TRUE(rejected);
  auto rejected_start = rejected.supervisor->start();
  EXPECT_FALSE(rejected_start);
  EXPECT_EQ(rejected_start.detail, "process did not apply a seccomp filter before route admission");

  auto configured                          = make_config({"--install-seccomp-filter"});
  configured.config.require_seccomp_filter = true;
  auto created                             = class_c_process_supervisor::create(std::move(configured.config));
  ASSERT_TRUE(created) << created.operation.detail;
  ASSERT_TRUE(created.supervisor->start()) << created.supervisor->snapshot().detail;
  EXPECT_TRUE(created.supervisor->stop());
}

TEST(dapp_class_c_process_supervisor, escalates_an_unresponsive_worker_to_sigkill)
{
  auto configured                = make_config({"--ignore-term"});
  configured.config.stop_timeout = 20ms;
  auto created                   = class_c_process_supervisor::create(std::move(configured.config));
  ASSERT_TRUE(created) << created.operation.detail;
  ASSERT_TRUE(created.supervisor->start());
  ASSERT_TRUE(created.supervisor->set_publication_enabled(true));
  expect_all_active(configured, true);
  EXPECT_TRUE(created.supervisor->stop());
  expect_all_active(configured, false);
  EXPECT_EQ(created.supervisor->snapshot().forced_kills, 1U);
}

TEST(dapp_class_c_process_supervisor, authenticates_periodic_heartbeats_without_enabling_publication)
{
  auto configured                     = make_config();
  configured.config.heartbeat_period  = 5ms;
  configured.config.heartbeat_timeout = 20ms;
  auto created                        = class_c_process_supervisor::create(std::move(configured.config));
  ASSERT_TRUE(created) << created.operation.detail;
  ASSERT_TRUE(created.supervisor->start());

  const auto               deadline = std::chrono::steady_clock::now() + 1s;
  class_c_process_snapshot snapshot;
  do {
    std::this_thread::sleep_for(1ms);
    snapshot = created.supervisor->snapshot();
  } while (snapshot.heartbeat_responses < 2 && std::chrono::steady_clock::now() < deadline);
  EXPECT_GE(snapshot.heartbeat_requests, 2U);
  EXPECT_GE(snapshot.heartbeat_responses, 2U);
  EXPECT_EQ(snapshot.heartbeat_losses, 0U);
  EXPECT_GT(snapshot.last_heartbeat_latency_ns, 0U);
  EXPECT_FALSE(snapshot.publication_enabled);
  EXPECT_TRUE(created.supervisor->stop());
}

TEST(dapp_class_c_process_instance, reports_heartbeat_loss_and_bounded_restart_through_management)
{
  auto configured                            = make_config({"--ignore-heartbeat"});
  configured.config.heartbeat_period         = 5ms;
  configured.config.heartbeat_timeout        = 10ms;
  configured.config.restart_backoff          = 1ms;
  configured.config.maximum_restart_attempts = 1;
  auto supervised                            = class_c_process_supervisor::create(std::move(configured.config));
  ASSERT_TRUE(supervised) << supervised.operation.detail;
  auto created = class_c_process_instance::create(std::move(supervised.supervisor));
  ASSERT_TRUE(created) << created.operation.detail;
  const uint64_t instance_id[2]{31, 32};
  const uint64_t package_id[2]{41, 42};
  created.instance->set_incident_identity(instance_id, package_id, 5);
  ASSERT_TRUE(created.instance->prepare(nullptr, nullptr));
  ASSERT_TRUE(created.instance->arm(nullptr));
  ASSERT_TRUE(created.instance->enter_shadow());

  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (created.instance->state() != OCUDU_DAPP_STATE_ERROR_V1 && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(1ms);
  ASSERT_EQ(created.instance->state(), OCUDU_DAPP_STATE_ERROR_V1);

  auto described = created.instance->describe_metrics(nullptr, 0);
  ASSERT_TRUE(described);
  std::vector<ocudu_dapp_metric_sample_v1> samples(described.count);
  ASSERT_TRUE(created.instance->read_metrics(samples.data(), samples.size()));
  const auto losses = std::find_if(samples.begin(), samples.end(), [](const auto& sample) {
    return std::string_view(sample.name) == "process.heartbeat_losses";
  });
  ASSERT_NE(losses, samples.end());
  EXPECT_EQ(losses->value.value.uint64_value, 2U);

  incident_store incidents(16);
  created.instance->drain_incidents(incidents);
  const auto records   = incidents.query();
  const auto heartbeat = std::find_if(records.begin(), records.end(), [](const auto& record) {
    return record.cause == incident_cause::heartbeat_loss;
  });
  ASSERT_NE(heartbeat, records.end());
  const uint32_t heartbeat_occurrences =
      std::accumulate(records.begin(), records.end(), 0U, [](uint32_t count, const auto& record) {
        return count + (record.cause == incident_cause::heartbeat_loss ? record.recurrence_count : 0U);
      });
  EXPECT_EQ(heartbeat_occurrences, 2U);
  EXPECT_EQ(heartbeat->context.instance_id[0], 31U);
  EXPECT_TRUE(std::any_of(records.begin(), records.end(), [](const auto& record) {
    return record.cause == incident_cause::process_exit && record.breaker_open;
  }));
  EXPECT_TRUE(created.instance->drain(std::chrono::steady_clock::now() + 1s));
  EXPECT_TRUE(created.instance->unload());
}

TEST(dapp_class_c_process_supervisor, authenticates_and_correlates_bounded_spectrum_results)
{
  auto configured               = make_config({"--send-result"});
  auto sink                     = std::make_shared<test_result_sink>();
  configured.config.result_sink = sink;
  configured.endpoints.front()->admit_result();
  auto created = class_c_process_supervisor::create(std::move(configured.config));
  ASSERT_TRUE(created) << created.operation.detail;
  const uint64_t instance_id[2]{11, 12};
  created.supervisor->set_result_identity(instance_id, 9);
  ASSERT_TRUE(created.supervisor->set_publication_enabled(true));
  ASSERT_TRUE(created.supervisor->start());
  ASSERT_TRUE(sink->wait(500ms));
  {
    std::lock_guard<std::mutex> lock(sink->mutex);
    EXPECT_EQ(sink->received_source.instance_id[0], 11U);
    EXPECT_EQ(sink->received_source.app_generation, 9U);
    EXPECT_EQ(sink->received_source.route_generation, 1U);
    EXPECT_EQ(sink->received_source.route_id, 100U);
    EXPECT_EQ(sink->received_result.source_grid_sequence, 77U);
    EXPECT_EQ(sink->received_result.prb_confidence[0], UINT8_MAX);
  }
  const auto snapshot = created.supervisor->snapshot();
  EXPECT_EQ(snapshot.accepted_results, 1U);
  EXPECT_EQ(snapshot.rejected_results, 0U);
  EXPECT_EQ(snapshot.result_protocol_failures, 0U);
  EXPECT_TRUE(created.supervisor->stop());
}

TEST(dapp_class_c_process_supervisor, rejects_control_results_when_an_observer_has_no_result_authority)
{
  auto configured                            = make_config({"--send-result"});
  configured.config.maximum_restart_attempts = 0;
  configured.config.results_prohibited       = true;
  auto created                               = class_c_process_supervisor::create(std::move(configured.config));
  ASSERT_TRUE(created) << created.operation.detail;
  const uint64_t instance_id[2]{41, 42};
  created.supervisor->set_result_identity(instance_id, 17);
  ASSERT_TRUE(created.supervisor->set_publication_enabled(true));
  ASSERT_TRUE(created.supervisor->start());
  ASSERT_TRUE(created.supervisor->wait_for_state(class_c_process_state::failed, 2s));

  const auto snapshot = created.supervisor->snapshot();
  EXPECT_EQ(snapshot.accepted_results, 0U);
  EXPECT_EQ(snapshot.rejected_results, 0U);
  EXPECT_EQ(snapshot.result_protocol_failures, 1U);
  EXPECT_EQ(snapshot.successful_restarts, 0U);
  EXPECT_FALSE(snapshot.publication_enabled);
  expect_all_active(configured, false);
  EXPECT_TRUE(created.supervisor->stop());
}

TEST(dapp_class_c_process_supervisor, authenticates_and_correlates_bounded_srs_results)
{
  auto configured               = make_config({"--send-result"}, 1, OCUDU_DAPP_INTERFACE_SRS_ISAC_V1);
  auto sink                     = std::make_shared<test_result_sink>();
  configured.config.result_sink = sink;
  configured.endpoints.front()->admit_result();
  auto created = class_c_process_supervisor::create(std::move(configured.config));
  ASSERT_TRUE(created) << created.operation.detail;
  const uint64_t instance_id[2]{21, 22};
  created.supervisor->set_result_identity(instance_id, 13);
  ASSERT_TRUE(created.supervisor->set_publication_enabled(true));
  ASSERT_TRUE(created.supervisor->start());
  ASSERT_TRUE(sink->wait(500ms));
  {
    std::lock_guard<std::mutex> lock(sink->mutex);
    EXPECT_EQ(sink->received_source.instance_id[0], 21U);
    EXPECT_EQ(sink->received_source.app_generation, 13U);
    EXPECT_EQ(sink->received_source.interface_id, OCUDU_DAPP_INTERFACE_SRS_ISAC_V1);
    EXPECT_EQ(sink->received_srs_result.preview.source_srs_sequence, 88U);
    EXPECT_EQ(sink->received_srs_result.preview.srs_resource_id, 17U);
    EXPECT_EQ(sink->received_srs_result.preview.srs_config_id, 19U);
    EXPECT_EQ(sink->received_srs_result.preview.peak_delay_bin, 1U);
    EXPECT_FLOAT_EQ(sink->received_srs_result.preview.peak_cir_power, 1.0F);
  }
  const auto snapshot = created.supervisor->snapshot();
  EXPECT_EQ(snapshot.accepted_results, 1U);
  EXPECT_EQ(snapshot.rejected_results, 0U);
  EXPECT_EQ(snapshot.result_protocol_failures, 0U);
  EXPECT_TRUE(created.supervisor->stop());
}

TEST(dapp_class_c_process_supervisor, authenticates_and_correlates_bounded_quiet_intents)
{
  auto configured               = make_config({"--send-quiet"});
  auto sink                     = std::make_shared<test_result_sink>();
  configured.config.result_sink = sink;
  configured.endpoints.front()->admit_result();
  auto created = class_c_process_supervisor::create(std::move(configured.config));
  ASSERT_TRUE(created) << created.operation.detail;
  const uint64_t instance_id[2]{31, 32};
  created.supervisor->set_result_identity(instance_id, 15);
  ASSERT_TRUE(created.supervisor->set_publication_enabled(true));
  ASSERT_TRUE(created.supervisor->start());
  ASSERT_TRUE(sink->wait(500ms));
  {
    std::lock_guard<std::mutex> lock(sink->mutex);
    EXPECT_EQ(sink->received_source.instance_id[0], 31U);
    EXPECT_EQ(sink->received_source.app_generation, 15U);
    EXPECT_EQ(sink->received_quiet_result.source_grid_sequence, 77U);
    EXPECT_EQ(sink->received_quiet_result.intent.first_prb, 5U);
  }
  const auto snapshot = created.supervisor->snapshot();
  EXPECT_EQ(snapshot.accepted_results, 1U);
  EXPECT_EQ(snapshot.rejected_results, 0U);
  EXPECT_EQ(snapshot.result_protocol_failures, 0U);
  EXPECT_TRUE(created.supervisor->stop());
}

TEST(dapp_class_c_process_supervisor, rejects_incomplete_or_inconsistent_route_sets_before_spawn)
{
  auto configured                                          = make_config();
  configured.config.routes.back().offer.metadata_schema_id = 18;
  auto created = class_c_process_supervisor::create(std::move(configured.config));
  EXPECT_FALSE(created);
  EXPECT_EQ(created.operation.status, OCUDU_DAPP_INVALID_INPUT_V1);
  EXPECT_EQ(created.operation.detail, "inconsistent process route set");
  expect_all_active(configured, false);

  auto contradictory                      = make_config();
  contradictory.config.results_prohibited = true;
  contradictory.config.result_sink        = std::make_shared<test_result_sink>();
  auto rejected_observer                  = class_c_process_supervisor::create(std::move(contradictory.config));
  EXPECT_FALSE(rejected_observer);
  EXPECT_EQ(rejected_observer.operation.detail, "observer-only process cannot have a result sink");
}

TEST(dapp_class_c_process_supervisor, admits_a_separate_spectrum_plan_route_but_rejects_it_for_other_interfaces)
{
  auto configured                             = make_config();
  configured.config.routes.back().offer.flags = OCUDU_DAPP_ROUTE_FLAG_UL_RESOURCE_PLAN_V1;
  auto created                                = class_c_process_supervisor::create(std::move(configured.config));
  ASSERT_TRUE(created) << created.operation.detail;
  ASSERT_TRUE(created.supervisor->start());
  EXPECT_TRUE(created.supervisor->stop());

  auto invalid                             = make_config({}, 2, OCUDU_DAPP_INTERFACE_SRS_ISAC_V1);
  invalid.config.routes.back().offer.flags = OCUDU_DAPP_ROUTE_FLAG_UL_RESOURCE_PLAN_V1;
  auto rejected                            = class_c_process_supervisor::create(std::move(invalid.config));
  EXPECT_FALSE(rejected);
  EXPECT_EQ(rejected.operation.status, OCUDU_DAPP_INVALID_INPUT_V1);
}

TEST(dapp_class_c_process_instance, separates_shadow_readiness_from_lifecycle_activation)
{
  auto configured = make_config();
  auto supervised = class_c_process_supervisor::create(std::move(configured.config));
  ASSERT_TRUE(supervised) << supervised.operation.detail;
  auto created = class_c_process_instance::create(std::move(supervised.supervisor));
  ASSERT_TRUE(created) << created.operation.detail;
  EXPECT_EQ(created.instance->state(), OCUDU_DAPP_STATE_LOADED_V1);

  EXPECT_TRUE(created.instance->prepare(nullptr, nullptr));
  EXPECT_TRUE(created.instance->arm(nullptr));
  EXPECT_TRUE(created.instance->enter_shadow());
  EXPECT_EQ(created.instance->state(), OCUDU_DAPP_STATE_SHADOW_V1);
  expect_all_active(configured, false);

  auto sized = created.instance->describe_metrics(nullptr, 0);
  ASSERT_TRUE(sized);
  EXPECT_EQ(sized.count, 41U);
  std::vector<ocudu_dapp_metric_descriptor_v1> descriptors(sized.count);
  ASSERT_TRUE(created.instance->describe_metrics(descriptors.data(), descriptors.size()));
  const auto copy_count = std::find_if(descriptors.begin(), descriptors.end(), [](const auto& descriptor) {
    return std::string_view(descriptor.name) == "class_c.copy.snapshot.count";
  });
  ASSERT_NE(copy_count, descriptors.end());
  EXPECT_EQ(copy_count->flags, OCUDU_DAPP_METRIC_COUNTER_V1);
  const auto export_pressure = std::find_if(descriptors.begin(), descriptors.end(), [](const auto& descriptor) {
    return std::string_view(descriptor.name) == OCUDU_DAPP_METRIC_CLASS_C_EXPORT_SLOTS_HIGH_WATERMARK_V1;
  });
  ASSERT_NE(export_pressure, descriptors.end());
  EXPECT_EQ(export_pressure->flags, OCUDU_DAPP_METRIC_GAUGE_V1);
  std::vector<ocudu_dapp_metric_sample_v1> samples(sized.count);
  auto                                     metrics = created.instance->read_metrics(samples.data(), samples.size());
  ASSERT_TRUE(metrics);
  auto publication = std::find_if(samples.begin(), samples.end(), [](const auto& sample) {
    return std::string_view(sample.name) == "process.publication_enabled";
  });
  ASSERT_NE(publication, samples.end());
  EXPECT_EQ(publication->value.value.uint64_value, 0U);

  EXPECT_TRUE(created.instance->activate());
  EXPECT_EQ(created.instance->state(), OCUDU_DAPP_STATE_ACTIVE_V1);
  expect_all_active(configured, true);
  EXPECT_TRUE(created.instance->drain(std::chrono::steady_clock::now() + 1s));
  EXPECT_EQ(created.instance->state(), OCUDU_DAPP_STATE_INACTIVE_V1);
  expect_all_active(configured, false);
  incident_store incidents(16);
  EXPECT_EQ(created.instance->drain_incidents(incidents), 0U);
  EXPECT_TRUE(created.instance->unload());
  EXPECT_EQ(created.instance->state(), OCUDU_DAPP_STATE_UNLOADED_V1);
}

TEST(dapp_class_c_process_supervisor, forwards_and_replays_generation_safe_config_and_model_state)
{
  auto configured = make_config();
  auto created    = class_c_process_supervisor::create(std::move(configured.config));
  ASSERT_TRUE(created) << created.operation.detail;
  ASSERT_TRUE(created.supervisor->start());

  ocudu_dapp_process_management_request_v1 schema{};
  schema.struct_size = sizeof(schema);
  schema.operation   = OCUDU_DAPP_PROCESS_MANAGEMENT_DESCRIBE_CONFIG_V1;
  auto described     = created.supervisor->manage(schema);
  ASSERT_TRUE(described) << described.operation.detail;
  EXPECT_EQ(described.response.count, 1U);
  EXPECT_STREQ(described.response.payload.descriptor.key, "enabled");

  ocudu_dapp_process_management_request_v1 update{};
  update.struct_size         = sizeof(update);
  update.operation           = OCUDU_DAPP_PROCESS_MANAGEMENT_SET_CONFIG_V1;
  update.expected_generation = 1;
  update.boundary_or_action  = OCUDU_DAPP_CONFIG_BOUNDARY_INVOCATION_V1;
  update.entry_count         = 1;
  std::strcpy(update.entries[0].key, "enabled");
  update.entries[0].value.kind          = OCUDU_DAPP_VALUE_BOOL_V1;
  update.entries[0].value.value.boolean = false;
  auto configured_result                = created.supervisor->manage(update);
  ASSERT_TRUE(configured_result) << configured_result.operation.detail;
  EXPECT_EQ(configured_result.response.resulting_generation, 2U);

  ocudu_dapp_process_management_request_v1 model{};
  model.struct_size         = sizeof(model);
  model.operation           = OCUDU_DAPP_PROCESS_MANAGEMENT_MODEL_V1;
  model.expected_generation = 1;
  model.boundary_or_action  = OCUDU_DAPP_MODEL_ACTIVATE_V1;
  std::strcpy(model.model_id, "fixture-model");
  auto modeled = created.supervisor->manage(model);
  ASSERT_TRUE(modeled) << modeled.operation.detail;
  EXPECT_EQ(modeled.response.resulting_generation, 2U);

  const auto first = created.supervisor->snapshot();
  ASSERT_GT(first.pid, 0);
  ASSERT_EQ(::kill(static_cast<pid_t>(first.pid), SIGKILL), 0);
  const auto deadline = std::chrono::steady_clock::now() + 1s;
  while ((created.supervisor->snapshot().state != class_c_process_state::running ||
          created.supervisor->snapshot().route_generation == first.route_generation) &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(1ms);
  }
  const auto restarted = created.supervisor->snapshot();
  ASSERT_EQ(restarted.state, class_c_process_state::running);
  EXPECT_GT(restarted.route_generation, first.route_generation);
  EXPECT_EQ(restarted.management_replays, 2U);

  ocudu_dapp_process_management_request_v1 query{};
  query.struct_size = sizeof(query);
  query.operation   = OCUDU_DAPP_PROCESS_MANAGEMENT_GET_CONFIG_V1;
  auto current      = created.supervisor->manage(query);
  ASSERT_TRUE(current) << current.operation.detail;
  EXPECT_EQ(current.response.resulting_generation, 2U);
  EXPECT_EQ(current.response.payload.entries[0].value.value.boolean, 0U);

  model.expected_generation = 2;
  model.flags               = OCUDU_DAPP_PROCESS_MANAGEMENT_DRY_RUN_V1;
  auto model_after_restart  = created.supervisor->manage(model);
  ASSERT_TRUE(model_after_restart) << model_after_restart.operation.detail;
  EXPECT_EQ(model_after_restart.response.resulting_generation, 2U);
  EXPECT_TRUE(created.supervisor->stop());
}

TEST(dapp_class_c_process_supervisor, withdraws_publication_and_restarts_after_management_timeout)
{
  auto configured                      = make_config({"--ignore-heartbeat"});
  configured.config.heartbeat_period   = 1s;
  configured.config.heartbeat_timeout  = 1s;
  configured.config.management_timeout = 10ms;
  configured.config.restart_backoff    = 1ms;
  auto created                         = class_c_process_supervisor::create(std::move(configured.config));
  ASSERT_TRUE(created) << created.operation.detail;
  ASSERT_TRUE(created.supervisor->start());
  ASSERT_TRUE(created.supervisor->set_publication_enabled(true));
  expect_all_active(configured, true);
  const auto first = created.supervisor->snapshot();

  ocudu_dapp_process_management_request_v1 query{};
  query.struct_size = sizeof(query);
  query.operation   = OCUDU_DAPP_PROCESS_MANAGEMENT_GET_CONFIG_V1;
  auto timed_out    = created.supervisor->manage(query);
  EXPECT_FALSE(timed_out);
  EXPECT_EQ(timed_out.operation.status, OCUDU_DAPP_DEADLINE_V1);
  expect_all_active(configured, false);

  const auto deadline = std::chrono::steady_clock::now() + 1s;
  while ((created.supervisor->snapshot().state != class_c_process_state::running ||
          created.supervisor->snapshot().route_generation == first.route_generation) &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(1ms);
  }
  const auto restarted = created.supervisor->snapshot();
  ASSERT_EQ(restarted.state, class_c_process_state::running);
  EXPECT_EQ(restarted.management_timeouts, 1U);
  EXPECT_GT(restarted.route_generation, first.route_generation);
  expect_all_active(configured, true);
  EXPECT_TRUE(created.supervisor->stop());
}

TEST(dapp_class_c_process_instance, exposes_common_config_and_model_management_for_process_placement)
{
  auto configured = make_config();
  auto supervised = class_c_process_supervisor::create(std::move(configured.config));
  ASSERT_TRUE(supervised) << supervised.operation.detail;
  auto created = class_c_process_instance::create(std::move(supervised.supervisor));
  ASSERT_TRUE(created) << created.operation.detail;
  ASSERT_TRUE(created.instance->prepare(nullptr, nullptr));
  ASSERT_TRUE(created.instance->arm(nullptr));
  ASSERT_TRUE(created.instance->enter_shadow());

  EXPECT_EQ(created.instance->management_capabilities(),
            OCUDU_DAPP_MANAGEMENT_CONFIG_V1 | OCUDU_DAPP_MANAGEMENT_METRICS_V1 | OCUDU_DAPP_MANAGEMENT_MODELS_V1);
  auto schema = created.instance->describe_config(nullptr, 0);
  ASSERT_TRUE(schema);
  ASSERT_EQ(schema.count, 1U);
  ocudu_dapp_config_descriptor_v1 descriptor{};
  ASSERT_TRUE(created.instance->describe_config(&descriptor, 1));
  EXPECT_STREQ(descriptor.key, "enabled");

  ocudu_dapp_config_entry_v1 entry{};
  auto                       snapshot = created.instance->get_config(&entry, 1);
  ASSERT_TRUE(snapshot);
  EXPECT_EQ(snapshot.generation, 1U);
  EXPECT_EQ(entry.value.value.boolean, 1U);
  ocudu_dapp_config_entry_v1 changed{};
  changed.key                 = "enabled";
  changed.value.kind          = OCUDU_DAPP_VALUE_BOOL_V1;
  changed.value.value.boolean = false;
  ocudu_dapp_config_update_v1 update{};
  update.struct_size         = sizeof(update);
  update.nof_entries         = 1;
  update.entries             = &changed;
  update.expected_generation = 1;
  update.boundary            = OCUDU_DAPP_CONFIG_BOUNDARY_INVOCATION_V1;
  auto applied               = created.instance->set_config(update);
  ASSERT_TRUE(applied);
  EXPECT_EQ(applied.generation, 2U);

  ocudu_dapp_model_request_v1 model{};
  model.struct_size         = sizeof(model);
  model.action              = OCUDU_DAPP_MODEL_ACTIVATE_V1;
  model.model_id            = "fixture-model";
  model.expected_generation = 1;
  auto modeled              = created.instance->manage_model(model);
  ASSERT_TRUE(modeled);
  EXPECT_EQ(modeled.generation, 2U);

  EXPECT_TRUE(created.instance->drain(std::chrono::steady_clock::now() + 1s));
  EXPECT_TRUE(created.instance->unload());
}

TEST(dapp_class_c_process_instance, exposes_restart_exhaustion_as_health_and_incidents)
{
  auto configured = make_config({"--exit-after-ready"});
  auto supervised = class_c_process_supervisor::create(std::move(configured.config));
  ASSERT_TRUE(supervised) << supervised.operation.detail;
  auto created = class_c_process_instance::create(std::move(supervised.supervisor));
  ASSERT_TRUE(created) << created.operation.detail;
  const uint64_t instance_id[2]{1, 2};
  const uint64_t package_id[2]{3, 4};
  created.instance->set_incident_identity(instance_id, package_id, 7);
  ASSERT_TRUE(created.instance->prepare(nullptr, nullptr));
  ASSERT_TRUE(created.instance->arm(nullptr));
  ASSERT_TRUE(created.instance->enter_shadow());

  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (created.instance->state() != OCUDU_DAPP_STATE_ERROR_V1 && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(1ms);
  }
  ASSERT_EQ(created.instance->state(), OCUDU_DAPP_STATE_ERROR_V1);
  EXPECT_EQ(created.instance->health().breaker, breaker_state::open);
  expect_all_active(configured, false);

  incident_store incidents(16);
  EXPECT_EQ(created.instance->drain_incidents(incidents), 3U);
  const auto records = incidents.query();
  ASSERT_EQ(records.size(), 1U);
  EXPECT_EQ(records.front().cause, incident_cause::process_exit);
  EXPECT_EQ(records.front().action, incident_action::disabled);
  EXPECT_EQ(records.front().recurrence_count, 3U);
  EXPECT_EQ(records.front().context.instance_id[0], 1U);
  EXPECT_EQ(records.front().context.package_id[0], 3U);
  EXPECT_EQ(records.front().context.app_generation, 7U);

  EXPECT_TRUE(created.instance->drain(std::chrono::steady_clock::now() + 1s));
  EXPECT_TRUE(created.instance->unload());
}

TEST(dapp_instance_manager, controls_registered_process_placement_without_protocol_pointers)
{
  ocudu_dapp_host_info_v1 host{};
  host.struct_size = sizeof(host);
  host.abi_major   = OCUDU_DAPP_ABI_MAJOR_V1;
  ocudu_dapp_host_api_v1 api{};
  api.struct_size = sizeof(api);
  api.abi_major   = OCUDU_DAPP_ABI_MAJOR_V1;
  instance_manager manager(DAPP_TEST_CATALOG, host, api);

  artifact_verifier verifier(DAPP_TEST_CATALOG);
  auto              inspected = verifier.inspect(DAPP_CLASS_C_PROCESS_WORKER);
  ASSERT_TRUE(inspected) << inspected.detail;
  management::package_record package;
  package.package_id              = make_identifier(20);
  package.global_name             = "org.deepsig.reference.spectrum-process";
  package.vendor                  = "DeepSig";
  package.version                 = "1.0.0";
  package.canonical_artifact_path = DAPP_CLASS_C_PROCESS_WORKER;
  package.content_digest          = inspected.digest;
  package.placement_mask          = OCUDU_DAPP_PLACEMENT_OUT_OF_PROCESS_V1;
  package.backends                = {management::backend::cpu};

  std::vector<std::shared_ptr<test_process_endpoint>> loaded_endpoints;
  std::vector<management::interface_record>           interfaces{{OCUDU_DAPP_INTERFACE_SPECTRUM_V1,
                                                                  1,
                                                                  0,
                                                                  OCUDU_DAPP_CLASS_C_OPEN_LOOP_V1,
                                                                  OCUDU_DAPP_PLACEMENT_OUT_OF_PROCESS_V1,
                                                                  OCUDU_DAPP_THREADING_PER_WORKER_V1,
                                                                  0}};
  package.interfaces = interfaces;
  ASSERT_TRUE(manager.register_package(package));
  auto registered = manager.register_process_factory(
      package.package_id,
      [&loaded_endpoints](std::unique_ptr<approved_artifact> executable,
                          management::backend                selected_backend) -> process_runtime_create_result {
        if (selected_backend != management::backend::cpu) {
          return {{OCUDU_DAPP_UNSUPPORTED_V1, OCUDU_DAPP_STATE_ERROR_V1, "test backend rejected"}, {}};
        }
        auto configured                            = make_config_with_artifact(std::move(executable));
        configured.config.maximum_restart_attempts = 1;
        loaded_endpoints                           = configured.endpoints;
        auto supervised                            = class_c_process_supervisor::create(std::move(configured.config));
        if (!supervised) {
          return {{supervised.operation.status, OCUDU_DAPP_STATE_ERROR_V1, supervised.operation.detail}, {}};
        }
        auto                          instance = class_c_process_instance::create(std::move(supervised.supervisor));
        process_runtime_create_result result{instance.operation, {}};
        result.runtime = std::move(instance.instance);
        return result;
      });
  ASSERT_TRUE(registered) << registered.detail;

  management::load_request load;
  load.transaction_id   = make_identifier(21);
  load.package_id       = package.package_id;
  load.placement        = OCUDU_DAPP_PLACEMENT_OUT_OF_PROCESS_V1;
  load.selected_backend = management::backend::cpu;
  management::operation_result loaded;
#ifdef OCUDU_DAPP_TEST_E3
  e3::embedded_agent agent(manager, 4, 16);
  auto               submitted = agent.submit(load);
  ASSERT_TRUE(submitted) << submitted.detail;
  ASSERT_TRUE(submitted.job_id);
  auto completed = agent.wait(*submitted.job_id, std::chrono::steady_clock::now() + 1s);
  ASSERT_TRUE(completed);
  ASSERT_TRUE(completed->result);
  loaded = *completed->result;
#else
  loaded = manager.load(load);
#endif
  ASSERT_TRUE(loaded) << loaded.detail;
  ASSERT_TRUE(loaded.instance_id);
  ASSERT_EQ(loaded_endpoints.size(), 2U);
  auto record = manager.instance(*loaded.instance_id);
  ASSERT_TRUE(record);
  EXPECT_EQ(record->placement, OCUDU_DAPP_PLACEMENT_OUT_OF_PROCESS_V1);
  ASSERT_EQ(record->interfaces.size(), interfaces.size());
  EXPECT_EQ(record->interfaces.front().interface_id, interfaces.front().interface_id);
  EXPECT_EQ(record->interfaces.front().dapp_class, OCUDU_DAPP_CLASS_C_OPEN_LOOP_V1);

  auto control = [&](management::lifecycle_action action, uint8_t transaction) {
    management::lifecycle_request request;
    request.transaction_id = make_identifier(transaction);
    request.instance_id    = *loaded.instance_id;
    request.action         = action;
    return manager.control(request);
  };
  ASSERT_TRUE(control(management::lifecycle_action::prepare, 22));
  ASSERT_TRUE(control(management::lifecycle_action::arm, 23));
  ASSERT_TRUE(control(management::lifecycle_action::enter_shadow, 24));
  for (const auto& endpoint : loaded_endpoints) {
    EXPECT_FALSE(endpoint->active());
  }
  ASSERT_TRUE(control(management::lifecycle_action::activate, 25));
  for (const auto& endpoint : loaded_endpoints) {
    EXPECT_TRUE(endpoint->active());
  }
#ifdef OCUDU_DAPP_TEST_E3
  auto config_snapshot = agent.config(*loaded.instance_id);
#else
  auto config_snapshot = manager.config(*loaded.instance_id);
#endif
  ASSERT_TRUE(config_snapshot) << config_snapshot.detail;
  ASSERT_EQ(config_snapshot.value.generation, 1U);
  management::config_set_request set_config;
  set_config.transaction_id             = make_identifier(60);
  set_config.instance_id                = *loaded.instance_id;
  set_config.expected_config_generation = 1;
  set_config.entries.push_back({"enabled", false});
#ifdef OCUDU_DAPP_TEST_E3
  auto config_job = agent.submit(set_config);
  ASSERT_TRUE(config_job) << config_job.detail;
  auto config_done = agent.wait(*config_job.job_id, std::chrono::steady_clock::now() + 1s);
  ASSERT_TRUE(config_done);
  ASSERT_TRUE(config_done->result);
  ASSERT_TRUE(*config_done->result) << config_done->result->detail;
#else
  ASSERT_TRUE(manager.set_config(set_config));
#endif
  management::model_request model;
  model.transaction_id            = make_identifier(61);
  model.instance_id               = *loaded.instance_id;
  model.expected_model_generation = 1;
  model.action                    = OCUDU_DAPP_MODEL_ACTIVATE_V1;
  model.model_id                  = "fixture-model";
#ifdef OCUDU_DAPP_TEST_E3
  auto model_job = agent.submit(model);
  ASSERT_TRUE(model_job) << model_job.detail;
  auto model_done = agent.wait(*model_job.job_id, std::chrono::steady_clock::now() + 1s);
  ASSERT_TRUE(model_done);
  ASSERT_TRUE(model_done->result);
  ASSERT_TRUE(*model_done->result) << model_done->result->detail;
#else
  ASSERT_TRUE(manager.manage_model(model));
#endif
  auto metrics = manager.metrics(*loaded.instance_id);
  ASSERT_TRUE(metrics) << metrics.detail;
  EXPECT_NE(std::find_if(metrics.value.begin(),
                         metrics.value.end(),
                         [](const auto& metric) {
                           return metric.name == "process.route_generation" &&
                                  std::get<uint64_t>(metric.sample_value) == 1;
                         }),
            metrics.value.end());

  auto pid_metric = std::find_if(
      metrics.value.begin(), metrics.value.end(), [](const auto& metric) { return metric.name == "process.pid"; });
  ASSERT_NE(pid_metric, metrics.value.end());
  const auto worker_pid = std::get<uint64_t>(pid_metric->sample_value);
  ASSERT_GT(worker_pid, 0U);
  ASSERT_EQ(::kill(static_cast<pid_t>(worker_pid), SIGKILL), 0);

  uint64_t   restarted_pid{};
  const auto restart_deadline = std::chrono::steady_clock::now() + 2s;
  do {
    metrics = manager.metrics(*loaded.instance_id);
    ASSERT_TRUE(metrics) << metrics.detail;
    auto pid = std::find_if(
        metrics.value.begin(), metrics.value.end(), [](const auto& metric) { return metric.name == "process.pid"; });
    auto restarts = std::find_if(metrics.value.begin(), metrics.value.end(), [](const auto& metric) {
      return metric.name == "process.successful_restarts";
    });
    if (pid != metrics.value.end() && restarts != metrics.value.end() &&
        std::get<uint64_t>(restarts->sample_value) == 1U) {
      restarted_pid = std::get<uint64_t>(pid->sample_value);
      break;
    }
    std::this_thread::sleep_for(1ms);
  } while (std::chrono::steady_clock::now() < restart_deadline);
  ASSERT_GT(restarted_pid, 0U);
  EXPECT_NE(restarted_pid, worker_pid);
  record = manager.instance(*loaded.instance_id);
  ASSERT_TRUE(record);
  ASSERT_EQ(record->state, OCUDU_DAPP_STATE_ACTIVE_V1);
  EXPECT_GT(record->generation, 5U);
#ifdef OCUDU_DAPP_TEST_E3
  config_snapshot = agent.config(*loaded.instance_id);
#else
  config_snapshot = manager.config(*loaded.instance_id);
#endif
  ASSERT_TRUE(config_snapshot) << config_snapshot.detail;
  ASSERT_EQ(config_snapshot.value.generation, 2U);
  ASSERT_EQ(config_snapshot.value.entries.size(), 1U);
  EXPECT_FALSE(std::get<bool>(config_snapshot.value.entries.front().entry_value));
  metrics = manager.metrics(*loaded.instance_id);
  ASSERT_TRUE(metrics) << metrics.detail;
  const auto replays = std::find_if(metrics.value.begin(), metrics.value.end(), [](const auto& metric) {
    return metric.name == "process.management_replays";
  });
  ASSERT_NE(replays, metrics.value.end());
  EXPECT_EQ(std::get<uint64_t>(replays->sample_value), 2U);
  const auto restarted_generation = record->generation;
  for (const auto& endpoint : loaded_endpoints) {
    EXPECT_TRUE(endpoint->active());
  }
  ASSERT_EQ(::kill(static_cast<pid_t>(restarted_pid), SIGKILL), 0);

  const auto failure_deadline = std::chrono::steady_clock::now() + 2s;
  do {
    record = manager.instance(*loaded.instance_id);
    ASSERT_TRUE(record);
    if (record->state == OCUDU_DAPP_STATE_DEGRADED_V1) {
      break;
    }
    std::this_thread::sleep_for(1ms);
  } while (std::chrono::steady_clock::now() < failure_deadline);
  ASSERT_EQ(record->state, OCUDU_DAPP_STATE_DEGRADED_V1);
  EXPECT_GT(record->generation, restarted_generation);
  for (const auto& endpoint : loaded_endpoints) {
    EXPECT_FALSE(endpoint->active());
  }
#ifdef OCUDU_DAPP_TEST_E3
  const auto incidents = agent.incidents();
#else
  manager.drain_incidents();
  const auto incidents = manager.incidents().query();
#endif
  ASSERT_EQ(incidents.size(), 1U);
  EXPECT_EQ(incidents.front().cause, incident_cause::process_exit);
  EXPECT_EQ(incidents.front().action, incident_action::disabled);
  EXPECT_EQ(incidents.front().recurrence_count, 2U);
  EXPECT_EQ(record->last_incident_id, incidents.front().incident_id);

  ASSERT_TRUE(control(management::lifecycle_action::deactivate, 26));
  for (const auto& endpoint : loaded_endpoints) {
    EXPECT_FALSE(endpoint->active());
  }
  ASSERT_TRUE(control(management::lifecycle_action::unload, 27));
  EXPECT_EQ(manager.instance(*loaded.instance_id)->state, OCUDU_DAPP_STATE_UNLOADED_V1);
#ifdef OCUDU_DAPP_TEST_E3
  agent.stop();
#endif
}
