// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/scheduler/dapp/scheduler_dapp_context.h"
#include "ocudu/ran/du_cell_index.h"
#include "ocudu/ran/resource_block.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <limits>
#include <new>

using namespace ocudu;

namespace {

constexpr uint32_t no_active_snapshot = 3;
constexpr int      max_radio_horizon  = 4096;

bool valid_radio_time(const ocudu_dapp_radio_time_v1& time) noexcept
{
  return time.numerology <= 4 && time.sfn < NOF_SFNS && time.slot < NOF_SUBFRAMES_PER_FRAME * (1U << time.numerology) &&
         time.reserved == 0;
}

slot_point to_slot(const ocudu_dapp_radio_time_v1& time) noexcept
{
  return slot_point{time.numerology, time.sfn, time.slot};
}

bool valid_context(const scheduler_dapp_interference_update& update) noexcept
{
  constexpr uint32_t minimum = offsetof(ocudu_dapp_context_header_v1, reserved);
  const auto&        context = update.context;
  if (context.struct_size < minimum || context.schema_id != OCUDU_DAPP_SCHEDULER_CONTEXT_INTERFERENCE_MAP_V1 ||
      context.schema_version != 1 || context.flags != 0 ||
      (context.producer_instance_id[0] == 0 && context.producer_instance_id[1] == 0) ||
      context.producer_generation == 0 || context.source_sequence == 0 || context.cell_id >= MAX_NOF_DU_CELLS ||
      context.confidence_q16 > (1U << 16U) || update.prb_confidence.empty() ||
      update.prb_confidence.size() > MAX_NOF_PRBS || update.publish_monotonic_ns == 0 ||
      !valid_radio_time(context.created) || !valid_radio_time(context.valid_from) ||
      !valid_radio_time(context.valid_until) || context.created.numerology != context.valid_from.numerology ||
      context.valid_from.numerology != context.valid_until.numerology) {
    return false;
  }
  const slot_point created = to_slot(context.created);
  const slot_point from    = to_slot(context.valid_from);
  const slot_point until   = to_slot(context.valid_until);
  const int        lead    = from - created;
  const int        horizon = until - from;
  return lead > 0 && lead <= max_radio_horizon && horizon >= 0 && horizon <= max_radio_horizon;
}

} // namespace

class ocudu::scheduler_dapp_interference_cache_impl final : public scheduler_dapp_interference_cache
{
  struct alignas(64) snapshot {
    std::atomic<uint32_t>             readers{};
    ocudu_dapp_context_header_v1      context{};
    ocudu_dapp_tensor_view_v1         tensor{};
    std::array<uint8_t, MAX_NOF_PRBS> mask{};
    uint64_t                          published_monotonic_ns{};
    uint64_t                          expiry_monotonic_ns{};
  };

  struct alignas(64) cell_cache {
    std::atomic<uint32_t>   active{no_active_snapshot};
    std::atomic_flag        publishing = ATOMIC_FLAG_INIT;
    uint64_t                next_generation{};
    std::array<snapshot, 3> snapshots;
  };

public:
  explicit scheduler_dapp_interference_cache_impl(uint64_t max_residence_ns_) noexcept :
    max_residence_ns(max_residence_ns_)
  {
  }

