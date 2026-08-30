// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "ocudu/dapp/abi/v1/memory.h"
#include "ocudu/dapp/abi/v1/module.h"
#include "ocudu/dapp/use_cases/v1/common.h"

namespace ocudu::dapp {

enum class validation_error {
  none,
  null_pointer,
  structure_too_small,
  incompatible_abi,
  invalid_enumeration,
  invalid_rank,
  invalid_dimensions,
  invalid_strides,
  arithmetic_overflow,
  invalid_byte_range,
  missing_generation,
  missing_callback,
  duplicate_interface,
  forbidden_placement,
  forbidden_threading
};

validation_error validate_tensor_view(const ocudu_dapp_tensor_view_v1& view) noexcept;
/// Compares caller-owned storage identity while allowing the producer to publish a completion token.
bool same_tensor_storage(const ocudu_dapp_tensor_view_v1& expected, const ocudu_dapp_tensor_view_v1& actual) noexcept;
/// Returns true only for the all-zero representation reserved for an absent v1 typed-metadata value.
bool             empty_typed_metadata(const ocudu_dapp_typed_metadata_v1& metadata) noexcept;
validation_error validate_module_descriptor(const ocudu_dapp_module_v1* module) noexcept;
validation_error validate_interface_placement(ocudu_dapp_class_v1     dapp_class,
                                              ocudu_dapp_placement_v1 placement,
                                              ocudu_dapp_threading_v1 threading) noexcept;

const char* to_string(validation_error error) noexcept;

} // namespace ocudu::dapp
