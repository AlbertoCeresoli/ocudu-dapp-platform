// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear
//
// The hand-written E3AP Protobuf codec: byte-for-byte agreement with vectors
// from libe3's own ProtobufE3Encoder (pinned in
// python/tests/fixtures/libe3_vectors.json and mirrored here), round trips of
// every procedure under both profiles, the libe3 profile's bounds, and the
// decoder's proto3 leniency (field order, packing, unknown fields). The
// libprotobuf cross-check of the OCUDU superset schema lives in
// e3ap_protobuf_conformance_test.cpp.

#include "ocudu/dapp/e3/e3ap_protobuf_codec.h"
#include <gtest/gtest.h>
#include <random>

using namespace ocudu::dapp::e3;

namespace {

constexpr auto libe3_profile = e3ap_profile::libe3;
constexpr auto ocudu_profile = e3ap_profile::ocudu;

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

e3ap_pdu pdu_of(e3ap_message message, uint32_t id = 7, std::optional<int64_t> timestamp = std::nullopt)
{
  e3ap_pdu pdu;
  pdu.id           = id;
  pdu.timestamp_ns = timestamp;
  pdu.message      = std::move(message);
  return pdu;
}

std::vector<uint8_t> encode(const e3ap_pdu& pdu, e3ap_profile profile)
{
  auto encoded = e3ap_protobuf_codec::encode(pdu, profile);
  EXPECT_TRUE(encoded) << encoded.detail;
  return encoded ? *encoded.value : std::vector<uint8_t>{};
}

/// Encodes, decodes and re-encodes; the second encoding must equal the first (a lossless round trip
/// for everything the profile carries). Returns the decoded body.
template <typename T>
T round_trip(const T& message, e3ap_profile profile, std::optional<int64_t> timestamp = std::nullopt)
{
  const auto pdu   = pdu_of(message, 9, timestamp);
  const auto bytes = encode(pdu, profile);
  auto       back  = e3ap_protobuf_codec::decode(bytes, profile);
  EXPECT_TRUE(back) << back.detail;
  if (!back) {
    return T{};
  }
  EXPECT_EQ(back.value->id, 9U);
  EXPECT_EQ(back.value->timestamp_ns, timestamp);
  EXPECT_EQ(to_hex(encode(*back.value, profile)), to_hex(bytes));
  const auto* body = std::get_if<T>(&back.value->message);
  EXPECT_NE(body, nullptr);
  return body != nullptr ? *body : T{};
}

// Minimal proto3 writer for hand-made wire input.
void varint(std::vector<uint8_t>& out, uint64_t value)
{
  while (value >= 0x80) {
    out.push_back(static_cast<uint8_t>(value | 0x80));
    value >>= 7;
  }
  out.push_back(static_cast<uint8_t>(value));
}

void tag(std::vector<uint8_t>& out, uint32_t field, uint8_t type)
{
  varint(out, (uint64_t{field} << 3) | type);
}

void put_varint(std::vector<uint8_t>& out, uint32_t field, uint64_t value)
{
  tag(out, field, 0);
  varint(out, value);
}

void put_bytes(std::vector<uint8_t>& out, uint32_t field, const std::vector<uint8_t>& value)
{
  tag(out, field, 2);
  varint(out, value.size());
  out.insert(out.end(), value.begin(), value.end());
}

void put_text(std::vector<uint8_t>& out, uint32_t field, const std::string& value)
{
  put_bytes(out, field, std::vector<uint8_t>(value.begin(), value.end()));
}

std::vector<uint8_t> wrap(uint32_t id, uint32_t message_field, const std::vector<uint8_t>& body)
{
  std::vector<uint8_t> out;
  put_varint(out, 1, id);
  put_bytes(out, message_field, body);
  return out;
}

e3ap_setup_response full_setup_response()
{
  e3ap_setup_response response;
  response.request_id            = 1;
  response.response_code         = e3ap_response_code::positive;
  response.ran_identifier        = "ocudu-gnb";
  response.e3ap_protocol_version = "1.0.0";
  response.dapp_identifier       = 101;
  e3ap_ran_function_definition function;
  function.ran_function_identifier = e3ap_ran_function_telemetry;
  function.telemetry_identifiers   = {1, 2, 300};
  function.ran_function_data       = std::vector<uint8_t>(200, 0x5A);
  function.ran_function_name       = "telemetry";
  function.ran_function_version    = "1.2";
  response.ran_functions.push_back(function);
  response.ran_functions_present  = true;
  response.cause                  = e3ap_cause::duplicate;
  response.detail                 = "détail ✓";
  response.ran_instance           = ocudu::dapp::management::identifier{};
  response.ran_instance->bytes[3] = 0x42;
  response.maximum_payload_bytes  = 4U * 1024U * 1024U;
  response.maximum_fragment_bytes = e3ap_maximum_fragment_bytes;
  return response;
}

} // namespace

