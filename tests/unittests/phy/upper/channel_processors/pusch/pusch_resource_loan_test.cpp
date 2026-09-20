// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear
#include "../../signal_processors/pusch/dmrs_pusch_estimator_test_doubles.h"
#include "../../upper_phy_rx_results_notifier_test_doubles.h"
#include "../uci/uci_decoder_test_doubles.h"
#include "phy/upper/uplink_processor_impl.h"
#include "pusch_demodulator_test_doubles.h"
#include "pusch_pooled_resources.h"
#include "pusch_processor_impl.h"
#include "ulsch_demultiplex_test_doubles.h"
#include <gtest/gtest.h>
#include <thread>

using namespace ocudu;
namespace {
using pool_type = pusch_processor_impl::concurrent_dependencies_pool_type;

struct fixture {
  uplink_processor_fsm                            fsm;
  detail::pusch_processor_result_notifier_adaptor adaptor;
  upper_phy_rx_results_notifier_spy               upper;
  std::shared_ptr<pool_type>                      pool;
  unsigned                                        completions{};
  bool                                            check_pool = true;
  const slot_point                                slot{0, 1};

  fixture()
  {
    pusch_processor::channel_size dimensions{};
    dimensions.nof_prb       = 4;
    dimensions.nof_symbols   = 14;
    dimensions.nof_rx_ports  = 1;
    dimensions.nof_tx_layers = 1;
    std::array<std::unique_ptr<pusch_processor_impl::concurrent_dependencies>, 1> objects;
    objects[0] =
        std::make_unique<pusch_processor_impl::concurrent_dependencies>(std::make_unique<dmrs_pusch_estimator_spy>(),
                                                                        std::make_unique<pusch_demodulator_spy>(),
                                                                        std::make_unique<ulsch_demultiplex_spy>(),
                                                                        std::make_unique<uci_decoder_spy>(),
                                                                        dimensions);
    pool = std::make_shared<pool_type>(span<std::unique_ptr<pusch_processor_impl::concurrent_dependencies>>(objects));
  }
  pusch_processor_result_notifier& start()
  {
    EXPECT_TRUE(fsm.start_new_slot(slot));
    fsm.increment_pending_pdu_count();
    fsm.stop_accepting_pdu();
    EXPECT_TRUE(fsm.on_create_pdu_task());
    return adaptor.configure(upper, to_rnti(0x4607), slot, to_harq_id(0), {}, {}, [this] {
      // Last resource loan must return the actual dependency entry before releasing its outer slot.
      if (check_pool) {
        EXPECT_TRUE(pool->get());
      }
      ++completions;
      fsm.on_finish_processing_pdu();
    });
  }
  void expect_busy()
  {
    EXPECT_TRUE(fsm.has_receive_request(slot));
    EXPECT_FALSE(pool->get());
  }
};

class unowned_notifier final : public pusch_processor_result_notifier
{
public:
  void on_sch(const pusch_processor_result_data&) override {}
  void on_uci(const pusch_processor_result_control&) override {}
};
} // namespace

TEST(pusch_resource_loan, fixed_inline_move_only_owner_releases_exactly_once)
{
  static_assert(!std::is_copy_constructible_v<pusch_resource_loan>);
  static_assert(std::is_nothrow_move_constructible_v<pusch_resource_loan>);
  static_assert(sizeof(pusch_resource_loan) <= 80);
  unsigned            releases = 0;
  auto                original = std::shared_ptr<unsigned>(&releases, [](unsigned* count) { ++*count; });
  pusch_resource_loan first{std::move(original)};
  EXPECT_FALSE(original);
  auto second = std::move(first);
  EXPECT_FALSE(first);
  EXPECT_TRUE(second);
  first = std::move(second);
  EXPECT_FALSE(second);
  first.reset();
  first.reset();
  EXPECT_EQ(releases, 1U);
  pusch_resource_loan empty{std::shared_ptr<void>{}};
  EXPECT_FALSE(empty);
}

