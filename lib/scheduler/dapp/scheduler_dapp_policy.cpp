// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/scheduler/dapp/scheduler_dapp_policy.h"
#include "../policy/scheduler_policy.h"
#include "../slicing/slice_ue_repository.h"
#include "../ue_context/ue_cell.h"
#include "ocudu/dapp/runtime/abi_validation.h"
#include "ocudu/dapp/runtime/native_hook_point.h"
#include "ocudu/ran/pdsch/pdsch_mcs.h"
#include "ocudu/ran/pusch/pusch_mcs.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <limits>
#include <new>
#include <optional>
#include <vector>

using namespace ocudu;

namespace {

uint64_t monotonic_now_ns() noexcept
{
  const auto now =
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
  return now < 0 ? 0 : static_cast<uint64_t>(now);
}

uint64_t deadline_from(uint64_t now, std::chrono::nanoseconds budget) noexcept
{
  if (now == 0 || budget.count() < 0) {
    return 0;
  }
  const uint64_t delta = static_cast<uint64_t>(budget.count());
  return delta > std::numeric_limits<uint64_t>::max() - now ? std::numeric_limits<uint64_t>::max() : now + delta;
}

uint64_t elapsed_ns(uint64_t start, uint64_t end) noexcept
{
  return end >= start ? end - start : 0;
}

ocudu_dapp_radio_time_v1 to_radio_time(slot_point slot) noexcept
{
  return {static_cast<uint16_t>(slot.sfn()),
          static_cast<uint16_t>(slot.slot_index()),
          static_cast<uint16_t>(slot.numerology()),
          0};
}

struct committed_intent_overrides {
  std::optional<sch_mcs_index>      mcs;
  std::optional<sch_mcs_index>      max_mcs;
  std::optional<interval<unsigned>> prb_limits;
  std::optional<vrb_interval>       prb_assign;
  std::optional<vrb_interval>       prb_pref;
  std::optional<uint8_t>            tpc;
  std::optional<uint8_t>            layers;
};

/// Directional served-rate history owned by the Class-B boundary rather than a
/// particular fallback policy. This keeps the feature available with both the
/// time-QoS and round-robin host schedulers.
class scheduler_feature_history
{
public:
  scheduler_feature_history() : active(MAX_NOF_DU_UES), dl_average(MAX_NOF_DU_UES), ul_average(MAX_NOF_DU_UES)
  {
    active_ues.reserve(MAX_NOF_DU_UES);
  }

  void add_ue(du_ue_index_t ue_index)
  {
    const uint32_t index = static_cast<uint32_t>(ue_index);
    if (index >= active.size() || active[index] != 0) {
      return;
    }
    active[index] = 1;
    active_ues.push_back(index);
  }

  void rem_ue(du_ue_index_t ue_index)
  {
    const uint32_t index = static_cast<uint32_t>(ue_index);
    if (index >= active.size() || active[index] == 0) {
      return;
    }
    active[index]     = 0;
    dl_average[index] = 0.0f;
    ul_average[index] = 0.0f;
    const auto pos    = std::find(active_ues.begin(), active_ues.end(), index);
    if (pos != active_ues.end()) {
      active_ues.erase(pos);
    }
  }

  void save_dl(span<const dl_msg_alloc> grants) noexcept
  {
    update(dl_average, grants, [](const dl_msg_alloc& grant) {
      uint64_t bytes = 0;
      for (const auto& cw : grant.pdsch_cfg.codewords) {
        bytes += cw.tb_size_bytes.value();
      }
      return bytes;
    });
  }

  void save_ul(span<const ul_sched_info> grants) noexcept
  {
    update(ul_average, grants, [](const ul_sched_info& grant) { return grant.pusch_cfg.tb_size_bytes.value(); });
  }

  std::optional<uint64_t>
  average_rate_bps(du_ue_index_t ue_index, uint32_t direction, slot_point transmission_slot) const noexcept
  {
    const uint32_t index = static_cast<uint32_t>(ue_index);
    if (index >= active.size() || active[index] == 0) {
      return std::nullopt;
    }
    const float bytes_per_opportunity = direction == OCUDU_DAPP_SCHEDULER_DL_V1 ? dl_average[index] : ul_average[index];
    const long double rate =
        static_cast<long double>(bytes_per_opportunity) * 8.0L * 1000.0L * transmission_slot.nof_slots_per_subframe();
    if (!std::isfinite(rate) || rate <= 0.0L) {
      return uint64_t{0};
    }
    if (rate >= static_cast<long double>(std::numeric_limits<uint64_t>::max())) {
      return std::numeric_limits<uint64_t>::max();
    }
    return static_cast<uint64_t>(std::llround(rate));
  }

private:
  template <typename Grant, typename GetBytes>
  void update(std::vector<float>& averages, span<const Grant> grants, GetBytes get_bytes) noexcept
  {
    if (grants.empty()) {
      return;
    }
    static constexpr float alpha = 0.01f;
    for (uint32_t index : active_ues) {
      averages[index] *= 1.0f - alpha;
    }
    for (const Grant& grant : grants) {
      const uint32_t index = static_cast<uint32_t>(grant.context.ue_index);
      if (index < active.size() && active[index] != 0) {
        averages[index] += alpha * static_cast<float>(get_bytes(grant));
      }
    }
  }

  std::vector<uint8_t>  active;
  std::vector<float>    dl_average;
  std::vector<float>    ul_average;
  std::vector<uint32_t> active_ues;
};

std::optional<uint64_t> oldest_dl_hol_delay_us(const slice_ue& ue, slot_point transmission_slot) noexcept
{
  std::optional<uint64_t> oldest;
  for (logical_channel_config_ptr lc : *ue.logical_channels()) {
    if (!ue.contains(lc->lcid) || ue.pending_dl_newtx_bytes(lc->lcid) == 0) {
      continue;
    }
    const slot_point hol_toa = ue.dl_hol_toa(lc->lcid);
    if (!hol_toa.valid() || transmission_slot < hol_toa) {
      continue;
    }
    const uint64_t slot_age = transmission_slot - hol_toa;
    const uint64_t age_us   = slot_age * 1000U / transmission_slot.nof_slots_per_subframe();
    oldest                  = std::max(oldest.value_or(0), age_us);
  }
  return oldest;
}

struct scheduler_workspace {
  scheduler_workspace(uint32_t capacity, uint32_t nof_prbs) :
    candidates(capacity),
    intents(capacity),
    priorities(capacity),
    seen(capacity),
    overrides(capacity),
    requested_avoidance(nof_prbs),
    reserved_avoidance(nof_prbs),
    committed_avoidance(nof_prbs),
    free_vrb(nof_prbs, uint8_t{1})
  {
  }

