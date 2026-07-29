// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "phy_acceleration_resource_grid_factory.h"

#ifdef ENABLE_CUDA
#include "resource_grid_cuda_visible_impl.h"
#include <cuda_runtime.h>
#endif

using namespace ocudu;

bool ocudu::phy_acceleration_cuda_current_device_is_discrete(int nvcuda_device_id)
{
#ifdef ENABLE_CUDA
  (void)cudaGetLastError();

  int integrated = 0;
  if (cudaDeviceGetAttribute(&integrated, cudaDevAttrIntegrated, nvcuda_device_id) != cudaSuccess) {
    (void)cudaGetLastError();
    return false;
  }

  return integrated == 0;
#else
  (void)nvcuda_device_id;
  return false;
#endif
}

std::shared_ptr<resource_grid_factory>
ocudu::create_phy_acceleration_resource_grid_factory(std::shared_ptr<resource_grid_factory>   fallback_factory,
                                                     phy_acceleration_resource_grid_direction direction,
                                                     bool                                     enable_accelerated_grid,
                                                     int                                      nvcuda_device_id)
{
  if (!enable_accelerated_grid) {
    return fallback_factory;
  }

#ifdef ENABLE_CUDA
  switch (direction) {
    case phy_acceleration_resource_grid_direction::downlink:
      return std::make_shared<resource_grid_cuda_visible_factory>(
          resource_grid_cuda_visible_factory::direction::downlink, nvcuda_device_id);
    case phy_acceleration_resource_grid_direction::uplink:
      return std::make_shared<resource_grid_cuda_visible_factory>(resource_grid_cuda_visible_factory::direction::uplink,
                                                                  nvcuda_device_id);
  }
#endif

  (void)nvcuda_device_id;
  return fallback_factory;
}
