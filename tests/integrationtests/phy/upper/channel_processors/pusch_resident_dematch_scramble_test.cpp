// SPDX-FileCopyrightText: Copyright (C) 2021-2026 DeepSig Inc
// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

/// \file
/// \brief Validates resident PUSCH non-uniform dematch fused descrambling offsets.

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cuda_fp16.h>
#include <cuda_runtime.h>
#include <rate_matching.h>
#include <scrambling.h>
#include <vector>

namespace {

bool check_cuda(cudaError_t status, const char* what)
{
  if (status == cudaSuccess) {
    return true;
  }
  std::fprintf(stderr, "%s: %s\n", what, cudaGetErrorString(status));
  return false;
}

bool check_status(nr_ldpc_status_t status, const char* what)
{
  if (status == NR_LDPC_SUCCESS) {
    return true;
  }
  std::fprintf(stderr, "%s: status=%d\n", what, static_cast<int>(status));
  return false;
}

uint16_t float_to_half_bits(float value)
{
  __half   half_value = __float2half(value);
  uint16_t bits       = 0;
  std::memcpy(&bits, &half_value, sizeof(bits));
  return bits;
}

struct device_buffer {
  void*  ptr   = nullptr;
  size_t bytes = 0;

  explicit device_buffer(size_t nof_bytes) : bytes(nof_bytes)
  {
    if (bytes != 0) {
      cudaMalloc(&ptr, bytes);
    }
  }

  device_buffer(const device_buffer&)            = delete;
  device_buffer& operator=(const device_buffer&) = delete;

  ~device_buffer()
  {
    if (ptr != nullptr) {
      cudaFree(ptr);
    }
  }

  template <typename T>
  T* as()
  {
    return static_cast<T*>(ptr);
  }
};

struct run_spec {
  unsigned first_cb;
  unsigned nof_cbs;
  unsigned rm_length;
  unsigned cw_offset;
};

std::vector<uint32_t> generate_gold_sequence_reference(uint32_t c_init, unsigned nof_bits)
{
  static constexpr unsigned nc = 1600;
  std::vector<uint8_t>      x1(nc + nof_bits + 31, 0);
  std::vector<uint8_t>      x2(nc + nof_bits + 31, 0);
  x1[0] = 1;
  for (unsigned bit = 0; bit != 31; ++bit) {
    x2[bit] = (c_init >> bit) & 1U;
  }
  for (unsigned n = 0; n != nc + nof_bits; ++n) {
    x1[n + 31] = x1[n + 3] ^ x1[n];
    x2[n + 31] = x2[n + 3] ^ x2[n + 2] ^ x2[n + 1] ^ x2[n];
  }

  std::vector<uint32_t> result((nof_bits + 31U) / 32U, 0);
  for (unsigned bit = 0; bit != nof_bits; ++bit) {
    result[bit / 32U] |= static_cast<uint32_t>(x1[nc + bit] ^ x2[nc + bit]) << (31U - (bit % 32U));
  }
  return result;
}

bool check_gold_sequence(uint32_t c_init, unsigned nof_bits)
{
  cudaStream_t stream = nullptr;
  if (!check_cuda(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking), "gold cudaStreamCreateWithFlags")) {
    return false;
  }
  scrambler_handle_t scrambler = nullptr;
  bool               ok        = check_status(scrambler_create(&scrambler), "gold scrambler_create") &&
            check_status(scrambler_configure_c_init(scrambler, c_init), "gold scrambler_configure_c_init") &&
            check_status(scrambler_generate_sequence(scrambler, static_cast<int>(nof_bits), stream),
                         "gold scrambler_generate_sequence") &&
            check_cuda(cudaStreamSynchronize(stream), "gold cudaStreamSynchronize");

  const auto            expected = generate_gold_sequence_reference(c_init, nof_bits);
  std::vector<uint32_t> actual(expected.size());
  if (ok) {
    ok = check_cuda(cudaMemcpy(actual.data(),
                               scrambler_get_sequence_ptr(scrambler),
                               actual.size() * sizeof(uint32_t),
                               cudaMemcpyDeviceToHost),
                    "gold copy sequence");
  }
  if (ok && actual != expected) {
    const auto mismatch = std::mismatch(actual.begin(), actual.end(), expected.begin());
    const auto index    = static_cast<size_t>(mismatch.first - actual.begin());
    std::fprintf(stderr,
                 "Gold sequence mismatch c_init=0x%08x word=%zu actual=0x%08x expected=0x%08x\n",
                 c_init,
                 index,
                 actual[index],
                 expected[index]);
    ok = false;
  }
  if (scrambler != nullptr) {
    scrambler_destroy(scrambler);
  }
  cudaStreamDestroy(stream);
  return ok;
}

