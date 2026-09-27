// SPDX-FileCopyrightText: Copyright (C) 2021-2026 DeepSig Inc
// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "cuda_rt_utils.h"
#include "pdsch_tb_encoder_cuda.h"
#include "ocudu/ocudulog/ocudulog.h"
#include "ocudu/ran/sch/ldpc_base_graph.h"
#include "ocudu/ran/sch/sch_segmentation.h"
#include "ocudu/support/units.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <cuComplex.h>
#include <cuda.h>
#include <cuda_runtime.h>
#include <limits>
#include <modulation.h>
#include <mutex>
#include <scrambling.h>
#include <transport_block.h>
#include <vector>

using namespace ocudu;

namespace {

bool is_symbol_d2h_deferred()
{
  static const bool deferred = []() {
    const char* value = std::getenv("OCUDU_PDSCH_DEFER_SYMBOL_D2H");
    return value != nullptr && std::strcmp(value, "0") != 0;
  }();
  return deferred;
}

enum class host_tb_crc_mode { disabled, enabled, automatic };

host_tb_crc_mode get_host_tb_crc_mode()
{
  static const host_tb_crc_mode mode = []() {
    const char* value = std::getenv("OCUDU_PDSCH_HOST_TB_CRC");
    if (value == nullptr) {
      return host_tb_crc_mode::disabled;
    }
    if ((std::strcmp(value, "0") == 0) || (std::strcmp(value, "off") == 0) || (std::strcmp(value, "false") == 0)) {
      return host_tb_crc_mode::disabled;
    }
    if (std::strcmp(value, "auto") == 0) {
      return host_tb_crc_mode::automatic;
    }
    return host_tb_crc_mode::enabled;
  }();
  return mode;
}

unsigned get_host_tb_crc_auto_max_bytes()
{
  static const unsigned max_bytes = []() {
    const char* value = std::getenv("OCUDU_PDSCH_HOST_TB_CRC_AUTO_MAX_BYTES");
    if (value == nullptr) {
      return 32768U;
    }
    char*         end    = nullptr;
    unsigned long parsed = std::strtoul(value, &end, 10);
    if ((end == value) || (*end != '\0') ||
        (parsed > static_cast<unsigned long>(std::numeric_limits<unsigned>::max()))) {
      return 32768U;
    }
    return static_cast<unsigned>(parsed);
  }();
  return max_bytes;
}

bool should_use_host_tb_crc(const pdsch_tb_encoder_gpu::config& cfg, unsigned tb_bytes_count)
{
  if ((cfg.tb_size_bits <= 3824) || ((cfg.tb_size_bits & 7U) != 0U) || ((cfg.tb_size_bits >> 3U) > tb_bytes_count)) {
    return false;
  }

  switch (get_host_tb_crc_mode()) {
    case host_tb_crc_mode::disabled:
      return false;
    case host_tb_crc_mode::enabled:
      return true;
    case host_tb_crc_mode::automatic:
      return (cfg.tb_size_bits >> 3U) <= get_host_tb_crc_auto_max_bytes();
  }
  return false;
}

tb_encoder_config_t make_tb_encoder_config(const pdsch_tb_encoder_gpu::config& cfg)
{
  tb_encoder_config_t tb_cfg = {};
  tb_cfg.tb_size_bits        = static_cast<int>(cfg.tb_size_bits);
  tb_cfg.num_layers          = static_cast<int>(cfg.num_layers);
  tb_cfg.modulation_order    = static_cast<int>(cfg.modulation_order);
  tb_cfg.num_allocated_res   = static_cast<int>(cfg.num_coded_bits);
  tb_cfg.redundancy_version  = static_cast<int>(cfg.rv);
  tb_cfg.n_RNTI              = cfg.n_rnti;
  tb_cfg.n_ID                = cfg.n_id;
  tb_cfg.q                   = cfg.cw_index;
  tb_cfg.enable_scrambling   = true;
  tb_cfg.code_rate           = static_cast<float>(cfg.tb_size_bits) / static_cast<float>(cfg.num_coded_bits);
  tb_cfg.base_graph          = static_cast<int>(cfg.base_graph);
  tb_cfg.num_code_blocks     = static_cast<int>(cfg.nof_codeblocks);
  tb_cfg.lifting_size        = static_cast<int>(cfg.lifting_size);
  tb_cfg.nof_filler_bits     = static_cast<int>(cfg.nof_filler_bits);
  tb_cfg.nof_short_segments  = static_cast<int>(cfg.nof_short_segments);
  tb_cfg.E_short             = static_cast<int>(cfg.E_short);
  tb_cfg.E_long              = static_cast<int>(cfg.E_long);
  tb_cfg.N_ref               = static_cast<int>(cfg.N_ref);
  return tb_cfg;
}

bool are_pdsch_cuda_graphs_enabled()
{
  // Match transport_block.cu: default OFF. (Previously this defaulted ON when unset,
  // which disagreed with the multi-sector gold default and with OTA launchers that
  // omit the env var.)
  static const bool enabled = []() {
    const char* value = std::getenv("OCUDU_PDSCH_CUDA_GRAPHS");
    if (value == nullptr || value[0] == '\0') {
      return false;
    }
    return (std::strcmp(value, "0") != 0) && (std::strcmp(value, "false") != 0) &&
           (std::strcmp(value, "off") != 0) && (std::strcmp(value, "no") != 0);
  }();
  return enabled;
}

bool are_full_encode_graphs_enabled()
{
  static const bool enabled = []() {
    const char* value = std::getenv("OCUDU_PDSCH_FULL_CUDA_GRAPHS");
    if (value == nullptr) {
      return false;
    }
    return std::strcmp(value, "0") != 0;
  }();
  return enabled;
}

std::mutex& pdsch_full_encode_graph_capture_mutex()
{
  static std::mutex mutex;
  return mutex;
}

uint32_t compute_crc24a_host_byte_aligned(const uint8_t* data, size_t nof_bytes)
{
  struct crc24a_tables {
    std::array<uint32_t, 256> table = {};
    std::array<uint32_t, 256> s0    = {};
    std::array<uint32_t, 256> s1    = {};
    std::array<uint32_t, 256> s2    = {};
    std::array<uint32_t, 256> s3    = {};
  };

  static const crc24a_tables tables = []() {
    crc24a_tables      values = {};
    constexpr uint32_t poly   = 0x864CFB;
    for (unsigned i = 0; i != 256; ++i) {
      uint32_t crc = i << 16;
      for (unsigned bit = 0; bit != 8; ++bit) {
        crc = (crc & 0x800000U) ? ((crc << 1U) ^ poly) : (crc << 1U);
      }
      values.table[i] = crc & 0xFFFFFFU;
      values.s3[i]    = values.table[i];
    }
    for (unsigned i = 0; i != 256; ++i) {
      values.s2[i] = ((values.s3[i] << 8U) ^ values.table[(values.s3[i] >> 16U) & 0xFFU]) & 0xFFFFFFU;
      values.s1[i] = ((values.s2[i] << 8U) ^ values.table[(values.s2[i] >> 16U) & 0xFFU]) & 0xFFFFFFU;
      values.s0[i] = ((values.s1[i] << 8U) ^ values.table[(values.s1[i] >> 16U) & 0xFFU]) & 0xFFFFFFU;
    }
    return values;
  }();

  uint32_t crc = 0;
  size_t   i   = 0;
  for (; i + 4 <= nof_bytes; i += 4) {
    uint8_t idx0 = static_cast<uint8_t>(((crc >> 16U) ^ data[i]) & 0xFFU);
    uint8_t idx1 = static_cast<uint8_t>(((crc >> 8U) ^ data[i + 1]) & 0xFFU);
    uint8_t idx2 = static_cast<uint8_t>((crc ^ data[i + 2]) & 0xFFU);
    crc          = tables.s0[idx0] ^ tables.s1[idx1] ^ tables.s2[idx2] ^ tables.s3[data[i + 3]];
  }
  for (; i != nof_bytes; ++i) {
    uint8_t index = static_cast<uint8_t>(((crc >> 16U) ^ data[i]) & 0xFFU);
    crc           = ((crc << 8U) ^ tables.table[index]) & 0xFFFFFFU;
  }
  return crc;
}

/// Maximum transport block size in bytes (max TB is ~1.3M bits = ~165KB).
static constexpr unsigned MAX_TB_BYTES = 200000;

/// Maximum coded bits (G can be very large for high MCS + large BW).
static constexpr unsigned MAX_CODED_BITS = 2000000;

/// Maximum symbols (G / min_mod_order = G / 2).
static constexpr unsigned MAX_SYMBOLS = MAX_CODED_BITS / 2;

static constexpr size_t PDSCH_FULL_ENCODE_GRAPH_CACHE_LIMIT = 16;

/// GPU PDSCH TB encoder implementation using CUDA.
class pdsch_tb_encoder_gpu_impl : public pdsch_tb_encoder_gpu
{
public:
  struct encode_graph_key {
    config              cfg            = {};
    tb_encoder_handle_t tb_encoder     = nullptr;
    uint8_t*            d_tb_input     = nullptr;
    uint32_t*           d_tb_crc       = nullptr;
    int8_t*             d_symbols_int8 = nullptr;
    cudaStream_t        stream         = nullptr;
    unsigned            tb_bytes_count = 0;
    bool                host_tb_crc    = false;
  };