TEST(dapp_e3ap_protobuf_codec, decodes_and_reproduces_vectors_from_the_upstream_libe3_protobuf_encoder)
{
  const std::vector<std::pair<std::string, std::string>> vectors = {
#include "e3ap_libe3_protobuf_vectors.inc"
  };
  ASSERT_EQ(vectors.size(), 11U) << "one vector per libe3 E3AP 1.0.0 procedure";
  for (const auto& [name, hex] : vectors) {
    SCOPED_TRACE(name);
    const auto bytes   = from_hex(hex);
    auto       decoded = e3ap_protobuf_codec::decode(bytes, libe3_profile);
    ASSERT_TRUE(decoded) << decoded.detail;
    EXPECT_EQ(decoded.value->id, 42U);
    EXPECT_EQ(decoded.value->timestamp_ns, std::optional<int64_t>(1756800000123456789LL));
    EXPECT_EQ(to_hex(encode(*decoded.value, libe3_profile)), hex) << "not byte-identical to libe3's ProtobufE3Encoder";
    // An OCUDU-profile peer reads libe3 bytes too (they are a subset of its schema).
    auto superset = e3ap_protobuf_codec::decode(bytes, ocudu_profile);
    ASSERT_TRUE(superset) << superset.detail;
    EXPECT_EQ(superset.value->message.index(), decoded.value->message.index());
    const auto view = e3ap_protobuf_codec::peek(bytes);
    ASSERT_TRUE(view);
    EXPECT_EQ(view->pdu_id, 42U);
  }
}

TEST(dapp_e3ap_protobuf_codec, round_trips_every_procedure_under_the_libe3_profile)
{
  e3ap_setup_request setup;
  setup.dapp_name    = "libe3-dapp";
  setup.dapp_version = "0.2.1";
  setup.vendor       = "Northeastern";
  EXPECT_EQ(round_trip(setup, libe3_profile, 1756800000000000000LL).dapp_name, "libe3-dapp");

  auto response      = full_setup_response();
  auto response_back = round_trip(response, libe3_profile);
  EXPECT_EQ(response_back.dapp_identifier, std::optional<uint16_t>(101));
  ASSERT_EQ(response_back.ran_functions.size(), 1U);
  EXPECT_EQ(response_back.ran_functions[0].telemetry_identifiers, (std::vector<uint16_t>{1, 2, 300}));
  EXPECT_FALSE(response_back.cause) << "OCUDU fields are not sent to libe3 peers";
  EXPECT_TRUE(response_back.detail.empty());
  EXPECT_TRUE(response_back.ran_functions[0].ran_function_name.empty());
  EXPECT_FALSE(response_back.ran_instance);

  e3ap_subscription_request subscription;
  subscription.dapp_identifier         = 3;
  subscription.ran_function_identifier = e3ap_ran_function_telemetry;
  subscription.telemetry_identifiers   = {2, 7};
  subscription.periodicity             = 0; // optional: present though zero
  subscription.subscription_time_s     = 60;
  subscription.subscription_data       = {1, 2, 3};
  auto subscription_back               = round_trip(subscription, libe3_profile);
  EXPECT_EQ(subscription_back.periodicity, std::optional<uint32_t>(0));
  EXPECT_EQ(subscription_back.telemetry_identifiers, (std::vector<uint16_t>{2, 7}));
  EXPECT_TRUE(subscription_back.subscription_data.empty());

  e3ap_subscription_response granted;
  granted.request_id            = 2;
  granted.dapp_identifier       = 3;
  granted.response_code         = e3ap_response_code::negative;
  granted.subscription_id       = 1;
  granted.telemetry_identifiers = std::vector<uint16_t>{2};
  auto granted_back             = round_trip(granted, libe3_profile);
  EXPECT_EQ(granted_back.response_code, e3ap_response_code::negative);
  EXPECT_FALSE(granted_back.telemetry_identifiers) << "libe3's Protobuf SubscriptionResponse has no lists";

  e3ap_indication_message indication;
  indication.dapp_identifier         = 3;
  indication.ran_function_identifier = e3ap_ran_function_telemetry;
  indication.protocol_data           = std::vector<uint8_t>(32768, 0xAB);
  indication.subscription_id         = 1;
  auto indication_back               = round_trip(indication, libe3_profile);
  EXPECT_EQ(indication_back.protocol_data.size(), 32768U);
  EXPECT_FALSE(indication_back.subscription_id);
  EXPECT_EQ(indication_back.payload_bytes, std::optional<uint32_t>(32768));

  e3ap_dapp_control_action control;
  control.dapp_identifier         = 3;
  control.ran_function_identifier = e3ap_ran_function_management;
  control.control_identifier      = e3ap_management_control_request;
  control.action_data             = {0x10};
  EXPECT_FALSE(round_trip(control, libe3_profile).sequence_identifier) << "sequence_id 0 is absent";
  control.sequence_identifier = 4000000000U;
  EXPECT_EQ(round_trip(control, libe3_profile).sequence_identifier, std::optional<uint32_t>(4000000000U));

  e3ap_dapp_report report;
  report.dapp_identifier         = 65535;
  report.ran_function_identifier = 1;
  report.report_data             = {1};
  report.sequence_identifier     = 1;
  EXPECT_EQ(round_trip(report, libe3_profile).dapp_identifier, 65535);

  e3ap_xapp_control_action xapp;
  xapp.dapp_identifier         = 3;
  xapp.ran_function_identifier = 1;
  xapp.xapp_control_data       = {9, 9};
  xapp.sequence_identifier     = 77;
  EXPECT_EQ(round_trip(xapp, libe3_profile).sequence_identifier, std::optional<uint32_t>(77));

  EXPECT_EQ(round_trip(e3ap_release_message{3}, libe3_profile).dapp_identifier, 3);

  e3ap_message_ack ack;
  ack.request_id    = 5;
  ack.response_code = e3ap_response_code::positive;
  ack.detail        = "dropped";
  EXPECT_TRUE(round_trip(ack, libe3_profile).detail.empty());
}