  std::vector<ocudu_dapp_scheduler_candidate_v1> candidates;
  std::vector<ocudu_dapp_scheduler_intent_v1>    intents;
  std::vector<ue_sched_priority>                 priorities;
  std::vector<uint8_t>                           seen;
  std::vector<committed_intent_overrides>        overrides;
  std::vector<uint8_t>                           requested_avoidance;
  std::vector<uint8_t>                           reserved_avoidance;
  std::vector<uint8_t>                           committed_avoidance;
  std::vector<uint8_t>                           free_vrb;
  scheduler_feature_history                      feature_history;
  uint16_t                                       remaining_slice_prbs = 0;
  bool                                           free_vrb_valid       = false;
};

static uint8_t mcs_table_id_dl(pdsch_mcs_table table) noexcept
{
  switch (table) {
    case pdsch_mcs_table::qam256:
      return OCUDU_DAPP_SCHEDULER_MCS_TABLE_QAM256_V1;
    case pdsch_mcs_table::qam64LowSe:
      return OCUDU_DAPP_SCHEDULER_MCS_TABLE_QAM64_LOWSE_V1;
    default:
      return OCUDU_DAPP_SCHEDULER_MCS_TABLE_QAM64_V1;
  }
}

static uint8_t mcs_table_id_ul(pusch_mcs_table table) noexcept
{
  switch (table) {
    case pusch_mcs_table::qam256:
      return OCUDU_DAPP_SCHEDULER_MCS_TABLE_QAM256_V1;
    case pusch_mcs_table::qam64LowSe:
      return OCUDU_DAPP_SCHEDULER_MCS_TABLE_QAM64_LOWSE_V1;
    default:
      return OCUDU_DAPP_SCHEDULER_MCS_TABLE_QAM64_V1;
  }
}

static uint32_t allowed_flags_from_guardrails(const scheduler_dapp_guardrails& g, bool ul_interference) noexcept
{
  uint32_t flags = 0;
  if (g.max_ul_prbs_avoided != 0 && ul_interference) {
    flags |= OCUDU_DAPP_SCHEDULER_ALLOW_AVOID_PRB_MASK_V1;
  }
  if (g.allow_mcs) {
    flags |= OCUDU_DAPP_SCHEDULER_ALLOW_MCS_V1;
  }
  if (g.allow_prb_limits) {
    flags |= OCUDU_DAPP_SCHEDULER_ALLOW_PRB_LIMITS_V1;
  }
  if (g.allow_prb_preference) {
    flags |= OCUDU_DAPP_SCHEDULER_ALLOW_PRB_PREFERENCE_V1;
  }
  if (g.allow_prb_assignment) {
    flags |= OCUDU_DAPP_SCHEDULER_ALLOW_PRB_ASSIGNMENT_V1;
  }
  if (g.allow_tpc) {
    flags |= OCUDU_DAPP_SCHEDULER_ALLOW_TPC_V1;
  }
  if (g.allow_layers) {
    flags |= OCUDU_DAPP_SCHEDULER_ALLOW_LAYERS_V1;
  }
  return flags;
}

ocudu_dapp_tensor_view_v1 make_avoidance_output(span<uint8_t> storage, uint64_t allocation_id, uint64_t generation)
{
  ocudu_dapp_tensor_view_v1 output{};
  output.struct_size           = sizeof(output);
  output.abi_major             = OCUDU_DAPP_ABI_MAJOR_V1;
  output.abi_minor             = OCUDU_DAPP_ABI_MINOR_V1;
  output.domain                = OCUDU_DAPP_MEM_HOST_V1;
  output.backend_id            = OCUDU_DAPP_BACKEND_HOST_V1;
  output.element_type          = OCUDU_DAPP_ELEMENT_U8_V1;
  output.rank                  = 1;
  output.access                = OCUDU_DAPP_ACCESS_WRITE_V1 | OCUDU_DAPP_ACCESS_HOST_VISIBLE_V1;
  output.allocation_id         = allocation_id;
  output.allocation_generation = generation;
  output.native_handle         = reinterpret_cast<uintptr_t>(storage.data());
  output.byte_length           = storage.size();
  output.dimensions[0]         = storage.size();
  output.byte_strides[0]       = sizeof(uint8_t);
  output.layout_id             = OCUDU_DAPP_SCHEDULER_LAYOUT_PRB_INTERFERENCE_U8_V1;
  output.layout_version        = 1;
  output.lease_token           = generation;
  return output;
}

class scheduler_policy_decorator final : public scheduler_policy
{
public:
  scheduler_policy_decorator(std::unique_ptr<scheduler_policy>                  fallback_,
                             std::shared_ptr<scheduler_dapp_target>             target_,
                             uint32_t                                           lane_,
                             scheduler_dapp_policy_context                      context_,
                             std::shared_ptr<scheduler_dapp_interference_cache> interference_,
                             std::shared_ptr<quiet_reservation_manager>         reservations_,
                             scheduler_dapp_guardrails                          guardrails_,
                             std::chrono::nanoseconds                           deadline_,
                             scheduler_dapp_policy_mode                         mode_,
                             std::unique_ptr<scheduler_workspace>               workspace_) noexcept :
    fallback(std::move(fallback_)),
    target(std::move(target_)),
    lane(lane_),
    context(context_),
    interference(std::move(interference_)),
    reservations(std::move(reservations_)),
    guardrails(guardrails_),
    deadline(deadline_),
    mode(mode_),
    workspace(std::move(workspace_))
  {
  }

  void add_ue(du_ue_index_t ue_index) override
  {
    fallback->add_ue(ue_index);
    workspace->feature_history.add_ue(ue_index);
  }
  void rem_ue(du_ue_index_t ue_index) override
  {
    fallback->rem_ue(ue_index);
    workspace->feature_history.rem_ue(ue_index);
  }

  void
  compute_ue_dl_priorities(slot_point pdcch_slot, slot_point pdsch_slot, span<ue_newtx_candidate> candidates) override
  {
    const uint64_t conventional_start = monotonic_now_ns();
    fallback->compute_ue_dl_priorities(pdcch_slot, pdsch_slot, candidates);
    const uint64_t boundary_start = monotonic_now_ns();
    apply(OCUDU_DAPP_SCHEDULER_DL_V1,
          pdcch_slot,
          pdsch_slot,
          candidates,
          elapsed_ns(conventional_start, boundary_start),
          boundary_start);
  }

  void
  compute_ue_ul_priorities(slot_point pdcch_slot, slot_point pusch_slot, span<ue_newtx_candidate> candidates) override
  {
    accepted_avoidance_slot.clear();
    const uint64_t conventional_start = monotonic_now_ns();
    fallback->compute_ue_ul_priorities(pdcch_slot, pusch_slot, candidates);
    const uint64_t boundary_start = monotonic_now_ns();
    prepare_authoritative_avoidance(pusch_slot);
    apply(OCUDU_DAPP_SCHEDULER_UL_V1,
          pdcch_slot,
          pusch_slot,
          candidates,
          elapsed_ns(conventional_start, boundary_start),
          boundary_start);
  }

  span<const uint8_t> get_ul_newtx_avoid_prbs(slot_point pusch_slot) const override
  {
    if (accepted_avoidance_slot == pusch_slot) {
      return workspace->committed_avoidance;
    }
    return fallback->get_ul_newtx_avoid_prbs(pusch_slot);
  }

