// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/runtime/guarded_call.h"
#include <gtest/gtest.h>
#include <stdexcept>

using namespace ocudu::dapp;

TEST(dapp_guarded_call, standard_exception_never_crosses_boundary)
{
  circuit_breaker  breaker;
  incident_ring<8> incidents;
  incident_context context{};
  context.invocation_sequence = 42;

  auto result = guarded_call(
      []() -> ocudu_dapp_status_v1 { throw std::runtime_error("bounded\nmessage"); }, breaker, incidents, context);
  EXPECT_EQ(result.status, OCUDU_DAPP_EXCEPTION_V1);
  EXPECT_TRUE(result.invoke_fallback);
  EXPECT_TRUE(result.incident_recorded);
  EXPECT_EQ(breaker.state(), breaker_state::open);

  incident_record incident;
  ASSERT_TRUE(incidents.try_pop(incident));
  EXPECT_EQ(incident.cause, incident_cause::standard_exception);
  EXPECT_EQ(incident.context.invocation_sequence, 42U);
  EXPECT_TRUE(incident.breaker_open);
  EXPECT_EQ(std::string(incident.sanitized_message.data(), incident.message_size), "bounded?message");
}

TEST(dapp_guarded_call, unknown_exception_is_contained)
{
  circuit_breaker  breaker;
  incident_ring<8> incidents;
  auto             result = guarded_call([]() -> ocudu_dapp_status_v1 { throw 7; }, breaker, incidents, {});
  EXPECT_EQ(result.status, OCUDU_DAPP_EXCEPTION_V1);
  incident_record incident;
  ASSERT_TRUE(incidents.try_pop(incident));
  EXPECT_EQ(incident.cause, incident_cause::unknown_exception);
}

TEST(dapp_guarded_call, open_breaker_bypasses_without_calling_module)
{
  circuit_breaker  breaker;
  incident_ring<8> incidents;
  breaker.force_open();
  bool called = false;
  auto result = guarded_call(
      [&]() {
        called = true;
        return OCUDU_DAPP_OK_V1;
      },
      breaker,
      incidents,
      {});
  EXPECT_FALSE(called);
  EXPECT_EQ(result.status, OCUDU_DAPP_BYPASS_V1);
  EXPECT_TRUE(result.invoke_fallback);
}

TEST(dapp_guarded_call, explicit_bypass_does_not_create_incident)
{
  circuit_breaker  breaker;
  incident_ring<8> incidents;
  auto             result = guarded_call([] { return OCUDU_DAPP_BYPASS_V1; }, breaker, incidents, {});
  EXPECT_FALSE(result.incident_recorded);
  incident_record unused;
  EXPECT_FALSE(incidents.try_pop(unused));
}
