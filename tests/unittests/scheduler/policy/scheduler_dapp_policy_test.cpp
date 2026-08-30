// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "lib/scheduler/policy/scheduler_policy.h"
#include "tests/test_doubles/scheduler/scheduler_config_helper.h"
#include "tests/unittests/scheduler/test_utils/scheduler_test_simulator.h"
#include "ocudu/scheduler/dapp/scheduler_dapp_policy.h"
#include "ocudu/scheduler/result/sched_result.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <gtest/gtest.h>
#include <vector>

using namespace ocudu;

namespace {

class test_target final : public scheduler_dapp_target
{
public:
  enum class behavior : uint8_t {
    priority,
    forbid,
    late_forbid,
    invalid,
    replace_storage,
    avoid,
    late_avoid,
    invalid_avoid,
    prb_pref_overlap,
    prb_pref_legal,
    prb_pref_malformed
  };

  bool     active() const noexcept override { return enabled.load(std::memory_order_relaxed); }
  uint32_t lane_count() const noexcept override { return 8; }
  uint32_t worker_id(uint32_t lane) const noexcept override { return 1000 + lane; }

  dapp::native_invocation_result invoke(uint32_t                             lane,
                                        const ocudu_dapp_scheduler_input_v1& input,
                                        ocudu_dapp_scheduler_output_v1&      output) noexcept override
  {
    if (input.direction == OCUDU_DAPP_SCHEDULER_DL_V1) {
      dl_calls.fetch_add(1, std::memory_order_relaxed);
    } else {
      ul_calls.fetch_add(1, std::memory_order_relaxed);
    }
    if (input.interference_context_present != 0) {
      interference_sequence.store(input.interference_context.source_sequence, std::memory_order_relaxed);
      const auto* mask = reinterpret_cast<const uint8_t*>(input.optional_interference_mask.native_handle);
      interference_first_prb.store(mask != nullptr ? mask[0] : 0, std::memory_order_relaxed);
    }
    allowed_output_flags.store(input.allowed_output_flags, std::memory_order_relaxed);
    last_lane.store(lane, std::memory_order_relaxed);
    last_worker.store(input.invocation.worker_id, std::memory_order_relaxed);
    last_cell.store(input.invocation.cell_id, std::memory_order_relaxed);
    last_slice.store(input.slice_id, std::memory_order_relaxed);
    last_candidates.store(input.nof_candidates, std::memory_order_relaxed);
    last_direction.store(input.direction, std::memory_order_relaxed);
    if (input.nof_candidates != 0) {
      const auto& candidate = input.candidates[0];
      last_candidate_flags.store(candidate.flags, std::memory_order_relaxed);
      last_hol_delay_us.store(candidate.hol_delay_us, std::memory_order_relaxed);
      last_average_rate_bps.store(candidate.average_rate_bps, std::memory_order_relaxed);
      last_bler.store(candidate.bler, std::memory_order_relaxed);
      last_bler_nof_observations.store(candidate.bler_nof_observations, std::memory_order_relaxed);
    }
    const behavior selected = mode.load(std::memory_order_relaxed);
    if (selected == behavior::replace_storage) {
      ++output.intent_capacity;
      output.nof_intents         = input.nof_candidates;
      output.decision_generation = input.candidate_generation;
      return {OCUDU_DAPP_OK_V1, false};
    }
    for (uint32_t index = 0; index != input.nof_candidates; ++index) {
      auto& intent        = output.intents[index];
      intent              = {};
      intent.struct_size  = sizeof(intent);
      intent.candidate_id = input.candidates[index].candidate_id;
      if (selected == behavior::forbid || selected == behavior::late_forbid) {
        intent.flags = OCUDU_DAPP_SCHEDULER_INTENT_FORBID_V1;
      } else {
        intent.flags    = OCUDU_DAPP_SCHEDULER_INTENT_PRIORITY_V1;
        intent.priority = static_cast<double>(input.candidates[index].pending_bytes);
      }
    }
    if (selected == behavior::invalid) {
      output.intents[0].flags |= OCUDU_DAPP_SCHEDULER_INTENT_MCS_V1;
    }
    if (selected == behavior::prb_pref_overlap || selected == behavior::prb_pref_legal ||
        selected == behavior::prb_pref_malformed) {
      last_cell_nof_prbs.store(input.cell_nof_prbs, std::memory_order_relaxed);
      if (input.nof_candidates != 0) {
        last_first_ue.store(input.candidates[0].ue_index, std::memory_order_relaxed);
      }
      const uint16_t win = 16;
      for (uint32_t index = 0; index != input.nof_candidates; ++index) {
        auto& intent = output.intents[index];
        if (selected == behavior::prb_pref_malformed) {
          intent.flags |= OCUDU_DAPP_SCHEDULER_INTENT_PRB_PREFERENCE_V1;
          intent.preferred_start_prb = 0;
          intent.preferred_nof_prbs  = static_cast<uint16_t>(input.cell_nof_prbs + 1);
          continue;
        }
        const auto& cand = input.candidates[index];
        uint8_t     hi   = 28;
        if ((cand.flags & OCUDU_DAPP_SCHEDULER_CANDIDATE_EXPERT_MCS_VALID_V1) != 0) {
          hi = cand.expert_mcs_max;
        }
        uint8_t want = static_cast<uint8_t>(cand.recommended_mcs);
        if ((input.allowed_output_flags & OCUDU_DAPP_SCHEDULER_ALLOW_MCS_V1) != 0) {
          if (want < hi) {
            want = static_cast<uint8_t>(std::min<int>(hi, static_cast<int>(want) + 2));
          }
          intent.flags |= OCUDU_DAPP_SCHEDULER_INTENT_MCS_V1;
          intent.recommended_mcs = want;
          intent.maximum_mcs     = hi;
        }
        last_emitted_mcs.store(want, std::memory_order_relaxed);
        intent.flags |= OCUDU_DAPP_SCHEDULER_INTENT_PRB_PREFERENCE_V1;
        if (selected == behavior::prb_pref_overlap) {
          intent.preferred_start_prb = 0;
          intent.preferred_nof_prbs  = static_cast<uint16_t>(std::min<uint32_t>(win, input.cell_nof_prbs));
        } else {
          const uint16_t slot        = static_cast<uint16_t>(cand.ue_index % 4);
          intent.preferred_start_prb = static_cast<uint16_t>(slot * 8);
          intent.preferred_nof_prbs  = 8;
        }
      }
    }
    if (selected == behavior::avoid || selected == behavior::late_avoid || selected == behavior::invalid_avoid) {
      auto* mask = reinterpret_cast<uint8_t*>(output.avoid_prb_mask.native_handle);
      if (mask != nullptr && output.avoid_prb_mask.dimensions[0] == input.cell_nof_prbs) {
        std::fill_n(mask, input.cell_nof_prbs, uint8_t{0});
        const uint32_t count = selected != behavior::invalid_avoid ? 8 : 9;
        std::fill_n(mask, std::min(count, input.cell_nof_prbs), uint8_t{1});
        output.flags |= OCUDU_DAPP_SCHEDULER_OUTPUT_AVOID_PRB_MASK_V1;
      }
    }
    output.nof_intents         = input.nof_candidates;
    output.decision_generation = input.candidate_generation;
    if (selected == behavior::late_forbid || selected == behavior::late_avoid) {
      while (static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                       std::chrono::steady_clock::now().time_since_epoch())
                                       .count()) <= input.invocation.deadline_monotonic_ns) {
      }
    }
    return {OCUDU_DAPP_OK_V1, false};
  }

  void report_invalid_output(uint32_t, const ocudu_dapp_invocation_v1&) noexcept override
  {
    invalid_reports.fetch_add(1, std::memory_order_relaxed);
  }

  void record_measurement(uint32_t, const dapp::class_b_policy_measurement& measurement) noexcept override
  {
    measurement_calls.fetch_add(1, std::memory_order_relaxed);
    conventional_time_ns.fetch_add(measurement.conventional_time_ns, std::memory_order_relaxed);
    feature_time_ns.fetch_add(measurement.feature_time_ns, std::memory_order_relaxed);
    dapp_time_ns.fetch_add(measurement.dapp_time_ns, std::memory_order_relaxed);
    validation_time_ns.fetch_add(measurement.validation_time_ns, std::memory_order_relaxed);
    commit_time_ns.fetch_add(measurement.commit_time_ns, std::memory_order_relaxed);
    total_time_ns.fetch_add(measurement.total_time_ns, std::memory_order_relaxed);
    deadline_misses.fetch_add(measurement.deadline_miss, std::memory_order_relaxed);
    switch (measurement.outcome) {
      case dapp::class_b_policy_outcome::accepted:
        accepted.fetch_add(1, std::memory_order_relaxed);
        break;
      case dapp::class_b_policy_outcome::evaluation_accepted:
        evaluation_accepted.fetch_add(1, std::memory_order_relaxed);
        break;
      case dapp::class_b_policy_outcome::fallback:
        fallbacks.fetch_add(1, std::memory_order_relaxed);
        break;
      case dapp::class_b_policy_outcome::validation_rejection:
        validation_rejections.fetch_add(1, std::memory_order_relaxed);
        break;
    }
  }

  std::atomic<bool>     enabled{true};
  std::atomic<behavior> mode{behavior::priority};
  std::atomic<uint32_t> dl_calls{};
  std::atomic<uint32_t> ul_calls{};
  std::atomic<uint32_t> invalid_reports{};
  std::atomic<uint32_t> last_lane{};
  std::atomic<uint32_t> last_worker{};
  std::atomic<uint32_t> last_cell{};
  std::atomic<uint32_t> last_slice{};
  std::atomic<uint32_t> last_candidates{};
  std::atomic<uint32_t> last_direction{};
  std::atomic<uint32_t> last_candidate_flags{};
  std::atomic<uint64_t> last_hol_delay_us{};
  std::atomic<uint64_t> last_average_rate_bps{};
  std::atomic<float>    last_bler{};
  std::atomic<uint32_t> last_bler_nof_observations{};
  std::atomic<uint64_t> interference_sequence{};
  std::atomic<uint32_t> interference_first_prb{};
  std::atomic<uint32_t> allowed_output_flags{};
  std::atomic<uint64_t> measurement_calls{};
  std::atomic<uint64_t> conventional_time_ns{};
  std::atomic<uint64_t> feature_time_ns{};
  std::atomic<uint64_t> dapp_time_ns{};
  std::atomic<uint64_t> validation_time_ns{};
  std::atomic<uint64_t> commit_time_ns{};
  std::atomic<uint64_t> total_time_ns{};
  std::atomic<uint64_t> accepted{};
  std::atomic<uint64_t> evaluation_accepted{};
  std::atomic<uint64_t> fallbacks{};
  std::atomic<uint64_t> validation_rejections{};
  std::atomic<uint64_t> deadline_misses{};
  std::atomic<uint32_t> last_cell_nof_prbs{};
  std::atomic<uint32_t> last_first_ue{};
  std::atomic<uint8_t>  last_emitted_mcs{};
};

