<!-- SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc. -->
<!-- SPDX-License-Identifier: BSD-3-Clause-Clear -->

# SRS channel-estimator dApps

A Class-A SRS channel-estimator dApp replaces the gNB's SRS channel estimator
(TS 38.211 6.4.1.4): for every SRS occasion it receives the SRS resource elements
of each receive port and the transmitted sequence of each transmit port, and it
returns the wideband channel matrix and the link measurements the gNB's own
estimator produces. The gNB publishes the dApp's estimate exactly as it
publishes its own — to the scheduler (channel matrix, time alignment), to the
SRS-ISAC Class-C route and to the E3 `srs.channel` stream — and falls back to
its own estimator for any occasion the dApp does not serve.

Typical uses:

- **A learned SRS estimator** for uplink channel knowledge (precoding, beam and
  rank selection, time-advance tracking) in harsh channels, deployed and rolled
  back without restarting the gNB.
- **Sounding-based sensing** that needs a better channel matrix than the
  wideband least-squares estimate.
- **Dataset generation**: the SDK's `examples/e3_srs_dataset` records the exact
  input of this interface from a live cell, with the gNB's answer as label.

The interface is `OCUDU_DAPP_INTERFACE_SRS_CHANNEL_ESTIMATOR_V1` (`0x00010007`),
declared in `include/ocudu/dapp/use_cases/v1/srs_channel_estimator.h`. The SDK
ships reference implementations for both backends
(`ref_srs_channel_estimator_cpu`, `ref_srs_channel_estimator_cuda`) that
reproduce the gNB's host estimator.

## Invocation

The gNB's SRS estimator objects run on the SRS executor; each one owns one hook
lane (`nof_srs_lanes`, the same lanes as the SRS-ISAC publication). An occasion
reaches the dApp when the hook is active and the adapter can express it in the
interface (at most 8 receive and 4 transmit ports, the SRS symbols of one slot,
combs 2 and 4, a sequence that fits the uplink bandwidth). The runtime then
checks the occasion against the module's profile and the tensors against the
metadata before the module is called.

| Input | Content |
|-------|---------|
| `rx_symbols` | CF32 `[pilot_set, rx_port, symbol, pilot]` (`OCUDU_DAPP_LAYOUT_SRS_RX_SET_PORT_SYMBOL_PILOT_CF32_V1`): the resource elements at each pilot set's comb subcarriers |
| `reference_symbols` | CF32 `[tx_port, pilot]` (`OCUDU_DAPP_LAYOUT_SRS_REFERENCE_TX_PILOT_CF32_V1`): each port's low-PAPR sequence with its cyclic shift |
| `srs` metadata | `ocudu_dapp_srs_channel_estimator_metadata_v1` (schema `0x00010301`): ports, symbols, sequence length, comb, pilot sets, `n_cs_max`, each port's pilot set and cyclic shift, each set's first subcarrier, sequence group and number, subcarrier spacing, numerology, carrier width, flags, C-RNTI and `max_time_alignment_s` = 1 / (`n_cs_max` · SCS · comb) |

A pilot set is one comb offset. All ports share set 0 unless four ports use a
cyclic shift in the upper half of the range; then the odd ports' pilots are on
their own comb offset (set 1) and `OCUDU_DAPP_SRS_FLAG_INTERLEAVED_PILOTS_V1` is
set.

| Output | Content |
|--------|---------|
| `channel_matrix` | CF32 `[rx_port, tx_port]` (`OCUDU_DAPP_LAYOUT_SRS_CHANNEL_MATRIX_RX_TX_CF32_V1`): the wideband coefficient of each port pair scaled by the inverse noise standard deviation, so that the squared Frobenius norm reads as SNR (the gNB caps it at 40 dB) |
| `measurements` | F32 `[8]` (`OCUDU_DAPP_LAYOUT_SRS_MEASUREMENTS_F32_V1`): noise variance (linear), EPRE (dB), RSRP (dB), time alignment, its resolution, minimum and maximum (seconds), one spare; NaN for a value the module does not provide |