  void set_newtx_free_prb_snapshot(slot_point /*tx_slot*/,
                                   span<const uint8_t> free_prbs,
                                   uint16_t            remaining_slice_prbs) override
  {
    workspace->free_vrb_valid       = false;
    workspace->remaining_slice_prbs = remaining_slice_prbs;
    if (free_prbs.size() != workspace->free_vrb.size()) {
      return;
    }
    std::copy(free_prbs.begin(), free_prbs.end(), workspace->free_vrb.begin());
    workspace->free_vrb_valid = true;
  }

  void save_dl_newtx_grants(span<const dl_msg_alloc> grants) override
  {
    fallback->save_dl_newtx_grants(grants);
    workspace->feature_history.save_dl(grants);
  }
  void save_ul_newtx_grants(span<const ul_sched_info> grants) override
  {
    fallback->save_ul_newtx_grants(grants);
    workspace->feature_history.save_ul(grants);
  }

private:
  void prepare_authoritative_avoidance(slot_point transmission_slot) noexcept
  {
    // Reservations first: with no active reservation and no bound dApp,
    // get_ul_newtx_avoid_prbs already serves the fallback mask verbatim
    // (accepted_avoidance_slot stays cleared), so the fill/merge below is
    // skippable work on the SCHED hot path.
    bool have_reservations = false;
    if (reservations) {
      const auto lookup =
          reservations->lookup(context.cell_id, transmission_slot, monotonic_now_ns(), workspace->reserved_avoidance);
      have_reservations = lookup.reservation_count != 0;
    }
    if (!have_reservations && !target->bound()) {
      return;
    }
    std::fill(workspace->committed_avoidance.begin(), workspace->committed_avoidance.end(), uint8_t{0});
    const auto fallback_mask = fallback->get_ul_newtx_avoid_prbs(transmission_slot);
    if (fallback_mask.size() == workspace->committed_avoidance.size()) {
      std::transform(fallback_mask.begin(),
                     fallback_mask.end(),
                     workspace->committed_avoidance.begin(),
                     [](uint8_t value) { return value != 0; });
    }
    if (have_reservations) {
      for (uint32_t prb = 0; prb != workspace->committed_avoidance.size(); ++prb) {
        workspace->committed_avoidance[prb] |= workspace->reserved_avoidance[prb];
      }
    }
    if (std::any_of(workspace->committed_avoidance.begin(), workspace->committed_avoidance.end(), [](uint8_t value) {
          return value != 0;
        })) {
      accepted_avoidance_slot = transmission_slot;
    }
  }

  void apply(uint32_t                 direction,
             slot_point               decision_slot,
             slot_point               transmission_slot,
             span<ue_newtx_candidate> candidates,
             uint64_t                 conventional_time_ns,
             uint64_t                 boundary_start_ns) noexcept
  {
    try {
      apply_impl(direction, decision_slot, transmission_slot, candidates, conventional_time_ns, boundary_start_ns);
    } catch (...) {
      // Conventional priorities are still present and remain the complete fallback.
    }
  }

