// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "fixtures/fixture_common.h"
#include "ocudu/dapp/runtime/instance_manager.h"
#include "ocudu/dapp/runtime/native_hook_registry.h"
#include "ocudu/dapp/runtime/native_instance.h"
#include <array>
#include <cstring>
#include <gtest/gtest.h>
#include <thread>

using namespace ocudu::dapp;
using namespace ocudu::dapp::management;
using namespace std::chrono_literals;

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

std::shared_ptr<native_module_library> load_multi_library()
{
  artifact_verifier verifier(DAPP_TEST_CATALOG);
  auto              inspected = verifier.inspect(DAPP_MULTI_FIXTURE);
  if (!inspected) {
    return {};
  }
  auto approved = verifier.verify(DAPP_MULTI_FIXTURE, inspected.digest);
  if (!approved) {
    return {};
  }
  auto loaded = load_native_module(std::move(*approved.artifact), host_info());
  return loaded.library;
}

instance_create_result create_multi(fixture_control& control)
{
  return native_instance::create(load_multi_library(), host_api(), &control);
}

interface_record interface(uint32_t id, ocudu_dapp_class_v1 dapp_class)
{
  return {id, 1, 0, dapp_class, OCUDU_DAPP_PLACEMENT_IN_PROCESS_V1, OCUDU_DAPP_THREADING_PER_WORKER_V1, 0};
}

interface_record optional_interface(uint32_t id, ocudu_dapp_class_v1 dapp_class)
{
  auto result        = interface(id, dapp_class);
  result.requirement = binding_requirement::optional;
  return result;
}

struct complex32 {
  float real;
  float imag;
};

ocudu_dapp_tensor_view_v1 tensor(const complex32* data)
{
  ocudu_dapp_tensor_view_v1 result{};
  result.struct_size           = sizeof(result);
  result.abi_major             = OCUDU_DAPP_ABI_MAJOR_V1;
  result.domain                = OCUDU_DAPP_MEM_HOST_V1;
  result.element_type          = OCUDU_DAPP_ELEMENT_CF32_V1;
  result.rank                  = 1;
  result.access                = OCUDU_DAPP_ACCESS_READ_V1 | OCUDU_DAPP_ACCESS_HOST_VISIBLE_V1;
  result.allocation_id         = 1;
  result.allocation_generation = 1;
  result.native_handle         = reinterpret_cast<uintptr_t>(data);
  result.byte_length           = 4 * sizeof(complex32);
  result.dimensions[0]         = 4;
  result.byte_strides[0]       = sizeof(complex32);
  result.lease_token           = 1;
  return result;
}

ocudu_dapp_invocation_v1 invocation(uint32_t worker_id)
{
  ocudu_dapp_invocation_v1 result{};
  result.struct_size      = sizeof(result);
  result.abi_major        = OCUDU_DAPP_ABI_MAJOR_V1;
  result.invocation_id    = worker_id + 1;
  result.worker_id        = worker_id;
  result.input_generation = worker_id + 1;
  return result;
}

void advance_to_shadow(native_instance& instance)
{
  ASSERT_TRUE(instance.prepare(nullptr, nullptr));
  ASSERT_TRUE(instance.arm(nullptr));
  ASSERT_TRUE(instance.enter_shadow());
}

package_record multi_package(bool optional_spectrum = false)
{
  artifact_verifier verifier(DAPP_TEST_CATALOG);
  auto              inspected = verifier.inspect(DAPP_MULTI_FIXTURE);
  EXPECT_TRUE(inspected) << inspected.detail;
  package_record package;
  package.package_id              = id(1);
  package.global_name             = "org.deepsig.ocudu.test.multi";
  package.vendor                  = "DeepSig";
  package.version                 = "1.0.0";
  package.canonical_artifact_path = DAPP_MULTI_FIXTURE;
  package.content_digest          = inspected.digest;
  package.backends                = {backend::cpu};
  package.interfaces              = {interface(OCUDU_DAPP_INTERFACE_CHANNEL_ESTIMATOR_V1, OCUDU_DAPP_CLASS_A_INLINE_V1),
                        optional_spectrum
                                         ? optional_interface(OCUDU_DAPP_INTERFACE_SPECTRUM_V1, OCUDU_DAPP_CLASS_C_OPEN_LOOP_V1)
                                         : interface(OCUDU_DAPP_INTERFACE_SPECTRUM_V1, OCUDU_DAPP_CLASS_C_OPEN_LOOP_V1)};
  return package;
}

package_record optional_fusion_package()
{
  artifact_verifier verifier(DAPP_TEST_CATALOG);
  auto              inspected = verifier.inspect(DAPP_OPTIONAL_MULTI_FIXTURE);
  EXPECT_TRUE(inspected) << inspected.detail;
  package_record package;
  package.package_id              = id(2);
  package.global_name             = "org.deepsig.ocudu.test.srs-dmrs-fusion";
  package.vendor                  = "DeepSig";
  package.version                 = "v1";
  package.canonical_artifact_path = DAPP_OPTIONAL_MULTI_FIXTURE;
  package.content_digest          = inspected.digest;
  package.backends                = {backend::cpu};
  package.interfaces              = {interface(OCUDU_DAPP_INTERFACE_SRS_ISAC_V1, OCUDU_DAPP_CLASS_C_OPEN_LOOP_V1),
                                     optional_interface(0x7f010002U, OCUDU_DAPP_CLASS_C_OPEN_LOOP_V1)};
  return package;
}

