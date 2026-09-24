// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/e3/aper_codec.h"

extern "C" {
#include "Backend.h"
#include "CatalogAdmit.h"
#include "CellDescriptor.h"
#include "CellInfoQuery.h"
#include "CellInfoResult.h"
#include "CellPrachConfig.h"
#include "CellTddPattern.h"
#include "ConfigDescriptor.h"
#include "ConfigEntry.h"
#include "ConfigQuery.h"
#include "ConfigResult.h"
#include "ConfigSet.h"
#include "DataPollQuery.h"
#include "DataPublicationChunk.h"
#include "DataPublicationResult.h"
#include "DataSubscriptionControl.h"
#include "DataSubscriptionRecord.h"
#include "DataSubscriptionResult.h"
#include "E3ConnectionRecord.h"
#include "E3SubscriptionRecord.h"
#include "HealthSummary.h"
#include "IncidentQuery.h"
#include "IncidentRecord.h"
#include "IncidentResult.h"
#include "InstanceRecord.h"
#include "InterfaceRecord.h"
#include "InventoryQuery.h"
#include "InventoryResult.h"
#include "JobQuery.h"
#include "JobRecord.h"
#include "JobResult.h"
#include "LifecycleControl.h"
#include "ManagementOperation.h"
#include "ManagementPayload.h"
#include "ManagementRequest.h"
#include "ManagementResponse.h"
#include "MetricDescriptor.h"
#include "MetricQuery.h"
#include "MetricResult.h"
#include "MetricSample.h"
#include "ModelControl.h"
#include "OperationResult.h"
#include "PackageRecord.h"
#include "PuschProfile.h"
#include "PuschRouteBinding.h"
#include "PuschRouteEntry.h"
#include "PuschRouteTarget.h"
#include "PuschRouting.h"
#include "PuschRoutingResult.h"
#include "QuietReservationQuery.h"
#include "QuietReservationRecord.h"
#include "QuietReservationResult.h"
#include "RadioTime.h"
#include "RanControl.h"
#include "RanControlCapabilities.h"
#include "RequestHeader.h"
#include "Signed64.h"
#include "SrsProfile.h"
#include "TypedValue.h"
#include "Unsigned64.h"
#include "aper_decoder.h"
#include "aper_encoder.h"
#include "asn_SEQUENCE_OF.h"
#include "constraints.h"
}

#include "asn1_codec_internal.h"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <string_view>
#include <type_traits>

using namespace ocudu::dapp;
using namespace ocudu::dapp::e3;
using namespace ocudu::dapp::management;
using namespace ocudu::dapp::e3::asn1_detail;

namespace {

/// Optional scalar helpers for asn1c OPTIONAL members (allocated pointers).
template <typename T>
void set_optional_long(long*& destination, const std::optional<T>& value)
{
  if (value) {
    destination  = make_asn<long>();
    *destination = static_cast<long>(*value);
  }
}

inline void set_optional_real(double*& destination, const std::optional<double>& value)
{
  if (value) {
    destination  = make_asn<double>();
    *destination = *value;
  }
}

inline std::optional<double> read_optional_real(const double* source)
{
  return source == nullptr ? std::nullopt : std::optional<double>(*source);
}

constexpr std::size_t maximum_wire_bytes  = 2 * 1024 * 1024;
constexpr std::size_t maximum_text_bytes  = 512;
constexpr std::size_t maximum_items       = 1024;
constexpr std::size_t maximum_data_chunks = 8;
constexpr std::size_t maximum_data_bytes  = 64 * 1024;

long to_wire(result_code code)
{
  const auto value = static_cast<unsigned>(code);
  if (value > ResultCode_resourceExhausted) {
    throw std::runtime_error("invalid result code");
  }
  return static_cast<long>(value);
}

result_code from_wire_result(long value)
{
  if (value < ResultCode_success || value > ResultCode_resourceExhausted) {
    throw std::runtime_error("unknown result code");
  }
  return static_cast<result_code>(value);
}

long to_wire(ocudu_dapp_lifecycle_state_v1 state)
{
  if (state < OCUDU_DAPP_STATE_DISCOVERED_V1 || state > OCUDU_DAPP_STATE_ERROR_V1) {
    throw std::runtime_error("invalid lifecycle state");
  }
  return static_cast<long>(state - 1);
}

ocudu_dapp_lifecycle_state_v1 from_wire_state(long value)
{
  if (value < LifecycleState_discovered || value > LifecycleState_error) {
    throw std::runtime_error("unknown lifecycle state");
  }
  return static_cast<ocudu_dapp_lifecycle_state_v1>(value + 1);
}

long to_wire(ocudu_dapp_placement_v1 placement)
{
  switch (placement) {
    case OCUDU_DAPP_PLACEMENT_IN_PROCESS_V1:
      return Placement_inProcess;
    case OCUDU_DAPP_PLACEMENT_OUT_OF_PROCESS_V1:
      return Placement_supervisedProcess;
  }
  throw std::runtime_error("invalid placement");
}

ocudu_dapp_placement_v1 from_wire_placement(long value)
{
  switch (value) {
    case Placement_inProcess:
      return OCUDU_DAPP_PLACEMENT_IN_PROCESS_V1;
    case Placement_supervisedProcess:
      return OCUDU_DAPP_PLACEMENT_OUT_OF_PROCESS_V1;
    default:
      throw std::runtime_error("unsupported placement");
  }
}

backend from_wire_backend(long value)
{
  if (value < Backend_cpu || value > Backend_otherAccelerator) {
    throw std::runtime_error("unknown backend");
  }
  return static_cast<backend>(value);
}

long to_wire(ocudu_dapp_value_kind_v1 kind)
{
  if (kind < OCUDU_DAPP_VALUE_BOOL_V1 || kind > OCUDU_DAPP_VALUE_BYTES_V1) {
    throw std::runtime_error("invalid value kind");
  }
  return static_cast<long>(kind - 1);
}

ocudu_dapp_value_kind_v1 from_wire_value_kind(long kind)
{
  if (kind < 0 || kind > 5) {
    throw std::runtime_error("unknown value kind");
  }
  return static_cast<ocudu_dapp_value_kind_v1>(kind + 1);
}

void set_value(TypedValue_t& destination, const management::value& source)
{
  std::visit(
      [&destination](const auto& item) {
        using item_type = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<item_type, bool>) {
          destination.present             = TypedValue_PR_booleanValue;
          destination.choice.booleanValue = item ? 1 : 0;
        } else if constexpr (std::is_same_v<item_type, int64_t>) {
          destination.present = TypedValue_PR_signedValue;
          set_i64(destination.choice.signedValue, item);
        } else if constexpr (std::is_same_v<item_type, uint64_t>) {
          destination.present = TypedValue_PR_unsignedValue;
          set_u64(destination.choice.unsignedValue, item);
        } else if constexpr (std::is_same_v<item_type, double>) {
          destination.present          = TypedValue_PR_realValue;
          destination.choice.realValue = item;
        } else if constexpr (std::is_same_v<item_type, std::string>) {
          destination.present = TypedValue_PR_stringValue;
          set_octets(destination.choice.stringValue, item);
        } else {
          destination.present = TypedValue_PR_bytesValue;
          set_octets(destination.choice.bytesValue, item);
        }
      },
      source);
}

management::value read_value(const TypedValue_t& source)
{
  switch (source.present) {
    case TypedValue_PR_booleanValue:
      if (source.choice.booleanValue != 0 && source.choice.booleanValue != 1) {
        throw std::runtime_error("invalid boolean value");
      }
      return source.choice.booleanValue != 0;
    case TypedValue_PR_signedValue:
      return read_i64(source.choice.signedValue);
    case TypedValue_PR_unsignedValue:
      return read_u64(source.choice.unsignedValue);
    case TypedValue_PR_realValue:
      return source.choice.realValue;
    case TypedValue_PR_stringValue:
      return read_string(source.choice.stringValue, 4096);
    case TypedValue_PR_bytesValue:
      return read_bytes(source.choice.bytesValue, 1024 * 1024);
    default:
      throw std::runtime_error("unknown typed value");
  }
}

void set_header(RequestHeader_t& destination, const request_header& source)
{
  set_identifier(destination.transactionId, source.transaction_id);
  if (source.instance_id) {
    destination.instanceId = make_asn<Identifier_t>();
    set_identifier(*destination.instanceId, *source.instance_id);
  }
  if (source.expected_instance_generation) {
    destination.expectedInstanceGeneration = make_asn<Generation_t>();
    set_u64(*destination.expectedInstanceGeneration, *source.expected_instance_generation);
  }
  if (source.expected_inventory_generation) {
    destination.expectedInventoryGeneration = make_asn<Generation_t>();
    set_u64(*destination.expectedInventoryGeneration, *source.expected_inventory_generation);
  }
  destination.dryRun = source.dry_run ? 1 : 0;
}

request_header read_header(const RequestHeader_t& source)
{
  request_header result;
  result.transaction_id = read_identifier(source.transactionId);
  if (source.instanceId != nullptr) {
    result.instance_id = read_identifier(*source.instanceId);
  }
  if (source.expectedInstanceGeneration != nullptr) {
    result.expected_instance_generation = read_u64(*source.expectedInstanceGeneration);
  }
  if (source.expectedInventoryGeneration != nullptr) {
    result.expected_inventory_generation = read_u64(*source.expectedInventoryGeneration);
  }
  if (source.dryRun != 0 && source.dryRun != 1) {
    throw std::runtime_error("invalid dry-run flag");
  }
  result.dry_run = source.dryRun != 0;
  return result;
}

template <typename AsnType>
std::vector<uint8_t> encode_asn(asn_TYPE_descriptor_t& descriptor, const AsnType& value)
{
  return asn1_detail::encode_asn(descriptor, value, maximum_wire_bytes);
}

struct request_owner {
  ManagementRequest_t* value{};
  request_owner() = default;
  explicit request_owner(ManagementRequest_t* value_) : value(value_) {}
  request_owner(request_owner&& other) noexcept : value(other.value) { other.value = nullptr; }
  request_owner(const request_owner&)            = delete;
  request_owner& operator=(const request_owner&) = delete;
  ~request_owner()
  {
    if (value != nullptr) {
      ASN_STRUCT_FREE(asn_DEF_ManagementRequest, value);
    }
  }
};

struct response_owner {
  ManagementResponse_t* value{};
  response_owner() = default;
  explicit response_owner(ManagementResponse_t* value_) : value(value_) {}
  response_owner(response_owner&& other) noexcept : value(other.value) { other.value = nullptr; }
  response_owner(const response_owner&)            = delete;
  response_owner& operator=(const response_owner&) = delete;
  ~response_owner()
  {
    if (value != nullptr) {
      ASN_STRUCT_FREE(asn_DEF_ManagementResponse, value);
    }
  }
};

request_owner decode_asn_request(ocudu::span<const uint8_t> bytes)
{
  if (bytes.empty() || bytes.size() > maximum_wire_bytes) {
    throw std::runtime_error("invalid APER request size");
  }
  request_owner   result;
  asn_codec_ctx_t context{};
  context.max_stack_size = 64 * 1024;
  auto decoded           = aper_decode_complete(
      &context, &asn_DEF_ManagementRequest, reinterpret_cast<void**>(&result.value), bytes.data(), bytes.size());
  if (decoded.code != RC_OK || result.value == nullptr) {
    throw std::runtime_error("malformed APER management request");
  }
  std::array<char, 256> error{};
  std::size_t           error_size = error.size();
  if (asn_check_constraints(&asn_DEF_ManagementRequest, result.value, error.data(), &error_size) != 0) {
    throw std::runtime_error(std::string("invalid APER management request: ") + error.data());
  }
  return result;
}

response_owner decode_asn_response(ocudu::span<const uint8_t> bytes)
{
  if (bytes.empty() || bytes.size() > maximum_wire_bytes) {
    throw std::runtime_error("invalid APER response size");
  }
  response_owner  result;
  asn_codec_ctx_t context{};
  context.max_stack_size = 64 * 1024;
  auto decoded           = aper_decode_complete(
      &context, &asn_DEF_ManagementResponse, reinterpret_cast<void**>(&result.value), bytes.data(), bytes.size());
  if (decoded.code != RC_OK || result.value == nullptr) {
    throw std::runtime_error("malformed APER management response");
  }
  std::array<char, 256> error{};
  std::size_t           error_size = error.size();
  if (asn_check_constraints(&asn_DEF_ManagementResponse, result.value, error.data(), &error_size) != 0) {
    throw std::runtime_error(std::string("invalid APER management response: ") + error.data());
  }
  return result;
}

void set_config_entry(ConfigEntry_t& destination, const config_entry& source)
{
  set_octets(destination.key, source.key);
  destination.value = make_asn<TypedValue_t>();
  set_value(*destination.value, source.entry_value);
}

config_entry read_config_entry(const ConfigEntry_t& source)
{
  if (source.value == nullptr) {
    throw std::runtime_error("config entry has no value");
  }
  return {read_string(source.key, 128), read_value(*source.value)};
}

void set_operation_result(OperationResult_t& destination, const operation_result& source)
{
  destination.code = to_wire(source.code);
  if (source.inventory_generation != 0) {
    destination.inventoryGeneration = make_asn<Generation_t>();
    set_u64(*destination.inventoryGeneration, source.inventory_generation);
  }
  if (source.instance_generation != 0) {
    destination.instanceGeneration = make_asn<Generation_t>();
    set_u64(*destination.instanceGeneration, source.instance_generation);
  }
  if (source.resource_generation != 0) {
    destination.resourceGeneration = make_asn<Generation_t>();
    set_u64(*destination.resourceGeneration, source.resource_generation);
  }
  if (source.instance_id) {
    destination.instanceId = make_asn<Identifier_t>();
    set_identifier(*destination.instanceId, *source.instance_id);
  }
  destination.state  = make_asn<LifecycleState_t>();
  *destination.state = to_wire(source.state);
  if (!source.detail.empty()) {
    destination.detail = make_asn<OCTET_STRING_t>();
    const std::string_view detail(source.detail.data(), std::min(source.detail.size(), maximum_text_bytes));
    set_text(*destination.detail, detail);
  }
}

