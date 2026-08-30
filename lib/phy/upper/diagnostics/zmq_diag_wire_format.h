// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// \file
/// \brief Stable ZMQ diagnostic wire schema and serializers.

#pragma once

#include "ocudu/phy/upper/diagnostics/phy_diagnostics.h"
#include <cstddef>
#include <cstdint>
#include <flatbuffers/flatbuffer_builder.h>

namespace ocudu {

/// FlatBuffers file identifier for the ZMQ diagnostic schema.
static constexpr const char* ZMQ_DIAG_FLATBUFFER_FILE_IDENTIFIER = "OCDG";

/// Diagnostics FlatBuffers payload version.
static constexpr uint8_t ZMQ_DIAG_FLATBUFFER_VERSION = 2;

/// Rate-limit topic identifiers.
enum class zmq_diag_topic : uint8_t { pusch_sym = 0, pusch_crc = 1, srs_chan = 2, pusch_dmrs_chan = 3 };

/// Runtime snapshot of the diagnostic publisher transport health.
struct zmq_diag_publisher_stats {
  uint64_t messages_attempted = 0;
  uint64_t messages_sent      = 0;
  uint64_t transport_drops    = 0;
  uint64_t oversized_drops    = 0;
  uint64_t rate_policy_skips  = 0;
};

/// Returns the ASCII ZMQ topic name for a diagnostic topic.
const char* zmq_diag_topic_name(zmq_diag_topic topic);

/// Returns the minimum publish period for a topic/RNTI pair.
uint64_t zmq_diag_min_period_ns(zmq_diag_topic topic);

/// Maps the transport-neutral diagnostic stream to the ZMQ topic identifier.
zmq_diag_topic to_zmq_diag_topic(phy_diagnostic_stream stream);

/// Serializes a PUSCH equalized-symbol diagnostic payload as a FlatBuffer.
///
/// The builder is cleared before use. Returns the number of bytes available in
/// builder.GetBufferPointer(), or zero if the event cannot be serialized.
std::size_t serialize_zmq_diag_pusch_symbols(flatbuffers::FlatBufferBuilder& builder,
                                             const pusch_symbols_diagnostic& event);

/// Serializes a PUSCH CRC diagnostic payload as a FlatBuffer.
///
/// The builder is cleared before use. Returns the number of bytes available in
/// builder.GetBufferPointer(), or zero if the event cannot be serialized.
std::size_t serialize_zmq_diag_pusch_crc(flatbuffers::FlatBufferBuilder& builder, const pusch_crc_diagnostic& event);

/// Serializes an SRS channel diagnostic payload as a FlatBuffer.
///
/// The builder is cleared before use. Returns the number of bytes available in
/// builder.GetBufferPointer(), or zero if the event cannot be serialized.
std::size_t serialize_zmq_diag_srs_channel(flatbuffers::FlatBufferBuilder& builder,
                                           const srs_channel_diagnostic&   event);

/// Serializes a PUSCH DM-RS channel diagnostic payload as a FlatBuffer.
///
/// The builder is cleared before use. Returns the number of bytes available in
/// builder.GetBufferPointer(), or zero if the event cannot be serialized.
std::size_t serialize_zmq_diag_pusch_dmrs_channel(flatbuffers::FlatBufferBuilder&       builder,
                                                  const pusch_dmrs_channel_diagnostic& event);

} // namespace ocudu