operation_result control(instance_manager& manager,
                         identifier        instance,
                         uint64_t          transaction,
                         lifecycle_action  action,
                         uint64_t          generation,
                         bool              dry_run    = false,
                         uint64_t          timeout_us = 100'000)
{
  lifecycle_request request;
  request.transaction_id      = id(transaction);
  request.instance_id         = instance;
  request.action              = action;
  request.expected_generation = generation;
  request.timeout_us          = timeout_us;
  request.payload_fingerprint = transaction;
  request.dry_run             = dry_run;
  return manager.control(request);
}

struct binding_observer final : native_hook_binding_observer {
  ocudu_dapp_status_v1 activated(const identifier&                    instance_id,
                                 const std::vector<interface_record>& interfaces) noexcept override
  {
    active_instance = instance_id;
    for (const auto& interface : interfaces) {
      active_interfaces.push_back(interface.interface_id);
    }
    ++activations;
    return OCUDU_DAPP_OK_V1;
  }

  ocudu_dapp_status_v1 deactivated(const identifier&                    instance_id,
                                   const std::vector<interface_record>& interfaces) noexcept override
  {
    inactive_instance = instance_id;
    for (const auto& interface : interfaces) {
      inactive_interfaces.push_back(interface.interface_id);
    }
    ++deactivations;
    return OCUDU_DAPP_OK_V1;
  }

  identifier            active_instance;
  identifier            inactive_instance;
  std::vector<uint32_t> active_interfaces;
  std::vector<uint32_t> inactive_interfaces;
  uint32_t              activations{};
  uint32_t              deactivations{};
};

} // namespace

TEST(dapp_native_hook_registry, commits_two_interfaces_through_one_gate)
{
  fixture_control control;
  auto            created = create_multi(control);
  ASSERT_TRUE(created) << created.operation.detail;
  advance_to_shadow(*created.instance);

  auto channel_result  = native_hook_point::create(OCUDU_DAPP_INTERFACE_CHANNEL_ESTIMATOR_V1, 10, 2);
  auto spectrum_result = native_hook_point::create(OCUDU_DAPP_INTERFACE_SPECTRUM_V1, 20, 1);
  ASSERT_TRUE(channel_result && spectrum_result);
  auto                 channel  = std::shared_ptr<native_hook_point>(std::move(channel_result.point));
  auto                 spectrum = std::shared_ptr<native_hook_point>(std::move(spectrum_result.point));
  native_hook_registry registry;
  ASSERT_TRUE(registry.register_point(channel));
  ASSERT_TRUE(registry.register_point(spectrum));
  auto observer = std::make_shared<binding_observer>();
  ASSERT_TRUE(registry.set_binding_observer(observer));

  auto gate = std::make_shared<native_activation_gate>();
  ASSERT_TRUE(registry.stage(id(9),
                             *created.instance,
                             {interface(OCUDU_DAPP_INTERFACE_CHANNEL_ESTIMATOR_V1, OCUDU_DAPP_CLASS_A_INLINE_V1),
                              interface(OCUDU_DAPP_INTERFACE_SPECTRUM_V1, OCUDU_DAPP_CLASS_C_OPEN_LOOP_V1)},
                             gate));
  EXPECT_TRUE(channel->bound());
  EXPECT_TRUE(spectrum->bound());
  EXPECT_FALSE(channel->active());
  EXPECT_FALSE(spectrum->active());
  EXPECT_EQ(control.data_worker_creates.load(std::memory_order_relaxed), 3U);

  const std::array<complex32, 4>        samples{};
  ocudu_dapp_channel_estimator_input_v1 channel_input{};
  channel_input.invocation = invocation(10);
  channel_input.rx_grid    = tensor(samples.data());
  ocudu_dapp_channel_estimator_output_v1 channel_output{};
  channel_output.channel_estimates            = channel_input.rx_grid;
  channel_output.channel_estimates.access     = OCUDU_DAPP_ACCESS_WRITE_V1 | OCUDU_DAPP_ACCESS_HOST_VISIBLE_V1;
  channel_output.noise_variances              = channel_output.channel_estimates;
  channel_output.noise_variances.element_type = OCUDU_DAPP_ELEMENT_F32_V1;
  EXPECT_EQ(channel->invoke(0, channel_input, channel_output).status, OCUDU_DAPP_BYPASS_V1);

  ASSERT_TRUE(created.instance->activate());
  ASSERT_TRUE(registry.commit(id(9)));
  EXPECT_EQ(observer->activations, 1U);
  EXPECT_EQ(observer->active_instance, id(9));
  EXPECT_EQ(observer->active_interfaces,
            (std::vector<uint32_t>{OCUDU_DAPP_INTERFACE_CHANNEL_ESTIMATOR_V1, OCUDU_DAPP_INTERFACE_SPECTRUM_V1}));
  EXPECT_TRUE(channel->active());
  EXPECT_TRUE(spectrum->active());
  EXPECT_TRUE(channel->invoke(0, channel_input, channel_output));

  ocudu_dapp_spectrum_input_v1 spectrum_input{};
  spectrum_input.invocation                           = invocation(20);
  spectrum_input.uplink_resource_grid                 = tensor(samples.data());
  spectrum_input.uplink_resource_grid.rank            = 3;
  spectrum_input.uplink_resource_grid.dimensions[0]   = 1;
  spectrum_input.uplink_resource_grid.dimensions[1]   = 1;
  spectrum_input.uplink_resource_grid.dimensions[2]   = 4;
  spectrum_input.uplink_resource_grid.byte_strides[0] = 4 * sizeof(complex32);
  spectrum_input.uplink_resource_grid.byte_strides[1] = 4 * sizeof(complex32);
  spectrum_input.uplink_resource_grid.byte_strides[2] = sizeof(complex32);
  ocudu_dapp_spectrum_slot_metadata_v1 spectrum_metadata{};
  spectrum_metadata.struct_size                        = sizeof(spectrum_metadata);
  spectrum_metadata.abi_major                          = OCUDU_DAPP_ABI_MAJOR_V1;
  spectrum_metadata.bandwidth_hz                       = 60'000;
  spectrum_metadata.rx_ports                           = 1;
  spectrum_metadata.symbols                            = 1;
  spectrum_metadata.subcarriers                        = 4;
  spectrum_metadata.amplitude_scale                    = 1.0F;
  spectrum_input.slot_metadata.struct_size             = sizeof(spectrum_input.slot_metadata);
  spectrum_input.slot_metadata.abi_major               = OCUDU_DAPP_ABI_MAJOR_V1;
  spectrum_input.slot_metadata.metadata_schema_id      = OCUDU_DAPP_METADATA_SPECTRUM_SLOT_V1;
  spectrum_input.slot_metadata.metadata_schema_version = 1;
  spectrum_input.slot_metadata.metadata                = {&spectrum_metadata, sizeof(spectrum_metadata), 0};
  spectrum_input.source_sequence                       = spectrum_input.invocation.input_generation;
  EXPECT_TRUE(spectrum->publish(0, spectrum_input));
  EXPECT_EQ(control.data_invocations.load(std::memory_order_relaxed), 2U);

  ASSERT_TRUE(registry.deactivate(id(9), std::chrono::steady_clock::now() + 100ms));
  EXPECT_EQ(observer->deactivations, 1U);
  EXPECT_EQ(observer->inactive_instance, id(9));
  EXPECT_EQ(observer->inactive_interfaces, observer->active_interfaces);
  EXPECT_FALSE(channel->active());
  EXPECT_FALSE(channel->bound());
  EXPECT_FALSE(spectrum->active());
  EXPECT_FALSE(spectrum->bound());
  EXPECT_EQ(control.data_worker_destroys.load(std::memory_order_relaxed), 3U);
  ASSERT_TRUE(created.instance->drain(std::chrono::steady_clock::now() + 100ms));
  ASSERT_TRUE(created.instance->unload());
}

