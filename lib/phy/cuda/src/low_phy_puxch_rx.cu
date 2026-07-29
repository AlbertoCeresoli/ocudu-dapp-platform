// SPDX-FileCopyrightText: Copyright (C) 2021-2026 DeepSig Inc
// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "low_phy_puxch_rx.h"
#include "vkFFT.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cuComplex.h>
#include <cuda.h>
#include <cuda_runtime.h>
#include <mutex>
#include <sched.h>
#include <vector>

#ifndef OCUDU_PHY_CUDA_ENABLE_VKFFT
#error "OCUDU PHY CUDA low-PHY PUxCH RX requires the bundled VkFFT backend."
#endif

namespace {

static constexpr int    THREADS                  = 256;
static constexpr int    HOST_STAGING_SLOTS       = 32;
static constexpr size_t DEVICE_GRAPH_CACHE_LIMIT = 256;

static bool check_cuda(cudaError_t err)
{
  return err == cudaSuccess;
}

static cudaError_t create_realtime_stream(cudaStream_t* stream)
{
  int         least_priority    = 0;
  int         greatest_priority = 0;
  cudaError_t status            = cudaDeviceGetStreamPriorityRange(&least_priority, &greatest_priority);
  if (status == cudaSuccess) {
    status = cudaStreamCreateWithPriority(stream, cudaStreamNonBlocking, greatest_priority);
    if (status == cudaSuccess) {
      return cudaSuccess;
    }
    (void)cudaGetLastError();
  }

  return cudaStreamCreateWithFlags(stream, cudaStreamNonBlocking);
}

static bool env_flag_enabled(const char* name)
{
  const char* value = std::getenv(name);
  if (value == nullptr) {
    return false;
  }
  return (std::strcmp(value, "1") == 0) || (std::strcmp(value, "true") == 0) || (std::strcmp(value, "on") == 0) ||
         (std::strcmp(value, "yes") == 0) || (std::strcmp(value, "enabled") == 0);
}

static bool env_flag_enabled_or_default(const char* name, bool default_value)
{
  const char* value = std::getenv(name);
  if (value == nullptr) {
    return default_value;
  }
  if ((std::strcmp(value, "1") == 0) || (std::strcmp(value, "true") == 0) || (std::strcmp(value, "on") == 0) ||
      (std::strcmp(value, "yes") == 0) || (std::strcmp(value, "enabled") == 0)) {
    return true;
  }
  if ((std::strcmp(value, "0") == 0) || (std::strcmp(value, "false") == 0) || (std::strcmp(value, "off") == 0) ||
      (std::strcmp(value, "no") == 0) || (std::strcmp(value, "disabled") == 0)) {
    return false;
  }
  return default_value;
}

static unsigned env_unsigned_or_default(const char* name, unsigned default_value, unsigned max_value)
{
  const char* value = std::getenv(name);
  if ((value == nullptr) || (*value == '\0')) {
    return default_value;
  }

  char*         end    = nullptr;
  unsigned long parsed = std::strtoul(value, &end, 10);
  if (end == value) {
    return default_value;
  }
  return static_cast<unsigned>(std::min<unsigned long>(parsed, max_value));
}

// Integrated GPUs (e.g. GB10) share host DRAM. Mapped pinned staging lets the
// load kernel read the packed FFT windows without an explicit H2D copy.
static bool device_is_integrated()
{
  static const bool integrated = []() {
    int value = 0;
    return (cudaDeviceGetAttribute(&value, cudaDevAttrIntegrated, 0) == cudaSuccess) && (value != 0);
  }();
  return integrated;
}

static bool zero_copy_staging_enabled()
{
  // Default on for integrated devices; force with =1, disable with =0.
  static const bool enabled =
      env_flag_enabled_or_default("OCUDU_LOWPHY_RX_ZERO_COPY_STAGING", device_is_integrated());
  return enabled;
}

static bool runtime_host_registration_enabled()
{
  // Default on for integrated zero-copy so scattered mapped loads can skip host packing.
  // Discrete GPUs stay opt-in: multi H2D without packing is often slower than one staged copy.
  // Force with =1, disable with =0.
  static const bool enabled =
      env_flag_enabled_or_default("OCUDU_LOWPHY_RX_RUNTIME_HOST_REGISTRATION",
                                  zero_copy_staging_enabled() && device_is_integrated());
  return enabled;
}

// Scattered zero-copy: load kernel reads each symbol×port window from its own
// mapped host pointer (no intermediate pack). Default on with integrated zero-copy.
static bool scattered_zero_copy_enabled()
{
  static const bool enabled =
      env_flag_enabled_or_default("OCUDU_LOWPHY_RX_SCATTERED_ZERO_COPY",
                                  zero_copy_staging_enabled() && device_is_integrated());
  return enabled;
}

static bool cuda_graphs_enabled()
{
  static const bool enabled = env_flag_enabled("OCUDU_LOWPHY_RX_CUDA_GRAPHS");
  return enabled;
}

static size_t warmup_output_words(const ocudu_lowphy_puxch_rx_config_t& cfg)
{
  return std::max(static_cast<size_t>(cfg.nof_ports) * cfg.rg_size,
                  static_cast<size_t>(cfg.nof_ports) * cfg.grid_nof_symbols * cfg.grid_nof_subc);
}

static int active_nof_symbols(int value)
{
  return std::max(1, value);
}

static std::mutex& graph_capture_mutex()
{
  static std::mutex mutex;
  return mutex;
}

__device__ __forceinline__ uint16_t float_to_bf16(float value)
{
  uint32_t bits = __float_as_uint(value);
  bits += 0x7fffu + ((bits >> 16) & 1u);
  return static_cast<uint16_t>(bits >> 16);
}

__device__ __forceinline__ uint32_t cfloat_to_cbf16(cuFloatComplex value)
{
  return static_cast<uint32_t>(float_to_bf16(value.x)) | (static_cast<uint32_t>(float_to_bf16(value.y)) << 16);
}

__device__ __forceinline__ cuFloatComplex cmul(cuFloatComplex a, cuFloatComplex b)
{
  return make_cuFloatComplex(a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x);
}

// Compact extract parameters: the full config is ~370B and is too heavy to pass by value
// on every launch/graph update. Keep only what the extract kernels need.
struct extract_kernel_params {
  int   dft_size;
  int   rg_size;
  int   grid_nof_subc;
  int   grid_nof_symbols;
  int   nof_ports;
  int   symbol_index;
  int   use_symbol_phases;
  int   port_indices[OCUDU_LOWPHY_PUXCH_RX_MAX_PORTS];
  float phase_scale_re; // phase_re * dft_scale
  float phase_scale_im;
  float symbol_phase_scale_re[OCUDU_LOWPHY_PUXCH_RX_MAX_SYMBOLS];
  float symbol_phase_scale_im[OCUDU_LOWPHY_PUXCH_RX_MAX_SYMBOLS];
};

static extract_kernel_params make_extract_params(const ocudu_lowphy_puxch_rx_config_t& cfg)
{
  extract_kernel_params p{};
  p.dft_size         = cfg.dft_size;
  p.rg_size          = cfg.rg_size;
  p.grid_nof_subc    = cfg.grid_nof_subc;
  p.grid_nof_symbols = cfg.grid_nof_symbols;
  p.nof_ports        = cfg.nof_ports;
  p.symbol_index     = cfg.symbol_index;
  p.use_symbol_phases = cfg.use_symbol_phases;
  p.phase_scale_re   = cfg.phase_re * cfg.dft_scale;
  p.phase_scale_im   = cfg.phase_im * cfg.dft_scale;
  for (int port = 0; port < OCUDU_LOWPHY_PUXCH_RX_MAX_PORTS; ++port) {
    p.port_indices[port] = cfg.port_indices[port];
  }
  if (cfg.use_symbol_phases != 0) {
    for (int s = 0; s < OCUDU_LOWPHY_PUXCH_RX_MAX_SYMBOLS; ++s) {
      p.symbol_phase_scale_re[s] = cfg.symbol_phase_re[s] * cfg.dft_scale;
      p.symbol_phase_scale_im[s] = cfg.symbol_phase_im[s] * cfg.dft_scale;
    }
  }
  return p;
}

__global__ void __launch_bounds__(THREADS) load_puxch_fft_input_ci16_kernel(cuFloatComplex* __restrict__ d_freq,
                                                                            const int16_t* __restrict__ d_input,
                                                                            int   dft_size,
                                                                            int   input_nof_samples,
                                                                            int   input_offset,
                                                                            int   nof_ports,
                                                                            float input_gain)
{
  int idx   = blockIdx.x * blockDim.x + threadIdx.x;
  int total = dft_size * nof_ports;
  if (idx >= total) {
    return;
  }

  int re   = idx % dft_size;
  int port = idx / dft_size;
  int src  = port * input_nof_samples + input_offset + re;
  // Complex int16 pairs are 4-byte aligned for normal sample buffers.
  const int32_t packed = *reinterpret_cast<const int32_t*>(d_input + 2 * src);
  const auto    re_i16 = static_cast<int16_t>(packed & 0xffff);
  const auto    im_i16 = static_cast<int16_t>((packed >> 16) & 0xffff);
  d_freq[idx]          = make_cuFloatComplex(static_cast<float>(re_i16) * input_gain,
                                    static_cast<float>(im_i16) * input_gain);
}

// Max symbol×port batches for scattered zero-copy (skip host packing).
static constexpr int MAX_SCATTERED_BATCHES = OCUDU_LOWPHY_PUXCH_RX_MAX_PORTS * OCUDU_LOWPHY_PUXCH_RX_MAX_SYMBOLS;

// Per-batch base pointers (device-accessible mapped host windows). Avoids an
// intermediate host pack of all FFT windows into one staging buffer.
struct scattered_ci16_bases {
  const int16_t* ptr[MAX_SCATTERED_BATCHES];
};

// Load from non-contiguous mapped host windows (one pointer per batch).
// Used on integrated GPUs to eliminate host memcpy packing before the FFT.
__global__ void __launch_bounds__(THREADS) load_puxch_fft_input_ci16_scattered_kernel(
    cuFloatComplex* __restrict__ d_freq,
    scattered_ci16_bases         bases,
    int                          dft_size,
    int                          nof_batches,
    float                        input_gain)
{
  int idx   = blockIdx.x * blockDim.x + threadIdx.x;
  int total = dft_size * nof_batches;
  if (idx >= total) {
    return;
  }

  const int            re    = idx % dft_size;
  const int            batch = idx / dft_size;
  const int16_t* const src   = bases.ptr[batch];
  const int32_t        packed = *reinterpret_cast<const int32_t*>(src + 2 * re);
  const auto           re_i16 = static_cast<int16_t>(packed & 0xffff);
  const auto           im_i16 = static_cast<int16_t>((packed >> 16) & 0xffff);
  d_freq[idx] =
      make_cuFloatComplex(static_cast<float>(re_i16) * input_gain, static_cast<float>(im_i16) * input_gain);
}

__global__ void init_window_phase_kernel(cuFloatComplex* __restrict__ d_window_phase, int dft_size, int window_offset)
{
  int idx = blockIdx.x * blockDim.x + threadIdx.x;
  if (idx >= dft_size) {
    return;
  }

  float angle = static_cast<float>(window_offset) * 6.28318530717958647692f * static_cast<float>(idx) /
                static_cast<float>(dft_size);
  float s;
  float c;
  sincosf(angle, &s, &c);
  d_window_phase[idx] = make_cuFloatComplex(c, s);
}

__device__ __forceinline__ int fft_bin_from_re(int k, int half_grid, int dft_size)
{
  // Centered RE k -> FFT bin. Equivalent to:
  //   (k < half) ? (dft - half + k) : (k - half)
  return k - half_grid + ((k < half_grid) ? dft_size : 0);
}

__global__ void __launch_bounds__(THREADS) extract_puxch_grid_kernel(uint32_t* __restrict__ output,
                                                                     const cuFloatComplex* __restrict__ d_freq,
                                                                     const cuFloatComplex* __restrict__ d_window_phase,
                                                                     extract_kernel_params p)
{
  int idx   = blockIdx.x * blockDim.x + threadIdx.x;
  int total = p.nof_ports * p.rg_size;
  if (idx >= total) {
    return;
  }

  const int      half_grid  = p.rg_size >> 1;
  const int      k          = idx % p.rg_size;
  const int      port_batch = idx / p.rg_size;
  const int      src        = fft_bin_from_re(k, half_grid, p.dft_size);
  cuFloatComplex x          = __ldg(&d_freq[port_batch * p.dft_size + src]);

  x = cmul(x, make_cuFloatComplex(p.phase_scale_re, p.phase_scale_im));
  if (d_window_phase != nullptr) {
    x = cmul(x, __ldg(&d_window_phase[src]));
  }

  const int    output_port = p.port_indices[port_batch];
  const size_t dst =
      (static_cast<size_t>(output_port) * p.grid_nof_symbols + p.symbol_index) * p.grid_nof_subc + k;
  output[dst] = cfloat_to_cbf16(x);
}

__global__ void __launch_bounds__(THREADS)
    extract_puxch_grid_symbol_batch_kernel(uint32_t* __restrict__ output,
                                           const cuFloatComplex* __restrict__ d_freq,
                                           const cuFloatComplex* __restrict__ d_window_phase,
                                           extract_kernel_params p,
                                           int                   nof_symbols)
{
  int idx   = blockIdx.x * blockDim.x + threadIdx.x;
  int total = nof_symbols * p.nof_ports * p.rg_size;
  if (idx >= total) {
    return;
  }

  const int half_grid    = p.rg_size >> 1;
  const int k            = idx % p.rg_size;
  const int port_batch   = (idx / p.rg_size) % p.nof_ports;
  const int symbol_batch = idx / (p.rg_size * p.nof_ports);
  const int src          = fft_bin_from_re(k, half_grid, p.dft_size);
  const int fft_batch    = symbol_batch * p.nof_ports + port_batch;
  cuFloatComplex x       = __ldg(&d_freq[fft_batch * p.dft_size + src]);

  float phase_re = p.phase_scale_re;
  float phase_im = p.phase_scale_im;
  if (p.use_symbol_phases != 0) {
    phase_re = p.symbol_phase_scale_re[symbol_batch];
    phase_im = p.symbol_phase_scale_im[symbol_batch];
  }
  x = cmul(x, make_cuFloatComplex(phase_re, phase_im));
  if (d_window_phase != nullptr) {
    x = cmul(x, __ldg(&d_window_phase[src]));
  }

  const int    output_port   = p.port_indices[port_batch];
  const int    output_symbol = p.symbol_index + symbol_batch;
  const size_t dst =
      (static_cast<size_t>(output_port) * p.grid_nof_symbols + output_symbol) * p.grid_nof_subc + k;
  output[dst] = cfloat_to_cbf16(x);
}

} // namespace

