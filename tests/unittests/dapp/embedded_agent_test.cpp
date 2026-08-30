// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/abi/v1/metric_catalog.h"
#include "ocudu/dapp/e3/embedded_agent.h"
#include <algorithm>
#include <cstring>
#include <gtest/gtest.h>

using namespace ocudu::dapp;
using namespace ocudu::dapp::e3;
using namespace ocudu::dapp::management;
using namespace std::chrono_literals;

namespace {

identifier test_id(uint64_t value)
{
  identifier result;
  std::memcpy(result.bytes.data() + 8, &value, sizeof(value));
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

package_record package()
{
  artifact_verifier verifier(DAPP_TEST_CATALOG);
  auto              inspected = verifier.inspect(DAPP_VALID_FIXTURE);
  EXPECT_TRUE(inspected);
  package_record result;
  result.package_id              = test_id(1);
  result.global_name             = "org.deepsig.ocudu.test.valid";
  result.vendor                  = "DeepSig";
  result.version                 = "1.0.0";
  result.canonical_artifact_path = DAPP_VALID_FIXTURE;
  result.content_digest          = inspected.digest;
  result.backends                = {backend::cpu};
  return result;
}

operation_result await(embedded_agent& agent, const job_submit_result& submitted)
{
  EXPECT_TRUE(submitted);
  EXPECT_TRUE(submitted.job_id);
  auto completed = agent.wait(*submitted.job_id, std::chrono::steady_clock::now() + 1s);
  EXPECT_TRUE(completed);
  EXPECT_EQ(completed->state, job_state::succeeded);
  EXPECT_TRUE(completed->result);
  return completed->result.value_or(operation_result{});
}

} // namespace

TEST(dapp_embedded_e3_agent, owns_async_requests_and_exposes_sync_queries)
{
  auto manager = make_manager();
  ASSERT_TRUE(manager->register_package(package()));
  embedded_agent agent(*manager, 8, 32);

  load_request load;
  load.transaction_id = test_id(10);
  load.package_id     = test_id(1);
  auto loaded         = await(agent, agent.submit(load));
  ASSERT_TRUE(loaded && loaded.instance_id);
  EXPECT_EQ(agent.inventory().instances.size(), 1U);
  EXPECT_TRUE(agent.config_schema(*loaded.instance_id));
  auto metric_schema = agent.metric_schema(*loaded.instance_id);
  ASSERT_TRUE(metric_schema);
  EXPECT_NE(std::find_if(metric_schema.value.begin(),
                         metric_schema.value.end(),
                         [](const auto& metric) { return metric.name == OCUDU_DAPP_METRIC_HOST_INPUT_CELL_BITMAP_V1; }),
            metric_schema.value.end());
  EXPECT_NE(std::find_if(metric_schema.value.begin(),
                         metric_schema.value.end(),
                         [](const auto& metric) { return metric.name == OCUDU_DAPP_METRIC_HOST_NATIVE_WORKERS_V1; }),
            metric_schema.value.end());
  EXPECT_NE(
      std::find_if(metric_schema.value.begin(),
                   metric_schema.value.end(),
                   [](const auto& metric) { return metric.name == OCUDU_DAPP_METRIC_HOST_NATIVE_WORKERS_ACTIVE_V1; }),
      metric_schema.value.end());
  auto metrics = agent.metrics(*loaded.instance_id);
  ASSERT_TRUE(metrics);
  const auto cells = std::find_if(metrics.value.begin(), metrics.value.end(), [](const auto& metric) {
    return metric.name == OCUDU_DAPP_METRIC_HOST_INPUT_CELL_BITMAP_V1;
  });
  ASSERT_NE(cells, metrics.value.end());
  EXPECT_EQ(std::get<uint64_t>(cells->sample_value), 0U);
  const auto native_workers = std::find_if(metrics.value.begin(), metrics.value.end(), [](const auto& metric) {
    return metric.name == OCUDU_DAPP_METRIC_HOST_NATIVE_WORKERS_V1;
  });
  ASSERT_NE(native_workers, metrics.value.end());
  EXPECT_EQ(std::get<uint64_t>(native_workers->sample_value), 0U);
  const auto active_native_workers = std::find_if(metrics.value.begin(), metrics.value.end(), [](const auto& metric) {
    return metric.name == OCUDU_DAPP_METRIC_HOST_NATIVE_WORKERS_ACTIVE_V1;
  });
  ASSERT_NE(active_native_workers, metrics.value.end());
  EXPECT_EQ(std::get<uint64_t>(active_native_workers->sample_value), 0U);

  config_set_request config;
  config.transaction_id               = test_id(11);
  config.instance_id                  = *loaded.instance_id;
  config.expected_instance_generation = 1;
  config.expected_config_generation   = 1;
  config.entries.push_back({"threshold", uint64_t{12}});
  auto configured = await(agent, agent.submit(config));
  EXPECT_EQ(configured.instance_generation, 2U);

  model_request model;
  model.transaction_id               = test_id(12);
  model.instance_id                  = *loaded.instance_id;
  model.expected_instance_generation = 2;
  model.expected_model_generation    = 1;
  model.action                       = OCUDU_DAPP_MODEL_VALIDATE_V1;
  model.model_id                     = "fixture-model";
  auto modeled                       = await(agent, agent.submit(model));
  EXPECT_EQ(modeled.instance_generation, 3U);

  config.transaction_id               = test_id(14);
  config.expected_instance_generation = 3;
  config.expected_config_generation   = 1;
  auto failed                         = agent.submit(config);
  ASSERT_TRUE(failed && failed.job_id);
  auto failed_job = agent.wait(*failed.job_id, std::chrono::steady_clock::now() + 1s);
  ASSERT_TRUE(failed_job);
  EXPECT_EQ(failed_job->state, job_state::failed);
  EXPECT_EQ(agent.instance(*loaded.instance_id)->generation, 4U);
  EXPECT_GT(agent.health().total, 0U);
  auto incidents = agent.incidents();
  ASSERT_FALSE(incidents.empty());
  EXPECT_GT(agent.clear_incidents_through(incidents.back().incident_id), 0U);

  lifecycle_request unload;
  unload.transaction_id      = test_id(13);
  unload.instance_id         = *loaded.instance_id;
  unload.action              = lifecycle_action::unload;
  unload.expected_generation = 4;
  EXPECT_TRUE(await(agent, agent.submit(unload)));
}

TEST(dapp_embedded_e3_agent, rejects_pointer_payloads_and_detects_transaction_mismatch)
{
  auto manager = make_manager();
  ASSERT_TRUE(manager->register_package(package()));
  embedded_agent agent(*manager);

  load_request load;
  load.transaction_id = test_id(20);
  load.package_id     = test_id(1);
  load.create_args    = reinterpret_cast<void*>(1);
  EXPECT_EQ(agent.submit(load).code, job_submit_code::invalid_request);

  load.create_args = nullptr;
  auto first       = agent.submit(load);
  auto loaded      = await(agent, first);
  ASSERT_TRUE(loaded && loaded.instance_id);
  load.selected_backend = backend::cuda;
  EXPECT_EQ(agent.submit(load).code, job_submit_code::transaction_mismatch);

  lifecycle_request unload;
  unload.transaction_id      = test_id(21);
  unload.instance_id         = *loaded.instance_id;
  unload.action              = lifecycle_action::unload;
  unload.expected_generation = 1;
  EXPECT_TRUE(await(agent, agent.submit(unload)));
}