  scheduler_dapp_context_result publish(const scheduler_dapp_interference_update& update) noexcept override
  {
    if (!valid_context(update)) {
      return scheduler_dapp_context_result::invalid;
    }
    cell_cache& cell = cells[update.context.cell_id];
    if (cell.publishing.test_and_set(std::memory_order_acquire)) {
      return scheduler_dapp_context_result::busy;
    }
    struct flag_guard {
      std::atomic_flag& flag;
      ~flag_guard() { flag.clear(std::memory_order_release); }
    } guard{cell.publishing};

    const uint32_t active = cell.active.load(std::memory_order_acquire);
    if (active != no_active_snapshot) {
      const auto& previous      = cell.snapshots[active].context;
      const bool  same_producer = previous.producer_instance_id[0] == update.context.producer_instance_id[0] &&
                                 previous.producer_instance_id[1] == update.context.producer_instance_id[1];
      const bool newer = update.context.producer_generation > previous.producer_generation ||
                         (update.context.producer_generation == previous.producer_generation &&
                          update.context.source_sequence > previous.source_sequence);
      if (!same_producer || !newer) {
        return scheduler_dapp_context_result::invalid;
      }
    }
    uint32_t selected = no_active_snapshot;
    for (uint32_t index = 0; index != cell.snapshots.size(); ++index) {
      if (index != active && cell.snapshots[index].readers.load(std::memory_order_acquire) == 0) {
        selected = index;
        break;
      }
    }
    if (selected == no_active_snapshot) {
      return scheduler_dapp_context_result::busy;
    }

    snapshot& destination = cell.snapshots[selected];
    destination.context   = update.context;
    std::copy(update.prb_confidence.begin(), update.prb_confidence.end(), destination.mask.begin());
    std::fill(destination.mask.begin() + update.prb_confidence.size(), destination.mask.end(), uint8_t{0});

    uint64_t generation = ++cell.next_generation;
    if (generation == 0) {
      generation = ++cell.next_generation;
    }
    destination.published_monotonic_ns = update.publish_monotonic_ns;
    destination.expiry_monotonic_ns =
        max_residence_ns > std::numeric_limits<uint64_t>::max() - update.publish_monotonic_ns
            ? std::numeric_limits<uint64_t>::max()
            : update.publish_monotonic_ns + max_residence_ns;
    destination.tensor                       = {};
    destination.tensor.struct_size           = sizeof(destination.tensor);
    destination.tensor.abi_major             = OCUDU_DAPP_ABI_MAJOR_V1;
    destination.tensor.abi_minor             = OCUDU_DAPP_ABI_MINOR_V1;
    destination.tensor.domain                = OCUDU_DAPP_MEM_HOST_V1;
    destination.tensor.backend_id            = OCUDU_DAPP_BACKEND_HOST_V1;
    destination.tensor.element_type          = OCUDU_DAPP_ELEMENT_U8_V1;
    destination.tensor.rank                  = 1;
    destination.tensor.access                = OCUDU_DAPP_ACCESS_READ_V1 | OCUDU_DAPP_ACCESS_HOST_VISIBLE_V1;
    destination.tensor.allocation_id         = static_cast<uint64_t>(update.context.cell_id) * 3 + selected + 1;
    destination.tensor.allocation_generation = generation;
    destination.tensor.native_handle         = reinterpret_cast<uintptr_t>(destination.mask.data());
    destination.tensor.byte_length           = update.prb_confidence.size();
    destination.tensor.dimensions[0]         = update.prb_confidence.size();
    destination.tensor.byte_strides[0]       = sizeof(uint8_t);
    destination.tensor.layout_id             = OCUDU_DAPP_SCHEDULER_LAYOUT_PRB_INTERFERENCE_U8_V1;
    destination.tensor.layout_version        = 1;
    destination.tensor.lease_token           = generation;
    cell.active.store(selected, std::memory_order_release);
    return scheduler_dapp_context_result::accepted;
  }

  scheduler_dapp_context_result invalidate(uint32_t cell_id) noexcept override
  {
    if (cell_id >= cells.size()) {
      return scheduler_dapp_context_result::invalid;
    }
    cell_cache& cell = cells[cell_id];
    if (cell.publishing.test_and_set(std::memory_order_acquire)) {
      return scheduler_dapp_context_result::busy;
    }
    cell.active.store(no_active_snapshot, std::memory_order_release);
    cell.publishing.clear(std::memory_order_release);
    return scheduler_dapp_context_result::accepted;
  }

