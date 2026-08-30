// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "apps/services/dapp/dapp_service.h"
#include "support/pusch_test_grid.h"
#include "support/resource_grid_test_doubles.h"
#include "ocudu/dapp/abi/v1/metric_catalog.h"
#include "ocudu/dapp/runtime/instance_manager.h"
#include "ocudu/phy/generic_functions/generic_functions_factories.h"
#include "ocudu/phy/generic_functions/transform_precoding/transform_precoding_factories.h"
#include "ocudu/phy/upper/channel_modulation/channel_modulation_factories.h"
#include "ocudu/phy/upper/channel_processors/pusch/factories.h"
#include "ocudu/phy/upper/channel_processors/pusch/pusch_codeword_buffer.h"
#include "ocudu/phy/upper/channel_processors/pusch/pusch_demodulator_notifier.h"
#include "ocudu/phy/upper/dapp/upper_phy_dapp_hook_provider.h"
#include "ocudu/phy/upper/equalization/equalization_factories.h"
#include "ocudu/phy/upper/sequence_generators/sequence_generator_factories.h"
#include "ocudu/support/versioning/build_info.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cuda_runtime.h>
#ifdef OCUDU_DAPP_HAS_NVML
#include <nvml.h>
#endif
#include <filesystem>
#include <fstream>
#include <getopt.h>
#include <limits>
#include <numeric>
#include <sched.h>
#include <set>
#include <sstream>
#include <string>
#include <sys/utsname.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace ocudu;
using namespace std::chrono_literals;

namespace {

unsigned                       repetitions                   = 1000;
unsigned                       warmups                       = 100;
unsigned                       concurrent_lanes              = 1;
unsigned                       nof_symbols                   = 12;
unsigned                       preflight_timeout_ms          = 500;
uint64_t                       channel_estimator_deadline_ns = 100'000;
uint64_t                       demodulator_deadline_ns       = 150'000;
bool                           enforce_p999;
bool                           allow_shared_gpu;
std::string                    csv_path;
std::string                    sample_csv_path;
std::string                    package_filter;
std::vector<unsigned>          prb_values           = {25, 106, 273};
std::vector<unsigned>          layer_values         = {1, 2};
std::vector<modulation_scheme> receiver_modulations = {modulation_scheme::QPSK,
                                                       modulation_scheme::QAM16,
                                                       modulation_scheme::QAM64,
                                                       modulation_scheme::QAM256};
const auto                     trace_epoch          = std::chrono::steady_clock::now();

void require(bool condition, const std::string& detail);

uint64_t trace_offset_ns(std::chrono::steady_clock::time_point value)
{
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(value - trace_epoch).count());
}

struct execution_environment {
  std::string host;
  std::string machine;
  std::string gpu;
  std::string build_hash;
  std::string build_mode;
  std::string scheduler;
  int         scheduler_priority{};
  std::string cpu_affinity;
  std::string stream_priority;
  int         cuda_device{};
  int         cuda_driver_version{};
  int         cuda_runtime_version{};
  std::string gpu_pci_bus_id;
};

const char* scheduler_name(int policy) noexcept
{
  switch (policy) {
    case SCHED_FIFO:
      return "fifo";
    case SCHED_RR:
      return "round-robin";
    case SCHED_BATCH:
      return "batch";
    case SCHED_IDLE:
      return "idle";
    default:
      return "other";
  }
}

std::string format_cpu_affinity()
{
  cpu_set_t cpus;
  CPU_ZERO(&cpus);
  require(::sched_getaffinity(0, sizeof(cpus), &cpus) == 0, "sched_getaffinity");
  std::ostringstream output;
  bool               first = true;
  for (int cpu = 0; cpu != CPU_SETSIZE;) {
    if (!CPU_ISSET(cpu, &cpus)) {
      ++cpu;
      continue;
    }
    const int begin = cpu;
    while (cpu + 1 != CPU_SETSIZE && CPU_ISSET(cpu + 1, &cpus)) {
      ++cpu;
    }
    if (!first) {
      output << ';';
    }
    output << begin;
    if (cpu != begin) {
      output << '-' << cpu;
    }
    first = false;
    ++cpu;
  }
  return output.str();
}

execution_environment describe_environment(const utsname& host, const cudaDeviceProp& gpu, int cuda_device)
{
  sched_param parameters{};
  const int   policy = ::sched_getscheduler(0);
  require(policy >= 0 && ::sched_getparam(0, &parameters) == 0, "scheduler policy");
  int cuda_driver_version{};
  int cuda_runtime_version{};
  require(cudaDriverGetVersion(&cuda_driver_version) == cudaSuccess, "CUDA driver version");
  require(cudaRuntimeGetVersion(&cuda_runtime_version) == cudaSuccess, "CUDA runtime version");
  char pci_bus_id[32]{};
  require(cudaDeviceGetPCIBusId(pci_bus_id, sizeof(pci_bus_id), cuda_device) == cudaSuccess, "CUDA PCI bus ID");
  const char* stream_priority = std::getenv("OCUDU_PUSCH_CUDA_STREAM_PRIORITY");
  if (stream_priority == nullptr || *stream_priority == '\0') {
    stream_priority = std::getenv("OCUDU_UPPER_PHY_CUDA_STREAM_PRIORITY");
  }
  return {host.nodename,
          host.machine,
          gpu.name,
          get_build_hash(),
          get_build_mode(),
          scheduler_name(policy),
          parameters.sched_priority,
          format_cpu_affinity(),
          stream_priority != nullptr && *stream_priority != '\0' ? stream_priority : "normal",
          cuda_device,
          cuda_driver_version,
          cuda_runtime_version,
          pci_bus_id};
}

class observed_unsigned_range
{
public:
  void add(unsigned value)
  {
    minimum = std::min(minimum, value);
    maximum = std::max(maximum, value);
    valid   = true;
  }

  int64_t minimum_or_unknown() const { return valid ? static_cast<int64_t>(minimum) : int64_t{-1}; }
  int64_t maximum_or_unknown() const { return valid ? static_cast<int64_t>(maximum) : int64_t{-1}; }

private:
  unsigned minimum = std::numeric_limits<unsigned>::max();
  unsigned maximum{};
  bool     valid{};
};

class gpu_client_monitor
{
public:
  explicit gpu_client_monitor(int cuda_device)
  {
#ifdef OCUDU_DAPP_HAS_NVML
    char pci_bus_id[32]{};
    if (cudaDeviceGetPCIBusId(pci_bus_id, sizeof(pci_bus_id), cuda_device) != cudaSuccess ||
        nvmlInit_v2() != NVML_SUCCESS) {
      return;
    }
    initialized = true;
    if (nvmlDeviceGetHandleByPciBusId_v2(pci_bus_id, &device) != NVML_SUCCESS) {
      return;
    }
    available = true;
    read_static_properties();
    observe();
#else
    static_cast<void>(cuda_device);
#endif
  }