The channel matrix must be finite. A measurement is a finite number or NaN; the
time-alignment values come as all four or none (none reads as the gNB's "no
measurement" value), the noise variance is not negative and the minimum does
not exceed the maximum. Anything else is an invalid output: the gNB uses its own
estimate for that occasion and the instance's circuit breaker sees the failure.

## Backends

The hook follows the Class-A rule of the gNB's L1 path: a host-L1 gNB runs CPU
packages and hands them host tensors read from the resource grid; an
accelerated gNB runs CUDA packages
(`OCUDU_DAPP_SRS_CAP_RESIDENT_ACCELERATOR_V1`) and hands them device tensors.
On an accelerated gNB the adapter gathers the SRS resource elements on the GPU
from the owned device snapshot of the uplink grid (never the live managed grid),
copies the reference sequences to the device, invokes the module with
`invocation.execution_stream` set to the lane's stream, reads the eight
measurements and the channel matrix back and drains the stream before the
result is used.

The invocation deadline (1 ms by default) bounds the module call; the soft
completion wall (also 1 ms) bounds a CUDA module's queued work including the
readback. A late completion is still used and counted in
`class_a.completion.deadline_misses`.

## Profile and packaging

A module declares its envelope in `ocudu_dapp_srs_profile_v1`
(`OCUDU_DAPP_SRS_PROFILE_V1`, `0x00010302`), which is mandatory for this
interface: maximum receive ports (1–8), transmit ports (1–4), sequence length
(1–1632) and the combs it accepts (`OCUDU_DAPP_SRS_COMB_2_V1`,
`OCUDU_DAPP_SRS_COMB_4_V1`). An occasion outside it falls back with
`class_a.fallback.unsupported.rx_ports`, `.layers` (transmit ports) or
`.allocation` without calling the module.

The package manifest carries the same envelope as `srs_profile` in place of the
PUSCH `pusch_profile`:

```json
{
  "interface_id": 65543, "major": 1, "minor": 0, "class": "A",
  "placements": ["in_process"], "threading": "per_worker",
  "capabilities": 1,
  "srs_profile": {"max_rx_ports": 8, "max_tx_ports": 4, "max_sequence_length": 1632, "comb_mask": 3}
}
```

The E3 inventory reports it as `srsProfile` on the interface record, and
`dappctl` prints `srs-profile max-rx-ports=… max-tx-ports=… max-sequence-length=… comb-mask=…`.
The per-UE PUSCH route table does not apply: one SRS estimator serves every UE.

## Metrics

The SRS estimator reports through the same host metrics as every Class-A
interface: `dapp.invoke.count` and `dapp.invoke.latency_ns` per instance, the
`class_a.fallback.*` reasons (profile, metadata, invalid input or output,
deadline, device, exception), `class_a.completion.deadline_misses`, and the
breaker state. The SDK references also expose the module-side
`dapp.invoke.count` and `dapp.input.max_layers` (the largest number of transmit
ports seen, for the CPU reference).

## Datasets

The gNB's `srs.channel` telemetry carries, for every port pair, the
least-squares estimate together with the received pilots (`receivedPilots`,
CF32 `[symbol, pilot]`) and the port's reference sequence (`referenceSymbols`),
as well as each transmit port's own first subcarrier and cyclic shift. These
fields are the module input, so a capture of the stream is a training and
evaluation set for this interface. The SDK example `examples/e3_srs_dataset`
records it into HDF5, and `ocudu-dapp-srs-replay` runs the reference estimator
on exported occasions and compares it with the live cell.

## Verification

- `tests/unittests/phy/upper/dapp_srs_channel_estimator_test.cpp`: the adapter
  (tensors, output mapping, fallback, lanes, Class-C ordering).
- `tests/unittests/dapp/native_instance_test.cpp`: the runtime checks of
  profile, metadata and tensors against a fixture module.
- `srs_dapp_channel_estimator_vectors` writes the golden vectors the SDK's
  reference tests use: occasions synthesised on a resource grid, the exact
  module input and the host estimator's answer.
- `srs_dapp_reference_parity_test`, with `OCUDU_DAPP_SRS_REFERENCE_MODULE` and
  `OCUDU_DAPP_SRS_REFERENCE_CUDA_MODULE` naming the SDK reference packages,
  loads each one through the native runtime and checks the gNB adapter's result
  against the host estimator on every golden occasion (CUDA through the device
  snapshot).