TEST(dapp_e3ap_protobuf_codec, round_trips_every_ocudu_field_under_the_ocudu_profile)
{
  e3ap_setup_request setup;
  setup.dapp_name                   = "native";
  setup.dapp_version                = "2.0.0-rc.1";
  setup.vendor                      = "DeepSig";
  setup.dapp_instance               = ocudu::dapp::management::identifier{};
  setup.dapp_instance->bytes.back() = 0x99;
  setup.ocudu_profile               = e3ap_ocudu_profile_version;
  auto setup_back                   = round_trip(setup, ocudu_profile);
  EXPECT_EQ(setup_back.ocudu_profile, std::optional<uint8_t>(1));
  ASSERT_TRUE(setup_back.dapp_instance);
  EXPECT_EQ(setup_back.dapp_instance->bytes.back(), 0x99);

  auto response_back = round_trip(full_setup_response(), ocudu_profile);
  EXPECT_EQ(response_back.cause, std::optional<e3ap_cause>(e3ap_cause::duplicate));
  EXPECT_EQ(response_back.detail, "détail ✓");
  EXPECT_EQ(response_back.ran_functions[0].ran_function_name, "telemetry");
  EXPECT_EQ(response_back.ran_functions[0].ran_function_version, "1.2");
  ASSERT_TRUE(response_back.ran_instance);
  EXPECT_EQ(response_back.ran_instance->bytes[3], 0x42);
  EXPECT_EQ(response_back.maximum_payload_bytes, std::optional<uint32_t>(4U * 1024U * 1024U));

  // An empty RAN function list that is present stays present.
  e3ap_setup_response empty_list = full_setup_response();
  empty_list.ran_functions.clear();
  EXPECT_TRUE(round_trip(empty_list, ocudu_profile).ran_functions_present);
  empty_list.ran_functions_present = false;
  EXPECT_FALSE(round_trip(empty_list, ocudu_profile).ran_functions_present);

  e3ap_subscription_request subscription;
  subscription.dapp_identifier            = 101;
  subscription.ran_function_identifier    = e3ap_ran_function_telemetry;
  subscription.telemetry_identifiers      = {5};
  subscription.subscription_time_s        = 86400;
  subscription.periodicity                = 60000;
  subscription.subscription_data          = std::vector<uint8_t>(1000, 0x11);
  subscription.delivery_mode              = e3ap_delivery_mode::by_reference;
  subscription.minimum_by_reference_bytes = 0;
  auto subscription_back                  = round_trip(subscription, ocudu_profile);
  EXPECT_EQ(subscription_back.delivery_mode, std::optional<e3ap_delivery_mode>(e3ap_delivery_mode::by_reference));
  EXPECT_EQ(subscription_back.minimum_by_reference_bytes, std::optional<uint32_t>(0));
  EXPECT_EQ(subscription_back.subscription_data.size(), 1000U);

  e3ap_subscription_response granted;
  granted.request_id            = 2;
  granted.dapp_identifier       = 101;
  granted.response_code         = e3ap_response_code::positive;
  granted.subscription_id       = 4;
  granted.telemetry_identifiers = std::vector<uint16_t>{5, 6};
  granted.control_identifiers   = std::vector<uint16_t>{}; // present but empty
  granted.detail                = "ok";
  granted.shared_region.emplace();
  granted.shared_region->region_name    = "/ocudu-e3-1-101-4";
  granted.shared_region->region_bytes   = 1ULL << 33;
  granted.shared_region->row_bytes      = 4ULL << 20;
  granted.shared_region->nof_rows       = 4;
  granted.shared_region->header_bytes   = 0;
  granted.shared_region->layout_version = 1;
  auto granted_back                     = round_trip(granted, ocudu_profile);
  EXPECT_EQ(granted_back.telemetry_identifiers, std::optional<std::vector<uint16_t>>(std::vector<uint16_t>{5, 6}));
  EXPECT_EQ(granted_back.control_identifiers, std::optional<std::vector<uint16_t>>(std::vector<uint16_t>{}));
  ASSERT_TRUE(granted_back.shared_region);
  EXPECT_EQ(granted_back.shared_region->region_bytes, 1ULL << 33);
  EXPECT_EQ(granted_back.shared_region->header_bytes, 0);

  e3ap_indication_message indication;
  indication.dapp_identifier         = 101;
  indication.ran_function_identifier = e3ap_ran_function_telemetry;
  indication.protocol_data           = std::vector<uint8_t>(e3ap_maximum_fragment_bytes, 0xCD);
  indication.subscription_id         = 4;
  indication.telemetry_identifier    = 5;
  indication.sequence                = ~uint64_t{0};
  indication.fragment_index          = 2;
  indication.fragment_count          = 65536;
  indication.payload_bytes           = e3ap_maximum_payload_bytes;
  auto indication_back               = round_trip(indication, ocudu_profile);
  EXPECT_EQ(indication_back.sequence, std::optional<uint64_t>(~uint64_t{0}));
  EXPECT_EQ(indication_back.fragment_count, 65536U);
  EXPECT_EQ(indication_back.fragment_index, 2);

  e3ap_indication_message by_reference = indication;
  by_reference.protocol_data           = {1, 2, 3};
  by_reference.fragment_index          = 0;
  by_reference.fragment_count          = 1;
  e3ap_tensor_descriptor posix;
  posix.memory_domain = e3ap_memory_domain::posix_shared;
  posix.location      = e3ap_posix_shared_location{"/ocudu-e3-1-101-4", 3, 4096};
  posix.byte_length   = 1 << 20;
  posix.generation    = 77;
  posix.element_type  = e3ap_element_type::int8; // optional enum at its zero value
  posix.dimensions    = {14, 273, 12, 0};
  posix.byte_strides  = {1ULL << 40, 8};
  posix.layout_id     = 0;
  by_reference.by_reference = posix;
  auto posix_back           = round_trip(by_reference, ocudu_profile);
  ASSERT_TRUE(posix_back.by_reference);
  EXPECT_EQ(posix_back.by_reference->element_type, std::optional<e3ap_element_type>(e3ap_element_type::int8));
  EXPECT_EQ(posix_back.by_reference->dimensions, (std::vector<uint32_t>{14, 273, 12, 0}));
  EXPECT_EQ(std::get<e3ap_posix_shared_location>(posix_back.by_reference->location).row_index, 3);

  e3ap_accelerator_ipc_location ipc;
  ipc.device_ordinal     = 1;
  ipc.allocation_id      = 0xDEADBEEFCAFEULL;
  ipc.memory_handle[0]   = 7;
  ipc.ready_event_handle = std::array<uint8_t, 64>{}; // present, all zero
  ipc.byte_offset        = 0;
  auto gpu               = posix;
  gpu.memory_domain      = e3ap_memory_domain::accelerator_ipc_export;
  gpu.location           = ipc;
  gpu.lease_expiry_ns    = 123;
  by_reference.by_reference = gpu;
  auto gpu_back             = round_trip(by_reference, ocudu_profile);
  ASSERT_TRUE(gpu_back.by_reference);
  const auto& ipc_back = std::get<e3ap_accelerator_ipc_location>(gpu_back.by_reference->location);
  EXPECT_EQ(ipc_back.memory_handle[0], 7);
  EXPECT_TRUE(ipc_back.ready_event_handle);
  EXPECT_EQ(gpu_back.by_reference->lease_expiry_ns, std::optional<uint64_t>(123));

  e3ap_dapp_control_action control;
  control.dapp_identifier         = 101;
  control.ran_function_identifier = e3ap_ran_function_management;
  control.control_identifier      = e3ap_management_control_request;
  control.action_data             = {1};
  control.fragment_index          = 1;
  control.fragment_count          = 3;
  control.payload_bytes           = 150000;
  auto control_back               = round_trip(control, ocudu_profile);
  EXPECT_EQ(control_back.fragment_count, 3U);
  EXPECT_EQ(control_back.payload_bytes, std::optional<uint32_t>(150000));

  e3ap_dapp_report report;
  report.dapp_identifier         = 101;
  report.ran_function_identifier = 1;
  report.report_data             = {1};
  EXPECT_FALSE(e3ap_protobuf_codec::representable(report, ocudu_profile)) << "a libe3 root field in both profiles";
  report.sequence_identifier = 5;
  EXPECT_EQ(round_trip(report, ocudu_profile).sequence_identifier, std::optional<uint32_t>(5));

  e3ap_message_ack ack;
  ack.request_id    = 5;
  ack.response_code = e3ap_response_code::negative;
  ack.cause         = e3ap_cause::unspecified; // optional enum at its zero value
  ack.detail        = "no";
  auto ack_back     = round_trip(ack, ocudu_profile);
  EXPECT_EQ(ack_back.cause, std::optional<e3ap_cause>(e3ap_cause::unspecified));
  EXPECT_EQ(ack_back.detail, "no");

  e3ap_control_response outcome;
  outcome.request_id              = 6;
  outcome.dapp_identifier         = 101;
  outcome.ran_function_identifier = e3ap_ran_function_management;
  outcome.control_identifier      = e3ap_management_control_request;
  outcome.response_code           = e3ap_response_code::positive;
  outcome.control_outcome         = {4, 5, 6};
  outcome.payload_bytes           = 3;
  auto outcome_back               = round_trip(outcome, ocudu_profile);
  EXPECT_EQ(outcome_back.control_outcome, outcome.control_outcome);
  EXPECT_EQ(outcome_back.payload_bytes, 3U);
}

