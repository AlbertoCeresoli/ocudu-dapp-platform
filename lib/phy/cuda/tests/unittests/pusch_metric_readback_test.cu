// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "../../../upper/channel_processors/pusch/pusch_metric_readback_completion.h"
#include "../../src/pusch_e2e_internal.cuh"
#include "pusch_e2e.h"
#include "pusch_equalized_metrics.h"
#include <vector>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <thread>

namespace {
void require(bool ok, const char* message)
{
  if (!ok)
    throw std::runtime_error(message);
}
void checked(cudaError_t status)
{
  require(status == cudaSuccess, cudaGetErrorString(status));
}

struct accumulators {
  float    noise;
  unsigned count;
  float    evm_error;
  unsigned evm_count;
};
static_assert(sizeof(accumulators) == 16);
struct pilot_metrics {
  float sinr, epre, rsrp, ta, cfo;
};
static_assert(sizeof(pilot_metrics) == 20);

void bind_host(pusch_e2e_ctx& ctx, accumulators* accum, pilot_metrics* pilot)
{
  ctx.h_sinr_eq_block          = accum;
  ctx.h_sinr_noise_var_sum     = &accum->noise;
  ctx.h_sinr_count             = &accum->count;
  ctx.h_evm_error_sum          = &accum->evm_error;
  ctx.h_evm_symbol_count       = &accum->evm_count;
  ctx.h_sinr_epre_result       = reinterpret_cast<float*>(pilot);
  ctx.config.enable_evm_metric = true;
}

void verify(pusch_e2e_ctx& ctx, accumulators expected, pilot_metrics pilot)
{
  const float mean  = expected.noise / static_cast<float>(expected.count);
  const bool  valid = expected.count && std::isfinite(expected.noise) && expected.noise > 0 && mean > 0;
  const float post  = pusch_e2e_post_eq_sinr_get_result(&ctx);
  if (valid) {
    const double reference = -10.0 * std::log10(static_cast<double>(mean));
    require(std::isfinite(post) && std::abs(post - reference) < 5.e-5, "post-EQ value");
  } else {
    require(post == -INFINITY, "post-EQ unavailable without fallback");
  }
  const float ch = pusch_e2e_channel_estimator_sinr_get_result(&ctx);
  require(ch == (std::isfinite(pilot.sinr) ? pilot.sinr : -INFINITY), "pilot provenance");
  const float legacy = pusch_e2e_sinr_get_result(&ctx);
  require(legacy == (valid ? post : ch), "legacy fallback compatibility");
  auto same = [](float a, float b) { return a == b || (std::isnan(a) && std::isnan(b)); };
  require(same(pusch_e2e_epre_get_result(&ctx), pilot.epre), "EPRE ordering");
  require(same(pusch_e2e_rsrp_get_result(&ctx), pilot.rsrp), "RSRP ordering");
  require(same(pusch_e2e_ta_get_result(&ctx), pilot.ta), "TA ordering");
  require(same(pusch_e2e_cfo_get_result(&ctx), pilot.cfo), "CFO ordering");
  const float evm = pusch_e2e_evm_get_result(&ctx);
  if (expected.evm_count && std::isfinite(expected.evm_error) && expected.evm_error >= 0) {
    require(std::isfinite(evm) && std::abs(evm - std::sqrt(expected.evm_error / expected.evm_count)) < 1.e-6f,
            "EVM ordering");
  } else {
    require(std::isnan(evm), "EVM unavailable");
  }
}

void cold_checks()
{
  pusch_e2e_ctx ctx{};
  require(pusch_e2e_post_eq_sinr_get_result(nullptr) == -INFINITY, "null post-EQ");
  require(pusch_e2e_channel_estimator_sinr_get_result(nullptr) == -INFINITY, "null pilot");
  require(pusch_e2e_post_eq_sinr_get_result(&ctx) == -INFINITY, "missing post-EQ storage");
  require(pusch_e2e_channel_estimator_sinr_get_result(&ctx) == -INFINITY, "missing pilot storage");
  accumulators  accum{1, 100, 1, 100};
  pilot_metrics pilot{4, -11, -13, 0.000001f, -81};
  bind_host(ctx, &accum, &pilot);
  verify(ctx, accum, pilot);
  const float invalid[] = {0, -1, NAN, INFINITY, -INFINITY, std::numeric_limits<float>::denorm_min()};
  for (float noise : invalid) {
    accum.noise = noise;
    verify(ctx, accum, pilot);
    pilot.sinr = NAN;
    verify(ctx, accum, pilot);
    pilot.sinr = 4;
  }
  accum = {1, 0, NAN, 0};
  verify(ctx, accum, pilot);
  ctx.config.enable_evm_metric = false;
  require(std::isnan(pusch_e2e_evm_get_result(&ctx)), "disabled EVM");
  ctx.h_sinr_count = nullptr;
  require(pusch_e2e_post_eq_sinr_get_result(&ctx) == -INFINITY, "missing count");
  std::puts("{\"kind\":\"cold\",\"status\":\"ACTUAL_READBACK_GETTERS_OK\"}");
}

__global__ void produce(accumulators* output, pilot_metrics* pilot_output, accumulators value, pilot_metrics pilot)
{
  if (threadIdx.x == 0) {
    *output       = value;
    *pilot_output = pilot;
  }
}

struct resources {
  pusch_e2e_ctx  ctx{};
  accumulators*  device{};
  pilot_metrics* device_pilot{};
  accumulators*  host{};
  pilot_metrics* host_pilot{};
  cudaStream_t   producer{}, readback{};
  cudaEvent_t    produced{}, completed{};
  ~resources()
  {
    if (producer && cudaStreamSynchronize(producer) != cudaSuccess)
      std::_Exit(5);
    if (readback && cudaStreamSynchronize(readback) != cudaSuccess)
      std::_Exit(5);
    if (completed)
      cudaEventDestroy(completed);
    if (produced)
      cudaEventDestroy(produced);
    if (host && !ctx.sinr_managed)
      cudaFreeHost(host);
    if (host_pilot && !ctx.sinr_managed)
      cudaFreeHost(host_pilot);
    if (device)
      cudaFree(device);
    if (device_pilot)
      cudaFree(device_pilot);
    if (readback)
      cudaStreamDestroy(readback);
    if (producer)
      cudaStreamDestroy(producer);
  }
  void create(bool managed)
  {
    ctx.sinr_managed = managed;
    int least{}, greatest{};
    checked(cudaDeviceGetStreamPriorityRange(&least, &greatest));
    checked(cudaStreamCreateWithPriority(&producer, cudaStreamNonBlocking, least));
    checked(cudaStreamCreateWithPriority(&readback, cudaStreamNonBlocking, least));
    checked(cudaEventCreateWithFlags(&produced, cudaEventDisableTiming));
    checked(cudaEventCreateWithFlags(&completed, cudaEventDisableTiming));
    if (managed) {
      checked(cudaMallocManaged(&device, sizeof(accumulators)));
      checked(cudaMallocManaged(&device_pilot, sizeof(pilot_metrics)));
      host       = device;
      host_pilot = device_pilot;
    } else {
      checked(cudaMalloc(&device, sizeof(accumulators)));
      checked(cudaMalloc(&device_pilot, sizeof(pilot_metrics)));
      checked(cudaMallocHost(&host, sizeof(accumulators)));
      checked(cudaMallocHost(&host_pilot, sizeof(pilot_metrics)));
    }
    bind_host(ctx, host, host_pilot);
    ctx.d_eq_noise_var_sum   = &device->noise;
    ctx.d_eq_noise_var_count = &device->count;
    ctx.d_evm_error_sum = &device->evm_error;
    ctx.d_evm_symbol_count = &device->evm_count;
    ctx.configured = true;
    ctx.d_sinr_epre_result   = reinterpret_cast<float*>(device_pilot);
  }
  void run(accumulators value, pilot_metrics pilot, bool side_stream)
  {
    produce<<<1, 1, 0, producer>>>(device, device_pilot, value, pilot);
    checked(cudaGetLastError());
    cudaStream_t stream = producer;
    if (side_stream) {
      checked(cudaEventRecord(produced, producer));
      checked(cudaStreamWaitEvent(readback, produced, 0));
      stream = readback;
    }
    require(pusch_e2e_sinr_async_launch_checked(&ctx, stream) == NR_LDPC_SUCCESS, "checked readback submission");
    checked(cudaGetLastError());
    checked(cudaEventRecord(completed, stream));
    // Model the decoder's producer-stream join, then execute the actual
    // host completion helper. Side-stream readback is joined by that helper.
    if (!side_stream)
      checked(cudaEventSynchronize(completed));
    require(ocudu::pusch_metric_readback_completed(true, true, side_stream, completed, stream), "host completion gate");
    verify(ctx, value, pilot);
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
  }
};

unsigned execute_external(bool managed)
{
  resources owner;
  owner.create(managed);
  constexpr unsigned capacity = 14U * 275U * 12U * 4U;
  struct scratch {
    float2* symbols{};
    float* noise{};
    ~scratch() { if (symbols) cudaFree(symbols); if (noise) cudaFree(noise); }
  } device;
  checked(cudaMalloc(&device.symbols, capacity * sizeof(float2)));
  checked(cudaMalloc(&device.noise, capacity * sizeof(float)));
  std::vector<float2> symbols(capacity);
  std::vector<float> noise(capacity, 0.01F);
  noise[0] = 1.0e6F; noise[1] = NAN; noise[2] = 0;
  checked(cudaMemcpy(device.noise, noise.data(), capacity * sizeof(float), cudaMemcpyHostToDevice));
  unsigned cases = 0;
  for (unsigned bits : {2U, 4U, 6U, 8U}) {
    for (unsigned i = 0; i < capacity; ++i) {
      const float value = float(int(i % 33) - 16) * 0.1F;
      symbols[i] = {value, value + 0.01F};
    }
    symbols[3] = {NAN, NAN};
    checked(cudaMemcpy(device.symbols, symbols.data(), capacity * sizeof(float2), cudaMemcpyHostToDevice));
    for (unsigned count : {0U, 1U, 7U, 612U, 7344U, capacity}) {
      for (unsigned optional = 0; optional != 4; ++optional) {
        const bool has_noise = optional & 1U, has_symbols = optional & 2U;
        double sum_noise = 0, sum_error = 0;
        unsigned nn = 0, ne = 0;
        for (unsigned i = 0; i < count; ++i) {
          if (has_noise && ocudu::detail::pusch_metric_noise_valid(noise[i])) { sum_noise += noise[i]; ++nn; }
          if (has_symbols && std::isfinite(symbols[i].x) && std::isfinite(symbols[i].y)) {
            sum_error += ocudu::detail::pusch_qam_error_power(symbols[i].x, symbols[i].y, bits); ++ne;
          }
        }
        produce<<<1, 1, 0, owner.producer>>>(owner.device, owner.device_pilot, {999, 1, 999, 1}, {4, -11, -13, 0, 81});
        require(pusch_e2e_measure_external_eq(&owner.ctx, has_symbols ? device.symbols : nullptr,
                    has_noise ? device.noise : nullptr, count, bits, owner.producer) == NR_LDPC_SUCCESS,
                "external GPU reduction");
        checked(cudaEventRecord(owner.produced, owner.producer));
        checked(cudaStreamWaitEvent(owner.readback, owner.produced, 0));
        require(pusch_e2e_sinr_async_launch_checked(&owner.ctx, owner.readback) == NR_LDPC_SUCCESS, "external readback");
        checked(cudaEventRecord(owner.completed, owner.readback));
        require(ocudu::pusch_metric_readback_completed(true, true, true, owner.completed, owner.readback), "external fence");
        require(owner.host->count == nn && owner.host->evm_count == ne, "optional/invalid/erasure counts");
        require(std::abs(owner.host->noise - sum_noise) <= 1e-4 * std::max(1.0, sum_noise), "noise reduction parity");
        require(std::abs(owner.host->evm_error - sum_error) <= 1e-4 * std::max(1.0, sum_error), "QAM EVM parity");
        if (!nn) require(!std::isfinite(pusch_e2e_post_eq_sinr_get_result(&owner.ctx)), "no stale SINR");
        if (!ne) require(std::isnan(pusch_e2e_evm_get_result(&owner.ctx)), "no stale EVM");
        ++cases;
      }
    }
  }
  require(pusch_e2e_measure_external_eq(&owner.ctx, device.symbols, device.noise, capacity + 1, 2,
                                      owner.producer) == NR_LDPC_ERROR_INVALID_CONFIG, "extent bound");
  require(pusch_e2e_measure_external_eq(&owner.ctx, device.symbols, device.noise, 1, 3,
                                      owner.producer) == NR_LDPC_ERROR_INVALID_CONFIG, "unsupported QAM");
  std::printf("{\"kind\":\"external_eq\",\"managed\":%s,\"cases\":%u,\"status\":\"GPU_REDUCTION_PARITY_OK\"}\n",
              managed ? "true" : "false", cases);
  return cases;
}

unsigned execute(bool managed)
{
  resources owner;
  owner.create(managed);
  unsigned cases = 0;
  for (unsigned prb = 1; prb <= 51; ++prb) {
    for (unsigned qm : {2U, 4U, 6U, 8U}) {
      const unsigned      count = prb * 144;
      const float         mean  = 0.002f * qm + 0.0001f * prb;
      const accumulators  value{mean * count, count, 0.04f * count, count};
      const pilot_metrics pilot{float(qm + prb), -float(prb), -float(prb + qm), prb * 1.e-7f, float(qm * 11)};
      for (bool side : {false, true}) {
        owner.run(value, pilot, side);
        ++cases;
      }
    }
  }
  for (float invalid : {0.f, -1.f, NAN, INFINITY, -INFINITY, std::numeric_limits<float>::denorm_min()}) {
    for (bool side : {false, true}) {
      owner.run({invalid, 100, NAN, 0}, {NAN, NAN, NAN, NAN, NAN}, side);
      ++cases;
      owner.run({1, 100, 1, 100}, {4, -11, -13, 0, 81}, side);
      ++cases;
    }
  }
  std::printf("{\"kind\":\"gpu\",\"managed\":%s,\"cases\":%u,\"status\":\"ACTUAL_ASYNC_READBACK_OK\"}\n",
              managed ? "true" : "false",
              cases);
  std::fflush(stdout);
  return cases;
}
} // namespace

