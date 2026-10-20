<!-- SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc. -->
<!-- SPDX-License-Identifier: BSD-3-Clause-Clear -->

# OCUDU dApp platform capabilities

Native catalog packages can be admitted at runtime by UUID over E3, using
locally staged signed bundles and a monotonic trust policy. Admission never
loads or activates a module; see [runtime_catalog.md](runtime_catalog.md).

Guided path: the [step-by-step tutorial series](README.md#start-here) walks
every workflow this reference covers.

Working version **v1**. This is the definitive capability reference: what a
dApp can observe, compute, and change on a live OCUDU gNB, and what an E3
client can operate and measure. Each section links to the reference that
carries the wire, configuration, or lifecycle detail; the code and the
normative schemas (`include/ocudu/dapp/**`, `schemas/e3/`, `schemas/dapp/`)
win over any prose.

## What the platform is

The OCUDU dApp platform lets out-of-tree, signed native packages extend a live
gNB at three seams of the radio stack: **Class A** replaces resident inline L1
PUSCH stages (channel estimation, equalization, or the full receiver-to-LLR
chain) with the module's algorithm running inline in the slot — on the host's
CUDA stream for accelerator backends; **Class B** advises the MAC scheduler with bounded, validated intents
inside a deadline-bound decision boundary; **Class C** observes uplink
spectrum and SRS channel estimates open-loop — in-process or in a supervised
worker process — and feeds sensing results (interference maps, quiet-period
requests) back into host-owned caches the scheduler consumes. Packages are
built out of tree against the installed SDK, described by a signed
[manifest](extension_model.md), admitted from a catalog named only in DU YAML,
and loaded through an explicit lifecycle. The conventional PHY and scheduler
paths always remain present as fallback: a faulting, late, or unloaded dApp
returns the radio to stock behavior, never to silence.

Everything is controlled and observed over **E3**: an embedded management
agent speaks a project-defined ASN.1 APER service model
(`E3SM-OCUDU-DAPP-MGMT-V1`) over a credentialed local socket and over SCTP
(`E3AP-OCUDU-V2`, libe3 E3AP 1.0.0 with OCUDU extension additions, APER or Protobuf), and a telemetry service model (`OCUDU-TELEMETRY-SM`)
streams per-UE PHY records, gNB metrics, and Class-C snapshots to
out-of-process consumers. The same agent exposes inventory, lifecycle jobs,
typed configuration, model staging, metrics, incidents, health, quiet
reservations, cell information, and a bounded set of RAN controls. Carrier and
message layout: [e3_wire.md](e3_wire.md); operations:
[e3_management.md](e3_management.md); DU configuration:
[yaml_configuration.md](yaml_configuration.md); end-to-end walkthrough:
[quickstart.md](quickstart.md).

### Capability map

| Seam | Class | Timing character | Placement | Authority |
|------|-------|------------------|-----------|-----------|
| PUSCH CE / EQ / RX | A | Inline, enqueue-bound; completion tracked with soft wall budgets | In-process only | Replaces one L1 stage; conventional stage remains fallback |
| SRS channel estimator | A | Inline on the SRS executor; completion tracked with a soft wall | In-process only | Replaces the SRS estimator; the host estimator remains fallback |
| Scheduler policy | B | Complete boundary ~100 µs including validation and commit | In-process | Bounded intents under host allow flags; host stays the oracle |
| Spectrum / SRS-ISAC observation | C | Open-loop; producer never waits | Native or supervised process | Publishes results into host caches; quiet requests need host grant |
| Management and telemetry | E3 | Off the real-time path (management executors) | Local socket, SCTP | Full lifecycle/config/model/RAN control; push and pull observation |

## Class A — resident inline L1 (PUSCH)

Class A modules run **in-process only**, on the PHY's own workers, inside the
slot-processing path. Three v1 interfaces cover the PUSCH receive chain
(`include/ocudu/dapp/use_cases/v1/`):

| Interface | Id | Replaces |
|-----------|----|----------|
| Channel estimator (`channel_estimator.h`) | `0x00010003` | PUSCH channel and noise estimation |
| Equalizer (`equalizer.h`) | `0x00010004` | Channel estimation + MIMO equalization + post-EQ noise |
| Receiver (`receiver.h`) | `0x00010005` | The full chain through soft demapping (grid in, LLRs out) |

A fourth Class-A interface replaces the SRS channel estimator
(`srs_channel_estimator.h`, `0x00010007`): SRS pilots and reference sequences
in, the wideband channel matrix, noise, EPRE, RSRP and time alignment out. Its
contract, profile and metrics are in
[srs_channel_estimator.md](srs_channel_estimator.md); the rest of this section
describes the PUSCH interfaces.

All three consume the same inputs — the received resource grid plus typed
PUSCH and DM-RS metadata — and differ in how much of the chain they own and
what they return. Interface ids are stable v1 constants
(`OCUDU_DAPP_INTERFACE_CHANNEL_ESTIMATOR_V1` etc.).

### What the host supplies

Every invocation carries `ocudu_dapp_invocation_v1` (`abi/v1/execution.h`):
invocation id, cell, worker, radio time, a monotonic deadline, and — for
accelerator-resident interfaces (`OCUDU_DAPP_PUSCH_CAP_RESIDENT_ACCELERATOR_V1`)
— the host-owned `execution_stream` on which all module work must be enqueued.
Tensors are `ocudu_dapp_tensor_view_v1` device views; the host retains storage
ownership and completion signaling.

- **Resource grid** (`rx_grid`): the received allocation, device-resident for
  CUDA placements.
- **PUSCH metadata** (`ocudu_dapp_pusch_metadata_v1`, schema `0x00010101`):
  rx ports, layers, modulation, symbol/subcarrier span, noise-variance floor,
  a dense `data_re_mask` U8 `[symbol, subcarrier]` and a compact
  `data_re_indices` U32 `[data_re]` (accelerator-preferred), plus flags for
  transform precoding, data-RE-selection completeness, and DC overlap.
  **Reserved-slot scalars** extend the v1 layout without breaking it:
  `reserved[0]` optional C-RNTI, `reserved[1]` subcarrier spacing in Hz,
  `reserved[2]` full carrier grid width in subcarriers, `reserved[3]` the
  transmitter **DC subcarrier index + 1** (zero = unknown; the DM-RS pilot on
  that subcarrier is UE LO leakage and must not be used for estimation, and
  the data RE on it should be demapped as an erasure). See
  [api_compatibility.md](api_compatibility.md) for the reserved-slot policy.
- **DM-RS metadata** (`ocudu_dapp_dmrs_metadata_v1`, schema `0x00010102`):
  explicit pilot values `reference_symbols` CF32 `[layer, pilot]` and
  coordinates U16 `[pilot, 2]` (`{symbol, subcarrier}` relative to `rx_grid`),
  DM-RS type, and an optional `reserved[0]` OFDM-symbol bitmask so accelerator
  modules can learn the DM-RS symbol set without a D2H read of coordinates.

### What the module returns

- **Channel estimator**: caller-owned writable `channel_estimates` CF32
  `[rx_port, layer, symbol, subcarrier]` and `noise_variances` F32
  `[rx_port, layer]`.
- **Equalizer**: channel estimates plus `equalized_symbols` — full
  `[layer, symbol, subcarrier]` or the compact `[data_re, layer]` layout
  (`OCUDU_DAPP_LAYOUT_PUSCH_EQUALIZED_DATA_RE_LAYER_V1`) — and a
  shape-matched `post_equalization_noise` F32 tensor.
- **Receiver**: pre-descrambling soft bits in `llrs`. Host invocations use I8
  `[data_re * modulation_order]`; accelerator-resident invocations use F16
  `[data_re, layer, bit]` (`OCUDU_DAPP_LAYOUT_PUSCH_LLR_DATA_RE_LAYER_BIT_V1`).
  In both, bit is fastest-varying, positive values favor bit zero, magnitude
  is reliability, values must be finite, and I8 must not use −128. The `uci`
  typed-metadata slot is reserved for a future versioned UCI result schema and
  must be all-zero in interface v1; the optional `metrics` slot requests
  post-EQ noise (below); `confidence_q16` is an advisory Q16 confidence the v1
  host does not gate decoding on; `reserved0` may contain only known output
  flags.

Data-RE indices are flattened relative to `rx_grid` and ordered exactly as the
LLRs, so an allocation-local grid carries allocation-local indices
(`use_cases/v1/pusch_metadata.h`).

### Admission, budgets, fallback, breaker

Each interface declares an immutable `ocudu_dapp_pusch_profile_v1` (max PRBs,
rx ports, layers, modulation mask, DM-RS type mask, transform-precoding and
DC-overlap flags). The host checks the profile **before** invoking module
code; out-of-profile grants take the conventional path and increment the
per-reason `class_a.fallback.unsupported.*` counters
(`abi/v1/metric_catalog.h`).

Completion is measured with events around the module plus its required
host continuation, against **soft wall budgets**: channel estimator **800 µs**,
demodulator **2000 µs** (`include/ocudu/phy/upper/dapp/upper_phy_dapp_hook_provider.h`;
divergences and the reasoning live in [quickstart.md](quickstart.md) § Class-A
equalizer health). A late completion is an incident and a
`class_a.completion.deadline_misses` count, not a rewrite of the in-flight
result. Recoverable failures (deadline, invalid output, resource) open the
per-worker **circuit breaker** after **8 consecutive** observations
(`runtime/circuit_breaker.h`); an open breaker routes traffic conventionally
until an operator-authorized `breakerProbe` closes it. Every fallback keeps
the conventional CE/EQ/RX result authoritative — a Class-A module can degrade
throughput, not availability.

### Scheduler CSI from post-EQ noise

After a successful inline equalizer or receiver invocation the host derives
the UL SINR the scheduler consumes from the module's **post-equalization
noise** tensor (receiver modules opt in by filling the metrics tensor and
setting `OCUDU_DAPP_RECEIVER_METRICS_VALID_V1`): with measured signal power it
reports true SINR, otherwise `-10·log10(mean σ²)` under an assumed unit-power
constellation, plus an optional `OCUDU_DAPP_UL_SINR_CAL_DB` calibration offset
(`lib/phy/upper/dapp/pusch_dapp_demodulator.cpp`). The channel estimator has a
parallel resident extension: the host may request a scheduler-facing total
disturbance tensor through `output.reserved[0..3]`
(`OCUDU_DAPP_CHANNEL_ESTIMATOR_SCHED_NOISE_*_V1`). Packages that fill valid
post-EQ noise need no ABI change to drive host link adaptation.

## Class B — scheduler policy

Class B (`use_cases/v1/scheduler.h`, interface `0x00010006`) is a synchronous
in-process contract on the MAC scheduler thread: all candidate, intent, and
mask storage is host-visible, and the complete result — module time plus host
validation and commit — must return within the invocation deadline (~100 µs
complete boundary, a host default, not a YAML key). Timing and acceptance are
accounted in `class_b.policy.*` metrics.

### Candidate data

Each invocation presents up to `OCUDU_DAPP_SCHEDULER_MAX_CANDIDATES_V1` (8192)
pointer-free per-UE records (`ocudu_dapp_scheduler_candidate_v1`) for one
direction and slice: RNTI, pending bytes, DL head-of-line delay, exponentially
averaged served rate, the conventional priority, CQI, SINR, first-transmission
BLER over a 64-outcome window (with `bler_nof_observations`), **PHR** —
optionally grant-size-normalized (`PHR_NORMALIZED` flag) so thresholds compare
across report sizes — the host OLLA offset, the last grant outcome and MCS
(for package-local OLLA), expert MCS bounds, the MCS table id, transform
precoding, and the UL closed-loop target SINR. Validity is explicit per field
(`CANDIDATE_*_VALID` flags); `candidate_id` is dense and invocation-scoped.

### Intents and authority bounds

The module returns bounded intents (`ocudu_dapp_scheduler_intent_v1`):
priority, forbid, min/max PRBs, soft PRB preference, **hard PRB assignment**,
MCS (recommended and maximum), layers, TPC command, beam — plus an optional
per-PRB avoid mask. The host grants authority per invocation through
`allowed_output_flags`, a subset of `OCUDU_DAPP_SCHEDULER_ALLOW_KNOWN_MASK_V1`
driven by the `scheduler_allow_*` YAML keys and guardrails
(`include/ocudu/scheduler/dapp/scheduler_dapp_policy.h`,
[yaml_configuration.md](yaml_configuration.md)): avoid-mask width capped by
`max_scheduler_ul_prbs_avoided`, MCS clamped to
`scheduler_max_mcs_delta_from_host` of the host recommendation, hard
assignments admitted only when they are a subset of the supplied **free-VRB
mask**, do not overlap another hard assignment in the same invoke, and fit
`remaining_slice_prbs` (failed occupancy checks demote to a soft preference).
Unset capabilities are observe-only; `scheduler_evaluation_only` runs and
validates the module while discarding its output. The host validates every
flagged field and rejects invalid output as a counted policy fallback —
conventional ordering always remains the oracle.

### Interference context and quiet reservations

The input optionally carries a host-cached **interference map**
(`OCUDU_DAPP_CONTEXT_INTERFERENCE_MAP_V1`, per-PRB U8 confidence with a
provenance-stamped `ocudu_dapp_context_header_v1`) produced by a Class-C
spectrum package, closing the sense→avoid loop without any dApp-to-dApp call.
Quiet-period reservations granted by the host authority
(`include/ocudu/scheduler/dapp/quiet_reservation.h`) suppress scheduling on
the reserved PRB-slots at the admitted quietness level; the duty budget is
operator-bounded (`quiet_*` keys).

## Class C — asynchronous observation

Class C modules observe open-loop: the producer never waits on them, and
overload drops publications rather than blocking UL work. Two v1 source
interfaces (`use_cases/v1/spectrum.h`, `srs_isac.h`) and two placements:

| Placement | Route |
|-----------|-------|
| Native in-process (CPU or CUDA) | Direct grid/estimate views on a host worker; CUDA modules share the host stream |
| Supervised process (CPU) | memfd/SHM bounded SPSC ring (`ipc/publication_route.h`) + FlatBuffers `OCC3` metadata (`schemas/dapp/class-c-publication-v1.fbs`) + credentialed seqpacket control channel |
| Supervised process (CUDA) | Route-owned **CUDA export pool**: exact D2D snapshots into fixed IPC-exported slots with ready events (`lib/phy/upper/dapp/class_c_cuda_export_pool.h`, `process/v1/cuda_export.h`), imported by the worker over CUDA IPC |

Supervised workers are spawned, resource-limited, heartbeat-monitored, and
restarted by the process supervisor; see [security_model.md](security_model.md)
for the containment contract (no_new_privs, optional seccomp requirement,
peer-credential checks).

### Spectrum (`0x00010001`)

The host publishes each uplink slot's resource grid together with a
`scheduled_re_mask` and slot metadata (bandwidth, rx ports, symbols,
subcarriers, amplitude scale, source plan generation), and — via
`publish_plan` — the committed **advance UL resource plan**: up to 64 spans
per slot, each typed by `ocudu_dapp_ul_use_category_v1` (planned-empty, PUSCH,
PUCCH, SRS, PRACH/common, reference signal, reserved-quiet), with an optional
dense expected-use map `[symbol][PRB]`
(`OCUDU_DAPP_LAYOUT_EXPECTED_UL_USE_SYMBOL_PRB_U8_V1`). A detector therefore
knows which REs *should* be empty before it classifies energy in them.

### SRS-ISAC (`0x00010002`)

The host publishes SRS channel estimates (layout
`OCUDU_DAPP_LAYOUT_SRS_CHANNEL_RX_TX_FREQUENCY_CF32_V1`) with
`ocudu_dapp_srs_isac_metadata_v1`: SRS resource/config ids, comb geometry,
first subcarrier and SCS in Hz, frequency points, noise variance, RSRP/EPRE,
timing advance in ns, and the sounding UE's **C-RNTI** (`rnti`; 0 means the
host could not attribute the SRS, and consumers fall back to resource-only
stream keying). A bounded diagnostic CIR preview report
(`ocudu_dapp_srs_isac_cir_preview_v1`, ≤64 taps) exists for E2E validation.

