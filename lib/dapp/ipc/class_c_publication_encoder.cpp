// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/ipc/class_c_publication_encoder.h"
#include "class-c-publication-v1_generated.h"
#include "ocudu/dapp/ipc/class_c_metadata.h"
#include "ocudu/dapp/runtime/abi_validation.h"
#include <array>
#include <cmath>
#include <cstddef>
#include <flatbuffers/flatbuffers.h>
#include <limits>
#include <new>
#include <optional>
#include <utility>

using namespace ocudu::dapp;
using namespace ocudu::dapp::ipc;
namespace schema = Ocudu::Dapp::ClassC::V1;

namespace {

constexpr uint32_t min_metadata_capacity = 1024;
constexpr uint32_t max_metadata_capacity = 64 * 1024;
static_assert(static_cast<uint8_t>(schema::UlUseCategory::Unknown) == OCUDU_DAPP_UL_USE_UNKNOWN_V1);
static_assert(static_cast<uint8_t>(schema::UlUseCategory::PlannedEmpty) == OCUDU_DAPP_UL_USE_PLANNED_EMPTY_V1);
static_assert(static_cast<uint8_t>(schema::UlUseCategory::Pusch) == OCUDU_DAPP_UL_USE_PUSCH_V1);
static_assert(static_cast<uint8_t>(schema::UlUseCategory::Pucch) == OCUDU_DAPP_UL_USE_PUCCH_V1);
static_assert(static_cast<uint8_t>(schema::UlUseCategory::Srs) == OCUDU_DAPP_UL_USE_SRS_V1);
static_assert(static_cast<uint8_t>(schema::UlUseCategory::PrachOrCommon) == OCUDU_DAPP_UL_USE_PRACH_OR_COMMON_V1);
static_assert(static_cast<uint8_t>(schema::UlUseCategory::ReferenceSignal) == OCUDU_DAPP_UL_USE_REFERENCE_SIGNAL_V1);
static_assert(static_cast<uint8_t>(schema::UlUseCategory::ReservedQuiet) == OCUDU_DAPP_UL_USE_RESERVED_QUIET_V1);

template <typename T>
const T* get_metadata(const ocudu_dapp_typed_metadata_v1& wrapper, uint32_t schema_id) noexcept
{
  if (wrapper.struct_size < offsetof(ocudu_dapp_typed_metadata_v1, metadata) + sizeof(wrapper.metadata) ||
      wrapper.abi_major != OCUDU_DAPP_ABI_MAJOR_V1 || wrapper.metadata_schema_id != schema_id ||
      wrapper.metadata_schema_version != 1 || wrapper.metadata.data == nullptr ||
      wrapper.metadata.size < offsetof(T, reserved)) {
    return nullptr;
  }
  const auto* value = static_cast<const T*>(wrapper.metadata.data);
  return value->struct_size >= offsetof(T, reserved) && value->abi_major == OCUDU_DAPP_ABI_MAJOR_V1 ? value : nullptr;
}

bool valid_invocation(const ocudu_dapp_invocation_v1& invocation, uint64_t source_sequence) noexcept
{
  return invocation.struct_size >= offsetof(ocudu_dapp_invocation_v1, reserved) &&
         invocation.abi_major == OCUDU_DAPP_ABI_MAJOR_V1 && invocation.radio_time.numerology <= 4 &&
         source_sequence != 0;
}

bool valid_radio_time(const ocudu_dapp_radio_time_v1& value) noexcept
{
  return value.numerology <= 4 && value.sfn < 1024 && value.slot < (10U << value.numerology) && value.reserved == 0;
}

bool same_radio_time(const ocudu_dapp_radio_time_v1& lhs, const ocudu_dapp_radio_time_v1& rhs) noexcept
{
  return lhs.sfn == rhs.sfn && lhs.slot == rhs.slot && lhs.numerology == rhs.numerology && lhs.reserved == rhs.reserved;
}

bool valid_host_tensor(const ocudu_dapp_tensor_view_v1& tensor) noexcept
{
  const bool host_domain = tensor.domain == OCUDU_DAPP_MEM_HOST_V1 || tensor.domain == OCUDU_DAPP_MEM_HOST_PINNED_V1 ||
                           tensor.domain == OCUDU_DAPP_MEM_MANAGED_COHERENT_V1;
  return validate_tensor_view(tensor) == validation_error::none && host_domain &&
         tensor.backend_id == OCUDU_DAPP_BACKEND_HOST_V1 &&
         (tensor.access & (OCUDU_DAPP_ACCESS_READ_V1 | OCUDU_DAPP_ACCESS_HOST_VISIBLE_V1)) ==
             (OCUDU_DAPP_ACCESS_READ_V1 | OCUDU_DAPP_ACCESS_HOST_VISIBLE_V1) &&
         tensor.native_handle != 0;
}

bool valid_cuda_export_tensor(const ocudu_dapp_tensor_view_v1& tensor) noexcept
{
  return validate_tensor_view(tensor) == validation_error::none &&
         tensor.domain == OCUDU_DAPP_MEM_ACCELERATOR_DEVICE_V1 && tensor.backend_id == OCUDU_DAPP_BACKEND_CUDA_V1 &&
         (tensor.access & OCUDU_DAPP_ACCESS_READ_V1) != 0 && (tensor.access & OCUDU_DAPP_ACCESS_HOST_VISIBLE_V1) == 0 &&
         tensor.native_handle != 0 && tensor.ready_token != 0 && tensor.completion_token != 0;
}

bool valid_expected_use(const ocudu_dapp_spectrum_input_v1&         input,
                        const ocudu_dapp_spectrum_slot_metadata_v1& metadata,
                        const uint8_t*&                             values) noexcept
{
  values             = nullptr;
  const auto& tensor = input.scheduled_re_mask;
  if ((metadata.flags & ~OCUDU_DAPP_SPECTRUM_FLAG_EXPECTED_UL_USE_COMPLETE_V1) != 0) {
    return false;
  }
  const bool present = (metadata.flags & OCUDU_DAPP_SPECTRUM_FLAG_EXPECTED_UL_USE_COMPLETE_V1) != 0;
  if (!present) {
    return metadata.source_plan_generation == 0 && tensor.struct_size == 0 && tensor.native_handle == 0 &&
           tensor.byte_length == 0;
  }
  if (metadata.source_plan_generation == 0 || metadata.subcarriers % 12 != 0) {
    return false;
  }
  const uint64_t nof_prbs = metadata.subcarriers / 12;
  const uint64_t count    = static_cast<uint64_t>(metadata.symbols) * nof_prbs;
  if (!valid_host_tensor(tensor) || tensor.element_type != OCUDU_DAPP_ELEMENT_U8_V1 || tensor.rank != 2 ||
      tensor.dimensions[0] != metadata.symbols || tensor.dimensions[1] != nof_prbs ||
      tensor.byte_strides[0] != nof_prbs || tensor.byte_strides[1] != sizeof(uint8_t) || tensor.byte_length != count ||
      tensor.layout_id != OCUDU_DAPP_LAYOUT_EXPECTED_UL_USE_SYMBOL_PRB_U8_V1 || tensor.layout_version != 1 ||
      tensor.allocation_generation != metadata.source_plan_generation ||
      tensor.byte_offset > std::numeric_limits<uintptr_t>::max() - tensor.native_handle) {
    return false;
  }
  values = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(tensor.native_handle + tensor.byte_offset));
  return std::all_of(
      values, values + count, [](uint8_t value) { return value <= OCUDU_DAPP_UL_USE_RESERVED_QUIET_V1; });
}

