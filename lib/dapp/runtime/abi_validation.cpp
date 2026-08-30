// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/runtime/abi_validation.h"
#include "ocudu/dapp/use_cases/v1/channel_estimator.h"
#include "ocudu/dapp/use_cases/v1/equalizer.h"
#include "ocudu/dapp/use_cases/v1/pusch_metadata.h"
#include "ocudu/dapp/use_cases/v1/receiver.h"
#include "ocudu/dapp/use_cases/v1/scheduler.h"
#include "ocudu/dapp/use_cases/v1/spectrum.h"
#include "ocudu/dapp/use_cases/v1/srs_isac.h"
#include <cstddef>
#include <limits>

using namespace ocudu::dapp;

namespace {

bool valid_abi(uint32_t size, uint16_t major, uint32_t minimum) noexcept
{
  return size >= minimum && major == OCUDU_DAPP_ABI_MAJOR_V1;
}

bool valid_pusch_profile(const ocudu_dapp_pusch_profile_v1& profile) noexcept
{
  constexpr uint32_t minimum     = offsetof(ocudu_dapp_pusch_profile_v1, reserved);
  constexpr uint32_t modulations = OCUDU_DAPP_PUSCH_MODULATION_QPSK_V1 | OCUDU_DAPP_PUSCH_MODULATION_QAM16_V1 |
                                   OCUDU_DAPP_PUSCH_MODULATION_QAM64_V1 | OCUDU_DAPP_PUSCH_MODULATION_QAM256_V1;
  constexpr uint32_t dmrs = OCUDU_DAPP_PUSCH_DMRS_TYPE1_V1 | OCUDU_DAPP_PUSCH_DMRS_TYPE2_V1;
  return valid_abi(profile.struct_size, profile.abi_major, minimum) &&
         profile.profile_id == OCUDU_DAPP_PUSCH_PROFILE_V1 && profile.max_nof_prb != 0 && profile.max_nof_prb <= 275 &&
         profile.max_nof_rx_ports != 0 && profile.max_nof_rx_ports <= 64 && profile.max_nof_layers != 0 &&
         profile.max_nof_layers <= 4 && profile.modulation_mask != 0 && (profile.modulation_mask & ~modulations) == 0 &&
         profile.dmrs_type_mask != 0 && (profile.dmrs_type_mask & ~dmrs) == 0 &&
         (profile.flags &
          ~(OCUDU_DAPP_PUSCH_PROFILE_TRANSFORM_PRECODING_V1 | OCUDU_DAPP_PUSCH_PROFILE_DC_OVERLAP_V1)) == 0;
}

validation_error validate_worker_api(const ocudu_dapp_worker_api_v1& worker) noexcept
{
  constexpr uint32_t minimum = offsetof(ocudu_dapp_worker_api_v1, reserved);
  if (!valid_abi(worker.struct_size, worker.abi_major, minimum)) {
    return worker.struct_size < minimum ? validation_error::structure_too_small : validation_error::incompatible_abi;
  }
  return worker.create_worker_context != nullptr && worker.destroy_worker_context != nullptr
             ? validation_error::none
             : validation_error::missing_callback;
}

template <typename Interface>
validation_error validate_interface_api(const Interface& api, uint16_t interface_major, bool callback_present) noexcept
{
  if (interface_major != 1) {
    return validation_error::incompatible_abi;
  }
  const auto worker = validate_worker_api(api.worker);
  if (worker != validation_error::none) {
    return worker;
  }
  return callback_present ? validation_error::none : validation_error::missing_callback;
}

validation_error validate_known_interface_api(const ocudu_dapp_interface_descriptor_v1& descriptor) noexcept
{
  switch (descriptor.interface_id) {
    case OCUDU_DAPP_INTERFACE_SPECTRUM_V1: {
      const auto& api = *static_cast<const ocudu_dapp_spectrum_interface_v1*>(descriptor.interface_vtable);
      return validate_interface_api(api, descriptor.interface_major, api.publish != nullptr);
    }
    case OCUDU_DAPP_INTERFACE_SRS_ISAC_V1: {
      const auto& api = *static_cast<const ocudu_dapp_srs_isac_interface_v1*>(descriptor.interface_vtable);
      return validate_interface_api(api, descriptor.interface_major, api.publish != nullptr);
    }
    case OCUDU_DAPP_INTERFACE_CHANNEL_ESTIMATOR_V1: {
      const auto& api = *static_cast<const ocudu_dapp_channel_estimator_interface_v1*>(descriptor.interface_vtable);
      return validate_interface_api(api, descriptor.interface_major, api.invoke != nullptr);
    }
    case OCUDU_DAPP_INTERFACE_EQUALIZER_V1: {
      const auto& api = *static_cast<const ocudu_dapp_equalizer_interface_v1*>(descriptor.interface_vtable);
      return validate_interface_api(api, descriptor.interface_major, api.invoke != nullptr);
    }
    case OCUDU_DAPP_INTERFACE_RECEIVER_V1: {
      const auto& api = *static_cast<const ocudu_dapp_receiver_interface_v1*>(descriptor.interface_vtable);
      return validate_interface_api(api, descriptor.interface_major, api.invoke != nullptr);
    }
    case OCUDU_DAPP_INTERFACE_SCHEDULER_V1: {
      const auto& api = *static_cast<const ocudu_dapp_scheduler_interface_v1*>(descriptor.interface_vtable);
      return validate_interface_api(api, descriptor.interface_major, api.invoke != nullptr);
    }
    default:
      return validation_error::none;
  }
}

uint32_t element_size(uint32_t type) noexcept
{
  switch (type) {
    case OCUDU_DAPP_ELEMENT_I8_V1:
    case OCUDU_DAPP_ELEMENT_U8_V1:
      return 1;
    case OCUDU_DAPP_ELEMENT_I16_V1:
    case OCUDU_DAPP_ELEMENT_U16_V1:
    case OCUDU_DAPP_ELEMENT_F16_V1:
    case OCUDU_DAPP_ELEMENT_BF16_V1:
      return 2;
    case OCUDU_DAPP_ELEMENT_I32_V1:
    case OCUDU_DAPP_ELEMENT_U32_V1:
    case OCUDU_DAPP_ELEMENT_F32_V1:
    case OCUDU_DAPP_ELEMENT_CF16_V1:
    case OCUDU_DAPP_ELEMENT_CBF16_V1:
      return 4;
    case OCUDU_DAPP_ELEMENT_CF32_V1:
      return 8;
    default:
      return 0;
  }
}

bool add_overflows(uint64_t lhs, uint64_t rhs) noexcept
{
  return rhs > std::numeric_limits<uint64_t>::max() - lhs;
}

} // namespace

