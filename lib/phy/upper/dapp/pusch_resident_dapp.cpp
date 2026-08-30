// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/phy/upper/dapp/pusch_resident_dapp.h"
#include "../signal_processors/dmrs_helper.h"
#include "ocudu/ocudulog/ocudulog.h"
#include "ocudu/phy/upper/dapp/pusch_dapp_channel_estimator.h"
#include "ocudu/phy/upper/dapp/pusch_dapp_demodulator.h"
#include "ocudu/phy/upper/sequence_generators/pseudo_random_generator.h"
#include "ocudu/ran/pusch/pusch_constants.h"
#include "ocudu/support/math/math_utils.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <limits>
#include <new>
#include <optional>
#include <thread>
#include <vector>

#ifdef ENABLE_CUDA
#include <cuda_runtime.h>
#endif

using namespace ocudu;

#ifdef ENABLE_CUDA
namespace {

const char* modulation_name(modulation_scheme value) noexcept
{
  switch (value) {
    case modulation_scheme::PI_2_BPSK:
      return "pi/2-BPSK";
    case modulation_scheme::QPSK:
      return "QPSK";
    case modulation_scheme::QAM16:
      return "16QAM";
    case modulation_scheme::QAM64:
      return "64QAM";
    case modulation_scheme::QAM256:
      return "256QAM";
    default:
      return "unknown";
  }
}

const char* fallback_reason_name(dapp::class_a_fallback_reason reason) noexcept
{
  switch (reason) {
    case dapp::class_a_fallback_reason::admission_closed:
      return "admission_closed";
    case dapp::class_a_fallback_reason::breaker_open:
      return "breaker_open";
    case dapp::class_a_fallback_reason::overlapped_hook:
      return "overlapped_hook";
    case dapp::class_a_fallback_reason::unsupported_metadata:
      return "unsupported_metadata";
    case dapp::class_a_fallback_reason::unsupported_layers:
      return "unsupported_layers";
    case dapp::class_a_fallback_reason::unsupported_rx_ports:
      return "unsupported_rx_ports";
    case dapp::class_a_fallback_reason::unsupported_allocation:
      return "unsupported_allocation";
    case dapp::class_a_fallback_reason::unsupported_modulation:
      return "unsupported_modulation";
    case dapp::class_a_fallback_reason::unsupported_dmrs:
      return "unsupported_dmrs";
    case dapp::class_a_fallback_reason::unsupported_transform_precoding:
      return "unsupported_transform_precoding";
    case dapp::class_a_fallback_reason::unsupported_dc_overlap:
      return "unsupported_dc_overlap";
    case dapp::class_a_fallback_reason::module_bypass:
      return "module_bypass";
    case dapp::class_a_fallback_reason::module_unsupported:
      return "module_unsupported";
    case dapp::class_a_fallback_reason::invalid_input:
      return "invalid_input";
    case dapp::class_a_fallback_reason::invalid_output:
      return "invalid_output";
    case dapp::class_a_fallback_reason::deadline:
      return "deadline";
    case dapp::class_a_fallback_reason::exception:
      return "exception";
    case dapp::class_a_fallback_reason::device:
      return "device";
    case dapp::class_a_fallback_reason::resource:
      return "resource";
    case dapp::class_a_fallback_reason::returned_error:
      return "returned_error";
    default:
      return "unknown";
  }
}

/// OTA bring-up diagnostics: always log fallbacks; rate-limit successful invokes.
void log_equalizer_diag(const char*                                  tag,
                        const pusch_demodulator::configuration&      config,
                        const pusch_resident_dapp_buffers&           buffers,
                        std::optional<dapp::class_a_fallback_reason> reason = std::nullopt) noexcept
{
  static std::atomic<uint64_t> ok_count{0};
  static std::atomic<uint64_t> fallback_count{0};

  // Throttle before deriving any formatting arguments: a dApp that falls back on
  // every PUSCH must not turn this into a per-PDU formatting cost on UL-PHY.
  // Same sampling for both branches: first 8, then every 128th.
  const uint64_t n = (reason.has_value() ? fallback_count : ok_count).fetch_add(1, std::memory_order_relaxed) + 1;
  if (n > 8 && (n % 128) != 0) {
    return;
  }

  const int      first_rb = config.rb_mask.find_lowest();
  const int      last_rb  = config.rb_mask.find_highest();
  const unsigned nof_prb  = (first_rb >= 0 && last_rb >= first_rb) ? static_cast<unsigned>(last_rb - first_rb + 1) : 0U;
  const char*    rapid    = config.n_rapid.has_value() ? "yes" : "no";

  // Compact DMRS symbol list so resident-dApp logs can be correlated (e.g. "2,7,11").
  char dmrs_syms[48]{};
  {
    int pos = 0;
    for (unsigned s = 0; s < 14 && pos < 44; ++s) {
      if (config.dmrs_symb_pos.test(s)) {
        pos += std::snprintf(dmrs_syms + pos, sizeof(dmrs_syms) - static_cast<size_t>(pos), pos ? ",%u" : "%u", s);
      }
    }
  }
  const unsigned nof_dmrs_sym = static_cast<unsigned>(config.dmrs_symb_pos.count());

  if (reason.has_value()) {
    ocudulog::fetch_basic_logger("PHY").debug(
        "[DAPP-EQ] {} #{} reason={} rnti=0x{:04x} slot={}.{} mod={} layers={} rx_ports={} "
        "sym=[{},{}) prb={} sc=[{},{}) data_re={} dmrs_type={} dmrs_pos=[{}](n={}) cdm={} "
        "dmrs_scale={:.3f} rapid={} stream=0x{:x}",
        tag,
        n,
        fallback_reason_name(*reason),
        static_cast<unsigned>(to_value(config.rnti)),
        config.slot.sfn(),
        config.slot.slot_index(),
        modulation_name(config.modulation),
        config.nof_tx_layers,
        config.rx_ports.size(),
        config.start_symbol_index,
        config.start_symbol_index + config.nof_symbols,
        nof_prb,
        first_rb >= 0 ? static_cast<unsigned>(first_rb) * NOF_SUBCARRIERS_PER_RB : 0U,
        first_rb >= 0 ? static_cast<unsigned>(last_rb + 1) * NOF_SUBCARRIERS_PER_RB : 0U,
        buffers.nof_data_re,
        static_cast<unsigned>(config.dmrs_type),
        dmrs_syms,
        nof_dmrs_sym,
        config.nof_cdm_groups_without_data,
        config.dmrs_scaling,
        rapid,
        static_cast<unsigned long long>(buffers.execution_stream));
    return;
  }

  ocudulog::fetch_basic_logger("PHY").debug(
      "[DAPP-EQ] {} #{} rnti=0x{:04x} slot={}.{} mod={} layers={} rx_ports={} "
      "sym=[{},{}) prb={} sc=[{},{}) data_re={} dmrs_type={} dmrs_pos=[{}](n={}) cdm={} "
      "dmrs_scale={:.3f} rapid={} stream=0x{:x}",
      tag,
      n,
      static_cast<unsigned>(to_value(config.rnti)),
      config.slot.sfn(),
      config.slot.slot_index(),
      modulation_name(config.modulation),
      config.nof_tx_layers,
      config.rx_ports.size(),
      config.start_symbol_index,
      config.start_symbol_index + config.nof_symbols,
      nof_prb,
      first_rb >= 0 ? static_cast<unsigned>(first_rb) * NOF_SUBCARRIERS_PER_RB : 0U,
      first_rb >= 0 ? static_cast<unsigned>(last_rb + 1) * NOF_SUBCARRIERS_PER_RB : 0U,
      buffers.nof_data_re,
      static_cast<unsigned>(config.dmrs_type),
      dmrs_syms,
      nof_dmrs_sym,
      config.nof_cdm_groups_without_data,
      config.dmrs_scaling,
      rapid,
      static_cast<unsigned long long>(buffers.execution_stream));
}

class non_owning_completion_barrier final : public dapp::native_completion_barrier
{
public:
  explicit non_owning_completion_barrier(std::weak_ptr<dapp::native_completion_barrier> target_) noexcept :
    target(std::move(target_))
  {
  }

