// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "../phy/support/resource_grid_test_doubles.h"
#include "apps/services/dapp/dapp_service.h"
#include "fixtures/fixture_common.h"
#include "tests/test_doubles/scheduler/scheduler_config_helper.h"
#include "tests/unittests/scheduler/test_utils/scheduler_test_simulator.h"
#include "ocudu/adt/complex.h"
#include "ocudu/dapp/abi/v1/metric_catalog.h"
#include "ocudu/dapp/runtime/artifact_verifier.h"
#include "ocudu/dapp/runtime/instance_manager.h"
#include "ocudu/dapp/runtime/native_hook_point.h"
#include "ocudu/dapp/use_cases/v1/spectrum.h"
#include "ocudu/phy/upper/dapp/upper_phy_dapp_hook_provider.h"
#include "ocudu/phy/upper/phy_tap/phy_tap.h"
#include "ocudu/scheduler/dapp/quiet_reservation.h"
#include "ocudu/scheduler/dapp/scheduler_dapp_context.h"
#include "ocudu/scheduler/dapp/scheduler_dapp_policy.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace ocudu;
using namespace ocudu::app_services;
using namespace ocudu::dapp;
using namespace ocudu::dapp::management;
using namespace std::chrono_literals;

namespace {

using json = nlohmann::json;

struct complex32 {
  float real;
  float imag;
};

enum class spectrum_placement { native, process };

struct composition_profile {
  std::filesystem::path catalog;
  std::string           spectrum_manifest;
  std::string           scheduler_manifest;
  std::string           spectrum_name;
  std::string           scheduler_name;
  spectrum_placement    placement{spectrum_placement::native};
  void*                 spectrum_create_args{};
  void*                 scheduler_create_args{};
  vrb_interval          expected_avoidance;
  bool                  expect_quiet_intent{};
};

class temporary_manifest
{
public:
  temporary_manifest(const std::filesystem::path& catalog, const json& document)
  {
    static std::atomic<uint64_t> sequence{};
    filename = "composition-" + std::to_string(::getpid()) + "-" +
               std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)) + ".json";
    path = catalog / filename;
    std::ofstream output(path);
    output << document.dump(2);
    output.close();
    if (!output || ::chmod(path.c_str(), 0444) != 0) {
      throw std::runtime_error("unable to create composition manifest");
    }
  }

  ~temporary_manifest()
  {
    static_cast<void>(::chmod(path.c_str(), 0644));
    std::error_code error;
    std::filesystem::remove(path, error);
  }

  temporary_manifest(const temporary_manifest&)            = delete;
  temporary_manifest& operator=(const temporary_manifest&) = delete;

  std::string filename;

private:
  std::filesystem::path path;
};

json fixture_manifest(const std::filesystem::path& catalog,
                      const std::filesystem::path& artifact_path,
                      std::string                  package_id,
                      std::string                  global_name,
                      std::string                  dapp_class,
                      uint32_t                     interface_id,
                      uint64_t                     capabilities,
                      bool                         interface_supports_process = false)
{
  artifact_verifier verifier(catalog);
  auto              artifact = verifier.inspect(artifact_path);
  if (!artifact) {
    throw std::runtime_error(artifact.detail);
  }
  json interface_placements = {"in_process"};
  if (interface_supports_process) {
    interface_placements.push_back("out_of_process");
  }
  return {{"schema_version", 1},
          {"package_id", std::move(package_id)},
          {"global_name", std::move(global_name)},
          {"vendor", "DeepSig"},
          {"version", "1.0.0"},
          {"artifact", artifact_path.filename().string()},
          {"sha256", to_hex(artifact.digest)},
          {"placements", {"in_process"}},
          {"backends", {"cpu"}},
          {"interfaces",
           {{{"interface_id", interface_id},
             {"major", 1},
             {"minor", 0},
             {"class", std::move(dapp_class)},
             {"placements", std::move(interface_placements)},
             {"threading", "per_worker"},
             {"capabilities", capabilities}}}}};
}

dapp_service_config service_config(const composition_profile& profile)
{
  dapp_service_config result;
  result.catalog_root                            = profile.catalog.string();
  result.native_preflight_helper                 = DAPP_PREFLIGHT_HELPER;
  result.package_signatures.mode                 = dapp::package_signature_mode::allow_unsigned;
  result.package_signatures.minimum_policy_epoch = 0;
  result.package_manifests                       = profile.spectrum_manifest == profile.scheduler_manifest
                                                       ? std::vector<std::string>{profile.spectrum_manifest}
                                                       : std::vector<std::string>{profile.spectrum_manifest, profile.scheduler_manifest};
  result.cells                                   = {{0, 1, 52}};
  result.nof_pusch_lanes                         = 1;
  result.nof_srs_lanes                           = 1;
  result.nof_scheduler_lanes                     = 8;
  result.max_scheduler_ul_prbs_avoided           = 8;
  result.scheduler_deadline                      = 100ms;
  result.maximum_transactions                    = 32;
  result.maximum_queued_jobs                     = 8;
  result.maximum_retained_jobs                   = 32;
  result.class_c_process.ring_slots              = 2;
  result.class_c_process.payload_capacity_bytes  = 128 * 1024;
  result.class_c_process.startup_timeout         = 500ms;
  result.class_c_process.stop_timeout            = 500ms;
  result.class_c_process.restart_backoff         = 10ms;
  return result;
}

