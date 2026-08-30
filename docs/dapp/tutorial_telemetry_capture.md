<!-- SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc. -->
<!-- SPDX-License-Identifier: BSD-3-Clause-Clear -->

# Tutorial: telemetry deep-dive & data capture

Everything the E3 telemetry plane serves, how its rate limiting really works,
and the full capture pipeline: live streams → HDF5 → per-burst export →
offline decode with the gNB's own receiver chain. This is the data half of
the train-tune-redeploy loop.

Reference docs: [e3_wire.md](e3_wire.md) (wire formats),
[yaml_configuration.md](yaml_configuration.md) (floor keys),
[tutorial 0](tutorial_docker_testbed.md) (no-radio testbed).

## 1. The eight streams

| Stream | id | What each record carries |
|---|---|---|
| `pusch.symbols` | 1 | per-UE equalized data I/Q for a whole grant + SINR/EVM/TA/CFO, **plus the full grant parameters and the gNB's CRC verdict** — the offline-replay stream |
| `pusch.crc` | 2 | rnti, slot, CRC ok/KO — the lightest link-quality signal |
| `srs.channel` | 3 | SRS channel coefficients + RSRP/EPRE/noise/TA(ns) |
| `pusch.dmrs_channel` | 4 | DM-RS channel estimates + per-grant quality |
| `gnb.metrics_json` | 5 | the gNB's JSON metrics report, byte-for-byte |
| `classc.spectrum` | 6 | Class-C spectrum snapshot (FlatBuffers metadata + tensor) |
| `classc.srs_isac` | 7 | Class-C SRS/ISAC snapshot |
| `pusch.grid` | 8 | **raw pre-equalization REs** + grant — re-run CE/EQ offline; the largest stream |

Every indication carries `sequence`, `route_epoch`, `monotonic_ns`, and
`dropped_objects`. Treat `droppedObjects` and `routeEpoch` changes as visible
discontinuities, not errors.

## 2. The rate model (read this before filing "telemetry is slow")

Two floors govern every stream, both advertised per stream in the setup
response:

- **`telemetry_<stream>_min_interval_ms`** — the *default* per-RNTI interval,
  used for subscribers that didn't request a rate.
- **`telemetry_<stream>_hard_floor_ms`** — a *hard* bound: subscription
  requests are clamped to it on admission, and it's enforced again
  per-stream across all RNTIs at publication. **No subscriber can go below
  it.**

Hard floors default **non-zero** — `pusch.symbols` 10 ms, `pusch.crc` 1 ms,
`srs.channel` 5 ms, `pusch.dmrs_channel` 5 ms, `pusch.grid` 20 ms — because a single
full-rate subscriber would otherwise remove all PHY-side thinning for every
subscriber on that stream. **Full-rate capture is a deliberate operator act**:

```yaml
dapp:
  telemetry_pusch_symbols_hard_floor_ms: 0   # explicit opt-in to full rate
```

then request `--interval-ms 0` / `minimum_interval_ms=0` client-side. The
Class-C snapshot streams (`classc.*`) carry no floor — full rate is theirs by
default. Floors are checked *before* any capture work, so an unsubscribed or
floored stream costs the radio near-nothing (one atomic load).

Per-subscription knobs (library `SubscriptionConfig`; the CLI exposes a
subset): `sample_every_n` (1..65535 — skipped records are *sampled*, not
counted as drops), `drop_policy` (`dropNewest` / `dropOldest` /
`coalesceLatest`), `rnti_filter` (up to 32), `cell_id`. Subscriptions are
**leased** — polling or renewal keeps them alive; an abandoned subscription
lapses on its own.

**`dropped` semantics:** a cumulative per-subscription counter, incremented
when the bounded queue overflows (per the drop policy) or the producer's
try-lock loses a race. Producers never block the radio — a climbing counter
means *your consumer* is behind. Slow down (`sample_every_n`, floors) or
drain faster.

## 3. Watch live

```bash
export PYTHONPATH=ocudu/python OCUDU_E3_SCHEMA_DIR=ocudu/schemas/e3

python3 -m ocudu_e3 --port 36423 catalog          # streams + advertised floors
python3 -m ocudu_e3 --port 36423 subscribe pusch.crc srs.channel \
    --rnti 0x4601 --duration 20                    # human-readable lines
python3 -m ocudu_e3 --port 36423 subscribe pusch.crc --json   # NDJSON (bytes stripped)
```

