// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear
//
// E3AP-OCUDU-V2 in APER: one codec for libe3 and OCUDU peers. The libe3 profile reproduces libe3's
// own encoder (e3ap_libe3_codec_test pins the vectors); here: every OCUDU addition round-trips;
// an OCUDU-profile PDU reads as the libe3 message when the additions are ignored (what a libe3
// decoder does); and APER and Protobuf (e3ap_protobuf_codec) are the same protocol: on random PDUs
// of every procedure and both profiles they accept and refuse the same values and translate into
// each other byte for byte.

#include "e3ap_pdu_generator.h"
#include "ocudu/dapp/e3/e3ap_protobuf_codec.h"
#include "ocudu/dapp/e3/e3ap_v2_codec.h"
#include <gtest/gtest.h>

using namespace ocudu::dapp::e3;
using ocudu::dapp::e3::testing::pdu_generator;

namespace {

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

const char* kind_name(const e3ap_message& message)
{
  static const char* names[] = {"SetupRequest",       "SetupResponse",      "SubscriptionRequest",
                                "SubscriptionDelete", "SubscriptionResponse", "IndicationMessage",
                                "DAppControlAction",  "DAppReport",         "XAppControlAction",
                                "ReleaseMessage",     "MessageAck",         "ControlResponse"};
  return message.index() < 12 ? names[message.index()] : "?";
}

} // namespace

TEST(dapp_e3ap_v2_codec, every_ocudu_addition_round_trips_and_libe3_peers_see_the_libe3_message)
{
  pdu_generator generator(7, e3ap_profile::ocudu);
  for (std::size_t iteration = 0; iteration != 6000; ++iteration) {
    const auto pdu     = generator.next(iteration);
    auto       encoded = e3ap_v2_codec::encode(pdu, e3ap_profile::ocudu);
    ASSERT_TRUE(encoded) << encoded.detail << " (" << kind_name(pdu.message) << ")";
    auto decoded = e3ap_v2_codec::decode(*encoded.value, e3ap_profile::ocudu);
    ASSERT_TRUE(decoded) << decoded.detail;
    auto again = e3ap_v2_codec::encode(*decoded.value, e3ap_profile::ocudu);
    ASSERT_TRUE(again) << again.detail;
    ASSERT_EQ(to_hex(*again.value), to_hex(*encoded.value)) << kind_name(pdu.message);

    // A libe3 decoder skips the extension additions. For every PDU the libe3 module can carry,
    // what it reads is exactly the libe3-profile PDU.
    auto plain = e3ap_v2_codec::encode(pdu, e3ap_profile::libe3);
    auto seen  = e3ap_v2_codec::decode(*encoded.value, e3ap_profile::libe3);
    if (std::holds_alternative<e3ap_control_response>(pdu.message)) {
      EXPECT_FALSE(seen) << "an OCUDU-only message is never misread as a libe3 one";
      continue;
    }
    if (!plain) {
      continue; // fragmented, by-reference, ... : not sent to libe3 peers
    }
    ASSERT_TRUE(seen) << seen.detail << " (" << kind_name(pdu.message) << ")";
    auto seen_bytes = e3ap_v2_codec::encode(*seen.value, e3ap_profile::libe3);
    ASSERT_TRUE(seen_bytes) << seen_bytes.detail;
    ASSERT_EQ(to_hex(*seen_bytes.value), to_hex(*plain.value)) << kind_name(pdu.message);
  }
}

