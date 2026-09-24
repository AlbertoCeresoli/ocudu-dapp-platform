// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "fixtures/fixture_common.h"
#include "ocudu/dapp/runtime/native_hook_point.h"
#include "ocudu/dapp/runtime/native_hook_slot.h"
#include "ocudu/dapp/runtime/native_instance.h"
#include "ocudu/dapp/use_cases/v1/srs_channel_estimator.h"
#include <array>
#include <gtest/gtest.h>
#include <initializer_list>
#include <thread>

using namespace ocudu::dapp;
using namespace std::chrono_literals;

namespace {

std::shared_ptr<native_module_library> load_fixture(const char* path = DAPP_VALID_FIXTURE)
{
  artifact_verifier verifier(DAPP_TEST_CATALOG);
  auto              inspected = verifier.inspect(path);
  if (!inspected) {
    return {};
  }
  auto approved = verifier.verify(path, inspected.digest);
  if (!approved) {
    return {};
  }
  ocudu_dapp_host_info_v1 host{};
  host.struct_size = sizeof(host);
  host.abi_major   = OCUDU_DAPP_ABI_MAJOR_V1;
  host.abi_minor   = OCUDU_DAPP_ABI_MINOR_V1;
  auto loaded      = load_native_module(std::move(*approved.artifact), host);
  return loaded.library;
}

uint64_t fixture_monotonic_ns(void* context)
{
  return static_cast<fixture_control*>(context)->monotonic_ns.load(std::memory_order_relaxed);
}

ocudu_dapp_host_api_v1 host_api(fixture_control* control = nullptr)
{
  ocudu_dapp_host_api_v1 api{};
  api.struct_size = sizeof(api);
  api.abi_major   = OCUDU_DAPP_ABI_MAJOR_V1;
  api.abi_minor   = OCUDU_DAPP_ABI_MINOR_V1;
  if (control != nullptr) {
    api.host_context = control;
    api.monotonic_ns = fixture_monotonic_ns;
  }
  return api;
}

instance_create_result create_fixture(fixture_control& control)
{
  return native_instance::create(load_fixture(), host_api(), &control);
}

instance_create_result create_class_a_fixture(fixture_control& control)
{
  return native_instance::create(load_fixture(DAPP_CLASS_A_FIXTURE), host_api(&control), &control);
}

instance_create_result create_class_a_srs_fixture(fixture_control& control)
{
  return native_instance::create(load_fixture(DAPP_CLASS_A_SRS_FIXTURE), host_api(&control), &control);
}

instance_create_result create_class_b_fixture(fixture_control& control)
{
  return native_instance::create(load_fixture(DAPP_CLASS_B_FIXTURE), host_api(&control), &control);
}

instance_create_result create_class_c_fixture(fixture_control& control)
{
  return native_instance::create(load_fixture(DAPP_CLASS_C_FIXTURE), host_api(&control), &control);
}

instance_create_result create_class_c_srs_fixture(fixture_control& control)
{
  return native_instance::create(load_fixture(DAPP_CLASS_C_SRS_FIXTURE), host_api(&control), &control);
}

struct complex32 {
  float real;
  float imag;
};

class controllable_completion_barrier final : public native_completion_barrier
{
public:
  bool wait_until(std::chrono::steady_clock::time_point) noexcept override
  {
    waits.fetch_add(1, std::memory_order_relaxed);
    return complete.load(std::memory_order_acquire);
  }

  std::atomic<bool>     complete{};
  std::atomic<uint32_t> waits{};
};

ocudu_dapp_tensor_view_v1 make_tensor(const complex32* data)
{
  ocudu_dapp_tensor_view_v1 tensor{};
  tensor.struct_size           = sizeof(tensor);
  tensor.abi_major             = OCUDU_DAPP_ABI_MAJOR_V1;
  tensor.abi_minor             = OCUDU_DAPP_ABI_MINOR_V1;
  tensor.domain                = OCUDU_DAPP_MEM_HOST_V1;
  tensor.element_type          = OCUDU_DAPP_ELEMENT_CF32_V1;
  tensor.rank                  = 2;
  tensor.access                = OCUDU_DAPP_ACCESS_READ_V1 | OCUDU_DAPP_ACCESS_HOST_VISIBLE_V1;
  tensor.allocation_id         = 1;
  tensor.allocation_generation = 1;
  tensor.native_handle         = reinterpret_cast<uintptr_t>(data);
  tensor.byte_length           = 4 * sizeof(complex32);
  tensor.dimensions[0]         = 2;
  tensor.dimensions[1]         = 2;
  tensor.byte_strides[0]       = 2 * sizeof(complex32);
  tensor.byte_strides[1]       = sizeof(complex32);
  tensor.lease_token           = 1;
  return tensor;
}

ocudu_dapp_channel_estimator_input_v1
make_channel_estimator_input(uint32_t worker_id, const complex32* data, uint32_t nof_layers = 1)
{
  static thread_local ocudu_dapp_pusch_metadata_v1 pusch;
  static thread_local ocudu_dapp_dmrs_metadata_v1  dmrs;
  pusch                 = {};
  pusch.struct_size     = sizeof(pusch);
  pusch.abi_major       = OCUDU_DAPP_ABI_MAJOR_V1;
  pusch.nof_rx_ports    = 1;
  pusch.nof_layers      = nof_layers;
  pusch.modulation      = OCUDU_DAPP_MODULATION_UNSPECIFIED_V1;
  pusch.nof_symbols     = 1;
  pusch.nof_subcarriers = 1;
  dmrs                  = {};
  dmrs.struct_size      = sizeof(dmrs);
  dmrs.abi_major        = OCUDU_DAPP_ABI_MAJOR_V1;
  dmrs.nof_pilots       = 1;
  dmrs.flags =
      OCUDU_DAPP_DMRS_REFERENCE_SYMBOLS_LAYER_COMPLETE_V1 | OCUDU_DAPP_DMRS_COORDINATES_SHARED_ACROSS_LAYERS_V1;
  dmrs.dmrs_type = 1;
  ocudu_dapp_channel_estimator_input_v1 input{};
  input.invocation.struct_size        = sizeof(input.invocation);
  input.invocation.abi_major          = OCUDU_DAPP_ABI_MAJOR_V1;
  input.invocation.abi_minor          = OCUDU_DAPP_ABI_MINOR_V1;
  input.invocation.invocation_id      = worker_id + 100;
  input.invocation.worker_id          = worker_id;
  input.rx_grid                       = make_tensor(data);
  input.pusch.struct_size             = sizeof(input.pusch);
  input.pusch.abi_major               = OCUDU_DAPP_ABI_MAJOR_V1;
  input.pusch.metadata_schema_id      = OCUDU_DAPP_METADATA_PUSCH_V1;
  input.pusch.metadata_schema_version = 1;
  input.pusch.metadata                = {reinterpret_cast<const uint8_t*>(&pusch), sizeof(pusch)};
  input.dmrs.struct_size              = sizeof(input.dmrs);
  input.dmrs.abi_major                = OCUDU_DAPP_ABI_MAJOR_V1;
  input.dmrs.metadata_schema_id       = OCUDU_DAPP_METADATA_DMRS_V1;
  input.dmrs.metadata_schema_version  = 1;
  input.dmrs.metadata                 = {reinterpret_cast<const uint8_t*>(&dmrs), sizeof(dmrs)};
  return input;
}

ocudu_dapp_channel_estimator_output_v1 make_channel_estimator_output(const ocudu_dapp_channel_estimator_input_v1& input)
{
  ocudu_dapp_channel_estimator_output_v1 output{};
  output.channel_estimates            = input.rx_grid;
  output.channel_estimates.access     = OCUDU_DAPP_ACCESS_WRITE_V1 | OCUDU_DAPP_ACCESS_HOST_VISIBLE_V1;
  output.noise_variances              = input.rx_grid;
  output.noise_variances.element_type = OCUDU_DAPP_ELEMENT_F32_V1;
  output.noise_variances.access       = OCUDU_DAPP_ACCESS_WRITE_V1 | OCUDU_DAPP_ACCESS_HOST_VISIBLE_V1;
  return output;
}

ocudu_dapp_scheduler_input_v1
make_scheduler_input(uint32_t worker_id, const ocudu_dapp_scheduler_candidate_v1* candidates, uint32_t nof_candidates)
{
  ocudu_dapp_scheduler_input_v1 input{};
  input.invocation.struct_size   = sizeof(input.invocation);
  input.invocation.abi_major     = OCUDU_DAPP_ABI_MAJOR_V1;
  input.invocation.abi_minor     = OCUDU_DAPP_ABI_MINOR_V1;
  input.invocation.invocation_id = worker_id + 200;
  input.invocation.worker_id     = worker_id;
  input.direction                = OCUDU_DAPP_SCHEDULER_UL_V1;
  input.nof_candidates           = nof_candidates;
  input.cell_nof_prbs            = 52;
  input.candidate_generation     = 9;
  input.candidates               = candidates;
  return input;
}

ocudu_dapp_scheduler_output_v1 make_scheduler_output(ocudu_dapp_scheduler_intent_v1* intents, uint32_t capacity)
{
  ocudu_dapp_scheduler_output_v1 output{};
  output.struct_size     = sizeof(output);
  output.abi_major       = OCUDU_DAPP_ABI_MAJOR_V1;
  output.abi_minor       = OCUDU_DAPP_ABI_MINOR_V1;
  output.intents         = intents;
  output.intent_capacity = capacity;
  return output;
}

void advance_to_active(native_instance& instance)
{
  ASSERT_TRUE(instance.prepare(nullptr, nullptr));
  ASSERT_TRUE(instance.arm(nullptr));
  ASSERT_TRUE(instance.enter_shadow());
  ASSERT_TRUE(instance.activate());
}

} // namespace