struct ocudu_lowphy_puxch_rx_handle {
  ocudu_lowphy_puxch_rx_config_t cfg{};
  cudaStream_t                   owned_stream       = nullptr;
  cudaStream_t                   stream             = nullptr;
  cudaEvent_t                    done_event         = nullptr;
  int                            active_nof_symbols = 1;

  int16_t*        d_input_ci16        = nullptr;
  cuFloatComplex* d_freq              = nullptr;
  cuFloatComplex* d_window_phase      = nullptr;
  size_t          input_ci16_count    = 0;
  size_t          input_ci16_capacity = 0;
  size_t          window_ci16_count   = 0;
  size_t          freq_count          = 0;
  size_t          window_phase_count  = 0;
  int             window_phase_offset = -1;

  int16_t*    h_input_ci16_staging[HOST_STAGING_SLOTS]        = {};
  int16_t*    d_input_ci16_mapped[HOST_STAGING_SLOTS]         = {}; // device ptr for mapped staging (or null)
  cudaEvent_t h_input_ci16_ready_events[HOST_STAGING_SLOTS]   = {};
  bool        h_input_ci16_event_recorded[HOST_STAGING_SLOTS] = {};
  size_t      h_input_ci16_staging_count                      = 0;
  int         h_input_ci16_next_slot                          = 0;
  bool        zero_copy_staging                               = false;

  VkFFTApplication vkfft_app{};
  CUdevice         vkfft_device      = 0;
  cudaStream_t     vkfft_stream      = nullptr;
  void*            vkfft_buffer      = nullptr;
  pfUINT           vkfft_buffer_size = 0;
  int              vkfft_nof_batches = 0;
  bool             vkfft_valid       = false;

  uint32_t* d_warmup_output     = nullptr;
  size_t    warmup_output_count = 0;

  struct device_graph_entry {
    const int16_t*                 d_input             = nullptr;
    void*                          output_cbf16        = nullptr;
    cudaStream_t                   stream              = nullptr;
    float                          input_scale         = 0.0f;
    int                            nof_symbols         = 1;
    bool                           input_is_fft_window = false;
    ocudu_lowphy_puxch_rx_config_t cfg{};
    cudaGraph_t                    graph        = nullptr;
    cudaGraphExec_t                exec         = nullptr;
    cudaGraphNode_t                extract_node = nullptr;
  };
  std::vector<device_graph_entry> device_graphs;
  size_t                          next_device_graph_replacement = 0;
  bool                            device_graph_capture_failed   = false;

  struct host_registration {
    const void*    ptr                   = nullptr;
    size_t         bytes                 = 0;
    bool           unregister_on_destroy = false;
    const int16_t* device_ptr            = nullptr; // mapped device view, if available
  };
  std::vector<host_registration> registered_hosts;
  std::vector<host_registration> unregistered_hosts;
  bool                           done_event_recorded = false;
};

