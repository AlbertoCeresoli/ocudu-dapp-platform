// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/e3/e3ap_protobuf_codec.h"
#include "utf8.h"
#include <cstring>
#include <stdexcept>
#include <type_traits>

using namespace ocudu::dapp;
using namespace ocudu::dapp::e3;

namespace {

// ---------------------------------------------------------------------------
// Wire format
// ---------------------------------------------------------------------------

constexpr uint8_t wire_varint = 0;
constexpr uint8_t wire_fixed64 = 1;
constexpr uint8_t wire_length = 2;
constexpr uint8_t wire_fixed32 = 5;

/// First OCUDU field number in every message (libe3 owns 1..99).
constexpr uint32_t first_ocudu_field = 100;

namespace field {
// E3Pdu
constexpr uint32_t pdu_id                    = 1;
constexpr uint32_t pdu_timestamp             = 2;
constexpr uint32_t setup_request             = 3;
constexpr uint32_t setup_response            = 4;
constexpr uint32_t subscription_request      = 5;
constexpr uint32_t subscription_delete       = 6;
constexpr uint32_t subscription_response     = 7;
constexpr uint32_t indication_message        = 8;
constexpr uint32_t dapp_control_action       = 9;
constexpr uint32_t dapp_report               = 10;
constexpr uint32_t xapp_control_action       = 11;
constexpr uint32_t release_message           = 12;
constexpr uint32_t message_ack               = 13;
constexpr uint32_t control_response          = 100;
// SetupRequest.ocudu_profile (also SetupResponse)
constexpr uint32_t ocudu_profile             = 100;
} // namespace field

/// Bounds of a profile: E3AP-OCUDU-V2's, whose root is libe3's module (so the two profiles share
/// every root bound and differ only in what the OCUDU additions allow).
struct limits {
  std::size_t version;
  std::size_t name;
  std::size_t text;
  /// Payload fragments and RAN function data (libe3's 32 KiB OCTET STRING bound).
  std::size_t payload;
  /// subscriptionData (an OCUDU addition).
  std::size_t subscription_data;
  std::size_t list;
  std::size_t ran_functions;
  uint32_t    subscription_time;
  uint32_t    periodicity;
};

constexpr limits libe3_limits{11, 64, 0, 32768, 0, 1000, 255, 3600, 1000};
/// The wider subscription time and periodicity are E3AP-OCUDU-V2's subscriptionTimeLong/periodicityLong.
constexpr limits ocudu_limits{11, 64, 512, e3ap_maximum_fragment_bytes, e3ap_maximum_function_data, 1000, 255, 86400,
                              60000};

[[noreturn]] void fail(const std::string& what)
{
  throw std::runtime_error(what);
}

constexpr std::size_t varint_size(uint64_t value) noexcept
{
  std::size_t size = 1;
  while (value >= 0x80) {
    value >>= 7;
    ++size;
  }
  return size;
}

/// Sizing pass: counts the bytes a message body serialises to.
struct size_sink {
  std::size_t size{};
  void        varint(uint64_t value) noexcept { size += varint_size(value); }
  void        raw(const uint8_t* data, std::size_t length) noexcept
  {
    static_cast<void>(data);
    size += length;
  }
};

/// Writing pass into a buffer sized by the sizing pass.
struct write_sink {
  uint8_t* cursor;
  void     varint(uint64_t value) noexcept
  {
    while (value >= 0x80) {
      *cursor++ = static_cast<uint8_t>(value | 0x80);
      value >>= 7;
    }
    *cursor++ = static_cast<uint8_t>(value);
  }
  void raw(const uint8_t* data, std::size_t length) noexcept
  {
    if (length != 0) {
      std::memcpy(cursor, data, length);
      cursor += length;
    }
  }
};

template <typename Sink>
void put_tag(Sink& sink, uint32_t number, uint8_t wire_type)
{
  sink.varint((uint64_t{number} << 3) | wire_type);
}

/// Any varint-encoded scalar, written even when zero (explicit presence).
template <typename Sink>
void put_varint(Sink& sink, uint32_t number, uint64_t value)
{
  put_tag(sink, number, wire_varint);
  sink.varint(value);
}

/// Implicit-presence scalar: omitted at its default.
template <typename Sink>
void put_scalar(Sink& sink, uint32_t number, uint64_t value)
{
  if (value != 0) {
    put_varint(sink, number, value);
  }
}

template <typename Sink, typename T>
void put_optional(Sink& sink, uint32_t number, const std::optional<T>& value)
{
  if (value) {
    put_varint(sink, number, static_cast<uint64_t>(*value));
  }
}

/// Length-delimited bytes, written even when empty (explicit presence).
template <typename Sink>
void put_length_delimited(Sink& sink, uint32_t number, const void* data, std::size_t length)
{
  put_tag(sink, number, wire_length);
  sink.varint(length);
  sink.raw(static_cast<const uint8_t*>(data), length);
}

/// Implicit-presence bytes/string: omitted when empty.
template <typename Sink>
void put_bytes(Sink& sink, uint32_t number, const void* data, std::size_t length)
{
  if (length != 0) {
    put_length_delimited(sink, number, data, length);
  }
}

template <typename Sink>
void put_bytes(Sink& sink, uint32_t number, const std::vector<uint8_t>& value)
{
  put_bytes(sink, number, value.data(), value.size());
}

template <typename Sink>
void put_string(Sink& sink, uint32_t number, const std::string& value)
{
  put_bytes(sink, number, value.data(), value.size());
}

/// Packed repeated varints (omitted when empty).
template <typename Sink, typename T>
void put_packed(Sink& sink, uint32_t number, const std::vector<T>& values)
{
  if (values.empty()) {
    return;
  }
  std::size_t length = 0;
  for (const T value : values) {
    length += varint_size(static_cast<uint64_t>(value));
  }
  put_tag(sink, number, wire_length);
  sink.varint(length);
  for (const T value : values) {
    sink.varint(static_cast<uint64_t>(value));
  }
}

/// Embedded message: \p body is called once to size it and once more to write it.
template <typename Sink, typename Body>
void put_message(Sink& sink, uint32_t number, const Body& body)
{
  size_sink sizer;
  body(sizer);
  put_tag(sink, number, wire_length);
  sink.varint(sizer.size);
  if constexpr (!std::is_same_v<Sink, size_sink>) {
    body(sink);
  } else {
    sink.size += sizer.size;
  }
}

// ---------------------------------------------------------------------------
// Encode-side checks
// ---------------------------------------------------------------------------

struct encode_context {
  e3ap_profile profile;
  const limits&         bounds;
  bool                  ocudu() const noexcept { return profile == e3ap_profile::ocudu; }
  const char*           module() const noexcept { return ocudu() ? "E3AP-OCUDU-V2" : "libe3"; }
};

uint32_t identifier(const encode_context& context, uint64_t value, const char* what)
{
  if (value < 1 || value > 65535) {
    fail(std::string(what) + " " + std::to_string(value) + " is outside " + context.module() + "'s 1..65535");
  }
  return static_cast<uint32_t>(value);
}

uint32_t message_id(const encode_context& context, uint32_t value, const char* what = "message id")
{
  if (value < 1) {
    fail(std::string(what) + " 0 is outside " + context.module() + "'s 1..4294967295");
  }
  return value;
}

void check_text(const std::string& value, std::size_t minimum, std::size_t maximum, const char* what)
{
  if (value.size() < minimum || value.size() > maximum) {
    fail(std::string(what) + " must be " + std::to_string(minimum) + ".." + std::to_string(maximum) + " bytes");
  }
  if (!valid_utf8(reinterpret_cast<const uint8_t*>(value.data()), value.size())) {
    fail(std::string(what) + " is not valid UTF-8");
  }
}

void check_payload(const std::vector<uint8_t>& value, std::size_t maximum, const char* what)
{
  if (value.empty() || value.size() > maximum) {
    fail(std::string(what) + " must be 1.." + std::to_string(maximum) + " bytes");
  }
}

/// libe3 drops identifiers it cannot name (as e3ap_libe3_codec does); OCUDU lists must be valid.
std::vector<uint16_t>
identifier_list(const encode_context& context, const std::vector<uint16_t>& values, const char* what)
{
  std::vector<uint16_t> result;
  result.reserve(values.size());
  for (const uint16_t value : values) {
    if (value == 0) {
      if (context.ocudu()) {
        fail(std::string(what) + " 0 is outside 1..65535");
      }
      continue;
    }
    result.push_back(value);
  }
  if (result.size() > context.bounds.list) {
    fail(std::string(what) + " list exceeds " + context.module() + "'s " + std::to_string(context.bounds.list) +
         " entries");
  }
  return result;
}

void require_whole(const encode_context& context, uint16_t fragment_index, uint32_t fragment_count, const char* what)
{
  if (!context.ocudu() && (fragment_index != 0 || fragment_count != 1)) {
    fail(std::string(what) + " is fragmented; libe3 carries whole payloads only");
  }
  if (fragment_count < 1 || fragment_count > 65536 || fragment_index >= fragment_count) {
    fail(std::string(what) + " fragment " + std::to_string(fragment_index) + " of " + std::to_string(fragment_count) +
         " is invalid");
  }
}

void check_cause(const std::optional<e3ap_cause>& cause)
{
  if (cause && static_cast<uint8_t>(*cause) > static_cast<uint8_t>(e3ap_cause::shutting_down)) {
    fail("unknown cause");
  }
}

uint64_t response_code(e3ap_response_code code)
{
  return code == e3ap_response_code::positive ? 0 : 1;
}

// ---------------------------------------------------------------------------
// Messages (encode)
// ---------------------------------------------------------------------------

template <typename Sink>
void put_body(Sink& sink, const encode_context& context, const e3ap_setup_request& source)
{
  check_text(source.e3ap_protocol_version, 1, context.bounds.version, "e3apProtocolVersion");
  check_text(source.dapp_name, 1, context.bounds.name, "dAppName");
  check_text(source.dapp_version, 1, context.bounds.version, "dAppVersion");
  check_text(source.vendor, 1, context.bounds.name, "vendor");
  put_string(sink, 1, source.e3ap_protocol_version);
  put_string(sink, 2, source.dapp_name);
  put_string(sink, 3, source.dapp_version);
  put_string(sink, 4, source.vendor);
  if (context.ocudu()) {
    put_optional(sink, field::ocudu_profile, source.ocudu_profile);
    if (source.dapp_instance) {
      put_bytes(sink, 101, source.dapp_instance->bytes.data(), source.dapp_instance->bytes.size());
    }
  }
}

template <typename Sink>
void put_body(Sink& sink, const encode_context& context, const e3ap_ran_function_definition& source)
{
  const auto telemetry = identifier_list(context, source.telemetry_identifiers, "telemetry identifier");
  const auto control   = identifier_list(context, source.control_identifiers, "control identifier");
  check_payload(source.ran_function_data, context.bounds.payload, "ranFunctionData");
  put_scalar(sink, 1, identifier(context, source.ran_function_identifier, "RAN function identifier"));
  put_packed(sink, 2, telemetry);
  put_packed(sink, 3, control);
  put_bytes(sink, 4, source.ran_function_data);
  if (context.ocudu()) {
    check_text(source.ran_function_name, 0, context.bounds.name, "ranFunctionName");
    check_text(source.ran_function_version, 0, context.bounds.version, "ranFunctionVersion");
    put_string(sink, 100, source.ran_function_name);
    put_string(sink, 101, source.ran_function_version);
  }
}

template <typename Sink>
void put_body(Sink& sink, const encode_context& context, const e3ap_setup_response& source)
{
  check_text(source.ran_identifier, 1, context.bounds.name, "ranIdentifier");
  if (source.ran_functions.size() > context.bounds.ran_functions) {
    fail("RAN function list exceeds its bound");
  }
  put_scalar(sink, 1, message_id(context, source.request_id, "request id"));
  put_scalar(sink, 2, response_code(source.response_code));
  if (source.e3ap_protocol_version) {
    check_text(*source.e3ap_protocol_version, 1, context.bounds.version, "e3apProtocolVersion");
    put_length_delimited(sink, 3, source.e3ap_protocol_version->data(), source.e3ap_protocol_version->size());
  }
  if (source.dapp_identifier) {
    put_varint(sink, 4, identifier(context, *source.dapp_identifier, "dApp identifier"));
  }
  put_string(sink, 5, source.ran_identifier);
  for (const auto& function : source.ran_functions) {
    put_message(sink, 6, [&](auto& inner) { put_body(inner, context, function); });
  }
  if (!context.ocudu()) {
    return;
  }
  check_cause(source.cause);
  check_text(source.detail, 0, context.bounds.text, "detail");
  put_optional(sink, field::ocudu_profile, source.ocudu_profile);
  put_optional(sink, 101, source.cause);
  put_string(sink, 102, source.detail);
  if (source.ran_instance) {
    put_bytes(sink, 103, source.ran_instance->bytes.data(), source.ran_instance->bytes.size());
  }
  if (source.maximum_payload_bytes &&
      (*source.maximum_payload_bytes < 1 || *source.maximum_payload_bytes > e3ap_maximum_payload_bytes)) {
    fail("maximumPayloadBytes is outside its bound");
  }
  if (source.maximum_fragment_bytes &&
      (*source.maximum_fragment_bytes < 1 || *source.maximum_fragment_bytes > e3ap_maximum_fragment_bytes)) {
    fail("maximumFragmentBytes is outside its bound");
  }
  put_optional(sink, 104, source.maximum_payload_bytes);
  put_optional(sink, 105, source.maximum_fragment_bytes);
  put_scalar(sink, 106, source.ran_functions_present || !source.ran_functions.empty() ? 1 : 0);
}

template <typename Sink>
void put_body(Sink& sink, const encode_context& context, const e3ap_subscription_request& source)
{
  const auto telemetry = identifier_list(context, source.telemetry_identifiers, "telemetry identifier");
  const auto control   = identifier_list(context, source.control_identifiers, "control identifier");
  if (source.subscription_time_s && *source.subscription_time_s > context.bounds.subscription_time) {
    fail("subscription time exceeds " + std::string(context.module()) + "'s bound");
  }
  if (source.periodicity_ms && *source.periodicity_ms > context.bounds.periodicity) {
    fail("periodicity exceeds " + std::string(context.module()) + "'s bound");
  }
  put_scalar(sink, 1, identifier(context, source.dapp_identifier, "dApp identifier"));
  put_scalar(sink, 2, identifier(context, source.ran_function_identifier, "RAN function identifier"));
  put_packed(sink, 3, telemetry);
  put_packed(sink, 4, control);
  put_optional(sink, 5, source.subscription_time_s);
  put_optional(sink, 6, source.periodicity_ms);
  if (!context.ocudu()) {
    return;
  }
  if (source.subscription_data.size() > context.bounds.subscription_data) {
    fail("subscriptionData exceeds its bound");
  }
  if (source.minimum_by_reference_bytes && *source.minimum_by_reference_bytes > e3ap_maximum_payload_bytes) {
    fail("minimumByReferenceBytes exceeds its bound");
  }
  put_bytes(sink, 100, source.subscription_data);
  put_optional(sink, 101, source.delivery_mode);
  put_optional(sink, 102, source.minimum_by_reference_bytes);
}

template <typename Sink>
void put_body(Sink& sink, const encode_context& context, const e3ap_subscription_delete& source)
{
  put_scalar(sink, 1, identifier(context, source.dapp_identifier, "dApp identifier"));
  put_scalar(sink, 2, identifier(context, source.subscription_id, "subscription identifier"));
}

template <typename Sink>
void put_body(Sink& sink, const e3ap_shared_region& source)
{
  check_text(source.region_name, 1, ocudu_limits.name, "regionName");
  if (source.nof_rows < 1 || source.row_bytes == 0) {
    fail("shared region needs at least one non-empty row");
  }
  put_scalar(sink, 1, static_cast<uint64_t>(source.memory_domain));
  put_string(sink, 2, source.region_name);
  put_scalar(sink, 3, source.region_bytes);
  put_scalar(sink, 4, source.header_bytes);
  put_scalar(sink, 5, source.row_bytes);
  put_scalar(sink, 6, source.nof_rows);
  put_scalar(sink, 7, source.layout_version);
}

template <typename Sink>
void put_body(Sink& sink, const e3ap_tensor_descriptor& source)
{
  if (source.dimensions.size() > 8 || source.byte_strides.size() > 8) {
    fail("tensor rank exceeds 8");
  }
  put_scalar(sink, 1, static_cast<uint64_t>(source.memory_domain));
  if (const auto* posix = std::get_if<e3ap_posix_shared_location>(&source.location)) {
    check_text(posix->region_name, 1, ocudu_limits.name, "regionName");
    put_message(sink, 2, [&](auto& inner) {
      put_string(inner, 1, posix->region_name);
      put_scalar(inner, 2, posix->row_index);
      put_scalar(inner, 3, posix->byte_offset);
    });
  } else {
    const auto& ipc = std::get<e3ap_accelerator_ipc_location>(source.location);
    put_message(sink, 3, [&](auto& inner) {
      put_scalar(inner, 1, ipc.device_ordinal);
      put_scalar(inner, 2, ipc.allocation_id);
      put_bytes(inner, 3, ipc.memory_handle.data(), ipc.memory_handle.size());
      if (ipc.ready_event_handle) {
        put_length_delimited(inner, 4, ipc.ready_event_handle->data(), ipc.ready_event_handle->size());
      }
      put_scalar(inner, 5, ipc.byte_offset);
    });
  }
  put_scalar(sink, 4, source.byte_length);
  put_scalar(sink, 5, source.generation);
  put_optional(sink, 6, source.element_type);
  put_packed(sink, 7, source.dimensions);
  put_packed(sink, 8, source.byte_strides);
  put_optional(sink, 9, source.layout_id);
  put_optional(sink, 10, source.lease_expiry_ns);
}

template <typename Sink>
void put_body(Sink& sink, const encode_context& context, const e3ap_subscription_response& source)
{
  put_scalar(sink, 1, message_id(context, source.request_id, "request id"));
  put_scalar(sink, 2, identifier(context, source.dapp_identifier, "dApp identifier"));
  put_scalar(sink, 3, response_code(source.response_code));
  if (source.subscription_id) {
    put_varint(sink, 4, identifier(context, *source.subscription_id, "subscription identifier"));
  }
  if (!context.ocudu()) {
    // libe3's Protobuf SubscriptionResponse has no identifier lists.
    return;
  }
  check_cause(source.cause);
  check_text(source.detail, 0, context.bounds.text, "detail");
  if (source.telemetry_identifiers) {
    const auto list = identifier_list(context, *source.telemetry_identifiers, "telemetry identifier");
    put_message(sink, 100, [&](auto& inner) { put_packed(inner, 1, list); });
  }
  if (source.control_identifiers) {
    const auto list = identifier_list(context, *source.control_identifiers, "control identifier");
    put_message(sink, 101, [&](auto& inner) { put_packed(inner, 1, list); });
  }
  put_optional(sink, 102, source.cause);
  put_string(sink, 103, source.detail);
  if (source.shared_region) {
    put_message(sink, 104, [&](auto& inner) { put_body(inner, *source.shared_region); });
  }
}

void check_payload_bytes(const std::optional<uint32_t>& value)
{
  if (value && (*value < 1 || *value > e3ap_maximum_payload_bytes)) {
    fail("payloadBytes is outside its bound");
  }
}

template <typename Sink>
void put_body(Sink& sink, const encode_context& context, const e3ap_indication_message& source)
{
  require_whole(context, source.fragment_index, source.fragment_count, "indication");
  if (source.by_reference && !context.ocudu()) {
    fail("by-reference indications cannot be expressed in the libe3 module");
  }
  check_payload(source.protocol_data, context.bounds.payload, "protocolData");
  put_scalar(sink, 1, identifier(context, source.dapp_identifier, "dApp identifier"));
  put_scalar(sink, 2, identifier(context, source.ran_function_identifier, "RAN function identifier"));
  put_bytes(sink, 3, source.protocol_data);
  if (!context.ocudu()) {
    return;
  }
  check_payload_bytes(source.payload_bytes);
  if (source.subscription_id) {
    put_varint(sink, 100, identifier(context, *source.subscription_id, "subscription identifier"));
  }
  if (source.telemetry_identifier) {
    put_varint(sink, 101, identifier(context, *source.telemetry_identifier, "telemetry identifier"));
  }
  put_optional(sink, 102, source.sequence);
  put_scalar(sink, 103, source.fragment_index);
  if (source.fragment_count != 1) {
    put_varint(sink, 104, source.fragment_count);
  }
  put_optional(sink, 105, source.payload_bytes);
  if (source.by_reference) {
    put_message(sink, 106, [&](auto& inner) { put_body(inner, *source.by_reference); });
  }
}

template <typename Sink>
void put_body(Sink& sink, const encode_context& context, const e3ap_dapp_control_action& source)
{
  require_whole(context, source.fragment_index, source.fragment_count, "control action");
  check_payload(source.action_data, context.bounds.payload, "actionData");
  put_scalar(sink, 1, identifier(context, source.dapp_identifier, "dApp identifier"));
  put_scalar(sink, 2, identifier(context, source.ran_function_identifier, "RAN function identifier"));
  put_scalar(sink, 3, identifier(context, source.control_identifier, "control identifier"));
  put_bytes(sink, 4, source.action_data);
  if (source.sequence_identifier) {
    put_scalar(sink, 5, message_id(context, *source.sequence_identifier, "sequence identifier"));
  }
  if (!context.ocudu()) {
    return;
  }
  check_payload_bytes(source.payload_bytes);
  put_scalar(sink, 100, source.fragment_index);
  if (source.fragment_count != 1) {
    put_varint(sink, 101, source.fragment_count);
  }
  put_optional(sink, 102, source.payload_bytes);
}

/// E3-SequenceID is a mandatory libe3 root field of reports and xApp controls, in both profiles.
uint32_t sequence(const encode_context& context, const std::optional<uint32_t>& value, const char* what)
{
  if (!value) {
    fail(std::string(what) + " needs a sequence identifier");
  }
  return message_id(context, *value, "sequence identifier");
}

template <typename Sink>
void put_body(Sink& sink, const encode_context& context, const e3ap_dapp_report& source)
{
  check_payload(source.report_data, context.bounds.payload, "reportData");
  put_scalar(sink, 1, identifier(context, source.dapp_identifier, "dApp identifier"));
  put_scalar(sink, 2, identifier(context, source.ran_function_identifier, "RAN function identifier"));
  put_bytes(sink, 3, source.report_data);
  put_scalar(sink, 4, sequence(context, source.sequence_identifier, "E3-DAppReport"));
}

template <typename Sink>
void put_body(Sink& sink, const encode_context& context, const e3ap_xapp_control_action& source)
{
  check_payload(source.xapp_control_data, context.bounds.payload, "xAppControlData");
  put_scalar(sink, 1, identifier(context, source.dapp_identifier, "dApp identifier"));
  put_scalar(sink, 2, identifier(context, source.ran_function_identifier, "RAN function identifier"));
  put_bytes(sink, 3, source.xapp_control_data);
  put_scalar(sink, 4, sequence(context, source.sequence_identifier, "E3-XAppControlAction"));
}

template <typename Sink>
void put_body(Sink& sink, const encode_context& context, const e3ap_release_message& source)
{
  put_scalar(sink, 1, identifier(context, source.dapp_identifier, "dApp identifier"));
}

template <typename Sink>
void put_body(Sink& sink, const encode_context& context, const e3ap_message_ack& source)
{
  put_scalar(sink, 1, message_id(context, source.request_id, "request id"));
  put_scalar(sink, 2, response_code(source.response_code));
  if (context.ocudu()) {
    check_cause(source.cause);
    check_text(source.detail, 0, context.bounds.text, "detail");
    put_optional(sink, 100, source.cause);
    put_string(sink, 101, source.detail);
  }
}

template <typename Sink>
void put_body(Sink& sink, const encode_context& context, const e3ap_control_response& source)
{
  if (!context.ocudu()) {
    fail("E3-ControlResponse has no counterpart in the libe3 module; answer with E3-MessageAck");
  }
  check_cause(source.cause);
  check_text(source.detail, 0, context.bounds.text, "detail");
  require_whole(context, source.fragment_index, source.fragment_count, "control response");
  if (source.control_outcome.size() > context.bounds.payload) {
    fail("controlOutcome exceeds its bound");
  }
  if (source.payload_bytes > e3ap_maximum_payload_bytes) {
    fail("payloadBytes is outside its bound");
  }
  put_scalar(sink, 1, message_id(context, source.request_id, "request id"));
  put_scalar(sink, 2, identifier(context, source.dapp_identifier, "dApp identifier"));
  put_scalar(sink, 3, identifier(context, source.ran_function_identifier, "RAN function identifier"));
  put_scalar(sink, 4, identifier(context, source.control_identifier, "control identifier"));
  put_scalar(sink, 5, response_code(source.response_code));
  put_optional(sink, 6, source.cause);
  put_string(sink, 7, source.detail);
  put_bytes(sink, 8, source.control_outcome);
  put_scalar(sink, 9, source.fragment_index);
  if (source.fragment_count != 1) {
    put_varint(sink, 10, source.fragment_count);
  }
  put_scalar(sink, 11, source.payload_bytes);
}

template <typename T>
constexpr uint32_t message_field() noexcept
{
  if constexpr (std::is_same_v<T, e3ap_setup_request>) {
    return field::setup_request;
  } else if constexpr (std::is_same_v<T, e3ap_setup_response>) {
    return field::setup_response;
  } else if constexpr (std::is_same_v<T, e3ap_subscription_request>) {
    return field::subscription_request;
  } else if constexpr (std::is_same_v<T, e3ap_subscription_delete>) {
    return field::subscription_delete;
  } else if constexpr (std::is_same_v<T, e3ap_subscription_response>) {
    return field::subscription_response;
  } else if constexpr (std::is_same_v<T, e3ap_indication_message>) {
    return field::indication_message;
  } else if constexpr (std::is_same_v<T, e3ap_dapp_control_action>) {
    return field::dapp_control_action;
  } else if constexpr (std::is_same_v<T, e3ap_dapp_report>) {
    return field::dapp_report;
  } else if constexpr (std::is_same_v<T, e3ap_xapp_control_action>) {
    return field::xapp_control_action;
  } else if constexpr (std::is_same_v<T, e3ap_release_message>) {
    return field::release_message;
  } else if constexpr (std::is_same_v<T, e3ap_message_ack>) {
    return field::message_ack;
  } else {
    static_assert(std::is_same_v<T, e3ap_control_response>);
    return field::control_response;
  }
}

template <typename Sink>
void put_pdu(Sink& sink, const encode_context& context, const e3ap_pdu& pdu)
{
  put_scalar(sink, field::pdu_id, message_id(context, pdu.id));
  if (pdu.timestamp_ns) {
    put_scalar(sink, field::pdu_timestamp, static_cast<uint64_t>(*pdu.timestamp_ns));
  }
  std::visit(
      [&](const auto& message) {
        using message_type = std::decay_t<decltype(message)>;
        put_message(sink, message_field<message_type>(), [&](auto& inner) { put_body(inner, context, message); });
      },
      pdu.message);
}

// ---------------------------------------------------------------------------
// Wire reader
// ---------------------------------------------------------------------------

struct reader {
  const uint8_t* cursor;
  const uint8_t* end;

