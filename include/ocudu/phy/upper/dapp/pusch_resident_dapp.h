// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "ocudu/dapp/runtime/native_control_context.h"
#include "ocudu/dapp/runtime/native_worker.h"
#include "ocudu/dapp/runtime/pusch_route_table.h"
#include "ocudu/phy/upper/channel_processors/pusch/pusch_demodulator.h"
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace ocudu {

class pseudo_random_generator;
class pusch_dapp_demodulator_target;
class pusch_dapp_channel_estimator_target;
enum class pusch_resident_dapp_mode : uint8_t { inactive, channel_estimator, equalizer, receiver, conflict };
inline constexpr uint32_t pusch_resident_dapp_completion_slots = 3;

/// Host-owned resident buffers exposed at the accelerated PUSCH replacement boundary.
struct pusch_resident_dapp_buffers {
  const void*     grid_cbf16{};
  const uint32_t* data_re_indices{};
  void*           equalized_cf32{};
  void*           post_eq_noise_f32{};
  void*           llrs_f16{};
  uint32_t        nof_grid_ports{};
  uint32_t        nof_grid_symbols{};
  uint32_t        nof_grid_subcarriers{};
  uint32_t        nof_data_re{};
  uint32_t        device_ordinal{};
  uint32_t        completion_slot{};
  uint64_t        execution_stream{};
  /// Complete, untransformed codeword and every host stream that may consume this grant's buffers.
  /// Required only for completion-aware receivers; ordinary receivers ignore these appended fields.
  resident_softbit_buffer codeword{};
  std::array<void*, 3> consumer_streams{};
};

/// Per-lane adapter from OCUDU's accelerated receiver storage to the stable Class-A ABI.
class pusch_resident_dapp
{
public:
  virtual ~pusch_resident_dapp() = default;

  /// Cold installation only: the GPU demodulator supplies an owner validated
  /// against its actual execution stream, before a native receiver is staged.
  virtual bool bind_control_context(std::shared_ptr<dapp::native_control_context>) noexcept { return false; }

  /// Start a serialized grant before reusing any host or device staging buffers, including baseline/CE/EQ grants.
  /// False means a previous receiver still owns those buffers: do not enqueue, reset its state, or fall back.
  /// This does not fence or reclaim pending work; the caller must retain its original dependency/processor.
  virtual bool begin_grant() noexcept { return true; }

  /// Per-UE routing: the route of one grant (primary without a route table).
  virtual dapp::pusch_route resolve_route(uint32_t /*cell_index*/, uint16_t /*rnti*/) const noexcept
  {
    return dapp::pusch_route::primary();
  }
  /// Selects the route of the current grant, after begin_grant() and before mode(); begin_grant()
  /// resets it to the primary route. mode(), the invocations and their fallback and deadline
  /// accounting then use the instance on that route.
  virtual void select_route(const dapp::pusch_route& /*route*/) noexcept {}

  virtual pusch_resident_dapp_mode       mode() const noexcept = 0;
  /// Host admission may reject a grant before invoking the module. Account it
  /// through the same bounded host counters as adapter preflight failures.
  virtual void report_fallback(pusch_resident_dapp_mode, dapp::class_a_fallback_reason) noexcept {}

  /// Snapshot selection once. Unsupported common-feedback profiles use the
  /// conventional CUDA receiver, with exactly one accounted admission skip.
  /// This is not module invocation and must not increment invocation counters.
  pusch_resident_dapp_mode mode_with_pilot_admission(bool common_feedback_required,
                                                   bool pilot_profile_supported) noexcept
  {
    const auto selected = mode();
    if (selected == pusch_resident_dapp_mode::receiver && common_feedback_required && !pilot_profile_supported) {
      report_fallback(selected, dapp::class_a_fallback_reason::unsupported_metadata);
      return pusch_resident_dapp_mode::inactive;
    }
    return selected;
  }

  virtual dapp::native_invocation_result invoke_channel_estimator(const pusch_resident_dapp_buffers&,
                                                                  const pusch_demodulator::configuration&) noexcept
  {
    return {OCUDU_DAPP_BYPASS_V1, true};
  }
  virtual dapp::native_invocation_result invoke_equalizer(const pusch_resident_dapp_buffers&      buffers,
                                                          const pusch_demodulator::configuration& config) noexcept = 0;
  virtual dapp::native_invocation_result invoke_receiver(const pusch_resident_dapp_buffers&      buffers,
                                                         const pusch_demodulator::configuration& config) noexcept  = 0;
  virtual const void*                    channel_estimates_cf32() const noexcept { return nullptr; }
  virtual const float*                   channel_noise_f32() const noexcept { return nullptr; }
  /// Optional scheduler-facing disturbance, distinct from equalizer/LLR noise.
  virtual const float* channel_scheduler_noise_f32() const noexcept { return nullptr; }
  /// True when the most recent receiver invocation filled buffers.post_eq_noise_f32.
  virtual bool receiver_post_eq_noise_valid() const noexcept { return false; }
  /// True only when the most recent successful receiver invocation filled its optional symbol output.
  virtual bool receiver_equalized_symbols_valid() const noexcept { return false; }
  /// Called by the real GPU demodulator when passing its final descriptor to the resident decoder.
  virtual resident_softbit_buffer receiver_softbits(const resident_softbit_buffer& current) const noexcept
  {
    return current;
  }
  /// Decoder has returned and every CPU diagnostic consumer has finished. GPU side streams still need fencing.
  /// Caller retains the original dependency/processor while this returns; no arbitrary owner destruction on L1.
  virtual void receiver_consumers_returned() noexcept {}
  /// Publishes the host-recorded event covering the most recent attempted module enqueue.
  virtual void on_completion(void* completion_token) noexcept = 0;
  /// Records the replacement boundary after the module and required continuation are enqueued.
  virtual bool on_inline_output_enqueued(uint32_t completion_slot, bool used) noexcept
  {
    (void)completion_slot;
    (void)used;
    return true;
  }
  /// Evaluates a completed replacement boundary after the consumer synchronizes its stream dependency.
  virtual void observe_inline_completion(uint32_t completion_slot) noexcept { (void)completion_slot; }
  /// Enables benchmark-only stage timing. Configure this before concurrent invocation begins.
  virtual void enable_diagnostic_timing(bool) noexcept {}
  /// Returns opaque timing tokens for the specified fixed completion slot.
  virtual resident_dapp_diagnostic_timing diagnostic_timing(uint32_t) const noexcept { return {}; }
};

/// Builds a fixed-capacity CUDA adapter. Returns null when CUDA support is not compiled.
std::shared_ptr<pusch_resident_dapp> create_pusch_resident_dapp(
    std::shared_ptr<pusch_dapp_demodulator_target>       target,
    std::shared_ptr<pusch_dapp_channel_estimator_target> channel_estimator,
    uint32_t                                             lane,
    std::unique_ptr<pseudo_random_generator>             pilot_generator,
    unsigned                                             max_nof_prb,
    unsigned                                             max_nof_rx_ports,
    unsigned                                             max_nof_layers,
    std::chrono::nanoseconds                             demodulator_deadline,
    float                                                noise_variance_floor,
    std::chrono::nanoseconds channel_estimator_deadline = std::chrono::microseconds(100)) noexcept;

} // namespace ocudu
