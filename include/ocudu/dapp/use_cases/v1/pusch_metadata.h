// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#ifndef OCUDU_DAPP_USE_CASES_V1_PUSCH_METADATA_H
#define OCUDU_DAPP_USE_CASES_V1_PUSCH_METADATA_H

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OCUDU_DAPP_METADATA_PUSCH_V1 0x00010101u
#define OCUDU_DAPP_METADATA_DMRS_V1 0x00010102u

/// Canonical Class-A layouts. Data-RE indices are flattened relative to rx_grid
/// and are ordered exactly as dApp LLRs. An allocation-local rx_grid therefore
/// has allocation-local indices, while a full-slot rx_grid has full-grid indices.
#define OCUDU_DAPP_LAYOUT_PUSCH_DATA_RE_MASK_U8_V1 0x00010101u
#define OCUDU_DAPP_LAYOUT_PUSCH_DATA_RE_INDICES_U32_V1 0x00010102u
#define OCUDU_DAPP_LAYOUT_PUSCH_CHANNEL_PORT_LAYER_SYMBOL_SUBCARRIER_V1 0x00010103u
#define OCUDU_DAPP_LAYOUT_PUSCH_EQUALIZED_LAYER_SYMBOL_SUBCARRIER_V1 0x00010104u
#define OCUDU_DAPP_LAYOUT_PUSCH_LLR_DATA_RE_LAYER_BIT_V1 0x00010105u
#define OCUDU_DAPP_LAYOUT_PUSCH_EQUALIZED_DATA_RE_LAYER_V1 0x00010106u
#define OCUDU_DAPP_LAYOUT_PUSCH_POST_EQ_NOISE_DATA_RE_LAYER_V1 0x00010107u

/// Interface requires accelerator-device tensors and queues work on invocation.execution_stream.
#define OCUDU_DAPP_PUSCH_CAP_RESIDENT_ACCELERATOR_V1 (UINT64_C(1) << 0u)

/// Declarative Class-A admission profile. The host checks this envelope before invoking module code.
#define OCUDU_DAPP_PUSCH_PROFILE_V1 0x00010101u
#define OCUDU_DAPP_PUSCH_MODULATION_QPSK_V1 (UINT32_C(1) << 0u)
#define OCUDU_DAPP_PUSCH_MODULATION_QAM16_V1 (UINT32_C(1) << 1u)
#define OCUDU_DAPP_PUSCH_MODULATION_QAM64_V1 (UINT32_C(1) << 2u)
#define OCUDU_DAPP_PUSCH_MODULATION_QAM256_V1 (UINT32_C(1) << 3u)
#define OCUDU_DAPP_PUSCH_DMRS_TYPE1_V1 (UINT32_C(1) << 0u)
#define OCUDU_DAPP_PUSCH_DMRS_TYPE2_V1 (UINT32_C(1) << 1u)
#define OCUDU_DAPP_PUSCH_PROFILE_TRANSFORM_PRECODING_V1 (UINT32_C(1) << 0u)
#define OCUDU_DAPP_PUSCH_PROFILE_DC_OVERLAP_V1 (UINT32_C(1) << 1u)
/// PUSCH uses DFT-s-OFDM transform precoding.
#define OCUDU_DAPP_PUSCH_TRANSFORM_PRECODING_V1 (UINT32_C(1) << 0u)
/// data_re_mask or data_re_indices describes every data RE, in output order.
#define OCUDU_DAPP_PUSCH_DATA_RE_SELECTION_COMPLETE_V1 (UINT32_C(1) << 1u)
/// The allocation contains the DC subcarrier; its data selection already accounts for puncturing.
#define OCUDU_DAPP_PUSCH_DC_OVERLAP_V1 (UINT32_C(1) << 2u)

/// Stable indices in ocudu_dapp_pusch_metadata_v1::reserved.
///
/// These values are scalar metadata, not tensor dimensions. Defining their
/// locations preserves the v1 structure layout while allowing algorithms to
/// express frequency in physical units.
#define OCUDU_DAPP_PUSCH_RESERVED_RNTI_V1 0u
#define OCUDU_DAPP_PUSCH_RESERVED_SUBCARRIER_SPACING_HZ_V1 1u
#define OCUDU_DAPP_PUSCH_RESERVED_GRID_NOF_SUBCARRIERS_V1 2u
/// Direct-current subcarrier of the transmitter as a grid subcarrier index, encoded as
/// (index + 1) so that a zero-initialised slot from an older host reads as "unknown".
/// Published whenever the UE signals a DC location; combine with
/// OCUDU_DAPP_PUSCH_DC_OVERLAP_V1 to know if it falls inside this allocation. The DM-RS
/// pilot on this subcarrier is corrupted by the UE's LO leakage and must not be used for
/// channel estimation; the data RE on it should be demapped as an erasure.
#define OCUDU_DAPP_PUSCH_RESERVED_DC_SUBCARRIER_PLUS_ONE_V1 3u

