// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/e3/e3ap_codec.h"

extern "C" {
#include "ClassCPortableRecord.h"
#include "E3-Cause.h"
#include "E3-ControlResponse.h"
#include "E3-DAppControlAction.h"
#include "E3-DAppReport.h"
#include "E3-DeliveryMode.h"
#include "E3-ElementType.h"
#include "E3-IdentifierList.h"
#include "E3-IndicationMessage.h"
#include "E3-MemoryDomain.h"
#include "E3-MessageAck.h"
#include "E3-PDU.h"
#include "E3-RanFunctionDefinition.h"
#include "E3-ReleaseMessage.h"
#include "E3-ResponseCode.h"
#include "E3-SetupRequest.h"
#include "E3-SetupResponse.h"
#include "E3-SharedRegion.h"
#include "E3-SubscriptionDelete.h"
#include "E3-SubscriptionRequest.h"
#include "E3-SubscriptionResponse.h"
#include "E3-TensorDescriptor.h"
#include "E3-TensorLocation.h"
#include "E3-Timestamp.h"
#include "E3-XAppControlAction.h"
#include "GnbMetricsJsonRecord.h"
#include "Mgmt-RanFunctionData.h"
#include "PuschCrcRecord.h"
#include "PuschDmrsChannelRecord.h"
#include "PuschGrantParameters.h"
#include "PuschGridRecord.h"
#include "PuschSymbolsRecord.h"
#include "RadioSlot.h"
#include "SrsChannelRecord.h"
#include "Telemetry-DropPolicy.h"
#include "Telemetry-IndicationData.h"
#include "Telemetry-Object.h"
#include "Telemetry-RanFunctionData.h"
#include "Telemetry-StreamDescriptor.h"
#include "Telemetry-SubscriptionConfig.h"
#include "Telemetry-SubscriptionData.h"
#include "aper_decoder.h"
#include "aper_encoder.h"
#include "asn_SEQUENCE_OF.h"
#include "constraints.h"
}

#include "asn1_codec_internal.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <type_traits>

using namespace ocudu::dapp;
using namespace ocudu::dapp::e3;
using namespace ocudu::dapp::e3::asn1_detail;

