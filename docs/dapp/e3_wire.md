<!-- SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc. -->
<!-- SPDX-License-Identifier: BSD-3-Clause-Clear -->

# E3 wire guide

Guided path: the [step-by-step tutorial series](README.md#start-here) walks
every workflow this reference covers.

Working version **v1**.  
**Normative sources:** `schemas/e3/*.asn1` (ASN.1, encoded with **Aligned PER**),
`schemas/dapp/e3-data-profile-v1.fbs` (FlatBuffers, compatibility carrier only).  
**Doxygen page:** `e3_wire`

This document describes every carrier the embedded OCUDU E3 agent exposes,
the messages that travel on them, and the rules an independent client must
honor. Prefer the shipped clients (`ocudu-dappctl`, `apps/tools/dappctl/ocudu_dappctl.py`,
`python/ocudu_e3`) unless you have a strong reason to re-implement a codec.

## Carriers at a glance

| Carrier | Transport | Encoding | Direction | Purpose |
|---------|-----------|----------|-----------|---------|
| **Local management** (`dapp.local_e3`) | `AF_UNIX` `SOCK_SEQPACKET`, one datagram per PDU | APER `E3SM-OCUDU-DAPP-MGMT-V1` | request/response | Same-host, credentialed dApp lifecycle, configuration, metrics, incidents, pull-based portable data |
| **E3AP** (`dapp.e3ap_sctp`) | SCTP one-to-one association, one PDU per SCTP message (or libe3's ZMQ link layer) | `E3AP-OCUDU-V2` (libe3 E3AP 1.0.0 + OCUDU additions) in APER or Protobuf; service-model bodies opaque APER | procedures + **push** indications | Out-of-process dApps and observers: setup, telemetry subscriptions, fragmented indications, management as control actions |
| **E3 data profile** (`dapp.e3dp_sctp`) | SCTP one-to-one association, one message per request/response | Size-prefixed FlatBuffers `E3DP` | request/response (poll) | Compatibility endpoint for the standalone E3 SCTP reference client (`dataSubscriptions` / `dataPoll` only) |

All three carriers dispatch into the same `management_service` and the same
bounded `portable_data_store`; a subscription made on one carrier is visible to
the others through `dataSubscriptions list`.

The E3AP module, **E3AP-OCUDU-V2** (`schemas/e3/e3ap-ocudu-v2.asn1`), is the
Northeastern University libe3 E3AP 1.0.0 module (`messages/asn1/V1/e3ap-1.0.0.asn1`,
libe3 0.2.2, unchanged from 0.2.1) with OCUDU **extension additions**: everything before each of
libe3's extension markers is libe3's module, type for type, and OCUDU adds only
after them. A message without additions is therefore a libe3 message, bit for
bit, and libe3 reads OCUDU's messages of its own procedures (it skips the
additions). The additions carry what OCUDU needs beyond libe3: payloads up to
the protocol maximum of 16 MiB in 32 KiB fragments (the embedded server
advertises its own `maximumPayloadBytes` in the setup response — 4 MiB by
default, tied to the portable store's `maximum_object_bytes`), by-reference
delivery through shared memory, causes and detail text, and a control response
that carries a service-model outcome. The same message set is defined in
Protobuf (`schemas/e3/protobuf/e3ap-ocudu-v2.proto`: libe3's schema plus fields
numbered from 100). See [Interoperability notes](#interoperability-notes).

## Local management carrier

| Property | Value |
|----------|-------|
| Medium | `AF_UNIX` `SOCK_SEQPACKET` |
| Path | `dapp.local_e3.socket_path` (mode 0600, owned by the DU euid) |
| Framing | **None.** One APER `ManagementRequest` per datagram; one `ManagementResponse` per reply datagram. There is no length prefix. |
| Auth | `SO_PEERCRED` UID equality (`allowed_uid`, default: DU euid); clients verify the server UID symmetrically |
| Max message | 2 MiB (`aper_codec::maximum_message_size`); oversize datagrams are rejected |
| Concurrency | One request per connection, served serially |
| Timeouts | `request_timeout_ms` (server) / `--timeout-ms` (clients) |

There is **no** TLS profile. Do not expose the socket off-host.

### Module `E3SM-OCUDU-DAPP-MGMT-V1`

```text
ManagementRequest ::= SEQUENCE { header RequestHeader, operation ManagementOperation, ... }
ManagementResponse ::= SEQUENCE { transactionId, code, jobId?, result?, payload?, errorText?, ... }
```

`ManagementOperation` alternatives: `inventory`, `lifecycle`, `configQuery`,
`configSet`, `model`, `metrics`, `incidents`, `health`, `jobs`,
`quietReservations`, `dataSubscriptions`, `dataPoll`, `ranControl`,
`cellInfo`. `RanControlAction` enumerates `releaseUeToIdle`, `handoverUe`,
`setTxGain` and `setRxGain`: release/handover identify a UE by serving PCI and
RNTI (handover adds the target PCI) and are deferred onto the CU-CP control
executor; the gain actions address a radio-unit port and carry the gain in dB.
`cellInfo` returns the served-cell table and the RAN-control capabilities.
`ManagementPayload` alternatives: `inventory`, `config`, `metrics`,
`incidents`, `health`, `jobs`, `quietReservations`, `dataSubscriptions`,
`dataPublications`, `cellInfo`. Inventory is an `InventoryResult`: the LCM
snapshot plus up to 64 current downstream E3 associations and their bounded
telemetry subscription descriptions.

Primitive conventions for OCUDU service-model modules (E3AP V2's libe3-rooted
text fields instead use `UTF8String`; see interoperability notes):

| Type | Encoding notes |
|------|----------------|
| `Identifier` | `OCTET STRING (SIZE (16))`, UUID-like |
| `Hash256` | 32 raw bytes |
| Bounded text | `OCTET STRING`, UTF-8; malformed UTF-8 **must** be rejected. The service models avoid the generator-dependent `UTF8String` length encoding used by the E3AP root. |
| `Unsigned64` / `Signed64` | 8-byte network-order integers carried as `OCTET STRING (SIZE (8))` |
| `REAL` | Present only for measurements; non-finite values are omitted (fields are OPTIONAL) |
| `TypedValue` | CHOICE of bool, signed, unsigned, real, string, bytes |

Extension additions since the first v1 cut (all after `...` markers, so v1
decoders keep working): `DataSubscriptionRecord.rfCenterFrequencyHz` /
`cellFrequencyOffsetHz` (RF placement of the measurement window, reported when
the DU knows its uplink carrier).

Concurrency and idempotency: `transactionId` correlates request and response;
`expectedInstanceGeneration` / `expectedInventoryGeneration` implement
compare-and-swap (`conflict` / `transactionMismatch` on mismatch); `dryRun`
never mutates durable state; `accepted` + `jobId` means the final outcome is on
the job record.

Bounds (schema constants): `maxName` 128, `maxVersion` 64, `maxText` 512,
`maxItems` 1024, `maxValueBytes` 1 MiB, `maxDataChunks` 8,
`maxDataChunkBytes` 65536. Reject oversize fields; never truncate silently.

### Portable data (pull path)

`DataSubscriptionControl` creates, renews (identical create), lists, or removes
a leased subscription on one store interface; `DataPollQuery` returns up to
eight `DataPublicationChunk`s and renews the lease. Chunk 0 of each object
carries the metadata; later chunks carry `metadataSize = 0`. Clients reassemble
by `(routeEpoch, publicationSequence)` with contiguous `chunkIndex`. Interfaces:

| Interface id | Producer | Metadata | Tensor |
|--------------|----------|----------|--------|
| `0x00010001` Spectrum | Class-C spectrum route | FlatBuffers `class-c-publication-v1` (`OCC3`) | uplink resource grid |
| `0x00010002` SRS-ISAC | Class-C SRS route | `OCC3` | channel estimates |
| `0x00020001` … `0x00020006` telemetry | upper-PHY telemetry publisher | packed `telemetry_metadata_header` record (below) | binary32 I/Q pairs, one CRC byte, or UTF-8 JSON |

## E3AP carrier (`E3AP-OCUDU-V2`)

### Transport discipline

| Property | Value |
|----------|-------|
| Socket | `socket(AF_INET/AF_INET6, SOCK_STREAM, IPPROTO_SCTP)`, one-to-one association |
| Default port | **36423** (`dapp.e3ap_sctp.port`) |
| Framing | **SCTP message boundaries.** Send each `E3-PDU` with one `send`/`sendmsg`; receive with `recvmsg` accumulating partial deliveries until `MSG_EOR`. No length prefix. |
| Streams | Stream 0 carries procedures; stream 1 carries indications. Clients may send everything on stream 0. |
| PPID | `0x00E3A001` (project-private, informational) |
| Largest PDU | 32 KiB fragment + envelope (64 KiB bound, `e3ap_v2_codec::maximum_pdu_bytes`) |
| Idle close | Quiet associations without a valid subscription close after `idle_timeout_ms` (default 30 s). Valid subscriptions retain a quiet, fully drained SCTP association; stalled output and transport failure still close it. Setup must complete within `setup_timeout_ms` (default 5 s). |
| Admission | Optional `allowed_peers` list of literal addresses; `maximum_associations` (default 8) |
| Event notifications | Not subscribed; receivers skip `MSG_NOTIFICATION` deliveries defensively |
| Socket options | **Both peers** preset `SO_SNDBUF`/`SO_RCVBUF` (2 MiB or more) *before* the association exists (listener before `listen()`, client before `connect()`) and set `SCTP_DELAYED_SACK` with `sack_delay = 0`, `sack_freq = 1`. See "Socket options" below. |

Linux SCTP cannot deliver a single message larger than the socket send buffer,
so payloads are fragmented at the E3AP layer (below) and socket buffers are only
a performance knob.

### Socket options

Linux SCTP defaults are tuned for signalling, not for multi-megabyte
indications: the send buffer defaults to 16 KiB (`net.sctp.sctp_wmem`), the
receive window is fixed from `SO_RCVBUF` when the association is established,
and SACKs are delayed by 200 ms. With those defaults at most one chunk is in
flight and every chunk waits for the SACK timer: a ~1.7 MB object in 64 KiB
fragments took 5-16 s over loopback (measured before the move to 32 KiB fragments, Linux 6.11, DGX Spark):

| Server buffers preset | Client buffers preset | Delayed SACK off | ~1.7 MB object |
|---|---|---|---|
| no | no | no | 9-16 s |
| no | no | both | 8-16 s |
| yes | yes | no | ~1.3 ms |
| yes | yes | both | ~1.0 ms |
| yes | no | both | 1.4 ms-6 s (varies) |
| no | yes | both | ~9 s |

The agent therefore requests `socket_buffer_bytes` (default 2 MiB) on the
listener **before** `listen()` so accepted associations inherit it, and
disables delayed SACK on the listener and on every accepted socket. A client
must do the same on its socket **before** `connect()`; `python/ocudu_e3/sctp.py`
and the C++ test client do. Enlarging buffers after the association exists
does not help. The kernel clamps requests to `net.core.wmem_max` /
`net.core.rmem_max`; the agent reads the effective send buffer back and never
sends a fragment larger than it.

### Envelope

```text
E3-PDU ::= SEQUENCE {                    -- libe3's envelope, unchanged
  id        E3-MessageID,                -- request id; responses echo it as requestId
  timestamp E3-Timestamp OPTIONAL,       -- realtime ns
  msg       CHOICE {
    setupRequest          [0]  E3-SetupRequest,
    setupResponse         [1]  E3-SetupResponse,
    subscriptionRequest   [2]  E3-SubscriptionRequest,
    subscriptionDelete    [3]  E3-SubscriptionDelete,
    subscriptionResponse  [4]  E3-SubscriptionResponse,
    indicationMessage     [5]  E3-IndicationMessage,
    dAppControlAction     [6]  E3-DAppControlAction,
    dAppReport            [7]  E3-DAppReport,
    xAppControlAction     [8]  E3-XAppControlAction,
    releaseMessage        [9]  E3-ReleaseMessage,
    messageAck            [10] E3-MessageAck,
    ...,
    controlResponse       [11] E3-ControlResponse  -- OCUDU addition (OCUDU profile only)
  },
  ...
}
```

The agent stamps every outbound PDU with `CLOCK_REALTIME` at send time **for
peers whose setup request carried a timestamp**, and decodes a peer's timestamp
when present (`e3ap_pdu::timestamp_ns`). The Python client stamps by default
(`E3Client.stamp_timestamps`).

**Profiles.** A dApp that wants the OCUDU procedures sets the setup request's
`ocuduProfile` addition (value 1); the agent echoes it in the setup response.
Without it the association runs libe3's procedures (see
[the libe3 profile](#libe3-peers-and-the-libe3-profile)). `ocudu_e3` and the
C++ test clients always ask for it.

OCUDU-profile dApp IDs use 101..65535; libe3-profile IDs use 1..100. Live IDs
are never reused. Queued xApp controls capture their association identity, and
callers with a report/discovery context should use the expected-association
overload to reject stale IDs before enqueue. Subscription alias wrap skips all
live aliases.

### dApp reports and xApp control

`E3-DAppReport` is consumed: the agent hands each accepted report to the
handler installed with `e3ap_sctp_server::set_dapp_report_handler()` (the
E2SM-DAPP bridge attaches here), keeps the last sixteen in
`recent_reports()`, and counts `reports_received`. `E3-XAppControlAction` is
sent by the agent to a named dApp through
`e3ap_sctp_server::send_xapp_control_action()` on the procedure stream; a
dApp that sends one is still answered with `protocolError`. The Python client
delivers relayed actions to `E3Client.on_xapp_control` and sends reports with
`E3Client.report()`.

### By-reference bulk data (shared memory)

A subscription may ask for `deliveryMode byReference` (optionally with
`minimumByReferenceBytes`; default one fragment, 32 KiB). The agent then maps
one POSIX shared-memory region for the subscription, named
`<shared_region_name_prefix>-<pid>-<association>-<subscription>` (default prefix
`/ocudu-e3`, mode `shared_region_mode`, default 0640), with
`shared_region_rows` rows (default 4) of `maximum_payload_bytes` each, and
advertises it in the subscription response as `E3-SharedRegion`. For every
object whose tensor is at least the threshold, the agent streams the store's
chunks straight into the next row as they arrive (one memcpy per chunk on the
carrier worker, never on a PHY thread; the tensor is not assembled or copied
into the record on the way), sends the service-model record with its bulk
field **empty**, and attaches an `E3-TensorDescriptor` to the indication: memory domain, location (region name,
row index, byte offset), byte length, seqlock generation, and the element type
when known. Smaller objects stay in band. The region is unlinked when the
subscription is deleted, released, or the association closes.

Layout (`include/ocudu/dapp/e3/shared_region_layout.h`, little-endian):

```text
region = region header (64 B) | row[0] | row[1] | ...        row = row header (64 B) | tensor bytes
region header: magic "OCE3SHM1", version, header_bytes, row_header_bytes, nof_rows, row_bytes, write_index, generation
row header:    generation (odd while writing, even when complete), sequence, byte_length, telemetry_id, cell_id, realtime_ns, route_epoch
```

Measured in the unit test (`by_reference_delivery_is_faster_than_in_band_for_large_objects`,
1.36 MB PUSCH-symbol objects, single subscriber, 2 ms drain timer, DGX Spark):
in band 29.6 ms median per object (46 MB/s), by reference 2.2 ms (612 MB/s)
including the client's copy out of the row; the by-reference figure is bounded
by the drain timer, not by the copy.

Readers map the region read-only, copy the bytes named by the descriptor, and
re-read the row generation: a changed value means the row was overwritten
(torn read) and the copy must be discarded. `python/ocudu_e3/shared_region.py`
implements the reader; `E3Client.subscribe(..., by_reference=True)` fills each
record's bulk field from the region and counts torn reads.

Readers validate canonical names, no symlink traversal, backing-file size,
layout geometry, memory domain, row index/extent, and committed even generations.
The safe default is a checked copy. A borrowed `view()` is not an asynchronous
lease and must be released before close. The Python mapping admission limit is
256 MiB by default. The server separately caps aggregate SHM mappings at 128 MiB
(`maximum_shared_region_bytes`), reserves backing storage before page access,
and rejects colliding names without unlinking another owner's region.

Portable publication borrows per-slot pointers through try-only atomic gates:
no shared_ptr acquisition or queue reclamation occurs on the producer. Whole
objects beyond `maximum_publication_copy_bytes` (8 MiB per call by default,
including fanout metadata) are dropped and counted; admission order rotates.
This is a copy-work bound, not a hardware-independent microsecond guarantee.

The descriptor also defines an `acceleratorIpc` location (device ordinal,
allocation id, 64-byte memory handle, optional 64-byte ready-event handle,
byte offset) mirroring the Class-C CUDA export slot, so GPU-resident dApps can
receive device tensors through the same IE. The codec and clients carry it;
the agent publishes only `posixShared` today (the CUDA export route is the
Class-C process ABI).

Identifier ranges: `E3-DAppID`, RAN function, telemetry, control and
subscription identifiers are `INTEGER (1..65535)`; `E3-MessageID` is
`INTEGER (1..4294967295)`. `E3-ResponseCode` is `positive(0)` / `negative(1)`;
negative responses carry an optional `E3-Cause` and `detail`.

### Procedures

1. **Setup.** `E3-SetupRequest { e3apProtocolVersion "1.0.0", dAppName, dAppVersion (1..11 bytes), vendor, ocuduProfile?, dAppInstance? }`.
   The agent answers `E3-SetupResponse` with the assigned `dAppIdentifier`,
   its `ranIdentifier`, `ranInstance`, payload/fragment bounds, and the
   `ranFunctionList`. A second setup on the same association is refused with
   `setupAlreadyComplete`; every other procedure before setup fails with
   `setupRequired`.
   The version must match exactly. Only `ocuduProfile = 1` is supported;
   absent selects libe3, and other values are refused before assigning an ID.
   OCUDU clients require the profile grant and exact version in the response.
   The string alone does **not** identify the libe3 schema revision: this build
   remains pinned to 0.2.2 and its compiler profile (see interoperability notes).
2. **Subscription.** `E3-SubscriptionRequest { dAppIdentifier, ranFunctionIdentifier, telemetryIdentifierList, controlIdentifierList, subscriptionTime? (0..3600 s), periodicity? (0..1000), subscriptionData?, deliveryMode?, minimumByReferenceBytes?, subscriptionTimeLong? (0..86400 s), periodicityLong? (0..60000 ms) }`.
   Values beyond libe3's root ranges travel in the `...Long` additions with the
   root field absent.
   Only RAN function 10 (telemetry) accepts subscriptions; `controlIdentifierList`
   must be empty. `periodicity` is the per-RNTI capture interval requested
   for every telemetry in the request (`0` = every event; absent = the stream's
   advertised default floor); `subscriptionData` is an APER
   `Telemetry-SubscriptionData` with per-telemetry filters and an overriding
   `minimumIntervalMs` (below). The producer serves the fastest active per-RNTI
   request and thins each subscription back to its own interval, so recorders
   and dashboards coexist on one stream; the advertised `hardFloorIntervalUs`
   is enforced as an aggregate per-stream bound that the stream as a whole
   never publishes faster than. The agent
   answers `E3-SubscriptionResponse` with a `subscriptionId` and the admitted
   identifier lists. `E3-SubscriptionDelete` is acknowledged with `E3-MessageAck`.
3. **Indication.** `E3-IndicationMessage { dAppIdentifier, ranFunctionIdentifier, protocolData, subscriptionId, telemetryIdentifier, sequence, fragmentIndex, fragmentCount, payloadBytes }`.
   `protocolData` is one fragment of an APER `Telemetry-IndicationData`.
4. **Control.** `E3-DAppControlAction { dAppIdentifier, ranFunctionIdentifier 11, controlIdentifier 1, actionData, fragment fields }` carries one APER
   `ManagementRequest`. The agent acknowledges acceptance with `E3-MessageAck`
   (negative acks carry the cause) and then sends `E3-ControlResponse` whose
   `controlOutcome` is the APER `ManagementResponse`, fragmented when large.
5. **Release.** `E3-ReleaseMessage` removes every subscription of the dApp and
   closes the association after a positive `E3-MessageAck`.
6. `E3-DAppReport` (its `sequenceId` is mandatory, as in libe3) is accepted and acknowledged (no consumer yet);
   `E3-XAppControlAction` is RAN→dApp only and is refused with `protocolError`.
   A PDU that does not decode is answered with one negative ack and the
   association is closed, because the stream state can no longer be trusted.

### Fragmentation

Any payload larger than `maximumFragmentBytes` (32 KiB, libe3's root payload
bound, advertised in the setup response) is split into ordered fragments.
Receivers reassemble fragments that share the PDU `id` (control) or
`subscriptionId` + `sequence` (indication) in `fragmentIndex` order and verify
the reassembled length against `payloadBytes` before decoding. `fragmentIndex`
and `fragmentCount` are `OPTIONAL` additions meaning 0 and 1 when absent, so a
whole payload carries no extension group and is byte-identical to libe3's
message.

### Subscription timing and lifetime

The agent normalizes wire intervals to `std::chrono::microseconds` at admission.
Units are explicit and must not be inferred from the value:

| Setting | Unit and bound | Meaning |
|---------|----------------|---------|
| libe3-profile `periodicity` | microseconds, 0..1000 (pinned 0.2.2 grammar) | 1000 requests 1 ms, not 1 s |
| OCUDU profile-1 `periodicity` / `periodicityLong` | milliseconds, 0..1000 / 0..60000 | Existing OCUDU wire/client behavior is retained |
| OCUDU `minimumIntervalMs` / Python `periodicity_ms` | milliseconds | Per-stream `minimumIntervalMs` takes precedence over procedure periodicity |
| `subscriptionTime` / OCUDU `subscriptionTimeLong` | seconds, 0..3600 / 0..86400 | Positive means finite lifetime; zero or absent means until delete, release or disconnect |

These units apply equally to APER and Protobuf. No value is silently clamped or
rounded. Zero interval requests every event, absent uses the stream default,
and admission enforces `hardFloorIntervalUs` after unit conversion. Recovering
a portable-store lease preserves the admitted interval, cell/sample filters and
drop policy. The C++ wire member is now `periodicity` (previously misleadingly
named `periodicity_ms`); protocol handlers must use `requested_interval(profile)`.

A finite protocol lifetime starts at admission on a monotonic clock and is
independent of renewable internal store leases. Traffic, polling and lease
recovery do not extend it. Expiry on the carrier worker removes the subscription,
producer demand, registry entry and owned shared region, returning its admission
budget. The worker checks expiry before draining; already queued/in-flight
indications can still arrive afterwards. `subscriptions_expired` counts these
removals and is included in `subscriptions_removed`.

Expiry sends no unsolicited delete response. A later delete returns not-found
(negative `SubscriptionResponse` for libe3; negative `MessageAck` with `notFound`
for OCUDU). The Python client treats not-found for a locally held subscription
as completed cleanup, closing its SHM reader; `close()`/`release()` also close
all remaining readers.

Quiet valid SCTP subscriptions are exempt from the application idle timeout
only with an empty outbound queue. Normal SCTP peer-loss detection, stalled
output timeout, setup timeout, and association/subscription limits remain in
force. After the last subscription is deleted or expires, normal idle policy
applies; an already-idle association may close immediately. ZMQ's configured
idle policy is unchanged: PUB/SUB lacks per-peer SCTP liveness, so it does not
use this exemption. Protocol subscription expiry applies on both transports.

The proposed libe3 periodicity-range widening is a separate APER wire change.
This update does not change ASN.1 constraints, the libe3 pin or golden vectors.

### RAN functions

| Id | Name | Telemetry ids | Control ids | `ranFunctionData` |
|----|------|---------------|-------------|-------------------|
| 10 | `OCUDU-TELEMETRY-SM` | 1..8 (table below) | — | APER `Telemetry-RanFunctionData` (stream catalog, object bound, subscription bound) |
| 11 | `E3SM-OCUDU-DAPP-MGMT-V1` | — | 1 (`ManagementRequest`) | APER `Mgmt-RanFunctionData` (schema module name, byte bound) |

## Telemetry service model (`OCUDU-TELEMETRY-SM`)

| Telemetry id | Name | Store interface | Record | Default per-RNTI floor (subscribers may request faster, down to the configured hard floor) |
|--------------|------|-----------------|--------|-------------------------|
| 1 | `pusch.symbols` | `0x00020001` | `PuschSymbolsRecord`: RNTI, slot, PRB/symbol/layer counts, modulation, SINR/EPRE/RSRP/EVM/TA/CFO/code rate, equalized symbols | 50 ms (`telemetry_pusch_symbols_min_interval_ms`) |
| 2 | `pusch.crc` | `0x00020002` | `PuschCrcRecord`: RNTI, slot, CRC outcome | none |
| 3 | `srs.channel` | `0x00020003` | `SrsChannelRecord`: RNTI, slot, port pair, RSRP/EPRE/noise/TA, SRS resource fields, coefficients, and the received pilots and reference sequence (`receivedPilots`, `referenceSymbols`) | 20 ms |
| 4 | `pusch.dmrs_channel` | `0x00020004` | `PuschDmrsChannelRecord`: RNTI, slot, PRB span, DM-RS parameters, estimates | 20 ms |
| 5 | `gnb.metrics_json` | `0x00020005` | `GnbMetricsJsonRecord`: one gNB JSON metrics report, byte for byte | none (report period) |
| 6 | `classc.spectrum` | `0x00010001` | `ClassCPortableRecord`: `OCC3` metadata + complete tensor | producer rate |
| 7 | `classc.srs_isac` | `0x00010002` | `ClassCPortableRecord` | producer rate |
| 8 | `pusch.grid` | `0x00020006` | `PuschGridRecord`: RNTI, slot, PRB/port/symbol counts, `PuschGrantParameters`, raw pre-equalization REs (all Rx ports, DM-RS included) | 100 ms |

```text
Telemetry-IndicationData ::= SEQUENCE {
  telemetryId, cellId, sequence Unsigned64, routeEpoch Unsigned64,
  monotonicNs Unsigned64, droppedObjects Unsigned64,
  object Telemetry-Object,   -- CHOICE over the seven record types
  ...
}
Telemetry-SubscriptionConfig ::= SEQUENCE {
  telemetryId, cellId OPTIONAL, sampleEveryN DEFAULT 1,
  dropPolicy DEFAULT coalesceLatest, minimumIntervalMs OPTIONAL,
  rntiFilter SEQUENCE OF Rnti OPTIONAL, ...
}
```

Complex vectors (`Cf32Samples`) are little-endian IEEE-754 binary32 `(re, im)`
pairs. `droppedObjects` is the store's cumulative drop counter for the
subscription at publication time, so gaps are visible. `routeEpoch` identifies
the producer instance; it changes when the DU restarts.

The SRS record carries the **per-subcarrier** least-squares channel response
for one receive/transmit port pair: `nofSubcarriers` equals the SRS sequence
length (allocated subcarriers divided by the comb size) and `coefficients`
holds that many complex samples in comb order. The response is averaged over
the SRS symbols and the linear phase ramp from the measured time alignment is
removed, so the phase is flat for a line-of-sight channel; the wideband
coefficient the scheduler uses is the mean of these samples. The estimate is
recomputed from the resource grid by the telemetry decorator, so it is
identical over the host and CUDA estimators. Every publisher stream (PUSCH,
SRS and gNB-metrics alike) reports the publisher's `default_cell_id` (0
today): the upper-PHY producers do not carry a cell identity (single-cell
attribution).

### Class-C snapshots for out-of-process sensing

`classc.spectrum` and `classc.srs_isac` deliver the producer's Class-C
publication unchanged: the `OCC3` FlatBuffers envelope (radio time, cell,
carrier reference frequency, bandwidth, tensor descriptor, expected uplink use
per symbol × PRB) as `metadata` and the complete tensor as `tensor`. The tensor
is an APER `OCTET STRING` — length prefix plus raw bytes, fragmented above
32 KiB at the E3AP level — so bulk data is never re-encoded; on the reference
cell a Python subscriber sustains the full ~120 grids/s (~4 MB/s) with ~120 µs
per decode (`ocudu_e3.classc.decode_spectrum`). The SDK's
`examples/e3_spectrum_sensing/emitter_detector.py` is an out-of-process
emitter detector built on this stream, and `examples/e3_isac_sensing/srs_isac_monitor.py` runs
a range-Doppler pipeline on `srs.channel` or `classc.srs_isac` (`decode_srs_isac`).

### Decoding recorded PUSCH bursts offline

`PuschSymbolsRecord` carries, as extension additions, `grant`
(`PuschGrantParameters`), `crcOk` (the gNB's transport-block verdict) and
`symbolsTruncated`. The grant block holds everything an offline receiver
needs: HARQ id, `nId` (scrambling), TBS, RV, NDI, LDPC base graph, LBRM TBS,
MCS index and table (TS 38.214 table 5.1.3.1-1/2/3; optional, present when
the FAPI producer supplies them), transform-precoding flag and `nRsId`,
cyclic prefix, start symbol / symbol count, DM-RS symbol mask, type, CDM
groups without data, scrambling id and `nScid`, BWP start/size, the 275-bit
CRB mask, receive-port and layer counts, DC position, HARQ-ACK / CSI part 1
bit counts with `alphaScaling` and the three beta offsets. `equalizedSymbols`
are the equalized data REs of the whole grant in codeword order (symbol-major,
then RE, then layer; DM-RS symbols excluded, which is a complete codeword when
all CDM groups are reserved — otherwise `symbolsTruncated` is set).

`ocudu-pusch-replay` (`apps/tools/pusch_replay`) consumes one exported burst
(`export_pusch_burst.py` in the SDK examples writes JSON + cf32 from a
recorder HDF5) and runs soft demapping → descrambling → UL-SCH
de-multiplexing → rate de-matching → LDPC → TB CRC with the gNB's own
components, printing its verdict next to `crcOk`.

`pusch.grid` publishes the raw resource elements of the allocation before
equalization (receive-port-major, then OFDM symbol, then subcarrier; DM-RS
included) together with the same grant block, for re-running channel
estimation and equalization offline. It is the largest stream (PRB × 12 ×
symbols × Rx ports × 8 bytes per grant); the default/requested floors apply
per RNTI like the other streams, and its configured hard floor (20 ms default)
bounds the stream as a whole across all RNTIs. `puschGrid` is a root
alternative of `Telemetry-Object` so that PER decoders without fragmented
open-type support can read it.

### Packed store metadata (pull path and independent decoders)

Telemetry publications in the portable store carry this little-endian packed
header followed by a kind-specific record (`include/ocudu/dapp/e3/telemetry_types.h`,
mirrored by `python/ocudu_e3/telemetry.py`):

```text
header  : magic 0x3345544F ("OTE3") u32 | version u16 (1 or 2) | kind u16 | monotonic_ns u64   (16 bytes)
kind 1  : pusch_symbols  v1 56 bytes; v2 152 bytes (v1 layout + symbol_flags + grant block)
kind 2  : pusch_crc 26 bytes
kind 3  : srs_channel    v1 66 bytes; v2 70 bytes (v1 layout + pilot_length u16 + pilot_symbols u8 + reserved u8)
kind 4  : pusch_dmrs_channel 70 bytes     kind 5 : gnb_metrics_json  header only (16 bytes);
                                                   the tensor is the JSON text
kind 6  : pusch_grid 120 bytes (28-byte capture header + 92-byte grant block)
```

The publisher now emits `pusch.symbols` as **header version 2**
(`pusch_symbols_metadata_v2`, 152 bytes): the v1 record followed by
`symbol_flags` (u8: bit0 `crc_known`, bit1 `crc_ok`, bit2 `symbols_truncated`,
bit3 `grant_valid`), 3 reserved bytes, and the 92-byte
`pusch_grant_metadata_v1` grant block shared with `pusch.grid`. The grant
block's own `flags` byte is: bit0 `mcs_known`, bit1 `transform_precoding`,
bit2 `n_scid`, bit3 `dc_present`, bit4 `csi_part2_expected`.

`srs.channel` is emitted as **header version 2** (`srs_channel_metadata_v2`, 70
bytes): the v1 record followed by `pilot_length` and `pilot_symbols`. With a
non-zero `pilot_length` the tensor is the least-squares estimate
(`nof_subcarriers` CF32), then the received pilots (`pilot_symbols` ×
`pilot_length` CF32, symbol major) and the transmit port's reference sequence
(`pilot_length` CF32) — the SRS channel-estimator dApp input of the port pair
([srs_channel_estimator.md](srs_channel_estimator.md)). The record's
`mapping_initial_subcarrier` and `cyclic_shift_index` are the transmit port's
own. Version 2 applies only to these two kinds; every other kind keeps version 1.

Every per-UE record places the RNTI (u16) directly after the header; the agent
uses that to apply `rntiFilter` without a codec.

## E3 data-profile carrier (`E3DP`, FlatBuffers)

A compatibility endpoint for the standalone **E3 SCTP reference client**:

| Property | Value |
|----------|-------|
| Socket | SCTP one-to-one, default port **38472** (`dapp.e3dp_sctp.port`) |
| Framing | One size-prefixed FlatBuffer per SCTP message (`MSG_EOR`); the 4-byte prefix is validated against the delivered length but never used to delimit |
| Root / identifier | `Message` / `"E3DP"` (`schemas/dapp/e3-data-profile-v1.fbs`) |
| Operations | `DataSubscriptionControl` (create/renew/remove/list) and `DataPoll`; anything else answers `InvalidRequest` and keeps the association |
| Bounds | 2 MiB message; chunks per poll are additionally capped so one response fits the kernel send buffer |
| Records | `SubscriptionRecord` includes `rf_center_frequency_hz` / `cell_frequency_offset_hz`; queue depth is server-assigned and absent from the wire |

The store interfaces are the same as on the other carriers, so the reference
client's spectrum subscription (`0x00010001`) activates the Class-C spectrum
route exactly as a management-socket subscription would.

## Interoperability notes

The CI [interop report](../../tests/interop/libe3/README.md) compares against
libe3 release 0.2.2 and links each OCUDU-related item in
[wineslab/libe3#84](https://github.com/wineslab/libe3/issues/84), with exact
test evidence and remaining differences. Known differences are informational,
not test failures. SCTP runs when supported by the runner kernel; otherwise
the report explicitly marks it skipped and still qualifies the ZMQ path.

| Topic | OCUDU | libe3 (Northeastern) | NVIDIA cuSense |
|-------|-------|----------------------|----------------|
| Module | E3AP-OCUDU-V2 = libe3 E3AP 1.0.0 + extension additions | E3AP 1.0.0 (0.2.2) | JSON messages |
| Encoding | ASN.1 APER or Protobuf (same messages, same profiles) | ASN.1 APER, JSON, or Protobuf (run-time selected) | JSON |
| Transport | SCTP, message boundaries, streams; libe3's ZMQ link layer for libe3 peers | ZMQ (IPC/TCP), POSIX sockets with a 4-byte length prefix (three channels), or, with the patch in `tests/interop/libe3/patches`, one native SCTP association with message boundaries (`E3LinkLayer::SCTP_ASSOCIATION`) | ZMQ REQ/REP + PUB/SUB |
| Envelope | `E3-PDU { id, timestamp OPTIONAL, msg CHOICE }` (libe3's) | the same | `common_headers { type, message_id, timestamp }` |
| Identifier ranges | 1..65535 (ids), 1..2^32-1 (message and sequence id), lists up to 1000 | the same | strings / 32-bit |
| Procedure correlation | `E3-SequenceID`: mandatory on report and xApp control, optional on control | the same | - |
| Payload bound | 32 KiB per PDU; 16 MiB with fragmentation (addition) or by reference through a shared region | 32 KiB, no fragmentation | out-of-band shared memory |
| Control outcome | `E3-ControlResponse` (CHOICE addition) with service-model body; `E3-MessageAck` to libe3 peers | `E3-MessageAck` only | `e3_control_response` JSON |
| Extensibility | libe3's markers; OCUDU additions behind them | `...` on every SEQUENCE and the PDU CHOICE (`E3-ResponseCode` deliberately closed) | JSON additive |

### libe3 peers and the libe3 profile

The E3AP agent serves libe3 peers and OCUDU peers on the same port with the
same codecs. The encoding is fixed per association by its setup request (every
enabled format is tried; when more than one reads a setup, only the one that
re-encodes the decoded setup to the very same bytes is kept, and an ambiguous
setup is refused); the profile by the request's `ocuduProfile`. A setup without
it (what libe3 sends) runs the libe3 profile
(`e3ap_sctp_server_config::libe3_profile`, on by default): the agent writes no
OCUDU addition, so every byte it sends is what libe3's own encoder would send,
and it ignores additions on receipt. A libe3-profile peer then gets:

| Aspect | libe3 profile | OCUDU profile |
|--------|---------------|---------------|
| dApp identifier | 1..100 (any 1..65535 is valid on the wire), from a pool handed out from 100 downward and returned on close | association-derived, 101..65535 |
| Message identifiers | 1..2^32-1 (per session over SCTP; one server-wide counter over ZMQ, where every session shares the PUB stream and `E3-MessageAck` names no dApp) | 1..2^32-1 |
| Sequence identifiers | report: the dApp's `sequenceId` is handed to the report handler (`e3ap_dapp_report_event::sequence_identifier`); xApp control: the caller's E2SM-DAPP sequence-id, or one the agent assigns | the same |
| RAN functions | 10 and 11 | 10 and 11, with names and versions |
| Subscriptions | plain request (no `subscriptionData`) | full request |
| Indications | whole payloads of at most 32 KiB; larger objects and by-reference rows are dropped and counted in `libe3_indications_dropped` (PUSCH CRC, SRS, DM-RS, JSON metrics fit; PUSCH symbols and grids do not) | fragmented up to 16 MiB or by reference |
| Control | one `E3-MessageAck` carrying the outcome after the service model ran | ack on acceptance plus `E3-ControlResponse` with the body |
| Subscription delete | confirmed by `E3-SubscriptionResponse` echoing the subscription identifier (what libe3's dApp state machine waits for) | `E3-MessageAck` |
| Timestamp | stamped when the peer stamped its setup | the same |
| OCUDU additions | not sent, ignored on receipt | carried |

`e3ap_libe3_codec` is the APER codec in the libe3 profile
(`e3ap_v2_codec::encode(pdu, e3ap_profile::libe3)`), pinned against eleven
vectors from libe3's own `Asn1E3Encoder`. Metrics: `libe3_setups_completed`,
`libe3_indications_dropped`.

### Protobuf

libe3 built with `LIBE3_ENABLE_PROTOBUF` encodes E3AP with Protocol Buffers
(`messages/proto/V1/e3ap-1.0.0.proto`, vendored in `schemas/e3/interop/libe3`
and checked against the submodule at configure time). The agent accepts those
peers on the same SCTP port (`e3ap_sctp_server_config::protobuf_profile`, on by
default) and over ZMQ (`zmq_carrier_config::encoding = e3ap_encoding::protobuf`;
libe3 fixes the encoding in its configuration, never on the wire, so a ZMQ
carrier serves one encoding). On SCTP the encoding is detected with the module
from the setup request, under the same canonical re-encode rule: every enabled
format (APER, Protobuf) is tried and the one that reads a
setup request, and re-encodes it to the same bytes if several do, binds the
association; the profile comes from `ocudu_profile` as in APER.

`schemas/e3/protobuf/e3ap-ocudu-v2.proto` is E3AP-OCUDU-V2 in Protobuf, a
strict wire superset of libe3's schema: every libe3 message, field number, type and
enum value is unchanged, and OCUDU adds fields numbered 100 and up (causes,
detail text, instance identifiers, RAN function names, payload and fragment
bounds, subscription data and delivery mode, granted identifier lists, shared
regions, indication bookkeeping, fragmentation, by-reference tensor
descriptors) plus one `E3Pdu.msg` alternative, `ControlResponse = 100`. A libe3
parser skips the additions as unknown fields, so an OCUDU-profile PDU of any
libe3 message type still reads as the libe3 message; only a `ControlResponse`
would reach libe3 as an empty oneof, and it is never sent to a libe3 peer.

| Protobuf peer | How it is recognised | Procedures | On the wire |
|---------------|----------------------|------------|-------------|
| libe3 (`EncodingFormat::PROTOBUF`) | `SetupRequest` without `ocudu_profile` | the libe3 dialect above (identifiers 1..100, whole payloads up to 32 KiB, `MessageAck` for control) | byte-identical to libe3's `ProtobufE3Encoder`; no field >= 100 is sent |
| OCUDU profile | `SetupRequest.ocudu_profile = 1` (echoed in the response) | the OCUDU procedures (fragmentation, by-reference delivery, `ControlResponse`) | the superset schema |

The codec (`e3ap_protobuf_codec`) is hand-written, so the gNB links no
libprotobuf: canonical libprotobuf output on encode (ascending field numbers,
proto3 defaults omitted, `optional` fields written when set, packed repeated
scalars, one size pass and one copy of each payload), any valid proto3
serialisation on decode (any field order, packed or unpacked, repeated message
fields merged, unknown fields skipped). Each profile enforces the bounds of the
module it mirrors, so a libe3 association behaves the same over APER and
Protobuf. proto3 cannot mark an implicit zero as present, so an identifier,
message identifier or sequence identifier of 0 means "absent" (all are 1..N on
both modules). Unlike libe3, which reads an unknown `ResponseCode` as
`POSITIVE`, the codec refuses it. The ZMQ carrier routes Protobuf PDUs with an
allocation-free read of the identifiers (`e3ap_protobuf_codec::peek`) instead
of a decode.

Metrics: `protobuf_setups_completed` (either profile; libe3-profile setups also
count in `libe3_setups_completed`).

### Python peers and libe3's text fields

libe3's grammar (and so V2's root) types names and versions as
`UTF8String (SIZE (..))`. asn1c, which generates both libe3 and OCUDU's codec
from one pinned commit, writes their length as a constrained bit field; a strict
X.691 encoder (plain asn1tools) writes an octet-aligned length, so it cannot
read or produce libe3's `E3-SetupRequest`/`E3-SetupResponse`. `ocudu_e3`
compiles V2 (and `ocudu_e3.libe3` the vendored libe3 grammar) with those fields
read the way asn1c encodes them, so Python peers produce libe3's bytes exactly:
`ocudu_e3`'s own codec reproduces all eleven libe3 vectors, and libe3 decodes
what it emits (both directions are tests). Text crosses `ocudu_e3`'s E3AP API as
UTF-8 bytes.

### libe3 over ZMQ

libe3 dApps that use libe3's own ZMQ link layer (`E3LinkLayer::ZMQ`, unpatched)
are served when `e3ap_sctp_server_config::zmq` is set: the agent binds libe3's
three sockets (REP for setup, PUB for everything it sends, SUB for everything
dApps send; `ipc://` or `tcp://`, libe3's defaults 9990/9991/9999 or
`/tmp/dapps/{setup,e3_socket,dapp_socket}`) and drives the same E3AP procedure
code through one virtual association per dApp session (`zmq_association_server`).
It speaks the libe3 module in the configured encoding (APER or Protobuf; a
Protobuf dApp may also select the OCUDU profile). Inbound PDUs are routed by the dApp identifier
they carry (an `E3-MessageAck`, which carries none, by the message identifier it
answers). Compiled in only with libzmq and instantiated only when configured;
the encoder's buffers are handed to ZMQ without a copy and nothing is decoded
twice while a single dApp is attached. It exists for libe3 interoperability
(`dapp_e3ap_libe3.e2e_zmq`); the gNB's E3 endpoint stays SCTP.

### Automated interoperability tests

| Test | Needs | Checks |
|------|-------|--------|
| `dapp_e3ap_libe3_codec.*`, `dapp_e3ap_v2_codec.*` (gtest, always built) | nothing | the eleven vectors produced by **libe3's own encoder** (pinned in `python/tests/fixtures/libe3_vectors.json` / `e3ap_libe3_vectors.inc`) decode and re-encode byte for byte through the V2 codec's libe3 profile; every OCUDU addition round-trips; a libe3-profile decoder reads every OCUDU-profile PDU as the libe3 message; on 24000 random PDUs APER and Protobuf accept and refuse the same values and translate into each other byte for byte, in both profiles |
| `dapp_e3ap_protobuf_codec.*` (gtest, always built) | nothing | the eleven vectors produced by **libe3's own `ProtobufE3Encoder`** (pinned beside the APER ones, `e3ap_libe3_protobuf_vectors.inc`) decode and re-encode byte for byte; every procedure round-trips under both profiles, every OCUDU field included; libe3-profile bounds; proto3 decoding leniency (order, packing, merging, unknown fields, wrong wire types); malformed input refused; 20000 mutated inputs never crash and whatever is accepted re-encodes to a fixed point |
| `dapp_e3ap_protobuf_conformance.*` (gtest, needs libprotobuf-dev and protoc) | libprotobuf | the OCUDU schema is a wire superset of libe3's (all 58 libe3 fields unchanged, additions >= 100); 12000 random PDUs of every procedure under both profiles serialise exactly as libprotobuf does; libe3's generated parser reads every OCUDU-profile PDU, and discarding the unknown fields leaves exactly the libe3-profile bytes; field order does not matter on decode |
| `dapp_e3ap_sctp_server.serves_libe3_module_peers_*`, `serves_every_encoding_and_profile_on_one_port`, `drops_indications_the_libe3_module_cannot_carry` (gtest) | nothing | a libe3-profile peer runs setup, subscription, indication, control, report, delete and release beside an OCUDU-profile peer on one port; APER and Protobuf peers of both profiles share one port, each served in its own procedures; oversize objects are dropped and counted |
| `dapp_e3ap_libe3.interop` (`python/tests/test_libe3_interop.py`) | asn1tools | every libe3 type has the identical root in E3AP-OCUDU-V2 and OCUDU adds only behind libe3's markers; `ocudu_e3`'s codec reproduces every libe3 vector; libe3 reads OCUDU-profile PDUs as the libe3 message |
| `dapp_e3ap_libe3.e2e_sctp`, `.e2e_zmq` (`python/tests/test_libe3_e2e.py`, `-DOCUDU_DAPP_LIBE3_INTEROP=ON`) | a libe3 checkout, SCTP headers, libzmq, libprotobuf | **the real upstream libe3** (dApp role) against `e3ap_test_server` over `SCTP_ASSOCIATION`, ZMQ `ipc://` and ZMQ `tcp://`, each in APER and in Protobuf: full session; two dApps in a row; two concurrent dApps on the ZMQ sockets; the encoder tool still matches the pinned APER and Protobuf vectors, and libe3 decodes both to the same fields; libe3 decodes what Python emits |

Remaining alignment includes stock-toolchain APER Setup compatibility (the
tests above use the patched/pinned generator), coordinated adoption of future
libe3 schema revisions, and proposing OCUDU additions (fragmentation,
by-reference delivery, causes, control response) upstream. Structural schema
alignment and passing the patched-peer tests are not a stock-libe3 compatibility
claim.

The reference client tarball ships a **trimmed** `class-c-publication-v1.fbs`
whose `ElementType` and `GridMaskGranularity` enumerators are numbered
differently from the normative OCUDU schema (`ComplexInt16 = 1` versus `8`,
`SymbolPrb = 1` versus `2`). OCUDU publishes the normative values; a client
built from the trimmed schema misreads the element type. Regenerate the client
from `schemas/dapp/class-c-publication-v1.fbs` before OTA interop.

## Security posture

The SCTP carriers bind loopback by default and admit every peer that reaches
the bound address; `allowed_peers` narrows that to literal addresses. There is
no TLS/DTLS or token authentication in v1: off-host exposure requires an
external boundary (IPsec, VPN, network policy). Management control actions on
E3AP carry the full authority of the management service model, so treat the
E3AP port exactly like the local management socket. Telemetry streams expose
per-UE I/Q and channel estimates; data minimization (subscribe to the derived
streams you need, apply RNTI filters) is a client responsibility.

## Independent client checklist

1. Compile `e3ap-ocudu-v2.asn1` and the three service-model modules with an APER stack (asn1c; with asn1tools, read SIZE-constrained `UTF8String` as `OCTET STRING`, see `ocudu_e3.libe3.asn1c_equivalent_grammar`), or use `schemas/e3/protobuf/e3ap-ocudu-v2.proto`.  
2. Set `ocuduProfile` in the setup request for the OCUDU procedures; carry 64-bit counters as 8-byte network-order octets.  
3. Frame by SCTP message boundaries (`MSG_EOR`); never rely on a length prefix.  
4. Complete setup before any other procedure; echo the assigned `dAppIdentifier`.  
5. Reassemble fragments by `(subscriptionId, sequence)` and PDU `id`; verify `payloadBytes`.  
6. Treat `droppedObjects` and `routeEpoch` changes as visible discontinuities, not errors.  
7. Golden vectors: `python/tests/fixtures/libe3_vectors.json` (libe3's own encoders, APER and Protobuf); a V2 message without additions must reproduce them.

## Related

- [e3_management.md](e3_management.md) — operations, clients, telemetry subscriptions  
- [e3_system_design.md](e3_system_design.md) — agent placement and data path  
- [yaml_configuration.md](yaml_configuration.md) — `local_e3`, `e3ap_sctp`, `e3dp_sctp`, `telemetry_*`  
- Headers: `include/ocudu/dapp/e3/*`; Python: `python/ocudu_e3/`