/// reference_symbols contains a complete [layer, pilot] plane for every PUSCH layer.
#define OCUDU_DAPP_DMRS_REFERENCE_SYMBOLS_LAYER_COMPLETE_V1 (UINT32_C(1) << 0u)
/// The single coordinates [pilot, 2] table applies to every layer in reference_symbols.
#define OCUDU_DAPP_DMRS_COORDINATES_SHARED_ACROSS_LAYERS_V1 (UINT32_C(1) << 1u)

typedef struct {
  uint32_t struct_size;
  uint16_t abi_major;
  uint16_t abi_minor;
  uint32_t profile_id;
  uint32_t max_nof_prb;
  uint32_t max_nof_rx_ports;
  uint32_t max_nof_layers;
  uint32_t modulation_mask;
  uint32_t dmrs_type_mask;
  uint32_t flags;
  uint32_t reserved0;
  uint64_t reserved[6];
} ocudu_dapp_pusch_profile_v1;

/// Values equal the number of soft bits produced for each modulation symbol.
typedef enum {
  /// Used by hooks, such as channel estimation, whose contract is independent of modulation.
  OCUDU_DAPP_MODULATION_UNSPECIFIED_V1 = 0,
  OCUDU_DAPP_MODULATION_QPSK_V1        = 2,
  OCUDU_DAPP_MODULATION_QAM16_V1       = 4,
  OCUDU_DAPP_MODULATION_QAM64_V1       = 6,
  OCUDU_DAPP_MODULATION_QAM256_V1      = 8
} ocudu_dapp_modulation_v1;

/// Canonical, backend-neutral PUSCH shape metadata used by the Class-A reference interfaces.
typedef struct {
  uint32_t                 struct_size;
  uint16_t                 abi_major;
  uint16_t                 abi_minor;
  uint32_t                 nof_rx_ports;
  uint32_t                 nof_layers;
  ocudu_dapp_modulation_v1 modulation;
  uint32_t                 start_symbol;
  uint32_t                 nof_symbols;
  uint32_t                 start_subcarrier;
  uint32_t                 nof_subcarriers;
  float                    noise_variance_floor;
  /// OCUDU_DAPP_PUSCH_* flags. Completeness is explicit; absence means unspecified.
  uint32_t flags;
  /// Dense U8 [symbol, subcarrier] membership map. Useful to host implementations.
  ocudu_dapp_tensor_view_v1 data_re_mask;
  /// Compact U32 [data_re] flattened indices. Preferred by accelerator implementations.
  ocudu_dapp_tensor_view_v1 data_re_indices;
  uint32_t                  nof_data_re;
  uint32_t                  reserved0;
  /// reserved[OCUDU_DAPP_PUSCH_RESERVED_RNTI_V1]: optional C-RNTI.
  /// reserved[OCUDU_DAPP_PUSCH_RESERVED_SUBCARRIER_SPACING_HZ_V1]: physical
  /// subcarrier spacing in Hz.
  /// reserved[OCUDU_DAPP_PUSCH_RESERVED_GRID_NOF_SUBCARRIERS_V1]: full carrier
  /// resource-grid width, independent of the scheduled allocation width.
  /// Remaining entries must be zero when unused.
  uint64_t                  reserved[4];
} ocudu_dapp_pusch_metadata_v1;

/// Explicit pilot values and locations for a standalone channel-estimation implementation.
///
/// reference_symbols is CF32 [layer, pilot]. coordinates is U16 [pilot, 2], where each row is
/// {symbol, subcarrier} relative to rx_grid. Both may reside in the same memory backend as rx_grid.
typedef struct {
  uint32_t struct_size;
  uint16_t abi_major;
  uint16_t abi_minor;
  uint32_t nof_pilots;
  /// OCUDU_DAPP_DMRS_* flags. Completeness is explicit; absence means unspecified.
  uint32_t                  flags;
  ocudu_dapp_tensor_view_v1 reference_symbols;
  ocudu_dapp_tensor_view_v1 coordinates;
  /// 1 for NR DM-RS configuration type 1; 2 for type 2.
  uint32_t dmrs_type;
  uint32_t reserved0;
  /// reserved[0]: optional DMRS OFDM symbol bitmask (bit s set ⇒ symbol s carries DMRS).
  /// When non-zero, accelerator equalizers may skip host D2H of coordinates solely to
  /// discover the symbol mask (coordinates remain required on-device for pilot scatter).
  /// reserved[1..6]: reserved for future use; must be zero when unused.
  uint64_t reserved[7];
} ocudu_dapp_dmrs_metadata_v1;

#ifdef __cplusplus
}
#endif
#endif
