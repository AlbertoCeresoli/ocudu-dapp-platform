<!-- SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc. -->
<!-- SPDX-License-Identifier: BSD-3-Clause-Clear -->

# Tutorial: your first dApp — a standalone Class-C client over E3

The fastest way onto this platform is a **standalone out-of-process Class-C
dApp**: an ordinary program, on any host, in any language, that connects to
the gNB's E3 interface, consumes telemetry (uplink spectrum, SRS channel
estimates, PUSCH quality), runs *your* algorithm, and acts back through
management operations. No CUDA, no in-process ABI, no package signing, no
certification — a Python file and a socket.

This tutorial builds one in Python (~40 lines), tests it with **no radio**
against the docker loopback agent, then upgrades it to the real Class-C
spectrum stream on a live cell. A final section gives the honest C++ options.

Related: [tutorial 0 (the zero-hardware testbed)](tutorial_docker_testbed.md),
[e3_wire.md](e3_wire.md), [e3_management.md](e3_management.md), and the SDK's
[`examples/e3_spectrum_sensing/`](https://gitlab.com/ocudu/work_groups/wg2_ai_ran/ocudu-dapp-sdk/-/blob/main/examples/e3_spectrum_sensing/README.md)
— the copy-me this tutorial is distilled from.

## 1. The client in one screen

The `ocudu_e3` Python client is stdlib + `asn1tools` (plus `flatbuffers` +
`numpy` for the Class-C decoders). Point it at the schemas and go:

```bash
export PYTHONPATH=/path/to/ocudu/python
export OCUDU_E3_SCHEMA_DIR=/path/to/ocudu/schemas/e3
sudo modprobe sctp        # once per boot
```

```python
from ocudu_e3 import E3Client, SubscriptionConfig

with E3Client("127.0.0.1", 36423, dapp_name="my-first-dapp") as client:
    setup = client.setup()                      # E3AP handshake; assigns your dApp id
    client.subscribe(
        ["pusch.crc"],
        configs=[SubscriptionConfig("pusch.crc", drop_policy="dropOldest")],
    )
    for ind in client.indications(timeout=1.0):
        r = ind.record          # PuschCrcRecord: rnti, radio_slot, crc_ok
        print(f"rnti=0x{r.rnti:04x} sfn={r.radio_slot.sfn}.{r.radio_slot.slot} "
              f"crc={'ok' if r.crc_ok else 'KO'}  dropped={ind.dropped_objects}")
```

The essentials:

- **Records arrive by iteration** — `client.indications(...)` /
  `client.next_indication(...)`. There is no callback API.
- **`SubscriptionConfig` is your rate/filter control**: `sample_every_n`,
  `minimum_interval_ms` (None = the stream's default floor; 0 = every event,
  still clamped to the host's hard floor), `rnti_filter=[0x4601]`,
  `drop_policy` ∈ `dropNewest | dropOldest | coalesceLatest`.
- **`ind.dropped_objects` is truth**: producers never block the radio; if you
  fall behind, records are counted and dropped. Watch it.

Run it right now against the [loopback testbed](tutorial_docker_testbed.md)
(`docker compose -f ocudu-docker/docker-compose.yml up e3-agent`) — the agent serves synthetic `pusch.symbols`,
`pusch.crc`, `srs.channel`, and `gnb.metrics_json` on port 36423.

> **Scope honestly stated:** the loopback agent does *not* produce the
> `classc.*` snapshot streams below — those need a live cell running the
> Class-C reference dApps. Develop your loop against the four synthetic
> streams; switching streams later is a one-line change.

## 2. The Class-C snapshot streams (the good stuff)

On a live cell with the Class-C reference dApps active, two portable streams
carry host-published snapshots, decoded to **numpy** by `ocudu_e3.classc`:

| Stream | Interface | Decoder → payload |
|---|---|---|
| `classc.spectrum` | `0x00010001` | `decode_spectrum(md, tensor)` → `grid`: complex64 `(rx_ports, symbols, subcarriers)`, plus occupancy per (symbol, PRB), `prb_power()`, frequencies |
| `classc.srs_isac` | `0x00010002` | `decode_srs_isac(md, tensor)` → `h`: complex64 `(rx_ports, tx_ports, frequency_points)`, plus RSRP/EPRE, noise variance, TA, CFO |

Neither stream has a rate floor — a subscriber may request full rate. A
minimal live spectrum consumer:

```python
from ocudu_e3 import E3Client, SubscriptionConfig
from ocudu_e3.classc import decode_spectrum

with E3Client("127.0.0.1", 36423, dapp_name="my-sensor") as client:
    client.setup()
    client.subscribe(["classc.spectrum"], configs=[
        SubscriptionConfig("classc.spectrum", sample_every_n=4, drop_policy="dropOldest")])
    for ind in client.indications(timeout=1.0):
        snap = decode_spectrum(ind.record.metadata, ind.record.tensor)
        power = snap.prb_power()          # (symbols, nof_prb) — your algorithm here
        ...
```

Start from the SDK's worked, production-shaped version:
[`examples/e3_spectrum_sensing/emitter_detector.py`](https://gitlab.com/ocudu/work_groups/wg2_ai_ran/ocudu-dapp-sdk/-/blob/main/examples/e3_spectrum_sensing/README.md)
(thresholded emitter detection with HDF5 recording) and
[`examples/e3_isac_sensing/srs_isac_monitor.py`](https://gitlab.com/ocudu/work_groups/wg2_ai_ran/ocudu-dapp-sdk/-/blob/main/examples/e3_isac_sensing/README.md)
(range–Doppler sketch on `srs.channel` or `--source classc.srs_isac`). Both
are single plain processes — copy the file and its `_common.py`, rename, and
you have a vendor dApp.

## 3. Acting on what you see

The same association carries the full management surface — your dApp can
close the loop, not just watch:

```python
inv    = client.inventory()          # packages + instances
health = client.health()            # breakers, fallbacks, deadline misses
m      = client.metrics(instance_id)  # any dApp's KPIs (see the KPI tutorial)
```

For operations without a convenience wrapper, build the request with
`ocudu_e3.management` and send it through `client.management(...)`:

```python
import ocudu_e3.management as mgmt

# Bounded RAN control (dry-run first — always)
resp = client.management(mgmt.ran_control_request(
    "releaseUeToIdle", serving_pci=1, rnti=0x4601, dry_run=True))

# Typed config write: read schema + generation, then CAS-write
resp = client.management({
    "header": mgmt.header(None, mgmt.new_transaction(), dry_run=True),
    "operation": ("configSet", {
        "expectedConfigGeneration": generation.to_bytes(8, "big"),
        "boundary": "invocation",
        "entries": entries,          # typed entries from the config schema
    }),
})
```

Two scoping facts to design around:

- **Quiet-period reservations and interference maps are observe-only over
  E3.** Only an in-process/supervised Class-C worker can *publish* intents;
  the host reservation authority alone grants them. A standalone client reads
  `quiet-reservations` and influences the system via `configSet` on the
  sensing/scheduler dApps instead.
- **RAN control is capability-gated** — read `cell_info()` first; a host
  without a RAN-control authority answers `incompatible` (the loopback agent
  does).

> **Security, plainly:** an E3AP peer holds full management authority, and
> this release has no TLS/token auth on the port. Loopback (or an external
> secured boundary + `allowed_peers`) *is* the perimeter — treat your
> standalone dApp's host accordingly. See [security_model.md](security_model.md).

## 4. The C++ options (honest edition)

There is **no C++ E3AP-over-SCTP client library today**. Three real paths:

1. **Local-socket portable consumer (recommended, same host).** The SDK's
   `OCUDUDapp::portable_e3_consumer` library (`create/subscribe/poll`) speaks
   the local `SOCK_SEQPACKET` E3 socket; the reference binaries
   `ref_spectrum_e3_cpu` / `ref_srs_isac_e3_cpu` are complete worked examples
   (`--socket /run/ocudu/dapp-e3.sock --subscription <32-hex>`). Built with
   `-DOCUDU_DAPP_REF_EXTRAS=ON`.
2. **Embed the APER codec, bring your own SCTP loop.** The installed
   `dapp_sdk` component exports `OCUDU::DAppE3apCodec` (encode/decode,
   `decode_telemetry`, fragmentation) — everything except the transport,
   which is a few hundred lines of standard SCTP socket code.
3. **The E3DP FlatBuffers profile** (`sctp/38472`): a deliberately tiny
   subscribe+poll carrier for data-plane-only consumers. See
   [e3_wire.md](e3_wire.md) § E3 data profile — and regenerate bindings from
   `schemas/dapp/class-c-publication-v1.fbs`, never from a vendored copy.

If you just want data into C++ fast, path 1; if you need network reach, the
pragmatic answer in 2026 is: prototype in Python (§1–§3), port the algorithm,
keep the Python transport.

## 5. Where this sits in the Class-C spectrum

| Placement | Latency to data | Isolation | Can publish intents? |
|---|---|---|---|
| In-process Class-C module | lowest | in-process | **yes** (host API) |
| Supervised worker process | low | process + seccomp | **yes** (host API) |
| **Standalone E3 client (this tutorial)** | network | total | no — observe + manage |

Graduating from a standalone prototype to a supervised or in-process package
(when you need intents, lower latency, or lifecycle management) is exactly
what the [vendor-repo tutorial](https://gitlab.com/ocudu/work_groups/wg2_ai_ran/ocudu-dapp-sdk/-/blob/main/docs/tutorial_new_vendor_repo.md)
covers — your algorithm code carries over.

## Next

- [Telemetry deep-dive & data capture](tutorial_telemetry_capture.md) — every
  stream, floors, recording to HDF5, offline replay.
- [Exposing your own KPIs](tutorial_dapp_kpis.md) — once you're a package,
  publish metrics back.
- [MCP monitoring](https://gitlab.com/ocudu/work_groups/wg2_ai_ran/ocudu-dapp-sdk/-/blob/main/docs/tutorial_mcp_monitoring.md) —
  put an LLM agent on the same surface you just learned.
