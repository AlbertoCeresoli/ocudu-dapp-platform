// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#ifndef OCUDU_DAPP_PROCESS_V1_PUBLICATION_RECORD_H
#define OCUDU_DAPP_PROCESS_V1_PUBLICATION_RECORD_H

#include "shared_ring.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OCUDU_DAPP_PUBLICATION_RECORD_MAGIC_V1 UINT32_C(0x4f435052)
#define OCUDU_DAPP_PUBLICATION_RECORD_VERSION_V1 1u
#define OCUDU_DAPP_PUBLICATION_RECORD_HEADER_BYTES_V1 64u
#define OCUDU_DAPP_PUBLICATION_RECORD_INLINE_TENSOR_V1 (UINT32_C(1) << 0u)
#define OCUDU_DAPP_PUBLICATION_RECORD_EXTERNAL_TENSOR_V1 (UINT32_C(1) << 1u)

typedef struct {
  uint32_t magic;
  uint16_t version;
  uint16_t header_size;
  uint32_t flags;
  uint32_t metadata_size;
  uint64_t tensor_offset;
  uint64_t tensor_size;
  uint64_t publication_sequence;
  uint64_t route_epoch;
  uint64_t reserved[2];
} ocudu_dapp_publication_record_header_v1;

/// For inline records tensor_offset is a byte offset in the record. For
/// external records it is the route export-slot identifier; tensor_size is the
/// exact valid byte extent in that slot.

typedef struct {
  ocudu_dapp_publication_record_header_v1 header;
  const uint8_t*                          metadata;
  const uint8_t*                          tensor;
} ocudu_dapp_publication_record_view_v1;

typedef enum {
  OCUDU_DAPP_PUBLICATION_RECORD_OK_V1               = 0,
  OCUDU_DAPP_PUBLICATION_RECORD_INVALID_ARGUMENT_V1 = 1,
  OCUDU_DAPP_PUBLICATION_RECORD_INVALID_HEADER_V1   = 2,
  OCUDU_DAPP_PUBLICATION_RECORD_INVALID_FLAGS_V1    = 3,
  OCUDU_DAPP_PUBLICATION_RECORD_INVALID_EXTENT_V1   = 4,
  OCUDU_DAPP_PUBLICATION_RECORD_NONZERO_PADDING_V1  = 5
} ocudu_dapp_publication_record_status_v1;

static inline ocudu_dapp_publication_record_status_v1
ocudu_dapp_parse_publication_record_v1(const void* bytes, size_t size, ocudu_dapp_publication_record_view_v1* output)
{
  if (bytes == NULL || output == NULL || size < sizeof(ocudu_dapp_publication_record_header_v1)) {
    return OCUDU_DAPP_PUBLICATION_RECORD_INVALID_ARGUMENT_V1;
  }
  ocudu_dapp_publication_record_header_v1 header;
  memcpy(&header, bytes, sizeof(header));
  if (header.magic != OCUDU_DAPP_PUBLICATION_RECORD_MAGIC_V1 ||
      header.version != OCUDU_DAPP_PUBLICATION_RECORD_VERSION_V1 ||
      header.header_size != OCUDU_DAPP_PUBLICATION_RECORD_HEADER_BYTES_V1 || header.metadata_size == 0 ||
      header.publication_sequence == 0 || header.route_epoch == 0 || header.reserved[0] != 0 ||
      header.reserved[1] != 0) {
    return OCUDU_DAPP_PUBLICATION_RECORD_INVALID_HEADER_V1;
  }
  const uint32_t known_flags =
      OCUDU_DAPP_PUBLICATION_RECORD_INLINE_TENSOR_V1 | OCUDU_DAPP_PUBLICATION_RECORD_EXTERNAL_TENSOR_V1;
  if ((header.flags & ~known_flags) != 0 || (header.flags & known_flags) == known_flags) {
    return OCUDU_DAPP_PUBLICATION_RECORD_INVALID_FLAGS_V1;
  }
  if (header.metadata_size > size - header.header_size) {
    return OCUDU_DAPP_PUBLICATION_RECORD_INVALID_EXTENT_V1;
  }
  const size_t   metadata_end = (size_t)header.header_size + header.metadata_size;
  const uint8_t* data         = (const uint8_t*)bytes;
  if ((header.flags & OCUDU_DAPP_PUBLICATION_RECORD_INLINE_TENSOR_V1) != 0) {
    if (header.tensor_size == 0 || header.tensor_offset < metadata_end || header.tensor_offset > size ||
        (header.tensor_offset % OCUDU_DAPP_SHARED_RING_ALIGNMENT_V1) != 0 ||
        header.tensor_size > size - header.tensor_offset || header.tensor_size != size - header.tensor_offset) {
      return OCUDU_DAPP_PUBLICATION_RECORD_INVALID_EXTENT_V1;
    }
    for (size_t i = metadata_end; i != header.tensor_offset; ++i) {
      if (data[i] != 0) {
        return OCUDU_DAPP_PUBLICATION_RECORD_NONZERO_PADDING_V1;
      }
    }
  } else if ((header.flags & OCUDU_DAPP_PUBLICATION_RECORD_EXTERNAL_TENSOR_V1) != 0) {
    if (header.tensor_size == 0 || header.tensor_offset > UINT32_MAX || metadata_end != size) {
      return OCUDU_DAPP_PUBLICATION_RECORD_INVALID_EXTENT_V1;
    }
  } else if (header.tensor_offset != 0 || header.tensor_size != 0 || metadata_end != size) {
    return OCUDU_DAPP_PUBLICATION_RECORD_INVALID_EXTENT_V1;
  }
  output->header   = header;
  output->metadata = data + header.header_size;
  output->tensor =
      (header.flags & OCUDU_DAPP_PUBLICATION_RECORD_INLINE_TENSOR_V1) != 0 ? data + header.tensor_offset : NULL;
  return OCUDU_DAPP_PUBLICATION_RECORD_OK_V1;
}

#if defined(__cplusplus)
static_assert(sizeof(ocudu_dapp_publication_record_header_v1) == 64, "Class-C publication record layout changed");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(ocudu_dapp_publication_record_header_v1) == 64, "Class-C publication record layout changed");
#endif

#ifdef __cplusplus
}
#endif
#endif