operation_result control(instance_manager& manager,
                         const identifier& instance,
                         uint8_t&          transaction,
                         lifecycle_action  action,
                         uint64_t          generation)
{
  lifecycle_request request;
  request.transaction_id.bytes.back() = transaction++;
  request.instance_id                 = instance;
  request.action                      = action;
  request.expected_generation         = generation;
  request.timeout_us                  = 1'000'000;
  return manager.control(request);
}

struct active_instance {
  identifier id;
  uint64_t   generation{};
};

active_instance load_and_activate(instance_manager&       manager,
                                  std::string_view        global_name,
                                  ocudu_dapp_placement_v1 placement,
                                  void*                   create_args,
                                  uint8_t&                transaction)
{
  const auto inventory = manager.inventory();
  auto       package   = std::find_if(inventory.packages.begin(), inventory.packages.end(), [&](const auto& item) {
    return item.global_name == global_name;
  });
  if (package == inventory.packages.end()) {
    throw std::runtime_error("composition package not found: " + std::string(global_name));
  }
  load_request load;
  load.transaction_id.bytes.back() = transaction++;
  load.package_id                  = package->package_id;
  load.placement                   = placement;
  load.selected_backend            = backend::cpu;
  load.create_args                 = create_args;
  auto result                      = manager.load(load);
  if (!result || !result.instance_id) {
    throw std::runtime_error(result.detail);
  }
  for (auto action :
       {lifecycle_action::prepare, lifecycle_action::arm, lifecycle_action::enter_shadow, lifecycle_action::activate}) {
    result = control(manager, *result.instance_id, transaction, action, result.instance_generation);
    if (!result) {
      throw std::runtime_error(result.detail);
    }
  }
  return {*result.instance_id, result.instance_generation};
}

ocudu_dapp_radio_time_v1 to_radio_time(slot_point slot) noexcept
{
  return {static_cast<uint16_t>(slot.sfn()),
          static_cast<uint16_t>(slot.slot_index()),
          static_cast<uint16_t>(slot.numerology()),
          0};
}

