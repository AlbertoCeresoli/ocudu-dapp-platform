// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#pragma once

#include "ocudu/ocudulog/logger.h"
#include "ocudu/ofh/compression/iq_compressor.h"
#include "ocudu/ofh/compression/iq_decompressor.h"
#include <memory>

namespace ocudu {
namespace ofh {

/// Bounds used to reserve implementation workspaces before real-time compression starts.
///
/// A default-constructed value disables preallocation. Callers that enable it must provide all four nonzero fields.
struct iq_compressor_preallocation_config {
  unsigned max_nof_prbs    = 0;
  unsigned max_nof_ports   = 0;
  unsigned max_nof_symbols = 0;
  unsigned data_width      = 0;
};

/// Bounds used to reserve implementation workspaces before real-time decompression starts.
///
/// A default-constructed value preserves the implementation default. Callers that provide bounds must set both fields.
struct iq_decompressor_preallocation_config {
  unsigned max_nof_ports   = 0;
  unsigned max_nof_symbols = 0;
};

/// \brief Creates and returns an IQ data compressor.
///
/// \param[in] type       Compression type.
/// \param[in] logger     Logger object.
/// \param[in] iq_scaling Scaling factor applied to IQ data prior to quantization.
/// \param[in] impl_type  Request for a specific implementation type ("auto", "enabled", "disabled", "avx2",
///                       "avx512", "neon", "generic", "cuda"). "auto" selects NVIDIA CUDA when available, otherwise the
///                       best CPU implementation.
/// \param[in] preallocation Optional maximum batch shape used to reserve implementation workspaces before use.
/// \return IQ data compressor.
std::unique_ptr<iq_compressor> create_iq_compressor(compression_type        type,
                                                    ocudulog::basic_logger& logger,
                                                    float                   iq_scaling    = 1.0,
                                                    const std::string&      impl_type     = "auto",
                                                    iq_compressor_preallocation_config preallocation = {});

/// Creates and returns a selector of IQ data compressors.
std::unique_ptr<iq_compressor>
create_iq_compressor_selector(std::array<std::unique_ptr<iq_compressor>, NOF_COMPRESSION_TYPES_SUPPORTED> compressors);

/// \brief Creates and returns an IQ data decompressor.
///
/// \param[in] type       Compression type.
/// \param[in] impl_type  Request for a specific implementation type ("auto", "enabled", "disabled", "avx2",
///                       "avx512", "neon", "generic", "cuda"). "auto" selects NVIDIA CUDA when available, otherwise the
///                       best CPU implementation.
/// \param[in] preallocation Optional maximum receive shape used to reserve implementation workspaces before use.
/// \return IQ data decompressor.
std::unique_ptr<iq_decompressor> create_iq_decompressor(compression_type                     type,
                                                        ocudulog::basic_logger&              logger,
                                                        const std::string&                   impl_type     = "auto",
                                                        iq_decompressor_preallocation_config preallocation = {});

/// Creates and returns a selector of IQ data decompressors.
std::unique_ptr<iq_decompressor> create_iq_decompressor_selector(
    std::array<std::unique_ptr<iq_decompressor>, NOF_COMPRESSION_TYPES_SUPPORTED> decompressors);

} // namespace ofh
} // namespace ocudu
