// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#pragma once

#include <cstddef>
#include <optional>

namespace ocudu {

/// Validate codeblock extents before indexing or staging CUDA buffers.
/// The reader returns zero for invalid metadata. Subtraction bounds the sum
/// without overflow; no allocation or backend reconfiguration occurs here.
template <typename Codeblocks, typename LengthReader>
std::optional<std::size_t> pusch_decoder_batch_extent(const Codeblocks& codeblocks,
                                                      std::size_t       max_codeblocks,
                                                      std::size_t       max_llrs,
                                                      LengthReader      read_length)
{
  if (codeblocks.empty() || codeblocks.size() > max_codeblocks) {
    return std::nullopt;
  }
  std::size_t total = 0;
  for (const auto& cb : codeblocks) {
    const std::size_t length = read_length(cb);
    if (length == 0 || length > max_llrs - total) {
      return std::nullopt;
    }
    total += length;
  }
  return total;
}

} // namespace ocudu
