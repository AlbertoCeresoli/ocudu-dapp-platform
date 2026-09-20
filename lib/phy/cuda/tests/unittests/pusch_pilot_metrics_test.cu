// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "pusch_e2e.h"
#include "dmrs_utils.h"
#include "../../src/pusch_e2e_internal.cuh"
#include <cuda.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <vector>
#include <unistd.h>

namespace {
void require(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
[[maybe_unused]] void checked(cudaError_t status) { require(status == cudaSuccess, cudaGetErrorString(status)); }
[[maybe_unused]] void success(nr_ldpc_status_t status) { require(status == NR_LDPC_SUCCESS, "PUSCH API failed"); }
[[maybe_unused]] void pause() { std::this_thread::sleep_for(std::chrono::milliseconds{10}); }

uint16_t bf16(float value)
{
    uint32_t bits; std::memcpy(&bits, &value, sizeof(bits));
    return static_cast<uint16_t>((bits + 0x7fffU + ((bits >> 16) & 1U)) >> 16);
}
float from_bf16(uint16_t value)
{
    uint32_t bits = uint32_t(value) << 16; float result;
    std::memcpy(&result, &bits, sizeof(result)); return result;
}
uint32_t pack(float re, float im) { return uint32_t(bf16(re)) | (uint32_t(bf16(im)) << 16); }

// Independent CPU arithmetic from actual received/known transmitted pilots.
// It never infers noise samples from unallocated REs or imputes an erased pilot.
std::array<float, 5> pilot_reference(const pusch_e2e_config_t& c,
    const std::vector<uint32_t>& grid, const std::vector<float2>& tx, int dc)
{
    using complex = std::complex<float>;
    constexpr std::array<float, 15> taps{-.0391585909F, -.0287990728F, 0.F, .0445245383F, .0970724536F,
        .1467045930F, .1821531663F, .1950058248F, .1821531663F, .1467045930F, .0970724536F,
        .0445245383F, 0.F, -.0287990728F, -.0391585909F};
    const int n = 6 * c.nof_prb;
    std::array<int, 2> rows{}; unsigned nr = 0;
    for (int row = 0; row < 14; ++row) if (c.dmrs_symbol_mask & (1 << row)) {
        require(nr < 2, "reference needs two pilot symbols"); rows[nr++] = row;
    }
    require(nr == 2 && c.scs_khz == 30 && c.dmrs_type == DMRS_TYPE_1, "reference profile");
    const auto present = [&](int k) { return c.start_prb * 12 + 2 * k != dc; };
    const auto half_complex = [](complex value) {
        return complex(__half2float(__float2half_rn(value.real())), __half2float(__float2half_rn(value.imag())));
    };
    std::array<std::vector<complex>, 2> h, received, pilot;
    float epre = 0.F; int count = 0;
    for (int k = 0; k < n; ++k) count += present(k);
    require(count > 0, "reference needs observed pilots");
    for (int d = 0; d < 2; ++d) {
        h[d].resize(n); received[d].resize(n); pilot[d].resize(n);
        for (int k = 0; k < n; ++k) if (present(k)) {
            const int at = rows[d] * c.grid_nof_subcarriers + c.start_prb * 12 + 2 * k;
            const uint32_t value = grid.at(at);
            const complex y(from_bf16(value), from_bf16(value >> 16));
            const complex p(tx.at(at).x, tx.at(at).y);
            require(std::isfinite(std::norm(y)) && std::norm(p) > 0, "invalid observed reference input");
            received[d][k] = y; pilot[d][k] = p;
            h[d][k] = half_complex(y * std::conj(p) / std::norm(p));
            epre += std::norm(y);
        }
        if (c.nof_prb > 1) {
            const auto raw = h[d];
            for (int k = 0; k < n; ++k) if (present(k)) {
                complex sum{}; float weight = 0.F;
                for (int t = 0; t < 15; ++t) {
                    const int at = k + t - 7;
                    if (at < 0 || at >= n || !present(at)) continue;
                    sum += taps[t] * raw[at]; weight += taps[t];
                }
                require(weight > 1e-6F, "insufficient FIR observations");
                h[d][k] = half_complex(sum / weight);
            }
        }
    }
    complex ta{}, cfo{};
    for (int k = 0; k < n; ++k) if (present(k)) {
        cfo += h[1][k] * std::conj(h[0][k]);
        if (k && present(k - 1))
            ta += .25F * (h[0][k] + h[1][k]) * std::conj(h[0][k - 1] + h[1][k - 1]);
    }
    constexpr float pi = 3.14159265358979F;
    const float dt = (rows[1] - rows[0]) * (72.F / 30720000.F + 1.F / 30000.F);
    const float frequency = std::arg(cfo) / (2 * pi * dt);
    const complex rotation = c.compensate_cfo ? std::polar(1.F, -2 * pi * frequency * dt) : complex(1, 0);
    float signal = 0.F, residual = 0.F;
    for (int k = 0; k < n; ++k) if (present(k)) {
        const auto average = .5F * (h[0][k] + h[1][k] * rotation);
        signal += std::norm(average);
        residual += std::norm(received[0][k] - average * pilot[0][k]);
        residual += std::norm(received[1][k] - average * std::conj(rotation) * pilot[1][k]);
    }
    signal /= count;
    const float noise = std::max(residual / (2 * count - 1), signal * 1e-10F);
    return {noise > 1e-10F ? 10.F * std::log10(signal / noise) : 60.F,
        10.F * std::log10(epre / (2 * count)), 10.F * std::log10(signal * c.dmrs_scaling * c.dmrs_scaling),
        -std::arg(ta) / (2 * pi * 60000.F), frequency};
}

#ifndef PILOT_METRICS_REFERENCE_ONLY
struct owner {
    pusch_e2e_handle_t handle{};
    cudaStream_t stream{};
    cudaEvent_t start{}, end{};
    uint32_t* grid{};
    int* indices{};
    __half* llrs{};
    float2* symbols{};
    float* noise{};
    std::vector<uint32_t> host_grid = std::vector<uint32_t>(14 * 612);
    std::vector<float2> transmitted_pilots = std::vector<float2>(14 * 612);
    int data_re_index = 0;
    ~owner()
    {
        // Never release an in-flight borrow on an error path.
        if (stream && cudaStreamSynchronize(stream) != cudaSuccess) std::_Exit(5);
        if (handle) pusch_e2e_destroy(handle);
        if (grid) cudaFree(grid);
        if (indices) cudaFree(indices);
        if (llrs) cudaFree(llrs);
        if (symbols) cudaFree(symbols);
        if (noise) cudaFree(noise);
        if (start) cudaEventDestroy(start);
        if (end) cudaEventDestroy(end);
        if (stream) cudaStreamDestroy(stream);
    }
    void create()
    {
        int least = 0, greatest = 0;
        checked(cudaDeviceGetStreamPriorityRange(&least, &greatest));
        checked(cudaStreamCreateWithPriority(&stream, cudaStreamNonBlocking, least));
        checked(cudaEventCreate(&start)); checked(cudaEventCreate(&end));
        success(pusch_e2e_create(&handle));
        pause();
        pusch_e2e_config_t c{};
        c.nof_prb = 51; c.nof_symbols = 14; c.nof_rx_ports = c.nof_tx_layers = 1;
        c.grid_nof_subcarriers = 612; c.grid_nof_symbols = 14;
        c.dmrs_type = DMRS_TYPE_1; c.dmrs_symbol_mask = (1 << 2) | (1 << 11);
        c.nof_cdm_groups_without_data = 2; c.scrambling_id = 2;
        c.dmrs_scaling = 1; c.tx_scaling = 1; c.mod_order = 8;
        c.rnti = 0x4607; c.n_id = 2; c.scs_khz = 30;
        c.compensate_cfo = 1; c.noise_mode = 1; c.equalizer_algorithm = EQUALIZER_MMSE;
        c.enable_evm_metric = 1;
        success(pusch_e2e_configure(handle, &c));
        checked(cudaMalloc(&grid, host_grid.size() * sizeof(uint32_t)));
        checked(cudaMalloc(&indices, sizeof(int))); checked(cudaMalloc(&llrs, 8 * sizeof(__half)));
        checked(cudaMalloc(&symbols, sizeof(float2))); checked(cudaMalloc(&noise, sizeof(float)));
        checked(cudaMemcpyAsync(indices, &data_re_index, sizeof(int), cudaMemcpyHostToDevice, stream));
        checked(cudaMemsetAsync(llrs, 0, 8 * sizeof(__half), stream));
        checked(cudaStreamSynchronize(stream));
        handle->d_eq_symbols_out = symbols; handle->d_eq_noise_var_out = noise;
        pause();
    }

    double prepare(unsigned index, int prb, int qm)
    {
        auto& c = handle->config;
        c.nof_prb = prb; c.start_prb = (index * 7) % (52 - prb);
        c.mod_order = qm; c.slot_idx = index % 20;
        c.dmrs_symbol_mask = index % 2 ? (1 << 3) | (1 << 10) : (1 << 2) | (1 << 11);
        c.dmrs_scaling = index % 3 ? 1.41421356237F : 1.0F;
        c.compensate_cfo = index % 2; c.noise_mode = (index / 2) % 2;
        // The conventional probe consumes an absolute grid index. Move it with
        // the allocation; leaving it at zero would index estimates before the
        // grant when start_prb changes. Row zero is data for both pilot masks.
        data_re_index = c.start_prb * 12 + 1;
        checked(cudaMemcpyAsync(indices, &data_re_index, sizeof(int), cudaMemcpyHostToDevice, stream));
        // Nonpilot REs are deliberately nonempty. No idle-RE noise assumption.
        std::fill(host_grid.begin(), host_grid.end(), pack(0.37F, -0.19F));
        double epre = 0;
        for (int symbol = 0; symbol < 14; ++symbol) {
            if (!(c.dmrs_symbol_mask & (1 << symbol))) continue;
            uint32_t x1 = 1, x2 = dmrs_compute_c_init(c.slot_idx, symbol, c.scrambling_id, c.n_scid);
            auto advance = [&]() {
                unsigned bit = (x1 ^ x2) & 1U;
                x1 = (x1 >> 1) | (((x1 ^ (x1 >> 3)) & 1U) << 30);
                x2 = (x2 >> 1) | (((x2 ^ (x2 >> 1) ^ (x2 >> 2) ^ (x2 >> 3)) & 1U) << 30);
                return bit;
            };
            for (int i = 0; i < 1600; ++i) advance();
            for (int pilot = 0; pilot < 51 * 6; ++pilot) {
                float re = (1 - 2 * int(advance())) * c.dmrs_scaling * 0.70710678118F;
                float im = (1 - 2 * int(advance())) * c.dmrs_scaling * 0.70710678118F;
                transmitted_pilots[symbol * 612 + pilot * 2] = make_float2(re, im);
                float phase = 0.014F * pilot + 0.018F * symbol;
                float gain = 0.55F + 0.07F * std::cos(0.071F * pilot);
                float hr = gain * std::cos(phase), hi = gain * std::sin(phase);
                // Deterministic colored perturbation: arithmetic/parity fixture,
                // not a claim of an independent realistic noise calibration panel.
                float nr = 0.025F * std::sin(1.79F * pilot + 1.31F * symbol);
                float ni = 0.025F * std::cos(1.53F * pilot - 1.19F * symbol);
                const uint32_t value = pack(re * hr - im * hi + nr, re * hi + im * hr + ni);
                host_grid[symbol * 612 + pilot * 2] = value;
                if (pilot >= c.start_prb * 6 && pilot < (c.start_prb + prb) * 6) {
                    double r = from_bf16(value), i = from_bf16(value >> 16);
                    epre += r * r + i * i;
                }
            }
        }
        checked(cudaMemcpyAsync(grid, host_grid.data(), host_grid.size() * sizeof(uint32_t),
                                cudaMemcpyHostToDevice, stream));
        checked(cudaStreamSynchronize(stream));
        return 10 * std::log10(epre / (prb * 12));
    }

    std::array<float, 5> run(bool pilot_only, float& elapsed, int dc = -2, bool preserve = false)
    {
        checked(cudaEventRecord(start, stream));
        if (pilot_only && preserve) success(pusch_e2e_refresh_pilot_metrics_with_dc(handle, grid, dc, stream));
        else if (pilot_only && dc >= -1) success(pusch_e2e_process_pilot_metrics_with_dc(handle, grid, dc, stream));
        else if (pilot_only) success(pusch_e2e_process_pilot_metrics(handle, grid, stream));
        else success(pusch_e2e_process_full_gpu_optimized(handle, grid, llrs, indices, 1, stream));
        checked(cudaEventRecord(end, stream));
        success(pusch_e2e_sinr_async_launch_checked(handle, stream));
        checked(cudaStreamSynchronize(stream));
        checked(cudaEventElapsedTime(&elapsed, start, end)); elapsed *= 1000;
        if (pilot_only && !preserve) {
            require(pusch_e2e_post_eq_sinr_get_result(handle) == -INFINITY, "stale post-EQ statistic");
            require(std::isnan(pusch_e2e_evm_get_result(handle)), "stale EVM statistic");
        }
        auto result = std::array<float, 5>{pusch_e2e_channel_estimator_sinr_get_result(handle),
            pusch_e2e_epre_get_result(handle), pusch_e2e_rsrp_get_result(handle),
            pusch_e2e_ta_get_result(handle), pusch_e2e_cfo_get_result(handle)};
        for (float value : result) require(std::isfinite(value), "missing pilot measurement");
        pause(); return result;
    }
    std::array<unsigned char, 28> data_outputs()
    {
        std::array<unsigned char, 28> output{};
        checked(cudaMemcpyAsync(output.data(), llrs, 16, cudaMemcpyDeviceToHost, stream));
        checked(cudaMemcpyAsync(output.data() + 16, symbols, 8, cudaMemcpyDeviceToHost, stream));
        checked(cudaMemcpyAsync(output.data() + 24, noise, 4, cudaMemcpyDeviceToHost, stream));
        checked(cudaStreamSynchronize(stream)); return output;
    }
    std::array<unsigned char, 16> data_metrics()
    {
        std::array<unsigned char, 16> output{};
        checked(cudaMemcpyAsync(output.data(), handle->d_eq_noise_var_sum, output.size(), cudaMemcpyDeviceToHost, stream));
        checked(cudaStreamSynchronize(stream)); return output;
    }
};

#endif

void check_dc_reference()
{
    unsigned cases = 0;
    for (int prb : {1, 4, 12, 51}) for (float beta : {1.F, 2.F}) {
        pusch_e2e_config_t c{};
        c.nof_prb = prb; c.start_prb = 51 - prb; c.grid_nof_subcarriers = 612;
        c.dmrs_symbol_mask = (1 << 2) | (1 << 11); c.dmrs_type = DMRS_TYPE_1;
        c.scs_khz = 30; c.dmrs_scaling = beta; c.compensate_cfo = 1;
        std::vector<uint32_t> grid(14 * 612, pack(NAN, NAN));
        std::vector<float2> pilots(grid.size());
        const int first = c.start_prb * 12;
        for (int row : {2, 11}) for (int k = 0; k < prb * 6; ++k) {
            const int at = row * 612 + first + 2 * k;
            grid[at] = pack(.5F * beta, .25F * beta); pilots[at] = make_float2(beta, 0);
        }
        for (int dc : {-1, first, first + 6 * prb, first + 12 * prb - 1}) {
            auto punctured = grid;
            if (dc >= 0) for (int row : {2, 11}) punctured[row * 612 + dc] = pack(NAN, NAN);
            const auto result = pilot_reference(c, punctured, pilots, dc);
            require(std::abs(result[0] - 60.F) < 1e-4F, "CPU clean-signal SINR control");
            const float expected = 10.F * std::log10(.3125F * beta * beta);
            require(std::abs(result[1] - expected) < 1e-4F && std::abs(result[2] - expected) < 1e-4F,
                    "CPU observed-power/DMRS-boost control");
            require(result[3] == 0 && result[4] == 0, "CPU zero timing/frequency control");
            ++cases;
        }
    }
    std::printf("{\"status\":\"DC_PILOT_CPU_REFERENCE_OK\",\"cases\":%u}\n", cases);
}

#ifndef PILOT_METRICS_REFERENCE_ONLY
void phase(const char* name)
{
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    std::printf("{\"kind\":\"phase\",\"phase\":\"%s\",\"monotonic_ns\":%lld}\n", name, static_cast<long long>(ns));
    std::fflush(stdout);
}

void compare_metric(float actual, float expected, unsigned metric, unsigned index, const char* stage)
{
    const float tolerance = metric < 3 ? .005F : metric == 3 ? 1e-9F : .05F;
    if (!std::isfinite(actual) || !std::isfinite(expected) || std::abs(actual - expected) > tolerance) {
        std::fprintf(stderr, "pilot comparison failed: stage=%s case=%u metric=%u actual=%.9g expected=%.9g tolerance=%.9g\n",
                     stage, index, metric, actual, expected, tolerance);
        throw std::runtime_error("pilot measurement mismatch");
    }
}

void run_dc_panel()
{
    const char* guard_pid = std::getenv("OCUDU_SHARED_GPU_GUARD_PID");
    require(guard_pid && std::to_string(getppid()) == guard_pid && std::getenv("OCUDU_SHARED_GPU_GUARD_START") &&
            std::getenv("OCUDU_SHARED_GPU_GUARD_OUTPUT"), "anchored direct guard child required");
    check_dc_reference();
    phase("context_begin");
    CUdevice device{}; CUcontext context{}, previous{};
    require(cuInit(0) == CUDA_SUCCESS && cuCtxGetCurrent(&previous) == CUDA_SUCCESS &&
            cuDeviceGet(&device, 0) == CUDA_SUCCESS && cuDevicePrimaryCtxRetain(&context, device) == CUDA_SUCCESS &&
            cuCtxPushCurrent(context) == CUDA_SUCCESS, "owned primary context setup failed");
    phase("context_ready");
    unsigned cases = 0;
    {
        owner resources; resources.create(); phase("owner_ready");
        unsigned index = 0;
        for (int prb : {1, 4, 12, 51}) for (int qm : {2, 4, 6, 8}) for (int position = 0; position < 4; ++position) {
            resources.prepare(index, prb, qm);
            const auto& c = resources.handle->config;
            const int first = c.start_prb * 12;
            const int dc = position == 0 ? -1 : position == 1 ? first : position == 2 ? first + 6 * prb : first + 12 * prb - 1;
            float baseline_us = 0;
            const auto baseline = resources.run(false, baseline_us);
            const auto clean_reference = pilot_reference(c, resources.host_grid, resources.transmitted_pilots, -1);
            for (unsigned k = 0; k < 5; ++k) {
                compare_metric(baseline[k], clean_reference[k], k, index, "conventional/CPU");
            }
            const auto saved = resources.data_outputs();
            const auto saved_data_metrics = resources.data_metrics();
            const float saved_post_eq = pusch_e2e_post_eq_sinr_get_result(resources.handle);
            const float saved_evm = pusch_e2e_evm_get_result(resources.handle);
            require(std::isfinite(saved_post_eq) && std::isfinite(saved_evm), "conventional data metric control");
            const auto expected = pilot_reference(c, resources.host_grid, resources.transmitted_pilots, dc);
            // Common feedback refresh must preserve the SAME already-produced
            // data statistics and outputs, even when erased pilots contain NaNs.
            std::array<float, 5> first_refresh{};
            for (unsigned pattern = 0; pattern < 3; ++pattern) {
                const uint32_t fill = pattern == 0 ? 0U : pattern == 1 ? pack(123.F, -81.F) : pack(NAN, NAN);
                if (dc >= 0) for (int row = 0; row < 14; ++row) resources.host_grid[row * 612 + dc] = fill;
                checked(cudaMemcpyAsync(resources.grid, resources.host_grid.data(), resources.host_grid.size() * sizeof(uint32_t),
                                        cudaMemcpyHostToDevice, resources.stream));
                checked(cudaStreamSynchronize(resources.stream));
                float elapsed = 0;
                const auto result = resources.run(true, elapsed, dc, true);
                require(resources.data_outputs() == saved && resources.data_metrics() == saved_data_metrics,
                        "pilot refresh modified data outputs or accumulators");
                require(pusch_e2e_post_eq_sinr_get_result(resources.handle) == saved_post_eq &&
                        pusch_e2e_evm_get_result(resources.handle) == saved_evm, "pilot refresh changed data readback");
                for (unsigned k = 0; k < 5; ++k) {
                    compare_metric(result[k], expected[k], k, index, "preserving CUDA/CPU");
                    if (pattern) compare_metric(result[k], first_refresh[k], k, index, "preserving DC poison invariance");
                }
                if (!pattern) first_refresh = result;
                std::printf("{\"kind\":\"refresh_case\",\"index\":%u,\"prb\":%d,\"qm\":%d,\"dc\":%d,\"pattern\":%u,"
                    "\"metrics\":[%.9g,%.9g,%.9g,%.9g,%.9g],\"reference\":[%.9g,%.9g,%.9g,%.9g,%.9g],"
                    "\"data_metrics_preserved\":true,\"data_outputs_preserved\":true,\"post_eq_sinr\":%.9g,\"evm\":%.9g,"
                    "\"baseline_us\":%.9g,\"pilot_us\":%.9g}\n", index, prb, qm, dc, pattern,
                    result[0], result[1], result[2], result[3], result[4], expected[0], expected[1], expected[2], expected[3], expected[4],
                    saved_post_eq, saved_evm, baseline_us, elapsed);
                std::fflush(stdout);
            }
            std::array<float, 5> first_result{};
            for (unsigned pattern = 0; pattern < 3; ++pattern) {
                const uint32_t fill = pattern == 0 ? 0U : pattern == 1 ? pack(123.F, -81.F) : pack(NAN, NAN);
                if (dc >= 0) for (int row = 0; row < 14; ++row) resources.host_grid[row * 612 + dc] = fill;
                checked(cudaMemcpyAsync(resources.grid, resources.host_grid.data(), resources.host_grid.size() * sizeof(uint32_t),
                                        cudaMemcpyHostToDevice, resources.stream));
                checked(cudaStreamSynchronize(resources.stream));
                float elapsed = 0;
                const auto result = resources.run(true, elapsed, dc);
                require(resources.data_outputs() == saved, "DC pilot measurements modified data outputs");
                double error = 0;
                for (unsigned k = 0; k < 5; ++k) {
                    compare_metric(result[k], expected[k], k, index, "masked CUDA/CPU");
                    if (pattern) compare_metric(result[k], first_result[k], k, index, "DC poison invariance");
                    error = std::max(error, double(std::abs(result[k] - expected[k])));
                }
                if (!pattern) first_result = result;
                std::printf("{\"kind\":\"dc_case\",\"index\":%u,\"prb\":%d,\"qm\":%d,\"dc\":%d,\"pattern\":%u,"
                    "\"metrics\":[%.9g,%.9g,%.9g,%.9g,%.9g],\"reference\":[%.9g,%.9g,%.9g,%.9g,%.9g],"
                    "\"max_delta\":%.9g,\"baseline_us\":%.9g,\"pilot_us\":%.9g}\n", index, prb, qm, dc, pattern,
                    result[0], result[1], result[2], result[3], result[4], expected[0], expected[1], expected[2], expected[3], expected[4],
                    error, baseline_us, elapsed);
                std::fflush(stdout); ++cases;
            }
            ++index;
        }
        phase("owner_retire_begin");
    }
    phase("owner_retired");
    CUcontext popped{}, restored{};
    require(cuCtxPopCurrent(&popped) == CUDA_SUCCESS && popped == context && cuCtxGetCurrent(&restored) == CUDA_SUCCESS &&
            restored == previous && cuDevicePrimaryCtxRelease(device) == CUDA_SUCCESS, "owned context release failed");
    phase("context_released");
    require(cases == 192, "incomplete DC panel");
    std::printf("{\"kind\":\"complete\",\"status\":\"DC_PILOT_METRIC_PARITY_OK\",\"cases\":%u}\n", cases);
}
#endif
} // namespace

int main(int argc, char** argv)
{
#ifdef PILOT_METRICS_REFERENCE_ONLY
    if (argc != 2 || std::strcmp(argv[1], "--check-dc-reference") != 0) return 2;
    try { check_dc_reference(); return 0; }
    catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
#else
    if (argc == 2 && (std::strcmp(argv[1], "--execute-dc") == 0 || std::strcmp(argv[1], "--check-dc-reference") == 0)) {
        try {
            if (std::strcmp(argv[1], "--execute-dc") == 0) run_dc_panel();
            else check_dc_reference();
            return 0;
        } catch (const std::exception& error) {
            std::fprintf(stderr, "DC pilot test failed: %s\n", error.what()); return 1;
        }
    }
    if (argc != 2 || std::strcmp(argv[1], "--execute") != 0) {
        std::puts("GPU execution requires explicit --execute and an external live-cell guard.");
        return argc == 1 ? 0 : 2;
    }
    try {
        {
            owner resources; resources.create();
            unsigned index = 0;
            for (int step = 0; step < 51; ++step) {
                const int prb = step % 2 ? 51 - step / 2 : 1 + step / 2;
                for (int qm : {2, 8, 4, 6}) {
                    const double expected_epre = resources.prepare(index, prb, qm);
                    float baseline_us = 0, pilot_us = 0, repeat_us = 0;
                    const auto baseline = resources.run(false, baseline_us);
                    const auto saved = resources.data_outputs();
                    const auto pilot = resources.run(true, pilot_us);
                    require(saved == resources.data_outputs(), "pilot pass modified decoder/constellation outputs");
                    const auto repeat = resources.run(false, repeat_us);
                    double maximum_delta = 0;
                    for (size_t k = 0; k < baseline.size(); ++k) {
                        // Absolute tolerances fixed before execution: dB, seconds, Hz.
                        const float tolerance = k < 3 ? 0.005F : k == 3 ? 1.e-9F : 0.05F;
                        require(std::abs(pilot[k] - baseline[k]) <= tolerance, "pilot/baseline mismatch");
                        require(std::abs(repeat[k] - baseline[k]) <= tolerance, "post-pilot baseline mismatch");
                        maximum_delta = std::max(maximum_delta, double(std::abs(pilot[k] - baseline[k])));
                    }
                    require(std::abs(pilot[1] - expected_epre) <= 0.0001, "observed DMRS EPRE mismatch");
                    std::printf("{\"kind\":\"case\",\"index\":%u,\"prb\":%d,\"qm\":%d,"
                        "\"pilot\":[%.9g,%.9g,%.9g,%.9g,%.9g],\"expected_epre\":%.9g,"
                        "\"max_delta\":%.9g,\"baseline_us\":%.9g,\"pilot_us\":%.9g,\"repeat_us\":%.9g}\n",
                        index++, prb, qm, pilot[0], pilot[1], pilot[2], pilot[3], pilot[4], expected_epre,
                        maximum_delta, baseline_us, pilot_us, repeat_us);
                    std::fflush(stdout);
                }
            }
            std::puts("{\"kind\":\"complete\",\"status\":\"PILOT_METRIC_PARITY_OK\",\"cases\":204}");
        }
        // Include context teardown pressure in the external guard's owned window.
        std::this_thread::sleep_for(std::chrono::seconds{10});
        return 0;
    } catch (const std::exception& e) { std::fprintf(stderr, "%s\n", e.what()); return 1; }
#endif
}
