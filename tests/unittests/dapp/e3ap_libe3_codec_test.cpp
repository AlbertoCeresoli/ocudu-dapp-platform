// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear
//
// The libe3 E3AP 1.0.0 codec: round trips of every procedure, the bounds of
// the libe3 module, and byte-for-byte agreement with vectors produced by the
// upstream libe3 encoder (tests/interop/libe3/libe3_interop_vectors, pinned in
// python/tests/fixtures/libe3_vectors.json and mirrored here).

#include "ocudu/dapp/e3/e3ap_codec.h"
#include "ocudu/dapp/e3/e3ap_libe3_codec.h"
#include "ocudu/dapp/e3/e3ap_v2_codec.h"
#include <cstdlib>
#include <fstream>
#include <gtest/gtest.h>
#include <sstream>

using namespace ocudu::dapp::e3;

namespace {

std::vector<uint8_t> from_hex(const std::string& hex)
{
  std::vector<uint8_t> bytes;
  for (std::size_t index = 0; index + 1 < hex.size(); index += 2) {
    bytes.push_back(static_cast<uint8_t>(std::stoul(hex.substr(index, 2), nullptr, 16)));
  }
  return bytes;
}

std::string to_hex(const std::vector<uint8_t>& bytes)
{
  static const char digits[] = "0123456789abcdef";
  std::string       text;
  for (const uint8_t byte : bytes) {
    text.push_back(digits[byte >> 4]);
    text.push_back(digits[byte & 15]);
  }
  return text;
}

template <typename T>
T round_trip(const T& message, uint32_t id = 7, std::optional<int64_t> timestamp = std::nullopt)
{
  e3ap_pdu pdu;
  pdu.id           = id;
  pdu.timestamp_ns = timestamp;
  pdu.message      = message;
  auto encoded     = e3ap_libe3_codec::encode(pdu);
  EXPECT_TRUE(encoded) << encoded.detail;
  if (!encoded) {
    return T{};
  }
  auto decoded = e3ap_libe3_codec::decode(*encoded.value);
  EXPECT_TRUE(decoded) << decoded.detail;
  if (!decoded) {
    return T{};
  }
  EXPECT_EQ(decoded.value->id, id);
  EXPECT_EQ(decoded.value->timestamp_ns, timestamp);
  auto* body = std::get_if<T>(&decoded.value->message);
  EXPECT_NE(body, nullptr);
  return body != nullptr ? *body : T{};
}

} // namespace

