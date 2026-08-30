// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "ocudu/dapp/runtime/native_worker.h"
#include "ocudu/scheduler/dapp/quiet_reservation.h"
#include "ocudu/scheduler/dapp/scheduler_dapp_context.h"
#include <chrono>
#include <cstdint>
#include <memory>

namespace ocudu {

class scheduler_policy;
class slot_point;
struct ul_sched_result;

namespace dapp {
class native_hook_point;
}

struct scheduler_dapp_policy_context {
  uint32_t cell_id;
  uint32_t slice_id;
  uint32_t cell_nof_prbs;
  uint32_t max_candidates;
};

struct scheduler_dapp_guardrails {
  /// Zero disables PRB avoidance. The host clamps this below the cell bandwidth.
  uint32_t max_ul_prbs_avoided{};
  /// Opt-in Class-B authority (default off = baseline-equivalent priority/forbid only).
  bool allow_mcs            = false;
  bool allow_prb_limits     = false;
  bool allow_prb_preference = false;
  bool allow_prb_assignment = false;
  bool allow_tpc            = false;
  bool allow_layers         = false;
  /// Reject MCS intents farther than this from host recommended when host MCS is valid.
  int16_t max_mcs_delta_from_host = 28;
};

enum class scheduler_dapp_policy_mode : uint8_t {
  /// Valid output may replace the conventional candidate ordering and admitted avoidance mask.
  authoritative,
  /// Execute and validate after conventional policy, but never apply dApp output.
  evaluation_only
};

/// Pre-bound Class-B target. A scheduler policy owns one lane and invokes it serially.
class scheduler_dapp_target
{
public:
  virtual ~scheduler_dapp_target() = default;

  virtual bool active() const noexcept = 0;
  /// Cheap gate check for per-slot hot paths; may report true transiently while the
  /// target is binding or draining. Invocation admission still uses active().
  virtual bool                           bound() const noexcept { return active(); }
  virtual uint32_t                       lane_count() const noexcept                             = 0;
  virtual uint32_t                       worker_id(uint32_t lane) const noexcept                 = 0;
  virtual dapp::native_invocation_result invoke(uint32_t                             lane,
                                                const ocudu_dapp_scheduler_input_v1& input,
                                                ocudu_dapp_scheduler_output_v1&      output) noexcept = 0;
  virtual void                           report_invalid_output(uint32_t, const ocudu_dapp_invocation_v1&) noexcept {}
  virtual void record_measurement(uint32_t, const dapp::class_b_policy_measurement&) noexcept {}
};

/// Receives a committed per-cell UL result after scheduling and before MAC forwards it to PHY.
class scheduler_dapp_ul_plan_sink
{
public:
  virtual ~scheduler_dapp_ul_plan_sink()                                                          = default;
  virtual void publish(uint32_t cell_id, slot_point slot, const ul_sched_result& result) noexcept = 0;
};

/// Optional scheduler construction hook. Implementations retain the supplied conventional policy as fallback.
class scheduler_dapp_hook_provider
{
public:
  virtual ~scheduler_dapp_hook_provider() = default;

  virtual std::unique_ptr<scheduler_policy> decorate_scheduler_policy(std::unique_ptr<scheduler_policy> fallback,
                                                                      scheduler_dapp_policy_context     context) = 0;
  virtual void                              publish_ul_plan(uint32_t, slot_point, const ul_sched_result&) noexcept {}
};

/// Decorates one conventional policy with a bounded priority-intent adapter.
std::unique_ptr<scheduler_policy>
create_scheduler_dapp_policy(std::unique_ptr<scheduler_policy>                  fallback,
                             std::shared_ptr<scheduler_dapp_target>             target,
                             uint32_t                                           lane,
                             scheduler_dapp_policy_context                      context,
                             std::shared_ptr<scheduler_dapp_interference_cache> interference = {},
                             scheduler_dapp_guardrails                          guardrails   = {},
                             std::chrono::nanoseconds                   deadline     = std::chrono::microseconds(100),
                             std::shared_ptr<quiet_reservation_manager> reservations = {},
                             scheduler_dapp_policy_mode mode = scheduler_dapp_policy_mode::authoritative) noexcept;

/// Assigns monotonically increasing policy lanes to one Class-B target without modulo reuse.
std::shared_ptr<scheduler_dapp_hook_provider> create_scheduler_dapp_hook_provider(
    std::shared_ptr<scheduler_dapp_target>             target,
    std::shared_ptr<scheduler_dapp_interference_cache> interference = {},
    scheduler_dapp_guardrails                          guardrails   = {},
    std::chrono::nanoseconds                           deadline     = std::chrono::microseconds(100),
    std::shared_ptr<quiet_reservation_manager>         reservations = {},
    std::shared_ptr<scheduler_dapp_ul_plan_sink>       plan_sink    = {},
    scheduler_dapp_policy_mode                         mode = scheduler_dapp_policy_mode::authoritative) noexcept;

/// Adapts an activated native hook point to the scheduler target.
std::shared_ptr<scheduler_dapp_target>
create_native_scheduler_dapp_target(std::shared_ptr<dapp::native_hook_point> point) noexcept;

} // namespace ocudu
