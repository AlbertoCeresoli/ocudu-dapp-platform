<!-- SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc. -->
<!-- SPDX-License-Identifier: BSD-3-Clause-Clear -->

# E3 and dApp Runtime — system design

Working version **v1**.  
**Doxygen page:** `e3_system_design`  
**Companions:** [e3_management.md](e3_management.md), [e3_wire.md](e3_wire.md),
[quickstart.md](quickstart.md), architecture PDF in the design tree.

## Goals

1. **Fault-contained trusted packages** near the modem: hash-bound packages,
   preflight, capability admission and conventional fallback. Native admission
   is not a malicious-code sandbox.
2. **Hard real-time isolation:** management and model staging never run on slot
   workers.  
3. **Backend-neutral ABI:** CPU and CUDA packages share C descriptors; host owns
   streams and continuation.  
4. **Composable sensing and control:** Class C publishes into host caches; Class
   B borrows; no peer dApp calls.  
5. **Operable lifecycle:** load → prepare → arm → shadow → activate → unload with
   generations, jobs, metrics, and incidents.

## Component diagram

```text
   Operator / automation          Out-of-process dApps, dashboards        Reference
   ocudu-dappctl / Python         python -m ocudu_e3, dappctl --sctp      SCTP client
            | APER, AF_UNIX                 | E3AP-OCUDU-V2, SCTP                | E3DP FlatBuffers, SCTP
            v                               v                                    v
+------------------------------------------------------------------------------------------+
| DU process                                                                                |
|  +---------------------+   +------------------------------------------------------------+ |
|  | dapp_service        |   | embedded E3 agent                                          | |
|  |  catalog + verifier |<->|  job dispatcher  management_service  (shared by carriers)  | |
|  |  instance_manager   |   |  local_management_server | e3ap_sctp_server | e3dp_server  | |
|  |  native_hook_reg.   |   |  portable_data_store  <-- telemetry_publisher (PHY sink)   | |
|  +----------+----------+   +------------------------------------------------------------+ |
|             |                          ^                        ^                          |
|   +---------+----------+---------------+---+          metrics JSON fanout (gNB app)       |
|   v                    v                   v                                              |
| Class A hooks      Class B hooks     Class C routes      upper-PHY diagnostics            |
| (PUSCH lanes)      (scheduler)       native + process    (PUSCH symbols/CRC, SRS)         |
|   |                    |                  |                                               |
|   +-------- conventional PHY / MAC fallback --------+                                     |
+------------------------------------------------------------------------------------------+
        |                                      |
        | SHM / CUDA-IPC                       | optional
        v                                      v
  supervised workers                    independent E3 consumers
  (ref_*d_*)                            (ref_*_e3_cpu, dashboards, agents)
```

## Timing classes

| Class | Name | Deadline character | Placement |
|-------|------|--------------------|-----------|
| **A** | Resident inline L1 | Enqueue-bound; completion tracked separately | In-process only |
| **B** | Deadline-bound decision | Complete boundary ~100 µs | In-process authority |
| **C** | Open-loop analytics | Best-effort; producer never waits | Native or supervised process |

Interface ids (v1 references): spectrum `0x00010001`, SRS–ISAC `0x00010002`,
CE `0x00010003`, EQ `0x00010004`, RX `0x00010005`, scheduler `0x00010006`,
SRS channel estimator `0x00010007`.

## Package admission path

```text
DU YAML package_manifests
  → read catalog-relative manifest
  → optional detached signature + trust policy (epoch, key, sequence floors)
  → verify artifact SHA-256 + SPDX digest binding
  → native preflight (disposable process): query + validate descriptor
  → admit to inventory (E3-visible)
  → load only via lifecycle (still no path from E3)
```

E3 **cannot** introduce new filesystem artifacts. It only references package
ids already admitted at process start (or re-read under operator restart
policy).

## Lifecycle state machine

```text
discovered/admitted
    → load → loaded
    → prepare → prepared
    → arm → armed
    → enterShadow → shadow     (hooks not authoritative)
    → activate → active        (hooks may affect radio path)
    → deactivate → inactive
    → unload → unloaded

error / quarantinedResident on fault paths
retryQuiescence when native unload is uncertain
```

Declarative `initial_instances` walk the same transitions synchronously before
E3 ingress, with reverse rollback on any failure.

## Class A data path

- Host binds concurrent PUSCH lanes (regular + UCI topology).  
- Capability and PUSCH profile checks run **before** module dispatch.  
- CUDA packages inherit `execution_stream`; outputs are caller-owned device
  tensors.  
- Fallback reasons are host-owned counters (`class_a.fallback.*`).  
- Completion deadline misses use CUDA events around module + required
  continuation; late completion is an incident, not a silent rewrite of the
  current result through conventional CE/EQ/RX.

### Equalizer-dApp CSI, DMRS mask, and soft wall budgets

See [quickstart.md](quickstart.md) § Class-A equalizer health under UL traffic
for the authoritative description of the equalizer-dApp CSI path (scheduler
SINR from mean post-EQ noise), the optional `dmrs.reserved[0]` OFDM-symbol
bitmask, and the default soft wall budgets (demodulator **2000 µs**, channel
estimator **800 µs**, with the resident-adapter factory's 100 µs CE parameter
default noted there). Recoverable deadline misses open a worker breaker only
after **8 consecutive** observations; soft deadline metrics are distinct from
circuit-breaker open and from 5G slot duration.

## Class B data path

- Host retains conventional policy as oracle.  
- dApp sees bounded candidates and optional interference context.  
- Validation + commit/rollback included in the 100 µs boundary.  
- Evaluation mode discards valid dApp decisions while measuring.