validation_error ocudu::dapp::validate_tensor_view(const ocudu_dapp_tensor_view_v1& view) noexcept
{
  constexpr uint32_t minimum = offsetof(ocudu_dapp_tensor_view_v1, reserved);
  if (view.struct_size < minimum) {
    return validation_error::structure_too_small;
  }
  if (view.abi_major != OCUDU_DAPP_ABI_MAJOR_V1) {
    return validation_error::incompatible_abi;
  }
  if (view.domain < OCUDU_DAPP_MEM_HOST_V1 || view.domain > OCUDU_DAPP_MEM_ACCELERATOR_IPC_EXPORT_V1 ||
      element_size(view.element_type) == 0 || view.access == 0) {
    return validation_error::invalid_enumeration;
  }
  if (view.rank == 0 || view.rank > OCUDU_DAPP_MAX_TENSOR_RANK_V1) {
    return validation_error::invalid_rank;
  }
  if (view.allocation_id == 0 || view.allocation_generation == 0 || view.lease_token == 0) {
    return validation_error::missing_generation;
  }
  if (view.byte_length == 0 || add_overflows(view.byte_offset, view.byte_length)) {
    return validation_error::invalid_byte_range;
  }

  uint64_t maximum_offset = 0;
  for (uint32_t i = 0; i != view.rank; ++i) {
    if (view.dimensions[i] == 0) {
      return validation_error::invalid_dimensions;
    }
    if (view.byte_strides[i] == 0) {
      return validation_error::invalid_strides;
    }
    const uint64_t count = view.dimensions[i] - 1;
    if (count != 0 && view.byte_strides[i] > std::numeric_limits<uint64_t>::max() / count) {
      return validation_error::arithmetic_overflow;
    }
    const uint64_t extent = count * view.byte_strides[i];
    if (add_overflows(maximum_offset, extent)) {
      return validation_error::arithmetic_overflow;
    }
    maximum_offset += extent;
  }
  const uint32_t item_size = element_size(view.element_type);
  if (add_overflows(maximum_offset, item_size) || maximum_offset + item_size > view.byte_length) {
    return validation_error::invalid_byte_range;
  }
  return validation_error::none;
}