TEST(dapp_e3ap_protobuf_codec, ocudu_profile_bytes_read_as_the_libe3_message_under_the_libe3_profile)
{
  // What a libe3 peer sees of an OCUDU-profile PDU: the libe3 fields, extensions skipped.
  auto       response = full_setup_response();
  const auto superset = encode(pdu_of(response), ocudu_profile);
  const auto plain    = encode(pdu_of(response), libe3_profile);
  EXPECT_GT(superset.size(), plain.size());
  auto as_libe3 = e3ap_protobuf_codec::decode(superset, libe3_profile);
  ASSERT_TRUE(as_libe3) << as_libe3.detail;
  EXPECT_EQ(to_hex(encode(*as_libe3.value, libe3_profile)), to_hex(plain));
  // libe3 would put an OCUDU-only message in an empty oneof: refused, never misread.
  e3ap_control_response outcome;
  outcome.request_id              = 1;
  outcome.dapp_identifier         = 1;
  outcome.ran_function_identifier = 1;
  outcome.control_identifier      = 1;
  const auto only_ocudu           = encode(pdu_of(outcome), ocudu_profile);
  EXPECT_FALSE(e3ap_protobuf_codec::decode(only_ocudu, libe3_profile));
}

TEST(dapp_e3ap_protobuf_codec, refuses_values_the_libe3_profile_cannot_carry)
{
  std::string detail;
  e3ap_control_response outcome;
  EXPECT_FALSE(e3ap_protobuf_codec::representable(outcome, libe3_profile, &detail));
  EXPECT_NE(detail.find("MessageAck"), std::string::npos) << detail;

  e3ap_indication_message indication;
  indication.dapp_identifier         = 1;
  indication.ran_function_identifier = 1;
  indication.protocol_data           = std::vector<uint8_t>(32769, 1);
  EXPECT_FALSE(e3ap_protobuf_codec::representable(indication, libe3_profile)) << "over 32 KiB";
  EXPECT_FALSE(e3ap_protobuf_codec::representable(indication, ocudu_profile)) << "OCUDU fragments at libe3's 32 KiB";
  indication.protocol_data  = {1};
  indication.fragment_count = 2;
  EXPECT_FALSE(e3ap_protobuf_codec::representable(indication, libe3_profile)) << "fragmented";
  indication.fragment_count = 1;
  indication.by_reference.emplace();
  indication.by_reference->location = e3ap_posix_shared_location{"/r", 0, 0};
  EXPECT_FALSE(e3ap_protobuf_codec::representable(indication, libe3_profile)) << "by reference";
  indication.by_reference.reset();
  indication.dapp_identifier = 0;
  EXPECT_FALSE(e3ap_protobuf_codec::representable(indication, libe3_profile)) << "identifier 0";
  EXPECT_FALSE(e3ap_protobuf_codec::representable(indication, ocudu_profile)) << "identifier 0";

  e3ap_dapp_report report;
  report.dapp_identifier         = 1;
  report.ran_function_identifier = 1;
  report.report_data             = {1};
  EXPECT_FALSE(e3ap_protobuf_codec::representable(report, libe3_profile)) << "libe3 reports need a sequence id";

  e3ap_setup_request setup;
  setup.dapp_name    = std::string(65, 'n');
  setup.dapp_version = "1";
  setup.vendor       = "v";
  EXPECT_FALSE(e3ap_protobuf_codec::representable(setup, libe3_profile)) << "name over 64 bytes";
  setup.dapp_name    = "n";
  setup.dapp_version = "1.0.0-beta.1"; // 12 bytes: libe3's root E3-Version allows 11, in both profiles
  EXPECT_FALSE(e3ap_protobuf_codec::representable(setup, libe3_profile));
  EXPECT_FALSE(e3ap_protobuf_codec::representable(setup, ocudu_profile));
  setup.dapp_version = "1";
  setup.vendor       = std::string("\xc0\xaf", 2); // overlong UTF-8
  EXPECT_FALSE(e3ap_protobuf_codec::representable(setup, libe3_profile));

  e3ap_subscription_request subscription;
  subscription.dapp_identifier         = 1;
  subscription.ran_function_identifier = 1;
  subscription.periodicity             = 1001;
  EXPECT_FALSE(e3ap_protobuf_codec::representable(subscription, libe3_profile)) << "periodicity over 1000";
  EXPECT_TRUE(e3ap_protobuf_codec::representable(subscription, ocudu_profile));
  subscription.periodicity           = std::nullopt;
  subscription.telemetry_identifiers = std::vector<uint16_t>(1001, 1);
  EXPECT_FALSE(e3ap_protobuf_codec::representable(subscription, libe3_profile)) << "list over 1000";

  e3ap_message_ack ack;
  EXPECT_FALSE(e3ap_protobuf_codec::representable(ack, libe3_profile)) << "request id 0";
}

