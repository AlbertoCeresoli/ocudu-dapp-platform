// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// Host-only selection/accounting checks: no CUDA runtime or driver needed.
#include "ocudu/phy/upper/dapp/pusch_resident_dapp.h"
#include <gtest/gtest.h>

using namespace ocudu;

namespace {
class admission_spy final : public pusch_resident_dapp
{
public:
  pusch_resident_dapp_mode selected = pusch_resident_dapp_mode::inactive;
  mutable unsigned        selections = 0;
  unsigned                fallbacks = 0;
  unsigned                invocations = 0;
  pusch_resident_dapp_mode fallback_mode = pusch_resident_dapp_mode::inactive;
  dapp::class_a_fallback_reason fallback_reason = dapp::class_a_fallback_reason::count;

  pusch_resident_dapp_mode mode() const noexcept override
  {
    ++selections;
    return selected;
  }
  void report_fallback(pusch_resident_dapp_mode mode, dapp::class_a_fallback_reason reason) noexcept override
  {
    ++fallbacks;
    fallback_mode = mode;
    fallback_reason = reason;
  }
  dapp::native_invocation_result invoke_equalizer(const pusch_resident_dapp_buffers&,
                                                  const pusch_demodulator::configuration&) noexcept override
  {
    ++invocations;
    return {OCUDU_DAPP_BYPASS_V1, true};
  }
  dapp::native_invocation_result invoke_receiver(const pusch_resident_dapp_buffers&,
                                                 const pusch_demodulator::configuration&) noexcept override
  {
    ++invocations;
    return {OCUDU_DAPP_BYPASS_V1, true};
  }
  void on_completion(void*) noexcept override {}
};
} // namespace

TEST(pusch_resident_admission, accounts_only_rejected_full_receiver_profiles)
{
  for (auto mode : {pusch_resident_dapp_mode::inactive,
                    pusch_resident_dapp_mode::channel_estimator,
                    pusch_resident_dapp_mode::equalizer,
                    pusch_resident_dapp_mode::receiver,
                    pusch_resident_dapp_mode::conflict}) {
    for (bool required : {false, true}) {
      for (bool supported : {false, true}) {
        admission_spy adapter;
        adapter.selected = mode;
        const bool rejected = mode == pusch_resident_dapp_mode::receiver && required && !supported;
        EXPECT_EQ(adapter.mode_with_pilot_admission(required, supported),
                  rejected ? pusch_resident_dapp_mode::inactive : mode);
        EXPECT_EQ(adapter.selections, 1U);
        EXPECT_EQ(adapter.fallbacks, rejected ? 1U : 0U);
        EXPECT_EQ(adapter.invocations, 0U);
        if (rejected) {
          EXPECT_EQ(adapter.fallback_mode, pusch_resident_dapp_mode::receiver);
          EXPECT_EQ(adapter.fallback_reason, dapp::class_a_fallback_reason::unsupported_metadata);
        }
      }
    }
  }
}

TEST(pusch_resident_admission, changing_grants_and_deactivation_do_not_reuse_admission)
{
  admission_spy adapter;
  adapter.selected = pusch_resident_dapp_mode::receiver;
  for (unsigned i = 0; i != 64; ++i) {
    EXPECT_EQ(adapter.mode_with_pilot_admission(true, false), pusch_resident_dapp_mode::inactive);
    EXPECT_EQ(adapter.fallbacks, i + 1);
    EXPECT_EQ(adapter.mode_with_pilot_admission(true, true), pusch_resident_dapp_mode::receiver);
    EXPECT_EQ(adapter.fallbacks, i + 1);
  }
  adapter.selected = pusch_resident_dapp_mode::inactive;
  EXPECT_EQ(adapter.mode_with_pilot_admission(true, false), pusch_resident_dapp_mode::inactive);
  EXPECT_EQ(adapter.fallbacks, 64U);
  EXPECT_EQ(adapter.selections, 129U);
  EXPECT_EQ(adapter.invocations, 0U);
}
