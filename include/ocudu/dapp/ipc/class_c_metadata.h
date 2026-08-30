// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "ocudu/adt/span.h"
#include <cstdint>

namespace ocudu::dapp::ipc {

inline constexpr const char* class_c_schema_id = "org.ocudu.dapp.class-c.publication";

enum class metadata_error : uint8_t {
  none,
  malformed_buffer,
  unsupported_schema,
  invalid_context,
  metadata_mismatch,
  tensor_required,
  tensor_forbidden,
  invalid_tensor,
  invalid_metadata
};

metadata_error validate_class_c_metadata(ocudu::span<const uint8_t> buffer) noexcept;
const char*    to_string(metadata_error error) noexcept;

} // namespace ocudu::dapp::ipc
