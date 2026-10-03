// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/e3/e3ap_v2_codec.h"

extern "C" {
#include "E3V2_E3-Cause.h"
#include "E3V2_E3-ControlResponse.h"
#include "E3V2_E3-DAppControlAction.h"
#include "E3V2_E3-DAppReport.h"
#include "E3V2_E3-DeliveryMode.h"
#include "E3V2_E3-ElementType.h"
#include "E3V2_E3-IndicationMessage.h"
#include "E3V2_E3-MemoryDomain.h"
#include "E3V2_E3-MessageAck.h"
#include "E3V2_E3-PDU.h"
#include "E3V2_E3-RanFunctionDefinition.h"
#include "E3V2_E3-ReleaseMessage.h"
#include "E3V2_E3-ResponseCode.h"
#include "E3V2_E3-SetupRequest.h"
#include "E3V2_E3-SetupResponse.h"
#include "E3V2_E3-SharedRegion.h"
#include "E3V2_E3-SubscriptionDelete.h"
#include "E3V2_E3-SubscriptionRequest.h"
#include "E3V2_E3-SubscriptionResponse.h"
#include "E3V2_E3-TensorDescriptor.h"
#include "E3V2_E3-TensorLocation.h"
#include "E3V2_E3-Timestamp.h"
#include "E3V2_E3-Unsigned64.h"
#include "E3V2_E3-XAppControlAction.h"
}

#include "asn1_codec_internal.h"
#include <cstring>
#include <type_traits>

using namespace ocudu::dapp;
using namespace ocudu::dapp::e3;
using namespace ocudu::dapp::e3::asn1_detail;

