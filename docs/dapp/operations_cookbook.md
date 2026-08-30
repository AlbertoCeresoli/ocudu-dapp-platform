<!-- SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc. -->
<!-- SPDX-License-Identifier: BSD-3-Clause-Clear -->

# dApp/E3 operations cookbook

Guided path: the [step-by-step tutorial series](README.md#start-here) walks
every workflow this reference covers.

End-to-end recipes for the platform's main workflows, each verified against a
live cell (51 PRB TDD n78, CUDA L1, two OTA UEs). Commands assume the gNB runs
with `dapp.e3ap_sctp` on `127.0.0.1:36423` and that `python/ocudu_e3` plus
`schemas/e3` are reachable (`export OCUDU_E3_SCHEMA_DIR=schemas/e3` from the
repository root when running in-tree). Capability background:
[capabilities.md](capabilities.md); wire detail: [e3_wire.md](e3_wire.md);
management model: [e3_management.md](e3_management.md).

A convenient alias used throughout:

```bash
alias dctl='python3 apps/tools/dappctl/ocudu_dappctl.py --sctp 127.0.0.1:36423'
```

## 1. Discover what the cell offers

```bash
python3 -m ocudu_e3 catalog          # RAN functions, telemetry streams, floors
dctl inventory                       # packages, instances, states, generations
dctl cell-info                       # PCI, band, ARFCN/centre, TDD, PRACH, gains
dctl health                          # runtime health counters
```

The catalog lists every telemetry stream with its default per-RNTI floor and
the aggregate hard floor; the setup response also carries the payload and
fragment bounds the association will honour.

## 2. Record telemetry at full rate and decode it offline

Recorders ask for `--interval-ms 0` (every event); the agent honours the
fastest active request per stream down to the configured hard floor while
other subscribers (for example a dashboard at 20–50 ms) keep their own rate.

```bash
# In the SDK checkout: examples/e3_telemetry_python/
python3 record_telemetry.py pusch.symbols pusch.crc pusch.grid srs.channel \
        --interval-ms 0 --duration 30 --output capture.h5

# Turn burst N into the JSON + cf32 pair the replay tool decodes:
python3 export_pusch_burst.py capture.h5 --index 0 --output burst0
# Or all of them:
python3 export_pusch_burst.py capture.h5 --all --output bursts/

# Demodulate, descramble, demultiplex, LDPC-decode and CRC-check with the
# production receiver components, comparing against the gNB's own verdict:
./build/apps/tools/pusch_replay/ocudu-pusch-replay burst0.json
#   replay CRC: OK | gNB CRC: OK
```

On a healthy cell the replay agrees with the gNB verdict on the large
majority of new-data bursts; systematic disagreement localizes a receiver
defect (this workflow found both the HARQ soft-buffer and DC-subcarrier
defects fixed on this branch). `pusch.grid` records carry the raw
pre-equalization resource elements plus the full grant parameters, so a
complete offline receiver study needs nothing else.

## 3. A/B a Class-A equalizer against the host receiver

Class-A arms are switched purely through the dApp lifecycle — no restart, no
YAML change. Deactivating every Class-A equalizer *is* the baseline: the host
falls back to its own GPU path.

```bash
dctl inventory                                   # find instanceId + generation
dctl lifecycle deactivate --instance <ID> --timeout-us 8000000
# ... and back:
for step in prepare arm shadow activate; do
  dctl lifecycle $step --instance <ID> --timeout-us 8000000
done
```

Measurement rules that were learned the hard way (a sequential comparison
once ranked a no-op control above both real arms):

- interleave the arms and reshuffle the order every round — link-adaptation
  drift is larger than the effects being measured;
- wait for MCS to stop ramping after a switch instead of sleeping a fixed
  time; discard blocks that never settle;
- read CRC counters as deltas within a block from a single RNTI — re-attach
  resets them;
- a block needs thousands of transport blocks to resolve BLER differences,
  which means sustained uplink load.

Lifecycle steps are asynchronous jobs; a step submitted while the previous
one is still applying returns a conflict — retry the full ladder rather than
declaring the switch failed.

## 4. Tune the reference scheduler over E3

The Class-B reference scheduler publishes its policy as a typed schema, so
`configSet` is the tuning interface. Every authority stage ships **off**;
enabling one changes live behavior immediately.

```bash
dctl config get --instance <SCHED_ID>            # schema + current generation
dctl config set --instance <SCHED_ID> --config-generation <N> \
     --set "enable_prb_avoidance=bool:true" \
     --set "olla_target_bler=f64:0.05"
```

The response is `accepted` plus a job id; the real outcome (`success`,
`invalidRequest` for out-of-range values, `conflict` for a stale generation)
is in the job record:

```bash
dctl jobs get --job <JOB_ID>
```

## 5. RAN control: release, handover, RF gain

```bash
dctl ue-release  --pci <pci> --rnti 0x4601                   # RRC release to idle
dctl ue-handover --pci <pci> --rnti 0x4601 --target-pci 3    # via the CU-CP mobility handler
dctl rf-gain --direction rx --port 0 --gain-db 40
```

Every mutation supports `--dry-run` (validated, no side effect). Validation
rules: the target PCI must differ from the serving cell; gains must be finite
and within −30..90 dB; unknown RNTIs are rejected. `dctl cell-info` reports
which controls the host can honour (`ranControlCapabilities`).

## 6. Out-of-process sensing without a catalog package

A plain process can subscribe to the Class-C snapshots over SCTP — no
manifest, no lifecycle, no root:

```bash
# SDK checkout:
python3 examples/e3_spectrum_sensing/emitter_detector.py --json
python3 examples/e3_isac_sensing/srs_isac_monitor.py
```

The spectrum stream delivers the complete uplink grid (cbf16, ~120 grids/s
on a 51 PRB cell) with the scheduler's expected-UL-use plan, so detections
can exclude every scheduled transmission. The bulk tensor is carried as raw
bytes inside the APER envelope — nothing per-sample is re-encoded.

## 7. Drive a controlled channel without moving hardware

When the RU-seam channel plugin is loaded at launch
(`OCUDU_CHANNEL_EMULATOR_LIB`/`_ARGS`), the channel-emulation dApp is its E3
control plane: `activate`/`deactivate` maps to the plugin's live enable, and
model, LOS, Doppler speed, delay spread, update period and bulk gain are
`configSet` keys applied without a restart:

```bash
dctl config set --instance <CHAN_ID> --config-generation <N> \
     --set "model=f64:1" --set "los=bool:true" --set "ue_speed_kmh=f64:3"
```

Live UEs ride through a model switch; RACH also passes through the emulated
channel, so attach is exercised under fading too.

## 8. Watch who else is on the E3 endpoint

```bash
dctl inventory            # dApp view
python3 -m ocudu_e3 catalog
dctl quiet-reservations   # authoritative bounded quiet windows
# downstream connections, their subscriptions and control counts:
#   `connections` field of the `inventory` result (see e3_management.md)
```

The connection inventory reports every downstream association with its
subscriptions — useful before restarting anything, since subscriptions die
with the association.

## 9. Verify a build before putting it on the air

```bash
ctest --test-dir <build> -R dapp          # unit + integration coverage
python3 -m ocudu_e3 catalog               # against the freshly started cell
dctl inventory && dctl health
```

The dummy-RU end-to-end campaigns (`dapp_dummy_ru_e2e.*`) exercise lifecycle,
process restart, failure containment and release provisioning without a
radio. GPU tests must not run concurrently with a live CUDA gNB on the same
device — expect soft-deadline noise if they do.

## Pitfalls index

| Symptom | Cause | Recipe |
|---|---|---|
| Subscription dies unexpectedly | Association closed (idle timeout, peer loss) | Re-setup + re-subscribe; see §8 |
| `accepted` but nothing changed | Mutation is an async job | Follow the job id (§4) |
| Lifecycle step returns conflict | Previous step still applying, or stale generation | Re-read generation, retry the ladder (§3) |
| Full-rate request delivers less than expected | Aggregate hard floor bounds the stream | `python3 -m ocudu_e3 catalog` shows both floors (§1) |
| Recorder sees `symbols_truncated` | Grant exceeded the capture bound | Bound documented in [known_limitations.md](known_limitations.md) |
| Replay cannot open old captures | Pre-fix exporters wrote JSON nulls | Re-export, or use a current `ocudu-pusch-replay` |