  ~gpu_client_monitor()
  {
#ifdef OCUDU_DAPP_HAS_NVML
    if (initialized) {
      static_cast<void>(nvmlShutdown());
    }
#endif
  }

  void observe()
  {
#ifdef OCUDU_DAPP_HAS_NVML
    if (!available) {
      return;
    }
    observe_telemetry();
    unsigned int count  = 0;
    nvmlReturn_t status = nvmlDeviceGetComputeRunningProcesses_v3(device, &count, nullptr);
    if (status == NVML_SUCCESS && count == 0) {
      update_health(true);
      return;
    }
    if (status != NVML_ERROR_INSUFFICIENT_SIZE || count == 0) {
      update_health(false);
      return;
    }
    std::vector<nvmlProcessInfo_t> processes(count + 8);
    for (unsigned attempt = 0; attempt != 3; ++attempt) {
      count  = processes.size();
      status = nvmlDeviceGetComputeRunningProcesses_v3(device, &count, processes.data());
      if (status == NVML_SUCCESS) {
        update_health(true);
        for (unsigned index = 0; index != count; ++index) {
          if (processes[index].pid != static_cast<unsigned int>(::getpid())) {
            foreign_pids.insert(processes[index].pid);
          }
        }
        return;
      }
      if (status != NVML_ERROR_INSUFFICIENT_SIZE || count <= processes.size()) {
        update_health(false);
        return;
      }
      processes.resize(count + 8);
    }
    update_health(false);
#endif
  }

  bool                          monitoring_available() const { return available; }
  bool                          observation_healthy() const { return healthy; }
  bool                          exclusive() const { return available && healthy && foreign_pids.empty(); }
  const std::set<unsigned int>& clients() const { return foreign_pids; }
  const std::string&            driver_version() const { return nvml_driver_version; }
  const std::string&            device_uuid() const { return uuid; }
  int64_t                       temperature_min_c() const { return temperature.minimum_or_unknown(); }
  int64_t                       temperature_max_c() const { return temperature.maximum_or_unknown(); }
  int64_t                       sm_clock_min_mhz() const { return sm_clock.minimum_or_unknown(); }
  int64_t                       sm_clock_max_mhz() const { return sm_clock.maximum_or_unknown(); }
  int64_t                       memory_clock_min_mhz() const { return memory_clock.minimum_or_unknown(); }
  int64_t                       memory_clock_max_mhz() const { return memory_clock.maximum_or_unknown(); }
  int64_t                       power_min_mw() const { return power.minimum_or_unknown(); }
  int64_t                       power_max_mw() const { return power.maximum_or_unknown(); }
  int64_t                       power_limit_mw() const { return enforced_power_limit; }
  const std::string&            persistence_mode() const { return persistence; }
  const std::string&            compute_mode() const { return compute; }
  int64_t                       performance_state_best() const { return performance_state.minimum_or_unknown(); }
  int64_t                       performance_state_worst() const { return performance_state.maximum_or_unknown(); }

private:
#ifdef OCUDU_DAPP_HAS_NVML
  void update_health(bool current)
  {
    healthy = observation_count == 0 ? current : healthy && current;
    ++observation_count;
  }

  void read_static_properties()
  {
    char buffer[NVML_SYSTEM_DRIVER_VERSION_BUFFER_SIZE]{};
    if (nvmlSystemGetDriverVersion(buffer, sizeof(buffer)) == NVML_SUCCESS) {
      nvml_driver_version = buffer;
    }
    char uuid_buffer[NVML_DEVICE_UUID_BUFFER_SIZE]{};
    if (nvmlDeviceGetUUID(device, uuid_buffer, sizeof(uuid_buffer)) == NVML_SUCCESS) {
      uuid = uuid_buffer;
    }
    unsigned int value{};
    if (nvmlDeviceGetEnforcedPowerLimit(device, &value) == NVML_SUCCESS &&
        value != std::numeric_limits<unsigned>::max()) {
      enforced_power_limit = value;
    }
    nvmlEnableState_t persistence_state{};
    if (nvmlDeviceGetPersistenceMode(device, &persistence_state) == NVML_SUCCESS) {
      persistence = persistence_state == NVML_FEATURE_ENABLED ? "enabled" : "disabled";
    }
    nvmlComputeMode_t compute_state{};
    if (nvmlDeviceGetComputeMode(device, &compute_state) == NVML_SUCCESS) {
      switch (compute_state) {
        case NVML_COMPUTEMODE_DEFAULT:
          compute = "default";
          break;
        case NVML_COMPUTEMODE_EXCLUSIVE_THREAD:
          compute = "exclusive-thread";
          break;
        case NVML_COMPUTEMODE_PROHIBITED:
          compute = "prohibited";
          break;
        case NVML_COMPUTEMODE_EXCLUSIVE_PROCESS:
          compute = "exclusive-process";
          break;
        default:
          compute = "unknown";
          break;
      }
    }
  }

  void observe_telemetry()
  {
    unsigned int value{};
    if (nvmlDeviceGetTemperature(device, NVML_TEMPERATURE_GPU, &value) == NVML_SUCCESS &&
        value != std::numeric_limits<unsigned>::max()) {
      temperature.add(value);
    }
    if (nvmlDeviceGetClockInfo(device, NVML_CLOCK_SM, &value) == NVML_SUCCESS &&
        value != std::numeric_limits<unsigned>::max()) {
      sm_clock.add(value);
    }
    if (nvmlDeviceGetClockInfo(device, NVML_CLOCK_MEM, &value) == NVML_SUCCESS &&
        value != std::numeric_limits<unsigned>::max()) {
      memory_clock.add(value);
    }
    if (nvmlDeviceGetPowerUsage(device, &value) == NVML_SUCCESS && value != std::numeric_limits<unsigned>::max()) {
      power.add(value);
    }
    nvmlPstates_t pstate{};
    if (nvmlDeviceGetPerformanceState(device, &pstate) == NVML_SUCCESS && pstate != NVML_PSTATE_UNKNOWN) {
      performance_state.add(static_cast<unsigned>(pstate));
    }
  }

  nvmlDevice_t device{};
#endif
  bool                    initialized{};
  bool                    available{};
  bool                    healthy{};
  unsigned                observation_count{};
  std::set<unsigned int>  foreign_pids;
  std::string             nvml_driver_version = "unavailable";
  std::string             uuid                = "unavailable";
  int64_t                 enforced_power_limit{-1};
  std::string             persistence = "unavailable";
  std::string             compute     = "unavailable";
  observed_unsigned_range temperature;
  observed_unsigned_range sm_clock;
  observed_unsigned_range memory_clock;
  observed_unsigned_range power;
  observed_unsigned_range performance_state;
};

[[noreturn]] void fail(const std::string& detail)
{
  fmt::print(stderr, "Class-A CUDA benchmark failed: {}\n", detail);
  std::exit(1);
}

void require(bool condition, const std::string& detail)
{
  if (!condition) {
    fail(detail);
  }
}

