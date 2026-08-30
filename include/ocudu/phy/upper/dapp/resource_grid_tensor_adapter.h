// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "ocudu/dapp/abi/v1/memory.h"
#include <cstdint>

namespace ocudu {

class resource_grid_reader;

namespace dapp {

struct resource_grid_lease {
  uint64_t allocation_id;
  uint64_t allocation_generation;
  uint64_t lease_token;
};

struct accelerator_resource_grid_lease : resource_grid_lease {
  uint32_t backend_id;
  uint32_t device_ordinal;
  void*    execution_stream;
  uint64_t completion_token;
};

struct resource_grid_tensor_result {
  ocudu_dapp_status_v1      status{OCUDU_DAPP_INTERNAL_ERROR_V1};
  ocudu_dapp_tensor_view_v1 tensor{};
  bool                      live_hold{false};
  explicit                  operator bool() const noexcept { return status == OCUDU_DAPP_OK_V1; }
};

/// Borrows a complete contiguous host grid without copying it.
resource_grid_tensor_result make_host_resource_grid_tensor(const resource_grid_reader& reader,
                                                           const resource_grid_lease&  lease) noexcept;

/// Borrows an in-process accelerator grid after validating its explicit ownership and ordering lease.
resource_grid_tensor_result
make_accelerator_resource_grid_tensor(const resource_grid_reader&            reader,
                                      const accelerator_resource_grid_lease& lease) noexcept;

/// Records that the dApp has enqueued all reads on the admitted execution stream.
bool finish_accelerator_resource_grid_read(const resource_grid_reader& reader, void* execution_stream) noexcept;

} // namespace dapp
} // namespace ocudu
