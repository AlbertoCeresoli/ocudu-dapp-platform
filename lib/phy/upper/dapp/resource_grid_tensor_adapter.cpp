// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/phy/upper/dapp/resource_grid_tensor_adapter.h"
#include "ocudu/adt/complex.h"
#include "ocudu/phy/support/resource_grid_reader.h"
#include <limits>
#ifdef ENABLE_CUDA
#include <cuda_runtime_api.h>
#endif

using namespace ocudu;
using namespace ocudu::dapp;

namespace {

bool valid_lease(const resource_grid_lease& lease) noexcept
{
  return lease.allocation_id != 0 && lease.allocation_generation != 0 && lease.lease_token != 0;
}

bool grid_byte_size(const resource_grid_reader& reader, uint64_t& byte_size) noexcept
{
  const uint64_t nof_ports   = reader.get_nof_ports();
  const uint64_t nof_symbols = reader.get_nof_symbols();
  const uint64_t nof_subc    = reader.get_nof_subc();
  if (nof_ports == 0 || nof_symbols == 0 || nof_subc == 0 ||
      nof_ports > std::numeric_limits<uint64_t>::max() / nof_symbols ||
      nof_ports * nof_symbols > std::numeric_limits<uint64_t>::max() / nof_subc ||
      nof_ports * nof_symbols * nof_subc > std::numeric_limits<uint64_t>::max() / sizeof(cbf16_t)) {
    return false;
  }
  byte_size = nof_ports * nof_symbols * nof_subc * sizeof(cbf16_t);
  return true;
}

ocudu_dapp_tensor_view_v1
make_grid_tensor(const resource_grid_reader& reader, const resource_grid_lease& lease, uint64_t byte_size) noexcept
{
  ocudu_dapp_tensor_view_v1 tensor{};
  tensor.struct_size           = sizeof(tensor);
  tensor.abi_major             = OCUDU_DAPP_ABI_MAJOR_V1;
  tensor.abi_minor             = OCUDU_DAPP_ABI_MINOR_V1;
  tensor.element_type          = OCUDU_DAPP_ELEMENT_CBF16_V1;
  tensor.rank                  = 3;
  tensor.access                = OCUDU_DAPP_ACCESS_READ_V1;
  tensor.allocation_id         = lease.allocation_id;
  tensor.allocation_generation = lease.allocation_generation;
  tensor.byte_length           = byte_size;
  tensor.dimensions[0]         = reader.get_nof_ports();
  tensor.dimensions[1]         = reader.get_nof_symbols();
  tensor.dimensions[2]         = reader.get_nof_subc();
  tensor.byte_strides[2]       = sizeof(cbf16_t);
  tensor.byte_strides[1]       = tensor.dimensions[2] * tensor.byte_strides[2];
  tensor.byte_strides[0]       = tensor.dimensions[1] * tensor.byte_strides[1];
  tensor.layout_id             = OCUDU_DAPP_LAYOUT_RESOURCE_GRID_PORT_SYMBOL_SUBCARRIER_V1;
  tensor.layout_version        = 1;
  tensor.lease_token           = lease.lease_token;
  return tensor;
}

} // namespace

resource_grid_tensor_result ocudu::dapp::make_host_resource_grid_tensor(const resource_grid_reader& reader,
                                                                        const resource_grid_lease&  lease) noexcept
{
  uint64_t byte_size = 0;
  if (!valid_lease(lease) || !grid_byte_size(reader, byte_size)) {
    return {OCUDU_DAPP_INVALID_INPUT_V1, {}};
  }
  const span<const cbf16_t> view = reader.get_contiguous_view();
  if (view.data() == nullptr || view.size_bytes() != byte_size) {
    return {OCUDU_DAPP_UNSUPPORTED_V1, {}};
  }

  auto tensor       = make_grid_tensor(reader, lease, byte_size);
  tensor.domain     = OCUDU_DAPP_MEM_HOST_V1;
  tensor.backend_id = OCUDU_DAPP_BACKEND_HOST_V1;
  tensor.access |= OCUDU_DAPP_ACCESS_HOST_VISIBLE_V1;
  tensor.native_handle = reinterpret_cast<uintptr_t>(view.data());
  return {OCUDU_DAPP_OK_V1, tensor};
}

resource_grid_tensor_result
ocudu::dapp::make_accelerator_resource_grid_tensor(const resource_grid_reader&            reader,
                                                   const accelerator_resource_grid_lease& lease) noexcept
{
  uint64_t byte_size = 0;
  if (!valid_lease(lease) || lease.backend_id == OCUDU_DAPP_BACKEND_HOST_V1 || lease.execution_stream == nullptr ||
      lease.completion_token == 0 || !grid_byte_size(reader, byte_size)) {
    return {OCUDU_DAPP_INVALID_INPUT_V1, {}};
  }
  if (!reader.supports_device_grid_reading()) {
    return {OCUDU_DAPP_UNSUPPORTED_V1, {}};
  }
  // Class-C may only read an owned cudaMalloc snapshot. Falling through to the
  // live managed UL VA is Xid 31 FAULT_PTE @ 0x140000 on GB10 (prepare can
  // succeed while the GPU PTE is still unusable). Miss the frame instead.
  const void* owned       = reader.get_owned_device_snapshot_cbf16();
  void*       owned_ready = reader.get_owned_device_snapshot_ready_event();
  if (owned == nullptr || owned_ready == nullptr) {
    if (!reader.stage_owned_snapshot_from_host()) {
      return {OCUDU_DAPP_BYPASS_V1, {}};
    }
    owned       = reader.get_owned_device_snapshot_cbf16();
    owned_ready = reader.get_owned_device_snapshot_ready_event();
    if (owned == nullptr || owned_ready == nullptr) {
      return {OCUDU_DAPP_BYPASS_V1, {}};
    }
  }
#ifdef ENABLE_CUDA
  if (cudaStreamWaitEvent(static_cast<cudaStream_t>(lease.execution_stream),
                          static_cast<cudaEvent_t>(owned_ready),
                          0) != cudaSuccess) {
    static_cast<void>(cudaGetLastError());
    return {OCUDU_DAPP_BYPASS_V1, {}};
  }
#endif
  auto tensor             = make_grid_tensor(reader, lease, byte_size);
  tensor.domain           = OCUDU_DAPP_MEM_ACCELERATOR_DEVICE_V1;
  tensor.backend_id       = lease.backend_id;
  tensor.device_ordinal   = lease.device_ordinal;
  tensor.native_handle    = reinterpret_cast<uintptr_t>(owned);
  tensor.ready_token      = reinterpret_cast<uintptr_t>(owned_ready);
  tensor.completion_token = lease.completion_token;
  return {OCUDU_DAPP_OK_V1, tensor, false};
}

bool ocudu::dapp::finish_accelerator_resource_grid_read(const resource_grid_reader& reader,
                                                        void*                       execution_stream) noexcept
{
  return execution_stream != nullptr && reader.on_device_grid_reading_enqueued(execution_stream);
}