TEST(dapp_e3ap_protobuf_codec, decoder_accepts_any_valid_proto3_serialisation)
{
  // SubscriptionRequest written backwards, with an unpacked list, unknown fields of every wire
  // type, and a scalar repeated (last one wins).
  std::vector<uint8_t> body;
  put_varint(body, 6, 50);
  tag(body, 3, 0);
  varint(body, 9); // unpacked telemetry id
  tag(body, 3, 2);
  varint(body, 2);
  varint(body, 10);
  varint(body, 11); // packed continuation: lists merge
  put_varint(body, 2, 10);
  put_varint(body, 1, 99); // overwritten below
  put_varint(body, 1, 4);
  put_varint(body, 77, 1);           // unknown varint
  tag(body, 78, 1);                  // unknown fixed64
  body.insert(body.end(), 8, 0xFF);
  tag(body, 79, 5);                  // unknown fixed32
  body.insert(body.end(), 4, 0xEE);
  put_text(body, 80, "unknown");     // unknown length-delimited
  put_text(body, 100, "not bytes?"); // OCUDU subscription_data: skipped under libe3
  tag(body, 2, 2);                   // known field, wrong wire type: unknown to libprotobuf too
  varint(body, 0);
  std::vector<uint8_t> pdu;
  put_bytes(pdu, 5, body);
  put_varint(pdu, 2, 0); // timestamp 0 = absent
  put_varint(pdu, 1, 12);
  auto decoded = e3ap_protobuf_codec::decode(pdu, libe3_profile);
  ASSERT_TRUE(decoded) << decoded.detail;
  EXPECT_EQ(decoded.value->id, 12U);
  EXPECT_FALSE(decoded.value->timestamp_ns);
  const auto& request = std::get<e3ap_subscription_request>(decoded.value->message);
  EXPECT_EQ(request.dapp_identifier, 4);
  EXPECT_EQ(request.ran_function_identifier, 10);
  EXPECT_EQ(request.telemetry_identifiers, (std::vector<uint16_t>{9, 10, 11}));
  EXPECT_EQ(request.periodicity, std::optional<uint32_t>(50));
  EXPECT_TRUE(request.subscription_data.empty());
  // Under the OCUDU profile field 100 is subscription_data.
  auto superset = e3ap_protobuf_codec::decode(pdu, ocudu_profile);
  ASSERT_TRUE(superset) << superset.detail;
  EXPECT_EQ(std::get<e3ap_subscription_request>(superset.value->message).subscription_data.size(), 10U);

  // A oneof member repeated merges; a later different member replaces it.
  std::vector<uint8_t> first, second, merged;
  put_varint(first, 1, 3);
  put_varint(second, 2, 1);
  put_varint(merged, 1, 8);
  put_bytes(merged, 6, first);
  put_bytes(merged, 6, second);
  auto deleted = e3ap_protobuf_codec::decode(merged, libe3_profile);
  ASSERT_TRUE(deleted) << deleted.detail;
  EXPECT_EQ(std::get<e3ap_subscription_delete>(deleted.value->message).dapp_identifier, 3);
  EXPECT_EQ(std::get<e3ap_subscription_delete>(deleted.value->message).subscription_id, 1);
  put_bytes(merged, 12, first);
  auto released = e3ap_protobuf_codec::decode(merged, libe3_profile);
  ASSERT_TRUE(released) << released.detail;
  EXPECT_TRUE(std::holds_alternative<e3ap_release_message>(released.value->message));
}

