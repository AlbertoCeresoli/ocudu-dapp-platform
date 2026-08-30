// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear
//
// Host-side input validation for native_worker (split TU — no behavior change).

#include "native_worker_validation.h"
#include "ocudu/dapp/abi/v1/metric_catalog.h"
#include "ocudu/dapp/runtime/abi_validation.h"
#include "ocudu/ran/gnb_constants.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>

namespace ocudu::dapp::native_worker_detail {

static_assert(OCUDU_DAPP_HOST_INPUT_CELL_BITMAP_BITS_V1 >= ocudu::MAX_CELLS_PER_DU);
static_assert(std::atomic<uint32_t>::is_always_lock_free);

bool valid_invocation(const ocudu_dapp_invocation_v1& invocation, uint32_t worker_id) noexcept
{
  constexpr uint32_t minimum = offsetof(ocudu_dapp_invocation_v1, reserved);
  return invocation.struct_size >= minimum && invocation.abi_major == OCUDU_DAPP_ABI_MAJOR_V1 &&
         invocation.worker_id == worker_id;
}

bool valid_input_tensor(const ocudu_dapp_tensor_view_v1& tensor) noexcept
{
  return validate_tensor_view(tensor) == validation_error::none && (tensor.access & OCUDU_DAPP_ACCESS_READ_V1) != 0;
}

uint32_t modulation_bit(ocudu_dapp_modulation_v1 modulation) noexcept
{
  switch (modulation) {
    case OCUDU_DAPP_MODULATION_QPSK_V1:
      return OCUDU_DAPP_PUSCH_MODULATION_QPSK_V1;
    case OCUDU_DAPP_MODULATION_QAM16_V1:
      return OCUDU_DAPP_PUSCH_MODULATION_QAM16_V1;
    case OCUDU_DAPP_MODULATION_QAM64_V1:
      return OCUDU_DAPP_PUSCH_MODULATION_QAM64_V1;
    case OCUDU_DAPP_MODULATION_QAM256_V1:
      return OCUDU_DAPP_PUSCH_MODULATION_QAM256_V1;
    default:
      return 0;
  }
}

bool read_pusch_metadata(const ocudu_dapp_typed_metadata_v1&  wrapper,
                         const ocudu_dapp_pusch_metadata_v1*& metadata) noexcept
{
  constexpr uint32_t wrapper_minimum  = offsetof(ocudu_dapp_typed_metadata_v1, metadata) + sizeof(wrapper.metadata);
  constexpr uint32_t metadata_minimum = offsetof(ocudu_dapp_pusch_metadata_v1, reserved);
  if (wrapper.struct_size < wrapper_minimum || wrapper.abi_major != OCUDU_DAPP_ABI_MAJOR_V1 ||
      wrapper.metadata_schema_id != OCUDU_DAPP_METADATA_PUSCH_V1 || wrapper.metadata_schema_version != 1 ||
      wrapper.metadata.data == nullptr || wrapper.metadata.size < metadata_minimum) {
    return false;
  }
  metadata = static_cast<const ocudu_dapp_pusch_metadata_v1*>(wrapper.metadata.data);
  return metadata->struct_size >= metadata_minimum && metadata->abi_major == OCUDU_DAPP_ABI_MAJOR_V1;
}

bool read_dmrs_metadata(const ocudu_dapp_typed_metadata_v1& wrapper,
                        const ocudu_dapp_dmrs_metadata_v1*& metadata) noexcept
{
  constexpr uint32_t wrapper_minimum  = offsetof(ocudu_dapp_typed_metadata_v1, metadata) + sizeof(wrapper.metadata);
  constexpr uint32_t metadata_minimum = offsetof(ocudu_dapp_dmrs_metadata_v1, reserved);
  if (wrapper.struct_size < wrapper_minimum || wrapper.abi_major != OCUDU_DAPP_ABI_MAJOR_V1 ||
      wrapper.metadata_schema_id != OCUDU_DAPP_METADATA_DMRS_V1 || wrapper.metadata_schema_version != 1 ||
      wrapper.metadata.data == nullptr || wrapper.metadata.size < metadata_minimum) {
    return false;
  }
  metadata = static_cast<const ocudu_dapp_dmrs_metadata_v1*>(wrapper.metadata.data);
  return metadata->struct_size >= metadata_minimum && metadata->abi_major == OCUDU_DAPP_ABI_MAJOR_V1;
}

std::optional<class_a_fallback_reason>
classify_unsupported_pusch(uint32_t                            interface_id,
                           const ocudu_dapp_pusch_profile_v1&  profile,
                           const ocudu_dapp_typed_metadata_v1& pusch_wrapper,
                           const ocudu_dapp_typed_metadata_v1& dmrs_wrapper) noexcept
{
  constexpr uint32_t known_pusch_flags = OCUDU_DAPP_PUSCH_TRANSFORM_PRECODING_V1 |
                                         OCUDU_DAPP_PUSCH_DATA_RE_SELECTION_COMPLETE_V1 |
                                         OCUDU_DAPP_PUSCH_DC_OVERLAP_V1;
  constexpr uint32_t required_dmrs_flags =
      OCUDU_DAPP_DMRS_REFERENCE_SYMBOLS_LAYER_COMPLETE_V1 | OCUDU_DAPP_DMRS_COORDINATES_SHARED_ACROSS_LAYERS_V1;
  const ocudu_dapp_pusch_metadata_v1* pusch = nullptr;
  const ocudu_dapp_dmrs_metadata_v1*  dmrs  = nullptr;
  if (!read_pusch_metadata(pusch_wrapper, pusch) || !read_dmrs_metadata(dmrs_wrapper, dmrs) ||
      (pusch->flags & ~known_pusch_flags) != 0 || (dmrs->flags & ~required_dmrs_flags) != 0 ||
      (dmrs->flags & required_dmrs_flags) != required_dmrs_flags ||
      ((interface_id == OCUDU_DAPP_INTERFACE_EQUALIZER_V1 || interface_id == OCUDU_DAPP_INTERFACE_RECEIVER_V1) &&
       ((pusch->flags & OCUDU_DAPP_PUSCH_DATA_RE_SELECTION_COMPLETE_V1) == 0 || pusch->nof_data_re == 0))) {
    return class_a_fallback_reason::unsupported_metadata;
  }
  if (pusch->nof_rx_ports == 0 || pusch->nof_rx_ports > profile.max_nof_rx_ports) {
    return class_a_fallback_reason::unsupported_rx_ports;
  }
  if (pusch->nof_layers == 0 || pusch->nof_layers > profile.max_nof_layers) {
    return class_a_fallback_reason::unsupported_layers;
  }
  if (pusch->nof_symbols == 0 || pusch->start_symbol >= 14 || pusch->nof_symbols > 14 - pusch->start_symbol ||
      pusch->nof_subcarriers == 0 ||
      static_cast<uint64_t>(pusch->start_subcarrier) + pusch->nof_subcarriers >
          static_cast<uint64_t>(profile.max_nof_prb) * 12U) {
    return class_a_fallback_reason::unsupported_allocation;
  }
  if ((pusch->flags & OCUDU_DAPP_PUSCH_TRANSFORM_PRECODING_V1) != 0 &&
      (profile.flags & OCUDU_DAPP_PUSCH_PROFILE_TRANSFORM_PRECODING_V1) == 0) {
    return class_a_fallback_reason::unsupported_transform_precoding;
  }
  if ((pusch->flags & OCUDU_DAPP_PUSCH_DC_OVERLAP_V1) != 0 &&
      (profile.flags & OCUDU_DAPP_PUSCH_PROFILE_DC_OVERLAP_V1) == 0) {
    return class_a_fallback_reason::unsupported_dc_overlap;
  }
  const uint32_t modulation = modulation_bit(pusch->modulation);
  if ((interface_id != OCUDU_DAPP_INTERFACE_CHANNEL_ESTIMATOR_V1 && modulation == 0) ||
      (modulation != 0 && (profile.modulation_mask & modulation) == 0)) {
    return class_a_fallback_reason::unsupported_modulation;
  }
  const uint32_t dmrs_bit = dmrs->dmrs_type == 1   ? OCUDU_DAPP_PUSCH_DMRS_TYPE1_V1
                            : dmrs->dmrs_type == 2 ? OCUDU_DAPP_PUSCH_DMRS_TYPE2_V1
                                                   : 0;
  if (dmrs_bit == 0 || (profile.dmrs_type_mask & dmrs_bit) == 0) {
    return class_a_fallback_reason::unsupported_dmrs;
  }
  return std::nullopt;
}

class_a_fallback_reason status_reason(ocudu_dapp_status_v1 status) noexcept
{
  switch (status) {
    case OCUDU_DAPP_BYPASS_V1:
      return class_a_fallback_reason::module_bypass;
    case OCUDU_DAPP_UNSUPPORTED_V1:
      return class_a_fallback_reason::module_unsupported;
    case OCUDU_DAPP_INVALID_INPUT_V1:
      return class_a_fallback_reason::invalid_input;
    case OCUDU_DAPP_INVALID_OUTPUT_V1:
      return class_a_fallback_reason::invalid_output;
    case OCUDU_DAPP_DEADLINE_V1:
      return class_a_fallback_reason::deadline;
    case OCUDU_DAPP_EXCEPTION_V1:
      return class_a_fallback_reason::exception;
    case OCUDU_DAPP_DEVICE_ERROR_V1:
      return class_a_fallback_reason::device;
    case OCUDU_DAPP_RESOURCE_V1:
      return class_a_fallback_reason::resource;
    default:
      return class_a_fallback_reason::returned_error;
  }
}

bool valid_radio_time(const ocudu_dapp_radio_time_v1& time) noexcept
{
  return time.numerology <= 4 && time.sfn < 1024 && time.slot < (10U << time.numerology) && time.reserved == 0;
}

bool valid_ul_resource_plan(const ocudu_dapp_ul_resource_plan_v1& plan) noexcept
{
  constexpr uint32_t plan_minimum    = offsetof(ocudu_dapp_ul_resource_plan_v1, reserved);
  constexpr uint32_t context_minimum = offsetof(ocudu_dapp_context_header_v1, reserved);
  if (plan.struct_size < plan_minimum || plan.abi_major != OCUDU_DAPP_ABI_MAJOR_V1 ||
      plan.context.struct_size < context_minimum || plan.context.schema_id != OCUDU_DAPP_CONTEXT_UL_RESOURCE_PLAN_V1 ||
      plan.context.schema_version != 1 ||
      (plan.context.producer_instance_id[0] == 0 && plan.context.producer_instance_id[1] == 0) ||
      plan.context.source_sequence != plan.plan_generation || plan.context.confidence_q16 > (1U << 16U) ||
      !valid_radio_time(plan.context.created) || !valid_radio_time(plan.context.valid_from) ||
      !valid_radio_time(plan.context.valid_until) || !valid_radio_time(plan.planned_time) ||
      plan.plan_generation == 0 || plan.planned_time.sfn != plan.context.valid_from.sfn ||
      plan.planned_time.slot != plan.context.valid_from.slot ||
      plan.planned_time.numerology != plan.context.valid_from.numerology ||
      plan.nof_spans > OCUDU_DAPP_UL_RESOURCE_PLAN_MAX_SPANS_V1 || (plan.nof_spans != 0 && plan.spans == nullptr) ||
      plan.cell_nof_prbs == 0 || plan.cell_nof_prbs > OCUDU_DAPP_SPECTRUM_MAX_PRBS_V1 || plan.reserved0 != 0 ||
      (plan.flags & ~OCUDU_DAPP_UL_RESOURCE_PLAN_COMPLETE_V1) != 0 || plan.optional_dense_mask.struct_size != 0 ||
      !std::all_of(std::begin(plan.reserved), std::end(plan.reserved), [](uint64_t value) { return value == 0; })) {
    return false;
  }
  for (uint32_t index = 0; index != plan.nof_spans; ++index) {
    const auto& span = plan.spans[index];
    if (span.struct_size < offsetof(ocudu_dapp_ul_resource_span_v1, reserved) || span.symbol_count == 0 ||
        span.symbol_start + span.symbol_count > 14 || span.nof_prbs == 0 ||
        static_cast<uint32_t>(span.first_prb) + span.nof_prbs > plan.cell_nof_prbs ||
        span.use_category > OCUDU_DAPP_UL_USE_RESERVED_QUIET_V1 || span.reserved0 != 0 || span.reserved1 != 0 ||
        !std::all_of(std::begin(span.reserved), std::end(span.reserved), [](uint64_t value) { return value == 0; })) {
      return false;
    }
  }
  return true;
}

bool valid_output_tensor(const ocudu_dapp_tensor_view_v1& tensor) noexcept
{
  return validate_tensor_view(tensor) == validation_error::none && (tensor.access & OCUDU_DAPP_ACCESS_WRITE_V1) != 0;
}

bool spectrum_plan_generation(const ocudu_dapp_spectrum_input_v1& input, uint64_t& generation) noexcept
{
  constexpr uint32_t wrapper_minimum =
      offsetof(ocudu_dapp_typed_metadata_v1, metadata) + sizeof(ocudu_dapp_blob_view_v1);
  constexpr uint32_t metadata_minimum = offsetof(ocudu_dapp_spectrum_slot_metadata_v1, reserved);
  const auto&        wrapper          = input.slot_metadata;
  if (wrapper.struct_size < wrapper_minimum || wrapper.abi_major != OCUDU_DAPP_ABI_MAJOR_V1 ||
      wrapper.metadata_schema_id != OCUDU_DAPP_METADATA_SPECTRUM_SLOT_V1 || wrapper.metadata_schema_version != 1 ||
      wrapper.metadata.data == nullptr || wrapper.metadata.size < metadata_minimum) {
    return false;
  }
  const auto& metadata = *static_cast<const ocudu_dapp_spectrum_slot_metadata_v1*>(wrapper.metadata.data);
  if (metadata.struct_size < metadata_minimum || metadata.abi_major != OCUDU_DAPP_ABI_MAJOR_V1 ||
      metadata.rx_ports == 0 || metadata.symbols == 0 || metadata.symbols > 14 || metadata.subcarriers == 0 ||
      (metadata.flags & ~OCUDU_DAPP_SPECTRUM_FLAG_EXPECTED_UL_USE_COMPLETE_V1) != 0 ||
      input.uplink_resource_grid.rank != 3 || input.uplink_resource_grid.dimensions[0] != metadata.rx_ports ||
      input.uplink_resource_grid.dimensions[1] != metadata.symbols ||
      input.uplink_resource_grid.dimensions[2] != metadata.subcarriers || input.source_sequence == 0 ||
      input.source_sequence != input.invocation.input_generation) {
    return false;
  }
  const bool  present = (metadata.flags & OCUDU_DAPP_SPECTRUM_FLAG_EXPECTED_UL_USE_COMPLETE_V1) != 0;
  const auto& plan    = input.scheduled_re_mask;
  if (!present) {
    generation = 0;
    return metadata.source_plan_generation == 0 && plan.struct_size == 0 && plan.native_handle == 0 &&
           plan.byte_length == 0;
  }
  const uint64_t nof_prbs  = metadata.subcarriers / 12;
  const bool     host_plan = plan.domain == OCUDU_DAPP_MEM_HOST_V1 || plan.domain == OCUDU_DAPP_MEM_HOST_PINNED_V1 ||
                         plan.domain == OCUDU_DAPP_MEM_MANAGED_COHERENT_V1;
  if (metadata.source_plan_generation == 0 || metadata.subcarriers % 12 != 0 || nof_prbs == 0 ||
      nof_prbs > OCUDU_DAPP_SPECTRUM_MAX_PRBS_V1 || !valid_input_tensor(plan) || !host_plan ||
      plan.backend_id != OCUDU_DAPP_BACKEND_HOST_V1 || (plan.access & OCUDU_DAPP_ACCESS_HOST_VISIBLE_V1) == 0 ||
      plan.native_handle == 0 || plan.element_type != OCUDU_DAPP_ELEMENT_U8_V1 || plan.rank != 2 ||
      plan.dimensions[0] != metadata.symbols || plan.dimensions[1] != nof_prbs || plan.byte_strides[0] != nof_prbs ||
      plan.byte_strides[1] != sizeof(uint8_t) ||
      plan.byte_length != static_cast<uint64_t>(metadata.symbols) * nof_prbs ||
      plan.layout_id != OCUDU_DAPP_LAYOUT_EXPECTED_UL_USE_SYMBOL_PRB_U8_V1 || plan.layout_version != 1 ||
      plan.allocation_generation != metadata.source_plan_generation ||
      plan.byte_offset > std::numeric_limits<uintptr_t>::max() - plan.native_handle) {
    return false;
  }
  const auto* values = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(plan.native_handle + plan.byte_offset));
  if (!std::all_of(values, values + plan.byte_length, [](uint8_t value) {
        return value <= OCUDU_DAPP_UL_USE_RESERVED_QUIET_V1;
      })) {
    return false;
  }
  generation = metadata.source_plan_generation;
  return true;
}