  bool wait_until(std::chrono::steady_clock::time_point deadline) noexcept override
  {
    auto owner = target.lock();
    return owner == nullptr || owner->wait_until(deadline);
  }

private:
  std::weak_ptr<dapp::native_completion_barrier> target;
};

constexpr float    inverse_sqrt_two   = 0.70710678118654752440F;
constexpr unsigned max_dmrs_re_per_rb = NOF_SUBCARRIERS_PER_RB / 2;

uint64_t deadline_from_now(std::chrono::nanoseconds budget) noexcept
{
  const auto now =
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
  if (now < 0 || budget.count() < 0) {
    return 0;
  }
  const uint64_t value = static_cast<uint64_t>(now);
  const uint64_t delta = static_cast<uint64_t>(budget.count());
  return delta > std::numeric_limits<uint64_t>::max() - value ? std::numeric_limits<uint64_t>::max() : value + delta;
}

template <typename T>
ocudu_dapp_tensor_view_v1 make_device_tensor(T*                              data,
                                             uint32_t                        element_type,
                                             uint32_t                        access,
                                             uint32_t                        device_ordinal,
                                             uint64_t                        allocation_id,
                                             uint64_t                        generation,
                                             std::initializer_list<uint64_t> dimensions,
                                             uint32_t                        layout = 0) noexcept
{
  ocudu_dapp_tensor_view_v1 tensor{};
  tensor.struct_size           = sizeof(tensor);
  tensor.abi_major             = OCUDU_DAPP_ABI_MAJOR_V1;
  tensor.abi_minor             = OCUDU_DAPP_ABI_MINOR_V1;
  tensor.domain                = OCUDU_DAPP_MEM_ACCELERATOR_DEVICE_V1;
  tensor.backend_id            = OCUDU_DAPP_BACKEND_CUDA_V1;
  tensor.element_type          = element_type;
  tensor.rank                  = dimensions.size();
  tensor.access                = access;
  tensor.device_ordinal        = device_ordinal;
  tensor.allocation_id         = allocation_id;
  tensor.allocation_generation = generation;
  tensor.native_handle         = reinterpret_cast<uintptr_t>(data);
  tensor.layout_id             = layout;
  tensor.layout_version        = 1;
  tensor.lease_token           = generation;
  uint64_t elements            = 1;
  uint32_t index               = 0;
  for (uint64_t dimension : dimensions) {
    tensor.dimensions[index++] = dimension;
    elements *= dimension;
  }
  uint64_t stride = sizeof(T);
  for (uint32_t dimension = tensor.rank; dimension != 0; --dimension) {
    tensor.byte_strides[dimension - 1] = stride;
    stride *= tensor.dimensions[dimension - 1];
  }
  tensor.byte_length = elements * sizeof(T);
  return tensor;
}

template <typename T>
ocudu_dapp_typed_metadata_v1 make_metadata(uint32_t schema, const T& value) noexcept
{
  ocudu_dapp_typed_metadata_v1 result{};
  result.struct_size             = sizeof(result);
  result.abi_major               = OCUDU_DAPP_ABI_MAJOR_V1;
  result.abi_minor               = OCUDU_DAPP_ABI_MINOR_V1;
  result.metadata_schema_id      = schema;
  result.metadata_schema_version = 1;
  result.metadata                = {&value, sizeof(value), 0};
  return result;
}

class cuda_resident_dapp final : public pusch_resident_dapp, public dapp::native_completion_barrier
{
public:
  cuda_resident_dapp(std::shared_ptr<pusch_dapp_demodulator_target>       target_,
                     std::shared_ptr<pusch_dapp_channel_estimator_target> channel_estimator_,
                     uint32_t                                             lane_,
                     std::unique_ptr<pseudo_random_generator>             pilot_generator_,
                     unsigned                                             max_nof_prb_,
                     unsigned                                             max_nof_rx_ports_,
                     unsigned                                             max_nof_layers_,
                     std::chrono::nanoseconds                             demodulator_deadline_,
                     float                                                noise_variance_floor_,
                     std::chrono::nanoseconds                             channel_estimator_deadline_) :
    target(std::move(target_)),
    channel_estimator(std::move(channel_estimator_)),
    lane(lane_),
    pilot_generator(std::move(pilot_generator_)),
    max_nof_prb(max_nof_prb_),
    max_nof_rx_ports(max_nof_rx_ports_),
    max_nof_layers(std::min(max_nof_layers_, 2U)),
    demodulator_deadline(demodulator_deadline_),
    channel_estimator_deadline(channel_estimator_deadline_),
    noise_variance_floor(noise_variance_floor_),
    max_nof_pilots(static_cast<size_t>(MAX_NSYMB_PER_SLOT) * max_nof_prb * max_dmrs_re_per_rb),
    max_nof_channel_elements(static_cast<size_t>(max_nof_rx_ports) * max_nof_layers * MAX_NSYMB_PER_SLOT * max_nof_prb *
                             NOF_SUBCARRIERS_PER_RB),
    pilot_sequence(static_cast<size_t>(max_nof_prb) * max_dmrs_re_per_rb)
  {
  }

  ~cuda_resident_dapp() override
  {
    for (auto& timing : timings) {
      if (timing.start != nullptr) {
        cudaEventDestroy(timing.start);
      }
      if (timing.module_end != nullptr) {
        cudaEventDestroy(timing.module_end);
      }
      if (timing.end != nullptr) {
        cudaEventDestroy(timing.end);
      }
    }
    if (device_coordinates != nullptr) {
      cudaFree(device_coordinates);
    }
    if (device_references != nullptr) {
      cudaFree(device_references);
    }
    if (device_channel_estimates != nullptr) {
      cudaFree(device_channel_estimates);
    }
    if (device_channel_noise != nullptr) {
      cudaFree(device_channel_noise);
    }
    if (device_channel_scheduler_noise != nullptr) {
      cudaFree(device_channel_scheduler_noise);
    }
    if (host_coordinates != nullptr) {
      cudaFreeHost(host_coordinates);
    }
    if (host_references != nullptr) {
      cudaFreeHost(host_references);
    }
  }

  bool initialize() noexcept
  {
    if (!target || !pilot_generator || max_nof_prb == 0 || max_nof_rx_ports == 0 || max_nof_layers == 0 ||
        demodulator_deadline.count() <= 0 || channel_estimator_deadline.count() <= 0 ||
        !std::isfinite(noise_variance_floor) || noise_variance_floor <= 0.0F ||
        cudaHostAlloc(&host_references, max_nof_layers * max_nof_pilots * sizeof(cf_t), cudaHostAllocPortable) !=
            cudaSuccess ||
        cudaHostAlloc(&host_coordinates, 2 * max_nof_pilots * sizeof(uint16_t), cudaHostAllocPortable) != cudaSuccess ||
        cudaMalloc(&device_references, max_nof_layers * max_nof_pilots * sizeof(cf_t)) != cudaSuccess ||
        cudaMalloc(&device_coordinates, 2 * max_nof_pilots * sizeof(uint16_t)) != cudaSuccess ||
        cudaMalloc(&device_channel_estimates, max_nof_channel_elements * sizeof(cf_t)) != cudaSuccess ||
        cudaMalloc(&device_channel_noise, max_nof_rx_ports * max_nof_layers * sizeof(float)) != cudaSuccess ||
        cudaMalloc(&device_channel_scheduler_noise, max_nof_rx_ports * max_nof_layers * sizeof(float)) != cudaSuccess) {
      return false;
    }
    for (auto& timing : timings) {
      if (cudaEventCreate(&timing.start) != cudaSuccess || cudaEventCreate(&timing.module_end) != cudaSuccess ||
          cudaEventCreate(&timing.end) != cudaSuccess) {
        static_cast<void>(cudaGetLastError());
        return false;
      }
    }
    return true;
  }