namespace {

// Tiny single-symbol / single-port cases pay more for mapped registration + scattered
// loads than for a short pack+H2D; keep a size gate (i16 sample count across batches).
static bool scattered_zero_copy_worthwhile(const ocudu_lowphy_puxch_rx_handle* h, int nof_symbols)
{
  if ((h == nullptr) || !scattered_zero_copy_enabled() || !h->zero_copy_staging) {
    return false;
  }
  // Multi-symbol batch always benefits (host pack of many windows dominates).
  if (nof_symbols > 1) {
    return true;
  }
  static const size_t min_i16 =
      static_cast<size_t>(env_unsigned_or_default("OCUDU_LOWPHY_RX_SCATTERED_MIN_I16", 16 * 1024, 16 * 1024 * 1024));
  const size_t samples_i16 =
      static_cast<size_t>(h->cfg.nof_ports) * static_cast<size_t>(nof_symbols) * static_cast<size_t>(h->cfg.dft_size) *
      2U;
  return samples_i16 >= min_i16;
}

static void destroy_device_graph_entry(ocudu_lowphy_puxch_rx_handle::device_graph_entry& entry)
{
  if (entry.exec != nullptr) {
    cudaGraphExecDestroy(entry.exec);
    entry.exec = nullptr;
  }
  if (entry.graph != nullptr) {
    cudaGraphDestroy(entry.graph);
    entry.graph = nullptr;
  }
  entry.d_input      = nullptr;
  entry.output_cbf16 = nullptr;
  entry.stream       = nullptr;
  entry.extract_node = nullptr;
}

static void destroy_device_graphs(ocudu_lowphy_puxch_rx_handle* h)
{
  if (h == nullptr) {
    return;
  }
  for (auto& entry : h->device_graphs) {
    destroy_device_graph_entry(entry);
  }
  h->device_graphs.clear();
  h->next_device_graph_replacement = 0;
  h->device_graph_capture_failed   = false;
}

static void destroy_fft_plan(ocudu_lowphy_puxch_rx_handle* h)
{
  destroy_device_graphs(h);
  if (h->vkfft_valid) {
    deleteVkFFT(&h->vkfft_app);
    std::memset(&h->vkfft_app, 0, sizeof(h->vkfft_app));
    h->vkfft_valid       = false;
    h->vkfft_nof_batches = 0;
  }
}

static void release_buffers(ocudu_lowphy_puxch_rx_handle* h)
{
  destroy_device_graphs(h);
  if (h->stream != nullptr) {
    (void)cudaStreamSynchronize(h->stream);
  }
  for (int slot = 0; slot != HOST_STAGING_SLOTS; ++slot) {
    if (h->h_input_ci16_ready_events[slot] != nullptr) {
      cudaEventDestroy(h->h_input_ci16_ready_events[slot]);
      h->h_input_ci16_ready_events[slot] = nullptr;
    }
    if (h->h_input_ci16_staging[slot] != nullptr) {
      cudaFreeHost(h->h_input_ci16_staging[slot]);
      h->h_input_ci16_staging[slot] = nullptr;
    }
    h->d_input_ci16_mapped[slot]         = nullptr;
    h->h_input_ci16_event_recorded[slot] = false;
  }
  h->zero_copy_staging = false;
  if (h->d_input_ci16 != nullptr) {
    cudaFree(h->d_input_ci16);
    h->d_input_ci16 = nullptr;
  }
  if (h->d_freq != nullptr) {
    cudaFree(h->d_freq);
    h->d_freq = nullptr;
  }
  if (h->d_window_phase != nullptr) {
    cudaFree(h->d_window_phase);
    h->d_window_phase = nullptr;
  }
  if (h->d_warmup_output != nullptr) {
    cudaFree(h->d_warmup_output);
    h->d_warmup_output = nullptr;
  }
  h->input_ci16_count           = 0;
  h->input_ci16_capacity        = 0;
  h->window_ci16_count          = 0;
  h->freq_count                 = 0;
  h->window_phase_count         = 0;
  h->window_phase_offset        = -1;
  h->warmup_output_count        = 0;
  h->h_input_ci16_staging_count = 0;
  h->h_input_ci16_next_slot     = 0;
}

static bool contains_registered_range(const std::vector<ocudu_lowphy_puxch_rx_handle::host_registration>& registrations,
                                      const void*                                                         ptr,
                                      size_t                                                              bytes)
{
  if ((ptr == nullptr) || (bytes == 0)) {
    return true;
  }

  const auto begin = reinterpret_cast<uintptr_t>(ptr);
  const auto end   = begin + bytes;
  for (const auto& registration : registrations) {
    const auto registered_begin = reinterpret_cast<uintptr_t>(registration.ptr);
    const auto registered_end   = registered_begin + registration.bytes;
    if ((begin >= registered_begin) && (end <= registered_end)) {
      return true;
    }
  }
  return false;
}

static const int16_t* mapped_device_ptr_for_range(
    const std::vector<ocudu_lowphy_puxch_rx_handle::host_registration>& registrations,
    const void*                                                         ptr,
    size_t                                                              bytes)
{
  if ((ptr == nullptr) || (bytes == 0)) {
    return nullptr;
  }
  const auto begin = reinterpret_cast<uintptr_t>(ptr);
  const auto end   = begin + bytes;
  for (const auto& registration : registrations) {
    if (registration.device_ptr == nullptr) {
      continue;
    }
    const auto registered_begin = reinterpret_cast<uintptr_t>(registration.ptr);
    const auto registered_end   = registered_begin + registration.bytes;
    if ((begin >= registered_begin) && (end <= registered_end)) {
      const size_t byte_off = begin - registered_begin;
      return registration.device_ptr + (byte_off / sizeof(int16_t));
    }
  }
  return nullptr;
}

static const int16_t* try_map_registered_host(const void* ptr, size_t bytes)
{
  void* device_ptr = nullptr;
  // Works when the allocation was mapped (cudaHostAllocMapped / HostRegisterMapped)
  // or when the driver already exposes a device view for registered memory.
  if (cudaHostGetDevicePointer(&device_ptr, const_cast<void*>(ptr), 0) != cudaSuccess) {
    (void)cudaGetLastError();
    return nullptr;
  }
  (void)bytes;
  return static_cast<const int16_t*>(device_ptr);
}

static bool register_host_range(ocudu_lowphy_puxch_rx_handle* h, const void* ptr, size_t bytes)
{
  if ((ptr == nullptr) || (bytes == 0)) {
    return true;
  }
  if (contains_registered_range(h->registered_hosts, ptr, bytes)) {
    return true;
  }
  if (contains_registered_range(h->unregistered_hosts, ptr, bytes)) {
    return false;
  }

  // Prefer mapped registration on integrated zero-copy so the load kernel can
  // read host windows without an intermediate pack / H2D.
  unsigned flags = cudaHostRegisterDefault;
  if (h->zero_copy_staging || device_is_integrated()) {
    flags = static_cast<unsigned>(cudaHostRegisterMapped);
  }

  cudaError_t err = cudaHostRegister(const_cast<void*>(ptr), bytes, flags);
  if ((err != cudaSuccess) && (flags != cudaHostRegisterDefault)) {
    (void)cudaGetLastError();
    err = cudaHostRegister(const_cast<void*>(ptr), bytes, cudaHostRegisterDefault);
  }
  if (err == cudaSuccess) {
    const int16_t* device_ptr = try_map_registered_host(ptr, bytes);
    h->registered_hosts.push_back({ptr, bytes, true, device_ptr});
    return true;
  }
  if (err == cudaErrorHostMemoryAlreadyRegistered) {
    (void)cudaGetLastError();
    const int16_t* device_ptr = try_map_registered_host(ptr, bytes);
    h->registered_hosts.push_back({ptr, bytes, false, device_ptr});
    return true;
  }

  (void)cudaGetLastError();
  h->unregistered_hosts.push_back({ptr, bytes, false, nullptr});
  return false;
}

// Resolve a host FFT-window pointer to a device-accessible mapped pointer.
static const int16_t* resolve_mapped_window(ocudu_lowphy_puxch_rx_handle* h, const void* host_window, size_t bytes)
{
  if ((h == nullptr) || (host_window == nullptr) || (bytes == 0)) {
    return nullptr;
  }
  if (const int16_t* existing = mapped_device_ptr_for_range(h->registered_hosts, host_window, bytes);
      existing != nullptr) {
    return existing;
  }
  if (!register_host_range(h, host_window, bytes)) {
    return nullptr;
  }
  return mapped_device_ptr_for_range(h->registered_hosts, host_window, bytes);
}

static bool validate_config(const ocudu_lowphy_puxch_rx_config_t& cfg)
{
  if ((cfg.dft_size <= 0) || (cfg.rg_size <= 0) || (cfg.grid_nof_subc <= 0) || (cfg.nof_ports <= 0) ||
      (cfg.grid_nof_symbols <= 0) || (cfg.input_nof_samples <= 0) || (cfg.cyclic_prefix_length < 0) ||
      (cfg.window_offset < 0) || (cfg.symbol_index < 0) || !std::isfinite(cfg.dft_scale) || (cfg.dft_scale <= 0.0f) ||
      !std::isfinite(cfg.phase_re) || !std::isfinite(cfg.phase_im)) {
    return false;
  }
  if ((cfg.rg_size > cfg.dft_size) || ((cfg.rg_size % 2) != 0) || (cfg.rg_size > cfg.grid_nof_subc) ||
      (cfg.symbol_index >= cfg.grid_nof_symbols) || (cfg.window_offset > cfg.cyclic_prefix_length) ||
      (cfg.nof_ports > OCUDU_LOWPHY_PUXCH_RX_MAX_PORTS)) {
    return false;
  }
  if (cfg.use_symbol_phases != 0) {
    for (int symbol = 0; symbol != OCUDU_LOWPHY_PUXCH_RX_MAX_SYMBOLS; ++symbol) {
      if (!std::isfinite(cfg.symbol_phase_re[symbol]) || !std::isfinite(cfg.symbol_phase_im[symbol])) {
        return false;
      }
    }
  }
  if (cfg.use_symbol_layout != 0) {
    for (int symbol = 0; symbol != OCUDU_LOWPHY_PUXCH_RX_MAX_SYMBOLS; ++symbol) {
      if ((cfg.symbol_input_nof_samples[symbol] <= 0) || (cfg.symbol_cyclic_prefix_length[symbol] < 0) ||
          (cfg.symbol_window_offset[symbol] < 0) ||
          (cfg.symbol_window_offset[symbol] > cfg.symbol_cyclic_prefix_length[symbol]) ||
          (cfg.symbol_cyclic_prefix_length[symbol] - cfg.symbol_window_offset[symbol] + cfg.dft_size >
           cfg.symbol_input_nof_samples[symbol])) {
        return false;
      }
    }
  }
  if (cfg.cyclic_prefix_length - cfg.window_offset + cfg.dft_size > cfg.input_nof_samples) {
    return false;
  }
  for (int port = 0; port != cfg.nof_ports; ++port) {
    if (cfg.port_indices[port] < 0) {
      return false;
    }
  }
  return true;
}

static bool ensure_buffers(ocudu_lowphy_puxch_rx_handle* h)
{
  const int    nof_active_symbols = active_nof_symbols(h->active_nof_symbols);
  const size_t nof_batches        = static_cast<size_t>(h->cfg.nof_ports) * nof_active_symbols;
  size_t       input_ci16_count   = nof_batches * h->cfg.input_nof_samples * 2U;
  size_t       window_ci16_count  = nof_batches * h->cfg.dft_size * 2U;
  size_t       input_samples_capacity =
      static_cast<size_t>(h->cfg.dft_size) +
      std::max(static_cast<size_t>(h->cfg.cyclic_prefix_length), static_cast<size_t>(std::max(1, h->cfg.dft_size / 4)));
  size_t input_ci16_capacity = nof_batches * input_samples_capacity * 2U;
  input_ci16_capacity        = std::max(input_ci16_capacity, window_ci16_count);
  size_t       freq_count    = nof_batches * h->cfg.dft_size;
  const size_t warmup_words  = warmup_output_words(h->cfg);

  if ((h->input_ci16_capacity >= window_ci16_count) && (h->freq_count == freq_count) && (h->d_input_ci16 != nullptr) &&
      (h->d_freq != nullptr) && (h->h_input_ci16_staging_count >= window_ci16_count) &&
      (h->h_input_ci16_staging[0] != nullptr) && (h->d_warmup_output != nullptr) &&
      (h->warmup_output_count >= warmup_words)) {
    h->input_ci16_count  = input_ci16_count;
    h->window_ci16_count = window_ci16_count;
    return true;
  }

  destroy_fft_plan(h);
  release_buffers(h);
  if (!check_cuda(cudaMalloc(&h->d_input_ci16, input_ci16_capacity * sizeof(int16_t))) ||
      !check_cuda(cudaMalloc(&h->d_freq, freq_count * sizeof(cuFloatComplex))) ||
      !check_cuda(cudaMalloc(&h->d_warmup_output, warmup_words * sizeof(uint32_t)))) {
    release_buffers(h);
    return false;
  }
  // Integrated/zero-copy path: mapped pinned staging (GPU reads host DRAM directly).
  // Discrete path: write-combined pinned staging + H2D (host packs windows).
  h->zero_copy_staging = zero_copy_staging_enabled();
  const unsigned host_alloc_flags =
      h->zero_copy_staging ? static_cast<unsigned>(cudaHostAllocMapped) : static_cast<unsigned>(cudaHostAllocWriteCombined);
  for (int slot = 0; slot != HOST_STAGING_SLOTS; ++slot) {
    cudaError_t host_err =
        cudaHostAlloc(&h->h_input_ci16_staging[slot], input_ci16_capacity * sizeof(int16_t), host_alloc_flags);
    if (host_err != cudaSuccess) {
      (void)cudaGetLastError();
      host_err = cudaHostAlloc(
          &h->h_input_ci16_staging[slot], input_ci16_capacity * sizeof(int16_t), cudaHostAllocDefault);
      h->zero_copy_staging = false;
    }
    if (!check_cuda(host_err) ||
        !check_cuda(cudaEventCreateWithFlags(&h->h_input_ci16_ready_events[slot], cudaEventDisableTiming))) {
      release_buffers(h);
      return false;
    }
    h->d_input_ci16_mapped[slot] = nullptr;
    if (h->zero_copy_staging) {
      void* device_ptr = nullptr;
      if (cudaHostGetDevicePointer(&device_ptr, h->h_input_ci16_staging[slot], 0) == cudaSuccess) {
        h->d_input_ci16_mapped[slot] = static_cast<int16_t*>(device_ptr);
      } else {
        (void)cudaGetLastError();
        h->zero_copy_staging = false;
      }
    }
  }
  // If any slot failed mapping, fall back to classic H2D for all slots.
  if (h->zero_copy_staging) {
    for (int slot = 0; slot != HOST_STAGING_SLOTS; ++slot) {
      if (h->d_input_ci16_mapped[slot] == nullptr) {
        h->zero_copy_staging = false;
        break;
      }
    }
  }
  h->input_ci16_count           = input_ci16_count;
  h->input_ci16_capacity        = input_ci16_capacity;
  h->window_ci16_count          = window_ci16_count;
  h->freq_count                 = freq_count;
  h->h_input_ci16_staging_count = input_ci16_capacity;
  h->warmup_output_count        = warmup_words;
  return true;
}

static bool ensure_window_phase(ocudu_lowphy_puxch_rx_handle* h)
{
  if (h->cfg.window_offset == 0) {
    if (h->d_window_phase != nullptr) {
      cudaFree(h->d_window_phase);
      h->d_window_phase = nullptr;
    }
    h->window_phase_count  = 0;
    h->window_phase_offset = 0;
    return true;
  }

  size_t phase_count = static_cast<size_t>(h->cfg.dft_size);
  if ((h->d_window_phase == nullptr) || (h->window_phase_count != phase_count)) {
    if (h->d_window_phase != nullptr) {
      cudaFree(h->d_window_phase);
      h->d_window_phase = nullptr;
    }
    if (!check_cuda(cudaMalloc(&h->d_window_phase, phase_count * sizeof(cuFloatComplex)))) {
      h->window_phase_count  = 0;
      h->window_phase_offset = -1;
      return false;
    }
    h->window_phase_count  = phase_count;
    h->window_phase_offset = -1;
  }

  if (h->window_phase_offset != h->cfg.window_offset) {
    int blocks = (h->cfg.dft_size + THREADS - 1) / THREADS;
    init_window_phase_kernel<<<blocks, THREADS, 0, h->stream>>>(
        h->d_window_phase, h->cfg.dft_size, h->cfg.window_offset);
    if (!check_cuda(cudaGetLastError())) {
      h->window_phase_offset = -1;
      return false;
    }
    h->window_phase_offset = h->cfg.window_offset;
  }
  return true;
}

static bool ensure_plan(ocudu_lowphy_puxch_rx_handle* h)
{
  const int nof_batches = h->cfg.nof_ports * active_nof_symbols(h->active_nof_symbols);
  if (h->vkfft_valid && (h->vkfft_stream == h->stream) && (h->vkfft_buffer == h->d_freq) &&
      (h->vkfft_nof_batches == nof_batches)) {
    return true;
  }
  if (h->vkfft_valid) {
    deleteVkFFT(&h->vkfft_app);
    std::memset(&h->vkfft_app, 0, sizeof(h->vkfft_app));
    h->vkfft_valid = false;
  }

  int cuda_device = 0;
  if (!check_cuda(cudaGetDevice(&cuda_device)) || (cuDeviceGet(&h->vkfft_device, cuda_device) != CUDA_SUCCESS)) {
    return false;
  }

  h->vkfft_stream      = h->stream;
  h->vkfft_buffer      = h->d_freq;
  h->vkfft_buffer_size = static_cast<pfUINT>(h->freq_count * sizeof(cuFloatComplex));

  VkFFTConfiguration configuration  = {};
  configuration.FFTdim              = 1;
  configuration.size[0]             = static_cast<pfUINT>(h->cfg.dft_size);
  configuration.device              = &h->vkfft_device;
  configuration.stream              = &h->vkfft_stream;
  configuration.num_streams         = 1;
  configuration.numberBatches       = nof_batches;
  configuration.bufferSize          = &h->vkfft_buffer_size;
  configuration.buffer              = &h->vkfft_buffer;
  configuration.makeForwardPlanOnly = 1;
  configuration.normalize           = 0;
  configuration.disableReorderFourStep =
      env_flag_enabled_or_default("OCUDU_LOWPHY_RX_VKFFT_DISABLE_REORDER_FOUR_STEP", true) ? 1 : 0;
  configuration.useLUT = env_flag_enabled_or_default("OCUDU_LOWPHY_RX_VKFFT_USE_LUT", true) ? 1 : 0;
  // Larger DFTs benefit from more threads on GB10-class GPUs; keep 128 for narrow carriers.
  const unsigned default_aim = (h->cfg.dft_size >= 2048) ? 256U : 128U;
  configuration.aimThreads   = env_unsigned_or_default("OCUDU_LOWPHY_RX_VKFFT_AIM_THREADS", default_aim, 1024);

  if (initializeVkFFT(&h->vkfft_app, configuration) != VKFFT_SUCCESS) {
    std::memset(&h->vkfft_app, 0, sizeof(h->vkfft_app));
    return false;
  }
  h->vkfft_valid       = true;
  h->vkfft_nof_batches = nof_batches;
  return true;
}

static cudaError_t wait_event_yielding(cudaEvent_t event)
{
  cudaError_t status;
  while ((status = cudaEventQuery(event)) == cudaErrorNotReady) {
    sched_yield();
  }
  return status;
}

static int16_t* acquire_ci16_staging_slot(ocudu_lowphy_puxch_rx_handle* h, int* acquired_slot)
{
  int slot                  = h->h_input_ci16_next_slot;
  h->h_input_ci16_next_slot = (h->h_input_ci16_next_slot + 1) % HOST_STAGING_SLOTS;
  if (acquired_slot != nullptr) {
    *acquired_slot = slot;
  }

  if ((h->h_input_ci16_staging[slot] == nullptr) || (h->h_input_ci16_ready_events[slot] == nullptr)) {
    return nullptr;
  }
  if (h->h_input_ci16_event_recorded[slot]) {
    if (wait_event_yielding(h->h_input_ci16_ready_events[slot]) != cudaSuccess) {
      h->h_input_ci16_event_recorded[slot] = false;
      return nullptr;
    }
    h->h_input_ci16_event_recorded[slot] = false;
  }
  return h->h_input_ci16_staging[slot];
}

static bool record_staging_slot_ready_event(ocudu_lowphy_puxch_rx_handle* h, int event_slot)
{
  if ((event_slot < 0) || (event_slot >= HOST_STAGING_SLOTS) ||
      !check_cuda(cudaEventRecord(h->h_input_ci16_ready_events[event_slot], h->stream))) {
    h->h_input_ci16_event_recorded[event_slot] = false;
    return false;
  }
  h->h_input_ci16_event_recorded[event_slot] = true;
  return true;
}

// Returns the device-side pointer the load kernel should read. On the zero-copy
// path this is the mapped host staging buffer; otherwise it is the device copy.
static const int16_t* publish_staged_ci16(ocudu_lowphy_puxch_rx_handle* h,
                                          const int16_t*                staged,
                                          size_t                        samples_i16,
                                          int                           event_slot)
{
  // Zero-copy pays off when the packed FFT windows are large enough that an
  // explicit H2D would dominate. Tiny single-port cases stay on the H2D path.
  static const size_t zero_copy_min_i16 =
      static_cast<size_t>(env_unsigned_or_default("OCUDU_LOWPHY_RX_ZERO_COPY_MIN_I16", 48 * 1024, 16 * 1024 * 1024));
  if (h->zero_copy_staging && (samples_i16 >= zero_copy_min_i16) && (event_slot >= 0) &&
      (event_slot < HOST_STAGING_SLOTS) && (h->d_input_ci16_mapped[event_slot] != nullptr) &&
      (staged == h->h_input_ci16_staging[event_slot])) {
    // Host has finished packing (synchronous memcpy into mapped staging). On
    // integrated coherent platforms the subsequent kernel launch observes those
    // stores; optional full sync is available for non-coherent edge cases.
    static const bool force_sync = env_flag_enabled("OCUDU_LOWPHY_RX_ZERO_COPY_HOST_SYNC");
    if (force_sync && (cudaStreamSynchronize(h->stream) != cudaSuccess)) {
      return nullptr;
    }
    // Keep slot lifetime tracking consistent with the H2D path.
    if (!record_staging_slot_ready_event(h, event_slot)) {
      return nullptr;
    }
    return h->d_input_ci16_mapped[event_slot];
  }

  if (!check_cuda(cudaMemcpyAsync(
          h->d_input_ci16, staged, samples_i16 * sizeof(int16_t), cudaMemcpyHostToDevice, h->stream))) {
    return nullptr;
  }
  if (!record_staging_slot_ready_event(h, event_slot)) {
    return nullptr;
  }
  return h->d_input_ci16;
}

static bool copy_registered_ci16_windows_to_device(ocudu_lowphy_puxch_rx_handle* h, const void* const* input_ports_ci16)
{
  const unsigned input_offset          = h->cfg.cyclic_prefix_length - h->cfg.window_offset;
  const size_t   window_i16_per_port   = static_cast<size_t>(h->cfg.dft_size) * 2U;
  const size_t   window_bytes_per_port = window_i16_per_port * sizeof(int16_t);

  for (int port = 0; port != h->cfg.nof_ports; ++port) {
    const auto* src = static_cast<const int16_t*>(input_ports_ci16[port]) + static_cast<size_t>(input_offset) * 2U;
    if (!register_host_range(h, src, window_bytes_per_port)) {
      return false;
    }
  }

  for (int port = 0; port != h->cfg.nof_ports; ++port) {
    const auto* src = static_cast<const int16_t*>(input_ports_ci16[port]) + static_cast<size_t>(input_offset) * 2U;
    int16_t*    dst = h->d_input_ci16 + static_cast<size_t>(port) * window_i16_per_port;
    if (!check_cuda(cudaMemcpyAsync(dst, src, window_bytes_per_port, cudaMemcpyHostToDevice, h->stream))) {
      return false;
    }
  }
  return true;
}

static bool copy_registered_ci16_symbol_batch_windows_to_device(ocudu_lowphy_puxch_rx_handle* h,
                                                                const void* const* input_symbols_ports_ci16,
                                                                int                nof_symbols)
{
  const unsigned input_offset             = h->cfg.cyclic_prefix_length - h->cfg.window_offset;
  const size_t   window_i16_per_batch     = static_cast<size_t>(h->cfg.dft_size) * 2U;
  const size_t   window_bytes_per_batch   = window_i16_per_batch * sizeof(int16_t);
  const auto* first_src =
      static_cast<const int16_t*>(input_symbols_ports_ci16[0]) + static_cast<size_t>(input_offset) * 2U;

  for (int symbol = 0; symbol != nof_symbols; ++symbol) {
    unsigned symbol_input_offset = input_offset;
    if (h->cfg.use_symbol_layout != 0) {
      symbol_input_offset =
          static_cast<unsigned>(h->cfg.symbol_cyclic_prefix_length[symbol] - h->cfg.symbol_window_offset[symbol]);
    }
    for (int port = 0; port != h->cfg.nof_ports; ++port) {
      const auto* src = static_cast<const int16_t*>(input_symbols_ports_ci16[symbol * h->cfg.nof_ports + port]) +
                        static_cast<size_t>(symbol_input_offset) * 2U;
      size_t batch = static_cast<size_t>(symbol) * h->cfg.nof_ports + port;
      // Many small H2D copies are slower than staging. Bypass staging only when the windows are already packed.
      if (src != first_src + batch * window_i16_per_batch) {
        return false;
      }
    }
  }

  size_t total_bytes = static_cast<size_t>(nof_symbols) * h->cfg.nof_ports * window_bytes_per_batch;
  if (!register_host_range(h, first_src, total_bytes)) {
    return false;
  }
  if (!check_cuda(cudaMemcpyAsync(h->d_input_ci16, first_src, total_bytes, cudaMemcpyHostToDevice, h->stream))) {
    return false;
  }
  return true;
}

static void
copy_puxch_fft_window_to_staging(ocudu_lowphy_puxch_rx_handle* h, int16_t* staged, const void* const* input_ports_ci16)
{
  const unsigned input_offset          = h->cfg.cyclic_prefix_length - h->cfg.window_offset;
  const size_t   window_i16_per_port   = static_cast<size_t>(h->cfg.dft_size) * 2U;
  const size_t   window_bytes_per_port = window_i16_per_port * sizeof(int16_t);
  for (int port = 0; port != h->cfg.nof_ports; ++port) {
    const auto* src = static_cast<const int16_t*>(input_ports_ci16[port]) + static_cast<size_t>(input_offset) * 2U;
    std::memcpy(staged + static_cast<size_t>(port) * window_i16_per_port, src, window_bytes_per_port);
  }
}

static void copy_puxch_fft_window_symbol_batch_to_staging(ocudu_lowphy_puxch_rx_handle* h,
                                                          int16_t*                      staged,
                                                          const void* const*            input_symbols_ports_ci16,
                                                          int                           nof_symbols)
{
  const unsigned input_offset           = h->cfg.cyclic_prefix_length - h->cfg.window_offset;
  const size_t   window_i16_per_batch   = static_cast<size_t>(h->cfg.dft_size) * 2U;
  const size_t   window_bytes_per_batch = window_i16_per_batch * sizeof(int16_t);
  for (int symbol = 0; symbol != nof_symbols; ++symbol) {
    unsigned symbol_input_offset = input_offset;
    if (h->cfg.use_symbol_layout != 0) {
      symbol_input_offset =
          static_cast<unsigned>(h->cfg.symbol_cyclic_prefix_length[symbol] - h->cfg.symbol_window_offset[symbol]);
    }
    for (int port = 0; port != h->cfg.nof_ports; ++port) {
      const auto* src = static_cast<const int16_t*>(input_symbols_ports_ci16[symbol * h->cfg.nof_ports + port]) +
                        static_cast<size_t>(symbol_input_offset) * 2U;
      size_t batch = static_cast<size_t>(symbol) * h->cfg.nof_ports + port;
      std::memcpy(staged + batch * window_i16_per_batch, src, window_bytes_per_batch);
    }
  }
}

static bool enqueue_vkfft_and_extract(ocudu_lowphy_puxch_rx_handle* h, void* output_cbf16, int nof_symbols)
{
  VkFFTLaunchParams launch_params = {};
  launch_params.buffer            = &h->vkfft_buffer;
  if (VkFFTAppend(&h->vkfft_app, -1, &launch_params) != VKFFT_SUCCESS) {
    return false;
  }

  if (nof_symbols > 1) {
    int extract_blocks = (nof_symbols * h->cfg.nof_ports * h->cfg.rg_size + THREADS - 1) / THREADS;
    extract_puxch_grid_symbol_batch_kernel<<<extract_blocks, THREADS, 0, h->stream>>>(
        static_cast<uint32_t*>(output_cbf16), h->d_freq, h->d_window_phase, make_extract_params(h->cfg), nof_symbols);
  } else {
    int extract_blocks = (h->cfg.nof_ports * h->cfg.rg_size + THREADS - 1) / THREADS;
    extract_puxch_grid_kernel<<<extract_blocks, THREADS, 0, h->stream>>>(
        static_cast<uint32_t*>(output_cbf16), h->d_freq, h->d_window_phase, make_extract_params(h->cfg));
  }
  return check_cuda(cudaGetLastError());
}

static bool enqueue_puxch_demod_ci16_uncaptured(ocudu_lowphy_puxch_rx_handle* h,
                                                const int16_t*                d_input,
                                                float                         input_scale,
                                                void*                         output_cbf16,
                                                bool                          input_is_fft_window)
{
  if (!std::isfinite(input_scale) || (input_scale <= 0.0f)) {
    return false;
  }

  int load_blocks  = (h->cfg.nof_ports * h->cfg.dft_size + THREADS - 1) / THREADS;
  int input_offset = input_is_fft_window ? 0 : (h->cfg.cyclic_prefix_length - h->cfg.window_offset);
  int input_stride = input_is_fft_window ? h->cfg.dft_size : h->cfg.input_nof_samples;
  load_puxch_fft_input_ci16_kernel<<<load_blocks, THREADS, 0, h->stream>>>(
      h->d_freq, d_input, h->cfg.dft_size, input_stride, input_offset, h->cfg.nof_ports, 1.0f / input_scale);
  if (!check_cuda(cudaGetLastError())) {
    return false;
  }

  return enqueue_vkfft_and_extract(h, output_cbf16, 1);
}

static bool enqueue_puxch_demod_ci16_symbol_batch_uncaptured(ocudu_lowphy_puxch_rx_handle* h,
                                                             const int16_t*                d_input,
                                                             float                         input_scale,
                                                             void*                         output_cbf16,
                                                             int                           nof_symbols,
                                                             bool                          input_is_fft_window)
{
  if (!std::isfinite(input_scale) || (input_scale <= 0.0f) || (nof_symbols <= 0)) {
    return false;
  }

  int nof_batches  = h->cfg.nof_ports * nof_symbols;
  int load_blocks  = (nof_batches * h->cfg.dft_size + THREADS - 1) / THREADS;
  int input_offset = input_is_fft_window ? 0 : (h->cfg.cyclic_prefix_length - h->cfg.window_offset);
  int input_stride = input_is_fft_window ? h->cfg.dft_size : h->cfg.input_nof_samples;
  load_puxch_fft_input_ci16_kernel<<<load_blocks, THREADS, 0, h->stream>>>(
      h->d_freq, d_input, h->cfg.dft_size, input_stride, input_offset, nof_batches, 1.0f / input_scale);
  if (!check_cuda(cudaGetLastError())) {
    return false;
  }

  return enqueue_vkfft_and_extract(h, output_cbf16, nof_symbols);
}

// Scattered zero-copy demod: each batch window is a separate mapped host pointer.
// Skips host packing and explicit H2D; pointer table is passed by value (≤112 ptrs).
static bool enqueue_puxch_demod_ci16_scattered_uncaptured(ocudu_lowphy_puxch_rx_handle* h,
                                                          const scattered_ci16_bases&    bases,
                                                          float                          input_scale,
                                                          void*                          output_cbf16,
                                                          int                            nof_symbols)
{
  if (!std::isfinite(input_scale) || (input_scale <= 0.0f) || (nof_symbols <= 0)) {
    return false;
  }

  const int nof_batches = h->cfg.nof_ports * nof_symbols;
  if ((nof_batches <= 0) || (nof_batches > MAX_SCATTERED_BATCHES)) {
    return false;
  }

  int load_blocks = (nof_batches * h->cfg.dft_size + THREADS - 1) / THREADS;
  load_puxch_fft_input_ci16_scattered_kernel<<<load_blocks, THREADS, 0, h->stream>>>(
      h->d_freq, bases, h->cfg.dft_size, nof_batches, 1.0f / input_scale);
  if (!check_cuda(cudaGetLastError())) {
    return false;
  }

  return enqueue_vkfft_and_extract(h, output_cbf16, nof_symbols);
}

// Register the full host sample buffer (page-friendly) and return a mapped
// device pointer to the FFT window at `sample_offset` complex samples in.
static const int16_t* resolve_mapped_window_in_buffer(ocudu_lowphy_puxch_rx_handle* h,
                                                      const void*                   host_buffer,
                                                      size_t                        buffer_bytes,
                                                      size_t                        window_sample_offset,
                                                      size_t                        window_bytes)
{
  if ((h == nullptr) || (host_buffer == nullptr) || (buffer_bytes == 0) || (window_bytes == 0)) {
    return nullptr;
  }
  if ((window_sample_offset * sizeof(int16_t) * 2U + window_bytes) > buffer_bytes) {
    return nullptr;
  }
  // Register the whole buffer so cudaHostRegister sees a typical allocation base
  // (windows alone are rarely page-aligned).
  if (!register_host_range(h, host_buffer, buffer_bytes)) {
    return nullptr;
  }
  const auto* host_window =
      static_cast<const int16_t*>(host_buffer) + window_sample_offset * 2U;
  return resolve_mapped_window(h, host_window, window_bytes);
}

// Build mapped base pointers for each symbol×port FFT window. Returns false if
// any window cannot be mapped (caller should fall back to pack+H2D/ZC staging).
static bool resolve_scattered_symbol_batch_bases(ocudu_lowphy_puxch_rx_handle* h,
                                                 const void* const*            input_symbols_ports_ci16,
                                                 int                           nof_symbols,
                                                 scattered_ci16_bases*         bases_out)
{
  if ((h == nullptr) || (input_symbols_ports_ci16 == nullptr) || (bases_out == nullptr) || (nof_symbols <= 0)) {
    return false;
  }

  const size_t window_bytes = static_cast<size_t>(h->cfg.dft_size) * 2U * sizeof(int16_t);
  const unsigned default_off =
      static_cast<unsigned>(h->cfg.cyclic_prefix_length - h->cfg.window_offset);

  scattered_ci16_bases bases{};
  for (int symbol = 0; symbol != nof_symbols; ++symbol) {
    unsigned symbol_input_offset = default_off;
    size_t   buffer_samples      = static_cast<size_t>(h->cfg.input_nof_samples);
    if (h->cfg.use_symbol_layout != 0) {
      symbol_input_offset =
          static_cast<unsigned>(h->cfg.symbol_cyclic_prefix_length[symbol] - h->cfg.symbol_window_offset[symbol]);
      buffer_samples = static_cast<size_t>(h->cfg.symbol_input_nof_samples[symbol]);
    }
    const size_t buffer_bytes = buffer_samples * 2U * sizeof(int16_t);
    for (int port = 0; port != h->cfg.nof_ports; ++port) {
      const void* host_buf = input_symbols_ports_ci16[symbol * h->cfg.nof_ports + port];
      const int   batch    = symbol * h->cfg.nof_ports + port;
      const int16_t* mapped = resolve_mapped_window_in_buffer(
          h, host_buf, buffer_bytes, static_cast<size_t>(symbol_input_offset), window_bytes);
      if (mapped == nullptr) {
        return false;
      }
      bases.ptr[batch] = mapped;
    }
  }
  *bases_out = bases;
  return true;
}

static bool resolve_scattered_ports_bases(ocudu_lowphy_puxch_rx_handle* h,
                                          const void* const*            input_ports_ci16,
                                          scattered_ci16_bases*         bases_out)
{
  if ((h == nullptr) || (input_ports_ci16 == nullptr) || (bases_out == nullptr)) {
    return false;
  }
  const size_t   window_bytes  = static_cast<size_t>(h->cfg.dft_size) * 2U * sizeof(int16_t);
  const unsigned input_offset  = static_cast<unsigned>(h->cfg.cyclic_prefix_length - h->cfg.window_offset);
  const size_t   buffer_bytes  = static_cast<size_t>(h->cfg.input_nof_samples) * 2U * sizeof(int16_t);

  scattered_ci16_bases bases{};
  for (int port = 0; port != h->cfg.nof_ports; ++port) {
    const int16_t* mapped = resolve_mapped_window_in_buffer(
        h, input_ports_ci16[port], buffer_bytes, static_cast<size_t>(input_offset), window_bytes);
    if (mapped == nullptr) {
      return false;
    }
    bases.ptr[port] = mapped;
  }
  *bases_out = bases;
  return true;
}

static bool finish_demod_with_event(ocudu_lowphy_puxch_rx_handle* h, bool enqueued, bool record_completion_event)
{
  h->done_event_recorded = false;
  if (!enqueued) {
    return false;
  }
  if (!record_completion_event) {
    return true;
  }
  if (!check_cuda(cudaEventRecord(h->done_event, h->stream))) {
    return false;
  }
  h->done_event_recorded = true;
  return true;
}

static bool graph_configs_match(const ocudu_lowphy_puxch_rx_config_t& lhs, const ocudu_lowphy_puxch_rx_config_t& rhs)
{
  return std::memcmp(&lhs, &rhs, sizeof(lhs)) == 0;
}

static ocudu_lowphy_puxch_rx_handle::device_graph_entry*
find_device_graph(ocudu_lowphy_puxch_rx_handle* h,
                  const int16_t*                d_input,
                  float                         input_scale,
                  int                           nof_symbols,
                  bool                          input_is_fft_window)
{
  for (auto& entry : h->device_graphs) {
    // Resource grids rotate in the live receiver. Keep the graph keyed on the invariant demodulation parameters
    // and patch the extract kernel output pointer before each launch.
    if ((entry.d_input == d_input) && (entry.stream == h->stream) && (entry.input_scale == input_scale) &&
        (entry.nof_symbols == nof_symbols) && (entry.input_is_fft_window == input_is_fft_window) &&
        graph_configs_match(entry.cfg, h->cfg) && (entry.exec != nullptr) && (entry.extract_node != nullptr)) {
      return &entry;
    }
  }
  return nullptr;
}

static cudaGraphNode_t find_extract_kernel_node(cudaGraph_t graph, int nof_symbols)
{
  size_t nof_nodes = 0;
  if ((graph == nullptr) || (cudaGraphGetNodes(graph, nullptr, &nof_nodes) != cudaSuccess) || (nof_nodes == 0)) {
    (void)cudaGetLastError();
    return nullptr;
  }

  std::vector<cudaGraphNode_t> nodes(nof_nodes);
  if (cudaGraphGetNodes(graph, nodes.data(), &nof_nodes) != cudaSuccess) {
    (void)cudaGetLastError();
    return nullptr;
  }

  for (cudaGraphNode_t node : nodes) {
    cudaGraphNodeType type;
    if ((cudaGraphNodeGetType(node, &type) != cudaSuccess) || (type != cudaGraphNodeTypeKernel)) {
      continue;
    }

    cudaKernelNodeParams params = {};
    if (cudaGraphKernelNodeGetParams(node, &params) != cudaSuccess) {
      continue;
    }
    void* expected_func = (nof_symbols > 1) ? reinterpret_cast<void*>(extract_puxch_grid_symbol_batch_kernel)
                                           : reinterpret_cast<void*>(extract_puxch_grid_kernel);
    if (params.func == expected_func) {
      return node;
    }
  }

  return nullptr;
}

static bool update_graph_extract_output(ocudu_lowphy_puxch_rx_handle::device_graph_entry& entry,
                                        ocudu_lowphy_puxch_rx_handle*                     h,
                                        void*                                             output_cbf16)
{
  if ((entry.exec == nullptr) || (entry.extract_node == nullptr) || (output_cbf16 == nullptr)) {
    return false;
  }

  uint32_t*             output         = static_cast<uint32_t*>(output_cbf16);
  const cuFloatComplex* d_freq         = h->d_freq;
  const cuFloatComplex* d_window_phase = h->d_window_phase;
  extract_kernel_params extract_params = make_extract_params(h->cfg);
  int                   nof_symbols    = entry.nof_symbols;
  void*                 args[]         = {&output, &d_freq, &d_window_phase, &extract_params, &nof_symbols};
  void*                 single_args[]  = {&output, &d_freq, &d_window_phase, &extract_params};

  cudaKernelNodeParams params = {};
  params.func = (entry.nof_symbols > 1) ? reinterpret_cast<void*>(extract_puxch_grid_symbol_batch_kernel)
                                        : reinterpret_cast<void*>(extract_puxch_grid_kernel);
  params.gridDim = (entry.nof_symbols > 1)
                       ? dim3((entry.nof_symbols * h->cfg.nof_ports * h->cfg.rg_size + THREADS - 1) / THREADS)
                       : dim3((h->cfg.nof_ports * h->cfg.rg_size + THREADS - 1) / THREADS);
  params.blockDim             = dim3(THREADS);
  params.sharedMemBytes       = 0;
  params.kernelParams         = (entry.nof_symbols > 1) ? args : single_args;
  params.extra                = nullptr;

  if (cudaGraphExecKernelNodeSetParams(entry.exec, entry.extract_node, &params) != cudaSuccess) {
    (void)cudaGetLastError();
    return false;
  }

  entry.output_cbf16 = output_cbf16;
  return true;
}

static bool launch_device_graph(ocudu_lowphy_puxch_rx_handle::device_graph_entry& entry,
                                ocudu_lowphy_puxch_rx_handle*                     h,
                                void*                                             output_cbf16)
{
  if (!update_graph_extract_output(entry, h, output_cbf16)) {
    return false;
  }
  return cudaGraphLaunch(entry.exec, h->stream) == cudaSuccess;
}

static ocudu_lowphy_puxch_rx_handle::device_graph_entry* reserve_device_graph_slot(ocudu_lowphy_puxch_rx_handle* h)
{
  if (h->device_graphs.size() < DEVICE_GRAPH_CACHE_LIMIT) {
    h->device_graphs.push_back({});
    return &h->device_graphs.back();
  }

  auto& entry                      = h->device_graphs[h->next_device_graph_replacement % h->device_graphs.size()];
  h->next_device_graph_replacement = (h->next_device_graph_replacement + 1) % h->device_graphs.size();
  destroy_device_graph_entry(entry);
  return &entry;
}

static bool capture_device_graph(ocudu_lowphy_puxch_rx_handle* h,
                                 const int16_t*                d_input,
                                 float                         input_scale,
                                 void*                         output_cbf16,
                                 int                           nof_symbols,
                                 bool                          input_is_fft_window)
{
  cudaGraph_t     captured_graph = nullptr;
  cudaGraphExec_t graph_exec     = nullptr;

  if (cudaStreamBeginCapture(h->stream, cudaStreamCaptureModeThreadLocal) != cudaSuccess) {
    (void)cudaGetLastError();
    h->device_graph_capture_failed = true;
    return false;
  }

  bool enqueue_ok = (nof_symbols > 1)
                        ? enqueue_puxch_demod_ci16_symbol_batch_uncaptured(
                              h, d_input, input_scale, output_cbf16, nof_symbols, input_is_fft_window)
                        : enqueue_puxch_demod_ci16_uncaptured(
                              h, d_input, input_scale, output_cbf16, input_is_fft_window);
  cudaError_t end_status = cudaStreamEndCapture(h->stream, &captured_graph);
  if (!enqueue_ok || (end_status != cudaSuccess) || (captured_graph == nullptr)) {
    if (captured_graph != nullptr) {
      cudaGraphDestroy(captured_graph);
    }
    (void)cudaGetLastError();
    h->device_graph_capture_failed = true;
    return false;
  }

  if (cudaGraphInstantiate(&graph_exec, captured_graph, nullptr, nullptr, 0) != cudaSuccess) {
    cudaGraphDestroy(captured_graph);
    (void)cudaGetLastError();
    h->device_graph_capture_failed = true;
    return false;
  }

  cudaGraphNode_t extract_node = find_extract_kernel_node(captured_graph, nof_symbols);
  if (extract_node == nullptr) {
    cudaGraphExecDestroy(graph_exec);
    cudaGraphDestroy(captured_graph);
    h->device_graph_capture_failed = true;
    return false;
  }

  auto* entry                = reserve_device_graph_slot(h);
  entry->d_input             = d_input;
  entry->output_cbf16        = output_cbf16;
  entry->stream              = h->stream;
  entry->input_scale         = input_scale;
  entry->nof_symbols         = nof_symbols;
  entry->input_is_fft_window = input_is_fft_window;
  entry->cfg                 = h->cfg;
  entry->graph               = captured_graph;
  entry->exec                = graph_exec;
  entry->extract_node        = extract_node;
  return true;
}

static bool enqueue_puxch_demod_ci16(ocudu_lowphy_puxch_rx_handle* h,
                                     const int16_t*                d_input,
                                     float                         input_scale,
                                     void*                         output_cbf16,
                                     int                           nof_symbols,
                                     bool                          input_is_fft_window,
                                     bool                          record_completion_event)
{
  bool enqueued          = false;
  h->done_event_recorded = false;
  nof_symbols            = active_nof_symbols(nof_symbols);

  if (!cuda_graphs_enabled() || h->device_graph_capture_failed) {
    enqueued = (nof_symbols > 1)
                   ? enqueue_puxch_demod_ci16_symbol_batch_uncaptured(
                         h, d_input, input_scale, output_cbf16, nof_symbols, input_is_fft_window)
                   : enqueue_puxch_demod_ci16_uncaptured(h, d_input, input_scale, output_cbf16, input_is_fft_window);
  } else if (auto* entry = find_device_graph(h, d_input, input_scale, nof_symbols, input_is_fft_window);
             entry != nullptr) {
    enqueued = launch_device_graph(*entry, h, output_cbf16);
    if (!enqueued) {
      destroy_device_graph_entry(*entry);
      (void)cudaGetLastError();
    }
  }

  if (!enqueued && cuda_graphs_enabled() && !h->device_graph_capture_failed) {
    std::lock_guard<std::mutex> lock(graph_capture_mutex());
    if (auto* entry = find_device_graph(h, d_input, input_scale, nof_symbols, input_is_fft_window); entry != nullptr) {
      enqueued = launch_device_graph(*entry, h, output_cbf16);
      if (!enqueued) {
        destroy_device_graph_entry(*entry);
        (void)cudaGetLastError();
      }
    }
    if (!enqueued &&
        capture_device_graph(h, d_input, input_scale, output_cbf16, nof_symbols, input_is_fft_window)) {
      if (auto* entry = find_device_graph(h, d_input, input_scale, nof_symbols, input_is_fft_window);
          entry != nullptr) {
        enqueued = launch_device_graph(*entry, h, output_cbf16);
        if (!enqueued) {
          destroy_device_graph_entry(*entry);
          (void)cudaGetLastError();
        }
      }
    }
  }

  if (!enqueued) {
    enqueued = (nof_symbols > 1)
                   ? enqueue_puxch_demod_ci16_symbol_batch_uncaptured(
                         h, d_input, input_scale, output_cbf16, nof_symbols, input_is_fft_window)
                   : enqueue_puxch_demod_ci16_uncaptured(h, d_input, input_scale, output_cbf16, input_is_fft_window);
  }

  if (!enqueued) {
    return false;
  }
  if (!record_completion_event) {
    return true;
  }
  if (!check_cuda(cudaEventRecord(h->done_event, h->stream))) {
    h->done_event_recorded = false;
    return false;
  }
  h->done_event_recorded = true;
  return true;
}

static bool warmup_handle(ocudu_lowphy_puxch_rx_handle* h)
{
  if ((h == nullptr) || (h->d_input_ci16 == nullptr) || (h->d_warmup_output == nullptr)) {
    return false;
  }

  cudaStream_t saved_stream             = h->stream;
  int          saved_active_nof_symbols = h->active_nof_symbols;
  h->active_nof_symbols                 = 1;
  h->stream                             = (h->owned_stream != nullptr) ? h->owned_stream : h->stream;
  if ((h->stream == nullptr) || !ensure_buffers(h) || !ensure_plan(h) || !ensure_window_phase(h)) {
    h->active_nof_symbols = saved_active_nof_symbols;
    h->stream             = saved_stream;
    return false;
  }

  if (!check_cuda(cudaMemsetAsync(h->d_input_ci16, 0, h->window_ci16_count * sizeof(int16_t), h->stream))) {
    h->active_nof_symbols = saved_active_nof_symbols;
    h->stream             = saved_stream;
    return false;
  }

  ocudu_lowphy_puxch_rx_config_t saved_cfg = h->cfg;
  h->cfg.grid_nof_subc                     = h->cfg.rg_size;
  h->cfg.grid_nof_symbols                  = 1;
  h->cfg.symbol_index                      = 0;
  for (int port = 0; port != h->cfg.nof_ports; ++port) {
    h->cfg.port_indices[port] = port;
  }
  bool ok = enqueue_puxch_demod_ci16_uncaptured(h, h->d_input_ci16, 1.0f, h->d_warmup_output, true) &&
            check_cuda(cudaStreamSynchronize(h->stream));
  h->cfg                = saved_cfg;
  h->active_nof_symbols = saved_active_nof_symbols;
  h->stream             = saved_stream;
  return ok;
}

static bool prime_current_config(ocudu_lowphy_puxch_rx_handle* h, float input_scale, cudaStream_t stream)
{
  if ((h == nullptr) || (h->d_input_ci16 == nullptr) || (h->d_warmup_output == nullptr) ||
      !std::isfinite(input_scale) || (input_scale <= 0.0f)) {
    return false;
  }

  cudaStream_t saved_stream             = h->stream;
  int          saved_active_nof_symbols = h->active_nof_symbols;
  h->active_nof_symbols                 = 1;
  h->stream = (stream != nullptr) ? stream : ((h->owned_stream != nullptr) ? h->owned_stream : h->stream);
  if ((h->stream == nullptr) || !ensure_buffers(h) || !ensure_plan(h) || !ensure_window_phase(h)) {
    h->active_nof_symbols = saved_active_nof_symbols;
    h->stream             = saved_stream;
    return false;
  }

  if (!check_cuda(cudaMemsetAsync(h->d_input_ci16, 0, h->window_ci16_count * sizeof(int16_t), h->stream))) {
    h->active_nof_symbols = saved_active_nof_symbols;
    h->stream             = saved_stream;
    return false;
  }

  const bool ok = enqueue_puxch_demod_ci16(h, h->d_input_ci16, input_scale, h->d_warmup_output, 1, true, false) &&
                  check_cuda(cudaStreamSynchronize(h->stream));
  h->active_nof_symbols = saved_active_nof_symbols;
  h->stream             = saved_stream;
  return ok;
}

} // namespace

