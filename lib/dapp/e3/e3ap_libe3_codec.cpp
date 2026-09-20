// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/e3/e3ap_libe3_codec.h"

extern "C" {
#include "LIBE3_E3-DAppControlAction.h"
#include "LIBE3_E3-DAppReport.h"
#include "LIBE3_E3-IndicationMessage.h"
#include "LIBE3_E3-MessageAck.h"
#include "LIBE3_E3-PDU.h"
#include "LIBE3_E3-RanFunctionDefinition.h"
#include "LIBE3_E3-ReleaseMessage.h"
#include "LIBE3_E3-ResponseCode.h"
#include "LIBE3_E3-SetupRequest.h"
#include "LIBE3_E3-SetupResponse.h"
#include "LIBE3_E3-SubscriptionDelete.h"
#include "LIBE3_E3-SubscriptionRequest.h"
#include "LIBE3_E3-SubscriptionResponse.h"
#include "LIBE3_E3-Timestamp.h"
#include "LIBE3_E3-XAppControlAction.h"
}

#include "asn1_codec_internal.h"
#include <cstring>
#include <type_traits>

using namespace ocudu::dapp;
using namespace ocudu::dapp::e3;
using namespace ocudu::dapp::e3::asn1_detail;

namespace {

constexpr uint64_t    maximum_identifier    = e3ap_libe3_codec::maximum_identifier;
constexpr uint64_t    maximum_message_id    = e3ap_libe3_codec::maximum_message_id;
constexpr std::size_t maximum_payload_bytes = e3ap_libe3_codec::maximum_payload_bytes;
constexpr std::size_t maximum_name_bytes    = e3ap_libe3_codec::maximum_name_bytes;
constexpr std::size_t maximum_version_bytes = e3ap_libe3_codec::maximum_version_bytes;
constexpr std::size_t maximum_list_entries  = 100;
constexpr std::size_t maximum_ran_functions = 255;

// ---------------------------------------------------------------------------
// Small conversions
// ---------------------------------------------------------------------------

long to_wire(e3ap_response_code code) noexcept
{
  return code == e3ap_response_code::positive ? LIBE3_E3_ResponseCode_positive : LIBE3_E3_ResponseCode_negative;
}

e3ap_response_code read_response_code(long value)
{
  if (value == LIBE3_E3_ResponseCode_positive) {
    return e3ap_response_code::positive;
  }
  if (value == LIBE3_E3_ResponseCode_negative) {
    return e3ap_response_code::negative;
  }
  throw std::runtime_error("invalid libe3 response code");
}

/// 1..100 identifiers (dApp, RAN function, telemetry, control, subscription).
long identifier(uint64_t value, const char* what)
{
  if (value < 1 || value > maximum_identifier) {
    throw std::runtime_error(std::string(what) + " " + std::to_string(value) + " is outside libe3's 1..100");
  }
  return static_cast<long>(value);
}

uint16_t read_identifier(long value, const char* what)
{
  return static_cast<uint16_t>(read_ranged(value, 1, maximum_identifier, what));
}

long message_id(uint32_t value)
{
  if (value < 1 || value > maximum_message_id) {
    throw std::runtime_error("message id " + std::to_string(value) + " is outside libe3's 1..1000");
  }
  return static_cast<long>(value);
}

uint32_t read_message_id(long value)
{
  return static_cast<uint32_t>(read_ranged(value, 1, maximum_message_id, "message id"));
}

void set_version(UTF8String_t& destination, const std::string& value)
{
  if (value.empty() || value.size() > maximum_version_bytes) {
    throw std::runtime_error("version text must be 1..11 bytes for libe3");
  }
  set_text(destination, value);
}

void set_name(UTF8String_t& destination, const std::string& value)
{
  if (value.empty() || value.size() > maximum_name_bytes) {
    throw std::runtime_error("name text must be 1..64 bytes for libe3");
  }
  set_text(destination, value);
}

void set_payload(OCTET_STRING_t& destination, const std::vector<uint8_t>& value, const char* what)
{
  if (value.empty() || value.size() > maximum_payload_bytes) {
    throw std::runtime_error(std::string(what) + " must be 1..32768 bytes for libe3");
  }
  set_octets(destination, value);
}

std::vector<uint8_t> read_payload(const OCTET_STRING_t& value)
{
  const auto bytes = read_bytes(value, maximum_payload_bytes);
  if (bytes.empty()) {
    throw std::runtime_error("libe3 payload must not be empty");
  }
  return bytes;
}

template <typename List>
void set_identifier_list(List& destination, const std::vector<uint16_t>& values, const char* what)
{
  // Identifiers the libe3 module cannot name are left out rather than failing
  // the whole message: a peer that cannot express them cannot use them.
  std::size_t written = 0;
  for (const uint16_t value : values) {
    if (value < 1 || value > maximum_identifier) {
      continue;
    }
    if (written == maximum_list_entries) {
      throw std::runtime_error(std::string(what) + " list exceeds libe3's 100 entries");
    }
    long* item = make_asn<long>();
    *item      = value;
    if (ASN_SEQUENCE_ADD(&destination.list, item) != 0) {
      std::free(item);
      throw std::bad_alloc();
    }
    ++written;
  }
}

template <typename List>
std::vector<uint16_t> read_identifier_list(const List& source, const char* what)
{
  if (source.list.count < 0 || static_cast<std::size_t>(source.list.count) > maximum_list_entries) {
    throw std::runtime_error(std::string(what) + " list exceeds its bound");
  }
  std::vector<uint16_t> result;
  result.reserve(static_cast<std::size_t>(source.list.count));
  for (int index = 0; index != source.list.count; ++index) {
    if (source.list.array[index] == nullptr) {
      throw std::runtime_error("identifier list entry is missing");
    }
    result.push_back(read_identifier(*source.list.array[index], what));
  }
  return result;
}

template <typename List>
std::optional<std::vector<uint16_t>> read_optional_identifier_list(const List* source, const char* what)
{
  if (source == nullptr) {
    return std::nullopt;
  }
  return read_identifier_list(*source, what);
}

void set_optional_identifier(LIBE3_E3_DAppID_t*& destination, const std::optional<uint16_t>& value, const char* what)
{
  if (value) {
    destination  = make_asn<LIBE3_E3_DAppID_t>();
    *destination = identifier(*value, what);
  }
}

void set_optional_long(long*& destination, const std::optional<uint32_t>& value, uint64_t maximum, const char* what)
{
  if (value) {
    if (*value > maximum) {
      throw std::runtime_error(std::string(what) + " exceeds libe3's bound");
    }
    destination  = make_asn<long>();
    *destination = static_cast<long>(*value);
  }
}

std::optional<uint32_t> read_optional_long(const long* source, uint64_t maximum, const char* what)
{
  if (source == nullptr) {
    return std::nullopt;
  }
  return static_cast<uint32_t>(read_ranged(*source, 0, maximum, what));
}

void require_whole(uint16_t fragment_index, uint32_t fragment_count, const char* what)
{
  if (fragment_index != 0 || fragment_count != 1) {
    throw std::runtime_error(std::string(what) + " is fragmented; libe3 carries whole payloads only");
  }
}

// ---------------------------------------------------------------------------
// Messages
// ---------------------------------------------------------------------------

void set_message(LIBE3_E3_PDU_t& pdu, const e3ap_setup_request& source)
{
  auto& destination = *(pdu.msg.choice.setupRequest = make_asn<LIBE3_E3_SetupRequest_t>());
  pdu.msg.present   = LIBE3_E3_PDU__msg_PR_setupRequest;
  set_version(destination.e3apProtocolVersion, source.e3ap_protocol_version);
  set_name(destination.dAppName, source.dapp_name);
  set_version(destination.dAppVersion, source.dapp_version);
  set_name(destination.vendor, source.vendor);
}

e3ap_setup_request read_message(const LIBE3_E3_SetupRequest_t& source)
{
  e3ap_setup_request result;
  result.e3ap_protocol_version = read_string(source.e3apProtocolVersion, maximum_version_bytes);
  result.dapp_name             = read_string(source.dAppName, maximum_name_bytes);
  result.dapp_version          = read_string(source.dAppVersion, maximum_version_bytes);
  result.vendor                = read_string(source.vendor, maximum_name_bytes);
  return result;
}

void set_ran_function(LIBE3_E3_RanFunctionDefinition_t& destination, const e3ap_ran_function_definition& source)
{
  destination.ranFunctionIdentifier = identifier(source.ran_function_identifier, "RAN function identifier");
  set_identifier_list(destination.telemetryIdentifierList, source.telemetry_identifiers, "telemetry identifier");
  set_identifier_list(destination.controlIdentifierList, source.control_identifiers, "control identifier");
  set_payload(destination.ranFunctionData, source.ran_function_data, "ranFunctionData");
}

e3ap_ran_function_definition read_ran_function(const LIBE3_E3_RanFunctionDefinition_t& source)
{
  e3ap_ran_function_definition result;
  result.ran_function_identifier = read_identifier(source.ranFunctionIdentifier, "RAN function identifier");
  result.telemetry_identifiers   = read_identifier_list(source.telemetryIdentifierList, "telemetry identifier");
  result.control_identifiers     = read_identifier_list(source.controlIdentifierList, "control identifier");
  result.ran_function_data       = read_payload(source.ranFunctionData);
  return result;
}

void set_message(LIBE3_E3_PDU_t& pdu, const e3ap_setup_response& source)
{
  auto& destination        = *(pdu.msg.choice.setupResponse = make_asn<LIBE3_E3_SetupResponse_t>());
  pdu.msg.present          = LIBE3_E3_PDU__msg_PR_setupResponse;
  destination.requestId    = message_id(source.request_id);
  destination.responseCode = to_wire(source.response_code);
  set_name(destination.ranIdentifier, source.ran_identifier);
  if (source.e3ap_protocol_version) {
    destination.e3apProtocolVersion = make_asn<LIBE3_E3_Version_t>();
    set_version(*destination.e3apProtocolVersion, *source.e3ap_protocol_version);
  }
  set_optional_identifier(destination.dAppIdentifier, source.dapp_identifier, "dApp identifier");
  if (source.ran_functions_present || !source.ran_functions.empty()) {
    if (source.ran_functions.size() > maximum_ran_functions) {
      throw std::runtime_error("RAN function list exceeds its bound");
    }
    using list_type             = std::remove_pointer_t<decltype(destination.ranFunctionList)>;
    destination.ranFunctionList = make_asn<list_type>();
    for (const auto& function : source.ran_functions) {
      set_ran_function(append_asn<LIBE3_E3_RanFunctionDefinition_t>(*destination.ranFunctionList), function);
    }
  }
}

e3ap_setup_response read_message(const LIBE3_E3_SetupResponse_t& source)
{
  e3ap_setup_response result;
  result.request_id     = read_message_id(source.requestId);
  result.response_code  = read_response_code(source.responseCode);
  result.ran_identifier = read_string(source.ranIdentifier, maximum_name_bytes);
  if (source.e3apProtocolVersion != nullptr) {
    result.e3ap_protocol_version = read_string(*source.e3apProtocolVersion, maximum_version_bytes);
  }
  if (source.dAppIdentifier != nullptr) {
    result.dapp_identifier = read_identifier(*source.dAppIdentifier, "dApp identifier");
  }
  if (source.ranFunctionList != nullptr) {
    result.ran_functions_present = true;
    const auto& list             = source.ranFunctionList->list;
    if (list.count < 0 || static_cast<std::size_t>(list.count) > maximum_ran_functions) {
      throw std::runtime_error("RAN function list exceeds its bound");
    }
    for (int index = 0; index != list.count; ++index) {
      if (list.array[index] == nullptr) {
        throw std::runtime_error("RAN function entry is missing");
      }
      result.ran_functions.push_back(read_ran_function(*list.array[index]));
    }
  }
  return result;
}

void set_message(LIBE3_E3_PDU_t& pdu, const e3ap_subscription_request& source)
{
  auto& destination          = *(pdu.msg.choice.subscriptionRequest = make_asn<LIBE3_E3_SubscriptionRequest_t>());
  pdu.msg.present            = LIBE3_E3_PDU__msg_PR_subscriptionRequest;
  destination.dAppIdentifier = identifier(source.dapp_identifier, "dApp identifier");
  destination.ranFunctionIdentifier = identifier(source.ran_function_identifier, "RAN function identifier");
  set_identifier_list(destination.telemetryIdentifierList, source.telemetry_identifiers, "telemetry identifier");
  set_identifier_list(destination.controlIdentifierList, source.control_identifiers, "control identifier");
  set_optional_long(destination.subscriptionTime, source.subscription_time_s, 3600, "subscription time");
  set_optional_long(destination.periodicity, source.periodicity_ms, 1000, "periodicity");
}

e3ap_subscription_request read_message(const LIBE3_E3_SubscriptionRequest_t& source)
{
  e3ap_subscription_request result;
  result.dapp_identifier         = read_identifier(source.dAppIdentifier, "dApp identifier");
  result.ran_function_identifier = read_identifier(source.ranFunctionIdentifier, "RAN function identifier");
  result.telemetry_identifiers   = read_identifier_list(source.telemetryIdentifierList, "telemetry identifier");
  result.control_identifiers     = read_identifier_list(source.controlIdentifierList, "control identifier");
  result.subscription_time_s     = read_optional_long(source.subscriptionTime, 3600, "subscription time");
  result.periodicity_ms          = read_optional_long(source.periodicity, 1000, "periodicity");
  return result;
}

void set_message(LIBE3_E3_PDU_t& pdu, const e3ap_subscription_delete& source)
{
  auto& destination          = *(pdu.msg.choice.subscriptionDelete = make_asn<LIBE3_E3_SubscriptionDelete_t>());
  pdu.msg.present            = LIBE3_E3_PDU__msg_PR_subscriptionDelete;
  destination.dAppIdentifier = identifier(source.dapp_identifier, "dApp identifier");
  destination.subscriptionId = identifier(source.subscription_id, "subscription identifier");
}

e3ap_subscription_delete read_message(const LIBE3_E3_SubscriptionDelete_t& source)
{
  e3ap_subscription_delete result;
  result.dapp_identifier = read_identifier(source.dAppIdentifier, "dApp identifier");
  result.subscription_id = read_identifier(source.subscriptionId, "subscription identifier");
  return result;
}

void set_message(LIBE3_E3_PDU_t& pdu, const e3ap_subscription_response& source)
{
  auto& destination          = *(pdu.msg.choice.subscriptionResponse = make_asn<LIBE3_E3_SubscriptionResponse_t>());
  pdu.msg.present            = LIBE3_E3_PDU__msg_PR_subscriptionResponse;
  destination.requestId      = message_id(source.request_id);
  destination.dAppIdentifier = identifier(source.dapp_identifier, "dApp identifier");
  destination.responseCode   = to_wire(source.response_code);
  if (source.subscription_id) {
    destination.subscriptionId  = make_asn<long>();
    *destination.subscriptionId = identifier(*source.subscription_id, "subscription identifier");
  }
  if (source.telemetry_identifiers) {
    using list_type                     = std::remove_pointer_t<decltype(destination.telemetryIdentifierList)>;
    destination.telemetryIdentifierList = make_asn<list_type>();
    set_identifier_list(*destination.telemetryIdentifierList, *source.telemetry_identifiers, "telemetry identifier");
  }
  if (source.control_identifiers) {
    using list_type                   = std::remove_pointer_t<decltype(destination.controlIdentifierList)>;
    destination.controlIdentifierList = make_asn<list_type>();
    set_identifier_list(*destination.controlIdentifierList, *source.control_identifiers, "control identifier");
  }
}

e3ap_subscription_response read_message(const LIBE3_E3_SubscriptionResponse_t& source)
{
  e3ap_subscription_response result;
  result.request_id      = read_message_id(source.requestId);
  result.dapp_identifier = read_identifier(source.dAppIdentifier, "dApp identifier");
  result.response_code   = read_response_code(source.responseCode);
  if (source.subscriptionId != nullptr) {
    result.subscription_id = read_identifier(*source.subscriptionId, "subscription identifier");
  }
  result.telemetry_identifiers = read_optional_identifier_list(source.telemetryIdentifierList, "telemetry identifier");
  result.control_identifiers   = read_optional_identifier_list(source.controlIdentifierList, "control identifier");
  return result;
}

void set_message(LIBE3_E3_PDU_t& pdu, const e3ap_indication_message& source)
{
  auto& destination = *(pdu.msg.choice.indicationMessage = make_asn<LIBE3_E3_IndicationMessage_t>());
  pdu.msg.present   = LIBE3_E3_PDU__msg_PR_indicationMessage;
  require_whole(source.fragment_index, source.fragment_count, "indication");
  if (source.by_reference) {
    throw std::runtime_error("by-reference indications cannot be expressed in the libe3 module");
  }
  destination.dAppIdentifier        = identifier(source.dapp_identifier, "dApp identifier");
  destination.ranFunctionIdentifier = identifier(source.ran_function_identifier, "RAN function identifier");
  set_payload(destination.protocolData, source.protocol_data, "protocolData");
}

e3ap_indication_message read_message(const LIBE3_E3_IndicationMessage_t& source)
{
  e3ap_indication_message result;
  result.dapp_identifier         = read_identifier(source.dAppIdentifier, "dApp identifier");
  result.ran_function_identifier = read_identifier(source.ranFunctionIdentifier, "RAN function identifier");
  result.protocol_data           = read_payload(source.protocolData);
  result.payload_bytes           = static_cast<uint32_t>(result.protocol_data.size());
  return result;
}

void set_message(LIBE3_E3_PDU_t& pdu, const e3ap_dapp_control_action& source)
{
  auto& destination = *(pdu.msg.choice.dAppControlAction = make_asn<LIBE3_E3_DAppControlAction_t>());
  pdu.msg.present   = LIBE3_E3_PDU__msg_PR_dAppControlAction;
  require_whole(source.fragment_index, source.fragment_count, "control action");
  destination.dAppIdentifier        = identifier(source.dapp_identifier, "dApp identifier");
  destination.ranFunctionIdentifier = identifier(source.ran_function_identifier, "RAN function identifier");
  destination.controlIdentifier     = identifier(source.control_identifier, "control identifier");
  set_payload(destination.actionData, source.action_data, "actionData");
}

e3ap_dapp_control_action read_message(const LIBE3_E3_DAppControlAction_t& source)
{
  e3ap_dapp_control_action result;
  result.dapp_identifier         = read_identifier(source.dAppIdentifier, "dApp identifier");
  result.ran_function_identifier = read_identifier(source.ranFunctionIdentifier, "RAN function identifier");
  result.control_identifier      = read_identifier(source.controlIdentifier, "control identifier");
  result.action_data             = read_payload(source.actionData);
  result.payload_bytes           = static_cast<uint32_t>(result.action_data.size());
  return result;
}

void set_message(LIBE3_E3_PDU_t& pdu, const e3ap_dapp_report& source)
{
  auto& destination                 = *(pdu.msg.choice.dAppReport = make_asn<LIBE3_E3_DAppReport_t>());
  pdu.msg.present                   = LIBE3_E3_PDU__msg_PR_dAppReport;
  destination.dAppIdentifier        = identifier(source.dapp_identifier, "dApp identifier");
  destination.ranFunctionIdentifier = identifier(source.ran_function_identifier, "RAN function identifier");
  set_payload(destination.reportData, source.report_data, "reportData");
}

e3ap_dapp_report read_message(const LIBE3_E3_DAppReport_t& source)
{
  e3ap_dapp_report result;
  result.dapp_identifier         = read_identifier(source.dAppIdentifier, "dApp identifier");
  result.ran_function_identifier = read_identifier(source.ranFunctionIdentifier, "RAN function identifier");
  result.report_data             = read_payload(source.reportData);
  return result;
}

void set_message(LIBE3_E3_PDU_t& pdu, const e3ap_xapp_control_action& source)
{
  auto& destination                 = *(pdu.msg.choice.xAppControlAction = make_asn<LIBE3_E3_XAppControlAction_t>());
  pdu.msg.present                   = LIBE3_E3_PDU__msg_PR_xAppControlAction;
  destination.dAppIdentifier        = identifier(source.dapp_identifier, "dApp identifier");
  destination.ranFunctionIdentifier = identifier(source.ran_function_identifier, "RAN function identifier");
  set_payload(destination.xAppControlData, source.xapp_control_data, "xAppControlData");
}

e3ap_xapp_control_action read_message(const LIBE3_E3_XAppControlAction_t& source)
{
  e3ap_xapp_control_action result;
  result.dapp_identifier         = read_identifier(source.dAppIdentifier, "dApp identifier");
  result.ran_function_identifier = read_identifier(source.ranFunctionIdentifier, "RAN function identifier");
  result.xapp_control_data       = read_payload(source.xAppControlData);
  return result;
}

void set_message(LIBE3_E3_PDU_t& pdu, const e3ap_release_message& source)
{
  auto& destination          = *(pdu.msg.choice.releaseMessage = make_asn<LIBE3_E3_ReleaseMessage_t>());
  pdu.msg.present            = LIBE3_E3_PDU__msg_PR_releaseMessage;
  destination.dAppIdentifier = identifier(source.dapp_identifier, "dApp identifier");
}

e3ap_release_message read_message(const LIBE3_E3_ReleaseMessage_t& source)
{
  e3ap_release_message result;
  result.dapp_identifier = read_identifier(source.dAppIdentifier, "dApp identifier");
  return result;
}

void set_message(LIBE3_E3_PDU_t& pdu, const e3ap_message_ack& source)
{
  auto& destination        = *(pdu.msg.choice.messageAck = make_asn<LIBE3_E3_MessageAck_t>());
  pdu.msg.present          = LIBE3_E3_PDU__msg_PR_messageAck;
  destination.requestId    = message_id(source.request_id);
  destination.responseCode = to_wire(source.response_code);
}

e3ap_message_ack read_message(const LIBE3_E3_MessageAck_t& source)
{
  e3ap_message_ack result;
  result.request_id    = read_message_id(source.requestId);
  result.response_code = read_response_code(source.responseCode);
  return result;
}

void set_message(LIBE3_E3_PDU_t& pdu, const e3ap_control_response& source)
{
  static_cast<void>(pdu);
  static_cast<void>(source);
  throw std::runtime_error("E3-ControlResponse has no counterpart in the libe3 module; answer with E3-MessageAck");
}

} // namespace