operation_result read_operation_result(const OperationResult_t& source)
{
  operation_result result;
  result.code = from_wire_result(source.code);
  if (source.inventoryGeneration != nullptr) {
    result.inventory_generation = read_u64(*source.inventoryGeneration);
  }
  if (source.instanceGeneration != nullptr) {
    result.instance_generation = read_u64(*source.instanceGeneration);
  }
  if (source.resourceGeneration != nullptr) {
    result.resource_generation = read_u64(*source.resourceGeneration);
  }
  if (source.instanceId != nullptr) {
    result.instance_id = read_identifier(*source.instanceId);
  }
  if (source.state != nullptr) {
    result.state = from_wire_state(*source.state);
  }
  if (source.detail != nullptr) {
    result.detail = read_string(*source.detail, maximum_text_bytes);
  }
  return result;
}

void set_route_target(PuschRouteTarget_t& destination, const pusch_route_target& source)
{
  switch (source.type) {
    case pusch_route_target::kind::primary:
      destination.present = PuschRouteTarget_PR_primary;
      return;
    case pusch_route_target::kind::conventional:
      destination.present = PuschRouteTarget_PR_conventional;
      return;
    case pusch_route_target::kind::instance:
      destination.present = PuschRouteTarget_PR_instance;
      set_identifier(destination.choice.instance, source.instance_id);
      return;
  }
  throw std::runtime_error("unsupported PUSCH route target");
}

pusch_route_target read_route_target(const PuschRouteTarget_t& source)
{
  switch (source.present) {
    case PuschRouteTarget_PR_primary:
      return {};
    case PuschRouteTarget_PR_conventional:
      return {pusch_route_target::kind::conventional, {}};
    case PuschRouteTarget_PR_instance:
      return {pusch_route_target::kind::instance, read_identifier(source.choice.instance)};
    default:
      throw std::runtime_error("unsupported PUSCH route target");
  }
}

void set_route_entry(PuschRouteEntry_t& destination, const pusch_route_entry& source)
{
  set_optional_long(destination.cellIndex, source.cell_index);
  destination.rnti   = source.rnti;
  destination.target = make_asn<PuschRouteTarget_t>();
  set_route_target(*destination.target, source.target);
}

pusch_route_entry read_route_entry(const PuschRouteEntry_t& source)
{
  pusch_route_entry entry;
  if (source.cellIndex != nullptr) {
    entry.cell_index = static_cast<uint16_t>(*source.cellIndex);
  }
  if (source.target == nullptr) {
    throw std::runtime_error("PUSCH route entry has no target");
  }
  entry.rnti   = static_cast<uint16_t>(source.rnti);
  entry.target = read_route_target(*source.target);
  return entry;
}

template <typename List>
std::vector<pusch_route_entry> read_route_entries(const List& entries)
{
  if (static_cast<std::size_t>(entries.list.count) > pusch_route_max_entries) {
    throw std::runtime_error("PUSCH route list exceeds bound");
  }
  std::vector<pusch_route_entry> result;
  result.reserve(entries.list.count);
  for (int index = 0; index != entries.list.count; ++index) {
    if (entries.list.array[index] == nullptr) {
      throw std::runtime_error("null PUSCH route entry");
    }
    result.push_back(read_route_entry(*entries.list.array[index]));
  }
  return result;
}

void set_routing_result(PuschRoutingResult_t& destination, const pusch_routing_result& source)
{
  set_u64(destination.generation, source.generation);
  destination.slotsPerHook  = source.slots_per_hook;
  destination.defaultTarget = make_asn<PuschRouteTarget_t>();
  set_route_target(*destination.defaultTarget, source.default_target);
  for (const auto& entry : source.entries) {
    set_route_entry(append_asn<PuschRouteEntry_t>(destination.entries), entry);
  }
  for (const auto& binding : source.bindings) {
    auto& wire = append_asn<PuschRouteBinding_t>(destination.bindings);
    set_identifier(wire.instanceId, binding.instance_id);
    wire.hook = static_cast<long>(binding.hook);
    wire.slot = binding.slot;
  }
  set_u64(destination.unavailableLookups, source.unavailable_lookups);
}

pusch_routing_result read_routing_result(const PuschRoutingResult_t& source)
{
  pusch_routing_result result;
  result.generation = read_u64(source.generation);
  if (source.defaultTarget == nullptr) {
    throw std::runtime_error("PUSCH routing result has no default target");
  }
  result.slots_per_hook = static_cast<uint8_t>(source.slotsPerHook);
  result.default_target = read_route_target(*source.defaultTarget);
  result.entries        = read_route_entries(source.entries);
  if (source.bindings.list.count > static_cast<int>(nof_pusch_route_hooks * pusch_route_max_slots_per_hook)) {
    throw std::runtime_error("PUSCH route binding list exceeds bound");
  }
  for (int index = 0; index != source.bindings.list.count; ++index) {
    const auto* binding = source.bindings.list.array[index];
    if (binding == nullptr || binding->hook < PuschRouteHook_channelEstimator ||
        binding->hook > PuschRouteHook_receiver) {
      throw std::runtime_error("malformed PUSCH route binding");
    }
    result.bindings.push_back({read_identifier(binding->instanceId),
                               static_cast<pusch_route_hook>(binding->hook),
                               static_cast<uint8_t>(binding->slot)});
  }
  result.unavailable_lookups = read_u64(source.unavailableLookups);
  return result;
}

} // namespace

codec_result<std::vector<uint8_t>> aper_codec::encode(const management_request& request) noexcept
{
  try {
    request_owner owner{make_asn<ManagementRequest_t>()};
    auto*         wire = owner.value;
    wire->header       = make_asn<RequestHeader_t>();
    wire->operation    = make_asn<ManagementOperation_t>();
    std::visit(
        [wire](const auto& operation) {
          using operation_type = std::decay_t<decltype(operation)>;
          set_header(*wire->header, operation.header);
          if constexpr (std::is_same_v<operation_type, inventory_query>) {
            wire->operation->present          = ManagementOperation_PR_inventory;
            wire->operation->choice.inventory = make_asn<InventoryQuery_t>();
            if (!operation.include_packages) {
              wire->operation->choice.inventory->includePackages  = make_asn<BOOLEAN_t>();
              *wire->operation->choice.inventory->includePackages = 0;
            }
            if (!operation.include_instances) {
              wire->operation->choice.inventory->includeInstances  = make_asn<BOOLEAN_t>();
              *wire->operation->choice.inventory->includeInstances = 0;
            }
          } else if constexpr (std::is_same_v<operation_type, lifecycle_control>) {
            wire->operation->present          = ManagementOperation_PR_lifecycle;
            wire->operation->choice.lifecycle = make_asn<LifecycleControl_t>();
            auto& value                       = *wire->operation->choice.lifecycle;
            value.action                      = static_cast<long>(operation.action);
            if (operation.package_id) {
              value.packageId = make_asn<Identifier_t>();
              set_identifier(*value.packageId, *operation.package_id);
            }
            if (operation.placement) {
              value.placement  = make_asn<Placement_t>();
              *value.placement = to_wire(*operation.placement);
            }
            if (operation.selected_backend) {
              value.backend  = make_asn<Backend_t>();
              *value.backend = static_cast<long>(*operation.selected_backend);
            }
            if (operation.timeout_us != 1'000'000) {
              if (operation.timeout_us > std::numeric_limits<unsigned long>::max()) {
                throw std::runtime_error("lifecycle timeout is too large");
              }
              using timeout_type = std::remove_pointer_t<decltype(value.timeoutUs)>;
              value.timeoutUs    = make_asn<timeout_type>();
              *value.timeoutUs   = static_cast<unsigned long>(operation.timeout_us);
            }
          } else if constexpr (std::is_same_v<operation_type, config_query>) {
            wire->operation->present                  = ManagementOperation_PR_configQuery;
            wire->operation->choice.configQuery       = make_asn<ConfigQuery_t>();
            wire->operation->choice.configQuery->kind = static_cast<long>(operation.kind);
          } else if constexpr (std::is_same_v<operation_type, config_set>) {
            wire->operation->present          = ManagementOperation_PR_configSet;
            wire->operation->choice.configSet = make_asn<ConfigSet_t>();
            auto& value                       = *wire->operation->choice.configSet;
            set_u64(value.expectedConfigGeneration, operation.expected_config_generation);
            value.boundary = static_cast<long>(operation.boundary) - 1;
            for (const auto& entry : operation.entries) {
              set_config_entry(append_asn<ConfigEntry_t>(value.entries), entry);
            }
          } else if constexpr (std::is_same_v<operation_type, model_control>) {
            wire->operation->present      = ManagementOperation_PR_model;
            wire->operation->choice.model = make_asn<ModelControl_t>();
            auto& value                   = *wire->operation->choice.model;
            value.action                  = static_cast<long>(operation.action) - 1;
            set_octets(value.modelId, operation.model_id);
            if (!operation.model_version.empty()) {
              value.modelVersion = make_asn<Version_t>();
              set_octets(*value.modelVersion, operation.model_version);
            }
            set_u64(value.expectedModelGeneration, operation.expected_model_generation);
            if (!operation.artifact.empty()) {
              value.artifact = make_asn<OCTET_STRING_t>();
              set_octets(*value.artifact, operation.artifact);
            }
          } else if constexpr (std::is_same_v<operation_type, metric_query>) {
            wire->operation->present              = ManagementOperation_PR_metrics;
            wire->operation->choice.metrics       = make_asn<MetricQuery_t>();
            wire->operation->choice.metrics->kind = static_cast<long>(operation.kind);
          } else if constexpr (std::is_same_v<operation_type, incident_query>) {
            wire->operation->present          = ManagementOperation_PR_incidents;
            wire->operation->choice.incidents = make_asn<IncidentQuery_t>();
            auto& value                       = *wire->operation->choice.incidents;
            value.action                      = static_cast<long>(operation.action);
            if (operation.cause) {
              value.cause  = make_asn<IncidentCause_t>();
              *value.cause = static_cast<long>(*operation.cause);
            }
            if (operation.after_incident_id) {
              using counter_type    = std::remove_pointer_t<decltype(value.afterIncidentId)>;
              value.afterIncidentId = make_asn<counter_type>();
              set_u64(*value.afterIncidentId, *operation.after_incident_id);
            }
            if (operation.maximum_records != 100) {
              if (operation.maximum_records > std::numeric_limits<long>::max()) {
                throw std::runtime_error("incident result bound is too large");
              }
              value.maximumRecords  = make_asn<long>();
              *value.maximumRecords = static_cast<long>(operation.maximum_records);
            }
            if (operation.header.instance_id) {
              value.instanceId = make_asn<Identifier_t>();
              set_identifier(*value.instanceId, *operation.header.instance_id);
            }
          } else if constexpr (std::is_same_v<operation_type, health_query>) {
            wire->operation->present       = ManagementOperation_PR_health;
            wire->operation->choice.health = 0;
          } else if constexpr (std::is_same_v<operation_type, job_query>) {
            wire->operation->present             = ManagementOperation_PR_jobs;
            wire->operation->choice.jobs         = make_asn<JobQuery_t>();
            wire->operation->choice.jobs->action = static_cast<long>(operation.action);
            if (operation.job_id) {
              wire->operation->choice.jobs->jobId = make_asn<Identifier_t>();
              set_identifier(*wire->operation->choice.jobs->jobId, *operation.job_id);
            }
          } else if constexpr (std::is_same_v<operation_type, quiet_reservation_query>) {
            wire->operation->present                  = ManagementOperation_PR_quietReservations;
            wire->operation->choice.quietReservations = make_asn<QuietReservationQuery_t>();
            auto& value                               = *wire->operation->choice.quietReservations;
            if (operation.cell_id) {
              using cell_type = std::remove_pointer_t<decltype(value.cellId)>;
              value.cellId    = make_asn<cell_type>();
              *value.cellId   = *operation.cell_id;
            }
            if (operation.maximum_records != 100) {
              if (operation.maximum_records > std::numeric_limits<long>::max()) {
                throw std::runtime_error("quiet-reservation result bound is too large");
              }
              value.maximumRecords  = make_asn<long>();
              *value.maximumRecords = static_cast<long>(operation.maximum_records);
            }
          } else if constexpr (std::is_same_v<operation_type, data_subscription_control>) {
            wire->operation->present                  = ManagementOperation_PR_dataSubscriptions;
            wire->operation->choice.dataSubscriptions = make_asn<DataSubscriptionControl_t>();
            auto& value                               = *wire->operation->choice.dataSubscriptions;
            value.action                              = static_cast<long>(operation.action);
            value.dropPolicy                          = static_cast<long>(operation.drop_policy);
            if (operation.subscription_id) {
              value.subscriptionId = make_asn<Identifier_t>();
              set_identifier(*value.subscriptionId, *operation.subscription_id);
            }
            if (operation.interface_id != 0) {
              value.interfaceId  = make_asn<long>();
              *value.interfaceId = operation.interface_id;
            }
            if (operation.cell_id) {
              value.cellId  = make_asn<long>();
              *value.cellId = *operation.cell_id;
            }
            if (operation.sample_every_n != 1) {
              value.sampleEveryN  = make_asn<long>();
              *value.sampleEveryN = operation.sample_every_n;
            }
            if (operation.queue_depth_chunks != 64) {
              value.queueDepthChunks  = make_asn<long>();
              *value.queueDepthChunks = operation.queue_depth_chunks;
            }
            if (operation.maximum_chunk_bytes != maximum_data_bytes) {
              value.maximumChunkBytes  = make_asn<long>();
              *value.maximumChunkBytes = operation.maximum_chunk_bytes;
            }
            if (operation.lease_us != 1'000'000) {
              if (operation.lease_us > static_cast<uint64_t>(std::numeric_limits<long>::max())) {
                throw std::runtime_error("portable E3 lease is too large");
              }
              value.leaseUs  = make_asn<long>();
              *value.leaseUs = operation.lease_us;
            }
          } else if constexpr (std::is_same_v<operation_type, data_poll_query>) {
            wire->operation->present         = ManagementOperation_PR_dataPoll;
            wire->operation->choice.dataPoll = make_asn<DataPollQuery_t>();
            auto& value                      = *wire->operation->choice.dataPoll;
            set_identifier(value.subscriptionId, operation.subscription_id);
            if (operation.maximum_chunks != maximum_data_chunks) {
              value.maximumChunks  = make_asn<long>();
              *value.maximumChunks = operation.maximum_chunks;
            }
          } else if constexpr (std::is_same_v<operation_type, ran_control>) {
            wire->operation->present           = ManagementOperation_PR_ranControl;
            wire->operation->choice.ranControl = make_asn<RanControl_t>();
            auto& value                        = *wire->operation->choice.ranControl;
            value.action                       = static_cast<long>(operation.action);
            value.servingPci                   = operation.serving_pci;
            value.rnti                         = operation.rnti;
            set_optional_long(value.targetPci, operation.target_pci);
            set_optional_long(value.portId, operation.port_id);
            set_optional_real(value.gainDb, operation.gain_db);
          } else if constexpr (std::is_same_v<operation_type, cell_info_query>) {
            wire->operation->present         = ManagementOperation_PR_cellInfo;
            wire->operation->choice.cellInfo = make_asn<CellInfoQuery_t>();
          } else if constexpr (std::is_same_v<operation_type, catalog_admit>) {
            wire->operation->present             = ManagementOperation_PR_catalogAdmit;
            wire->operation->choice.catalogAdmit = make_asn<CatalogAdmit_t>();
            set_identifier(wire->operation->choice.catalogAdmit->packageId, operation.package_id);
          } else if constexpr (std::is_same_v<operation_type, pusch_routing>) {
            wire->operation->present             = ManagementOperation_PR_puschRouting;
            wire->operation->choice.puschRouting = make_asn<PuschRouting_t>();
            auto& value                          = *wire->operation->choice.puschRouting;
            value.action                         = static_cast<long>(operation.action);
            if (operation.default_target) {
              value.defaultTarget = make_asn<PuschRouteTarget_t>();
              set_route_target(*value.defaultTarget, *operation.default_target);
            }
            if (!operation.entries.empty()) {
              using entry_list = std::remove_pointer_t<decltype(value.entries)>;
              value.entries    = make_asn<entry_list>();
              for (const auto& entry : operation.entries) {
                set_route_entry(append_asn<PuschRouteEntry_t>(*value.entries), entry);
              }
            }
          }
        },
        request);
    return {encode_asn(asn_DEF_ManagementRequest, *wire), {}};
  } catch (const std::exception& exception) {
    return {std::nullopt, exception.what()};
  } catch (...) {
    return {std::nullopt, "unknown APER request encoding failure"};
  }
}