namespace {

constexpr uint64_t    maximum_identifier    = e3ap_v2_codec::maximum_identifier;
constexpr uint64_t    maximum_message_id    = e3ap_v2_codec::maximum_message_id;
constexpr std::size_t maximum_payload_bytes = e3ap_v2_codec::maximum_payload_bytes;
constexpr std::size_t maximum_name_bytes    = e3ap_v2_codec::maximum_name_bytes;
constexpr std::size_t maximum_version_bytes = e3ap_v2_codec::maximum_version_bytes;
constexpr std::size_t maximum_text_bytes    = e3ap_v2_codec::maximum_text_bytes;
constexpr std::size_t maximum_list_entries  = e3ap_v2_codec::maximum_list_entries;
constexpr std::size_t maximum_ran_functions = 255;
// Root ranges of libe3's E3-SubscriptionRequest; wider values travel in the OCUDU additions.
constexpr uint32_t root_subscription_time = 3600;
constexpr uint32_t root_periodicity       = 1000;
constexpr uint32_t long_subscription_time = 86400;
constexpr uint32_t long_periodicity       = 60000;

/// What the current PDU may carry.
struct context {
  e3ap_profile profile;
  bool         ocudu() const noexcept { return profile == e3ap_profile::ocudu; }
};

// ---------------------------------------------------------------------------
// Small conversions
// ---------------------------------------------------------------------------

long to_wire(e3ap_response_code code) noexcept
{
  return code == e3ap_response_code::positive ? E3V2_E3_ResponseCode_positive : E3V2_E3_ResponseCode_negative;
}

e3ap_response_code read_response_code(long value)
{
  if (value == E3V2_E3_ResponseCode_positive) {
    return e3ap_response_code::positive;
  }
  if (value == E3V2_E3_ResponseCode_negative) {
    return e3ap_response_code::negative;
  }
  throw std::runtime_error("invalid E3 response code");
}

/// 1..65535 identifiers (dApp, RAN function, telemetry, control, subscription).
long identifier(uint64_t value, const char* what)
{
  if (value < 1 || value > maximum_identifier) {
    throw std::runtime_error(std::string(what) + " " + std::to_string(value) + " is outside 1..65535");
  }
  return static_cast<long>(value);
}

uint16_t read_identifier(long value, const char* what)
{
  return static_cast<uint16_t>(read_ranged(value, 1, maximum_identifier, what));
}

uint16_t read_u16(long value, const char* what)
{
  return static_cast<uint16_t>(read_ranged(value, 0, 65535, what));
}

unsigned long message_id(uint32_t value, const char* what = "message id")
{
  if (value < 1) {
    throw std::runtime_error(std::string(what) + " 0 is outside 1..4294967295");
  }
  return static_cast<unsigned long>(value);
}

uint32_t read_message_id(unsigned long value, const char* what = "message id")
{
  if (value < 1 || value > maximum_message_id) {
    throw std::runtime_error(std::string(what) + " " + std::to_string(value) + " is outside 1..4294967295");
  }
  return static_cast<uint32_t>(value);
}

/// E3-SequenceID: mandatory on E3-DAppReport and E3-XAppControlAction (libe3 >= 0.2 root fields).
unsigned long required_sequence(const std::optional<uint32_t>& value, const char* what)
{
  if (!value) {
    throw std::runtime_error(std::string(what) + " needs a sequence identifier");
  }
  return message_id(*value, "sequence identifier");
}

void set_bounded_text(UTF8String_t& destination, const std::string& value, std::size_t minimum, std::size_t maximum,
                      const char* what)
{
  if (value.size() < minimum || value.size() > maximum) {
    throw std::runtime_error(std::string(what) + " must be " + std::to_string(minimum) + ".." +
                             std::to_string(maximum) + " bytes");
  }
  set_text(destination, value);
}

void set_version(UTF8String_t& destination, const std::string& value, const char* what = "version text")
{
  set_bounded_text(destination, value, 1, maximum_version_bytes, what);
}

void set_name(UTF8String_t& destination, const std::string& value, const char* what = "name text")
{
  set_bounded_text(destination, value, 1, maximum_name_bytes, what);
}

void set_payload(OCTET_STRING_t& destination, const std::vector<uint8_t>& value, const char* what)
{
  if (value.empty() || value.size() > maximum_payload_bytes) {
    throw std::runtime_error(std::string(what) + " must be 1..32768 bytes");
  }
  set_octets(destination, value);
}

std::vector<uint8_t> read_payload(const OCTET_STRING_t& value)
{
  const auto bytes = read_bytes(value, maximum_payload_bytes);
  if (bytes.empty()) {
    throw std::runtime_error("E3 payload must not be empty");
  }
  return bytes;
}

template <typename List>
void set_identifier_list(const context& ctx, List& destination, const std::vector<uint16_t>& values, const char* what)
{
  std::size_t written = 0;
  for (const uint16_t value : values) {
    if (value < 1) {
      // A libe3 peer cannot name identifier 0 and cannot use it; an OCUDU peer must not send it.
      if (ctx.ocudu()) {
        throw std::runtime_error(std::string(what) + " 0 is outside 1..65535");
      }
      continue;
    }
    if (written == maximum_list_entries) {
      throw std::runtime_error(std::string(what) + " list exceeds 1000 entries");
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

template <typename T>
void set_optional(long*& destination, const std::optional<T>& value, uint64_t minimum, uint64_t maximum, const char* what)
{
  if (value) {
    const auto number = static_cast<uint64_t>(*value);
    if (number < minimum || number > maximum) {
      throw std::runtime_error(std::string(what) + " " + std::to_string(number) + " is outside its bound");
    }
    destination  = make_asn<long>();
    *destination = static_cast<long>(number);
  }
}

std::optional<uint32_t> read_optional(const long* source, uint64_t minimum, uint64_t maximum, const char* what)
{
  if (source == nullptr) {
    return std::nullopt;
  }
  return static_cast<uint32_t>(read_ranged(*source, minimum, maximum, what));
}

void set_optional_cause(E3V2_E3_Cause_t*& destination, const std::optional<e3ap_cause>& cause)
{
  if (cause) {
    if (static_cast<uint8_t>(*cause) > static_cast<uint8_t>(e3ap_cause::shutting_down)) {
      throw std::runtime_error("unknown E3 cause");
    }
    destination  = make_asn<E3V2_E3_Cause_t>();
    *destination = static_cast<long>(*cause);
  }
}

std::optional<e3ap_cause> read_optional_cause(const E3V2_E3_Cause_t* source)
{
  if (source == nullptr) {
    return std::nullopt;
  }
  if (*source < 0 || *source > static_cast<long>(e3ap_cause::shutting_down)) {
    // An extensible ENUMERATED: a newer peer's cause reads as unspecified.
    return e3ap_cause::unspecified;
  }
  return static_cast<e3ap_cause>(*source);
}

void set_optional_text(E3V2_E3_Text_t*& destination, const std::string& text)
{
  if (!text.empty()) {
    destination = make_asn<E3V2_E3_Text_t>();
    set_bounded_text(*destination, text, 0, maximum_text_bytes, "detail text");
  }
}

std::string read_optional_text(const E3V2_E3_Text_t* source)
{
  return source != nullptr ? read_string(*source, maximum_text_bytes) : std::string();
}

void set_optional_instance(E3V2_E3_Identifier_t*& destination, const std::optional<management::identifier>& value)
{
  if (value) {
    destination = make_asn<E3V2_E3_Identifier_t>();
    asn1_detail::set_identifier(*destination, *value);
  }
}

std::optional<management::identifier> read_optional_instance(const E3V2_E3_Identifier_t* source)
{
  if (source == nullptr) {
    return std::nullopt;
  }
  return asn1_detail::read_identifier(*source);
}

void set_optional_profile(long*& destination, const std::optional<uint8_t>& value)
{
  if (value && *value == 0) {
    throw std::runtime_error("ocuduProfile 0 is outside 1..255");
  }
  set_optional(destination, value, 1, 255, "ocuduProfile");
}

std::optional<uint8_t> read_optional_profile(const long* source)
{
  if (source == nullptr) {
    return std::nullopt;
  }
  return static_cast<uint8_t>(read_ranged(*source, 1, 255, "ocuduProfile"));
}

/// Fragment bookkeeping: carried only by OCUDU peers; libe3 carries whole payloads. A whole payload
/// writes nothing, so it carries no extension group.
void check_fragments(uint16_t fragment_index, uint32_t fragment_count, const char* what)
{
  if (fragment_count == 0 || fragment_count > 65536 || fragment_index >= fragment_count) {
    throw std::runtime_error(std::string(what) + " has invalid fragment fields");
  }
}

void set_fragments(const context&                 ctx,
                   long*&                         index,
                   long*&                         count,
                   long**                         bytes,
                   uint16_t                       fragment_index,
                   uint32_t                       fragment_count,
                   const std::optional<uint32_t>& payload_bytes,
                   const char*                    what)
{
  check_fragments(fragment_index, fragment_count, what);
  if (!ctx.ocudu()) {
    if (fragment_index != 0 || fragment_count != 1) {
      throw std::runtime_error(std::string(what) + " is fragmented; libe3 carries whole payloads only");
    }
    return;
  }
  if (fragment_index != 0) {
    index  = make_asn<long>();
    *index = fragment_index;
  }
  if (fragment_count != 1) {
    count  = make_asn<long>();
    *count = fragment_count;
  }
  if (bytes != nullptr) {
    set_optional(*bytes, payload_bytes, 1, e3ap_maximum_payload_bytes, "payload bytes");
  }
}

void read_fragments(const long* index, const long* count, uint16_t& fragment_index, uint32_t& fragment_count)
{
  fragment_index = index != nullptr ? read_u16(*index, "fragment index") : 0;
  fragment_count = count != nullptr ? static_cast<uint32_t>(read_ranged(*count, 1, 65536, "fragment count")) : 1;
  if (fragment_index >= fragment_count) {
    throw std::runtime_error("fragment index exceeds the fragment count");
  }
}

// ---------------------------------------------------------------------------
// By-reference bulk data (OCUDU additions)
// ---------------------------------------------------------------------------

e3ap_memory_domain read_memory_domain(long value)
{
  if (value < 0 || value > static_cast<long>(e3ap_memory_domain::accelerator_ipc_export)) {
    throw std::runtime_error("unknown E3 memory domain");
  }
  return static_cast<e3ap_memory_domain>(value);
}

e3ap_element_type read_element_type(long value)
{
  if (value < 0 || value > static_cast<long>(e3ap_element_type::complex_fp32)) {
    throw std::runtime_error("unknown E3 element type");
  }
  return static_cast<e3ap_element_type>(value);
}

void set_region(E3V2_E3_SharedRegion_t& destination, const e3ap_shared_region& source)
{
  if (source.row_bytes == 0 || source.nof_rows == 0) {
    throw std::runtime_error("shared region needs at least one non-empty row");
  }
  destination.memoryDomain = static_cast<long>(source.memory_domain);
  set_name(destination.regionName, source.region_name, "shared region name");
  set_u64(destination.regionBytes, source.region_bytes);
  destination.headerBytes = source.header_bytes;
  set_u64(destination.rowBytes, source.row_bytes);
  destination.nofRows       = source.nof_rows;
  destination.layoutVersion = source.layout_version;
}

e3ap_shared_region read_region(const E3V2_E3_SharedRegion_t& source)
{
  e3ap_shared_region result;
  result.memory_domain  = read_memory_domain(source.memoryDomain);
  result.region_name    = read_string(source.regionName, maximum_name_bytes);
  result.region_bytes   = read_u64(source.regionBytes);
  result.header_bytes   = read_u16(source.headerBytes, "shared region header bytes");
  result.row_bytes      = read_u64(source.rowBytes);
  result.nof_rows       = read_identifier(source.nofRows, "shared region rows");
  result.layout_version = read_u16(source.layoutVersion, "shared region layout version");
  if (result.region_name.empty() || result.row_bytes == 0) {
    throw std::runtime_error("shared region is malformed");
  }
  return result;
}

void set_descriptor(E3V2_E3_TensorDescriptor_t& destination, const e3ap_tensor_descriptor& source)
{
  destination.memoryDomain = static_cast<long>(source.memory_domain);
  destination.location     = make_asn<E3V2_E3_TensorLocation_t>();
  auto& location           = *destination.location;
  if (const auto* shared = std::get_if<e3ap_posix_shared_location>(&source.location)) {
    auto& wire = *(location.choice.posixShared = make_asn<std::remove_pointer_t<decltype(location.choice.posixShared)>>());
    location.present = E3V2_E3_TensorLocation_PR_posixShared;
    set_name(wire.regionName, shared->region_name, "tensor region name");
    wire.rowIndex = shared->row_index;
    set_u64(wire.byteOffset, shared->byte_offset);
  } else {
    const auto& ipc  = std::get<e3ap_accelerator_ipc_location>(source.location);
    auto&       wire = *(location.choice.acceleratorIpc =
                       make_asn<std::remove_pointer_t<decltype(location.choice.acceleratorIpc)>>());
    location.present = E3V2_E3_TensorLocation_PR_acceleratorIpc;
    wire.deviceOrdinal = ipc.device_ordinal;
    set_u64(wire.allocationId, ipc.allocation_id);
    set_octets(wire.memoryHandle, ipc.memory_handle.data(), ipc.memory_handle.size());
    if (ipc.ready_event_handle) {
      wire.readyEventHandle = make_asn<OCTET_STRING_t>();
      set_octets(*wire.readyEventHandle, ipc.ready_event_handle->data(), ipc.ready_event_handle->size());
    }
    set_u64(wire.byteOffset, ipc.byte_offset);
  }
  set_u64(destination.byteLength, source.byte_length);
  set_u64(destination.generation, source.generation);
  if (source.element_type) {
    destination.elementType  = make_asn<E3V2_E3_ElementType_t>();
    *destination.elementType = static_cast<long>(*source.element_type);
  }
  if (source.dimensions.size() > 8 || source.byte_strides.size() > 8) {
    throw std::runtime_error("tensor descriptor carries more than eight dimensions");
  }
  if (!source.dimensions.empty()) {
    destination.dimensions = make_asn<std::remove_pointer_t<decltype(destination.dimensions)>>();
    for (const uint32_t dimension : source.dimensions) {
      long* item = make_asn<long>();
      *item      = static_cast<long>(dimension);
      if (ASN_SEQUENCE_ADD(&destination.dimensions->list, item) != 0) {
        std::free(item);
        throw std::bad_alloc();
      }
    }
  }
  if (!source.byte_strides.empty()) {
    destination.byteStrides = make_asn<std::remove_pointer_t<decltype(destination.byteStrides)>>();
    for (const uint64_t stride : source.byte_strides) {
      auto* item = make_asn<E3V2_E3_Unsigned64_t>();
      set_u64(*item, stride);
      if (ASN_SEQUENCE_ADD(&destination.byteStrides->list, item) != 0) {
        ASN_STRUCT_FREE(asn_DEF_E3V2_E3_Unsigned64, item);
        throw std::bad_alloc();
      }
    }
  }
  set_optional(destination.layoutId, source.layout_id, 0, 65535, "layout id");
  if (source.lease_expiry_ns) {
    destination.leaseExpiryNs = make_asn<E3V2_E3_Unsigned64_t>();
    set_u64(*destination.leaseExpiryNs, *source.lease_expiry_ns);
  }
}

e3ap_tensor_descriptor read_descriptor(const E3V2_E3_TensorDescriptor_t& source)
{
  e3ap_tensor_descriptor result;
  result.memory_domain = read_memory_domain(source.memoryDomain);
  if (source.location == nullptr) {
    throw std::runtime_error("tensor descriptor misses its location");
  }
  switch (source.location->present) {
    case E3V2_E3_TensorLocation_PR_posixShared: {
      const auto&                wire = *source.location->choice.posixShared;
      e3ap_posix_shared_location location;
      location.region_name = read_string(wire.regionName, maximum_name_bytes);
      location.row_index   = read_u16(wire.rowIndex, "tensor row index");
      location.byte_offset = read_u64(wire.byteOffset);
      result.location      = location;
      break;
    }
    case E3V2_E3_TensorLocation_PR_acceleratorIpc: {
      const auto&                   wire = *source.location->choice.acceleratorIpc;
      e3ap_accelerator_ipc_location location;
      location.device_ordinal = read_u16(wire.deviceOrdinal, "tensor device ordinal");
      location.allocation_id  = read_u64(wire.allocationId);
      const auto handle       = read_bytes(wire.memoryHandle, 64);
      if (handle.size() != 64) {
        throw std::runtime_error("accelerator memory handle must be 64 bytes");
      }
      std::copy(handle.begin(), handle.end(), location.memory_handle.begin());
      if (wire.readyEventHandle != nullptr) {
        const auto event = read_bytes(*wire.readyEventHandle, 64);
        if (event.size() != 64) {
          throw std::runtime_error("accelerator event handle must be 64 bytes");
        }
        location.ready_event_handle.emplace();
        std::copy(event.begin(), event.end(), location.ready_event_handle->begin());
      }
      location.byte_offset = read_u64(wire.byteOffset);
      result.location      = location;
      break;
    }
    default:
      throw std::runtime_error("unknown tensor location");
  }
  result.byte_length = read_u64(source.byteLength);
  result.generation  = read_u64(source.generation);
  if (source.elementType != nullptr) {
    result.element_type = read_element_type(*source.elementType);
  }
  if (source.dimensions != nullptr) {
    if (source.dimensions->list.count < 0 || source.dimensions->list.count > 8) {
      throw std::runtime_error("tensor descriptor dimensions exceed their bound");
    }
    for (int index = 0; index != source.dimensions->list.count; ++index) {
      if (source.dimensions->list.array[index] == nullptr) {
        throw std::runtime_error("tensor dimension is missing");
      }
      result.dimensions.push_back(
          static_cast<uint32_t>(read_ranged(*source.dimensions->list.array[index], 0, 4294967295ULL, "dimension")));
    }
  }
  if (source.byteStrides != nullptr) {
    if (source.byteStrides->list.count < 0 || source.byteStrides->list.count > 8) {
      throw std::runtime_error("tensor descriptor strides exceed their bound");
    }
    for (int index = 0; index != source.byteStrides->list.count; ++index) {
      if (source.byteStrides->list.array[index] == nullptr) {
        throw std::runtime_error("tensor stride is missing");
      }
      result.byte_strides.push_back(read_u64(*source.byteStrides->list.array[index]));
    }
  }
  if (source.layoutId != nullptr) {
    result.layout_id = read_u16(*source.layoutId, "layout id");
  }
  if (source.leaseExpiryNs != nullptr) {
    result.lease_expiry_ns = read_u64(*source.leaseExpiryNs);
  }
  return result;
}

// ---------------------------------------------------------------------------
// Messages
// ---------------------------------------------------------------------------

void set_message(const context& ctx, E3V2_E3_PDU_t& pdu, const e3ap_setup_request& source)
{
  auto& destination = *(pdu.msg.choice.setupRequest = make_asn<E3V2_E3_SetupRequest_t>());
  pdu.msg.present   = E3V2_E3_PDU__msg_PR_setupRequest;
  set_version(destination.e3apProtocolVersion, source.e3ap_protocol_version, "e3apProtocolVersion");
  set_name(destination.dAppName, source.dapp_name, "dAppName");
  set_version(destination.dAppVersion, source.dapp_version, "dAppVersion");
  set_name(destination.vendor, source.vendor, "vendor");
  if (ctx.ocudu()) {
    set_optional_profile(destination.ocuduProfile, source.ocudu_profile);
    set_optional_instance(destination.dAppInstance, source.dapp_instance);
  }
}

e3ap_setup_request read_message(const context& ctx, const E3V2_E3_SetupRequest_t& source)
{
  e3ap_setup_request result;
  result.e3ap_protocol_version = read_string(source.e3apProtocolVersion, maximum_version_bytes);
  result.dapp_name             = read_string(source.dAppName, maximum_name_bytes);
  result.dapp_version          = read_string(source.dAppVersion, maximum_version_bytes);
  result.vendor                = read_string(source.vendor, maximum_name_bytes);
  if (ctx.ocudu()) {
    result.ocudu_profile = read_optional_profile(source.ocuduProfile);
    result.dapp_instance = read_optional_instance(source.dAppInstance);
  }
  return result;
}

void set_ran_function(const context&                      ctx,
                      E3V2_E3_RanFunctionDefinition_t&    destination,
                      const e3ap_ran_function_definition& source)
{
  destination.ranFunctionIdentifier = identifier(source.ran_function_identifier, "RAN function identifier");
  set_identifier_list(ctx, destination.telemetryIdentifierList, source.telemetry_identifiers, "telemetry identifier");
  set_identifier_list(ctx, destination.controlIdentifierList, source.control_identifiers, "control identifier");
  set_payload(destination.ranFunctionData, source.ran_function_data, "ranFunctionData");
  if (ctx.ocudu()) {
    if (!source.ran_function_name.empty()) {
      destination.ranFunctionName = make_asn<E3V2_E3_Name_t>();
      set_name(*destination.ranFunctionName, source.ran_function_name, "ranFunctionName");
    }
    if (!source.ran_function_version.empty()) {
      destination.ranFunctionVersion = make_asn<E3V2_E3_Version_t>();
      set_version(*destination.ranFunctionVersion, source.ran_function_version, "ranFunctionVersion");
    }
  }
}

e3ap_ran_function_definition read_ran_function(const context& ctx, const E3V2_E3_RanFunctionDefinition_t& source)
{
  e3ap_ran_function_definition result;
  result.ran_function_identifier = read_identifier(source.ranFunctionIdentifier, "RAN function identifier");
  result.telemetry_identifiers   = read_identifier_list(source.telemetryIdentifierList, "telemetry identifier");
  result.control_identifiers     = read_identifier_list(source.controlIdentifierList, "control identifier");
  result.ran_function_data       = read_payload(source.ranFunctionData);
  if (ctx.ocudu()) {
    if (source.ranFunctionName != nullptr) {
      result.ran_function_name = read_string(*source.ranFunctionName, maximum_name_bytes);
    }
    if (source.ranFunctionVersion != nullptr) {
      result.ran_function_version = read_string(*source.ranFunctionVersion, maximum_version_bytes);
    }
  }
  return result;
}

void set_message(const context& ctx, E3V2_E3_PDU_t& pdu, const e3ap_setup_response& source)
{
  auto& destination        = *(pdu.msg.choice.setupResponse = make_asn<E3V2_E3_SetupResponse_t>());
  pdu.msg.present          = E3V2_E3_PDU__msg_PR_setupResponse;
  destination.requestId    = message_id(source.request_id);
  destination.responseCode = to_wire(source.response_code);
  set_name(destination.ranIdentifier, source.ran_identifier, "ranIdentifier");
  if (source.e3ap_protocol_version) {
    destination.e3apProtocolVersion = make_asn<E3V2_E3_Version_t>();
    set_version(*destination.e3apProtocolVersion, *source.e3ap_protocol_version, "e3apProtocolVersion");
  }
  if (source.dapp_identifier) {
    destination.dAppIdentifier  = make_asn<E3V2_E3_DAppID_t>();
    *destination.dAppIdentifier = identifier(*source.dapp_identifier, "dApp identifier");
  }
  if (source.ran_functions_present || !source.ran_functions.empty()) {
    if (source.ran_functions.size() > maximum_ran_functions) {
      throw std::runtime_error("RAN function list exceeds its bound");
    }
    using list_type             = std::remove_pointer_t<decltype(destination.ranFunctionList)>;
    destination.ranFunctionList = make_asn<list_type>();
    for (const auto& function : source.ran_functions) {
      set_ran_function(ctx, append_asn<E3V2_E3_RanFunctionDefinition_t>(*destination.ranFunctionList), function);
    }
  }
  if (ctx.ocudu()) {
    set_optional_profile(destination.ocuduProfile, source.ocudu_profile);
    set_optional_cause(destination.cause, source.cause);
    set_optional_text(destination.detail, source.detail);
    set_optional_instance(destination.ranInstance, source.ran_instance);
    set_optional(destination.maximumPayloadBytes, source.maximum_payload_bytes, 1, e3ap_maximum_payload_bytes,
                 "maximumPayloadBytes");
    set_optional(destination.maximumFragmentBytes, source.maximum_fragment_bytes, 1, e3ap_maximum_fragment_bytes,
                 "maximumFragmentBytes");
  }
}

e3ap_setup_response read_message(const context& ctx, const E3V2_E3_SetupResponse_t& source)
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
      result.ran_functions.push_back(read_ran_function(ctx, *list.array[index]));
    }
  }
  if (ctx.ocudu()) {
    result.ocudu_profile          = read_optional_profile(source.ocuduProfile);
    result.cause                  = read_optional_cause(source.cause);
    result.detail                 = read_optional_text(source.detail);
    result.ran_instance           = read_optional_instance(source.ranInstance);
    result.maximum_payload_bytes  = read_optional(source.maximumPayloadBytes, 1, e3ap_maximum_payload_bytes,
                                                 "maximumPayloadBytes");
    result.maximum_fragment_bytes = read_optional(source.maximumFragmentBytes, 1, e3ap_maximum_fragment_bytes,
                                                  "maximumFragmentBytes");
  }
  return result;
}

void set_message(const context& ctx, E3V2_E3_PDU_t& pdu, const e3ap_subscription_request& source)
{
  auto& destination          = *(pdu.msg.choice.subscriptionRequest = make_asn<E3V2_E3_SubscriptionRequest_t>());
  pdu.msg.present            = E3V2_E3_PDU__msg_PR_subscriptionRequest;
  destination.dAppIdentifier = identifier(source.dapp_identifier, "dApp identifier");
  destination.ranFunctionIdentifier = identifier(source.ran_function_identifier, "RAN function identifier");
  set_identifier_list(ctx, destination.telemetryIdentifierList, source.telemetry_identifiers, "telemetry identifier");
  set_identifier_list(ctx, destination.controlIdentifierList, source.control_identifiers, "control identifier");
  // libe3's root ranges first; a wider value is an OCUDU addition with the root field absent.
  const auto place = [&](const std::optional<uint32_t>& value, long*& root, long*& wide, uint32_t root_maximum,
                         uint32_t wide_maximum, const char* what) {
    if (!value) {
      return;
    }
    if (*value <= root_maximum) {
      set_optional(root, value, 0, root_maximum, what);
    } else if (ctx.ocudu()) {
      set_optional(wide, value, 0, wide_maximum, what);
    } else {
      throw std::runtime_error(std::string(what) + " " + std::to_string(*value) + " exceeds libe3's bound");
    }
  };
  place(source.subscription_time_s, destination.subscriptionTime, destination.subscriptionTimeLong,
        root_subscription_time, long_subscription_time, "subscription time");
  place(source.periodicity_ms, destination.periodicity, destination.periodicityLong, root_periodicity,
        long_periodicity, "periodicity");
  if (ctx.ocudu()) {
    if (!source.subscription_data.empty()) {
      if (source.subscription_data.size() > e3ap_maximum_function_data) {
        throw std::runtime_error("subscriptionData exceeds its bound");
      }
      destination.subscriptionData = make_asn<OCTET_STRING_t>();
      set_octets(*destination.subscriptionData, source.subscription_data);
    }
    if (source.delivery_mode) {
      destination.deliveryMode  = make_asn<E3V2_E3_DeliveryMode_t>();
      *destination.deliveryMode = static_cast<long>(*source.delivery_mode);
    }
    set_optional(destination.minimumByReferenceBytes, source.minimum_by_reference_bytes, 0,
                 e3ap_maximum_payload_bytes, "minimumByReferenceBytes");
  }
}

e3ap_subscription_request read_message(const context& ctx, const E3V2_E3_SubscriptionRequest_t& source)
{
  e3ap_subscription_request result;
  result.dapp_identifier         = read_identifier(source.dAppIdentifier, "dApp identifier");
  result.ran_function_identifier = read_identifier(source.ranFunctionIdentifier, "RAN function identifier");
  result.telemetry_identifiers   = read_identifier_list(source.telemetryIdentifierList, "telemetry identifier");
  result.control_identifiers     = read_identifier_list(source.controlIdentifierList, "control identifier");
  result.subscription_time_s     = read_optional(source.subscriptionTime, 0, root_subscription_time, "subscription time");
  result.periodicity_ms          = read_optional(source.periodicity, 0, root_periodicity, "periodicity");
  if (ctx.ocudu()) {
    if (source.subscriptionTimeLong != nullptr) {
      result.subscription_time_s =
          read_optional(source.subscriptionTimeLong, 0, long_subscription_time, "subscription time");
    }
    if (source.periodicityLong != nullptr) {
      result.periodicity_ms = read_optional(source.periodicityLong, 0, long_periodicity, "periodicity");
    }
    if (source.subscriptionData != nullptr) {
      result.subscription_data = read_bytes(*source.subscriptionData, e3ap_maximum_function_data);
    }
    if (source.deliveryMode != nullptr) {
      if (*source.deliveryMode < 0 || *source.deliveryMode > static_cast<long>(e3ap_delivery_mode::by_reference)) {
        throw std::runtime_error("unknown E3 delivery mode");
      }
      result.delivery_mode = static_cast<e3ap_delivery_mode>(*source.deliveryMode);
    }
    result.minimum_by_reference_bytes =
        read_optional(source.minimumByReferenceBytes, 0, e3ap_maximum_payload_bytes, "minimumByReferenceBytes");
  }
  return result;
}

void set_message(const context&, E3V2_E3_PDU_t& pdu, const e3ap_subscription_delete& source)
{
  auto& destination          = *(pdu.msg.choice.subscriptionDelete = make_asn<E3V2_E3_SubscriptionDelete_t>());
  pdu.msg.present            = E3V2_E3_PDU__msg_PR_subscriptionDelete;
  destination.dAppIdentifier = identifier(source.dapp_identifier, "dApp identifier");
  destination.subscriptionId = identifier(source.subscription_id, "subscription identifier");
}

e3ap_subscription_delete read_message(const context&, const E3V2_E3_SubscriptionDelete_t& source)
{
  e3ap_subscription_delete result;
  result.dapp_identifier = read_identifier(source.dAppIdentifier, "dApp identifier");
  result.subscription_id = read_identifier(source.subscriptionId, "subscription identifier");
  return result;
}

void set_message(const context& ctx, E3V2_E3_PDU_t& pdu, const e3ap_subscription_response& source)
{
  auto& destination          = *(pdu.msg.choice.subscriptionResponse = make_asn<E3V2_E3_SubscriptionResponse_t>());
  pdu.msg.present            = E3V2_E3_PDU__msg_PR_subscriptionResponse;
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
    set_identifier_list(
        ctx, *destination.telemetryIdentifierList, *source.telemetry_identifiers, "telemetry identifier");
  }
  if (source.control_identifiers) {
    using list_type                   = std::remove_pointer_t<decltype(destination.controlIdentifierList)>;
    destination.controlIdentifierList = make_asn<list_type>();
    set_identifier_list(ctx, *destination.controlIdentifierList, *source.control_identifiers, "control identifier");
  }
  if (ctx.ocudu()) {
    set_optional_cause(destination.cause, source.cause);
    set_optional_text(destination.detail, source.detail);
    if (source.shared_region) {
      destination.sharedRegion = make_asn<E3V2_E3_SharedRegion_t>();
      set_region(*destination.sharedRegion, *source.shared_region);
    }
  }
}