std::optional<schema::ElementType> element_type(uint32_t value) noexcept
{
  switch (value) {
    case OCUDU_DAPP_ELEMENT_CBF16_V1:
      return schema::ElementType::ComplexBf16;
    case OCUDU_DAPP_ELEMENT_CF32_V1:
      return schema::ElementType::ComplexFp32;
    default:
      return std::nullopt;
  }
}

uint16_t complete_symbol_mask(uint32_t symbols) noexcept
{
  return symbols >= 16 ? std::numeric_limits<uint16_t>::max() : static_cast<uint16_t>((1U << symbols) - 1U);
}

} // namespace

struct class_c_publication_encoder::impl {
  explicit impl(class_c_encoder_config config_) : config(std::move(config_)), builder(config.metadata_capacity) {}

  class_c_encode_result
  encode(const ocudu_dapp_spectrum_input_v1& input, uint64_t route_epoch, uint64_t observation_time_ns)
  {
    const auto* metadata =
        get_metadata<ocudu_dapp_spectrum_slot_metadata_v1>(input.slot_metadata, OCUDU_DAPP_METADATA_SPECTRUM_SLOT_V1);
    if (!valid_invocation(input.invocation, input.source_sequence) || metadata == nullptr ||
        metadata->bandwidth_hz == 0 || metadata->rx_ports == 0 ||
        metadata->rx_ports > std::numeric_limits<uint16_t>::max() || metadata->symbols == 0 || metadata->symbols > 14 ||
        metadata->subcarriers == 0 || !std::isfinite(metadata->amplitude_scale) || metadata->amplitude_scale <= 0.0F) {
      return {class_c_encode_error::invalid_input, {}};
    }
    const uint8_t* expected_use = nullptr;
    if (!valid_expected_use(input, *metadata, expected_use)) {
      return {class_c_encode_error::invalid_input, {}};
    }
    const bool     plan_present = metadata->source_plan_generation != 0;
    const uint64_t plan_size =
        plan_present ? static_cast<uint64_t>(metadata->symbols) * (metadata->subcarriers / 12) : 0;
    return encode_common(input.invocation,
                         input.source_sequence,
                         input.uplink_resource_grid,
                         route_epoch,
                         observation_time_ns,
                         "resource-grid-port-symbol-subcarrier",
                         complete_symbol_mask(metadata->symbols),
                         schema::PublicationKind::Spectrum,
                         schema::PublicationMetadata::SpectrumMetadata,
                         [&]() {
                           flatbuffers::Offset<flatbuffers::Vector<uint8_t>> annotations;
                           if (plan_present) {
                             annotations = builder.CreateVector(expected_use, static_cast<std::size_t>(plan_size));
                           }
                           return schema::CreateSpectrumMetadata(builder,
                                                                 metadata->bandwidth_hz,
                                                                 static_cast<uint16_t>(metadata->rx_ports),
                                                                 static_cast<uint8_t>(metadata->symbols),
                                                                 metadata->subcarriers,
                                                                 metadata->amplitude_scale,
                                                                 0,
                                                                 0,
                                                                 observation_time_ns,
                                                                 annotations,
                                                                 metadata->source_plan_generation,
                                                                 plan_present ? schema::GridMaskGranularity::SymbolPrb
                                                                              : schema::GridMaskGranularity::None,
                                                                 plan_present)
                               .Union();
                         });
  }