  void apply_impl(uint32_t                 direction,
                  slot_point               decision_slot,
                  slot_point               transmission_slot,
                  span<ue_newtx_candidate> candidates,
                  uint64_t                 conventional_time_ns,
                  uint64_t                 boundary_start_ns)
  {
    if (!target->active() || candidates.empty() || candidates.size() > workspace->candidates.size()) {
      return;
    }

    uint64_t generation = ++invocation_sequence;
    if (generation == 0) {
      generation = ++invocation_sequence;
    }
    for (uint32_t index = 0; index != candidates.size(); ++index) {
      const auto& source = candidates[index];
      if (source.ue == nullptr) {
        return;
      }
      // Clear any stale Class-B overrides before this invocation.
      candidates[index].dapp_mcs.reset();
      candidates[index].dapp_max_mcs.reset();
      candidates[index].dapp_prb_limits.reset();
      candidates[index].dapp_prb_assign.reset();
      candidates[index].dapp_prb_pref.reset();
      candidates[index].dapp_tpc.reset();
      candidates[index].dapp_layers.reset();
      workspace->overrides[index] = {};

      auto& feature                 = workspace->candidates[index];
      feature                       = {};
      feature.struct_size           = sizeof(feature);
      feature.candidate_id          = index;
      feature.ue_index              = static_cast<uint32_t>(source.ue->ue_index());
      feature.rnti                  = static_cast<uint16_t>(source.ue->crnti());
      feature.pending_bytes         = source.pending_bytes.value();
      feature.conventional_priority = source.priority;
      if (const auto rate =
              workspace->feature_history.average_rate_bps(source.ue->ue_index(), direction, transmission_slot);
          rate.has_value()) {
        feature.average_rate_bps = *rate;
        feature.flags |= OCUDU_DAPP_SCHEDULER_CANDIDATE_AVERAGE_RATE_VALID_V1;
      }
      workspace->intents[index] = {};
      if (source.ue->has_pending_sr()) {
        feature.flags |= OCUDU_DAPP_SCHEDULER_CANDIDATE_PENDING_SR_V1;
      }
      if (source.priority == forbid_sched_priority) {
        feature.flags |= OCUDU_DAPP_SCHEDULER_CANDIDATE_CONVENTIONAL_FORBID_V1;
      }

      const ue_cell& ue_cc  = source.ue->get_cc();
      const auto&    la     = ue_cc.link_adaptation_controller();
      const auto&    ch     = ue_cc.channel_state_manager();
      const auto&    expert = ue_cc.cfg().cell_cfg_common.expert_cfg.ue;

      if (direction == OCUDU_DAPP_SCHEDULER_DL_V1) {
        if (const auto hol = oldest_dl_hol_delay_us(*source.ue, transmission_slot); hol.has_value()) {
          feature.hol_delay_us = *hol;
          feature.flags |= OCUDU_DAPP_SCHEDULER_CANDIDATE_HOL_DELAY_VALID_V1;
        }
        const auto dl_bler = ue_cc.dapp_dl_bler();
        if (dl_bler.nof_observations != 0) {
          feature.bler                  = dl_bler.bler;
          feature.bler_nof_observations = dl_bler.nof_observations;
          feature.flags |= OCUDU_DAPP_SCHEDULER_CANDIDATE_BLER_VALID_V1;
        }
        const float eff_cqi = la.get_effective_cqi();
        if (eff_cqi > 0.0f) {
          feature.cqi = eff_cqi;
          feature.flags |= OCUDU_DAPP_SCHEDULER_CANDIDATE_CQI_VALID_V1;
        }
        feature.host_olla_offset_db = la.dl_cqi_offset();
        feature.flags |= OCUDU_DAPP_SCHEDULER_CANDIDATE_OLLA_OFFSET_VALID_V1;
        pdsch_mcs_table dl_table = pdsch_mcs_table::qam64;
        if (ue_cc.cfg().init_bwp().dl.pdsch().ded() != nullptr) {
          dl_table = ue_cc.cfg().init_bwp().dl.pdsch().ded()->mcs_table;
        }
        feature.mcs_table_id   = mcs_table_id_dl(dl_table);
        feature.expert_mcs_min = static_cast<uint8_t>(expert.dl_mcs.start().value());
        feature.expert_mcs_max = static_cast<uint8_t>(expert.dl_mcs.stop().value());
        if (expert.dl_mcs.length() == 0) {
          feature.expert_mcs_max = feature.expert_mcs_min;
        }
        feature.flags |= OCUDU_DAPP_SCHEDULER_CANDIDATE_EXPERT_MCS_VALID_V1;
        if (const auto mcs = la.calculate_dl_mcs(dl_table); mcs.has_value()) {
          feature.recommended_mcs = static_cast<uint16_t>(mcs->value());
          feature.flags |= OCUDU_DAPP_SCHEDULER_CANDIDATE_MCS_VALID_V1;
        }
        feature.nof_layers = static_cast<uint8_t>(std::max(1u, ch.get_nof_dl_layers()));
        feature.flags |= OCUDU_DAPP_SCHEDULER_CANDIDATE_LAYERS_VALID_V1;
        if (ue_cc.dapp_dl_ack_is_fresh()) {
          // Closes the package's DL loop: without this, DL OLLA seeded once
          // from the host offset and never updated.
          const auto ack          = ue_cc.dapp_peek_dl_ack();
          feature.last_tx_outcome = ack.crc_ok ? OCUDU_DAPP_SCHEDULER_OUTCOME_ACK_OR_CRC_OK_V1
                                               : OCUDU_DAPP_SCHEDULER_OUTCOME_NACK_OR_CRC_KO_V1;
          feature.last_used_mcs   = ack.used_mcs;
          feature.flags |= OCUDU_DAPP_SCHEDULER_CANDIDATE_OUTCOME_VALID_V1;
        }
      } else {
        const auto ul_bler = ue_cc.dapp_ul_bler();
        if (ul_bler.nof_observations != 0) {
          feature.bler                  = ul_bler.bler;
          feature.bler_nof_observations = ul_bler.nof_observations;
          feature.flags |= OCUDU_DAPP_SCHEDULER_CANDIDATE_BLER_VALID_V1;
        }
        feature.sinr_db = ch.get_pusch_snr();
        feature.flags |= OCUDU_DAPP_SCHEDULER_CANDIDATE_SINR_VALID_V1;
        feature.host_olla_offset_db = la.ul_snr_offset_db();
        feature.flags |= OCUDU_DAPP_SCHEDULER_CANDIDATE_OLLA_OFFSET_VALID_V1;
        const bool tp              = ue_cc.cfg().use_pusch_transform_precoding_dci_0_1();
        feature.transform_precoder = tp ? 1 : 0;
        pusch_mcs_table ul_table   = pusch_mcs_table::qam64;
        if (ue_cc.cfg().init_bwp().ul.pusch_ded() != nullptr) {
          ul_table = ue_cc.cfg().init_bwp().ul.pusch_ded()->mcs_table;
        }
        feature.mcs_table_id   = mcs_table_id_ul(ul_table);
        feature.expert_mcs_min = static_cast<uint8_t>(expert.ul_mcs.start().value());
        feature.expert_mcs_max = static_cast<uint8_t>(expert.ul_mcs.stop().value());
        if (expert.ul_mcs.length() == 0) {
          feature.expert_mcs_max = feature.expert_mcs_min;
        }
        feature.flags |= OCUDU_DAPP_SCHEDULER_CANDIDATE_EXPERT_MCS_VALID_V1;
        const auto mcs          = la.calculate_ul_mcs(ul_table, tp);
        feature.recommended_mcs = static_cast<uint16_t>(mcs.value());
        feature.flags |= OCUDU_DAPP_SCHEDULER_CANDIDATE_MCS_VALID_V1;
        feature.nof_layers = static_cast<uint8_t>(std::max(1u, ch.get_nof_ul_layers()));
        feature.flags |= OCUDU_DAPP_SCHEDULER_CANDIDATE_LAYERS_VALID_V1;
        feature.tpc_target_sinr_db = expert.ul_power_ctrl.target_pusch_sinr;
        if (auto phn = ue_cc.get_pusch_power_controller().latest_ph_normalized_db(); phn.has_value()) {
          // Grant-size-normalized headroom: absolute thresholds against raw PH
          // limit-cycle with the grant size the PHR happened to be measured on.
          feature.phr_db = *phn;
          feature.flags |=
              OCUDU_DAPP_SCHEDULER_CANDIDATE_PHR_VALID_V1 | OCUDU_DAPP_SCHEDULER_CANDIDATE_PHR_NORMALIZED_V1;
        } else if (auto ph = ue_cc.get_pusch_power_controller().latest_ph_db(); ph.has_value()) {
          feature.phr_db = *ph;
          feature.flags |= OCUDU_DAPP_SCHEDULER_CANDIDATE_PHR_VALID_V1;
        }
        if (ue_cc.dapp_ul_crc_is_fresh()) {
          const auto crc          = ue_cc.dapp_peek_ul_crc();
          feature.last_tx_outcome = crc.crc_ok ? OCUDU_DAPP_SCHEDULER_OUTCOME_ACK_OR_CRC_OK_V1
                                               : OCUDU_DAPP_SCHEDULER_OUTCOME_NACK_OR_CRC_KO_V1;
          feature.last_used_mcs   = crc.used_mcs;
          feature.flags |= OCUDU_DAPP_SCHEDULER_CANDIDATE_OUTCOME_VALID_V1;
        }
      }
    }

    ocudu_dapp_scheduler_input_v1 input{};
    const uint64_t                now      = monotonic_now_ns();
    input.invocation.struct_size           = sizeof(input.invocation);
    input.invocation.abi_major             = OCUDU_DAPP_ABI_MAJOR_V1;
    input.invocation.abi_minor             = OCUDU_DAPP_ABI_MINOR_V1;
    input.invocation.invocation_id         = generation;
    input.invocation.cell_id               = context.cell_id;
    input.invocation.worker_id             = target->worker_id(lane);
    input.invocation.radio_time            = to_radio_time(decision_slot);
    input.invocation.deadline_monotonic_ns = deadline_from(boundary_start_ns, deadline);
    input.invocation.input_generation      = generation;
    input.invocation.output_generation     = generation;
    input.direction                        = direction;
    input.slice_id                         = context.slice_id;
    input.transmission_time                = to_radio_time(transmission_slot);
    input.nof_candidates                   = candidates.size();
    input.cell_nof_prbs                    = context.cell_nof_prbs;
    input.candidate_generation             = generation;
    input.candidates                       = workspace->candidates.data();
    input.remaining_slice_prbs             = workspace->remaining_slice_prbs != 0 ? workspace->remaining_slice_prbs
                                                                                  : static_cast<uint16_t>(context.cell_nof_prbs);
    scheduler_dapp_interference_lease interference_lease;
    bool                              have_interference = false;
    if (direction == OCUDU_DAPP_SCHEDULER_UL_V1 && interference) {
      interference_lease = interference->acquire(context.cell_id, context.cell_nof_prbs, transmission_slot, now);
      if (interference_lease) {
        input.interference_context         = interference_lease.context();
        input.optional_interference_mask   = interference_lease.mask();
        input.interference_context_present = 1;
        have_interference                  = true;
      }
    }
    input.allowed_output_flags = allowed_flags_from_guardrails(guardrails, have_interference);
    // Assignment requires free-VRB snapshot from the allocator.
    if ((input.allowed_output_flags & OCUDU_DAPP_SCHEDULER_ALLOW_PRB_ASSIGNMENT_V1) != 0 &&
        !workspace->free_vrb_valid) {
      input.allowed_output_flags &= ~OCUDU_DAPP_SCHEDULER_ALLOW_PRB_ASSIGNMENT_V1;
    }
    if (workspace->free_vrb_valid) {
      input.free_vrb_context_present            = 1;
      input.free_vrb_context.struct_size        = sizeof(input.free_vrb_context);
      input.free_vrb_context.schema_id          = OCUDU_DAPP_SCHEDULER_CONTEXT_FREE_VRB_MASK_V1;
      input.free_vrb_context.schema_version     = 1;
      input.free_vrb_context.cell_id            = context.cell_id;
      input.free_vrb_mask.struct_size           = sizeof(input.free_vrb_mask);
      input.free_vrb_mask.abi_major             = OCUDU_DAPP_ABI_MAJOR_V1;
      input.free_vrb_mask.abi_minor             = OCUDU_DAPP_ABI_MINOR_V1;
      input.free_vrb_mask.domain                = OCUDU_DAPP_MEM_HOST_V1;
      input.free_vrb_mask.backend_id            = OCUDU_DAPP_BACKEND_HOST_V1;
      input.free_vrb_mask.element_type          = OCUDU_DAPP_ELEMENT_U8_V1;
      input.free_vrb_mask.rank                  = 1;
      input.free_vrb_mask.access                = OCUDU_DAPP_ACCESS_READ_V1 | OCUDU_DAPP_ACCESS_HOST_VISIBLE_V1;
      input.free_vrb_mask.allocation_id         = (static_cast<uint64_t>(context.cell_id) + 1) << 16U | (lane + 1);
      input.free_vrb_mask.allocation_generation = generation;
      input.free_vrb_mask.native_handle         = reinterpret_cast<uintptr_t>(workspace->free_vrb.data());
      input.free_vrb_mask.byte_length           = workspace->free_vrb.size();
      input.free_vrb_mask.dimensions[0]         = workspace->free_vrb.size();
      input.free_vrb_mask.byte_strides[0]       = sizeof(uint8_t);
      input.free_vrb_mask.layout_id             = OCUDU_DAPP_SCHEDULER_LAYOUT_PRB_FREE_U8_V1;
      input.free_vrb_mask.layout_version        = 1;
      input.free_vrb_mask.lease_token           = generation;
    }

    ocudu_dapp_scheduler_output_v1 output{};
    output.struct_size     = sizeof(output);
    output.abi_major       = OCUDU_DAPP_ABI_MAJOR_V1;
    output.abi_minor       = OCUDU_DAPP_ABI_MINOR_V1;
    output.intents         = workspace->intents.data();
    output.intent_capacity = candidates.size();
    if ((input.allowed_output_flags & OCUDU_DAPP_SCHEDULER_ALLOW_AVOID_PRB_MASK_V1) != 0) {
      std::fill(workspace->requested_avoidance.begin(), workspace->requested_avoidance.end(), uint8_t{0});
      const uint64_t mask_allocation_id = (static_cast<uint64_t>(context.cell_id) + 1) << 32U | (lane + 1);
      output.avoid_prb_mask = make_avoidance_output(workspace->requested_avoidance, mask_allocation_id, generation);
    }
    const auto                       expected_avoidance = output.avoid_prb_mask;
    dapp::class_b_policy_measurement measurement;
    measurement.direction                    = direction;
    measurement.nof_candidates               = candidates.size();
    measurement.cell_nof_prbs                = context.cell_nof_prbs;
    measurement.interference_context_present = input.interference_context_present != 0;
    measurement.conventional_time_ns         = conventional_time_ns;
    const auto record = [&](dapp::class_b_policy_outcome outcome, uint64_t boundary_end_ns) noexcept {
      measurement.outcome       = outcome;
      measurement.total_time_ns = elapsed_ns(boundary_start_ns, boundary_end_ns);
      measurement.deadline_miss |= measurement.total_time_ns > static_cast<uint64_t>(deadline.count());
      target->record_measurement(lane, measurement);
    };
    const uint64_t feature_end  = monotonic_now_ns();
    measurement.feature_time_ns = elapsed_ns(boundary_start_ns, feature_end);
    if (measurement.feature_time_ns > static_cast<uint64_t>(deadline.count())) {
      measurement.deadline_miss = true;
      record(dapp::class_b_policy_outcome::fallback, feature_end);
      return;
    }
    const uint64_t invoke_start = monotonic_now_ns();
    const auto     invoked      = target->invoke(lane, input, output);
    const uint64_t invoke_end   = monotonic_now_ns();
    // Consume one-shot CRC outcomes after the package has seen them (even on
    // fallback) so OLLA does not re-apply the same ACK/NACK every slot.
    for (uint32_t index = 0; index != candidates.size(); ++index) {
      if (candidates[index].ue == nullptr ||
          (workspace->candidates[index].flags & OCUDU_DAPP_SCHEDULER_CANDIDATE_OUTCOME_VALID_V1) == 0) {
        continue;
      }
      if (direction == OCUDU_DAPP_SCHEDULER_UL_V1) {
        candidates[index].ue->get_cc().dapp_mark_ul_crc_consumed(candidates[index].ue->get_cc().dapp_peek_ul_crc().seq);
      } else {
        candidates[index].ue->get_cc().dapp_mark_dl_ack_consumed(candidates[index].ue->get_cc().dapp_peek_dl_ack().seq);
      }
    }
    measurement.avoidance_output = (output.flags & OCUDU_DAPP_SCHEDULER_OUTPUT_AVOID_PRB_MASK_V1) != 0;
    measurement.dapp_time_ns     = elapsed_ns(invoke_start, invoke_end);
    if (invoked.status != OCUDU_DAPP_OK_V1 || invoked.invoke_fallback) {
      measurement.deadline_miss = invoked.status == OCUDU_DAPP_DEADLINE_V1;
      record(dapp::class_b_policy_outcome::fallback, invoke_end);
      return;
    }
    const uint64_t validation_start = monotonic_now_ns();
    const bool     valid            = validate(input, output, expected_avoidance, generation, candidates.size());
    const uint64_t validation_end   = monotonic_now_ns();
    measurement.validation_time_ns  = elapsed_ns(validation_start, validation_end);
    if (!valid) {
      target->report_invalid_output(lane, input.invocation);
      record(dapp::class_b_policy_outcome::validation_rejection, validation_end);
      return;
    }
    if (elapsed_ns(boundary_start_ns, validation_end) > static_cast<uint64_t>(deadline.count())) {
      measurement.deadline_miss = true;
      record(dapp::class_b_policy_outcome::fallback, validation_end);
      return;
    }
    if (mode == scheduler_dapp_policy_mode::evaluation_only) {
      record(dapp::class_b_policy_outcome::evaluation_accepted, validation_end);
      return;
    }
    const uint64_t commit_start = monotonic_now_ns();
    for (uint32_t index = 0; index != candidates.size(); ++index) {
      candidates[index].priority        = workspace->priorities[index];
      const auto& ov                    = workspace->overrides[index];
      candidates[index].dapp_mcs        = ov.mcs;
      candidates[index].dapp_max_mcs    = ov.max_mcs;
      candidates[index].dapp_prb_limits = ov.prb_limits;
      candidates[index].dapp_prb_assign = ov.prb_assign;
      candidates[index].dapp_prb_pref   = ov.prb_pref;
      candidates[index].dapp_tpc        = ov.tpc;
      candidates[index].dapp_layers     = ov.layers;
    }
    if ((output.flags & OCUDU_DAPP_SCHEDULER_OUTPUT_AVOID_PRB_MASK_V1) != 0) {
      for (uint32_t prb = 0; prb != workspace->requested_avoidance.size(); ++prb) {
        workspace->committed_avoidance[prb] |= workspace->requested_avoidance[prb];
      }
      accepted_avoidance_slot = transmission_slot;
    }
    const uint64_t commit_end  = monotonic_now_ns();
    measurement.commit_time_ns = elapsed_ns(commit_start, commit_end);
    if (elapsed_ns(boundary_start_ns, commit_end) > static_cast<uint64_t>(deadline.count())) {
      for (uint32_t index = 0; index != candidates.size(); ++index) {
        candidates[index].priority = workspace->candidates[index].conventional_priority;
        candidates[index].dapp_mcs.reset();
        candidates[index].dapp_max_mcs.reset();
        candidates[index].dapp_prb_limits.reset();
        candidates[index].dapp_prb_assign.reset();
        candidates[index].dapp_prb_pref.reset();
        candidates[index].dapp_tpc.reset();
        candidates[index].dapp_layers.reset();
      }
      accepted_avoidance_slot.clear();
      if (direction == OCUDU_DAPP_SCHEDULER_UL_V1) {
        prepare_authoritative_avoidance(transmission_slot);
      }
      const uint64_t rollback_end = monotonic_now_ns();
      measurement.deadline_miss   = true;
      measurement.commit_time_ns  = elapsed_ns(commit_start, rollback_end);
      record(dapp::class_b_policy_outcome::fallback, rollback_end);
      return;
    }
    record(dapp::class_b_policy_outcome::accepted, commit_end);
  }