e3ap_subscription_response read_message(const context& ctx, const E3V2_E3_SubscriptionResponse_t& source)
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
  if (ctx.ocudu()) {
    result.cause  = read_optional_cause(source.cause);
    result.detail = read_optional_text(source.detail);
    if (source.sharedRegion != nullptr) {
      result.shared_region = read_region(*source.sharedRegion);
    }
  }
  return result;
}

void set_message(const context& ctx, E3V2_E3_PDU_t& pdu, const e3ap_indication_message& source)
{
  auto& destination = *(pdu.msg.choice.indicationMessage = make_asn<E3V2_E3_IndicationMessage_t>());
  pdu.msg.present   = E3V2_E3_PDU__msg_PR_indicationMessage;
  if (source.by_reference && !ctx.ocudu()) {
    throw std::runtime_error("by-reference indications cannot be expressed in the libe3 module");
  }
  destination.dAppIdentifier        = identifier(source.dapp_identifier, "dApp identifier");
  destination.ranFunctionIdentifier = identifier(source.ran_function_identifier, "RAN function identifier");
  set_payload(destination.protocolData, source.protocol_data, "protocolData");
  set_fragments(ctx,
                destination.fragmentIndex,
                destination.fragmentCount,
                &destination.payloadBytes,
                source.fragment_index,
                source.fragment_count,
                source.payload_bytes,
                "indication");
  if (ctx.ocudu()) {
    if (source.subscription_id) {
      destination.subscriptionId  = make_asn<long>();
      *destination.subscriptionId = identifier(*source.subscription_id, "subscription identifier");
    }
    if (source.telemetry_identifier) {
      destination.telemetryIdentifier  = make_asn<long>();
      *destination.telemetryIdentifier = identifier(*source.telemetry_identifier, "telemetry identifier");
    }
    if (source.sequence) {
      destination.sequence = make_asn<E3V2_E3_Unsigned64_t>();
      set_u64(*destination.sequence, *source.sequence);
    }
    if (source.by_reference) {
      destination.byReference = make_asn<E3V2_E3_TensorDescriptor_t>();
      set_descriptor(*destination.byReference, *source.by_reference);
    }
  }
}

