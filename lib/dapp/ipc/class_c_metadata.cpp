// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/ipc/class_c_metadata.h"
#include "class-c-publication-v1_generated.h"
#include "ocudu/dapp/use_cases/v1/spectrum.h"
#include <algorithm>
#include <flatbuffers/verifier.h>
#include <limits>
#include <string_view>

using namespace ocudu::dapp::ipc;
namespace schema = Ocudu::Dapp::ClassC::V1;

namespace {

bool nonzero(const schema::Id128* id) noexcept
{
  return id != nullptr && (id->low() != 0 || id->high() != 0);
}

uint64_t element_size(schema::ElementType type) noexcept
{
  switch (type) {
    case schema::ElementType::Int8:
    case schema::ElementType::Uint8:
      return 1;
    case schema::ElementType::Int16:
    case schema::ElementType::Uint16:
    case schema::ElementType::Fp16:
    case schema::ElementType::Bf16:
      return 2;
    case schema::ElementType::Int32:
    case schema::ElementType::Fp32:
    case schema::ElementType::ComplexInt16:
    case schema::ElementType::ComplexBf16:
    case schema::ElementType::ComplexFp16:
      return 4;
    case schema::ElementType::ComplexFp32:
      return 8;
  }
  return 0;
}

bool valid_tensor(const schema::TensorDescriptor& tensor, uint64_t observation_time) noexcept
{
  const auto* dimensions = tensor.dimensions();
  const auto* strides    = tensor.byte_strides();
  const auto* layout     = tensor.layout_id();
  if (!nonzero(tensor.object_id()) || !nonzero(tensor.lease_id()) || tensor.allocation_generation() == 0 ||
      tensor.byte_length() == 0 || tensor.lease_expiry_ns() <= observation_time || !tensor.read_only() ||
      layout == nullptr || layout->size() == 0 || layout->size() > 128 || dimensions == nullptr || strides == nullptr ||
      dimensions->size() == 0 || dimensions->size() > 8 || dimensions->size() != strides->size()) {
    return false;
  }
  uint64_t extent = element_size(tensor.element_type());
  if (extent == 0) {
    return false;
  }
  for (flatbuffers::uoffset_t i = 0; i != dimensions->size(); ++i) {
    if ((*dimensions)[i] == 0) {
      return false;
    }
    const uint64_t count = (*dimensions)[i] - 1;
    if ((*strides)[i] != 0 && count > (std::numeric_limits<uint64_t>::max() - extent) / (*strides)[i]) {
      return false;
    }
    extent += count * (*strides)[i];
  }
  if (extent > tensor.byte_length() || tensor.byte_offset() > std::numeric_limits<uint64_t>::max() - extent) {
    return false;
  }
  if (tensor.domain() == schema::MemoryDomain::AcceleratorIpcExport &&
      (!nonzero(tensor.capability_id()) || tensor.device_ordinal() == std::numeric_limits<uint16_t>::max())) {
    return false;
  }
  return true;
}

schema::PublicationMetadata expected_metadata(schema::PublicationKind kind) noexcept
{
  switch (kind) {
    case schema::PublicationKind::Spectrum:
      return schema::PublicationMetadata::SpectrumMetadata;
    case schema::PublicationKind::SrsIsac:
      return schema::PublicationMetadata::SrsIsacMetadata;
    case schema::PublicationKind::UlResourcePlan:
      return schema::PublicationMetadata::UlResourcePlanMetadata;
    case schema::PublicationKind::InterferenceMap:
      return schema::PublicationMetadata::InterferenceMapMetadata;
    case schema::PublicationKind::QuietPeriodIntent:
      return schema::PublicationMetadata::QuietPeriodIntentMetadata;
    case schema::PublicationKind::QuietPeriodReservation:
      return schema::PublicationMetadata::QuietPeriodReservationMetadata;
  }
  return schema::PublicationMetadata::NONE;
}

bool valid_metadata(const schema::PublicationEnvelope& envelope) noexcept
{
  switch (envelope.publication_kind()) {
    case schema::PublicationKind::Spectrum: {
      const auto* item = envelope.metadata_as_SpectrumMetadata();
      if (item == nullptr || item->bandwidth_hz() == 0 || item->rx_ports() == 0 || item->symbols() == 0 ||
          item->symbols() > 14 || item->subcarriers() == 0) {
        return false;
      }
      const auto* annotations = item->occupancy_annotations();
      if (item->plan_generation() == 0) {
        return !item->occupancy_complete() && item->occupancy_granularity() == schema::GridMaskGranularity::None &&
               (annotations == nullptr || annotations->size() == 0);
      }
      if (!item->occupancy_complete() || item->occupancy_granularity() != schema::GridMaskGranularity::SymbolPrb ||
          annotations == nullptr || item->subcarriers() % 12 != 0 ||
          annotations->size() != static_cast<uint64_t>(item->symbols()) * (item->subcarriers() / 12)) {
        return false;
      }
      return std::all_of(annotations->begin(), annotations->end(), [](uint8_t value) {
        return value <= static_cast<uint8_t>(schema::UlUseCategory::ReservedQuiet);
      });
    }
    case schema::PublicationKind::SrsIsac: {
      const auto* item = envelope.metadata_as_SrsIsacMetadata();
      return item != nullptr && item->rx_ports() != 0 && item->tx_ports() != 0 && item->frequency_points() != 0 &&
             (item->comb_size() == 2 || item->comb_size() == 4 || item->comb_size() == 8) &&
             item->subcarrier_spacing_hz() != 0;
    }
    case schema::PublicationKind::UlResourcePlan: {
      const auto* item = envelope.metadata_as_UlResourcePlanMetadata();
      if (item == nullptr || item->plan_generation() == 0 ||
          item->plan_generation() != envelope.publication_sequence() || item->first_prb() != 0 ||
          item->prb_count() == 0 || item->prb_count() > OCUDU_DAPP_SPECTRUM_MAX_PRBS_V1 ||
          item->mask_granularity() != schema::GridMaskGranularity::None) {
        return false;
      }
      if (const auto* spans = item->spans()) {
        if (spans->size() > OCUDU_DAPP_UL_RESOURCE_PLAN_MAX_SPANS_V1) {
          return false;
        }
        for (const auto* span : *spans) {
          if (span->symbol_count() == 0 || span->symbol_start() + span->symbol_count() > 14 || span->prb_count() == 0 ||
              static_cast<uint32_t>(span->prb_start()) + span->prb_count() > item->prb_count() ||
              span->use_category() > schema::UlUseCategory::ReservedQuiet) {
            return false;
          }
        }
      }
      return true;
    }
    case schema::PublicationKind::InterferenceMap: {
      const auto* item = envelope.metadata_as_InterferenceMapMetadata();
      return item != nullptr && item->valid_for_slots() != 0 && item->prb_count() != 0;
    }
    case schema::PublicationKind::QuietPeriodIntent: {
      const auto* item = envelope.metadata_as_QuietPeriodIntentMetadata();
      return item != nullptr && nonzero(item->request_id()) && item->duration_slots() != 0 &&
             item->repeat_count() != 0 && item->prb_count() != 0 && item->expiry_time_ns() != 0;
    }
    case schema::PublicationKind::QuietPeriodReservation: {
      const auto* item = envelope.metadata_as_QuietPeriodReservationMetadata();
      return item != nullptr && nonzero(item->request_id()) && nonzero(item->reservation_id()) &&
             item->reservation_generation() != 0 && item->duration_slots() != 0 && item->repeat_count() != 0 &&
             item->prb_count() != 0;
    }
  }
  return false;
}

} // namespace