  bool validate(const ocudu_dapp_scheduler_input_v1&  input,
                const ocudu_dapp_scheduler_output_v1& output,
                const ocudu_dapp_tensor_view_v1&      expected_avoidance,
                uint64_t                              generation,
                uint32_t                              count) noexcept
  {
    constexpr uint32_t output_minimum = offsetof(ocudu_dapp_scheduler_output_v1, reserved);
    constexpr uint32_t intent_minimum = offsetof(ocudu_dapp_scheduler_intent_v1, reserved);
    if (output.struct_size < output_minimum || output.abi_major != OCUDU_DAPP_ABI_MAJOR_V1 ||
        output.intents != workspace->intents.data() || output.intent_capacity != count || output.nof_intents != count ||
        output.decision_generation != generation ||
        (output.flags & ~OCUDU_DAPP_SCHEDULER_OUTPUT_AVOID_PRB_MASK_V1) != 0 ||
        !dapp::same_tensor_storage(expected_avoidance, output.avoid_prb_mask)) {
      return false;
    }
    // Intent flags permitted for this invocation.
    uint32_t allowed_intent = OCUDU_DAPP_SCHEDULER_INTENT_PRIORITY_V1 | OCUDU_DAPP_SCHEDULER_INTENT_FORBID_V1;
    if ((input.allowed_output_flags & OCUDU_DAPP_SCHEDULER_ALLOW_MCS_V1) != 0) {
      allowed_intent |= OCUDU_DAPP_SCHEDULER_INTENT_MCS_V1;
    }
    if ((input.allowed_output_flags & OCUDU_DAPP_SCHEDULER_ALLOW_PRB_LIMITS_V1) != 0) {
      allowed_intent |= OCUDU_DAPP_SCHEDULER_INTENT_PRB_LIMITS_V1;
    }
    if ((input.allowed_output_flags &
         (OCUDU_DAPP_SCHEDULER_ALLOW_PRB_PREFERENCE_V1 | OCUDU_DAPP_SCHEDULER_ALLOW_PRB_ASSIGNMENT_V1)) != 0) {
      allowed_intent |= OCUDU_DAPP_SCHEDULER_INTENT_PRB_PREFERENCE_V1;
    }
    if ((input.allowed_output_flags & OCUDU_DAPP_SCHEDULER_ALLOW_TPC_V1) != 0) {
      allowed_intent |= OCUDU_DAPP_SCHEDULER_INTENT_TPC_V1;
    }
    if ((input.allowed_output_flags & OCUDU_DAPP_SCHEDULER_ALLOW_LAYERS_V1) != 0) {
      allowed_intent |= OCUDU_DAPP_SCHEDULER_INTENT_LAYERS_V1;
    }

    std::fill_n(workspace->seen.begin(), count, uint8_t{0});
    for (uint32_t index = 0; index != count; ++index) {
      workspace->overrides[index] = {};
    }
    // Working paint for joint hard packing validation.
    std::vector<uint8_t> pack_used(input.cell_nof_prbs, uint8_t{0});
    uint32_t             pack_total = 0;
    const bool           hard_pack = (input.allowed_output_flags & OCUDU_DAPP_SCHEDULER_ALLOW_PRB_ASSIGNMENT_V1) != 0 &&
                           input.free_vrb_context_present;
    const auto* free_mask = hard_pack ? reinterpret_cast<const uint8_t*>(input.free_vrb_mask.native_handle) : nullptr;

    for (uint32_t index = 0; index != count; ++index) {
      const auto& intent = output.intents[index];
      if (intent.struct_size < intent_minimum || intent.candidate_id >= count || workspace->seen[intent.candidate_id]) {
        return false;
      }
      workspace->seen[intent.candidate_id] = 1;
      if ((intent.flags & ~allowed_intent) != 0 || ((intent.flags & OCUDU_DAPP_SCHEDULER_INTENT_PRIORITY_V1) != 0) ==
                                                       ((intent.flags & OCUDU_DAPP_SCHEDULER_INTENT_FORBID_V1) != 0)) {
        return false;
      }
      const uint32_t cid = intent.candidate_id;
      if ((intent.flags & OCUDU_DAPP_SCHEDULER_INTENT_FORBID_V1) != 0) {
        workspace->priorities[cid] = forbid_sched_priority;
        continue;
      }
      if (!std::isfinite(intent.priority) || intent.priority == forbid_sched_priority) {
        return false;
      }
      workspace->priorities[cid] = intent.priority;
      auto& ov                   = workspace->overrides[cid];

      if ((intent.flags & OCUDU_DAPP_SCHEDULER_INTENT_MCS_V1) != 0) {
        const auto& cand = workspace->candidates[cid];
        // Table-aware max: 256QAM or transform precoder → 27; else 28 (host get_max_mcs_ul).
        const uint8_t table_max =
            (cand.mcs_table_id == OCUDU_DAPP_SCHEDULER_MCS_TABLE_QAM256_V1 || cand.transform_precoder != 0) ? 27 : 28;
        if (intent.recommended_mcs < 0 || intent.recommended_mcs > table_max ||
            intent.maximum_mcs < intent.recommended_mcs) {
          return false;
        }
        if ((cand.flags & OCUDU_DAPP_SCHEDULER_CANDIDATE_EXPERT_MCS_VALID_V1) != 0) {
          if (intent.recommended_mcs < cand.expert_mcs_min || intent.recommended_mcs > cand.expert_mcs_max) {
            return false;
          }
        }
        if ((cand.flags & OCUDU_DAPP_SCHEDULER_CANDIDATE_MCS_VALID_V1) != 0) {
          const int delta = std::abs(static_cast<int>(intent.recommended_mcs) - static_cast<int>(cand.recommended_mcs));
          if (delta > guardrails.max_mcs_delta_from_host) {
            return false;
          }
        }
        ov.mcs = sch_mcs_index{static_cast<uint8_t>(intent.recommended_mcs)};
        ov.max_mcs =
            sch_mcs_index{static_cast<uint8_t>(std::min<int16_t>(intent.maximum_mcs, static_cast<int16_t>(table_max)))};
      }
      if ((intent.flags & OCUDU_DAPP_SCHEDULER_INTENT_LAYERS_V1) != 0) {
        if (intent.nof_layers < 1 || intent.nof_layers > 4) {
          return false;
        }
        ov.layers = intent.nof_layers;
      }
      if ((intent.flags & OCUDU_DAPP_SCHEDULER_INTENT_TPC_V1) != 0) {
        if (intent.tpc_command < 0 || intent.tpc_command > 3) {
          return false;
        }
        ov.tpc = static_cast<uint8_t>(intent.tpc_command);
      }
      if ((intent.flags & OCUDU_DAPP_SCHEDULER_INTENT_PRB_LIMITS_V1) != 0) {
        if (intent.max_prbs == 0 || intent.min_prbs > intent.max_prbs || intent.max_prbs > input.cell_nof_prbs) {
          return false;
        }
        ov.prb_limits = interval<unsigned>{intent.min_prbs, intent.max_prbs + 1u};
      }
      if ((intent.flags & OCUDU_DAPP_SCHEDULER_INTENT_PRB_PREFERENCE_V1) != 0) {
        if (intent.preferred_nof_prbs == 0 ||
            static_cast<uint32_t>(intent.preferred_start_prb) + intent.preferred_nof_prbs > input.cell_nof_prbs) {
          return false;
        }
        const vrb_interval vrbs{intent.preferred_start_prb,
                                static_cast<unsigned>(intent.preferred_start_prb + intent.preferred_nof_prbs)};
        if (!hard_pack) {
          ov.prb_pref = vrbs;
        } else {
          if (free_mask == nullptr) {
            return false;
          }
          // Occupancy classifies hard assign vs soft hint. An in-band run that
          // is not free, overlaps another hard assign, or misses the slice
          // budget must not reject MCS/TPC/priority.
          bool assignable = true;
          for (uint16_t p = 0; p != intent.preferred_nof_prbs; ++p) {
            const uint32_t prb = static_cast<uint32_t>(intent.preferred_start_prb) + p;
            if (free_mask[prb] == 0 || pack_used[prb] != 0) {
              assignable = false;
              break;
            }
          }
          if (assignable && input.remaining_slice_prbs != 0 &&
              pack_total + intent.preferred_nof_prbs > input.remaining_slice_prbs) {
            assignable = false;
          }
          if (assignable) {
            for (uint16_t p = 0; p != intent.preferred_nof_prbs; ++p) {
              pack_used[static_cast<uint32_t>(intent.preferred_start_prb) + p] = 1;
            }
            pack_total += intent.preferred_nof_prbs;
            ov.prb_assign = vrbs;
          } else {
            ov.prb_pref = vrbs;
          }
        }
      }
    }
    if ((output.flags & OCUDU_DAPP_SCHEDULER_OUTPUT_AVOID_PRB_MASK_V1) != 0) {
      if (input.direction != OCUDU_DAPP_SCHEDULER_UL_V1 || input.interference_context_present == 0 ||
          guardrails.max_ul_prbs_avoided == 0 ||
          (input.allowed_output_flags & OCUDU_DAPP_SCHEDULER_ALLOW_AVOID_PRB_MASK_V1) == 0) {
        return false;
      }
      const auto* interference_mask = reinterpret_cast<const uint8_t*>(input.optional_interference_mask.native_handle);
      const auto* requested_mask    = reinterpret_cast<const uint8_t*>(output.avoid_prb_mask.native_handle);
      if (interference_mask == nullptr || requested_mask != workspace->requested_avoidance.data()) {
        return false;
      }
      uint32_t avoided          = 0;
      uint32_t combined_avoided = 0;
      for (uint32_t prb = 0; prb != workspace->requested_avoidance.size(); ++prb) {
        if (requested_mask[prb] > 1 || (requested_mask[prb] != 0 && interference_mask[prb] == 0)) {
          return false;
        }
        avoided += requested_mask[prb];
        combined_avoided += requested_mask[prb] != 0 || workspace->committed_avoidance[prb] != 0;
      }
      if (avoided > guardrails.max_ul_prbs_avoided || combined_avoided >= workspace->committed_avoidance.size()) {
        return false;
      }
    }
    return true;
  }