e3ap_indication_message read_message(const context& ctx, const E3V2_E3_IndicationMessage_t& source)
{
  e3ap_indication_message result;
  result.dapp_identifier         = read_identifier(source.dAppIdentifier, "dApp identifier");
  result.ran_function_identifier = read_identifier(source.ranFunctionIdentifier, "RAN function identifier");
  result.protocol_data           = read_payload(source.protocolData);
  if (!ctx.ocudu()) {
    result.payload_bytes = static_cast<uint32_t>(result.protocol_data.size());
    return result;
  }
  read_fragments(source.fragmentIndex, source.fragmentCount, result.fragment_index, result.fragment_count);
  result.payload_bytes = read_optional(source.payloadBytes, 1, e3ap_maximum_payload_bytes, "payload bytes");
  if (source.subscriptionId != nullptr) {
    result.subscription_id = read_identifier(*source.subscriptionId, "subscription identifier");
  }
  if (source.telemetryIdentifier != nullptr) {
    result.telemetry_identifier = read_identifier(*source.telemetryIdentifier, "telemetry identifier");
  }
  if (source.sequence != nullptr) {
    result.sequence = read_u64(*source.sequence);
  }
  if (source.byReference != nullptr) {
    result.by_reference = read_descriptor(*source.byReference);
  }
  return result;
}