  struct encode_graph_entry {
    encode_graph_key key         = {};
    cudaGraph_t      graph       = nullptr;
    cudaGraphExec_t  exec        = nullptr;
    uint64_t         use_counter = 0;
  };

  pdsch_tb_encoder_gpu_impl()
  {
    // Initialize CUDA Driver API.
    CUresult res = cuInit(0);
    if (res != CUDA_SUCCESS) {
      gpu_available_ = false;
      return;
    }

    // Get device 0.
    res = cuDeviceGet(&device_, 0);
    if (res != CUDA_SUCCESS) {
      gpu_available_ = false;
      return;
    }

    // Retain the primary context (thread-safe).
    res = cuDevicePrimaryCtxRetain(&context_, device_);
    if (res != CUDA_SUCCESS) {
      gpu_available_ = false;
      return;
    }

    // Set context ONCE here - will be inherited by calling thread.
    res = cuCtxSetCurrent(context_);
    if (res != CUDA_SUCCESS) {
      cuDevicePrimaryCtxRelease(device_);
      gpu_available_ = false;
      return;
    }

    cudaError_t err = ocudu::cudaStreamCreateUpperPhy(&stream_, "OCUDU_PDSCH_CUDA_STREAM_PRIORITY");
    if (err != cudaSuccess) {
      cuDevicePrimaryCtxRelease(device_);
      gpu_available_ = false;
      return;
    }

    // Create CUDA event for async completion tracking (no timing needed for speed).
    err = cudaEventCreateWithFlags(&completion_event_, cudaEventDisableTiming);
    if (err != cudaSuccess) {
      cudaStreamDestroy(stream_);
      cuDevicePrimaryCtxRelease(device_);
      gpu_available_ = false;
      return;
    }

    // Track when the asynchronous H2D transfer has stopped reading the reusable pinned TB staging buffers.
    err = cudaEventCreateWithFlags(&tb_input_copied_event_, cudaEventDisableTiming);
    if (err != cudaSuccess) {
      cudaEventDestroy(completion_event_);
      completion_event_ = nullptr;
      cudaStreamDestroy(stream_);
      cuDevicePrimaryCtxRelease(device_);
      gpu_available_ = false;
      return;
    }

    const char* timing_env = std::getenv("OCUDU_PDSCH_TIMING");
    timing_enabled_        = timing_env && (std::strcmp(timing_env, "0") != 0);
    if (timing_enabled_) {
      if ((cudaEventCreate(&timing_start_event_) != cudaSuccess) ||
          (cudaEventCreate(&timing_h2d_event_) != cudaSuccess) ||
          (cudaEventCreate(&timing_encode_event_) != cudaSuccess) ||
          (cudaEventCreate(&timing_d2h_event_) != cudaSuccess)) {
        cleanup();
        return;
      }
      last_timing_stats_.enabled = true;
    }

    // Allocate device memory for TB input.
    err = cudaMalloc(&d_tb_input_, MAX_TB_BYTES);
    if (err != cudaSuccess) {
      cleanup();
      return;
    }

    err = cudaMalloc(&d_tb_crc_, sizeof(uint32_t));
    if (err != cudaSuccess) {
      cleanup();
      return;
    }

    // Allocate device memory for coded bits (rate-matched + scrambled).
    // Need bytes for bit-packed output.
    err = cudaMalloc(&d_coded_bits_, (MAX_CODED_BITS + 7) / 8);
    if (err != cudaSuccess) {
      cleanup();
      return;
    }

    // Allocate device memory for symbols.
    err = cudaMalloc(&d_symbols_, MAX_SYMBOLS * sizeof(cuFloatComplex));
    if (err != cudaSuccess) {
      cleanup();
      return;
    }

    // Allocate device memory for int8 symbols (2 bytes per symbol: real + imag).
    err = cudaMalloc(&d_symbols_int8_, MAX_SYMBOLS * 2);
    if (err != cudaSuccess) {
      cleanup();
      return;
    }

    // Allocate pinned host memory for fast async D2H transfer.
    err = cudaMallocHost(&h_symbols_pinned_, MAX_SYMBOLS * 2);
    if (err != cudaSuccess) {
      cleanup();
      return;
    }

    err = cudaMallocHost(&h_tb_input_pinned_, MAX_TB_BYTES);
    if (err != cudaSuccess) {
      cleanup();
      return;
    }

    err = cudaMallocHost(&h_tb_crc_pinned_, sizeof(uint32_t));
    if (err != cudaSuccess) {
      cleanup();
      return;
    }

    // Create CUDA modulator.
    if (modulator_create(&modulator_) != 0) {
      cleanup();
      return;
    }

    // Create CUDA scrambler for fused scramble+modulate path.
    if (scrambler_create(&scrambler_) != NR_LDPC_SUCCESS) {
      modulator_destroy(modulator_);
      modulator_ = nullptr;
      cleanup();
      return;
    }

    // Preallocate scrambling sequence buffer for max coded bits to avoid
    // runtime allocations during real-time operation.
    if (scrambler_preallocate_sequence(scrambler_, MAX_CODED_BITS) != NR_LDPC_SUCCESS) {
      // Not fatal - will allocate on demand
    }

    gpu_available_ = true;

    // Comprehensive warmup to eliminate 15-18ms cold-start penalty.
    // This forces CUDA context initialization and ALL kernel JIT compilation to happen now.
    // Warmup includes: TB encoder, scrambler sequence generation, and modulation kernels.

    ocudulog::fetch_basic_logger("PHY").info("CUDA: Warming up PDSCH GPU encoder for real-time operation...");

    // 1. Resident TB encoder for this lane (one create at startup — never on the slot path).
    if (tb_encoder_create(&resident_tb_encoder_) != NR_LDPC_SUCCESS) {
      ocudulog::fetch_basic_logger("PHY").error(
          "CUDA: Failed to create resident PDSCH TB encoder - GPU PDSCH disabled for this lane");
      cleanup();
      return;
    }
    nr_ldpc_status_t warmup_status = tb_encoder_warmup(resident_tb_encoder_, stream_);
    if (warmup_status != NR_LDPC_SUCCESS) {
      ocudulog::fetch_basic_logger("PHY").warning("CUDA: TB encoder warmup failed - first call will be slower");
    }

    // 2. Warmup scrambler sequence generation for typical 100MHz PDSCH configuration
    //    Max G for 100MHz 273 PRB = 273 * 12 * 13 * 8 (256QAM) = 340704 bits
    //    Note: PDSCH uses symbols 1-13 (13 symbols), symbol 0 is reserved for PDCCH
    //    Use representative config for warmup
    constexpr int      WARMUP_N_CODED_BITS = 340704; // 273 PRB * 12 * 13 * 8
    constexpr unsigned WARMUP_N_RNTI       = 0x4601;
    constexpr unsigned WARMUP_N_ID         = 1;
    constexpr unsigned WARMUP_CW_INDEX     = 0;

    nr_scrambling_config_t scr_cfg = {};
    scr_cfg.n_RNTI                 = WARMUP_N_RNTI;
    scr_cfg.n_ID                   = WARMUP_N_ID;
    scr_cfg.q                      = WARMUP_CW_INDEX;

    if (scrambler_configure(scrambler_, &scr_cfg) == NR_LDPC_SUCCESS) {
      // Generate scrambling sequence to trigger kernel JIT compilation
      if (scrambler_generate_sequence(scrambler_, WARMUP_N_CODED_BITS, stream_) == NR_LDPC_SUCCESS) {
        // 3. Warmup modulation kernels for all common modulation orders
        //    Run each modulation kernel to force JIT compilation
        const uint32_t* d_scramble_seq = reinterpret_cast<const uint32_t*>(scrambler_get_sequence_ptr(scrambler_));

        // Warmup QPSK (Qm=2)
        modulator_scramble_and_modulate_int8(modulator_,
                                             reinterpret_cast<uint32_t*>(d_coded_bits_),
                                             d_scramble_seq,
                                             d_symbols_int8_,
                                             WARMUP_N_CODED_BITS,
                                             2, // QPSK
                                             stream_);

        // Warmup 16QAM (Qm=4)
        modulator_scramble_and_modulate_int8(modulator_,
                                             reinterpret_cast<uint32_t*>(d_coded_bits_),
                                             d_scramble_seq,
                                             d_symbols_int8_,
                                             WARMUP_N_CODED_BITS,
                                             4, // 16QAM
                                             stream_);

        // Warmup 64QAM (Qm=6)
        modulator_scramble_and_modulate_int8(modulator_,
                                             reinterpret_cast<uint32_t*>(d_coded_bits_),
                                             d_scramble_seq,
                                             d_symbols_int8_,
                                             WARMUP_N_CODED_BITS,
                                             6, // 64QAM
                                             stream_);

        // Warmup 256QAM (Qm=8) - most common for high MCS
        modulator_scramble_and_modulate_int8(modulator_,
                                             reinterpret_cast<uint32_t*>(d_coded_bits_),
                                             d_scramble_seq,
                                             d_symbols_int8_,
                                             WARMUP_N_CODED_BITS,
                                             8, // 256QAM
                                             stream_);

        // Wait for all warmup kernels to complete
        cudaStreamSynchronize(stream_);

        // Pre-warm a RAR/Msg2-class small BG2 QPSK encode so the first OTA RAR does not pay
        // tb_encoder_create on the 500 us slot deadline (historically 500-1200 us cold RAR).
        //
        // CRITICAL: the cache key / same_encode_shape() includes lifting_size and filler bits
        // from the CPU segmenter. A prior warmup with Z=0/F=0 never matched the live RAR
        // (A=72 → CRC16 → B=88 → BG2 Z=15, F=62), so first live RAR still created a new
        // encoder (~700 us). Use the same sch_segmentation helpers as the segmenter.
        //
        // Also warm encode_to_device_grid (the OTA path under DIRECT_DEVICE_GRID) — plain
        // encode() only exercises the int8 symbol path, not the bf16 grid kernel chain.
        {
          using namespace units::literals;
          constexpr units::bits rar_tbs_bits{72}; // 9-byte MAC RAR
          // Live OTA RAR on this profile maps ~324 data REs after DMRS exclusion
          // (prb=[0,3) symb=[2,14)), not the naive 3*12*12=432. Warming G=864 caused a
          // same_encode_shape miss → tb_encoder_create on every first RAR (~250-600 us).
          // Measured: first live encode_enqueue ~618 us create; second same G ~20 us hit.
          constexpr unsigned rar_G      = 648; // 324 RE * Qm=2
          constexpr unsigned rar_Qm     = 2;
          constexpr unsigned rar_nof_re = rar_G / rar_Qm; // 324

          const unsigned rar_nof_cb =
              compute_nof_codeblocks(rar_tbs_bits, ldpc_base_graph_type::BG2);
          const unsigned rar_Z =
              compute_lifting_size(rar_tbs_bits, ldpc_base_graph_type::BG2, rar_nof_cb);
          const units::bits rar_K = compute_codeblock_size(ldpc_base_graph_type::BG2, rar_Z);
          const units::bits rar_B = rar_tbs_bits + compute_tb_crc_size(rar_tbs_bits);
          // C=1: no per-CB CRC; filler fills K - B.
          const unsigned rar_filler =
              (rar_K.value() > rar_B.value()) ? static_cast<unsigned>(rar_K.value() - rar_B.value()) : 0U;

          config rar_warm             = {};
          rar_warm.tb_size_bits       = static_cast<unsigned>(rar_tbs_bits.value());
          rar_warm.num_layers         = 1;
          rar_warm.modulation_order   = rar_Qm;
          rar_warm.num_coded_bits     = rar_G;
          rar_warm.rv                 = 0;
          rar_warm.n_rnti             = 0x007f; // common RA-RNTI for this lab profile
          // Warm the cell scrambling ID used by this lab profile (PCI=2 → n_id often 2).
          // Wrong n_id left the first live RAR on the shape+scramble path and forced a
          // cold gold-sequence generate. Override with OCUDU_PDSCH_RAR_WARM_N_ID.
          {
            const char* n_id_env = std::getenv("OCUDU_PDSCH_RAR_WARM_N_ID");
            unsigned    warm_nid = 2; // matches pci=2 OTA lab default
            if (n_id_env != nullptr && n_id_env[0] != '\0') {
              char* end = nullptr;
              const unsigned long parsed = std::strtoul(n_id_env, &end, 0);
              if (end != n_id_env && *end == '\0' && parsed <= 1023UL) {
                warm_nid = static_cast<unsigned>(parsed);
              }
            }
            rar_warm.n_id = static_cast<uint16_t>(warm_nid);
          }
          rar_warm.cw_index           = 0;
          rar_warm.base_graph         = 2;
          rar_warm.nof_codeblocks     = rar_nof_cb;
          rar_warm.lifting_size    = rar_Z;
          rar_warm.nof_filler_bits = rar_filler;
          // CPU segmenter for C=1 sets nof_short_segments = C - (N_symb % C) = 1.
          // Warming with 0 made same_encode_shape() miss and forced tb_encoder_create
          // on every live RAR (~400-650 us), even when Z/F/G matched.
          rar_warm.nof_short_segments = rar_nof_cb; // 1 for single-CB RAR
          rar_warm.E_short            = rar_G;
          rar_warm.E_long             = rar_G;

          std::array<uint8_t, 9> dummy_tb{};
          const unsigned         warm_symbols =
              encode(span<const uint8_t>(dummy_tb.data(), dummy_tb.size()), rar_warm);
          cudaStreamSynchronize(stream_);

          // Direct-grid path: allocate tiny device scratch matching RAR RE count.
          unsigned grid_warm_ok = 0;
          {
            uint32_t* d_re_offsets = nullptr;
            uint16_t* d_grid_bf16  = nullptr;
            // nof_grid_re_per_port must cover max offset; use identity offsets 0..N-1.
            const unsigned nof_grid_re = rar_nof_re;
            if (cudaMalloc(&d_re_offsets, rar_nof_re * sizeof(uint32_t)) == cudaSuccess &&
                cudaMalloc(&d_grid_bf16, nof_grid_re * 2U * sizeof(uint16_t)) == cudaSuccess) {
              std::vector<uint32_t> h_offs(rar_nof_re);
              for (unsigned i = 0; i < rar_nof_re; ++i) {
                h_offs[i] = i;
              }
              if (cudaMemcpyAsync(d_re_offsets,
                                  h_offs.data(),
                                  rar_nof_re * sizeof(uint32_t),
                                  cudaMemcpyHostToDevice,
                                  stream_) == cudaSuccess) {
                grid_warm_ok = encode_to_device_grid(span<const uint8_t>(dummy_tb.data(), dummy_tb.size()),
                                                     rar_warm,
                                                     d_re_offsets,
                                                     d_grid_bf16,
                                                     /*symbol_offset=*/0,
                                                     rar_nof_re,
                                                     /*nof_ports=*/1,
                                                     /*nof_layers=*/1,
                                                     nof_grid_re,
                                                     /*weight_real=*/1.0F);
                cudaStreamSynchronize(stream_);
              }
            }
            if (d_re_offsets != nullptr) {
              cudaFree(d_re_offsets);
            }
            if (d_grid_bf16 != nullptr) {
              cudaFree(d_grid_bf16);
            }
          }

          if (warm_symbols == 0) {
            ocudulog::fetch_basic_logger("PHY").warning(
                "CUDA: RAR-shape PDSCH warmup encode returned 0 symbols - first RAR may be slow");
          } else {
            ocudulog::fetch_basic_logger("PHY").info(
                "CUDA: RAR-shape PDSCH encoder warm (int8_syms={} grid_syms={} Z={} F={} G={}) - "
                "small DL grants ready",
                warm_symbols,
                grid_warm_ok,
                rar_Z,
                rar_filler,
                rar_G);
          }
        }

        // Freeze resident encoder: no further cudaMalloc/grow on this lane's hot path.
        tb_encoder_lock_rt(resident_tb_encoder_);
        ocudulog::fetch_basic_logger("PHY").info(
            "CUDA: PDSCH GPU encoder warmup complete - resident RT lane locked (no hot-path alloc)");
      } else {
        ocudulog::fetch_basic_logger("PHY").warning("CUDA: Scrambler warmup failed - first call will be slower");
        tb_encoder_lock_rt(resident_tb_encoder_);
      }
    } else {
      ocudulog::fetch_basic_logger("PHY").warning("CUDA: Scrambler config failed - first call will be slower");
      if (resident_tb_encoder_ != nullptr) {
        tb_encoder_lock_rt(resident_tb_encoder_);
      }
    }
  }