TEST(dapp_e3ap_libe3_codec, round_trips_every_procedure_of_the_libe3_module)
{
  e3ap_setup_request setup;
  setup.dapp_name    = "libe3-dapp";
  setup.dapp_version = "0.2.1";
  setup.vendor       = "Northeastern";
  auto setup_back    = round_trip(setup, 1, 1756800000000000000LL);
  EXPECT_EQ(setup_back.dapp_name, "libe3-dapp");
  EXPECT_EQ(setup_back.e3ap_protocol_version, "1.0.0");

  e3ap_setup_response response;
  response.request_id            = 1;
  response.response_code         = e3ap_response_code::positive;
  response.ran_identifier        = "ocudu-gnb";
  response.e3ap_protocol_version = "1.0.0";
  response.dapp_identifier       = 3;
  e3ap_ran_function_definition function;
  function.ran_function_identifier = e3ap_ran_function_telemetry;
  function.telemetry_identifiers   = {1, 2, 3, 400}; // 400: nameable since libe3 0.2 (1..65535)
  function.ran_function_data       = {0x01, 0x02};
  response.ran_functions.push_back(function);
  response.ran_functions_present = true;
  response.cause                 = e3ap_cause::unspecified; // OCUDU-only, dropped
  response.detail                = "dropped";
  auto response_back             = round_trip(response, 1);
  EXPECT_EQ(response_back.dapp_identifier, std::optional<uint16_t>(3));
  ASSERT_EQ(response_back.ran_functions.size(), 1U);
  EXPECT_EQ(response_back.ran_functions[0].telemetry_identifiers, (std::vector<uint16_t>{1, 2, 3, 400}));
  EXPECT_FALSE(response_back.cause.has_value());
  EXPECT_TRUE(response_back.detail.empty());

  e3ap_subscription_request subscription;
  subscription.dapp_identifier         = 3;
  subscription.ran_function_identifier = e3ap_ran_function_telemetry;
  subscription.telemetry_identifiers   = {2};
  subscription.periodicity             = 100;
  subscription.subscription_time_s     = 60;
  auto subscription_back               = round_trip(subscription, 2);
  EXPECT_EQ(subscription_back.telemetry_identifiers, (std::vector<uint16_t>{2}));
  EXPECT_EQ(subscription_back.periodicity, std::optional<uint32_t>(100));
  EXPECT_TRUE(subscription_back.subscription_data.empty());

  e3ap_subscription_response granted;
  granted.request_id            = 2;
  granted.dapp_identifier       = 3;
  granted.response_code         = e3ap_response_code::positive;
  granted.subscription_id       = 1;
  granted.telemetry_identifiers = std::vector<uint16_t>{2};
  auto granted_back             = round_trip(granted, 2);
  EXPECT_EQ(granted_back.subscription_id, std::optional<uint16_t>(1));
  EXPECT_EQ(granted_back.telemetry_identifiers, std::optional<std::vector<uint16_t>>(std::vector<uint16_t>{2}));

  e3ap_subscription_delete remove;
  remove.dapp_identifier = 3;
  remove.subscription_id = 1;
  EXPECT_EQ(round_trip(remove, 3).subscription_id, 1);

  e3ap_indication_message indication;
  indication.dapp_identifier         = 3;
  indication.ran_function_identifier = e3ap_ran_function_telemetry;
  indication.protocol_data           = std::vector<uint8_t>(1000, 0xAB);
  indication.subscription_id         = 1; // OCUDU-only, dropped
  auto indication_back               = round_trip(indication, 4);
  EXPECT_EQ(indication_back.protocol_data.size(), 1000U);
  EXPECT_FALSE(indication_back.subscription_id.has_value());
  EXPECT_EQ(indication_back.fragment_count, 1U);

  e3ap_dapp_control_action control;
  control.dapp_identifier         = 3;
  control.ran_function_identifier = e3ap_ran_function_management;
  control.control_identifier      = e3ap_management_control_request;
  control.action_data             = {0x10, 0x20, 0x30};
  auto control_back               = round_trip(control, 5);
  EXPECT_EQ(control_back.action_data, control.action_data);
  EXPECT_FALSE(control_back.sequence_identifier.has_value()) << "a control the dApp decided itself";
  control.sequence_identifier = 3000000000U; // a re-issued xApp control (libe3 0.2 optional sequenceId)
  EXPECT_EQ(round_trip(control, 5).sequence_identifier, std::optional<uint32_t>(3000000000U));

  e3ap_dapp_report report;
  report.dapp_identifier         = 300; // beyond the libe3 0.1.x 1..100 range
  report.ran_function_identifier = e3ap_ran_function_telemetry;
  report.report_data             = {0x7F};
  report.sequence_identifier     = 4242;
  auto report_back               = round_trip(report, 6);
  EXPECT_EQ(report_back.report_data, report.report_data);
  EXPECT_EQ(report_back.dapp_identifier, 300);
  EXPECT_EQ(report_back.sequence_identifier, std::optional<uint32_t>(4242));

  e3ap_xapp_control_action xapp;
  xapp.dapp_identifier         = 40000;
  xapp.ran_function_identifier = e3ap_ran_function_telemetry;
  xapp.xapp_control_data       = {0x01};
  xapp.sequence_identifier     = 70000;
  auto xapp_back               = round_trip(xapp, 4000000000U); // 32-bit message id since libe3 0.2
  EXPECT_EQ(xapp_back.xapp_control_data, xapp.xapp_control_data);
  EXPECT_EQ(xapp_back.sequence_identifier, std::optional<uint32_t>(70000));

  e3ap_release_message release;
  release.dapp_identifier = 3;
  EXPECT_EQ(round_trip(release, 8).dapp_identifier, 3);

  e3ap_message_ack ack;
  ack.request_id    = 5;
  ack.response_code = e3ap_response_code::negative;
  ack.cause         = e3ap_cause::invalid_request; // OCUDU-only, dropped
  auto ack_back     = round_trip(ack, 1000);
  EXPECT_EQ(ack_back.response_code, e3ap_response_code::negative);
  EXPECT_FALSE(ack_back.cause.has_value());
}

