<!-- SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc. -->
<!-- SPDX-License-Identifier: BSD-3-Clause-Clear -->

# Glossary

One or two lines per term, with the authoritative doc linked. Terms
capitalized as they appear in the docs and code.

**allow-flags** — Operator YAML switches (`scheduler_allow_*`) that grant a
Class-B dApp authority over one intent class each; everything defaults off
(observe-only). [tutorial_class_b_scheduler.md](tutorial_class_b_scheduler.md)

**APER** — Aligned Packed Encoding Rules; the ASN.1 encoding used by E3AP and
the management service model. [e3_wire.md](e3_wire.md)

**breaker (circuit breaker)** — Per-instance failure latch: 8 consecutive
faults/deadline misses open it, routing traffic conventionally until an
operator `breaker-probe`. An open breaker does **not** change the lifecycle
state — check `health`, not `inventory`.
[tutorial_troubleshooting.md](tutorial_troubleshooting.md)

**bypass** — A dApp declining an invocation (`OCUDU_DAPP_BYPASS_V1`): the
host falls back to its conventional processing. Unhandled-shape-becomes-
bypass (never a crash) is the core safety idea.
[extension_model.md](extension_model.md)

**catalog** — The operator-owned directory of admitted package bundles,
named only in YAML; E3 can select from it but never supply a path.
[tutorial_install_and_yaml.md](tutorial_install_and_yaml.md)

**Class A / B / C** — The three use-case contracts: **A** resident inline L1
processing (CE/EQ/receiver, in the slot); **B** bounded (~100 µs) scheduler
advice; **C** asynchronous observation/advisory (spectrum, SRS/ISAC).
[capabilities.md](capabilities.md)

**dApp** — A distributed application extending the gNB through one or more
of the three contracts — in-process, supervised-process, or standalone over
E3.

**drop policy** — Per-subscription overflow behavior: `dropNewest`,
`dropOldest`, or `coalesceLatest`. Producers never block the radio; the
`dropped` counter is cumulative truth.
[tutorial_telemetry_capture.md](tutorial_telemetry_capture.md)

**dry-run** — `dry_run=true` on a management request: full validation, no
mutation, `dryRunSuccess` outcome. The default for every mutating MCP tool.
[e3_management.md](e3_management.md)

**E3** — The umbrella name for the gNB's dApp management + telemetry plane:
the local socket, E3AP, and E3DP carriers over one service model.
[e3_system_design.md](e3_system_design.md)

**E3AP** — The SCTP carrier (default `127.0.0.1:36423`): APER-encoded setup,
push telemetry subscriptions, and management control actions.
[e3_wire.md](e3_wire.md)

**E3DP** — The minimal FlatBuffers data-profile carrier (default port
38472): subscription + poll only, for consumers that don't want an APER
stack. [e3_wire.md](e3_wire.md)

**evaluation mode** — `scheduler_evaluation_only: true`: the Class-B dApp is
invoked and validated on every decision but its output is discarded —
measurement without authority. Not the same as *shadow* (a lifecycle state).
[tutorial_class_b_scheduler.md](tutorial_class_b_scheduler.md)

**generation** — A monotonic counter used as a compare-and-swap guard:
instances, configs, and models each have one. A `conflict` means your view
is stale — re-read and retry. [e3_management.md](e3_management.md)

**hard floor** — `telemetry_<stream>_hard_floor_ms`: a per-stream bound no
subscriber may go below; defaults non-zero, full rate is an explicit
operator opt-in (`0`). Distinct from `*_min_interval_ms`, the default rate
for subscribers that didn't ask.
[tutorial_telemetry_capture.md](tutorial_telemetry_capture.md)

**incident** — An attributed fault/event record (exception, deadline miss,
breaker transition, lifecycle change) in the bounded incident store — the
audit trail. `incidents list` / `incidents clear --through`.
[e3_management.md](e3_management.md)

**intent** — A Class-B dApp's *advice* to the scheduler (priority, PRB
limits, MCS, TPC, layers, avoid-mask). The host validates against
allow-flags and commits or discards atomically.
[tutorial_class_b_scheduler.md](tutorial_class_b_scheduler.md)

