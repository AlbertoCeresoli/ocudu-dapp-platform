// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear
// Linker wrappers are compiled into the standalone test, never into gNB or a dApp.
#include "pusch_test_receiver_capture.h"
#include "ocudu/dapp/runtime/native_hook_point.h"
#include "ocudu/ran/resource_block.h"
#include "ldpc_decoder.h"
#include "rate_matching.h"
#include "scrambling.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace {
constexpr unsigned width = 48, symbols = 14, grid_width = 3300;
constexpr unsigned grid_count = symbols * grid_width, data_count = 12 * width, max_llrs = data_count * 8;
static_assert(ocudu::MAX_NOF_SUBCARRIERS == grid_width);
struct captured_tensors {
  std::array<uint32_t, grid_count> grid;
  std::array<ocudu::cf_t, width> pilots;
  std::array<uint16_t, 2 * width> coordinates;
  std::array<uint32_t, data_count> indices;
  std::array<uint16_t, max_llrs> before_descramble;
  std::array<uint16_t, max_llrs> decoder;
};
struct capture_state {
  captured_tensors* host{};
  unsigned qm{}, mcs{}, frame{}, invokes{}, pre{}, decoded{}, route{};
  bool neural{}, active{}, pending{};
  const void* output{};
  cudaStream_t stream{};
  const char* error{};
  void reject(const char* why) noexcept { if (!error) error = why; }
  void copy(void* dest, const void* src, size_t bytes, cudaStream_t on) noexcept
  {
    if (error) return;
    if (!src || !on || (stream && stream != on)) { reject("capture stream/pointer mismatch"); return; }
    stream = on;
    if (cudaMemcpyAsync(dest, src, bytes, cudaMemcpyDeviceToHost, on) != cudaSuccess) {
      reject("capture D2H enqueue failed");
      return;
    }
    pending = true;
  }
};
// The CLI enforces a single worker and serialized process/require_idle pairs.
capture_state* current{};
capture_state* active() noexcept { return current && current->active ? current : nullptr; }
bool dense(const ocudu_dapp_tensor_view_v1& tensor, uint32_t element, std::initializer_list<uint64_t> shape,
           unsigned element_bytes)
{
  if (tensor.element_type != element || tensor.rank != shape.size() || !tensor.native_handle ||
      tensor.domain != OCUDU_DAPP_MEM_ACCELERATOR_DEVICE_V1 || tensor.backend_id != OCUDU_DAPP_BACKEND_CUDA_V1) return false;
  unsigned dim = 0;
  for (uint64_t extent : shape) if (tensor.dimensions[dim++] != extent) return false;
  uint64_t bytes = element_bytes;
  while (dim) {
    --dim;
    if (tensor.byte_strides[dim] != bytes) return false;
    bytes *= tensor.dimensions[dim];
  }
  return tensor.byte_length >= bytes && tensor.byte_offset <= UINT64_MAX - tensor.native_handle;
}
const void* address(const ocudu_dapp_tensor_view_v1& tensor)
{
  return reinterpret_cast<const void*>(uintptr_t(tensor.native_handle + tensor.byte_offset));
}
void capture_input(const ocudu_dapp_receiver_input_v1& input, const ocudu_dapp_receiver_output_v1& output) noexcept
{
  auto* s = active();
  if (!s) return;
  if (++s->invokes != 1 || !s->neural || input.pusch.metadata.size != sizeof(ocudu_dapp_pusch_metadata_v1) ||
      input.dmrs.metadata.size != sizeof(ocudu_dapp_dmrs_metadata_v1) ||
      !input.pusch.metadata.data || !input.dmrs.metadata.data) {
    s->reject("capture requires exactly one complete native receiver invocation"); return;
  }
  const auto& p = *static_cast<const ocudu_dapp_pusch_metadata_v1*>(input.pusch.metadata.data);
  const auto& d = *static_cast<const ocudu_dapp_dmrs_metadata_v1*>(input.dmrs.metadata.data);
  if (p.nof_rx_ports != 1 || p.nof_layers != 1 || unsigned(p.modulation) != s->qm ||
      p.start_symbol != 0 || p.nof_symbols != symbols || p.start_subcarrier != 0 || p.nof_subcarriers != width ||
      p.nof_data_re != data_count || p.reserved[1] != 30000 || p.reserved[2] != grid_width || p.reserved[3] != 0 ||
      d.nof_pilots != width || d.dmrs_type != 1 || d.reserved[0] != ((1U << 2) | (1U << 11)) ||
      !dense(input.rx_grid, OCUDU_DAPP_ELEMENT_CBF16_V1, {1, symbols, grid_width}, 4) ||
      !dense(d.reference_symbols, OCUDU_DAPP_ELEMENT_CF32_V1, {1, width}, 8) ||
      !dense(d.coordinates, OCUDU_DAPP_ELEMENT_U16_V1, {width, 2}, 2) ||
      !dense(p.data_re_indices, OCUDU_DAPP_ELEMENT_U32_V1, {data_count}, 4) ||
      !dense(output.llrs, OCUDU_DAPP_ELEMENT_F16_V1, {data_count, 1, s->qm}, 2)) {
    s->reject("capture descriptor outside explicit diagnostic envelope"); return;
  }
  const auto stream = reinterpret_cast<cudaStream_t>(input.invocation.execution_stream);
  s->output = address(output.llrs);
  s->copy(s->host->grid.data(), address(input.rx_grid), sizeof(s->host->grid), stream);
  s->copy(s->host->pilots.data(), address(d.reference_symbols), sizeof(s->host->pilots), stream);
  s->copy(s->host->coordinates.data(), address(d.coordinates), sizeof(s->host->coordinates), stream);
  s->copy(s->host->indices.data(), address(p.data_re_indices), sizeof(s->host->indices), stream);
}
void capture_decoder(const void* llrs, int qm, int count, const unsigned int* scrambling,
                     int scramble_offset, cudaStream_t stream, unsigned route) noexcept
{
  auto* s = active();
  if (!s) return;
  if (++s->decoded != 1 || qm != int(s->qm) || count != 1 || scrambling || scramble_offset != 0 ||
      (s->neural && (llrs != s->output || s->pre != 1))) {
    s->reject("capture requires one already-descrambled codeword at real decoder boundary"); return;
  }
  s->route = route;
  s->copy(s->host->decoder.data(), llrs, data_count * s->qm * sizeof(uint16_t), stream);
}
void write(std::FILE* file, const void* data, size_t bytes)
{
  if (std::fwrite(data, 1, bytes, file) != bytes) throw std::runtime_error("capture output write failed");
}
} // namespace