  class_c_encode_result
  encode(const ocudu_dapp_srs_isac_input_v1& input, uint64_t route_epoch, uint64_t observation_time_ns)
  {
    const auto* metadata =
        get_metadata<ocudu_dapp_srs_isac_metadata_v1>(input.srs_metadata, OCUDU_DAPP_METADATA_SRS_ISAC_V1);
    if (!valid_invocation(input.invocation, input.source_sequence) || metadata == nullptr ||
        metadata->srs_resource_id > std::numeric_limits<uint16_t>::max() ||
        metadata->srs_config_id > std::numeric_limits<uint16_t>::max() || metadata->rx_ports == 0 ||
        metadata->rx_ports > std::numeric_limits<uint16_t>::max() || metadata->tx_ports == 0 ||
        metadata->tx_ports > std::numeric_limits<uint16_t>::max() ||
        (metadata->comb_size != 2 && metadata->comb_size != 4 && metadata->comb_size != 8) ||
        metadata->comb_offset >= metadata->comb_size || metadata->comb_offset > std::numeric_limits<uint8_t>::max() ||
        metadata->subcarrier_spacing_hz == 0 || metadata->frequency_points == 0 ||
        !std::isfinite(metadata->noise_variance) || metadata->noise_variance < 0.0F ||
        !std::isfinite(metadata->rsrp_db) || !std::isfinite(metadata->epre_db) ||
        metadata->timing_advance_ns < std::numeric_limits<int32_t>::min() ||
        metadata->timing_advance_ns > std::numeric_limits<int32_t>::max() || input.channel_estimates.rank != 3 ||
        input.channel_estimates.dimensions[0] != metadata->rx_ports ||
        input.channel_estimates.dimensions[1] != metadata->tx_ports ||
        input.channel_estimates.dimensions[2] != metadata->frequency_points ||
        !std::all_of(
            std::begin(metadata->reserved), std::end(metadata->reserved), [](uint64_t value) { return value == 0; })) {
      return {class_c_encode_error::invalid_input, {}};
    }
    return encode_common(input.invocation,
                         input.source_sequence,
                         input.channel_estimates,
                         route_epoch,
                         observation_time_ns,
                         "srs-rx-port-tx-port-frequency",
                         0,
                         schema::PublicationKind::SrsIsac,
                         schema::PublicationMetadata::SrsIsacMetadata,
                         [&]() {
                           return schema::CreateSrsIsacMetadata(builder,
                                                                static_cast<uint16_t>(metadata->srs_resource_id),
                                                                static_cast<uint16_t>(metadata->srs_config_id),
                                                                static_cast<uint16_t>(metadata->rx_ports),
                                                                static_cast<uint16_t>(metadata->tx_ports),
                                                                static_cast<uint8_t>(metadata->comb_size),
                                                                static_cast<uint8_t>(metadata->comb_offset),
                                                                metadata->first_subcarrier,
                                                                metadata->subcarrier_spacing_hz,
                                                                metadata->frequency_points,
                                                                metadata->noise_variance,
                                                                metadata->rsrp_db,
                                                                metadata->epre_db,
                                                                static_cast<int32_t>(metadata->timing_advance_ns),
                                                                0,
                                                                nullptr,
                                                                observation_time_ns)
                               .Union();
                         });
  }

