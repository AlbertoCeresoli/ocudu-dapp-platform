// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "ocudu/adt/span.h"
#include "ocudu/dapp/e3/e3ap_protocol.h"
#include "ocudu/dapp/e3/result.h"

namespace ocudu::dapp::e3 {

/// APER codec for the upstream libe3 E3AP 1.0.0 module (Northeastern
/// University, schemas/e3/interop/libe3/e3ap-1.0.0.asn1), generated verbatim
/// from that grammar with a type prefix so it links beside E3AP-OCUDU-V1.
///
/// The codec maps libe3 PDUs onto the same \ref e3ap_pdu values the native
/// codec produces, so the E3AP agent runs one procedure implementation for
/// both dialects. Encoding fails, with a diagnostic, for values the libe3
/// module cannot carry: identifiers above 100, message identifiers above
/// 1000, payloads above 32 KiB, a controlResponse, or any OCUDU-only field
/// that is not optional in spirit (fragments other than the whole payload).
/// OCUDU-only optional fields (causes, detail text, subscription data,
/// by-reference descriptors, fragment bookkeeping) are dropped on encode and
/// left absent on decode.
class e3ap_libe3_codec
{
public:
  static constexpr uint32_t    maximum_identifier    = 100;
  static constexpr uint32_t    maximum_message_id    = 1000;
  static constexpr std::size_t maximum_payload_bytes = 32768;
  static constexpr std::size_t maximum_name_bytes    = 64;
  static constexpr std::size_t maximum_version_bytes = 11;
  /// Largest single E3-PDU accepted on the wire.
  static constexpr std::size_t maximum_pdu_bytes = 64 * 1024;

  static codec_result<std::vector<uint8_t>> encode(const e3ap_pdu& pdu) noexcept;
  static codec_result<e3ap_pdu>             decode(ocudu::span<const uint8_t> bytes) noexcept;

  /// True when \c message can be expressed in the libe3 module as-is.
  static bool representable(const e3ap_message& message, std::string* detail = nullptr) noexcept;
};

} // namespace ocudu::dapp::e3
