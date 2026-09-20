// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear
#include "../../../support/resource_grid_test_doubles.h"
#include "lib/phy/metrics/phy_metrics_pusch_demodulator_decorator.h"
#include "lib/phy/upper/channel_processors/pusch/pusch_demodulator_layer_select.h"
#include "pusch_test_pooled_processor.h"
#include <array>
#include <gtest/gtest.h>

using namespace ocudu;
namespace {
class metric_sink final : public pusch_demodulator_metric_notifier
{
public:
  unsigned calls{};
  void     on_new_metric(const pusch_demodulator_metrics&) override { ++calls; }
};

class demodulator_spy final : public pusch_demodulator
{
public:
  unsigned                  finish_calls{}, completion_calls{}, capture_waits{}, enables{}, disables{};
  bool                      device{}, owned{}, discarded{};
  pusch_processing_activity loan;
  std::array<cf_t, 2>       symbols{{{1.0F, -1.0F}, {-1.0F, 1.0F}}};
  void                      demodulate(pusch_codeword_buffer&,
                                       pusch_demodulator_notifier&,
                                       const resource_grid_reader&,
                                       const dmrs_pusch_estimator_results&,
                                       const configuration&) override
  {
  }
  void finish_resident_consumers() override
  {
    ++finish_calls;
    loan.reset();
  }
  bool             used_device_grid() const override { return device; }
  bool             used_owned_device_grid() const override { return owned; }
  bool             discarded_inline_input() const override { return discarded; }
  void             report_deferred_dapp_completion() override { ++completion_calls; }
  float            get_last_grid_staging_us() const override { return 17.0F; }
  float            get_last_demod_sync_us() const override { return 23.0F; }
  void             enable_symbol_capture() override { ++enables; }
  void             disable_symbol_capture() override { ++disables; }
  void             wait_captured_symbols() override { ++capture_waits; }
  span<const cf_t> get_captured_symbols() const override { return symbols; }
};

struct chain_fixture {
  metric_sink                        metrics;
  demodulator_spy*                   gpu{};
  demodulator_spy*                   software{};
  std::unique_ptr<pusch_demodulator> chain;
  chain_fixture(bool layered, bool measured)
  {
    auto inner = std::make_unique<demodulator_spy>();
    gpu        = inner.get();
    chain      = std::move(inner);
    if (layered) {
      auto sw  = std::make_unique<demodulator_spy>();
      software = sw.get();
      chain    = std::make_unique<pusch_demodulator_layer_select>(std::move(chain), std::move(sw));
    }
    if (measured) {
      chain = std::make_unique<phy_metrics_pusch_demodulator_decorator>(std::move(chain), metrics);
    }
  }
};

class result_sink final : public pusch_processor_result_notifier
{
public:
  unsigned results{};
  void     on_uci(const pusch_processor_result_control&) override {}
  void     on_sch(const pusch_processor_result_data&) override { ++results; }
};

class loan_processor final : public pusch_processor
{
public:
  explicit loan_processor(demodulator_spy& receiver_) : receiver(receiver_) {}
  void process(span<uint8_t>,
               unique_rx_buffer,
               pusch_processor_result_notifier& notifier,
               const resource_grid_reader&,
               const pdu_t&) override
  {
    receiver.loan = notifier.acquire_processing_activity();
    EXPECT_TRUE(receiver.loan);
    notifier.on_sch({});
  }

private:
  demodulator_spy& receiver;
};
} // namespace

TEST(pusch_demodulator_forwarding, layer_selector_finishes_accelerated_consumer_exactly_once)
{
  chain_fixture f(true, false);
  f.chain->finish_resident_consumers();
  EXPECT_EQ(f.gpu->finish_calls, 1U);
  EXPECT_EQ(f.software->finish_calls, 0U);
}

TEST(pusch_demodulator_forwarding, metrics_decorator_finishes_consumer_without_publishing_metrics)
{
  chain_fixture f(false, true);
  f.chain->finish_resident_consumers();
  EXPECT_EQ(f.gpu->finish_calls, 1U);
  EXPECT_EQ(f.metrics.calls, 0U);
}

TEST(pusch_demodulator_forwarding, nested_wrappers_preserve_grid_origin_and_discard_flags)
{
  chain_fixture f(true, true);
  for (unsigned mask = 0; mask != 8; ++mask) {
    f.gpu->device    = (mask & 1) != 0;
    f.gpu->owned     = (mask & 2) != 0;
    f.gpu->discarded = (mask & 4) != 0;
    EXPECT_EQ(f.chain->used_device_grid(), f.gpu->device);
    EXPECT_EQ(f.chain->used_owned_device_grid(), f.gpu->owned);
    EXPECT_EQ(f.chain->discarded_inline_input(), f.gpu->discarded);
  }
}

TEST(pusch_demodulator_forwarding, completion_notification_does_not_release_consumer_loan)
{
  chain_fixture f(true, true);
  unsigned      releases{};
  f.gpu->loan = {&releases, [](void* owner) noexcept { ++*static_cast<unsigned*>(owner); }};
  f.chain->report_deferred_dapp_completion();
  EXPECT_EQ(f.gpu->completion_calls, 1U);
  EXPECT_EQ(releases, 0U);
  EXPECT_TRUE(f.gpu->loan);
  f.chain->finish_resident_consumers();
  EXPECT_EQ(releases, 1U);
  EXPECT_FALSE(f.gpu->loan);
  // CPU fake cleanup keeps a forwarding regression an ordinary test failure.
  f.gpu->loan.reset();
}

TEST(pusch_demodulator_forwarding, nested_wrappers_preserve_timing_and_optional_constellation)
{
  chain_fixture f(true, true);
  EXPECT_FLOAT_EQ(f.chain->get_last_grid_staging_us(), 17.0F);
  EXPECT_FLOAT_EQ(f.chain->get_last_demod_sync_us(), 23.0F);
  f.chain->enable_symbol_capture();
  f.chain->wait_captured_symbols();
  EXPECT_EQ(f.chain->get_captured_symbols().data(), f.gpu->symbols.data());
  EXPECT_EQ(f.chain->get_captured_symbols().size(), 2U);
  f.chain->disable_symbol_capture();
  EXPECT_EQ(f.gpu->enables, 1U);
  EXPECT_EQ(f.gpu->disables, 1U);
  EXPECT_EQ(f.gpu->capture_waits, 1U);
  EXPECT_EQ(f.metrics.calls, 0U);
}

TEST(pusch_demodulator_forwarding, nested_finish_returns_real_processor_pool_after_sch)
{
  chain_fixture               f(true, true);
  resource_grid_reader_spy    grid;
  result_sink                 sink;
  pusch_test_pooled_processor owner(std::make_unique<loan_processor>(*f.gpu));
  for (unsigned grant = 0; grant != 1000; ++grant) {
    owner.process({}, unique_rx_buffer{}, sink, grid, {});
    EXPECT_EQ(sink.results, grant + 1);
    EXPECT_THROW(owner.require_idle(), std::runtime_error);
    f.chain->finish_resident_consumers();
    EXPECT_NO_THROW(owner.require_idle());
    // The fake owns no CUDA work. Never apply this test cleanup to real uncertain tensors.
    f.gpu->loan.reset();
  }
  EXPECT_EQ(f.gpu->finish_calls, 1000U);
  EXPECT_EQ(f.software->finish_calls, 0U);
}