struct ocudu::test_support::pusch_test_receiver_capture::implementation {
  std::string prefix;
  capture_state state;
  std::FILE* file{};
  ~implementation()
  {
    // Never release DMA targets after an uncertain/exceptional decoder completion.
    if (state.pending) std::terminate();
    if (file) std::fclose(file);
    if (state.host) cudaFreeHost(state.host);
    if (current == &state) current = nullptr;
  }
};
ocudu::test_support::pusch_test_receiver_capture::pusch_test_receiver_capture(
    const std::string& prefix, unsigned qm, unsigned mcs, bool neural) : impl(std::make_unique<implementation>())
{
  if (current || prefix.empty() || (qm != 2 && qm != 4 && qm != 6 && qm != 8))
    throw std::runtime_error("invalid/overlapping receiver capture");
  impl->prefix = prefix;
  impl->state.qm = qm;
  impl->state.mcs = mcs;
  impl->state.neural = neural;
  if (cudaHostAlloc(reinterpret_cast<void**>(&impl->state.host), sizeof(captured_tensors), cudaHostAllocDefault) != cudaSuccess)
    throw std::runtime_error("receiver capture pinned allocation failed");
  current = &impl->state;
}
ocudu::test_support::pusch_test_receiver_capture::~pusch_test_receiver_capture() = default;
void ocudu::test_support::pusch_test_receiver_capture::begin(unsigned frame)
{
  auto& s = impl->state;
  if (s.active || s.pending || frame >= 64 || impl->file) throw std::runtime_error("capture frame lifetime violation");
  impl->file = std::fopen((impl->prefix + ".frame-" + std::to_string(frame) + ".raw").c_str(), "wbx");
  if (!impl->file) throw std::runtime_error("capture requires exclusive new output files");
  s.frame = frame; s.invokes = s.pre = s.decoded = s.route = 0;
  s.output = nullptr; s.stream = nullptr; s.error = nullptr; s.active = true;
}
void ocudu::test_support::pusch_test_receiver_capture::finish(
    span<const uint8_t> payload, span<const cbf16_t> tx, span<const cbf16_t> rx)
{
  auto& s = impl->state;
  if (!s.active || !impl->file) throw std::runtime_error("capture finish without frame");
  // Real decode has returned, but independently verify the stream before dereferencing D2H storage.
  // No device-wide fence and no unbounded extra wait; failure stays a failed diagnostic.
  if (s.pending && cudaStreamQuery(s.stream) != cudaSuccess) std::terminate();
  s.pending = false; s.active = false;
  if (s.error) throw std::runtime_error(s.error);
  if (s.decoded != 1 || s.invokes != unsigned(s.neural) || s.pre != unsigned(s.neural) ||
      tx.size() != grid_count || rx.size() != grid_count || payload.empty() || payload.size() > 4096)
    throw std::runtime_error("incomplete receiver capture");
  const std::array<uint32_t, 16> header{{1, s.frame, width, s.qm, grid_width, symbols, data_count, width,
      unsigned(payload.size()), unsigned(s.neural), s.route, s.mcs, s.frame + 1, (1U << 2) | (1U << 11), 30000, 0}};
  write(impl->file, "DSCAP001", 8); write(impl->file, header.data(), sizeof(header));
  write(impl->file, payload.data(), payload.size());
  write(impl->file, tx.data(), tx.size_bytes()); write(impl->file, rx.data(), rx.size_bytes());
  if (s.neural) {
    write(impl->file, s.host->grid.data(), sizeof(s.host->grid));
    write(impl->file, s.host->pilots.data(), sizeof(s.host->pilots));
    write(impl->file, s.host->coordinates.data(), sizeof(s.host->coordinates));
    write(impl->file, s.host->indices.data(), sizeof(s.host->indices));
    write(impl->file, s.host->before_descramble.data(), data_count * s.qm * sizeof(uint16_t));
  }
  write(impl->file, s.host->decoder.data(), data_count * s.qm * sizeof(uint16_t));
  write(impl->file, "DSCAPEND", 8);
  const int status = std::fclose(impl->file); impl->file = nullptr;
  if (status != 0) throw std::runtime_error("receiver capture close failed");
}