  std::unique_ptr<scheduler_policy>                  fallback;
  std::shared_ptr<scheduler_dapp_target>             target;
  uint32_t                                           lane;
  scheduler_dapp_policy_context                      context;
  std::shared_ptr<scheduler_dapp_interference_cache> interference;
  std::shared_ptr<quiet_reservation_manager>         reservations;
  scheduler_dapp_guardrails                          guardrails;
  std::chrono::nanoseconds                           deadline;
  scheduler_dapp_policy_mode                         mode;
  std::unique_ptr<scheduler_workspace>               workspace;
  uint64_t                                           invocation_sequence{};
  slot_point                                         accepted_avoidance_slot;
};

class scheduler_hook_provider_impl final : public scheduler_dapp_hook_provider
{
public:
  scheduler_hook_provider_impl(std::shared_ptr<scheduler_dapp_target>             target_,
                               std::shared_ptr<scheduler_dapp_interference_cache> interference_,
                               std::shared_ptr<quiet_reservation_manager>         reservations_,
                               std::shared_ptr<scheduler_dapp_ul_plan_sink>       plan_sink_,
                               scheduler_dapp_guardrails                          guardrails_,
                               std::chrono::nanoseconds                           deadline_,
                               scheduler_dapp_policy_mode                         mode_,
                               std::shared_ptr<scheduler_dapp_ue_event_sink>      ue_events_) :
    target(std::move(target_)),
    interference(std::move(interference_)),
    reservations(std::move(reservations_)),
    plan_sink(std::move(plan_sink_)),
    ue_events(std::move(ue_events_)),
    guardrails(guardrails_),
    deadline(deadline_),
    mode(mode_)
  {
  }