  scheduler_dapp_interference_lease acquire(uint32_t   cell_id,
                                            uint32_t   cell_nof_prbs,
                                            slot_point transmission_time,
                                            uint64_t   now_monotonic_ns) noexcept override
  {
    if (cell_id >= cells.size() || cell_nof_prbs == 0 || cell_nof_prbs > MAX_NOF_PRBS || !transmission_time.valid() ||
        now_monotonic_ns == 0) {
      return {};
    }
    cell_cache& cell = cells[cell_id];
    for (unsigned attempt = 0; attempt != 3; ++attempt) {
      const uint32_t index = cell.active.load(std::memory_order_acquire);
      if (index == no_active_snapshot) {
        return {};
      }
      snapshot& source = cell.snapshots[index];
      source.readers.fetch_add(1, std::memory_order_acquire);
      if (cell.active.load(std::memory_order_acquire) != index) {
        source.readers.fetch_sub(1, std::memory_order_release);
        continue;
      }
      const slot_point from  = to_slot(source.context.valid_from);
      const slot_point until = to_slot(source.context.valid_until);
      if (source.context.cell_id != cell_id || source.tensor.dimensions[0] != cell_nof_prbs ||
          source.context.valid_from.numerology != transmission_time.numerology() || transmission_time < from ||
          transmission_time > until || now_monotonic_ns < source.published_monotonic_ns ||
          now_monotonic_ns > source.expiry_monotonic_ns) {
        source.readers.fetch_sub(1, std::memory_order_release);
        return {};
      }
      return {&source.context, &source.tensor, &source, release_snapshot};
    }
    return {};
  }

private:
  static void release_snapshot(void* value) noexcept
  {
    static_cast<snapshot*>(value)->readers.fetch_sub(1, std::memory_order_release);
  }

  std::array<cell_cache, MAX_NOF_DU_CELLS> cells;
  uint64_t                                 max_residence_ns;
};

scheduler_dapp_interference_lease::scheduler_dapp_interference_lease(const ocudu_dapp_context_header_v1* context_,
                                                                     const ocudu_dapp_tensor_view_v1*    mask_,
                                                                     void*                               release_token_,
                                                                     void (*release_)(void*) noexcept) noexcept :
  context_value(context_), mask_value(mask_), release_token(release_token_), release(release_)
{
}

scheduler_dapp_interference_lease::scheduler_dapp_interference_lease(scheduler_dapp_interference_lease&& other) noexcept
  :
  context_value(other.context_value),
  mask_value(other.mask_value),
  release_token(other.release_token),
  release(other.release)
{
  other.context_value = nullptr;
  other.mask_value    = nullptr;
  other.release_token = nullptr;
  other.release       = nullptr;
}

scheduler_dapp_interference_lease&
scheduler_dapp_interference_lease::operator=(scheduler_dapp_interference_lease&& other) noexcept
{
  if (this != &other) {
    reset();
    context_value       = other.context_value;
    mask_value          = other.mask_value;
    release_token       = other.release_token;
    release             = other.release;
    other.context_value = nullptr;
    other.mask_value    = nullptr;
    other.release_token = nullptr;
    other.release       = nullptr;
  }
  return *this;
}

scheduler_dapp_interference_lease::~scheduler_dapp_interference_lease()
{
  reset();
}

void scheduler_dapp_interference_lease::reset() noexcept
{
  if (release != nullptr) {
    release(release_token);
  }
  context_value = nullptr;
  mask_value    = nullptr;
  release_token = nullptr;
  release       = nullptr;
}

std::shared_ptr<scheduler_dapp_interference_cache>
ocudu::create_scheduler_dapp_interference_cache(std::chrono::nanoseconds max_residence) noexcept
{
  if (max_residence.count() <= 0) {
    return {};
  }
  try {
    return std::make_shared<scheduler_dapp_interference_cache_impl>(static_cast<uint64_t>(max_residence.count()));
  } catch (...) {
    return {};
  }
}