  class_c_encode_result
  encode(const ocudu_dapp_ul_resource_plan_v1& plan, uint64_t route_epoch, uint64_t observation_time_ns)
  {
    constexpr uint32_t minimum = offsetof(ocudu_dapp_ul_resource_plan_v1, reserved);
    if (plan.struct_size < minimum || plan.abi_major != OCUDU_DAPP_ABI_MAJOR_V1 || plan.plan_generation == 0 ||
        plan.context.struct_size < offsetof(ocudu_dapp_context_header_v1, reserved) ||
        plan.context.schema_id != OCUDU_DAPP_CONTEXT_UL_RESOURCE_PLAN_V1 || plan.context.schema_version != 1 ||
        plan.context.flags != 0 ||
        (plan.context.producer_instance_id[0] == 0 && plan.context.producer_instance_id[1] == 0) ||
        plan.context.producer_generation == 0 || plan.context.source_sequence != plan.plan_generation ||
        plan.context.confidence_q16 > (1U << 16U) || plan.context.provenance_hash != 0 ||
        !std::all_of(std::begin(plan.context.reserved),
                     std::end(plan.context.reserved),
                     [](uint64_t value) { return value == 0; }) ||
        !valid_radio_time(plan.context.created) || !valid_radio_time(plan.context.valid_from) ||
        !valid_radio_time(plan.context.valid_until) || !valid_radio_time(plan.planned_time) ||
        !same_radio_time(plan.planned_time, plan.context.valid_from) ||
        plan.nof_spans > OCUDU_DAPP_UL_RESOURCE_PLAN_MAX_SPANS_V1 || (plan.nof_spans != 0 && plan.spans == nullptr) ||
        plan.cell_nof_prbs == 0 || plan.cell_nof_prbs > OCUDU_DAPP_SPECTRUM_MAX_PRBS_V1 || plan.reserved0 != 0 ||
        (plan.flags & ~OCUDU_DAPP_UL_RESOURCE_PLAN_COMPLETE_V1) != 0 || plan.optional_dense_mask.struct_size != 0 ||
        !std::all_of(std::begin(plan.reserved), std::end(plan.reserved), [](uint64_t value) { return value == 0; }) ||
        route_epoch == 0 || observation_time_ns == 0) {
      return {class_c_encode_error::invalid_input, {}};
    }

    for (uint32_t index = 0; index != plan.nof_spans; ++index) {
      const auto& source = plan.spans[index];
      if (source.struct_size < offsetof(ocudu_dapp_ul_resource_span_v1, reserved) || source.symbol_count == 0 ||
          source.symbol_start + source.symbol_count > 14 || source.nof_prbs == 0 ||
          static_cast<uint32_t>(source.first_prb) + source.nof_prbs > plan.cell_nof_prbs ||
          source.use_category > OCUDU_DAPP_UL_USE_RESERVED_QUIET_V1 || source.reserved0 != 0 || source.reserved1 != 0 ||
          !std::all_of(
              std::begin(source.reserved), std::end(source.reserved), [](uint64_t value) { return value == 0; })) {
        return {class_c_encode_error::invalid_input, {}};
      }
    }

    builder.Clear();
    std::array<flatbuffers::Offset<schema::UlAllocationSpan>, OCUDU_DAPP_UL_RESOURCE_PLAN_MAX_SPANS_V1> spans{};
    for (uint32_t index = 0; index != plan.nof_spans; ++index) {
      const auto& source = plan.spans[index];
      spans[index]       = schema::CreateUlAllocationSpan(builder,
                                                    source.symbol_start,
                                                    source.symbol_count,
                                                    source.first_prb,
                                                    source.nof_prbs,
                                                    static_cast<schema::UlUseCategory>(source.use_category),
                                                    source.dmrs_symbol_mask,
                                                    source.allocation_id);
    }
    const schema::Id128     producer(config.producer_instance.low, config.producer_instance.high);
    const schema::Id128     ran_node(config.ran_node.low, config.ran_node.high);
    const schema::RadioTime radio_time(
        plan.planned_time.sfn, plan.planned_time.slot, static_cast<uint8_t>(plan.planned_time.numerology), 0);
    const auto schema_name   = builder.CreateString(class_c_schema_id);
    const auto clock_domain  = builder.CreateString(config.clock_domain_id);
    const auto context       = schema::CreateObservationContext(builder,
                                                          &radio_time,
                                                          plan.context.cell_id,
                                                          config.ran_node ? &ran_node : nullptr,
                                                          clock_domain,
                                                          observation_time_ns,
                                                          nullptr,
                                                          config.reference_frequency_hz);
    const auto encoded_spans = builder.CreateVector(spans.data(), plan.nof_spans);
    const auto metadata =
        schema::CreateUlResourcePlanMetadata(builder,
                                             plan.plan_generation,
                                             observation_time_ns,
                                             0,
                                             static_cast<uint16_t>(plan.cell_nof_prbs),
                                             schema::GridMaskGranularity::None,
                                             encoded_spans,
                                             (plan.flags & OCUDU_DAPP_UL_RESOURCE_PLAN_COMPLETE_V1) != 0)
            .Union();
    const auto envelope = schema::CreatePublicationEnvelope(builder,
                                                            schema_name,
                                                            1,
                                                            0,
                                                            schema::PublicationKind::UlResourcePlan,
                                                            plan.plan_generation,
                                                            &producer,
                                                            context,
                                                            0,
                                                            schema::PublicationMetadata::UlResourcePlanMetadata,
                                                            metadata);
    schema::FinishPublicationEnvelopeBuffer(builder, envelope);
    if (builder.GetSize() > config.metadata_capacity) {
      return {class_c_encode_error::metadata_too_large, {}};
    }
    ocudu::span<const uint8_t> encoded(builder.GetBufferPointer(), builder.GetSize());
    return validate_class_c_metadata(encoded) == metadata_error::none
               ? class_c_encode_result{class_c_encode_error::none, encoded}
               : class_c_encode_result{class_c_encode_error::internal_error, {}};
  }