bool srs_source_metadata(const ocudu_dapp_srs_isac_input_v1& input, uint32_t& resource_id, uint32_t& config_id) noexcept
{
  constexpr uint32_t wrapper_minimum =
      offsetof(ocudu_dapp_typed_metadata_v1, metadata) + sizeof(ocudu_dapp_blob_view_v1);
  constexpr uint32_t metadata_minimum = offsetof(ocudu_dapp_srs_isac_metadata_v1, reserved);
  const auto&        wrapper          = input.srs_metadata;
  if (wrapper.struct_size < wrapper_minimum || wrapper.abi_major != OCUDU_DAPP_ABI_MAJOR_V1 ||
      wrapper.metadata_schema_id != OCUDU_DAPP_METADATA_SRS_ISAC_V1 || wrapper.metadata_schema_version != 1 ||
      wrapper.metadata.data == nullptr || wrapper.metadata.size < metadata_minimum) {
    return false;
  }
  const auto& metadata = *static_cast<const ocudu_dapp_srs_isac_metadata_v1*>(wrapper.metadata.data);
  if (metadata.struct_size < metadata_minimum || metadata.abi_major != OCUDU_DAPP_ABI_MAJOR_V1 ||
      metadata.rx_ports == 0 || metadata.tx_ports == 0 || metadata.frequency_points == 0 ||
      (metadata.comb_size != 2 && metadata.comb_size != 4 && metadata.comb_size != 8) ||
      metadata.comb_offset >= metadata.comb_size || metadata.subcarrier_spacing_hz == 0 ||
      !std::isfinite(metadata.noise_variance) || metadata.noise_variance < 0.0F || !std::isfinite(metadata.rsrp_db) ||
      !std::isfinite(metadata.epre_db) || input.channel_estimates.rank != 3 ||
      input.channel_estimates.dimensions[0] != metadata.rx_ports ||
      input.channel_estimates.dimensions[1] != metadata.tx_ports ||
      input.channel_estimates.dimensions[2] != metadata.frequency_points || input.source_sequence == 0 ||
      input.source_sequence != input.invocation.input_generation || metadata.reserved0 != 0 ||
      !std::all_of(
          std::begin(metadata.reserved), std::end(metadata.reserved), [](uint64_t value) { return value == 0; })) {
    return false;
  }
  resource_id = metadata.srs_resource_id;
  config_id   = metadata.srs_config_id;
  return true;
}