  bool register_completion_barrier(const std::shared_ptr<dapp::native_completion_barrier>& self) noexcept
  {
    try {
      auto       barrier   = std::make_shared<non_owning_completion_barrier>(self);
      const bool equalizer = target->set_completion_barrier(lane, pusch_dapp_demodulator_mode::equalizer, barrier);
      const bool receiver  = target->set_completion_barrier(lane, pusch_dapp_demodulator_mode::receiver, barrier);
      const bool channel   = channel_estimator && channel_estimator->set_completion_barrier(lane, barrier);
      return equalizer || receiver || channel;
    } catch (...) {
      return false;
    }
  }

  pusch_resident_dapp_mode mode() const noexcept override
  {
    const auto demodulator_mode = target->active_mode();
    if (demodulator_mode == pusch_dapp_demodulator_mode::equalizer) {
      return pusch_resident_dapp_mode::equalizer;
    }
    if (demodulator_mode == pusch_dapp_demodulator_mode::receiver) {
      return pusch_resident_dapp_mode::receiver;
    }
    if (demodulator_mode == pusch_dapp_demodulator_mode::conflict) {
      return pusch_resident_dapp_mode::conflict;
    }
    return channel_estimator && channel_estimator->active() ? pusch_resident_dapp_mode::channel_estimator
                                                            : pusch_resident_dapp_mode::inactive;
  }

  dapp::native_invocation_result
  invoke_channel_estimator(const pusch_resident_dapp_buffers&      buffers,
                           const pusch_demodulator::configuration& config) noexcept override
  {
    try {
      return invoke_channel_estimator_impl(buffers, config);
    } catch (...) {
      if (channel_estimator) {
        channel_estimator->report_fallback(lane, dapp::class_a_fallback_reason::exception);
      }
      return {OCUDU_DAPP_EXCEPTION_V1, true};
    }
  }

  dapp::native_invocation_result invoke_equalizer(const pusch_resident_dapp_buffers&      buffers,
                                                  const pusch_demodulator::configuration& config) noexcept override
  {
    try {
      return invoke_equalizer_impl(buffers, config);
    } catch (...) {
      target->report_fallback(lane, pusch_dapp_demodulator_mode::equalizer, dapp::class_a_fallback_reason::exception);
      return {OCUDU_DAPP_EXCEPTION_V1, true};
    }
  }

  dapp::native_invocation_result invoke_receiver(const pusch_resident_dapp_buffers&      buffers,
                                                 const pusch_demodulator::configuration& config) noexcept override
  {
    receiver_noise_valid = false;
    try {
      return invoke_receiver_impl(buffers, config);
    } catch (...) {
      target->report_fallback(lane, pusch_dapp_demodulator_mode::receiver, dapp::class_a_fallback_reason::exception);
      return {OCUDU_DAPP_EXCEPTION_V1, true};
    }
  }

  bool receiver_post_eq_noise_valid() const noexcept override { return receiver_noise_valid; }

  void on_completion(void* completion_token) noexcept override
  {
    latest_completion.store(completion_token, std::memory_order_release);
    uint32_t pending = pending_callbacks.load(std::memory_order_acquire);
    while (pending != 0 && !pending_callbacks.compare_exchange_weak(
                               pending, pending - 1, std::memory_order_acq_rel, std::memory_order_acquire)) {
    }
  }

  bool on_inline_output_enqueued(uint32_t completion_slot, bool used) noexcept override
  {
    if (completion_slot >= timings.size()) {
      return false;
    }
    auto& timing = timings[completion_slot];
    if (!timing.armed) {
      return true;
    }
    if (!used) {
      timing.armed            = false;
      timing.diagnostic_valid = false;
      return true;
    }
    auto stream = reinterpret_cast<cudaStream_t>(static_cast<uintptr_t>(timing.invocation.execution_stream));
    if (cudaEventRecord(timing.end, stream) != cudaSuccess) {
      static_cast<void>(cudaGetLastError());
      timing.armed = false;
      report_fallback(timing.mode, dapp::class_a_fallback_reason::device);
      return false;
    }
    timing.pending          = true;
    timing.diagnostic_valid = timing.module_end_recorded;
    return true;
  }

  void observe_inline_completion(uint32_t completion_slot) noexcept override
  {
    if (completion_slot >= timings.size()) {
      return;
    }
    auto& timing = timings[completion_slot];
    if (!timing.pending) {
      return;
    }
    const cudaError_t query = cudaEventQuery(timing.end);
    if (query == cudaErrorNotReady) {
      return;
    }
    timing.pending = false;
    timing.armed   = false;
    float elapsed_ms{};
    if (query != cudaSuccess || cudaEventElapsedTime(&elapsed_ms, timing.start, timing.end) != cudaSuccess) {
      static_cast<void>(cudaGetLastError());
      report_fallback(timing.mode, dapp::class_a_fallback_reason::device);
      return;
    }
    const auto observed_latency_ns = static_cast<uint64_t>(static_cast<double>(elapsed_ms) * 1.0e6);
    const auto deadline_ns         = timing.deadline_ns;
    if (observed_latency_ns <= deadline_ns) {
      return;
    }
    if (timing.mode == pusch_resident_dapp_mode::channel_estimator) {
      channel_estimator->report_completion_deadline(lane, timing.invocation, observed_latency_ns, deadline_ns);
    } else {
      target->report_completion_deadline(lane,
                                         timing.mode == pusch_resident_dapp_mode::equalizer
                                             ? pusch_dapp_demodulator_mode::equalizer
                                             : pusch_dapp_demodulator_mode::receiver,
                                         timing.invocation,
                                         observed_latency_ns,
                                         deadline_ns);
    }
  }

  void enable_diagnostic_timing(bool enabled) noexcept override
  {
    diagnostic_timing_enabled.store(enabled, std::memory_order_release);
  }

  resident_dapp_diagnostic_timing diagnostic_timing(uint32_t completion_slot) const noexcept override
  {
    if (completion_slot >= timings.size()) {
      return {};
    }
    const auto& timing = timings[completion_slot];
    return {.module_begin_token = static_cast<void*>(timing.start),
            .module_end_token   = static_cast<void*>(timing.module_end),
            .inline_end_token   = static_cast<void*>(timing.end),
            .completion_slot    = completion_slot,
            .valid              = timing.diagnostic_valid};
  }

  const void*  channel_estimates_cf32() const noexcept override { return device_channel_estimates; }
  const float* channel_noise_f32() const noexcept override { return static_cast<const float*>(device_channel_noise); }
  const float* channel_scheduler_noise_f32() const noexcept override
  {
    return scheduler_noise_valid ? static_cast<const float*>(device_channel_scheduler_noise) : nullptr;
  }

