// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear
#include "ocudu/phy/upper/dapp/pusch_dapp_demodulator.h"
#include "ocudu/dapp/runtime/native_hook_point.h"
#include <algorithm>

using namespace ocudu;
namespace {
uint32_t common_lane_count(const std::shared_ptr<dapp::native_hook_point>& equalizer,
                           const std::shared_ptr<dapp::native_hook_point>& receiver) noexcept
{
  if (equalizer && receiver) {
    return std::min(equalizer->lane_count(), receiver->lane_count());
  }
  return equalizer ? equalizer->lane_count() : (receiver ? receiver->lane_count() : 0U);
}

/// One equalizer slot and one receiver slot, mutually exclusive. Either point may be absent; with
/// neither the target is permanently inactive (the stock-receiver view of a routed target).
class native_demodulator_target final : public pusch_dapp_demodulator_target
{
public:
  native_demodulator_target(std::shared_ptr<dapp::native_hook_point> equalizer_,
                            std::shared_ptr<dapp::native_hook_point> receiver_,
                            uint32_t                                 nof_lanes_) :
    equalizer(std::move(equalizer_)), receiver(std::move(receiver_)), nof_lanes(nof_lanes_)
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

  uint32_t lane_count() const noexcept override { return nof_lanes; }

  uint32_t worker_id(uint32_t lane, pusch_dapp_demodulator_mode mode) const noexcept override
  {
    const auto& point = mode == pusch_dapp_demodulator_mode::equalizer ? equalizer : receiver;
    return point ? point->worker_id(lane) : lane;
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
  const uint32_t                           nof_lanes;
};

/// Route-table slots of the equalizer and receiver hooks behind one target.
///
/// Every call without a route is served by the primary view (slot 0 of both hooks, the single-slot
/// behaviour). route() hands out prebuilt views, so a grant's routing allocates nothing: a routed UE
/// sees exactly one slot of one hook, and the stock-receiver view is permanently inactive. Cold
/// per-lane setup (completion barriers, control contexts) reaches every slot of the hook.
class routed_demodulator_target final : public pusch_dapp_demodulator_target
{
public:
  routed_demodulator_target(std::vector<std::shared_ptr<dapp::native_hook_point>> equalizers_,
                            std::vector<std::shared_ptr<dapp::native_hook_point>> receivers_,
                            std::shared_ptr<const dapp::pusch_route_table>        table_,
                            uint32_t                                              nof_lanes) :
    equalizers(std::move(equalizers_)),
    receivers(std::move(receivers_)),
    table(std::move(table_)),
    primary(equalizers.front(), receivers.front(), nof_lanes),
    conventional(nullptr, nullptr, nof_lanes)
  {
    for (std::size_t slot = 0; slot != equalizers.size(); ++slot) {
      equalizer_views.push_back(std::make_unique<native_demodulator_target>(equalizers[slot], nullptr, nof_lanes));
      receiver_views.push_back(std::make_unique<native_demodulator_target>(nullptr, receivers[slot], nof_lanes));
    }
  }

  dapp::pusch_route resolve_route(uint32_t cell_index, uint16_t rnti) const noexcept override
  {
    return table->resolve(cell_index, rnti);
  }

  pusch_dapp_demodulator_target& route(const dapp::pusch_route& selected) noexcept override
  {
    switch (selected.type) {
      case dapp::pusch_route::kind::primary:
        return primary;
      case dapp::pusch_route::kind::conventional:
        return conventional;
      case dapp::pusch_route::kind::instance:
        break;
    }
    if (selected.hook == dapp::pusch_route_hook::equalizer && selected.slot < equalizer_views.size()) {
      return *equalizer_views[selected.slot];
    }
    if (selected.hook == dapp::pusch_route_hook::receiver && selected.slot < receiver_views.size()) {
      return *receiver_views[selected.slot];
    }
    // A channel-estimator route (or a slot this topology does not have) keeps the conventional demodulator.
    return conventional;
  }

  pusch_dapp_demodulator_mode active_mode() const noexcept override { return primary.active_mode(); }
  uint32_t                    lane_count() const noexcept override { return primary.lane_count(); }
  uint32_t                    worker_id(uint32_t lane, pusch_dapp_demodulator_mode mode) const noexcept override
  {
    return primary.worker_id(lane, mode);
  }
  uint32_t backend_id(pusch_dapp_demodulator_mode mode) const noexcept override { return primary.backend_id(mode); }
  bool     receiver_completion_required() const noexcept override { return primary.receiver_completion_required(); }

