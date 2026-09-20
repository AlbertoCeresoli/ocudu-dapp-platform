// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear
#include "../../../support/resource_grid_test_doubles.h"
#include "../../upper_phy_rx_results_notifier_test_doubles.h"
#include "logging_pusch_processor_decorator.h"
#include "phy/metrics/phy_metrics_pusch_processor_decorator.h"
#include "phy/upper/uplink_processor_impl.h"
#include "pusch_processor_notifier_adaptor.h"
#include "pusch_processor_pool.h"
#include <functional>
#include <gtest/gtest.h>
#include <thread>
#include <type_traits>

using namespace ocudu;
namespace {
class sink final : public pusch_processor_result_notifier
{
public:
  unsigned results{}, uci{};
  void     on_sch(const pusch_processor_result_data&) override { ++results; }
  void     on_uci(const pusch_processor_result_control&) override { ++uci; }
};
class metrics final : public pusch_processor_metric_notifier
{
public:
  void     on_new_metric(const pusch_processor_metrics&) override { ++count; }
  unsigned count{};
};
class diagnostics final : public phy_diagnostic_sink
{
public:
  std::function<void()> tail;
  unsigned              count{};
  bool                  should_collect(phy_diagnostic_stream, uint16_t) override { return true; }
  bool                  is_collect_due(phy_diagnostic_stream, uint16_t) override { return true; }
  void                  on_pusch_crc(const pusch_crc_diagnostic&) override
  {
    ++count;
    if (tail)
      tail();
  }
  void on_pusch_symbols(const pusch_symbols_diagnostic&) override {}
  void on_srs_channel(const srs_channel_diagnostic&) override {}
  void on_pusch_dmrs_channel(const pusch_dmrs_channel_diagnostic&) override {}
};
class tail_processor final : public pusch_processor, public pusch_processor_csi_part1_feedback
{
public:
  explicit tail_processor(diagnostics& diag_) : diag(diag_) {}
  bool                             synchronous{};
  std::function<void()>            process_tail;
  pusch_processor_result_notifier* target{};
  void                             process(span<uint8_t>,
                                           unique_rx_buffer,
                                           pusch_processor_result_notifier& notifier,
                                           const resource_grid_reader&,
                                           const pdu_t&) override
  {
    target = &notifier;
    adaptor.new_transmission(notifier, *this, {});
    adaptor.set_crc_diag(&diag, slot_point(0, 1), 0x4607);
    if (synchronous)
      emit();
    if (process_tail)
      process_tail();
  }
  void emit() { adaptor.get_sch_data_notifier().on_sch_data({}); }
  void on_csi_part1(const uci_payload_type&) override {}

private:
  diagnostics&                     diag;
  pusch_processor_notifier_adaptor adaptor;
};
struct harness {
  diagnostics                                    diag;
  metrics                                        metric;
  sink                                           result;
  resource_grid_reader_spy                       grid;
  tail_processor*                                model{};
  std::unique_ptr<pusch_processor_wrapper::pool> pool;
  explicit harness(unsigned decorators = 0)
  {
    auto algorithm                         = std::make_unique<tail_processor>(diag);
    model                                  = algorithm.get();
    std::unique_ptr<pusch_processor> chain = std::move(algorithm);
    if (decorators & 1) {
      auto& logger = ocudulog::fetch_basic_logger("ACTIVITY_TEST");
      logger.set_level(ocudulog::basic_levels::none);
      chain = std::make_unique<logging_pusch_processor_decorator>(logger, std::move(chain));
    }
    if (decorators & 2) {
      chain = std::make_unique<phy_metrics_pusch_processor_decorator>(std::move(chain), metric);
    }
    std::array<std::unique_ptr<pusch_processor_wrapper>, 1> wrappers;
    wrappers[0] = std::make_unique<pusch_processor_wrapper>(std::move(chain));
    pool = std::make_unique<pusch_processor_wrapper::pool>(span<std::unique_ptr<pusch_processor_wrapper>>(wrappers));
  }
  bool available() { return bool(pool->get()); }
  void start(pusch_processor_result_notifier* target = nullptr)
  {
    auto lease = pool->get();
    ASSERT_TRUE(lease);
    auto& wrapper = *lease;
    wrapper.set_unique_ptr(std::move(lease));
    pusch_processor::pdu_t pdu{};
    pdu.slot = slot_point(0, 1);
    pdu.codeword.emplace();
    wrapper.process({}, unique_rx_buffer{}, target ? *target : result, grid, pdu);
  }
};
} // namespace