codec_result<management_request> aper_codec::decode_request(ocudu::span<const uint8_t> bytes) noexcept
{
  try {
    auto owner = decode_asn_request(bytes);
    if (owner.value->header == nullptr || owner.value->operation == nullptr) {
      throw std::runtime_error("management request is incomplete");
    }
    const request_header common = read_header(*owner.value->header);
    const auto&          wire   = *owner.value->operation;
    switch (wire.present) {
      case ManagementOperation_PR_inventory: {
        if (wire.choice.inventory == nullptr) {
          break;
        }
        inventory_query result;
        result.header = common;
        result.include_packages =
            wire.choice.inventory->includePackages == nullptr || *wire.choice.inventory->includePackages != 0;
        result.include_instances =
            wire.choice.inventory->includeInstances == nullptr || *wire.choice.inventory->includeInstances != 0;
        return {management_request{std::move(result)}, {}};
      }
      case ManagementOperation_PR_lifecycle: {
        if (wire.choice.lifecycle == nullptr || wire.choice.lifecycle->action < LifecycleAction_load ||
            wire.choice.lifecycle->action > LifecycleAction_breakerProbe) {
          break;
        }
        lifecycle_control result;
        result.header = common;
        result.action = static_cast<ocudu::dapp::e3::lifecycle_action>(wire.choice.lifecycle->action);
        if (wire.choice.lifecycle->packageId != nullptr) {
          result.package_id = read_identifier(*wire.choice.lifecycle->packageId);
        }
        if (wire.choice.lifecycle->placement != nullptr) {
          result.placement = from_wire_placement(*wire.choice.lifecycle->placement);
        }
        if (wire.choice.lifecycle->backend != nullptr) {
          result.selected_backend = from_wire_backend(*wire.choice.lifecycle->backend);
        }
        result.timeout_us = wire.choice.lifecycle->timeoutUs != nullptr ? *wire.choice.lifecycle->timeoutUs : 1'000'000;
        return {management_request{std::move(result)}, {}};
      }
      case ManagementOperation_PR_configQuery: {
        if (wire.choice.configQuery == nullptr || wire.choice.configQuery->kind < ConfigQuery__kind_schema ||
            wire.choice.configQuery->kind > ConfigQuery__kind_effective) {
          break;
        }
        return {management_request{config_query{common, static_cast<query_kind>(wire.choice.configQuery->kind)}}, {}};
      }
      case ManagementOperation_PR_configSet: {
        if (wire.choice.configSet == nullptr || wire.choice.configSet->boundary < ConfigSet__boundary_immediate ||
            wire.choice.configSet->boundary > ConfigSet__boundary_restart ||
            static_cast<std::size_t>(wire.choice.configSet->entries.list.count) > maximum_items) {
          break;
        }
        config_set result;
        result.header                     = common;
        result.expected_config_generation = read_u64(wire.choice.configSet->expectedConfigGeneration);
        result.boundary = static_cast<ocudu_dapp_config_boundary_v1>(wire.choice.configSet->boundary + 1);
        result.entries.reserve(wire.choice.configSet->entries.list.count);
        for (int index = 0; index != wire.choice.configSet->entries.list.count; ++index) {
          if (wire.choice.configSet->entries.list.array[index] == nullptr) {
            throw std::runtime_error("null config entry");
          }
          result.entries.push_back(read_config_entry(*wire.choice.configSet->entries.list.array[index]));
        }
        return {management_request{std::move(result)}, {}};
      }
      case ManagementOperation_PR_model: {
        if (wire.choice.model == nullptr || wire.choice.model->action < ModelAction_stage ||
            wire.choice.model->action > ModelAction_retire) {
          break;
        }
        model_control result;
        result.header                    = common;
        result.action                    = static_cast<ocudu_dapp_model_action_v1>(wire.choice.model->action + 1);
        result.model_id                  = read_string(wire.choice.model->modelId, 128);
        result.expected_model_generation = read_u64(wire.choice.model->expectedModelGeneration);
        if (wire.choice.model->modelVersion != nullptr) {
          result.model_version = read_string(*wire.choice.model->modelVersion, 64);
        }
        if (wire.choice.model->artifact != nullptr) {
          result.artifact = read_bytes(*wire.choice.model->artifact, 1024 * 1024);
        }
        return {management_request{std::move(result)}, {}};
      }
      case ManagementOperation_PR_metrics: {
        if (wire.choice.metrics == nullptr || wire.choice.metrics->kind < MetricQuery__kind_schema ||
            wire.choice.metrics->kind > MetricQuery__kind_snapshot) {
          break;
        }
        return {management_request{metric_query{common, static_cast<query_kind>(wire.choice.metrics->kind)}}, {}};
      }
      case ManagementOperation_PR_incidents: {
        if (wire.choice.incidents == nullptr || wire.choice.incidents->action < IncidentQuery__action_list ||
            wire.choice.incidents->action > IncidentQuery__action_clearThrough) {
          break;
        }
        incident_query result;
        result.header = common;
        result.action = static_cast<incident_query_action>(wire.choice.incidents->action);
        if (wire.choice.incidents->instanceId != nullptr) {
          const auto instance = read_identifier(*wire.choice.incidents->instanceId);
          if (result.header.instance_id && *result.header.instance_id != instance) {
            throw std::runtime_error("conflicting incident instance IDs");
          }
          result.header.instance_id = instance;
        }
        if (wire.choice.incidents->cause != nullptr) {
          if (*wire.choice.incidents->cause < IncidentCause_returnedError ||
              *wire.choice.incidents->cause > IncidentCause_policyRejection) {
            break;
          }
          result.cause = static_cast<incident_cause>(*wire.choice.incidents->cause);
        }
        if (wire.choice.incidents->afterIncidentId != nullptr) {
          result.after_incident_id = read_u64(*wire.choice.incidents->afterIncidentId);
        }
        result.maximum_records = wire.choice.incidents->maximumRecords != nullptr
                                     ? static_cast<std::size_t>(*wire.choice.incidents->maximumRecords)
                                     : 100;
        return {management_request{std::move(result)}, {}};
      }
      case ManagementOperation_PR_health:
        return {management_request{health_query{common}}, {}};
      case ManagementOperation_PR_jobs: {
        if (wire.choice.jobs == nullptr || wire.choice.jobs->action < JobQuery__action_get ||
            wire.choice.jobs->action > JobQuery__action_cancel) {
          break;
        }
        job_query result;
        result.header = common;
        result.action = static_cast<job_query_action>(wire.choice.jobs->action);
        if (wire.choice.jobs->jobId != nullptr) {
          result.job_id = read_identifier(*wire.choice.jobs->jobId);
        }
        return {management_request{std::move(result)}, {}};
      }
      case ManagementOperation_PR_quietReservations: {
        if (wire.choice.quietReservations == nullptr) {
          break;
        }
        quiet_reservation_query result;
        result.header = common;
        if (wire.choice.quietReservations->cellId != nullptr) {
          result.cell_id = static_cast<uint32_t>(*wire.choice.quietReservations->cellId);
        }
        result.maximum_records = wire.choice.quietReservations->maximumRecords != nullptr
                                     ? static_cast<std::size_t>(*wire.choice.quietReservations->maximumRecords)
                                     : 100;
        return {management_request{std::move(result)}, {}};
      }
      case ManagementOperation_PR_dataSubscriptions: {
        if (wire.choice.dataSubscriptions == nullptr ||
            wire.choice.dataSubscriptions->action < DataSubscriptionAction_create ||
            wire.choice.dataSubscriptions->action > DataSubscriptionAction_list ||
            wire.choice.dataSubscriptions->dropPolicy < DataDropPolicy_dropNewest ||
            wire.choice.dataSubscriptions->dropPolicy > DataDropPolicy_coalesceLatest) {
          break;
        }
        data_subscription_control result;
        result.header = common;
        result.action = static_cast<data_subscription_action>(wire.choice.dataSubscriptions->action);
        if (wire.choice.dataSubscriptions->subscriptionId != nullptr) {
          result.subscription_id = read_identifier(*wire.choice.dataSubscriptions->subscriptionId);
        }
        result.interface_id = wire.choice.dataSubscriptions->interfaceId != nullptr
                                  ? static_cast<uint32_t>(*wire.choice.dataSubscriptions->interfaceId)
                                  : 0;
        if (wire.choice.dataSubscriptions->cellId != nullptr) {
          result.cell_id = static_cast<uint32_t>(*wire.choice.dataSubscriptions->cellId);
        }
        result.sample_every_n      = wire.choice.dataSubscriptions->sampleEveryN != nullptr
                                         ? static_cast<uint32_t>(*wire.choice.dataSubscriptions->sampleEveryN)
                                         : 1;
        result.queue_depth_chunks  = wire.choice.dataSubscriptions->queueDepthChunks != nullptr
                                         ? static_cast<uint32_t>(*wire.choice.dataSubscriptions->queueDepthChunks)
                                         : 64;
        result.maximum_chunk_bytes = wire.choice.dataSubscriptions->maximumChunkBytes != nullptr
                                         ? static_cast<uint32_t>(*wire.choice.dataSubscriptions->maximumChunkBytes)
                                         : maximum_data_bytes;
        result.lease_us            = wire.choice.dataSubscriptions->leaseUs != nullptr
                                         ? static_cast<uint64_t>(*wire.choice.dataSubscriptions->leaseUs)
                                         : 1'000'000;
        result.drop_policy         = static_cast<portable_drop_policy>(wire.choice.dataSubscriptions->dropPolicy);
        return {management_request{std::move(result)}, {}};
      }
      case ManagementOperation_PR_dataPoll: {
        if (wire.choice.dataPoll == nullptr) {
          break;
        }
        data_poll_query result;
        result.header          = common;
        result.subscription_id = read_identifier(wire.choice.dataPoll->subscriptionId);
        result.maximum_chunks  = wire.choice.dataPoll->maximumChunks != nullptr
                                     ? static_cast<uint32_t>(*wire.choice.dataPoll->maximumChunks)
                                     : maximum_data_chunks;
        return {management_request{std::move(result)}, {}};
      }
      case ManagementOperation_PR_ranControl: {
        if (wire.choice.ranControl == nullptr) {
          break;
        }
        const auto& value = *wire.choice.ranControl;
        ran_control result;
        result.header = common;
        switch (value.action) {
          case RanControlAction_releaseUeToIdle:
            result.action = ran_control_action::release_ue_to_idle;
            break;
          case RanControlAction_handoverUe:
            result.action = ran_control_action::handover_ue;
            break;
          case RanControlAction_setTxGain:
            result.action = ran_control_action::set_tx_gain;
            break;
          case RanControlAction_setRxGain:
            result.action = ran_control_action::set_rx_gain;
            break;
          default:
            return {std::nullopt, "unsupported RAN control action"};
        }
        result.serving_pci = static_cast<uint16_t>(value.servingPci);
        result.rnti        = static_cast<uint16_t>(value.rnti);
        if (value.targetPci != nullptr) {
          result.target_pci = static_cast<uint16_t>(*value.targetPci);
        }
        if (value.portId != nullptr) {
          result.port_id = static_cast<uint8_t>(*value.portId);
        }
        result.gain_db = read_optional_real(value.gainDb);
        return {management_request{std::move(result)}, {}};
      }
      case ManagementOperation_PR_cellInfo: {
        cell_info_query result;
        result.header = common;
        return {management_request{std::move(result)}, {}};
      }
      case ManagementOperation_PR_catalogAdmit: {
        if (wire.choice.catalogAdmit == nullptr) {
          break;
        }
        catalog_admit result;
        result.header     = common;
        result.package_id = read_identifier(wire.choice.catalogAdmit->packageId);
        return {management_request{std::move(result)}, {}};
      }
      case ManagementOperation_PR_puschRouting: {
        if (wire.choice.puschRouting == nullptr) {
          break;
        }
        const auto&   value = *wire.choice.puschRouting;
        pusch_routing result;
        result.header = common;
        if (value.action < PuschRoutingAction_query || value.action > PuschRoutingAction_clear) {
          return {std::nullopt, "unsupported PUSCH routing action"};
        }
        result.action = static_cast<pusch_routing_action>(value.action);
        if (value.defaultTarget != nullptr) {
          result.default_target = read_route_target(*value.defaultTarget);
        }
        if (value.entries != nullptr) {
          result.entries = read_route_entries(*value.entries);
        }
        return {management_request{std::move(result)}, {}};
      }
      default:
        break;
    }
    return {std::nullopt, "unsupported or malformed management operation"};
  } catch (const std::exception& exception) {
    return {std::nullopt, exception.what()};
  } catch (...) {
    return {std::nullopt, "unknown APER request decoding failure"};
  }
}