std::string format_client_pids(const gpu_client_monitor& monitor)
{
  std::ostringstream result;
  bool               first = true;
  for (unsigned int pid : monitor.clients()) {
    result << (first ? "" : ";") << pid;
    first = false;
  }
  return first ? "none" : result.str();
}

bool gpu_environment_failed(const gpu_client_monitor& monitor)
{
  return !monitor.monitoring_available() || !monitor.observation_healthy() ||
         (!allow_shared_gpu && !monitor.exclusive());
}

dapp::management::identifier transaction_id(uint64_t value)
{
  dapp::management::identifier result;
  std::memcpy(result.bytes.data() + result.bytes.size() - sizeof(value), &value, sizeof(value));
  return result;
}

class empty_results final : public dmrs_pusch_estimator_results
{
public:
  float                           get_noise_variance(unsigned) const override { return 0; }
  float                           get_rsrp(unsigned, unsigned = 0) const override { return 0; }
  static_vector<float, MAX_PORTS> get_rsrp_all_ports(unsigned = 0) const override { return {}; }
  float                           get_epre(unsigned) const override { return 0; }
  float                           get_snr(unsigned) const override { return 0; }
  float                           get_layer_average_snr(unsigned = 0) const override { return 0; }
  phy_time_unit                   get_time_alignment(unsigned) const override { return {}; }
  std::optional<float>            get_cfo_Hz(unsigned) const override { return std::nullopt; }
  void                            get_symbol_ch_estimate(span<cbf16_t>, unsigned, unsigned, unsigned) const override {}
  void                            get_symbol_ch_estimate(span<cbf16_t>,
                                                         unsigned,
                                                         unsigned,
                                                         unsigned,
                                                         const bounded_bitset<MAX_NOF_SUBCARRIERS>&) const override
  {
  }
  void get_channel_state_information(channel_state_information&) const override {}
};

class codeword_sink final : public pusch_codeword_buffer
{
public:
  span<log_likelihood_ratio> get_next_block_view(unsigned block_size) override
  {
    scratch.resize(block_size);
    return scratch;
  }
  void on_new_block(span<const log_likelihood_ratio>, const bit_buffer&) override {}
  void on_end_codeword() override {}

private:
  std::vector<log_likelihood_ratio> scratch;
};

class notifier_sink final : public pusch_demodulator_notifier
{
public:
  void on_provisional_stats(unsigned, const demodulation_stats&) override {}
  void on_end_stats(const demodulation_stats&) override {}
};

pusch_demodulator::configuration make_config(unsigned nof_prb, unsigned nof_layers)
{
  pusch_demodulator::configuration config{};
  config.cell_id = 0;
  config.rnti    = to_rnti(0x1234);
  config.rb_mask = crb_bitmap(nof_prb);
  for (unsigned rb = 0; rb != nof_prb; ++rb) {
    config.rb_mask.set(rb);
  }
  config.modulation         = modulation_scheme::QPSK;
  config.start_symbol_index = 2;
  config.nof_symbols        = nof_symbols;
  config.dmrs_symb_pos      = symbol_slot_mask(MAX_NSYMB_PER_SLOT);
  config.dmrs_symb_pos.set(2);
  config.dmrs_type                   = dmrs_config_type::type1;
  config.nof_cdm_groups_without_data = 1;
  config.n_id                        = 17;
  config.dmrs_scrambling_id          = 19;
  config.dmrs_scaling                = std::sqrt(2.0F);
  config.c_prefix                    = cyclic_prefix::NORMAL;
  config.nof_tx_layers               = nof_layers;
  config.slot                        = slot_point{1, 22};
  config.rx_ports.push_back(0);
  if (nof_layers == 2) {
    config.rx_ports.push_back(1);
  }
  return config;
}

void synchronize_last(pusch_demodulator& demodulator)
{
  const resident_softbit_buffer output = demodulator.get_resident_softbits();
  require(output.valid && output.completion_token != nullptr, "missing resident completion token");
  require(cudaEventSynchronize(static_cast<cudaEvent_t>(output.completion_token)) == cudaSuccess,
          "CUDA completion synchronization");
  demodulator.report_deferred_dapp_completion();
}

class benchmark_fixture
{
public:
  benchmark_fixture(unsigned nof_prb_, unsigned nof_layers_) : nof_prb(nof_prb_)
  {
    app_services::dapp_service_config service_config;
    service_config.catalog_root             = OCUDU_DAPP_REFERENCE_CUDA_DIR;
    service_config.native_preflight_helper  = OCUDU_DAPP_PREFLIGHT_HELPER;
    service_config.native_preflight_timeout = std::chrono::milliseconds(preflight_timeout_ms);
    // This local benchmark consumes freshly built, read-only test artifacts.
    // Production services remain fail-closed on detached package signatures.
    service_config.package_signatures.mode                 = dapp::package_signature_mode::allow_unsigned;
    service_config.package_signatures.minimum_policy_epoch = 0;
    service_config.package_manifests                       = {"ref_channel_estimator_cuda.manifest.json",
                                                              "ref_equalizer_cuda.manifest.json",
                                                              "ref_receiver_cuda.manifest.json"};
    service_config.cells                                   = {{0, 1, static_cast<uint16_t>(nof_prb)}};
    service_config.nof_pusch_lanes                         = concurrent_lanes;
    service_config.nof_srs_lanes                           = 1;
    service_config.nof_scheduler_lanes                     = 1;
    service_config.backend_mask =
        (uint64_t{1} << OCUDU_DAPP_BACKEND_HOST_V1) | (uint64_t{1} << OCUDU_DAPP_BACKEND_CUDA_V1);
    auto created = app_services::dapp_service::create(std::move(service_config));
    require(static_cast<bool>(created), created.detail);
    service = std::move(created.service);

    pusch_demodulator_factory_accelerated_configuration factory_config;
    factory_config.equalizer_factory = create_channel_equalizer_generic_factory();
    factory_config.precoder_factory  = create_dft_transform_precoder_factory(create_dft_processor_factory(), nof_prb);
    factory_config.demodulation_factory = create_demodulation_mapper_factory();
    factory_config.prg_factory          = create_pseudo_random_generator_sw_factory();
    factory_config.max_nof_prb          = nof_prb;
    auto fallback                       = create_pusch_demodulator_factory_accelerated(factory_config);
    require(static_cast<bool>(fallback), "accelerated PUSCH factory");
    auto decorated = service->upper_phy_hooks()->decorate_pusch_demodulator(
        fallback,
        {.nof_lanes                  = concurrent_lanes,
         .max_nof_prb                = nof_prb,
         .max_nof_rx_ports           = nof_layers_,
         .max_nof_layers             = nof_layers_,
         .channel_estimator_deadline = std::chrono::nanoseconds(channel_estimator_deadline_ns),
         .demodulator_deadline       = std::chrono::nanoseconds(demodulator_deadline_ns)});
    require(static_cast<bool>(decorated), "decorated PUSCH factory");
    lanes.reserve(concurrent_lanes);
    for (unsigned lane = 0; lane != concurrent_lanes; ++lane) {
      auto context         = std::make_unique<lane_context>(nof_layers_, nof_prb);
      context->demodulator = decorated->create();
      require(static_cast<bool>(context->demodulator), "accelerated PUSCH demodulator lane");
      context->demodulator->enable_resident_mode();
      context->demodulator->enable_resident_dapp_diagnostic_timing(!sample_csv_path.empty());
      test_support::populate_identity_pusch_grid(context->grid, context->config, nof_prb);
      lanes.push_back(std::move(context));
    }
  }