  template <typename MetadataBuilder>
  class_c_encode_result encode_common(const ocudu_dapp_invocation_v1&  invocation,
                                      uint64_t                         source_sequence,
                                      const ocudu_dapp_tensor_view_v1& tensor,
                                      uint64_t                         route_epoch,
                                      uint64_t                         observation_time_ns,
                                      const char*                      layout,
                                      uint16_t                         symbol_mask,
                                      schema::PublicationKind          kind,
                                      schema::PublicationMetadata      metadata_type,
                                      MetadataBuilder&&                make_metadata)
  {
    const auto encoded_type = element_type(tensor.element_type);
    const bool host_tensor  = valid_host_tensor(tensor);
    const bool cuda_tensor  = valid_cuda_export_tensor(tensor);
    if ((!host_tensor && !cuda_tensor) || !encoded_type || route_epoch == 0 || observation_time_ns == 0 ||
        observation_time_ns == std::numeric_limits<uint64_t>::max()) {
      return {encoded_type ? class_c_encode_error::invalid_input : class_c_encode_error::unsupported_tensor, {}};
    }
    const uint64_t lease_expiry = config.lease_duration_ns > std::numeric_limits<uint64_t>::max() - observation_time_ns
                                      ? std::numeric_limits<uint64_t>::max()
                                      : observation_time_ns + config.lease_duration_ns;

    builder.Clear();
    const schema::Id128     producer(config.producer_instance.low, config.producer_instance.high);
    const schema::Id128     ran_node(config.ran_node.low, config.ran_node.high);
    const schema::Id128     object_id(tensor.allocation_id, source_sequence);
    const schema::Id128     lease_id(route_epoch, source_sequence);
    const schema::Id128     capability_id(route_epoch, tensor.allocation_id);
    const schema::RadioTime radio_time(invocation.radio_time.sfn,
                                       invocation.radio_time.slot,
                                       static_cast<uint8_t>(invocation.radio_time.numerology),
                                       symbol_mask);
    const auto              schema_name  = builder.CreateString(class_c_schema_id);
    const auto              clock_domain = builder.CreateString(config.clock_domain_id);
    const auto              layout_name  = builder.CreateString(layout);
    const auto              dimensions   = builder.CreateVector(tensor.dimensions, tensor.rank);
    const auto              strides      = builder.CreateVector(tensor.byte_strides, tensor.rank);
    const auto              context      = schema::CreateObservationContext(builder,
                                                          &radio_time,
                                                          invocation.cell_id,
                                                          config.ran_node ? &ran_node : nullptr,
                                                          clock_domain,
                                                          observation_time_ns,
                                                          nullptr,
                                                          config.reference_frequency_hz);
    const auto              descriptor   = schema::CreateTensorDescriptor(
        builder,
        &object_id,
        tensor.allocation_generation,
        cuda_tensor ? schema::MemoryDomain::AcceleratorIpcExport
                       : config.host_object_profile == class_c_host_object_profile::embedded_payload
                           ? schema::MemoryDomain::Host
                           : schema::MemoryDomain::PosixShared,
        cuda_tensor ? OCUDU_DAPP_BACKEND_CUDA_V1 : OCUDU_DAPP_BACKEND_HOST_V1,
        *encoded_type,
        layout_name,
        1,
        0,
        0,
        tensor.byte_length,
        dimensions,
        strides,
        cuda_tensor ? static_cast<uint16_t>(tensor.device_ordinal) : std::numeric_limits<uint16_t>::max(),
        cuda_tensor ? &capability_id : nullptr,
        &lease_id,
        lease_expiry,
        true);
    const auto metadata = make_metadata();
    const auto envelope = schema::CreatePublicationEnvelope(
        builder, schema_name, 1, 0, kind, source_sequence, &producer, context, descriptor, metadata_type, metadata);
    schema::FinishPublicationEnvelopeBuffer(builder, envelope);
    if (builder.GetSize() > config.metadata_capacity) {
      return {class_c_encode_error::metadata_too_large, {}};
    }
    ocudu::span<const uint8_t> encoded(builder.GetBufferPointer(), builder.GetSize());
    if (validate_class_c_metadata(encoded) != metadata_error::none) {
      return {class_c_encode_error::internal_error, {}};
    }
    return {class_c_encode_error::none, encoded};
  }

