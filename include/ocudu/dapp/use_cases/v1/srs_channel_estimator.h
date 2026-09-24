// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear
#ifndef OCUDU_DAPP_USE_CASES_V1_SRS_CHANNEL_ESTIMATOR_H
#define OCUDU_DAPP_USE_CASES_V1_SRS_CHANNEL_ESTIMATOR_H
#include "common.h"
#ifdef __cplusplus
extern "C" {
#endif

/// Class-A replacement of the gNB's SRS channel estimator (TS 38.211 Section 6.4.1.4).
///
/// For every SRS occasion the host extracts the SRS resource elements of each receive port and symbol
/// (TS 38.211 Section 6.4.1.4.3), generates the reference sequence of every transmit port and invokes the
/// module synchronously. The module returns the wideband channel matrix and the link measurements the
/// host's own estimator produces; the host publishes them to the scheduler, the SRS-ISAC Class-C route
/// and the E3 srs.channel stream exactly as it publishes its own. Any failure falls back to the host
/// estimator for that occasion.
#define OCUDU_DAPP_INTERFACE_SRS_CHANNEL_ESTIMATOR_V1 0x00010007u

#define OCUDU_DAPP_METADATA_SRS_CHANNEL_ESTIMATOR_V1 0x00010301u
#define OCUDU_DAPP_SRS_PROFILE_V1 0x00010302u

/// Received SRS resource elements, CF32 [pilot_set, rx_port, symbol, pilot].
#define OCUDU_DAPP_LAYOUT_SRS_RX_SET_PORT_SYMBOL_PILOT_CF32_V1 0x00010301u
/// Reference (transmitted) sequences, CF32 [tx_port, pilot], cyclic shift included.
#define OCUDU_DAPP_LAYOUT_SRS_REFERENCE_TX_PILOT_CF32_V1 0x00010302u
/// Wideband channel matrix, CF32 [rx_port, tx_port].
#define OCUDU_DAPP_LAYOUT_SRS_CHANNEL_MATRIX_RX_TX_CF32_V1 0x00010303u
/// Link measurements, F32 [OCUDU_DAPP_SRS_MEASUREMENT_COUNT_V1].
#define OCUDU_DAPP_LAYOUT_SRS_MEASUREMENTS_F32_V1 0x00010304u

#define OCUDU_DAPP_SRS_MAX_RX_PORTS_V1 8u
#define OCUDU_DAPP_SRS_MAX_TX_PORTS_V1 4u
#define OCUDU_DAPP_SRS_MAX_PILOT_SETS_V1 2u
#define OCUDU_DAPP_SRS_MAX_SYMBOLS_V1 14u
/// Longest SRS sequence: 272 PRB at comb 2.
#define OCUDU_DAPP_SRS_MAX_SEQUENCE_LENGTH_V1 1632u

/// Indices of the F32 measurement tensor. A value the module cannot provide is NaN, and the host then
/// reports that measurement as unavailable; the channel matrix is always required.
#define OCUDU_DAPP_SRS_MEASUREMENT_NOISE_VARIANCE_V1 0u   ///< Linear, per resource element.
#define OCUDU_DAPP_SRS_MEASUREMENT_EPRE_DB_V1 1u          ///< Energy per resource element, dB.
#define OCUDU_DAPP_SRS_MEASUREMENT_RSRP_DB_V1 2u          ///< Reference-signal received power, dB.
#define OCUDU_DAPP_SRS_MEASUREMENT_TIME_ALIGNMENT_S_V1 3u ///< Time alignment, seconds (positive: late).
#define OCUDU_DAPP_SRS_MEASUREMENT_TIME_ALIGNMENT_RESOLUTION_S_V1 4u
#define OCUDU_DAPP_SRS_MEASUREMENT_TIME_ALIGNMENT_MIN_S_V1 5u
#define OCUDU_DAPP_SRS_MEASUREMENT_TIME_ALIGNMENT_MAX_S_V1 6u
#define OCUDU_DAPP_SRS_MEASUREMENT_COUNT_V1 8u

/// The pilots of odd transmit ports use their own pilot set (four ports, cyclic shift at or above
/// n_cs_max / 2; TS 38.211 Section 6.4.1.4.3).
#define OCUDU_DAPP_SRS_FLAG_INTERLEAVED_PILOTS_V1 (UINT32_C(1) << 0u)

/// Transmission combs of ocudu_dapp_srs_profile_v1::comb_mask.
#define OCUDU_DAPP_SRS_COMB_2_V1 (UINT32_C(1) << 0u)
#define OCUDU_DAPP_SRS_COMB_4_V1 (UINT32_C(1) << 1u)

/// Capability bit: the module takes device-resident (CUDA) tensors on invocation.execution_stream.
#define OCUDU_DAPP_SRS_CAP_RESIDENT_ACCELERATOR_V1 (UINT64_C(1) << 0u)

/// Occasion description, carried in the input's typed metadata (OCUDU_DAPP_METADATA_SRS_CHANNEL_ESTIMATOR_V1).
typedef struct {
  uint32_t struct_size;
  uint16_t abi_major;
  uint16_t abi_minor;
  uint32_t nof_rx_ports;                                       ///< 1 .. OCUDU_DAPP_SRS_MAX_RX_PORTS_V1.
  uint32_t nof_tx_ports;                                       ///< 1, 2 or 4 SRS antenna ports.
  uint32_t start_symbol;                                       ///< First SRS symbol in the slot.
  uint32_t nof_symbols;                                        ///< SRS symbols (the same sequence is sent in each).
  uint32_t sequence_length;                                    ///< Pilots per symbol and set (M_sc,b^SRS).
  uint32_t comb_size;                                          ///< Transmission comb K_TC: 2 or 4.
  uint32_t nof_pilot_sets;                                     ///< 1, or 2 with interleaved pilots.
  uint32_t n_cs_max;                                           ///< Maximum number of cyclic shifts: 8 or 12.
  uint32_t pilot_set[OCUDU_DAPP_SRS_MAX_TX_PORTS_V1];          ///< Pilot set of each transmit port.
  uint32_t cyclic_shift[OCUDU_DAPP_SRS_MAX_TX_PORTS_V1];       ///< n_SRS^cs of each transmit port.
  uint32_t first_subcarrier[OCUDU_DAPP_SRS_MAX_PILOT_SETS_V1]; ///< k0 of each pilot set, in grid subcarriers.
  uint32_t sequence_group;                                     ///< u.
  uint32_t sequence_number;                                    ///< v.
  uint32_t subcarrier_spacing_hz;
  uint32_t numerology; ///< mu; the SRS symbols use the normal cyclic prefix.
  uint32_t grid_nof_subcarriers;
  uint32_t flags; ///< OCUDU_DAPP_SRS_FLAG_*.
  uint32_t rnti;  ///< C-RNTI of the sounding UE, 0 when unknown.
  uint32_t reserved0;
  /// Largest time alignment the cyclic shifts leave unambiguous: 1 / (n_cs_max * scs * comb_size), seconds.
  double   max_time_alignment_s;
  uint64_t reserved[4];
} ocudu_dapp_srs_channel_estimator_metadata_v1;

/// Envelope a module accepts; the host skips (and falls back on) occasions outside it.
typedef struct {
  uint32_t struct_size;
  uint16_t abi_major;
  uint16_t abi_minor;
  uint32_t profile_id; ///< OCUDU_DAPP_SRS_PROFILE_V1.
  uint32_t max_nof_rx_ports;
  uint32_t max_nof_tx_ports;
  uint32_t max_sequence_length;
  uint32_t comb_mask; ///< OCUDU_DAPP_SRS_COMB_* bits.
  uint32_t flags;     ///< Zero in v1.
  uint64_t reserved[4];
} ocudu_dapp_srs_profile_v1;

typedef struct {
  ocudu_dapp_invocation_v1 invocation;
  /// CF32 [pilot_set, rx_port, symbol, pilot], OCUDU_DAPP_LAYOUT_SRS_RX_SET_PORT_SYMBOL_PILOT_CF32_V1.
  ocudu_dapp_tensor_view_v1 rx_symbols;
  /// CF32 [tx_port, pilot], OCUDU_DAPP_LAYOUT_SRS_REFERENCE_TX_PILOT_CF32_V1.
  ocudu_dapp_tensor_view_v1    reference_symbols;
  ocudu_dapp_typed_metadata_v1 srs;
} ocudu_dapp_srs_channel_estimator_input_v1;

typedef struct {
  /// Caller-owned writable CF32 [rx_port, tx_port]: the wideband channel coefficient of every port pair,
  /// scaled by the inverse noise standard deviation, so that |H|^2 summed over the matrix is the
  /// wideband SNR (the host estimator's convention, srs_channel_matrix).
  ocudu_dapp_tensor_view_v1 channel_matrix;
  /// Caller-owned writable F32 [OCUDU_DAPP_SRS_MEASUREMENT_COUNT_V1].
  ocudu_dapp_tensor_view_v1 measurements;
  uint64_t                  reserved[4];
} ocudu_dapp_srs_channel_estimator_output_v1;

typedef ocudu_dapp_status_v1 (*ocudu_dapp_srs_channel_estimator_invoke_v1)(
    void*,
    const ocudu_dapp_srs_channel_estimator_input_v1*,
    ocudu_dapp_srs_channel_estimator_output_v1*);

typedef struct {
  ocudu_dapp_worker_api_v1                   worker;
  ocudu_dapp_srs_channel_estimator_invoke_v1 invoke;
  uint64_t                                   reserved[8];
} ocudu_dapp_srs_channel_estimator_interface_v1;

#ifdef __cplusplus
}
#endif
#endif