All of this works radio-free against [tutorial 0's loopback
agent](tutorial_docker_testbed.md) for `pusch.symbols`, `pusch.crc`,
`srs.channel`, and `gnb.metrics_json`.

## 4. Capture to HDF5

The SDK ships a production-shaped recorder,
[`examples/e3_telemetry_python/record_telemetry.py`](https://gitlab.com/ocudu/work_groups/wg2_ai_ran/ocudu-dapp-sdk/-/blob/main/examples/e3_telemetry_python/README.md):

```bash
# One UE's PUSCH, everything needed for offline replay
python3 record_telemetry.py pusch.symbols pusch.crc --rnti 0x4601 --output ue.h5

# The full-fat capture (mind the floors and disk!)
python3 record_telemetry.py pusch.symbols pusch.crc pusch.grid srs.channel \
    --interval-ms 0 --duration 30 --output capture.h5
```

What you get: one HDF5 group per stream; scalar columns plus a single
contiguous complex64 `samples` dataset with per-record offsets; the PUSCH
grant fields and CRC verdicts; group/file attributes carrying stream ids,
advertised floors, RAN identity, and capture parameters — datasets that
document their own provenance. Records flush in batches of 256, so long
captures are disk-bounded, and the tool warns at startup about any stream
whose hard floor will clamp your request. (It defaults to `dropOldest` —
for recording you want the oldest data dropped, not the newest.)

## 5. Offline replay: decode your capture with the gNB's own receiver

The killer feature of `pusch.symbols`: each record carries the **complete
grant parameters** (MCS/table, RV, base graph, DM-RS config, scrambling, CRB
mask, UCI splits — everything). So a capture replays through the gNB's own
demapper → descrambler → UL-SCH demux → LDPC decoder → CRC:

```bash
# 1. Record on a live cell
python3 record_telemetry.py pusch.symbols --duration 30 --output ue.h5
# 2. Export per-burst JSON + cf32
python3 export_pusch_burst.py ue.h5 --all --only-complete --output bursts/
# 3. Replay each burst offline; compare with the gNB's verdict
for b in bursts/*.json; do ocudu-pusch-replay "$b"; done
#    → "replay CRC: OK | gNB CRC: OK"   (a MISMATCH is worth investigating)
```

This is the ground-truth harness for receiver work: capture once, iterate on
your algorithm offline against real over-the-air grants, with the shipped
receiver as the referee. `pusch.grid` extends the same idea upstream — raw
REs + grant, so you can re-run channel estimation and equalization too.

> **Live cell required for this section.** The loopback test agent's
> synthetic `pusch.symbols` carry no grant block or CRC verdict, so the
> export→replay chain needs a real gNB (with the `pusch.symbols` hard floor
> opened and, for complete codewords, `--only-complete` to skip truncated
> captures).

## 6. The pull path: E3DP for minimal consumers

Besides E3AP's push subscriptions, a deliberately tiny **poll** carrier
serves the same store: the E3 data profile (FlatBuffers over SCTP, default
port 38472). Two operations only — create/remove/list subscriptions and
`dataPoll` — with chunked tensors, lease renewal on every poll, and the same
eight streams reachable by interface id. Use it when embedding a full APER
stack is unwarranted; use E3AP for everything else. Wire details and the
reference-client interop notes (including the schema-regeneration warning)
are in [e3_wire.md](e3_wire.md) § E3 data profile.

## 7. Capture etiquette on a shared cell

- Floors are per-stream and shared: your full-rate `pusch.grid` capture and a
  colleague's dashboard ride the same publication path. The carrier thins
  per-subscription (a fast recorder doesn't speed up a slow dashboard), but
  the *capture work* happens at the fastest requested rate.
- `pusch.grid` at full rate is the heaviest thing you can ask of the
  telemetry plane — bound the duration.
- Watch your own `dropped` counter during capture; a recording with drops is
  a biased dataset.

## Next

- [Standalone Class-C dApp](tutorial_class_c_standalone.md) — turn a
  subscription into a dApp.
- [Model operations](tutorial_model_operations.md) — close the loop: publish
  what you trained on this data.
- [Troubleshooting](tutorial_troubleshooting.md) — the floor/drop entries
  condensed.