TEST(dapp_e3ap_protobuf_codec, decoder_rejects_malformed_and_out_of_bound_input)
{
  const auto rejected = [](const std::vector<uint8_t>& bytes, const char* why) {
    for (const auto profile : {libe3_profile, ocudu_profile}) {
      auto decoded = e3ap_protobuf_codec::decode(bytes, profile);
      EXPECT_FALSE(decoded) << why;
    }
  };
  std::vector<uint8_t> release;
  put_varint(release, 1, 3);
  const auto valid = wrap(1, 12, release);
  ASSERT_TRUE(e3ap_protobuf_codec::decode(valid, libe3_profile));

  rejected({}, "empty: no message");
  rejected(std::vector<uint8_t>(valid.begin(), valid.end() - 1), "truncated");
  rejected(wrap(0, 12, release), "message id 0");
  rejected(wrap(1, 12, {}), "release without a dApp identifier");
  std::vector<uint8_t> big_id;
  put_varint(big_id, 1, 65536);
  rejected(wrap(1, 12, big_id), "dApp identifier above 65535");
  rejected({0x08, 0x01, 0x18}, "group / truncated tag");
  rejected({0x0b, 0x0c}, "group wire type");
  rejected({0x08, 0x81, 0x81, 0x81, 0x81, 0x81, 0x81, 0x81, 0x81, 0x81, 0x81, 0x01}, "11-byte varint");
  std::vector<uint8_t> ack;
  put_varint(ack, 1, 4);
  put_varint(ack, 2, 2); // ResponseCode 2: libe3 would read POSITIVE
  rejected(wrap(1, 13, ack), "unknown response code");
  std::vector<uint8_t> report;
  put_varint(report, 1, 1);
  put_varint(report, 2, 1);
  put_bytes(report, 3, {1});
  rejected(wrap(1, 10, report), "report without sequence");
  std::vector<uint8_t> setup;
  put_text(setup, 1, "1.0.0");
  put_text(setup, 2, "d");
  put_text(setup, 3, "1");
  put_bytes(setup, 4, {0xff});
  rejected(wrap(1, 3, setup), "invalid UTF-8 vendor");
  std::vector<uint8_t> indication;
  put_varint(indication, 1, 1);
  put_varint(indication, 2, 1);
  put_bytes(indication, 3, std::vector<uint8_t>(32769, 0));
  rejected(wrap(1, 8, indication), "over libe3's 32 KiB (OCUDU fragments larger payloads)");
}