TEST(pusch_processing_activity, token_is_small_move_only_and_releases_exactly_once)
{
  static_assert(sizeof(pusch_processing_activity) == 2 * sizeof(void*));
  static_assert(!std::is_copy_constructible_v<pusch_processing_activity>);
  static_assert(std::is_nothrow_move_constructible_v<pusch_processing_activity>);
  unsigned                  count   = 0;
  auto                      release = [](void* value) noexcept { ++*static_cast<unsigned*>(value); };
  pusch_processing_activity first(&count, release);
  auto                      second = std::move(first);
  EXPECT_FALSE(first);
  EXPECT_TRUE(second);
  first = std::move(second);
  first.reset();
  first.reset();
  EXPECT_EQ(count, 1U);
  EXPECT_FALSE(first);
}

TEST(pusch_processing_activity, synchronous_result_does_not_recycle_during_process_tail)
{
  harness h;
  h.model->synchronous  = true;
  h.model->process_tail = [&] {
    EXPECT_EQ(h.result.results, 1U);
    EXPECT_FALSE(h.available());
  };
  h.diag.tail = [&] {
    EXPECT_EQ(h.result.results, 1U);
    EXPECT_FALSE(h.available());
  };
  h.start();
  EXPECT_EQ(h.diag.count, 1U);
  EXPECT_TRUE(h.available());
}

TEST(pusch_processing_activity, asynchronous_actual_notifier_retains_diagnostic_tail)
{
  harness h;
  h.diag.tail = [&] {
    EXPECT_EQ(h.result.results, 1U);
    EXPECT_FALSE(h.available());
  };
  h.start();
  EXPECT_FALSE(h.available());
  h.model->emit();
  EXPECT_EQ(h.result.results, 1U);
  EXPECT_EQ(h.diag.count, 1U);
  EXPECT_TRUE(h.available());
}

TEST(pusch_processing_activity, deferred_consumer_retains_real_pool_lease_after_result)
{
  harness h;
  h.start();
  auto consumer = h.model->target->acquire_processing_activity();
  ASSERT_TRUE(consumer);
  h.model->emit();
  EXPECT_EQ(h.result.results, 1U);
  EXPECT_FALSE(h.available());
  consumer.reset();
  EXPECT_TRUE(h.available());
}

TEST(pusch_processing_activity, logging_and_metrics_forward_original_activity)
{
  for (unsigned decorators : {1U, 2U, 3U}) {
    harness h(decorators);
    h.diag.tail = [&] { EXPECT_FALSE(h.available()); };
    h.start();
    auto consumer = h.model->target->acquire_processing_activity();
    ASSERT_TRUE(consumer);
    h.model->emit();
    EXPECT_EQ(h.result.results, 1U);
    EXPECT_EQ(h.diag.count, 1U);
    EXPECT_EQ(h.metric.count, (decorators & 2) ? 1U : 0U);
    EXPECT_FALSE(h.available());
    consumer.reset();
    EXPECT_TRUE(h.available());
  }
}

TEST(pusch_processing_activity, repeated_sync_and_async_reuse_balances_activity)
{
  harness h;
  h.diag.tail = [&] { EXPECT_FALSE(h.available()); };
  for (unsigned i = 0; i != 1000; ++i) {
    h.model->synchronous = i % 2;
    h.start();
    if (!h.model->synchronous)
      h.model->emit();
    EXPECT_TRUE(h.available());
  }
  EXPECT_EQ(h.result.results, 1000U);
  EXPECT_EQ(h.diag.count, 1000U);
}