  ~pdsch_tb_encoder_gpu_impl() override
  {
    if (gpu_available_) {
      gpu_available_ = false;
      cuCtxSetCurrent(context_);

      if (stream_) {
        cudaStreamSynchronize(stream_);
      }

      if (scrambler_) {
        scrambler_destroy(scrambler_);
        scrambler_ = nullptr;
      }
      if (modulator_) {
        modulator_destroy(modulator_);
        modulator_ = nullptr;
      }
      destroy_resident_tb_encoder();

      cleanup();
    }
  }

  unsigned encode(span<const uint8_t> tb_bytes, const config& cfg) override
  {
    if (!gpu_available_) {
      return 0;
    }

    // NOTE: Context switch removed - context is set once at construction.
    // The calling thread inherits the context, which is thread-safe for primary context.

    // Save modulation order for later use in get_symbols() scaling.
    last_modulation_order_ = cfg.modulation_order;

    tb_encoder_config_t tb_cfg = make_tb_encoder_config(cfg);

#ifdef OCUDU_PDSCH_DIAGNOSTICS
    fprintf(stderr,
            "[GPU PDSCH] TBS=%d bits, G=%d, rate=%.4f, Qm=%d, rv=%d, n_RNTI=0x%04X, n_ID=%d, q=%d\n",
            tb_cfg.tb_size_bits,
            tb_cfg.num_allocated_res,
            tb_cfg.code_rate,
            tb_cfg.modulation_order,
            tb_cfg.redundancy_version,
            tb_cfg.n_RNTI,
            tb_cfg.n_ID,
            tb_cfg.q);
#endif

    tb_encoder_handle_t tb_encoder = get_tb_encoder_for_config(cfg, tb_cfg);
    if (tb_encoder == nullptr) {
      return 0;
    }

    // Upload TB bytes to GPU (single H2D transfer).
    unsigned tb_bytes_count = tb_bytes.size();
    if (tb_bytes_count > MAX_TB_BYTES) {
      return 0;
    }
    if (!wait_for_tb_input_staging()) {
      return 0;
    }
    if (timing_enabled_) {
      cudaEventRecord(timing_start_event_, stream_);
    }
    std::memcpy(h_tb_input_pinned_, tb_bytes.data(), tb_bytes_count);

    const bool use_host_tb_crc = should_use_host_tb_crc(cfg, tb_bytes_count);
    if (use_host_tb_crc) {
      *h_tb_crc_pinned_ = compute_crc24a_host_byte_aligned(tb_bytes.data(), cfg.tb_size_bits >> 3U);
    }

    // Encode the complete TB and leave INT8 symbols resident for host or device-grid consumers.
    unsigned         num_symbols = cfg.num_coded_bits / cfg.modulation_order;
    nr_ldpc_status_t status      = encode_to_symbols_int8(tb_encoder, cfg, tb_bytes_count, use_host_tb_crc);
    if (status != NR_LDPC_SUCCESS) {
      return 0;
    }
    if (timing_enabled_) {
      cudaEventRecord(timing_encode_event_, stream_);
    }

    symbols_pinned_valid_ = false;
    if (!defer_symbol_download_ && !is_symbol_d2h_deferred()) {
      // Start D2H transfer NOW so it overlaps with CPU work.
      // Copy all symbols to pinned memory in one transfer.
      cudaMemcpyAsync(h_symbols_pinned_,
                      d_symbols_int8_,
                      num_symbols * 2, // 2 bytes per symbol (I + Q)
                      cudaMemcpyDeviceToHost,
                      stream_);
      symbols_pinned_valid_ = true;
    }
    if (timing_enabled_) {
      cudaEventRecord(timing_d2h_event_, stream_);
    }

    // Record event for completion tracking - get_symbols() can poll this.
    cudaEventRecord(completion_event_, stream_);

    num_symbols_            = num_symbols;
    pending_timing_nof_cbs_ = cfg.nof_codeblocks;
    timing_report_pending_  = timing_enabled_;
    return num_symbols;
  }

