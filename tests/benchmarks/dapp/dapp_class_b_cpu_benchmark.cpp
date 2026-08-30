// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "tests/test_doubles/scheduler/cell_config_builder_profiles.h"
#include "ocudu/scheduler/rrm/pucch_resource_manager.h"
#include "tests/test_doubles/scheduler/scheduler_config_helper.h"
#include "tests/test_doubles/utils/test_rng_seed.h"
#include "tests/unittests/scheduler/test_utils/scheduler_test_simulator.h"
#include "ocudu/dapp/runtime/artifact_verifier.h"
#include "ocudu/dapp/runtime/module_loader.h"
#include "ocudu/dapp/runtime/native_hook_point.h"
#include "ocudu/dapp/runtime/native_instance.h"
#include "ocudu/scheduler/dapp/scheduler_dapp_context.h"
#include "ocudu/scheduler/dapp/scheduler_dapp_policy.h"
#include "ocudu_dapp_sdk/reference_scheduler_pipeline.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <getopt.h>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace ocudu;
using namespace ocudu::dapp;
using namespace std::chrono_literals;

namespace {

constexpr uint32_t class_b_deadline_us = 100;

struct options {
  std::filesystem::path output{"class-b-cpu-raw.csv"};
  uint32_t              repetitions{1000};
  uint32_t              warmups{100};
  uint32_t              deadline_us{class_b_deadline_us};
};

struct profile {
  const char*          name;
  uint32_t             direction;
  bs_channel_bandwidth bandwidth;
  uint32_t             expected_prbs;
  uint32_t             candidates;
  bool                 interference;
};

constexpr profile profiles[] = {
    {"dl_20mhz_c16", OCUDU_DAPP_SCHEDULER_DL_V1, bs_channel_bandwidth::MHz20, 51, 16, false},
    {"dl_100mhz_c1", OCUDU_DAPP_SCHEDULER_DL_V1, bs_channel_bandwidth::MHz100, 273, 1, false},
    {"dl_100mhz_c16", OCUDU_DAPP_SCHEDULER_DL_V1, bs_channel_bandwidth::MHz100, 273, 16, false},
    {"dl_100mhz_c64", OCUDU_DAPP_SCHEDULER_DL_V1, bs_channel_bandwidth::MHz100, 273, 64, false},
    {"ul_20mhz_c16_context", OCUDU_DAPP_SCHEDULER_UL_V1, bs_channel_bandwidth::MHz20, 51, 16, true},
    {"ul_100mhz_c1", OCUDU_DAPP_SCHEDULER_UL_V1, bs_channel_bandwidth::MHz100, 273, 1, false},
    {"ul_100mhz_c16", OCUDU_DAPP_SCHEDULER_UL_V1, bs_channel_bandwidth::MHz100, 273, 16, false},
    {"ul_100mhz_c64", OCUDU_DAPP_SCHEDULER_UL_V1, bs_channel_bandwidth::MHz100, 273, 64, false},
    {"ul_100mhz_c64_context", OCUDU_DAPP_SCHEDULER_UL_V1, bs_channel_bandwidth::MHz100, 273, 64, true},
};

[[noreturn]] void fail(std::string_view detail)
{
  fmt::print(stderr, "Class-B CPU benchmark failed: {}\n", detail);
  std::exit(1);
}

void require(bool condition, std::string_view detail)
{
  if (!condition) {
    fail(detail);
  }
}

uint32_t parse_count(const char* value, uint32_t maximum, const char* name)
{
  char*               end{};
  const unsigned long parsed = std::strtoul(value, &end, 10);
  if (end == value || *end != '\0' || parsed == 0 || parsed > maximum) {
    fail(fmt::format("{} must be in [1, {}]", name, maximum));
  }
  return static_cast<uint32_t>(parsed);
}

options parse_args(int argc, char** argv)
{
  options result;
  int     option;
  while ((option = ::getopt(argc, argv, "o:R:W:D:h")) != -1) {
    switch (option) {
      case 'o':
        result.output = optarg;
        break;
      case 'R':
        result.repetitions = parse_count(optarg, 1'000'000, "repetitions");
        break;
      case 'W':
        result.warmups = parse_count(optarg, 100'000, "warmups");
        break;
      case 'D':
        result.deadline_us = parse_count(optarg, 1'000'000, "deadline");
        break;
      default:
        fmt::print("Usage: {} [-o raw.csv] [-R repetitions] [-W warmups] [-D deadline-us]\n", argv[0]);
        std::exit(option == 'h' ? 0 : 1);
    }
  }
  require(!result.output.empty(), "output path is empty");
  return result;
}

uint64_t monotonic_ns(void*)
{
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

ocudu_dapp_radio_time_v1 to_radio_time(slot_point slot) noexcept
{
  return {static_cast<uint16_t>(slot.sfn()),
          static_cast<uint16_t>(slot.slot_index()),
          static_cast<uint16_t>(slot.numerology()),
          0};
}

class module_fixture
{
public:
  module_fixture()
  {
    const std::filesystem::path root{OCUDU_DAPP_REFERENCE_BUILD_DIR};
    artifact_path = root / "ref_scheduler_cpu.so";
    artifact_verifier verifier(root.string());
    auto              inspected = verifier.inspect(artifact_path.string());
    require(static_cast<bool>(inspected), inspected.detail);
    auto verified = verifier.verify(artifact_path.string(), inspected.digest);
    require(static_cast<bool>(verified), verified.detail);

    ocudu_dapp_host_info_v1 host_info{};
    host_info.struct_size = sizeof(host_info);
    host_info.abi_major   = OCUDU_DAPP_ABI_MAJOR_V1;
    host_info.abi_minor   = OCUDU_DAPP_ABI_MINOR_V1;
    auto loaded           = load_native_module(std::move(*verified.artifact), host_info);
    require(static_cast<bool>(loaded), loaded.detail);

    ocudu_dapp_host_api_v1 host_api{};
    host_api.struct_size  = sizeof(host_api);
    host_api.abi_major    = OCUDU_DAPP_ABI_MAJOR_V1;
    host_api.abi_minor    = OCUDU_DAPP_ABI_MINOR_V1;
    host_api.monotonic_ns = monotonic_ns;
    // The shipped scheduler remains baseline-equivalent by default. This
    // qualification explicitly opts into the one policy output whose
    // feature-to-commit timing it measures.
    reference::scheduler_reference_create_args_v1 create_args{};
    create_args.enable_prb_avoidance = 1;
    auto created = native_instance::create(std::move(loaded.library), host_api, &create_args);
    require(static_cast<bool>(created), created.operation.detail);
    instance = std::move(created.instance);
    require(static_cast<bool>(instance->prepare(nullptr, nullptr)), "module prepare");
    require(static_cast<bool>(instance->arm(nullptr)), "module arm");
    require(static_cast<bool>(instance->enter_shadow()), "module shadow entry");

    auto point_result = native_hook_point::create(OCUDU_DAPP_INTERFACE_SCHEDULER_V1, 0, 8);
    require(static_cast<bool>(point_result), point_result.detail);
    point = std::shared_ptr<native_hook_point>(std::move(point_result.point));
    require(point->activate(*instance) == OCUDU_DAPP_OK_V1, "scheduler hook activation");
    target = create_native_scheduler_dapp_target(point);
    require(target != nullptr && target->active(), "scheduler target activation");
  }

  ~module_fixture()
  {
    target.reset();
    if (point != nullptr) {
      static_cast<void>(point->deactivate(std::chrono::steady_clock::now() + 1s));
    }
    if (instance != nullptr) {
      static_cast<void>(instance->drain(std::chrono::steady_clock::now() + 1s));
      static_cast<void>(instance->unload());
    }
  }

  std::filesystem::path                  artifact_path;
  std::unique_ptr<native_instance>       instance;
  std::shared_ptr<native_hook_point>     point;
  std::shared_ptr<scheduler_dapp_target> target;
};

struct sample {
  const profile*                   scenario{};
  uint32_t                         index{};
  dapp::class_b_policy_measurement measurement;
};

class recording_target final : public scheduler_dapp_target
{
public:
  explicit recording_target(std::shared_ptr<scheduler_dapp_target> delegate_) : delegate(std::move(delegate_)) {}

  void configure(const profile& scenario_, uint32_t warmups_, uint32_t repetitions_)
  {
    scenario          = &scenario_;
    warmups           = warmups_;
    repetitions       = repetitions_;
    matched           = 0;
    measurement_calls = 0;
    observed.fill(0);
    rows.clear();
    rows.reserve(repetitions);
  }

  bool complete() const noexcept { return rows.size() == repetitions; }

  bool     active() const noexcept override { return delegate->active(); }
  uint32_t lane_count() const noexcept override { return delegate->lane_count(); }
  uint32_t worker_id(uint32_t lane) const noexcept override { return delegate->worker_id(lane); }

  native_invocation_result invoke(uint32_t                             lane,
                                  const ocudu_dapp_scheduler_input_v1& input,
                                  ocudu_dapp_scheduler_output_v1&      output) noexcept override
  {
    return delegate->invoke(lane, input, output);
  }

  void report_invalid_output(uint32_t lane, const ocudu_dapp_invocation_v1& invocation) noexcept override
  {
    delegate->report_invalid_output(lane, invocation);
  }

  void record_measurement(uint32_t lane, const class_b_policy_measurement& measurement) noexcept override
  {
    delegate->record_measurement(lane, measurement);
    ++measurement_calls;
    if (measurement.nof_candidates < observed.size()) {
      ++observed[measurement.nof_candidates];
    }
    const bool matched_invocation = scenario != nullptr && measurement.direction == scenario->direction &&
                                    measurement.nof_candidates == scenario->candidates &&
                                    measurement.cell_nof_prbs == scenario->expected_prbs &&
                                    measurement.interference_context_present == scenario->interference;
    if (!matched_invocation || complete()) {
      return;
    }
    const uint32_t current = matched++;
    if (current >= warmups) {
      rows.push_back({scenario, current - warmups, measurement});
    }
  }

  std::vector<sample> rows;

  std::string diagnostic() const
  {
    std::string result = fmt::format("measurement_calls={} candidate_counts=", measurement_calls);
    for (uint32_t count = 0; count != observed.size(); ++count) {
      if (observed[count] != 0) {
        result += fmt::format("{}:{};", count, observed[count]);
      }
    }
    return result;
  }

private:
  std::shared_ptr<scheduler_dapp_target> delegate;
  const profile*                         scenario{};
  uint32_t                               warmups{};
  uint32_t                               repetitions{};
  uint32_t                               matched{};
  std::array<uint64_t, 65>               observed{};
  uint64_t                               measurement_calls{};
};

void publish_interference(scheduler_dapp_interference_cache& cache,
                          scheduler_test_simulator&          simulator,
                          uint32_t                           nof_prbs,
                          uint64_t                           sequence)
{
  std::vector<uint8_t> mask(nof_prbs);
  std::fill_n(mask.begin(), std::min<uint32_t>(32, nof_prbs), uint8_t{255});
  const slot_point next = simulator.next_slot.without_hyper_sfn();

  scheduler_dapp_interference_update update{};
  update.context.struct_size             = sizeof(update.context);
  update.context.schema_id               = OCUDU_DAPP_SCHEDULER_CONTEXT_INTERFERENCE_MAP_V1;
  update.context.schema_version          = 1;
  update.context.producer_instance_id[0] = 0xb0;
  update.context.producer_generation     = 1;
  update.context.source_sequence         = sequence;
  update.context.cell_id                 = 0;
  update.context.confidence_q16          = 1U << 16U;
  update.context.created                 = to_radio_time(next);
  update.context.valid_from              = to_radio_time(next + 1);
  update.context.valid_until             = to_radio_time(next + 4096);
  update.prb_confidence                  = mask;
  update.publish_monotonic_ns            = monotonic_ns(nullptr);
  require(cache.publish(update) == scheduler_dapp_context_result::accepted, "interference publication");
}

void run_profile(const profile& scenario, const options& args, const std::shared_ptr<recording_target>& recorder)
{
  auto cache = create_scheduler_dapp_interference_cache(std::chrono::hours(1));
  require(cache != nullptr, "interference cache creation");
  auto hooks = create_scheduler_dapp_hook_provider(recorder,
                                                   cache,
                                                   {32},
                                                   std::chrono::microseconds(args.deadline_us),
                                                   {},
                                                   {},
                                                   scheduler_dapp_policy_mode::authoritative);
  require(hooks != nullptr, "scheduler hook provider creation");

  scheduler_test_sim_config config;
  config.sched_cfg.ue.max_pucchs_per_slot         = 120;
  config.sched_cfg.ue.max_ul_grants_per_slot      = 128;
  config.sched_cfg.ue.pre_policy_rr_ue_group_size = 64;
  config.max_scs                                  = subcarrier_spacing::kHz30;
  config.auto_uci                                 = true;
  config.auto_crc                                 = true;
  config.dapp_hooks                               = hooks;
  scheduler_test_simulator simulator(config);
  simulator.logger.set_level(ocudulog::basic_levels::warning);
  simulator.test_logger.set_level(ocudulog::basic_levels::warning);

  auto builder = cell_config_builder_profiles::create(duplex_mode::TDD, frequency_range::FR1, scenario.bandwidth);
  auto cell    = sched_config_helper::make_default_sched_cell_configuration_request(builder);
  pucch_resource_builder_params pucch_params{
      .res_set_size = 8, .nof_cell_res_set_configs = 2, .nof_cell_sr_resources = 80, .nof_cell_csi_resources = 80};
  auto& f1_params                   = pucch_params.f0_or_f1_params.emplace<pucch_f1_params>();
  f1_params.nof_cyc_shifts          = pucch_nof_cyclic_shifts::twelve;
  f1_params.occ_supported           = true;
  cell.ran.init_bwp.pucch.resources = pucch_params;
  simulator.add_cell(cell);
  require(simulator.cell_cfg().params.dl_cfg_common.init_dl_bwp.generic_params.crbs.length() == scenario.expected_prbs,
          "unexpected cell PRB width");

  // Upstream replaced the pucch_res_builder_test_helper test double with the
  // production pucch_resource_manager; same role, per-cell registration.
  pucch_resource_manager pucch_builder{MAX_PUCCH_PDUS_PER_SLOT};
  pucch_builder.add_cell(to_du_cell_index(0), simulator.cell_cfg().params);
  for (uint32_t index = 0; index != scenario.candidates; ++index) {
    auto ue =
        sched_config_helper::create_default_sched_ue_creation_request(simulator.cell_cfg().params, {LCID_MIN_DRB});
    ue.ue_index = to_du_ue_index(index);
    ue.crnti    = to_rnti(0x4601 + index);
    require(pucch_builder.alloc_resources(ue.cfg.cells.value()[0]), "PUCCH resource allocation");
    simulator.add_ue(ue, true);
  }

  const auto refresh_traffic = [&] {
    for (uint32_t index = 0; index != scenario.candidates; ++index) {
      const du_ue_index_t ue_index = to_du_ue_index(index);
      const rnti_t        crnti    = to_rnti(0x4601 + index);
      if (scenario.direction == OCUDU_DAPP_SCHEDULER_DL_V1) {
        simulator.push_dl_buffer_state(dl_buffer_state_indication_message{
            ue_index, LCID_MIN_DRB, std::numeric_limits<uint32_t>::max() / 2, simulator.next_slot_rx()});
      } else {
        simulator.push_bsr(ul_bsr_indication_message{to_du_cell_index(0),
                                                     ue_index,
                                                     crnti,
                                                     bsr_format::LONG_BSR,
                                                     {{uint_to_lcg_id(2), std::numeric_limits<uint32_t>::max() / 2}}});
      }
    }
  };
  refresh_traffic();

  if (scenario.interference) {
    publish_interference(*cache, simulator, scenario.expected_prbs, scenario.candidates);
  }
  recorder->configure(scenario, args.warmups, args.repetitions);
  const uint32_t maximum_slots = (args.warmups + args.repetitions) * 10 + 2000;
  for (uint32_t slot = 0; slot != maximum_slots && !recorder->complete(); ++slot) {
    if (slot != 0 && slot % 10 == 0) {
      refresh_traffic();
    }
    simulator.run_slot();
  }
  if (!recorder->complete()) {
    fmt::print(stderr, "Class-B profile incomplete: {} {}\n", scenario.name, recorder->diagnostic());
  }
  require(recorder->complete(), fmt::format("scenario {} did not produce its exact sample count", scenario.name));
  fmt::print(stderr, "Class-B profile complete: {} ({} samples)\n", scenario.name, recorder->rows.size());
}

const char* outcome_name(class_b_policy_outcome outcome) noexcept
{
  switch (outcome) {
    case class_b_policy_outcome::accepted:
      return "accepted";
    case class_b_policy_outcome::evaluation_accepted:
      return "evaluation_accepted";
    case class_b_policy_outcome::fallback:
      return "fallback";
    case class_b_policy_outcome::validation_rejection:
      return "validation_rejection";
  }
  return "unknown";
}

void write_header(std::ofstream& output)
{
  output << "schema,scenario,sample,direction,candidates,prbs,interference,avoidance_output,conventional_ns,"
            "feature_ns,dapp_ns,validation_ns,commit_ns,total_ns,outcome,deadline_miss\n";
}

void write_rows(std::ofstream& output, const std::vector<sample>& rows)
{
  for (const auto& row : rows) {
    const auto& measurement = row.measurement;
    output << "ocudu.dapp.class-b-cpu-sample.v1," << row.scenario->name << ',' << row.index << ','
           << (measurement.direction == OCUDU_DAPP_SCHEDULER_DL_V1 ? "dl" : "ul") << ',' << measurement.nof_candidates
           << ',' << measurement.cell_nof_prbs << ',' << measurement.interference_context_present << ','
           << measurement.avoidance_output << ',' << measurement.conventional_time_ns << ','
           << measurement.feature_time_ns << ',' << measurement.dapp_time_ns << ',' << measurement.validation_time_ns
           << ',' << measurement.commit_time_ns << ',' << measurement.total_time_ns << ','
           << outcome_name(measurement.outcome) << ',' << measurement.deadline_miss << '\n';
  }
}

} // namespace

int main(int argc, char** argv)
{
  const options args = parse_args(argc, argv);
  test_rng::init_base_seed(0xb0);
  ocudulog::init();
  module_fixture fixture;
  auto           recorder = std::make_shared<recording_target>(fixture.target);
  std::ofstream  output(args.output, std::ios::trunc);
  require(static_cast<bool>(output), "cannot open raw CSV");
  write_header(output);
  for (const auto& scenario : profiles) {
    run_profile(scenario, args, recorder);
    write_rows(output, recorder->rows);
  }
  output.close();
  require(static_cast<bool>(output), "cannot write raw CSV");
  return 0;
}