TEST(dapp_e3ap_libe3_codec, refuses_values_the_libe3_module_cannot_carry)
{
  const auto rejects = [](const e3ap_message& message, uint32_t id = 1) {
    e3ap_pdu pdu;
    pdu.id      = id;
    pdu.message = message;
    return !e3ap_libe3_codec::encode(pdu);
  };
  e3ap_release_message release;
  release.dapp_identifier = 3;
  EXPECT_TRUE(rejects(release, 0)) << "message id 0 (E3-MessageID is 1..4294967295)";
  EXPECT_FALSE(rejects(release, 0xFFFFFFFFU));
  release.dapp_identifier = 0;
  EXPECT_TRUE(rejects(release)) << "dApp identifier 0 (E3-DAppID is 1..65535)";
  release.dapp_identifier = 65535;
  EXPECT_FALSE(rejects(release));

  // libe3 0.2: E3-SequenceID is mandatory on E3-DAppReport and E3-XAppControlAction.
  e3ap_dapp_report report;
  report.dapp_identifier         = 3;
  report.ran_function_identifier = 10;
  report.report_data             = {1};
  EXPECT_TRUE(rejects(report)) << "report without a sequence identifier";
  report.sequence_identifier = 1;
  EXPECT_FALSE(rejects(report));
  e3ap_xapp_control_action xapp;
  xapp.dapp_identifier         = 3;
  xapp.ran_function_identifier = 10;
  xapp.xapp_control_data       = {1};
  EXPECT_TRUE(rejects(xapp)) << "xApp control without a sequence identifier";
  xapp.sequence_identifier = 0;
  EXPECT_TRUE(rejects(xapp)) << "sequence identifier 0";

  // Identifier lists hold up to 1000 entries since libe3 0.2.
  e3ap_subscription_request subscription;
  subscription.dapp_identifier         = 3;
  subscription.ran_function_identifier = 10;
  subscription.telemetry_identifiers.assign(1000, 1);
  EXPECT_FALSE(rejects(subscription));
  subscription.telemetry_identifiers.assign(1001, 1);
  EXPECT_TRUE(rejects(subscription)) << "identifier list above 1000 entries";

  e3ap_indication_message indication;
  indication.dapp_identifier         = 3;
  indication.ran_function_identifier = 10;
  indication.protocol_data           = std::vector<uint8_t>(32769, 1);
  EXPECT_TRUE(rejects(indication)) << "payload above 32 KiB";
  indication.protocol_data.resize(32768);
  EXPECT_FALSE(rejects(indication));
  indication.fragment_count = 2;
  EXPECT_TRUE(rejects(indication)) << "fragmented payload";
  indication.fragment_count = 1;
  indication.by_reference   = e3ap_tensor_descriptor{};
  EXPECT_TRUE(rejects(indication)) << "by-reference descriptor";

  e3ap_control_response outcome;
  outcome.request_id      = 1;
  outcome.dapp_identifier = 3;
  std::string detail;
  EXPECT_FALSE(e3ap_libe3_codec::representable(outcome, &detail));
  EXPECT_NE(detail.find("E3-MessageAck"), std::string::npos);

  e3ap_setup_request setup;
  setup.dapp_name    = "x";
  setup.dapp_version = "0.0.0.0.0.0.1"; // 13 bytes, above libe3's E3-Version (1..11)
  setup.vendor       = "v";
  EXPECT_TRUE(rejects(setup));
}