void set_message(const context& ctx, E3V2_E3_PDU_t& pdu, const e3ap_dapp_control_action& source)
{
  auto& destination = *(pdu.msg.choice.dAppControlAction = make_asn<E3V2_E3_DAppControlAction_t>());
  pdu.msg.present   = E3V2_E3_PDU__msg_PR_dAppControlAction;
  destination.dAppIdentifier        = identifier(source.dapp_identifier, "dApp identifier");
  destination.ranFunctionIdentifier = identifier(source.ran_function_identifier, "RAN function identifier");
  destination.controlIdentifier     = identifier(source.control_identifier, "control identifier");
  if (source.sequence_identifier) {
    destination.sequenceId  = make_asn<E3V2_E3_SequenceID_t>();
    *destination.sequenceId = message_id(*source.sequence_identifier, "sequence identifier");
  }
  set_payload(destination.actionData, source.action_data, "actionData");
  set_fragments(ctx,
                destination.fragmentIndex,
                destination.fragmentCount,
                &destination.payloadBytes,
                source.fragment_index,
                source.fragment_count,
                source.payload_bytes,
                "control action");
}

e3ap_dapp_control_action read_message(const context& ctx, const E3V2_E3_DAppControlAction_t& source)
{
  e3ap_dapp_control_action result;
  result.dapp_identifier         = read_identifier(source.dAppIdentifier, "dApp identifier");
  result.ran_function_identifier = read_identifier(source.ranFunctionIdentifier, "RAN function identifier");
  result.control_identifier      = read_identifier(source.controlIdentifier, "control identifier");
  if (source.sequenceId != nullptr) {
    result.sequence_identifier = read_message_id(*source.sequenceId, "sequence identifier");
  }
  result.action_data = read_payload(source.actionData);
  if (!ctx.ocudu()) {
    result.payload_bytes = static_cast<uint32_t>(result.action_data.size());
    return result;
  }
  read_fragments(source.fragmentIndex, source.fragmentCount, result.fragment_index, result.fragment_count);
  result.payload_bytes = read_optional(source.payloadBytes, 1, e3ap_maximum_payload_bytes, "payload bytes");
  return result;
}

