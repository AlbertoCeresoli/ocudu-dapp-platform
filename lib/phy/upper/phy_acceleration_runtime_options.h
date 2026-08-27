// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

/// \file
/// \brief Runtime overrides for implementation-specific PHY acceleration paths.

#pragma once

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace ocudu {

/// Returns true if \c value is one of the accepted false-like environment flag values.
inline bool phy_acceleration_env_flag_value_is_false(const char* value)
{
  return (std::strcmp(value, "0") == 0) || (std::strcmp(value, "false") == 0) || (std::strcmp(value, "False") == 0) ||
         (std::strcmp(value, "FALSE") == 0) || (std::strcmp(value, "off") == 0) || (std::strcmp(value, "OFF") == 0) ||
         (std::strcmp(value, "no") == 0) || (std::strcmp(value, "NO") == 0);
}

/// Returns true if \c value is one of the accepted true-like environment flag values.
inline bool phy_acceleration_env_flag_value_is_true(const char* value)
{
  return (std::strcmp(value, "1") == 0) || (std::strcmp(value, "true") == 0) || (std::strcmp(value, "True") == 0) ||
         (std::strcmp(value, "TRUE") == 0) || (std::strcmp(value, "on") == 0) || (std::strcmp(value, "ON") == 0) ||
         (std::strcmp(value, "yes") == 0) || (std::strcmp(value, "YES") == 0);
}

/// Reads a boolean environment flag with the accepted true/false string set.
inline bool phy_acceleration_env_flag_enabled(const char* name, bool default_value = false)
{
  const char* value = std::getenv(name);
  if ((value == nullptr) || (*value == '\0')) {
    return default_value;
  }

  if (phy_acceleration_env_flag_value_is_true(value)) {
    return true;
  }
  if (phy_acceleration_env_flag_value_is_false(value)) {
    return false;
  }

  return default_value;
}

/// Reads an unsigned integer environment override, returning \c default_value for unset or malformed values.
inline unsigned phy_acceleration_env_unsigned(const char* name, unsigned default_value)
{
  const char* value = std::getenv(name);
  if ((value == nullptr) || (*value == '\0')) {
    return default_value;
  }
  if (*value == '-') {
    return default_value;
  }

  char* end_ptr        = nullptr;
  errno                = 0;
  unsigned long parsed = std::strtoul(value, &end_ptr, 10);
  if ((end_ptr == value) || (*end_ptr != '\0') || (errno == ERANGE) ||
      (parsed > std::numeric_limits<unsigned>::max())) {
    return default_value;
  }

  return static_cast<unsigned>(parsed);
}

/// Returns true when a grid allocation mode string requests managed memory.
inline bool phy_acceleration_env_mode_is_managed(const char* value)
{
  return (value != nullptr) && (std::strcmp(value, "managed") == 0);
}

/// Returns the CUDA-visible grid mode, preferring an optional direction-specific override.
inline const char* phy_acceleration_cuda_visible_grid_mode(const char* direction_env_name = nullptr)
{
  const char* mode = nullptr;
  if (direction_env_name != nullptr) {
    mode = std::getenv(direction_env_name);
  }
  if (mode == nullptr) {
    mode = std::getenv("OCUDU_CUDA_VISIBLE_GRID");
  }
  return mode;
}

/// Returns true when CUDA-visible resource grids should use managed memory.
inline bool phy_acceleration_cuda_visible_grid_managed_requested(const char* direction_env_name = nullptr)
{
  return phy_acceleration_env_mode_is_managed(phy_acceleration_cuda_visible_grid_mode(direction_env_name));
}

/// Returns true when the PDSCH auto-enable override has been configured.
inline bool phy_acceleration_pdsch_auto_enable_discrete_configured()
{
  const char* value = std::getenv("OCUDU_PDSCH_AUTO_ENABLE_DISCRETE");
  return (value != nullptr) && (*value != '\0');
}

/// Returns true when PDSCH auto-enable for discrete GPUs is requested.
inline bool phy_acceleration_pdsch_auto_enable_discrete_requested()
{
  return phy_acceleration_env_flag_enabled("OCUDU_PDSCH_AUTO_ENABLE_DISCRETE");
}

/// Capability-derived defaults for a CUDA-visible managed resource grid.
///
/// Inputs are the three CUDA device attributes that actually differ across the
/// boxes we run on. Product names are only examples; the policy never keys off
/// them.
///
/// | class | CMA | host PT | CPU→GPU-resident | examples | default |
/// |---|---|---|---|---|---|
/// | fully coherent | 1 | 1 | 1 | GH200, Grace-Hopper | no prefetch; concurrent DL writes; prefer device |
/// | GPU sees host pages, CPU cannot read GPU-resident | 1 | 1 | 0 | GB10, many Jetson | prefetch both ways; no concurrent writes |
/// | migrate-on-fault / no ATS | 0 or host PT 0 | * | 0 | discrete, older Jetson | same as the row above if managed is forced (auto uses pinned) |
///
/// `cudaMemPrefetchAsync` returning NotSupported is a later no-op, so turning
/// prefetch on by default is safe on parts that cannot migrate.
struct cuda_visible_managed_access_policy {
  bool host_reads_device_resident_directly = false;
  bool prefetch_by_default                 = false;
  bool prefer_device_memory                = false;
  bool allow_concurrent_downlink_writes    = false;
};

/// True when CPU and GPU can touch the same managed pages with no migration.
inline bool cuda_visible_managed_access_is_fully_coherent(bool concurrent_managed_access,
                                                          bool host_uses_host_page_tables,
                                                          bool direct_managed_access_from_host)
{
  return concurrent_managed_access && host_uses_host_page_tables && direct_managed_access_from_host;
}

inline cuda_visible_managed_access_policy
make_cuda_visible_managed_access_policy(bool concurrent_managed_access,
                                        bool host_uses_host_page_tables,
                                        bool direct_managed_access_from_host,
                                        bool enable_concurrent_downlink_writes = false)
{
  const bool fully_coherent = cuda_visible_managed_access_is_fully_coherent(
      concurrent_managed_access, host_uses_host_page_tables, direct_managed_access_from_host);

  cuda_visible_managed_access_policy policy;
  policy.host_reads_device_resident_directly = direct_managed_access_from_host;
  policy.prefetch_by_default                 = !fully_coherent;
  policy.prefer_device_memory                = fully_coherent;
  policy.allow_concurrent_downlink_writes    = enable_concurrent_downlink_writes && fully_coherent;
  return policy;
}

} // namespace ocudu
