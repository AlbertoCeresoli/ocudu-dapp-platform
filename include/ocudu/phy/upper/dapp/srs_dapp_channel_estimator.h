// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "ocudu/dapp/runtime/native_worker.h"
#include <chrono>
#include <cstdint>
#include <memory>

namespace ocudu {

class low_papr_sequence_generator_factory;
class srs_estimator_factory;

namespace dapp {
class native_hook_point;
}

/// Pre-bound Class-A SRS channel-estimator target. Each lane is entered by at most one SRS estimator object.
class srs_dapp_channel_estimator_target
{
public:
  virtual ~srs_dapp_channel_estimator_target() = default;

  virtual bool                           active() const noexcept                                             = 0;
  virtual uint32_t                       lane_count() const noexcept                                         = 0;
  virtual uint32_t                       worker_id(uint32_t lane) const noexcept                             = 0;
  virtual uint32_t                       backend_id() const noexcept                                         = 0;
  virtual dapp::native_invocation_result invoke(uint32_t                                         lane,
                                                const ocudu_dapp_srs_channel_estimator_input_v1& input,
                                                ocudu_dapp_srs_channel_estimator_output_v1&      output) noexcept = 0;
  virtual void report_invalid_output(uint32_t lane, const ocudu_dapp_invocation_v1& invocation) noexcept     = 0;
  virtual void report_fallback(uint32_t lane, dapp::class_a_fallback_reason reason) noexcept                 = 0;
  /// Soft completion wall of an accelerator invocation: the module's queued work outlived \p deadline_ns.
  virtual void report_completion_deadline(uint32_t                        lane,
                                          const ocudu_dapp_invocation_v1& invocation,
                                          uint64_t                        observed_latency_ns,
                                          uint64_t                        deadline_ns) noexcept = 0;
};

struct srs_dapp_channel_estimator_config {
  /// Lanes of the target handed out, one per SRS estimator object; further estimators keep the host estimator.
  unsigned nof_lanes;
  /// Uplink bandwidth, sizing the per-lane workspace.
  unsigned max_nof_prb;
  /// Invocation deadline handed to the module; the host falls back when the call returns later.
  std::chrono::nanoseconds deadline{std::chrono::milliseconds(1)};
  /// Soft wall on the completion of a CUDA module's queued work and the readback: a late result is still used and
  /// counted in class_a.completion.deadline_misses.
  std::chrono::nanoseconds completion_deadline{std::chrono::milliseconds(1)};
};

/// \brief Creates the Class-A SRS channel-estimator adapter.
///
/// Each estimator the factory creates wraps one estimator of \p fallback. While the target is active, an SRS
/// occasion within the module's profile is estimated by the module: the adapter extracts the SRS resource
/// elements (from the host grid for a host package, from the owned device snapshot for a CUDA package), generates
/// the reference sequences, invokes the module and maps its outputs to an srs_estimator_result. Any other occasion,
/// and any failed invocation, is estimated by the fallback.
std::shared_ptr<srs_estimator_factory>
create_srs_dapp_channel_estimator_factory(std::shared_ptr<srs_estimator_factory>               fallback,
                                          std::shared_ptr<srs_dapp_channel_estimator_target>   target,
                                          std::shared_ptr<low_papr_sequence_generator_factory> low_papr_factory,
                                          srs_dapp_channel_estimator_config                    config) noexcept;

/// Adapts an activated native hook point of OCUDU_DAPP_INTERFACE_SRS_CHANNEL_ESTIMATOR_V1 to the adapter target.
std::shared_ptr<srs_dapp_channel_estimator_target>
create_native_srs_dapp_channel_estimator_target(std::shared_ptr<dapp::native_hook_point> point) noexcept;

} // namespace ocudu