  bool set_receiver_control_context(uint32_t                                      lane,
                                    std::shared_ptr<dapp::native_control_context> context) noexcept override
  {
    bool ok = true;
    for (const auto& receiver : receivers) {
      ok &= receiver->set_control_context(lane, context);
    }
    return ok;
  }

  bool set_completion_barrier(uint32_t                                         lane,
                              pusch_dapp_demodulator_mode                      mode,
                              std::shared_ptr<dapp::native_completion_barrier> barrier) noexcept override
  {
    const auto& slots = mode == pusch_dapp_demodulator_mode::equalizer ? equalizers : receivers;
    bool        ok    = true;
    for (const auto& point : slots) {
      ok &= point->set_completion_barrier(lane, barrier);
    }
    return ok;
  }

  dapp::native_invocation_result invoke(uint32_t                             lane,
                                        const ocudu_dapp_equalizer_input_v1& input,
                                        ocudu_dapp_equalizer_output_v1&      output) noexcept override
  {
    return primary.invoke(lane, input, output);
  }
  dapp::native_invocation_result invoke(uint32_t                            lane,
                                        const ocudu_dapp_receiver_input_v1& input,
                                        ocudu_dapp_receiver_output_v1&      output) noexcept override
  {
    return primary.invoke(lane, input, output);
  }
  dapp::native_invocation_result invoke_receiver_deferred(uint32_t                              lane,
                                                          const ocudu_dapp_receiver_input_v1&   input,
                                                          ocudu_dapp_receiver_output_v1&        output,
                                                          dapp::native_receiver_ticket&         ticket,
                                                          std::shared_ptr<dapp::native_worker>& owner) noexcept override
  {
    return primary.invoke_receiver_deferred(lane, input, output, ticket, owner);
  }
  void report_invalid_output(uint32_t                        lane,
                             pusch_dapp_demodulator_mode     mode,
                             const ocudu_dapp_invocation_v1& invocation) noexcept override
  {
    primary.report_invalid_output(lane, mode, invocation);
  }
  void report_completion_deadline(uint32_t                        lane,
                                  pusch_dapp_demodulator_mode     mode,
                                  const ocudu_dapp_invocation_v1& invocation,
                                  uint64_t                        observed_latency_ns,
                                  uint64_t                        deadline_ns) noexcept override
  {
    primary.report_completion_deadline(lane, mode, invocation, observed_latency_ns, deadline_ns);
  }
  void report_fallback(uint32_t                      lane,
                       pusch_dapp_demodulator_mode   mode,
                       dapp::class_a_fallback_reason reason) noexcept override
  {
    primary.report_fallback(lane, mode, reason);
  }

private:
  std::vector<std::shared_ptr<dapp::native_hook_point>>   equalizers;
  std::vector<std::shared_ptr<dapp::native_hook_point>>   receivers;
  std::shared_ptr<const dapp::pusch_route_table>          table;
  native_demodulator_target                               primary;
  native_demodulator_target                               conventional;
  std::vector<std::unique_ptr<native_demodulator_target>> equalizer_views;
  std::vector<std::unique_ptr<native_demodulator_target>> receiver_views;
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
    const uint32_t nof_lanes = common_lane_count(equalizer, receiver);
    return std::make_shared<native_demodulator_target>(std::move(equalizer), std::move(receiver), nof_lanes);
  } catch (...) {
    return {};
  }
}

std::shared_ptr<pusch_dapp_demodulator_target>
ocudu::create_native_pusch_dapp_demodulator_target(std::vector<std::shared_ptr<dapp::native_hook_point>> equalizers,
                                                   std::vector<std::shared_ptr<dapp::native_hook_point>> receivers,
                                                   std::shared_ptr<const dapp::pusch_route_table>        table) noexcept
{
  if (table == nullptr || equalizers.empty() || equalizers.size() != receivers.size() ||
      equalizers.size() > table->slots_per_hook()) {
    return {};
  }
  uint32_t nof_lanes = UINT32_MAX;
  for (std::size_t slot = 0; slot != equalizers.size(); ++slot) {
    const auto& equalizer = equalizers[slot];
    const auto& receiver  = receivers[slot];
    if (!equalizer || !receiver || equalizer->interface_id() != OCUDU_DAPP_INTERFACE_EQUALIZER_V1 ||
        receiver->interface_id() != OCUDU_DAPP_INTERFACE_RECEIVER_V1) {
      return {};
    }
    nof_lanes = std::min(nof_lanes, common_lane_count(equalizer, receiver));
  }
  try {
    return std::make_shared<routed_demodulator_target>(
        std::move(equalizers), std::move(receivers), std::move(table), nof_lanes);
  } catch (...) {
    return {};
  }
}
