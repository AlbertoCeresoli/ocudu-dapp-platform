// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "ocudu/adt/span.h"
#include "ocudu/dapp/e3/e3ap_protocol.h"
#include "ocudu/dapp/e3/result.h"

namespace ocudu::dapp::e3 {

/// Hand-written Protobuf (proto3) codec for E3AP, so the gNB needs no libprotobuf. Profiles
/// (\ref e3ap_profile): libe3 = libe3's messages/proto/V1/e3ap-1.0.0.proto exactly; ocudu =
/// schemas/e3/protobuf/e3ap-ocudu-v2.proto, those messages plus OCUDU fields numbered >= 100 that
/// libe3 skips as unknown (the same additions E3AP-OCUDU-V2 carries in APER).
///
/// Encoding is canonical libprotobuf output: fields in ascending number, implicit-presence scalars
/// omitted at their default, \c optional fields written whenever set, repeated scalars packed. The
/// libe3 profile is byte-identical to libe3's ProtobufE3Encoder and the OCUDU profile to libprotobuf
/// built from schemas/e3/protobuf/e3ap-ocudu-v2.proto (both checked in tests/interop/libe3).
///
/// Decoding accepts any valid proto3 serialisation (any field order, packed or unpacked repeated
/// scalars, repeated fields merged, unknown fields skipped). Under the libe3 profile OCUDU fields are
/// skipped like any unknown field; under the OCUDU profile they are read.
///
/// Both profiles enforce the bounds of the module they mirror: libe3 profile values must fit the
/// libe3 E3AP 1.0.0 module (the same checks as \ref e3ap_libe3_codec, so one libe3 association
/// behaves the same over APER and Protobuf); OCUDU profile values must fit E3AP-OCUDU-V2's bounds. In proto3
/// a zero identifier, message identifier or sequence identifier means "absent".
class e3ap_protobuf_codec
{
public:
  /// Largest single E3-PDU accepted on the wire (either profile).
  static constexpr std::size_t maximum_pdu_bytes = e3ap_maximum_fragment_bytes + 4096;

  static codec_result<std::vector<uint8_t>> encode(const e3ap_pdu& pdu, e3ap_profile profile) noexcept;
  static codec_result<e3ap_pdu> decode(ocudu::span<const uint8_t> bytes, e3ap_profile profile) noexcept;

  /// The profile a SetupRequest PDU asks for (OCUDU when it carries \c ocudu_profile), or nothing
  /// when \p bytes is not a well-formed Protobuf E3Pdu holding a SetupRequest.
  static std::optional<e3ap_profile> setup_profile(ocudu::span<const uint8_t> bytes) noexcept;

  /// What a link layer needs to route a PDU, read without decoding it (no allocation).
  struct routing_view {
    uint32_t pdu_id{};
    /// E3Pdu.msg field number (3 = SetupRequest ... 13 = MessageAck, 100 = ControlResponse).
    uint32_t message{};
    /// dApp identifier the message names, when it names one.
    std::optional<uint16_t> dapp_identifier;
    /// MessageAck/SetupResponse request identifier.
    uint32_t request_id{};
    /// SetupResponse/MessageAck response code is positive.
    bool positive{};
  };
  static std::optional<routing_view> peek(ocudu::span<const uint8_t> bytes) noexcept;

  /// True when \p message can be expressed under \p profile as-is.
  static bool
  representable(const e3ap_message& message, e3ap_profile profile, std::string* detail = nullptr) noexcept;
};

} // namespace ocudu::dapp::e3
