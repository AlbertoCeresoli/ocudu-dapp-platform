// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

/// \file memory.h
/// Memory domains, element types, and the tensor view carried across the ABI.
///
/// A tensor view is a BORROWED description of host-owned memory: it is valid
/// only for the duration of the call that delivered it (or until its lease
/// token expires, when one is present). Modules never free, resize, or retain
/// the underlying allocation.

#ifndef OCUDU_DAPP_ABI_V1_MEMORY_H
#define OCUDU_DAPP_ABI_V1_MEMORY_H

#include "base.h"

#ifdef __cplusplus
extern "C" {
#endif

/// Where the bytes live. HOST/HOST_PINNED are CPU-addressable;
/// ACCELERATOR_DEVICE is device memory addressed via the backend (a CPU-only
/// module must bypass inputs it cannot address); MANAGED_COHERENT is
/// unified/managed memory; POSIX_SHARED backs supervised-process transport;
/// ACCELERATOR_IPC_EXPORT is a device allocation exported for another
/// process (native_handle carries the IPC handle).
typedef enum {
  OCUDU_DAPP_MEM_HOST_V1                   = 1,
  OCUDU_DAPP_MEM_HOST_PINNED_V1            = 2,
  OCUDU_DAPP_MEM_ACCELERATOR_DEVICE_V1     = 3,
  OCUDU_DAPP_MEM_MANAGED_COHERENT_V1       = 4,
  OCUDU_DAPP_MEM_POSIX_SHARED_V1           = 5,
  OCUDU_DAPP_MEM_ACCELERATOR_IPC_EXPORT_V1 = 6
} ocudu_dapp_memory_domain_v1;

/// Accelerator backend that owns a device-domain allocation. The ABI is
/// backend-neutral: modules declare what they support and bypass the rest.
typedef enum {
  OCUDU_DAPP_BACKEND_HOST_V1 = 0,
  OCUDU_DAPP_BACKEND_CUDA_V1 = 1,
  OCUDU_DAPP_BACKEND_HIP_V1  = 2,
  OCUDU_DAPP_BACKEND_SYCL_V1 = 3
} ocudu_dapp_backend_v1;

/// Element encodings. CF*/CBF16 are interleaved complex pairs (re, im) of the
/// underlying real type; BF16 is bfloat16.
typedef enum {
  OCUDU_DAPP_ELEMENT_I8_V1    = 1,
  OCUDU_DAPP_ELEMENT_U8_V1    = 2,
  OCUDU_DAPP_ELEMENT_I16_V1   = 3,
  OCUDU_DAPP_ELEMENT_U16_V1   = 4,
  OCUDU_DAPP_ELEMENT_I32_V1   = 5,
  OCUDU_DAPP_ELEMENT_U32_V1   = 6,
  OCUDU_DAPP_ELEMENT_F16_V1   = 7,
  OCUDU_DAPP_ELEMENT_BF16_V1  = 8,
  OCUDU_DAPP_ELEMENT_F32_V1   = 9,
  OCUDU_DAPP_ELEMENT_CF16_V1  = 10,
  OCUDU_DAPP_ELEMENT_CBF16_V1 = 11,
  OCUDU_DAPP_ELEMENT_CF32_V1  = 12
} ocudu_dapp_element_type_v1;

/// Access rights granted on a view. Write only what WRITE grants, at the
/// given offsets; HOST_VISIBLE marks device memory that is also
/// CPU-addressable; EXPORTABLE marks allocations eligible for IPC export.
enum {
  OCUDU_DAPP_ACCESS_READ_V1         = 1u << 0,
  OCUDU_DAPP_ACCESS_WRITE_V1        = 1u << 1,
  OCUDU_DAPP_ACCESS_HOST_VISIBLE_V1 = 1u << 2,
  OCUDU_DAPP_ACCESS_EXPORTABLE_V1   = 1u << 3
};

/// Well-known layout id: the uplink resource grid ordered
/// port-major, then OFDM symbol, then subcarrier.
#define OCUDU_DAPP_LAYOUT_RESOURCE_GRID_PORT_SYMBOL_SUBCARRIER_V1 0x00010001u

/// Borrowed n-dimensional view of one host-owned allocation.
///
/// Addressing: element (i0..i(rank-1)) lives at
/// base(native_handle) + byte_offset + sum(i_k * byte_strides[k]),
/// within [byte_offset, byte_offset + byte_length). \c dimensions[k] is the
/// extent of axis k; axes beyond \c rank are zero.
///
/// Identity and validity: (allocation_id, allocation_generation) name the
/// allocation instance — a generation change means the memory was recycled
/// and any cached derived state is stale. \c ready_token /
/// \c completion_token order asynchronous producers/consumers on the
/// execution stream (zero = not used); \c lease_token, when nonzero, bounds
/// how long the borrow stays valid. \c layout_id / \c layout_version name
/// the semantic arrangement (see the well-known ids), and \c reserved is
/// zero.
typedef struct {
  uint32_t struct_size;
  uint16_t abi_major;
  uint16_t abi_minor;
  uint32_t domain;       ///< ocudu_dapp_memory_domain_v1
  uint32_t backend_id;   ///< ocudu_dapp_backend_v1 for device domains
  uint32_t element_type; ///< ocudu_dapp_element_type_v1
  uint32_t rank;
  uint32_t access; ///< OCUDU_DAPP_ACCESS_* bits
  uint32_t device_ordinal;
  uint64_t allocation_id;
  uint64_t allocation_generation;
  uint64_t native_handle; ///< Domain-specific base: pointer, fd, or IPC handle.
  uint64_t byte_offset;
  uint64_t byte_length;
  uint64_t dimensions[OCUDU_DAPP_MAX_TENSOR_RANK_V1];
  uint64_t byte_strides[OCUDU_DAPP_MAX_TENSOR_RANK_V1];
  uint32_t layout_id;
  uint32_t layout_version;
  uint64_t ready_token;
  uint64_t completion_token;
  uint64_t lease_token;
  uint64_t reserved[8]; ///< Must be zero; a zero value always reads as "not provided".
} ocudu_dapp_tensor_view_v1;

#ifdef __cplusplus
}
#endif
#endif
