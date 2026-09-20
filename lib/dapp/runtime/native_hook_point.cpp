// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/runtime/native_hook_point.h"
#include "ocudu/dapp/runtime/native_instance.h"
#include <algorithm>
#include <limits>
#include <new>

using namespace ocudu::dapp;

namespace {

constexpr uint32_t max_hook_lanes = 1024;

} // namespace

native_hook_point_create_result
native_hook_point::create(uint32_t interface_id, uint32_t first_worker_id, uint32_t nof_lanes) noexcept
{
  if (interface_id == 0 || nof_lanes == 0 || nof_lanes > max_hook_lanes ||
      first_worker_id > std::numeric_limits<uint16_t>::max() ||
      nof_lanes - 1 > std::numeric_limits<uint16_t>::max() - first_worker_id) {
    return {OCUDU_DAPP_INVALID_INPUT_V1, "invalid native hook lane range", {}};
  }
  try {
    return {OCUDU_DAPP_OK_V1,
            {},
            std::unique_ptr<native_hook_point>(new native_hook_point(interface_id, first_worker_id, nof_lanes))};
  } catch (const std::bad_alloc&) {
    return {OCUDU_DAPP_RESOURCE_V1, "unable to allocate native hook lanes", {}};
  }
}

native_hook_point::native_hook_point(uint32_t interface_id_, uint32_t first_worker_id_, uint32_t nof_lanes) :
  required_interface_id(interface_id_), first_worker(first_worker_id_)
{
  control_contexts.resize(nof_lanes);
  lanes.reserve(nof_lanes);
  for (uint32_t lane = 0; lane != nof_lanes; ++lane) {
    lanes.push_back(std::make_unique<native_hook_slot>(required_interface_id));
  }
}

native_hook_point::~native_hook_point()
{
  if (activation != nullptr) {
    activation->enabled.store(false, std::memory_order_release);
  }
}

ocudu_dapp_status_v1 native_hook_point::activate(native_instance& instance)
{
  std::lock_guard<std::mutex> lock(control_mutex);
  if (activation != nullptr || draining) {
    return OCUDU_DAPP_CONFLICT_V1;
  }
  std::shared_ptr<native_activation_gate> gate;
  try {
    gate = std::make_shared<native_activation_gate>();
  } catch (const std::bad_alloc&) {
    return OCUDU_DAPP_RESOURCE_V1;
  }
  auto status = stage_locked(instance, gate);
  if (status != OCUDU_DAPP_OK_V1) {
    return status;
  }
  gate->enabled.store(true, std::memory_order_release);
  return OCUDU_DAPP_OK_V1;
}

ocudu_dapp_status_v1 native_hook_point::stage(native_instance& instance, std::shared_ptr<native_activation_gate> gate)
{
  std::lock_guard<std::mutex> lock(control_mutex);
  return stage_locked(instance, std::move(gate));
}

ocudu_dapp_status_v1 native_hook_point::stage_locked(native_instance&                        instance,
                                                     std::shared_ptr<native_activation_gate> gate)
{
  if (activation != nullptr || gate == nullptr || draining || gate->enabled.load(std::memory_order_acquire)) {
    return OCUDU_DAPP_CONFLICT_V1;
  }

  std::vector<std::shared_ptr<native_worker>> workers;
  try {
    workers.reserve(lanes.size());
  } catch (const std::bad_alloc&) {
    return OCUDU_DAPP_RESOURCE_V1;
  }
  for (uint32_t lane = 0; lane != lanes.size(); ++lane) {
    auto created = instance.create_worker(required_interface_id, worker_id(lane), control_contexts[lane]);
    if (!created) {
      return created.status;
    }
    workers.push_back(std::move(created.worker));
  }

  const uint32_t backend = workers.front()->backend_id();
  const bool receiver_completion = workers.front()->receiver_completion_required();
  if (std::any_of(
          workers.begin(), workers.end(), [backend, receiver_completion](const auto& worker) {
            return worker->backend_id() != backend || worker->receiver_completion_required() != receiver_completion;
          })) {
    return OCUDU_DAPP_CONFLICT_V1;
  }
  // Gate package backend against the L1 data path this hook is wired into
  // (host PUSCH vs accelerated CUDA PUSCH, host-record Class-B, etc.).
  const uint32_t allowed = allowed_backends.load(std::memory_order_acquire);
  if (allowed != 0) {
    if (backend > 31 || ((allowed & (uint32_t{1} << backend)) == 0)) {
      return OCUDU_DAPP_UNSUPPORTED_V1;
    }
  }

  activation = gate;
  selected_backend.store(backend, std::memory_order_release);
  selected_receiver_completion.store(receiver_completion, std::memory_order_release);
  is_bound.store(true, std::memory_order_release);
  for (uint32_t lane = 0; lane != lanes.size(); ++lane) {
    const auto status = lanes[lane]->bind(std::move(workers[lane]), gate);
    if (status != OCUDU_DAPP_OK_V1) {
      static_cast<void>(clear_locked(gate, std::chrono::steady_clock::now() + std::chrono::seconds(1)));
      return status;
    }
  }
  return OCUDU_DAPP_OK_V1;
}

ocudu_dapp_status_v1 native_hook_point::deactivate(std::chrono::steady_clock::time_point deadline)
{
  std::lock_guard<std::mutex> lock(control_mutex);
  if (activation == nullptr) {
    return OCUDU_DAPP_OK_V1;
  }
  activation->enabled.store(false, std::memory_order_release);
  return clear_locked(activation, deadline);
}