uint64_t monotonic_now_ns() noexcept
{
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

ocudu_dapp_spectrum_input_v1 make_spectrum_input(std::vector<complex32>&               samples,
                                                 std::vector<uint8_t>&                 expected_use,
                                                 ocudu_dapp_spectrum_slot_metadata_v1& metadata,
                                                 slot_point                            created,
                                                 uint64_t                              sequence,
                                                 uint32_t                              nof_prbs)
{
  ocudu_dapp_spectrum_input_v1 input{};
  input.invocation.struct_size                     = sizeof(input.invocation);
  input.invocation.abi_major                       = OCUDU_DAPP_ABI_MAJOR_V1;
  input.invocation.abi_minor                       = OCUDU_DAPP_ABI_MINOR_V1;
  input.invocation.invocation_id                   = sequence;
  input.invocation.cell_id                         = 0;
  input.invocation.worker_id                       = 0;
  input.invocation.radio_time                      = to_radio_time(created);
  input.invocation.input_generation                = sequence;
  input.uplink_resource_grid.struct_size           = sizeof(input.uplink_resource_grid);
  input.uplink_resource_grid.abi_major             = OCUDU_DAPP_ABI_MAJOR_V1;
  input.uplink_resource_grid.abi_minor             = OCUDU_DAPP_ABI_MINOR_V1;
  input.uplink_resource_grid.domain                = OCUDU_DAPP_MEM_HOST_V1;
  input.uplink_resource_grid.backend_id            = OCUDU_DAPP_BACKEND_HOST_V1;
  input.uplink_resource_grid.element_type          = OCUDU_DAPP_ELEMENT_CF32_V1;
  input.uplink_resource_grid.rank                  = 3;
  input.uplink_resource_grid.access                = OCUDU_DAPP_ACCESS_READ_V1 | OCUDU_DAPP_ACCESS_HOST_VISIBLE_V1;
  input.uplink_resource_grid.allocation_id         = sequence;
  input.uplink_resource_grid.allocation_generation = sequence;
  input.uplink_resource_grid.native_handle         = reinterpret_cast<uintptr_t>(samples.data());
  input.uplink_resource_grid.byte_length           = samples.size() * sizeof(complex32);
  input.uplink_resource_grid.dimensions[0]         = 1;
  input.uplink_resource_grid.dimensions[1]         = 1;
  input.uplink_resource_grid.dimensions[2]         = nof_prbs * NOF_SUBCARRIERS_PER_RB;
  input.uplink_resource_grid.byte_strides[0]       = input.uplink_resource_grid.byte_length;
  input.uplink_resource_grid.byte_strides[1]       = input.uplink_resource_grid.byte_length;
  input.uplink_resource_grid.byte_strides[2]       = sizeof(complex32);
  input.uplink_resource_grid.lease_token           = sequence;
  input.scheduled_re_mask.struct_size              = sizeof(input.scheduled_re_mask);
  input.scheduled_re_mask.abi_major                = OCUDU_DAPP_ABI_MAJOR_V1;
  input.scheduled_re_mask.abi_minor                = OCUDU_DAPP_ABI_MINOR_V1;
  input.scheduled_re_mask.domain                   = OCUDU_DAPP_MEM_HOST_V1;
  input.scheduled_re_mask.backend_id               = OCUDU_DAPP_BACKEND_HOST_V1;
  input.scheduled_re_mask.element_type             = OCUDU_DAPP_ELEMENT_U8_V1;
  input.scheduled_re_mask.rank                     = 2;
  input.scheduled_re_mask.access                   = OCUDU_DAPP_ACCESS_READ_V1 | OCUDU_DAPP_ACCESS_HOST_VISIBLE_V1;
  input.scheduled_re_mask.allocation_id            = sequence;
  input.scheduled_re_mask.allocation_generation    = sequence;
  input.scheduled_re_mask.native_handle            = reinterpret_cast<uintptr_t>(expected_use.data());
  input.scheduled_re_mask.byte_length              = expected_use.size();
  input.scheduled_re_mask.dimensions[0]            = 1;
  input.scheduled_re_mask.dimensions[1]            = nof_prbs;
  input.scheduled_re_mask.byte_strides[0]          = nof_prbs;
  input.scheduled_re_mask.byte_strides[1]          = 1;
  input.scheduled_re_mask.layout_id                = OCUDU_DAPP_LAYOUT_EXPECTED_UL_USE_SYMBOL_PRB_U8_V1;
  input.scheduled_re_mask.layout_version           = 1;
  input.scheduled_re_mask.lease_token              = sequence;
  metadata.struct_size                             = sizeof(metadata);
  metadata.abi_major                               = OCUDU_DAPP_ABI_MAJOR_V1;
  metadata.abi_minor                               = OCUDU_DAPP_ABI_MINOR_V1;
  metadata.bandwidth_hz                            = nof_prbs * NOF_SUBCARRIERS_PER_RB * 15'000U;
  metadata.rx_ports                                = 1;
  metadata.symbols                                 = 1;
  metadata.subcarriers                             = nof_prbs * NOF_SUBCARRIERS_PER_RB;
  metadata.amplitude_scale                         = 1.0F;
  metadata.flags                                   = OCUDU_DAPP_SPECTRUM_FLAG_EXPECTED_UL_USE_COMPLETE_V1;
  metadata.source_plan_generation                  = sequence;
  input.slot_metadata.struct_size                  = sizeof(input.slot_metadata);
  input.slot_metadata.abi_major                    = OCUDU_DAPP_ABI_MAJOR_V1;
  input.slot_metadata.abi_minor                    = OCUDU_DAPP_ABI_MINOR_V1;
  input.slot_metadata.metadata_schema_id           = OCUDU_DAPP_METADATA_SPECTRUM_SLOT_V1;
  input.slot_metadata.metadata_schema_version      = 1;
  input.slot_metadata.metadata                     = {&metadata, sizeof(metadata), 0};
  input.source_sequence                            = sequence;
  return input;
}

class contiguous_bf16_grid final : public resource_grid_reader_spy
{
public:
  contiguous_bf16_grid(uint32_t nof_prbs, uint32_t occupied_prbs) :
    resource_grid_reader_spy(1, MAX_NSYMB_PER_SLOT, nof_prbs),
    storage(MAX_NSYMB_PER_SLOT * nof_prbs * NOF_SUBCARRIERS_PER_RB, to_cbf16(cf_t{}))
  {
    for (uint32_t symbol = 0; symbol != MAX_NSYMB_PER_SLOT; ++symbol) {
      for (uint32_t subcarrier = 0; subcarrier != occupied_prbs * NOF_SUBCARRIERS_PER_RB; ++subcarrier) {
        storage[symbol * nof_prbs * NOF_SUBCARRIERS_PER_RB + subcarrier] = to_cbf16(cf_t{2.0F, 0.0F});
      }
    }
  }

  span<const cbf16_t> get_contiguous_view() const override { return storage; }

private:
  std::vector<cbf16_t> storage;
};

scheduler_test_sim_config simulator_config(std::shared_ptr<scheduler_dapp_hook_provider> hooks)
{
  scheduler_test_sim_config result;
  result.dapp_hooks = std::move(hooks);
  return result;
}

const ul_sched_info* run_until_pusch(scheduler_test_simulator& simulator, const sched_ue_creation_request_message& ue)
{
  const ul_sched_info* result = nullptr;
  simulator.run_slot_until(
      [&] {
        for (const auto& pusch : simulator.last_sched_result()->ul.puschs) {
          if (pusch.pusch_cfg.rnti == ue.crnti) {
            result = &pusch;
            return true;
          }
        }
        return false;
      },
      100);
  return result;
}

void publish_spectrum(dapp_service&      service,
                      spectrum_placement placement,
                      slot_point         created,
                      uint32_t           nof_prbs,
                      uint32_t           occupied_prbs)
{
  if (placement == spectrum_placement::process) {
    auto tap = service.upper_phy_hooks()->wrap_phy_tap({0, 0, 0}, nullptr);
    ASSERT_NE(tap, nullptr);
    contiguous_bf16_grid grid(nof_prbs, occupied_prbs);
    tap->handle_quiet_grid(grid, created);
    return;
  }
  auto hook = service.native_hooks()->find(OCUDU_DAPP_INTERFACE_SPECTRUM_V1);
  ASSERT_NE(hook, nullptr);
  std::vector<complex32> samples(nof_prbs * NOF_SUBCARRIERS_PER_RB);
  std::fill_n(samples.begin(), occupied_prbs * NOF_SUBCARRIERS_PER_RB, complex32{2.0F, 0.0F});
  std::vector<uint8_t>                 expected_use(nof_prbs, OCUDU_DAPP_UL_USE_PLANNED_EMPTY_V1);
  ocudu_dapp_spectrum_slot_metadata_v1 metadata{};
  auto input = make_spectrum_input(samples, expected_use, metadata, created, 1, nof_prbs);
  ASSERT_TRUE(hook->publish(0, input));
}

scheduler_dapp_interference_lease wait_for_interference(dapp_service& service, uint32_t nof_prbs, slot_point valid_slot)
{
  const auto deadline = std::chrono::steady_clock::now() + 1s;
  do {
    auto lease = service.interference_cache()->acquire(0, nof_prbs, valid_slot, monotonic_now_ns());
    if (lease) {
      return lease;
    }
    std::this_thread::sleep_for(1ms);
  } while (std::chrono::steady_clock::now() < deadline);
  return {};
}

std::optional<uint64_t> metric_value(instance_manager& manager, const identifier& instance, std::string_view name)
{
  auto metrics = manager.metrics(instance);
  if (!metrics) {
    return std::nullopt;
  }
  auto found =
      std::find_if(metrics.value.begin(), metrics.value.end(), [&](const auto& metric) { return metric.name == name; });
  if (found == metrics.value.end() || !std::holds_alternative<uint64_t>(found->sample_value)) {
    return std::nullopt;
  }
  return std::get<uint64_t>(found->sample_value);
}

void run_composition(const composition_profile& profile, bool scheduler_evaluation_only = false)
{
  auto service_cfg                      = service_config(profile);
  service_cfg.scheduler_evaluation_only = scheduler_evaluation_only;
  auto created                          = dapp_service::create(std::move(service_cfg));
  ASSERT_TRUE(created) << created.detail;
  uint8_t transaction = 1;
  auto    spectrum =
      load_and_activate(created.service->instances(),
                        profile.spectrum_name,
                        profile.placement == spectrum_placement::native ? OCUDU_DAPP_PLACEMENT_IN_PROCESS_V1
                                                                        : OCUDU_DAPP_PLACEMENT_OUT_OF_PROCESS_V1,
                        profile.spectrum_create_args,
                        transaction);
  auto scheduler = load_and_activate(created.service->instances(),
                                     profile.scheduler_name,
                                     OCUDU_DAPP_PLACEMENT_IN_PROCESS_V1,
                                     profile.scheduler_create_args,
                                     transaction);
  EXPECT_NE(spectrum.id, scheduler.id);

  {
    // The reference scheduler ships with every authority stage off so an
    // activated package preserves host policy ("align PHY and scheduler
    // behavior with host"). This composition verifies the avoidance path, so
    // opt in the way an operator would: a typed configSet.
    auto config = created.service->instances().config(scheduler.id);
    ASSERT_TRUE(config) << config.detail;
    config_set_request request;
    request.transaction_id.bytes.back() = transaction++;
    request.instance_id                 = scheduler.id;
    request.expected_config_generation  = config.value.generation;
    request.entries.push_back({"enable_prb_avoidance", true});
    auto result = created.service->instances().set_config(request);
    ASSERT_TRUE(result) << result.detail;
    scheduler.generation = result.instance_generation;
  }

  if (profile.placement == spectrum_placement::process && profile.spectrum_create_args == nullptr) {
    auto config = created.service->instances().config(spectrum.id);
    ASSERT_TRUE(config) << config.detail;
    ASSERT_EQ(config.value.generation, 1U);
    for (bool enabled : {false, true}) {
      config_set_request request;
      request.transaction_id.bytes.back() = transaction++;
      request.instance_id                 = spectrum.id;
      request.expected_config_generation  = config.value.generation;
      request.entries.push_back({"enabled", enabled});
      auto result = created.service->instances().set_config(request);
      ASSERT_TRUE(result) << result.detail;
      config.value.generation = result.resource_generation;
      spectrum.generation     = result.instance_generation;
    }
    EXPECT_EQ(config.value.generation, 3U);

    uint64_t model_generation = 1;
    for (auto action : {OCUDU_DAPP_MODEL_STAGE_V1,
                        OCUDU_DAPP_MODEL_VALIDATE_V1,
                        OCUDU_DAPP_MODEL_WARM_V1,
                        OCUDU_DAPP_MODEL_ACTIVATE_V1}) {
      model_request request;
      request.transaction_id.bytes.back() = transaction++;
      request.instance_id                 = spectrum.id;
      request.expected_model_generation   = model_generation;
      request.action                      = static_cast<ocudu_dapp_model_action_v1>(action);
      request.model_id                    = "composition-model";
      request.model_version               = "1";
      if (action == OCUDU_DAPP_MODEL_STAGE_V1) {
        request.artifact = {1, 2, 3, 4};
      }
      auto result = created.service->instances().manage_model(request);
      ASSERT_TRUE(result) << result.detail;
      model_generation    = result.resource_generation;
      spectrum.generation = result.instance_generation;
    }
    EXPECT_EQ(model_generation, 5U);

    auto pid = metric_value(created.service->instances(), spectrum.id, "process.pid");
    ASSERT_TRUE(pid);
    ASSERT_GT(*pid, 0U);
    ASSERT_EQ(::kill(static_cast<pid_t>(*pid), SIGKILL), 0);
    const auto restart_deadline = std::chrono::steady_clock::now() + 2s;
    do {
      auto restarts = metric_value(created.service->instances(), spectrum.id, "process.successful_restarts");
      if (restarts && *restarts == 1U) {
        break;
      }
      std::this_thread::sleep_for(1ms);
    } while (std::chrono::steady_clock::now() < restart_deadline);
    auto replays = metric_value(created.service->instances(), spectrum.id, "process.management_replays");
    ASSERT_TRUE(replays);
    EXPECT_EQ(*replays, 6U);
    config = created.service->instances().config(spectrum.id);
    ASSERT_TRUE(config) << config.detail;
    ASSERT_EQ(config.value.generation, 3U);
    ASSERT_EQ(config.value.entries.size(), 1U);
    EXPECT_TRUE(std::get<bool>(config.value.entries.front().entry_value));

    model_request probe;
    probe.transaction_id.bytes.back() = transaction++;
    probe.instance_id                 = spectrum.id;
    probe.expected_model_generation   = 5;
    probe.action                      = OCUDU_DAPP_MODEL_RETIRE_V1;
    probe.model_id                    = "composition-model";
    probe.dry_run                     = true;
    auto probed                       = created.service->instances().manage_model(probe);
    ASSERT_TRUE(probed) << probed.detail;
    EXPECT_EQ(probed.resource_generation, 5U);
    spectrum.generation = probed.instance_generation;
  }

  scheduler_test_simulator simulator(simulator_config(created.service->scheduler_hooks()));
  auto                     cell = sched_config_helper::make_default_sched_cell_configuration_request();
  simulator.add_cell(cell);
  auto ue = sched_config_helper::create_default_sched_ue_creation_request(simulator.cell_cfg().params, {LCID_MIN_DRB});
  simulator.add_ue(ue, true);
  const uint32_t   nof_prbs    = simulator.cell_cfg().params.dl_cfg_common.init_dl_bwp.generic_params.crbs.length();
  const slot_point source_slot = simulator.next_slot.without_hyper_sfn();
  created.service->scheduler_hooks()->publish_ul_plan(0, source_slot, ul_sched_result{});
  if (profile.placement == spectrum_placement::native && profile.spectrum_create_args == nullptr) {
    const auto invocations = metric_value(created.service->instances(), spectrum.id, "dapp.invoke.count");
    ASSERT_TRUE(invocations);
    EXPECT_EQ(*invocations, 1U);
  }
  publish_spectrum(*created.service, profile.placement, source_slot, nof_prbs, profile.expected_avoidance.length());
  EXPECT_FALSE(created.service->interference_cache()->acquire(0, nof_prbs, source_slot, monotonic_now_ns()));
  auto interference = wait_for_interference(*created.service, nof_prbs, source_slot + 1);
  ASSERT_TRUE(interference);
  EXPECT_EQ(interference.context().source_sequence, 1U);
  EXPECT_NE(interference.context().producer_instance_id[0] | interference.context().producer_instance_id[1], 0U);
  const auto* confidence = reinterpret_cast<const uint8_t*>(interference.mask().native_handle);
  ASSERT_NE(confidence, nullptr);
  ASSERT_EQ(interference.mask().dimensions[0], nof_prbs);
  for (uint32_t prb = 0; prb != nof_prbs; ++prb) {
    const bool expected = profile.expected_avoidance.contains(prb);
    EXPECT_EQ(confidence[prb], expected ? UINT8_MAX : 0U) << "PRB " << prb;
  }
  interference = {};

  std::vector<uint8_t> quiet_mask(nof_prbs);
  if (profile.expect_quiet_intent) {
    const auto               deadline = std::chrono::steady_clock::now() + 1s;
    quiet_reservation_lookup quiet{};
    do {
      quiet = created.service->quiet_reservations()->lookup(0, source_slot + 4, monotonic_now_ns(), quiet_mask);
      if (quiet.reservation_count != 0) {
        break;
      }
      std::this_thread::sleep_for(1ms);
    } while (std::chrono::steady_clock::now() < deadline);
    ASSERT_EQ(quiet.reservation_count, 1U);
    EXPECT_EQ(quiet_mask[profile.expected_avoidance.start()], 1U);
  }

  simulator.push_bsr(ul_bsr_indication_message{to_du_cell_index(0),
                                               ue.ue_index,
                                               ue.crnti,
                                               bsr_format::SHORT_BSR,
                                               {ul_bsr_lcg_report{uint_to_lcg_id(2), 100000}}});
  const auto* pusch = run_until_pusch(simulator, ue);
  ASSERT_NE(pusch, nullptr);
  ASSERT_TRUE(pusch->pusch_cfg.rbs.is_type1());
  if (!scheduler_evaluation_only) {
    EXPECT_FALSE(pusch->pusch_cfg.rbs.type1().overlaps(profile.expected_avoidance));
  }
  created.service->scheduler_hooks()->publish_ul_plan(0, source_slot + 1, simulator.last_sched_result()->ul);

  if (profile.scheduler_create_args == nullptr) {
    auto invocations = metric_value(created.service->instances(), scheduler.id, "dapp.invoke.count");
    ASSERT_TRUE(invocations);
    EXPECT_GT(*invocations, 0U);
    const std::string_view spectrum_metric =
        profile.placement == spectrum_placement::native ? "dapp.invoke.count" : "process.results_accepted";
    invocations = metric_value(created.service->instances(), spectrum.id, spectrum_metric);
    ASSERT_TRUE(invocations);
    EXPECT_GT(*invocations, 0U);
  }
  const auto class_b_invocations =
      metric_value(created.service->instances(), scheduler.id, OCUDU_DAPP_METRIC_CLASS_B_INVOCATIONS_V1);
  const auto class_b_accepted =
      metric_value(created.service->instances(), scheduler.id, OCUDU_DAPP_METRIC_CLASS_B_ACCEPTED_V1);
  const auto class_b_evaluation =
      metric_value(created.service->instances(), scheduler.id, OCUDU_DAPP_METRIC_CLASS_B_EVALUATION_ACCEPTED_V1);
  const auto class_b_rejections =
      metric_value(created.service->instances(), scheduler.id, OCUDU_DAPP_METRIC_CLASS_B_VALIDATION_REJECTIONS_V1);
  ASSERT_TRUE(class_b_invocations && class_b_accepted && class_b_evaluation && class_b_rejections);
  EXPECT_GT(*class_b_invocations, 0U);
  EXPECT_EQ(*class_b_accepted == 0U, scheduler_evaluation_only);
  EXPECT_EQ(*class_b_evaluation != 0U, scheduler_evaluation_only);
  EXPECT_EQ(*class_b_rejections, 0U);
  if (profile.placement == spectrum_placement::process) {
    const auto copies       = metric_value(created.service->instances(), spectrum.id, "class_c.copy.snapshot.count");
    const auto bytes        = metric_value(created.service->instances(), spectrum.id, "class_c.copy.snapshot.bytes");
    const auto publications = metric_value(created.service->instances(), spectrum.id, "class_c.route.publications");
    ASSERT_TRUE(copies && bytes && publications);
    EXPECT_GT(*copies, 0U);
    EXPECT_GT(*bytes, 0U);
    EXPECT_GE(*publications, *copies);
  }

  auto stopped = control(
      created.service->instances(), spectrum.id, transaction, lifecycle_action::deactivate, spectrum.generation);
  ASSERT_TRUE(stopped) << stopped.detail;
  if (profile.expect_quiet_intent) {
    EXPECT_EQ(created.service->quiet_reservations()
                  ->lookup(0, source_slot + 4, monotonic_now_ns(), quiet_mask)
                  .reservation_count,
              0U);
  }
  auto unloaded = control(
      created.service->instances(), spectrum.id, transaction, lifecycle_action::unload, stopped.instance_generation);
  ASSERT_TRUE(unloaded) << unloaded.detail;
  EXPECT_FALSE(created.service->interference_cache()->acquire(0, nof_prbs, source_slot + 1, monotonic_now_ns()));
  simulator.push_bsr(ul_bsr_indication_message{to_du_cell_index(0),
                                               ue.ue_index,
                                               ue.crnti,
                                               bsr_format::SHORT_BSR,
                                               {ul_bsr_lcg_report{uint_to_lcg_id(2), 100000}}});
  EXPECT_NE(run_until_pusch(simulator, ue), nullptr);

  stopped = control(
      created.service->instances(), scheduler.id, transaction, lifecycle_action::deactivate, scheduler.generation);
  ASSERT_TRUE(stopped) << stopped.detail;
  unloaded = control(
      created.service->instances(), scheduler.id, transaction, lifecycle_action::unload, stopped.instance_generation);
  ASSERT_TRUE(unloaded) << unloaded.detail;
}

// Only the DAPP_REFERENCE_BUILD_DIR tests below call this. Guarding it with the
// same condition keeps the file warning-clean when the reference build is not
// available, which -Werror=unused-function otherwise turns into a build failure.
#ifdef DAPP_REFERENCE_BUILD_DIR
void run_joint_composition(const composition_profile& profile)
{
  ASSERT_EQ(profile.spectrum_manifest, profile.scheduler_manifest);
  ASSERT_EQ(profile.spectrum_name, profile.scheduler_name);
  auto created = dapp_service::create(service_config(profile));
  ASSERT_TRUE(created) << created.detail;
  uint8_t transaction = 1;
  auto    joint       = load_and_activate(
      created.service->instances(), profile.spectrum_name, OCUDU_DAPP_PLACEMENT_IN_PROCESS_V1, nullptr, transaction);
  const auto inventory = created.service->instances().inventory();
  ASSERT_EQ(inventory.instances.size(), 1U);
  EXPECT_EQ(inventory.instances.front().instance_id, joint.id);

  scheduler_test_simulator simulator(simulator_config(created.service->scheduler_hooks()));
  auto                     cell = sched_config_helper::make_default_sched_cell_configuration_request();
  simulator.add_cell(cell);
  auto ue = sched_config_helper::create_default_sched_ue_creation_request(simulator.cell_cfg().params, {LCID_MIN_DRB});
  simulator.add_ue(ue, true);
  const uint32_t   nof_prbs    = simulator.cell_cfg().params.dl_cfg_common.init_dl_bwp.generic_params.crbs.length();
  const slot_point source_slot = simulator.next_slot.without_hyper_sfn();
  publish_spectrum(*created.service, spectrum_placement::native, source_slot, nof_prbs, 8);
  EXPECT_FALSE(created.service->interference_cache()->acquire(0, nof_prbs, source_slot, monotonic_now_ns()));
  auto interference = wait_for_interference(*created.service, nof_prbs, source_slot + 1);
  ASSERT_TRUE(interference);
  EXPECT_EQ(interference.context().source_sequence, 1U);
  interference = {};

  auto spectrum_hook = created.service->native_hooks()->find(OCUDU_DAPP_INTERFACE_SPECTRUM_V1);
  ASSERT_NE(spectrum_hook, nullptr);
  std::vector<complex32>               fault_samples(nof_prbs * NOF_SUBCARRIERS_PER_RB);
  std::vector<uint8_t>                 fault_expected_use(nof_prbs, OCUDU_DAPP_UL_USE_PLANNED_EMPTY_V1);
  ocudu_dapp_spectrum_slot_metadata_v1 fault_metadata{};
  auto                                 fault =
      make_spectrum_input(fault_samples, fault_expected_use, fault_metadata, source_slot, UINT64_MAX, nof_prbs);
  EXPECT_EQ(spectrum_hook->publish(0, fault).status, OCUDU_DAPP_EXCEPTION_V1);
  EXPECT_EQ(spectrum_hook->publish(0, fault).status, OCUDU_DAPP_BYPASS_V1);
  EXPECT_EQ(created.service->instances().drain_incidents(), 1U);
  incident_filter incident_query;
  incident_query.instance_id = std::array<uint64_t, 2>{};
  std::memcpy(incident_query.instance_id->data(), joint.id.bytes.data(), joint.id.bytes.size());
  incident_query.maximum_records = 8;
  const auto incidents           = created.service->instances().incidents().query(incident_query);
  ASSERT_EQ(incidents.size(), 1U);
  EXPECT_EQ(incidents.front().context.hook_id, OCUDU_DAPP_INTERFACE_SPECTRUM_V1);
  EXPECT_EQ(incidents.front().recurrence_count, 1U);
  EXPECT_TRUE(incidents.front().breaker_open);
  const auto active = created.service->instances().instance(joint.id);
  ASSERT_TRUE(active);
  EXPECT_EQ(active->state, OCUDU_DAPP_STATE_ACTIVE_V1);
  joint.generation = active->generation;

  simulator.push_bsr(ul_bsr_indication_message{to_du_cell_index(0),
                                               ue.ue_index,
                                               ue.crnti,
                                               bsr_format::SHORT_BSR,
                                               {ul_bsr_lcg_report{uint_to_lcg_id(2), 100000}}});
  const auto* pusch = run_until_pusch(simulator, ue);
  ASSERT_NE(pusch, nullptr);
  ASSERT_TRUE(pusch->pusch_cfg.rbs.is_type1());
  EXPECT_FALSE(pusch->pusch_cfg.rbs.type1().overlaps(vrb_interval{0, 8}));

  const auto invocations = metric_value(created.service->instances(), joint.id, "dapp.invoke.count");
  const auto class_b_accepted =
      metric_value(created.service->instances(), joint.id, OCUDU_DAPP_METRIC_CLASS_B_ACCEPTED_V1);
  const auto class_b_rejections =
      metric_value(created.service->instances(), joint.id, OCUDU_DAPP_METRIC_CLASS_B_VALIDATION_REJECTIONS_V1);
  ASSERT_TRUE(invocations && class_b_accepted && class_b_rejections);
  EXPECT_GT(*invocations, 1U);
  EXPECT_GT(*class_b_accepted, 0U);
  EXPECT_EQ(*class_b_rejections, 0U);

  auto stopped =
      control(created.service->instances(), joint.id, transaction, lifecycle_action::deactivate, joint.generation);
  ASSERT_TRUE(stopped) << stopped.detail;
  EXPECT_FALSE(created.service->interference_cache()->acquire(0, nof_prbs, source_slot + 1, monotonic_now_ns()));
  simulator.push_bsr(ul_bsr_indication_message{to_du_cell_index(0),
                                               ue.ue_index,
                                               ue.crnti,
                                               bsr_format::SHORT_BSR,
                                               {ul_bsr_lcg_report{uint_to_lcg_id(2), 100000}}});
  EXPECT_NE(run_until_pusch(simulator, ue), nullptr);
  auto unloaded = control(
      created.service->instances(), joint.id, transaction, lifecycle_action::unload, stopped.instance_generation);
  ASSERT_TRUE(unloaded) << unloaded.detail;
}
#endif

} // namespace