namespace {

void set_interface_record(InterfaceRecord_t& destination, const interface_record& source)
{
  destination.interfaceId    = source.interface_id;
  destination.interfaceMajor = source.interface_major;
  destination.interfaceMinor = source.interface_minor;
  if (source.dapp_class < OCUDU_DAPP_CLASS_A_INLINE_V1 || source.dapp_class > OCUDU_DAPP_CLASS_C_OPEN_LOOP_V1 ||
      source.threading < OCUDU_DAPP_THREADING_PER_WORKER_V1 || source.threading > OCUDU_DAPP_THREADING_SERIALIZED_V1) {
    throw std::runtime_error("invalid interface classification");
  }
  destination.Class     = static_cast<long>(source.dapp_class - 1);
  destination.threading = static_cast<long>(source.threading - 1);
  set_bits(destination.placementMask, source.placement_mask, 1);
  set_bits(destination.capabilityBits, source.capability_bits, 8);
  using requirement_type          = std::remove_pointer_t<decltype(destination.bindingRequirement)>;
  destination.bindingRequirement  = make_asn<requirement_type>();
  *destination.bindingRequirement = source.requirement == binding_requirement::optional ? 1 : 0;
  using state_type                = std::remove_pointer_t<decltype(destination.bindingState)>;
  destination.bindingState        = make_asn<state_type>();
  *destination.bindingState       = static_cast<long>(source.state);
  if (source.pusch) {
    destination.puschProfile                 = make_asn<PuschProfile_t>();
    destination.puschProfile->maxPrb         = source.pusch->max_nof_prb;
    destination.puschProfile->maxRxPorts     = source.pusch->max_nof_rx_ports;
    destination.puschProfile->maxLayers      = source.pusch->max_nof_layers;
    destination.puschProfile->modulationMask = source.pusch->modulation_mask;
    destination.puschProfile->dmrsTypeMask   = source.pusch->dmrs_type_mask;
    destination.puschProfile->flags          = source.pusch->flags;
  }
  if (source.srs) {
    destination.srsProfile                    = make_asn<SrsProfile_t>();
    destination.srsProfile->maxRxPorts        = source.srs->max_nof_rx_ports;
    destination.srsProfile->maxTxPorts        = source.srs->max_nof_tx_ports;
    destination.srsProfile->maxSequenceLength = source.srs->max_sequence_length;
    destination.srsProfile->combMask          = source.srs->comb_mask;
  }
}

interface_record read_interface_record(const InterfaceRecord_t& source)
{
  if (source.Class < DappClass_residentInlineL1 || source.Class > DappClass_openLoopAnalytics ||
      source.threading < InterfaceRecord__threading_perWorker ||
      source.threading > InterfaceRecord__threading_serialized || source.interfaceMajor < 0 ||
      source.interfaceMajor > std::numeric_limits<uint16_t>::max() || source.interfaceMinor < 0 ||
      source.interfaceMinor > std::numeric_limits<uint16_t>::max()) {
    throw std::runtime_error("invalid interface record");
  }
  std::optional<interface_record::pusch_profile> pusch;
  binding_requirement                            requirement = binding_requirement::required;
  binding_state                                  state       = binding_state::unbound;
  if (source.bindingRequirement != nullptr) {
    if (*source.bindingRequirement < 0 || *source.bindingRequirement > 1) {
      throw std::runtime_error("invalid interface binding requirement");
    }
    requirement = *source.bindingRequirement == 1 ? binding_requirement::optional : binding_requirement::required;
  }
  if (source.bindingState != nullptr) {
    if (*source.bindingState < 0 || *source.bindingState > 2) {
      throw std::runtime_error("invalid interface binding state");
    }
    state = static_cast<binding_state>(*source.bindingState);
  }
  if (source.puschProfile != nullptr) {
    const auto& profile = *source.puschProfile;
    if (profile.maxPrb < 1 || profile.maxPrb > 275 || profile.maxRxPorts < 1 || profile.maxRxPorts > 64 ||
        profile.maxLayers < 1 || profile.maxLayers > 4 || profile.modulationMask < 1 || profile.modulationMask > 15 ||
        profile.dmrsTypeMask < 1 || profile.dmrsTypeMask > 3 || profile.flags < 0 ||
        static_cast<uint64_t>(profile.flags) > std::numeric_limits<uint32_t>::max()) {
      throw std::runtime_error("invalid PUSCH profile");
    }
    pusch = interface_record::pusch_profile{static_cast<uint32_t>(profile.maxPrb),
                                            static_cast<uint32_t>(profile.maxRxPorts),
                                            static_cast<uint32_t>(profile.maxLayers),
                                            static_cast<uint32_t>(profile.modulationMask),
                                            static_cast<uint32_t>(profile.dmrsTypeMask),
                                            static_cast<uint32_t>(profile.flags)};
  }
  std::optional<interface_record::srs_profile> srs;
  if (source.srsProfile != nullptr) {
    const auto& profile = *source.srsProfile;
    if (profile.maxRxPorts < 1 || profile.maxRxPorts > 8 || profile.maxTxPorts < 1 || profile.maxTxPorts > 4 ||
        profile.maxSequenceLength < 1 || profile.maxSequenceLength > 1632 || profile.combMask < 1 ||
        profile.combMask > 3) {
      throw std::runtime_error("invalid SRS profile");
    }
    srs = interface_record::srs_profile{static_cast<uint32_t>(profile.maxRxPorts),
                                        static_cast<uint32_t>(profile.maxTxPorts),
                                        static_cast<uint32_t>(profile.maxSequenceLength),
                                        static_cast<uint32_t>(profile.combMask)};
  }
  return {static_cast<uint32_t>(source.interfaceId),
          static_cast<uint16_t>(source.interfaceMajor),
          static_cast<uint16_t>(source.interfaceMinor),
          static_cast<ocudu_dapp_class_v1>(source.Class + 1),
          static_cast<uint32_t>(read_bits(source.placementMask, 1)),
          static_cast<ocudu_dapp_threading_v1>(source.threading + 1),
          read_bits(source.capabilityBits, 8),
          pusch,
          requirement,
          state,
          srs};
}

void set_package_record(PackageRecord_t& destination, const package_record& source)
{
  set_identifier(destination.packageId, source.package_id);
  set_octets(destination.globalName, source.global_name);
  set_octets(destination.vendor, source.vendor);
  set_octets(destination.version, source.version);
  set_octets(destination.contentHash, source.content_digest.data(), source.content_digest.size());
  set_bits(destination.placementMask, source.placement_mask, 1);
  for (backend item : source.backends) {
    append_asn<Backend_t>(destination.backends) = static_cast<long>(item);
  }
  if (!source.interfaces.empty()) {
    using interface_list   = std::remove_pointer_t<decltype(destination.interfaces)>;
    destination.interfaces = make_asn<interface_list>();
    for (const auto& item : source.interfaces) {
      set_interface_record(append_asn<InterfaceRecord_t>(*destination.interfaces), item);
    }
  }
  destination.signatureStatus  = make_asn<PackageSignatureStatus_t>();
  *destination.signatureStatus = source.signature_status == package_signature_status::verified
                                     ? PackageSignatureStatus_verified
                                     : PackageSignatureStatus_unsignedPackage;
  if (source.signature_status == package_signature_status::verified) {
    destination.signingKeyId = make_asn<Hash256_t>();
    set_octets(*destination.signingKeyId, source.signing_key_id.data(), source.signing_key_id.size());
    destination.signatureSequence = make_asn<Unsigned64_t>();
    set_u64(*destination.signatureSequence, source.signature_sequence);
    destination.trustPolicyEpoch = make_asn<Unsigned64_t>();
    set_u64(*destination.trustPolicyEpoch, source.trust_policy_epoch);
  }
  if (source.sbom_verified) {
    if (source.manifest_schema_version != 1) {
      throw std::runtime_error("invalid package provenance metadata");
    }
    destination.manifestSchemaVersion  = make_asn<long>();
    *destination.manifestSchemaVersion = source.manifest_schema_version;
    destination.sbomSha256             = make_asn<Hash256_t>();
    set_octets(*destination.sbomSha256, source.sbom_digest.data(), source.sbom_digest.size());
  }
  destination.compatible = source.compatible ? 1 : 0;
  if (!source.incompatibility.empty()) {
    destination.incompatibility = make_asn<OCTET_STRING_t>();
    set_octets(*destination.incompatibility, source.incompatibility);
  }
}

package_record read_package_record(const PackageRecord_t& source)
{
  package_record result;
  result.package_id  = read_identifier(source.packageId);
  result.global_name = read_string(source.globalName, 128);
  result.vendor      = read_string(source.vendor, 128);
  result.version     = read_string(source.version, 64);
  auto digest        = read_bytes(source.contentHash, result.content_digest.size());
  if (digest.size() != result.content_digest.size()) {
    throw std::runtime_error("invalid package digest");
  }
  std::copy(digest.begin(), digest.end(), result.content_digest.begin());
  result.placement_mask = static_cast<uint32_t>(read_bits(source.placementMask, 1));
  if (source.backends.list.count < 1 || source.backends.list.count > 16) {
    throw std::runtime_error("invalid package backend count");
  }
  result.backends.reserve(source.backends.list.count);
  for (int index = 0; index != source.backends.list.count; ++index) {
    if (source.backends.list.array[index] == nullptr) {
      throw std::runtime_error("null package backend");
    }
    result.backends.push_back(from_wire_backend(*source.backends.list.array[index]));
  }
  if (source.interfaces != nullptr) {
    if (source.interfaces->list.count > 64) {
      throw std::runtime_error("invalid package interface count");
    }
    result.interfaces.reserve(source.interfaces->list.count);
    for (int index = 0; index != source.interfaces->list.count; ++index) {
      if (source.interfaces->list.array[index] == nullptr) {
        throw std::runtime_error("null package interface");
      }
      result.interfaces.push_back(read_interface_record(*source.interfaces->list.array[index]));
    }
  }
  if (source.signatureStatus != nullptr && *source.signatureStatus != PackageSignatureStatus_unsignedPackage &&
      *source.signatureStatus != PackageSignatureStatus_verified) {
    throw std::runtime_error("invalid package signature status");
  }
  result.signature_status =
      source.signatureStatus != nullptr && *source.signatureStatus == PackageSignatureStatus_verified
          ? package_signature_status::verified
          : package_signature_status::unsigned_package;
  const bool has_signature_metadata =
      source.signingKeyId != nullptr || source.signatureSequence != nullptr || source.trustPolicyEpoch != nullptr;
  if (result.signature_status == package_signature_status::verified) {
    if (source.signingKeyId == nullptr || source.signatureSequence == nullptr || source.trustPolicyEpoch == nullptr) {
      throw std::runtime_error("verified package is missing signature metadata");
    }
    const auto key_id = read_bytes(*source.signingKeyId, result.signing_key_id.size());
    if (key_id.size() != result.signing_key_id.size()) {
      throw std::runtime_error("invalid signing key identifier");
    }
    std::copy(key_id.begin(), key_id.end(), result.signing_key_id.begin());
    result.signature_sequence = read_u64(*source.signatureSequence);
    result.trust_policy_epoch = read_u64(*source.trustPolicyEpoch);
    if (std::all_of(
            result.signing_key_id.begin(), result.signing_key_id.end(), [](uint8_t value) { return value == 0; }) ||
        result.signature_sequence == 0 || result.trust_policy_epoch == 0) {
      throw std::runtime_error("invalid verified package signature metadata");
    }
  } else if (has_signature_metadata) {
    throw std::runtime_error("unsigned package carries signature metadata");
  }
  const bool has_provenance_metadata = source.manifestSchemaVersion != nullptr || source.sbomSha256 != nullptr;
  if (has_provenance_metadata) {
    if (source.manifestSchemaVersion == nullptr || source.sbomSha256 == nullptr || *source.manifestSchemaVersion != 1) {
      throw std::runtime_error("invalid package provenance metadata");
    }
    const auto sbom_digest = read_bytes(*source.sbomSha256, result.sbom_digest.size());
    if (sbom_digest.size() != result.sbom_digest.size() ||
        std::all_of(sbom_digest.begin(), sbom_digest.end(), [](uint8_t value) { return value == 0; })) {
      throw std::runtime_error("invalid package SBOM digest");
    }
    std::copy(sbom_digest.begin(), sbom_digest.end(), result.sbom_digest.begin());
    result.manifest_schema_version = 1;
    result.sbom_verified           = true;
  }
  if (source.compatible != 0 && source.compatible != 1) {
    throw std::runtime_error("invalid package compatibility flag");
  }
  result.compatible = source.compatible != 0;
  if (source.incompatibility != nullptr) {
    result.incompatibility = read_string(*source.incompatibility, maximum_text_bytes);
  }
  return result;
}

void set_instance_record(InstanceRecord_t& destination, const instance_record& source)
{
  set_identifier(destination.instanceId, source.instance_id);
  set_identifier(destination.packageId, source.package_id);
  set_octets(destination.globalName, source.global_name);
  set_octets(destination.vendor, source.vendor);
  set_octets(destination.version, source.version);
  destination.placement = to_wire(source.placement);
  destination.backend   = static_cast<long>(source.selected_backend);
  destination.state     = to_wire(source.state);
  set_u64(destination.generation, source.generation);
  set_bits(destination.managementCapabilities, source.management_capabilities, 8);
  for (const auto& item : source.interfaces) {
    set_interface_record(append_asn<InterfaceRecord_t>(destination.interfaces), item);
  }
  if (source.last_incident_id != 0) {
    using counter_type         = std::remove_pointer_t<decltype(destination.lastIncidentId)>;
    destination.lastIncidentId = make_asn<counter_type>();
    set_u64(*destination.lastIncidentId, source.last_incident_id);
  }
  if (!source.last_error.empty()) {
    destination.lastError = make_asn<OCTET_STRING_t>();
    const std::string_view error(source.last_error.data(), std::min(source.last_error.size(), maximum_text_bytes));
    set_text(*destination.lastError, error);
  }
}

instance_record read_instance_record(const InstanceRecord_t& source)
{
  instance_record result;
  result.instance_id             = read_identifier(source.instanceId);
  result.package_id              = read_identifier(source.packageId);
  result.global_name             = read_string(source.globalName, 128);
  result.vendor                  = read_string(source.vendor, 128);
  result.version                 = read_string(source.version, 64);
  result.placement               = from_wire_placement(source.placement);
  result.selected_backend        = from_wire_backend(source.backend);
  result.state                   = from_wire_state(source.state);
  result.generation              = read_u64(source.generation);
  result.management_capabilities = read_bits(source.managementCapabilities, 8);
  if (source.interfaces.list.count < 1 || source.interfaces.list.count > 64) {
    throw std::runtime_error("invalid instance interface count");
  }
  result.interfaces.reserve(source.interfaces.list.count);
  for (int index = 0; index != source.interfaces.list.count; ++index) {
    if (source.interfaces.list.array[index] == nullptr) {
      throw std::runtime_error("null instance interface");
    }
    result.interfaces.push_back(read_interface_record(*source.interfaces.list.array[index]));
  }
  if (source.lastIncidentId != nullptr) {
    result.last_incident_id = read_u64(*source.lastIncidentId);
  }
  if (source.lastError != nullptr) {
    result.last_error = read_string(*source.lastError, maximum_text_bytes);
  }
  return result;
}

void set_e3_subscription_record(E3SubscriptionRecord_t& destination, const e3_subscription_record& source)
{
  destination.subscriptionId = source.subscription_id;
  destination.ranFunctionId  = source.ran_function_id;
  for (uint16_t telemetry_id : source.telemetry_identifiers) {
    append_asn<long>(destination.telemetryIdentifiers) = telemetry_id;
  }
}

e3_subscription_record read_e3_subscription_record(const E3SubscriptionRecord_t& source)
{
  e3_subscription_record result;
  result.subscription_id = static_cast<uint16_t>(source.subscriptionId);
  result.ran_function_id = static_cast<uint16_t>(source.ranFunctionId);
  if (source.telemetryIdentifiers.list.count > 64) {
    throw std::runtime_error("E3 subscription telemetry list exceeds bound");
  }
  result.telemetry_identifiers.reserve(source.telemetryIdentifiers.list.count);
  for (int index = 0; index != source.telemetryIdentifiers.list.count; ++index) {
    if (source.telemetryIdentifiers.list.array[index] == nullptr) {
      throw std::runtime_error("null E3 subscription telemetry identifier");
    }
    result.telemetry_identifiers.push_back(static_cast<uint16_t>(*source.telemetryIdentifiers.list.array[index]));
  }
  return result;
}

void set_e3_connection_record(E3ConnectionRecord_t& destination, const e3_connection_record& source)
{
  set_u64(destination.associationId, source.association_id);
  set_u64(destination.openedNs, source.opened_ns);
  set_octets(destination.peerAddress, source.peer_address);
  destination.setupComplete = source.setup_complete ? 1 : 0;
  if (source.dapp_id) {
    destination.dappId  = make_asn<long>();
    *destination.dappId = *source.dapp_id;
  }
  if (!source.dapp_name.empty()) {
    destination.dappName = make_asn<OCTET_STRING_t>();
    set_octets(*destination.dappName, source.dapp_name);
  }
  for (const auto& subscription : source.subscriptions) {
    set_e3_subscription_record(append_asn<E3SubscriptionRecord_t>(destination.subscriptions), subscription);
  }
  set_u64(destination.controlRequests, source.control_requests);
}

e3_connection_record read_e3_connection_record(const E3ConnectionRecord_t& source)
{
  e3_connection_record result;
  result.association_id = read_u64(source.associationId);
  result.opened_ns      = read_u64(source.openedNs);
  result.peer_address   = read_string(source.peerAddress, 128);
  result.setup_complete = source.setupComplete != 0;
  if (source.dappId != nullptr) {
    result.dapp_id = static_cast<uint16_t>(*source.dappId);
  }
  if (source.dappName != nullptr) {
    result.dapp_name = read_string(*source.dappName, 128);
  }
  if (source.subscriptions.list.count > 64) {
    throw std::runtime_error("E3 connection subscription list exceeds bound");
  }
  result.subscriptions.reserve(source.subscriptions.list.count);
  for (int index = 0; index != source.subscriptions.list.count; ++index) {
    if (source.subscriptions.list.array[index] == nullptr) {
      throw std::runtime_error("null E3 connection subscription");
    }
    result.subscriptions.push_back(read_e3_subscription_record(*source.subscriptions.list.array[index]));
  }
  result.control_requests = read_u64(source.controlRequests);
  return result;
}

void set_cell_descriptor(CellDescriptor_t& destination, const cell_descriptor& source)
{
  destination.cellId = source.cell_id;
  destination.pci    = read_ranged(source.pci, 0, 1007, "PCI");
  if (source.plmn.size() < 5 || source.plmn.size() > 6) {
    throw std::runtime_error("PLMN must be 5 or 6 digits");
  }
  set_octets(destination.plmn, source.plmn);
  destination.tac = read_ranged(source.tac, 0, 16777215, "TAC");
  set_u64(destination.nrCellIdentity, source.nr_cell_identity);
  destination.band    = read_ranged(source.band, 0, 1023, "band");
  destination.dlArfcn = read_ranged(source.dl_arfcn, 0, 3279165, "DL ARFCN");
  destination.ulArfcn = read_ranged(source.ul_arfcn, 0, 3279165, "UL ARFCN");
  set_u64(destination.dlCenterHz, source.dl_center_hz);
  set_u64(destination.ulCenterHz, source.ul_center_hz);
  destination.bandwidthMhz  = read_ranged(source.bandwidth_mhz, 0, 400, "bandwidth");
  destination.nofPrbs       = read_ranged(source.nof_prbs, 0, 275, "PRB count");
  destination.scsKhz        = read_ranged(source.scs_khz, 15, 480, "subcarrier spacing");
  destination.nofTxAntennas = read_ranged(source.nof_tx_antennas, 0, 64, "TX antennas");
  destination.nofRxAntennas = read_ranged(source.nof_rx_antennas, 0, 64, "RX antennas");
  destination.barred        = source.barred ? 1 : 0;
  if (source.tdd) {
    destination.tdd              = make_asn<CellTddPattern_t>();
    destination.tdd->periodSlots = read_ranged(source.tdd->period_slots, 1, 320, "TDD period");
    destination.tdd->dlSlots     = read_ranged(source.tdd->dl_slots, 0, 320, "TDD DL slots");
    destination.tdd->dlSymbols   = read_ranged(source.tdd->dl_symbols, 0, 13, "TDD DL symbols");
    destination.tdd->ulSlots     = read_ranged(source.tdd->ul_slots, 0, 320, "TDD UL slots");
    destination.tdd->ulSymbols   = read_ranged(source.tdd->ul_symbols, 0, 13, "TDD UL symbols");
  }
  if (source.prach) {
    destination.prach                      = make_asn<CellPrachConfig_t>();
    destination.prach->configIndex         = source.prach->config_index;
    destination.prach->rootSequenceIndex   = read_ranged(source.prach->root_sequence_index, 0, 837, "PRACH root");
    destination.prach->zeroCorrelationZone = read_ranged(source.prach->zero_correlation_zone, 0, 15, "PRACH ZCZ");
    destination.prach->totalPreambles      = read_ranged(source.prach->total_preambles, 0, 64, "PRACH preambles");
    destination.prach->prbStart            = read_ranged(source.prach->prb_start, 0, 274, "PRACH PRB start");
  }
  set_optional_real(destination.txGainDb, source.tx_gain_db);
  set_optional_real(destination.rxGainDb, source.rx_gain_db);
  if (source.sample_rate_hz) {
    destination.sampleRateHz = make_asn<Unsigned64_t>();
    set_u64(*destination.sampleRateHz, *source.sample_rate_hz);
  }
}

cell_descriptor read_cell_descriptor(const CellDescriptor_t& source)
{
  cell_descriptor result;
  result.cell_id          = static_cast<uint16_t>(read_ranged(source.cellId, 0, 65535, "cell id"));
  result.pci              = static_cast<uint16_t>(read_ranged(source.pci, 0, 1007, "PCI"));
  result.plmn             = read_string(source.plmn, 6);
  result.tac              = static_cast<uint32_t>(read_ranged(source.tac, 0, 16777215, "TAC"));
  result.nr_cell_identity = read_u64(source.nrCellIdentity);
  result.band             = static_cast<uint16_t>(read_ranged(source.band, 0, 1023, "band"));
  result.dl_arfcn         = static_cast<uint32_t>(read_ranged(source.dlArfcn, 0, 3279165, "DL ARFCN"));
  result.ul_arfcn         = static_cast<uint32_t>(read_ranged(source.ulArfcn, 0, 3279165, "UL ARFCN"));
  result.dl_center_hz     = read_u64(source.dlCenterHz);
  result.ul_center_hz     = read_u64(source.ulCenterHz);
  result.bandwidth_mhz    = static_cast<uint16_t>(read_ranged(source.bandwidthMhz, 0, 400, "bandwidth"));
  result.nof_prbs         = static_cast<uint16_t>(read_ranged(source.nofPrbs, 0, 275, "PRB count"));
  result.scs_khz          = static_cast<uint16_t>(read_ranged(source.scsKhz, 15, 480, "subcarrier spacing"));
  result.nof_tx_antennas  = static_cast<uint8_t>(read_ranged(source.nofTxAntennas, 0, 64, "TX antennas"));
  result.nof_rx_antennas  = static_cast<uint8_t>(read_ranged(source.nofRxAntennas, 0, 64, "RX antennas"));
  result.barred           = source.barred != 0;
  if (source.tdd != nullptr) {
    result.tdd = cell_tdd_pattern{static_cast<uint16_t>(source.tdd->periodSlots),
                                  static_cast<uint8_t>(source.tdd->dlSlots),
                                  static_cast<uint8_t>(source.tdd->dlSymbols),
                                  static_cast<uint8_t>(source.tdd->ulSlots),
                                  static_cast<uint8_t>(source.tdd->ulSymbols)};
  }
  if (source.prach != nullptr) {
    result.prach = cell_prach_config{static_cast<uint8_t>(source.prach->configIndex),
                                     static_cast<uint16_t>(source.prach->rootSequenceIndex),
                                     static_cast<uint16_t>(source.prach->zeroCorrelationZone),
                                     static_cast<uint8_t>(source.prach->totalPreambles),
                                     static_cast<uint16_t>(source.prach->prbStart)};
  }
  result.tx_gain_db = read_optional_real(source.txGainDb);
  result.rx_gain_db = read_optional_real(source.rxGainDb);
  if (source.sampleRateHz != nullptr) {
    result.sample_rate_hz = read_u64(*source.sampleRateHz);
  }
  return result;
}

void set_cell_info_result(CellInfoResult_t& destination, const cell_info_result& source)
{
  for (const auto& cell : source.cells) {
    set_cell_descriptor(append_asn<CellDescriptor_t>(destination.cells), cell);
  }
  destination.capabilities            = make_asn<RanControlCapabilities_t>();
  destination.capabilities->ueRelease = source.capabilities.ue_release ? 1 : 0;
  destination.capabilities->handover  = source.capabilities.handover ? 1 : 0;
  destination.capabilities->txGain    = source.capabilities.tx_gain ? 1 : 0;
  destination.capabilities->rxGain    = source.capabilities.rx_gain ? 1 : 0;
}

cell_info_result read_cell_info_result(const CellInfoResult_t& source)
{
  cell_info_result result;
  if (source.cells.list.count > 32) {
    throw std::runtime_error("cell list exceeds bound");
  }
  for (int index = 0; index != source.cells.list.count; ++index) {
    if (source.cells.list.array[index] == nullptr) {
      throw std::runtime_error("null cell descriptor");
    }
    result.cells.push_back(read_cell_descriptor(*source.cells.list.array[index]));
  }
  if (source.capabilities != nullptr) {
    result.capabilities.ue_release = source.capabilities->ueRelease != 0;
    result.capabilities.handover   = source.capabilities->handover != 0;
    result.capabilities.tx_gain    = source.capabilities->txGain != 0;
    result.capabilities.rx_gain    = source.capabilities->rxGain != 0;
  }
  return result;
}

void set_inventory_result(InventoryResult_t& destination, const inventory_result& source)
{
  set_u64(destination.generation, source.inventory.generation);
  using package_list    = std::remove_pointer_t<decltype(destination.packages)>;
  using instance_list   = std::remove_pointer_t<decltype(destination.instances)>;
  destination.packages  = make_asn<package_list>();
  destination.instances = make_asn<instance_list>();
  for (const auto& item : source.inventory.packages) {
    set_package_record(append_asn<PackageRecord_t>(*destination.packages), item);
  }
  for (const auto& item : source.inventory.instances) {
    set_instance_record(append_asn<InstanceRecord_t>(*destination.instances), item);
  }
  if (!source.connections.empty()) {
    using connection_list   = std::remove_pointer_t<decltype(destination.connections)>;
    destination.connections = make_asn<connection_list>();
    for (const auto& connection : source.connections) {
      set_e3_connection_record(append_asn<E3ConnectionRecord_t>(*destination.connections), connection);
    }
  }
}

inventory_result read_inventory_result(const InventoryResult_t& source)
{
  inventory_result result;
  result.inventory.generation = read_u64(source.generation);
  if (source.packages != nullptr) {
    if (static_cast<std::size_t>(source.packages->list.count) > maximum_items) {
      throw std::runtime_error("package inventory exceeds bound");
    }
    result.inventory.packages.reserve(source.packages->list.count);
    for (int index = 0; index != source.packages->list.count; ++index) {
      if (source.packages->list.array[index] == nullptr) {
        throw std::runtime_error("null package record");
      }
      result.inventory.packages.push_back(read_package_record(*source.packages->list.array[index]));
    }
  }
  if (source.instances != nullptr) {
    if (static_cast<std::size_t>(source.instances->list.count) > maximum_items) {
      throw std::runtime_error("instance inventory exceeds bound");
    }
    result.inventory.instances.reserve(source.instances->list.count);
    for (int index = 0; index != source.instances->list.count; ++index) {
      if (source.instances->list.array[index] == nullptr) {
        throw std::runtime_error("null instance record");
      }
      result.inventory.instances.push_back(read_instance_record(*source.instances->list.array[index]));
    }
  }
  if (source.connections != nullptr) {
    if (source.connections->list.count > 64) {
      throw std::runtime_error("E3 connection inventory exceeds bound");
    }
    result.connections.reserve(source.connections->list.count);
    for (int index = 0; index != source.connections->list.count; ++index) {
      if (source.connections->list.array[index] == nullptr) {
        throw std::runtime_error("null E3 connection record");
      }
      result.connections.push_back(read_e3_connection_record(*source.connections->list.array[index]));
    }
  }
  return result;
}

void set_config_descriptor(ConfigDescriptor_t& destination, const config_descriptor& source)
{
  set_octets(destination.key, source.key);
  set_octets(destination.description, source.description);
  destination.kind = to_wire(source.value_kind);
  set_bits(destination.flags, source.flags, 4);
  auto set_optional = [](TypedValue_t*& target, const std::optional<management::value>& item) {
    if (item) {
      target = make_asn<TypedValue_t>();
      set_value(*target, *item);
    }
  };
  set_optional(destination.defaultValue, source.default_value);
  set_optional(destination.minimumValue, source.minimum_value);
  set_optional(destination.maximumValue, source.maximum_value);
}

config_descriptor read_config_descriptor(const ConfigDescriptor_t& source)
{
  config_descriptor result;
  result.key         = read_string(source.key, 128);
  result.description = read_string(source.description, maximum_text_bytes);
  result.value_kind  = from_wire_value_kind(source.kind);
  result.flags       = static_cast<uint32_t>(read_bits(source.flags, 4));
  if (source.defaultValue != nullptr) {
    result.default_value = read_value(*source.defaultValue);
  }
  if (source.minimumValue != nullptr) {
    result.minimum_value = read_value(*source.minimumValue);
  }
  if (source.maximumValue != nullptr) {
    result.maximum_value = read_value(*source.maximumValue);
  }
  return result;
}

void set_config_result(ConfigResult_t& destination, const config_result& source)
{
  if (source.generation) {
    destination.generation = make_asn<Generation_t>();
    set_u64(*destination.generation, *source.generation);
  }
  if (!source.descriptors.empty()) {
    using descriptor_list   = std::remove_pointer_t<decltype(destination.descriptors)>;
    destination.descriptors = make_asn<descriptor_list>();
    for (const auto& item : source.descriptors) {
      set_config_descriptor(append_asn<ConfigDescriptor_t>(*destination.descriptors), item);
    }
  }
  if (!source.entries.empty()) {
    using entry_list    = std::remove_pointer_t<decltype(destination.entries)>;
    destination.entries = make_asn<entry_list>();
    for (const auto& item : source.entries) {
      set_config_entry(append_asn<ConfigEntry_t>(*destination.entries), item);
    }
  }
}

config_result read_config_result(const ConfigResult_t& source)
{
  config_result result;
  if (source.generation != nullptr) {
    result.generation = read_u64(*source.generation);
  }
  if (source.descriptors != nullptr) {
    if (static_cast<std::size_t>(source.descriptors->list.count) > maximum_items) {
      throw std::runtime_error("config descriptor result exceeds bound");
    }
    result.descriptors.reserve(source.descriptors->list.count);
    for (int index = 0; index != source.descriptors->list.count; ++index) {
      if (source.descriptors->list.array[index] == nullptr) {
        throw std::runtime_error("null config descriptor");
      }
      result.descriptors.push_back(read_config_descriptor(*source.descriptors->list.array[index]));
    }
  }
  if (source.entries != nullptr) {
    if (static_cast<std::size_t>(source.entries->list.count) > maximum_items) {
      throw std::runtime_error("config result exceeds bound");
    }
    result.entries.reserve(source.entries->list.count);
    for (int index = 0; index != source.entries->list.count; ++index) {
      if (source.entries->list.array[index] == nullptr) {
        throw std::runtime_error("null config result entry");
      }
      result.entries.push_back(read_config_entry(*source.entries->list.array[index]));
    }
  }
  return result;
}

void set_metric_descriptor(MetricDescriptor_t& destination, const metric_descriptor& source)
{
  set_octets(destination.name, source.name);
  set_octets(destination.description, source.description);
  set_octets(destination.unit, source.unit);
  destination.kind = to_wire(source.value_kind);
  set_bits(destination.flags, source.flags, 4);
}

metric_descriptor read_metric_descriptor(const MetricDescriptor_t& source)
{
  return {read_string(source.name, 128),
          read_string(source.description, maximum_text_bytes),
          read_string(source.unit, 64),
          from_wire_value_kind(source.kind),
          static_cast<uint32_t>(read_bits(source.flags, 4))};
}

void set_metric_sample(MetricSample_t& destination, const metric_sample& source)
{
  set_octets(destination.name, source.name);
  destination.value = make_asn<TypedValue_t>();
  set_value(*destination.value, source.sample_value);
  set_u64(destination.timestampNs, source.monotonic_timestamp_ns);
}

metric_sample read_metric_sample(const MetricSample_t& source)
{
  if (source.value == nullptr) {
    throw std::runtime_error("metric sample has no value");
  }
  return {read_string(source.name, 128), read_value(*source.value), read_u64(source.timestampNs)};
}

void set_metric_result(MetricResult_t& destination, const metric_result& source)
{
  if (!source.descriptors.empty()) {
    using descriptor_list   = std::remove_pointer_t<decltype(destination.descriptors)>;
    destination.descriptors = make_asn<descriptor_list>();
    for (const auto& item : source.descriptors) {
      set_metric_descriptor(append_asn<MetricDescriptor_t>(*destination.descriptors), item);
    }
  }
  if (!source.samples.empty()) {
    using sample_list   = std::remove_pointer_t<decltype(destination.samples)>;
    destination.samples = make_asn<sample_list>();
    for (const auto& item : source.samples) {
      set_metric_sample(append_asn<MetricSample_t>(*destination.samples), item);
    }
  }
}

metric_result read_metric_result(const MetricResult_t& source)
{
  metric_result result;
  if (source.descriptors != nullptr) {
    if (static_cast<std::size_t>(source.descriptors->list.count) > maximum_items) {
      throw std::runtime_error("metric descriptor result exceeds bound");
    }
    result.descriptors.reserve(source.descriptors->list.count);
    for (int index = 0; index != source.descriptors->list.count; ++index) {
      if (source.descriptors->list.array[index] == nullptr) {
        throw std::runtime_error("null metric descriptor");
      }
      result.descriptors.push_back(read_metric_descriptor(*source.descriptors->list.array[index]));
    }
  }
  if (source.samples != nullptr) {
    if (static_cast<std::size_t>(source.samples->list.count) > maximum_items) {
      throw std::runtime_error("metric result exceeds bound");
    }
    result.samples.reserve(source.samples->list.count);
    for (int index = 0; index != source.samples->list.count; ++index) {
      if (source.samples->list.array[index] == nullptr) {
        throw std::runtime_error("null metric sample");
      }
      result.samples.push_back(read_metric_sample(*source.samples->list.array[index]));
    }
  }
  return result;
}

identifier identifier_from_words(const uint64_t words[2])
{
  identifier result;
  std::memcpy(result.bytes.data(), words, result.bytes.size());
  return result;
}

void words_from_identifier(uint64_t words[2], const Identifier_t& source)
{
  const identifier value = read_identifier(source);
  std::memcpy(words, value.bytes.data(), value.bytes.size());
}

void set_incident_record(IncidentRecord_t& destination, const incident_record& source)
{
  set_u64(destination.incidentId, source.incident_id);
  set_u64(destination.timestampNs, source.monotonic_ns);
  set_identifier(destination.instanceId, identifier_from_words(source.context.instance_id));
  set_identifier(destination.packageId, identifier_from_words(source.context.package_id));
  set_u64(destination.appGeneration, source.context.app_generation);
  if (source.context.model_generation != 0) {
    destination.modelGeneration = make_asn<Generation_t>();
    set_u64(*destination.modelGeneration, source.context.model_generation);
  }
  using hook_type             = std::remove_pointer_t<decltype(destination.hookId)>;
  destination.hookId          = make_asn<hook_type>();
  *destination.hookId         = source.context.hook_id;
  destination.cellId          = make_asn<long>();
  *destination.cellId         = source.context.cell_id;
  destination.workerId        = make_asn<long>();
  *destination.workerId       = source.context.worker_id;
  destination.phase           = static_cast<long>(source.context.phase);
  destination.cause           = static_cast<long>(source.cause);
  destination.action          = static_cast<long>(source.action);
  destination.nativeStatus    = source.native_status;
  destination.recurrenceCount = source.recurrence_count;
  if (source.message_size != 0) {
    destination.sanitizedMessage = make_asn<OCTET_STRING_t>();
    set_text(*destination.sanitizedMessage, std::string_view(source.sanitized_message.data(), source.message_size));
  }
  if (source.context.invocation_sequence != 0) {
    using counter_type             = std::remove_pointer_t<decltype(destination.invocationSequence)>;
    destination.invocationSequence = make_asn<counter_type>();
    set_u64(*destination.invocationSequence, source.context.invocation_sequence);
  }
  if (source.exception_type_hash != 0) {
    using counter_type            = std::remove_pointer_t<decltype(destination.exceptionTypeHash)>;
    destination.exceptionTypeHash = make_asn<counter_type>();
    set_u64(*destination.exceptionTypeHash, source.exception_type_hash);
  }
  if (source.observed_latency_ns != 0) {
    using counter_type            = std::remove_pointer_t<decltype(destination.observedLatencyNs)>;
    destination.observedLatencyNs = make_asn<counter_type>();
    set_u64(*destination.observedLatencyNs, source.observed_latency_ns);
  }
  if (source.deadline_ns != 0) {
    using counter_type     = std::remove_pointer_t<decltype(destination.deadlineNs)>;
    destination.deadlineNs = make_asn<counter_type>();
    set_u64(*destination.deadlineNs, source.deadline_ns);
  }
  destination.fallbackSucceeded = source.fallback_succeeded ? 1 : 0;
  destination.breakerOpen       = source.breaker_open ? 1 : 0;
}

incident_record read_incident_record(const IncidentRecord_t& source)
{
  if (source.phase < IncidentRecord__phase_query || source.phase > IncidentRecord__phase_unload ||
      source.cause < IncidentCause_returnedError || source.cause > IncidentCause_policyRejection ||
      source.action < IncidentAction_observed || source.action > IncidentAction_duRestartRequired) {
    throw std::runtime_error("invalid incident enumeration");
  }
  incident_record result;
  result.incident_id  = read_u64(source.incidentId);
  result.monotonic_ns = read_u64(source.timestampNs);
  words_from_identifier(result.context.instance_id, source.instanceId);
  words_from_identifier(result.context.package_id, source.packageId);
  result.context.app_generation = read_u64(source.appGeneration);
  if (source.modelGeneration != nullptr) {
    result.context.model_generation = read_u64(*source.modelGeneration);
  }
  if (source.hookId != nullptr) {
    result.context.hook_id = static_cast<uint32_t>(*source.hookId);
  }
  if (source.cellId != nullptr) {
    result.context.cell_id = static_cast<uint16_t>(*source.cellId);
  }
  if (source.workerId != nullptr) {
    result.context.worker_id = static_cast<uint16_t>(*source.workerId);
  }
  result.context.phase    = static_cast<incident_phase>(source.phase);
  result.cause            = static_cast<incident_cause>(source.cause);
  result.action           = static_cast<incident_action>(source.action);
  result.native_status    = static_cast<int32_t>(source.nativeStatus);
  result.recurrence_count = static_cast<uint32_t>(source.recurrenceCount);
  if (source.sanitizedMessage != nullptr) {
    const auto message  = read_string(*source.sanitizedMessage, maximum_text_bytes);
    result.message_size = static_cast<uint16_t>(std::min(message.size(), result.sanitized_message.size()));
    std::copy_n(message.data(), result.message_size, result.sanitized_message.data());
  }
  if (source.invocationSequence != nullptr) {
    result.context.invocation_sequence = read_u64(*source.invocationSequence);
  }
  if (source.exceptionTypeHash != nullptr) {
    result.exception_type_hash = read_u64(*source.exceptionTypeHash);
  }
  if (source.observedLatencyNs != nullptr) {
    result.observed_latency_ns = read_u64(*source.observedLatencyNs);
  }
  if (source.deadlineNs != nullptr) {
    result.deadline_ns = read_u64(*source.deadlineNs);
  }
  result.fallback_succeeded = source.fallbackSucceeded != 0;
  result.breaker_open       = source.breakerOpen != 0;
  return result;
}

void set_incident_result(IncidentResult_t& destination, const incident_result& source)
{
  for (const auto& item : source.incidents) {
    set_incident_record(append_asn<IncidentRecord_t>(destination.incidents), item);
  }
  if (source.cleared_count) {
    using counter_type       = std::remove_pointer_t<decltype(destination.clearedCount)>;
    destination.clearedCount = make_asn<counter_type>();
    set_u64(*destination.clearedCount, *source.cleared_count);
  }
}

incident_result read_incident_result(const IncidentResult_t& source)
{
  if (static_cast<std::size_t>(source.incidents.list.count) > maximum_items) {
    throw std::runtime_error("incident result exceeds bound");
  }
  incident_result result;
  result.incidents.reserve(source.incidents.list.count);
  for (int index = 0; index != source.incidents.list.count; ++index) {
    if (source.incidents.list.array[index] == nullptr) {
      throw std::runtime_error("null incident record");
    }
    result.incidents.push_back(read_incident_record(*source.incidents.list.array[index]));
  }
  if (source.clearedCount != nullptr) {
    result.cleared_count = read_u64(*source.clearedCount);
  }
  return result;
}

void set_health_summary(HealthSummary_t& destination, const incident_health_summary& source)
{
  set_u64(destination.total, source.total);
  set_u64(destination.standardExceptions, source.standard_exceptions);
  set_u64(destination.unknownExceptions, source.unknown_exceptions);
  set_u64(destination.deadlineMisses, source.deadline_misses);
  set_u64(destination.invalidInputs, source.invalid_inputs);
  set_u64(destination.invalidOutputs, source.invalid_outputs);
  set_u64(destination.deviceErrors, source.device_errors);
  set_u64(destination.resourceExhaustions, source.resource_exhaustions);
  set_u64(destination.quiescenceTimeouts, source.quiescence_timeouts);
  set_u64(destination.breakerOpened, source.breaker_opened);
  set_u64(destination.fallbacks, source.fallbacks);
  set_u64(destination.quarantined, source.quarantined);
  set_u64(destination.historyEvictions, source.history_evictions);
  set_u64(destination.sourceRingDrops, source.source_ring_drops);
  set_u64(destination.lastIncidentId, source.last_incident_id);
}

incident_health_summary read_health_summary(const HealthSummary_t& source)
{
  return {read_u64(source.total),
          read_u64(source.standardExceptions),
          read_u64(source.unknownExceptions),
          read_u64(source.deadlineMisses),
          read_u64(source.invalidInputs),
          read_u64(source.invalidOutputs),
          read_u64(source.deviceErrors),
          read_u64(source.resourceExhaustions),
          read_u64(source.quiescenceTimeouts),
          read_u64(source.breakerOpened),
          read_u64(source.fallbacks),
          read_u64(source.quarantined),
          read_u64(source.historyEvictions),
          read_u64(source.sourceRingDrops),
          read_u64(source.lastIncidentId)};
}

void set_quiet_reservation_record(QuietReservationRecord_t& destination, const quiet_reservation_record& source)
{
  const auto& reservation = source.reservation;
  if (reservation.start.numerology > 4 || reservation.start.sfn >= 1024 || reservation.start.slot >= 160 ||
      reservation.duration_slots == 0 || reservation.repeat_count == 0 ||
      reservation.repeat_count > OCUDU_DAPP_QUIET_RESERVATION_MAX_REPEAT_V1 || reservation.nof_prbs == 0 ||
      reservation.achieved_level > OCUDU_DAPP_QUIET_RF_SILENCE_REQUEST_V1 ||
      reservation.status > OCUDU_DAPP_RESERVATION_EXPIRED_V1 ||
      reservation.reason > OCUDU_DAPP_QUIET_REASON_EXPIRED_V1 || reservation.confidence_q16 > (1U << 16U)) {
    throw std::runtime_error("invalid quiet-reservation record");
  }
  if (reservation.repeat_count > 1 &&
      (reservation.repeat_period_slots == 0 || reservation.repeat_period_slots < reservation.duration_slots)) {
    throw std::runtime_error("invalid repeated quiet-reservation record");
  }
  set_u64(destination.cellId, source.cell_id);
  set_identifier(destination.requestId, identifier_from_words(reservation.request_id));
  set_identifier(destination.reservationId, identifier_from_words(reservation.reservation_id));
  set_u64(destination.reservationGeneration, reservation.reservation_generation);
  set_identifier(destination.sourceInstanceId, identifier_from_words(reservation.request_source_instance_id));
  set_u64(destination.sourceGeneration, reservation.request_source_generation);
  set_u64(destination.sourceSequence, reservation.request_source_sequence);
  destination.start = make_asn<RadioTime_t>();
  set_u64(destination.start->sfn, reservation.start.sfn);
  set_u64(destination.start->slot, reservation.start.slot);
  set_u64(destination.start->numerology, reservation.start.numerology);
  set_u64(destination.durationSlots, reservation.duration_slots);
  set_u64(destination.repeatPeriodSlots, reservation.repeat_period_slots);
  set_u64(destination.repeatCount, reservation.repeat_count);
  set_u64(destination.firstPrb, reservation.first_prb);
  set_u64(destination.nofPrbs, reservation.nof_prbs);
  destination.achievedLevel = reservation.achieved_level;
  destination.status        = reservation.status;
  destination.reason        = reservation.reason;
  set_u64(destination.budgetClass, reservation.budget_class);
  set_u64(destination.confidenceQ16, reservation.confidence_q16);
  set_u64(destination.expiryMonotonicNs, reservation.expiry_monotonic_ns);
}

quiet_reservation_record read_quiet_reservation_record(const QuietReservationRecord_t& source)
{
  if (source.achievedLevel < 0 || source.achievedLevel > OCUDU_DAPP_QUIET_RF_SILENCE_REQUEST_V1 || source.status < 0 ||
      source.status > OCUDU_DAPP_RESERVATION_EXPIRED_V1 || source.reason < 0 ||
      source.reason > OCUDU_DAPP_QUIET_REASON_EXPIRED_V1) {
    throw std::runtime_error("invalid quiet-reservation enumeration");
  }
  quiet_reservation_record result;
  result.cell_id          = static_cast<uint32_t>(read_u64(source.cellId));
  auto& reservation       = result.reservation;
  reservation.struct_size = sizeof(reservation);
  reservation.abi_major   = OCUDU_DAPP_ABI_MAJOR_V1;
  reservation.abi_minor   = OCUDU_DAPP_ABI_MINOR_V1;
  words_from_identifier(reservation.request_id, source.requestId);
  words_from_identifier(reservation.reservation_id, source.reservationId);
  reservation.reservation_generation = read_u64(source.reservationGeneration);
  words_from_identifier(reservation.request_source_instance_id, source.sourceInstanceId);
  reservation.request_source_generation = read_u64(source.sourceGeneration);
  reservation.request_source_sequence   = read_u64(source.sourceSequence);
  if (source.start == nullptr) {
    throw std::runtime_error("quiet reservation has no radio time");
  }
  reservation.start.sfn           = static_cast<uint16_t>(read_u64(source.start->sfn));
  reservation.start.slot          = static_cast<uint16_t>(read_u64(source.start->slot));
  reservation.start.numerology    = static_cast<uint16_t>(read_u64(source.start->numerology));
  reservation.duration_slots      = static_cast<uint16_t>(read_u64(source.durationSlots));
  reservation.repeat_period_slots = static_cast<uint16_t>(read_u64(source.repeatPeriodSlots));
  reservation.repeat_count        = static_cast<uint16_t>(read_u64(source.repeatCount));
  reservation.first_prb           = static_cast<uint16_t>(read_u64(source.firstPrb));
  reservation.nof_prbs            = static_cast<uint16_t>(read_u64(source.nofPrbs));
  reservation.achieved_level      = static_cast<uint8_t>(source.achievedLevel);
  reservation.status              = static_cast<uint8_t>(source.status);
  reservation.reason              = static_cast<uint16_t>(source.reason);
  reservation.budget_class        = static_cast<uint16_t>(read_u64(source.budgetClass));
  reservation.confidence_q16      = static_cast<uint32_t>(read_u64(source.confidenceQ16));
  reservation.expiry_monotonic_ns = read_u64(source.expiryMonotonicNs);
  if (reservation.repeat_count > 1 &&
      (reservation.repeat_period_slots == 0 || reservation.repeat_period_slots < reservation.duration_slots)) {
    throw std::runtime_error("invalid repeated quiet-reservation record");
  }
  return result;
}

void set_quiet_reservation_result(QuietReservationResult_t& destination, const quiet_reservation_result& source)
{
  for (const auto& item : source.reservations) {
    set_quiet_reservation_record(append_asn<QuietReservationRecord_t>(destination.reservations), item);
  }
  destination.truncated = source.truncated ? 1 : 0;
}

quiet_reservation_result read_quiet_reservation_result(const QuietReservationResult_t& source)
{
  if (static_cast<std::size_t>(source.reservations.list.count) > maximum_items ||
      (source.truncated != 0 && source.truncated != 1)) {
    throw std::runtime_error("invalid quiet-reservation result");
  }
  quiet_reservation_result result;
  result.reservations.reserve(source.reservations.list.count);
  for (int index = 0; index != source.reservations.list.count; ++index) {
    if (source.reservations.list.array[index] == nullptr) {
      throw std::runtime_error("null quiet-reservation record");
    }
    result.reservations.push_back(read_quiet_reservation_record(*source.reservations.list.array[index]));
  }
  result.truncated = source.truncated != 0;
  return result;
}

void set_data_subscription_record(DataSubscriptionRecord_t& destination, const portable_subscription_record& source)
{
  const auto& request = source.request;
  if (request.subscription_id.is_zero() || request.interface_id == 0 || request.sample_every_n == 0 ||
      request.sample_every_n > 65535 || request.queue_depth_chunks == 0 || request.queue_depth_chunks > 1024 ||
      request.maximum_chunk_bytes < 1024 || request.maximum_chunk_bytes > maximum_data_bytes || request.lease_us == 0 ||
      request.lease_us > 60'000'000 || request.drop_policy < portable_drop_policy::drop_newest ||
      request.drop_policy > portable_drop_policy::coalesce_latest) {
    throw std::runtime_error("invalid portable E3 subscription record");
  }
  set_identifier(destination.subscriptionId, request.subscription_id);
  set_u64(destination.interfaceId, request.interface_id);
  if (request.cell_id) {
    destination.cellId = make_asn<long>();
    set_u64(*destination.cellId, *request.cell_id);
  }
  set_u64(destination.sampleEveryN, request.sample_every_n);
  set_u64(destination.queueDepthChunks, request.queue_depth_chunks);
  set_u64(destination.maximumChunkBytes, request.maximum_chunk_bytes);
  set_u64(destination.leaseUs, request.lease_us);
  destination.dropPolicy = static_cast<long>(request.drop_policy);
  set_u64(destination.leaseExpiryNs, source.lease_expiry_ns);
  set_u64(destination.queuedChunks, source.queued_chunks);
  set_u64(destination.publishedObjects, source.published_objects);
  set_u64(destination.publishedTensorBytes, source.published_tensor_bytes);
  set_u64(destination.droppedObjects, source.dropped_objects);
  set_u64(destination.busyDrops, source.busy_drops);
  set_u64(destination.sampledObjects, source.sampled_objects);
  destination.active = source.active ? 1 : 0;
  if (source.rf_center_frequency_hz != 0 || source.cell_frequency_offset_hz != 0) {
    destination.rfCenterFrequencyHz = make_asn<Unsigned64_t>();
    set_u64(*destination.rfCenterFrequencyHz, source.rf_center_frequency_hz);
    destination.cellFrequencyOffsetHz = make_asn<Signed64_t>();
    set_i64(*destination.cellFrequencyOffsetHz, source.cell_frequency_offset_hz);
  }
}

portable_subscription_record read_data_subscription_record(const DataSubscriptionRecord_t& source)
{
  if (source.dropPolicy < DataDropPolicy_dropNewest || source.dropPolicy > DataDropPolicy_coalesceLatest ||
      (source.active != 0 && source.active != 1)) {
    throw std::runtime_error("invalid portable E3 subscription record");
  }
  portable_subscription_record result;
  auto&                        request = result.request;
  request.subscription_id              = read_identifier(source.subscriptionId);
  request.interface_id                 = static_cast<uint32_t>(read_u64(source.interfaceId));
  if (source.cellId != nullptr) {
    request.cell_id = static_cast<uint32_t>(read_u64(*source.cellId));
  }
  request.sample_every_n        = static_cast<uint32_t>(read_u64(source.sampleEveryN));
  request.queue_depth_chunks    = static_cast<uint32_t>(read_u64(source.queueDepthChunks));
  request.maximum_chunk_bytes   = static_cast<uint32_t>(read_u64(source.maximumChunkBytes));
  request.lease_us              = read_u64(source.leaseUs);
  request.drop_policy           = static_cast<portable_drop_policy>(source.dropPolicy);
  result.lease_expiry_ns        = read_u64(source.leaseExpiryNs);
  result.queued_chunks          = read_u64(source.queuedChunks);
  result.published_objects      = read_u64(source.publishedObjects);
  result.published_tensor_bytes = read_u64(source.publishedTensorBytes);
  result.dropped_objects        = read_u64(source.droppedObjects);
  result.busy_drops             = read_u64(source.busyDrops);
  result.sampled_objects        = read_u64(source.sampledObjects);
  result.active                 = source.active != 0;
  if (source.rfCenterFrequencyHz != nullptr) {
    result.rf_center_frequency_hz = read_u64(*source.rfCenterFrequencyHz);
  }
  if (source.cellFrequencyOffsetHz != nullptr) {
    result.cell_frequency_offset_hz = read_i64(*source.cellFrequencyOffsetHz);
  }
  if (request.subscription_id.is_zero() || request.interface_id == 0 || request.sample_every_n == 0 ||
      request.queue_depth_chunks == 0 || request.maximum_chunk_bytes < 1024 ||
      request.maximum_chunk_bytes > maximum_data_bytes || request.lease_us == 0 || request.lease_us > 60'000'000) {
    throw std::runtime_error("invalid portable E3 subscription values");
  }
  return result;
}

void set_data_subscription_result(DataSubscriptionResult_t& destination, const data_subscription_result& source)
{
  if (source.subscriptions.size() > 32) {
    throw std::runtime_error("portable E3 subscription result exceeds bound");
  }
  for (const auto& item : source.subscriptions) {
    set_data_subscription_record(append_asn<DataSubscriptionRecord_t>(destination.subscriptions), item);
  }
}

data_subscription_result read_data_subscription_result(const DataSubscriptionResult_t& source)
{
  if (source.subscriptions.list.count < 0 || static_cast<std::size_t>(source.subscriptions.list.count) > 32) {
    throw std::runtime_error("portable E3 subscription result exceeds bound");
  }
  data_subscription_result result;
  result.subscriptions.reserve(source.subscriptions.list.count);
  for (int index = 0; index != source.subscriptions.list.count; ++index) {
    if (source.subscriptions.list.array[index] == nullptr) {
      throw std::runtime_error("null portable E3 subscription record");
    }
    result.subscriptions.push_back(read_data_subscription_record(*source.subscriptions.list.array[index]));
  }
  return result;
}

void set_data_publication_chunk(DataPublicationChunk_t& destination, const portable_publication_chunk& source)
{
  if (source.subscription_id.is_zero() || source.interface_id == 0 || source.publication_sequence == 0 ||
      source.route_epoch == 0 || source.chunk_count == 0 || source.chunk_index >= source.chunk_count ||
      source.tensor_size == 0 || source.tensor_chunk.empty() || source.tensor_chunk.size() > maximum_data_bytes ||
      source.metadata.size() > maximum_data_bytes || (source.chunk_index == 0) != !source.metadata.empty()) {
    throw std::runtime_error("invalid portable E3 publication chunk");
  }
  set_identifier(destination.subscriptionId, source.subscription_id);
  set_u64(destination.interfaceId, source.interface_id);
  set_u64(destination.cellId, source.cell_id);
  set_u64(destination.publicationSequence, source.publication_sequence);
  set_u64(destination.routeEpoch, source.route_epoch);
  set_u64(destination.chunkIndex, source.chunk_index);
  set_u64(destination.chunkCount, source.chunk_count);
  set_u64(destination.tensorSize, source.tensor_size);
  set_u64(destination.droppedObjects, source.dropped_objects);
  if (!source.metadata.empty()) {
    destination.metadata = make_asn<OCTET_STRING_t>();
    set_octets(*destination.metadata, source.metadata);
  }
  set_octets(destination.tensorChunk, source.tensor_chunk);
}

portable_publication_chunk read_data_publication_chunk(const DataPublicationChunk_t& source)
{
  portable_publication_chunk result;
  result.subscription_id      = read_identifier(source.subscriptionId);
  result.interface_id         = static_cast<uint32_t>(read_u64(source.interfaceId));
  result.cell_id              = static_cast<uint32_t>(read_u64(source.cellId));
  result.publication_sequence = read_u64(source.publicationSequence);
  result.route_epoch          = read_u64(source.routeEpoch);
  result.chunk_index          = static_cast<uint32_t>(read_u64(source.chunkIndex));
  result.chunk_count          = static_cast<uint32_t>(read_u64(source.chunkCount));
  result.tensor_size          = read_u64(source.tensorSize);
  result.dropped_objects      = read_u64(source.droppedObjects);
  if (source.metadata != nullptr) {
    result.metadata = read_bytes(*source.metadata, maximum_data_bytes);
  }
  result.tensor_chunk = read_bytes(source.tensorChunk, maximum_data_bytes);
  if (result.subscription_id.is_zero() || result.interface_id == 0 || result.publication_sequence == 0 ||
      result.route_epoch == 0 || result.chunk_count == 0 || result.chunk_index >= result.chunk_count ||
      result.tensor_size == 0 || result.tensor_chunk.empty() || (result.chunk_index == 0) != !result.metadata.empty()) {
    throw std::runtime_error("invalid portable E3 publication chunk");
  }
  return result;
}

void set_data_publication_result(DataPublicationResult_t& destination, const data_publication_result& source)
{
  if (source.chunks.size() > maximum_data_chunks) {
    throw std::runtime_error("portable E3 publication result exceeds bound");
  }
  for (const auto& item : source.chunks) {
    set_data_publication_chunk(append_asn<DataPublicationChunk_t>(destination.chunks), item);
  }
  if (source.subscription) {
    destination.subscription = make_asn<DataSubscriptionRecord_t>();
    set_data_subscription_record(*destination.subscription, *source.subscription);
  }
}

data_publication_result read_data_publication_result(const DataPublicationResult_t& source)
{
  if (source.chunks.list.count < 0 || static_cast<std::size_t>(source.chunks.list.count) > maximum_data_chunks) {
    throw std::runtime_error("portable E3 publication result exceeds bound");
  }
  data_publication_result result;
  result.chunks.reserve(source.chunks.list.count);
  for (int index = 0; index != source.chunks.list.count; ++index) {
    if (source.chunks.list.array[index] == nullptr) {
      throw std::runtime_error("null portable E3 publication chunk");
    }
    result.chunks.push_back(read_data_publication_chunk(*source.chunks.list.array[index]));
  }
  if (source.subscription != nullptr) {
    result.subscription = read_data_subscription_record(*source.subscription);
  }
  return result;
}

void set_job_record(JobRecord_t& destination, const job_record& source)
{
  set_identifier(destination.jobId, source.job_id);
  set_identifier(destination.transactionId, source.transaction_id);
  destination.state           = static_cast<long>(source.state);
  destination.progressPercent = source.progress_percent;
  set_u64(destination.submittedNs, source.submitted_monotonic_ns);
  if (source.started_monotonic_ns != 0) {
    destination.startedNs = make_asn<TimestampNs_t>();
    set_u64(*destination.startedNs, source.started_monotonic_ns);
  }
  if (source.completed_monotonic_ns != 0) {
    destination.completedNs = make_asn<TimestampNs_t>();
    set_u64(*destination.completedNs, source.completed_monotonic_ns);
  }
  if (source.result) {
    destination.result = make_asn<OperationResult_t>();
    set_operation_result(*destination.result, *source.result);
  }
  if (!source.detail.empty()) {
    destination.detail = make_asn<OCTET_STRING_t>();
    const std::string_view detail(source.detail.data(), std::min(source.detail.size(), maximum_text_bytes));
    set_text(*destination.detail, detail);
  }
}

job_record read_job_record(const JobRecord_t& source)
{
  if (source.state < JobState_accepted || source.state > JobState_cancelled || source.progressPercent < 0 ||
      source.progressPercent > 100) {
    throw std::runtime_error("invalid job record");
  }
  job_record result;
  result.job_id                 = read_identifier(source.jobId);
  result.transaction_id         = read_identifier(source.transactionId);
  result.state                  = static_cast<job_state>(source.state);
  result.progress_percent       = static_cast<uint8_t>(source.progressPercent);
  result.submitted_monotonic_ns = read_u64(source.submittedNs);
  if (source.startedNs != nullptr) {
    result.started_monotonic_ns = read_u64(*source.startedNs);
  }
  if (source.completedNs != nullptr) {
    result.completed_monotonic_ns = read_u64(*source.completedNs);
  }
  if (source.result != nullptr) {
    result.result = read_operation_result(*source.result);
  }
  if (source.detail != nullptr) {
    result.detail = read_string(*source.detail, maximum_text_bytes);
  } else if (result.result) {
    result.detail = result.result->detail;
  }
  return result;
}

void set_job_result(JobResult_t& destination, const job_result& source)
{
  for (const auto& item : source.jobs) {
    set_job_record(append_asn<JobRecord_t>(destination.jobs), item);
  }
}

job_result read_job_result(const JobResult_t& source)
{
  if (static_cast<std::size_t>(source.jobs.list.count) > maximum_items) {
    throw std::runtime_error("job result exceeds bound");
  }
  job_result result;
  result.jobs.reserve(source.jobs.list.count);
  for (int index = 0; index != source.jobs.list.count; ++index) {
    if (source.jobs.list.array[index] == nullptr) {
      throw std::runtime_error("null job record");
    }
    result.jobs.push_back(read_job_record(*source.jobs.list.array[index]));
  }
  return result;
}

} // namespace