  void publish_ul_plan(uint32_t cell_id, slot_point slot, const ul_sched_result& result) noexcept override
  {
    if (plan_sink) {
      plan_sink->publish(cell_id, slot, result);
    }
  }

  void publish_srs_config(const scheduler_dapp_srs_config_event& event) noexcept override
  {
    if (ue_events) {
      ue_events->publish_srs_config(event);
    }
  }

  bool ta_commands_active() noexcept override { return ue_events && ue_events->ta_commands_active(); }

  void publish_ta_command(slot_point slot, const scheduler_dapp_ta_command_event& event) noexcept override
  {
    if (ue_events) {
      ue_events->publish_ta_command(slot, event);
    }
  }

  std::unique_ptr<scheduler_policy> decorate_scheduler_policy(std::unique_ptr<scheduler_policy> fallback,
                                                              scheduler_dapp_policy_context     context) override
  {
    const uint32_t lane             = next_lane.fetch_add(1, std::memory_order_relaxed);
    auto           local_guardrails = guardrails;
    local_guardrails.max_ul_prbs_avoided =
        context.cell_nof_prbs == 0 ? 0 : std::min(local_guardrails.max_ul_prbs_avoided, context.cell_nof_prbs - 1);
    return create_scheduler_dapp_policy(
        std::move(fallback), target, lane, context, interference, local_guardrails, deadline, reservations, mode);
  }

private:
  std::shared_ptr<scheduler_dapp_target>             target;
  std::shared_ptr<scheduler_dapp_interference_cache> interference;
  std::shared_ptr<quiet_reservation_manager>         reservations;
  std::shared_ptr<scheduler_dapp_ul_plan_sink>       plan_sink;
  std::shared_ptr<scheduler_dapp_ue_event_sink>      ue_events;
  scheduler_dapp_guardrails                          guardrails;
  std::chrono::nanoseconds                           deadline;
  scheduler_dapp_policy_mode                         mode;
  std::atomic<uint32_t>                              next_lane{};
};

class native_scheduler_target final : public scheduler_dapp_target
{
public:
  explicit native_scheduler_target(std::shared_ptr<dapp::native_hook_point> point_) : point(std::move(point_)) {}