TEST(dapp_native_hook_registry, rolls_back_every_staged_point_when_a_later_interface_cannot_bind)
{
  fixture_control control;
  auto            created = create_multi(control);
  ASSERT_TRUE(created);
  advance_to_shadow(*created.instance);

  auto channel_result = native_hook_point::create(OCUDU_DAPP_INTERFACE_CHANNEL_ESTIMATOR_V1, 1, 1);
  auto srs_result     = native_hook_point::create(OCUDU_DAPP_INTERFACE_SRS_ISAC_V1, 2, 1);
  ASSERT_TRUE(channel_result && srs_result);
  auto                 channel = std::shared_ptr<native_hook_point>(std::move(channel_result.point));
  auto                 srs     = std::shared_ptr<native_hook_point>(std::move(srs_result.point));
  native_hook_registry registry;
  ASSERT_TRUE(registry.register_point(channel));
  ASSERT_TRUE(registry.register_point(srs));

  auto staged = registry.stage(id(10),
                               *created.instance,
                               {interface(OCUDU_DAPP_INTERFACE_CHANNEL_ESTIMATOR_V1, OCUDU_DAPP_CLASS_A_INLINE_V1),
                                interface(OCUDU_DAPP_INTERFACE_SRS_ISAC_V1, OCUDU_DAPP_CLASS_C_OPEN_LOOP_V1)},
                               std::make_shared<native_activation_gate>());
  EXPECT_EQ(staged.status, OCUDU_DAPP_UNSUPPORTED_V1);
  EXPECT_FALSE(channel->bound());
  EXPECT_FALSE(srs->bound());
  EXPECT_FALSE(registry.bound(id(10)));
  EXPECT_EQ(control.data_worker_creates.load(std::memory_order_relaxed), 1U);
  EXPECT_EQ(control.data_worker_destroys.load(std::memory_order_relaxed), 1U);
  ASSERT_TRUE(created.instance->drain(std::chrono::steady_clock::now() + 100ms));
  ASSERT_TRUE(created.instance->unload());
}

TEST(dapp_native_hook_registry, rejects_stage_when_package_backend_is_outside_hook_l1_path_mask)
{
  // Host-path Class-A fixture (backend=HOST). Restrict the hook to CUDA-only L1 path.
  fixture_control control;
  auto            created = create_multi(control);
  ASSERT_TRUE(created);
  advance_to_shadow(*created.instance);

  auto channel_result = native_hook_point::create(OCUDU_DAPP_INTERFACE_CHANNEL_ESTIMATOR_V1, 1, 1);
  ASSERT_TRUE(channel_result);
  auto channel = std::shared_ptr<native_hook_point>(std::move(channel_result.point));
  channel->set_allowed_backend_mask(uint32_t{1} << OCUDU_DAPP_BACKEND_CUDA_V1);

  native_hook_registry registry;
  ASSERT_TRUE(registry.register_point(channel));

  auto staged = registry.stage(id(19),
                               *created.instance,
                               {interface(OCUDU_DAPP_INTERFACE_CHANNEL_ESTIMATOR_V1, OCUDU_DAPP_CLASS_A_INLINE_V1)},
                               std::make_shared<native_activation_gate>());
  EXPECT_EQ(staged.status, OCUDU_DAPP_UNSUPPORTED_V1);
  EXPECT_NE(staged.detail.find("L1 data path"), std::string::npos) << staged.detail;
  EXPECT_FALSE(channel->bound());
  EXPECT_FALSE(registry.bound(id(19)));
  ASSERT_TRUE(created.instance->drain(std::chrono::steady_clock::now() + 100ms));
  ASSERT_TRUE(created.instance->unload());
}