int ocudu_lowphy_puxch_rx_create(const ocudu_lowphy_puxch_rx_config_t* config, ocudu_lowphy_puxch_rx_handle_t** handle)
{
  if ((config == nullptr) || (handle == nullptr) || !validate_config(*config)) {
    return 0;
  }

  auto* h = new ocudu_lowphy_puxch_rx_handle();
  h->cfg  = *config;
  if (!check_cuda(create_realtime_stream(&h->owned_stream)) ||
      !check_cuda(cudaEventCreateWithFlags(&h->done_event, cudaEventDisableTiming))) {
    ocudu_lowphy_puxch_rx_destroy(h);
    return 0;
  }
  h->stream = h->owned_stream;

  if (!ensure_buffers(h) || !ensure_plan(h) || !ensure_window_phase(h) || !warmup_handle(h)) {
    ocudu_lowphy_puxch_rx_destroy(h);
    return 0;
  }

  *handle = h;
  return 1;
}

void ocudu_lowphy_puxch_rx_destroy(ocudu_lowphy_puxch_rx_handle_t* handle)
{
  if (handle == nullptr) {
    return;
  }
  destroy_fft_plan(handle);
  release_buffers(handle);
  for (const auto& registration : handle->registered_hosts) {
    if (registration.unregister_on_destroy) {
      cudaHostUnregister(const_cast<void*>(registration.ptr));
    }
  }
  handle->registered_hosts.clear();
  handle->unregistered_hosts.clear();
  if (handle->done_event != nullptr) {
    cudaEventDestroy(handle->done_event);
    handle->done_event = nullptr;
  }
  if (handle->owned_stream != nullptr) {
    cudaStreamDestroy(handle->owned_stream);
    handle->owned_stream = nullptr;
  }
  delete handle;
}