bool configure_rate_dematcher(rate_matcher_handle_t handle, unsigned q_m, unsigned e, unsigned filler_bits)
{
  static constexpr int bg = 1;
  static constexpr int z  = 64;
  static constexpr int k  = 22 * z;

  nr_ldpc_config_t ldpc_cfg   = {};
  ldpc_cfg.base_graph         = bg;
  ldpc_cfg.lifting_size       = z;
  ldpc_cfg.lifting_set_index  = nr_ldpc_get_lifting_set_index(z);
  ldpc_cfg.num_info_bits      = k - static_cast<int>(filler_bits);
  ldpc_cfg.num_parity_bits    = 46 * z;
  ldpc_cfg.num_filler_bits    = static_cast<int>(filler_bits);
  ldpc_cfg.num_codeword_bits  = 68 * z;
  ldpc_cfg.puncture           = true;
  ldpc_cfg.max_parity_nodes   = 46;
  ldpc_cfg.redundancy_version = 0;

  nr_rate_match_config_t rm_cfg = {};
  rm_cfg.E                      = static_cast<int>(e);
  rm_cfg.Q_m                    = static_cast<int>(q_m);
  rm_cfg.rv                     = 0;
  rm_cfg.N_cb                   = ldpc_cfg.num_codeword_bits - 2 * z;
  rm_cfg.k0                     = rate_matcher_compute_k0(bg, z, rm_cfg.rv, rm_cfg.N_cb);
  rm_cfg.limited_buffer         = false;

  return check_status(rate_matcher_configure_rx(handle, &ldpc_cfg, &rm_cfg), "rate_matcher_configure_rx");
}

std::vector<run_spec> build_runs(unsigned e_short, unsigned e_long)
{
  std::vector<run_spec> runs;
  runs.reserve(3);

  unsigned cb_offset = 0;
  unsigned cw_offset = 0;
  auto     add_run   = [&](unsigned nof_cbs, unsigned e) {
    runs.push_back(run_spec{cb_offset, nof_cbs, e, cw_offset});
    cb_offset += nof_cbs;
    cw_offset += nof_cbs * e;
  };

  add_run(2, e_short);
  add_run(2, e_long);
  add_run(1, e_short);

  return runs;
}

