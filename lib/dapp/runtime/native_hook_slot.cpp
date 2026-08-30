// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/runtime/native_hook_slot.h"
#include <thread>

using namespace ocudu::dapp;

native_hook_slot::reader_guard::reader_guard(native_hook_slot& owner_) noexcept : owner(owner_)
{
  // Sequential consistency establishes this order against clear(): either clear observes this reader, or this reader
  // observes the null target. This prevents a load-before-drain use-after-free window without a hot-path lock.
  owner.readers.fetch_add(1, std::memory_order_seq_cst);
  published_target* publication = owner.target.load(std::memory_order_seq_cst);
  worker                        = publication != nullptr && publication->gate->enabled.load(std::memory_order_acquire)
                                      ? publication->worker.get()
                                      : nullptr;
}

native_hook_slot::reader_guard::~reader_guard()
{
  owner.readers.fetch_sub(1, std::memory_order_seq_cst);
}

native_worker* native_hook_slot::reader_guard::get() const noexcept
{
  return worker;
}

native_hook_slot::~native_hook_slot()
{
  target.store(nullptr, std::memory_order_seq_cst);
  std::lock_guard<std::mutex> lock(control_mutex);
  while (readers.load(std::memory_order_seq_cst) != 0) {
    std::this_thread::yield();
  }
  if (completion_barrier != nullptr) {
    static_cast<void>(completion_barrier->wait_until(std::chrono::steady_clock::time_point::max()));
  }
  owned_target.reset();
}

bool native_hook_slot::set_completion_barrier(std::shared_ptr<native_completion_barrier> barrier_) noexcept
{
  if (!barrier_) {
    return false;
  }
  std::lock_guard<std::mutex> lock(control_mutex);
  if (owned_target != nullptr || completion_barrier != nullptr) {
    return false;
  }
  completion_barrier = std::move(barrier_);
  return true;
}

ocudu_dapp_status_v1 native_hook_slot::bind(std::shared_ptr<native_worker>          worker,
                                            std::shared_ptr<native_activation_gate> gate) noexcept
{
  if (worker == nullptr || gate == nullptr || worker->interface_id() != required_interface_id ||
      gate->enabled.load(std::memory_order_acquire)) {
    return OCUDU_DAPP_INVALID_INPUT_V1;
  }
  std::lock_guard<std::mutex> lock(control_mutex);
  if (owned_target != nullptr) {
    return OCUDU_DAPP_CONFLICT_V1;
  }
  try {
    owned_target = std::make_unique<published_target>(published_target{std::move(worker), std::move(gate)});
  } catch (const std::bad_alloc&) {
    return OCUDU_DAPP_RESOURCE_V1;
  }
  target.store(owned_target.get(), std::memory_order_seq_cst);
  return OCUDU_DAPP_OK_V1;
}

ocudu_dapp_status_v1 native_hook_slot::clear(std::chrono::steady_clock::time_point deadline) noexcept
{
  std::lock_guard<std::mutex> lock(control_mutex);
  target.store(nullptr, std::memory_order_seq_cst);
  while (readers.load(std::memory_order_seq_cst) != 0) {
    if (std::chrono::steady_clock::now() >= deadline) {
      return OCUDU_DAPP_DEADLINE_V1;
    }
    std::this_thread::yield();
  }
  if (completion_barrier != nullptr && !completion_barrier->wait_until(deadline)) {
    return OCUDU_DAPP_DEADLINE_V1;
  }
  owned_target.reset();
  return OCUDU_DAPP_OK_V1;
}

bool native_hook_slot::active() noexcept
{
  reader_guard guard(*this);
  return guard.get() != nullptr;
}

native_invocation_result native_hook_slot::invoke(const ocudu_dapp_channel_estimator_input_v1& input,
                                                  ocudu_dapp_channel_estimator_output_v1&      output) noexcept
{
  reader_guard   guard(*this);
  native_worker* worker = guard.get();
  return worker != nullptr ? worker->invoke(input, output) : native_invocation_result{OCUDU_DAPP_BYPASS_V1, true};
}

native_invocation_result native_hook_slot::invoke(const ocudu_dapp_equalizer_input_v1& input,
                                                  ocudu_dapp_equalizer_output_v1&      output) noexcept
{
  reader_guard   guard(*this);
  native_worker* worker = guard.get();
  return worker != nullptr ? worker->invoke(input, output) : native_invocation_result{OCUDU_DAPP_BYPASS_V1, true};
}

native_invocation_result native_hook_slot::invoke(const ocudu_dapp_receiver_input_v1& input,
                                                  ocudu_dapp_receiver_output_v1&      output) noexcept
{
  reader_guard   guard(*this);
  native_worker* worker = guard.get();
  return worker != nullptr ? worker->invoke(input, output) : native_invocation_result{OCUDU_DAPP_BYPASS_V1, true};
}

native_invocation_result native_hook_slot::invoke(const ocudu_dapp_scheduler_input_v1& input,
                                                  ocudu_dapp_scheduler_output_v1&      output) noexcept
{
  reader_guard   guard(*this);
  native_worker* worker = guard.get();
  return worker != nullptr ? worker->invoke(input, output) : native_invocation_result{OCUDU_DAPP_BYPASS_V1, true};
}

void native_hook_slot::report_failure(const ocudu_dapp_invocation_v1& invocation, ocudu_dapp_status_v1 status) noexcept
{
  reader_guard   guard(*this);
  native_worker* worker = guard.get();
  if (worker != nullptr) {
    worker->report_failure(invocation, status);
  }
}

void native_hook_slot::report_completion_deadline(const ocudu_dapp_invocation_v1& invocation,
                                                  uint64_t                        observed_latency_ns,
                                                  uint64_t                        deadline_ns) noexcept
{
  reader_guard   guard(*this);
  native_worker* worker = guard.get();
  if (worker != nullptr) {
    worker->report_completion_deadline(invocation, observed_latency_ns, deadline_ns);
  }
}

void native_hook_slot::record_class_a_fallback(class_a_fallback_reason reason) noexcept
{
  reader_guard   guard(*this);
  native_worker* worker = guard.get();
  if (worker != nullptr) {
    worker->record_class_a_fallback(reason);
  }
}

void native_hook_slot::record_class_b_policy(const class_b_policy_measurement& measurement) noexcept
{
  reader_guard   guard(*this);
  native_worker* worker = guard.get();
  if (worker != nullptr) {
    worker->record_class_b_policy(measurement);
  }
}

native_invocation_result native_hook_slot::publish(const ocudu_dapp_spectrum_input_v1& input) noexcept
{
  reader_guard   guard(*this);
  native_worker* worker = guard.get();
  return worker != nullptr ? worker->publish(input) : native_invocation_result{OCUDU_DAPP_BYPASS_V1, true};
}

native_invocation_result native_hook_slot::publish(const ocudu_dapp_ul_resource_plan_v1& plan) noexcept
{
  reader_guard   guard(*this);
  native_worker* worker = guard.get();
  return worker != nullptr ? worker->publish(plan) : native_invocation_result{OCUDU_DAPP_BYPASS_V1, true};
}

native_invocation_result native_hook_slot::publish(const ocudu_dapp_srs_isac_input_v1& input) noexcept
{
  reader_guard   guard(*this);
  native_worker* worker = guard.get();
  return worker != nullptr ? worker->publish(input) : native_invocation_result{OCUDU_DAPP_BYPASS_V1, true};
}