  bool wait_until(std::chrono::steady_clock::time_point deadline_) noexcept override
  {
    while (pending_callbacks.load(std::memory_order_acquire) != 0) {
      if (std::chrono::steady_clock::now() >= deadline_) {
        return false;
      }
      std::this_thread::yield();
    }
    auto event = static_cast<cudaEvent_t>(latest_completion.load(std::memory_order_acquire));
    if (event == nullptr) {
      return true;
    }
    for (;;) {
      const cudaError_t status = cudaEventQuery(event);
      if (status == cudaSuccess) {
        return true;
      }
      if (status != cudaErrorNotReady) {
        static_cast<void>(cudaGetLastError());
        return false;
      }
      if (std::chrono::steady_clock::now() >= deadline_) {
        return false;
      }
      std::this_thread::yield();
    }
  }

private:
  struct timing_record {
    cudaEvent_t              start{};
    cudaEvent_t              module_end{};
    cudaEvent_t              end{};
    ocudu_dapp_invocation_v1 invocation{};
    pusch_resident_dapp_mode mode{pusch_resident_dapp_mode::inactive};
    uint64_t                 deadline_ns{};
    bool                     armed{};
    bool                     pending{};
    bool                     module_end_recorded{};
    bool                     diagnostic_valid{};
  };

  struct prepared_invocation {
    ocudu_dapp_invocation_v1     invocation{};
    ocudu_dapp_tensor_view_v1    grid{};
    ocudu_dapp_pusch_metadata_v1 pusch{};
    ocudu_dapp_dmrs_metadata_v1  dmrs{};
    ocudu_dapp_typed_metadata_v1 pusch_view{};
    ocudu_dapp_typed_metadata_v1 dmrs_view{};
    uint64_t                     generation{};
    uint64_t                     allocation_base{};
  };

  bool begin_timing(uint32_t                        completion_slot,
                    pusch_resident_dapp_mode        mode,
                    const ocudu_dapp_invocation_v1& invocation) noexcept
  {
    if (completion_slot >= timings.size()) {
      return false;
    }
    auto& timing               = timings[completion_slot];
    timing.module_end_recorded = false;
    timing.diagnostic_valid    = false;
    if (timing.pending) {
      observe_inline_completion(completion_slot);
      if (timing.pending) {
        return false;
      }
    }
    auto stream = reinterpret_cast<cudaStream_t>(static_cast<uintptr_t>(invocation.execution_stream));
    if (cudaEventRecord(timing.start, stream) != cudaSuccess) {
      static_cast<void>(cudaGetLastError());
      return false;
    }
    timing.invocation  = invocation;
    timing.mode        = mode;
    timing.deadline_ns = static_cast<uint64_t>(
        (mode == pusch_resident_dapp_mode::channel_estimator ? channel_estimator_deadline : demodulator_deadline)
            .count());
    timing.armed = true;
    return true;
  }

  void record_module_end(uint32_t completion_slot, const dapp::native_invocation_result& result) noexcept
  {
    if (!diagnostic_timing_enabled.load(std::memory_order_acquire) || completion_slot >= timings.size() ||
        result.status != OCUDU_DAPP_OK_V1 || result.invoke_fallback) {
      return;
    }
    auto& timing = timings[completion_slot];
    auto  stream = reinterpret_cast<cudaStream_t>(static_cast<uintptr_t>(timing.invocation.execution_stream));
    if (timing.armed && cudaEventRecord(timing.module_end, stream) == cudaSuccess) {
      timing.module_end_recorded = true;
      return;
    }
    static_cast<void>(cudaGetLastError());
  }

  void report_fallback(pusch_resident_dapp_mode mode, dapp::class_a_fallback_reason reason) noexcept
  {
    if (mode == pusch_resident_dapp_mode::channel_estimator) {
      channel_estimator->report_fallback(lane, reason);
    } else {
      target->report_fallback(lane,
                              mode == pusch_resident_dapp_mode::equalizer ? pusch_dapp_demodulator_mode::equalizer
                                                                          : pusch_dapp_demodulator_mode::receiver,
                              reason);
    }
  }

  static ocudu_dapp_modulation_v1 modulation(modulation_scheme value) noexcept
  {
    switch (value) {
      case modulation_scheme::QPSK:
        return OCUDU_DAPP_MODULATION_QPSK_V1;
      case modulation_scheme::QAM16:
        return OCUDU_DAPP_MODULATION_QAM16_V1;
      case modulation_scheme::QAM64:
        return OCUDU_DAPP_MODULATION_QAM64_V1;
      case modulation_scheme::QAM256:
        return OCUDU_DAPP_MODULATION_QAM256_V1;
      default:
        return OCUDU_DAPP_MODULATION_UNSPECIFIED_V1;
    }
  }

  std::optional<dapp::class_a_fallback_reason> preflight(const pusch_resident_dapp_buffers&      buffers,
                                                         const pusch_demodulator::configuration& config,
                                                         pusch_resident_dapp_mode                mode) const noexcept
  {
    if (buffers.completion_slot >= pusch_resident_dapp_completion_slots) {
      return dapp::class_a_fallback_reason::unsupported_metadata;
    }
    if (config.nof_tx_layers == 0 || config.nof_tx_layers > max_nof_layers) {
      return dapp::class_a_fallback_reason::unsupported_layers;
    }
    if (config.enable_transform_precoding) {
      return dapp::class_a_fallback_reason::unsupported_transform_precoding;
    }
    if (modulation(config.modulation) == OCUDU_DAPP_MODULATION_UNSPECIFIED_V1) {
      return dapp::class_a_fallback_reason::unsupported_modulation;
    }
    if (config.rx_ports.empty() || config.rx_ports.size() > max_nof_rx_ports ||
        buffers.nof_grid_ports != config.rx_ports.size()) {
      return dapp::class_a_fallback_reason::unsupported_rx_ports;
    }
    if (config.rb_mask.count() == 0 || config.rb_mask.find_highest() >= static_cast<int>(max_nof_prb) ||
        buffers.nof_grid_symbols < config.start_symbol_index + config.nof_symbols || buffers.nof_data_re == 0) {
      return dapp::class_a_fallback_reason::unsupported_allocation;
    }
    if (!std::isfinite(config.dmrs_scaling) || config.dmrs_scaling <= 0.0F) {
      return dapp::class_a_fallback_reason::unsupported_dmrs;
    }
    // Missing accelerator hand-off fields: always unsupported for the resident CUDA path.
    if (buffers.grid_cbf16 == nullptr || buffers.data_re_indices == nullptr || buffers.execution_stream == 0) {
      return dapp::class_a_fallback_reason::unsupported_metadata;
    }
    // RAPID / UCI demux / SCH compaction only affect scrambling and post-EQ LLR layout.
    // Equalizer and channel-estimator modules never consume those knobs (NeuralEq is equalizer-
    // only), so rejecting them here blocked every Msg3 (MAC always stamps context.rapid) and
    // hybrid resident-UCI grants from ever invoking the Class-A equalizer. Keep the guard for
    // full-receiver modules, which would need RAPID in the ABI to descramble MsgA/Msg3 and
    // would need matching LLR compaction semantics when device UCI demux is enabled.
    if (mode == pusch_resident_dapp_mode::receiver &&
        (config.n_rapid.has_value() || config.resident.host_uci_demux || config.resident.device_uci_demux ||
         config.resident.sch_compaction.enabled)) {
      return dapp::class_a_fallback_reason::unsupported_metadata;
    }
    return std::nullopt;
  }