TEST(dapp_e3ap_v2_codec, aper_and_protobuf_carry_the_same_protocol_in_both_profiles)
{
  for (const auto profile : {e3ap_profile::libe3, e3ap_profile::ocudu}) {
    // Random PDUs inside the profile's bounds, then the same PDUs pushed past them: the two
    // encodings must agree on acceptance as well.
    pdu_generator inside(profile == e3ap_profile::ocudu ? 101 : 202, profile);
    pdu_generator beyond(303, e3ap_profile::ocudu);
    for (std::size_t iteration = 0; iteration != 12000; ++iteration) {
      const auto pdu      = (iteration % 3 == 2 ? beyond : inside).next(iteration);
      auto       aper     = e3ap_v2_codec::encode(pdu, profile);
      auto       protobuf = e3ap_protobuf_codec::encode(pdu, profile);
      ASSERT_EQ(static_cast<bool>(aper), static_cast<bool>(protobuf))
          << kind_name(pdu.message) << ": APER says '" << aper.detail << "', Protobuf says '" << protobuf.detail << "'";
      if (!aper) {
        continue;
      }
      // APER -> value -> Protobuf equals Protobuf directly, and the reverse.
      auto from_aper = e3ap_v2_codec::decode(*aper.value, profile);
      ASSERT_TRUE(from_aper) << from_aper.detail;
      auto as_protobuf = e3ap_protobuf_codec::encode(*from_aper.value, profile);
      ASSERT_TRUE(as_protobuf) << as_protobuf.detail;
      ASSERT_EQ(to_hex(*as_protobuf.value), to_hex(*protobuf.value)) << kind_name(pdu.message);
      auto from_protobuf = e3ap_protobuf_codec::decode(*protobuf.value, profile);
      ASSERT_TRUE(from_protobuf) << from_protobuf.detail;
      auto as_aper = e3ap_v2_codec::encode(*from_protobuf.value, profile);
      ASSERT_TRUE(as_aper) << as_aper.detail;
      ASSERT_EQ(to_hex(*as_aper.value), to_hex(*aper.value)) << kind_name(pdu.message);
    }
  }
}

TEST(dapp_e3ap_v2_codec, wider_subscription_ranges_ride_in_additions_and_root_stays_libe3)
{
  e3ap_subscription_request request;
  request.dapp_identifier         = 101;
  request.ran_function_identifier = e3ap_ran_function_telemetry;
  request.telemetry_identifiers   = {2};
  request.subscription_time_s     = 86400;
  request.periodicity             = 1000; // fits libe3's root
  e3ap_pdu pdu;
  pdu.id      = 3;
  pdu.message = request;
  EXPECT_FALSE(e3ap_v2_codec::encode(pdu, e3ap_profile::libe3)) << "86400 s is beyond libe3's 3600";
  auto encoded = e3ap_v2_codec::encode(pdu, e3ap_profile::ocudu);
  ASSERT_TRUE(encoded) << encoded.detail;
  auto back = e3ap_v2_codec::decode(*encoded.value, e3ap_profile::ocudu);
  ASSERT_TRUE(back) << back.detail;
  const auto& read = std::get<e3ap_subscription_request>(back.value->message);
  EXPECT_EQ(read.subscription_time_s, std::optional<uint32_t>(86400));
  EXPECT_EQ(read.periodicity, std::optional<uint32_t>(1000));
  // A libe3 peer sees no subscription time (the root field is absent) and the root periodicity.
  auto libe3_view = e3ap_v2_codec::decode(*encoded.value, e3ap_profile::libe3);
  ASSERT_TRUE(libe3_view) << libe3_view.detail;
  const auto& plain = std::get<e3ap_subscription_request>(libe3_view.value->message);
  EXPECT_FALSE(plain.subscription_time_s);
  EXPECT_EQ(plain.periodicity, std::optional<uint32_t>(1000));
}

TEST(dapp_e3ap_v2_codec, sequence_identifiers_are_mandatory_on_reports_and_xapp_controls)
{
  e3ap_dapp_report report;
  report.dapp_identifier         = 101;
  report.ran_function_identifier = 10;
  report.report_data             = {1};
  for (const auto profile : {e3ap_profile::libe3, e3ap_profile::ocudu}) {
    EXPECT_FALSE(e3ap_v2_codec::representable(report, profile)) << "libe3's root field, in both profiles";
    EXPECT_FALSE(e3ap_protobuf_codec::representable(report, profile));
  }
  report.sequence_identifier = 9;
  EXPECT_TRUE(e3ap_v2_codec::representable(report, e3ap_profile::ocudu));
}