### Result path back

Class-C results return through the host API `publish_prepared`
(`abi/v1/host_api.h`) as typed, host-copied objects: the per-PRB
**interference map** (`ocudu_dapp_spectrum_interference_map_v1`, stamped with
source grid sequence and plan generation) lands in the scheduler's context
cache, and **quiet-period intents** (`ocudu_dapp_quiet_period_intent_v1`) go
to the host reservation authority, which alone accepts, clamps, or rejects
them (status, reason, and duty accounting per
`use_cases/v1/coordination.h`); reservations are observable over management
`quietReservations`. The host stamps identity, time, and generation on every
result — a request is advice until the host returns a reservation.

Bounded Class-C publications are also snapshotted into the portable E3 store
(streams 6/7 below) for off-host consumers; copy costs are accounted per
reason in `class_c.copy.{lease,snapshot,export,e3_payload}.*` metrics. The
store itself is bounded (defaults: 32 subscriptions, 128 MiB retained,
4 MiB largest object, 64 KiB chunks; `portable_e3_data` in
[yaml_configuration.md](yaml_configuration.md)) and is a lower-rate
convenience — high-rate tensor transport stays on the SHM/CUDA-IPC process
routes, and hot tensors never travel over E3.

The single-authority rule applies throughout: there is one spectrum **result
authority** per cell; observers consume, and the host stamps every published
context with producer identity, generation, and validity window
(`ocudu_dapp_context_header_v1`) so consumers can reject stale or foreign
results.