  explicit reader(ocudu::span<const uint8_t> bytes) : cursor(bytes.data()), end(bytes.data() + bytes.size()) {}

  bool done() const noexcept { return cursor == end; }

  uint64_t varint()
  {
    uint64_t value = 0;
    for (unsigned shift = 0; shift < 64; shift += 7) {
      if (cursor == end) {
        fail("truncated Protobuf varint");
      }
      const uint8_t byte = *cursor++;
      if (shift == 63 && byte > 1) {
        fail("Protobuf varint overflows 64 bits");
      }
      value |= uint64_t{byte & 0x7fU} << shift;
      if ((byte & 0x80) == 0) {
        return value;
      }
    }
    fail("Protobuf varint is longer than 10 bytes");
  }

  /// Next field: number and wire type.
  std::pair<uint32_t, uint8_t> tag()
  {
    const uint64_t key  = varint();
    const uint64_t number = key >> 3;
    const auto     type   = static_cast<uint8_t>(key & 7);
    if (number < 1 || number > 536870911) {
      fail("invalid Protobuf field number");
    }
    return {static_cast<uint32_t>(number), type};
  }

  ocudu::span<const uint8_t> length_delimited()
  {
    const uint64_t length = varint();
    if (length > static_cast<uint64_t>(end - cursor)) {
      fail("truncated Protobuf length-delimited field");
    }
    ocudu::span<const uint8_t> result(cursor, static_cast<std::size_t>(length));
    cursor += length;
    return result;
  }