class no_op_policy final : public scheduler_policy
{
public:
  void add_ue(du_ue_index_t) override {}
  void rem_ue(du_ue_index_t) override {}
  void compute_ue_dl_priorities(slot_point, slot_point, span<ue_newtx_candidate>) override {}
  void compute_ue_ul_priorities(slot_point, slot_point, span<ue_newtx_candidate>) override {}
  void save_dl_newtx_grants(span<const dl_msg_alloc>) override {}
  void save_ul_newtx_grants(span<const ul_sched_info>) override {}
};

class plan_sink_spy final : public scheduler_dapp_ul_plan_sink
{
public:
  void publish(uint32_t cell_id, slot_point slot, const ul_sched_result& result) noexcept override
  {
    ++calls;
    last_cell   = cell_id;
    last_slot   = slot;
    last_result = &result;
  }

  uint32_t               calls{};
  uint32_t               last_cell{};
  slot_point             last_slot;
  const ul_sched_result* last_result{};
};

struct scheduler_fixture {
  std::shared_ptr<test_target>                       target = std::make_shared<test_target>();
  std::shared_ptr<scheduler_dapp_interference_cache> interference =
      create_scheduler_dapp_interference_cache(std::chrono::seconds(1));
  scheduler_dapp_policy_mode        policy_mode;
  std::chrono::nanoseconds          deadline;
  scheduler_dapp_guardrails         guardrails;
  scheduler_test_sim_config         config;
  scheduler_test_simulator          simulator;
  sched_ue_creation_request_message ue_request;
  sched_ue_creation_request_message ue2_request{};