## Telemetry streams

The upper PHY and the MAC publish ten streams (`OCUDU-TELEMETRY-SM`, RAN function 10;
ids in `python/ocudu_e3/schema.py`, store records in
`include/ocudu/dapp/e3/telemetry_types.h`). All are reachable by **push**
(E3AP subscriptions with fragmented indications) and by **pull**
(`dataSubscriptions`/`dataPoll` on any carrier, using the store interface id).
Full record layouts: [e3_wire.md](e3_wire.md).

| Id | Stream | Store interface | Content and notable record fields | Default per-RNTI floor |
|----|--------|-----------------|-----------------------------------|------------------------|
| 1 | `pusch.symbols` | `0x00020001` | Equalized data symbols of one grant plus link quality: SINR, EPRE, RSRP, EVM, TA, CFO, target code rate, PRB/symbol/layer/modulation shape. Packed store header is **v2** (152 B): v1 record + `symbol_flags` (crc_known/crc_ok/symbols_truncated/grant_valid) + the 92-byte grant block | 50 ms (`telemetry_pusch_symbols_min_interval_ms`) |
| 2 | `pusch.crc` | `0x00020002` | Transport-block CRC verdict per grant (RNTI, slot, `crc_ok`) | none (every TB) |
| 3 | `srs.channel` | `0x00020003` | Per-subcarrier least-squares SRS channel response per rx/tx port pair, TA phase ramp removed; RSRP/EPRE/noise/TA and the full SRS resource parameter set | 20 ms |
| 4 | `pusch.dmrs_channel` | `0x00020004` | PUSCH DM-RS channel estimate per rx port and layer with DM-RS parameters, SINR/EPRE/RSRP/noise/TA/CFO | 20 ms |
| 5 | `gnb.metrics_json` | `0x00020005` | The periodic gNB JSON metrics report, byte for byte (requires `metrics.enable_json`) | none (report period) |
| 6 | `classc.spectrum` | `0x00010001` | Class-C spectrum snapshot: `OCC3` FlatBuffers envelope + complete uplink resource grid tensor | producer rate |
| 7 | `classc.srs_isac` | `0x00010002` | Class-C SRS-ISAC snapshot: `OCC3` envelope + channel-estimate tensor | producer rate |
| 8 | `pusch.grid` | `0x00020006` | Raw **pre-equalization** REs of one grant, all rx ports, DM-RS included, plus `PuschGrantParameters` — everything needed to re-run CE/EQ/decode offline (HARQ/RV/NDI, scrambling ids, LDPC base graph, TBS/LBRM, MCS index+table, DM-RS mask/type/CDM, BWP and 275-bit CRB mask, DC position, UCI bit counts and beta offsets) | 100 ms; **20 ms default hard floor** |
| 9 | `srs.config` | `0x00020007` | SRS configuration of each UE context as the MAC hands it to the scheduler: the periodic TS 38.331 SRS-Resource (ports, comb, cyclic shift, symbols, C_SRS/B_SRS/b_hop, hopping, sequence id, **period and slot offset**), BWP, N_TA,offset and, without hopping, the TS 38.211 mapping of port 0 (start symbol, k0, sequence length, u, v). Events: created, reconfigured, applied, removed, plus a **snapshot** of every current context when a subscriber arrives. `configEpoch` advances on every creation, removal or change. Subscribe with `dropOldest`/`dropNewest`: `coalesceLatest` would merge events | none (every event) |
| 10 | `ta.command` | `0x00020008` | Every timing advance command in a committed DL result: the RAR's 12-bit T_A (by TC-RNTI) and each new TA Command MAC CE (6-bit T_A, TAG id), with the PDSCH slot. Same drop-policy advice as `srs.config` | none (every command) |

