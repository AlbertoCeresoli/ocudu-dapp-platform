// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear
#include "ocudu/phy/upper/dapp/native_receiver_resident_adapter.h"
#include "ocudu/dapp/runtime/abi_validation.h"
#include <exception>
#include <limits>

using namespace ocudu;
namespace {
class try_guard
{
public:
  explicit try_guard(std::atomic_flag& flag_) noexcept :
    flag(flag_), acquired(!flag.test_and_set(std::memory_order_acquire))
  {
  }
  ~try_guard()
  {
    if (acquired)
      flag.clear(std::memory_order_release);
  }
  explicit operator bool() const noexcept { return acquired; }

private:
  std::atomic_flag& flag;
  bool              acquired;
};
} // namespace

native_receiver_resident_adapter::~native_receiver_resident_adapter()
{
  // Never implicitly free an asynchronous loan or run CUDA cleanup on an arbitrary destructor thread.
  for (const auto& slot : records) {
    if (slot.tensors || slot.worker) {
      std::terminate();
    }
  }
}

bool native_receiver_resident_adapter::same_codeword(const resident_softbit_buffer& a,
                                                     const resident_softbit_buffer& b) noexcept
{
  return a.data == b.data && a.nof_softbits == b.nof_softbits && a.execution_context == b.execution_context &&
         a.completion_token == b.completion_token && a.valid == b.valid && a.n_id == b.n_id && a.rnti == b.rnti &&
         a.buffer_index == b.buffer_index && a.acceptance_required == b.acceptance_required &&
         a.completion_context == b.completion_context && a.accept_completion == b.accept_completion &&
         a.completion_generation == b.completion_generation;
}

bool native_receiver_resident_adapter::valid_codeword(const ocudu_dapp_receiver_input_v1&  input,
                                                      const ocudu_dapp_receiver_output_v1& output,
                                                      const resident_softbit_buffer&       codeword) noexcept
{
  const auto& view = output.llrs;
  if (!codeword.valid || !codeword.data || !codeword.execution_context || !codeword.nof_softbits ||
      codeword.buffer_index < 0 || codeword.acceptance_required || codeword.completion_context ||
      codeword.accept_completion || codeword.completion_generation ||
      dapp::validate_tensor_view(view) != dapp::validation_error::none ||
      (view.access & OCUDU_DAPP_ACCESS_WRITE_V1) == 0 || view.domain != OCUDU_DAPP_MEM_ACCELERATOR_DEVICE_V1 ||
      view.backend_id != OCUDU_DAPP_BACKEND_CUDA_V1 || !view.allocation_id || !view.allocation_generation ||
      !view.lease_token || view.element_type != OCUDU_DAPP_ELEMENT_F16_V1 || view.rank != 3 ||
      view.layout_id != OCUDU_DAPP_LAYOUT_PUSCH_LLR_DATA_RE_LAYER_BIT_V1 || view.layout_version != 1 ||
      !view.dimensions[0] || view.dimensions[0] > 14U * 275U * 12U || !view.dimensions[1] || view.dimensions[1] > 4 ||
      (view.dimensions[2] != 2 && view.dimensions[2] != 4 && view.dimensions[2] != 6 && view.dimensions[2] != 8) ||
      view.native_handle > UINT64_MAX - view.byte_offset ||
      reinterpret_cast<uintptr_t>(codeword.data) != view.native_handle + view.byte_offset ||
      reinterpret_cast<uintptr_t>(codeword.execution_context) != input.invocation.execution_stream) {
    return false;
  }
  const uint64_t bits = view.dimensions[0] * view.dimensions[1] * view.dimensions[2];
  return codeword.nof_softbits == bits && view.byte_length >= bits * sizeof(uint16_t) &&
         view.byte_strides[2] == sizeof(uint16_t) && view.byte_strides[1] == view.dimensions[2] * sizeof(uint16_t) &&
         view.byte_strides[0] == view.dimensions[1] * view.byte_strides[1];
}