codec_result<std::vector<uint8_t>> aper_codec::encode(const management_response& response) noexcept
{
  try {
    response_owner owner{make_asn<ManagementResponse_t>()};
    auto*          wire = owner.value;
    set_identifier(wire->transactionId, response.transaction_id);
    wire->code = to_wire(response.code);
    if (response.job_id) {
      wire->jobId = make_asn<Identifier_t>();
      set_identifier(*wire->jobId, *response.job_id);
    }
    if (response.result) {
      wire->result = make_asn<OperationResult_t>();
      set_operation_result(*wire->result, *response.result);
    }
    std::visit(
        [wire](const auto& payload) {
          using payload_type = std::decay_t<decltype(payload)>;
          if constexpr (!std::is_same_v<payload_type, std::monostate>) {
            wire->payload = make_asn<ManagementPayload_t>();
            if constexpr (std::is_same_v<payload_type, pusch_routing_result>) {
              wire->payload->present             = ManagementPayload_PR_puschRouting;
              wire->payload->choice.puschRouting = make_asn<PuschRoutingResult_t>();
              set_routing_result(*wire->payload->choice.puschRouting, payload);
            } else if constexpr (std::is_same_v<payload_type, cell_info_result>) {
              wire->payload->present         = ManagementPayload_PR_cellInfo;
              wire->payload->choice.cellInfo = make_asn<CellInfoResult_t>();
              set_cell_info_result(*wire->payload->choice.cellInfo, payload);
            } else if constexpr (std::is_same_v<payload_type, inventory_result>) {
              wire->payload->present          = ManagementPayload_PR_inventory;
              wire->payload->choice.inventory = make_asn<InventoryResult_t>();
              set_inventory_result(*wire->payload->choice.inventory, payload);
            } else if constexpr (std::is_same_v<payload_type, config_result>) {
              wire->payload->present       = ManagementPayload_PR_config;
              wire->payload->choice.config = make_asn<ConfigResult_t>();
              set_config_result(*wire->payload->choice.config, payload);
            } else if constexpr (std::is_same_v<payload_type, metric_result>) {
              wire->payload->present        = ManagementPayload_PR_metrics;
              wire->payload->choice.metrics = make_asn<MetricResult_t>();
              set_metric_result(*wire->payload->choice.metrics, payload);
            } else if constexpr (std::is_same_v<payload_type, incident_result>) {
              wire->payload->present          = ManagementPayload_PR_incidents;
              wire->payload->choice.incidents = make_asn<IncidentResult_t>();
              set_incident_result(*wire->payload->choice.incidents, payload);
            } else if constexpr (std::is_same_v<payload_type, incident_health_summary>) {
              wire->payload->present       = ManagementPayload_PR_health;
              wire->payload->choice.health = make_asn<HealthSummary_t>();
              set_health_summary(*wire->payload->choice.health, payload);
            } else if constexpr (std::is_same_v<payload_type, job_result>) {
              wire->payload->present     = ManagementPayload_PR_jobs;
              wire->payload->choice.jobs = make_asn<JobResult_t>();
              set_job_result(*wire->payload->choice.jobs, payload);
            } else if constexpr (std::is_same_v<payload_type, quiet_reservation_result>) {
              wire->payload->present                  = ManagementPayload_PR_quietReservations;
              wire->payload->choice.quietReservations = make_asn<QuietReservationResult_t>();
              set_quiet_reservation_result(*wire->payload->choice.quietReservations, payload);
            } else if constexpr (std::is_same_v<payload_type, data_subscription_result>) {
              wire->payload->present                  = ManagementPayload_PR_dataSubscriptions;
              wire->payload->choice.dataSubscriptions = make_asn<DataSubscriptionResult_t>();
              set_data_subscription_result(*wire->payload->choice.dataSubscriptions, payload);
            } else if constexpr (std::is_same_v<payload_type, data_publication_result>) {
              wire->payload->present                 = ManagementPayload_PR_dataPublications;
              wire->payload->choice.dataPublications = make_asn<DataPublicationResult_t>();
              set_data_publication_result(*wire->payload->choice.dataPublications, payload);
            }
          }
        },
        response.payload);
    if (!response.error_text.empty()) {
      wire->errorText = make_asn<OCTET_STRING_t>();
      const std::string_view error(response.error_text.data(),
                                   std::min(response.error_text.size(), maximum_text_bytes));
      set_text(*wire->errorText, error);
    }
    return {encode_asn(asn_DEF_ManagementResponse, *wire), {}};
  } catch (const std::exception& exception) {
    return {std::nullopt, exception.what()};
  } catch (...) {
    return {std::nullopt, "unknown APER response encoding failure"};
  }
}