void set_message(const context&, E3V2_E3_PDU_t& pdu, const e3ap_dapp_report& source)
{
  auto& destination                 = *(pdu.msg.choice.dAppReport = make_asn<E3V2_E3_DAppReport_t>());
  pdu.msg.present                   = E3V2_E3_PDU__msg_PR_dAppReport;
  destination.dAppIdentifier        = identifier(source.dapp_identifier, "dApp identifier");
  destination.ranFunctionIdentifier = identifier(source.ran_function_identifier, "RAN function identifier");
  destination.sequenceId            = required_sequence(source.sequence_identifier, "E3-DAppReport");
  set_payload(destination.reportData, source.report_data, "reportData");
}

e3ap_dapp_report read_message(const context&, const E3V2_E3_DAppReport_t& source)
{
  e3ap_dapp_report result;
  result.dapp_identifier         = read_identifier(source.dAppIdentifier, "dApp identifier");
  result.ran_function_identifier = read_identifier(source.ranFunctionIdentifier, "RAN function identifier");
  result.sequence_identifier     = read_message_id(source.sequenceId, "sequence identifier");
  result.report_data             = read_payload(source.reportData);
  return result;
}

void set_message(const context&, E3V2_E3_PDU_t& pdu, const e3ap_xapp_control_action& source)
{
  auto& destination                 = *(pdu.msg.choice.xAppControlAction = make_asn<E3V2_E3_XAppControlAction_t>());
  pdu.msg.present                   = E3V2_E3_PDU__msg_PR_xAppControlAction;
  destination.dAppIdentifier        = identifier(source.dapp_identifier, "dApp identifier");
  destination.ranFunctionIdentifier = identifier(source.ran_function_identifier, "RAN function identifier");
  destination.sequenceId            = required_sequence(source.sequence_identifier, "E3-XAppControlAction");
  set_payload(destination.xAppControlData, source.xapp_control_data, "xAppControlData");
}