  void invoke(unsigned lane = 0)
  {
    auto& context = *lanes.at(lane);
    context.demodulator->demodulate(
        context.codeword, context.notifier, context.grid, context.estimates, context.config);
  }

  void set_modulation(modulation_scheme value)
  {
    for (auto& lane : lanes) {
      lane->config.modulation = value;
    }
  }

  unsigned           modulation_order() const { return get_bits_per_symbol(lanes.front()->config.modulation); }
  unsigned           lane_count() const { return lanes.size(); }
  pusch_demodulator& demodulator(unsigned lane = 0) { return *lanes.at(lane)->demodulator; }

  uint64_t invocation_count(const dapp::management::identifier& instance) const
  {
    return metric_counter(instance, OCUDU_DAPP_METRIC_INVOKE_COUNT_V1);
  }

  uint64_t completion_deadline_misses(const dapp::management::identifier& instance) const
  {
    return metric_counter(instance, OCUDU_DAPP_METRIC_CLASS_A_COMPLETION_DEADLINE_MISSES_V1);
  }

  dapp::management::identifier load(const dapp::management::package_record& package)
  {
    dapp::management::load_request request;
    request.transaction_id   = transaction_id(next_transaction++);
    request.package_id       = package.package_id;
    request.placement        = OCUDU_DAPP_PLACEMENT_IN_PROCESS_V1;
    request.selected_backend = dapp::management::backend::cuda;
    auto result              = service->instances().load(request);
    require(static_cast<bool>(result) && result.instance_id.has_value(), package.global_name + ": " + result.detail);
    uint64_t generation = result.instance_generation;
    for (auto action : {dapp::management::lifecycle_action::prepare,
                        dapp::management::lifecycle_action::arm,
                        dapp::management::lifecycle_action::enter_shadow,
                        dapp::management::lifecycle_action::activate}) {
      dapp::management::lifecycle_request control;
      control.transaction_id      = transaction_id(next_transaction++);
      control.instance_id         = *result.instance_id;
      control.action              = action;
      control.expected_generation = generation;
      control.timeout_us          = 1'000'000;
      auto transitioned           = service->instances().control(control);
      require(static_cast<bool>(transitioned), package.global_name + ": " + transitioned.detail);
      generation = transitioned.instance_generation;
    }
    return *result.instance_id;
  }

  void unload(dapp::management::identifier instance)
  {
    auto inventory = service->instances().inventory();
    auto found     = std::find_if(inventory.instances.begin(), inventory.instances.end(), [&](const auto& value) {
      return value.instance_id == instance;
    });
    require(found != inventory.instances.end(), "active instance missing from inventory");
    uint64_t generation = found->generation;
    for (auto action : {dapp::management::lifecycle_action::deactivate, dapp::management::lifecycle_action::unload}) {
      dapp::management::lifecycle_request control;
      control.transaction_id      = transaction_id(next_transaction++);
      control.instance_id         = instance;
      control.action              = action;
      control.expected_generation = generation;
      control.timeout_us          = 1'000'000;
      auto transitioned           = service->instances().control(control);
      require(static_cast<bool>(transitioned), transitioned.detail);
      generation = transitioned.instance_generation;
    }
  }

  std::unique_ptr<app_services::dapp_service> service;

private:
  struct lane_context {
    lane_context(unsigned nof_layers, unsigned nof_prb) :
      grid(nof_layers, MAX_NSYMB_PER_SLOT, nof_prb), config(make_config(nof_prb, nof_layers))
    {
    }

    resource_grid_reader_spy           grid;
    empty_results                      estimates;
    codeword_sink                      codeword;
    notifier_sink                      notifier;
    pusch_demodulator::configuration   config;
    std::unique_ptr<pusch_demodulator> demodulator;
  };

  uint64_t metric_counter(const dapp::management::identifier& instance, const char* name) const
  {
    auto metrics = service->instances().metrics(instance);
    require(static_cast<bool>(metrics), "read dApp metrics");
    auto found = std::find_if(
        metrics.value.begin(), metrics.value.end(), [&](const auto& sample) { return sample.name == name; });
    require(found != metrics.value.end(), std::string("dApp metric is missing: ") + name);
    const auto* value = std::get_if<uint64_t>(&found->sample_value);
    require(value != nullptr, std::string("dApp metric has the wrong type: ") + name);
    return *value;
  }

  unsigned                                   nof_prb;
  std::vector<std::unique_ptr<lane_context>> lanes;
  uint64_t                                   next_transaction = 1;
};

template <typename Operation>
void run_concurrently(benchmark_fixture& fixture, Operation&& operation)
{
  std::atomic<unsigned>    ready{};
  std::atomic<bool>        start{};
  std::vector<std::thread> workers;
  workers.reserve(fixture.lane_count());
  for (unsigned lane = 0; lane != fixture.lane_count(); ++lane) {
    workers.emplace_back([&, lane] {
      ready.fetch_add(1, std::memory_order_release);
      while (!start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      operation(lane);
    });
  }
  while (ready.load(std::memory_order_acquire) != fixture.lane_count()) {
    std::this_thread::yield();
  }
  start.store(true, std::memory_order_release);
  for (auto& worker : workers) {
    worker.join();
  }
}

void warm_lane(benchmark_fixture& fixture, unsigned lane)
{
  for (unsigned i = 0; i != warmups; ++i) {
    fixture.invoke(lane);
    synchronize_last(fixture.demodulator(lane));
  }
}

struct measurement {
  struct trace_sample {
    uint64_t latency_ns{};
    uint64_t started_ns{};
    uint64_t finished_ns{};
  };

  std::string               path;
  std::string               scope;
  unsigned                  nof_prb{};
  unsigned                  nof_layers{};
  unsigned                  nof_lanes{};
  unsigned                  modulation_order{};
  uint64_t                  pipeline_deadline_ns{};
  uint64_t                  dapp_deadline_ns{};
  uint64_t                  expected_invocations{};
  uint64_t                  accepted_invocations{};
  uint64_t                  production_deadline_misses{};
  std::vector<uint64_t>     samples;
  std::vector<trace_sample> trace_samples;

  uint64_t percentile(double fraction) const
  {
    const auto index = static_cast<std::size_t>(std::ceil(fraction * samples.size())) - 1;
    return samples[std::min(index, samples.size() - 1)];
  }

  uint64_t mean() const { return std::accumulate(samples.begin(), samples.end(), uint64_t{}) / samples.size(); }

  uint64_t pipeline_deadline_misses() const
  {
    return static_cast<uint64_t>(
        std::count_if(samples.begin(), samples.end(), [&](uint64_t sample) { return sample > pipeline_deadline_ns; }));
  }
};

measurement collect(benchmark_fixture& fixture,
                    const std::string& label,
                    const std::string& scope,
                    unsigned           nof_prb,
                    unsigned           nof_layers,
                    uint64_t           pipeline_deadline_ns)
{
  measurement result{label,
                     scope,
                     nof_prb,
                     nof_layers,
                     fixture.lane_count(),
                     fixture.modulation_order(),
                     pipeline_deadline_ns,
                     0,
                     0,
                     0,
                     0,
                     {},
                     {}};
  result.samples.resize(static_cast<size_t>(repetitions) * fixture.lane_count());
  if (!sample_csv_path.empty()) {
    result.trace_samples.resize(result.samples.size());
  }
  run_concurrently(fixture, [&](unsigned lane) {
    warm_lane(fixture, lane);
    for (unsigned i = 0; i != repetitions; ++i) {
      const auto begin = std::chrono::steady_clock::now();
      fixture.invoke(lane);
      if (scope == "completion") {
        synchronize_last(fixture.demodulator(lane));
      }
      const auto end   = std::chrono::steady_clock::now();
      const auto index = static_cast<size_t>(lane) * repetitions + i;
      const auto elapsed_ns =
          static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count());
      result.samples[index] = elapsed_ns;
      if (!result.trace_samples.empty()) {
        result.trace_samples[index] = {elapsed_ns, trace_offset_ns(begin), trace_offset_ns(end)};
      }
      if (scope == "submit") {
        synchronize_last(fixture.demodulator(lane));
      }
    }
  });
  std::sort(result.samples.begin(), result.samples.end());
  return result;
}