  ocudu_dapp_status_v1 prepare(const pusch_resident_dapp_buffers&      buffers,
                               const pusch_demodulator::configuration& config,
                               uint32_t                                worker,
                               std::chrono::nanoseconds                invocation_deadline,
                               prepared_invocation&                    prepared)
  {
    unsigned nof_pilots = 0;
    if (!generate_pilots(config, nof_pilots)) {
      return OCUDU_DAPP_UNSUPPORTED_V1;
    }
    auto stream = reinterpret_cast<cudaStream_t>(static_cast<uintptr_t>(buffers.execution_stream));
    for (unsigned layer = 0; layer != config.nof_tx_layers; ++layer) {
      if (cudaMemcpyAsync(static_cast<cf_t*>(device_references) + layer * nof_pilots,
                          host_references + layer * max_nof_pilots,
                          nof_pilots * sizeof(cf_t),
                          cudaMemcpyHostToDevice,
                          stream) != cudaSuccess) {
        static_cast<void>(cudaGetLastError());
        return OCUDU_DAPP_RESOURCE_V1;
      }
    }
    if (cudaMemcpyAsync(
            device_coordinates, host_coordinates, 2 * nof_pilots * sizeof(uint16_t), cudaMemcpyHostToDevice, stream) !=
        cudaSuccess) {
      static_cast<void>(cudaGetLastError());
      return OCUDU_DAPP_RESOURCE_V1;
    }

    prepared.generation = ++invocation_sequence;
    if (prepared.generation == 0) {
      prepared.generation = ++invocation_sequence;
    }
    prepared.allocation_base = (static_cast<uint64_t>(worker) + 1U) << 8U;

    prepared.pusch.struct_size      = sizeof(prepared.pusch);
    prepared.pusch.abi_major        = OCUDU_DAPP_ABI_MAJOR_V1;
    prepared.pusch.abi_minor        = OCUDU_DAPP_ABI_MINOR_V1;
    prepared.pusch.nof_rx_ports     = buffers.nof_grid_ports;
    prepared.pusch.nof_layers       = config.nof_tx_layers;
    prepared.pusch.modulation       = modulation(config.modulation);
    prepared.pusch.start_symbol     = config.start_symbol_index;
    prepared.pusch.nof_symbols      = config.nof_symbols;
    prepared.pusch.start_subcarrier = static_cast<uint32_t>(config.rb_mask.find_lowest()) * NOF_SUBCARRIERS_PER_RB;
    prepared.pusch.nof_subcarriers =
        static_cast<uint32_t>(config.rb_mask.find_highest() - config.rb_mask.find_lowest() + 1) *
        NOF_SUBCARRIERS_PER_RB;
    prepared.pusch.noise_variance_floor = noise_variance_floor;
    prepared.pusch.flags                = OCUDU_DAPP_PUSCH_DATA_RE_SELECTION_COMPLETE_V1;
    // Preserve physical-frequency metadata independently of the scheduled
    // allocation. Sparse and learned channel estimators must not silently
    // change their delay basis when the scheduler changes the grant width.
    prepared.pusch.reserved[OCUDU_DAPP_PUSCH_RESERVED_RNTI_V1] = static_cast<uint64_t>(to_value(config.rnti));
    prepared.pusch.reserved[OCUDU_DAPP_PUSCH_RESERVED_SUBCARRIER_SPACING_HZ_V1] =
        static_cast<uint64_t>(scs_to_khz(config.slot.scs())) * 1000U;
    prepared.pusch.reserved[OCUDU_DAPP_PUSCH_RESERVED_GRID_NOF_SUBCARRIERS_V1] = buffers.nof_grid_subcarriers;
    if (config.dc_position.has_value()) {
      // Always publish the DC subcarrier so device-side estimators can exclude its pilot; the overlap flag tells
      // the dApp whether it matters for this grant.
      prepared.pusch.reserved[OCUDU_DAPP_PUSCH_RESERVED_DC_SUBCARRIER_PLUS_ONE_V1] =
          static_cast<uint64_t>(*config.dc_position) + 1U;
      if (*config.dc_position >= prepared.pusch.start_subcarrier &&
          *config.dc_position < prepared.pusch.start_subcarrier + prepared.pusch.nof_subcarriers) {
        prepared.pusch.flags |= OCUDU_DAPP_PUSCH_DC_OVERLAP_V1;
      }
    }
    prepared.pusch.nof_data_re     = buffers.nof_data_re;
    prepared.pusch.data_re_indices = make_device_tensor(const_cast<uint32_t*>(buffers.data_re_indices),
                                                        OCUDU_DAPP_ELEMENT_U32_V1,
                                                        OCUDU_DAPP_ACCESS_READ_V1,
                                                        buffers.device_ordinal,
                                                        prepared.allocation_base + 2,
                                                        prepared.generation,
                                                        {buffers.nof_data_re},
                                                        OCUDU_DAPP_LAYOUT_PUSCH_DATA_RE_INDICES_U32_V1);

    prepared.dmrs.struct_size = sizeof(prepared.dmrs);
    prepared.dmrs.abi_major   = OCUDU_DAPP_ABI_MAJOR_V1;
    prepared.dmrs.abi_minor   = OCUDU_DAPP_ABI_MINOR_V1;
    prepared.dmrs.nof_pilots  = nof_pilots;
    prepared.dmrs.flags =
        OCUDU_DAPP_DMRS_REFERENCE_SYMBOLS_LAYER_COMPLETE_V1 | OCUDU_DAPP_DMRS_COORDINATES_SHARED_ACROSS_LAYERS_V1;
    prepared.dmrs.dmrs_type = static_cast<uint32_t>(config.dmrs_type);
    // Publish OFDM-symbol DMRS mask for CUDA equalizers (avoids coords D2H on the hot path).
    {
      uint64_t mask = 0;
      for (unsigned symbol = 0; symbol != MAX_NSYMB_PER_SLOT; ++symbol) {
        if (config.dmrs_symb_pos.test(symbol)) {
          mask |= (uint64_t{1} << symbol);
        }
      }
      prepared.dmrs.reserved[0] = mask;
    }
    prepared.dmrs.reference_symbols = make_device_tensor(static_cast<cf_t*>(device_references),
                                                         OCUDU_DAPP_ELEMENT_CF32_V1,
                                                         OCUDU_DAPP_ACCESS_READ_V1,
                                                         buffers.device_ordinal,
                                                         prepared.allocation_base + 3,
                                                         prepared.generation,
                                                         {config.nof_tx_layers, nof_pilots});
    prepared.dmrs.coordinates       = make_device_tensor(static_cast<uint16_t*>(device_coordinates),
                                                   OCUDU_DAPP_ELEMENT_U16_V1,
                                                   OCUDU_DAPP_ACCESS_READ_V1,
                                                   buffers.device_ordinal,
                                                   prepared.allocation_base + 4,
                                                   prepared.generation,
                                                         {nof_pilots, 2});

    prepared.invocation.struct_size           = sizeof(prepared.invocation);
    prepared.invocation.abi_major             = OCUDU_DAPP_ABI_MAJOR_V1;
    prepared.invocation.abi_minor             = OCUDU_DAPP_ABI_MINOR_V1;
    prepared.invocation.invocation_id         = prepared.generation;
    prepared.invocation.cell_id               = config.cell_id;
    prepared.invocation.worker_id             = worker;
    prepared.invocation.radio_time.sfn        = config.slot.sfn();
    prepared.invocation.radio_time.slot       = config.slot.slot_index();
    prepared.invocation.radio_time.numerology = config.slot.numerology();
    prepared.invocation.deadline_monotonic_ns = deadline_from_now(invocation_deadline);
    prepared.invocation.execution_stream      = buffers.execution_stream;
    prepared.invocation.input_generation      = prepared.generation;
    prepared.invocation.output_generation     = prepared.generation;
    prepared.grid       = make_device_tensor(const_cast<uint32_t*>(static_cast<const uint32_t*>(buffers.grid_cbf16)),
                                       OCUDU_DAPP_ELEMENT_CBF16_V1,
                                       OCUDU_DAPP_ACCESS_READ_V1,
                                       buffers.device_ordinal,
                                       prepared.allocation_base + 1,
                                       prepared.generation,
                                             {buffers.nof_grid_ports, buffers.nof_grid_symbols, buffers.nof_grid_subcarriers},
                                       OCUDU_DAPP_LAYOUT_RESOURCE_GRID_PORT_SYMBOL_SUBCARRIER_V1);
    prepared.pusch_view = make_metadata(OCUDU_DAPP_METADATA_PUSCH_V1, prepared.pusch);
    prepared.dmrs_view  = make_metadata(OCUDU_DAPP_METADATA_DMRS_V1, prepared.dmrs);
    return OCUDU_DAPP_OK_V1;
  }