TEST(dapp_e3ap_protobuf_codec, peek_and_setup_profile_read_routing_fields_without_decoding)
{
  e3ap_setup_request setup;
  setup.dapp_name    = "d";
  setup.dapp_version = "1";
  setup.vendor       = "v";
  EXPECT_EQ(e3ap_protobuf_codec::setup_profile(encode(pdu_of(setup), ocudu_profile)), std::optional(libe3_profile))
      << "the profile is the setup's ocudu_profile field, not the encoder's";
  setup.ocudu_profile = e3ap_ocudu_profile_version;
  EXPECT_EQ(e3ap_protobuf_codec::setup_profile(encode(pdu_of(setup), libe3_profile)), std::optional(libe3_profile))
      << "a libe3-profile encoder drops it";
  EXPECT_EQ(e3ap_protobuf_codec::setup_profile(encode(pdu_of(setup), ocudu_profile)), std::optional(ocudu_profile));
  EXPECT_FALSE(e3ap_protobuf_codec::setup_profile(encode(pdu_of(e3ap_release_message{2}), libe3_profile)));
  EXPECT_FALSE(e3ap_protobuf_codec::setup_profile(std::vector<uint8_t>{0x0b}));

  auto response = full_setup_response();
  auto view     = e3ap_protobuf_codec::peek(encode(pdu_of(response, 31), libe3_profile));
  ASSERT_TRUE(view);
  EXPECT_EQ(view->pdu_id, 31U);
  EXPECT_EQ(view->message, 4U);
  EXPECT_EQ(view->dapp_identifier, std::optional<uint16_t>(101));
  EXPECT_EQ(view->request_id, 1U);
  EXPECT_TRUE(view->positive);

  e3ap_message_ack ack;
  ack.request_id    = 77;
  ack.response_code = e3ap_response_code::negative;
  view              = e3ap_protobuf_codec::peek(encode(pdu_of(ack), libe3_profile));
  ASSERT_TRUE(view);
  EXPECT_FALSE(view->dapp_identifier);
  EXPECT_EQ(view->request_id, 77U);
  EXPECT_FALSE(view->positive);

  e3ap_dapp_report report;
  report.dapp_identifier         = 4000;
  report.ran_function_identifier = 1;
  report.report_data             = {1};
  report.sequence_identifier     = 1;
  view                           = e3ap_protobuf_codec::peek(encode(pdu_of(report), libe3_profile));
  ASSERT_TRUE(view);
  EXPECT_EQ(view->dapp_identifier, std::optional<uint16_t>(4000));
}