  bool                           active() const noexcept override { return point->active(); }
  bool                           bound() const noexcept override { return point->bound(); }
  uint32_t                       lane_count() const noexcept override { return point->lane_count(); }
  uint32_t                       worker_id(uint32_t lane) const noexcept override { return point->worker_id(lane); }
  dapp::native_invocation_result invoke(uint32_t                             lane,
                                        const ocudu_dapp_scheduler_input_v1& input,
                                        ocudu_dapp_scheduler_output_v1&      output) noexcept override
  {
    return point->invoke(lane, input, output);
  }
  void report_invalid_output(uint32_t lane, const ocudu_dapp_invocation_v1& invocation) noexcept override
  {
    point->report_failure(lane, invocation, OCUDU_DAPP_INVALID_OUTPUT_V1);
  }
  void record_measurement(uint32_t lane, const dapp::class_b_policy_measurement& measurement) noexcept override
  {
    point->record_class_b_policy(lane, measurement);
  }

private:
  std::shared_ptr<dapp::native_hook_point> point;
};

} // namespace

std::unique_ptr<scheduler_policy>
ocudu::create_scheduler_dapp_policy(std::unique_ptr<scheduler_policy>                  fallback,
                                    std::shared_ptr<scheduler_dapp_target>             target,
                                    uint32_t                                           lane,
                                    scheduler_dapp_policy_context                      context,
                                    std::shared_ptr<scheduler_dapp_interference_cache> interference,
                                    scheduler_dapp_guardrails                          guardrails,
                                    std::chrono::nanoseconds                           deadline,
                                    std::shared_ptr<quiet_reservation_manager>         reservations,
                                    scheduler_dapp_policy_mode                         mode) noexcept
{
  if (!fallback || !target || lane >= target->lane_count() || context.cell_nof_prbs == 0 ||
      context.max_candidates == 0 || context.max_candidates > OCUDU_DAPP_SCHEDULER_MAX_CANDIDATES_V1 ||
      deadline.count() <= 0) {
    return fallback;
  }
  std::unique_ptr<scheduler_workspace> workspace;
  try {
    workspace = std::make_unique<scheduler_workspace>(context.max_candidates, context.cell_nof_prbs);
  } catch (...) {
    return fallback;
  }
  auto* result = new (std::nothrow) scheduler_policy_decorator(std::move(fallback),
                                                               std::move(target),
                                                               lane,
                                                               context,
                                                               std::move(interference),
                                                               std::move(reservations),
                                                               guardrails,
                                                               deadline,
                                                               mode,
                                                               std::move(workspace));
  return result != nullptr ? std::unique_ptr<scheduler_policy>(result) : std::move(fallback);
}

std::shared_ptr<scheduler_dapp_hook_provider>
ocudu::create_scheduler_dapp_hook_provider(std::shared_ptr<scheduler_dapp_target>             target,
                                           std::shared_ptr<scheduler_dapp_interference_cache> interference,
                                           scheduler_dapp_guardrails                          guardrails,
                                           std::chrono::nanoseconds                           deadline,
                                           std::shared_ptr<quiet_reservation_manager>         reservations,
                                           std::shared_ptr<scheduler_dapp_ul_plan_sink>       plan_sink,
                                           scheduler_dapp_policy_mode                         mode,
                                           std::shared_ptr<scheduler_dapp_ue_event_sink>      ue_events) noexcept
{
  if (!target || target->lane_count() == 0 || deadline.count() <= 0 || deadline > std::chrono::seconds(1)) {
    return {};
  }
  try {
    return std::make_shared<scheduler_hook_provider_impl>(std::move(target),
                                                          std::move(interference),
                                                          std::move(reservations),
                                                          std::move(plan_sink),
                                                          guardrails,
                                                          deadline,
                                                          mode,
                                                          std::move(ue_events));
  } catch (...) {
    return {};
  }
}

std::shared_ptr<scheduler_dapp_target>
ocudu::create_native_scheduler_dapp_target(std::shared_ptr<dapp::native_hook_point> point) noexcept
{
  if (!point || point->interface_id() != OCUDU_DAPP_INTERFACE_SCHEDULER_V1) {
    return {};
  }
  try {
    return std::make_shared<native_scheduler_target>(std::move(point));
  } catch (...) {
    return {};
  }
}