TEST(dapp_native_hook_registry, skips_unavailable_optional_input_and_reports_manifest_ordered_binding_state)
{
  fixture_control control;
  auto            created = create_multi(control);
  ASSERT_TRUE(created);
  advance_to_shadow(*created.instance);

  auto channel_result = native_hook_point::create(OCUDU_DAPP_INTERFACE_CHANNEL_ESTIMATOR_V1, 1, 1);
  ASSERT_TRUE(channel_result);
  auto channel = std::shared_ptr<native_hook_point>(std::move(channel_result.point));

  native_hook_registry registry;
  ASSERT_TRUE(registry.register_point(channel));
  auto observer = std::make_shared<binding_observer>();
  ASSERT_TRUE(registry.set_binding_observer(observer));

  std::vector<interface_record> interfaces{
      interface(OCUDU_DAPP_INTERFACE_CHANNEL_ESTIMATOR_V1, OCUDU_DAPP_CLASS_A_INLINE_V1),
      optional_interface(OCUDU_DAPP_INTERFACE_SPECTRUM_V1, OCUDU_DAPP_CLASS_C_OPEN_LOOP_V1)};
  ASSERT_TRUE(registry.validate(interfaces));
  ASSERT_TRUE(registry.stage(id(11), *created.instance, interfaces, std::make_shared<native_activation_gate>()));
  ASSERT_TRUE(created.instance->activate());
  ASSERT_TRUE(registry.commit(id(11)));
  EXPECT_EQ(observer->active_interfaces, (std::vector<uint32_t>{OCUDU_DAPP_INTERFACE_CHANNEL_ESTIMATOR_V1}));
  ASSERT_TRUE(registry.binding_states(id(11), interfaces));
  EXPECT_EQ(interfaces[0].state, binding_state::bound);
  EXPECT_EQ(interfaces[1].state, binding_state::unavailable);
  EXPECT_EQ(control.data_worker_creates.load(std::memory_order_relaxed), 1U);

  ASSERT_TRUE(registry.deactivate(id(11), std::chrono::steady_clock::now() + 100ms));
  EXPECT_EQ(observer->inactive_interfaces, observer->active_interfaces);
  EXPECT_FALSE(channel->bound());
  ASSERT_TRUE(created.instance->drain(std::chrono::steady_clock::now() + 100ms));
  ASSERT_TRUE(created.instance->unload());
}

TEST(dapp_native_hook_registry, activates_an_all_optional_module_when_no_hook_is_available)
{
  fixture_control control;
  auto            created = create_multi(control);
  ASSERT_TRUE(created);
  advance_to_shadow(*created.instance);

  native_hook_registry          registry;
  std::vector<interface_record> interfaces{
      optional_interface(OCUDU_DAPP_INTERFACE_CHANNEL_ESTIMATOR_V1, OCUDU_DAPP_CLASS_A_INLINE_V1),
      optional_interface(OCUDU_DAPP_INTERFACE_SPECTRUM_V1, OCUDU_DAPP_CLASS_C_OPEN_LOOP_V1)};
  ASSERT_TRUE(registry.validate(interfaces));
  ASSERT_TRUE(registry.stage(id(12), *created.instance, interfaces, std::make_shared<native_activation_gate>()));
  ASSERT_TRUE(created.instance->activate());
  ASSERT_TRUE(registry.commit(id(12)));
  EXPECT_TRUE(registry.bound(id(12)));
  ASSERT_TRUE(registry.binding_states(id(12), interfaces));
  EXPECT_EQ(interfaces[0].state, binding_state::unavailable);
  EXPECT_EQ(interfaces[1].state, binding_state::unavailable);
  ASSERT_TRUE(registry.deactivate(id(12), std::chrono::steady_clock::now() + 100ms));
  ASSERT_TRUE(created.instance->drain(std::chrono::steady_clock::now() + 100ms));
  ASSERT_TRUE(created.instance->unload());
}

TEST(dapp_native_hook_registry, instance_manager_activation_publishes_and_withdraws_complete_hook_set)
{
  auto channel_result  = native_hook_point::create(OCUDU_DAPP_INTERFACE_CHANNEL_ESTIMATOR_V1, 30, 2);
  auto spectrum_result = native_hook_point::create(OCUDU_DAPP_INTERFACE_SPECTRUM_V1, 40, 1);
  ASSERT_TRUE(channel_result && spectrum_result);
  auto channel  = std::shared_ptr<native_hook_point>(std::move(channel_result.point));
  auto spectrum = std::shared_ptr<native_hook_point>(std::move(spectrum_result.point));
  auto registry = std::make_shared<native_hook_registry>();
  ASSERT_TRUE(registry->register_point(channel));
  ASSERT_TRUE(registry->register_point(spectrum));

  instance_manager manager(DAPP_TEST_CATALOG, host_info(), host_api(), 32, registry);
  ASSERT_TRUE(manager.register_package(multi_package()));
  fixture_control control_state;
  load_request    load;
  load.transaction_id = id(100);
  load.package_id     = id(1);
  load.create_args    = &control_state;
  auto loaded         = manager.load(load);
  ASSERT_TRUE(loaded && loaded.instance_id) << loaded.detail;
  const identifier instance_id = *loaded.instance_id;

  ASSERT_TRUE(control(manager, instance_id, 101, lifecycle_action::prepare, 1));
  ASSERT_TRUE(control(manager, instance_id, 102, lifecycle_action::arm, 2));
  ASSERT_TRUE(control(manager, instance_id, 103, lifecycle_action::enter_shadow, 3));
  EXPECT_FALSE(channel->bound());
  EXPECT_FALSE(spectrum->bound());
  EXPECT_EQ(control(manager, instance_id, 104, lifecycle_action::activate, 4, true).code, result_code::dry_run_ok);
  ASSERT_TRUE(control(manager, instance_id, 105, lifecycle_action::activate, 4));
  EXPECT_TRUE(channel->active());
  EXPECT_TRUE(spectrum->active());
  EXPECT_TRUE(registry->bound(instance_id));

  ASSERT_TRUE(control(manager, instance_id, 106, lifecycle_action::deactivate, 5));
  EXPECT_FALSE(channel->bound());
  EXPECT_FALSE(spectrum->bound());
  EXPECT_FALSE(registry->bound(instance_id));
  ASSERT_TRUE(control(manager, instance_id, 107, lifecycle_action::unload, 6));
}