  class_c_encoder_config         config;
  flatbuffers::FlatBufferBuilder builder;
};

class_c_encoder_create_result class_c_publication_encoder::create(class_c_encoder_config config) noexcept
{
  if (!config.producer_instance || config.clock_domain_id.empty() || config.clock_domain_id.size() > 128 ||
      config.lease_duration_ns == 0 || config.metadata_capacity < min_metadata_capacity ||
      config.metadata_capacity > max_metadata_capacity ||
      (config.host_object_profile != class_c_host_object_profile::posix_shared &&
       config.host_object_profile != class_c_host_object_profile::embedded_payload)) {
    return {nullptr, "invalid Class-C encoder configuration"};
  }
  try {
    return {std::unique_ptr<class_c_publication_encoder>(
                new class_c_publication_encoder(std::make_unique<impl>(std::move(config)))),
            {}};
  } catch (const std::bad_alloc&) {
    return {nullptr, "unable to allocate Class-C metadata encoder"};
  } catch (...) {
    return {nullptr, "unknown Class-C metadata encoder construction failure"};
  }
}

class_c_publication_encoder::class_c_publication_encoder(std::unique_ptr<impl> state_) : state(std::move(state_)) {}
class_c_publication_encoder::~class_c_publication_encoder()                                                 = default;
class_c_publication_encoder::class_c_publication_encoder(class_c_publication_encoder&&) noexcept            = default;
class_c_publication_encoder& class_c_publication_encoder::operator=(class_c_publication_encoder&&) noexcept = default;