bool run_case(unsigned q_m, unsigned scramble_offset)
{
  static constexpr unsigned z           = 64;
  static constexpr unsigned n_full      = 68 * z;
  static constexpr unsigned filler_bits = 96;

  unsigned e_short = ((880 + q_m - 1) / q_m) * q_m;
  unsigned e_long  = ((944 + q_m - 1) / q_m) * q_m;
  auto     runs    = build_runs(e_short, e_long);

  unsigned nof_cbs    = 0;
  unsigned total_llrs = 0;
  for (const run_spec& run : runs) {
    nof_cbs = std::max(nof_cbs, run.first_cb + run.nof_cbs);
    total_llrs += run.nof_cbs * run.rm_length;
  }

  std::vector<uint16_t> h_input(total_llrs);
  for (unsigned i = 0; i != total_llrs; ++i) {
    float value = (static_cast<int>((i * 17U + 11U) % 41U) - 20) / 8.0F;
    if (value == 0.0F) {
      value = 0.125F;
    }
    h_input[i] = float_to_half_bits(value);
  }

  cudaStream_t stream = nullptr;
  if (!check_cuda(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking), "cudaStreamCreateWithFlags")) {
    return false;
  }

  const size_t  input_bytes  = h_input.size() * sizeof(uint16_t);
  const size_t  output_bytes = static_cast<size_t>(nof_cbs) * n_full * sizeof(uint16_t);
  device_buffer d_scrambled(input_bytes);
  device_buffer d_reference_input(input_bytes);
  device_buffer d_reference_output(output_bytes);
  device_buffer d_candidate_output(output_bytes);

  bool ok = d_scrambled.ptr != nullptr && d_reference_input.ptr != nullptr && d_reference_output.ptr != nullptr &&
            d_candidate_output.ptr != nullptr;
  ok = ok && check_cuda(cudaMemcpyAsync(
                            d_scrambled.as<uint16_t>(), h_input.data(), input_bytes, cudaMemcpyHostToDevice, stream),
                        "copy scrambled input");
  ok = ok &&
       check_cuda(cudaMemcpyAsync(
                      d_reference_input.as<uint16_t>(), h_input.data(), input_bytes, cudaMemcpyHostToDevice, stream),
                  "copy reference input");
  ok = ok && check_cuda(cudaMemsetAsync(d_reference_output.as<uint16_t>(), 0x5a, output_bytes, stream),
                        "initialize reference output");
  ok = ok && check_cuda(cudaMemsetAsync(d_candidate_output.as<uint16_t>(), 0xa5, output_bytes, stream),
                        "initialize candidate output");
  if (!ok) {
    cudaStreamDestroy(stream);
    return false;
  }

  scrambler_handle_t    scrambler = nullptr;
  rate_matcher_handle_t matcher   = nullptr;
  ok                              = check_status(scrambler_create(&scrambler), "scrambler_create") &&
       check_status(rate_matcher_create(&matcher), "rate_matcher_create");
  if (!ok) {
    if (scrambler != nullptr) {
      scrambler_destroy(scrambler);
    }
    if (matcher != nullptr) {
      rate_matcher_destroy(matcher);
    }
    cudaStreamDestroy(stream);
    return false;
  }

  nr_scrambling_config_t scr_cfg = {};
  scr_cfg.n_RNTI                 = 0x4601;
  scr_cfg.n_ID                   = 17;
  scr_cfg.q                      = 0;
  scr_cfg.n_s                    = 3;

  ok = check_status(scrambler_configure(scrambler, &scr_cfg), "scrambler_configure") &&
       check_status(scrambler_set_offset(scrambler, static_cast<int>(scramble_offset)), "scrambler_set_offset") &&
       check_status(scrambler_generate_sequence(scrambler, static_cast<int>(total_llrs), stream),
                    "scrambler_generate_sequence");
  const unsigned int* d_sequence = ok ? scrambler_get_sequence_ptr(scrambler) : nullptr;
  ok                             = ok && (d_sequence != nullptr);

  ok = ok && check_status(scrambler_descramble_llr_half_inplace(
                              scrambler, d_reference_input.as<uint16_t>(), static_cast<int>(total_llrs), stream),
                          "scrambler_descramble_llr_half_inplace");

  for (const run_spec& run : runs) {
    ok = ok && configure_rate_dematcher(matcher, q_m, run.rm_length, filler_bits);
    ok = ok && check_status(rate_matcher_deinterleave_and_dematch_batch_half(
                                matcher,
                                d_reference_input.as<uint16_t>() + run.cw_offset,
                                d_reference_output.as<uint16_t>() + static_cast<size_t>(run.first_cb) * n_full,
                                static_cast<int>(q_m),
                                static_cast<int>(run.nof_cbs),
                                stream,
                                nullptr,
                                0),
                            "reference dematch");
    ok = ok && check_status(rate_matcher_deinterleave_and_dematch_batch_half(
                                matcher,
                                d_scrambled.as<uint16_t>() + run.cw_offset,
                                d_candidate_output.as<uint16_t>() + static_cast<size_t>(run.first_cb) * n_full,
                                static_cast<int>(q_m),
                                static_cast<int>(run.nof_cbs),
                                stream,
                                d_sequence,
                                static_cast<int>(run.cw_offset)),
                            "candidate dematch");
  }

  ok = ok && check_cuda(cudaStreamSynchronize(stream), "cudaStreamSynchronize");

  std::vector<uint16_t> h_reference(output_bytes / sizeof(uint16_t));
  std::vector<uint16_t> h_candidate(output_bytes / sizeof(uint16_t));
  ok =
      ok && check_cuda(
                cudaMemcpy(h_reference.data(), d_reference_output.as<uint16_t>(), output_bytes, cudaMemcpyDeviceToHost),
                "copy reference output");
  ok =
      ok && check_cuda(
                cudaMemcpy(h_candidate.data(), d_candidate_output.as<uint16_t>(), output_bytes, cudaMemcpyDeviceToHost),
                "copy candidate output");

  if (ok && h_reference != h_candidate) {
    auto   mismatch = std::mismatch(h_reference.begin(), h_reference.end(), h_candidate.begin());
    size_t index    = static_cast<size_t>(mismatch.first - h_reference.begin());
    std::fprintf(stderr,
                 "mismatch: q_m=%u scramble_offset=%u half_index=%zu reference=0x%04x candidate=0x%04x\n",
                 q_m,
                 scramble_offset,
                 index,
                 h_reference[index],
                 h_candidate[index]);
    ok = false;
  }

  rate_matcher_destroy(matcher);
  scrambler_destroy(scrambler);
  cudaStreamDestroy(stream);

  if (ok) {
    std::printf("PASS q_m=%u scramble_offset=%u total_llrs=%u cbs=%u\n", q_m, scramble_offset, total_llrs, nof_cbs);
  }
  return ok;
}

} // namespace

int main()
{
  bool ok = true;
  for (const uint32_t c_init : {0U, 1U, 17U, (0x4601U << 15U) + 17U, 0x7fffffffU}) {
    ok = check_gold_sequence(c_init, 4096U) && ok;
  }
  for (unsigned q_m : {1U, 2U, 4U, 6U, 8U}) {
    for (unsigned scramble_offset : {0U, 1U, 31U, 32U, 47U}) {
      ok = run_case(q_m, scramble_offset) && ok;
    }
  }

  if (!ok) {
    std::fprintf(stderr, "FAIL: fused per-run descramble dematch differs from full-buffer descramble dematch\n");
    return 1;
  }

  std::printf("PASS: fused per-run descramble dematch matches full-buffer descramble dematch\n");
  return 0;
}
