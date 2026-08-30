// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "management_protocol.h"
#include "result.h"
#include "ocudu/adt/span.h"

namespace ocudu::dapp::e3 {

/// Generated ASN.1 aligned-PER binding for E3SM-OCUDU-DAPP-MGMT-V1.
/// The codec owns all decoded storage and returns ordinary C++ values.
class aper_codec
{
public:
  static constexpr std::size_t maximum_message_size = 2 * 1024 * 1024;

  static codec_result<std::vector<uint8_t>> encode(const management_request& request) noexcept;
  static codec_result<std::vector<uint8_t>> encode(const management_response& response) noexcept;
  static codec_result<management_request>   decode_request(ocudu::span<const uint8_t> bytes) noexcept;
  static codec_result<management_response>  decode_response(ocudu::span<const uint8_t> bytes) noexcept;
};

} // namespace ocudu::dapp::e3