TEST(dapp_composition, independent_fixture_spectrum_output_controls_only_a_later_new_ul_grant)
{
  const std::filesystem::path catalog = DAPP_TEST_CATALOG;
  fixture_control             spectrum_control;
  fixture_control             scheduler_control;
  spectrum_control.publish_interference.store(true, std::memory_order_relaxed);
  scheduler_control.publish_prb_avoidance.store(true, std::memory_order_relaxed);
  temporary_manifest spectrum_manifest(catalog,
                                       fixture_manifest(catalog,
                                                        DAPP_CLASS_C_FIXTURE,
                                                        "00112233-4455-6677-8899-aabbccddef01",
                                                        "org.deepsig.ocudu.test.class-c",
                                                        "C",
                                                        OCUDU_DAPP_INTERFACE_SPECTRUM_V1,
                                                        OCUDU_DAPP_SPECTRUM_CAP_INTERFERENCE_MAP_V1 |
                                                            OCUDU_DAPP_SPECTRUM_CAP_QUIET_PERIOD_INTENT_V1 |
                                                            OCUDU_DAPP_SPECTRUM_CAP_FUTURE_UL_PLAN_V1,
                                                        true));
  temporary_manifest scheduler_manifest(catalog,
                                        fixture_manifest(catalog,
                                                         DAPP_CLASS_B_FIXTURE,
                                                         "00112233-4455-6677-8899-aabbccddef02",
                                                         "org.deepsig.ocudu.test.class-b",
                                                         "B",
                                                         OCUDU_DAPP_INTERFACE_SCHEDULER_V1,
                                                         0));
  run_composition({catalog,
                   spectrum_manifest.filename,
                   scheduler_manifest.filename,
                   "org.deepsig.ocudu.test.class-c",
                   "org.deepsig.ocudu.test.class-b",
                   spectrum_placement::native,
                   &spectrum_control,
                   &scheduler_control,
                   vrb_interval{7, 8}});
  run_composition({catalog,
                   spectrum_manifest.filename,
                   scheduler_manifest.filename,
                   "org.deepsig.ocudu.test.class-c",
                   "org.deepsig.ocudu.test.class-b",
                   spectrum_placement::native,
                   &spectrum_control,
                   &scheduler_control,
                   vrb_interval{7, 8}},
                  true);
  EXPECT_GT(spectrum_control.data_invocations.load(std::memory_order_relaxed), 0U);
  EXPECT_EQ(spectrum_control.advance_plan_calls.load(std::memory_order_relaxed), 4U);
  EXPECT_GT(spectrum_control.advance_plan_spans.load(std::memory_order_relaxed), 0U);
  EXPECT_GT(scheduler_control.data_invocations.load(std::memory_order_relaxed), 0U);
}