ocudu_dapp_status_v1 native_hook_point::clear(const std::shared_ptr<native_activation_gate>& gate,
                                              std::chrono::steady_clock::time_point          deadline)
{
  std::lock_guard<std::mutex> lock(control_mutex);
  return clear_locked(gate, deadline);
}

ocudu_dapp_status_v1 native_hook_point::clear_locked(const std::shared_ptr<native_activation_gate>& gate,
                                                     std::chrono::steady_clock::time_point          deadline)
{
  if (activation == nullptr) {
    return OCUDU_DAPP_OK_V1;
  }
  if (activation != gate) {
    return OCUDU_DAPP_CONFLICT_V1;
  }
  gate->enabled.store(false, std::memory_order_release);
  ocudu_dapp_status_v1 status = OCUDU_DAPP_OK_V1;
  for (auto& lane : lanes) {
    const auto lane_status = lane->clear(deadline);
    if (lane_status != OCUDU_DAPP_OK_V1) {
      status = lane_status;
    }
  }
  draining = status != OCUDU_DAPP_OK_V1;
  if (!draining) {
    is_bound.store(false, std::memory_order_release);
    selected_backend.store(OCUDU_DAPP_BACKEND_HOST_V1, std::memory_order_release);
    selected_receiver_completion.store(false, std::memory_order_release);
    activation.reset();
  }
  return status;
}

bool native_hook_point::active() const noexcept
{
  return !lanes.empty() && lanes.front()->active();
}

bool native_hook_point::set_control_context(uint32_t lane, std::shared_ptr<native_control_context> context) noexcept
{
  std::lock_guard<std::mutex> lock(control_mutex);
  if (!context || !context->identity() || lane >= control_contexts.size() || activation || draining || control_contexts[lane])
    return false;
  control_contexts[lane] = std::move(context);
  return true;
}

bool native_hook_point::set_completion_barrier(uint32_t                                   lane,
                                               std::shared_ptr<native_completion_barrier> barrier) noexcept
{
  return lane < lanes.size() && lanes[lane]->set_completion_barrier(std::move(barrier));
}

native_invocation_result native_hook_point::invoke(uint32_t                                     lane,
                                                   const ocudu_dapp_channel_estimator_input_v1& input,
                                                   ocudu_dapp_channel_estimator_output_v1&      output) noexcept
{
  return dispatch(lane, [&](native_hook_slot& slot) { return slot.invoke(input, output); });
}

native_invocation_result native_hook_point::invoke(uint32_t                             lane,
                                                   const ocudu_dapp_equalizer_input_v1& input,
                                                   ocudu_dapp_equalizer_output_v1&      output) noexcept
{
  return dispatch(lane, [&](native_hook_slot& slot) { return slot.invoke(input, output); });
}

native_invocation_result native_hook_point::invoke(uint32_t                            lane,
                                                   const ocudu_dapp_receiver_input_v1& input,
                                                   ocudu_dapp_receiver_output_v1&      output) noexcept
{
  return dispatch(lane, [&](native_hook_slot& slot) { return slot.invoke(input, output); });
}

native_invocation_result native_hook_point::invoke(uint32_t                             lane,
                                                   const ocudu_dapp_scheduler_input_v1& input,
                                                   ocudu_dapp_scheduler_output_v1&      output) noexcept
{
  return dispatch(lane, [&](native_hook_slot& slot) { return slot.invoke(input, output); });
}

native_invocation_result native_hook_point::invoke_receiver_deferred(
    uint32_t lane, const ocudu_dapp_receiver_input_v1& input, ocudu_dapp_receiver_output_v1& output,
    native_receiver_ticket& ticket, std::shared_ptr<native_worker>& owner) noexcept
{
  return dispatch(lane, [&](native_hook_slot& slot) { return slot.invoke_receiver_deferred(input, output, ticket, owner); });
}

void native_hook_point::report_failure(uint32_t                        lane,
                                       const ocudu_dapp_invocation_v1& invocation,
                                       ocudu_dapp_status_v1            status) noexcept
{
  if (lane < lanes.size()) {
    lanes[lane]->report_failure(invocation, status);
  }
}

void native_hook_point::report_completion_deadline(uint32_t                        lane,
                                                   const ocudu_dapp_invocation_v1& invocation,
                                                   uint64_t                        observed_latency_ns,
                                                   uint64_t                        deadline_ns) noexcept
{
  if (lane < lanes.size()) {
    lanes[lane]->report_completion_deadline(invocation, observed_latency_ns, deadline_ns);
  }
}

void native_hook_point::record_class_a_fallback(uint32_t lane, class_a_fallback_reason reason) noexcept
{
  if (lane < lanes.size()) {
    lanes[lane]->record_class_a_fallback(reason);
  }
}

void native_hook_point::record_class_b_policy(uint32_t lane, const class_b_policy_measurement& measurement) noexcept
{
  if (lane < lanes.size()) {
    lanes[lane]->record_class_b_policy(measurement);
  }
}

native_invocation_result native_hook_point::publish(uint32_t lane, const ocudu_dapp_spectrum_input_v1& input) noexcept
{
  return dispatch(lane, [&](native_hook_slot& slot) { return slot.publish(input); });
}

native_invocation_result native_hook_point::publish(uint32_t lane, const ocudu_dapp_ul_resource_plan_v1& plan) noexcept
{
  return dispatch(lane, [&](native_hook_slot& slot) { return slot.publish(plan); });
}

native_invocation_result native_hook_point::publish(uint32_t lane, const ocudu_dapp_srs_isac_input_v1& input) noexcept
{
  return dispatch(lane, [&](native_hook_slot& slot) { return slot.publish(input); });
}