  unsigned encode_to_device_grid(span<const uint8_t> tb_bytes,
                                 const config&       cfg,
                                 const uint32_t*     d_re_offsets,
                                 uint16_t*           d_grid_bf16,
                                 unsigned            symbol_offset,
                                 unsigned            nof_re,
                                 unsigned            nof_ports,
                                 unsigned            nof_layers,
                                 unsigned            nof_grid_re_per_port,
                                 float               weight_real) override
  {
    if (!gpu_available_ || (d_re_offsets == nullptr) || (d_grid_bf16 == nullptr) || (nof_re == 0) || (nof_ports == 0) ||
        (nof_layers == 0) || (nof_grid_re_per_port == 0) || ((nof_layers > 1) && (nof_layers != nof_ports))) {
      return 0;
    }

    last_modulation_order_ = cfg.modulation_order;

    tb_encoder_config_t tb_cfg     = make_tb_encoder_config(cfg);
    tb_encoder_handle_t tb_encoder = get_tb_encoder_for_config(cfg, tb_cfg);
    if (tb_encoder == nullptr) {
      return 0;
    }

    unsigned tb_bytes_count = tb_bytes.size();
    unsigned num_symbols    = cfg.num_coded_bits / cfg.modulation_order;
    if ((tb_bytes_count > MAX_TB_BYTES) || (symbol_offset + nof_re * nof_layers > num_symbols)) {
      return 0;
    }

    if (!wait_for_tb_input_staging()) {
      return 0;
    }
    if (timing_enabled_) {
      cudaEventRecord(timing_start_event_, stream_);
    }
    std::memcpy(h_tb_input_pinned_, tb_bytes.data(), tb_bytes_count);
    if (cudaMemcpyAsync(d_tb_input_, h_tb_input_pinned_, tb_bytes_count, cudaMemcpyHostToDevice, stream_) !=
        cudaSuccess) {
      return 0;
    }
    if (!record_tb_input_copied()) {
      return 0;
    }
    if (timing_enabled_) {
      cudaEventRecord(timing_h2d_event_, stream_);
    }

    nr_ldpc_status_t status = tb_encoder_encode_to_bf16_grid(tb_encoder,
                                                             d_tb_input_,
                                                             d_re_offsets,
                                                             d_grid_bf16,
                                                             static_cast<int>(symbol_offset),
                                                             static_cast<int>(nof_re),
                                                             static_cast<int>(nof_ports),
                                                             static_cast<int>(nof_layers),
                                                             static_cast<int>(nof_grid_re_per_port),
                                                             weight_real,
                                                             stream_);
    if (status != NR_LDPC_SUCCESS) {
      return 0;
    }
    if (timing_enabled_) {
      cudaEventRecord(timing_encode_event_, stream_);
      cudaEventRecord(timing_d2h_event_, stream_);
    }

    symbols_pinned_valid_ = false;
    cudaEventRecord(completion_event_, stream_);

    num_symbols_            = num_symbols;
    pending_timing_nof_cbs_ = cfg.nof_codeblocks;
    timing_report_pending_  = timing_enabled_;
    return num_symbols;
  }