std::vector<measurement> collect_device_pipeline(benchmark_fixture& fixture,
                                                 const std::string& label,
                                                 unsigned           nof_prb,
                                                 unsigned           nof_layers,
                                                 uint64_t           pipeline_deadline_ns,
                                                 bool               collect_stages)
{
  const std::array<const char*, 5> scopes      = {"device-pipeline",
                                                  "device-stage-pre-module",
                                                  "device-stage-dapp",
                                                  "device-stage-continuation",
                                                  "device-stage-post-inline"};
  const unsigned                   nof_results = collect_stages ? scopes.size() : 1;
  std::vector<measurement>         results;
  results.reserve(nof_results);
  for (unsigned i = 0; i != nof_results; ++i) {
    results.push_back({label,
                       scopes[i],
                       nof_prb,
                       nof_layers,
                       fixture.lane_count(),
                       fixture.modulation_order(),
                       pipeline_deadline_ns,
                       0,
                       0,
                       0,
                       0,
                       {},
                       {}});
    results.back().samples.resize(static_cast<size_t>(repetitions) * fixture.lane_count());
    if (!sample_csv_path.empty()) {
      results.back().trace_samples.resize(results.back().samples.size());
    }
  }
  run_concurrently(fixture, [&](unsigned lane) {
    cudaEvent_t begin{};
    cudaEvent_t end{};
    const bool  trace_enabled = !results.front().trace_samples.empty();
    require(cudaEventCreate(&begin) == cudaSuccess && cudaEventCreate(&end) == cudaSuccess,
            "CUDA device-pipeline timing events");
    warm_lane(fixture, lane);
    for (unsigned i = 0; i != repetitions; ++i) {
      std::chrono::steady_clock::time_point host_begin;
      if (trace_enabled) {
        host_begin = std::chrono::steady_clock::now();
      }
      const resident_softbit_buffer previous = fixture.demodulator(lane).get_resident_softbits();
      require(previous.valid && previous.execution_context != nullptr, "missing resident execution context");
      auto stream = static_cast<cudaStream_t>(previous.execution_context);
      require(cudaEventRecord(begin, stream) == cudaSuccess, "CUDA device-pipeline start record");
      fixture.invoke(lane);
      const resident_softbit_buffer output = fixture.demodulator(lane).get_resident_softbits();
      require(output.valid && output.execution_context == previous.execution_context &&
                  output.completion_token != nullptr,
              "invalid resident device-pipeline completion");
      resident_dapp_diagnostic_timing stages;
      if (collect_stages) {
        require(output.buffer_index >= 0, "missing resident completion slot");
        stages = fixture.demodulator(lane).get_resident_dapp_diagnostic_timing(output.buffer_index);
        require(stages.valid && stages.completion_slot == static_cast<unsigned>(output.buffer_index) &&
                    stages.module_begin_token != nullptr && stages.module_end_token != nullptr &&
                    stages.inline_end_token != nullptr,
                "invalid resident dApp stage timing");
      }
      require(cudaEventRecord(end, stream) == cudaSuccess, "CUDA device-pipeline end record");
      require(cudaEventSynchronize(end) == cudaSuccess, "CUDA device-pipeline completion synchronization");
      fixture.demodulator(lane).report_deferred_dapp_completion();
      const std::array<cudaEvent_t, 5> boundaries = {
          begin,
          collect_stages ? static_cast<cudaEvent_t>(stages.module_begin_token) : end,
          collect_stages ? static_cast<cudaEvent_t>(stages.module_end_token) : end,
          collect_stages ? static_cast<cudaEvent_t>(stages.inline_end_token) : end,
          end};
      const auto index = static_cast<size_t>(lane) * repetitions + i;
      for (unsigned result_index = 0; result_index != nof_results; ++result_index) {
        const cudaEvent_t stage_begin = result_index == 0 ? boundaries[0] : boundaries[result_index - 1];
        const cudaEvent_t stage_end   = result_index == 0 ? boundaries[4] : boundaries[result_index];
        float             elapsed_ms{};
        require(cudaEventElapsedTime(&elapsed_ms, stage_begin, stage_end) == cudaSuccess,
                "CUDA device-pipeline stage elapsed time");
        results[result_index].samples[index] =
            static_cast<uint64_t>(std::llround(static_cast<double>(elapsed_ms) * 1.0e6));
      }
      if (trace_enabled) {
        const auto host_end = std::chrono::steady_clock::now();
        for (auto& result : results) {
          result.trace_samples[index] = {result.samples[index], trace_offset_ns(host_begin), trace_offset_ns(host_end)};
        }
      }
    }
    require(cudaEventDestroy(begin) == cudaSuccess && cudaEventDestroy(end) == cudaSuccess,
            "CUDA device-pipeline timing event destruction");
  });
  for (auto& result : results) {
    std::sort(result.samples.begin(), result.samples.end());
  }
  return results;
}