bool ocudu::dapp::same_tensor_storage(const ocudu_dapp_tensor_view_v1& expected,
                                      const ocudu_dapp_tensor_view_v1& actual) noexcept
{
  if (expected.struct_size != actual.struct_size || expected.abi_major != actual.abi_major ||
      expected.abi_minor != actual.abi_minor || expected.domain != actual.domain ||
      expected.backend_id != actual.backend_id || expected.element_type != actual.element_type ||
      expected.rank != actual.rank || expected.access != actual.access ||
      expected.device_ordinal != actual.device_ordinal || expected.allocation_id != actual.allocation_id ||
      expected.allocation_generation != actual.allocation_generation ||
      expected.native_handle != actual.native_handle || expected.byte_offset != actual.byte_offset ||
      expected.byte_length != actual.byte_length || expected.layout_id != actual.layout_id ||
      expected.layout_version != actual.layout_version || expected.ready_token != actual.ready_token ||
      expected.lease_token != actual.lease_token) {
    return false;
  }
  for (unsigned dimension = 0; dimension != OCUDU_DAPP_MAX_TENSOR_RANK_V1; ++dimension) {
    if (expected.dimensions[dimension] != actual.dimensions[dimension] ||
        expected.byte_strides[dimension] != actual.byte_strides[dimension]) {
      return false;
    }
  }
  return true;
}

bool ocudu::dapp::empty_typed_metadata(const ocudu_dapp_typed_metadata_v1& metadata) noexcept
{
  return metadata.struct_size == 0 && metadata.abi_major == 0 && metadata.abi_minor == 0 &&
         metadata.metadata_schema_id == 0 && metadata.metadata_schema_version == 0 &&
         metadata.metadata.data == nullptr && metadata.metadata.size == 0 && metadata.metadata.reserved == 0;
}

validation_error ocudu::dapp::validate_interface_placement(ocudu_dapp_class_v1     dapp_class,
                                                           ocudu_dapp_placement_v1 placement,
                                                           ocudu_dapp_threading_v1 threading) noexcept
{
  if (dapp_class < OCUDU_DAPP_CLASS_A_INLINE_V1 || dapp_class > OCUDU_DAPP_CLASS_C_OPEN_LOOP_V1 ||
      placement < OCUDU_DAPP_PLACEMENT_IN_PROCESS_V1 || placement > OCUDU_DAPP_PLACEMENT_OUT_OF_PROCESS_V1 ||
      threading < OCUDU_DAPP_THREADING_PER_WORKER_V1 || threading > OCUDU_DAPP_THREADING_SERIALIZED_V1) {
    return validation_error::invalid_enumeration;
  }
  if ((dapp_class == OCUDU_DAPP_CLASS_A_INLINE_V1 || dapp_class == OCUDU_DAPP_CLASS_B_DEADLINE_V1) &&
      placement != OCUDU_DAPP_PLACEMENT_IN_PROCESS_V1) {
    return validation_error::forbidden_placement;
  }
  if ((dapp_class == OCUDU_DAPP_CLASS_A_INLINE_V1 || dapp_class == OCUDU_DAPP_CLASS_B_DEADLINE_V1) &&
      threading == OCUDU_DAPP_THREADING_SERIALIZED_V1) {
    return validation_error::forbidden_threading;
  }
  return validation_error::none;
}