  void get_symbols(span<ci8_t> symbols, unsigned offset, unsigned count) override
  {
    if (!gpu_available_ || count == 0) {
      return;
    }

    // NOTE: Context switch removed - using inherited context.

    // Wait for GPU completion using event with yielding (RT-friendly).
    // Event was recorded at end of encode() after the optional full D2H copy.
    cudaEventSynchronizeYielding(completion_event_);
    collect_timing_stats();

    if (symbols_pinned_valid_) {
      // Data is already in pinned memory from the async D2H in encode().
      // Just copy the requested slice to output buffer.
      std::memcpy(symbols.data(),
                  h_symbols_pinned_ + offset * 2, // 2 bytes per symbol
                  count * sizeof(ci8_t));
      return;
    }

    cudaMemcpyAsync(h_symbols_pinned_ + offset * 2,
                    d_symbols_int8_ + offset * 2,
                    count * sizeof(ci8_t),
                    cudaMemcpyDeviceToHost,
                    stream_);
    cudaStreamSynchronizeYielding(stream_);
    std::memcpy(symbols.data(), h_symbols_pinned_ + offset * 2, count * sizeof(ci8_t));
    if (count == num_symbols_) {
      symbols_pinned_valid_ = true;
    }
  }

  void get_symbols_float(span<cf_t> symbols, unsigned offset, unsigned count) override
  {
    if (!gpu_available_ || count == 0) {
      return;
    }

    // NOTE: Context switch removed - using inherited context.

    // Copy directly - cuFloatComplex is layout-compatible with cf_t.
    static_assert(sizeof(cf_t) == sizeof(cuFloatComplex), "cf_t and cuFloatComplex must have same size");

    // Wait for completion and copy (RT-friendly yielding sync).
    cudaEventSynchronize(completion_event_);
    collect_timing_stats();
    cudaMemcpyAsync(symbols.data(),
                    reinterpret_cast<cuFloatComplex*>(d_symbols_) + offset,
                    count * sizeof(cuFloatComplex),
                    cudaMemcpyDeviceToHost,
                    stream_);
    cudaStreamSynchronizeYielding(stream_);
  }

  void* get_device_symbols() const override { return d_symbols_; }

  const int8_t* get_device_symbols_int8() const override { return d_symbols_int8_; }

  void* get_execution_context() const override { return stream_; }

  void synchronize() override
  {
    if (!gpu_available_) {
      return;
    }
    cudaEventSynchronizeYielding(completion_event_);
    collect_timing_stats();
  }

  unsigned get_num_symbols() const override { return num_symbols_; }

  const timing_stats& get_last_timing_stats() const override { return last_timing_stats_; }