int ocudu_lowphy_puxch_rx_update_config(ocudu_lowphy_puxch_rx_handle_t*       handle,
                                        const ocudu_lowphy_puxch_rx_config_t* config)
{
  if ((handle == nullptr) || (config == nullptr) || !validate_config(*config)) {
    return 0;
  }

  bool plan_changed = (handle->cfg.dft_size != config->dft_size) || (handle->cfg.nof_ports != config->nof_ports);
  handle->cfg       = *config;
  if (plan_changed) {
    destroy_fft_plan(handle);
  }
  if (!ensure_buffers(handle) || !ensure_plan(handle) || !ensure_window_phase(handle)) {
    return 0;
  }
  return !plan_changed || warmup_handle(handle);
}

int ocudu_lowphy_puxch_rx_prime_config(ocudu_lowphy_puxch_rx_handle_t*       handle,
                                       const ocudu_lowphy_puxch_rx_config_t* config,
                                       float                                 input_scale,
                                       void*                                 external_stream)
{
  if ((handle == nullptr) || (config == nullptr) || !validate_config(*config)) {
    return 0;
  }

  bool plan_changed = (handle->cfg.dft_size != config->dft_size) || (handle->cfg.nof_ports != config->nof_ports);
  handle->cfg       = *config;
  if (plan_changed) {
    destroy_fft_plan(handle);
  }
  if (!ensure_buffers(handle) || !ensure_plan(handle) || !ensure_window_phase(handle)) {
    return 0;
  }

  return prime_current_config(handle, input_scale, static_cast<cudaStream_t>(external_stream)) ? 1 : 0;
}

