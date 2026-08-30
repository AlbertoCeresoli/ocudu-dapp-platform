// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#pragma once

#include "ocudu/adt/interval.h"
#include "ocudu/ran/resource_allocation/rb_interval.h"
#include "ocudu/ran/sch/sch_mcs.h"
#include "ocudu/scheduler/result/pdsch_info.h"
#include "ocudu/scheduler/result/pusch_info.h"
#include "ocudu/support/units.h"
#include <optional>

namespace ocudu {

class slice_ue;
class ue_cell;

/// Type used to represent a UE priority.
using ue_sched_priority = double;

/// Priority value used to represent that the UE must not be scheduled.
const ue_sched_priority forbid_sched_priority = std::numeric_limits<ue_sched_priority>::lowest();

/// Lowest priority possible
const ue_sched_priority lowest_sched_priority = forbid_sched_priority + std::numeric_limits<ue_sched_priority>::min();

/// Maximum priority possible
constexpr ue_sched_priority max_sched_priority = std::numeric_limits<ue_sched_priority>::max();

/// UE candidate for DL or UL scheduling.
struct ue_newtx_candidate {
  const slice_ue*   ue;
  ue_sched_priority priority;
  units::bytes      pending_bytes;

  /// Optional Class-B overrides (nullopt = use host baseline path).
  std::optional<sch_mcs_index>      dapp_mcs;
  std::optional<sch_mcs_index>      dapp_max_mcs;
  std::optional<interval<unsigned>> dapp_prb_limits; // min/max PRBs
  std::optional<vrb_interval>       dapp_prb_assign; // hard assignment
  std::optional<vrb_interval>       dapp_prb_pref;   // soft preference
  std::optional<uint8_t>            dapp_tpc;        // 0..3
  std::optional<uint8_t>            dapp_layers;
};

/// Interface of data scheduler that is used to allocate UE DL and UL grants in a given slot
/// The data_scheduler object will be common to all cells and slots.
class scheduler_policy
{
public:
  virtual ~scheduler_policy() = default;

  virtual void add_ue(du_ue_index_t ue_index) = 0;

  virtual void rem_ue(du_ue_index_t ue_index) = 0;

  /// \brief Compute UE candidate priorities for DL scheduling.
  ///
  /// This function is only called in DL slots and only when there are UE candidates with new HARQ transmissions to be
  /// scheduled.
  /// \param[in] pdcch_slot PDCCH slot.
  /// \param[in] pdsch_slot PDSCH slot.
  /// \param[in] ue_candidates UE candidates with new HARQ transmissions to be scheduled.
  virtual void
  compute_ue_dl_priorities(slot_point pdcch_slot, slot_point pdsch_slot, span<ue_newtx_candidate> ue_candidates) = 0;

  /// \brief Compute UE candidate priorities for UL scheduling.
  ///
  /// This function is only called in UL slots and only when there are UE candidates with new HARQ transmissions to be
  /// scheduled.
  /// \param[in] pdcch_slot PDCCH slot.
  /// \param[in] pusch_slot PUSCH slot.
  /// \param[in] ue_candidates UE candidates with new HARQ transmissions to be scheduled.
  virtual void
  compute_ue_ul_priorities(slot_point pdcch_slot, slot_point pusch_slot, span<ue_newtx_candidate> ue_candidates) = 0;

  /// Optional PRB mask applied only to new UL transmissions after retransmissions are scheduled.
  virtual span<const uint8_t> get_ul_newtx_avoid_prbs(slot_point) const { return {}; }

  /// Optional free-for-newTx PRB snapshot (1 = free) after retransmissions, for Class-B packing.
  /// remaining_slice_prbs is the host slice budget for this opportunity.
  virtual void set_newtx_free_prb_snapshot(slot_point /*tx_slot*/,
                                           span<const uint8_t> /*free_prbs*/,
                                           uint16_t /*remaining_slice_prbs*/)
  {
  }

  /// Save UE DL newtx grants.
  virtual void save_dl_newtx_grants(span<const dl_msg_alloc> dl_grants) = 0;

  /// Save UE UL newtx grants.
  virtual void save_ul_newtx_grants(span<const ul_sched_info> ul_grants) = 0;
};

} // namespace ocudu