  explicit scheduler_fixture(scheduler_dapp_policy_mode mode        = scheduler_dapp_policy_mode::authoritative,
                             std::chrono::nanoseconds   deadline_   = std::chrono::seconds(1),
                             scheduler_dapp_guardrails  guardrails_ = scheduler_dapp_guardrails{8}) :
    policy_mode(mode),
    deadline(deadline_),
    guardrails(guardrails_),
    config{},
    simulator(make_config()),
    ue_request(add_cell_and_ue())
  {
  }

private:
  scheduler_test_sim_config make_config()
  {
    config.dapp_hooks =
        create_scheduler_dapp_hook_provider(target, interference, guardrails, deadline, {}, {}, policy_mode);
    return config;
  }

  sched_ue_creation_request_message add_cell_and_ue()
  {
    auto cell = sched_config_helper::make_default_sched_cell_configuration_request();
    simulator.add_cell(cell);
    auto ue =
        sched_config_helper::create_default_sched_ue_creation_request(simulator.cell_cfg().params, {LCID_MIN_DRB});
    simulator.add_ue(ue, true);
    return ue;
  }

public:
  void push_dl_data(unsigned bytes = 10000)
  {
    simulator.push_dl_buffer_state(
        dl_buffer_state_indication_message{ue_request.ue_index, LCID_MIN_DRB, bytes, simulator.next_slot_rx()});
  }

  bool run_until_next_dl_call()
  {
    const uint32_t before = target->dl_calls.load(std::memory_order_relaxed);
    return simulator.run_slot_until([&] { return target->dl_calls.load(std::memory_order_relaxed) != before; }, 100);
  }

  /// Run slots until the UE actually gets a DL PDCCH.
  ///
  /// Distinct from run_until_next_dl_call(), which only waits for the policy to
  /// be *invoked*. A policy call does not guarantee a grant: the scheduler can
  /// still drop the PDSCH for reasons that have nothing to do with the policy --
  /// most commonly "UCI allocation failed" when no dedicated PUCCH resource
  /// indicator is free in that slot. Asserting a grant on the first invoked slot
  /// therefore fails whenever PUCCH resources happen to be exhausted there,
  /// which measured at roughly one run in eight.
  bool run_until_dl_pdcch()
  {
    return simulator.run_slot_until([&] { return simulator.find_ue_dl_pdcch(ue_request.crnti) != nullptr; }, 100);
  }

