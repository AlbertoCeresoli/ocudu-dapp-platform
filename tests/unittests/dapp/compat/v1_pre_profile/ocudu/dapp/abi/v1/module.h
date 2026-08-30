// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// Frozen test-only snapshot from 8ff8190cad^.
// It is not installed and must not be used as an authoring header.

#ifndef OCUDU_DAPP_ABI_V1_MODULE_H
#define OCUDU_DAPP_ABI_V1_MODULE_H

#include "host_api.h"
#include "management.h"

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#define OCUDU_DAPP_EXPORT_V1 __declspec(dllexport)
#else
#define OCUDU_DAPP_EXPORT_V1 __attribute__((visibility("default")))
#endif

typedef struct {
  uint32_t    struct_size;
  uint16_t    abi_major;
  uint16_t    abi_minor;
  uint32_t    interface_id;
  uint16_t    interface_major;
  uint16_t    interface_minor;
  uint32_t    dapp_class;
  uint32_t    placement_mask;
  uint32_t    threading;
  uint32_t    reserved0;
  const void* interface_vtable;
  uint64_t    capability_bits;
  uint64_t    reserved[7];
} ocudu_dapp_interface_descriptor_v1;

typedef struct {
  uint32_t                                  struct_size;
  uint16_t                                  abi_major;
  uint16_t                                  abi_minor;
  const char*                               global_name;
  const char*                               vendor;
  const char*                               version;
  uint64_t                                  capability_bits;
  uint32_t                                  nof_interfaces;
  uint32_t                                  reserved0;
  const ocudu_dapp_interface_descriptor_v1* interfaces;
  ocudu_dapp_status_v1 (*create)(const ocudu_dapp_host_api_v1*, const void* create_args, void** instance);
  ocudu_dapp_status_v1 (*configure)(void* instance, const void* typed_config);
  ocudu_dapp_status_v1 (*prepare)(void* instance, const void* prepare_args);
  ocudu_dapp_status_v1 (*warmup)(void* instance, const void* shapes);
  ocudu_dapp_status_v1 (*start)(void* instance);
  ocudu_dapp_status_v1 (*stop)(void* instance);
  ocudu_dapp_status_v1 (*quiesce)(void* instance, uint64_t deadline_ns);
  void (*destroy)(void* instance);
  const ocudu_dapp_management_v1* management;
  uint64_t                        reserved[15];
} ocudu_dapp_module_v1;

typedef struct {
  uint32_t struct_size;
  uint16_t abi_major;
  uint16_t abi_minor;
  uint64_t host_capabilities;
  uint64_t backend_mask;
  uint64_t host_build_id[2];
  uint64_t reserved[12];
} ocudu_dapp_host_info_v1;

typedef ocudu_dapp_status_v1 (*ocudu_dapp_query_v1_fn)(const ocudu_dapp_host_info_v1* host,
                                                       ocudu_dapp_module_v1*          module_out);
OCUDU_DAPP_EXPORT_V1 ocudu_dapp_status_v1 ocudu_dapp_query_v1(const ocudu_dapp_host_info_v1* host,
                                                              ocudu_dapp_module_v1*          module_out);

#ifdef __cplusplus
}
#endif
#endif