dapp::native_invocation_result native_receiver_resident_adapter::submit(pusch_dapp_demodulator_target&      target,
                                                                        uint32_t                            lane,
                                                                        const ocudu_dapp_receiver_input_v1& input,
                                                                        ocudu_dapp_receiver_output_v1&      output,
                                                                        resident_softbit_buffer&            codeword,
                                                                        pusch_resource_loan& tensors) noexcept
{
  try_guard lock(gate);
  if (!lock) {
    return {OCUDU_DAPP_RESOURCE_V1, true};
  }
  if (stopped.load(std::memory_order_acquire)) {
    return {OCUDU_DAPP_BYPASS_V1, true};
  }
  if (!tensors || !valid_codeword(input, output, codeword)) {
    return {OCUDU_DAPP_INVALID_INPUT_V1, true};
  }
  record* available = nullptr;
  for (auto& slot : records) {
    if (!slot.tensors) {
      available = &slot;
      break;
    }
  }
  if (!available || generation == std::numeric_limits<uint64_t>::max()) {
    return {OCUDU_DAPP_RESOURCE_V1, true};
  }
  auto& slot        = *available;
  slot.owner        = this;
  slot.tensors      = std::move(tensors);
  const auto result = target.invoke_receiver_deferred(lane, input, output, slot.ticket, slot.worker);
  if (!slot.ticket.sequence) {
    // Native dispatch guarantees no possible submission without a retained ticket/owner.
    tensors = std::move(slot.tensors);
    return result;
  }
  if (!slot.worker || result.invoke_fallback) {
    std::terminate(); // Violation of the trusted native runtime's ownership contract.
  }
  if (result.status != OCUDU_DAPP_OK_V1) {
    slot.quarantined = true;
    stop();
    return result;
  }
  slot.expected                     = codeword;
  slot.expected.acceptance_required = true;
  slot.expected.completion_context  = &slot;
  slot.expected.accept_completion   = &accept;
  // Adapter lifetime identity, not module sequence: protects slot reuse across module replacements too.
  slot.expected.completion_generation = ++generation;
  codeword                            = slot.expected;
  return result;
}

bool native_receiver_resident_adapter::accept(void* context, const resident_softbit_buffer& codeword) noexcept
{
  auto&     slot = *static_cast<record*>(context);
  try_guard lock(slot.owner->gate);
  if (!lock || !slot.tensors || !slot.worker || slot.quarantined || slot.checked ||
      !same_codeword(codeword, slot.expected)) {
    return false;
  }
  slot.checked = true;
  ocudu_dapp_receiver_completion_result_v1 completion{};
  const auto                               status = slot.worker->inspect_receiver(slot.ticket, completion);
  return status == OCUDU_DAPP_OK_V1 && completion.ready == 1 && completion.valid == 1;
}

void native_receiver_resident_adapter::reclaim(record& slot, native_receiver_reclaimed& reclaimed) noexcept
{
  reclaimed.worker  = std::move(slot.worker);
  reclaimed.tensors = std::move(slot.tensors);
  slot.ticket       = {};
  slot.expected     = {};
  slot.checked      = false;
  slot.quarantined  = false;
}

ocudu_dapp_status_v1
native_receiver_resident_adapter::release_after_consumers(const resident_softbit_buffer& codeword,
                                                          native_receiver_reclaimed&     reclaimed) noexcept
{
  try_guard lock(gate);
  if (!lock) {
    return OCUDU_DAPP_RESOURCE_V1;
  }
  if (reclaimed.worker || reclaimed.tensors) {
    return OCUDU_DAPP_INVALID_INPUT_V1;
  }
  for (auto& slot : records) {
    if (slot.tensors && !slot.quarantined && same_codeword(codeword, slot.expected)) {
      const auto status = slot.worker->release_receiver(slot.ticket);
      if (status == OCUDU_DAPP_OK_V1) {
        reclaim(slot, reclaimed);
      }
      return status;
    }
  }
  return OCUDU_DAPP_INVALID_INPUT_V1;
}

ocudu_dapp_status_v1
native_receiver_resident_adapter::retire(std::array<native_receiver_reclaimed, capacity>& reclaimed) noexcept
{
  stop();
  try_guard lock(gate);
  if (!lock) {
    return OCUDU_DAPP_RESOURCE_V1;
  }
  for (const auto& output : reclaimed) {
    if (output.worker || output.tensors) {
      return OCUDU_DAPP_INVALID_INPUT_V1;
    }
  }
  for (const auto& slot : records) {
    if (slot.tensors && !slot.quarantined) {
      return OCUDU_DAPP_RESOURCE_V1;
    }
  }
  for (auto& slot : records) {
    if (slot.worker) {
      const auto status = slot.worker->retire_receiver();
      if (status != OCUDU_DAPP_OK_V1) {
        return status;
      }
    }
  }
  for (unsigned i = 0; i != capacity; ++i) {
    reclaim(records[i], reclaimed[i]);
  }
  return OCUDU_DAPP_OK_V1;
}