codec_result<management_response> aper_codec::decode_response(ocudu::span<const uint8_t> bytes) noexcept
{
  try {
    auto                owner = decode_asn_response(bytes);
    management_response result;
    result.transaction_id = read_identifier(owner.value->transactionId);
    result.code           = from_wire_result(owner.value->code);
    if (owner.value->jobId != nullptr) {
      result.job_id = read_identifier(*owner.value->jobId);
    }
    if (owner.value->result != nullptr) {
      result.result = read_operation_result(*owner.value->result);
    }
    if (owner.value->errorText != nullptr) {
      result.error_text = read_string(*owner.value->errorText, maximum_text_bytes);
    }
    if (owner.value->payload != nullptr) {
      const auto& wire = *owner.value->payload;
      switch (wire.present) {
        case ManagementPayload_PR_inventory:
          if (wire.choice.inventory != nullptr) {
            result.payload = read_inventory_result(*wire.choice.inventory);
            break;
          }
          throw std::runtime_error("null inventory payload");
        case ManagementPayload_PR_config:
          if (wire.choice.config != nullptr) {
            result.payload = read_config_result(*wire.choice.config);
            break;
          }
          throw std::runtime_error("null config payload");
        case ManagementPayload_PR_metrics:
          if (wire.choice.metrics != nullptr) {
            result.payload = read_metric_result(*wire.choice.metrics);
            break;
          }
          throw std::runtime_error("null metric payload");
        case ManagementPayload_PR_incidents:
          if (wire.choice.incidents != nullptr) {
            result.payload = read_incident_result(*wire.choice.incidents);
            break;
          }
          throw std::runtime_error("null incident payload");
        case ManagementPayload_PR_health:
          if (wire.choice.health != nullptr) {
            result.payload = read_health_summary(*wire.choice.health);
            break;
          }
          throw std::runtime_error("null health payload");
        case ManagementPayload_PR_cellInfo:
          if (wire.choice.cellInfo != nullptr) {
            result.payload = read_cell_info_result(*wire.choice.cellInfo);
            break;
          }
          throw std::runtime_error("null cell info payload");
        case ManagementPayload_PR_puschRouting:
          if (wire.choice.puschRouting != nullptr) {
            result.payload = read_routing_result(*wire.choice.puschRouting);
            break;
          }
          throw std::runtime_error("null PUSCH routing payload");
        case ManagementPayload_PR_jobs:
          if (wire.choice.jobs != nullptr) {
            result.payload = read_job_result(*wire.choice.jobs);
            break;
          }
          throw std::runtime_error("null job payload");
        case ManagementPayload_PR_quietReservations:
          if (wire.choice.quietReservations != nullptr) {
            result.payload = read_quiet_reservation_result(*wire.choice.quietReservations);
            break;
          }
          throw std::runtime_error("null quiet-reservation payload");
        case ManagementPayload_PR_dataSubscriptions:
          if (wire.choice.dataSubscriptions != nullptr) {
            result.payload = read_data_subscription_result(*wire.choice.dataSubscriptions);
            break;
          }
          throw std::runtime_error("null portable E3 subscription payload");
        case ManagementPayload_PR_dataPublications:
          if (wire.choice.dataPublications != nullptr) {
            result.payload = read_data_publication_result(*wire.choice.dataPublications);
            break;
          }
          throw std::runtime_error("null portable E3 publication payload");
        default:
          throw std::runtime_error("unknown management response payload");
      }
    }
    return {std::move(result), {}};
  } catch (const std::exception& exception) {
    return {std::nullopt, exception.what()};
  } catch (...) {
    return {std::nullopt, "unknown APER response decoding failure"};
  }
}