TEST(pusch_resource_loan, actual_dependency_entry_survives_provider_and_stack_lease)
{
  fixture             f;
  auto&               notifier = f.start();
  pusch_resource_loan retained;
  auto                entry = f.pool->get();
  ASSERT_TRUE(entry);
  auto* demodulator = &entry->get_demodulator();
  {
    pooled_pusch_resource_provider<pool_type> provider(f.pool, entry, notifier);
    retained = provider.acquire();
    ASSERT_TRUE(retained);
    EXPECT_FALSE(entry.unique());
  }
  entry.reset();
  notifier.on_sch({});
  EXPECT_TRUE(f.upper.has_pusch_data_result_been_notified());
  EXPECT_EQ(f.completions, 0U);
  f.expect_busy();
  // This is the actual dependency-owned demodulator, still alive after the original lease returned.
  EXPECT_FALSE(demodulator->supports_resident_mode());
  retained.reset();
  EXPECT_EQ(f.completions, 1U);
  auto next = f.pool->get();
  ASSERT_TRUE(next);
  EXPECT_EQ(&next->get_demodulator(), demodulator);
}

TEST(pusch_resource_loan, two_consumers_release_dependency_before_slot_on_another_thread)
{
  fixture                                   f;
  auto&                                     notifier = f.start();
  auto                                      entry    = f.pool->get();
  pooled_pusch_resource_provider<pool_type> provider(f.pool, entry, notifier);
  auto                                      first  = provider.acquire();
  auto                                      second = provider.acquire();
  ASSERT_TRUE(first && second);
  entry.reset();
  notifier.on_sch({});
  first.reset();
  f.expect_busy();
  EXPECT_EQ(f.completions, 0U);
  std::thread consumer([last = std::move(second)]() mutable { last.reset(); });
  consumer.join();
  EXPECT_EQ(f.completions, 1U);
  EXPECT_FALSE(f.fsm.has_receive_request(f.slot));
  EXPECT_TRUE(f.pool->get());
}

TEST(pusch_resource_loan, rejects_missing_slot_or_dependency_ownership_without_cloning)
{
  fixture          f;
  unowned_notifier unowned;
  auto             entry = f.pool->get();
  ASSERT_TRUE(entry && entry.unique());
  pooled_pusch_resource_provider<pool_type> provider(f.pool, entry, unowned);
  EXPECT_FALSE(provider.acquire());
  EXPECT_TRUE(entry.unique());
  fixture other;
  auto& owned_notifier = other.start();
  pooled_pusch_resource_provider<pool_type> mismatched(other.pool, entry, owned_notifier);
  EXPECT_FALSE(mismatched.acquire());
  EXPECT_TRUE(entry.unique());
  owned_notifier.on_sch({});
  EXPECT_EQ(other.completions, 1U);
  entry.reset();
  EXPECT_FALSE(provider.acquire());
  auto available = f.pool->get();
  ASSERT_TRUE(available);
  EXPECT_TRUE(available.unique());
}

TEST(pusch_resource_loan, retained_pool_survives_external_owner_release)
{
  fixture f;
  f.check_pool                                       = false;
  auto&                                     notifier = f.start();
  auto                                      entry    = f.pool->get();
  pooled_pusch_resource_provider<pool_type> provider(f.pool, entry, notifier);
  auto                                      retained = provider.acquire();
  ASSERT_TRUE(retained);
  std::weak_ptr<pool_type> lifetime = f.pool;
  entry.reset();
  notifier.on_sch({});
  f.pool.reset();
  EXPECT_FALSE(lifetime.expired());
  EXPECT_TRUE(f.fsm.has_receive_request(f.slot));
  retained.reset();
  EXPECT_TRUE(lifetime.expired());
  EXPECT_EQ(f.completions, 1U);
  EXPECT_FALSE(f.fsm.has_receive_request(f.slot));
}

TEST(pusch_resource_loan, repeated_generations_do_not_accumulate_dependency_references)
{
  fixture f;
  for (unsigned i = 0; i != 1000; ++i) {
    auto&                                     notifier = f.start();
    auto                                      entry    = f.pool->get();
    pooled_pusch_resource_provider<pool_type> provider(f.pool, entry, notifier);
    auto                                      retained = provider.acquire();
    ASSERT_TRUE(retained);
    notifier.on_sch({});
    EXPECT_EQ(f.completions, i);
    entry.reset();
    f.expect_busy();
    retained.reset();
    EXPECT_EQ(f.completions, i + 1);
    auto available = f.pool->get();
    ASSERT_TRUE(available);
    EXPECT_TRUE(available.unique());
  }
}
