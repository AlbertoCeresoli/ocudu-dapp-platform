// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
#pragma once

#ifdef __CUDACC__
#define OCUDU_PILOT_MASK_HD __host__ __device__
#else
#define OCUDU_PILOT_MASK_HD
#endif

namespace ocudu::cuda::pilot_mask {
/// Canonical grant-local erased Type-1 pilot. An odd DC lies between pilots;
/// an out-of-grant DC affects neither their values nor their sample counts.
OCUDU_PILOT_MASK_HD constexpr int erased_type1_pilot(int absolute_dc, int first_sc, int width)
{
  return absolute_dc >= first_sc && absolute_dc - first_sc < width &&
                 (absolute_dc - first_sc) % 2 == 0
             ? absolute_dc - first_sc
             : -1;
}

OCUDU_PILOT_MASK_HD constexpr bool observed(int local_sc, int erased_sc)
{
  return erased_sc < 0 || local_sc != erased_sc;
}

OCUDU_PILOT_MASK_HD constexpr int observed_type1_pilots(int width, int erased_sc)
{
  return width / 2 - (erased_sc >= 0 ? 1 : 0);
}
} // namespace ocudu::cuda::pilot_mask

#undef OCUDU_PILOT_MASK_HD