int main(int argc, char** argv)
{
  try {
    if (argc == 2 && std::strcmp(argv[1], "--external-only") == 0) {
      cold_checks();
      execute_external(false);
      execute_external(true);
      return 0;
    }
    require(argc == 1 || ((argc == 2 || argc == 4) && std::strcmp(argv[1], "--execute") == 0 &&
                          (argc == 2 || std::strcmp(argv[2], "--output") == 0)),
            "usage: [--execute [--output new-report.json]]");
    FILE* report = nullptr;
    if (argc == 4) {
      report = std::fopen(argv[3], "wx");
      require(report != nullptr, "report must be a new writable path");
    }
    cold_checks();
    std::fflush(stdout);
    if (argc >= 2) {
      const unsigned pinned  = execute(false);
      const unsigned managed = execute(true);
      execute_external(false);
      execute_external(true);
      if (report) {
        require(std::fprintf(report,
                             "{\"status\":\"ACTUAL_ASYNC_READBACK_OK\",\"pinned_cases\":%u,\"managed_cases\":%u,"
                             "\"same_and_side_stream\":true,\"live_activation\":false,\"scope\":\"Synthetic device "
                             "accumulators through actual readback, not channel estimation or full PUSCH\"}\n",
                             pinned,
                             managed) > 0,
                "write report");
        require(std::fclose(report) == 0, "close report");
      }
    }
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "FAIL: %s\n", e.what());
    return 1;
  }
}