  dapp::native_invocation_result invoke_channel_estimator_impl(const pusch_resident_dapp_buffers&      buffers,
                                                               const pusch_demodulator::configuration& config)
  {
    if (!channel_estimator || !channel_estimator->active() ||
        channel_estimator->backend_id() != OCUDU_DAPP_BACKEND_CUDA_V1 ||
        target->active_mode() != pusch_dapp_demodulator_mode::inactive) {
      return {OCUDU_DAPP_BYPASS_V1, true};
    }
    if (const auto reason = preflight(buffers, config, pusch_resident_dapp_mode::channel_estimator)) {
      channel_estimator->report_fallback(lane, *reason);
      return {OCUDU_DAPP_UNSUPPORTED_V1, true};
    }
    prepared_invocation prepared;
    const auto          status =
        prepare(buffers, config, channel_estimator->worker_id(lane), channel_estimator_deadline, prepared);
    if (status != OCUDU_DAPP_OK_V1) {
      channel_estimator->report_fallback(lane,
                                         status == OCUDU_DAPP_RESOURCE_V1
                                             ? dapp::class_a_fallback_reason::resource
                                             : dapp::class_a_fallback_reason::unsupported_dmrs);
      return {status, true};
    }

    ocudu_dapp_channel_estimator_input_v1 input{};
    input.invocation = prepared.invocation;
    input.rx_grid    = prepared.grid;
    input.pusch      = prepared.pusch_view;
    input.dmrs       = prepared.dmrs_view;
    ocudu_dapp_channel_estimator_output_v1 output{};
    output.channel_estimates = make_device_tensor(
        static_cast<cf_t*>(device_channel_estimates),
        OCUDU_DAPP_ELEMENT_CF32_V1,
        OCUDU_DAPP_ACCESS_WRITE_V1,
        buffers.device_ordinal,
        prepared.allocation_base + 5,
        prepared.generation,
        {buffers.nof_grid_ports, config.nof_tx_layers, buffers.nof_grid_symbols, buffers.nof_grid_subcarriers},
        OCUDU_DAPP_LAYOUT_PUSCH_CHANNEL_PORT_LAYER_SYMBOL_SUBCARRIER_V1);
    output.noise_variances = make_device_tensor(static_cast<float*>(device_channel_noise),
                                                OCUDU_DAPP_ELEMENT_F32_V1,
                                                OCUDU_DAPP_ACCESS_WRITE_V1,
                                                buffers.device_ordinal,
                                                prepared.allocation_base + 6,
                                                prepared.generation,
                                                {buffers.nof_grid_ports, config.nof_tx_layers});
    const uint64_t scheduler_noise_bytes =
        static_cast<uint64_t>(buffers.nof_grid_ports) * config.nof_tx_layers * sizeof(float);
    output.reserved[OCUDU_DAPP_CHANNEL_ESTIMATOR_SCHED_NOISE_REQUEST_INDEX_V1] =
        OCUDU_DAPP_CHANNEL_ESTIMATOR_SCHED_NOISE_REQUEST_V1;
    output.reserved[OCUDU_DAPP_CHANNEL_ESTIMATOR_SCHED_NOISE_HANDLE_INDEX_V1] =
        reinterpret_cast<uintptr_t>(device_channel_scheduler_noise);
    output.reserved[OCUDU_DAPP_CHANNEL_ESTIMATOR_SCHED_NOISE_BYTES_INDEX_V1]    = scheduler_noise_bytes;
    output.reserved[OCUDU_DAPP_CHANNEL_ESTIMATOR_SCHED_NOISE_RESPONSE_INDEX_V1] = 0;
    scheduler_noise_valid                                                       = false;
    if (!begin_timing(buffers.completion_slot, pusch_resident_dapp_mode::channel_estimator, prepared.invocation)) {
      channel_estimator->report_fallback(lane, dapp::class_a_fallback_reason::device);
      return {OCUDU_DAPP_DEVICE_ERROR_V1, true};
    }
    pending_callbacks.fetch_add(1, std::memory_order_acq_rel);
    const auto result = channel_estimator->invoke(lane, input, output);
    scheduler_noise_valid =
        result.status == OCUDU_DAPP_OK_V1 && !result.invoke_fallback &&
        output.reserved[OCUDU_DAPP_CHANNEL_ESTIMATOR_SCHED_NOISE_REQUEST_INDEX_V1] ==
            OCUDU_DAPP_CHANNEL_ESTIMATOR_SCHED_NOISE_REQUEST_V1 &&
        output.reserved[OCUDU_DAPP_CHANNEL_ESTIMATOR_SCHED_NOISE_HANDLE_INDEX_V1] ==
            reinterpret_cast<uintptr_t>(device_channel_scheduler_noise) &&
        output.reserved[OCUDU_DAPP_CHANNEL_ESTIMATOR_SCHED_NOISE_BYTES_INDEX_V1] == scheduler_noise_bytes &&
        output.reserved[OCUDU_DAPP_CHANNEL_ESTIMATOR_SCHED_NOISE_RESPONSE_INDEX_V1] ==
            OCUDU_DAPP_CHANNEL_ESTIMATOR_SCHED_NOISE_RESPONSE_V1;
    record_module_end(buffers.completion_slot, result);
    return result;
  }