void print(const measurement& result)
{
  constexpr double ns_to_us = 1.0 / 1000.0;
  fmt::print("path={} scope={} prbs={} layers={} lanes={} modulation={} samples={} mean_us={:.3f} p50_us={:.3f} "
             "p90_us={:.3f} "
             "p95_us={:.3f} "
             "p99_us={:.3f} "
             "p999_us={:.3f} max_us={:.3f} pipeline_deadline_us={:.3f} pipeline_misses={}\n",
             result.path,
             result.scope,
             result.nof_prb,
             result.nof_layers,
             result.nof_lanes,
             result.modulation_order,
             result.samples.size(),
             result.mean() * ns_to_us,
             result.percentile(0.50) * ns_to_us,
             result.percentile(0.90) * ns_to_us,
             result.percentile(0.95) * ns_to_us,
             result.percentile(0.99) * ns_to_us,
             result.percentile(0.999) * ns_to_us,
             result.samples.back() * ns_to_us,
             result.pipeline_deadline_ns * ns_to_us,
             result.pipeline_deadline_misses());
}

void append_csv(std::ofstream&               output,
                const measurement&           result,
                const gpu_client_monitor&    monitor,
                const execution_environment& environment)
{
  output << result.path << ',' << result.scope << ',' << result.nof_prb << ',' << result.nof_layers << ','
         << result.nof_lanes << ',' << nof_symbols << ',' << warmups << ',' << result.modulation_order << ','
         << result.samples.size() << ',' << result.mean() << ',' << result.percentile(0.50) << ','
         << result.percentile(0.90) << ',' << result.percentile(0.95) << ',' << result.percentile(0.99) << ','
         << result.percentile(0.999) << ',' << result.samples.back() << ',' << result.pipeline_deadline_ns << ','
         << result.pipeline_deadline_misses() << ',' << result.dapp_deadline_ns << ',' << result.expected_invocations
         << ',' << result.accepted_invocations << ',' << result.production_deadline_misses << ','
         << monitor.monitoring_available() << ',' << monitor.observation_healthy() << ',' << monitor.clients().size()
         << ',' << format_client_pids(monitor) << ',' << (allow_shared_gpu ? "contention" : "exclusive") << ','
         << environment.host << ',' << environment.machine << ',' << environment.gpu << ',' << environment.scheduler
         << ',' << environment.scheduler_priority << ',' << environment.cpu_affinity << ','
         << environment.stream_priority << ',' << (package_filter.empty() ? "all" : package_filter) << ','
         << preflight_timeout_ms << ',' << environment.build_hash << ',' << environment.build_mode << ','
         << environment.cuda_device << ',' << environment.cuda_driver_version << ',' << environment.cuda_runtime_version
         << ',' << environment.gpu_pci_bus_id << ',' << monitor.driver_version() << ',' << monitor.device_uuid() << ','
         << monitor.temperature_min_c() << ',' << monitor.temperature_max_c() << ',' << monitor.sm_clock_min_mhz()
         << ',' << monitor.sm_clock_max_mhz() << ',' << monitor.memory_clock_min_mhz() << ','
         << monitor.memory_clock_max_mhz() << ',' << monitor.power_min_mw() << ',' << monitor.power_max_mw() << ','
         << monitor.power_limit_mw() << ',' << monitor.persistence_mode() << ',' << monitor.compute_mode() << ','
         << monitor.performance_state_best() << ',' << monitor.performance_state_worst() << '\n';
}

void append_sample_csv(std::ofstream& output, const measurement& result)
{
  require(result.trace_samples.size() == static_cast<size_t>(result.nof_lanes) * repetitions,
          "raw-sample trace cardinality");
  for (unsigned lane = 0; lane != result.nof_lanes; ++lane) {
    for (unsigned ordinal = 0; ordinal != repetitions; ++ordinal) {
      const auto& sample = result.trace_samples[static_cast<size_t>(lane) * repetitions + ordinal];
      output << result.path << ',' << result.scope << ',' << result.nof_prb << ',' << result.nof_layers << ',' << lane
             << ',' << ordinal << ',' << result.modulation_order << ',' << sample.latency_ns << ',' << sample.started_ns
             << ',' << sample.finished_ns << ',' << result.pipeline_deadline_ns << ','
             << (sample.latency_ns > result.pipeline_deadline_ns) << '\n';
    }
  }
}

void measure_path(benchmark_fixture&                  fixture,
                  const std::string&                  label,
                  unsigned                            nof_prb,
                  unsigned                            nof_layers,
                  uint64_t                            dapp_deadline_ns,
                  const dapp::management::identifier* instance,
                  gpu_client_monitor&                 monitor,
                  std::vector<measurement>&           results)
{
  const uint64_t before                 = instance != nullptr ? fixture.invocation_count(*instance) : 0;
  const uint64_t deadline_misses_before = instance != nullptr ? fixture.completion_deadline_misses(*instance) : 0;
  const size_t   first                  = results.size();
  results.push_back(collect(fixture, label, "submit", nof_prb, nof_layers, demodulator_deadline_ns));
  print(results.back());
  results.push_back(collect(fixture, label, "completion", nof_prb, nof_layers, demodulator_deadline_ns));
  print(results.back());
  auto device_results = collect_device_pipeline(
      fixture, label, nof_prb, nof_layers, demodulator_deadline_ns, instance != nullptr && !sample_csv_path.empty());
  for (auto& result : device_results) {
    print(result);
    results.push_back(std::move(result));
  }
  const uint64_t expected =
      instance != nullptr ? 3ULL * fixture.lane_count() * (static_cast<uint64_t>(warmups) + repetitions) : 0;
  const uint64_t accepted = instance != nullptr ? fixture.invocation_count(*instance) - before : 0;
  const uint64_t production_deadline_misses =
      instance != nullptr ? fixture.completion_deadline_misses(*instance) - deadline_misses_before : 0;
  for (size_t index = first; index != results.size(); ++index) {
    results[index].dapp_deadline_ns           = dapp_deadline_ns;
    results[index].expected_invocations       = expected;
    results[index].accepted_invocations       = accepted;
    results[index].production_deadline_misses = production_deadline_misses;
  }
  if (instance != nullptr) {
    fmt::print("path={} qualification dapp_deadline_us={:.3f} expected_invocations={} accepted_invocations={} "
               "production_deadline_misses={} lanes={} admitted={}\n",
               label,
               dapp_deadline_ns / 1000.0,
               expected,
               accepted,
               production_deadline_misses,
               fixture.lane_count(),
               accepted == expected);
  }
  monitor.observe();
}

std::vector<unsigned> parse_prbs(const char* text)
{
  std::vector<unsigned> values;
  std::istringstream    input(text);
  std::string           item;
  while (std::getline(input, item, ',')) {
    char*      end   = nullptr;
    const auto value = std::strtoul(item.c_str(), &end, 10);
    if (item.empty() || *end != '\0' || value == 0 || value > MAX_NOF_PRBS) {
      fail("PRB list entries must be in [1, MAX_NOF_PRBS]");
    }
    values.push_back(static_cast<unsigned>(value));
  }
  require(!values.empty(), "empty PRB list");
  return values;
}

std::vector<unsigned> parse_layers(const char* text)
{
  std::vector<unsigned> values;
  std::istringstream    input(text);
  std::string           item;
  while (std::getline(input, item, ',')) {
    char*      end   = nullptr;
    const auto value = std::strtoul(item.c_str(), &end, 10);
    if (item.empty() || *end != '\0' || (value != 1 && value != 2)) {
      fail("layer entries must be comma-separated values from {1,2}");
    }
    values.push_back(static_cast<unsigned>(value));
  }
  require(!values.empty(), "empty layer list");
  return values;
}