bool valid_scheduler_interference(const ocudu_dapp_scheduler_input_v1& input) noexcept
{
  constexpr uint32_t context_minimum = offsetof(ocudu_dapp_context_header_v1, reserved);
  const auto&        context         = input.interference_context;
  const auto&        mask            = input.optional_interference_mask;
  return context.struct_size >= context_minimum &&
         context.schema_id == OCUDU_DAPP_SCHEDULER_CONTEXT_INTERFERENCE_MAP_V1 && context.schema_version == 1 &&
         context.cell_id == input.invocation.cell_id && context.confidence_q16 <= (1U << 16U) &&
         valid_input_tensor(mask) && mask.domain == OCUDU_DAPP_MEM_HOST_V1 &&
         mask.backend_id == OCUDU_DAPP_BACKEND_HOST_V1 && mask.element_type == OCUDU_DAPP_ELEMENT_U8_V1 &&
         mask.rank == 1 && mask.dimensions[0] == input.cell_nof_prbs &&
         mask.layout_id == OCUDU_DAPP_SCHEDULER_LAYOUT_PRB_INTERFERENCE_U8_V1 &&
         (mask.access & OCUDU_DAPP_ACCESS_HOST_VISIBLE_V1) != 0 && mask.native_handle != 0;
}

bool valid_scheduler_free_vrb(const ocudu_dapp_scheduler_input_v1& input) noexcept
{
  constexpr uint32_t context_minimum = offsetof(ocudu_dapp_context_header_v1, reserved);
  const auto&        context         = input.free_vrb_context;
  const auto&        mask            = input.free_vrb_mask;
  return context.struct_size >= context_minimum &&
         context.schema_id == OCUDU_DAPP_SCHEDULER_CONTEXT_FREE_VRB_MASK_V1 && context.schema_version == 1 &&
         context.cell_id == input.invocation.cell_id && valid_input_tensor(mask) &&
         mask.domain == OCUDU_DAPP_MEM_HOST_V1 && mask.backend_id == OCUDU_DAPP_BACKEND_HOST_V1 &&
         mask.element_type == OCUDU_DAPP_ELEMENT_U8_V1 && mask.rank == 1 &&
         mask.dimensions[0] == input.cell_nof_prbs &&
         mask.layout_id == OCUDU_DAPP_SCHEDULER_LAYOUT_PRB_FREE_U8_V1 &&
         (mask.access & OCUDU_DAPP_ACCESS_HOST_VISIBLE_V1) != 0 && mask.native_handle != 0;
}

