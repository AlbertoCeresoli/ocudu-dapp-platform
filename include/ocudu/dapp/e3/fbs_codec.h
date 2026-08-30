// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "ocudu/adt/span.h"
#include "ocudu/dapp/e3/management_protocol.h"
#include "ocudu/dapp/e3/result.h"

namespace ocudu::dapp::e3 {

/// FlatBuffers binding for the data profile of E3SM-OCUDU-DAPP-MGMT-V1,
/// schema e3-data-profile-v1.fbs (file identifier "E3DP"). This is the
/// compatibility carrier used by the standalone E3 SCTP reference client:
/// messages are size-prefixed FlatBuffers, every decode runs the generated
/// verifier before any field is touched, and only the data-profile operations
/// (dataSubscriptions, dataPoll) are representable. Semantics mirror the APER
/// carrier exactly; queue depth is server-assigned and absent from the wire.
class fbs_codec
{
public:
  static constexpr std::size_t maximum_message_size = 2 * 1024 * 1024;

  static codec_result<std::vector<uint8_t>> encode(const management_request& request) noexcept;
  static codec_result<std::vector<uint8_t>> encode(const management_response& response) noexcept;
  static codec_result<management_request>   decode_request(ocudu::span<const uint8_t> bytes) noexcept;
  static codec_result<management_response>  decode_response(ocudu::span<const uint8_t> bytes) noexcept;
};

} // namespace ocudu::dapp::e3