## Class C data path

| Path | Mechanism |
|------|-----------|
| Native CPU | Direct grid/H views |
| Native CUDA | Host stream + compact pinned results |
| Process CPU | memfd/SHM ring + FlatBuffers + seqpacket results |
| Process CUDA | D2D export pool + CUDA IPC import |
| Portable E3 | Host snapshot chunked over management APER (pull) or streamed as E3AP indications (push) |
| Telemetry | Upper-PHY diagnostics and gNB JSON copied once into the store; E3AP drains them into fragmented indications |

Single spectrum **result authority**; observers consume only. Quiet intents are
requests; host reservation manager enforces duty.

## Process supervisor

- Spawns package executable; no caller argv.  
- Verifies `no_new_privs` and, when required by deployment policy, Linux
  seccomp filter mode before transferring routes. Applies bounded RLIMIT
  file/AS/core policy.
- Heartbeats on control channel; withdraw routes before kill/restart.  
- Bounded restart attempts; incidents on exit/heartbeat loss.  
- Multi-interface route bundles: required/optional atomic binding.

## E3 agent placement

- Runs inside `dapp_service` on **management** executors; each SCTP carrier
  owns one non-real-time thread (`sctp_association_server`) that accepts
  associations, frames PDUs by SCTP message boundaries, runs procedures, and
  drains admitted publications every few milliseconds.  
- Job queue bounds: max transactions, queued jobs, retained jobs (config).  
- Never called from slot indication or PUSCH worker threads. Producers touch
  only the store: one metadata memcpy plus a bounded copy, no codec, no lock
  wait (`try_lock` → counted busy drop).  
- Portable data store is optional (auto-created by the SCTP carriers) and
  rate-limited; it does not replace Class-C process routes.  
- Telemetry streams are gated by `has_active(interface)` before any capture
  work, so an unsubscribed stream costs one atomic load per event.

### Thread-ownership invariant (must not regress)

E3 serialization runs entirely off the PHY threads. APER and FlatBuffers
encoding happen on the local-connector worker (`local_connector.cpp`) and the
SCTP association worker (`sctp_association_server.cpp`); the PHY thread's only
E3 work is the memcpy into the subscription queue. This split is deliberate:
any future change that moves codec work, blocking locks, or unbounded waits
onto a slot-indication or PUSCH worker thread is a regression, not a refactor.

### Telemetry real-time budget

- With no subscriber, a stream's entire capture cost is one relaxed/acquire
  atomic load (`should_collect`).
- With a subscriber, admission is three atomic loads plus a CAS-consumed
  timestamp check (per-RNTI floor, then the stream-wide hard floor, enforced
  first), so concurrent lanes cannot double-publish.
- Per publication the producer performs exactly **1 `write` + 2 `read`**
  syscalls (non-blocking eventfd notification and completion reclaim); a full
  ring or busy consumer is a counted drop, never a wait.
- Per-slot capture work scales linearly with the number of matching
  subscribers (`pusch.grid` copies the captured tensor once per subscription)
  and with `nof_rx × nof_tx` for `srs.channel`. Budget subscriber counts on
  production cells accordingly.

## Composition rules

1. dApps never call dApps.  
2. Host validates and stamps identity/time/generation on Class-C results.  
3. Scheduler interference context is host-cache with provenance.  
4. Multi-interface packages share one lifecycle gate; breakers stay per
   interface where designed.

## Trust and multi-tenancy posture

- Packages are **trusted native code** once admitted (especially CUDA IPC).  
- Signing and SBOM bind **what** was admitted; they do not sandbox GPU malice.  
- Prefer process CPU placement when stronger isolation is required.  
- Reference process workers install a seccomp deny-list for executable-image
  replacement, process injection and privileged kernel administration. This
  reduces attack surface but is not a complete filesystem/device sandbox.
- Landlock, namespaces, distinct service identities and cgroups remain
  deployment hardening layers rather than ABI v1 dependencies.

## Observability

| Plane | Content |
|-------|---------|
| Metrics | Module + host counters/gauges via E3 |
| Incidents | Bounded ring with cause/action/phase |
| Health | Aggregate process-level summary |
| Jobs | Async mutation progress |
| Copy accounting | lease / snapshot / export / e3_payload reasons |
| Telemetry | Per-UE PUSCH/SRS/DM-RS records and gNB JSON over E3AP subscriptions; per-stream published/rate-limited/dropped counters in the publisher and per-carrier association/indication counters in the SCTP servers |

See Doxygen `dapp_observability`.

## Versioning

- Package manifest schema **v1**, management ASN.1 **v1**, E3AP and telemetry
  ASN.1 **v1**, Class-C FlatBuffers **v1**, E3 data-profile FlatBuffers **v1**,
  public C ABI **v1**.  
- SPDX 2.3 is the external SBOM format id, not a local package version.  
- Breaking changes require a formal release and dual-run story; do not silently
  widen admitted profiles (e.g. layers > 2, transform precoding) without a new
  profile contract.

## Related implementation entry points

| Area | Location |
|------|----------|
| Service | `apps/services/dapp/` |
| Runtime | `include/ocudu/dapp/runtime/`, `lib/...` |
| E3 | `include/ocudu/dapp/e3/` |
| PHY hooks | `lib/phy/upper/dapp/` |
| Scheduler hooks | `lib/scheduler/dapp/` |
| Schemas | `schemas/e3/`, `schemas/dapp/` |
| Python client | `python/ocudu_e3/` |
| SDK references | `ocudu-dapp-sdk/reference/` |