std::vector<modulation_scheme> parse_modulations(const char* text)
{
  std::vector<modulation_scheme> values;
  std::istringstream             input(text);
  std::string                    item;
  while (std::getline(input, item, ',')) {
    char*      end   = nullptr;
    const auto value = std::strtoul(item.c_str(), &end, 10);
    if (item.empty() || *end != '\0') {
      fail("modulation entries must be comma-separated bit orders from {2,4,6,8}");
    }
    switch (value) {
      case 2:
        values.push_back(modulation_scheme::QPSK);
        break;
      case 4:
        values.push_back(modulation_scheme::QAM16);
        break;
      case 6:
        values.push_back(modulation_scheme::QAM64);
        break;
      case 8:
        values.push_back(modulation_scheme::QAM256);
        break;
      default:
        fail("modulation entries must be comma-separated bit orders from {2,4,6,8}");
    }
  }
  require(!values.empty(), "empty modulation list");
  return values;
}

const char* modulation_name(modulation_scheme value)
{
  switch (value) {
    case modulation_scheme::QPSK:
      return "qpsk";
    case modulation_scheme::QAM16:
      return "qam16";
    case modulation_scheme::QAM64:
      return "qam64";
    case modulation_scheme::QAM256:
      return "qam256";
    default:
      return "unsupported";
  }
}

void parse_args(int argc, char** argv)
{
  int option = 0;
  while ((option = ::getopt(argc, argv, "R:W:N:P:L:M:F:S:T:C:D:GXQ:O:h")) != -1) {
    const bool numeric = option != 'h' && option != 'G' && option != 'X' && option != 'P' && option != 'L' &&
                         option != 'M' && option != 'F' && option != 'Q' && option != 'O';
    const auto value = numeric ? std::strtoul(optarg, nullptr, 10) : 0UL;
    if (option == 'R' && value > 0) {
      repetitions = value;
    } else if (option == 'W') {
      warmups = value;
    } else if (option == 'N' && value > 0 && value <= 64) {
      concurrent_lanes = value;
    } else if (option == 'P') {
      prb_values = parse_prbs(optarg);
    } else if (option == 'L') {
      layer_values = parse_layers(optarg);
    } else if (option == 'M') {
      receiver_modulations = parse_modulations(optarg);
    } else if (option == 'F' && optarg[0] != '\0' && std::strpbrk(optarg, ",\r\n") == nullptr) {
      package_filter = optarg;
    } else if (option == 'S' && value > 0 && value <= MAX_NSYMB_PER_SLOT - 2) {
      nof_symbols = value;
    } else if (option == 'T' && value > 0 && value <= 60'000) {
      preflight_timeout_ms = value;
    } else if (option == 'C' && value > 0) {
      channel_estimator_deadline_ns = value * 1000;
    } else if (option == 'D' && value > 0) {
      demodulator_deadline_ns = value * 1000;
    } else if (option == 'G') {
      enforce_p999 = true;
    } else if (option == 'X') {
      allow_shared_gpu = true;
    } else if (option == 'Q') {
      sample_csv_path = optarg;
    } else if (option == 'O') {
      csv_path = optarg;
    } else {
      fmt::print("Usage: {} [-R repetitions] [-W warmups] [-N concurrent_lanes] [-P prb,...] [-L 1,2] "
                 "[-M 2,4,6,8] [-F package_substring] [-S symbols] [-T preflight_timeout_ms] "
                 "[-C ce_deadline_us] [-D demodulator_deadline_us] [-G] [-X allow_shared_gpu] "
                 "[-O results.csv] [-Q diagnostic_samples.csv]\n",
                 argv[0]);
      std::exit(option == 'h' ? 0 : 1);
    }
  }
  require(!enforce_p999 || preflight_timeout_ms == 500,
          "qualification requires the production 500-ms native preflight timeout");
  require(!enforce_p999 || package_filter.empty(), "qualification requires the complete package matrix");
  require(!enforce_p999 || sample_csv_path.empty(), "qualification forbids diagnostic raw-sample instrumentation");
  require(csv_path.empty() || sample_csv_path.empty() || csv_path != sample_csv_path,
          "summary and raw-sample CSV paths must differ");
}

} // namespace

