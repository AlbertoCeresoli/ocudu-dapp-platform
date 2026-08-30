// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: LicenseRef-DeepSig-Proprietary
//
// ocudu channel-emulation plugin — version 1 C ABI.
//
// HOST-SIDE COPY. The master copy of this contract lives in the
// ocudu-channel-plugin repository (include/ocudu_channel/plugin_api_v1.h);
// keep the two byte-identical below this comment block. Version-1 structs
// are append-only.
//
// The host (ocudu gNB) dlopens the plugin shared object, resolves
// ocudu_channel_plugin_get_api_v1(), and drives everything through the
// returned vtable. The host never sees CUDA types or channel math: it hands
// the plugin a device pointer to a resource-grid slot and a cudaStream_t
// (passed as void*), and the plugin multiplies its current channel
// realization onto the grid in place. Generation, throttling and buffering
// are internal to the plugin.
//
// ABI rules: plain C, no ownership transfer of host memory, every struct
// carries its size for forward compatibility, all functions are
// thread-compatible (apply() may be called concurrently for UL and DL).

#ifndef OCUDU_CHANNEL_PLUGIN_API_V1_H
#define OCUDU_CHANNEL_PLUGIN_API_V1_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Opaque plugin instance.
typedef struct ocudu_channel_plugin ocudu_channel_plugin_t;

// Direction of the grid being processed.
typedef enum ocudu_channel_direction {
  OCUDU_CHANNEL_UPLINK   = 0, // grid produced by the lower PHY from radio RX
  OCUDU_CHANNEL_DOWNLINK = 1, // grid about to be modulated toward the radio
} ocudu_channel_direction_t;

// Complex sample format of the grid memory handed to apply().
typedef enum ocudu_channel_sample_format {
  OCUDU_CHANNEL_FORMAT_CF32   = 0, // interleaved float re,im
  OCUDU_CHANNEL_FORMAT_CBF16  = 1, // interleaved bfloat16 re,im
} ocudu_channel_sample_format_t;

// Static configuration, filled by the host from its YAML config.
typedef struct ocudu_channel_plugin_config_v1 {
  uint32_t struct_size; // = sizeof(ocudu_channel_plugin_config_v1)

  // --- grid geometry (fixed at cell start) ---
  uint32_t nof_subcarriers;      // occupied subcarriers (N_RB * 12)
  uint32_t nof_symbols_per_slot; // 14 for normal CP
  uint32_t nof_rx_ports;         // gNB receive ports to emulate
  uint32_t nof_tx_ports;         // gNB transmit ports to emulate
  double   carrier_frequency_hz;
  double   subcarrier_spacing_hz;
  double   slot_duration_s;      // e.g. 5e-4 at 30 kHz SCS

  // --- model selection ---
  // "passthrough", "uma", "umi", "rma", "tdl_a", "tdl_b", "tdl_c",
  // "cdl_a", "cdl_b", "cdl_c"
  const char* model;
  double   delay_spread_s;  // <=0: model default (TDL/CDL: 100 ns)
  double   ue_speed_kmh;    // Doppler; 0 = static channel
  uint64_t seed;            // reproducible realizations
  double   bulk_gain_db;    // deliberate flat gain on top of unit-power H
  double   dl_backoff_db;   // extra DL-only backoff to protect the DAC

  // --- update policy ---
  uint32_t update_period_slots; // recompute H every N slots (>=1)
  double   drop_period_s;       // re-draw large-scale topology; 0 = never

  // --- noise injection ---
  double   noise_floor_db; // AWGN power per RE in dB rel. unit RE; NAN = off
  uint8_t  noise_ul;       // inject on UL grids (default expected: 1)
  uint8_t  noise_dl;       // inject on DL grids (default expected: 0)

  int32_t  cuda_device; // device the grids live on
} ocudu_channel_plugin_config_v1_t;

