# CUDA PHY Acceleration

This document covers building, configuring and benchmarking the CUDA acceleration
backend for the OCUDU PHY and Open Fronthaul IQ compression paths. For the layering,
interface contracts and runtime policy behind it, see the PHY Acceleration page in
the Doxygen documentation (`docs/doxygen/phy_acceleration.dox`).

Acceleration is opt-in at build time (`-DENABLE_CUDA=ON`) and every accelerated path
keeps its CPU implementation as a fallback.

## Motivation And Purpose

CUDA acceleration for OCUDU was written to become part of the OCUDU project, not
to live as a long-term fork. The immediate motivation is to provide a
high-performance OCUDU implementation on CUDA-enabled platforms across a broad
range of GPU devices, from datacenter systems to mobile, edge, and embedded
systems, including both integrated GPUs and discrete GPUs. The longer-term goal
is to make accelerated L1 processing available in the same open,
community-developed codebase as the rest of the CU/DU stack, so it can be
reviewed, extended, tested, and maintained alongside the existing L1/L2/L3
implementation.

The near-term implementation focuses on NVIDIA CUDA because that is the first
accelerated backend brought up in tree. The broader architectural
intent is a coherent CPU/GPU acceleration framework that can support multiple
vendors, multiple processor architectures, and a diverse feature set over time.
That includes conventional PHY acceleration, future accelerated RAN use cases
embedded in the same framework, and runtime selection between CPU and
accelerated implementations without fragmenting OCUDU into separate projects.
The same framework should provide a platform for accelerating diverse OCUDU use
cases as GPU capabilities, deployment targets, and community requirements
evolve.

Where possible, this work follows OCUDU's existing backend factory model and
per-architecture implementation pattern: common interfaces and selectors at the
OCUDU layer, with optimized CPU, CUDA, and future backend implementations behind
those interfaces. Keeping the accelerated paths aligned with those abstractions
is important for upstream review, community contribution, multi-platform
testing, and long-term maintainability.

Measurement metadata for the DGX Spark bring-up sample numbers:

- Measurement commit: `9fd4047b43`
- Sample gNB version: `OCUDU 5G gNB version 26.04.0 (9fd4047b43)`
- Build: `Release`, `ENABLE_CUDA=ON`, `CMAKE_CUDA_ARCHITECTURES=121`,
  `MCPU=neoverse-v2`
- Platform used for the sample numbers below: NVIDIA GB10, driver `580.95.05`,
  CUDA toolkit `13.0.88`, aarch64 Cortex-X925/A725 CPU complex

## What Is Accelerated

CUDA-backed acceleration is available across the PHY and Open Fronthaul
paths while preserving CPU fallback paths and explicit configuration controls.

- Upper-PHY PUSCH: resident GPU demodulation, equalization, soft-bit handling,
  descrambling/rate-dematching support, and LDPC decode integration. The
  resident path avoids avoidable host round trips on supported configurations.
- Upper-PHY PDSCH: CUDA PDSCH block processing and GPU grid-output paths,
  including direct CUDA-visible resource-grid writing where supported.
- SRS: CUDA SRS channel estimation benchmarks and runtime selection.
- PRACH: CUDA PRACH detector support and lower-PHY PRACH OFDM demodulation
  support. The sample OTA profile currently keeps upper-PHY PRACH detection on
  the CPU while retaining lower-PHY PRACH demodulation acceleration.
- Split-8 lower PHY: CUDA lower-PHY TX baseband processing and PUxCH RX OFDM
  demodulation.
- Open Fronthaul IQ compression: CUDA BFP and no-compression TX compression and
  RX decompression paths, including direct device-grid and batched symbol modes.
- CUDA-visible resource grids and buffers: managed/device-visible grid policies
  reduce host/device copies in the PUSCH, PDSCH, PRACH, lower-PHY, and OFH
  paths.

## DGX Spark Quick Numbers

These are smoke measurements from this DGX Spark/GB10 system after running the
performance tuning script. They show the rough order of magnitude of the
acceleration, not a final benchmark claim. Broader benchmarking across multiple
platforms, radios, bandwidths, MCS tables, layer counts, and live traffic
profiles is still in progress.