  void add_second_ue()
  {
    ue2_request =
        sched_config_helper::create_default_sched_ue_creation_request(simulator.cell_cfg().params, {LCID_MIN_DRB});
    ue2_request.ue_index = to_du_ue_index(1);
    ue2_request.crnti    = to_rnti(0x4602);
    simulator.add_ue(ue2_request, true);
  }

  void push_ul_data_for(const sched_ue_creation_request_message& ue, unsigned bytes = 10000)
  {
    simulator.push_bsr(ul_bsr_indication_message{to_du_cell_index(0),
                                                 ue.ue_index,
                                                 ue.crnti,
                                                 bsr_format::SHORT_BSR,
                                                 {ul_bsr_lcg_report{uint_to_lcg_id(2), bytes}}});
  }

  void push_ul_data(unsigned bytes = 10000) { push_ul_data_for(ue_request, bytes); }

  bool run_until_next_ul_call()
  {
    const uint32_t before = target->ul_calls.load(std::memory_order_relaxed);
    return simulator.run_slot_until([&] { return target->ul_calls.load(std::memory_order_relaxed) != before; }, 100);
  }

  const ul_sched_info* run_until_pusch()
  {
    const ul_sched_info* result = nullptr;
    simulator.run_slot_until(
        [&] {
          for (const auto& pusch : simulator.last_sched_result()->ul.puschs) {
            if (pusch.pusch_cfg.rnti == ue_request.crnti) {
              result = &pusch;
              return true;
            }
          }
          return false;
        },
        100);
    return result;
  }
};

ocudu_dapp_radio_time_v1 to_radio_time(slot_point slot) noexcept
{
  return {static_cast<uint16_t>(slot.sfn()),
          static_cast<uint16_t>(slot.slot_index()),
          static_cast<uint16_t>(slot.numerology()),
          0};
}

uint64_t monotonic_now_ns() noexcept
{
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

void publish_interference(scheduler_fixture& test, span<const uint8_t> mask)
{
  const slot_point                   next = test.simulator.next_slot.without_hyper_sfn();
  scheduler_dapp_interference_update update{};
  update.context.struct_size             = sizeof(update.context);
  update.context.schema_id               = OCUDU_DAPP_SCHEDULER_CONTEXT_INTERFERENCE_MAP_V1;
  update.context.schema_version          = 1;
  update.context.producer_instance_id[0] = 91;
  update.context.producer_generation     = 2;
  update.context.source_sequence         = 77;
  update.context.cell_id                 = 0;
  update.context.confidence_q16          = 1U << 15U;
  update.context.created                 = to_radio_time(next - 1);
  update.context.valid_from              = to_radio_time(next);
  update.context.valid_until             = to_radio_time(next + 100);
  update.prb_confidence                  = mask;
  update.publish_monotonic_ns            = monotonic_now_ns();
  ASSERT_EQ(test.interference->publish(update), scheduler_dapp_context_result::accepted);
}

} // namespace

TEST(scheduler_dapp_policy, publishes_stable_features_and_applies_valid_priority_intent)
{
  scheduler_fixture test;
  test.push_dl_data();
  ASSERT_TRUE(test.run_until_dl_pdcch());
  EXPECT_EQ(test.target->last_cell.load(std::memory_order_relaxed), 0U);
  EXPECT_EQ(test.target->last_slice.load(std::memory_order_relaxed), DEFAULT_DRB_RAN_SLICE_ID.value());
  EXPECT_EQ(test.target->last_candidates.load(std::memory_order_relaxed), 1U);
  EXPECT_EQ(test.target->last_worker.load(std::memory_order_relaxed),
            1000U + test.target->last_lane.load(std::memory_order_relaxed));
  EXPECT_GE(test.target->total_time_ns.load(std::memory_order_relaxed),
            test.target->feature_time_ns.load(std::memory_order_relaxed) +
                test.target->dapp_time_ns.load(std::memory_order_relaxed) +
                test.target->validation_time_ns.load(std::memory_order_relaxed) +
                test.target->commit_time_ns.load(std::memory_order_relaxed));
}