TEST(pusch_processing_activity, cross_thread_release_does_not_recycle_before_last_consumer)
{
  harness h;
  h.start();
  auto first  = h.model->target->acquire_processing_activity();
  auto second = h.model->target->acquire_processing_activity();
  h.model->emit();
  std::thread consumer([activity = std::move(first)]() mutable { activity.reset(); });
  consumer.join();
  EXPECT_FALSE(h.available());
  second.reset();
  EXPECT_TRUE(h.available());
}

TEST(pusch_processing_activity, nonpooled_notifiers_keep_empty_default_contract)
{
  sink unpooled;
  EXPECT_FALSE(unpooled.acquire_processing_activity());
  pusch_processing_activity empty;
  auto                      moved = std::move(empty);
  EXPECT_FALSE(empty);
  EXPECT_FALSE(moved);
}

namespace {
class upper_sink : public upper_phy_rx_results_notifier_spy
{
public:
  unsigned                 results{}, controls{};
  ul_pusch_results_data    data{};
  ul_pusch_results_control control{};
  std::function<void()>    data_tail;
  void                     on_new_pusch_results_data(const ul_pusch_results_data& result) override
  {
    ++results;
    data = result;
    if (data_tail) {
      data_tail();
    }
  }
  void on_new_pusch_results_control(const ul_pusch_results_control& result) override
  {
    ++controls;
    control = result;
  }
};

void begin_slot(uplink_processor_fsm& fsm, slot_point slot, unsigned pdus = 1)
{
  ASSERT_TRUE(fsm.start_new_slot(slot));
  for (unsigned i = 0; i != pdus; ++i) {
    fsm.increment_pending_pdu_count();
  }
  fsm.stop_accepting_pdu();
  for (unsigned i = 0; i != pdus; ++i) {
    ASSERT_TRUE(fsm.on_create_pdu_task());
  }
}

void expect_slot_busy(uplink_processor_fsm& fsm, slot_point slot)
{
  EXPECT_TRUE(fsm.has_receive_request(slot));
  EXPECT_FALSE(fsm.start_new_slot(slot + 1));
}

void expect_slot_reusable(uplink_processor_fsm& fsm, slot_point slot)
{
  EXPECT_FALSE(fsm.has_receive_request(slot));
  ASSERT_TRUE(fsm.start_new_slot(slot + 1));
  fsm.stop_accepting_pdu();
}
} // namespace

TEST(pusch_slot_activity, task_loan_retains_slot_without_delaying_data_or_changing_metrics)
{
  uplink_processor_fsm                            fsm;
  detail::pusch_processor_result_notifier_adaptor adaptor;
  upper_sink                                      upper;
  const slot_point                                slot(0, 1);
  const std::array<uint8_t, 3>                    payload{1, 2, 3};
  unsigned                                        completions = 0;
  begin_slot(fsm, slot);
  auto& notifier  = adaptor.configure(upper, to_rnti(0x4607), slot, to_harq_id(3), payload, 7, [&] {
    ++completions;
    fsm.on_finish_processing_pdu();
  });
  auto  task      = notifier.acquire_processing_activity();
  upper.data_tail = [&] {
    EXPECT_EQ(completions, 0U);
    expect_slot_busy(fsm, slot);
  };
  pusch_processor_result_control uci{};
  uci.harq_ack = {uci_payload_type(2), uci_status::valid};
  uci.csi.set_sinr_dB(channel_state_information::sinr_type::channel_estimator, 12.5F);
  notifier.on_uci(uci);
  EXPECT_EQ(upper.controls, 1U);
  ASSERT_TRUE(upper.control.harq_ack.has_value());
  EXPECT_EQ(upper.control.harq_ack->payload.size(), 2U);
  EXPECT_EQ(upper.control.harq_ack->status, uci_status::valid);
  EXPECT_EQ(upper.control.csi.get_sinr_dB(), uci.csi.get_sinr_dB());
  expect_slot_busy(fsm, slot);

  pusch_processor_result_data result{};
  result.data.tb_crc_ok = true;
  result.csi            = uci.csi;
  notifier.on_sch(result);
  EXPECT_EQ(upper.results, 1U);
  EXPECT_EQ(upper.data.rnti, to_rnti(0x4607));
  EXPECT_EQ(upper.data.slot, slot);
  EXPECT_EQ(upper.data.harq_id, to_harq_id(3));
  EXPECT_EQ(upper.data.n_rapid, 7U);
  EXPECT_EQ(upper.data.csi.get_sinr_dB(), result.csi.get_sinr_dB());
  EXPECT_EQ(upper.data.payload.data(), payload.data());
  EXPECT_EQ(upper.data.payload.size(), payload.size());
  EXPECT_EQ(completions, 0U);
  expect_slot_busy(fsm, slot);
  task.reset();
  EXPECT_EQ(completions, 1U);
  expect_slot_reusable(fsm, slot);
}