Rate control has three layers ([yaml_configuration.md](yaml_configuration.md)
§ Telemetry publication floors): the `telemetry_*_min_interval_ms` **default
per-RNTI floor** applies to subscribers that request no rate; a subscription
may request its own capture interval (`periodicity` or
`Telemetry-SubscriptionConfig.minimumIntervalMs`, 0 = every event) and the
producer runs at the fastest active request while the E3AP carrier **thins
each subscription back to its own interval**; the configured
`telemetry_*_hard_floor_ms` is an **aggregate per-stream bound** — the stream
as a whole never publishes faster, whatever the mix of UEs and subscribers.
Floors are checked before any capture work, so an idle stream costs one atomic
load per event. Per-subscription filters add cell, `sampleEveryN`, drop policy
(`coalesceLatest` default), and an RNTI list.

Capture-cost notes ([known_limitations.md](known_limitations.md)):
`pusch.grid` captures and the per-subcarrier `srs.channel` recompute read the
resource grid on PHY threads, and on accelerated grids each read stages the
grid to the host; the `pusch.symbols` equalized capture is bounded at 262144
samples (≈2 MiB per published grant). Equalized symbols are delivered in
codeword order (symbol-major, then RE, then layer, DM-RS symbols excluded);
`symbolsTruncated` marks captures that are not a complete codeword. The
combination of `pusch.symbols` (or `pusch.grid`) with its embedded
`PuschGrantParameters` is sufficient to decode the transport block offline —
`ocudu-pusch-replay` (`apps/tools/pusch_replay`) does exactly that with the
gNB's own receiver components and compares its CRC verdict with the recorded
one ([e3_wire.md](e3_wire.md) § Decoding recorded PUSCH bursts offline).