TEST(scheduler_dapp_policy, publishes_dl_hol_served_rate_and_first_tx_bler_with_explicit_validity)
{
  scheduler_fixture test;
  test.simulator.auto_uci = true;
  test.simulator.register_uci_handler([](uci_indication& indication) {
    for (auto& pdu : indication.ucis) {
      std::visit(
          [](auto& payload) {
            std::fill(payload.harqs.begin(), payload.harqs.end(), mac_harq_ack_report_status::nack);
          },
          pdu.pdu);
    }
  });
  test.push_dl_data(100000);
  ASSERT_TRUE(test.run_until_next_dl_call());

  uint32_t flags = test.target->last_candidate_flags.load(std::memory_order_relaxed);
  EXPECT_NE(flags & OCUDU_DAPP_SCHEDULER_CANDIDATE_HOL_DELAY_VALID_V1, 0U);
  EXPECT_NE(flags & OCUDU_DAPP_SCHEDULER_CANDIDATE_AVERAGE_RATE_VALID_V1, 0U);
  EXPECT_EQ(flags & OCUDU_DAPP_SCHEDULER_CANDIDATE_BLER_VALID_V1, 0U);
  EXPECT_EQ(test.target->last_average_rate_bps.load(std::memory_order_relaxed), 0U);

  // The simulator supplies NACK first-transmission HARQ feedback. Wait until at
  // least one grant has advanced rate history and its NACK has reached
  // ue_cell, then verify that both values cross the Class-B boundary.
  ASSERT_TRUE(test.simulator.run_slot_until(
      [&] {
        const uint32_t current_flags = test.target->last_candidate_flags.load(std::memory_order_relaxed);
        return (current_flags & OCUDU_DAPP_SCHEDULER_CANDIDATE_BLER_VALID_V1) != 0 &&
               test.target->last_average_rate_bps.load(std::memory_order_relaxed) != 0;
      },
      100));
  EXPECT_EQ(test.target->last_direction.load(std::memory_order_relaxed), OCUDU_DAPP_SCHEDULER_DL_V1);
  EXPECT_FLOAT_EQ(test.target->last_bler.load(std::memory_order_relaxed), 1.0f);
  EXPECT_GE(test.target->last_bler_nof_observations.load(std::memory_order_relaxed), 1U);
  EXPECT_LE(test.target->last_bler_nof_observations.load(std::memory_order_relaxed),
            OCUDU_DAPP_SCHEDULER_BLER_WINDOW_SIZE_V1);
}

TEST(scheduler_dapp_policy, publishes_ul_served_rate_and_first_tx_bler_but_not_synthetic_hol)
{
  scheduler_fixture test;
  test.simulator.auto_crc = true;
  test.push_ul_data(100000);
  ASSERT_TRUE(test.run_until_next_ul_call());

  uint32_t flags = test.target->last_candidate_flags.load(std::memory_order_relaxed);
  EXPECT_EQ(flags & OCUDU_DAPP_SCHEDULER_CANDIDATE_HOL_DELAY_VALID_V1, 0U);
  EXPECT_NE(flags & OCUDU_DAPP_SCHEDULER_CANDIDATE_AVERAGE_RATE_VALID_V1, 0U);
  EXPECT_EQ(flags & OCUDU_DAPP_SCHEDULER_CANDIDATE_BLER_VALID_V1, 0U);
  EXPECT_EQ(test.target->last_average_rate_bps.load(std::memory_order_relaxed), 0U);

  ASSERT_TRUE(test.simulator.run_slot_until(
      [&] {
        const uint32_t current_flags = test.target->last_candidate_flags.load(std::memory_order_relaxed);
        return (current_flags & OCUDU_DAPP_SCHEDULER_CANDIDATE_BLER_VALID_V1) != 0 &&
               test.target->last_average_rate_bps.load(std::memory_order_relaxed) != 0;
      },
      100));
  EXPECT_EQ(test.target->last_direction.load(std::memory_order_relaxed), OCUDU_DAPP_SCHEDULER_UL_V1);
  EXPECT_FLOAT_EQ(test.target->last_bler.load(std::memory_order_relaxed), 0.0f);
  EXPECT_GE(test.target->last_bler_nof_observations.load(std::memory_order_relaxed), 1U);
  EXPECT_LE(test.target->last_bler_nof_observations.load(std::memory_order_relaxed),
            OCUDU_DAPP_SCHEDULER_BLER_WINDOW_SIZE_V1);
}

TEST(scheduler_dapp_policy, forbid_is_authoritative_but_invalid_output_selects_complete_fallback)
{
  scheduler_fixture test;
  test.target->mode.store(test_target::behavior::forbid, std::memory_order_relaxed);
  test.push_dl_data();
  ASSERT_TRUE(test.run_until_next_dl_call());
  EXPECT_EQ(test.simulator.find_ue_dl_pdcch(test.ue_request.crnti), nullptr);

  const uint32_t invalid_before = test.target->invalid_reports.load(std::memory_order_relaxed);
  test.target->mode.store(test_target::behavior::invalid, std::memory_order_relaxed);
  // Wait for the grant itself rather than for the policy call, so an unrelated
  // UCI allocation failure in one slot does not read as a fallback failure.
  ASSERT_TRUE(test.run_until_dl_pdcch());
  EXPECT_GT(test.target->invalid_reports.load(std::memory_order_relaxed), invalid_before);
}