codec_result<std::vector<uint8_t>> e3ap_libe3_codec::encode(const e3ap_pdu& pdu) noexcept
{
  try {
    asn_owner<LIBE3_E3_PDU_t> value(asn_DEF_LIBE3_E3_PDU, make_asn<LIBE3_E3_PDU_t>());
    auto&                     destination = *value.get();
    destination.id                        = message_id(pdu.id);
    if (pdu.timestamp_ns) {
      destination.timestamp  = make_asn<LIBE3_E3_Timestamp_t>();
      *destination.timestamp = static_cast<LIBE3_E3_Timestamp_t>(*pdu.timestamp_ns);
    }
    std::visit([&](const auto& message) { set_message(destination, message); }, pdu.message);
    return {encode_asn(asn_DEF_LIBE3_E3_PDU, destination, maximum_pdu_bytes), {}};
  } catch (const std::bad_alloc&) {
    return {std::nullopt, "APER encoding allocation failed"};
  } catch (const std::exception& exception) {
    return {std::nullopt, exception.what()};
  } catch (...) {
    return {std::nullopt, "unknown APER encoding failure"};
  }
}

codec_result<e3ap_pdu> e3ap_libe3_codec::decode(ocudu::span<const uint8_t> bytes) noexcept
{
  try {
    auto        owner = decode_asn<LIBE3_E3_PDU_t>(asn_DEF_LIBE3_E3_PDU, bytes, maximum_pdu_bytes, "libe3 E3-PDU");
    const auto& value = *owner.get();
    e3ap_pdu    result;
    result.id = read_message_id(value.id);
    if (value.timestamp != nullptr) {
      result.timestamp_ns = static_cast<int64_t>(*value.timestamp);
    }
    switch (value.msg.present) {
      case LIBE3_E3_PDU__msg_PR_setupRequest:
        result.message = read_message(*value.msg.choice.setupRequest);
        break;
      case LIBE3_E3_PDU__msg_PR_setupResponse:
        result.message = read_message(*value.msg.choice.setupResponse);
        break;
      case LIBE3_E3_PDU__msg_PR_subscriptionRequest:
        result.message = read_message(*value.msg.choice.subscriptionRequest);
        break;
      case LIBE3_E3_PDU__msg_PR_subscriptionDelete:
        result.message = read_message(*value.msg.choice.subscriptionDelete);
        break;
      case LIBE3_E3_PDU__msg_PR_subscriptionResponse:
        result.message = read_message(*value.msg.choice.subscriptionResponse);
        break;
      case LIBE3_E3_PDU__msg_PR_indicationMessage:
        result.message = read_message(*value.msg.choice.indicationMessage);
        break;
      case LIBE3_E3_PDU__msg_PR_dAppControlAction:
        result.message = read_message(*value.msg.choice.dAppControlAction);
        break;
      case LIBE3_E3_PDU__msg_PR_dAppReport:
        result.message = read_message(*value.msg.choice.dAppReport);
        break;
      case LIBE3_E3_PDU__msg_PR_xAppControlAction:
        result.message = read_message(*value.msg.choice.xAppControlAction);
        break;
      case LIBE3_E3_PDU__msg_PR_releaseMessage:
        result.message = read_message(*value.msg.choice.releaseMessage);
        break;
      case LIBE3_E3_PDU__msg_PR_messageAck:
        result.message = read_message(*value.msg.choice.messageAck);
        break;
      default:
        throw std::runtime_error("unknown libe3 E3-PDU message");
    }
    return {std::move(result), {}};
  } catch (const std::bad_alloc&) {
    return {std::nullopt, "APER decoding allocation failed"};
  } catch (const std::exception& exception) {
    return {std::nullopt, exception.what()};
  } catch (...) {
    return {std::nullopt, "unknown APER decoding failure"};
  }
}

bool e3ap_libe3_codec::representable(const e3ap_message& message, std::string* detail) noexcept
{
  e3ap_pdu probe;
  probe.id      = 1;
  probe.message = message;
  auto encoded  = encode(probe);
  if (!encoded && detail != nullptr) {
    *detail = encoded.detail;
  }
  return static_cast<bool>(encoded);
}