  dapp::native_invocation_result invoke_equalizer_impl(const pusch_resident_dapp_buffers&      buffers,
                                                       const pusch_demodulator::configuration& config)
  {
    if (target->active_mode() != pusch_dapp_demodulator_mode::equalizer ||
        target->backend_id(pusch_dapp_demodulator_mode::equalizer) != OCUDU_DAPP_BACKEND_CUDA_V1) {
      return {OCUDU_DAPP_BYPASS_V1, true};
    }
    if (buffers.equalized_cf32 == nullptr || buffers.post_eq_noise_f32 == nullptr) {
      log_equalizer_diag("fallback", config, buffers, dapp::class_a_fallback_reason::unsupported_metadata);
      target->report_fallback(
          lane, pusch_dapp_demodulator_mode::equalizer, dapp::class_a_fallback_reason::unsupported_metadata);
      return {OCUDU_DAPP_UNSUPPORTED_V1, true};
    }
    if (const auto reason = preflight(buffers, config, pusch_resident_dapp_mode::equalizer)) {
      log_equalizer_diag("fallback", config, buffers, *reason);
      target->report_fallback(lane, pusch_dapp_demodulator_mode::equalizer, *reason);
      return {OCUDU_DAPP_UNSUPPORTED_V1, true};
    }
    prepared_invocation prepared;
    const auto          status = prepare(buffers,
                                config,
                                target->worker_id(lane, pusch_dapp_demodulator_mode::equalizer),
                                demodulator_deadline,
                                prepared);
    if (status != OCUDU_DAPP_OK_V1) {
      const auto reason = status == OCUDU_DAPP_RESOURCE_V1 ? dapp::class_a_fallback_reason::resource
                                                           : dapp::class_a_fallback_reason::unsupported_dmrs;
      log_equalizer_diag("fallback", config, buffers, reason);
      target->report_fallback(lane, pusch_dapp_demodulator_mode::equalizer, reason);
      return {status, true};
    }

    ocudu_dapp_equalizer_input_v1 input{};
    input.invocation = prepared.invocation;
    input.rx_grid    = prepared.grid;
    input.pusch      = prepared.pusch_view;
    input.dmrs       = prepared.dmrs_view;
    ocudu_dapp_equalizer_output_v1 output{};
    output.channel_estimates = make_device_tensor(
        static_cast<cf_t*>(device_channel_estimates),
        OCUDU_DAPP_ELEMENT_CF32_V1,
        OCUDU_DAPP_ACCESS_WRITE_V1,
        buffers.device_ordinal,
        prepared.allocation_base + 5,
        prepared.generation,
        {buffers.nof_grid_ports, config.nof_tx_layers, buffers.nof_grid_symbols, buffers.nof_grid_subcarriers},
        OCUDU_DAPP_LAYOUT_PUSCH_CHANNEL_PORT_LAYER_SYMBOL_SUBCARRIER_V1);
    output.equalized_symbols       = make_device_tensor(static_cast<cf_t*>(buffers.equalized_cf32),
                                                  OCUDU_DAPP_ELEMENT_CF32_V1,
                                                  OCUDU_DAPP_ACCESS_WRITE_V1,
                                                  buffers.device_ordinal,
                                                  prepared.allocation_base + 6,
                                                  prepared.generation,
                                                        {buffers.nof_data_re, config.nof_tx_layers},
                                                  OCUDU_DAPP_LAYOUT_PUSCH_EQUALIZED_DATA_RE_LAYER_V1);
    output.post_equalization_noise = make_device_tensor(static_cast<float*>(buffers.post_eq_noise_f32),
                                                        OCUDU_DAPP_ELEMENT_F32_V1,
                                                        OCUDU_DAPP_ACCESS_WRITE_V1,
                                                        buffers.device_ordinal,
                                                        prepared.allocation_base + 7,
                                                        prepared.generation,
                                                        {buffers.nof_data_re, config.nof_tx_layers},
                                                        OCUDU_DAPP_LAYOUT_PUSCH_POST_EQ_NOISE_DATA_RE_LAYER_V1);
    if (!begin_timing(buffers.completion_slot, pusch_resident_dapp_mode::equalizer, prepared.invocation)) {
      log_equalizer_diag("fallback", config, buffers, dapp::class_a_fallback_reason::device);
      target->report_fallback(lane, pusch_dapp_demodulator_mode::equalizer, dapp::class_a_fallback_reason::device);
      return {OCUDU_DAPP_DEVICE_ERROR_V1, true};
    }
    pending_callbacks.fetch_add(1, std::memory_order_acq_rel);
    const auto result = target->invoke(lane, input, output);
    record_module_end(buffers.completion_slot, result);
    if (result.status == OCUDU_DAPP_OK_V1 && !result.invoke_fallback) {
      log_equalizer_diag("ok", config, buffers);
    } else {
      // Module returned non-OK or asked for fallback — surface the host-classified reason if any.
      const auto reason = result.status == OCUDU_DAPP_DEADLINE_V1       ? dapp::class_a_fallback_reason::deadline
                          : result.status == OCUDU_DAPP_DEVICE_ERROR_V1 ? dapp::class_a_fallback_reason::device
                          : result.status == OCUDU_DAPP_UNSUPPORTED_V1
                              ? dapp::class_a_fallback_reason::module_unsupported
                          : result.status == OCUDU_DAPP_BYPASS_V1 ? dapp::class_a_fallback_reason::module_bypass
                                                                  : dapp::class_a_fallback_reason::returned_error;
      log_equalizer_diag("fallback", config, buffers, reason);
    }
    return result;
  }

  dapp::native_invocation_result invoke_receiver_impl(const pusch_resident_dapp_buffers&      buffers,
                                                      const pusch_demodulator::configuration& config)
  {
    if (target->active_mode() != pusch_dapp_demodulator_mode::receiver ||
        target->backend_id(pusch_dapp_demodulator_mode::receiver) != OCUDU_DAPP_BACKEND_CUDA_V1) {
      return {OCUDU_DAPP_BYPASS_V1, true};
    }
    if (buffers.llrs_f16 == nullptr) {
      target->report_fallback(
          lane, pusch_dapp_demodulator_mode::receiver, dapp::class_a_fallback_reason::unsupported_metadata);
      return {OCUDU_DAPP_UNSUPPORTED_V1, true};
    }
    if (const auto reason = preflight(buffers, config, pusch_resident_dapp_mode::receiver)) {
      target->report_fallback(lane, pusch_dapp_demodulator_mode::receiver, *reason);
      return {OCUDU_DAPP_UNSUPPORTED_V1, true};
    }
    prepared_invocation prepared;
    const auto          status = prepare(buffers,
                                config,
                                target->worker_id(lane, pusch_dapp_demodulator_mode::receiver),
                                demodulator_deadline,
                                prepared);
    if (status != OCUDU_DAPP_OK_V1) {
      target->report_fallback(lane,
                              pusch_dapp_demodulator_mode::receiver,
                              status == OCUDU_DAPP_RESOURCE_V1 ? dapp::class_a_fallback_reason::resource
                                                               : dapp::class_a_fallback_reason::unsupported_dmrs);
      return {status, true};
    }
    ocudu_dapp_receiver_input_v1 input{};
    input.invocation = prepared.invocation;
    input.rx_grid    = prepared.grid;
    input.pusch      = prepared.pusch_view;
    input.dmrs       = prepared.dmrs_view;

    ocudu_dapp_receiver_output_v1 output{};
    output.llrs = make_device_tensor(
        static_cast<uint16_t*>(buffers.llrs_f16),
        OCUDU_DAPP_ELEMENT_F16_V1,
        OCUDU_DAPP_ACCESS_WRITE_V1,
        buffers.device_ordinal,
        prepared.allocation_base + 5,
        prepared.generation,
        {buffers.nof_data_re, config.nof_tx_layers, static_cast<uint64_t>(prepared.pusch.modulation)},
        OCUDU_DAPP_LAYOUT_PUSCH_LLR_DATA_RE_LAYER_BIT_V1);
    ocudu_dapp_receiver_metrics_v1 metrics{};
    if (buffers.post_eq_noise_f32 != nullptr) {
      metrics.struct_size             = sizeof(metrics);
      metrics.abi_major               = OCUDU_DAPP_ABI_MAJOR_V1;
      metrics.abi_minor               = OCUDU_DAPP_ABI_MINOR_V1;
      metrics.post_equalization_noise = make_device_tensor(static_cast<float*>(buffers.post_eq_noise_f32),
                                                           OCUDU_DAPP_ELEMENT_F32_V1,
                                                           OCUDU_DAPP_ACCESS_WRITE_V1,
                                                           buffers.device_ordinal,
                                                           prepared.allocation_base + 6,
                                                           prepared.generation,
                                                           {buffers.nof_data_re, config.nof_tx_layers},
                                                           OCUDU_DAPP_LAYOUT_PUSCH_POST_EQ_NOISE_DATA_RE_LAYER_V1);
      output.metrics                  = make_metadata(OCUDU_DAPP_METADATA_RECEIVER_METRICS_V1, metrics);
    }
    if (!begin_timing(buffers.completion_slot, pusch_resident_dapp_mode::receiver, prepared.invocation)) {
      target->report_fallback(lane, pusch_dapp_demodulator_mode::receiver, dapp::class_a_fallback_reason::device);
      return {OCUDU_DAPP_DEVICE_ERROR_V1, true};
    }
    pending_callbacks.fetch_add(1, std::memory_order_acq_rel);
    const auto result    = target->invoke(lane, input, output);
    receiver_noise_valid = result.status == OCUDU_DAPP_OK_V1 && !result.invoke_fallback &&
                           (output.reserved0 & OCUDU_DAPP_RECEIVER_METRICS_VALID_V1) != 0;
    record_module_end(buffers.completion_slot, result);
    return result;
  }