TEST(scheduler_dapp_policy, evaluation_only_validates_outputs_but_preserves_complete_conventional_policy)
{
  scheduler_fixture test(scheduler_dapp_policy_mode::evaluation_only);
  test.target->mode.store(test_target::behavior::forbid, std::memory_order_relaxed);
  test.push_dl_data();
  // Evaluation-only never lets the dApp block a grant, so wait for the grant
  // rather than for the policy call: how many slots that takes depends on PUCCH
  // resource availability, which is not what this test is about.
  ASSERT_TRUE(test.run_until_dl_pdcch());
  EXPECT_EQ(test.target->accepted.load(std::memory_order_relaxed), 0U);
  EXPECT_GE(test.target->evaluation_accepted.load(std::memory_order_relaxed), 1U);
  EXPECT_EQ(test.target->fallbacks.load(std::memory_order_relaxed), 0U);

  const uint32_t invalid_before      = test.target->invalid_reports.load(std::memory_order_relaxed);
  const uint32_t rejections_before   = test.target->validation_rejections.load(std::memory_order_relaxed);
  const uint32_t measurements_before = test.target->measurement_calls.load(std::memory_order_relaxed);
  test.target->mode.store(test_target::behavior::invalid, std::memory_order_relaxed);
  ASSERT_TRUE(test.run_until_dl_pdcch());
  // Deltas, not absolutes. Each of these counts one policy call, and the number
  // of policy calls is exactly the quantity that varies when a slot fails UCI
  // allocation -- so asserting absolute totals here is asserting the scheduler's
  // slot timing, which was the source of a one-in-four flake.
  EXPECT_GT(test.target->invalid_reports.load(std::memory_order_relaxed), invalid_before);
  EXPECT_GT(test.target->validation_rejections.load(std::memory_order_relaxed), rejections_before);
  EXPECT_GT(test.target->measurement_calls.load(std::memory_order_relaxed), measurements_before);
}

TEST(scheduler_dapp_policy, valid_output_after_the_complete_class_b_deadline_retains_conventional_policy)
{
  scheduler_fixture test(scheduler_dapp_policy_mode::authoritative, std::chrono::microseconds(100));
  test.target->mode.store(test_target::behavior::late_forbid, std::memory_order_relaxed);
  test.push_dl_data();
  ASSERT_TRUE(test.run_until_dl_pdcch());
  EXPECT_EQ(test.target->accepted.load(std::memory_order_relaxed), 0U);
  EXPECT_GT(test.target->fallbacks.load(std::memory_order_relaxed), 0U);
  EXPECT_GT(test.target->deadline_misses.load(std::memory_order_relaxed), 0U);
}

TEST(scheduler_dapp_policy, rejects_an_unavailable_lane_without_replacing_the_conventional_policy)
{
  auto  target   = std::make_shared<test_target>();
  auto  fallback = std::make_unique<no_op_policy>();
  auto* original = fallback.get();
  auto  result   = create_scheduler_dapp_policy(std::move(fallback), target, target->lane_count(), {0, 1, 52, 32});
  EXPECT_EQ(result.get(), original);

  EXPECT_FALSE(create_scheduler_dapp_hook_provider(target, {}, {}, std::chrono::nanoseconds::zero()));
  EXPECT_FALSE(create_scheduler_dapp_hook_provider(target, {}, {}, std::chrono::seconds(2)));
}

TEST(scheduler_dapp_policy, forwards_the_committed_ul_result_to_the_optional_plan_sink)
{
  auto target = std::make_shared<test_target>();
  auto sink   = std::make_shared<plan_sink_spy>();
  auto hooks  = create_scheduler_dapp_hook_provider(target, {}, {}, std::chrono::microseconds(100), {}, sink);
  ASSERT_NE(hooks, nullptr);
  const slot_point      slot{0, 41, 1};
  const ul_sched_result result{};
  hooks->publish_ul_plan(3, slot, result);
  EXPECT_EQ(sink->calls, 1U);
  EXPECT_EQ(sink->last_cell, 3U);
  EXPECT_EQ(sink->last_slot, slot);
  EXPECT_EQ(sink->last_result, &result);
}

TEST(scheduler_dapp_policy, enforces_host_quiet_reservation_without_invoking_the_dapp)
{
  const slot_point        decision_slot{0, 41, 3};
  const slot_point        transmission_slot = decision_slot + 4;
  std::array<uint16_t, 1> widths{52};
  auto                    reservations = create_quiet_reservation_manager(widths);
  ASSERT_NE(reservations, nullptr);

  quiet_reservation_request request{};
  request.intent.struct_size         = sizeof(request.intent);
  request.intent.abi_major           = OCUDU_DAPP_ABI_MAJOR_V1;
  request.intent.abi_minor           = OCUDU_DAPP_ABI_MINOR_V1;
  request.intent.request_id[0]       = 81;
  request.intent.earliest_start      = to_radio_time(transmission_slot);
  request.intent.latest_start        = to_radio_time(transmission_slot);
  request.intent.duration_slots      = 1;
  request.intent.repeat_count        = 1;
  request.intent.first_prb           = 11;
  request.intent.nof_prbs            = 6;
  request.intent.requested_level     = OCUDU_DAPP_QUIET_NO_NEW_PUSCH_V1;
  request.intent.budget_class        = 1;
  request.intent.confidence_q16      = 1U << 15U;
  request.publish_monotonic_ns       = monotonic_now_ns();
  request.intent.expiry_monotonic_ns = request.publish_monotonic_ns + 500'000'000;
  request.source_instance_id         = {91, 92};
  request.source_generation          = 3;
  request.source_sequence            = 4;
  request.observed_slot              = decision_slot;
  ASSERT_EQ(reservations->submit(request).result, quiet_reservation_result::accepted);

  auto target = std::make_shared<test_target>();
  target->enabled.store(false, std::memory_order_relaxed);
  auto policy = create_scheduler_dapp_policy(std::make_unique<no_op_policy>(),
                                             target,
                                             0,
                                             {0, 1, 52, 32},
                                             {},
                                             {},
                                             std::chrono::microseconds(100),
                                             reservations);
  ASSERT_NE(policy, nullptr);
  policy->compute_ue_ul_priorities(decision_slot, transmission_slot, {});
  const auto mask = policy->get_ul_newtx_avoid_prbs(transmission_slot);
  ASSERT_EQ(mask.size(), widths[0]);
  EXPECT_TRUE(std::all_of(mask.begin() + 11, mask.begin() + 17, [](uint8_t value) { return value == 1; }));
  EXPECT_EQ(target->ul_calls.load(std::memory_order_relaxed), 0U);
  EXPECT_TRUE(policy->get_ul_newtx_avoid_prbs(transmission_slot + 1).empty());
}