e3ap_xapp_control_action read_message(const context&, const E3V2_E3_XAppControlAction_t& source)
{
  e3ap_xapp_control_action result;
  result.dapp_identifier         = read_identifier(source.dAppIdentifier, "dApp identifier");
  result.ran_function_identifier = read_identifier(source.ranFunctionIdentifier, "RAN function identifier");
  result.sequence_identifier     = read_message_id(source.sequenceId, "sequence identifier");
  result.xapp_control_data       = read_payload(source.xAppControlData);
  return result;
}

void set_message(const context&, E3V2_E3_PDU_t& pdu, const e3ap_release_message& source)
{
  auto& destination          = *(pdu.msg.choice.releaseMessage = make_asn<E3V2_E3_ReleaseMessage_t>());
  pdu.msg.present            = E3V2_E3_PDU__msg_PR_releaseMessage;
  destination.dAppIdentifier = identifier(source.dapp_identifier, "dApp identifier");
}

e3ap_release_message read_message(const context&, const E3V2_E3_ReleaseMessage_t& source)
{
  e3ap_release_message result;
  result.dapp_identifier = read_identifier(source.dAppIdentifier, "dApp identifier");
  return result;
}

void set_message(const context& ctx, E3V2_E3_PDU_t& pdu, const e3ap_message_ack& source)
{
  auto& destination        = *(pdu.msg.choice.messageAck = make_asn<E3V2_E3_MessageAck_t>());
  pdu.msg.present          = E3V2_E3_PDU__msg_PR_messageAck;
  destination.requestId    = message_id(source.request_id);
  destination.responseCode = to_wire(source.response_code);
  if (ctx.ocudu()) {
    set_optional_cause(destination.cause, source.cause);
    set_optional_text(destination.detail, source.detail);
  }
}

e3ap_message_ack read_message(const context& ctx, const E3V2_E3_MessageAck_t& source)
{
  e3ap_message_ack result;
  result.request_id    = read_message_id(source.requestId);
  result.response_code = read_response_code(source.responseCode);
  if (ctx.ocudu()) {
    result.cause  = read_optional_cause(source.cause);
    result.detail = read_optional_text(source.detail);
  }
  return result;
}