Telemetry attribution is single-cell today: every publisher stream reports the
publisher's `default_cell_id` (0), because upper-PHY diagnostics carry no cell
identity.

## Management capabilities

The management service model (`schemas/e3/e3sm-ocudu-dapp-mgmt-v1.asn1`,
dispatched by `lib/dapp/e3/management_service.cpp`) exposes one
`ManagementOperation` CHOICE. Every request carries a transaction id,
optional instance id, optional generation CAS values, and `dryRun`; mutations
return `accepted` + `jobId` and complete asynchronously. Client commands and
recipes: [e3_management.md](e3_management.md).

### The generation model

Three monotonic generations make concurrent control safe: the **inventory
generation** advances when the package set changes, the **instance
generation** advances on every committed lifecycle/config/model transition of
an instance, and each instance additionally exposes **config** and **model**
generations. A mutation names the generation it expects
(`expectedInstanceGeneration`, `expectedConfigGeneration`, …) and fails with
`conflict` or `transactionMismatch` when stale; the succeeding job's
`OperationResult` returns the new values to carry into the next step. The
enable walk is therefore a chain: `load` → job succeeds with `instanceId` and
generation → `prepare` → `arm` → `enterShadow` (warm, hooks not
authoritative) → `activate`, re-reading the generation at each step, with
`dryRun` available at every stage. Lifecycle states mirror
`ocudu_dapp_lifecycle_state_v1`: `discovered`, `loaded`, `prepared`, `armed`,
`shadow`, `active`, `draining`, `inactive`, `unloaded`, plus the fault states
`degraded`, `quarantinedResident`, and `error`
(`retryQuiescence` re-attempts an uncertain native unload).