TEST(scheduler_dapp_policy, caller_owned_intent_storage_cannot_be_substituted)
{
  scheduler_fixture test;
  test.target->mode.store(test_target::behavior::replace_storage, std::memory_order_relaxed);
  test.push_dl_data();
  ASSERT_TRUE(test.run_until_dl_pdcch());
  EXPECT_GT(test.target->invalid_reports.load(std::memory_order_relaxed), 0U);
}

TEST(scheduler_dapp_policy, supplies_only_valid_future_interference_context_to_ul_policy)
{
  scheduler_fixture test;
  const unsigned    nof_prbs = test.simulator.cell_cfg().params.dl_cfg_common.init_dl_bwp.generic_params.crbs.length();
  std::vector<uint8_t> mask(nof_prbs);
  mask[0] = 213;
  publish_interference(test, mask);

  test.push_ul_data();
  ASSERT_TRUE(test.run_until_next_ul_call());
  EXPECT_EQ(test.target->interference_sequence.load(std::memory_order_relaxed), 77U);
  EXPECT_EQ(test.target->interference_first_prb.load(std::memory_order_relaxed), mask[0]);
  EXPECT_EQ(test.target->allowed_output_flags.load(std::memory_order_relaxed),
            OCUDU_DAPP_SCHEDULER_ALLOW_AVOID_PRB_MASK_V1);
}

TEST(scheduler_dapp_policy, applies_budgeted_interference_derived_mask_only_to_new_ul_transmissions)
{
  scheduler_fixture test;
  const unsigned    nof_prbs = test.simulator.cell_cfg().params.dl_cfg_common.init_dl_bwp.generic_params.crbs.length();
  std::vector<uint8_t> mask(nof_prbs);
  std::fill_n(mask.begin(), 8, uint8_t{255});
  publish_interference(test, mask);
  test.target->mode.store(test_target::behavior::avoid, std::memory_order_relaxed);
  test.push_ul_data(100000);

  const ul_sched_info* pusch = test.run_until_pusch();
  ASSERT_NE(pusch, nullptr);
  ASSERT_TRUE(pusch->pusch_cfg.rbs.is_type1());
  EXPECT_FALSE(pusch->pusch_cfg.rbs.type1().overlaps(vrb_interval{0, 8}));
}

TEST(scheduler_dapp_policy, late_ul_output_retains_conventional_allocation_and_discards_prb_avoidance)
{
  scheduler_fixture test(scheduler_dapp_policy_mode::authoritative, std::chrono::microseconds(100));
  const unsigned    nof_prbs = test.simulator.cell_cfg().params.dl_cfg_common.init_dl_bwp.generic_params.crbs.length();
  std::vector<uint8_t> mask(nof_prbs);
  std::fill_n(mask.begin(), 8, uint8_t{255});
  publish_interference(test, mask);
  test.target->mode.store(test_target::behavior::late_avoid, std::memory_order_relaxed);
  test.push_ul_data(100000);

  const ul_sched_info* pusch = test.run_until_pusch();
  ASSERT_NE(pusch, nullptr);
  ASSERT_TRUE(pusch->pusch_cfg.rbs.is_type1());
  // Conventional first-fit is free to choose any legal run; the invariant is
  // that the late mask was not accepted or committed.
  EXPECT_EQ(test.target->accepted.load(std::memory_order_relaxed), 0U);
  EXPECT_GT(test.target->fallbacks.load(std::memory_order_relaxed), 0U);
  EXPECT_GT(test.target->deadline_misses.load(std::memory_order_relaxed), 0U);
}

TEST(scheduler_dapp_policy, rejects_avoidance_beyond_host_budget_and_keeps_conventional_allocation)
{
  scheduler_fixture test;
  const unsigned    nof_prbs = test.simulator.cell_cfg().params.dl_cfg_common.init_dl_bwp.generic_params.crbs.length();
  std::vector<uint8_t> mask(nof_prbs, uint8_t{255});
  publish_interference(test, mask);
  test.target->mode.store(test_target::behavior::invalid_avoid, std::memory_order_relaxed);
  test.push_ul_data(100000);

  ASSERT_NE(test.run_until_pusch(), nullptr);
  EXPECT_GT(test.target->invalid_reports.load(std::memory_order_relaxed), 0U);
}