int ocudu_lowphy_puxch_rx_process_ci16(ocudu_lowphy_puxch_rx_handle_t* handle,
                                       const void*                     input_ci16,
                                       float                           input_scale,
                                       void*                           output_cbf16,
                                       void*                           external_stream)
{
  if ((handle == nullptr) || (input_ci16 == nullptr) || (output_cbf16 == nullptr) || (handle->cfg.nof_ports != 1)) {
    return 0;
  }
  const bool record_completion_event = (external_stream == nullptr);
  handle->active_nof_symbols         = 1;
  handle->stream = (external_stream != nullptr) ? static_cast<cudaStream_t>(external_stream) : handle->owned_stream;
  if (!ensure_buffers(handle) || !ensure_plan(handle) || !ensure_window_phase(handle)) {
    return 0;
  }

  const int16_t* d_input             = static_cast<const int16_t*>(input_ci16);
  bool           input_is_fft_window = false;
  if (!handle->cfg.input_is_device) {
    const void* input_ports[1] = {input_ci16};

    // Prefer scattered mapped load (no host pack) when the window set is large enough.
    if (scattered_zero_copy_worthwhile(handle, 1)) {
      scattered_ci16_bases bases{};
      if (resolve_scattered_ports_bases(handle, input_ports, &bases)) {
        const bool enqueued =
            enqueue_puxch_demod_ci16_scattered_uncaptured(handle, bases, input_scale, output_cbf16, 1);
        return finish_demod_with_event(handle, enqueued, record_completion_event) ? 1 : 0;
      }
    }

    bool direct_copy_enqueued = false;
    // Single-port: optional host registration avoids an extra host pack via H2D.
    if (runtime_host_registration_enabled()) {
      direct_copy_enqueued = copy_registered_ci16_windows_to_device(handle, input_ports);
      if (!direct_copy_enqueued) {
        (void)cudaGetLastError();
      }
    }
    if (!direct_copy_enqueued) {
      int      staging_slot = -1;
      int16_t* staged       = acquire_ci16_staging_slot(handle, &staging_slot);
      if (staged == nullptr) {
        return 0;
      }
      copy_puxch_fft_window_to_staging(handle, staged, input_ports);
      d_input = publish_staged_ci16(handle, staged, handle->window_ci16_count, staging_slot);
      if (d_input == nullptr) {
        return 0;
      }
    } else {
      d_input = handle->d_input_ci16;
    }
    input_is_fft_window = true;
  } else {
    // Device-resident callers may pass either a full CP+window buffer (input_is_fft_window=false)
    // or a packed FFT window of length dft_size (input_nof_samples==dft_size and zero CP/offset).
    input_is_fft_window = (handle->cfg.input_nof_samples == handle->cfg.dft_size) &&
                          (handle->cfg.cyclic_prefix_length == 0) && (handle->cfg.window_offset == 0);
  }

  return enqueue_puxch_demod_ci16(
             handle, d_input, input_scale, output_cbf16, 1, input_is_fft_window, record_completion_event)
             ? 1
             : 0;
}

