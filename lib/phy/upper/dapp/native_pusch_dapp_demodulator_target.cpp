// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear
#include "ocudu/phy/upper/dapp/pusch_dapp_demodulator.h"
#include "ocudu/dapp/runtime/native_hook_point.h"
#include <algorithm>

using namespace ocudu;
namespace {
class native_demodulator_target final : public pusch_dapp_demodulator_target
{
public:
  native_demodulator_target(std::shared_ptr<dapp::native_hook_point> equalizer_,
                            std::shared_ptr<dapp::native_hook_point> receiver_) :
    equalizer(std::move(equalizer_)), receiver(std::move(receiver_))
  {
  }

  pusch_dapp_demodulator_mode active_mode() const noexcept override
  {
    const bool equalizer_active = equalizer && equalizer->active();
    const bool receiver_active  = receiver && receiver->active();
    if (equalizer_active && receiver_active) {
      return pusch_dapp_demodulator_mode::conflict;
    }
    if (receiver_active) {
      return pusch_dapp_demodulator_mode::receiver;
    }
    return equalizer_active ? pusch_dapp_demodulator_mode::equalizer : pusch_dapp_demodulator_mode::inactive;
  }

  uint32_t lane_count() const noexcept override
  {
    if (equalizer && receiver) {
      return std::min(equalizer->lane_count(), receiver->lane_count());
    }
    return equalizer ? equalizer->lane_count() : receiver->lane_count();
  }

  uint32_t worker_id(uint32_t lane, pusch_dapp_demodulator_mode mode) const noexcept override
  {
    return mode == pusch_dapp_demodulator_mode::equalizer ? equalizer->worker_id(lane) : receiver->worker_id(lane);
  }

  uint32_t backend_id(pusch_dapp_demodulator_mode mode) const noexcept override
  {
    const auto& point = mode == pusch_dapp_demodulator_mode::equalizer ? equalizer : receiver;
    return point ? point->backend_id() : OCUDU_DAPP_BACKEND_HOST_V1;
  }

  bool receiver_completion_required() const noexcept override
  {
    return receiver && receiver->receiver_completion_required();
  }

  bool set_receiver_control_context(uint32_t lane, std::shared_ptr<dapp::native_control_context> context) noexcept override
  {
    return !receiver || receiver->set_control_context(lane, std::move(context));
  }

  bool set_completion_barrier(uint32_t                                         lane,
                              pusch_dapp_demodulator_mode                      mode,
                              std::shared_ptr<dapp::native_completion_barrier> barrier) noexcept override
  {
    auto& point = mode == pusch_dapp_demodulator_mode::equalizer ? equalizer : receiver;
    return point && point->set_completion_barrier(lane, std::move(barrier));
  }

  dapp::native_invocation_result invoke(uint32_t                             lane,
                                        const ocudu_dapp_equalizer_input_v1& input,
                                        ocudu_dapp_equalizer_output_v1&      output) noexcept override
  {
    return equalizer ? equalizer->invoke(lane, input, output)
                     : dapp::native_invocation_result{OCUDU_DAPP_BYPASS_V1, true};
  }

  dapp::native_invocation_result invoke(uint32_t                            lane,
                                        const ocudu_dapp_receiver_input_v1& input,
                                        ocudu_dapp_receiver_output_v1&      output) noexcept override
  {
    return receiver ? receiver->invoke(lane, input, output)
                    : dapp::native_invocation_result{OCUDU_DAPP_BYPASS_V1, true};
  }

  dapp::native_invocation_result
  invoke_receiver_deferred(uint32_t lane,
                           const ocudu_dapp_receiver_input_v1& input,
                           ocudu_dapp_receiver_output_v1& output,
                           dapp::native_receiver_ticket& ticket,
                           std::shared_ptr<dapp::native_worker>& owner) noexcept override
  {
    return receiver ? receiver->invoke_receiver_deferred(lane, input, output, ticket, owner)
                    : dapp::native_invocation_result{OCUDU_DAPP_BYPASS_V1, true};
  }

  void report_invalid_output(uint32_t                        lane,
                             pusch_dapp_demodulator_mode     mode,
                             const ocudu_dapp_invocation_v1& invocation_) noexcept override
  {
    auto& point = mode == pusch_dapp_demodulator_mode::equalizer ? equalizer : receiver;
    if (point) {
      point->report_failure(lane, invocation_, OCUDU_DAPP_INVALID_OUTPUT_V1);
    }
  }

  void report_completion_deadline(uint32_t                        lane,
                                  pusch_dapp_demodulator_mode     mode,
                                  const ocudu_dapp_invocation_v1& invocation,
                                  uint64_t                        observed_latency_ns,
                                  uint64_t                        deadline_ns) noexcept override
  {
    auto& point = mode == pusch_dapp_demodulator_mode::equalizer ? equalizer : receiver;
    if (point) {
      point->report_completion_deadline(lane, invocation, observed_latency_ns, deadline_ns);
    }
  }

  void report_fallback(uint32_t                      lane,
                       pusch_dapp_demodulator_mode   mode,
                       dapp::class_a_fallback_reason reason) noexcept override
  {
    if (mode == pusch_dapp_demodulator_mode::conflict) {
      if (equalizer) {
        equalizer->record_class_a_fallback(lane, reason);
      }
      if (receiver) {
        receiver->record_class_a_fallback(lane, reason);
      }
      return;
    }
    auto& point = mode == pusch_dapp_demodulator_mode::equalizer ? equalizer : receiver;
    if (point) {
      point->record_class_a_fallback(lane, reason);
    }
  }

private:
  std::shared_ptr<dapp::native_hook_point> equalizer;
  std::shared_ptr<dapp::native_hook_point> receiver;
};

} // namespace

std::shared_ptr<pusch_dapp_demodulator_target>
ocudu::create_native_pusch_dapp_demodulator_target(std::shared_ptr<dapp::native_hook_point> equalizer,
                                                   std::shared_ptr<dapp::native_hook_point> receiver) noexcept
{
  if ((!equalizer && !receiver) || (equalizer && equalizer->interface_id() != OCUDU_DAPP_INTERFACE_EQUALIZER_V1) ||
      (receiver && receiver->interface_id() != OCUDU_DAPP_INTERFACE_RECEIVER_V1)) {
    return {};
  }
  try {
    return std::make_shared<native_demodulator_target>(std::move(equalizer), std::move(receiver));
  } catch (...) {
    return {};
  }
}