TEST(dapp_e3ap_protobuf_codec, mutated_input_never_crashes_and_accepted_input_is_canonicalised)
{
  std::mt19937                       random(20261002);
  std::vector<std::vector<uint8_t>> seeds;
  seeds.push_back(encode(pdu_of(full_setup_response()), ocudu_profile));
  seeds.push_back(encode(pdu_of(full_setup_response()), libe3_profile));
  e3ap_indication_message indication;
  indication.dapp_identifier         = 3;
  indication.ran_function_identifier = 10;
  indication.protocol_data           = {1, 2, 3, 4};
  indication.subscription_id         = 1;
  indication.by_reference.emplace();
  indication.by_reference->location = e3ap_accelerator_ipc_location{};
  seeds.push_back(encode(pdu_of(indication), ocudu_profile));
  for (int iteration = 0; iteration != 20000; ++iteration) {
    auto bytes = seeds[iteration % seeds.size()];
    for (int edit = 0, edits = 1 + static_cast<int>(random() % 4); edit != edits; ++edit) {
      const std::size_t at = random() % bytes.size();
      switch (random() % 3) {
        case 0:
          bytes[at] = static_cast<uint8_t>(random());
          break;
        case 1:
          bytes.erase(bytes.begin() + static_cast<std::ptrdiff_t>(at));
          break;
        default:
          bytes.insert(bytes.begin() + static_cast<std::ptrdiff_t>(at), static_cast<uint8_t>(random()));
          break;
      }
      if (bytes.empty()) {
        bytes.push_back(0);
      }
    }
    for (const auto profile : {libe3_profile, ocudu_profile}) {
      auto decoded = e3ap_protobuf_codec::decode(bytes, profile);
      static_cast<void>(e3ap_protobuf_codec::peek(bytes));
      static_cast<void>(e3ap_protobuf_codec::setup_profile(bytes));
      if (decoded) {
        // Whatever is accepted re-encodes, and that encoding is a fixed point.
        // The decoder accepts nothing the encoder would refuse (same bounds both ways).
        auto again = e3ap_protobuf_codec::encode(*decoded.value, profile);
        ASSERT_TRUE(again) << again.detail << " for accepted input " << to_hex(bytes);
        auto twice = e3ap_protobuf_codec::decode(*again.value, profile);
        ASSERT_TRUE(twice) << twice.detail;
        auto thrice = e3ap_protobuf_codec::encode(*twice.value, profile);
        ASSERT_TRUE(thrice);
        ASSERT_EQ(to_hex(*thrice.value), to_hex(*again.value));
      }
    }
  }
}