  bool is_gpu_available() const override { return gpu_available_; }

  void set_defer_symbol_download(bool defer) override { defer_symbol_download_ = defer; }

private:
  bool wait_for_tb_input_staging()
  {
    if (!tb_input_copy_pending_) {
      return true;
    }
    if (cudaEventSynchronizeYielding(tb_input_copied_event_) != cudaSuccess) {
      return false;
    }
    tb_input_copy_pending_ = false;
    return true;
  }

  bool record_tb_input_copied()
  {
    if (cudaEventRecord(tb_input_copied_event_, stream_) != cudaSuccess) {
      return false;
    }
    tb_input_copy_pending_ = true;
    return true;
  }

  void record_tb_input_copied_after_graph()
  {
    // Event nodes are not reliable in the opt-in full-encode graph on all CUDA versions. Recording after the graph
    // preserves staging-buffer correctness for that experimental path, at the cost of waiting for the whole graph
    // before the next submission can overwrite the pinned input. The default non-graph path records immediately after
    // H2D and retains encode overlap.
    if (!record_tb_input_copied()) {
      (void)cudaStreamSynchronizeYielding(stream_);
      tb_input_copy_pending_ = false;
    }
  }

  static bool are_encode_graph_keys_equal(const encode_graph_key& left, const encode_graph_key& right)
  {
    return (left.cfg == right.cfg) && (left.tb_encoder == right.tb_encoder) &&
           (left.d_tb_input == right.d_tb_input) && (left.d_tb_crc == right.d_tb_crc) &&
           (left.d_symbols_int8 == right.d_symbols_int8) && (left.stream == right.stream) &&
           (left.tb_bytes_count == right.tb_bytes_count) && (left.host_tb_crc == right.host_tb_crc);
  }

  void destroy_encode_graph_entry(encode_graph_entry& entry)
  {
    if (entry.exec != nullptr) {
      cudaGraphExecDestroy(entry.exec);
      entry.exec = nullptr;
    }
    if (entry.graph != nullptr) {
      cudaGraphDestroy(entry.graph);
      entry.graph = nullptr;
    }
    entry.key         = {};
    entry.use_counter = 0;
  }

  void destroy_encode_graphs_for_encoder(tb_encoder_handle_t tb_encoder)
  {
    std::lock_guard<std::mutex> lock(encode_graph_mutex_);
    for (encode_graph_entry& entry : encode_graphs_) {
      if ((tb_encoder == nullptr) || (entry.key.tb_encoder == tb_encoder)) {
        destroy_encode_graph_entry(entry);
      }
    }
    encode_graphs_.erase(std::remove_if(encode_graphs_.begin(),
                                        encode_graphs_.end(),
                                        [](const encode_graph_entry& entry) { return entry.exec == nullptr; }),
                         encode_graphs_.end());
    if ((tb_encoder == nullptr) || (pending_encode_graph_key_.tb_encoder == tb_encoder)) {
      pending_encode_graph_key_       = {};
      pending_encode_graph_key_valid_ = false;
    }
    if (tb_encoder == nullptr) {
      encode_graph_capture_failed_ = false;
      encode_graph_use_counter_    = 0;
    }
  }

  encode_graph_entry* find_encode_graph_entry_locked(const encode_graph_key& key)
  {
    for (encode_graph_entry& entry : encode_graphs_) {
      if ((entry.exec != nullptr) && are_encode_graph_keys_equal(entry.key, key)) {
        return &entry;
      }
    }
    return nullptr;
  }

  encode_graph_entry* reserve_encode_graph_entry_locked()
  {
    if (encode_graphs_.size() < PDSCH_FULL_ENCODE_GRAPH_CACHE_LIMIT) {
      encode_graphs_.push_back({});
      return &encode_graphs_.back();
    }

    encode_graph_entry* victim = &encode_graphs_.front();
    for (encode_graph_entry& entry : encode_graphs_) {
      if (entry.use_counter < victim->use_counter) {
        victim = &entry;
      }
    }
    destroy_encode_graph_entry(*victim);
    return victim;
  }

  bool launch_cached_encode_graph(const encode_graph_key& key)
  {
    std::lock_guard<std::mutex> lock(encode_graph_mutex_);
    encode_graph_entry*         entry = find_encode_graph_entry_locked(key);
    if (entry == nullptr) {
      return false;
    }
    if (cudaGraphLaunch(entry->exec, key.stream) == cudaSuccess) {
      entry->use_counter = ++encode_graph_use_counter_;
      record_tb_input_copied_after_graph();
      return true;
    }

    destroy_encode_graph_entry(*entry);
    (void)cudaGetLastError();
    return false;
  }

  bool should_capture_encode_graph(const encode_graph_key& key)
  {
    std::lock_guard<std::mutex> lock(encode_graph_mutex_);
    if (encode_graph_capture_failed_) {
      return false;
    }
    if (pending_encode_graph_key_valid_ && are_encode_graph_keys_equal(pending_encode_graph_key_, key)) {
      return true;
    }

    pending_encode_graph_key_       = key;
    pending_encode_graph_key_valid_ = true;
    return false;
  }

  void mark_encode_graph_capture_failed()
  {
    std::lock_guard<std::mutex> lock(encode_graph_mutex_);
    encode_graph_capture_failed_    = true;
    pending_encode_graph_key_       = {};
    pending_encode_graph_key_valid_ = false;
  }

  nr_ldpc_status_t enqueue_encode_to_symbols_int8(tb_encoder_handle_t tb_encoder,
                                                  unsigned            tb_bytes_count,
                                                  bool                use_host_tb_crc,
                                                  bool                record_h2d_event,
                                                  bool                track_tb_input_copy = true)
  {
    if (cudaMemcpyAsync(d_tb_input_, h_tb_input_pinned_, tb_bytes_count, cudaMemcpyHostToDevice, stream_) !=
        cudaSuccess) {
      return NR_LDPC_ERROR_CUDA_FAILED;
    }
    if (use_host_tb_crc &&
        (cudaMemcpyAsync(d_tb_crc_, h_tb_crc_pinned_, sizeof(uint32_t), cudaMemcpyHostToDevice, stream_) !=
         cudaSuccess)) {
      return NR_LDPC_ERROR_CUDA_FAILED;
    }
    if (track_tb_input_copy && !record_tb_input_copied()) {
      return NR_LDPC_ERROR_CUDA_FAILED;
    }
    if (record_h2d_event) {
      cudaEventRecord(timing_h2d_event_, stream_);
    }

    return use_host_tb_crc ? tb_encoder_encode_to_symbols_int8_precomputed_crc(
                                 tb_encoder, d_tb_input_, d_tb_crc_, d_symbols_int8_, stream_)
                           : tb_encoder_encode_to_symbols_int8(tb_encoder, d_tb_input_, d_symbols_int8_, stream_);
  }

