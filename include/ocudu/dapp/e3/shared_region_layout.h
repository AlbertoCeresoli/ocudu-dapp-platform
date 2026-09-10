// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

/// Layout of the POSIX shared-memory region used for by-reference E3AP
/// delivery (E3-SharedRegion / E3-TensorDescriptor). Frozen v1; consumers in
/// any language mirror these offsets. All integers are little-endian.
///
///   region = region_header (64 B) | row[0] | row[1] | ... | row[nof_rows-1]
///   row    = row_header (64 B)    | tensor bytes (row_bytes - 64)
///
/// Writers publish a row with a seqlock: row generation is set to an odd
/// value before the copy and to the following even value after it; the
/// descriptor carries the even value. Readers copy the bytes, then re-read the
/// row generation and discard the copy if it changed. The region header's
/// write_index and generation name the most recent complete row.

#ifndef OCUDU_DAPP_E3_SHARED_REGION_LAYOUT_H
#define OCUDU_DAPP_E3_SHARED_REGION_LAYOUT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// "OCE3SHM1" as a little-endian 64-bit integer.
#define OCUDU_E3_SHARED_REGION_MAGIC_V1 UINT64_C(0x314D48533345434F)
#define OCUDU_E3_SHARED_REGION_VERSION_V1 1u
#define OCUDU_E3_SHARED_REGION_HEADER_BYTES_V1 64u
#define OCUDU_E3_SHARED_ROW_HEADER_BYTES_V1 64u
#define OCUDU_E3_SHARED_REGION_NO_ROW UINT64_C(0xFFFFFFFFFFFFFFFF)

typedef struct {
  uint64_t magic;
  uint32_t version;
  uint32_t header_bytes;
  uint32_t row_header_bytes;
  uint32_t nof_rows;
  uint64_t row_bytes;
  /// Row index of the most recent complete publication; NO_ROW before the first.
  uint64_t write_index;
  /// Generation of the most recent complete publication (even).
  uint64_t generation;
  uint64_t reserved[2];
} ocudu_e3_shared_region_header_v1;

typedef struct {
  /// Seqlock generation: odd while the writer copies, even when complete.
  uint64_t generation;
  uint64_t sequence;
  uint64_t byte_length;
  uint16_t telemetry_id;
  uint16_t cell_id;
  uint32_t reserved32;
  /// Realtime clock at publication in nanoseconds since the Unix epoch.
  uint64_t realtime_ns;
  uint64_t route_epoch;
  uint64_t reserved[2];
} ocudu_e3_shared_row_header_v1;

#if defined(__cplusplus)
static_assert(sizeof(ocudu_e3_shared_region_header_v1) == OCUDU_E3_SHARED_REGION_HEADER_BYTES_V1,
              "E3 shared region header layout changed");
static_assert(sizeof(ocudu_e3_shared_row_header_v1) == OCUDU_E3_SHARED_ROW_HEADER_BYTES_V1,
              "E3 shared row header layout changed");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(ocudu_e3_shared_region_header_v1) == OCUDU_E3_SHARED_REGION_HEADER_BYTES_V1,
               "E3 shared region header layout changed");
_Static_assert(sizeof(ocudu_e3_shared_row_header_v1) == OCUDU_E3_SHARED_ROW_HEADER_BYTES_V1,
               "E3 shared row header layout changed");
#endif

#ifdef __cplusplus
}
#endif

#endif