int ocudu_lowphy_puxch_rx_process_ci16_ports(ocudu_lowphy_puxch_rx_handle_t* handle,
                                             const void* const*              input_ports_ci16,
                                             float                           input_scale,
                                             void*                           output_cbf16,
                                             void*                           external_stream)
{
  if ((handle == nullptr) || (input_ports_ci16 == nullptr) || (output_cbf16 == nullptr)) {
    return 0;
  }
  const bool record_completion_event = (external_stream == nullptr);
  handle->active_nof_symbols         = 1;
  handle->stream = (external_stream != nullptr) ? static_cast<cudaStream_t>(external_stream) : handle->owned_stream;
  if (!ensure_buffers(handle) || !ensure_plan(handle) || !ensure_window_phase(handle)) {
    return 0;
  }

  if (handle->cfg.input_is_device) {
    return 0;
  }

  for (int port = 0; port != handle->cfg.nof_ports; ++port) {
    if (input_ports_ci16[port] == nullptr) {
      return 0;
    }
  }

  // Scattered mapped load: skip host pack when every port window is mappable.
  if (scattered_zero_copy_worthwhile(handle, 1)) {
    scattered_ci16_bases bases{};
    if (resolve_scattered_ports_bases(handle, input_ports_ci16, &bases)) {
      const bool enqueued =
          enqueue_puxch_demod_ci16_scattered_uncaptured(handle, bases, input_scale, output_cbf16, 1);
      return finish_demod_with_event(handle, enqueued, record_completion_event) ? 1 : 0;
    }
  }

  bool direct_copy_enqueued = false;
  if (runtime_host_registration_enabled()) {
    direct_copy_enqueued = copy_registered_ci16_windows_to_device(handle, input_ports_ci16);
    if (!direct_copy_enqueued) {
      (void)cudaGetLastError();
    }
  }
  const int16_t* d_input = handle->d_input_ci16;
  if (!direct_copy_enqueued) {
    int      staging_slot = -1;
    int16_t* staged       = acquire_ci16_staging_slot(handle, &staging_slot);
    if (staged == nullptr) {
      return 0;
    }
    copy_puxch_fft_window_to_staging(handle, staged, input_ports_ci16);
    d_input = publish_staged_ci16(handle, staged, handle->window_ci16_count, staging_slot);
    if (d_input == nullptr) {
      return 0;
    }
  }

  return enqueue_puxch_demod_ci16(handle, d_input, input_scale, output_cbf16, 1, true, record_completion_event) ? 1
                                                                                                              : 0;
}