validation_error ocudu::dapp::validate_module_descriptor(const ocudu_dapp_module_v1* module) noexcept
{
  if (module == nullptr) {
    return validation_error::null_pointer;
  }
  constexpr uint32_t minimum = offsetof(ocudu_dapp_module_v1, reserved);
  if (!valid_abi(module->struct_size, module->abi_major, minimum)) {
    return module->struct_size < minimum ? validation_error::structure_too_small : validation_error::incompatible_abi;
  }
  if (module->global_name == nullptr || module->global_name[0] == '\0' || module->vendor == nullptr ||
      module->version == nullptr || module->nof_interfaces == 0 || module->interfaces == nullptr) {
    return validation_error::null_pointer;
  }
  if (module->create == nullptr || module->configure == nullptr || module->prepare == nullptr ||
      module->start == nullptr || module->stop == nullptr || module->quiesce == nullptr || module->destroy == nullptr) {
    return validation_error::missing_callback;
  }
  if (module->management != nullptr) {
    constexpr uint32_t management_minimum = offsetof(ocudu_dapp_management_v1, reserved);
    if (module->management->struct_size < management_minimum ||
        module->management->major != OCUDU_DAPP_MANAGEMENT_MAJOR_V1) {
      return module->management->struct_size < management_minimum ? validation_error::structure_too_small
                                                                  : validation_error::incompatible_abi;
    }
    const uint64_t capabilities = module->management->capability_bits;
    if (((capabilities & OCUDU_DAPP_MANAGEMENT_CONFIG_V1) != 0 &&
         (module->management->describe_config == nullptr || module->management->get_config == nullptr ||
          module->management->set_config == nullptr)) ||
        ((capabilities & OCUDU_DAPP_MANAGEMENT_METRICS_V1) != 0 &&
         (module->management->describe_metrics == nullptr || module->management->read_metrics == nullptr)) ||
        ((capabilities & OCUDU_DAPP_MANAGEMENT_MODELS_V1) != 0 && module->management->manage_model == nullptr)) {
      return validation_error::missing_callback;
    }
  }
  for (uint32_t i = 0; i != module->nof_interfaces; ++i) {
    const auto&        interface         = module->interfaces[i];
    constexpr uint32_t interface_minimum = offsetof(ocudu_dapp_interface_descriptor_v1, reserved);
    if (!valid_abi(interface.struct_size, interface.abi_major, interface_minimum)) {
      return interface.struct_size < interface_minimum ? validation_error::structure_too_small
                                                       : validation_error::incompatible_abi;
    }
    if (interface.interface_id == 0 || interface.interface_vtable == nullptr || interface.placement_mask == 0) {
      return validation_error::missing_callback;
    }
    const auto interface_api = validate_known_interface_api(interface);
    if (interface_api != validation_error::none) {
      return interface_api;
    }
    const bool class_a = interface.dapp_class == OCUDU_DAPP_CLASS_A_INLINE_V1;
    if (interface.use_case_profile != nullptr &&
        (!class_a ||
         !valid_pusch_profile(*static_cast<const ocudu_dapp_pusch_profile_v1*>(interface.use_case_profile)))) {
      return validation_error::invalid_dimensions;
    }
    for (uint32_t j = 0; j != i; ++j) {
      if (module->interfaces[j].interface_id == interface.interface_id &&
          module->interfaces[j].interface_major == interface.interface_major) {
        return validation_error::duplicate_interface;
      }
    }
    for (uint32_t bit = 0; bit != 2; ++bit) {
      if ((interface.placement_mask & (1u << bit)) != 0) {
        auto error = validate_interface_placement(static_cast<ocudu_dapp_class_v1>(interface.dapp_class),
                                                  static_cast<ocudu_dapp_placement_v1>(bit + 1),
                                                  static_cast<ocudu_dapp_threading_v1>(interface.threading));
        if (error != validation_error::none) {
          return error;
        }
      }
    }
  }
  return validation_error::none;
}

const char* ocudu::dapp::to_string(validation_error error) noexcept
{
  static constexpr const char* names[] = {"none",
                                          "null pointer",
                                          "structure too small",
                                          "incompatible ABI",
                                          "invalid enumeration",
                                          "invalid rank",
                                          "invalid dimensions",
                                          "invalid strides",
                                          "arithmetic overflow",
                                          "invalid byte range",
                                          "missing generation",
                                          "missing callback",
                                          "duplicate interface",
                                          "forbidden placement",
                                          "forbidden threading"};
  const auto                   index   = static_cast<unsigned>(error);
  return index < sizeof(names) / sizeof(names[0]) ? names[index] : "unknown validation error";
}