void set_message(const context& ctx, E3V2_E3_PDU_t& pdu, const e3ap_control_response& source)
{
  if (!ctx.ocudu()) {
    throw std::runtime_error("E3-ControlResponse has no counterpart in the libe3 module; answer with E3-MessageAck");
  }
  auto& destination = *(pdu.msg.choice.controlResponse = make_asn<E3V2_E3_ControlResponse_t>());
  pdu.msg.present   = E3V2_E3_PDU__msg_PR_controlResponse;
  destination.requestId             = message_id(source.request_id);
  destination.dAppIdentifier        = identifier(source.dapp_identifier, "dApp identifier");
  destination.ranFunctionIdentifier = identifier(source.ran_function_identifier, "RAN function identifier");
  destination.controlIdentifier     = identifier(source.control_identifier, "control identifier");
  destination.responseCode          = to_wire(source.response_code);
  set_optional_cause(destination.cause, source.cause);
  set_optional_text(destination.detail, source.detail);
  if (!source.control_outcome.empty()) {
    destination.controlOutcome = make_asn<OCTET_STRING_t>();
    set_payload(*destination.controlOutcome, source.control_outcome, "controlOutcome");
  }
  check_fragments(source.fragment_index, source.fragment_count, "control response");
  destination.fragmentIndex = source.fragment_index;
  if (source.fragment_count != 1) {
    destination.fragmentCount  = make_asn<long>();
    *destination.fragmentCount = source.fragment_count;
  }
  if (source.payload_bytes > e3ap_maximum_payload_bytes) {
    throw std::runtime_error("control response payload bytes exceed their bound");
  }
  destination.payloadBytes = source.payload_bytes;
}

e3ap_control_response read_message(const context&, const E3V2_E3_ControlResponse_t& source)
{
  e3ap_control_response result;
  result.request_id              = read_message_id(source.requestId);
  result.dapp_identifier         = read_identifier(source.dAppIdentifier, "dApp identifier");
  result.ran_function_identifier = read_identifier(source.ranFunctionIdentifier, "RAN function identifier");
  result.control_identifier      = read_identifier(source.controlIdentifier, "control identifier");
  result.response_code           = read_response_code(source.responseCode);
  result.cause                   = read_optional_cause(source.cause);
  result.detail                  = read_optional_text(source.detail);
  if (source.controlOutcome != nullptr) {
    result.control_outcome = read_payload(*source.controlOutcome);
  }
  result.fragment_index = read_u16(source.fragmentIndex, "fragment index");
  result.fragment_count =
      source.fragmentCount != nullptr ? static_cast<uint32_t>(read_ranged(*source.fragmentCount, 1, 65536, "fragment count")) : 1;
  if (result.fragment_index >= result.fragment_count) {
    throw std::runtime_error("fragment index exceeds the fragment count");
  }
  result.payload_bytes =
      static_cast<uint32_t>(read_ranged(source.payloadBytes, 0, e3ap_maximum_payload_bytes, "payload bytes"));
  return result;
}

} // namespace

codec_result<std::vector<uint8_t>> e3ap_v2_codec::encode(const e3ap_pdu& pdu, e3ap_profile profile) noexcept
{
  try {
    const context              ctx{profile};
    asn_owner<E3V2_E3_PDU_t> value(asn_DEF_E3V2_E3_PDU, make_asn<E3V2_E3_PDU_t>());
    auto&                      destination = *value.get();
    destination.id                         = message_id(pdu.id);
    if (pdu.timestamp_ns) {
      destination.timestamp  = make_asn<E3V2_E3_Timestamp_t>();
      *destination.timestamp = static_cast<E3V2_E3_Timestamp_t>(*pdu.timestamp_ns);
    }
    std::visit([&](const auto& message) { set_message(ctx, destination, message); }, pdu.message);
    return {encode_asn(asn_DEF_E3V2_E3_PDU, destination, maximum_pdu_bytes), {}};
  } catch (const std::bad_alloc&) {
    return {std::nullopt, "APER encoding allocation failed"};
  } catch (const std::exception& exception) {
    return {std::nullopt, exception.what()};
  } catch (...) {
    return {std::nullopt, "unknown APER encoding failure"};
  }
}

codec_result<e3ap_pdu> e3ap_v2_codec::decode(ocudu::span<const uint8_t> bytes, e3ap_profile profile) noexcept
{
  try {
    const context ctx{profile};
    auto        owner = decode_asn<E3V2_E3_PDU_t>(asn_DEF_E3V2_E3_PDU, bytes, maximum_pdu_bytes, "E3-PDU");
    const auto& value = *owner.get();
    e3ap_pdu    result;
    result.id = read_message_id(value.id);
    if (value.timestamp != nullptr) {
      result.timestamp_ns = static_cast<int64_t>(*value.timestamp);
    }
    switch (value.msg.present) {
      case E3V2_E3_PDU__msg_PR_setupRequest:
        result.message = read_message(ctx, *value.msg.choice.setupRequest);
        break;
      case E3V2_E3_PDU__msg_PR_setupResponse:
        result.message = read_message(ctx, *value.msg.choice.setupResponse);
        break;
      case E3V2_E3_PDU__msg_PR_subscriptionRequest:
        result.message = read_message(ctx, *value.msg.choice.subscriptionRequest);
        break;
      case E3V2_E3_PDU__msg_PR_subscriptionDelete:
        result.message = read_message(ctx, *value.msg.choice.subscriptionDelete);
        break;
      case E3V2_E3_PDU__msg_PR_subscriptionResponse:
        result.message = read_message(ctx, *value.msg.choice.subscriptionResponse);
        break;
      case E3V2_E3_PDU__msg_PR_indicationMessage:
        result.message = read_message(ctx, *value.msg.choice.indicationMessage);
        break;
      case E3V2_E3_PDU__msg_PR_dAppControlAction:
        result.message = read_message(ctx, *value.msg.choice.dAppControlAction);
        break;
      case E3V2_E3_PDU__msg_PR_dAppReport:
        result.message = read_message(ctx, *value.msg.choice.dAppReport);
        break;
      case E3V2_E3_PDU__msg_PR_xAppControlAction:
        result.message = read_message(ctx, *value.msg.choice.xAppControlAction);
        break;
      case E3V2_E3_PDU__msg_PR_releaseMessage:
        result.message = read_message(ctx, *value.msg.choice.releaseMessage);
        break;
      case E3V2_E3_PDU__msg_PR_messageAck:
        result.message = read_message(ctx, *value.msg.choice.messageAck);
        break;
      case E3V2_E3_PDU__msg_PR_controlResponse:
        if (!ctx.ocudu()) {
          // libe3 would see an unknown CHOICE extension here.
          throw std::runtime_error("E3-ControlResponse is not a libe3 message");
        }
        result.message = read_message(ctx, *value.msg.choice.controlResponse);
        break;
      default:
        throw std::runtime_error("unknown E3-PDU message");
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

bool e3ap_v2_codec::representable(const e3ap_message& message, e3ap_profile profile, std::string* detail) noexcept
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