### Operation catalog

| Operation | Capability |
|-----------|-----------|
| `inventory` | Package records (id, name, vendor, version, content hash, placements, backends, interfaces with PUSCH profiles, signature status/key/sequence, trust-policy epoch, SBOM hash), instance records (state, generation, placement, backend, last error/incident), and up to 64 live **E3 connections** (peer address, setup identity, subscriptions, control-request count) — external observers are reported as connections, never as loaded instances |
| `lifecycle` | `load` (package + placement + backend) and instance actions `prepare`, `arm`, `enterShadow`, `activate`, `deactivate`, `retryQuiescence`, `unload`, `breakerProbe`. Each instance mutation carries `expectedInstanceGeneration`; the **generation** advances on every committed transition, so concurrent controllers fail with `conflict` instead of clobbering. Outcomes land on the async **job** record |
| `configQuery` / `configSet` | Typed key/value configuration (`TypedValue`: bool, signed/unsigned 64-bit, real, string, bytes) with a self-describing schema (descriptors with kind, flags, default/min/max), a **config generation** CAS, and an application boundary: `immediate`, `invocation`, `slot`, or `restart` |
| `model` | Model lifecycle off the hot path: `stage`, `validate`, `warm`, `activate`, `rollback`, `retire`, with a model-generation CAS and a bounded inline artifact (≤ 1 MiB on the management path) |
| `metrics` | Per-instance metric schema (descriptors: name, unit, kind, counter/gauge flags) and snapshots; host-owned Class A/B/C counters use the canonical names in `abi/v1/metric_catalog.h` |
| `incidents` | Bounded ring of attributable failures — cause (errors, exceptions, invalid I/O, deadline, device, resource, process exit, heartbeat loss, lease expired, quiescence timeout, security violation, policy rejection), action taken (fallback, breaker opened, quarantined, restarted, …), phase, latency vs deadline — with filtered `list` and acknowledged `clearThrough` |
| `health` | Aggregate counters: exceptions, deadline misses, breaker opens, fallbacks, quarantines, ring drops, last incident id |
| `jobs` | `get` / `list` / `cancel` on async mutations; states `accepted`, `running`, `waitingBoundary`, `succeeded`, `failed`, `cancelled`, with progress and the final `OperationResult` (including new generations) |
| `quietReservations` | Host-authoritative quiet grants per cell: request/reservation ids, source instance, start/duration/repeat, PRB span, achieved level, status (accepted/clamped/rejected/cancelled/expired), and reason |
| `dataSubscriptions` / `dataPoll` | Leased pull subscriptions on any store interface (Class-C snapshots and telemetry streams) with sampling, queue depth, chunk size, and drop policy; polls return up to 8 chunks and renew the lease |
| `cellInfo` | Served-cell descriptors — PCI, PLMN/TAC, NR cell identity, band, DL/UL ARFCN and center frequency, bandwidth/PRBs, SCS, antenna counts, barred flag, TDD pattern, PRACH configuration, RU TX/RX gains, radio sampling rate (when the radio exposes one) — plus the `RanControlCapabilities` the host will honor |
| `puschRouting` | Per-UE routing of Class-A PUSCH grants: `query`, `replace` (default + entries), `update` (insert/overwrite/remove UEs) and `clear` a table of up to 512 (cell, C-RNTI) → `primary` / `conventional` / instance routes; answers with the table, the hook-slot bindings and a stale-route counter, supports `dryRun`. See [pusch_routing.md](pusch_routing.md) |
| `ranControl` | `releaseUeToIdle` and `handoverUe` (serving PCI + RNTI ≠ 0; handover requires a distinct valid target PCI), deferred onto the CU-CP control executor; `setTxGain` / `setRxGain` (RU port + finite gain in −30..90 dB), applied on the radio unit. Every action is capability-gated, validated before dispatch, and supports `dryRun` |