TEST(dapp_native_hook_registry, instance_manager_keeps_optional_unavailable_input_visible_without_binding_it)
{
  auto channel_result = native_hook_point::create(OCUDU_DAPP_INTERFACE_CHANNEL_ESTIMATOR_V1, 45, 1);
  ASSERT_TRUE(channel_result);
  auto channel  = std::shared_ptr<native_hook_point>(std::move(channel_result.point));
  auto registry = std::make_shared<native_hook_registry>();
  ASSERT_TRUE(registry->register_point(channel));

  instance_manager manager(DAPP_TEST_CATALOG, host_info(), host_api(), 32, registry);
  ASSERT_TRUE(manager.register_package(multi_package(true)));
  fixture_control control_state;
  load_request    load;
  load.transaction_id = id(150);
  load.package_id     = id(1);
  load.create_args    = &control_state;
  auto loaded         = manager.load(load);
  ASSERT_TRUE(loaded && loaded.instance_id) << loaded.detail;
  const identifier instance_id = *loaded.instance_id;

  auto inventory = manager.instance(instance_id);
  ASSERT_TRUE(inventory);
  ASSERT_EQ(inventory->interfaces.size(), 2U);
  EXPECT_EQ(inventory->interfaces[0].requirement, binding_requirement::required);
  EXPECT_EQ(inventory->interfaces[1].requirement, binding_requirement::optional);
  EXPECT_EQ(inventory->interfaces[0].state, binding_state::unbound);
  EXPECT_EQ(inventory->interfaces[1].state, binding_state::unbound);

  ASSERT_TRUE(control(manager, instance_id, 151, lifecycle_action::prepare, 1));
  ASSERT_TRUE(control(manager, instance_id, 152, lifecycle_action::arm, 2));
  ASSERT_TRUE(control(manager, instance_id, 153, lifecycle_action::enter_shadow, 3));
  EXPECT_EQ(control(manager, instance_id, 154, lifecycle_action::activate, 4, true).code, result_code::dry_run_ok);
  ASSERT_TRUE(control(manager, instance_id, 155, lifecycle_action::activate, 4));
  inventory = manager.instance(instance_id);
  ASSERT_TRUE(inventory);
  EXPECT_EQ(inventory->interfaces[0].state, binding_state::bound);
  EXPECT_EQ(inventory->interfaces[1].state, binding_state::unavailable);
  EXPECT_TRUE(channel->active());
  EXPECT_EQ(control_state.data_worker_creates.load(std::memory_order_relaxed), 1U);

  ASSERT_TRUE(control(manager, instance_id, 156, lifecycle_action::deactivate, 5));
  inventory = manager.instance(instance_id);
  ASSERT_TRUE(inventory);
  EXPECT_EQ(inventory->interfaces[0].state, binding_state::unbound);
  EXPECT_EQ(inventory->interfaces[1].state, binding_state::unbound);
  EXPECT_FALSE(channel->bound());
  ASSERT_TRUE(control(manager, instance_id, 157, lifecycle_action::unload, 6));
}

TEST(dapp_native_hook_registry, loads_a_v1_module_with_a_test_only_future_optional_interface)
{
  auto srs_result = native_hook_point::create(OCUDU_DAPP_INTERFACE_SRS_ISAC_V1, 46, 1);
  ASSERT_TRUE(srs_result);
  auto srs      = std::shared_ptr<native_hook_point>(std::move(srs_result.point));
  auto registry = std::make_shared<native_hook_registry>();
  ASSERT_TRUE(registry->register_point(srs));

  instance_manager manager(DAPP_TEST_CATALOG, host_info(), host_api(), 32, registry);
  ASSERT_TRUE(manager.register_package(optional_fusion_package()));
  fixture_control control_state;
  load_request    load;
  load.transaction_id = id(160);
  load.package_id     = id(2);
  load.create_args    = &control_state;
  auto loaded         = manager.load(load);
  ASSERT_TRUE(loaded && loaded.instance_id) << loaded.detail;
  const identifier instance_id = *loaded.instance_id;

  ASSERT_TRUE(control(manager, instance_id, 161, lifecycle_action::prepare, 1));
  ASSERT_TRUE(control(manager, instance_id, 162, lifecycle_action::arm, 2));
  ASSERT_TRUE(control(manager, instance_id, 163, lifecycle_action::enter_shadow, 3));
  ASSERT_TRUE(control(manager, instance_id, 164, lifecycle_action::activate, 4));
  auto inventory = manager.instance(instance_id);
  ASSERT_TRUE(inventory);
  ASSERT_EQ(inventory->interfaces.size(), 2U);
  EXPECT_EQ(inventory->interfaces[0].interface_id, OCUDU_DAPP_INTERFACE_SRS_ISAC_V1);
  EXPECT_EQ(inventory->interfaces[0].state, binding_state::bound);
  EXPECT_EQ(inventory->interfaces[1].interface_id, 0x7f010002U);
  EXPECT_EQ(inventory->interfaces[1].requirement, binding_requirement::optional);
  EXPECT_EQ(inventory->interfaces[1].state, binding_state::unavailable);
  EXPECT_TRUE(srs->active());
  EXPECT_EQ(control_state.data_worker_creates.load(std::memory_order_relaxed), 1U);

  ASSERT_TRUE(control(manager, instance_id, 165, lifecycle_action::deactivate, 5));
  ASSERT_TRUE(control(manager, instance_id, 166, lifecycle_action::unload, 6));
}