#ifdef DAPP_REFERENCE_BUILD_DIR
TEST(dapp_composition, standalone_native_spectrum_and_scheduler_artifacts_drive_a_later_new_ul_grant)
{
  const std::filesystem::path catalog = DAPP_REFERENCE_BUILD_DIR;
  run_composition({catalog,
                   "ref_spectrum_cpu.manifest.json",
                   "ref_scheduler_cpu.manifest.json",
                   "org.ocudu.reference.spectrum.cpu",
                   "org.ocudu.reference.scheduler.cpu",
                   spectrum_placement::native,
                   nullptr,
                   nullptr,
                   vrb_interval{0, 8},
                   true});
}

TEST(dapp_composition, standalone_process_spectrum_and_native_scheduler_artifacts_drive_a_later_new_ul_grant)
{
  const std::filesystem::path catalog = DAPP_REFERENCE_BUILD_DIR;
  run_composition({catalog,
                   "ref_spectrumd_cpu.manifest.json",
                   "ref_scheduler_cpu.manifest.json",
                   "org.ocudu.reference.spectrum.process.cpu",
                   "org.ocudu.reference.scheduler.cpu",
                   spectrum_placement::process,
                   nullptr,
                   nullptr,
                   vrb_interval{0, 8},
                   true});
}

TEST(dapp_composition, standalone_joint_spectrum_scheduler_artifact_binds_both_interfaces_atomically)
{
  const std::filesystem::path catalog = DAPP_REFERENCE_BUILD_DIR;
  run_joint_composition({catalog,
                         "test_joint_spectrum_scheduler_cpu.manifest.json",
                         "test_joint_spectrum_scheduler_cpu.manifest.json",
                         "org.ocudu.test.joint-spectrum-scheduler.cpu",
                         "org.ocudu.test.joint-spectrum-scheduler.cpu",
                         spectrum_placement::native,
                         nullptr,
                         nullptr,
                         vrb_interval{0, 8}});
}
#endif
