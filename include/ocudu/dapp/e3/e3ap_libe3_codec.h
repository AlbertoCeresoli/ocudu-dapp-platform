// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "ocudu/adt/span.h"
#include "ocudu/dapp/e3/e3ap_protocol.h"
#include "ocudu/dapp/e3/result.h"

namespace ocudu::dapp::e3 {

/// The upstream libe3 E3AP 1.0.0 module in APER: \ref e3ap_v2_codec with \ref e3ap_profile::libe3.
/// E3AP-OCUDU-V2's root is libe3's module, so this is the same generated codec with every OCUDU
/// addition omitted; the bytes are libe3's own (pinned against vectors from libe3's Asn1E3Encoder).
/// Encoding fails, with a diagnostic, for values the libe3 module cannot carry: identifiers outside
/// 1..65535, message identifiers of 0, payloads above 32 KiB, a controlResponse, fragments other than
/// the whole payload, or a report / xApp control without a sequence identifier. OCUDU-only optional
/// fields (causes, detail text, subscription data, by-reference descriptors, fragment bookkeeping)
/// are dropped on encode and left absent on decode.
class e3ap_libe3_codec
{
public:
  /// libe3 >= 0.2.0 wire bounds (E3-DAppID, RAN function, telemetry, control and subscription ids).
  static constexpr uint32_t maximum_identifier = 65535;
  /// E3-MessageID and E3-SequenceID: 1..4294967295 since libe3 0.2.0.
  static constexpr uint32_t    maximum_message_id   = 4294967295U;
  static constexpr std::size_t maximum_list_entries = 1000;
  /// dApp identifiers this RAN hands to libe3 peers (any value up to \ref maximum_identifier is valid
  /// on the wire); native-module peers are numbered above it so the two never collide.
  static constexpr uint32_t    dapp_identifier_pool  = 100;
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