// Layout of one slot's worth of grid memory handed to apply().
// The grid is (port, symbol, subcarrier), strides in SAMPLES (complex).
typedef struct ocudu_channel_grid_view_v1 {
  uint32_t struct_size; // = sizeof(ocudu_channel_grid_view_v1)

  void*    data;           // DEVICE pointer to first RE (port 0, symbol 0, sc 0)
  int32_t  format;         // ocudu_channel_sample_format_t
  uint32_t nof_ports;
  uint32_t nof_symbols;    // symbols covered by this view
  uint32_t nof_subcarriers;
  size_t   port_stride;    // samples between ports
  size_t   symbol_stride;  // samples between symbols
  uint32_t first_symbol;   // slot-relative index of the first symbol in view
  // Optional full-slot owned device buffer. When non-null the apply kernel
  // also stores the faded RE (from registers) at
  //   port * snapshot_port_stride + (first_symbol + sym) * snapshot_symbol_stride + sc
  // Callers with the original struct_size leave these unread.
  void*    snapshot;
  size_t   snapshot_port_stride;
  size_t   snapshot_symbol_stride;
  // Optional mapped-pinned host buffer (device pointer from
  // cudaHostGetDevicePointer). Same layout as snapshot. Written from the
  // same registers as the in-place store — no cudaMemcpy D2H.
  void*    snapshot_host;
  size_t   snapshot_host_port_stride;
  size_t   snapshot_host_symbol_stride;
} ocudu_channel_grid_view_v1_t;

typedef struct ocudu_channel_plugin_api_v1 {
  uint32_t struct_size; // = sizeof(ocudu_channel_plugin_api_v1)
  uint32_t abi_version; // = 1

  // Create an instance. Returns NULL on error; *error_msg (static storage,
  // valid until next call on this thread) describes why.
  ocudu_channel_plugin_t* (*create)(const ocudu_channel_plugin_config_v1_t* cfg,
                                    const char** error_msg);

  void (*destroy)(ocudu_channel_plugin_t*);

  // Multiply the current channel realization onto `view` in place and
  // (optionally) add noise, on `cuda_stream` (a cudaStream_t). `slot_count`
  // is a monotonically increasing absolute slot index used for the Doppler
  // time base and the noise RNG counter. Asynchronous: enqueues device work
  // only, never blocks the host. Returns 0 on success, nonzero on error
  // (host should fail open: an error means the grid was left untouched).
  int (*apply)(ocudu_channel_plugin_t*,
               const ocudu_channel_grid_view_v1_t* view,
               int32_t direction, // ocudu_channel_direction_t
               uint64_t slot_count,
               void* cuda_stream);

  // One-line human-readable status: model, generation count, last update.
  // Static storage, refreshed per call.
  const char* (*describe)(ocudu_channel_plugin_t*);

  // Runtime knob updates (thread-safe, take effect next update window):
  // returns 0 on success. Supported keys: "noise_floor_db", "ue_speed_kmh",
  // "bulk_gain_db", "update_period_slots", "enabled" (0/1), "model",
  // "delay_spread_ns", "los" (UMa/UMi/RMa 38.901 LOS), "drop_now".
  int (*set_param)(ocudu_channel_plugin_t*, const char* key, double value);

  // Returns the plugin-owned cudaStream_t (as void*) used for apply() work
  // in the given direction. Hosts that do not manage CUDA streams of their
  // own pass this stream BOTH to apply() and to their grid's device-ordering
  // API (wait-producers / record-consumers), so the multiply is correctly
  // fenced without the host linking CUDA at all.
  void* (*get_apply_stream)(ocudu_channel_plugin_t*, int32_t direction);

  // PRACH occasion apply: like apply(), but the view's REs are mapped onto
  // the realization's frequency axis explicitly — H subcarrier index for RE
  // j is round(h_subc_first + j*h_subc_step), and the single H row
  // `view->first_symbol` covers the whole occasion. Uplink semantics
  // (uplink noise/gain policy). The PRACH buffer layout is expressed
  // through the same grid view struct.
  int (*apply_prach)(ocudu_channel_plugin_t*,
                     const ocudu_channel_grid_view_v1_t* view,
                     uint64_t slot_count,
                     void*    cuda_stream,
                     double   h_subc_first,
                     double   h_subc_step);
} ocudu_channel_plugin_api_v1_t;

// The single exported symbol.
//   const ocudu_channel_plugin_api_v1_t* ocudu_channel_plugin_get_api_v1(void);
#define OCUDU_CHANNEL_PLUGIN_ENTRY_SYMBOL "ocudu_channel_plugin_get_api_v1"

#ifdef __cplusplus
} // extern "C"
#endif

#endif // OCUDU_CHANNEL_PLUGIN_API_V1_H