To reproduce the table: the PUSCH and PDSCH rows come from
`scripts/cuda_accel/run_type1_dmrs_ul_dl_gpu_cpu_sweeps.sh` and the OFH BFP rows
come from `tests/benchmarks/ofh/run_ofh_compression_matrix.sh`. The exact
commands and expected output are in
[PUSCH And PDSCH Benchmark Examples](#pusch-and-pdsch-benchmark-examples) and
[Open Fronthaul Compression Benchmark](#open-fronthaul-compression-benchmark)
below.

| Path | Workload | CPU | GPU | Result |
| --- | --- | ---: | ---: | ---: |
| PUSCH sensitivity | 100 MHz, 273 PRB, 4 layers, 8 RX ports, MCS 20, 64QAM, 100 frames/point | 13.1 dB at 10% BLER | 13.0 dB at 10% BLER | -0.1 dB delta |
| PUSCH latency | 100 MHz, 273 PRB, 4 layers, 8 RX ports, MCS 20, 64QAM | 13122.3 us mean | 618.9 us mean | 21.20x faster |
| PDSCH latency | 100 MHz, 273 PRB, 4 layers, 4 ports, MCS 20, 64QAM | 625.0 us p50 | 186.0 us p50 | 3.36x faster |
| OFH BFP RX decompression | 100 MHz, 1 port, 9-bit, 14 symbols | 389.2 us p50 | 14.6 us p50 | 26.66x faster |
| OFH BFP TX compression | 100 MHz, 1 port, 9-bit, 14 symbols | 539.2 us p50 | 14.1 us p50 | 38.24x faster |
| OFH BFP RX decompression | 100 MHz, 4 ports, 9-bit, 14 symbols | 1335.5 us p50 | 18.4 us p50 | 72.58x faster |
| OFH BFP TX compression | 100 MHz, 4 ports, 9-bit, 14 symbols | 1502.1 us p50 | 30.9 us p50 | 48.61x faster |

Small workloads may not amortize GPU launch and synchronization costs. For
example, the same quick sweep measured 20 MHz 1-layer PUSCH latency at 225.8 us
CPU versus 262.3 us GPU, and 20 MHz 1-layer PDSCH latency at 29.4 us CPU versus
79.9 us GPU.

## Build for the right NVIDIA architecture

You must specify the correct CUDA architecture (CMAKE_CUDA_ARCHITECTURES) because NVIDIA’s compiler needs it to generate hardware-specific binary code (SASS) and forward-compatible virtual code (PTX) for your exact GPU model. Failing to set it correctly will cause your application to crash at runtime, take a massive performance hit, or throw build-time errors. For example, if you attempt to execute that compiled binary on a newer GPU, the GPU will not recognize the instructions and crash with runtime error: 'CUDA Error: no kernel image is available for execution on the device'.

You can learn the maximum supported architecture of your specific NVIDIA GPU by running the following command:

```bash
nvidia-smi --query-gpu=gpu_name,compute_cap --format=csv
```
It reports the runtime state and identity of GPU card(s) in a system. The column 'compute_cap' shows the compute capability that can be converted into an architecture code. For example,

```
name, compute_cap
NVIDIA RTX 5000 Ada Generation Laptop GPU, 8.9
```
corresponds to architecture 89 that can be specified in a build config:

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DENABLE_CUDA=ON \
  -DCMAKE_CUDA_ARCHITECTURES=89

cmake --build build -j 8
```
Useful architecture values:

- DGX Spark / GB10: `121`
- GH200 / H100 / H200: `90`
- GB200 / B200: `100`
- GB300 / B300: `103`
- RTX 40xx / Ada: `89`
- RTX 5000 / Ada: `89`
- RTX 50xx / Blackwell client GPUs: `120`

Check the resulting build cache:

```bash
grep -nE '^CMAKE_CUDA_ARCHITECTURES|^ENABLE_CUDA|^MCPU|^CMAKE_BUILD_TYPE' \
  build/CMakeCache.txt
```

Expected DGX Spark values:

```text
CMAKE_BUILD_TYPE:STRING=Release
CMAKE_CUDA_ARCHITECTURES:UNINITIALIZED=121
ENABLE_CUDA:BOOL=ON
MCPU:STRING=neoverse-v2
```

## Build For DGX Spark

DGX Spark / GB10 uses an ARM CPU complex with Cortex-X925 and Cortex-A725 cores.
GCC 13.3 on Ubuntu 24.04 does not know these exact core names, so the build
uses `-DMCPU=neoverse-v2` as a practical ARMv9/SVE2/BF16/I8MM-compatible
workaround. It is correct and much better than silently falling back to generic
ARMv8 code, but it is not perfectly tuned for the GB10 cores.
CUDA acceleration is opt-in at configure time; pass `-DENABLE_CUDA=ON` and an
explicit `CMAKE_CUDA_ARCHITECTURES` value for the target GPU.

Configure and build:

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DENABLE_CUDA=ON \
  -DCMAKE_CUDA_ARCHITECTURES=121 \
  -DMCPU=neoverse-v2

cmake --build build -j 8
```

The CPU reference paths in the benchmark examples below are compiled from the
same Release build as the CUDA paths. In the `cmake` configuration above (the
build used to produce the sample numbers in this README), the CPU objects were
compiled with `-mcpu=neoverse-v2 -O3 -DNDEBUG -fno-trapping-math
-fno-math-errno` and the CUDA objects were built for `sm_121`. On
the GCC 13.3 toolchain used for the DGX Spark measurements, `-mcpu=native` does
not emit a useful GB10 CPU target and `-mcpu=cortex-x925` / `-mcpu=cortex-a725`
are not supported, so `-mcpu=neoverse-v2` is the best confirmed GCC target flag
for these sample numbers.

## Run A CUDA gNB Profile

Start from an in-tree gNB configuration under `configs/`, adapt it for the
deployment's core network, radio, band, bandwidth, antenna count, and timing.

All CUDA acceleration mode selectors default to `auto`, which automatically
enables GPU acceleration whenever the build and runtime support it. You do not
need to add any of the options from the next section to turn acceleration on;
they are only needed to force or disable a specific path. The sample
`configs/gnb_rf_b200_tdd_n78_20mhz.yml` config leaves them out entirely and
still runs accelerated.

Run the performance tuning script once per boot before launching the gNB (see
[Performance Tuning](#performance-tuning)):

```bash
./scripts/ocudu_performance
```

Example run from the repository checkout:

```bash
./build/apps/gnb/gnb \
  -c configs/gnb_rf_b200_tdd_n78_20mhz.yml
```

Configuration is validated at startup; an invalid config fails fast with an
explanatory error before the radio is brought up.

## YAML Configuration Options

All operator-facing acceleration mode selectors accept the same values and
default to `auto`, so any selector left out of the config behaves as `auto`:

- `auto`: apply the component's runtime policy when acceleration is available.
  This is the default for every selector below, but a policy can still prefer
  the host path (notably PDSCH on a discrete CUDA device).
- `enabled`: require the accelerated path and fail configuration if it cannot be
  used.
- `disabled`: force the CPU path, or an explicitly configured non-CUDA hardware
  path where applicable.

Upper-PHY options under `expert_phy`:

| Option | Values | Effect |
| --- | --- | --- |
| `pusch_acceleration_mode` | `auto`, `enabled`, `disabled` | Selects the CUDA resident PUSCH demodulator/decoder path when available. The GPU channel estimator follows `pusch_channel_estimator_td_strategy` (default `interpolate`), as the CPU estimator does. |
| `ul_cuda_visible_grid_mode` | `auto`, `managed`, `pinned` | Selects UL resource-grid memory. `auto` uses managed memory on integrated GPUs and pinned host memory otherwise; `managed` requires a CUDA managed grid suitable for direct OFH-to-PUSCH GPU processing. |
| `srs_acceleration_mode` | `auto`, `enabled`, `disabled` | Selects the CUDA SRS estimator when available. |
| `pdsch_acceleration_mode` | `auto`, `enabled`, `disabled` | Selects the CUDA PDSCH block processor when the PDSCH processor type is `auto` or `flexible`. On a discrete CUDA device, `auto` prefers host unless the downlink grid is CUDA-visible or `OCUDU_PDSCH_AUTO_ENABLE_DISCRETE=1`; use `enabled` to require CUDA. |
| `prach_acceleration_mode` | `auto`, `enabled`, `disabled` | Selects the CUDA upper-PHY PRACH detector when available. |
| `pdsch_acceleration_nof_lanes` | `0` or positive integer | `0` uses the default lane policy; non-zero values bound concurrent accelerated PDSCH lanes. |
| `ldpc_decoder_algorithm` | `auto`, `boxplus`, `min_sum` | Selects the CUDA LDPC decoder algorithm. `auto` chooses the lower-latency algorithm for the current codeblock batch. |

Split-8 SDR lower-PHY options under `ru_sdr.expert_cfg`:

| Option | Values | Effect |
| --- | --- | --- |
| `low_phy_tx_acceleration_mode` | `auto`, `enabled`, `disabled` | Selects CUDA lower-PHY TX baseband processing. The modulator reads the downlink grid directly on the device only on fully coherent parts (GH200 class); elsewhere, including GB10 and discrete GPUs, it stages the host grid every slot and logs `Lower-PHY TX GPU path selected: host resource-grid staging fallback.` once. |
| `low_phy_rx_acceleration_mode` | `auto`, `enabled`, `disabled` | Selects CUDA PUxCH RX OFDM demodulation. |
| `low_phy_prach_demodulation_acceleration_mode` | `auto`, `enabled`, `disabled` | Selects CUDA PRACH OFDM demodulation in the lower PHY. |

Split-7.2 Open Fronthaul option under `ru_ofh`:

| Option | Values | Effect |
| --- | --- | --- |
| `compression_acceleration_mode` | `auto`, `enabled`, `disabled` | Selects CUDA OFH IQ compression/decompression for supported compression types. |

The example below sets explicit values only to illustrate the syntax. Omitting
any key leaves it at `auto`, which follows the component policy described
above. Use `enabled` when deployment validation must fail rather than fall back.

```yaml
ru_sdr:
  expert_cfg:
    low_phy_tx_acceleration_mode: auto
    low_phy_rx_acceleration_mode: auto
    low_phy_prach_demodulation_acceleration_mode: auto

expert_phy:
  pusch_acceleration_mode: enabled
  ul_cuda_visible_grid_mode: managed
  srs_acceleration_mode: enabled
  pdsch_acceleration_mode: enabled
  prach_acceleration_mode: disabled
  pdsch_acceleration_nof_lanes: 0
  ldpc_decoder_algorithm: auto

ru_ofh:
  compression_acceleration_mode: auto
```

Low-level `OCUDU_*` environment variables are profiling hooks
unless a launch script documents them explicitly. Prefer the YAML options above
for normal operation.

Useful environment hooks:

| Variable | Values | Effect |
| --- | --- | --- |
| `OCUDU_CUDA_VISIBLE_GRID` | `managed`, `pinned`, `auto`, or unset | Global CUDA-visible resource-grid override. Prefer the YAML option for UL application configuration. |
| `OCUDU_DL_CUDA_VISIBLE_GRID` | `managed`, `pinned`, `auto`, or unset | Direction-specific DL resource-grid override. |
| `OCUDU_UL_CUDA_VISIBLE_GRID` | `managed`, `pinned`, `auto`, or unset | Direction-specific UL resource-grid override used when the YAML mode is `auto`. |
| `OCUDU_PDSCH_DIRECT_DEVICE_GRID` | `1`, `0`, `true`, `false`, `on`, `off` | Enables or disables the direct CUDA-visible PDSCH grid writer. The GB10 OTA path defaults to enabled where available. |
| `OCUDU_PDSCH_DIRECT_GRID_ENCODE` | `1`, `0`, `true`, `false`, `on`, `off` | Enables fused PDSCH encode-direct-to-grid. Keep enabled for isolated PDSCH latency benchmarks; disable for sustained multi-sector full-gNB runs when DL and UL GPU work contend. |
| `OCUDU_DU_LOW_PDSCH_EXECUTOR_MAX_BATCH` | positive integer or unset | Caps the DU-low PDSCH executor fork-limiter batch size. Use `1` for sustained multisector GPU runs to avoid long PDSCH bursts monopolizing worker threads. Defaults to the upstream batch size. |
| `OCUDU_DU_LOW_PDSCH_EXECUTOR` | `rt`, `non-rt-high`, `non-rt-medium` | Routes sustained full-gNB PDSCH data-processing tasks to a different main-pool executor queue. Defaults to `rt`; `non-rt-medium` is useful for GB10 5-sector stress testing where PDSCH and PUSCH need fairer sharing. |
| `OCUDU_DU_LOW_EXTRA_DL_RG` | non-negative integer or unset | Adds extra downlink resource grids per cell without changing PHY processing delay. Use this for sustained multi-sector dummy-RU stress when DL grid release jitter causes pool misses. |
| `OCUDU_PDSCH_CUDA_STREAM_PRIORITY` | `normal`, `high`, `low` | Overrides PDSCH CUDA stream priority for A/B testing. Defaults to normal. |
| `OCUDU_PUSCH_CUDA_STREAM_PRIORITY` | `normal`, `high`, `low` | Overrides PUSCH CUDA stream priority for A/B testing. Defaults to normal. |
| `OCUDU_OFH_COMPRESSION_IMPL` | `cuda`, `gpu`, `cpu_auto`, `cpu`, `host`, `neon` | Implementation-selection override for both OFH TX compression and RX decompression. |
| `OCUDU_OFH_TX_COMPRESSION_IMPL` | same as above | Implementation-selection override for OFH TX compression only. |
| `OCUDU_OFH_RX_COMPRESSION_IMPL` | same as above | Implementation-selection override for OFH RX decompression only. |
| `OCUDU_PUSCH_ACCELERATION_TIMING` | `1`, `0`, `true`, `false`, `on`, `off` | Enables the resident PUSCH per-stage timing breakdown printed by the integration benchmarks. Total latency is still measured without it. |
| `OCUDU_PUSCH_ENABLE_ACCELERATED_UCI` | `1`, `0`, `true`, `false`, `on`, `off` | Opts into resident PUSCH HARQ/CSI Part 1 demux/decode. Leave unset for OTA unless that path is under test. |

Current production support boundaries:

- PUSCH and PDSCH startup validators on this dev rebase currently accept normal
  CP-OFDM Type-1 DM-RS. Some standalone CUDA benchmarks can exercise Type-2
  DM-RS kernels, but treat that as benchmark coverage until the product
  validators and OTA path enable it.
- Resident PUSCH SCH acceleration is intended for 1, 2, 3, or 4 layers and 1,
  2, 4, or 8 RX ports, with no UCI by default. Resident UCI is limited to
  HARQ/CSI Part 1 and remains opt-in; CSI Part 2 keeps the host path.
- Transform-precoded PUSCH is limited to one layer in the resident path. It is
  useful for Msg3-style smoke coverage, but non-transform Type-1 DM-RS remains
  the primary latency and sensitivity target for the bring-up platform.
- HARQ retransmissions always decode on the host LDPC path (`decode_batch`
  with `new_data=false`), soft-combining with the rate-matching buffer of the
  HARQ process. Because a resident first transmission keeps its dematched LLRs
  on the device, the decoder exports them into that buffer when the resident
  transport block fails its CRC (`export_resident_softbits`, fp16 device LLRs
  on the demodulator's x4 host scale, punctured 2Z prefix skipped). If the
  backend cannot export (direct rate-match→LDPC kernel, used for ≥32
  codeblocks by default — tunable via `OCUDU_LDPC_DIRECT_RM_MIN_CBS`, or
  disabled with `OCUDU_LDPC_DIRECT_RM=0`),
  the buffer is zeroed so the retransmission decodes on its own instead of
  against stale soft bits of an unrelated transport block. Before this, OTA
  retransmissions on the reference cell failed at 94% while the same bursts
  decoded standalone; `pusch_codeblock_decoder_cuda_batch_test` pins the
  export against the host rate dematcher.
- PDSCH direct device-grid mapping supports one-layer allocations and
  multi-layer identity-style allocations where the number of layers equals the
  number of ports. Other precoding layouts use the host resource-grid writer.
- `pusch_e2e_sensitivity_sweep` currently uses a host resource grid, so it
  validates BLER parity but not the direct CUDA-visible RX-grid path. Use
  `pusch_e2e_pipeline_test --rx-device-grid direct-managed --require-gpu-resident`
  for producer-event ordering, direct-grid enforcement and latency coverage.

## Support of multi-GPU hosts

A single, externally-configurable option ```nvcuda_device_id``` now threads from operator config down into every CUDA-accelerated component. Default is 0. The option is registered in the static library ```ocudu_o_du_low_unit_helpers```, so any executable that links the DU-low config unit gets it.

It can be set in YAML via the nesting ```du_low``` → ```expert_phy``` → ```nvcuda_device_id```:

```
du_low:
  expert_phy:
    nvcuda_device_id: 2 # using GPU device ID=2
```

and then run: ```gnb -c your_config.yml```. The command line flag ```expert_phy --nvcuda_device_id <N>``` and this YAML key populate the identical field. If both are given, the command line overrides the config file.

The following applications support option ```nvcuda_device_id```:

- apps/du_low/odu_low
- apps/du/odu
- apps/gnb/gnb

Example: running gNB on a NVIDIA-enabled host via GPU device ID=2
```
sudo apps/gnb/gnb -c /path/to/configfile.yaml expert_phy --nvcuda_device_id 2
```

## Performance Tuning

Run the repository tuning script before latency or sensitivity capture:

```bash
./scripts/ocudu_performance
```

For a non-interactive run with the default yes responses:

```bash
printf '\n\n\n' | ./scripts/ocudu_performance
```

Output from the DGX Spark run:

```text
Scaling governor set to performance
Disabled DRM KMS polling
net.core.wmem_max = 33554432
net.core.rmem_max = 33554432
net.core.wmem_default = 33554432
net.core.rmem_default = 33554432
Tweaked network buffer sizes
```

## PUSCH And PDSCH Benchmark Examples

Build the benchmark targets:

```bash
cmake --build build --target \
  pusch_e2e_sensitivity_sweep \
  pusch_e2e_pipeline_test \
  pdsch_gpu_latency_benchmark \
  -j 8
```

The combined CPU/GPU Type-1 DMRS sweep script runs PUSCH sensitivity, PUSCH
latency, and PDSCH latency:

```bash
bash scripts/cuda_accel/run_type1_dmrs_ul_dl_gpu_cpu_sweeps.sh \
  --build-dir build \
  --out-dir build/cuda-benchmark-dgx-spark-100mhz-4l \
  --quick \
  --prbs 273 \
  --topologies 4x8 \
  --mcs 20 \
  --pusch-snr-step 1.0 \
  --pusch-frames 100 \
  --pusch-latency-iterations 10 \
  --pdsch-latency-iterations 10 \
  --rx-device-grid managed \
  --resource-grid-memory managed \
  --device-grid-memory managed
```

Example output measured on this DGX Spark using MCS 20 with the `qam64` table
for the PUSCH and PDSCH measurements:

```text
PUSCH sensitivity summary
BW      PRB   ULTopo      L  ULPorts    CPU10dB    GPU10dB     Delta
100MHz  273   4L8P        4        8       13.1       13.0      -0.1

PUSCH latency summary
BW      PRB   ULTopo      L  ULPorts    CPU(us)    GPU(us)  Speedup        CPU        GPU Mismatches
100MHz  273   4L8P        4        8    13122.3      618.9    21.20x      10/10      10/10          0

PDSCH latency summary
BW      PRB   DLTopo      L  DLPorts Backend    Avg(us)    P50(us)    P90(us)    P99(us)       Grid
100MHz  273   4L4P        4        4 cpu        626.477    625.043    633.731    633.731       host
100MHz  273   4L4P        4        4 gpu        259.555    186.017    438.082    438.082     direct
```

Direct PUSCH-only examples:

```bash
build/tests/integrationtests/phy/upper/channel_processors/pusch_e2e_sensitivity_sweep \
  --nof_prb 273 --mcs_index 20 --mcs_table 1 \
  --snr_start -5.0 --snr_stop 25.0 --snr_step 1.0 \
  --nof_frames 100 --gpu-type gpu --layers 4 --ports 8 \
  --dmrs-type type1 --dmrs-symbols 2,11 --equalizer mmse

build/tests/integrationtests/phy/upper/channel_processors/pusch_e2e_pipeline_test \
  --prb 273 --sinr 25 --mcs 20 --mcs-table qam64 \
  --iterations 10 --warmup 5 --layers 4 --ports 8 \
  --dmrs-type type1 --dmrs-symbols 2,11 \
  --rx-device-grid managed --quiet
```

For broader sweeps, drop `--quick` and select the PRB/topology list:

```bash
bash scripts/cuda_accel/run_type1_dmrs_ul_dl_gpu_cpu_sweeps.sh \
  --build-dir build \
  --out-dir build/type1-dmrs-full \
  --full \
  --prbs 51,106,273 \
  --topologies 1x1,2x4,4x8 \
  --rx-device-grid managed \
  --resource-grid-memory managed \
  --device-grid-memory managed
```

## Open Fronthaul Compression Benchmark

Build and run the OFH benchmark matrix:

```bash
cmake --build build --target ofh_compression_benchmark -j 8

bash tests/benchmarks/ofh/run_ofh_compression_matrix.sh \
  --build-dir build \
  --output-dir build/ofh-compression-dgx-spark \
  --repetitions 200 \
  --symbols 14 \
  --types "bfp" \
  --bandwidths "100" \
  --ports "1 4"
```

The script sweeps BFP bit widths `8, 9, 10, 12, 14, 16`, reports CPU and GPU
p50 latency, and writes raw CSV plus speedup CSV files. The excerpt below
highlights 9-bit BFP because it is a common Open Fronthaul configuration.

Example output excerpt measured on this DGX Spark:

```text
CPU/GPU p50 speedups (>1 means GPU faster):
type  bw     port  path               bits      CPU us     GPU us   speedup
bfp   100M   1     rx_decompression   9          389.2       14.6     26.66
bfp   100M   1     tx_compression     9          539.2       14.1     38.24
bfp   100M   4     rx_decompression   9         1335.5       18.4     72.58
bfp   100M   4     tx_compression     9         1502.1       30.9     48.61
```

For a full OFH matrix across compression types, bandwidths, ports, and bit
widths:

```bash
bash tests/benchmarks/ofh/run_ofh_compression_matrix.sh \
  --build-dir build \
  --output-dir build/ofh-compression-full \
  --repetitions 1000 \
  --symbols 14 \
  --types "bfp none" \
  --bandwidths "5 10 20 100" \
  --ports "1 2 4"
```

## Validation Run

Build every target the validation uses first. `ctest -R` exits 0 when the regex matches nothing, so a target that
was not built silently drops out of the run; `--no-tests=error` turns that into a failure.

```bash
cmake --build build -j 8 --target \
  ofdm_demodulator_cuda_test ofdm_prach_demodulator_cuda_test pdxch_baseband_modulator_cuda_test \
  ldpc_encoder_gpu_cpu_test ldpc_decoder_gpu_cpu_test prach_detector_cuda_test prach_detector_geometry_test \
  vkfft_kernel_cache_test pusch_gpu_cpu_comparison_test pdsch_gpu_e2e_test pusch_e2e_pipeline_test \
  pusch_resident_dematch_scramble_test srs_estimator_gpu_latency_benchmark srs_estimator_gpu_sensitivity_sweep \
  srs_estimator_gpu_thread_safety_test ofh_iq_compression_cuda_test pusch_resident_dapp_adapter_test
```

CUDA PHY, Open Fronthaul compression and resident-receiver subset:

```bash
ctest --test-dir build --output-on-failure --no-tests=error -j1 -R \
'^(ofdm_demodulator_cuda_test|ofdm_prach_demodulator_cuda_test|pdxch_baseband_modulator_cuda_test|ldpc_encoder_gpu_cpu_test|ldpc_decoder_gpu_cpu_test|prach_detector_cuda_test|prach_detector_geometry_test|vkfft_kernel_cache_test|pusch_gpu_cpu_comparison_test|pdsch_gpu_e2e_test|pusch_e2e_pipeline_test|pusch_resident_dematch_scramble_test|srs_estimator_gpu_latency_baseline_4x4_n4|srs_estimator_gpu_sensitivity_baseline_4x4_n4|pusch_gpu_cpu_cfo_interpolation_test|pusch_gpu_cpu_sync_sinr_test|srs_estimator_gpu_thread_safety_host)$|^ofh_iq_compression_cuda/|pusch_resident_dapp_adapter'
```

Result on DGX Spark (GB10), 2026-09-27:

```text
100% tests passed, 0 tests failed out of 63
Total Test time (real) = 410.00 sec
```

The subset includes the field-derived regressions: CPU/GPU PUSCH parity with 400 Hz CFO and time interpolation,
synchronous-path post-equalization SINR, PDSCH grid equivalence for every redundancy version under limited-buffer
rate matching, codebook-precoded PDSCH on host and managed grids, and 16 OFH compression cases.

Total CUDA-focused tests available in this build:

```bash
ctest --test-dir build -N -R \
'pusch.*(gpu|resident|pipeline|sensitivity)|pdsch.*gpu|srs.*gpu|prach.*cuda|ofdm.*cuda|pdxch.*cuda|ldpc.*gpu'
```

This lists 25 CUDA-focused PHY tests in the configured build.