  void skip_bytes(std::size_t count)
  {
    if (count > static_cast<std::size_t>(end - cursor)) {
      fail("truncated Protobuf field");
    }
    cursor += count;
  }

  /// Skips a field this profile does not read (libprotobuf keeps it as unknown).
  void skip(uint8_t type)
  {
    switch (type) {
      case wire_varint:
        varint();
        return;
      case wire_fixed64:
        skip_bytes(8);
        return;
      case wire_length:
        length_delimited();
        return;
      case wire_fixed32:
        skip_bytes(4);
        return;
      default:
        // Groups (3, 4) do not exist in proto3; 6 and 7 are not wire types.
        fail("unsupported Protobuf wire type " + std::to_string(type));
    }
  }
};

/// Decodes one message body: \p on_field(number, type, reader) returns false for fields it does
/// not read, which are then skipped. A known field arriving with an unexpected wire type is unknown
/// to libprotobuf too, so the handlers check the type themselves.
template <typename Handler>
void read_message(ocudu::span<const uint8_t> bytes, const Handler& on_field)
{
  reader input(bytes);
  while (!input.done()) {
    const auto [number, type] = input.tag();
    if (!on_field(number, type, input)) {
      input.skip(type);
    }
  }
}

/// Repeated varint scalars, packed or not; values are appended (repeated fields merge).
template <typename T>
bool read_repeated(uint8_t type, reader& input, std::vector<T>& destination, uint64_t maximum, const char* what)
{
  const auto check = [&](uint64_t value) {
    if constexpr (std::is_same_v<T, uint32_t> || std::is_same_v<T, uint16_t>) {
      value = static_cast<uint32_t>(value); // proto uint32 keeps the low 32 bits
    }
    if (value > maximum) {
      fail(std::string(what) + " " + std::to_string(value) + " is outside its bound");
    }
    destination.push_back(static_cast<T>(value));
  };
  if (type == wire_varint) {
    check(input.varint());
    return true;
  }
  if (type == wire_length) {
    reader packed(input.length_delimited());
    while (!packed.done()) {
      check(packed.varint());
    }
    return true;
  }
  return false;
}

struct decode_context {
  e3ap_profile profile;
  const limits&         bounds;
  bool                  ocudu() const noexcept { return profile == e3ap_profile::ocudu; }
  const char*           module() const noexcept { return ocudu() ? "E3AP-OCUDU-V2" : "libe3"; }
  /// Field numbers this profile reads (libe3 skips OCUDU additions as unknown fields).
  bool                  reads(uint32_t number) const noexcept { return number < first_ocudu_field || ocudu(); }
};

uint32_t read_uint32(reader& input)
{
  return static_cast<uint32_t>(input.varint());
}

uint16_t checked_identifier(const decode_context& context, uint64_t value, const char* what)
{
  if (value < 1 || value > 65535) {
    fail(std::string(what) + " " + std::to_string(value) + " is outside " + context.module() + "'s 1..65535");
  }
  return static_cast<uint16_t>(value);
}

uint32_t checked_message_id(const decode_context& context, uint32_t value, const char* what = "message id")
{
  if (value < 1) {
    fail(std::string(what) + " is missing (0 is outside " + context.module() + "'s 1..4294967295)");
  }
  return value;
}

std::string read_text(reader& input, std::size_t maximum, const char* what)
{
  const auto bytes = input.length_delimited();
  if (bytes.size() > maximum) {
    fail(std::string(what) + " exceeds " + std::to_string(maximum) + " bytes");
  }
  if (!valid_utf8(bytes.data(), bytes.size())) {
    fail(std::string(what) + " is not valid UTF-8");
  }
  return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

void require_text(const std::string& value, const char* what)
{
  if (value.empty()) {
    fail(std::string(what) + " is missing");
  }
}

std::vector<uint8_t> read_bytes(reader& input, std::size_t maximum, const char* what)
{
  const auto bytes = input.length_delimited();
  if (bytes.size() > maximum) {
    fail(std::string(what) + " exceeds " + std::to_string(maximum) + " bytes");
  }
  return std::vector<uint8_t>(bytes.begin(), bytes.end());
}

void require_payload(const std::vector<uint8_t>& value, const char* what)
{
  if (value.empty()) {
    fail(std::string(what) + " must not be empty");
  }
}

e3ap_response_code read_response_code(reader& input)
{
  // proto3 enums are open; libe3 maps unknown values to POSITIVE, which would turn a garbled
  // rejection into an acceptance, so they are refused here.
  const uint64_t value = input.varint();
  if (value == 0) {
    return e3ap_response_code::positive;
  }
  if (value == 1) {
    return e3ap_response_code::negative;
  }
  fail("unknown response code " + std::to_string(static_cast<int32_t>(value)));
}

template <typename Enum>
Enum read_enum(reader& input, Enum last, const char* what)
{
  const uint64_t value = input.varint();
  if (value > static_cast<uint64_t>(last)) {
    fail(std::string("unknown ") + what + " " + std::to_string(static_cast<int32_t>(value)));
  }
  return static_cast<Enum>(value);
}

/// 16-byte instance identity; empty bytes are the proto3 default (no identity).
std::optional<management::identifier> read_instance(reader& input, const char* what)
{
  const auto bytes = input.length_delimited();
  if (bytes.empty()) {
    return std::nullopt;
  }
  if (bytes.size() != 16) {
    fail(std::string(what) + " must be 16 bytes");
  }
  management::identifier result;
  std::memcpy(result.bytes.data(), bytes.data(), 16);
  return result;
}

/// SetupRequest/SetupResponse.ocudu_profile: 1..255 (E3AP-OCUDU-V2 INTEGER (1..255)).
uint8_t read_profile(reader& input)
{
  const uint32_t value = read_uint32(input);
  if (value < 1 || value > 255) {
    fail("ocudu_profile " + std::to_string(value) + " is outside 1..255");
  }
  return static_cast<uint8_t>(value);
}

uint16_t read_u16(reader& input, const char* what, uint32_t minimum = 0)
{
  const uint32_t value = read_uint32(input);
  if (value < minimum || value > 65535) {
    fail(std::string(what) + " " + std::to_string(value) + " is outside its bound");
  }
  return static_cast<uint16_t>(value);
}

/// Fragment bookkeeping (fragment_count absent means 1).
void check_fragments(uint16_t index, uint32_t count, const char* what)
{
  if (count < 1 || count > 65536 || index >= count) {
    fail(std::string(what) + " fragment " + std::to_string(index) + " of " + std::to_string(count) + " is invalid");
  }
}

std::optional<uint32_t> read_payload_bytes(reader& input, uint32_t minimum = 1)
{
  const uint32_t value = read_uint32(input);
  if (value < minimum || value > e3ap_maximum_payload_bytes) {
    fail("payloadBytes is outside its bound");
  }
  return value;
}

// ---------------------------------------------------------------------------
// Messages (decode)
// ---------------------------------------------------------------------------

#define E3_EXPECT(wire) \
  if (type != (wire)) { \
    return false;       \
  }

e3ap_setup_request read_setup_request(const decode_context& context, ocudu::span<const uint8_t> bytes)
{
  e3ap_setup_request result;
  result.e3ap_protocol_version.clear();
  read_message(bytes, [&](uint32_t number, uint8_t type, reader& input) {
    if (!context.reads(number)) {
      return false;
    }
    switch (number) {
      case 1:
        E3_EXPECT(wire_length);
        result.e3ap_protocol_version = read_text(input, context.bounds.version, "e3apProtocolVersion");
        return true;
      case 2:
        E3_EXPECT(wire_length);
        result.dapp_name = read_text(input, context.bounds.name, "dAppName");
        return true;
      case 3:
        E3_EXPECT(wire_length);
        result.dapp_version = read_text(input, context.bounds.version, "dAppVersion");
        return true;
      case 4:
        E3_EXPECT(wire_length);
        result.vendor = read_text(input, context.bounds.name, "vendor");
        return true;
      case field::ocudu_profile:
        E3_EXPECT(wire_varint);
        result.ocudu_profile = read_profile(input);
        return true;
      case 101:
        E3_EXPECT(wire_length);
        result.dapp_instance = read_instance(input, "dAppInstance");
        return true;
      default:
        return false;
    }
  });
  require_text(result.e3ap_protocol_version, "e3apProtocolVersion");
  require_text(result.dapp_name, "dAppName");
  require_text(result.dapp_version, "dAppVersion");
  require_text(result.vendor, "vendor");
  return result;
}

void check_list(const decode_context& context, const std::vector<uint16_t>& values, const char* what)
{
  if (values.size() > context.bounds.list) {
    fail(std::string(what) + " list exceeds " + context.module() + "'s bound");
  }
  for (const uint16_t value : values) {
    checked_identifier(context, value, what);
  }
}

e3ap_ran_function_definition read_ran_function(const decode_context& context, ocudu::span<const uint8_t> bytes)
{
  e3ap_ran_function_definition result;
  uint32_t                     function_id = 0;
  read_message(bytes, [&](uint32_t number, uint8_t type, reader& input) {
    if (!context.reads(number)) {
      return false;
    }
    switch (number) {
      case 1:
        E3_EXPECT(wire_varint);
        function_id = read_uint32(input);
        return true;
      case 2:
        return read_repeated(type, input, result.telemetry_identifiers, 65535, "telemetry identifier");
      case 3:
        return read_repeated(type, input, result.control_identifiers, 65535, "control identifier");
      case 4:
        E3_EXPECT(wire_length);
        result.ran_function_data = read_bytes(input, context.bounds.payload, "ranFunctionData");
        return true;
      case 100:
        E3_EXPECT(wire_length);
        result.ran_function_name = read_text(input, context.bounds.name, "ranFunctionName");
        return true;
      case 101:
        E3_EXPECT(wire_length);
        result.ran_function_version = read_text(input, context.bounds.version, "ranFunctionVersion");
        return true;
      default:
        return false;
    }
  });
  result.ran_function_identifier = checked_identifier(context, function_id, "RAN function identifier");
  check_list(context, result.telemetry_identifiers, "telemetry identifier");
  check_list(context, result.control_identifiers, "control identifier");
  require_payload(result.ran_function_data, "ranFunctionData");
  return result;
}

e3ap_setup_response read_setup_response(const decode_context& context, ocudu::span<const uint8_t> bytes)
{
  e3ap_setup_response result;
  result.response_code = e3ap_response_code::positive;
  bool list_present    = false;
  read_message(bytes, [&](uint32_t number, uint8_t type, reader& input) {
    if (!context.reads(number)) {
      return false;
    }
    switch (number) {
      case 1:
        E3_EXPECT(wire_varint);
        result.request_id = read_uint32(input);
        return true;
      case 2:
        E3_EXPECT(wire_varint);
        result.response_code = read_response_code(input);
        return true;
      case 3:
        E3_EXPECT(wire_length);
        result.e3ap_protocol_version = read_text(input, context.bounds.version, "e3apProtocolVersion");
        return true;
      case 4:
        E3_EXPECT(wire_varint);
        result.dapp_identifier = checked_identifier(context, read_uint32(input), "dApp identifier");
        return true;
      case 5:
        E3_EXPECT(wire_length);
        result.ran_identifier = read_text(input, context.bounds.name, "ranIdentifier");
        return true;
      case 6:
        E3_EXPECT(wire_length);
        if (result.ran_functions.size() == context.bounds.ran_functions) {
          fail("RAN function list exceeds its bound");
        }
        result.ran_functions.push_back(read_ran_function(context, input.length_delimited()));
        return true;
      case field::ocudu_profile:
        E3_EXPECT(wire_varint);
        result.ocudu_profile = read_profile(input);
        return true;
      case 101:
        E3_EXPECT(wire_varint);
        result.cause = read_enum(input, e3ap_cause::shutting_down, "cause");
        return true;
      case 102:
        E3_EXPECT(wire_length);
        result.detail = read_text(input, context.bounds.text, "detail");
        return true;
      case 103:
        E3_EXPECT(wire_length);
        result.ran_instance = read_instance(input, "ranInstance");
        return true;
      case 104: {
        E3_EXPECT(wire_varint);
        const uint32_t value = read_uint32(input);
        if (value < 1 || value > e3ap_maximum_payload_bytes) {
          fail("maximumPayloadBytes is outside its bound");
        }
        result.maximum_payload_bytes = value;
        return true;
      }
      case 105: {
        E3_EXPECT(wire_varint);
        const uint32_t value = read_uint32(input);
        if (value < 1 || value > e3ap_maximum_fragment_bytes) {
          fail("maximumFragmentBytes is outside its bound");
        }
        result.maximum_fragment_bytes = value;
        return true;
      }
      case 106:
        E3_EXPECT(wire_varint);
        list_present = input.varint() != 0;
        return true;
      default:
        return false;
    }
  });
  checked_message_id(context, result.request_id, "request id");
  require_text(result.ran_identifier, "ranIdentifier");
  if (result.e3ap_protocol_version) {
    require_text(*result.e3ap_protocol_version, "e3apProtocolVersion");
  }
  result.ran_functions_present = list_present || !result.ran_functions.empty();
  return result;
}

e3ap_subscription_request read_subscription_request(const decode_context& context, ocudu::span<const uint8_t> bytes)
{
  e3ap_subscription_request result;
  uint32_t                  dapp = 0, function = 0;
  read_message(bytes, [&](uint32_t number, uint8_t type, reader& input) {
    if (!context.reads(number)) {
      return false;
    }
    switch (number) {
      case 1:
        E3_EXPECT(wire_varint);
        dapp = read_uint32(input);
        return true;
      case 2:
        E3_EXPECT(wire_varint);
        function = read_uint32(input);
        return true;
      case 3:
        return read_repeated(type, input, result.telemetry_identifiers, 65535, "telemetry identifier");
      case 4:
        return read_repeated(type, input, result.control_identifiers, 65535, "control identifier");
      case 5:
        E3_EXPECT(wire_varint);
        result.subscription_time_s = read_uint32(input);
        return true;
      case 6:
        E3_EXPECT(wire_varint);
        result.periodicity_ms = read_uint32(input);
        return true;
      case 100:
        E3_EXPECT(wire_length);
        result.subscription_data = read_bytes(input, context.bounds.subscription_data, "subscriptionData");
        return true;
      case 101:
        E3_EXPECT(wire_varint);
        result.delivery_mode = read_enum(input, e3ap_delivery_mode::by_reference, "delivery mode");
        return true;
      case 102: {
        E3_EXPECT(wire_varint);
        const uint32_t value = read_uint32(input);
        if (value > e3ap_maximum_payload_bytes) {
          fail("minimumByReferenceBytes exceeds its bound");
        }
        result.minimum_by_reference_bytes = value;
        return true;
      }
      default:
        return false;
    }
  });
  result.dapp_identifier         = checked_identifier(context, dapp, "dApp identifier");
  result.ran_function_identifier = checked_identifier(context, function, "RAN function identifier");
  check_list(context, result.telemetry_identifiers, "telemetry identifier");
  check_list(context, result.control_identifiers, "control identifier");
  if (result.subscription_time_s && *result.subscription_time_s > context.bounds.subscription_time) {
    fail("subscription time exceeds " + std::string(context.module()) + "'s bound");
  }
  if (result.periodicity_ms && *result.periodicity_ms > context.bounds.periodicity) {
    fail("periodicity exceeds " + std::string(context.module()) + "'s bound");
  }
  return result;
}

e3ap_subscription_delete read_subscription_delete(const decode_context& context, ocudu::span<const uint8_t> bytes)
{
  uint32_t dapp = 0, subscription = 0;
  read_message(bytes, [&](uint32_t number, uint8_t type, reader& input) {
    if (type != wire_varint || (number != 1 && number != 2)) {
      return false;
    }
    (number == 1 ? dapp : subscription) = read_uint32(input);
    return true;
  });
  e3ap_subscription_delete result;
  result.dapp_identifier = checked_identifier(context, dapp, "dApp identifier");
  result.subscription_id = checked_identifier(context, subscription, "subscription identifier");
  return result;
}

e3ap_shared_region read_shared_region(ocudu::span<const uint8_t> bytes)
{
  e3ap_shared_region result;
  result.memory_domain  = e3ap_memory_domain::host;
  result.header_bytes   = 0;
  result.layout_version = 0;
  read_message(bytes, [&](uint32_t number, uint8_t type, reader& input) {
    switch (number) {
      case 1:
        E3_EXPECT(wire_varint);
        result.memory_domain = read_enum(input, e3ap_memory_domain::accelerator_ipc_export, "memory domain");
        return true;
      case 2:
        E3_EXPECT(wire_length);
        result.region_name = read_text(input, ocudu_limits.name, "regionName");
        return true;
      case 3:
        E3_EXPECT(wire_varint);
        result.region_bytes = input.varint();
        return true;
      case 4:
        E3_EXPECT(wire_varint);
        result.header_bytes = read_u16(input, "headerBytes");
        return true;
      case 5:
        E3_EXPECT(wire_varint);
        result.row_bytes = input.varint();
        return true;
      case 6:
        E3_EXPECT(wire_varint);
        result.nof_rows = read_u16(input, "nofRows");
        return true;
      case 7:
        E3_EXPECT(wire_varint);
        result.layout_version = read_u16(input, "layoutVersion");
        return true;
      default:
        return false;
    }
  });
  require_text(result.region_name, "regionName");
  if (result.nof_rows < 1 || result.row_bytes == 0) {
    fail("shared region needs at least one non-empty row");
  }
  return result;
}

e3ap_tensor_descriptor read_tensor_descriptor(ocudu::span<const uint8_t> bytes)
{
  e3ap_tensor_descriptor result;
  result.memory_domain = e3ap_memory_domain::host;
  bool located         = false;
  read_message(bytes, [&](uint32_t number, uint8_t type, reader& input) {
    switch (number) {
      case 1:
        E3_EXPECT(wire_varint);
        result.memory_domain = read_enum(input, e3ap_memory_domain::accelerator_ipc_export, "memory domain");
        return true;
      case 2: {
        E3_EXPECT(wire_length);
        // A repeated oneof member merges into the one already set; a different member replaces it.
        if (!located || !std::holds_alternative<e3ap_posix_shared_location>(result.location)) {
          result.location = e3ap_posix_shared_location{};
        }
        auto& location = std::get<e3ap_posix_shared_location>(result.location);
        read_message(input.length_delimited(), [&](uint32_t inner, uint8_t inner_type, reader& in) {
          switch (inner) {
            case 1:
              if (inner_type != wire_length) {
                return false;
              }
              location.region_name = read_text(in, ocudu_limits.name, "regionName");
              return true;
            case 2:
              if (inner_type != wire_varint) {
                return false;
              }
              location.row_index = read_u16(in, "rowIndex");
              return true;
            case 3:
              if (inner_type != wire_varint) {
                return false;
              }
              location.byte_offset = in.varint();
              return true;
            default:
              return false;
          }
        });
        located = true;
        return true;
      }
      case 3: {
        E3_EXPECT(wire_length);
        if (!located || !std::holds_alternative<e3ap_accelerator_ipc_location>(result.location)) {
          result.location = e3ap_accelerator_ipc_location{};
        }
        auto& location = std::get<e3ap_accelerator_ipc_location>(result.location);
        read_message(input.length_delimited(), [&](uint32_t inner, uint8_t inner_type, reader& in) {
          const auto handle = [&](std::array<uint8_t, 64>& destination) {
            const auto value = in.length_delimited();
            if (value.size() != 64) {
              fail("accelerator IPC handles are 64 bytes");
            }
            std::memcpy(destination.data(), value.data(), 64);
          };
          switch (inner) {
            case 1:
              if (inner_type != wire_varint) {
                return false;
              }
              location.device_ordinal = read_u16(in, "deviceOrdinal");
              return true;
            case 2:
              if (inner_type != wire_varint) {
                return false;
              }
              location.allocation_id = in.varint();
              return true;
            case 3:
              if (inner_type != wire_length) {
                return false;
              }
              handle(location.memory_handle);
              return true;
            case 4:
              if (inner_type != wire_length) {
                return false;
              }
              location.ready_event_handle.emplace();
              handle(*location.ready_event_handle);
              return true;
            case 5:
              if (inner_type != wire_varint) {
                return false;
              }
              location.byte_offset = in.varint();
              return true;
            default:
              return false;
          }
        });
        located = true;
        return true;
      }
      case 4:
        E3_EXPECT(wire_varint);
        result.byte_length = input.varint();
        return true;
      case 5:
        E3_EXPECT(wire_varint);
        result.generation = input.varint();
        return true;
      case 6:
        E3_EXPECT(wire_varint);
        result.element_type = read_enum(input, e3ap_element_type::complex_fp32, "element type");
        return true;
      case 7:
        return read_repeated(type, input, result.dimensions, 0xFFFFFFFFULL, "dimension");
      case 8:
        return read_repeated(type, input, result.byte_strides, ~uint64_t{0}, "byte stride");
      case 9:
        E3_EXPECT(wire_varint);
        result.layout_id = read_u16(input, "layoutId");
        return true;
      case 10:
        E3_EXPECT(wire_varint);
        result.lease_expiry_ns = input.varint();
        return true;
      default:
        return false;
    }
  });
  if (!located) {
    fail("tensor descriptor has no location");
  }
  if (const auto* posix = std::get_if<e3ap_posix_shared_location>(&result.location)) {
    require_text(posix->region_name, "regionName");
  }
  if (result.dimensions.size() > 8 || result.byte_strides.size() > 8) {
    fail("tensor rank exceeds 8");
  }
  return result;
}

e3ap_subscription_response read_subscription_response(const decode_context& context,
                                                       ocudu::span<const uint8_t> bytes)
{
  e3ap_subscription_response result;
  result.response_code = e3ap_response_code::positive;
  uint32_t dapp        = 0;
  read_message(bytes, [&](uint32_t number, uint8_t type, reader& input) {
    if (!context.reads(number)) {
      return false;
    }
    switch (number) {
      case 1:
        E3_EXPECT(wire_varint);
        result.request_id = read_uint32(input);
        return true;
      case 2:
        E3_EXPECT(wire_varint);
        dapp = read_uint32(input);
        return true;
      case 3:
        E3_EXPECT(wire_varint);
        result.response_code = read_response_code(input);
        return true;
      case 4:
        E3_EXPECT(wire_varint);
        result.subscription_id = checked_identifier(context, read_uint32(input), "subscription identifier");
        return true;
      case 100:
      case 101: {
        E3_EXPECT(wire_length);
        auto& list = number == 100 ? result.telemetry_identifiers : result.control_identifiers;
        if (!list) {
          list.emplace();
        }
        read_message(input.length_delimited(), [&](uint32_t inner, uint8_t inner_type, reader& in) {
          return inner == 1 && read_repeated(inner_type, in, *list, 65535, "identifier");
        });
        return true;
      }
      case 102:
        E3_EXPECT(wire_varint);
        result.cause = read_enum(input, e3ap_cause::shutting_down, "cause");
        return true;
      case 103:
        E3_EXPECT(wire_length);
        result.detail = read_text(input, context.bounds.text, "detail");
        return true;
      case 104:
        E3_EXPECT(wire_length);
        result.shared_region = read_shared_region(input.length_delimited());
        return true;
      default:
        return false;
    }
  });
  checked_message_id(context, result.request_id, "request id");
  result.dapp_identifier = checked_identifier(context, dapp, "dApp identifier");
  if (result.telemetry_identifiers) {
    check_list(context, *result.telemetry_identifiers, "telemetry identifier");
  }
  if (result.control_identifiers) {
    check_list(context, *result.control_identifiers, "control identifier");
  }
  return result;
}

e3ap_indication_message read_indication(const decode_context& context, ocudu::span<const uint8_t> bytes)
{
  e3ap_indication_message result;
  uint32_t                dapp = 0, function = 0, fragment_count = 1;
  read_message(bytes, [&](uint32_t number, uint8_t type, reader& input) {
    if (!context.reads(number)) {
      return false;
    }
    switch (number) {
      case 1:
        E3_EXPECT(wire_varint);
        dapp = read_uint32(input);
        return true;
      case 2:
        E3_EXPECT(wire_varint);
        function = read_uint32(input);
        return true;
      case 3:
        E3_EXPECT(wire_length);
        result.protocol_data = read_bytes(input, context.bounds.payload, "protocolData");
        return true;
      case 100:
        E3_EXPECT(wire_varint);
        result.subscription_id = checked_identifier(context, read_uint32(input), "subscription identifier");
        return true;
      case 101:
        E3_EXPECT(wire_varint);
        result.telemetry_identifier = checked_identifier(context, read_uint32(input), "telemetry identifier");
        return true;
      case 102:
        E3_EXPECT(wire_varint);
        result.sequence = input.varint();
        return true;
      case 103:
        E3_EXPECT(wire_varint);
        result.fragment_index = read_u16(input, "fragmentIndex");
        return true;
      case 104:
        E3_EXPECT(wire_varint);
        fragment_count = read_uint32(input);
        return true;
      case 105:
        E3_EXPECT(wire_varint);
        result.payload_bytes = read_payload_bytes(input);
        return true;
      case 106:
        E3_EXPECT(wire_length);
        result.by_reference = read_tensor_descriptor(input.length_delimited());
        return true;
      default:
        return false;
    }
  });
  result.dapp_identifier         = checked_identifier(context, dapp, "dApp identifier");
  result.ran_function_identifier = checked_identifier(context, function, "RAN function identifier");
  require_payload(result.protocol_data, "protocolData");
  check_fragments(result.fragment_index, fragment_count, "indication");
  result.fragment_count = fragment_count;
  if (!context.ocudu()) {
    result.payload_bytes = static_cast<uint32_t>(result.protocol_data.size());
  }
  return result;
}

e3ap_dapp_control_action read_control_action(const decode_context& context, ocudu::span<const uint8_t> bytes)
{
  e3ap_dapp_control_action result;
  uint32_t                 dapp = 0, function = 0, control = 0, sequence = 0, fragment_count = 1;
  read_message(bytes, [&](uint32_t number, uint8_t type, reader& input) {
    if (!context.reads(number)) {
      return false;
    }
    switch (number) {
      case 1:
        E3_EXPECT(wire_varint);
        dapp = read_uint32(input);
        return true;
      case 2:
        E3_EXPECT(wire_varint);
        function = read_uint32(input);
        return true;
      case 3:
        E3_EXPECT(wire_varint);
        control = read_uint32(input);
        return true;
      case 4:
        E3_EXPECT(wire_length);
        result.action_data = read_bytes(input, context.bounds.payload, "actionData");
        return true;
      case 5:
        E3_EXPECT(wire_varint);
        sequence = read_uint32(input);
        return true;
      case 100:
        E3_EXPECT(wire_varint);
        result.fragment_index = read_u16(input, "fragmentIndex");
        return true;
      case 101:
        E3_EXPECT(wire_varint);
        fragment_count = read_uint32(input);
        return true;
      case 102:
        E3_EXPECT(wire_varint);
        result.payload_bytes = read_payload_bytes(input);
        return true;
      default:
        return false;
    }
  });
  result.dapp_identifier         = checked_identifier(context, dapp, "dApp identifier");
  result.ran_function_identifier = checked_identifier(context, function, "RAN function identifier");
  result.control_identifier      = checked_identifier(context, control, "control identifier");
  if (sequence != 0) {
    result.sequence_identifier = sequence;
  }
  require_payload(result.action_data, "actionData");
  check_fragments(result.fragment_index, fragment_count, "control action");
  result.fragment_count = fragment_count;
  if (!context.ocudu()) {
    result.payload_bytes = static_cast<uint32_t>(result.action_data.size());
  }
  return result;
}

/// DAppReport and XAppControlAction share their layout (ids, payload, sequence).
template <typename T>
T read_report_like(const decode_context& context, ocudu::span<const uint8_t> bytes, const char* payload_name)
{
  T                    result;
  uint32_t             dapp = 0, function = 0, sequence = 0;
  std::vector<uint8_t> payload;
  read_message(bytes, [&](uint32_t number, uint8_t type, reader& input) {
    switch (number) {
      case 1:
        E3_EXPECT(wire_varint);
        dapp = read_uint32(input);
        return true;
      case 2:
        E3_EXPECT(wire_varint);
        function = read_uint32(input);
        return true;
      case 3:
        E3_EXPECT(wire_length);
        payload = read_bytes(input, context.bounds.payload, payload_name);
        return true;
      case 4:
        E3_EXPECT(wire_varint);
        sequence = read_uint32(input);
        return true;
      default:
        return false;
    }
  });
  result.dapp_identifier         = checked_identifier(context, dapp, "dApp identifier");
  result.ran_function_identifier = checked_identifier(context, function, "RAN function identifier");
  require_payload(payload, payload_name);
  if (sequence == 0) {
    fail(std::string(payload_name) + " PDU has no sequence identifier (mandatory)");
  }
  result.sequence_identifier = sequence;
  if constexpr (std::is_same_v<T, e3ap_dapp_report>) {
    result.report_data = std::move(payload);
  } else {
    result.xapp_control_data = std::move(payload);
  }
  return result;
}

e3ap_release_message read_release(const decode_context& context, ocudu::span<const uint8_t> bytes)
{
  uint32_t dapp = 0;
  read_message(bytes, [&](uint32_t number, uint8_t type, reader& input) {
    if (number != 1 || type != wire_varint) {
      return false;
    }
    dapp = read_uint32(input);
    return true;
  });
  e3ap_release_message result;
  result.dapp_identifier = checked_identifier(context, dapp, "dApp identifier");
  return result;
}

e3ap_message_ack read_ack(const decode_context& context, ocudu::span<const uint8_t> bytes)
{
  e3ap_message_ack result;
  result.response_code = e3ap_response_code::positive;
  read_message(bytes, [&](uint32_t number, uint8_t type, reader& input) {
    if (!context.reads(number)) {
      return false;
    }
    switch (number) {
      case 1:
        E3_EXPECT(wire_varint);
        result.request_id = read_uint32(input);
        return true;
      case 2:
        E3_EXPECT(wire_varint);
        result.response_code = read_response_code(input);
        return true;
      case 100:
        E3_EXPECT(wire_varint);
        result.cause = read_enum(input, e3ap_cause::shutting_down, "cause");
        return true;
      case 101:
        E3_EXPECT(wire_length);
        result.detail = read_text(input, context.bounds.text, "detail");
        return true;
      default:
        return false;
    }
  });
  checked_message_id(context, result.request_id, "request id");
  return result;
}

e3ap_control_response read_control_response(const decode_context& context, ocudu::span<const uint8_t> bytes)
{
  e3ap_control_response result;
  result.response_code    = e3ap_response_code::positive;
  uint32_t dapp           = 0, function = 0, control = 0, fragment_count = 1;
  read_message(bytes, [&](uint32_t number, uint8_t type, reader& input) {
    switch (number) {
      case 1:
        E3_EXPECT(wire_varint);
        result.request_id = read_uint32(input);
        return true;
      case 2:
        E3_EXPECT(wire_varint);
        dapp = read_uint32(input);
        return true;
      case 3:
        E3_EXPECT(wire_varint);
        function = read_uint32(input);
        return true;
      case 4:
        E3_EXPECT(wire_varint);
        control = read_uint32(input);
        return true;
      case 5:
        E3_EXPECT(wire_varint);
        result.response_code = read_response_code(input);
        return true;
      case 6:
        E3_EXPECT(wire_varint);
        result.cause = read_enum(input, e3ap_cause::shutting_down, "cause");
        return true;
      case 7:
        E3_EXPECT(wire_length);
        result.detail = read_text(input, context.bounds.text, "detail");
        return true;
      case 8:
        E3_EXPECT(wire_length);
        result.control_outcome = read_bytes(input, context.bounds.payload, "controlOutcome");
        return true;
      case 9:
        E3_EXPECT(wire_varint);
        result.fragment_index = read_u16(input, "fragmentIndex");
        return true;
      case 10:
        E3_EXPECT(wire_varint);
        fragment_count = read_uint32(input);
        return true;
      case 11:
        E3_EXPECT(wire_varint);
        result.payload_bytes = *read_payload_bytes(input, 0);
        return true;
      default:
        return false;
    }
  });
  checked_message_id(context, result.request_id, "request id");
  result.dapp_identifier         = checked_identifier(context, dapp, "dApp identifier");
  result.ran_function_identifier = checked_identifier(context, function, "RAN function identifier");
  result.control_identifier      = checked_identifier(context, control, "control identifier");
  check_fragments(result.fragment_index, fragment_count, "control response");
  result.fragment_count = fragment_count;
  return result;
}

#undef E3_EXPECT

e3ap_message read_body(const decode_context& context, uint32_t number, ocudu::span<const uint8_t> bytes)
{
  switch (number) {
    case field::setup_request:
      return read_setup_request(context, bytes);
    case field::setup_response:
      return read_setup_response(context, bytes);
    case field::subscription_request:
      return read_subscription_request(context, bytes);
    case field::subscription_delete:
      return read_subscription_delete(context, bytes);
    case field::subscription_response:
      return read_subscription_response(context, bytes);
    case field::indication_message:
      return read_indication(context, bytes);
    case field::dapp_control_action:
      return read_control_action(context, bytes);
    case field::dapp_report:
      return read_report_like<e3ap_dapp_report>(context, bytes, "reportData");
    case field::xapp_control_action:
      return read_report_like<e3ap_xapp_control_action>(context, bytes, "xAppControlData");
    case field::release_message:
      return read_release(context, bytes);
    case field::message_ack:
      return read_ack(context, bytes);
    case field::control_response:
      return read_control_response(context, bytes);
    default:
      fail("unknown E3Pdu message");
  }
}

bool is_message_field(uint32_t number, const decode_context& context) noexcept
{
  return (number >= field::setup_request && number <= field::message_ack) ||
         (number == field::control_response && context.ocudu());
}

/// Top-level E3Pdu scan shared by decode and peek: id, timestamp and the (last) oneof member. A
/// oneof member repeated with the same number merges, so its byte ranges are all kept.
struct pdu_scan {
  uint32_t                                id{};
  uint64_t                                timestamp{};
  uint32_t                                message{};
  std::vector<ocudu::span<const uint8_t>> parts;
};

template <typename IsMessage>
void scan_pdu(ocudu::span<const uint8_t> bytes, const IsMessage& is_message, pdu_scan& scan)
{
  reader input(bytes);
  while (!input.done()) {
    const auto [number, type] = input.tag();
    if (number == field::pdu_id && type == wire_varint) {
      scan.id = read_uint32(input);
    } else if (number == field::pdu_timestamp && type == wire_varint) {
      scan.timestamp = input.varint();
    } else if (type == wire_length && is_message(number)) {
      if (number != scan.message) {
        scan.parts.clear();
        scan.message = number;
      }
      scan.parts.push_back(input.length_delimited());
    } else {
      input.skip(type);
    }
  }
}

/// E3Pdu.msg alternatives of either profile.
bool any_message_field(uint32_t number) noexcept
{
  return (number >= field::setup_request && number <= field::message_ack) || number == field::control_response;
}

const limits& limits_of(e3ap_profile profile) noexcept
{
  return profile == e3ap_profile::ocudu ? ocudu_limits : libe3_limits;
}

} // namespace

codec_result<std::vector<uint8_t>> e3ap_protobuf_codec::encode(const e3ap_pdu& pdu,
                                                               e3ap_profile profile) noexcept
{
  try {
    const encode_context context{profile, limits_of(profile)};
    size_sink            sizer;
    put_pdu(sizer, context, pdu);
    if (sizer.size > maximum_pdu_bytes) {
      return {std::nullopt, "E3-PDU of " + std::to_string(sizer.size) + " bytes exceeds the PDU bound"};
    }
    std::vector<uint8_t> result(sizer.size);
    write_sink           writer{result.data()};
    put_pdu(writer, context, pdu);
    return {std::move(result), {}};
  } catch (const std::bad_alloc&) {
    return {std::nullopt, "Protobuf encoding allocation failed"};
  } catch (const std::exception& exception) {
    return {std::nullopt, exception.what()};
  } catch (...) {
    return {std::nullopt, "unknown Protobuf encoding failure"};
  }
}

codec_result<e3ap_pdu> e3ap_protobuf_codec::decode(ocudu::span<const uint8_t> bytes,
                                                   e3ap_profile      profile) noexcept
{
  try {
    if (bytes.size() > maximum_pdu_bytes) {
      return {std::nullopt, "E3-PDU exceeds the PDU bound"};
    }
    const decode_context context{profile, limits_of(profile)};
    pdu_scan             scan;
    scan_pdu(bytes, [&](uint32_t number) { return is_message_field(number, context); }, scan);
    if (scan.message == 0) {
      return {std::nullopt, "E3Pdu carries no message this profile knows"};
    }
    e3ap_pdu result;
    result.id = checked_message_id(context, scan.id);
    if (scan.timestamp != 0) {
      result.timestamp_ns = static_cast<int64_t>(scan.timestamp);
    }
    if (scan.parts.size() == 1) {
      result.message = read_body(context, scan.message, scan.parts.front());
    } else {
      // Rare: a repeated singular message merges, which equals parsing the concatenation.
      std::vector<uint8_t> merged;
      for (const auto part : scan.parts) {
        merged.insert(merged.end(), part.begin(), part.end());
      }
      result.message = read_body(context, scan.message, merged);
    }
    return {std::move(result), {}};
  } catch (const std::bad_alloc&) {
    return {std::nullopt, "Protobuf decoding allocation failed"};
  } catch (const std::exception& exception) {
    return {std::nullopt, exception.what()};
  } catch (...) {
    return {std::nullopt, "unknown Protobuf decoding failure"};
  }
}

std::optional<e3ap_profile> e3ap_protobuf_codec::setup_profile(ocudu::span<const uint8_t> bytes) noexcept
{
  try {
    pdu_scan scan;
    scan_pdu(bytes, any_message_field, scan);
    if (scan.message != field::setup_request) {
      return std::nullopt;
    }
    bool ocudu = false;
    for (const auto part : scan.parts) {
      read_message(part, [&](uint32_t number, uint8_t type, reader& input) {
        if (number == field::ocudu_profile && type == wire_varint) {
          input.varint();
          ocudu = true;
          return true;
        }
        return false;
      });
    }
    return ocudu ? e3ap_profile::ocudu : e3ap_profile::libe3;
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<e3ap_protobuf_codec::routing_view> e3ap_protobuf_codec::peek(ocudu::span<const uint8_t> bytes) noexcept
{
  try {
    // Fixed-size scan: no allocation unless a message is split across repeated members.
    routing_view view;
    reader       input(bytes);
    ocudu::span<const uint8_t> body;
    while (!input.done()) {
      const auto [number, type] = input.tag();
      if (number == field::pdu_id && type == wire_varint) {
        view.pdu_id = read_uint32(input);
      } else if (type == wire_length && any_message_field(number)) {
        view.message = number;
        body         = input.length_delimited();
      } else {
        input.skip(type);
      }
    }
    if (view.message == 0) {
      return std::nullopt;
    }
    // Where each message keeps its dApp identifier, request id and response code.
    uint32_t dapp_field = 1, request_field = 0, code_field = 0;
    switch (view.message) {
      case field::setup_request:
        dapp_field = 0;
        break;
      case field::setup_response:
        dapp_field = 4, request_field = 1, code_field = 2;
        break;
      case field::subscription_response:
        dapp_field = 2, request_field = 1, code_field = 3;
        break;
      case field::message_ack:
        dapp_field = 0, request_field = 1, code_field = 2;
        break;
      case field::control_response:
        dapp_field = 2, request_field = 1, code_field = 5;
        break;
      default:
        break;
    }
    view.positive = true;
    read_message(body, [&](uint32_t number, uint8_t type, reader& field_input) {
      if (type != wire_varint) {
        return false;
      }
      if (number == dapp_field) {
        const uint32_t value = read_uint32(field_input);
        if (value >= 1 && value <= 65535) {
          view.dapp_identifier = static_cast<uint16_t>(value);
        }
        return true;
      }
      if (number == request_field) {
        view.request_id = read_uint32(field_input);
        return true;
      }
      if (number == code_field) {
        view.positive = field_input.varint() == 0;
        return true;
      }
      return false;
    });
    return view;
  } catch (...) {
    return std::nullopt;
  }
}

bool e3ap_protobuf_codec::representable(const e3ap_message&   message,
                                        e3ap_profile profile,
                                        std::string*          detail) noexcept
{
  e3ap_pdu probe;
  probe.id      = 1;
  probe.message = message;
  auto encoded  = encode(probe, profile);
  if (!encoded && detail != nullptr) {
    *detail = encoded.detail;
  }
  return static_cast<bool>(encoded);
}
