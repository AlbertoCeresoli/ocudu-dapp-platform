// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "ocudu/dapp/runtime/native_completion_barrier.h"
#include "ocudu/dapp/runtime/native_worker.h"
#include "ocudu/dapp/runtime/native_control_context.h"
#include <chrono>
#include <cstdint>
#include <memory>

namespace ocudu {

class demodulation_mapper_factory;
class pseudo_random_generator_factory;
class pusch_demodulator_factory;
class pusch_dapp_channel_estimator_target;

namespace dapp {
class native_hook_point;
}

enum class pusch_dapp_demodulator_mode : uint8_t { inactive, equalizer, receiver, conflict };

/// Pre-bound Class-A CE+EQ/receiver target. One PUSCH dependency object enters each lane at most once concurrently.
class pusch_dapp_demodulator_target
{
public:
  virtual ~pusch_dapp_demodulator_target() = default;

  virtual pusch_dapp_demodulator_mode active_mode() const noexcept                                              = 0;
  virtual uint32_t                    lane_count() const noexcept                                               = 0;
  virtual uint32_t                    worker_id(uint32_t lane, pusch_dapp_demodulator_mode mode) const noexcept = 0;
  virtual uint32_t backend_id(pusch_dapp_demodulator_mode) const noexcept { return OCUDU_DAPP_BACKEND_HOST_V1; }
  /// Advisory routing only: invocation independently verifies the bound worker's required contract.
  virtual bool receiver_completion_required() const noexcept { return false; }
  virtual bool set_receiver_control_context(uint32_t, std::shared_ptr<dapp::native_control_context>) noexcept
  {
    return true; // Non-native test/adaptor targets have no module lifecycle to bind.
  }
  virtual bool     set_completion_barrier(uint32_t,
                                          pusch_dapp_demodulator_mode,
                                          std::shared_ptr<dapp::native_completion_barrier>) noexcept
  {
    return true;
  }

  virtual dapp::native_invocation_result invoke(uint32_t                             lane,
                                                const ocudu_dapp_equalizer_input_v1& input,
                                                ocudu_dapp_equalizer_output_v1&      output) noexcept = 0;
  virtual dapp::native_invocation_result
  invoke(uint32_t lane, const ocudu_dapp_receiver_input_v1& input, ocudu_dapp_receiver_output_v1& output) noexcept = 0;
  /// Completion-aware dispatch is a separate contract: never silently call ordinary invoke().
  /// Before dispatch rejection leaves ticket/owner untouched. A possibly submitted operation must retain its
  /// original worker and ticket, and must forbid fallback. Caller retains tensor/metadata storage independently.
  virtual dapp::native_invocation_result
  invoke_receiver_deferred(uint32_t,
                           const ocudu_dapp_receiver_input_v1&,
                           ocudu_dapp_receiver_output_v1&,
                           dapp::native_receiver_ticket&,
                           std::shared_ptr<dapp::native_worker>&) noexcept
  {
    return {OCUDU_DAPP_UNSUPPORTED_V1, true};
  }
  virtual void report_invalid_output(uint32_t, pusch_dapp_demodulator_mode, const ocudu_dapp_invocation_v1&) noexcept {}
  virtual void report_completion_deadline(uint32_t,
                                          pusch_dapp_demodulator_mode,
                                          const ocudu_dapp_invocation_v1&,
                                          uint64_t,
                                          uint64_t) noexcept
  {
  }
  virtual void report_fallback(uint32_t, pusch_dapp_demodulator_mode, dapp::class_a_fallback_reason) noexcept {}
};

struct pusch_dapp_demodulator_config {
  /// Total number of dependency objects, including the regular and UCI-only PUSCH pools.
  unsigned                 nof_lanes;
  unsigned                 max_nof_prb;
  unsigned                 max_nof_rx_ports;
  unsigned                 max_nof_layers;
  // Soft wall for one Class-A equalizer/receiver invoke (enqueue through GPU
  // completion). Not a 3GPP slot deadline. 500 µs (one 30 kHz slot) and then
  // 1000 µs both still classified good PBC results as late under 4-UE OTA
  // contention; 2000 µs keeps the tripwire without disabling a working EQ.
  // Tests may inject a tighter budget.
  std::chrono::nanoseconds demodulator_deadline{std::chrono::microseconds(2000)};
  // The CUDA CE completion measurement also includes same-stream host consumer
  // work after the dApp returns. Keep a bounded soft wall, but allow the shared
  // GPU one 30 kHz slot plus 300 us of scheduling jitter before declaring a miss.
  std::chrono::nanoseconds channel_estimator_deadline{std::chrono::microseconds(800)};
  float                    noise_variance_floor{1.0e-6F};
};

/// Creates an initial one-layer CPU Class-A CE+EQ/receiver adapter with fixed state per demodulator object.
std::shared_ptr<pusch_demodulator_factory> create_pusch_dapp_demodulator_factory(
    std::shared_ptr<pusch_demodulator_factory>           fallback,
    std::shared_ptr<pusch_dapp_demodulator_target>       target,
    std::shared_ptr<pseudo_random_generator_factory>     prg_factory,
    std::shared_ptr<demodulation_mapper_factory>         demapper_factory,
    pusch_dapp_demodulator_config                        config,
    std::shared_ptr<pusch_dapp_channel_estimator_target> channel_estimator = {}) noexcept;

/// Combines optional equalizer and receiver hook points behind one mutually exclusive PUSCH target.
std::shared_ptr<pusch_dapp_demodulator_target>
create_native_pusch_dapp_demodulator_target(std::shared_ptr<dapp::native_hook_point> equalizer,
                                            std::shared_ptr<dapp::native_hook_point> receiver) noexcept;

} // namespace ocudu