TEST(dapp_native_instance, executes_nominal_lifecycle_and_unloads_only_after_quiescence)
{
  fixture_control control;
  auto            created = create_fixture(control);
  ASSERT_TRUE(created) << created.operation.detail;
  EXPECT_EQ(created.instance->state(), OCUDU_DAPP_STATE_LOADED_V1);
  advance_to_active(*created.instance);
  EXPECT_TRUE(created.instance->accepts_activity());
  EXPECT_TRUE(created.instance->drain(std::chrono::steady_clock::now() + 100ms));
  EXPECT_EQ(created.instance->state(), OCUDU_DAPP_STATE_INACTIVE_V1);
  EXPECT_TRUE(created.instance->unload());
  EXPECT_EQ(created.instance->state(), OCUDU_DAPP_STATE_UNLOADED_V1);
  EXPECT_EQ(control.creates, 1U);
  EXPECT_EQ(control.configures, 1U);
  EXPECT_EQ(control.prepares, 1U);
  EXPECT_EQ(control.warmups, 1U);
  EXPECT_EQ(control.starts, 1U);
  EXPECT_EQ(control.stops, 1U);
  EXPECT_EQ(control.quiesces, 1U);
  EXPECT_EQ(control.destroys, 1U);
}

TEST(dapp_native_instance, outstanding_activity_causes_resident_quarantine_then_retry)
{
  fixture_control control;
  auto            created = create_fixture(control);
  ASSERT_TRUE(created);
  advance_to_active(*created.instance);
  auto invocation = created.instance->try_enter();
  auto lease      = created.instance->try_enter(activity_kind::lease);
  auto gpu_work   = created.instance->try_enter(activity_kind::accelerator_work);
  ASSERT_TRUE(invocation && lease && gpu_work);

  auto drained = created.instance->drain(std::chrono::steady_clock::now() + 2ms);
  EXPECT_FALSE(drained);
  EXPECT_EQ(created.instance->state(), OCUDU_DAPP_STATE_QUARANTINED_RESIDENT_V1);
  EXPECT_FALSE(created.instance->try_enter());
  EXPECT_FALSE(created.instance->unload());

  invocation.reset();
  lease.reset();
  gpu_work.reset();
  EXPECT_TRUE(created.instance->retry_quiescence(std::chrono::steady_clock::now() + 100ms));
  EXPECT_TRUE(created.instance->unload());
}

TEST(dapp_native_instance, throwing_stop_is_contained_and_quarantines_module)
{
  fixture_control control;
  control.throw_stop = true;
  auto created       = create_fixture(control);
  ASSERT_TRUE(created);
  advance_to_active(*created.instance);
  EXPECT_FALSE(created.instance->drain(std::chrono::steady_clock::now() + 100ms));
  EXPECT_EQ(created.instance->state(), OCUDU_DAPP_STATE_QUARANTINED_RESIDENT_V1);
  incident_record incident;
  bool            saw_exception = false;
  while (created.instance->try_pop_incident(incident)) {
    saw_exception |= incident.cause == incident_cause::standard_exception;
  }
  EXPECT_TRUE(saw_exception);
}

TEST(dapp_native_instance, throwing_destroy_never_releases_executable_image)
{
  fixture_control control;
  control.throw_destroy = true;
  auto created          = create_fixture(control);
  ASSERT_TRUE(created);
  advance_to_active(*created.instance);
  ASSERT_TRUE(created.instance->drain(std::chrono::steady_clock::now() + 100ms));
  EXPECT_FALSE(created.instance->unload());
  EXPECT_EQ(created.instance->state(), OCUDU_DAPP_STATE_QUARANTINED_RESIDENT_V1);
  EXPECT_FALSE(created.instance->retry_quiescence(std::chrono::steady_clock::now() + 100ms));
  const auto before = quarantined_native_library_count();
  created.instance.reset();
  EXPECT_EQ(quarantined_native_library_count(), before + 1);
}

TEST(dapp_native_instance, preparation_error_is_visible_and_prevents_activation)
{
  fixture_control control;
  control.prepare_status = OCUDU_DAPP_RESOURCE_V1;
  auto created           = create_fixture(control);
  ASSERT_TRUE(created);
  auto prepared = created.instance->prepare(nullptr, nullptr);
  EXPECT_EQ(prepared.status, OCUDU_DAPP_RESOURCE_V1);
  EXPECT_EQ(created.instance->state(), OCUDU_DAPP_STATE_ERROR_V1);
  EXPECT_FALSE(created.instance->arm(nullptr));
  EXPECT_TRUE(created.instance->drain(std::chrono::steady_clock::now() + 100ms));
  EXPECT_TRUE(created.instance->unload());
}

TEST(dapp_native_instance, loaded_but_unused_instance_can_be_destroyed_without_start_stop)
{
  fixture_control control;
  auto            created = create_fixture(control);
  ASSERT_TRUE(created);
  EXPECT_TRUE(created.instance->unload());
  EXPECT_EQ(control.starts, 0U);
  EXPECT_EQ(control.stops, 0U);
  EXPECT_EQ(control.destroys, 1U);
}

TEST(dapp_native_instance, unsafe_owner_destruction_retains_tracker_until_outstanding_token_releases)
{
  fixture_control control;
  auto            created = create_fixture(control);
  ASSERT_TRUE(created);
  advance_to_active(*created.instance);
  auto token = created.instance->try_enter();
  ASSERT_TRUE(token);
  const auto before = quarantined_native_library_count();
  created.instance.reset();
  EXPECT_EQ(quarantined_native_library_count(), before + 1);
  token.reset();
}

TEST(dapp_native_instance, common_management_is_available_independently_of_data_path_class)
{
  fixture_control control;
  auto            created = create_fixture(control);
  ASSERT_TRUE(created);
  EXPECT_EQ(created.instance->management_capabilities(),
            OCUDU_DAPP_MANAGEMENT_CONFIG_V1 | OCUDU_DAPP_MANAGEMENT_METRICS_V1 | OCUDU_DAPP_MANAGEMENT_MODELS_V1);

  auto config_count = created.instance->describe_config(nullptr, 0);
  ASSERT_TRUE(config_count);
  EXPECT_EQ(config_count.count, 1U);
  ocudu_dapp_config_descriptor_v1 config_descriptor{};
  EXPECT_TRUE(created.instance->describe_config(&config_descriptor, 1));
  EXPECT_STREQ(config_descriptor.key, "threshold");

  ocudu_dapp_config_entry_v1 entry{};
  auto                       snapshot = created.instance->get_config(&entry, 1);
  ASSERT_TRUE(snapshot);
  EXPECT_EQ(snapshot.generation, 1U);
  EXPECT_STREQ(entry.key, "threshold");

  ocudu_dapp_config_update_v1 update{};
  update.struct_size         = sizeof(update);
  update.expected_generation = snapshot.generation;
  update.boundary            = OCUDU_DAPP_CONFIG_BOUNDARY_INVOCATION_V1;
  auto updated               = created.instance->set_config(update);
  ASSERT_TRUE(updated);
  EXPECT_EQ(updated.generation, 2U);

  auto metric_count = created.instance->describe_metrics(nullptr, 0);
  ASSERT_TRUE(metric_count);
  EXPECT_EQ(metric_count.count, 1U);
  ocudu_dapp_metric_sample_v1 metric{};
  EXPECT_TRUE(created.instance->read_metrics(&metric, 1));
  EXPECT_STREQ(metric.name, "fixture.calls");

  ocudu_dapp_model_request_v1 model{};
  model.struct_size         = sizeof(model);
  model.action              = OCUDU_DAPP_MODEL_ACTIVATE_V1;
  model.model_id            = "fixture-model";
  model.expected_generation = 1;
  auto modeled              = created.instance->manage_model(model);
  ASSERT_TRUE(modeled);
  EXPECT_EQ(modeled.generation, 2U);
}

TEST(dapp_native_instance, management_exceptions_are_contained_and_reported)
{
  fixture_control control;
  auto            created = create_fixture(control);
  ASSERT_TRUE(created);
  control.throw_management = true;
  auto queried             = created.instance->get_config(nullptr, 0);
  EXPECT_FALSE(queried);
  EXPECT_EQ(queried.operation.status, OCUDU_DAPP_EXCEPTION_V1);
  incident_record incident;
  ASSERT_TRUE(created.instance->try_pop_incident(incident));
  EXPECT_EQ(incident.cause, incident_cause::standard_exception);
}