  bool generate_pilots(const pusch_demodulator::configuration& config, unsigned& nof_pilots)
  {
    const dmrs_pxsch_parameters parameters        = get_pxsch_dmrs_params(config.dmrs_type, 0);
    const dmrs_pxsch_parameters layer1_parameters = get_pxsch_dmrs_params(config.dmrs_type, 1);
    if (config.nof_tx_layers == 2 && layer1_parameters.re_pattern != parameters.re_pattern) {
      return false;
    }
    const unsigned pilots_per_symbol = config.rb_mask.count() * parameters.re_pattern.count();
    unsigned       nof_dmrs_symbols  = 0;
    for (unsigned symbol = config.start_symbol_index; symbol != config.start_symbol_index + config.nof_symbols;
         ++symbol) {
      nof_dmrs_symbols += config.dmrs_symb_pos.test(symbol);
    }
    if (pilots_per_symbol == 0 || nof_dmrs_symbols == 0 ||
        static_cast<size_t>(pilots_per_symbol) * nof_dmrs_symbols > max_nof_pilots ||
        pilots_per_symbol > pilot_sequence.size()) {
      return false;
    }

    nof_pilots                 = 0;
    unsigned dmrs_symbol_index = 0;
    for (unsigned symbol = config.start_symbol_index; symbol != config.start_symbol_index + config.nof_symbols;
         ++symbol) {
      if (!config.dmrs_symb_pos.test(symbol)) {
        continue;
      }
      span<cf_t>     sequence(pilot_sequence.data(), pilots_per_symbol);
      const unsigned c_init = ((get_nsymb_per_slot(config.c_prefix) * config.slot.slot_index() + symbol + 1) *
                                   (2 * config.dmrs_scrambling_id + 1) * pow2(17) +
                               2 * config.dmrs_scrambling_id + (config.n_scid ? 1 : 0)) %
                              pow2(31);
      pilot_generator->init(c_init);
      dmrs_sequence_generate(
          sequence, *pilot_generator, inverse_sqrt_two, 0, get_nof_re_per_prb(config.dmrs_type), config.rb_mask);

      unsigned sequence_index = 0;
      config.rb_mask.for_each(0, config.rb_mask.size(), [&](unsigned rb) {
        parameters.re_pattern.for_each(0, parameters.re_pattern.size(), [&](unsigned re) {
          const cf_t base_reference   = sequence[sequence_index] * config.dmrs_scaling;
          host_references[nof_pilots] = base_reference;
          if (config.nof_tx_layers == 2) {
            host_references[max_nof_pilots + nof_pilots] = base_reference * layer1_parameters.w_f[sequence_index % 2] *
                                                           layer1_parameters.w_t[dmrs_symbol_index % 2];
          }
          ++sequence_index;
          host_coordinates[2 * nof_pilots]     = static_cast<uint16_t>(symbol);
          host_coordinates[2 * nof_pilots + 1] = static_cast<uint16_t>(rb * NOF_SUBCARRIERS_PER_RB + re);
          ++nof_pilots;
        });
      });
      ++dmrs_symbol_index;
    }
    return nof_pilots == pilots_per_symbol * nof_dmrs_symbols;
  }

  std::shared_ptr<pusch_dapp_demodulator_target>                  target;
  std::shared_ptr<pusch_dapp_channel_estimator_target>            channel_estimator;
  uint32_t                                                        lane;
  std::unique_ptr<pseudo_random_generator>                        pilot_generator;
  unsigned                                                        max_nof_prb;
  unsigned                                                        max_nof_rx_ports;
  unsigned                                                        max_nof_layers;
  std::chrono::nanoseconds                                        demodulator_deadline;
  std::chrono::nanoseconds                                        channel_estimator_deadline;
  float                                                           noise_variance_floor;
  size_t                                                          max_nof_pilots;
  size_t                                                          max_nof_channel_elements;
  std::vector<cf_t>                                               pilot_sequence;
  cf_t*                                                           host_references{};
  uint16_t*                                                       host_coordinates{};
  void*                                                           device_references{};
  void*                                                           device_coordinates{};
  void*                                                           device_channel_estimates{};
  void*                                                           device_channel_noise{};
  void*                                                           device_channel_scheduler_noise{};
  bool                                                            scheduler_noise_valid{};
  bool                                                            receiver_noise_valid{};
  uint64_t                                                        invocation_sequence{};
  std::atomic<uint32_t>                                           pending_callbacks{};
  std::atomic<void*>                                              latest_completion{};
  std::atomic<bool>                                               diagnostic_timing_enabled{};
  std::array<timing_record, pusch_resident_dapp_completion_slots> timings{};
};

} // namespace
#endif

std::shared_ptr<pusch_resident_dapp>
ocudu::create_pusch_resident_dapp(std::shared_ptr<pusch_dapp_demodulator_target>       target,
                                  std::shared_ptr<pusch_dapp_channel_estimator_target> channel_estimator,
                                  uint32_t                                             lane,
                                  std::unique_ptr<pseudo_random_generator>             pilot_generator,
                                  unsigned                                             max_nof_prb,
                                  unsigned                                             max_nof_rx_ports,
                                  unsigned                                             max_nof_layers,
                                  std::chrono::nanoseconds                             demodulator_deadline,
                                  float                                                noise_variance_floor,
                                  std::chrono::nanoseconds channel_estimator_deadline) noexcept
{
#ifdef ENABLE_CUDA
  try {
    auto result = std::make_shared<cuda_resident_dapp>(std::move(target),
                                                       std::move(channel_estimator),
                                                       lane,
                                                       std::move(pilot_generator),
                                                       max_nof_prb,
                                                       max_nof_rx_ports,
                                                       max_nof_layers,
                                                       demodulator_deadline,
                                                       noise_variance_floor,
                                                       channel_estimator_deadline);
    if (!result->initialize()) {
      return {};
    }
    std::shared_ptr<dapp::native_completion_barrier> barrier = result;
    return result->register_completion_barrier(barrier) ? std::move(result) : nullptr;
  } catch (...) {
    return {};
  }
#else
  static_cast<void>(target);
  static_cast<void>(channel_estimator);
  static_cast<void>(lane);
  static_cast<void>(pilot_generator);
  static_cast<void>(max_nof_prb);
  static_cast<void>(max_nof_rx_ports);
  static_cast<void>(max_nof_layers);
  static_cast<void>(demodulator_deadline);
  static_cast<void>(noise_variance_floor);
  static_cast<void>(channel_estimator_deadline);
  return {};
#endif
}