  nr_ldpc_status_t capture_and_launch_encode_graph(const encode_graph_key& key)
  {
    if (cudaStreamBeginCapture(key.stream, cudaStreamCaptureModeThreadLocal) != cudaSuccess) {
      (void)cudaGetLastError();
      mark_encode_graph_capture_failed();
      return enqueue_encode_to_symbols_int8(key.tb_encoder, key.tb_bytes_count, key.host_tb_crc, false);
    }

    nr_ldpc_status_t capture_status =
        enqueue_encode_to_symbols_int8(key.tb_encoder, key.tb_bytes_count, key.host_tb_crc, false, false);

    cudaGraph_t captured_graph = nullptr;
    cudaError_t end_status     = cudaStreamEndCapture(key.stream, &captured_graph);
    if ((capture_status != NR_LDPC_SUCCESS) || (end_status != cudaSuccess) || (captured_graph == nullptr)) {
      if (captured_graph != nullptr) {
        cudaGraphDestroy(captured_graph);
      }
      (void)cudaGetLastError();
      mark_encode_graph_capture_failed();
      return enqueue_encode_to_symbols_int8(key.tb_encoder, key.tb_bytes_count, key.host_tb_crc, false);
    }

    cudaGraphExec_t graph_exec = nullptr;
    if (cudaGraphInstantiate(&graph_exec, captured_graph, nullptr, nullptr, 0) != cudaSuccess) {
      cudaGraphDestroy(captured_graph);
      (void)cudaGetLastError();
      mark_encode_graph_capture_failed();
      return enqueue_encode_to_symbols_int8(key.tb_encoder, key.tb_bytes_count, key.host_tb_crc, false);
    }

    {
      std::lock_guard<std::mutex> lock(encode_graph_mutex_);
      encode_graph_entry*         entry = reserve_encode_graph_entry_locked();
      entry->key                        = key;
      entry->graph                      = captured_graph;
      entry->exec                       = graph_exec;
      entry->use_counter                = ++encode_graph_use_counter_;
      pending_encode_graph_key_valid_   = false;

      if (cudaGraphLaunch(entry->exec, key.stream) == cudaSuccess) {
        record_tb_input_copied_after_graph();
        return NR_LDPC_SUCCESS;
      }

      destroy_encode_graph_entry(*entry);
    }

    (void)cudaGetLastError();
    return enqueue_encode_to_symbols_int8(key.tb_encoder, key.tb_bytes_count, key.host_tb_crc, false);
  }

  nr_ldpc_status_t encode_to_symbols_int8(tb_encoder_handle_t tb_encoder,
                                          const config&       cfg,
                                          unsigned            tb_bytes_count,
                                          bool                use_host_tb_crc)
  {
    if (!are_pdsch_cuda_graphs_enabled() || !are_full_encode_graphs_enabled() || timing_enabled_ ||
        (stream_ == nullptr)) {
      return enqueue_encode_to_symbols_int8(tb_encoder, tb_bytes_count, use_host_tb_crc, timing_enabled_);
    }

    encode_graph_key key = {};
    key.cfg              = cfg;
    key.tb_encoder       = tb_encoder;
    key.d_tb_input       = d_tb_input_;
    key.d_tb_crc         = d_tb_crc_;
    key.d_symbols_int8   = d_symbols_int8_;
    key.stream           = stream_;
    key.tb_bytes_count   = tb_bytes_count;
    key.host_tb_crc      = use_host_tb_crc;

    if (launch_cached_encode_graph(key)) {
      return NR_LDPC_SUCCESS;
    }
    if (!should_capture_encode_graph(key)) {
      return enqueue_encode_to_symbols_int8(tb_encoder, tb_bytes_count, use_host_tb_crc, false);
    }

    std::lock_guard<std::mutex> capture_lock(pdsch_full_encode_graph_capture_mutex());
    if (launch_cached_encode_graph(key)) {
      return NR_LDPC_SUCCESS;
    }
    return capture_and_launch_encode_graph(key);
  }

  void destroy_resident_tb_encoder()
  {
    if (resident_tb_encoder_ != nullptr) {
      destroy_encode_graphs_for_encoder(resident_tb_encoder_);
      tb_encoder_destroy(resident_tb_encoder_);
      resident_tb_encoder_ = nullptr;
    }
    resident_cfg_       = {};
    resident_cfg_valid_ = false;
  }

  /// Resident lane: reconfigure in place only — never create/destroy on the hot path.
  tb_encoder_handle_t get_tb_encoder_for_config(const config& cfg, const tb_encoder_config_t& tb_cfg)
  {
    if (resident_tb_encoder_ == nullptr) {
      ocudulog::fetch_basic_logger("PHY").error("CUDA PDSCH: resident TB encoder missing (lane not initialized)");
      return nullptr;
    }

    const bool log_small = cfg.tb_size_bits <= 128;
    const auto t0        = std::chrono::steady_clock::now();

    // Exact match including RNTI / n_ID: nothing to do.
    if (resident_cfg_valid_ && (resident_cfg_ == cfg)) {
      if (log_small) {
        const double us =
            std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
        ocudulog::fetch_basic_logger("PHY").info(
            "CUDA PDSCH resident HIT exact rnti=0x{:x} n_id={} Z={} F={} G={} us={:.1f}",
            cfg.n_rnti,
            cfg.n_id,
            cfg.lifting_size,
            cfg.nof_filler_bits,
            cfg.num_coded_bits,
            us);
      }
      return resident_tb_encoder_;
    }

    // Same LDPC/RM/mod shape: only scrambling identity (RNTI / n_ID / q).
    if (resident_cfg_valid_ && same_encode_shape(resident_cfg_, cfg)) {
      if (tb_encoder_reconfigure_scrambling(resident_tb_encoder_, cfg.n_rnti, cfg.n_id, cfg.cw_index) !=
          NR_LDPC_SUCCESS) {
        ocudulog::fetch_basic_logger("PHY").error(
            "CUDA PDSCH resident scramble reconfigure failed rnti=0x{:x} n_id={}", cfg.n_rnti, cfg.n_id);
        return nullptr;
      }
      resident_cfg_ = cfg;
      if (log_small) {
        const double us =
            std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
        ocudulog::fetch_basic_logger("PHY").info(
            "CUDA PDSCH resident HIT shape+scramble rnti=0x{:x} n_id={} Z={} F={} G={} us={:.1f}",
            cfg.n_rnti,
            cfg.n_id,
            cfg.lifting_size,
            cfg.nof_filler_bits,
            cfg.num_coded_bits,
            us);
      }
      return resident_tb_encoder_;
    }

    // Shape change: reconfigure in place (no create, no buffer grow when RT-locked).
    if (tb_encoder_configure(resident_tb_encoder_, &tb_cfg) != NR_LDPC_SUCCESS) {
      ocudulog::fetch_basic_logger("PHY").error(
          "CUDA PDSCH resident configure failed rnti=0x{:x} n_id={} Z={} F={} G={} (shape exceeds max or RT lock)",
          cfg.n_rnti,
          cfg.n_id,
          cfg.lifting_size,
          cfg.nof_filler_bits,
          cfg.num_coded_bits);
      return nullptr;
    }
    resident_cfg_       = cfg;
    resident_cfg_valid_ = true;
    if (log_small) {
      const double us =
          std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
      ocudulog::fetch_basic_logger("PHY").info(
          "CUDA PDSCH resident RECONFIG shape rnti=0x{:x} n_id={} Z={} F={} G={} us={:.1f}",
          cfg.n_rnti,
          cfg.n_id,
          cfg.lifting_size,
          cfg.nof_filler_bits,
          cfg.num_coded_bits,
          us);
    }
    return resident_tb_encoder_;
  }