## Carriers and clients

Three carriers front the same `management_service` and portable data store
([e3_wire.md](e3_wire.md) is the wire reference):

| Carrier | Transport | Authentication |
|---------|-----------|----------------|
| `dapp.local_e3` | `AF_UNIX` `SOCK_SEQPACKET`, one APER PDU per datagram | **Credentialed**: `SO_PEERCRED` UID equality on a mode-0600 socket |
| `dapp.e3ap_sctp` | SCTP, `E3AP-OCUDU-V2` (libe3 E3AP 1.0.0 + OCUDU additions, APER or Protobuf; libe3 ZMQ link layer optional), default port 36423, 32 KiB fragments up to 16 MiB payloads | **None** — peers are admitted by literal address (`allowed_peers`; empty admits all). Loopback by default; a peer holds full management authority. See [security_model.md](security_model.md) |
| `dapp.e3dp_sctp` | Size-prefixed FlatBuffers `E3DP`, default port 38472 | **None** — same address-only admission; serves only `dataSubscriptions`/`dataPoll` for the standalone reference client |

Clients: `python/ocudu_e3` (`python3 -m ocudu_e3` — catalog, telemetry
subscriptions with fragment reassembly, management, cell info, RAN control),
`ocudu-dappctl` (C++) and `apps/tools/dappctl/ocudu_dappctl.py` (local socket
or `--sctp`, JSON output, `codec-self-test` golden vectors). The SDK ships
worked out-of-process consumers (`examples/e3_spectrum_sensing`,
`examples/e3_isac_sensing`) and an MCP server that exposes this management and
telemetry surface as assistant tools (`ocudu-dapp-sdk/docs/mcp_companion.md`).

## Related

- [quickstart.md](quickstart.md) — build → package → enable → verify
- [extension_model.md](extension_model.md) — packaging, manifest, SDK
- [security_model.md](security_model.md) — trust boundaries and containment
- [api_compatibility.md](api_compatibility.md) — ABI and schema stability
- [e3_system_design.md](e3_system_design.md) — architecture and data paths
