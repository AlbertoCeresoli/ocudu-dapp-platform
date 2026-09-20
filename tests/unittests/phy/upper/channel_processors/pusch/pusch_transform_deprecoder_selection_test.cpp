// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear
#include "pusch_transform_deprecoder_selection.h"
#include <gtest/gtest.h>

TEST(pusch_transform_deprecoder_selection, explicit_custom_never_selects_auto_or_vkfft)
{
  for (bool compiled : {false, true}) {
    EXPECT_EQ(pusch_select_transform_deprecoder("custom", compiled), PUSCH_DEPRECODER_CUSTOM);
    EXPECT_EQ(pusch_select_transform_deprecoder("auto", compiled), PUSCH_DEPRECODER_AUTO);
    EXPECT_EQ(pusch_select_transform_deprecoder("vkfft", compiled), PUSCH_DEPRECODER_VKFFT);
  }
}

TEST(pusch_transform_deprecoder_selection, defaults_remain_compatible)
{
  for (const char* value : {static_cast<const char*>(nullptr), "", "0", "1", "unknown", "CUSTOM"}) {
    EXPECT_EQ(pusch_select_transform_deprecoder(value, true), PUSCH_DEPRECODER_AUTO);
    EXPECT_EQ(pusch_select_transform_deprecoder(value, false), PUSCH_DEPRECODER_CUSTOM);
  }
}

TEST(pusch_transform_deprecoder_selection, harness_and_library_share_the_canonical_setting)
{
  EXPECT_STREQ(PUSCH_TRANSFORM_DEPRECODER_ENV, "OCUDU_PHY_CUDA_PUSCH_TRANSFORM_DEPRECODER");
}