TEST(pusch_slot_activity, terminal_notification_tail_retains_slot_without_explicit_task_loan)
{
  uplink_processor_fsm                            fsm;
  detail::pusch_processor_result_notifier_adaptor adaptor;
  upper_sink                                      upper;
  const slot_point                                slot(0, 1);
  unsigned                                        completions = 0;
  begin_slot(fsm, slot);
  auto& notifier  = adaptor.configure(upper, to_rnti(0x4607), slot, to_harq_id(0), {}, {}, [&] {
    ++completions;
    fsm.on_finish_processing_pdu();
  });
  upper.data_tail = [&] {
    EXPECT_EQ(completions, 0U);
    expect_slot_busy(fsm, slot);
  };
  notifier.on_sch({});
  EXPECT_EQ(upper.results, 1U);
  EXPECT_EQ(completions, 1U);
  EXPECT_TRUE(upper.data.payload.empty());
  expect_slot_reusable(fsm, slot);
}

TEST(pusch_slot_activity, pooled_process_and_actual_diagnostic_tails_retain_original_slot)
{
  for (unsigned decorators : {0U, 1U, 2U, 3U}) {
    for (bool synchronous : {false, true}) {
      SCOPED_TRACE(::testing::Message() << decorators << "/" << synchronous);
      uplink_processor_fsm                            fsm;
      detail::pusch_processor_result_notifier_adaptor adaptor;
      upper_sink                                      upper;
      harness                                         h(decorators);
      const slot_point                                slot(0, 1);
      unsigned                                        completions = 0;
      begin_slot(fsm, slot);
      auto& notifier        = adaptor.configure(upper, to_rnti(0x4607), slot, to_harq_id(0), {}, {}, [&] {
        EXPECT_TRUE(h.available());
        ++completions;
        fsm.on_finish_processing_pdu();
      });
      h.model->synchronous  = synchronous;
      h.model->process_tail = [&] { expect_slot_busy(fsm, slot); };
      h.diag.tail           = [&] {
        // Existing metrics join SCH with process return; synchronous diagnostics precede that join.
        EXPECT_EQ(upper.results, synchronous && (decorators & 2) ? 0U : 1U);
        EXPECT_EQ(completions, 0U);
        expect_slot_busy(fsm, slot);
      };
      // No outer task loan: checks the wrapper's original-owner propagation through actual notifier tails.
      h.start(&notifier);
      if (!synchronous) {
        expect_slot_busy(fsm, slot);
        h.model->emit();
      }
      EXPECT_EQ(upper.results, 1U);
      EXPECT_EQ(h.diag.count, 1U);
      EXPECT_EQ(completions, 1U);
      expect_slot_reusable(fsm, slot);
    }
  }
}

TEST(pusch_slot_activity, last_cross_thread_consumer_returns_inner_pool_before_outer_slot)
{
  uplink_processor_fsm                            fsm;
  detail::pusch_processor_result_notifier_adaptor adaptor;
  upper_sink                                      upper;
  harness                                         h(3);
  const slot_point                                slot(0, 1);
  unsigned                                        completions = 0;
  begin_slot(fsm, slot);
  auto& notifier = adaptor.configure(upper, to_rnti(0x4607), slot, to_harq_id(0), {}, {}, [&] {
    EXPECT_TRUE(h.available());
    ++completions;
    fsm.on_finish_processing_pdu();
  });
  auto  task     = notifier.acquire_processing_activity();
  h.start(&notifier);
  auto consumer = h.model->target->acquire_processing_activity();
  h.model->emit();
  task.reset();
  EXPECT_EQ(upper.results, 1U);
  EXPECT_EQ(completions, 0U);
  EXPECT_FALSE(h.available());
  expect_slot_busy(fsm, slot);
  std::thread worker([activity = std::move(consumer)]() mutable { activity.reset(); });
  worker.join();
  EXPECT_EQ(completions, 1U);
  EXPECT_TRUE(h.available());
  expect_slot_reusable(fsm, slot);
}

