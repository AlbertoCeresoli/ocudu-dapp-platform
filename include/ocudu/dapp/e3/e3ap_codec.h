// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "ocudu/adt/span.h"
#include "ocudu/dapp/e3/e3ap_protocol.h"
#include "ocudu/dapp/e3/result.h"

namespace ocudu::dapp::e3 {

/// Generated ASN.1 aligned-PER bindings for E3AP-OCUDU-V1, OCUDU-TELEMETRY-SM
/// and OCUDU-DAPP-MGMT-CARRIER-SM. The codec owns decoded storage and returns
/// plain C++ values; it never touches sockets or the data store.
class e3ap_codec
{
public:
  /// Largest single E3-PDU accepted on the wire (one fragment plus envelope).
  static constexpr std::size_t maximum_pdu_bytes = e3ap_maximum_fragment_bytes + 4096;

  static codec_result<std::vector<uint8_t>> encode(const e3ap_pdu& pdu) noexcept;
  static codec_result<e3ap_pdu>             decode(ocudu::span<const uint8_t> bytes) noexcept;

  static codec_result<std::vector<uint8_t>>        encode(const telemetry_indication& indication) noexcept;
  static codec_result<telemetry_indication>        decode_telemetry(ocudu::span<const uint8_t> bytes) noexcept;
  static codec_result<std::vector<uint8_t>>        encode(const telemetry_ran_function_data& data) noexcept;
  static codec_result<telemetry_ran_function_data> decode_telemetry_function(ocudu::span<const uint8_t> bytes) noexcept;
  static codec_result<std::vector<uint8_t>>        encode(const telemetry_subscription_data& data) noexcept;
  static codec_result<telemetry_subscription_data> decode_subscription(ocudu::span<const uint8_t> bytes) noexcept;
  static codec_result<std::vector<uint8_t>>        encode(const management_ran_function_data& data) noexcept;
  static codec_result<management_ran_function_data>
  decode_management_function(ocudu::span<const uint8_t> bytes) noexcept;

  /// Converts one reassembled portable publication (metadata + tensor) into a
  /// telemetry indication. Telemetry streams decode the packed metadata
  /// record; Class-C portable interfaces are forwarded verbatim.
  static codec_result<telemetry_indication> from_publication(uint32_t                   interface_id,
                                                             uint32_t                   cell_id,
                                                             uint64_t                   sequence,
                                                             uint64_t                   route_epoch,
                                                             uint64_t                   dropped_objects,
                                                             ocudu::span<const uint8_t> metadata,
                                                             ocudu::span<const uint8_t> tensor) noexcept;

  /// Splits an encoded payload into fragments no larger than the bound.
  static std::vector<ocudu::span<const uint8_t>> fragment(ocudu::span<const uint8_t> payload,
                                                          std::size_t                maximum_fragment_bytes) noexcept;
};

} // namespace ocudu::dapp::e3