static scheduler_dapp_guardrails assignment_guardrails() noexcept
{
  scheduler_dapp_guardrails g{8};
  g.allow_mcs            = true;
  g.allow_prb_preference = true;
  g.allow_prb_assignment = true;
  return g;
}

TEST(scheduler_dapp_policy, overlapping_preference_under_assignment_keeps_mcs_and_does_not_reject)
{
  scheduler_fixture test(scheduler_dapp_policy_mode::authoritative, std::chrono::seconds(1), assignment_guardrails());
  test.add_second_ue();
  test.target->mode.store(test_target::behavior::prb_pref_overlap, std::memory_order_relaxed);
  test.push_ul_data(100000);
  test.push_ul_data_for(test.ue2_request, 100000);

  ASSERT_TRUE(test.simulator.run_slot_until(
      [&] { return test.target->last_candidates.load(std::memory_order_relaxed) >= 2U; }, 100));
  EXPECT_EQ(test.target->validation_rejections.load(std::memory_order_relaxed), 0U);
  EXPECT_EQ(test.target->invalid_reports.load(std::memory_order_relaxed), 0U);
  EXPECT_GE(test.target->accepted.load(std::memory_order_relaxed), 1U);

  const ul_sched_info* first_assign = nullptr;
  const uint32_t       first_ue     = test.target->last_first_ue.load(std::memory_order_relaxed);
  const rnti_t         first_rnti =
      first_ue == static_cast<uint32_t>(test.ue2_request.ue_index) ? test.ue2_request.crnti : test.ue_request.crnti;
  test.simulator.run_slot_until(
      [&] {
        for (const auto& pusch : test.simulator.last_sched_result()->ul.puschs) {
          if (pusch.pusch_cfg.rnti == first_rnti) {
            first_assign = &pusch;
            return true;
          }
        }
        return false;
      },
      100);
  ASSERT_NE(first_assign, nullptr);
  ASSERT_TRUE(first_assign->pusch_cfg.rbs.is_type1());
  // This test spans policy and transmission slots. A later invocation may
  // reserve the preferred run before the observed grant is transmitted, so
  // the conventional allocator can legally place the soft-overlap candidate
  // elsewhere. Exact hard-assignment placement is covered by the next test.
  EXPECT_EQ(first_assign->pusch_cfg.mcs_index.value(), test.target->last_emitted_mcs.load(std::memory_order_relaxed));
}

TEST(scheduler_dapp_policy, legal_preferences_under_assignment_are_hard_assigns)
{
  scheduler_fixture test(scheduler_dapp_policy_mode::authoritative, std::chrono::seconds(1), assignment_guardrails());
  test.simulator.auto_crc = true;
  test.add_second_ue();
  test.target->mode.store(test_target::behavior::prb_pref_legal, std::memory_order_relaxed);
  test.push_ul_data(100000);
  test.push_ul_data_for(test.ue2_request, 100000);

  std::optional<vrb_interval> ue0_vrbs;
  std::optional<vrb_interval> ue1_vrbs;
  test.simulator.run_slot_until(
      [&] {
        const sched_result* res = test.simulator.last_sched_result();
        if (res == nullptr) {
          return false;
        }
        for (const auto& pusch : res->ul.puschs) {
          if (!pusch.pusch_cfg.rbs.is_type1()) {
            continue;
          }
          const vrb_interval vrbs = pusch.pusch_cfg.rbs.type1();
          // Ignore grants already in flight from a one-candidate policy call;
          // capture the first grant that proves each legal hard assignment was
          // committed after both UEs entered the candidate set.
          if (pusch.pusch_cfg.rnti == test.ue_request.crnti && vrbs.start() == 0 && vrbs.length() == 8) {
            ue0_vrbs = vrbs;
          } else if (pusch.pusch_cfg.rnti == test.ue2_request.crnti && vrbs.start() == 8 && vrbs.length() == 8) {
            ue1_vrbs = vrbs;
          }
        }
        return ue0_vrbs.has_value() && ue1_vrbs.has_value();
      },
      100);
  ASSERT_TRUE(ue0_vrbs.has_value());
  ASSERT_TRUE(ue1_vrbs.has_value());
  EXPECT_EQ(ue0_vrbs->start(), 0U);
  EXPECT_EQ(ue0_vrbs->length(), 8U);
  EXPECT_EQ(ue1_vrbs->start(), 8U);
  EXPECT_EQ(ue1_vrbs->length(), 8U);
  EXPECT_EQ(test.target->validation_rejections.load(std::memory_order_relaxed), 0U);
}

TEST(scheduler_dapp_policy, off_cell_preference_is_still_a_validation_rejection)
{
  scheduler_fixture test(scheduler_dapp_policy_mode::authoritative, std::chrono::seconds(1), assignment_guardrails());
  test.target->mode.store(test_target::behavior::prb_pref_malformed, std::memory_order_relaxed);
  test.push_ul_data(100000);

  ASSERT_NE(test.run_until_pusch(), nullptr);
  EXPECT_GT(test.target->validation_rejections.load(std::memory_order_relaxed), 0U);
  EXPECT_GT(test.target->invalid_reports.load(std::memory_order_relaxed), 0U);
}