#define CAPTURE_INVOKE_SYMBOL "_ZN5ocudu4dapp17native_hook_point24invoke_receiver_deferredEjRK28ocudu_dapp_receiver_input_v1R29ocudu_dapp_receiver_output_v1RNS0_22native_receiver_ticketERSt10shared_ptrINS0_13native_workerEE"
using ocudu::dapp::native_hook_point;
using ocudu::dapp::native_invocation_result;
using ocudu::dapp::native_receiver_ticket;
using ocudu::dapp::native_worker;
native_invocation_result real_capture_invoke(native_hook_point*, uint32_t, const ocudu_dapp_receiver_input_v1&,
    ocudu_dapp_receiver_output_v1&, native_receiver_ticket&, std::shared_ptr<native_worker>&) noexcept asm("__real_" CAPTURE_INVOKE_SYMBOL);
native_invocation_result wrap_capture_invoke(native_hook_point*, uint32_t, const ocudu_dapp_receiver_input_v1&,
    ocudu_dapp_receiver_output_v1&, native_receiver_ticket&, std::shared_ptr<native_worker>&) noexcept asm("__wrap_" CAPTURE_INVOKE_SYMBOL);
native_invocation_result wrap_capture_invoke(native_hook_point* point, uint32_t lane,
    const ocudu_dapp_receiver_input_v1& input, ocudu_dapp_receiver_output_v1& output,
    native_receiver_ticket& ticket, std::shared_ptr<native_worker>& owner) noexcept
{
  capture_input(input, output);
  return real_capture_invoke(point, lane, input, output, ticket, owner);
}

extern "C" nr_ldpc_status_t __real_scrambler_descramble_llr_half_inplace(scrambler_handle_t, void*, int, cudaStream_t);
extern "C" nr_ldpc_status_t __wrap_scrambler_descramble_llr_half_inplace(scrambler_handle_t handle, void* llrs,
                                                                       int count, cudaStream_t stream)
{
  if (auto* s = active(); s && s->neural) {
    if (++s->pre != 1 || s->invokes != 1 || llrs != s->output || count != int(data_count * s->qm))
      s->reject("capture pre-descrambling boundary mismatch");
    else s->copy(s->host->before_descramble.data(), llrs, count * sizeof(uint16_t), stream);
  }
  return __real_scrambler_descramble_llr_half_inplace(handle, llrs, count, stream);
}
extern "C" nr_ldpc_status_t __real_rate_matcher_deinterleave_and_dematch_batch_half(
    rate_matcher_handle_t, const void*, void*, int, int, cudaStream_t, const unsigned int*, int);
extern "C" nr_ldpc_status_t __wrap_rate_matcher_deinterleave_and_dematch_batch_half(
    rate_matcher_handle_t handle, const void* input, void* output, int qm, int count,
    cudaStream_t stream, const unsigned int* scrambling, int offset)
{
  capture_decoder(input, qm, count, scrambling, offset, stream, 1);
  return __real_rate_matcher_deinterleave_and_dematch_batch_half(handle, input, output, qm, count, stream, scrambling, offset);
}
extern "C" nr_ldpc_status_t __real_ldpc_decoder_decode_batch_half_from_rate_matched(ldpc_decoder_handle_t,
    const void*, uint32_t*, int, int, int, int, const int*, const unsigned int*, const unsigned int*, int,
    cudaStream_t, int, const ldpc_fused_tb_output_t*, bool*);
extern "C" nr_ldpc_status_t __wrap_ldpc_decoder_decode_batch_half_from_rate_matched(ldpc_decoder_handle_t handle,
    const void* input, uint32_t* output, int count, int qm, int ncb, int k0, const int* lengths,
    const unsigned int* offsets, const unsigned int* scrambling, int offset, cudaStream_t stream,
    int workspace, const ldpc_fused_tb_output_t* fused, bool* engaged)
{
  capture_decoder(input, qm, count, scrambling, offset, stream, 2);
  return __real_ldpc_decoder_decode_batch_half_from_rate_matched(handle, input, output, count, qm, ncb, k0,
      lengths, offsets, scrambling, offset, stream, workspace, fused, engaged);
}