TEST(dapp_native_hook_registry, manager_destruction_unpublishes_active_native_hooks)
{
  auto channel_result  = native_hook_point::create(OCUDU_DAPP_INTERFACE_CHANNEL_ESTIMATOR_V1, 50, 1);
  auto spectrum_result = native_hook_point::create(OCUDU_DAPP_INTERFACE_SPECTRUM_V1, 60, 1);
  ASSERT_TRUE(channel_result && spectrum_result);
  auto channel  = std::shared_ptr<native_hook_point>(std::move(channel_result.point));
  auto spectrum = std::shared_ptr<native_hook_point>(std::move(spectrum_result.point));
  auto registry = std::make_shared<native_hook_registry>();
  ASSERT_TRUE(registry->register_point(channel));
  ASSERT_TRUE(registry->register_point(spectrum));
  fixture_control control_state;

  {
    instance_manager manager(DAPP_TEST_CATALOG, host_info(), host_api(), 32, registry);
    ASSERT_TRUE(manager.register_package(multi_package()));
    load_request load;
    load.transaction_id = id(200);
    load.package_id     = id(1);
    load.create_args    = &control_state;
    auto loaded         = manager.load(load);
    ASSERT_TRUE(loaded && loaded.instance_id);
    ASSERT_TRUE(control(manager, *loaded.instance_id, 201, lifecycle_action::prepare, 1));
    ASSERT_TRUE(control(manager, *loaded.instance_id, 202, lifecycle_action::arm, 2));
    ASSERT_TRUE(control(manager, *loaded.instance_id, 203, lifecycle_action::enter_shadow, 3));
    ASSERT_TRUE(control(manager, *loaded.instance_id, 204, lifecycle_action::activate, 4));
    ASSERT_TRUE(channel->active());
    ASSERT_TRUE(spectrum->active());
  }

  EXPECT_FALSE(channel->active());
  EXPECT_FALSE(channel->bound());
  EXPECT_FALSE(spectrum->active());
  EXPECT_FALSE(spectrum->bound());
  EXPECT_EQ(control_state.data_worker_destroys.load(std::memory_order_relaxed), 2U);
}

TEST(dapp_native_hook_registry, dry_run_rejects_an_incomplete_production_hook_set_without_staging_workers)
{
  auto channel_result = native_hook_point::create(OCUDU_DAPP_INTERFACE_CHANNEL_ESTIMATOR_V1, 70, 1);
  ASSERT_TRUE(channel_result);
  auto channel  = std::shared_ptr<native_hook_point>(std::move(channel_result.point));
  auto registry = std::make_shared<native_hook_registry>();
  ASSERT_TRUE(registry->register_point(channel));

  instance_manager manager(DAPP_TEST_CATALOG, host_info(), host_api(), 32, registry);
  ASSERT_TRUE(manager.register_package(multi_package()));
  fixture_control control_state;
  load_request    load;
  load.transaction_id = id(300);
  load.package_id     = id(1);
  load.create_args    = &control_state;
  auto loaded         = manager.load(load);
  ASSERT_TRUE(loaded && loaded.instance_id);
  ASSERT_TRUE(control(manager, *loaded.instance_id, 301, lifecycle_action::prepare, 1));
  ASSERT_TRUE(control(manager, *loaded.instance_id, 302, lifecycle_action::arm, 2));
  ASSERT_TRUE(control(manager, *loaded.instance_id, 303, lifecycle_action::enter_shadow, 3));

  auto validated = control(manager, *loaded.instance_id, 304, lifecycle_action::activate, 4, true);
  EXPECT_EQ(validated.code, result_code::incompatible);
  EXPECT_EQ(control_state.data_worker_creates.load(std::memory_order_relaxed), 0U);
  EXPECT_FALSE(channel->bound());
  ASSERT_TRUE(control(manager, *loaded.instance_id, 305, lifecycle_action::deactivate, 4));
  ASSERT_TRUE(control(manager, *loaded.instance_id, 306, lifecycle_action::unload, 5));
}