int ocudu_lowphy_puxch_rx_process_ci16_symbol_batch_ports(ocudu_lowphy_puxch_rx_handle_t* handle,
                                                          const void* const*              input_symbols_ports_ci16,
                                                          int                             nof_symbols,
                                                          float                           input_scale,
                                                          void*                           output_cbf16,
                                                          void*                           external_stream)
{
  if ((handle == nullptr) || (input_symbols_ports_ci16 == nullptr) || (output_cbf16 == nullptr) || (nof_symbols <= 0) ||
      (nof_symbols > OCUDU_LOWPHY_PUXCH_RX_MAX_SYMBOLS) ||
      ((handle->cfg.symbol_index + nof_symbols) > handle->cfg.grid_nof_symbols)) {
    return 0;
  }

  if (handle->cfg.input_is_device) {
    return 0;
  }

  for (int symbol = 0; symbol != nof_symbols; ++symbol) {
    for (int port = 0; port != handle->cfg.nof_ports; ++port) {
      if (input_symbols_ports_ci16[symbol * handle->cfg.nof_ports + port] == nullptr) {
        return 0;
      }
    }
  }

  const bool record_completion_event = (external_stream == nullptr);
  handle->active_nof_symbols         = nof_symbols;
  handle->stream = (external_stream != nullptr) ? static_cast<cudaStream_t>(external_stream) : handle->owned_stream;
  if (!ensure_buffers(handle) || !ensure_plan(handle) || !ensure_window_phase(handle)) {
    return 0;
  }
  handle->done_event_recorded = false;

  // Primary fast path on integrated GPUs: map each symbol×port window and load
  // straight into the FFT buffer — avoids ~0.5–1 MB of host packing per slot batch.
  if (scattered_zero_copy_worthwhile(handle, nof_symbols)) {
    scattered_ci16_bases bases{};
    if (resolve_scattered_symbol_batch_bases(handle, input_symbols_ports_ci16, nof_symbols, &bases)) {
      const bool enqueued = enqueue_puxch_demod_ci16_scattered_uncaptured(
          handle, bases, input_scale, output_cbf16, nof_symbols);
      return finish_demod_with_event(handle, enqueued, record_completion_event) ? 1 : 0;
    }
  }

  bool direct_copy_enqueued = false;
  if (runtime_host_registration_enabled()) {
    direct_copy_enqueued =
        copy_registered_ci16_symbol_batch_windows_to_device(handle, input_symbols_ports_ci16, nof_symbols);
    if (!direct_copy_enqueued) {
      (void)cudaGetLastError();
    }
  }
  const int16_t* d_input = handle->d_input_ci16;
  if (!direct_copy_enqueued) {
    int      staging_slot = -1;
    int16_t* staged       = acquire_ci16_staging_slot(handle, &staging_slot);
    if (staged == nullptr) {
      return 0;
    }
    copy_puxch_fft_window_symbol_batch_to_staging(handle, staged, input_symbols_ports_ci16, nof_symbols);
    d_input = publish_staged_ci16(handle, staged, handle->window_ci16_count, staging_slot);
    if (d_input == nullptr) {
      return 0;
    }
  }

  return enqueue_puxch_demod_ci16(
             handle, d_input, input_scale, output_cbf16, nof_symbols, true, record_completion_event)
             ? 1
             : 0;
}

int ocudu_lowphy_puxch_rx_synchronize(ocudu_lowphy_puxch_rx_handle_t* handle)
{
  if ((handle == nullptr) || (handle->done_event == nullptr)) {
    return 0;
  }
  if (!handle->done_event_recorded) {
    return cudaStreamSynchronize(handle->stream ? handle->stream : handle->owned_stream) == cudaSuccess;
  }
  // cudaEventSynchronize has lower overhead than a yield-poll loop for short low-PHY
  // kernels (unit-test and single-symbol paths complete in ~10-50 us).
  return cudaEventSynchronize(handle->done_event) == cudaSuccess;
}

void* ocudu_lowphy_puxch_rx_get_completion_event(ocudu_lowphy_puxch_rx_handle_t* handle)
{
  return (handle == nullptr) ? nullptr : static_cast<void*>(handle->done_event);
}

void* ocudu_lowphy_puxch_rx_get_stream(ocudu_lowphy_puxch_rx_handle_t* handle)
{
  return (handle == nullptr) ? nullptr : static_cast<void*>(handle->stream ? handle->stream : handle->owned_stream);
}