namespace {

constexpr std::size_t maximum_name_bytes        = 64;
constexpr std::size_t maximum_version_bytes     = 32;
constexpr std::size_t maximum_text_bytes        = 512;
constexpr std::size_t maximum_identifiers       = 256;
constexpr std::size_t maximum_ran_functions     = 64;
constexpr std::size_t maximum_telemetry_streams = 64;
constexpr std::size_t maximum_rnti_filter       = 32;
constexpr std::size_t maximum_telemetry_bytes   = e3ap_maximum_payload_bytes + 4096;

// ---------------------------------------------------------------------------
// Small conversions
// ---------------------------------------------------------------------------

long to_wire(e3ap_response_code code) noexcept
{
  return code == e3ap_response_code::positive ? E3_ResponseCode_positive : E3_ResponseCode_negative;
}

e3ap_response_code read_response_code(long value)
{
  if (value == E3_ResponseCode_positive) {
    return e3ap_response_code::positive;
  }
  if (value == E3_ResponseCode_negative) {
    return e3ap_response_code::negative;
  }
  throw std::runtime_error("invalid E3 response code");
}

long to_wire(e3ap_cause cause) noexcept
{
  return static_cast<long>(cause);
}

e3ap_cause read_cause(long value)
{
  if (value < E3_Cause_unspecified || value > E3_Cause_shuttingDown) {
    throw std::runtime_error("invalid E3 cause");
  }
  return static_cast<e3ap_cause>(value);
}

void set_optional_cause(E3_Cause_t*& destination, const std::optional<e3ap_cause>& cause)
{
  if (cause) {
    destination  = make_asn<E3_Cause_t>();
    *destination = to_wire(*cause);
  }
}

std::optional<e3ap_cause> read_optional_cause(const E3_Cause_t* source)
{
  return source != nullptr ? std::optional<e3ap_cause>(read_cause(*source)) : std::nullopt;
}

void set_optional_text(E3_Text_t*& destination, const std::string& text)
{
  if (!text.empty()) {
    destination = make_asn<E3_Text_t>();
    set_text(*destination, text);
  }
}

std::string read_optional_text(const E3_Text_t* source)
{
  return source != nullptr ? read_string(*source, maximum_text_bytes) : std::string{};
}

template <typename T>
void set_optional_long(long*& destination, const std::optional<T>& value)
{
  if (value) {
    destination = make_asn<long>();
    set_u64(*destination, static_cast<uint64_t>(*value));
  }
}

template <typename T>
void set_optional_typed(T*& destination, const std::optional<uint16_t>& value)
{
  if (value) {
    destination = make_asn<T>();
    set_u64(*destination, *value);
  }
}

uint16_t read_u16(long value, const char* what)
{
  return static_cast<uint16_t>(read_ranged(value, 0, 65535, what));
}

uint16_t read_id16(long value, const char* what)
{
  return static_cast<uint16_t>(read_ranged(value, 1, 65535, what));
}

void set_identifier_list(E3_IdentifierList_t*& destination, const std::vector<uint16_t>& values)
{
  if (values.size() > maximum_identifiers) {
    throw std::runtime_error("identifier list exceeds its bound");
  }
  destination = make_asn<E3_IdentifierList_t>();
  for (const uint16_t value : values) {
    if (value == 0) {
      throw std::runtime_error("identifier list entries must be positive");
    }
    long* item = make_asn<long>();
    *item      = value;
    if (ASN_SEQUENCE_ADD(&destination->list, item) != 0) {
      std::free(item);
      throw std::bad_alloc();
    }
  }
}

std::vector<uint16_t> read_identifier_list(const E3_IdentifierList_t& source)
{
  if (source.list.count < 0 || static_cast<std::size_t>(source.list.count) > maximum_identifiers) {
    throw std::runtime_error("identifier list exceeds its bound");
  }
  std::vector<uint16_t> result;
  result.reserve(static_cast<std::size_t>(source.list.count));
  for (int index = 0; index != source.list.count; ++index) {
    if (source.list.array[index] == nullptr) {
      throw std::runtime_error("identifier list entry is missing");
    }
    result.push_back(read_id16(*source.list.array[index], "identifier"));
  }
  return result;
}

void set_optional_u64(OCTET_STRING_t*& destination, const std::optional<uint64_t>& value)
{
  if (value) {
    destination = make_asn<OCTET_STRING_t>();
    set_u64(*destination, *value);
  }
}

void set_optional_identifier(OCTET_STRING_t*& destination, const std::optional<management::identifier>& value)
{
  if (value) {
    destination = make_asn<OCTET_STRING_t>();
    set_identifier(*destination, *value);
  }
}

void set_fragment_fields(long&                          index,
                         long*&                         count,
                         long*&                         bytes,
                         uint16_t                       fragment_index,
                         uint32_t                       fragment_count,
                         const std::optional<uint32_t>& payload_bytes)
{
  if (fragment_count == 0 || fragment_count > 65536 || fragment_index >= fragment_count ||
      (payload_bytes && (*payload_bytes == 0 || *payload_bytes > e3ap_maximum_payload_bytes))) {
    throw std::runtime_error("invalid E3 fragment fields");
  }
  index = fragment_index;
  if (fragment_count != 1) {
    count  = make_asn<long>();
    *count = fragment_count;
  }
  set_optional_long(bytes, payload_bytes);
}

void read_fragment_fields(long                     index,
                          const long*              count,
                          const long*              bytes,
                          uint16_t&                fragment_index,
                          uint32_t&                fragment_count,
                          std::optional<uint32_t>& payload_bytes)
{
  fragment_index = read_u16(index, "fragment index");
  fragment_count = count != nullptr ? static_cast<uint32_t>(read_ranged(*count, 1, 65536, "fragment count")) : 1;
  if (fragment_index >= fragment_count) {
    throw std::runtime_error("fragment index exceeds the fragment count");
  }
  payload_bytes = bytes != nullptr ? std::optional<uint32_t>(static_cast<uint32_t>(
                                         read_ranged(*bytes, 1, e3ap_maximum_payload_bytes, "payload bytes")))
                                   : std::nullopt;
}

void set_payload_fragment(OCTET_STRING_t& destination, const std::vector<uint8_t>& payload)
{
  if (payload.empty() || payload.size() > e3ap_maximum_fragment_bytes) {
    throw std::runtime_error("E3 payload fragment is empty or exceeds the fragment bound");
  }
  set_octets(destination, payload);
}

// ---------------------------------------------------------------------------
// E3AP messages
// ---------------------------------------------------------------------------

void set_ran_function(E3_RanFunctionDefinition_t& destination, const e3ap_ran_function_definition& source)
{
  set_u64(destination.ranFunctionIdentifier, read_ranged(source.ran_function_identifier, 1, 65535, "RAN function"));
  set_identifier_list(destination.telemetryIdentifierList, source.telemetry_identifiers);
  set_identifier_list(destination.controlIdentifierList, source.control_identifiers);
  if (source.ran_function_data.empty() || source.ran_function_data.size() > e3ap_maximum_function_data) {
    throw std::runtime_error("RAN function data is empty or exceeds its bound");
  }
  set_octets(destination.ranFunctionData, source.ran_function_data);
  if (!source.ran_function_name.empty()) {
    destination.ranFunctionName = make_asn<E3_Name_t>();
    set_text(*destination.ranFunctionName, source.ran_function_name);
  }
  if (!source.ran_function_version.empty()) {
    destination.ranFunctionVersion = make_asn<E3_Version_t>();
    set_text(*destination.ranFunctionVersion, source.ran_function_version);
  }
}

e3ap_ran_function_definition read_ran_function(const E3_RanFunctionDefinition_t& source)
{
  e3ap_ran_function_definition result;
  result.ran_function_identifier = read_id16(source.ranFunctionIdentifier, "RAN function");
  if (source.telemetryIdentifierList == nullptr || source.controlIdentifierList == nullptr) {
    throw std::runtime_error("RAN function definition misses its identifier lists");
  }
  result.telemetry_identifiers = read_identifier_list(*source.telemetryIdentifierList);
  result.control_identifiers   = read_identifier_list(*source.controlIdentifierList);
  result.ran_function_data     = read_bytes(source.ranFunctionData, e3ap_maximum_function_data);
  if (result.ran_function_data.empty()) {
    throw std::runtime_error("RAN function data is empty");
  }
  if (source.ranFunctionName != nullptr) {
    result.ran_function_name = read_string(*source.ranFunctionName, maximum_name_bytes);
  }
  if (source.ranFunctionVersion != nullptr) {
    result.ran_function_version = read_string(*source.ranFunctionVersion, maximum_version_bytes);
  }
  return result;
}

void set_message(E3_PDU_t& pdu, const e3ap_setup_request& source)
{
  auto& destination = *(pdu.msg.choice.setupRequest = make_asn<E3_SetupRequest_t>());
  pdu.msg.present   = E3_PDU__msg_PR_setupRequest;
  if (source.e3ap_protocol_version.empty() || source.dapp_name.empty() || source.dapp_version.empty() ||
      source.vendor.empty()) {
    throw std::runtime_error("E3 setup request requires version, name, and vendor");
  }
  set_text(destination.e3apProtocolVersion, source.e3ap_protocol_version);
  set_text(destination.dAppName, source.dapp_name);
  set_text(destination.dAppVersion, source.dapp_version);
  set_text(destination.vendor, source.vendor);
  set_optional_identifier(destination.dAppInstance, source.dapp_instance);
}

e3ap_setup_request read_message(const E3_SetupRequest_t& source)
{
  e3ap_setup_request result;
  result.e3ap_protocol_version = read_string(source.e3apProtocolVersion, maximum_version_bytes);
  result.dapp_name             = read_string(source.dAppName, maximum_name_bytes);
  result.dapp_version          = read_string(source.dAppVersion, maximum_version_bytes);
  result.vendor                = read_string(source.vendor, maximum_name_bytes);
  if (source.dAppInstance != nullptr) {
    result.dapp_instance = read_identifier(*source.dAppInstance);
  }
  return result;
}

void set_message(E3_PDU_t& pdu, const e3ap_setup_response& source)
{
  auto& destination = *(pdu.msg.choice.setupResponse = make_asn<E3_SetupResponse_t>());
  pdu.msg.present   = E3_PDU__msg_PR_setupResponse;
  set_u64(destination.requestId, read_ranged(source.request_id, 1, 4294967295ULL, "request id"));
  destination.responseCode = to_wire(source.response_code);
  if (source.ran_identifier.empty()) {
    throw std::runtime_error("E3 setup response requires a RAN identifier");
  }
  set_text(destination.ranIdentifier, source.ran_identifier);
  if (source.e3ap_protocol_version) {
    destination.e3apProtocolVersion = make_asn<E3_Version_t>();
    set_text(*destination.e3apProtocolVersion, *source.e3ap_protocol_version);
  }
  set_optional_typed(destination.dAppIdentifier, source.dapp_identifier);
  if (source.ran_functions_present || !source.ran_functions.empty()) {
    if (source.ran_functions.size() > maximum_ran_functions) {
      throw std::runtime_error("RAN function list exceeds its bound");
    }
    destination.ranFunctionList = make_asn<E3_SetupResponse::E3_SetupResponse__ranFunctionList>();
    for (const auto& function : source.ran_functions) {
      set_ran_function(append_asn<E3_RanFunctionDefinition_t>(*destination.ranFunctionList), function);
    }
  }
  set_optional_cause(destination.cause, source.cause);
  set_optional_text(destination.detail, source.detail);
  set_optional_identifier(destination.ranInstance, source.ran_instance);
  set_optional_long(destination.maximumPayloadBytes, source.maximum_payload_bytes);
  set_optional_long(destination.maximumFragmentBytes, source.maximum_fragment_bytes);
}

e3ap_setup_response read_message(const E3_SetupResponse_t& source)
{
  e3ap_setup_response result;
  result.request_id     = static_cast<uint32_t>(read_ranged(source.requestId, 1, 4294967295ULL, "request id"));
  result.response_code  = read_response_code(source.responseCode);
  result.ran_identifier = read_string(source.ranIdentifier, maximum_name_bytes);
  if (source.e3apProtocolVersion != nullptr) {
    result.e3ap_protocol_version = read_string(*source.e3apProtocolVersion, maximum_version_bytes);
  }
  if (source.dAppIdentifier != nullptr) {
    result.dapp_identifier = read_id16(*source.dAppIdentifier, "dApp identifier");
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
  result.cause  = read_optional_cause(source.cause);
  result.detail = read_optional_text(source.detail);
  if (source.ranInstance != nullptr) {
    result.ran_instance = read_identifier(*source.ranInstance);
  }
  if (source.maximumPayloadBytes != nullptr) {
    result.maximum_payload_bytes =
        static_cast<uint32_t>(read_ranged(*source.maximumPayloadBytes, 1, e3ap_maximum_payload_bytes, "payload bound"));
  }
  if (source.maximumFragmentBytes != nullptr) {
    result.maximum_fragment_bytes = static_cast<uint32_t>(
        read_ranged(*source.maximumFragmentBytes, 1, e3ap_maximum_fragment_bytes, "fragment bound"));
  }
  return result;
}

// ---------------------------------------------------------------------------
// By-reference bulk data: shared region and tensor descriptor
// ---------------------------------------------------------------------------

long to_wire(e3ap_memory_domain domain) noexcept
{
  return static_cast<long>(domain);
}

e3ap_memory_domain read_memory_domain(long value)
{
  if (value < 0 || value > static_cast<long>(e3ap_memory_domain::accelerator_ipc_export)) {
    throw std::runtime_error("unknown E3 memory domain");
  }
  return static_cast<e3ap_memory_domain>(value);
}

long to_wire(e3ap_element_type type) noexcept
{
  return static_cast<long>(type);
}

e3ap_element_type read_element_type(long value)
{
  if (value < 0 || value > static_cast<long>(e3ap_element_type::complex_fp32)) {
    throw std::runtime_error("unknown E3 element type");
  }
  return static_cast<e3ap_element_type>(value);
}

void set_region(E3_SharedRegion_t& destination, const e3ap_shared_region& source)
{
  if (source.region_name.empty() || source.region_name.size() > maximum_name_bytes) {
    throw std::runtime_error("shared region name is empty or too long");
  }
  if (source.row_bytes == 0 || source.nof_rows == 0) {
    throw std::runtime_error("shared region needs at least one non-empty row");
  }
  destination.memoryDomain = to_wire(source.memory_domain);
  set_text(destination.regionName, source.region_name);
  set_u64(destination.regionBytes, source.region_bytes);
  set_u64(destination.headerBytes, source.header_bytes);
  set_u64(destination.rowBytes, source.row_bytes);
  set_u64(destination.nofRows, read_ranged(source.nof_rows, 1, 65535, "shared region rows"));
  set_u64(destination.layoutVersion, source.layout_version);
}

e3ap_shared_region read_region(const E3_SharedRegion_t& source)
{
  e3ap_shared_region result;
  result.memory_domain  = read_memory_domain(source.memoryDomain);
  result.region_name    = read_string(source.regionName, maximum_name_bytes);
  result.region_bytes   = read_u64(source.regionBytes);
  result.header_bytes   = read_u16(source.headerBytes, "shared region header bytes");
  result.row_bytes      = read_u64(source.rowBytes);
  result.nof_rows       = read_id16(source.nofRows, "shared region rows");
  result.layout_version = read_u16(source.layoutVersion, "shared region layout version");
  if (result.region_name.empty() || result.row_bytes == 0) {
    throw std::runtime_error("shared region is malformed");
  }
  return result;
}

void set_descriptor(E3_TensorDescriptor_t& destination, const e3ap_tensor_descriptor& source)
{
  destination.memoryDomain = to_wire(source.memory_domain);
  destination.location     = make_asn<E3_TensorLocation_t>();
  std::visit(
      [&](const auto& location) {
        using location_type = std::decay_t<decltype(location)>;
        if constexpr (std::is_same_v<location_type, e3ap_posix_shared_location>) {
          if (location.region_name.empty() || location.region_name.size() > maximum_name_bytes) {
            throw std::runtime_error("tensor region name is empty or too long");
          }
          auto& shared                  = *(destination.location->choice.posixShared =
                               make_asn<std::remove_pointer_t<decltype(destination.location->choice.posixShared)>>());
          destination.location->present = E3_TensorLocation_PR_posixShared;
          set_text(shared.regionName, location.region_name);
          set_u64(shared.rowIndex, location.row_index);
          set_u64(shared.byteOffset, location.byte_offset);
        } else {
          auto& ipc                     = *(destination.location->choice.acceleratorIpc =
                            make_asn<std::remove_pointer_t<decltype(destination.location->choice.acceleratorIpc)>>());
          destination.location->present = E3_TensorLocation_PR_acceleratorIpc;
          set_u64(ipc.deviceOrdinal, location.device_ordinal);
          set_u64(ipc.allocationId, location.allocation_id);
          set_octets(ipc.memoryHandle, location.memory_handle.data(), location.memory_handle.size());
          if (location.ready_event_handle) {
            ipc.readyEventHandle = make_asn<OCTET_STRING_t>();
            set_octets(*ipc.readyEventHandle, location.ready_event_handle->data(), location.ready_event_handle->size());
          }
          set_u64(ipc.byteOffset, location.byte_offset);
        }
      },
      source.location);
  set_u64(destination.byteLength, source.byte_length);
  set_u64(destination.generation, source.generation);
  if (source.element_type) {
    destination.elementType  = make_asn<E3_ElementType_t>();
    *destination.elementType = to_wire(*source.element_type);
  }
  if (!source.dimensions.empty()) {
    if (source.dimensions.size() > 8) {
      throw std::runtime_error("tensor descriptor carries more than eight dimensions");
    }
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
    if (source.byte_strides.size() > 8) {
      throw std::runtime_error("tensor descriptor carries more than eight strides");
    }
    destination.byteStrides = make_asn<std::remove_pointer_t<decltype(destination.byteStrides)>>();
    for (const uint64_t stride : source.byte_strides) {
      auto* item = make_asn<E3_Unsigned64_t>();
      set_u64(*item, stride);
      if (ASN_SEQUENCE_ADD(&destination.byteStrides->list, item) != 0) {
        ASN_STRUCT_FREE(asn_DEF_E3_Unsigned64, item);
        throw std::bad_alloc();
      }
    }
  }
  set_optional_long(destination.layoutId, source.layout_id);
  set_optional_u64(destination.leaseExpiryNs, source.lease_expiry_ns);
}

e3ap_tensor_descriptor read_descriptor(const E3_TensorDescriptor_t& source)
{
  e3ap_tensor_descriptor result;
  result.memory_domain = read_memory_domain(source.memoryDomain);
  if (source.location == nullptr) {
    throw std::runtime_error("tensor descriptor misses its location");
  }
  switch (source.location->present) {
    case E3_TensorLocation_PR_posixShared: {
      const auto&                shared = *source.location->choice.posixShared;
      e3ap_posix_shared_location location;
      location.region_name = read_string(shared.regionName, maximum_name_bytes);
      location.row_index   = read_u16(shared.rowIndex, "tensor row index");
      location.byte_offset = read_u64(shared.byteOffset);
      result.location      = location;
      break;
    }
    case E3_TensorLocation_PR_acceleratorIpc: {
      const auto&                   ipc = *source.location->choice.acceleratorIpc;
      e3ap_accelerator_ipc_location location;
      location.device_ordinal = read_u16(ipc.deviceOrdinal, "tensor device ordinal");
      location.allocation_id  = read_u64(ipc.allocationId);
      const auto handle       = read_bytes(ipc.memoryHandle, 64);
      if (handle.size() != location.memory_handle.size()) {
        throw std::runtime_error("accelerator memory handle must be 64 bytes");
      }
      std::copy(handle.begin(), handle.end(), location.memory_handle.begin());
      if (ipc.readyEventHandle != nullptr) {
        const auto event = read_bytes(*ipc.readyEventHandle, 64);
        if (event.size() != 64) {
          throw std::runtime_error("accelerator event handle must be 64 bytes");
        }
        std::array<uint8_t, 64> value{};
        std::copy(event.begin(), event.end(), value.begin());
        location.ready_event_handle = value;
      }
      location.byte_offset = read_u64(ipc.byteOffset);
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

void set_message(E3_PDU_t& pdu, const e3ap_subscription_request& source)
{
  auto& destination = *(pdu.msg.choice.subscriptionRequest = make_asn<E3_SubscriptionRequest_t>());
  pdu.msg.present   = E3_PDU__msg_PR_subscriptionRequest;
  set_u64(destination.dAppIdentifier, read_ranged(source.dapp_identifier, 1, 65535, "dApp identifier"));
  set_u64(destination.ranFunctionIdentifier, read_ranged(source.ran_function_identifier, 1, 65535, "RAN function"));
  set_identifier_list(destination.telemetryIdentifierList, source.telemetry_identifiers);
  set_identifier_list(destination.controlIdentifierList, source.control_identifiers);
  if (source.subscription_time_s) {
    read_ranged(*source.subscription_time_s, 0, 86400, "subscription time");
  }
  if (source.periodicity_ms) {
    read_ranged(*source.periodicity_ms, 0, 60000, "periodicity");
  }
  set_optional_long(destination.subscriptionTime, source.subscription_time_s);
  set_optional_long(destination.periodicity, source.periodicity_ms);
  if (!source.subscription_data.empty()) {
    if (source.subscription_data.size() > e3ap_maximum_function_data) {
      throw std::runtime_error("subscription data exceeds its bound");
    }
    destination.subscriptionData = make_asn<OCTET_STRING_t>();
    set_octets(*destination.subscriptionData, source.subscription_data);
  }
  if (source.delivery_mode) {
    destination.deliveryMode  = make_asn<E3_DeliveryMode_t>();
    *destination.deliveryMode = static_cast<long>(*source.delivery_mode);
  }
  if (source.minimum_by_reference_bytes) {
    read_ranged(*source.minimum_by_reference_bytes, 0, e3ap_maximum_payload_bytes, "minimum by-reference bytes");
  }
  set_optional_long(destination.minimumByReferenceBytes, source.minimum_by_reference_bytes);
}

e3ap_subscription_request read_message(const E3_SubscriptionRequest_t& source)
{
  e3ap_subscription_request result;
  result.dapp_identifier         = read_id16(source.dAppIdentifier, "dApp identifier");
  result.ran_function_identifier = read_id16(source.ranFunctionIdentifier, "RAN function");
  if (source.telemetryIdentifierList == nullptr || source.controlIdentifierList == nullptr) {
    throw std::runtime_error("subscription request misses its identifier lists");
  }
  result.telemetry_identifiers = read_identifier_list(*source.telemetryIdentifierList);
  result.control_identifiers   = read_identifier_list(*source.controlIdentifierList);
  if (source.subscriptionTime != nullptr) {
    result.subscription_time_s =
        static_cast<uint32_t>(read_ranged(*source.subscriptionTime, 0, 86400, "subscription time"));
  }
  if (source.periodicity != nullptr) {
    result.periodicity_ms = static_cast<uint32_t>(read_ranged(*source.periodicity, 0, 60000, "periodicity"));
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
  if (source.minimumByReferenceBytes != nullptr) {
    result.minimum_by_reference_bytes = static_cast<uint32_t>(
        read_ranged(*source.minimumByReferenceBytes, 0, e3ap_maximum_payload_bytes, "minimum by-reference bytes"));
  }
  return result;
}

void set_message(E3_PDU_t& pdu, const e3ap_subscription_delete& source)
{
  auto& destination = *(pdu.msg.choice.subscriptionDelete = make_asn<E3_SubscriptionDelete_t>());
  pdu.msg.present   = E3_PDU__msg_PR_subscriptionDelete;
  set_u64(destination.dAppIdentifier, read_ranged(source.dapp_identifier, 1, 65535, "dApp identifier"));
  set_u64(destination.subscriptionId, read_ranged(source.subscription_id, 1, 65535, "subscription id"));
}

e3ap_subscription_delete read_message(const E3_SubscriptionDelete_t& source)
{
  e3ap_subscription_delete result;
  result.dapp_identifier = read_id16(source.dAppIdentifier, "dApp identifier");
  result.subscription_id = read_id16(source.subscriptionId, "subscription id");
  return result;
}

void set_message(E3_PDU_t& pdu, const e3ap_subscription_response& source)
{
  auto& destination = *(pdu.msg.choice.subscriptionResponse = make_asn<E3_SubscriptionResponse_t>());
  pdu.msg.present   = E3_PDU__msg_PR_subscriptionResponse;
  set_u64(destination.requestId, read_ranged(source.request_id, 1, 4294967295ULL, "request id"));
  set_u64(destination.dAppIdentifier, read_ranged(source.dapp_identifier, 1, 65535, "dApp identifier"));
  destination.responseCode = to_wire(source.response_code);
  set_optional_typed(destination.subscriptionId, source.subscription_id);
  if (source.telemetry_identifiers) {
    set_identifier_list(destination.telemetryIdentifierList, *source.telemetry_identifiers);
  }
  if (source.control_identifiers) {
    set_identifier_list(destination.controlIdentifierList, *source.control_identifiers);
  }
  set_optional_cause(destination.cause, source.cause);
  set_optional_text(destination.detail, source.detail);
  if (source.shared_region) {
    destination.sharedRegion = make_asn<E3_SharedRegion_t>();
    set_region(*destination.sharedRegion, *source.shared_region);
  }
}

e3ap_subscription_response read_message(const E3_SubscriptionResponse_t& source)
{
  e3ap_subscription_response result;
  result.request_id      = static_cast<uint32_t>(read_ranged(source.requestId, 1, 4294967295ULL, "request id"));
  result.dapp_identifier = read_id16(source.dAppIdentifier, "dApp identifier");
  result.response_code   = read_response_code(source.responseCode);
  if (source.subscriptionId != nullptr) {
    result.subscription_id = read_id16(*source.subscriptionId, "subscription id");
  }
  if (source.telemetryIdentifierList != nullptr) {
    result.telemetry_identifiers = read_identifier_list(*source.telemetryIdentifierList);
  }
  if (source.controlIdentifierList != nullptr) {
    result.control_identifiers = read_identifier_list(*source.controlIdentifierList);
  }
  result.cause  = read_optional_cause(source.cause);
  result.detail = read_optional_text(source.detail);
  if (source.sharedRegion != nullptr) {
    result.shared_region = read_region(*source.sharedRegion);
  }
  return result;
}

void set_message(E3_PDU_t& pdu, const e3ap_indication_message& source)
{
  auto& destination = *(pdu.msg.choice.indicationMessage = make_asn<E3_IndicationMessage_t>());
  pdu.msg.present   = E3_PDU__msg_PR_indicationMessage;
  set_u64(destination.dAppIdentifier, read_ranged(source.dapp_identifier, 1, 65535, "dApp identifier"));
  set_u64(destination.ranFunctionIdentifier, read_ranged(source.ran_function_identifier, 1, 65535, "RAN function"));
  set_payload_fragment(destination.protocolData, source.protocol_data);
  set_optional_typed(destination.subscriptionId, source.subscription_id);
  set_optional_typed(destination.telemetryIdentifier, source.telemetry_identifier);
  set_optional_u64(destination.sequence, source.sequence);
  set_fragment_fields(destination.fragmentIndex,
                      destination.fragmentCount,
                      destination.payloadBytes,
                      source.fragment_index,
                      source.fragment_count,
                      source.payload_bytes);
  if (source.by_reference) {
    destination.byReference = make_asn<E3_TensorDescriptor_t>();
    set_descriptor(*destination.byReference, *source.by_reference);
  }
}

e3ap_indication_message read_message(const E3_IndicationMessage_t& source)
{
  e3ap_indication_message result;
  result.dapp_identifier         = read_id16(source.dAppIdentifier, "dApp identifier");
  result.ran_function_identifier = read_id16(source.ranFunctionIdentifier, "RAN function");
  result.protocol_data           = read_bytes(source.protocolData, e3ap_maximum_fragment_bytes);
  if (result.protocol_data.empty()) {
    throw std::runtime_error("indication payload is empty");
  }
  if (source.subscriptionId != nullptr) {
    result.subscription_id = read_id16(*source.subscriptionId, "subscription id");
  }
  if (source.telemetryIdentifier != nullptr) {
    result.telemetry_identifier = read_id16(*source.telemetryIdentifier, "telemetry identifier");
  }
  if (source.sequence != nullptr) {
    result.sequence = read_u64(*source.sequence);
  }
  read_fragment_fields(source.fragmentIndex,
                       source.fragmentCount,
                       source.payloadBytes,
                       result.fragment_index,
                       result.fragment_count,
                       result.payload_bytes);
  if (source.byReference != nullptr) {
    result.by_reference = read_descriptor(*source.byReference);
  }
  return result;
}

void set_message(E3_PDU_t& pdu, const e3ap_dapp_control_action& source)
{
  auto& destination = *(pdu.msg.choice.dAppControlAction = make_asn<E3_DAppControlAction_t>());
  pdu.msg.present   = E3_PDU__msg_PR_dAppControlAction;
  set_u64(destination.dAppIdentifier, read_ranged(source.dapp_identifier, 1, 65535, "dApp identifier"));
  set_u64(destination.ranFunctionIdentifier, read_ranged(source.ran_function_identifier, 1, 65535, "RAN function"));
  set_u64(destination.controlIdentifier, read_ranged(source.control_identifier, 1, 65535, "control identifier"));
  set_payload_fragment(destination.actionData, source.action_data);
  set_fragment_fields(destination.fragmentIndex,
                      destination.fragmentCount,
                      destination.payloadBytes,
                      source.fragment_index,
                      source.fragment_count,
                      source.payload_bytes);
}

e3ap_dapp_control_action read_message(const E3_DAppControlAction_t& source)
{
  e3ap_dapp_control_action result;
  result.dapp_identifier         = read_id16(source.dAppIdentifier, "dApp identifier");
  result.ran_function_identifier = read_id16(source.ranFunctionIdentifier, "RAN function");
  result.control_identifier      = read_id16(source.controlIdentifier, "control identifier");
  result.action_data             = read_bytes(source.actionData, e3ap_maximum_fragment_bytes);
  if (result.action_data.empty()) {
    throw std::runtime_error("control action payload is empty");
  }
  read_fragment_fields(source.fragmentIndex,
                       source.fragmentCount,
                       source.payloadBytes,
                       result.fragment_index,
                       result.fragment_count,
                       result.payload_bytes);
  return result;
}

void set_message(E3_PDU_t& pdu, const e3ap_control_response& source)
{
  auto& destination = *(pdu.msg.choice.controlResponse = make_asn<E3_ControlResponse_t>());
  pdu.msg.present   = E3_PDU__msg_PR_controlResponse;
  set_u64(destination.requestId, read_ranged(source.request_id, 1, 4294967295ULL, "request id"));
  set_u64(destination.dAppIdentifier, read_ranged(source.dapp_identifier, 1, 65535, "dApp identifier"));
  set_u64(destination.ranFunctionIdentifier, read_ranged(source.ran_function_identifier, 1, 65535, "RAN function"));
  set_u64(destination.controlIdentifier, read_ranged(source.control_identifier, 1, 65535, "control identifier"));
  destination.responseCode = to_wire(source.response_code);
  set_optional_cause(destination.cause, source.cause);
  set_optional_text(destination.detail, source.detail);
  if (!source.control_outcome.empty()) {
    destination.controlOutcome = make_asn<E3_PayloadFragment_t>();
    set_payload_fragment(*destination.controlOutcome, source.control_outcome);
  }
  if (source.fragment_count == 0 || source.fragment_count > 65536 || source.fragment_index >= source.fragment_count ||
      source.payload_bytes > e3ap_maximum_payload_bytes ||
      (source.control_outcome.empty() && (source.payload_bytes != 0 || source.fragment_count != 1))) {
    throw std::runtime_error("invalid E3 control response fragment fields");
  }
  destination.fragmentIndex = source.fragment_index;
  if (source.fragment_count != 1) {
    destination.fragmentCount  = make_asn<long>();
    *destination.fragmentCount = source.fragment_count;
  }
  destination.payloadBytes = source.payload_bytes;
}

e3ap_control_response read_message(const E3_ControlResponse_t& source)
{
  e3ap_control_response result;
  result.request_id              = static_cast<uint32_t>(read_ranged(source.requestId, 1, 4294967295ULL, "request id"));
  result.dapp_identifier         = read_id16(source.dAppIdentifier, "dApp identifier");
  result.ran_function_identifier = read_id16(source.ranFunctionIdentifier, "RAN function");
  result.control_identifier      = read_id16(source.controlIdentifier, "control identifier");
  result.response_code           = read_response_code(source.responseCode);
  result.cause                   = read_optional_cause(source.cause);
  result.detail                  = read_optional_text(source.detail);
  if (source.controlOutcome != nullptr) {
    result.control_outcome = read_bytes(*source.controlOutcome, e3ap_maximum_fragment_bytes);
  }
  result.fragment_index = read_u16(source.fragmentIndex, "fragment index");
  result.fragment_count = source.fragmentCount != nullptr
                              ? static_cast<uint32_t>(read_ranged(*source.fragmentCount, 1, 65536, "fragment count"))
                              : 1;
  result.payload_bytes =
      static_cast<uint32_t>(read_ranged(source.payloadBytes, 0, e3ap_maximum_payload_bytes, "payload bytes"));
  if (result.fragment_index >= result.fragment_count ||
      (result.control_outcome.empty() && (result.payload_bytes != 0 || result.fragment_count != 1))) {
    throw std::runtime_error("invalid E3 control response fragment fields");
  }
  return result;
}

void set_message(E3_PDU_t& pdu, const e3ap_dapp_report& source)
{
  auto& destination = *(pdu.msg.choice.dAppReport = make_asn<E3_DAppReport_t>());
  pdu.msg.present   = E3_PDU__msg_PR_dAppReport;
  set_u64(destination.dAppIdentifier, read_ranged(source.dapp_identifier, 1, 65535, "dApp identifier"));
  set_u64(destination.ranFunctionIdentifier, read_ranged(source.ran_function_identifier, 1, 65535, "RAN function"));
  set_payload_fragment(destination.reportData, source.report_data);
}

e3ap_dapp_report read_message(const E3_DAppReport_t& source)
{
  e3ap_dapp_report result;
  result.dapp_identifier         = read_id16(source.dAppIdentifier, "dApp identifier");
  result.ran_function_identifier = read_id16(source.ranFunctionIdentifier, "RAN function");
  result.report_data             = read_bytes(source.reportData, e3ap_maximum_fragment_bytes);
  return result;
}

void set_message(E3_PDU_t& pdu, const e3ap_xapp_control_action& source)
{
  auto& destination = *(pdu.msg.choice.xAppControlAction = make_asn<E3_XAppControlAction_t>());
  pdu.msg.present   = E3_PDU__msg_PR_xAppControlAction;
  set_u64(destination.dAppIdentifier, read_ranged(source.dapp_identifier, 1, 65535, "dApp identifier"));
  set_u64(destination.ranFunctionIdentifier, read_ranged(source.ran_function_identifier, 1, 65535, "RAN function"));
  set_payload_fragment(destination.xAppControlData, source.xapp_control_data);
}

e3ap_xapp_control_action read_message(const E3_XAppControlAction_t& source)
{
  e3ap_xapp_control_action result;
  result.dapp_identifier         = read_id16(source.dAppIdentifier, "dApp identifier");
  result.ran_function_identifier = read_id16(source.ranFunctionIdentifier, "RAN function");
  result.xapp_control_data       = read_bytes(source.xAppControlData, e3ap_maximum_fragment_bytes);
  return result;
}

void set_message(E3_PDU_t& pdu, const e3ap_release_message& source)
{
  auto& destination = *(pdu.msg.choice.releaseMessage = make_asn<E3_ReleaseMessage_t>());
  pdu.msg.present   = E3_PDU__msg_PR_releaseMessage;
  set_u64(destination.dAppIdentifier, read_ranged(source.dapp_identifier, 1, 65535, "dApp identifier"));
}

e3ap_release_message read_message(const E3_ReleaseMessage_t& source)
{
  return {read_id16(source.dAppIdentifier, "dApp identifier")};
}

void set_message(E3_PDU_t& pdu, const e3ap_message_ack& source)
{
  auto& destination = *(pdu.msg.choice.messageAck = make_asn<E3_MessageAck_t>());
  pdu.msg.present   = E3_PDU__msg_PR_messageAck;
  set_u64(destination.requestId, read_ranged(source.request_id, 1, 4294967295ULL, "request id"));
  destination.responseCode = to_wire(source.response_code);
  set_optional_cause(destination.cause, source.cause);
  set_optional_text(destination.detail, source.detail);
}

e3ap_message_ack read_message(const E3_MessageAck_t& source)
{
  e3ap_message_ack result;
  result.request_id    = static_cast<uint32_t>(read_ranged(source.requestId, 1, 4294967295ULL, "request id"));
  result.response_code = read_response_code(source.responseCode);
  result.cause         = read_optional_cause(source.cause);
  result.detail        = read_optional_text(source.detail);
  return result;
}

// ---------------------------------------------------------------------------
// OCUDU-TELEMETRY-SM
// ---------------------------------------------------------------------------

void set_radio_slot(RadioSlot_t*& destination, const telemetry_radio_slot_value& source)
{
  destination = make_asn<RadioSlot_t>();
  set_u64(destination->sfn, read_ranged(source.sfn, 0, 1023, "SFN"));
  set_u64(destination->slot, read_ranged(source.slot, 0, 159, "slot"));
  set_u64(destination->numerology, read_ranged(source.numerology, 0, 4, "numerology"));
}

telemetry_radio_slot_value read_radio_slot(const RadioSlot_t* source)
{
  if (source == nullptr) {
    throw std::runtime_error("radio slot is missing");
  }
  telemetry_radio_slot_value result;
  result.sfn        = static_cast<uint16_t>(read_ranged(source->sfn, 0, 1023, "SFN"));
  result.slot       = static_cast<uint8_t>(read_ranged(source->slot, 0, 159, "slot"));
  result.numerology = static_cast<uint8_t>(read_ranged(source->numerology, 0, 4, "numerology"));
  return result;
}

void set_optional_real(double*& destination, const std::optional<double>& value)
{
  if (value && std::isfinite(*value)) {
    destination  = make_asn<double>();
    *destination = *value;
  }
}

std::optional<double> read_optional_real(const double* source)
{
  if (source == nullptr) {
    return std::nullopt;
  }
  if (!std::isfinite(*source)) {
    throw std::runtime_error("non-finite real value");
  }
  return *source;
}

template <typename T>
void set_optional_small(long*& destination, const std::optional<T>& value, uint64_t maximum, const char* what)
{
  if (value) {
    destination = make_asn<long>();
    set_u64(*destination, read_ranged(*value, 0, maximum, what));
  }
}

template <typename T>
std::optional<T> read_optional_small(const long* source, uint64_t maximum, const char* what)
{
  if (source == nullptr) {
    return std::nullopt;
  }
  return static_cast<T>(read_ranged(*source, 0, maximum, what));
}

void set_samples(OCTET_STRING_t& destination, const std::vector<uint8_t>& samples)
{
  if (samples.size() > e3ap_maximum_payload_bytes || samples.size() % (2 * sizeof(float)) != 0) {
    throw std::runtime_error("complex sample vector must hold whole binary32 pairs within the bound");
  }
  set_octets(destination, samples);
}

std::vector<uint8_t> read_samples(const OCTET_STRING_t& source)
{
  auto bytes = read_bytes(source, e3ap_maximum_payload_bytes);
  if (bytes.size() % (2 * sizeof(float)) != 0) {
    throw std::runtime_error("complex sample vector must hold whole binary32 pairs");
  }
  return bytes;
}

pusch_grant_parameters grant_from_packed(const pusch_grant_metadata_v1& source)
{
  pusch_grant_parameters result;
  result.harq_id         = source.harq_id;
  result.n_id            = source.n_id;
  result.tbs_bytes       = source.tbs_bytes;
  result.rv              = source.rv;
  result.new_data        = source.new_data != 0;
  result.ldpc_base_graph = source.ldpc_base_graph;
  result.tbs_lbrm_bytes  = source.tbs_lbrm_bytes;
  if ((source.flags & 0x01U) != 0) {
    result.mcs_index = source.mcs_index;
    result.mcs_table = source.mcs_table;
  }
  result.transform_precoding = (source.flags & 0x02U) != 0;
  if (result.transform_precoding) {
    result.n_rs_id = source.n_rs_id;
  }
  result.cyclic_prefix               = source.cyclic_prefix;
  result.start_symbol                = source.start_symbol;
  result.nof_symbols                 = source.nof_symbols;
  result.dmrs_symbol_mask            = source.dmrs_symbol_mask;
  result.dmrs_type                   = source.dmrs_type;
  result.nof_cdm_groups_without_data = source.nof_cdm_groups_without_data;
  result.dmrs_scrambling_id          = source.dmrs_scrambling_id;
  result.n_scid                      = (source.flags & 0x04U) != 0;
  result.bwp_start_rb                = source.bwp_start_rb;
  result.bwp_size_rb                 = source.bwp_size_rb;
  std::copy(std::begin(source.prb_mask), std::end(source.prb_mask), result.prb_mask.begin());
  result.nof_rx_ports = source.nof_rx_ports;
  result.nof_layers   = source.nof_layers;
  if ((source.flags & 0x08U) != 0) {
    result.dc_position = source.dc_position;
  }
  result.nof_harq_ack_bits     = source.nof_harq_ack_bits;
  result.nof_csi_part1_bits    = source.nof_csi_part1_bits;
  result.csi_part2_expected    = (source.flags & 0x10U) != 0;
  result.alpha_scaling         = source.alpha_scaling;
  result.beta_offset_harq_ack  = source.beta_offset_harq_ack;
  result.beta_offset_csi_part1 = source.beta_offset_csi_part1;
  result.beta_offset_csi_part2 = source.beta_offset_csi_part2;
  return result;
}

void set_grant(PuschGrantParameters_t& destination, const pusch_grant_parameters& source)
{
  set_u64(destination.harqId, read_ranged(source.harq_id, 0, 15, "HARQ id"));
  set_u64(destination.nId, read_ranged(source.n_id, 0, 1023, "scrambling identity"));
  set_u64(destination.tbsBytes, read_ranged(source.tbs_bytes, 0, 16777215, "TBS"));
  set_u64(destination.rvIndex, read_ranged(source.rv, 0, 3, "RV"));
  destination.newData = source.new_data ? 1 : 0;
  set_u64(destination.ldpcBaseGraph, read_ranged(source.ldpc_base_graph, 1, 2, "LDPC base graph"));
  set_u64(destination.tbsLbrmBytes, read_ranged(source.tbs_lbrm_bytes, 0, 16777215, "LBRM TBS"));
  set_optional_long(destination.mcsIndex, source.mcs_index);
  set_optional_long(destination.mcsTable, source.mcs_table);
  destination.transformPrecoding = source.transform_precoding ? 1 : 0;
  set_optional_long(destination.nRsId, source.n_rs_id);
  set_u64(destination.cyclicPrefix, read_ranged(source.cyclic_prefix, 0, 1, "cyclic prefix"));
  set_u64(destination.startSymbol, read_ranged(source.start_symbol, 0, 13, "start symbol"));
  set_u64(destination.nofSymbols, read_ranged(source.nof_symbols, 1, 14, "symbol count"));
  set_u64(destination.dmrsSymbolMask, read_ranged(source.dmrs_symbol_mask, 0, 16383, "DM-RS symbol mask"));
  set_u64(destination.dmrsType, read_ranged(source.dmrs_type, 1, 2, "DM-RS type"));
  set_u64(destination.nofCdmGroupsWithoutData,
          read_ranged(source.nof_cdm_groups_without_data, 1, 3, "CDM groups without data"));
  set_u64(destination.dmrsScramblingId, source.dmrs_scrambling_id);
  destination.nScid = source.n_scid ? 1 : 0;
  set_u64(destination.bwpStartRb, read_ranged(source.bwp_start_rb, 0, 274, "BWP start"));
  set_u64(destination.bwpSizeRb, read_ranged(source.bwp_size_rb, 1, 275, "BWP size"));
  set_octets(destination.prbMask, source.prb_mask.data(), source.prb_mask.size());
  set_u64(destination.nofRxPorts, read_ranged(source.nof_rx_ports, 0, 8, "receive port count"));
  set_u64(destination.nofLayers, read_ranged(source.nof_layers, 0, 4, "layer count"));
  set_optional_long(destination.dcPosition, source.dc_position);
  set_u64(destination.nofHarqAckBits, read_ranged(source.nof_harq_ack_bits, 0, 1706, "HARQ-ACK bits"));
  set_u64(destination.nofCsiPart1Bits, read_ranged(source.nof_csi_part1_bits, 0, 1706, "CSI part 1 bits"));
  destination.csiPart2Expected   = source.csi_part2_expected ? 1 : 0;
  destination.alphaScaling       = source.alpha_scaling;
  destination.betaOffsetHarqAck  = source.beta_offset_harq_ack;
  destination.betaOffsetCsiPart1 = source.beta_offset_csi_part1;
  destination.betaOffsetCsiPart2 = source.beta_offset_csi_part2;
}

pusch_grant_parameters read_grant(const PuschGrantParameters_t& source)
{
  pusch_grant_parameters result;
  result.harq_id         = static_cast<uint8_t>(read_ranged(source.harqId, 0, 15, "HARQ id"));
  result.n_id            = static_cast<uint16_t>(read_ranged(source.nId, 0, 1023, "scrambling identity"));
  result.tbs_bytes       = static_cast<uint32_t>(read_ranged(source.tbsBytes, 0, 16777215, "TBS"));
  result.rv              = static_cast<uint8_t>(read_ranged(source.rvIndex, 0, 3, "RV"));
  result.new_data        = source.newData != 0;
  result.ldpc_base_graph = static_cast<uint8_t>(read_ranged(source.ldpcBaseGraph, 1, 2, "LDPC base graph"));
  result.tbs_lbrm_bytes  = static_cast<uint32_t>(read_ranged(source.tbsLbrmBytes, 0, 16777215, "LBRM TBS"));
  if (source.mcsIndex != nullptr) {
    result.mcs_index = static_cast<uint8_t>(read_ranged(*source.mcsIndex, 0, 31, "MCS index"));
  }
  if (source.mcsTable != nullptr) {
    result.mcs_table = static_cast<uint8_t>(read_ranged(*source.mcsTable, 0, 2, "MCS table"));
  }
  result.transform_precoding = source.transformPrecoding != 0;
  if (source.nRsId != nullptr) {
    result.n_rs_id = static_cast<uint16_t>(read_ranged(*source.nRsId, 0, 1023, "DM-RS identity"));
  }
  result.cyclic_prefix    = static_cast<uint8_t>(read_ranged(source.cyclicPrefix, 0, 1, "cyclic prefix"));
  result.start_symbol     = static_cast<uint8_t>(read_ranged(source.startSymbol, 0, 13, "start symbol"));
  result.nof_symbols      = static_cast<uint8_t>(read_ranged(source.nofSymbols, 1, 14, "symbol count"));
  result.dmrs_symbol_mask = static_cast<uint16_t>(read_ranged(source.dmrsSymbolMask, 0, 16383, "DM-RS symbol mask"));
  result.dmrs_type        = static_cast<uint8_t>(read_ranged(source.dmrsType, 1, 2, "DM-RS type"));
  result.nof_cdm_groups_without_data =
      static_cast<uint8_t>(read_ranged(source.nofCdmGroupsWithoutData, 1, 3, "CDM groups without data"));
  result.dmrs_scrambling_id = static_cast<uint16_t>(read_ranged(source.dmrsScramblingId, 0, 65535, "DM-RS scrambling"));
  result.n_scid             = source.nScid != 0;
  result.bwp_start_rb       = static_cast<uint16_t>(read_ranged(source.bwpStartRb, 0, 274, "BWP start"));
  result.bwp_size_rb        = static_cast<uint16_t>(read_ranged(source.bwpSizeRb, 1, 275, "BWP size"));
  const auto mask           = read_bytes(source.prbMask, result.prb_mask.size());
  if (mask.size() != result.prb_mask.size()) {
    throw std::runtime_error("PRB mask must hold 35 octets");
  }
  std::copy(mask.begin(), mask.end(), result.prb_mask.begin());
  result.nof_rx_ports = static_cast<uint8_t>(read_ranged(source.nofRxPorts, 0, 8, "receive port count"));
  result.nof_layers   = static_cast<uint8_t>(read_ranged(source.nofLayers, 0, 4, "layer count"));
  if (source.dcPosition != nullptr) {
    result.dc_position = static_cast<uint16_t>(read_ranged(*source.dcPosition, 0, 3299, "DC position"));
  }
  result.nof_harq_ack_bits     = static_cast<uint16_t>(read_ranged(source.nofHarqAckBits, 0, 1706, "HARQ-ACK bits"));
  result.nof_csi_part1_bits    = static_cast<uint16_t>(read_ranged(source.nofCsiPart1Bits, 0, 1706, "CSI part 1 bits"));
  result.csi_part2_expected    = source.csiPart2Expected != 0;
  result.alpha_scaling         = source.alphaScaling;
  result.beta_offset_harq_ack  = source.betaOffsetHarqAck;
  result.beta_offset_csi_part1 = source.betaOffsetCsiPart1;
  result.beta_offset_csi_part2 = source.betaOffsetCsiPart2;
  return result;
}

void set_object(Telemetry_Object_t& destination, const pusch_symbols_record& source)
{
  auto& record        = *(destination.choice.puschSymbols = make_asn<PuschSymbolsRecord_t>());
  destination.present = Telemetry_Object_PR_puschSymbols;
  set_u64(record.rnti, source.rnti);
  set_radio_slot(record.radioSlot, source.radio_slot);
  set_u64(record.nofPrb, read_ranged(source.nof_prb, 0, 275, "PRB count"));
  set_u64(record.nofDataSymbols, read_ranged(source.nof_data_symbols, 0, 14, "symbol count"));
  set_u64(record.nofLayers, read_ranged(source.nof_layers, 0, 4, "layer count"));
  set_u64(record.modulationBits, read_ranged(source.modulation_bits, 0, 8, "modulation bits"));
  set_optional_real(record.sinrDb, source.sinr_db);
  set_optional_real(record.epreDb, source.epre_db);
  set_optional_real(record.rsrpDb, source.rsrp_db);
  set_optional_real(record.evm, source.evm);
  set_optional_real(record.timeAlignmentUs, source.time_alignment_us);
  set_optional_real(record.cfoHz, source.cfo_hz);
  set_optional_real(record.targetCodeRate, source.target_code_rate);
  set_samples(record.equalizedSymbols, source.equalized_symbols);
  if (source.grant) {
    record.grant = make_asn<PuschGrantParameters_t>();
    set_grant(*record.grant, *source.grant);
  }
  if (source.crc_ok) {
    record.crcOk  = make_asn<BOOLEAN_t>();
    *record.crcOk = *source.crc_ok ? 1 : 0;
  }
  if (source.symbols_truncated) {
    record.symbolsTruncated  = make_asn<BOOLEAN_t>();
    *record.symbolsTruncated = 1;
  }
}

pusch_symbols_record read_object(const PuschSymbolsRecord_t& source)
{
  pusch_symbols_record result;
  result.rnti              = read_u16(source.rnti, "RNTI");
  result.radio_slot        = read_radio_slot(source.radioSlot);
  result.nof_prb           = static_cast<uint16_t>(read_ranged(source.nofPrb, 0, 275, "PRB count"));
  result.nof_data_symbols  = static_cast<uint8_t>(read_ranged(source.nofDataSymbols, 0, 14, "symbol count"));
  result.nof_layers        = static_cast<uint8_t>(read_ranged(source.nofLayers, 0, 4, "layer count"));
  result.modulation_bits   = static_cast<uint8_t>(read_ranged(source.modulationBits, 0, 8, "modulation bits"));
  result.sinr_db           = read_optional_real(source.sinrDb);
  result.epre_db           = read_optional_real(source.epreDb);
  result.rsrp_db           = read_optional_real(source.rsrpDb);
  result.evm               = read_optional_real(source.evm);
  result.time_alignment_us = read_optional_real(source.timeAlignmentUs);
  result.cfo_hz            = read_optional_real(source.cfoHz);
  result.target_code_rate  = read_optional_real(source.targetCodeRate);
  result.equalized_symbols = read_samples(source.equalizedSymbols);
  if (source.grant != nullptr) {
    result.grant = read_grant(*source.grant);
  }
  if (source.crcOk != nullptr) {
    result.crc_ok = *source.crcOk != 0;
  }
  result.symbols_truncated = source.symbolsTruncated != nullptr && *source.symbolsTruncated != 0;
  return result;
}

void set_object(Telemetry_Object_t& destination, const pusch_grid_record& source)
{
  auto& record        = *(destination.choice.puschGrid = make_asn<PuschGridRecord_t>());
  destination.present = Telemetry_Object_PR_puschGrid;
  set_u64(record.rnti, source.rnti);
  set_radio_slot(record.radioSlot, source.radio_slot);
  set_u64(record.nofPrb, read_ranged(source.nof_prb, 0, 275, "PRB count"));
  set_u64(record.nofRxPorts, read_ranged(source.nof_rx_ports, 0, 8, "receive port count"));
  set_u64(record.startSymbol, read_ranged(source.start_symbol, 0, 13, "start symbol"));
  set_u64(record.nofSymbols, read_ranged(source.nof_symbols, 1, 14, "symbol count"));
  record.grant = make_asn<PuschGrantParameters_t>();
  set_grant(*record.grant, source.grant);
  set_samples(record.resourceElements, source.resource_elements);
}

pusch_grid_record read_object(const PuschGridRecord_t& source)
{
  pusch_grid_record result;
  result.rnti         = read_u16(source.rnti, "RNTI");
  result.radio_slot   = read_radio_slot(source.radioSlot);
  result.nof_prb      = static_cast<uint16_t>(read_ranged(source.nofPrb, 0, 275, "PRB count"));
  result.nof_rx_ports = static_cast<uint8_t>(read_ranged(source.nofRxPorts, 0, 8, "receive port count"));
  result.start_symbol = static_cast<uint8_t>(read_ranged(source.startSymbol, 0, 13, "start symbol"));
  result.nof_symbols  = static_cast<uint8_t>(read_ranged(source.nofSymbols, 1, 14, "symbol count"));
  if (source.grant == nullptr) {
    throw std::runtime_error("PUSCH grid record is missing its grant");
  }
  result.grant             = read_grant(*source.grant);
  result.resource_elements = read_samples(source.resourceElements);
  return result;
}

void set_object(Telemetry_Object_t& destination, const pusch_crc_record& source)
{
  auto& record        = *(destination.choice.puschCrc = make_asn<PuschCrcRecord_t>());
  destination.present = Telemetry_Object_PR_puschCrc;
  set_u64(record.rnti, source.rnti);
  set_radio_slot(record.radioSlot, source.radio_slot);
  record.crcOk = source.crc_ok ? 1 : 0;
}

pusch_crc_record read_object(const PuschCrcRecord_t& source)
{
  if (source.crcOk != 0 && source.crcOk != 1) {
    throw std::runtime_error("invalid CRC flag");
  }
  pusch_crc_record result;
  result.rnti       = read_u16(source.rnti, "RNTI");
  result.radio_slot = read_radio_slot(source.radioSlot);
  result.crc_ok     = source.crcOk != 0;
  return result;
}

void set_object(Telemetry_Object_t& destination, const srs_channel_record& source)
{
  auto& record        = *(destination.choice.srsChannel = make_asn<SrsChannelRecord_t>());
  destination.present = Telemetry_Object_PR_srsChannel;
  set_u64(record.rnti, source.rnti);
  set_radio_slot(record.radioSlot, source.radio_slot);
  set_u64(record.nofSubcarriers, source.nof_subcarriers);
  set_u64(record.combSize, source.comb_size);
  set_u64(record.nofRxPorts, source.nof_rx_ports);
  set_u64(record.nofTxPorts, source.nof_tx_ports);
  set_u64(record.rxPortIndex, source.rx_port_index);
  set_u64(record.txPortIndex, source.tx_port_index);
  set_u64(record.startSymbol, read_ranged(source.start_symbol, 0, 13, "start symbol"));
  set_u64(record.nofSymbols, read_ranged(source.nof_symbols, 0, 14, "symbol count"));
  set_optional_real(record.rsrpDb, source.rsrp_db);
  set_optional_real(record.epreDb, source.epre_db);
  set_optional_real(record.noiseVariance, source.noise_variance);
  set_optional_real(record.timeAlignmentNs, source.time_alignment_ns);
  set_optional_small(record.mappingInitialSubcarrier, source.mapping_initial_subcarrier, 65535, "subcarrier");
  set_optional_small(record.combOffset, source.comb_offset, 255, "comb offset");
  set_optional_small(record.cyclicShift, source.cyclic_shift, 255, "cyclic shift");
  set_optional_small(record.sequenceId, source.sequence_id, 65535, "sequence id");
  set_optional_small(record.sequenceGroup, source.sequence_group, 65535, "sequence group");
  set_optional_small(record.sequenceNumber, source.sequence_number, 65535, "sequence number");
  set_optional_small(record.cyclicShiftIndex, source.cyclic_shift_index, 65535, "cyclic shift index");
  set_optional_small(record.cyclicShiftMaximum, source.cyclic_shift_maximum, 65535, "cyclic shift maximum");
  set_optional_small(record.bandwidthIndex, source.bandwidth_index, 255, "bandwidth index");
  set_optional_small(record.frequencyPosition, source.frequency_position, 255, "frequency position");
  set_optional_small(record.frequencyShift, source.frequency_shift, 65535, "frequency shift");
  set_optional_small(record.frequencyHopping, source.frequency_hopping, 255, "frequency hopping");
  set_samples(record.coefficients, source.coefficients);
}

srs_channel_record read_object(const SrsChannelRecord_t& source)
{
  srs_channel_record result;
  result.rnti              = read_u16(source.rnti, "RNTI");
  result.radio_slot        = read_radio_slot(source.radioSlot);
  result.nof_subcarriers   = read_u16(source.nofSubcarriers, "subcarrier count");
  result.comb_size         = static_cast<uint8_t>(read_ranged(source.combSize, 0, 255, "comb size"));
  result.nof_rx_ports      = static_cast<uint8_t>(read_ranged(source.nofRxPorts, 0, 255, "rx ports"));
  result.nof_tx_ports      = static_cast<uint8_t>(read_ranged(source.nofTxPorts, 0, 255, "tx ports"));
  result.rx_port_index     = static_cast<uint8_t>(read_ranged(source.rxPortIndex, 0, 255, "rx port"));
  result.tx_port_index     = static_cast<uint8_t>(read_ranged(source.txPortIndex, 0, 255, "tx port"));
  result.start_symbol      = static_cast<uint8_t>(read_ranged(source.startSymbol, 0, 13, "start symbol"));
  result.nof_symbols       = static_cast<uint8_t>(read_ranged(source.nofSymbols, 0, 14, "symbol count"));
  result.rsrp_db           = read_optional_real(source.rsrpDb);
  result.epre_db           = read_optional_real(source.epreDb);
  result.noise_variance    = read_optional_real(source.noiseVariance);
  result.time_alignment_ns = read_optional_real(source.timeAlignmentNs);
  result.mapping_initial_subcarrier =
      read_optional_small<uint16_t>(source.mappingInitialSubcarrier, 65535, "subcarrier");
  result.comb_offset          = read_optional_small<uint8_t>(source.combOffset, 255, "comb offset");
  result.cyclic_shift         = read_optional_small<uint8_t>(source.cyclicShift, 255, "cyclic shift");
  result.sequence_id          = read_optional_small<uint16_t>(source.sequenceId, 65535, "sequence id");
  result.sequence_group       = read_optional_small<uint16_t>(source.sequenceGroup, 65535, "sequence group");
  result.sequence_number      = read_optional_small<uint16_t>(source.sequenceNumber, 65535, "sequence number");
  result.cyclic_shift_index   = read_optional_small<uint16_t>(source.cyclicShiftIndex, 65535, "cyclic shift index");
  result.cyclic_shift_maximum = read_optional_small<uint16_t>(source.cyclicShiftMaximum, 65535, "cyclic shift max");
  result.bandwidth_index      = read_optional_small<uint8_t>(source.bandwidthIndex, 255, "bandwidth index");
  result.frequency_position   = read_optional_small<uint8_t>(source.frequencyPosition, 255, "frequency position");
  result.frequency_shift      = read_optional_small<uint16_t>(source.frequencyShift, 65535, "frequency shift");
  result.frequency_hopping    = read_optional_small<uint8_t>(source.frequencyHopping, 255, "frequency hopping");
  result.coefficients         = read_samples(source.coefficients);
  return result;
}

void set_object(Telemetry_Object_t& destination, const pusch_dmrs_channel_record& source)
{
  auto& record        = *(destination.choice.puschDmrsChannel = make_asn<PuschDmrsChannelRecord_t>());
  destination.present = Telemetry_Object_PR_puschDmrsChannel;
  set_u64(record.rnti, source.rnti);
  set_radio_slot(record.radioSlot, source.radio_slot);
  set_u64(record.startPrb, read_ranged(source.start_prb, 0, 274, "start PRB"));
  set_u64(record.nofPrb, read_ranged(source.nof_prb, 0, 275, "PRB count"));
  set_u64(record.nofSubcarriers, source.nof_subcarriers);
  set_u64(record.dmrsSymbolIndex, read_ranged(source.dmrs_symbol_index, 0, 13, "DM-RS symbol"));
  set_u64(record.startSymbol, read_ranged(source.start_symbol, 0, 13, "start symbol"));
  set_u64(record.nofSymbols, read_ranged(source.nof_symbols, 0, 14, "symbol count"));
  set_u64(record.nofRxPorts, source.nof_rx_ports);
  set_u64(record.nofTxLayers, source.nof_tx_layers);
  set_u64(record.rxPortIndex, source.rx_port_index);
  set_u64(record.txLayerIndex, source.tx_layer_index);
  set_u64(record.dmrsType, read_ranged(source.dmrs_type, 1, 2, "DM-RS type"));
  set_u64(record.nofCdmGroupsWithoutData, read_ranged(source.nof_cdm_groups_without_data, 0, 3, "CDM groups"));
  record.transformPrecoding = source.transform_precoding ? 1 : 0;
  set_u64(record.scramblingId, source.scrambling_id);
  record.nScid = source.n_scid ? 1 : 0;
  set_optional_real(record.sinrDb, source.sinr_db);
  set_optional_real(record.epreDb, source.epre_db);
  set_optional_real(record.rsrpDb, source.rsrp_db);
  set_optional_real(record.noiseVariance, source.noise_variance);
  set_optional_real(record.timeAlignmentUs, source.time_alignment_us);
  set_optional_real(record.cfoHz, source.cfo_hz);
  set_optional_real(record.targetCodeRate, source.target_code_rate);
  set_samples(record.estimates, source.estimates);
}

pusch_dmrs_channel_record read_object(const PuschDmrsChannelRecord_t& source)
{
  if ((source.transformPrecoding != 0 && source.transformPrecoding != 1) || (source.nScid != 0 && source.nScid != 1)) {
    throw std::runtime_error("invalid DM-RS flags");
  }
  pusch_dmrs_channel_record result;
  result.rnti                        = read_u16(source.rnti, "RNTI");
  result.radio_slot                  = read_radio_slot(source.radioSlot);
  result.start_prb                   = static_cast<uint16_t>(read_ranged(source.startPrb, 0, 274, "start PRB"));
  result.nof_prb                     = static_cast<uint16_t>(read_ranged(source.nofPrb, 0, 275, "PRB count"));
  result.nof_subcarriers             = read_u16(source.nofSubcarriers, "subcarrier count");
  result.dmrs_symbol_index           = static_cast<uint8_t>(read_ranged(source.dmrsSymbolIndex, 0, 13, "DM-RS symbol"));
  result.start_symbol                = static_cast<uint8_t>(read_ranged(source.startSymbol, 0, 13, "start symbol"));
  result.nof_symbols                 = static_cast<uint8_t>(read_ranged(source.nofSymbols, 0, 14, "symbol count"));
  result.nof_rx_ports                = static_cast<uint8_t>(read_ranged(source.nofRxPorts, 0, 255, "rx ports"));
  result.nof_tx_layers               = static_cast<uint8_t>(read_ranged(source.nofTxLayers, 0, 255, "layers"));
  result.rx_port_index               = static_cast<uint8_t>(read_ranged(source.rxPortIndex, 0, 255, "rx port"));
  result.tx_layer_index              = static_cast<uint8_t>(read_ranged(source.txLayerIndex, 0, 255, "layer"));
  result.dmrs_type                   = static_cast<uint8_t>(read_ranged(source.dmrsType, 1, 2, "DM-RS type"));
  result.nof_cdm_groups_without_data = static_cast<uint8_t>(read_ranged(source.nofCdmGroupsWithoutData, 0, 3, "CDM"));
  result.transform_precoding         = source.transformPrecoding != 0;
  result.scrambling_id               = read_u16(source.scramblingId, "scrambling id");
  result.n_scid                      = source.nScid != 0;
  result.sinr_db                     = read_optional_real(source.sinrDb);
  result.epre_db                     = read_optional_real(source.epreDb);
  result.rsrp_db                     = read_optional_real(source.rsrpDb);
  result.noise_variance              = read_optional_real(source.noiseVariance);
  result.time_alignment_us           = read_optional_real(source.timeAlignmentUs);
  result.cfo_hz                      = read_optional_real(source.cfoHz);
  result.target_code_rate            = read_optional_real(source.targetCodeRate);
  result.estimates                   = read_samples(source.estimates);
  return result;
}

void set_object(Telemetry_Object_t& destination, const gnb_metrics_json_record& source)
{
  auto& record        = *(destination.choice.gnbMetricsJson = make_asn<GnbMetricsJsonRecord_t>());
  destination.present = Telemetry_Object_PR_gnbMetricsJson;
  if (source.json.size() > e3ap_maximum_payload_bytes || !valid_utf8(source.json.data(), source.json.size())) {
    throw std::runtime_error("gNB metrics JSON must be nonempty UTF-8 within the bound");
  }
  set_octets(record.json, source.json);
}

gnb_metrics_json_record read_object(const GnbMetricsJsonRecord_t& source)
{
  gnb_metrics_json_record result;
  result.json = read_bytes(source.json, e3ap_maximum_payload_bytes);
  if (!result.json.empty() && !valid_utf8(result.json.data(), result.json.size())) {
    throw std::runtime_error("gNB metrics JSON must be nonempty UTF-8");
  }
  return result;
}

void set_object(Telemetry_Object_t& destination, const class_c_portable_record& source)
{
  auto& record        = *(destination.choice.classCPortable = make_asn<ClassCPortableRecord_t>());
  destination.present = Telemetry_Object_PR_classCPortable;
  set_u64(record.interfaceId, read_ranged(source.interface_id, 1, 4294967295ULL, "interface id"));
  // An empty tensor is legal: the bytes were delivered by reference.
  if (source.metadata.size() > 65536 || source.tensor.size() > e3ap_maximum_payload_bytes) {
    throw std::runtime_error("Class-C portable record exceeds its bounds");
  }
  set_octets(record.metadata, source.metadata);
  set_octets(record.tensor, source.tensor);
}

class_c_portable_record read_object(const ClassCPortableRecord_t& source)
{
  class_c_portable_record result;
  result.interface_id = static_cast<uint32_t>(read_ranged(source.interfaceId, 1, 4294967295ULL, "interface id"));
  result.metadata     = read_bytes(source.metadata, 65536);
  result.tensor       = read_bytes(source.tensor, e3ap_maximum_payload_bytes);
  return result;
}

void set_indication(Telemetry_IndicationData_t& destination, const telemetry_indication& source)
{
  set_u64(destination.telemetryId, read_ranged(source.telemetry_id, 1, 65535, "telemetry id"));
  set_u64(destination.cellId, source.cell_id);
  set_u64(destination.sequence, source.sequence);
  set_u64(destination.routeEpoch, source.route_epoch);
  set_u64(destination.monotonicNs, source.monotonic_ns);
  set_u64(destination.droppedObjects, source.dropped_objects);
  destination.object = make_asn<Telemetry_Object_t>();
  std::visit([&](const auto& record) { set_object(*destination.object, record); }, source.object);
}

telemetry_indication read_indication(const Telemetry_IndicationData_t& source)
{
  telemetry_indication result;
  result.telemetry_id    = read_id16(source.telemetryId, "telemetry id");
  result.cell_id         = read_u16(source.cellId, "cell id");
  result.sequence        = read_u64(source.sequence);
  result.route_epoch     = read_u64(source.routeEpoch);
  result.monotonic_ns    = read_u64(source.monotonicNs);
  result.dropped_objects = read_u64(source.droppedObjects);
  if (source.object == nullptr) {
    throw std::runtime_error("telemetry object is missing");
  }
  const auto& object = *source.object;
  switch (object.present) {
    case Telemetry_Object_PR_puschSymbols:
      result.object = read_object(*object.choice.puschSymbols);
      break;
    case Telemetry_Object_PR_puschCrc:
      result.object = read_object(*object.choice.puschCrc);
      break;
    case Telemetry_Object_PR_srsChannel:
      result.object = read_object(*object.choice.srsChannel);
      break;
    case Telemetry_Object_PR_puschDmrsChannel:
      result.object = read_object(*object.choice.puschDmrsChannel);
      break;
    case Telemetry_Object_PR_gnbMetricsJson:
      result.object = read_object(*object.choice.gnbMetricsJson);
      break;
    case Telemetry_Object_PR_classCPortable:
      result.object = read_object(*object.choice.classCPortable);
      break;
    case Telemetry_Object_PR_puschGrid:
      result.object = read_object(*object.choice.puschGrid);
      break;
    default:
      throw std::runtime_error("unknown telemetry object");
  }
  return result;
}

void set_stream(Telemetry_StreamDescriptor_t& destination, const telemetry_stream_record& source)
{
  set_u64(destination.telemetryId, read_ranged(source.telemetry_id, 1, 65535, "telemetry id"));
  if (source.name.empty() || source.name.size() > 128 || source.description.size() > 128) {
    throw std::runtime_error("telemetry stream name/description exceed their bounds");
  }
  set_text(destination.name, source.name);
  set_text(destination.description, source.description);
  destination.classCPortable = source.class_c_portable ? 1 : 0;
  set_u64(destination.interfaceId, read_ranged(source.interface_id, 1, 4294967295ULL, "interface id"));
  set_optional_u64(destination.minimumIntervalUs, source.minimum_interval_us);
  set_optional_u64(destination.hardFloorIntervalUs, source.hard_floor_interval_us);
  if (!source.cell_ids.empty()) {
    if (source.cell_ids.size() > 32) {
      throw std::runtime_error("telemetry cell list exceeds its bound");
    }
    destination.cellIds = make_asn<Telemetry_StreamDescriptor::Telemetry_StreamDescriptor__cellIds>();
    for (const uint16_t cell : source.cell_ids) {
      long* item = make_asn<long>();
      *item      = cell;
      if (ASN_SEQUENCE_ADD(&destination.cellIds->list, item) != 0) {
        std::free(item);
        throw std::bad_alloc();
      }
    }
  }
}

telemetry_stream_record read_stream(const Telemetry_StreamDescriptor_t& source)
{
  telemetry_stream_record result;
  result.telemetry_id     = read_id16(source.telemetryId, "telemetry id");
  result.name             = read_string(source.name, 128);
  result.description      = read_string(source.description, 128);
  result.class_c_portable = source.classCPortable != 0;
  result.interface_id     = static_cast<uint32_t>(read_ranged(source.interfaceId, 1, 4294967295ULL, "interface id"));
  if (source.hardFloorIntervalUs != nullptr) {
    result.hard_floor_interval_us = read_u64(*source.hardFloorIntervalUs);
  }
  if (source.minimumIntervalUs != nullptr) {
    result.minimum_interval_us = read_u64(*source.minimumIntervalUs);
  }
  if (source.cellIds != nullptr) {
    if (source.cellIds->list.count < 0 || source.cellIds->list.count > 32) {
      throw std::runtime_error("telemetry cell list exceeds its bound");
    }
    for (int index = 0; index != source.cellIds->list.count; ++index) {
      if (source.cellIds->list.array[index] == nullptr) {
        throw std::runtime_error("telemetry cell entry is missing");
      }
      result.cell_ids.push_back(read_u16(*source.cellIds->list.array[index], "cell id"));
    }
  }
  return result;
}

void set_function(Telemetry_RanFunctionData_t& destination, const telemetry_ran_function_data& source)
{
  if (source.name.empty() || source.name.size() > 32 || source.description.empty() || source.description.size() > 128 ||
      source.streams.size() > maximum_telemetry_streams) {
    throw std::runtime_error("telemetry RAN function data exceeds its bounds");
  }
  set_text(destination.name, source.name);
  set_u64(destination.version, source.version);
  set_text(destination.description, source.description);
  for (const auto& stream : source.streams) {
    set_stream(append_asn<Telemetry_StreamDescriptor_t>(destination.streams), stream);
  }
  set_u64(destination.maximumObjectBytes,
          read_ranged(source.maximum_object_bytes, 1, e3ap_maximum_payload_bytes, "object bound"));
  set_u64(destination.maximumSubscriptions, read_ranged(source.maximum_subscriptions, 0, 1024, "subscription bound"));
}

telemetry_ran_function_data read_function(const Telemetry_RanFunctionData_t& source)
{
  telemetry_ran_function_data result;
  result.name        = read_string(source.name, 32);
  result.version     = read_u16(source.version, "version");
  result.description = read_string(source.description, 128);
  if (source.streams.list.count < 0 ||
      static_cast<std::size_t>(source.streams.list.count) > maximum_telemetry_streams) {
    throw std::runtime_error("telemetry stream list exceeds its bound");
  }
  result.streams.clear();
  for (int index = 0; index != source.streams.list.count; ++index) {
    if (source.streams.list.array[index] == nullptr) {
      throw std::runtime_error("telemetry stream entry is missing");
    }
    result.streams.push_back(read_stream(*source.streams.list.array[index]));
  }
  result.maximum_object_bytes =
      static_cast<uint32_t>(read_ranged(source.maximumObjectBytes, 1, e3ap_maximum_payload_bytes, "object bound"));
  result.maximum_subscriptions =
      static_cast<uint32_t>(read_ranged(source.maximumSubscriptions, 0, 1024, "subscription bound"));
  return result;
}

void set_config(Telemetry_SubscriptionConfig_t& destination, const telemetry_subscription_config& source)
{
  set_u64(destination.telemetryId, read_ranged(source.telemetry_id, 1, 65535, "telemetry id"));
  set_optional_typed(destination.cellId, source.cell_id);
  if (source.sample_every_n == 0) {
    throw std::runtime_error("sampleEveryN must be positive");
  }
  if (source.sample_every_n != 1) {
    destination.sampleEveryN  = make_asn<long>();
    *destination.sampleEveryN = source.sample_every_n;
  }
  if (source.drop_policy != telemetry_drop_policy::coalesce_latest) {
    destination.dropPolicy  = make_asn<Telemetry_DropPolicy_t>();
    *destination.dropPolicy = static_cast<long>(source.drop_policy);
  }
  set_optional_small(destination.minimumIntervalMs, source.minimum_interval_ms, 60000, "minimum interval");
  if (!source.rnti_filter.empty()) {
    if (source.rnti_filter.size() > maximum_rnti_filter) {
      throw std::runtime_error("RNTI filter exceeds its bound");
    }
    destination.rntiFilter = make_asn<Telemetry_SubscriptionConfig::Telemetry_SubscriptionConfig__rntiFilter>();
    for (const uint16_t rnti : source.rnti_filter) {
      Rnti_t* item = make_asn<Rnti_t>();
      *item        = rnti;
      if (ASN_SEQUENCE_ADD(&destination.rntiFilter->list, item) != 0) {
        std::free(item);
        throw std::bad_alloc();
      }
    }
  }
}

telemetry_subscription_config read_config(const Telemetry_SubscriptionConfig_t& source)
{
  telemetry_subscription_config result;
  result.telemetry_id = read_id16(source.telemetryId, "telemetry id");
  if (source.cellId != nullptr) {
    result.cell_id = read_u16(*source.cellId, "cell id");
  }
  result.sample_every_n = source.sampleEveryN != nullptr
                              ? static_cast<uint16_t>(read_ranged(*source.sampleEveryN, 1, 65535, "sampleEveryN"))
                              : 1;
  if (source.dropPolicy != nullptr) {
    if (*source.dropPolicy < Telemetry_DropPolicy_dropNewest ||
        *source.dropPolicy > Telemetry_DropPolicy_coalesceLatest) {
      throw std::runtime_error("invalid drop policy");
    }
    result.drop_policy = static_cast<telemetry_drop_policy>(*source.dropPolicy);
  }
  result.minimum_interval_ms = read_optional_small<uint32_t>(source.minimumIntervalMs, 60000, "minimum interval");
  if (source.rntiFilter != nullptr) {
    if (source.rntiFilter->list.count <= 0 ||
        static_cast<std::size_t>(source.rntiFilter->list.count) > maximum_rnti_filter) {
      throw std::runtime_error("RNTI filter exceeds its bound");
    }
    for (int index = 0; index != source.rntiFilter->list.count; ++index) {
      if (source.rntiFilter->list.array[index] == nullptr) {
        throw std::runtime_error("RNTI filter entry is missing");
      }
      result.rnti_filter.push_back(read_u16(*source.rntiFilter->list.array[index], "RNTI"));
    }
  }
  return result;
}

void set_management_function(Mgmt_RanFunctionData_t& destination, const management_ran_function_data& source)
{
  if (source.name.empty() || source.name.size() > 32 || source.description.empty() || source.description.size() > 128 ||
      source.schema_module.empty() || source.schema_module.size() > 64) {
    throw std::runtime_error("management RAN function data exceeds its bounds");
  }
  set_text(destination.name, source.name);
  set_u64(destination.version, source.version);
  set_text(destination.description, source.description);
  set_text(destination.schemaModule, source.schema_module);
  set_u64(destination.maximumBytes, read_ranged(source.maximum_bytes, 1, 2U * 1024U * 1024U, "management bound"));
}

management_ran_function_data read_management_function(const Mgmt_RanFunctionData_t& source)
{
  management_ran_function_data result;
  result.name          = read_string(source.name, 32);
  result.version       = read_u16(source.version, "version");
  result.description   = read_string(source.description, 128);
  result.schema_module = read_string(source.schemaModule, 64);
  result.maximum_bytes =
      static_cast<uint32_t>(read_ranged(source.maximumBytes, 1, 2U * 1024U * 1024U, "management bound"));
  return result;
}

template <typename AsnType, typename Fill>
codec_result<std::vector<uint8_t>>
encode_with(asn_TYPE_descriptor_t& descriptor, std::size_t bound, Fill&& fill) noexcept
{
  try {
    asn_owner<AsnType> value(descriptor, make_asn<AsnType>());
    fill(*value.get());
    return {encode_asn(descriptor, *value.get(), bound), {}};
  } catch (const std::bad_alloc&) {
    return {std::nullopt, "APER encoding allocation failed"};
  } catch (const std::exception& exception) {
    return {std::nullopt, exception.what()};
  } catch (...) {
    return {std::nullopt, "unknown APER encoding failure"};
  }
}

template <typename Result, typename AsnType, typename Read>
codec_result<Result> decode_with(asn_TYPE_descriptor_t&     descriptor,
                                 ocudu::span<const uint8_t> bytes,
                                 std::size_t                bound,
                                 const char*                what,
                                 Read&&                     read) noexcept
{
  try {
    auto owner = decode_asn<AsnType>(descriptor, bytes, bound, what);
    return {read(*owner.get()), {}};
  } catch (const std::bad_alloc&) {
    return {std::nullopt, "APER decoding allocation failed"};
  } catch (const std::exception& exception) {
    return {std::nullopt, exception.what()};
  } catch (...) {
    return {std::nullopt, "unknown APER decoding failure"};
  }
}

} // namespace

codec_result<std::vector<uint8_t>> e3ap_codec::encode(const e3ap_pdu& pdu) noexcept
{
  return encode_with<E3_PDU_t>(asn_DEF_E3_PDU, maximum_pdu_bytes, [&](E3_PDU_t& value) {
    set_u64(value.id, read_ranged(pdu.id, 1, 4294967295ULL, "PDU id"));
    if (pdu.timestamp_ns) {
      value.timestamp  = make_asn<E3_Timestamp_t>();
      *value.timestamp = static_cast<long>(*pdu.timestamp_ns);
    }
    std::visit([&](const auto& message) { set_message(value, message); }, pdu.message);
  });
}

codec_result<e3ap_pdu> e3ap_codec::decode(ocudu::span<const uint8_t> bytes) noexcept
{
  return decode_with<e3ap_pdu, E3_PDU_t>(asn_DEF_E3_PDU, bytes, maximum_pdu_bytes, "E3-PDU", [](const E3_PDU_t& value) {
    e3ap_pdu result;
    result.id = static_cast<uint32_t>(read_ranged(value.id, 1, 4294967295ULL, "PDU id"));
    if (value.timestamp != nullptr) {
      result.timestamp_ns = static_cast<int64_t>(*value.timestamp);
    }
    switch (value.msg.present) {
      case E3_PDU__msg_PR_setupRequest:
        result.message = read_message(*value.msg.choice.setupRequest);
        break;
      case E3_PDU__msg_PR_setupResponse:
        result.message = read_message(*value.msg.choice.setupResponse);
        break;
      case E3_PDU__msg_PR_subscriptionRequest:
        result.message = read_message(*value.msg.choice.subscriptionRequest);
        break;
      case E3_PDU__msg_PR_subscriptionDelete:
        result.message = read_message(*value.msg.choice.subscriptionDelete);
        break;
      case E3_PDU__msg_PR_subscriptionResponse:
        result.message = read_message(*value.msg.choice.subscriptionResponse);
        break;
      case E3_PDU__msg_PR_indicationMessage:
        result.message = read_message(*value.msg.choice.indicationMessage);
        break;
      case E3_PDU__msg_PR_dAppControlAction:
        result.message = read_message(*value.msg.choice.dAppControlAction);
        break;
      case E3_PDU__msg_PR_dAppReport:
        result.message = read_message(*value.msg.choice.dAppReport);
        break;
      case E3_PDU__msg_PR_xAppControlAction:
        result.message = read_message(*value.msg.choice.xAppControlAction);
        break;
      case E3_PDU__msg_PR_releaseMessage:
        result.message = read_message(*value.msg.choice.releaseMessage);
        break;
      case E3_PDU__msg_PR_messageAck:
        result.message = read_message(*value.msg.choice.messageAck);
        break;
      case E3_PDU__msg_PR_controlResponse:
        result.message = read_message(*value.msg.choice.controlResponse);
        break;
      default:
        throw std::runtime_error("unknown E3-PDU message");
    }
    return result;
  });
}

codec_result<std::vector<uint8_t>> e3ap_codec::encode(const telemetry_indication& indication) noexcept
{
  return encode_with<Telemetry_IndicationData_t>(
      asn_DEF_Telemetry_IndicationData, maximum_telemetry_bytes, [&](Telemetry_IndicationData_t& value) {
        set_indication(value, indication);
      });
}

codec_result<telemetry_indication> e3ap_codec::decode_telemetry(ocudu::span<const uint8_t> bytes) noexcept
{
  return decode_with<telemetry_indication, Telemetry_IndicationData_t>(
      asn_DEF_Telemetry_IndicationData, bytes, maximum_telemetry_bytes, "Telemetry-IndicationData", read_indication);
}

codec_result<std::vector<uint8_t>> e3ap_codec::encode(const telemetry_ran_function_data& data) noexcept
{
  return encode_with<Telemetry_RanFunctionData_t>(
      asn_DEF_Telemetry_RanFunctionData, e3ap_maximum_function_data, [&](Telemetry_RanFunctionData_t& value) {
        set_function(value, data);
      });
}

codec_result<telemetry_ran_function_data>
e3ap_codec::decode_telemetry_function(ocudu::span<const uint8_t> bytes) noexcept
{
  return decode_with<telemetry_ran_function_data, Telemetry_RanFunctionData_t>(
      asn_DEF_Telemetry_RanFunctionData, bytes, e3ap_maximum_function_data, "Telemetry-RanFunctionData", read_function);
}

codec_result<std::vector<uint8_t>> e3ap_codec::encode(const telemetry_subscription_data& data) noexcept
{
  return encode_with<Telemetry_SubscriptionData_t>(
      asn_DEF_Telemetry_SubscriptionData, e3ap_maximum_function_data, [&](Telemetry_SubscriptionData_t& value) {
        if (data.configs.size() > maximum_telemetry_streams) {
          throw std::runtime_error("subscription config list exceeds its bound");
        }
        for (const auto& config : data.configs) {
          set_config(append_asn<Telemetry_SubscriptionConfig_t>(value.configs), config);
        }
      });
}

codec_result<telemetry_subscription_data> e3ap_codec::decode_subscription(ocudu::span<const uint8_t> bytes) noexcept
{
  return decode_with<telemetry_subscription_data, Telemetry_SubscriptionData_t>(
      asn_DEF_Telemetry_SubscriptionData,
      bytes,
      e3ap_maximum_function_data,
      "Telemetry-SubscriptionData",
      [](const Telemetry_SubscriptionData_t& value) {
        telemetry_subscription_data result;
        if (value.configs.list.count < 0 ||
            static_cast<std::size_t>(value.configs.list.count) > maximum_telemetry_streams) {
          throw std::runtime_error("subscription config list exceeds its bound");
        }
        for (int index = 0; index != value.configs.list.count; ++index) {
          if (value.configs.list.array[index] == nullptr) {
            throw std::runtime_error("subscription config entry is missing");
          }
          result.configs.push_back(read_config(*value.configs.list.array[index]));
        }
        return result;
      });
}

codec_result<std::vector<uint8_t>> e3ap_codec::encode(const management_ran_function_data& data) noexcept
{
  return encode_with<Mgmt_RanFunctionData_t>(
      asn_DEF_Mgmt_RanFunctionData, e3ap_maximum_function_data, [&](Mgmt_RanFunctionData_t& value) {
        set_management_function(value, data);
      });
}

codec_result<management_ran_function_data>
e3ap_codec::decode_management_function(ocudu::span<const uint8_t> bytes) noexcept
{
  return decode_with<management_ran_function_data, Mgmt_RanFunctionData_t>(asn_DEF_Mgmt_RanFunctionData,
                                                                           bytes,
                                                                           e3ap_maximum_function_data,
                                                                           "Mgmt-RanFunctionData",
                                                                           read_management_function);
}

codec_result<telemetry_indication> e3ap_codec::from_publication(uint32_t                   interface_id,
                                                                uint32_t                   cell_id,
                                                                uint64_t                   sequence,
                                                                uint64_t                   route_epoch,
                                                                uint64_t                   dropped_objects,
                                                                ocudu::span<const uint8_t> metadata,
                                                                ocudu::span<const uint8_t> tensor,
                                                                bool                       include_bulk) noexcept
{
  try {
    const auto telemetry_id = telemetry_id_for(interface_id);
    if (!telemetry_id || tensor.empty() || cell_id > 65535) {
      return {std::nullopt, "publication does not map to a telemetry stream"};
    }
    telemetry_indication result;
    result.telemetry_id    = *telemetry_id;
    result.cell_id         = static_cast<uint16_t>(cell_id);
    result.sequence        = sequence;
    result.route_epoch     = route_epoch;
    result.dropped_objects = dropped_objects;

    const auto kind = telemetry_kind_of(interface_id);
    if (!kind) {
      class_c_portable_record record;
      record.interface_id = interface_id;
      record.metadata.assign(metadata.begin(), metadata.end());
      if (include_bulk) {
        record.tensor.assign(tensor.begin(), tensor.end());
      }
      result.monotonic_ns = 0;
      result.object       = std::move(record);
      return {std::move(result), {}};
    }

    const auto header = read_telemetry_header(metadata.data(), metadata.size());
    if (!header || header->kind != static_cast<uint16_t>(*kind)) {
      return {std::nullopt, "telemetry metadata header is invalid"};
    }
    result.monotonic_ns = header->monotonic_ns;

    const auto optional_float = [](float value) -> std::optional<double> {
      return std::isfinite(value) ? std::optional<double>(value) : std::nullopt;
    };
    const auto radio = [](const telemetry_radio_slot& slot) {
      return telemetry_radio_slot_value{slot.sfn, slot.slot, slot.numerology};
    };

    switch (*kind) {
      case telemetry_kind::pusch_symbols: {
        pusch_symbols_metadata_v1                source;
        std::optional<pusch_symbols_metadata_v2> extended;
        if (metadata.size() == sizeof(pusch_symbols_metadata_v2)) {
          extended.emplace();
          std::memcpy(&*extended, metadata.data(), sizeof(*extended));
          source = extended->base;
        } else if (metadata.size() == sizeof(source)) {
          std::memcpy(&source, metadata.data(), sizeof(source));
        } else {
          return {std::nullopt, "PUSCH symbols metadata size mismatch"};
        }
        pusch_symbols_record record;
        if (extended) {
          if ((extended->symbol_flags & 0x08U) != 0) {
            record.grant = grant_from_packed(extended->grant);
          }
          if ((extended->symbol_flags & 0x01U) != 0) {
            record.crc_ok = (extended->symbol_flags & 0x02U) != 0;
          }
          record.symbols_truncated = (extended->symbol_flags & 0x04U) != 0;
        }
        record.rnti              = source.rnti;
        record.radio_slot        = radio(source.radio_slot);
        record.nof_prb           = source.nof_prb;
        record.nof_data_symbols  = source.nof_data_symbols;
        record.nof_layers        = source.nof_layers;
        record.modulation_bits   = source.modulation_bits;
        record.sinr_db           = optional_float(source.sinr_db);
        record.epre_db           = optional_float(source.epre_db);
        record.rsrp_db           = optional_float(source.rsrp_db);
        record.evm               = optional_float(source.evm);
        record.time_alignment_us = optional_float(source.time_alignment_us);
        record.cfo_hz            = optional_float(source.cfo_hz);
        record.target_code_rate  = optional_float(source.target_code_rate);
        if (include_bulk) {
          record.equalized_symbols.assign(tensor.begin(), tensor.end());
        }
        result.object = std::move(record);
        break;
      }
      case telemetry_kind::pusch_grid: {
        pusch_grid_metadata_v1 source;
        if (metadata.size() != sizeof(source)) {
          return {std::nullopt, "PUSCH grid metadata size mismatch"};
        }
        std::memcpy(&source, metadata.data(), sizeof(source));
        pusch_grid_record record;
        record.rnti         = source.rnti;
        record.radio_slot   = radio(source.radio_slot);
        record.nof_prb      = source.nof_prb;
        record.nof_rx_ports = source.nof_rx_ports;
        record.start_symbol = source.start_symbol;
        record.nof_symbols  = source.nof_symbols;
        record.grant        = grant_from_packed(source.grant);
        if (include_bulk) {
          record.resource_elements.assign(tensor.begin(), tensor.end());
        }
        result.object = std::move(record);
        break;
      }
      case telemetry_kind::pusch_crc: {
        pusch_crc_metadata_v1 source;
        if (metadata.size() != sizeof(source)) {
          return {std::nullopt, "PUSCH CRC metadata size mismatch"};
        }
        std::memcpy(&source, metadata.data(), sizeof(source));
        pusch_crc_record record;
        record.rnti       = source.rnti;
        record.radio_slot = radio(source.radio_slot);
        record.crc_ok     = source.crc_ok != 0;
        result.object     = record;
        break;
      }
      case telemetry_kind::srs_channel: {
        srs_channel_metadata_v1 source;
        if (metadata.size() != sizeof(source)) {
          return {std::nullopt, "SRS channel metadata size mismatch"};
        }
        std::memcpy(&source, metadata.data(), sizeof(source));
        srs_channel_record record;
        record.rnti                       = source.rnti;
        record.radio_slot                 = radio(source.radio_slot);
        record.nof_subcarriers            = source.nof_subcarriers;
        record.comb_size                  = source.comb_size;
        record.nof_rx_ports               = source.nof_rx_ports;
        record.nof_tx_ports               = source.nof_tx_ports;
        record.rx_port_index              = source.rx_port_index;
        record.tx_port_index              = source.tx_port_index;
        record.start_symbol               = source.start_symbol;
        record.nof_symbols                = source.nof_symbols;
        record.rsrp_db                    = optional_float(source.rsrp_db);
        record.epre_db                    = optional_float(source.epre_db);
        record.noise_variance             = optional_float(source.noise_variance);
        record.time_alignment_ns          = optional_float(source.time_alignment_ns);
        record.mapping_initial_subcarrier = source.mapping_initial_subcarrier;
        record.comb_offset                = source.comb_offset;
        record.cyclic_shift               = source.cyclic_shift;
        record.sequence_id                = source.sequence_id;
        record.sequence_group             = source.sequence_group;
        record.sequence_number            = source.sequence_number;
        record.cyclic_shift_index         = source.cyclic_shift_index;
        record.cyclic_shift_maximum       = source.cyclic_shift_maximum;
        record.bandwidth_index            = source.bandwidth_index;
        record.frequency_position         = source.frequency_position;
        record.frequency_shift            = source.frequency_shift;
        record.frequency_hopping          = source.frequency_hopping;
        if (include_bulk) {
          record.coefficients.assign(tensor.begin(), tensor.end());
        }
        result.object = std::move(record);
        break;
      }
      case telemetry_kind::pusch_dmrs_channel: {
        pusch_dmrs_channel_metadata_v1 source;
        if (metadata.size() != sizeof(source)) {
          return {std::nullopt, "PUSCH DM-RS metadata size mismatch"};
        }
        std::memcpy(&source, metadata.data(), sizeof(source));
        pusch_dmrs_channel_record record;
        record.rnti                        = source.rnti;
        record.radio_slot                  = radio(source.radio_slot);
        record.start_prb                   = source.start_prb;
        record.nof_prb                     = source.nof_prb;
        record.nof_subcarriers             = source.nof_subcarriers;
        record.dmrs_symbol_index           = source.dmrs_symbol_index;
        record.start_symbol                = source.start_symbol;
        record.nof_symbols                 = source.nof_symbols;
        record.nof_rx_ports                = source.nof_rx_ports;
        record.nof_tx_layers               = source.nof_tx_layers;
        record.rx_port_index               = source.rx_port_index;
        record.tx_layer_index              = source.tx_layer_index;
        record.dmrs_type                   = source.dmrs_type;
        record.nof_cdm_groups_without_data = source.nof_cdm_groups_without_data;
        record.transform_precoding         = source.transform_precoding != 0;
        record.scrambling_id               = source.scrambling_id;
        record.n_scid                      = source.n_scid != 0;
        record.sinr_db                     = optional_float(source.sinr_db);
        record.epre_db                     = optional_float(source.epre_db);
        record.rsrp_db                     = optional_float(source.rsrp_db);
        record.noise_variance              = optional_float(source.noise_variance);
        record.time_alignment_us           = optional_float(source.time_alignment_us);
        record.cfo_hz                      = optional_float(source.cfo_hz);
        record.target_code_rate            = optional_float(source.target_code_rate);
        if (include_bulk) {
          record.estimates.assign(tensor.begin(), tensor.end());
        }
        result.object = std::move(record);
        break;
      }
      case telemetry_kind::gnb_metrics_json: {
        if (metadata.size() != sizeof(gnb_metrics_json_metadata_v1)) {
          return {std::nullopt, "gNB metrics metadata size mismatch"};
        }
        gnb_metrics_json_record record;
        if (include_bulk) {
          record.json.assign(tensor.begin(), tensor.end());
        }
        result.object = std::move(record);
        break;
      }
    }
    return {std::move(result), {}};
  } catch (const std::bad_alloc&) {
    return {std::nullopt, "telemetry conversion allocation failed"};
  } catch (...) {
    return {std::nullopt, "unknown telemetry conversion failure"};
  }
}

std::vector<ocudu::span<const uint8_t>> e3ap_codec::fragment(ocudu::span<const uint8_t> payload,
                                                             std::size_t                maximum_fragment_bytes) noexcept
{
  std::vector<ocudu::span<const uint8_t>> fragments;
  if (payload.empty() || maximum_fragment_bytes == 0) {
    return fragments;
  }
  try {
    for (std::size_t offset = 0; offset < payload.size(); offset += maximum_fragment_bytes) {
      fragments.push_back(payload.subspan(offset, std::min(maximum_fragment_bytes, payload.size() - offset)));
    }
  } catch (...) {
    fragments.clear();
  }
  return fragments;
}
