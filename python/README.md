<!-- SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc. -->
<!-- SPDX-License-Identifier: BSD-3-Clause-Clear -->

# `ocudu_e3` — Python client for the OCUDU E3 interface

A small, dependency-light client for the embedded OCUDU E3 agent:

- **E3AP over SCTP** (`E3AP-OCUDU-V1`): setup, telemetry subscriptions with
  transparent fragment reassembly, release, and the dApp management service
  model carried as control actions.
- **Telemetry decoding** (`OCUDU-TELEMETRY-SM`): typed records for PUSCH
  symbols, PUSCH CRC, SRS and DM-RS channel estimates, gNB JSON metrics, and
  Class-C snapshots, plus a decoder for the packed metadata used by the
  pull-based `dataPoll` path.
- **Management helpers**: request builders and a normalizer that turns APER
  responses into JSON-friendly Python objects (the same shape `ocudu_dappctl.py`
  prints).

Requirements: Python ≥ 3.10, `asn1tools`, a Linux kernel with SCTP
(`modprobe sctp`). `numpy` is optional and used for zero-copy complex views.

```bash
export OCUDU_E3_SCHEMA_DIR=/path/to/ocudu/schemas/e3   # or an installed share/ocudu/dapp/schema
export PYTHONPATH=/path/to/ocudu/python:$PYTHONPATH

python3 -m ocudu_e3 --port 36423 catalog
python3 -m ocudu_e3 --port 36423 subscribe pusch.symbols pusch.crc --rnti 0x4601 --duration 10
python3 -m ocudu_e3 --port 36423 inventory
```

```python
from ocudu_e3 import E3Client, SubscriptionConfig, PuschSymbolsRecord

with E3Client("127.0.0.1", 36423, dapp_name="my-observer") as client:
    setup = client.setup()                       # RAN functions, streams, bounds
    client.subscribe(["pusch.symbols", "gnb.metrics_json"],
                     configs=[SubscriptionConfig("pusch.symbols", rnti_filter=[0x4601])])
    for indication in client.indications(timeout=1.0):
        if isinstance(indication.record, PuschSymbolsRecord):
            iq = indication.record.samples()     # numpy complex64 view when numpy is installed
        else:
            report = indication.record.json()    # dict of the gNB metrics report
    health = client.health()                     # management request over the same association
```

## Layout

| Module | Role |
|--------|------|
| `ocudu_e3/schema.py` | Schema discovery (`OCUDU_E3_SCHEMA_DIR`, source tree, install prefixes) and the asn1tools APER codec |
| `ocudu_e3/sctp.py` | One-to-one SCTP association with `MSG_EOR` framing (standard library only) |
| `ocudu_e3/e3ap.py` | `E3Client`: procedures, fragment reassembly, indication iteration, management |
| `ocudu_e3/telemetry.py` | Record dataclasses, `decode_indication`, `decode_store_publication` |
| `ocudu_e3/management.py` | `ManagementRequest` builders and response normalization |
| `ocudu_e3/cli.py` | `python3 -m ocudu_e3` |
| `tests/test_e3ap_client.py` | Offline codec tests plus a live run against `e3ap_test_server` (`ctest -R dapp_e3ap_python`) |

## Behavior worth knowing

- One `E3Client` owns one association; drive it from one thread. Control
  procedures issued while iterating indications are matched by PDU id, so both
  can share the association.
- `next_indication()` returns complete objects only; fragments are reassembled
  per `(subscriptionId, sequence)` and length-checked against `payloadBytes`.
- Text fields are bounded UTF-8 `OCTET STRING`s on the wire; the client decodes
  them to `str` and encodes `str` inputs for you.
- A closed association raises `E3TransportError`; a refused procedure raises
  `E3ProtocolError` with the agent's cause and detail.
- The default drop policy is `coalesceLatest`: a slow consumer sees the freshest
  object and `dropped_objects` counts what it missed.

The wire contract is documented in `docs/dapp/e3_wire.md`; operations in
`docs/dapp/e3_management.md`.

## Class-C publications (`classc.spectrum`, `classc.srs_isac`)

`ocudu_e3.classc.decode_srs_isac` does the same for `classc.srs_isac` (channel estimate as
`(rx_ports, tx_ports, frequency_points)` complex64 with comb, first subcarrier, TA, CFO).
`ocudu_e3.classc.decode_spectrum(record.metadata, record.tensor)` turns a
`ClassCPortableRecord` into a `SpectrumSnapshot`: radio time, cell, carrier
reference frequency, bandwidth, per symbol × PRB expected uplink use and the
resource grid as a `(rx_ports, symbols, subcarriers)` complex64 array. The
FlatBuffers bindings under `ocudu_e3/_classc_fbs` are generated from
`schemas/dapp/class-c-publication-v1.fbs` with `flatc --python`; regenerate
them when the schema changes. The grid itself is never re-encoded on the wire
(APER `OCTET STRING`), so decoding is a header read plus one `frombuffer`.

