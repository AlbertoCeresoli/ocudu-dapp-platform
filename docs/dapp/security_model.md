<!-- SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc. -->
<!-- SPDX-License-Identifier: BSD-3-Clause-Clear -->

# dApp security and containment model

Guided path: the [step-by-step tutorial series](README.md#start-here) walks
every workflow this reference covers.

Working version **v1**. This guide states the trust boundaries precisely:
what signing proves, what each E3 carrier authenticates, what the Class-C
process route contains, and what an operator must do before production
exposure. Packaging detail: [extension_model.md](extension_model.md); carrier
wire detail: [e3_wire.md](e3_wire.md); explicit caveats:
[known_limitations.md](known_limitations.md).

## Trust boundaries

Class A and B dApps are **signed, operator-trusted native code loaded into the
DU process**. Signatures establish provenance and approval; they do not
sandbox hostile code. Admission verifies, before any module code runs: the
artifact SHA-256 and SPDX SBOM digest bound by the manifest, the detached
`ecdsa-p256-sha256` signature against the offline **trust store**
(`package_trust_store`), the trust policy epoch against the YAML
`minimum_trust_policy_epoch` floor, the per-package `package_sequence` against
the policy's `minimum_sequence` (rollback protection), key revocation, ABI
version, declared interfaces, backend, placement, and the bounded PUSCH
profiles. `package_signature_mode: allow_unsigned` (lab only) admits a
*missing* sidecar; a present bad signature fails closed in every mode.

The **native preflight** helper (`native_preflight_helper`, required whenever
a `dapp:` block exists) executes the package lifecycle in a disposable process
before host admission: it receives only the approved artifact descriptor and
an allowlisted environment, closes unrelated descriptors, disables dumps and
privilege gain, applies resource limits and a seccomp deny-list, binds its
lifetime to the host, and is killed as a process group on timeout. It catches
crashes, hangs, and lifecycle failures; it does not convert an untrusted
native package into safe code.

At runtime, Class A/B failures return conventional authority to the host with
attributable fallback/exception/deadline/breaker metrics; the scheduler
consumes bounded, validated intent only ([capabilities.md](capabilities.md)).

## Carrier authentication and authority

| Carrier | Authentication | Authority granted |
|---------|----------------|-------------------|
| `dapp.local_e3` (`AF_UNIX` seqpacket) | **Credentialed**: `SO_PEERCRED` UID equality (`allowed_uid`, default the DU euid) on a mode-0600 socket; clients verify the server UID symmetrically (`lib/dapp/e3/local_client.cpp`, `local_connector.cpp`) | Full management service model |
| `dapp.e3ap_sctp` (E3AP over SCTP) | **None.** After `accept`, the peer's literal address is compared against `allowed_peers` — an empty list admits all (`lib/dapp/e3/sctp_association_server.cpp`). No TLS/DTLS, no token layer in v1 | **Full management authority, equal to the local socket**: UE release to idle, UE handover, RU TX/RX gain changes, and complete dApp lifecycle/configuration/model control, plus all telemetry |
| `dapp.e3dp_sctp` (FlatBuffers data profile) | **None** — same address-only admission | Data plane only: `dataSubscriptions` / `dataPoll` (per-UE I/Q, channel estimates, Class-C snapshots) |

Treat the E3AP port exactly like root access to the management socket.
`allowed_peers` is an address filter, not authentication — any host that can
source an admitted address holds the authority above.

## Class-C process containment

Class C may run in-process (trusted) or under the supervised process route
when bounded fault isolation is worth the transport cost. The route uses
bounded SHM rings with leases, epochs, and sequence validation
(`ipc/publication_route.h`), non-blocking producer admission (overload drops
sensing publications before they can block UL or scheduler work), and a
credentialed control channel: `ipc/seqpacket_channel` enables `SO_PASSCRED`
and rejects any control packet whose sender UID differs from the expected UID
(the DU euid) or whose PID differs from the supervised child once pinned
(`lib/dapp/ipc/seqpacket_channel.cpp`).

Before transferring routes, the supervisor verifies from `/proc/<pid>/status`
that the worker applied **`no_new_privs`**, and — when
`class_c_process_require_seccomp_filter: true` — that it runs in **seccomp
filter mode** (`Seccomp: 2`); a worker that has not is never admitted
(`lib/dapp/runtime/class_c_process_supervisor_support.cpp`). Irrevocable
RLIMITs apply before route transfer: core dumps disabled by default
(`class_c_process_max_core_file_bytes: 0` — core files can contain RF
samples), bounded open files and address space, plus authenticated
heartbeats, bounded restart attempts, and route withdrawal before kill
([yaml_configuration.md](yaml_configuration.md) § Class-C process). The
reference workers install a seccomp deny-list against executable-image
replacement, process injection, and privileged kernel administration. This
boundary is **fault containment, not hostile multi-tenancy**; Landlock,
namespaces, distinct service identities, and cgroups remain deployment
hardening layers.

## Production requirements

1. **Signature mode `required`**, with a maintained trust store, a nonzero
   `minimum_trust_policy_epoch`, and rotation/revocation procedures; protect
   signing keys offline.
2. **Keep the SCTP carriers on loopback** (the default `bind_address:
   127.0.0.1`) or behind an external boundary — IPsec, VPN, or network
   policy — before any off-host exposure. Never expose the local socket
   off-host; it has no TLS profile.
3. **Least privilege**: run the DU without unnecessary capabilities, restrict
   device and filesystem access, protect core files, and require seccomp for
   supervised workers where the deployment policy demands it.
4. **Data minimization** on telemetry: subscribe to derived streams where
   possible, apply RNTI filters, and set `telemetry_*_hard_floor_ms` caps on
   production cells ([capabilities.md](capabilities.md) § Telemetry).
5. **Alert** on admission rejections, signature failures, deadline and
   breaker incidents, queue drops, and process restarts (`incidents`,
   `health`; [e3_management.md](e3_management.md)).