uint64_t steady_now_ns() noexcept
{
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

const ocudu_dapp_worker_api_v1* get_worker_api(const ocudu_dapp_interface_descriptor_v1& descriptor) noexcept
{
  switch (descriptor.interface_id) {
    case OCUDU_DAPP_INTERFACE_CHANNEL_ESTIMATOR_V1:
      return &static_cast<const ocudu_dapp_channel_estimator_interface_v1*>(descriptor.interface_vtable)->worker;
    case OCUDU_DAPP_INTERFACE_EQUALIZER_V1:
      return &static_cast<const ocudu_dapp_equalizer_interface_v1*>(descriptor.interface_vtable)->worker;
    case OCUDU_DAPP_INTERFACE_RECEIVER_V1:
      return &static_cast<const ocudu_dapp_receiver_interface_v1*>(descriptor.interface_vtable)->worker;
    case OCUDU_DAPP_INTERFACE_SCHEDULER_V1:
      return &static_cast<const ocudu_dapp_scheduler_interface_v1*>(descriptor.interface_vtable)->worker;
    case OCUDU_DAPP_INTERFACE_SPECTRUM_V1:
      return &static_cast<const ocudu_dapp_spectrum_interface_v1*>(descriptor.interface_vtable)->worker;
    case OCUDU_DAPP_INTERFACE_SRS_ISAC_V1:
      return &static_cast<const ocudu_dapp_srs_isac_interface_v1*>(descriptor.interface_vtable)->worker;
    default:
      return nullptr;
  }
}

bool valid_interface_callback(const ocudu_dapp_interface_descriptor_v1& descriptor) noexcept
{
  switch (descriptor.interface_id) {
    case OCUDU_DAPP_INTERFACE_CHANNEL_ESTIMATOR_V1:
      return static_cast<const ocudu_dapp_channel_estimator_interface_v1*>(descriptor.interface_vtable)->invoke !=
             nullptr;
    case OCUDU_DAPP_INTERFACE_EQUALIZER_V1:
      return static_cast<const ocudu_dapp_equalizer_interface_v1*>(descriptor.interface_vtable)->invoke != nullptr;
    case OCUDU_DAPP_INTERFACE_RECEIVER_V1:
      return static_cast<const ocudu_dapp_receiver_interface_v1*>(descriptor.interface_vtable)->invoke != nullptr;
    case OCUDU_DAPP_INTERFACE_SCHEDULER_V1:
      return static_cast<const ocudu_dapp_scheduler_interface_v1*>(descriptor.interface_vtable)->invoke != nullptr;
    case OCUDU_DAPP_INTERFACE_SPECTRUM_V1:
      return static_cast<const ocudu_dapp_spectrum_interface_v1*>(descriptor.interface_vtable)->publish != nullptr;
    case OCUDU_DAPP_INTERFACE_SRS_ISAC_V1:
      return static_cast<const ocudu_dapp_srs_isac_interface_v1*>(descriptor.interface_vtable)->publish != nullptr;
    default:
      return false;
  }
}


} // namespace ocudu::dapp::native_worker_detail
