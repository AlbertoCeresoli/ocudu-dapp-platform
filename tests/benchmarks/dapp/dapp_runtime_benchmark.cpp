// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "fixtures/fixture_common.h"
#include "ocudu/dapp/ipc/publication_route.h"
#include "ocudu/dapp/runtime/artifact_verifier.h"
#include "ocudu/dapp/runtime/class_c_process_supervisor.h"
#include "ocudu/dapp/runtime/module_loader.h"
#include "ocudu/dapp/runtime/native_hook_slot.h"
#include "ocudu/dapp/runtime/native_instance.h"
#include "ocudu/dapp/use_cases/v1/spectrum.h"
#include "ocudu/phy/upper/dapp/upper_phy_dapp_process_target.h"
#include "ocudu/scheduler/dapp/scheduler_dapp_context.h"
#include "ocudu/support/benchmark_utils.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <getopt.h>
#include <memory>
#include <string_view>
#include <sys/utsname.h>
#include <utility>
#include <vector>

using namespace ocudu;
using namespace ocudu::dapp;
using namespace ocudu::dapp::ipc;
using namespace std::chrono_literals;

namespace {

unsigned repetitions = 2000;

[[noreturn]] void fail(const char* detail)
{
  fmt::print(stderr, "dApp benchmark setup failed: {}\n", detail);
  std::exit(1);
}

void require(bool condition, const char* detail)
{
  if (!condition) {
    fail(detail);
  }
}

template <typename Callable>
void report_budget_misses(const char* label, std::chrono::nanoseconds budget, unsigned count, Callable&& callable)
{
  unsigned misses = 0;
  for (unsigned index = 0; index != count; ++index) {
    const auto start = std::chrono::steady_clock::now();
    callable();
    misses += std::chrono::steady_clock::now() - start > budget ? 1U : 0U;
  }
  fmt::print("Budget check: path={} budget_ns={} samples={} misses={} miss_rate={:.6f}\n",
             label,
             budget.count(),
             count,
             misses,
             static_cast<double>(misses) / count);
}

void parse_args(int argc, char** argv)
{
  int option = 0;
  while ((option = ::getopt(argc, argv, "R:h")) != -1) {
    if (option == 'R') {
      const auto value = std::strtoul(optarg, nullptr, 10);
      if (value == 0 || value > 1'000'000) {
        fail("repetitions must be in [1, 1000000]");
      }
      repetitions = value;
    } else {
      fmt::print("Usage: {} [-R repetitions]\n", argv[0]);
      std::exit(option == 'h' ? 0 : 1);
    }
  }
}

uint64_t monotonic_ns(void*)
{
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

std::unique_ptr<approved_artifact> admit(const char* path)
{
  artifact_verifier verifier(DAPP_TEST_CATALOG);
  auto              inspected = verifier.inspect(path);
  require(static_cast<bool>(inspected), "artifact inspection");
  auto verified = verifier.verify(path, inspected.digest);
  require(static_cast<bool>(verified), "artifact verification");
  return std::move(verified.artifact);
}

std::shared_ptr<native_module_library> load_module(const char* path)
{
  ocudu_dapp_host_info_v1 host{};
  host.struct_size = sizeof(host);
  host.abi_major   = OCUDU_DAPP_ABI_MAJOR_V1;
  host.abi_minor   = OCUDU_DAPP_ABI_MINOR_V1;
  auto artifact    = admit(path);
  auto loaded      = load_native_module(std::move(*artifact), host);
  require(static_cast<bool>(loaded), "native fixture load");
  return std::move(loaded.library);
}

struct spectrum_fixture {
  explicit spectrum_fixture(uint32_t ports, uint32_t symbols, uint32_t subcarriers, uint32_t element_type) :
    bytes(static_cast<std::size_t>(ports) * symbols * subcarriers *
              (element_type == OCUDU_DAPP_ELEMENT_CBF16_V1 ? 4 : 8),
          0x5a)
  {
    metadata.struct_size     = sizeof(metadata);
    metadata.abi_major       = OCUDU_DAPP_ABI_MAJOR_V1;
    metadata.abi_minor       = OCUDU_DAPP_ABI_MINOR_V1;
    metadata.bandwidth_hz    = 100'000'000;
    metadata.rx_ports        = ports;
    metadata.symbols         = symbols;
    metadata.subcarriers     = subcarriers;
    metadata.amplitude_scale = 1.0F;

    input.invocation.struct_size                = sizeof(input.invocation);
    input.invocation.abi_major                  = OCUDU_DAPP_ABI_MAJOR_V1;
    input.invocation.abi_minor                  = OCUDU_DAPP_ABI_MINOR_V1;
    input.invocation.invocation_id              = 1;
    input.invocation.worker_id                  = 0;
    input.invocation.radio_time.numerology      = 1;
    input.invocation.input_generation           = 1;
    input.source_sequence                       = 1;
    input.slot_metadata.struct_size             = sizeof(input.slot_metadata);
    input.slot_metadata.abi_major               = OCUDU_DAPP_ABI_MAJOR_V1;
    input.slot_metadata.abi_minor               = OCUDU_DAPP_ABI_MINOR_V1;
    input.slot_metadata.metadata_schema_id      = OCUDU_DAPP_METADATA_SPECTRUM_SLOT_V1;
    input.slot_metadata.metadata_schema_version = 1;
    input.slot_metadata.metadata                = {&metadata, sizeof(metadata), 0};

    auto& tensor                 = input.uplink_resource_grid;
    tensor.struct_size           = sizeof(tensor);
    tensor.abi_major             = OCUDU_DAPP_ABI_MAJOR_V1;
    tensor.abi_minor             = OCUDU_DAPP_ABI_MINOR_V1;
    tensor.domain                = OCUDU_DAPP_MEM_HOST_V1;
    tensor.backend_id            = OCUDU_DAPP_BACKEND_HOST_V1;
    tensor.element_type          = element_type;
    tensor.rank                  = 3;
    tensor.access                = OCUDU_DAPP_ACCESS_READ_V1 | OCUDU_DAPP_ACCESS_HOST_VISIBLE_V1;
    tensor.allocation_id         = 1;
    tensor.allocation_generation = 1;
    tensor.native_handle         = reinterpret_cast<uintptr_t>(bytes.data());
    tensor.byte_length           = bytes.size();
    tensor.dimensions[0]         = ports;
    tensor.dimensions[1]         = symbols;
    tensor.dimensions[2]         = subcarriers;
    tensor.byte_strides[2]       = element_type == OCUDU_DAPP_ELEMENT_CBF16_V1 ? 4 : 8;
    tensor.byte_strides[1]       = subcarriers * tensor.byte_strides[2];
    tensor.byte_strides[0]       = symbols * tensor.byte_strides[1];
    tensor.layout_id             = OCUDU_DAPP_LAYOUT_RESOURCE_GRID_PORT_SYMBOL_SUBCARRIER_V1;
    tensor.layout_version        = 1;
    tensor.lease_token           = 1;
  }

  void next() noexcept
  {
    ++input.source_sequence;
    input.invocation.invocation_id                   = input.source_sequence;
    input.invocation.input_generation                = input.source_sequence;
    input.uplink_resource_grid.allocation_generation = input.source_sequence;
  }

  std::vector<uint8_t>                 bytes;
  ocudu_dapp_spectrum_slot_metadata_v1 metadata{};
  ocudu_dapp_spectrum_input_v1         input{};
};

void benchmark_native_hooks()
{
  fixture_control        control;
  ocudu_dapp_host_api_v1 host{};
  host.struct_size  = sizeof(host);
  host.abi_major    = OCUDU_DAPP_ABI_MAJOR_V1;
  host.abi_minor    = OCUDU_DAPP_ABI_MINOR_V1;
  host.monotonic_ns = monotonic_ns;
  auto created      = native_instance::create(load_module(DAPP_CLASS_C_FIXTURE), host, &control);
  require(static_cast<bool>(created), "native instance create");
  require(static_cast<bool>(created.instance->prepare(nullptr, nullptr)), "native prepare");
  require(static_cast<bool>(created.instance->arm(nullptr)), "native arm");
  require(static_cast<bool>(created.instance->enter_shadow()), "native shadow");
  auto worker = created.instance->create_worker(OCUDU_DAPP_INTERFACE_SPECTRUM_V1, 0);
  require(static_cast<bool>(worker), "native worker create");

  spectrum_fixture data(1, 1, 12, OCUDU_DAPP_ELEMENT_CF32_V1);
  native_hook_slot unbound(OCUDU_DAPP_INTERFACE_SPECTRUM_V1);
  native_hook_slot bound(OCUDU_DAPP_INTERFACE_SPECTRUM_V1);
  auto             gate = std::make_shared<native_activation_gate>();
  require(bound.bind(std::move(worker.worker), gate) == OCUDU_DAPP_OK_V1, "native hook bind");
  benchmarker              hooks("dApp native hook dispatch", repetitions);
  native_invocation_result result;
  hooks.new_measure("unbound bypass", 1, [&] {
    result = unbound.publish(data.input);
    do_not_optimize(result.status);
  });
  hooks.new_measure("bound disabled bypass", 1, [&] {
    result = bound.publish(data.input);
    do_not_optimize(result.status);
  });
  gate->enabled.store(true, std::memory_order_release);
  hooks.new_measure(
      "active native Class-C callback",
      1,
      [&] {
        result = bound.publish(data.input);
        do_not_optimize(result.status);
      },
      [&] { data.next(); });
  hooks.print_percentiles_time();

  gate->enabled.store(false, std::memory_order_release);
  require(bound.clear(std::chrono::steady_clock::now() + 1s) == OCUDU_DAPP_OK_V1, "native hook clear");
  require(static_cast<bool>(created.instance->drain(std::chrono::steady_clock::now() + 1s)), "native drain");
  require(static_cast<bool>(created.instance->unload()), "native unload");
}

void benchmark_inline_hook()
{
  fixture_control        control;
  ocudu_dapp_host_api_v1 host{};
  host.struct_size  = sizeof(host);
  host.abi_major    = OCUDU_DAPP_ABI_MAJOR_V1;
  host.abi_minor    = OCUDU_DAPP_ABI_MINOR_V1;
  host.monotonic_ns = monotonic_ns;
  auto created      = native_instance::create(load_module(DAPP_CLASS_A_FIXTURE), host, &control);
  require(static_cast<bool>(created), "Class-A instance create");
  require(static_cast<bool>(created.instance->prepare(nullptr, nullptr)), "Class-A prepare");
  require(static_cast<bool>(created.instance->arm(nullptr)), "Class-A arm");
  require(static_cast<bool>(created.instance->enter_shadow()), "Class-A shadow");
  auto worker = created.instance->create_worker(OCUDU_DAPP_INTERFACE_CHANNEL_ESTIMATOR_V1, 0);
  require(static_cast<bool>(worker), "Class-A worker create");
  native_hook_slot slot(OCUDU_DAPP_INTERFACE_CHANNEL_ESTIMATOR_V1);
  auto             gate = std::make_shared<native_activation_gate>();
  require(slot.bind(std::move(worker.worker), gate) == OCUDU_DAPP_OK_V1, "Class-A hook bind");
  gate->enabled.store(true, std::memory_order_release);

  struct complex32 {
    float real;
    float imag;
  };
  std::array<complex32, 4>  storage{};
  ocudu_dapp_tensor_view_v1 tensor{};
  tensor.struct_size           = sizeof(tensor);
  tensor.abi_major             = OCUDU_DAPP_ABI_MAJOR_V1;
  tensor.abi_minor             = OCUDU_DAPP_ABI_MINOR_V1;
  tensor.domain                = OCUDU_DAPP_MEM_HOST_V1;
  tensor.backend_id            = OCUDU_DAPP_BACKEND_HOST_V1;
  tensor.element_type          = OCUDU_DAPP_ELEMENT_CF32_V1;
  tensor.rank                  = 2;
  tensor.access                = OCUDU_DAPP_ACCESS_READ_V1 | OCUDU_DAPP_ACCESS_HOST_VISIBLE_V1;
  tensor.allocation_id         = 1;
  tensor.allocation_generation = 1;
  tensor.native_handle         = reinterpret_cast<uintptr_t>(storage.data());
  tensor.byte_length           = sizeof(storage);
  tensor.dimensions[0]         = 2;
  tensor.dimensions[1]         = 2;
  tensor.byte_strides[0]       = 2 * sizeof(complex32);
  tensor.byte_strides[1]       = sizeof(complex32);
  tensor.lease_token           = 1;
  ocudu_dapp_channel_estimator_input_v1 input{};
  input.invocation.struct_size = sizeof(input.invocation);
  input.invocation.abi_major   = OCUDU_DAPP_ABI_MAJOR_V1;
  input.invocation.abi_minor   = OCUDU_DAPP_ABI_MINOR_V1;
  input.invocation.worker_id   = 0;
  input.rx_grid                = tensor;
  ocudu_dapp_pusch_metadata_v1 pusch{};
  pusch.struct_size     = sizeof(pusch);
  pusch.abi_major       = OCUDU_DAPP_ABI_MAJOR_V1;
  pusch.nof_rx_ports    = 1;
  pusch.nof_layers      = 1;
  pusch.nof_symbols     = 1;
  pusch.nof_subcarriers = 1;
  ocudu_dapp_dmrs_metadata_v1 dmrs{};
  dmrs.struct_size = sizeof(dmrs);
  dmrs.abi_major   = OCUDU_DAPP_ABI_MAJOR_V1;
  dmrs.nof_pilots  = 1;
  dmrs.flags =
      OCUDU_DAPP_DMRS_REFERENCE_SYMBOLS_LAYER_COMPLETE_V1 | OCUDU_DAPP_DMRS_COORDINATES_SHARED_ACROSS_LAYERS_V1;
  dmrs.dmrs_type                      = 1;
  input.pusch.struct_size             = sizeof(input.pusch);
  input.pusch.abi_major               = OCUDU_DAPP_ABI_MAJOR_V1;
  input.pusch.metadata_schema_id      = OCUDU_DAPP_METADATA_PUSCH_V1;
  input.pusch.metadata_schema_version = 1;
  input.pusch.metadata                = {&pusch, sizeof(pusch), 0};
  input.dmrs.struct_size              = sizeof(input.dmrs);
  input.dmrs.abi_major                = OCUDU_DAPP_ABI_MAJOR_V1;
  input.dmrs.metadata_schema_id       = OCUDU_DAPP_METADATA_DMRS_V1;
  input.dmrs.metadata_schema_version  = 1;
  input.dmrs.metadata                 = {&dmrs, sizeof(dmrs), 0};
  ocudu_dapp_channel_estimator_output_v1 output{};
  output.channel_estimates            = tensor;
  output.channel_estimates.access     = OCUDU_DAPP_ACCESS_WRITE_V1 | OCUDU_DAPP_ACCESS_HOST_VISIBLE_V1;
  output.noise_variances              = output.channel_estimates;
  output.noise_variances.element_type = OCUDU_DAPP_ELEMENT_F32_V1;

  benchmarker              inline_hook("dApp Class-A inline hook", repetitions);
  native_invocation_result result;
  inline_hook.new_measure("CE fixture dispatch+validation", 1, [&] {
    result = slot.invoke(input, output);
    do_not_optimize(result.status);
  });
  require(static_cast<bool>(result), "Class-A invocation");
  inline_hook.print_percentiles_time();
  report_budget_misses("class_a_hook_overhead", 100us, repetitions, [&] {
    result = slot.invoke(input, output);
    do_not_optimize(result.status);
  });

  gate->enabled.store(false, std::memory_order_release);
  require(slot.clear(std::chrono::steady_clock::now() + 1s) == OCUDU_DAPP_OK_V1, "Class-A hook clear");
  require(static_cast<bool>(created.instance->drain(std::chrono::steady_clock::now() + 1s)), "Class-A drain");
  require(static_cast<bool>(created.instance->unload()), "Class-A unload");
}

void benchmark_process_snapshot()
{
  spectrum_fixture                     data(4, 14, 3276, OCUDU_DAPP_ELEMENT_CBF16_V1);
  upper_phy_dapp_process_target_config config{};
  config.interface_id              = OCUDU_DAPP_INTERFACE_SPECTRUM_V1;
  config.slot_count                = 2;
  config.payload_capacity          = static_cast<uint32_t>(data.bytes.size() + 64 * 1024);
  config.encoder.producer_instance = {1, 2};
  config.encoder.ran_node          = {3, 4};
  auto created                     = create_upper_phy_dapp_process_target(std::move(config));
  require(static_cast<bool>(created), "process target create");
  auto capabilities = created.target->export_capabilities();
  auto consumer     = publication_consumer::attach(
      capabilities.ring.value(), capabilities.notification.value(), capabilities.completion.value());
  require(static_cast<bool>(consumer), "process consumer attach");
  created.target->set_active(true);

  benchmarker snapshot("dApp Class-C CPU process snapshot", repetitions);
  snapshot.new_measure(
      "4Rx full-slot grid commit",
      data.bytes.size(),
      [&] { created.target->publish(data.input); },
      [&] {
        consumer.consumer->drain_notifications();
        auto lease = consumer.consumer->try_acquire();
        require(static_cast<bool>(lease), "process snapshot completion");
        data.next();
      });
  snapshot.print_percentiles_time();
  snapshot.print_percentiles_throughput("bytes");
  const auto metrics = created.target->snapshot_metrics();
  require(metrics.publications == repetitions, "process publication count");
  require(metrics.snapshot_copy_operations == repetitions, "process copy count");
  require(metrics.snapshot_copy_bytes == static_cast<uint64_t>(repetitions) * data.bytes.size(), "process copy bytes");
  fmt::print("Copy profile: reason=snapshot count={} tensor_bytes={} payload_bytes={} copies/publication=1\n",
             metrics.snapshot_copy_operations,
             metrics.snapshot_copy_bytes,
             metrics.payload_bytes);
}

void benchmark_context_cache()
{
  auto cache = create_scheduler_dapp_interference_cache(1s);
  require(cache != nullptr, "interference cache create");
  std::array<uint8_t, 275>           confidence{};
  scheduler_dapp_interference_update update{};
  update.context.struct_size             = sizeof(update.context);
  update.context.schema_id               = OCUDU_DAPP_SCHEDULER_CONTEXT_INTERFERENCE_MAP_V1;
  update.context.schema_version          = 1;
  update.context.producer_instance_id[0] = 1;
  update.context.producer_generation     = 1;
  update.context.source_sequence         = 1;
  update.context.confidence_q16          = 1U << 16U;
  update.context.created                 = {0, 1, 0, 0};
  update.context.valid_from              = {0, 2, 0, 0};
  update.context.valid_until             = {0, 9, 0, 0};
  update.prb_confidence                  = confidence;
  update.publish_monotonic_ns            = 1;
  require(cache->publish(update) == scheduler_dapp_context_result::accepted, "interference cache publish");

  benchmarker                       acquisition("dApp scheduler context cache", repetitions);
  scheduler_dapp_interference_lease lease;
  bool                              acquired = false;
  acquisition.new_measure("latest context acquire+release", 1, [&] {
    lease    = cache->acquire(0, confidence.size(), slot_point{0, 3}, 2);
    acquired = static_cast<bool>(lease);
    do_not_optimize(acquired);
    lease = {};
  });
  require(acquired, "interference cache acquire");
  acquisition.print_percentiles_time();
  report_budget_misses("class_b_context_acquire_release", 100us, repetitions, [&] {
    lease    = cache->acquire(0, confidence.size(), slot_point{0, 3}, 2);
    acquired = static_cast<bool>(lease);
    do_not_optimize(acquired);
    lease = {};
  });
}

class benchmark_endpoint final : public class_c_process_endpoint
{
public:
  explicit benchmark_endpoint(std::unique_ptr<publication_producer> producer_) : producer(std::move(producer_)) {}
  uint32_t           interface_id() const noexcept override { return OCUDU_DAPP_INTERFACE_SPECTRUM_V1; }
  route_capabilities export_capabilities() const noexcept override { return producer->export_capabilities(); }
  void               set_active(bool) noexcept override {}
  bool reset_after_consumer_restart() noexcept override { return producer->reset_after_consumer_restart(); }

private:
  std::unique_ptr<publication_producer> producer;
};

void benchmark_management_round_trip()
{
  auto producer = publication_producer::create(2, 64);
  require(static_cast<bool>(producer), "management route create");
  auto                              endpoint = std::make_shared<benchmark_endpoint>(std::move(producer.producer));
  class_c_process_supervisor_config config;
  config.executable         = admit(DAPP_CLASS_C_PROCESS_WORKER);
  config.startup_timeout    = 1s;
  config.stop_timeout       = 1s;
  config.heartbeat_period   = 1s;
  config.heartbeat_timeout  = 3s;
  config.management_timeout = 1s;
  ocudu_dapp_route_offer_v1 offer{};
  offer.struct_size             = sizeof(offer);
  offer.interface_id            = OCUDU_DAPP_INTERFACE_SPECTRUM_V1;
  offer.metadata_schema_id      = 17;
  offer.metadata_schema_version = 1;
  offer.route_id                = 1;
  offer.producer_shard_count    = 1;
  config.routes.push_back({offer, endpoint});
  auto created = class_c_process_supervisor::create(std::move(config));
  require(static_cast<bool>(created), "management supervisor create");
  require(static_cast<bool>(created.supervisor->start()), "management worker start");

  const auto                               management_repetitions = std::max(100U, repetitions / 10U);
  benchmarker                              round_trip("dApp Class-C management forwarding", management_repetitions);
  ocudu_dapp_process_management_request_v1 request{};
  request.struct_size = sizeof(request);
  request.operation   = OCUDU_DAPP_PROCESS_MANAGEMENT_GET_CONFIG_V1;
  class_c_process_management_result result;
  round_trip.new_measure("authenticated get-config round trip", 1, [&] {
    result = created.supervisor->manage(request);
    do_not_optimize(result.response.resulting_generation);
  });
  require(static_cast<bool>(result), "management round trip");
  round_trip.print_percentiles_time();
  require(static_cast<bool>(created.supervisor->stop()), "management worker stop");
}

void print_environment()
{
  utsname value{};
  require(::uname(&value) == 0, "uname");
#ifdef NDEBUG
  constexpr std::string_view build = "release-like";
#else
  constexpr std::string_view build = "debug";
#endif
  fmt::print("Benchmark host: node={} system={} release={} machine={} compiler={} build={} repetitions={}\n",
             value.nodename,
             value.sysname,
             value.release,
             value.machine,
             __VERSION__,
             build,
             repetitions);
  fmt::print("Timing results are invalid under sanitizers, emulation, frequency instability, or competing load.\n");
}

} // namespace

int main(int argc, char** argv)
{
  parse_args(argc, argv);
  print_environment();
  benchmark_native_hooks();
  benchmark_inline_hook();
  benchmark_context_cache();
  benchmark_process_snapshot();
  benchmark_management_round_trip();
  return 0;
}