TEST(dapp_native_hook_registry, failed_grace_period_is_retryable_without_quarantining_the_instance)
{
  auto channel_result  = native_hook_point::create(OCUDU_DAPP_INTERFACE_CHANNEL_ESTIMATOR_V1, 80, 1);
  auto spectrum_result = native_hook_point::create(OCUDU_DAPP_INTERFACE_SPECTRUM_V1, 90, 1);
  ASSERT_TRUE(channel_result && spectrum_result);
  auto channel  = std::shared_ptr<native_hook_point>(std::move(channel_result.point));
  auto spectrum = std::shared_ptr<native_hook_point>(std::move(spectrum_result.point));
  auto registry = std::make_shared<native_hook_registry>();
  ASSERT_TRUE(registry->register_point(channel));
  ASSERT_TRUE(registry->register_point(spectrum));

  instance_manager manager(DAPP_TEST_CATALOG, host_info(), host_api(), 32, registry);
  ASSERT_TRUE(manager.register_package(multi_package()));
  fixture_control control_state;
  load_request    load;
  load.transaction_id = id(400);
  load.package_id     = id(1);
  load.create_args    = &control_state;
  auto loaded         = manager.load(load);
  ASSERT_TRUE(loaded && loaded.instance_id);
  const auto instance_id = *loaded.instance_id;
  ASSERT_TRUE(control(manager, instance_id, 401, lifecycle_action::prepare, 1));
  ASSERT_TRUE(control(manager, instance_id, 402, lifecycle_action::arm, 2));
  ASSERT_TRUE(control(manager, instance_id, 403, lifecycle_action::enter_shadow, 3));
  ASSERT_TRUE(control(manager, instance_id, 404, lifecycle_action::activate, 4));

  const std::array<complex32, 4>        samples{};
  ocudu_dapp_channel_estimator_input_v1 input{};
  input.invocation = invocation(80);
  input.rx_grid    = tensor(samples.data());
  ocudu_dapp_channel_estimator_output_v1 output{};
  output.channel_estimates            = input.rx_grid;
  output.channel_estimates.access     = OCUDU_DAPP_ACCESS_WRITE_V1 | OCUDU_DAPP_ACCESS_HOST_VISIBLE_V1;
  output.noise_variances              = output.channel_estimates;
  output.noise_variances.element_type = OCUDU_DAPP_ELEMENT_F32_V1;

  native_invocation_result invoked;
  control_state.block_data_invocations.store(true, std::memory_order_release);
  std::thread invocation_thread([&] { invoked = channel->invoke(0, input, output); });
  const auto  enter_deadline = std::chrono::steady_clock::now() + 500ms;
  while (control_state.active_data_invocations.load(std::memory_order_acquire) == 0 &&
         std::chrono::steady_clock::now() < enter_deadline) {
    std::this_thread::yield();
  }
  ASSERT_EQ(control_state.active_data_invocations.load(std::memory_order_acquire), 1U);

  auto timed_out = control(manager, instance_id, 405, lifecycle_action::deactivate, 5, false, 0);
  EXPECT_EQ(timed_out.code, result_code::lifecycle_failed);
  EXPECT_EQ(timed_out.state, OCUDU_DAPP_STATE_ACTIVE_V1);
  EXPECT_FALSE(channel->active());
  EXPECT_TRUE(channel->bound());
  EXPECT_TRUE(registry->bound(instance_id));
  auto bypass_output = output;
  EXPECT_EQ(channel->invoke(0, input, bypass_output).status, OCUDU_DAPP_BYPASS_V1);

  control_state.block_data_invocations.store(false, std::memory_order_release);
  invocation_thread.join();
  EXPECT_TRUE(invoked);
  auto deactivated =
      control(manager, instance_id, 406, lifecycle_action::deactivate, timed_out.instance_generation, false, 100'000);
  ASSERT_TRUE(deactivated) << deactivated.detail;
  EXPECT_EQ(deactivated.state, OCUDU_DAPP_STATE_INACTIVE_V1);
  EXPECT_FALSE(channel->bound());
  EXPECT_FALSE(spectrum->bound());
  EXPECT_FALSE(registry->bound(instance_id));
  ASSERT_TRUE(control(manager, instance_id, 407, lifecycle_action::unload, deactivated.instance_generation));
}

TEST(dapp_native_hook_registry, complete_hook_set_has_one_exclusive_native_owner)
{
  auto channel_result  = native_hook_point::create(OCUDU_DAPP_INTERFACE_CHANNEL_ESTIMATOR_V1, 100, 1);
  auto spectrum_result = native_hook_point::create(OCUDU_DAPP_INTERFACE_SPECTRUM_V1, 110, 1);
  ASSERT_TRUE(channel_result && spectrum_result);
  auto channel  = std::shared_ptr<native_hook_point>(std::move(channel_result.point));
  auto spectrum = std::shared_ptr<native_hook_point>(std::move(spectrum_result.point));
  auto registry = std::make_shared<native_hook_registry>();
  ASSERT_TRUE(registry->register_point(channel));
  ASSERT_TRUE(registry->register_point(spectrum));

  instance_manager manager(DAPP_TEST_CATALOG, host_info(), host_api(), 64, registry);
  ASSERT_TRUE(manager.register_package(multi_package()));
  fixture_control first_control;
  fixture_control second_control;
  load_request    first_load;
  first_load.transaction_id = id(500);
  first_load.package_id     = id(1);
  first_load.create_args    = &first_control;
  auto first                = manager.load(first_load);
  ASSERT_TRUE(first && first.instance_id);
  load_request second_load   = first_load;
  second_load.transaction_id = id(501);
  second_load.create_args    = &second_control;
  auto second                = manager.load(second_load);
  ASSERT_TRUE(second && second.instance_id);

  for (const auto& item :
       std::array<std::pair<identifier, uint64_t>, 2>{{{*first.instance_id, 510}, {*second.instance_id, 520}}}) {
    ASSERT_TRUE(control(manager, item.first, item.second, lifecycle_action::prepare, 1));
    ASSERT_TRUE(control(manager, item.first, item.second + 1, lifecycle_action::arm, 2));
    ASSERT_TRUE(control(manager, item.first, item.second + 2, lifecycle_action::enter_shadow, 3));
  }
  auto first_active = control(manager, *first.instance_id, 513, lifecycle_action::activate, 4);
  ASSERT_TRUE(first_active);
  auto conflict = control(manager, *second.instance_id, 523, lifecycle_action::activate, 4);
  EXPECT_EQ(conflict.code, result_code::lifecycle_failed);
  EXPECT_EQ(conflict.state, OCUDU_DAPP_STATE_SHADOW_V1);
  EXPECT_EQ(second_control.data_worker_creates.load(std::memory_order_relaxed), 0U);

  auto first_inactive =
      control(manager, *first.instance_id, 514, lifecycle_action::deactivate, first_active.instance_generation);
  ASSERT_TRUE(first_inactive);
  auto second_active =
      control(manager, *second.instance_id, 524, lifecycle_action::activate, conflict.instance_generation);
  ASSERT_TRUE(second_active) << second_active.detail;
  EXPECT_TRUE(channel->active());
  EXPECT_TRUE(spectrum->active());
  EXPECT_EQ(second_control.data_worker_creates.load(std::memory_order_relaxed), 2U);

  auto second_inactive =
      control(manager, *second.instance_id, 525, lifecycle_action::deactivate, second_active.instance_generation);
  ASSERT_TRUE(second_inactive);
  ASSERT_TRUE(control(manager, *first.instance_id, 515, lifecycle_action::unload, first_inactive.instance_generation));
  ASSERT_TRUE(
      control(manager, *second.instance_id, 526, lifecycle_action::unload, second_inactive.instance_generation));
}

TEST(dapp_native_hook_registry, route_table_slots_let_one_hook_hold_several_instances)
{
  const auto make_point = [](uint32_t interface_id) {
    auto created = native_hook_point::create(interface_id, 100, 1);
    EXPECT_TRUE(created) << created.detail;
    return std::shared_ptr<native_hook_point>(std::move(created.point));
  };
  {
    // Without a route table an interface has exactly one point.
    native_hook_registry plain;
    ASSERT_TRUE(plain.register_point(make_point(OCUDU_DAPP_INTERFACE_CHANNEL_ESTIMATOR_V1)));
    EXPECT_EQ(plain.register_point(make_point(OCUDU_DAPP_INTERFACE_CHANNEL_ESTIMATOR_V1)).status,
              OCUDU_DAPP_CONFLICT_V1);
  }

  auto table    = std::make_shared<pusch_route_table>(2);
  auto registry = std::make_shared<native_hook_registry>();
  ASSERT_TRUE(registry->set_route_table(table));
  auto slot0    = make_point(OCUDU_DAPP_INTERFACE_CHANNEL_ESTIMATOR_V1);
  auto slot1    = make_point(OCUDU_DAPP_INTERFACE_CHANNEL_ESTIMATOR_V1);
  auto spectrum = make_point(OCUDU_DAPP_INTERFACE_SPECTRUM_V1);
  ASSERT_TRUE(registry->register_point(slot0));
  ASSERT_TRUE(registry->register_point(slot1));
  ASSERT_TRUE(registry->register_point(spectrum));
  EXPECT_EQ(registry->register_point(make_point(OCUDU_DAPP_INTERFACE_CHANNEL_ESTIMATOR_V1)).status,
            OCUDU_DAPP_CONFLICT_V1)
      << "the table bounds the slots";
  EXPECT_EQ(registry->register_point(make_point(OCUDU_DAPP_INTERFACE_SPECTRUM_V1)).status, OCUDU_DAPP_CONFLICT_V1)
      << "only Class-A PUSCH hooks are routable";
  EXPECT_EQ(registry->find(OCUDU_DAPP_INTERFACE_CHANNEL_ESTIMATOR_V1), slot0);
  EXPECT_EQ(registry->find_all(OCUDU_DAPP_INTERFACE_CHANNEL_ESTIMATOR_V1).size(), 2U);

  instance_manager manager(DAPP_TEST_CATALOG, host_info(), host_api(), 64, registry);
  ASSERT_TRUE(manager.register_package(multi_package(true)));
  std::array<fixture_control, 3> controls;
  std::array<identifier, 3>      instances;
  for (uint64_t index = 0; index != instances.size(); ++index) {
    load_request load;
    load.transaction_id = id(600 + index);
    load.package_id     = id(1);
    load.create_args    = &controls[index];
    auto loaded         = manager.load(load);
    ASSERT_TRUE(loaded && loaded.instance_id);
    instances[index]   = *loaded.instance_id;
    const uint64_t txn = 610 + 10 * index;
    ASSERT_TRUE(control(manager, instances[index], txn, lifecycle_action::prepare, 1));
    ASSERT_TRUE(control(manager, instances[index], txn + 1, lifecycle_action::arm, 2));
    ASSERT_TRUE(control(manager, instances[index], txn + 2, lifecycle_action::enter_shadow, 3));
  }

  auto first = control(manager, instances[0], 700, lifecycle_action::activate, 4);
  ASSERT_TRUE(first) << first.detail;
  auto second = control(manager, instances[1], 701, lifecycle_action::activate, 4);
  ASSERT_TRUE(second) << second.detail;
  EXPECT_TRUE(slot0->active());
  EXPECT_TRUE(slot1->active());
  auto third = control(manager, instances[2], 702, lifecycle_action::activate, 4);
  EXPECT_EQ(third.code, result_code::lifecycle_failed) << "every slot is owned";
  EXPECT_EQ(third.state, OCUDU_DAPP_STATE_SHADOW_V1);

  auto bindings = table->state().bindings;
  ASSERT_EQ(bindings.size(), 2U);
  EXPECT_EQ(bindings[0].instance_id, instances[0]);
  EXPECT_EQ(bindings[0].slot, 0U);
  EXPECT_EQ(bindings[1].instance_id, instances[1]);
  EXPECT_EQ(bindings[1].slot, 1U);
  EXPECT_EQ(bindings[1].hook, pusch_route_hook::channel_estimator);

  ASSERT_TRUE(table->replace({}, {{std::nullopt, 0x4601, {pusch_route_target::kind::instance, instances[1]}}}));
  EXPECT_EQ(table->resolve(0, 0x4601), pusch_route::instance(pusch_route_hook::channel_estimator, 1));

  // The deactivated instance leaves the table under the registry mutex; the freed slot is reused.
  auto first_inactive = control(manager, instances[0], 703, lifecycle_action::deactivate, first.instance_generation);
  ASSERT_TRUE(first_inactive);
  EXPECT_FALSE(slot0->active());
  ASSERT_EQ(table->state().bindings.size(), 1U);
  auto third_active = control(manager, instances[2], 704, lifecycle_action::activate, third.instance_generation);
  ASSERT_TRUE(third_active) << third_active.detail;
  bindings = table->state().bindings;
  ASSERT_EQ(bindings.size(), 2U);
  EXPECT_EQ(bindings.back().instance_id, instances[2]);
  EXPECT_EQ(bindings.back().slot, 0U);
  EXPECT_EQ(table->resolve(0, 0x4601), pusch_route::instance(pusch_route_hook::channel_estimator, 1));

  auto second_inactive = control(manager, instances[1], 705, lifecycle_action::deactivate, second.instance_generation);
  ASSERT_TRUE(second_inactive);
  EXPECT_EQ(table->resolve(0, 0x4601), pusch_route::conventional()) << "a routed UE never falls to another instance";
  auto third_inactive =
      control(manager, instances[2], 706, lifecycle_action::deactivate, third_active.instance_generation);
  ASSERT_TRUE(third_inactive);
  EXPECT_TRUE(table->state().bindings.empty());
  ASSERT_TRUE(control(manager, instances[0], 707, lifecycle_action::unload, first_inactive.instance_generation));
  ASSERT_TRUE(control(manager, instances[1], 708, lifecycle_action::unload, second_inactive.instance_generation));
  ASSERT_TRUE(control(manager, instances[2], 709, lifecycle_action::unload, third_inactive.instance_generation));
}