class_c_encode_result class_c_publication_encoder::encode(const ocudu_dapp_spectrum_input_v1& input,
                                                          uint64_t                            route_epoch,
                                                          uint64_t observation_time_ns) noexcept
{
  try {
    return state->encode(input, route_epoch, observation_time_ns);
  } catch (const std::bad_alloc&) {
    return {class_c_encode_error::metadata_too_large, {}};
  } catch (...) {
    return {class_c_encode_error::internal_error, {}};
  }
}

class_c_encode_result class_c_publication_encoder::encode(const ocudu_dapp_srs_isac_input_v1& input,
                                                          uint64_t                            route_epoch,
                                                          uint64_t observation_time_ns) noexcept
{
  try {
    return state->encode(input, route_epoch, observation_time_ns);
  } catch (const std::bad_alloc&) {
    return {class_c_encode_error::metadata_too_large, {}};
  } catch (...) {
    return {class_c_encode_error::internal_error, {}};
  }
}

class_c_encode_result class_c_publication_encoder::encode(const ocudu_dapp_ul_resource_plan_v1& plan,
                                                          uint64_t                              route_epoch,
                                                          uint64_t observation_time_ns) noexcept
{
  try {
    return state->encode(plan, route_epoch, observation_time_ns);
  } catch (const std::bad_alloc&) {
    return {class_c_encode_error::metadata_too_large, {}};
  } catch (...) {
    return {class_c_encode_error::internal_error, {}};
  }
}

const char* ocudu::dapp::ipc::to_string(class_c_encode_error error) noexcept
{
  static constexpr const char* names[] = {
      "none", "invalid config", "invalid input", "unsupported tensor", "metadata too large", "internal error"};
  const auto index = static_cast<unsigned>(error);
  return index < sizeof(names) / sizeof(names[0]) ? names[index] : "unknown Class-C encode error";
}