TEST(dapp_e3ap_libe3_codec, ocudu_and_libe3_setups_are_the_same_bytes_at_every_id_width)
{
  // One module: an OCUDU setup without additions is libe3's setup, whatever the id width and with or
  // without the envelope timestamp; with the OCUDU profile requested, the libe3 profile still reads it.
  e3ap_pdu pdu;
  e3ap_setup_request setup;
  setup.dapp_name    = "x";
  setup.dapp_version = "0";
  setup.vendor       = "v";
  pdu.message        = setup;
  for (uint32_t id : {1U, 7U, 256U, 257U, 1000U, 65536U, 70000U, 16777217U, 4000000000U, 4294967295U}) {
    for (bool stamped : {false, true}) {
      pdu.id           = id;
      pdu.timestamp_ns = stamped ? std::optional<int64_t>(1756800000123456789LL) : std::nullopt;
      auto libe3_wire  = e3ap_libe3_codec::encode(pdu);
      auto ocudu_wire  = e3ap_v2_codec::encode(pdu, e3ap_profile::ocudu);
      ASSERT_TRUE(libe3_wire && ocudu_wire);
      EXPECT_EQ(*libe3_wire.value, *ocudu_wire.value) << "id " << id;
      auto requested                                     = pdu;
      std::get<e3ap_setup_request>(requested.message).ocudu_profile = e3ap_ocudu_profile_version;
      auto with_profile                                  = e3ap_v2_codec::encode(requested, e3ap_profile::ocudu);
      ASSERT_TRUE(with_profile) << with_profile.detail;
      auto seen = e3ap_libe3_codec::decode(*with_profile.value);
      ASSERT_TRUE(seen) << seen.detail;
      auto seen_bytes = e3ap_libe3_codec::encode(*seen.value);
      ASSERT_TRUE(seen_bytes);
      EXPECT_EQ(*seen_bytes.value, *libe3_wire.value) << "id " << id;
    }
  }
}

// Vectors emitted by the upstream libe3 encoder (Asn1E3Encoder, libe3 0.2.1 at
// f2329e0 (tag 0.2.1), asn1c mouse07410 844f9ca). Regenerate with scripts/regen_libe3_vectors.py, i.e.
//   tests/interop/libe3/libe3_interop_vectors encode
// and update python/tests/fixtures/libe3_vectors.json together with this table.
struct libe3_vector {
  const char* name;
  const char* hex;
};

const libe3_vector pinned_vectors[] = {
#include "e3ap_libe3_vectors.inc"
};

TEST(dapp_e3ap_libe3_codec, decodes_and_reproduces_vectors_from_the_upstream_libe3_encoder)
{
  const std::size_t count = sizeof(pinned_vectors) / sizeof(pinned_vectors[0]);
  ASSERT_EQ(count, 11U) << "one vector per libe3 procedure";
  for (const auto& vector : pinned_vectors) {
    SCOPED_TRACE(vector.name);
    const auto bytes   = from_hex(vector.hex);
    auto       decoded = e3ap_libe3_codec::decode(bytes);
    ASSERT_TRUE(decoded) << decoded.detail;
    EXPECT_EQ(decoded.value->id, 42U);
    EXPECT_EQ(decoded.value->timestamp_ns, std::optional<int64_t>(1756800000123456789LL));
    // Re-encoding the decoded value with OCUDU's codec must reproduce libe3's bytes.
    auto encoded = e3ap_libe3_codec::encode(*decoded.value);
    ASSERT_TRUE(encoded) << encoded.detail;
    EXPECT_EQ(to_hex(*encoded.value), vector.hex);
  }
}