int main(int argc, char** argv)
{
  parse_args(argc, argv);
  utsname host{};
  require(::uname(&host) == 0, "uname");
  int device = -1;
  require(cudaGetDevice(&device) == cudaSuccess, "CUDA device");
  cudaDeviceProp properties{};
  require(cudaGetDeviceProperties(&properties, device) == cudaSuccess, "CUDA device properties");
  const execution_environment environment = describe_environment(host, properties, device);
  gpu_client_monitor          gpu_clients(device);
  fmt::print(
      "Class-A CUDA benchmark: host={} machine={} gpu={} repetitions={} warmups_per_phase={} lanes={} symbols={} "
      "build={}\n",
      host.nodename,
      host.machine,
      properties.name,
      repetitions,
      warmups,
      concurrent_lanes,
      nof_symbols,
#ifdef NDEBUG
      "release-like"
#else
      "assertion-enabled"
#endif
  );
  fmt::print("Execution environment: scheduler={} priority={} cpu_affinity={} pusch_stream_priority={}.\n",
             environment.scheduler,
             environment.scheduler_priority,
             environment.cpu_affinity,
             environment.stream_priority);
  fmt::print("Build and CUDA environment: build_hash={} build_mode={} cuda_device={} cuda_driver={} cuda_runtime={} "
             "pci_bus_id={}.\n",
             environment.build_hash,
             environment.build_mode,
             environment.cuda_device,
             environment.cuda_driver_version,
             environment.cuda_runtime_version,
             environment.gpu_pci_bus_id);
  fmt::print("GPU qualification policy={} monitoring_available={} observation_healthy={} foreign_compute_clients={} "
             "pids={}.\n",
             allow_shared_gpu ? "contention" : "exclusive",
             gpu_clients.monitoring_available(),
             gpu_clients.observation_healthy(),
             gpu_clients.clients().size(),
             format_client_pids(gpu_clients));
  fmt::print("Package selection: {}.\n", package_filter.empty() ? "all" : package_filter);
  fmt::print("Native preflight timeout: {} ms.\n", preflight_timeout_ms);
  fmt::print("Raw sample trace: {}{}\n",
             sample_csv_path.empty() ? "disabled" : sample_csv_path,
             sample_csv_path.empty() ? "." : " (diagnostic mode; not qualification evidence).");
  if (!sample_csv_path.empty()) {
    fmt::print("Resident dApp stage diagnostics: pre-module, dApp module, required continuation, and post-inline "
               "handoff (non-qualifying CUDA-event decomposition).\n");
  }
  if (enforce_p999 && gpu_environment_failed(gpu_clients)) {
    fmt::print(stderr,
               "Class-A CUDA qualification refused before fixture construction: GPU monitoring is unavailable, "
               "unhealthy, or the selected GPU has a foreign compute client.\n");
    return 2;
  }
  fmt::print("Each lane is one production pre-bound PUSCH demodulator driven by one persistent thread per sampling "
             "phase. Each phase worker runs its own warmups immediately before its recorded samples, which are "
             "pooled across lanes. Submit excludes explicit completion wait; completion is host wall time through "
             "the CUDA event wait.\n");
  fmt::print("Device-pipeline is CUDA-event time from before full demodulator enqueue to resident completion; "
             "gate={} its p99.9, complete module admission and zero production-boundary misses. "
             "Pipeline budget={:.3f} us; CE dApp budget={:.3f} us; CE+EQ/RX dApp budget={:.3f} us.\n",
             enforce_p999 ? "enforces" : "reports",
             demodulator_deadline_ns / 1000.0,
             channel_estimator_deadline_ns / 1000.0,
             demodulator_deadline_ns / 1000.0);

  std::vector<measurement> results;
  for (unsigned nof_prb : prb_values) {
    for (unsigned nof_layers : layer_values) {
      benchmark_fixture fixture(nof_prb, nof_layers);
      measure_path(fixture, "accelerated-conventional", nof_prb, nof_layers, 0, nullptr, gpu_clients, results);
      const auto inventory = fixture.service->instances().inventory();
      unsigned   selected  = 0;
      for (const auto& package : inventory.packages) {
        if (!package_filter.empty() && package.global_name.find(package_filter) == std::string::npos) {
          continue;
        }
        ++selected;
        auto       instance = fixture.load(package);
        const bool receiver = std::any_of(package.interfaces.begin(), package.interfaces.end(), [](const auto& value) {
          return value.interface_id == OCUDU_DAPP_INTERFACE_RECEIVER_V1;
        });
        if (receiver) {
          for (modulation_scheme modulation : receiver_modulations) {
            fixture.set_modulation(modulation);
            measure_path(fixture,
                         package.global_name + "-" + modulation_name(modulation),
                         nof_prb,
                         nof_layers,
                         demodulator_deadline_ns,
                         &instance,
                         gpu_clients,
                         results);
          }
        } else {
          fixture.set_modulation(modulation_scheme::QPSK);
          const bool channel_estimator =
              std::any_of(package.interfaces.begin(), package.interfaces.end(), [](const auto& value) {
                return value.interface_id == OCUDU_DAPP_INTERFACE_CHANNEL_ESTIMATOR_V1;
              });
          measure_path(fixture,
                       package.global_name,
                       nof_prb,
                       nof_layers,
                       channel_estimator ? channel_estimator_deadline_ns : demodulator_deadline_ns,
                       &instance,
                       gpu_clients,
                       results);
        }
        fixture.unload(instance);
      }
      require(package_filter.empty() || selected != 0, "package filter matched no package");
    }
  }

  gpu_clients.observe();
  fmt::print("GPU client observation complete: monitoring_available={} observation_healthy={} "
             "foreign_compute_clients={} pids={}.\n",
             gpu_clients.monitoring_available(),
             gpu_clients.observation_healthy(),
             gpu_clients.clients().size(),
             format_client_pids(gpu_clients));
  fmt::print("GPU telemetry: nvml_driver={} uuid={} temperature_c={}..{} sm_clock_mhz={}..{} "
             "memory_clock_mhz={}..{} power_mw={}..{} power_limit_mw={} persistence={} compute_mode={} "
             "performance_state={}..{}.\n",
             gpu_clients.driver_version(),
             gpu_clients.device_uuid(),
             gpu_clients.temperature_min_c(),
             gpu_clients.temperature_max_c(),
             gpu_clients.sm_clock_min_mhz(),
             gpu_clients.sm_clock_max_mhz(),
             gpu_clients.memory_clock_min_mhz(),
             gpu_clients.memory_clock_max_mhz(),
             gpu_clients.power_min_mw(),
             gpu_clients.power_max_mw(),
             gpu_clients.power_limit_mw(),
             gpu_clients.persistence_mode(),
             gpu_clients.compute_mode(),
             gpu_clients.performance_state_best(),
             gpu_clients.performance_state_worst());

  if (!csv_path.empty()) {
    std::ofstream output(csv_path, std::ios::trunc);
    require(output.is_open(), "open CSV output");
    output << "path,scope,prbs,layers,pusch_lanes,symbols,warmups_per_phase,modulation_order,samples,mean_ns,p50_ns,"
              "p90_ns,p95_ns,p99_ns,p999_ns,max_ns,"
              "pipeline_deadline_ns,pipeline_misses,dapp_deadline_ns,path_expected_invocations,"
              "path_accepted_invocations,"
              "path_production_deadline_misses,gpu_monitor_available,gpu_observation_healthy,"
              "foreign_compute_processes,foreign_compute_pids,gpu_policy,host,machine,gpu,scheduler_policy,"
              "scheduler_priority,cpu_affinity,pusch_stream_priority,package_filter,preflight_timeout_ms,"
              "build_hash,build_mode,cuda_device,cuda_driver_version,cuda_runtime_version,gpu_pci_bus_id,"
              "nvml_driver_version,gpu_uuid,gpu_temperature_min_c,gpu_temperature_max_c,gpu_sm_clock_min_mhz,"
              "gpu_sm_clock_max_mhz,gpu_memory_clock_min_mhz,gpu_memory_clock_max_mhz,gpu_power_min_mw,"
              "gpu_power_max_mw,gpu_power_limit_mw,gpu_persistence_mode,gpu_compute_mode,"
              "gpu_performance_state_best,gpu_performance_state_worst\n";
    for (const auto& result : results) {
      append_csv(output, result, gpu_clients, environment);
    }
    require(static_cast<bool>(output), "write CSV output");
  }
  if (!sample_csv_path.empty()) {
    std::ofstream output(sample_csv_path, std::ios::trunc);
    require(output.is_open(), "open raw-sample CSV output");
    output << "path,scope,prbs,layers,pusch_lane,sample_ordinal,modulation_order,latency_ns,"
              "host_started_offset_ns,host_finished_offset_ns,pipeline_deadline_ns,scope_deadline_miss\n";
    for (const auto& result : results) {
      append_sample_csv(output, result);
    }
    require(static_cast<bool>(output), "write raw-sample CSV output");
  }
  const bool path_failed = std::any_of(results.begin(), results.end(), [](const measurement& result) {
    return result.scope == "device-pipeline" &&
           (result.percentile(0.999) > result.pipeline_deadline_ns ||
            result.accepted_invocations != result.expected_invocations || result.production_deadline_misses != 0);
  });
  if (enforce_p999 && (gpu_environment_failed(gpu_clients) || path_failed)) {
    fmt::print(stderr, "Class-A CUDA environment, pipeline, admission or production-boundary deadline gate failed.\n");
    return 2;
  }
  return 0;
}
