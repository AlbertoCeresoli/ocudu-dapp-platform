// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

/// \file
/// \brief Implementation-neutral PHY acceleration contracts.
///
/// These contracts are the public boundary between generic upper-PHY processing and optional resident accelerator
/// implementations. Generic code may carry opaque resident buffers and execution tokens through these types, but it
/// must not interpret backend-specific details such as streams, events or device pointers directly.

#pragma once

#include "ocudu/adt/span.h"
#include "ocudu/phy/upper/channel_processors/uci/uci_status.h"
#include "ocudu/phy/upper/log_likelihood_ratio.h"
#include <cstddef>
#include <cstdint>

namespace ocudu {

/// Describes a PUSCH codeword whose softbits remain resident in an accelerator-owned buffer.
struct resident_softbit_buffer {
  /// Opaque pointer to the resident softbit buffer.
  void* data = nullptr;
  /// Number of softbits in the resident codeword.
  std::size_t nof_softbits = 0;
  /// Opaque execution context, for example an accelerator stream.
  void* execution_context = nullptr;
  /// Optional opaque completion token covering production of the softbit buffer.
  void* completion_token = nullptr;
  /// True when the buffer and metadata are valid for downstream consumption.
  bool valid = false;
  /// Scrambling identifier used to produce the codeword.
  unsigned n_id = 0;
  /// RNTI used to produce the codeword.
  uint16_t rnti = 0;
  /// Optional resident buffer index for implementations that use a fixed buffer ring.
  int buffer_index = -1;
  /// Optional host-owned acceptance check, evaluated after the backend fences this exact codeword and before
  /// committing TB/CB CRCs or exporting HARQ softbits. This is not a module-supplied function pointer: the native
  /// host adapter must retain the issuing worker, receipt and tensor leases in completion_context until decode
  /// returns, including during deactivation. The callback must be bounded, nonblocking and noexcept, and validate
  /// the original invocation/generations. It must not release storage still used by the decoder.
  ///
  /// Conventional producers leave all three fields empty. A producer requiring asynchronous acceptance sets
  /// acceptance_required; a missing callback/context then rejects, never silently accepts. This internal C++
  /// contract neither changes the dApp v1 ABI nor supplies the still-required native module/receipt adapter.
  bool  acceptance_required = false;
  void* completion_context  = nullptr;
  bool (*accept_completion)(void*, const resident_softbit_buffer&) noexcept = nullptr;
  /// Adapter-owned generation of this completion_context publication. It prevents a copied descriptor from
  /// accepting a later grant when the same fixed context and tensor addresses are reused. Zero is conventional.
  uint64_t completion_generation = 0;

  bool accept_completed_input() const noexcept
  {
    if (!acceptance_required) {
      return completion_context == nullptr && accept_completion == nullptr && completion_generation == 0;
    }
    return completion_context != nullptr && accept_completion != nullptr && accept_completion(completion_context, *this);
  }
};

/// Opaque accelerator-event tokens for opt-in resident dApp stage diagnostics.
///
/// The tokens are valid only for the associated fixed completion slot and only until that slot is reused. Generic
/// upper-PHY code must not interpret them; diagnostic tools for the active accelerator backend may do so after the
/// stream completion token has been synchronized.
struct resident_dapp_diagnostic_timing {
  /// Event recorded immediately before the independently built dApp module is invoked.
  void* module_begin_token = nullptr;
  /// Event recorded after the dApp module has enqueued its work.
  void* module_end_token = nullptr;
  /// Event recorded after the module and required OCUDU continuation have been enqueued.
  void* inline_end_token = nullptr;
  /// Fixed completion slot to which the tokens belong.
  unsigned completion_slot = 0;
  /// True when all tokens describe the same successful inline invocation.
  bool valid = false;
};

/// Compact UCI results produced alongside a resident PUSCH SCH codeword.
struct resident_uci_buffer {
  /// HARQ-ACK LLRs staged for host-side decoding or metric reporting.
  span<const log_likelihood_ratio> harq_ack_llrs;
  /// CSI Part 1 LLRs staged for host-side decoding or metric reporting.
  span<const log_likelihood_ratio> csi_part1_llrs;
  /// Decoded HARQ-ACK payload bits, when produced by the resident backend.
  span<const uint8_t> harq_ack_payload;
  /// Decoded CSI Part 1 payload bits, when produced by the resident backend.
  span<const uint8_t> csi_part1_payload;
  /// HARQ-ACK decoder status associated with \ref harq_ack_payload.
  uci_status harq_ack_status = uci_status::unknown;
  /// CSI Part 1 decoder status associated with \ref csi_part1_payload.
  uci_status csi_part1_status = uci_status::unknown;
  /// True when HARQ-ACK payload bits were decoded by the resident backend.
  bool harq_ack_decoded = false;
  /// True when CSI Part 1 payload bits were decoded by the resident backend.
  bool csi_part1_decoded = false;
  /// True when the LLR spans are valid for host-side consumption.
  bool llrs_valid = false;
  /// True when the decoded payload spans and statuses are valid.
  bool decoded_valid = false;
  /// True when any resident UCI output is available.
  bool valid = false;
};

/// Device-side SCH compaction parameters for UCI-bearing resident PUSCH.
struct pusch_resident_sch_compaction {
  /// Enables device-side compaction of full PUSCH LLRs into SCH-only LLRs.
  bool enabled = false;
  /// Number of UL-SCH LLR bits after UCI demultiplexing.
  unsigned nof_ul_sch_bits = 0;
  /// Number of encoded HARQ-ACK placeholder/reserved bits.
  unsigned nof_harq_ack_rvd = 0;
  /// Number of HARQ-ACK information bits.
  unsigned nof_harq_ack_bits = 0;
  /// Number of encoded HARQ-ACK bits.
  unsigned nof_enc_harq_ack_bits = 0;
  /// Number of CSI Part 1 information bits.
  unsigned nof_csi_part1_bits = 0;
  /// Number of encoded CSI Part 1 bits.
  unsigned nof_enc_csi_part1_bits = 0;
  /// Number of CSI Part 2 information bits.
  unsigned nof_csi_part2_bits = 0;
  /// Number of encoded CSI Part 2 bits.
  unsigned nof_enc_csi_part2_bits = 0;
};

/// PUSCH demodulator options for resident accelerator handoff.
struct pusch_resident_demodulation_config {
  /// Keep SCH softbits resident while still feeding the host demux path for UCI.
  bool host_uci_demux = false;
  /// Compact HARQ/CSI Part 1 on the accelerator while keeping SCH resident for decode.
  bool device_uci_demux = false;
  /// SCH compaction configuration for UCI-bearing PUSCH.
  pusch_resident_sch_compaction sch_compaction;
};

} // namespace ocudu