TEST(pusch_slot_activity, completion_can_reconfigure_same_adaptor_immediately)
{
  uplink_processor_fsm                            fsm;
  detail::pusch_processor_result_notifier_adaptor adaptor;
  upper_sink                                      upper;
  const slot_point                                slot(0, 1);
  unsigned                                        completions = 0;
  begin_slot(fsm, slot);
  auto& notifier = adaptor.configure(upper, to_rnti(0x4607), slot, to_harq_id(0), {}, {}, [&] {
    ++completions;
    fsm.on_finish_processing_pdu();
    begin_slot(fsm, slot + 1);
    auto& next = adaptor.configure(upper, to_rnti(0x4601), slot + 1, to_harq_id(1), {}, {}, [&] {
      ++completions;
      fsm.on_finish_processing_pdu();
    });
    next.on_sch({});
  });
  notifier.on_sch({});
  EXPECT_EQ(completions, 2U);
  EXPECT_EQ(upper.results, 2U);
  EXPECT_EQ(upper.data.rnti, to_rnti(0x4601));
  EXPECT_EQ(upper.data.harq_id, to_harq_id(1));
  EXPECT_EQ(upper.data.slot, slot + 1);
  expect_slot_reusable(fsm, slot + 1);
}

TEST(pusch_slot_activity, repeated_slot_reuse_waits_for_both_task_and_deferred_consumer)
{
  uplink_processor_fsm                            fsm;
  detail::pusch_processor_result_notifier_adaptor adaptor;
  upper_sink                                      upper;
  harness                                         h(3);
  unsigned                                        completions = 0;
  for (unsigned i = 0; i != 1000; ++i) {
    const slot_point slot(0, i);
    begin_slot(fsm, slot);
    auto& notifier = adaptor.configure(upper, to_rnti(0x4607), slot, to_harq_id(0), {}, {}, [&] {
      EXPECT_TRUE(h.available());
      ++completions;
      fsm.on_finish_processing_pdu();
    });
    auto  task     = notifier.acquire_processing_activity();
    h.start(&notifier);
    auto consumer = h.model->target->acquire_processing_activity();
    h.model->emit();
    EXPECT_EQ(upper.results, i + 1);
    if (i % 2) {
      task.reset();
    } else {
      consumer.reset();
    }
    EXPECT_EQ(completions, i);
    expect_slot_busy(fsm, slot);
    consumer.reset();
    task.reset();
    EXPECT_EQ(completions, i + 1);
    expect_slot_reusable(fsm, slot);
  }
}

TEST(pusch_slot_activity, multiple_pdus_prevent_reuse_until_all_activities_finish)
{
  uplink_processor_fsm                                           fsm;
  std::array<detail::pusch_processor_result_notifier_adaptor, 2> adaptors;
  upper_sink                                                     upper;
  const slot_point                                               slot(0, 1);
  std::array<pusch_processing_activity, 2>                       tasks;
  unsigned                                                       completions = 0;
  begin_slot(fsm, slot, 2);
  for (unsigned i = 0; i != 2; ++i) {
    auto& notifier = adaptors[i].configure(upper, to_rnti(0x4607), slot, to_harq_id(i), {}, {}, [&] {
      ++completions;
      fsm.on_finish_processing_pdu();
    });
    tasks[i]       = notifier.acquire_processing_activity();
    notifier.on_sch({});
    expect_slot_busy(fsm, slot);
  }
  EXPECT_EQ(upper.results, 2U);
  tasks[1].reset();
  EXPECT_EQ(completions, 1U);
  expect_slot_busy(fsm, slot);
  tasks[0].reset();
  EXPECT_EQ(completions, 2U);
  expect_slot_reusable(fsm, slot);
}
