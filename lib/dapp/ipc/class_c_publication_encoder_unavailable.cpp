// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/ipc/class_c_publication_encoder.h"
#include <utility>

using namespace ocudu::dapp::ipc;

struct class_c_publication_encoder::impl {};

class_c_encoder_create_result class_c_publication_encoder::create(class_c_encoder_config) noexcept
{
  return {nullptr, "Class-C FlatBuffers support is unavailable"};
}

class_c_publication_encoder::class_c_publication_encoder(std::unique_ptr<impl> state_) : state(std::move(state_)) {}
class_c_publication_encoder::~class_c_publication_encoder()                                                 = default;
class_c_publication_encoder::class_c_publication_encoder(class_c_publication_encoder&&) noexcept            = default;
class_c_publication_encoder& class_c_publication_encoder::operator=(class_c_publication_encoder&&) noexcept = default;

class_c_encode_result
class_c_publication_encoder::encode(const ocudu_dapp_spectrum_input_v1&, uint64_t, uint64_t) noexcept
{
  return {class_c_encode_error::internal_error, {}};
}

class_c_encode_result
class_c_publication_encoder::encode(const ocudu_dapp_srs_isac_input_v1&, uint64_t, uint64_t) noexcept
{
  return {class_c_encode_error::internal_error, {}};
}

const char* ocudu::dapp::ipc::to_string(class_c_encode_error error) noexcept
{
  static constexpr const char* names[] = {
      "none", "invalid config", "invalid input", "unsupported tensor", "metadata too large", "internal error"};
  const auto index = static_cast<unsigned>(error);
  return index < sizeof(names) / sizeof(names[0]) ? names[index] : "unknown Class-C encode error";
}