TEST(dapp_native_instance, class_a_worker_contexts_execute_concurrently_without_a_global_hot_path_lock)
{
  fixture_control control;
  auto            created = create_class_a_fixture(control);
  ASSERT_TRUE(created);
  advance_to_active(*created.instance);
  auto first  = created.instance->create_worker(OCUDU_DAPP_INTERFACE_CHANNEL_ESTIMATOR_V1, 3);
  auto second = created.instance->create_worker(OCUDU_DAPP_INTERFACE_CHANNEL_ESTIMATOR_V1, 4);
  ASSERT_TRUE(first) << first.detail;
  ASSERT_TRUE(second) << second.detail;
  EXPECT_EQ(first.worker->backend_id(), OCUDU_DAPP_BACKEND_HOST_V1);
  EXPECT_EQ(second.worker->backend_id(), OCUDU_DAPP_BACKEND_HOST_V1);

  const std::array<complex32, 4> samples{{{1, 0}, {0, 1}, {1, 1}, {2, 0}}};
  auto                           input_first  = make_channel_estimator_input(3, samples.data());
  auto                           input_second = make_channel_estimator_input(4, samples.data());
  input_first.invocation.cell_id              = 1;
  input_second.invocation.cell_id             = 3;
  auto                     output_first       = make_channel_estimator_output(input_first);
  auto                     output_second      = make_channel_estimator_output(input_second);
  native_invocation_result result_first;
  native_invocation_result result_second;
  control.block_data_invocations.store(true, std::memory_order_release);
  std::thread first_thread([&] { result_first = first.worker->invoke(input_first, output_first); });
  std::thread second_thread([&] { result_second = second.worker->invoke(input_second, output_second); });

  const auto deadline = std::chrono::steady_clock::now() + 500ms;
  while (control.active_data_invocations.load(std::memory_order_acquire) != 2 &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  EXPECT_EQ(control.active_data_invocations.load(std::memory_order_acquire), 2U);
  control.block_data_invocations.store(false, std::memory_order_release);
  first_thread.join();
  second_thread.join();

  EXPECT_TRUE(result_first);
  EXPECT_TRUE(result_second);
  const auto health = created.instance->health();
  EXPECT_EQ(health.input_cell_bitmap, (uint32_t{1} << 1U) | (uint32_t{1} << 3U));
  EXPECT_EQ(health.native_workers, 2U);
  EXPECT_EQ(health.active_native_workers, 2U);
  EXPECT_GE(control.maximum_active_data_invocations.load(std::memory_order_relaxed), 2U);
  EXPECT_EQ(control.data_worker_creates.load(std::memory_order_relaxed), 2U);
  first.worker.reset();
  second.worker.reset();
  EXPECT_EQ(control.data_worker_destroys.load(std::memory_order_relaxed), 2U);
  EXPECT_TRUE(created.instance->drain(std::chrono::steady_clock::now() + 100ms));
  EXPECT_TRUE(created.instance->unload());
}

/// A one-port-pair SRS occasion of four comb-4 pilots, with host tensors the test owns.
struct srs_invocation_fixture {
  explicit srs_invocation_fixture(uint32_t worker_id, uint32_t nof_rx_ports = 1)
  {
    metadata.struct_size              = sizeof(metadata);
    metadata.abi_major                = OCUDU_DAPP_ABI_MAJOR_V1;
    metadata.nof_rx_ports             = nof_rx_ports;
    metadata.nof_tx_ports             = 1;
    metadata.start_symbol             = 13;
    metadata.nof_symbols              = 1;
    metadata.sequence_length          = 4;
    metadata.comb_size                = 4;
    metadata.nof_pilot_sets           = 1;
    metadata.n_cs_max                 = 12;
    metadata.subcarrier_spacing_hz    = 30000;
    metadata.numerology               = 1;
    metadata.grid_nof_subcarriers     = 48;
    metadata.max_time_alignment_s     = 1.0 / (12.0 * 30000.0 * 4.0);
    input.invocation.struct_size      = sizeof(input.invocation);
    input.invocation.abi_major        = OCUDU_DAPP_ABI_MAJOR_V1;
    input.invocation.invocation_id    = worker_id + 200;
    input.invocation.worker_id        = worker_id;
    input.invocation.cell_id          = 2;
    input.rx_symbols                  = tensor(rx.data(),
                              OCUDU_DAPP_ELEMENT_CF32_V1,
                              sizeof(complex32),
                              OCUDU_DAPP_ACCESS_READ_V1,
                              OCUDU_DAPP_LAYOUT_SRS_RX_SET_PORT_SYMBOL_PILOT_CF32_V1,
                                               {1, nof_rx_ports, 1, 4},
                              1);
    input.reference_symbols           = tensor(reference.data(),
                                     OCUDU_DAPP_ELEMENT_CF32_V1,
                                     sizeof(complex32),
                                     OCUDU_DAPP_ACCESS_READ_V1,
                                     OCUDU_DAPP_LAYOUT_SRS_REFERENCE_TX_PILOT_CF32_V1,
                                               {1, 4},
                                     2);
    input.srs.struct_size             = sizeof(input.srs);
    input.srs.abi_major               = OCUDU_DAPP_ABI_MAJOR_V1;
    input.srs.metadata_schema_id      = OCUDU_DAPP_METADATA_SRS_CHANNEL_ESTIMATOR_V1;
    input.srs.metadata_schema_version = 1;
    input.srs.metadata                = {&metadata, sizeof(metadata), 0};
    output.channel_matrix             = tensor(channel.data(),
                                   OCUDU_DAPP_ELEMENT_CF32_V1,
                                   sizeof(complex32),
                                   OCUDU_DAPP_ACCESS_WRITE_V1,
                                   OCUDU_DAPP_LAYOUT_SRS_CHANNEL_MATRIX_RX_TX_CF32_V1,
                                               {nof_rx_ports, 1},
                                   3);
    output.measurements               = tensor(measurements.data(),
                                 OCUDU_DAPP_ELEMENT_F32_V1,
                                 sizeof(float),
                                 OCUDU_DAPP_ACCESS_WRITE_V1,
                                 OCUDU_DAPP_LAYOUT_SRS_MEASUREMENTS_F32_V1,
                                               {OCUDU_DAPP_SRS_MEASUREMENT_COUNT_V1},
                                 4);
  }

  static ocudu_dapp_tensor_view_v1 tensor(void*                           data,
                                          uint32_t                        element_type,
                                          uint32_t                        element_size,
                                          uint32_t                        access,
                                          uint32_t                        layout_id,
                                          std::initializer_list<uint64_t> dimensions,
                                          uint64_t                        allocation)
  {
    ocudu_dapp_tensor_view_v1 view{};
    view.struct_size           = sizeof(view);
    view.abi_major             = OCUDU_DAPP_ABI_MAJOR_V1;
    view.domain                = OCUDU_DAPP_MEM_HOST_V1;
    view.element_type          = element_type;
    view.rank                  = dimensions.size();
    view.access                = access | OCUDU_DAPP_ACCESS_HOST_VISIBLE_V1;
    view.allocation_id         = allocation;
    view.allocation_generation = 1;
    view.lease_token           = 1;
    view.native_handle         = reinterpret_cast<uintptr_t>(data);
    view.layout_id             = layout_id;
    view.layout_version        = 1;
    uint64_t elements          = 1;
    uint32_t index             = 0;
    for (uint64_t dimension : dimensions) {
      view.dimensions[index++] = dimension;
      elements *= dimension;
    }
    uint64_t stride = element_size;
    for (uint32_t dimension = view.rank; dimension != 0; --dimension) {
      view.byte_strides[dimension - 1] = stride;
      stride *= view.dimensions[dimension - 1];
    }
    view.byte_length = elements * element_size;
    return view;
  }

  ocudu_dapp_srs_channel_estimator_metadata_v1           metadata{};
  std::array<complex32, 8>                               rx{{{2, 0}, {0, 2}, {-2, 0}, {0, -2}, {1, 0}, {0, 1}}};
  std::array<complex32, 4>                               reference{{{1, 0}, {0, 1}, {-1, 0}, {0, -1}}};
  std::array<complex32, 2>                               channel{};
  std::array<float, OCUDU_DAPP_SRS_MEASUREMENT_COUNT_V1> measurements{};
  ocudu_dapp_srs_channel_estimator_input_v1              input{};
  ocudu_dapp_srs_channel_estimator_output_v1             output{};
};

TEST(dapp_native_instance, srs_channel_estimator_worker_invokes_the_module_with_validated_tensors)
{
  fixture_control control;
  auto            created = create_class_a_srs_fixture(control);
  ASSERT_TRUE(created);
  advance_to_active(*created.instance);
  auto worker = created.instance->create_worker(OCUDU_DAPP_INTERFACE_SRS_CHANNEL_ESTIMATOR_V1, 6);
  ASSERT_TRUE(worker) << worker.detail;
  EXPECT_EQ(worker.worker->backend_id(), OCUDU_DAPP_BACKEND_HOST_V1);

  srs_invocation_fixture call(6);
  EXPECT_TRUE(worker.worker->invoke(call.input, call.output));
  EXPECT_EQ(control.data_invocations.load(std::memory_order_relaxed), 1U);
  EXPECT_FLOAT_EQ(call.channel[0].real, 2.0F);
  EXPECT_FLOAT_EQ(call.channel[0].imag, 0.0F);
  EXPECT_FLOAT_EQ(call.measurements[OCUDU_DAPP_SRS_MEASUREMENT_RSRP_DB_V1], 2.0F);
  EXPECT_EQ(created.instance->health().input_cell_bitmap, uint32_t{1} << 2U);

  // The worker of another interface is not an SRS estimator.
  EXPECT_FALSE(created.instance->create_worker(OCUDU_DAPP_INTERFACE_CHANNEL_ESTIMATOR_V1, 7));
  worker.worker.reset();
  EXPECT_TRUE(created.instance->drain(std::chrono::steady_clock::now() + 100ms));
  EXPECT_TRUE(created.instance->unload());
}

TEST(dapp_native_instance, srs_channel_estimator_profile_and_contract_are_checked_before_module_callback)
{
  fixture_control control;
  auto            created = create_class_a_srs_fixture(control);
  ASSERT_TRUE(created);
  advance_to_active(*created.instance);
  auto worker = created.instance->create_worker(OCUDU_DAPP_INTERFACE_SRS_CHANNEL_ESTIMATOR_V1, 6);
  ASSERT_TRUE(worker) << worker.detail;
  const auto reason_count = [&](class_a_fallback_reason reason) {
    return created.instance->health().class_a_fallbacks.reasons[static_cast<std::size_t>(reason)];
  };

  // The profile admits two rx ports, one tx port and comb 4.
  {
    srs_invocation_fixture call(6);
    call.metadata.comb_size = 2;
    call.metadata.n_cs_max  = 8;
    EXPECT_EQ(worker.worker->invoke(call.input, call.output).status, OCUDU_DAPP_UNSUPPORTED_V1);
    EXPECT_EQ(reason_count(class_a_fallback_reason::unsupported_allocation), 1U);
  }
  {
    srs_invocation_fixture call(6, 2);
    EXPECT_TRUE(worker.worker->invoke(call.input, call.output));
    EXPECT_EQ(control.data_invocations.load(std::memory_order_relaxed), 1U);
  }
  // Occasions the SRS configuration cannot produce, and tensors that disagree with the metadata.
  const auto malformed = {
      +[](srs_invocation_fixture& call) { call.metadata.n_cs_max = 8; },
      +[](srs_invocation_fixture& call) { call.metadata.cyclic_shift[0] = 12; },
      +[](srs_invocation_fixture& call) { call.metadata.flags = OCUDU_DAPP_SRS_FLAG_INTERLEAVED_PILOTS_V1; },
      +[](srs_invocation_fixture& call) { call.metadata.first_subcarrier[0] = 40; },
      +[](srs_invocation_fixture& call) { call.metadata.subcarrier_spacing_hz = 15000; },
      +[](srs_invocation_fixture& call) { call.input.srs.metadata_schema_id = OCUDU_DAPP_METADATA_PUSCH_V1; },
  };
  for (const auto corrupt : malformed) {
    srs_invocation_fixture call(6);
    corrupt(call);
    EXPECT_EQ(worker.worker->invoke(call.input, call.output).status, OCUDU_DAPP_UNSUPPORTED_V1);
  }
  EXPECT_EQ(reason_count(class_a_fallback_reason::unsupported_metadata), malformed.size());
  const auto invalid = {
      +[](srs_invocation_fixture& call) { call.input.rx_symbols.layout_id = 0; },
      +[](srs_invocation_fixture& call) { call.input.reference_symbols.dimensions[1] = 3; },
      +[](srs_invocation_fixture& call) { call.output.channel_matrix.dimensions[0] = 2; },
      +[](srs_invocation_fixture& call) { call.output.measurements.access = OCUDU_DAPP_ACCESS_READ_V1; },
      +[](srs_invocation_fixture& call) { call.output.measurements.domain = OCUDU_DAPP_MEM_ACCELERATOR_DEVICE_V1; },
  };
  for (const auto corrupt : invalid) {
    srs_invocation_fixture call(6);
    corrupt(call);
    EXPECT_EQ(worker.worker->invoke(call.input, call.output).status, OCUDU_DAPP_INVALID_INPUT_V1);
  }
  EXPECT_EQ(reason_count(class_a_fallback_reason::invalid_input), invalid.size());
  EXPECT_EQ(control.data_invocations.load(std::memory_order_relaxed), 1U);

  // A module that swaps the host-owned output storage produced an invalid output.
  control.replace_data_output.store(true, std::memory_order_relaxed);
  srs_invocation_fixture call(6);
  const auto             replaced = worker.worker->invoke(call.input, call.output);
  EXPECT_NE(replaced.status, OCUDU_DAPP_OK_V1);
  EXPECT_TRUE(replaced.invoke_fallback);
  worker.worker.reset();
  EXPECT_TRUE(created.instance->drain(std::chrono::steady_clock::now() + 100ms));
  EXPECT_TRUE(created.instance->unload());
}

TEST(dapp_native_instance, class_a_profile_rejects_unsupported_shape_before_module_callback)
{
  fixture_control control;
  auto            created = create_class_a_fixture(control);
  ASSERT_TRUE(created);
  advance_to_active(*created.instance);
  auto worker = created.instance->create_worker(OCUDU_DAPP_INTERFACE_CHANNEL_ESTIMATOR_V1, 15);
  ASSERT_TRUE(worker) << worker.detail;

  const std::array<complex32, 4> samples{{{1, 0}, {0, 1}, {1, 1}, {2, 0}}};
  auto                           input = make_channel_estimator_input(15, samples.data(), 2);
  input.invocation.cell_id             = 5;
  auto output                          = make_channel_estimator_output(input);
  auto result                          = worker.worker->invoke(input, output);
  EXPECT_EQ(result.status, OCUDU_DAPP_UNSUPPORTED_V1);
  EXPECT_TRUE(result.invoke_fallback);
  EXPECT_EQ(control.data_invocations.load(std::memory_order_relaxed), 0U);
  EXPECT_EQ(created.instance->health().input_cell_bitmap, 0U);
  EXPECT_EQ(created.instance->health()
                .class_a_fallbacks.reasons[static_cast<std::size_t>(class_a_fallback_reason::unsupported_layers)],
            1U);

  input                    = make_channel_estimator_input(15, samples.data(), 1);
  input.invocation.cell_id = 4;
  output                   = make_channel_estimator_output(input);
  EXPECT_TRUE(worker.worker->invoke(input, output));
  EXPECT_EQ(control.data_invocations.load(std::memory_order_relaxed), 1U);
  EXPECT_EQ(created.instance->health().input_cell_bitmap, uint32_t{1} << 4U);

  auto* pusch = const_cast<ocudu_dapp_pusch_metadata_v1*>(
      static_cast<const ocudu_dapp_pusch_metadata_v1*>(input.pusch.metadata.data));
  pusch->flags = OCUDU_DAPP_PUSCH_TRANSFORM_PRECODING_V1;
  EXPECT_EQ(worker.worker->invoke(input, output).status, OCUDU_DAPP_UNSUPPORTED_V1);
  EXPECT_EQ(created.instance->health().class_a_fallbacks.reasons[static_cast<std::size_t>(
                class_a_fallback_reason::unsupported_transform_precoding)],
            1U);
  pusch->flags = 0;

  auto* dmrs = const_cast<ocudu_dapp_dmrs_metadata_v1*>(
      static_cast<const ocudu_dapp_dmrs_metadata_v1*>(input.dmrs.metadata.data));
  dmrs->flags = 0;
  EXPECT_EQ(worker.worker->invoke(input, output).status, OCUDU_DAPP_UNSUPPORTED_V1);
  dmrs->flags = OCUDU_DAPP_DMRS_REFERENCE_SYMBOLS_LAYER_COMPLETE_V1 |
                OCUDU_DAPP_DMRS_COORDINATES_SHARED_ACROSS_LAYERS_V1 | (UINT32_C(1) << 31U);
  EXPECT_EQ(worker.worker->invoke(input, output).status, OCUDU_DAPP_UNSUPPORTED_V1);
  EXPECT_EQ(control.data_invocations.load(std::memory_order_relaxed), 1U);
  EXPECT_EQ(created.instance->health()
                .class_a_fallbacks.reasons[static_cast<std::size_t>(class_a_fallback_reason::unsupported_metadata)],
            2U);
}

TEST(dapp_native_instance, resident_backend_requires_a_matching_class_a_capability)
{
  fixture_control control;
  auto            created = native_instance::create(
      load_fixture(DAPP_CLASS_A_FIXTURE), host_api(&control), &control, OCUDU_DAPP_BACKEND_CUDA_V1);
  ASSERT_TRUE(created);
  EXPECT_EQ(created.instance->backend_id(), OCUDU_DAPP_BACKEND_CUDA_V1);
  advance_to_active(*created.instance);
  auto worker = created.instance->create_worker(OCUDU_DAPP_INTERFACE_CHANNEL_ESTIMATOR_V1, 17);
  EXPECT_FALSE(worker);
  EXPECT_EQ(worker.status, OCUDU_DAPP_UNSUPPORTED_V1);
}

TEST(dapp_native_instance, class_b_scheduler_may_use_cuda_internally_with_host_records)
{
  fixture_control control;
  auto            created = native_instance::create(
      load_fixture(DAPP_CLASS_B_FIXTURE), host_api(&control), &control, OCUDU_DAPP_BACKEND_CUDA_V1);
  ASSERT_TRUE(created);
  EXPECT_EQ(created.instance->backend_id(), OCUDU_DAPP_BACKEND_CUDA_V1);
  advance_to_active(*created.instance);

  auto worker = created.instance->create_worker(OCUDU_DAPP_INTERFACE_SCHEDULER_V1, 19);
  ASSERT_TRUE(worker) << worker.detail;
  EXPECT_EQ(worker.worker->backend_id(), OCUDU_DAPP_BACKEND_CUDA_V1);

  std::array<ocudu_dapp_scheduler_candidate_v1, 1> candidates{};
  candidates[0].struct_size           = sizeof(candidates[0]);
  candidates[0].candidate_id          = 0;
  candidates[0].conventional_priority = 2.0;
  std::array<ocudu_dapp_scheduler_intent_v1, 1> intents{};
  auto                                          input  = make_scheduler_input(19, candidates.data(), candidates.size());
  auto                                          output = make_scheduler_output(intents.data(), intents.size());
  EXPECT_TRUE(worker.worker->invoke(input, output));
  EXPECT_EQ(output.nof_intents, 1U);
  EXPECT_EQ(output.intents[0].candidate_id, 0U);
  EXPECT_EQ(output.intents[0].priority, 3.0);
  EXPECT_EQ(control.data_worker_creates.load(std::memory_order_relaxed), 1U);
  worker.worker.reset();
  EXPECT_TRUE(created.instance->drain(std::chrono::steady_clock::now() + 100ms));
  EXPECT_TRUE(created.instance->unload());
}

TEST(dapp_native_instance, class_a_module_unsupported_is_counted_without_opening_the_breaker)
{
  fixture_control control;
  auto            created = create_class_a_fixture(control);
  ASSERT_TRUE(created);
  advance_to_active(*created.instance);
  auto worker = created.instance->create_worker(OCUDU_DAPP_INTERFACE_CHANNEL_ESTIMATOR_V1, 18);
  ASSERT_TRUE(worker);
  const std::array<complex32, 4> samples{};
  auto                           input  = make_channel_estimator_input(18, samples.data());
  auto                           output = make_channel_estimator_output(input);

  control.data_status.store(OCUDU_DAPP_UNSUPPORTED_V1, std::memory_order_relaxed);
  for (unsigned count = 0; count != 4; ++count) {
    EXPECT_EQ(worker.worker->invoke(input, output).status, OCUDU_DAPP_UNSUPPORTED_V1);
  }
  const auto health = created.instance->health();
  EXPECT_NE(health.breaker, breaker_state::open);
  EXPECT_EQ(health.class_a_fallbacks.reasons[static_cast<std::size_t>(class_a_fallback_reason::module_unsupported)],
            4U);

  control.data_status.store(OCUDU_DAPP_OK_V1, std::memory_order_relaxed);
  EXPECT_TRUE(worker.worker->invoke(input, output));
  worker.worker.reset();
  EXPECT_TRUE(created.instance->drain(std::chrono::steady_clock::now() + 100ms));
  EXPECT_TRUE(created.instance->unload());
}

TEST(dapp_native_instance, async_completion_deadline_is_an_incident_but_not_a_current_call_fallback)
{
  fixture_control control;
  auto            created = create_class_a_fixture(control);
  ASSERT_TRUE(created);
  advance_to_active(*created.instance);
  auto worker = created.instance->create_worker(OCUDU_DAPP_INTERFACE_CHANNEL_ESTIMATOR_V1, 19);
  ASSERT_TRUE(worker);
  const std::array<complex32, 4> samples{};
  auto                           input = make_channel_estimator_input(19, samples.data());

  worker.worker->report_completion_deadline(input.invocation, 125'000, 100'000);
  const auto health = created.instance->health();
  EXPECT_EQ(health.class_a_completion.deadline_misses, 1U);
  EXPECT_EQ(health.class_a_fallbacks.reasons[static_cast<std::size_t>(class_a_fallback_reason::deadline)], 0U);
  incident_record incident;
  ASSERT_TRUE(created.instance->try_pop_incident(incident));
  EXPECT_EQ(incident.context.phase, incident_phase::completion);
  EXPECT_EQ(incident.cause, incident_cause::deadline_miss);
  EXPECT_EQ(incident.action, incident_action::observed);
  EXPECT_EQ(incident.observed_latency_ns, 125'000U);
  EXPECT_EQ(incident.deadline_ns, 100'000U);
  EXPECT_FALSE(incident.fallback_succeeded);

  worker.worker.reset();
  EXPECT_TRUE(created.instance->drain(std::chrono::steady_clock::now() + 100ms));
  EXPECT_TRUE(created.instance->unload());
}

TEST(dapp_native_instance, class_b_scheduler_uses_caller_owned_bounded_intents)
{
  fixture_control control;
  auto            created = create_class_b_fixture(control);
  ASSERT_TRUE(created);
  advance_to_active(*created.instance);
  auto worker = created.instance->create_worker(OCUDU_DAPP_INTERFACE_SCHEDULER_V1, 12);
  ASSERT_TRUE(worker) << worker.detail;

  std::array<ocudu_dapp_scheduler_candidate_v1, 2> candidates{};
  for (uint32_t index = 0; index != candidates.size(); ++index) {
    // A v1-base producer remains valid after reserved-tail feature promotion.
    candidates[index].struct_size           = OCUDU_DAPP_SCHEDULER_CANDIDATE_BASE_SIZE_V1;
    candidates[index].candidate_id          = index;
    candidates[index].conventional_priority = static_cast<double>(index);
  }
  std::array<ocudu_dapp_scheduler_intent_v1, 2> intents{};
  auto                                          input = make_scheduler_input(12, candidates.data(), candidates.size());
  input.invocation.cell_id                            = 2;
  auto output                                         = make_scheduler_output(intents.data(), intents.size());
  auto result                                         = worker.worker->invoke(input, output);
  ASSERT_TRUE(result);
  ASSERT_EQ(output.nof_intents, candidates.size());
  EXPECT_EQ(output.decision_generation, input.candidate_generation);
  EXPECT_EQ(output.intents, intents.data());
  EXPECT_DOUBLE_EQ(output.intents[0].priority, 1.0);
  EXPECT_DOUBLE_EQ(output.intents[1].priority, 2.0);
  EXPECT_EQ(created.instance->health().input_cell_bitmap, uint32_t{1} << 2U);

  control.replace_data_output.store(true, std::memory_order_relaxed);
  output = make_scheduler_output(intents.data(), intents.size());
  result = worker.worker->invoke(input, output);
  EXPECT_EQ(result.status, OCUDU_DAPP_INVALID_OUTPUT_V1);
  EXPECT_TRUE(result.invoke_fallback);

  input.allowed_output_flags = 1U << 31U;
  input.invocation.cell_id   = 5;
  output                     = make_scheduler_output(intents.data(), intents.size());
  result                     = worker.worker->invoke(input, output);
  EXPECT_EQ(result.status, OCUDU_DAPP_INVALID_INPUT_V1);
  EXPECT_TRUE(result.invoke_fallback);
  EXPECT_EQ(created.instance->health().input_cell_bitmap, uint32_t{1} << 2U);
  worker.worker.reset();
  EXPECT_TRUE(created.instance->drain(std::chrono::steady_clock::now() + 100ms));
  EXPECT_TRUE(created.instance->unload());
}

TEST(dapp_native_instance, class_b_scheduler_exception_is_contained_and_opens_only_its_worker)
{
  fixture_control control;
  auto            created = create_class_b_fixture(control);
  ASSERT_TRUE(created);
  advance_to_active(*created.instance);
  auto worker = created.instance->create_worker(OCUDU_DAPP_INTERFACE_SCHEDULER_V1, 14);
  ASSERT_TRUE(worker) << worker.detail;

  std::array<ocudu_dapp_scheduler_candidate_v1, 1> candidates{};
  candidates[0].struct_size  = sizeof(candidates[0]);
  candidates[0].candidate_id = 0;
  std::array<ocudu_dapp_scheduler_intent_v1, 1> intents{};
  auto                                          input  = make_scheduler_input(14, candidates.data(), candidates.size());
  auto                                          output = make_scheduler_output(intents.data(), intents.size());
  control.throw_data_invocation.store(true, std::memory_order_relaxed);
  auto result = worker.worker->invoke(input, output);
  EXPECT_EQ(result.status, OCUDU_DAPP_EXCEPTION_V1);
  EXPECT_TRUE(result.invoke_fallback);
  control.throw_data_invocation.store(false, std::memory_order_relaxed);
  output = make_scheduler_output(intents.data(), intents.size());
  EXPECT_EQ(worker.worker->invoke(input, output).status, OCUDU_DAPP_BYPASS_V1);

  incident_record incident;
  ASSERT_TRUE(created.instance->try_pop_incident(incident));
  EXPECT_EQ(incident.cause, incident_cause::standard_exception);
  EXPECT_EQ(incident.context.hook_id, OCUDU_DAPP_INTERFACE_SCHEDULER_V1);
  EXPECT_EQ(incident.context.worker_id, 14U);
  worker.worker.reset();
  EXPECT_TRUE(created.instance->drain(std::chrono::steady_clock::now() + 100ms));
  EXPECT_TRUE(created.instance->unload());
}

TEST(dapp_native_instance, class_a_exception_opens_only_the_faulting_worker_and_records_context)
{
  fixture_control control;
  auto            created = create_class_a_fixture(control);
  ASSERT_TRUE(created);
  advance_to_active(*created.instance);
  auto first  = created.instance->create_worker(OCUDU_DAPP_INTERFACE_CHANNEL_ESTIMATOR_V1, 1);
  auto second = created.instance->create_worker(OCUDU_DAPP_INTERFACE_CHANNEL_ESTIMATOR_V1, 2);
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  const std::array<complex32, 4> samples{};
  auto                           input_first  = make_channel_estimator_input(1, samples.data());
  auto                           input_second = make_channel_estimator_input(2, samples.data());
  auto                           output       = make_channel_estimator_output(input_first);

  control.throw_data_invocation.store(true, std::memory_order_relaxed);
  auto failed = first.worker->invoke(input_first, output);
  EXPECT_EQ(failed.status, OCUDU_DAPP_EXCEPTION_V1);
  EXPECT_TRUE(failed.invoke_fallback);
  control.throw_data_invocation.store(false, std::memory_order_relaxed);
  EXPECT_TRUE(second.worker->invoke(input_second, output));
  EXPECT_EQ(first.worker->invoke(input_first, output).status, OCUDU_DAPP_BYPASS_V1);
  EXPECT_EQ(control.data_invocations.load(std::memory_order_relaxed), 2U);
  const auto health = created.instance->health();
  EXPECT_EQ(health.class_a_fallbacks.reasons[static_cast<std::size_t>(class_a_fallback_reason::exception)], 1U);
  EXPECT_EQ(health.class_a_fallbacks.reasons[static_cast<std::size_t>(class_a_fallback_reason::breaker_open)], 1U);

  incident_record incident;
  ASSERT_TRUE(created.instance->try_pop_incident(incident));
  EXPECT_EQ(incident.cause, incident_cause::standard_exception);
  EXPECT_EQ(incident.context.hook_id, OCUDU_DAPP_INTERFACE_CHANNEL_ESTIMATOR_V1);
  EXPECT_EQ(incident.context.worker_id, 1U);
  first.worker.reset();
  second.worker.reset();
  EXPECT_TRUE(created.instance->drain(std::chrono::steady_clock::now() + 100ms));
  EXPECT_TRUE(created.instance->unload());
}

TEST(dapp_native_instance, reset_breakers_closes_leftover_worker_latches)
{
  fixture_control control;
  auto            created = create_class_a_fixture(control);
  ASSERT_TRUE(created);
  advance_to_active(*created.instance);
  auto first  = created.instance->create_worker(OCUDU_DAPP_INTERFACE_CHANNEL_ESTIMATOR_V1, 1);
  auto second = created.instance->create_worker(OCUDU_DAPP_INTERFACE_CHANNEL_ESTIMATOR_V1, 2);
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  const std::array<complex32, 4> samples{};
  auto                           input_first = make_channel_estimator_input(1, samples.data());
  auto                           output      = make_channel_estimator_output(input_first);

  control.throw_data_invocation.store(true, std::memory_order_relaxed);
  EXPECT_EQ(first.worker->invoke(input_first, output).status, OCUDU_DAPP_EXCEPTION_V1);
  control.throw_data_invocation.store(false, std::memory_order_relaxed);
  EXPECT_EQ(created.instance->health().breaker, breaker_state::open);

  EXPECT_TRUE(created.instance->reset_breakers());
  const auto health = created.instance->health();
  EXPECT_EQ(health.breaker, breaker_state::closed);
  EXPECT_EQ(health.consecutive_failures, 0U);
  EXPECT_TRUE(first.worker->invoke(input_first, output));

  first.worker.reset();
  second.worker.reset();
  EXPECT_TRUE(created.instance->drain(std::chrono::steady_clock::now() + 100ms));
  EXPECT_TRUE(created.instance->unload());
}

TEST(dapp_native_instance, class_a_invalid_output_is_rejected_before_the_phy_can_consume_it)
{
  fixture_control control;
  auto            created = create_class_a_fixture(control);
  ASSERT_TRUE(created);
  advance_to_active(*created.instance);
  auto worker = created.instance->create_worker(OCUDU_DAPP_INTERFACE_CHANNEL_ESTIMATOR_V1, 5);
  ASSERT_TRUE(worker);
  const std::array<complex32, 4> samples{};
  auto                           input  = make_channel_estimator_input(5, samples.data());
  auto                           output = make_channel_estimator_output(input);
  control.invalid_data_output.store(true, std::memory_order_relaxed);
  auto invoked = worker.worker->invoke(input, output);
  EXPECT_EQ(invoked.status, OCUDU_DAPP_INVALID_OUTPUT_V1);
  EXPECT_TRUE(invoked.invoke_fallback);
  EXPECT_EQ(created.instance->health()
                .class_a_fallbacks.reasons[static_cast<std::size_t>(class_a_fallback_reason::invalid_output)],
            1U);
  incident_record incident;
  ASSERT_TRUE(created.instance->try_pop_incident(incident));
  EXPECT_EQ(incident.cause, incident_cause::invalid_output);
  worker.worker.reset();
  EXPECT_TRUE(created.instance->drain(std::chrono::steady_clock::now() + 100ms));
  EXPECT_TRUE(created.instance->unload());
}

TEST(dapp_native_instance, class_a_read_only_output_is_rejected_before_the_phy_can_consume_it)
{
  fixture_control control;
  auto            created = create_class_a_fixture(control);
  ASSERT_TRUE(created);
  advance_to_active(*created.instance);
  auto worker = created.instance->create_worker(OCUDU_DAPP_INTERFACE_CHANNEL_ESTIMATOR_V1, 5);
  ASSERT_TRUE(worker);
  const std::array<complex32, 4> samples{};
  auto                           input  = make_channel_estimator_input(5, samples.data());
  auto                           output = make_channel_estimator_output(input);
  output.channel_estimates.access       = OCUDU_DAPP_ACCESS_READ_V1 | OCUDU_DAPP_ACCESS_HOST_VISIBLE_V1;
  auto invoked                          = worker.worker->invoke(input, output);
  EXPECT_EQ(invoked.status, OCUDU_DAPP_INVALID_OUTPUT_V1);
  EXPECT_TRUE(invoked.invoke_fallback);
  incident_record incident;
  ASSERT_TRUE(created.instance->try_pop_incident(incident));
  EXPECT_EQ(incident.cause, incident_cause::invalid_output);
  worker.worker.reset();
  EXPECT_TRUE(created.instance->drain(std::chrono::steady_clock::now() + 100ms));
  EXPECT_TRUE(created.instance->unload());
}

TEST(dapp_native_instance, class_a_cannot_replace_caller_owned_output_storage)
{
  fixture_control control;
  auto            created = create_class_a_fixture(control);
  ASSERT_TRUE(created);
  advance_to_active(*created.instance);
  auto worker = created.instance->create_worker(OCUDU_DAPP_INTERFACE_CHANNEL_ESTIMATOR_V1, 5);
  ASSERT_TRUE(worker);
  const std::array<complex32, 4> samples{};
  auto                           input  = make_channel_estimator_input(5, samples.data());
  auto                           output = make_channel_estimator_output(input);
  control.replace_data_output.store(true, std::memory_order_relaxed);
  auto invoked = worker.worker->invoke(input, output);
  EXPECT_EQ(invoked.status, OCUDU_DAPP_INVALID_OUTPUT_V1);
  EXPECT_TRUE(invoked.invoke_fallback);
  incident_record incident;
  ASSERT_TRUE(created.instance->try_pop_incident(incident));
  EXPECT_EQ(incident.cause, incident_cause::invalid_output);
  worker.worker.reset();
  EXPECT_TRUE(created.instance->drain(std::chrono::steady_clock::now() + 100ms));
  EXPECT_TRUE(created.instance->unload());
}

TEST(dapp_native_instance, class_a_deadline_is_checked_before_output_is_committed)
{
  fixture_control control;
  auto            created = create_class_a_fixture(control);
  ASSERT_TRUE(created);
  advance_to_active(*created.instance);
  auto worker = created.instance->create_worker(OCUDU_DAPP_INTERFACE_CHANNEL_ESTIMATOR_V1, 6);
  ASSERT_TRUE(worker);
  const std::array<complex32, 4> samples{};
  auto                           input   = make_channel_estimator_input(6, samples.data());
  input.invocation.deadline_monotonic_ns = 100;
  control.monotonic_ns.store(101, std::memory_order_relaxed);
  auto output  = make_channel_estimator_output(input);
  auto invoked = worker.worker->invoke(input, output);
  EXPECT_EQ(invoked.status, OCUDU_DAPP_DEADLINE_V1);
  EXPECT_TRUE(invoked.invoke_fallback);
  incident_record incident;
  ASSERT_TRUE(created.instance->try_pop_incident(incident));
  EXPECT_EQ(incident.cause, incident_cause::deadline_miss);
  worker.worker.reset();
  EXPECT_TRUE(created.instance->drain(std::chrono::steady_clock::now() + 100ms));
  EXPECT_TRUE(created.instance->unload());
}

TEST(dapp_native_instance, in_process_class_c_uses_a_prebound_worker_callback)
{
  fixture_control control;
  auto            created = create_class_c_fixture(control);
  ASSERT_TRUE(created);
  advance_to_active(*created.instance);
  auto worker = created.instance->create_worker(OCUDU_DAPP_INTERFACE_SPECTRUM_V1, 8);
  ASSERT_TRUE(worker) << worker.detail;
  const std::array<complex32, 4> samples{};
  ocudu_dapp_spectrum_input_v1   input{};
  input.invocation.struct_size               = sizeof(input.invocation);
  input.invocation.abi_major                 = OCUDU_DAPP_ABI_MAJOR_V1;
  input.invocation.abi_minor                 = OCUDU_DAPP_ABI_MINOR_V1;
  input.invocation.invocation_id             = 108;
  input.invocation.cell_id                   = 6;
  input.invocation.worker_id                 = 8;
  input.invocation.input_generation          = 108;
  input.uplink_resource_grid                 = make_tensor(samples.data());
  input.uplink_resource_grid.rank            = 3;
  input.uplink_resource_grid.dimensions[0]   = 1;
  input.uplink_resource_grid.dimensions[1]   = 2;
  input.uplink_resource_grid.dimensions[2]   = 2;
  input.uplink_resource_grid.byte_strides[0] = 4 * sizeof(complex32);
  input.uplink_resource_grid.byte_strides[1] = 2 * sizeof(complex32);
  input.uplink_resource_grid.byte_strides[2] = sizeof(complex32);
  ocudu_dapp_spectrum_slot_metadata_v1 metadata{};
  metadata.struct_size                        = sizeof(metadata);
  metadata.abi_major                          = OCUDU_DAPP_ABI_MAJOR_V1;
  metadata.bandwidth_hz                       = 30'000;
  metadata.rx_ports                           = 1;
  metadata.symbols                            = 2;
  metadata.subcarriers                        = 2;
  metadata.amplitude_scale                    = 1.0F;
  input.slot_metadata.struct_size             = sizeof(input.slot_metadata);
  input.slot_metadata.abi_major               = OCUDU_DAPP_ABI_MAJOR_V1;
  input.slot_metadata.metadata_schema_id      = OCUDU_DAPP_METADATA_SPECTRUM_SLOT_V1;
  input.slot_metadata.metadata_schema_version = 1;
  input.slot_metadata.metadata                = {&metadata, sizeof(metadata), 0};
  input.source_sequence                       = input.invocation.input_generation;
  EXPECT_TRUE(worker.worker->publish(input));
  EXPECT_EQ(control.data_invocations.load(std::memory_order_relaxed), 1U);
  EXPECT_EQ(created.instance->health().input_cell_bitmap, uint32_t{1} << 6U);
  worker.worker.reset();
  EXPECT_TRUE(created.instance->drain(std::chrono::steady_clock::now() + 100ms));
  EXPECT_TRUE(created.instance->unload());
}

TEST(dapp_native_instance, class_c_advance_plan_is_validated_and_exceptions_open_only_the_worker)
{
  fixture_control control;
  auto            created = create_class_c_fixture(control);
  ASSERT_TRUE(created);
  advance_to_active(*created.instance);
  auto worker = created.instance->create_worker(OCUDU_DAPP_INTERFACE_SPECTRUM_V1, 8);
  ASSERT_TRUE(worker) << worker.detail;

  ocudu_dapp_ul_resource_span_v1 span{};
  span.struct_size   = sizeof(span);
  span.symbol_start  = 2;
  span.symbol_count  = 4;
  span.first_prb     = 10;
  span.nof_prbs      = 8;
  span.use_category  = OCUDU_DAPP_UL_USE_PUSCH_V1;
  span.allocation_id = 1;
  ocudu_dapp_ul_resource_plan_v1 plan{};
  plan.struct_size                     = sizeof(plan);
  plan.abi_major                       = OCUDU_DAPP_ABI_MAJOR_V1;
  plan.abi_minor                       = OCUDU_DAPP_ABI_MINOR_V1;
  plan.context.struct_size             = sizeof(plan.context);
  plan.context.schema_id               = OCUDU_DAPP_CONTEXT_UL_RESOURCE_PLAN_V1;
  plan.context.schema_version          = 1;
  plan.context.producer_instance_id[0] = 1;
  plan.context.producer_generation     = 1;
  plan.context.source_sequence         = 9;
  plan.context.cell_id                 = 9;
  plan.context.confidence_q16          = 1U << 16U;
  plan.planned_time                    = {10, 4, 1, 0};
  plan.context.created                 = plan.planned_time;
  plan.context.valid_from              = plan.planned_time;
  plan.context.valid_until             = plan.planned_time;
  plan.plan_generation                 = 9;
  plan.spans                           = &span;
  plan.nof_spans                       = 1;
  plan.flags                           = OCUDU_DAPP_UL_RESOURCE_PLAN_COMPLETE_V1;
  plan.cell_nof_prbs                   = 52;

  EXPECT_TRUE(worker.worker->publish(plan));
  EXPECT_EQ(control.data_invocations.load(std::memory_order_relaxed), 1U);
  EXPECT_EQ(created.instance->health().input_cell_bitmap, 0U);
  plan.cell_nof_prbs = 0;
  EXPECT_EQ(worker.worker->publish(plan).status, OCUDU_DAPP_INVALID_INPUT_V1);
  plan.cell_nof_prbs = 52;
  control.throw_data_invocation.store(true, std::memory_order_relaxed);
  EXPECT_EQ(worker.worker->publish(plan).status, OCUDU_DAPP_EXCEPTION_V1);
  control.throw_data_invocation.store(false, std::memory_order_relaxed);
  EXPECT_EQ(worker.worker->publish(plan).status, OCUDU_DAPP_BYPASS_V1);

  incident_record incident;
  ASSERT_TRUE(created.instance->try_pop_incident(incident));
  EXPECT_EQ(incident.cause, incident_cause::standard_exception);
  EXPECT_EQ(incident.context.hook_id, OCUDU_DAPP_INTERFACE_SPECTRUM_V1);
  EXPECT_EQ(incident.context.worker_id, 8U);
  worker.worker.reset();
  EXPECT_TRUE(created.instance->drain(std::chrono::steady_clock::now() + 100ms));
  EXPECT_TRUE(created.instance->unload());
}

TEST(dapp_native_instance, in_process_srs_records_only_valid_callback_input_cells)
{
  fixture_control control;
  auto            created = create_class_c_srs_fixture(control);
  ASSERT_TRUE(created);
  advance_to_active(*created.instance);
  auto worker = created.instance->create_worker(OCUDU_DAPP_INTERFACE_SRS_ISAC_V1, 9);
  ASSERT_TRUE(worker) << worker.detail;

  const std::array<complex32, 4> response{};
  auto                           tensor = make_tensor(response.data());
  tensor.rank                           = 3;
  tensor.dimensions[0]                  = 1;
  tensor.dimensions[1]                  = 1;
  tensor.dimensions[2]                  = response.size();
  tensor.byte_strides[0]                = sizeof(response);
  tensor.byte_strides[1]                = sizeof(response);
  tensor.byte_strides[2]                = sizeof(complex32);
  ocudu_dapp_srs_isac_metadata_v1 metadata{};
  metadata.struct_size           = sizeof(metadata);
  metadata.abi_major             = OCUDU_DAPP_ABI_MAJOR_V1;
  metadata.srs_resource_id       = 17;
  metadata.srs_config_id         = 19;
  metadata.rx_ports              = 1;
  metadata.tx_ports              = 1;
  metadata.comb_size             = 2;
  metadata.subcarrier_spacing_hz = 30'000;
  metadata.frequency_points      = response.size();
  ocudu_dapp_srs_isac_input_v1 input{};
  input.invocation.struct_size               = sizeof(input.invocation);
  input.invocation.abi_major                 = OCUDU_DAPP_ABI_MAJOR_V1;
  input.invocation.abi_minor                 = OCUDU_DAPP_ABI_MINOR_V1;
  input.invocation.invocation_id             = 109;
  input.invocation.cell_id                   = 11;
  input.invocation.worker_id                 = 9;
  input.invocation.input_generation          = 109;
  input.channel_estimates                    = tensor;
  input.srs_metadata.struct_size             = sizeof(input.srs_metadata);
  input.srs_metadata.abi_major               = OCUDU_DAPP_ABI_MAJOR_V1;
  input.srs_metadata.metadata_schema_id      = OCUDU_DAPP_METADATA_SRS_ISAC_V1;
  input.srs_metadata.metadata_schema_version = 1;
  input.srs_metadata.metadata                = {&metadata, sizeof(metadata), 0};
  input.source_sequence                      = 109;

  EXPECT_TRUE(worker.worker->publish(input));
  EXPECT_EQ(created.instance->health().input_cell_bitmap, uint32_t{1} << 11U);
  input.invocation.cell_id              = 12;
  input.srs_metadata.metadata_schema_id = OCUDU_DAPP_METADATA_SPECTRUM_SLOT_V1;
  EXPECT_EQ(worker.worker->publish(input).status, OCUDU_DAPP_INVALID_INPUT_V1);
  EXPECT_EQ(created.instance->health().input_cell_bitmap, uint32_t{1} << 11U);

  worker.worker.reset();
  EXPECT_TRUE(created.instance->drain(std::chrono::steady_clock::now() + 100ms));
  EXPECT_TRUE(created.instance->unload());
}

TEST(dapp_native_hook_slot, unpublishes_before_waiting_for_an_active_reader)
{
  fixture_control control;
  auto            created = create_class_a_fixture(control);
  ASSERT_TRUE(created);
  advance_to_active(*created.instance);
  auto worker = created.instance->create_worker(OCUDU_DAPP_INTERFACE_CHANNEL_ESTIMATOR_V1, 9);
  ASSERT_TRUE(worker);
  native_hook_slot slot(OCUDU_DAPP_INTERFACE_CHANNEL_ESTIMATOR_V1);
  auto             gate = std::make_shared<native_activation_gate>();
  ASSERT_EQ(slot.bind(worker.worker, gate), OCUDU_DAPP_OK_V1);
  gate->enabled.store(true, std::memory_order_release);
  worker.worker.reset();

  const std::array<complex32, 4> samples{};
  auto                           input  = make_channel_estimator_input(9, samples.data());
  auto                           output = make_channel_estimator_output(input);
  native_invocation_result       invoked;
  control.block_data_invocations.store(true, std::memory_order_release);
  std::thread invocation_thread([&] { invoked = slot.invoke(input, output); });
  const auto  enter_deadline = std::chrono::steady_clock::now() + 500ms;
  while (control.active_data_invocations.load(std::memory_order_acquire) == 0 &&
         std::chrono::steady_clock::now() < enter_deadline) {
    std::this_thread::yield();
  }
  ASSERT_EQ(control.active_data_invocations.load(std::memory_order_acquire), 1U);

  EXPECT_EQ(slot.clear(std::chrono::steady_clock::now()), OCUDU_DAPP_DEADLINE_V1);
  EXPECT_FALSE(slot.bound());
  auto bypass_output = make_channel_estimator_output(input);
  EXPECT_EQ(slot.invoke(input, bypass_output).status, OCUDU_DAPP_BYPASS_V1);
  control.block_data_invocations.store(false, std::memory_order_release);
  invocation_thread.join();
  EXPECT_TRUE(invoked);
  EXPECT_EQ(slot.clear(std::chrono::steady_clock::now() + 100ms), OCUDU_DAPP_OK_V1);
  EXPECT_EQ(control.data_worker_destroys.load(std::memory_order_relaxed), 1U);
  EXPECT_TRUE(created.instance->drain(std::chrono::steady_clock::now() + 100ms));
  EXPECT_TRUE(created.instance->unload());
}

TEST(dapp_native_hook_slot, rejects_a_worker_for_a_different_interface)
{
  fixture_control control;
  auto            created = create_class_a_fixture(control);
  ASSERT_TRUE(created);
  advance_to_active(*created.instance);
  auto worker = created.instance->create_worker(OCUDU_DAPP_INTERFACE_CHANNEL_ESTIMATOR_V1, 10);
  ASSERT_TRUE(worker);
  native_hook_slot spectrum_slot(OCUDU_DAPP_INTERFACE_SPECTRUM_V1);
  EXPECT_EQ(spectrum_slot.bind(worker.worker, std::make_shared<native_activation_gate>()), OCUDU_DAPP_INVALID_INPUT_V1);
  worker.worker.reset();
  EXPECT_TRUE(created.instance->drain(std::chrono::steady_clock::now() + 100ms));
  EXPECT_TRUE(created.instance->unload());
}

TEST(dapp_native_hook_slot, retains_worker_until_accelerator_completion_barrier_finishes)
{
  fixture_control control;
  auto            created = create_class_a_fixture(control);
  ASSERT_TRUE(created);
  advance_to_active(*created.instance);
  auto worker = created.instance->create_worker(OCUDU_DAPP_INTERFACE_CHANNEL_ESTIMATOR_V1, 11);
  ASSERT_TRUE(worker);

  native_hook_slot slot(OCUDU_DAPP_INTERFACE_CHANNEL_ESTIMATOR_V1);
  auto             gate    = std::make_shared<native_activation_gate>();
  auto             barrier = std::make_shared<controllable_completion_barrier>();
  ASSERT_TRUE(slot.set_completion_barrier(barrier));
  ASSERT_EQ(slot.bind(worker.worker, gate), OCUDU_DAPP_OK_V1);
  gate->enabled.store(true, std::memory_order_release);
  worker.worker.reset();

  EXPECT_EQ(slot.clear(std::chrono::steady_clock::now()), OCUDU_DAPP_DEADLINE_V1);
  EXPECT_FALSE(slot.bound());
  EXPECT_EQ(control.data_worker_destroys.load(std::memory_order_relaxed), 0U);
  EXPECT_EQ(barrier->waits.load(std::memory_order_relaxed), 1U);

  barrier->complete.store(true, std::memory_order_release);
  EXPECT_EQ(slot.clear(std::chrono::steady_clock::now() + 100ms), OCUDU_DAPP_OK_V1);
  EXPECT_EQ(control.data_worker_destroys.load(std::memory_order_relaxed), 1U);
  EXPECT_EQ(barrier->waits.load(std::memory_order_relaxed), 2U);
  EXPECT_TRUE(created.instance->drain(std::chrono::steady_clock::now() + 100ms));
  EXPECT_TRUE(created.instance->unload());
}

TEST(dapp_native_hook_point, validates_and_activates_the_complete_phy_lane_set)
{
  EXPECT_FALSE(native_hook_point::create(OCUDU_DAPP_INTERFACE_CHANNEL_ESTIMATOR_V1, 0, 0));
  EXPECT_FALSE(native_hook_point::create(OCUDU_DAPP_INTERFACE_CHANNEL_ESTIMATOR_V1, 65535, 2));

  fixture_control control;
  auto            created = create_class_a_fixture(control);
  ASSERT_TRUE(created);
  advance_to_active(*created.instance);
  auto point = native_hook_point::create(OCUDU_DAPP_INTERFACE_CHANNEL_ESTIMATOR_V1, 100, 3);
  ASSERT_TRUE(point);

  const std::array<complex32, 4> samples{};
  for (uint32_t lane = 0; lane != point.point->lane_count(); ++lane) {
    auto input  = make_channel_estimator_input(point.point->worker_id(lane), samples.data());
    auto output = make_channel_estimator_output(input);
    EXPECT_EQ(point.point->invoke(lane, input, output).status, OCUDU_DAPP_BYPASS_V1);
  }

  ASSERT_EQ(point.point->activate(*created.instance), OCUDU_DAPP_OK_V1);
  EXPECT_TRUE(point.point->active());
  EXPECT_EQ(control.data_worker_creates.load(std::memory_order_relaxed), 3U);
  for (uint32_t lane = 0; lane != point.point->lane_count(); ++lane) {
    auto input  = make_channel_estimator_input(point.point->worker_id(lane), samples.data());
    auto output = make_channel_estimator_output(input);
    EXPECT_TRUE(point.point->invoke(lane, input, output));
  }
  auto failed_input = make_channel_estimator_input(point.point->worker_id(1), samples.data());
  point.point->report_failure(1, failed_input.invocation, OCUDU_DAPP_INVALID_OUTPUT_V1);
  incident_record incident;
  ASSERT_TRUE(created.instance->try_pop_incident(incident));
  EXPECT_EQ(incident.cause, incident_cause::invalid_output);
  EXPECT_EQ(incident.context.worker_id, point.point->worker_id(1));
  EXPECT_EQ(point.point->activate(*created.instance), OCUDU_DAPP_CONFLICT_V1);
  EXPECT_EQ(point.point->deactivate(std::chrono::steady_clock::now() + 100ms), OCUDU_DAPP_OK_V1);
  EXPECT_FALSE(point.point->active());
  EXPECT_EQ(control.data_worker_destroys.load(std::memory_order_relaxed), 3U);
  EXPECT_TRUE(created.instance->drain(std::chrono::steady_clock::now() + 100ms));
  EXPECT_TRUE(created.instance->unload());
}

TEST(dapp_native_hook_point, disables_all_lanes_before_a_bounded_grace_period)
{
  fixture_control control;
  auto            created = create_class_a_fixture(control);
  ASSERT_TRUE(created);
  advance_to_active(*created.instance);
  auto point = native_hook_point::create(OCUDU_DAPP_INTERFACE_CHANNEL_ESTIMATOR_V1, 200, 2);
  ASSERT_TRUE(point);
  ASSERT_EQ(point.point->activate(*created.instance), OCUDU_DAPP_OK_V1);

  const std::array<complex32, 4> samples{};
  auto                           input0  = make_channel_estimator_input(200, samples.data());
  auto                           output0 = make_channel_estimator_output(input0);
  native_invocation_result       invoked;
  control.block_data_invocations.store(true, std::memory_order_release);
  std::thread invocation_thread([&] { invoked = point.point->invoke(0, input0, output0); });
  const auto  enter_deadline = std::chrono::steady_clock::now() + 500ms;
  while (control.active_data_invocations.load(std::memory_order_acquire) == 0 &&
         std::chrono::steady_clock::now() < enter_deadline) {
    std::this_thread::yield();
  }
  ASSERT_EQ(control.active_data_invocations.load(std::memory_order_acquire), 1U);

  EXPECT_EQ(point.point->deactivate(std::chrono::steady_clock::now()), OCUDU_DAPP_DEADLINE_V1);
  EXPECT_FALSE(point.point->active());
  auto input1  = make_channel_estimator_input(201, samples.data());
  auto output1 = make_channel_estimator_output(input1);
  EXPECT_EQ(point.point->invoke(1, input1, output1).status, OCUDU_DAPP_BYPASS_V1);
  EXPECT_EQ(point.point->activate(*created.instance), OCUDU_DAPP_CONFLICT_V1);

  control.block_data_invocations.store(false, std::memory_order_release);
  invocation_thread.join();
  EXPECT_TRUE(invoked);
  EXPECT_EQ(point.point->deactivate(std::chrono::steady_clock::now() + 100ms), OCUDU_DAPP_OK_V1);
  EXPECT_EQ(control.data_worker_destroys.load(std::memory_order_relaxed), 2U);
  EXPECT_TRUE(created.instance->drain(std::chrono::steady_clock::now() + 100ms));
  EXPECT_TRUE(created.instance->unload());
}