metadata_error ocudu::dapp::ipc::validate_class_c_metadata(ocudu::span<const uint8_t> buffer) noexcept
{
  if (buffer.empty() || buffer.size() > 1024 * 1024) {
    return metadata_error::malformed_buffer;
  }
  flatbuffers::Verifier verifier(buffer.data(), buffer.size());
  if (!schema::VerifyPublicationEnvelopeBuffer(verifier)) {
    return metadata_error::malformed_buffer;
  }
  const auto* envelope = schema::GetPublicationEnvelope(buffer.data());
  if (envelope->schema_id() == nullptr || envelope->schema_id()->string_view() != class_c_schema_id ||
      envelope->schema_version_major() != 1) {
    return metadata_error::unsupported_schema;
  }
  const auto* context = envelope->context();
  if (context == nullptr || context->radio_time() == nullptr || context->clock_domain_id() == nullptr ||
      context->clock_domain_id()->size() == 0 || context->clock_domain_id()->size() > 128 ||
      context->radio_time()->numerology() > 4 || !nonzero(envelope->producer_instance())) {
    return metadata_error::invalid_context;
  }
  if (envelope->metadata_type() != expected_metadata(envelope->publication_kind())) {
    return metadata_error::metadata_mismatch;
  }
  const bool tensor_required = envelope->publication_kind() == schema::PublicationKind::Spectrum ||
                               envelope->publication_kind() == schema::PublicationKind::SrsIsac;
  const bool tensor_forbidden = envelope->publication_kind() == schema::PublicationKind::UlResourcePlan ||
                                envelope->publication_kind() == schema::PublicationKind::QuietPeriodIntent ||
                                envelope->publication_kind() == schema::PublicationKind::QuietPeriodReservation;
  if (tensor_required && envelope->tensor() == nullptr) {
    return metadata_error::tensor_required;
  }
  if (tensor_forbidden && envelope->tensor() != nullptr) {
    return metadata_error::tensor_forbidden;
  }
  if (envelope->tensor() != nullptr && !valid_tensor(*envelope->tensor(), context->observation_time_ns())) {
    return metadata_error::invalid_tensor;
  }
  return valid_metadata(*envelope) ? metadata_error::none : metadata_error::invalid_metadata;
}

const char* ocudu::dapp::ipc::to_string(metadata_error error) noexcept
{
  static constexpr const char* names[] = {"none",
                                          "malformed buffer",
                                          "unsupported schema",
                                          "invalid context",
                                          "metadata mismatch",
                                          "tensor required",
                                          "tensor forbidden",
                                          "invalid tensor",
                                          "invalid metadata"};
  const auto                   index   = static_cast<unsigned>(error);
  return index < sizeof(names) / sizeof(names[0]) ? names[index] : "unknown metadata error";
}