**interface id** — The 32-bit contract identifier a dApp implements:
use-case contracts `0x0001xxxx` (spectrum `…0001`, SRS-ISAC `…0002`, CE
`…0003`, EQ `…0004`, receiver `…0005`, scheduler `…0006`); telemetry-stream
interfaces `0x0002xxxx`. [proposing_extensions.md](proposing_extensions.md)

**lease** — Every data-store subscription is leased; polling or renewal
keeps it alive, abandonment lets it lapse. Nothing queues forever for a dead
consumer. [e3_wire.md](e3_wire.md)

**lifecycle** — The instance state machine: `DISCOVERED → LOADED → PREPARED
→ ARMED → SHADOW → ACTIVE → DRAINING → INACTIVE → UNLOADED`, every
transition generation-guarded and driven over E3.
[tutorial_dappctl_operations.md](tutorial_dappctl_operations.md)

**manifest** — The hash-bound JSON record naming a package's artifact,
identity, interfaces, and provenance; what actually gets signed.
[tutorial_signing_and_trust.md](tutorial_signing_and_trust.md)

**model generation / model slot** — The CAS counter and staged/active/
previous storage behind `stage → validate → warm → activate → rollback →
retire`. Activation is an atomic slot publication.
[tutorial_model_operations.md](tutorial_model_operations.md)

**package sequence** — The monotonic release number a vendor signs into each
bundle; trust policies set per-package floors, and runtime refresh refuses
downgrades. [tutorial_signing_and_trust.md](tutorial_signing_and_trust.md)

**preflight** — A disposable helper process that queries an untrusted module
descriptor before the gNB ever loads it in-process.
[security_model.md](security_model.md)

**quiescence** — Proof that no worker is still inside a module; required
before unload. A module that can't prove it is quarantined, not freed under
a live kernel. [tutorial_troubleshooting.md](tutorial_troubleshooting.md)

**quiet reservation** — A host-granted, budget-clamped UL quiet window
requested by a sensing dApp (via the in-process host API); applied to
scheduling even if the requester dies. Observe with
`dappctl quiet-reservations`.
[tutorial_class_b_scheduler.md](tutorial_class_b_scheduler.md)

**resident (Class-A)** — A dApp whose kernels run inside the L1 slot
pipeline on the host's CUDA stream; the highest-trust, highest-performance
placement. [capabilities.md](capabilities.md)

**route epoch** — A counter that advances when a stream's producer routing
changes; treat changes as visible discontinuities in a capture, not errors.
[e3_wire.md](e3_wire.md)

**sample_every_n** — Subscription decimation; skipped records are counted as
*sampled*, not dropped.
[tutorial_telemetry_capture.md](tutorial_telemetry_capture.md)

**SBOM** — The SPDX 2.3 software bill of materials bound to each package by
digest and covered (via the manifest) by the signature.
[tutorial_signing_and_trust.md](tutorial_signing_and_trust.md)

**shadow** — The lifecycle state between armed and active: warm,
configurable, model-loadable, **zero traffic exposure**. The safe staging
ground for anything you intend to A/B.
[tutorial_dappctl_operations.md](tutorial_dappctl_operations.md)

**telemetry stream** — One of the eight named E3 data feeds
(`pusch.symbols`, `pusch.crc`, `srs.channel`, `pusch.dmrs_channel`,
`gnb.metrics_json`, `classc.spectrum`, `classc.srs_isac`, `pusch.grid`).
[tutorial_telemetry_capture.md](tutorial_telemetry_capture.md)

**trust policy** — The operator's offline file of trusted vendor keys,
per-package sequence floors, revocations, and a monotonic policy epoch.
[tutorial_signing_and_trust.md](tutorial_signing_and_trust.md)

**worker / lane** — The host threads that invoke a dApp; worker state is
lane-local (no locks), instance state is shared. Breakers are tracked per
instance across lanes. [extension_model.md](extension_model.md)
