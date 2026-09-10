// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "../../../lib/dapp/runtime/native_worker_validation.h"
#include <array>
#include <cstring>
#include <gtest/gtest.h>

using ocudu::dapp::native_worker_detail::receiver_symbol_storage;

TEST(receiver_symbol_storage, requires_both_metadata_extents)
{
  ocudu_dapp_receiver_metrics_v1 metrics{};
  metrics.struct_size                   = sizeof(metrics);
  metrics.equalized_symbols.struct_size = sizeof(metrics.equalized_symbols);
  ocudu_dapp_typed_metadata_v1 wrapper{};
  wrapper.metadata.data = &metrics;
  wrapper.metadata.size = sizeof(metrics);
  EXPECT_EQ(receiver_symbol_storage(wrapper), &metrics.equalized_symbols);
  wrapper.metadata.size = OCUDU_DAPP_RECEIVER_EQUALIZED_SYMBOLS_MIN_SIZE_V1 - 1;
  EXPECT_EQ(receiver_symbol_storage(wrapper), nullptr);
  wrapper.metadata.size = sizeof(metrics);
  metrics.struct_size   = OCUDU_DAPP_RECEIVER_EQUALIZED_SYMBOLS_MIN_SIZE_V1 - 1;
  EXPECT_EQ(receiver_symbol_storage(wrapper), nullptr);
  metrics.struct_size                   = sizeof(metrics);
  metrics.equalized_symbols.struct_size = 0;
  EXPECT_EQ(receiver_symbol_storage(wrapper), nullptr);
  wrapper.metadata.data = nullptr;
  EXPECT_EQ(receiver_symbol_storage(wrapper), nullptr);
}

TEST(receiver_symbol_storage, genuine_legacy_object_never_reads_the_tail)
{
  // Allocate only the old prefix so ASan also detects any speculative tail read.
  constexpr auto legacy_size = offsetof(ocudu_dapp_receiver_metrics_v1, equalized_symbols);
  alignas(ocudu_dapp_receiver_metrics_v1) std::array<unsigned char, legacy_size> legacy{};
  const uint32_t declared_size = sizeof(ocudu_dapp_receiver_metrics_v1);
  std::memcpy(legacy.data(), &declared_size, sizeof(declared_size));
  ocudu_dapp_typed_metadata_v1 wrapper{};
  wrapper.metadata.data = legacy.data();
  wrapper.metadata.size = legacy.size();
  EXPECT_EQ(receiver_symbol_storage(wrapper), nullptr);
}