  void collect_timing_stats()
  {
    if (!timing_enabled_) {
      return;
    }
    if (!timing_report_pending_) {
      return;
    }
    timing_report_pending_ = false;

    float h2d_ms    = 0.0F;
    float encode_ms = 0.0F;
    float d2h_ms    = 0.0F;
    float total_ms  = 0.0F;
    cudaEventElapsedTime(&h2d_ms, timing_start_event_, timing_h2d_event_);
    cudaEventElapsedTime(&encode_ms, timing_h2d_event_, timing_encode_event_);
    cudaEventElapsedTime(&d2h_ms, timing_encode_event_, timing_d2h_event_);
    cudaEventElapsedTime(&total_ms, timing_start_event_, timing_d2h_event_);

    last_timing_stats_.valid       = true;
    last_timing_stats_.enabled     = true;
    last_timing_stats_.h2d_us      = h2d_ms * 1000.0F;
    last_timing_stats_.encode_us   = encode_ms * 1000.0F;
    last_timing_stats_.d2h_us      = d2h_ms * 1000.0F;
    last_timing_stats_.total_us    = total_ms * 1000.0F;
    last_timing_stats_.nof_cbs     = pending_timing_nof_cbs_;
    last_timing_stats_.nof_symbols = num_symbols_;

    std::fprintf(stderr,
                 "CUDA PDSCH timing: cbs=%u symbols=%u h2d=%.1fus encode=%.1fus d2h=%.1fus total=%.1fus\n",
                 last_timing_stats_.nof_cbs,
                 last_timing_stats_.nof_symbols,
                 last_timing_stats_.h2d_us,
                 last_timing_stats_.encode_us,
                 last_timing_stats_.d2h_us,
                 last_timing_stats_.total_us);
  }

  void cleanup()
  {
    if (h_symbols_pinned_) {
      cudaFreeHost(h_symbols_pinned_);
      h_symbols_pinned_ = nullptr;
    }
    if (h_tb_input_pinned_) {
      cudaFreeHost(h_tb_input_pinned_);
      h_tb_input_pinned_ = nullptr;
    }
    if (h_tb_crc_pinned_) {
      cudaFreeHost(h_tb_crc_pinned_);
      h_tb_crc_pinned_ = nullptr;
    }
    if (d_symbols_int8_) {
      cudaFree(d_symbols_int8_);
      d_symbols_int8_ = nullptr;
    }
    if (d_symbols_) {
      cudaFree(d_symbols_);
      d_symbols_ = nullptr;
    }
    if (d_coded_bits_) {
      cudaFree(d_coded_bits_);
      d_coded_bits_ = nullptr;
    }
    if (d_tb_crc_) {
      cudaFree(d_tb_crc_);
      d_tb_crc_ = nullptr;
    }
    if (d_tb_input_) {
      cudaFree(d_tb_input_);
      d_tb_input_ = nullptr;
    }
    if (completion_event_) {
      cudaEventDestroy(completion_event_);
      completion_event_ = nullptr;
    }
    if (tb_input_copied_event_) {
      cudaEventDestroy(tb_input_copied_event_);
      tb_input_copied_event_ = nullptr;
    }
    if (timing_start_event_) {
      cudaEventDestroy(timing_start_event_);
      timing_start_event_ = nullptr;
    }
    if (timing_h2d_event_) {
      cudaEventDestroy(timing_h2d_event_);
      timing_h2d_event_ = nullptr;
    }
    if (timing_encode_event_) {
      cudaEventDestroy(timing_encode_event_);
      timing_encode_event_ = nullptr;
    }
    if (timing_d2h_event_) {
      cudaEventDestroy(timing_d2h_event_);
      timing_d2h_event_ = nullptr;
    }
    if (stream_) {
      cudaStreamDestroy(stream_);
      stream_ = nullptr;
    }
    if (context_) {
      cuDevicePrimaryCtxRelease(device_);
      context_ = nullptr;
    }
    gpu_available_ = false;
  }

  bool gpu_available_ = false;

  // CUDA context and stream.
  CUdevice     device_              = 0;
  CUcontext    context_             = nullptr;
  cudaStream_t stream_              = nullptr;
  cudaEvent_t  completion_event_    = nullptr; // Event for async completion tracking
  cudaEvent_t  tb_input_copied_event_ = nullptr;
  cudaEvent_t  timing_start_event_  = nullptr;
  cudaEvent_t  timing_h2d_event_    = nullptr;
  cudaEvent_t  timing_encode_event_ = nullptr;
  cudaEvent_t  timing_d2h_event_    = nullptr;

  // Device memory.
  uint8_t*  d_tb_input_     = nullptr;
  uint32_t* d_tb_crc_       = nullptr;
  uint8_t*  d_coded_bits_   = nullptr;
  void*     d_symbols_      = nullptr;
  int8_t*   d_symbols_int8_ = nullptr; // GPU buffer for int8 symbols

  // Pinned host memory for fast D2H transfer.
  int8_t*   h_symbols_pinned_  = nullptr;
  uint8_t*  h_tb_input_pinned_ = nullptr;
  uint32_t* h_tb_crc_pinned_   = nullptr;

  // CUDA handles.
  /// One resident TB encoder per lane (created at startup, never on the slot path).
  tb_encoder_handle_t resident_tb_encoder_ = nullptr;
  config              resident_cfg_        = {};
  bool                resident_cfg_valid_  = false;
  std::mutex                      encode_graph_mutex_;
  std::vector<encode_graph_entry> encode_graphs_;
  encode_graph_key                pending_encode_graph_key_;
  bool                            pending_encode_graph_key_valid_ = false;
  bool                            encode_graph_capture_failed_    = false;
  uint64_t                        encode_graph_use_counter_       = 0;
  modulator_handle_t              modulator_                      = nullptr;
  scrambler_handle_t              scrambler_                      = nullptr;

  // Output info.
  unsigned     num_symbols_            = 0;
  unsigned     last_modulation_order_  = 2; // Default to QPSK
  bool         symbols_pinned_valid_   = false;
  bool         defer_symbol_download_  = false;
  bool         timing_enabled_         = false;
  bool         timing_report_pending_  = false;
  bool         tb_input_copy_pending_  = false;
  unsigned     pending_timing_nof_cbs_ = 0;
  timing_stats last_timing_stats_;

  // Cached scrambling config for sequence reuse.
};

} // namespace

std::unique_ptr<pdsch_tb_encoder_gpu> ocudu::create_pdsch_tb_encoder_cuda()
{
  auto encoder = std::make_unique<pdsch_tb_encoder_gpu_impl>();
  if (!encoder->is_gpu_available()) {
    return nullptr;
  }
  return encoder;
}

bool ocudu::is_pdsch_tb_encoder_gpu_available()
{
  CUresult res = cuInit(0);
  if (res != CUDA_SUCCESS) {
    return false;
  }

  int device_count = 0;
  res              = cuDeviceGetCount(&device_count);
  return (res == CUDA_SUCCESS) && (device_count > 0);
}
