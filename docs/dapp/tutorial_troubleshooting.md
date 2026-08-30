<!-- SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc. -->
<!-- SPDX-License-Identifier: BSD-3-Clause-Clear -->

# Tutorial: troubleshooting dApps and E3 — a symptom-driven runbook

Every entry here is a failure mode someone has actually hit. Find your
symptom, apply the fix, and read the one-line *why* so it doesn't bite twice.
Reference docs: [e3_management.md](e3_management.md),
[yaml_configuration.md](yaml_configuration.md),
[security_model.md](security_model.md),
[known_limitations.md](known_limitations.md).

## Admission & startup

**The gNB refuses to start once I add a `dapp:` block —
"`native_preflight_helper must be a trusted absolute executable`".**
Every `dapp:` block requires `native_preflight_helper` set to an **absolute**
path (the installed `ocudu-dapp-preflight`), not group/other-writable, owned
by root or the gNB euid. There is no default. See the
[install tutorial](tutorial_install_and_yaml.md) §0 for a minimal block.

**`verification_failed` on load, but my bundle is signed.**
Check in this order:
1. Mode is `required` but the signer's public key isn't in the
   `package_trust_store` policy (or the policy's epoch is below the YAML's
   `minimum_trust_policy_epoch`).
2. The bundle's `package_sequence` is below that package's floor in the trust
   policy — floors key on the **16-byte hex package-id**, not the reverse-DNS
   name.
3. You replaced the on-disk artifact while the gNB was running with an
   **older** validly-signed bundle: runtime refresh refuses downgrades even
   when startup would admit them. Restart the gNB to deliberately roll back.

**`allow_unsigned` is set but a package still fails signature checks.**
`allow_unsigned` forgives a **missing** `.sig.json` sidecar only; a sidecar
that is *present but invalid* (or undecodable) fails closed in every mode.
Delete the stale sidecar or fix the signature — don't ship a broken one.

**Load fails with a conflict about "unload all instances before replacing
package content".**
You changed the artifact on disk while an instance from that package is still
resident. Deactivate and unload every instance of the package (prove
quiescence), then load again — the refresh happens on the next load.

## Lifecycle & health

**`inventory` shows the instance `ACTIVE`, but its output looks conventional
(bypassing).**
An open circuit breaker does **not** change the lifecycle state. Use
`health`, not `inventory`: it reports breaker state, consecutive failures,
fallback and deadline-miss counters per instance. Reset with the
`breaker-probe` lifecycle action once you've addressed the cause.

**Unload hangs in `DRAINING`.**
The runtime is waiting for proof of quiescence (no worker still inside the
module). If a lane is wedged, `retry-quiescence` retries the proof; a module
that never quiesces is quarantined rather than unloaded under a live kernel —
that is deliberate (use-after-free protection), not a hang to force.

**`config set` is rejected.**
Two usual causes: entries must be **typed** (`--set key=i64:42`,
`key=bool:true` — bare `key=42` is a parse error), and
`--config-generation` is mandatory: read the current generation with
`config get` first. A generation mismatch means someone else (a human, an
agent, an initial-instance bring-up) changed config since you read it —
re-read and re-apply. That's the lost-update check working, not a bug.

## Telemetry

**A stream is silent, or much slower than expected.**
Publication floors default **non-zero** (e.g. `pusch.symbols` hard floor
10 ms, `srs.channel` 5 ms, `pusch.dmrs_channel` 5 ms, `pusch.crc` 1 ms). Full-rate
capture requires an explicit `telemetry_<stream>_hard_floor_ms: 0` in YAML —
the floor is a hard bound the subscription cannot override. See the floor
table in [yaml_configuration.md](yaml_configuration.md).

**Records arrive but `dropped` is climbing.**
Producers never block the radio: when your consumer (or the SCTP link) can't
keep up, records are counted and dropped, never queued unboundedly. Reduce
the subscribed rate (`sample_every_n`, floors), subscribe to fewer streams,
or move the consumer closer to the gNB. A nonzero drop counter is
back-pressure telling you the truth, not data loss to fix in the host.

## E3 connectivity

**`cannot connect SCTP` / connection refused.**
- The kernel needs SCTP: `sudo modprobe sctp` (containers use the *host's*
  kernel module).
- The loopback test agent (`e3ap_test_server`) **exits when stdin closes** —
  run it under a held-open stdin (compose uses `tty: true`; in a script:
  `sleep 300 | e3ap_test_server ... &`).
- Something else may own the port — a live gNB already listening on 36423
  means your test agent silently lost the bind race. Pick another port or
  run inside a container network.

**The local socket gives `Permission denied`.**
The management socket is created `0600` and peer-credential-checked
(`SO_PEERCRED`). Run the client as the gNB's user or root, or use the
E3AP/SCTP endpoint instead (`--sctp 127.0.0.1:36423`).

**I bound E3AP beyond loopback and now anyone can manage the cell.**
That is the documented behavior: this release has no TLS/DTLS or token
authentication on E3AP, and `allowed_peers: []` admits every peer. Loopback
(or an external IPsec/VPN boundary plus a real `allowed_peers` list) **is**
the security boundary. See [security_model.md](security_model.md).

## Agents & automation

**My agent's first `dapp_lifecycle` / `load_dapp` call "did nothing".**
It validated. Every mutating MCP tool defaults to `dry_run=true`; the reply
carries a `dryRunSuccess` outcome (and no job record). Re-issue with
`dry_run=false` to apply — that asymmetry is the safety model, not a fault.

**Two controllers (human + agent, or two agents) keep overwriting each
other's config.**
They can't — that's the point of `expected_config_generation`. The loser gets
a generation conflict and must re-read. Design loops to *observe → dry-run →
confirm* rather than blind-writing.

## Containers & CI

**One dApp runtime unit test fails only inside Docker
(`requires_seccomp_filter_before_route_admission_when_configured`).**
Expected: the Docker runtime already applies a seccomp filter to every
process, so the "no filter must be rejected" negative case can't be
exercised there. Run it on a bare host; CI excludes it in-container.

**The release-docs check fails on links into a sibling repo.**
The checkers verify cross-repo links only when the sibling repo
(`ocudu-dapp-sdk` next to `ocudu`, or vice versa) is checked out alongside;
a standalone checkout skips them. If it *fails*, the sibling is present and
the target file genuinely doesn't exist — fix the link.

**An SCTP-heavy unit test flakes under a loaded CI host.**
The loopback SCTP tests are timing-sensitive under heavy CPU contention
(observed roughly once per hundreds of runs in a saturated container build).
Retry the stage; if it repeats on an idle host, that's a real failure.

See the whole journey in one story: [the capstone](tutorial_capstone.md).

## Still stuck?

`incidents list` is the audit trail — every fault, fallback, breaker
transition and lifecycle event lands there with an instance attribution, and
`jobs list` covers async operations (the MCP equivalents are
`list_incidents` / `list_jobs`). When reporting a problem, capture
`inventory`, `health`, and `incidents list` output together; that triple
almost always localizes it.
