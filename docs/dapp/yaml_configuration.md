<!-- SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc. -->
<!-- SPDX-License-Identifier: BSD-3-Clause-Clear -->

# dApp YAML / CLI configuration reference

Guided path: the [step-by-step tutorial series](README.md#start-here) walks
every workflow this reference covers.

Working version **v1**. The same schema is shared by dynamic, split-8, and
split-7.2 DU front-ends. Fields are validated by `dapp_config_validator` before
service construction. Absence of the entire `dapp:` block leaves the modem path
unchanged.

## Root `dapp:` fields

| Key | Type | Default / notes |
|-----|------|-----------------|
| `catalog_root` | path | Root of approved artifacts; **required whenever a `dapp:` block exists** |
| `native_preflight_helper` | path | Absolute preflight binary; **required whenever a `dapp:` block exists**. Not group/other-writable. Owner must be root or the gNB euid; **root may use a developer-owned helper** (e.g. under `sudo`) |
| `native_preflight_timeout_ms` | u32 (ms) | Default 500 (1..60000); do not raise for qualification evidence |
| `package_manifests` | list of relative paths | Catalog-relative manifest names only |
| `initial_instances` | list | ≤64; each names a listed manifest |
| `package_signature_mode` | `required` \| `allow_unsigned` | Production: `required` |
| `package_trust_store` | absolute path | Required when mode is `required` |
| `minimum_trust_policy_epoch` (default **1**; **must be 0** with `allow_unsigned` and no trust store) | u64 | Nonzero with required signatures |
| `max_transactions` | u32 | Default 4096 (1..65536); retained idempotent management transactions |
| `max_queued_jobs` | u32 | Default 64 (1..4096); queued dApp management jobs |
| `max_retained_jobs` | u32 | Default 1024 (1..65536); retained dApp management job records |
| `max_scheduler_ul_prbs_avoided` | u32 | Cap on Class-B avoid mask width; zero disables this authority |
| `pusch_route_slots` | u32 | Default 1 (1..8); instances each Class-A PUSCH hook (channel estimator, equalizer, receiver) can hold at once for per-UE routing over E3 ([pusch_routing.md](pusch_routing.md)) |
| `scheduler_evaluation_only` | bool | Measure Class B without authority (validate, then discard) |
| `scheduler_allow_mcs` | bool | Commit Class-B MCS intents for newTx |
| `scheduler_allow_prb_limits` | bool | Commit min/max PRB limits |
| `scheduler_allow_prb_preference` | bool | Soft PRB start/length preferences |
| `scheduler_allow_prb_assignment` | bool | Hard multi-UE PRB assignments (needs free-VRB snapshot) |
| `scheduler_allow_tpc` | bool | UL TPC command overrides |
| `scheduler_allow_layers` | bool | Layer/RI overrides |
| `scheduler_max_mcs_delta_from_host` | i16 | Default 28 (0..28); max \|dApp−host\| MCS when host LA valid |
| `class_c_process_*` | see below | Supervised worker limits |
| `portable_e3_data` | optional block | Bounded host store behind every E3 data path (auto-enabled by the SCTP carriers) |
| `quiet_*` | see below | Duty bounds for quiet intents |
| `local_e3` | optional block | Credentialed same-host management socket |
| `e3ap_sctp` | optional block | E3AP-OCUDU-V1 endpoint over SCTP (out-of-process dApps, dashboards) |
| `e3dp_sctp` | optional block | FlatBuffers E3 data-profile endpoint (reference SCTP client) |
| `telemetry_*_min_interval_ms` / `telemetry_*_hard_floor_ms` | u32 | Publication floors for the upper-PHY telemetry streams (exact keys below) |

There is **no** `scheduler_deadline` YAML key: the Class-B complete boundary
(typically 100 µs) is a host default in the service configuration, not an
operator knob.

Class-A **soft** equalizer/CE wall budgets are also **host factory defaults**
(not YAML keys today): see [quickstart.md](quickstart.md) § Class-A equalizer
health under UL traffic for the authoritative numbers (demodulator 2000 µs,
channel estimator 800 µs, 8-consecutive-miss latch) and the
`dmrs.reserved[0]` convention.

Class-B allow flags map into ABI `allowed_output_flags` (subset of
`OCUDU_DAPP_SCHEDULER_ALLOW_KNOWN_MASK_V1`). The host invoke path must accept
those known bits; an older gate that only permitted avoid-mask rejects MCS/PRB/TPC
as `INVALID_INPUT` and records `class_b.policy.fallbacks`. Hard PRB assignment
additionally requires a post-retx free-VRB mask on the input. Reference package
algorithms and defaults:
`ocudu-dapp-sdk/docs/class_b_scheduler_baseline.md`.

Topology-derived lane counts (`nof_pusch_lanes`, SRS, scheduler) are filled by
the DU factory from cell configuration; operators normally do not hand-edit
them.

## `initial_instances[]`

| Key | Values | Notes |
|-----|--------|-------|
| `package_manifest` | relative path | Must appear in `package_manifests` |
| `placement` | `in_process` / `out_of_process` | Must match signed manifest |
| `backend` | `cpu` / `cuda` / … | Must match manifest + host |
| `target` | `shadow` \| `active` only | No other startup targets |
| `lifecycle_timeout_ms` | positive | Per prepare/arm/shadow/activate/cleanup |

Startup is synchronous, all-or-nothing, **before** E3 ingress. Failure unwinds
every selected instance in reverse.

## `local_e3`

| Key | Notes |
|-----|-------|
| `socket_path` | Filesystem path for the AF_UNIX management endpoint |
| `allowed_uid` | Optional peer UID restriction |
| `request_timeout_ms` | Client/server request bound |

Without `local_e3`, embedded management may be unavailable for external clients
even if `ENABLE_E3` is compiled.

## `e3ap_sctp`

| Key | Default | Notes |
|-----|---------|-------|
| `bind_address` | `127.0.0.1` | Literal IPv4/IPv6 address. Anything but loopback exposes the agent to the network; add an external boundary first |
| `port` | `36423` | SCTP port |
| `allowed_peers` | empty (admit all) | Literal peer addresses admitted after accept |
| `maximum_associations` | 8 | Concurrent dApp associations |
| `maximum_subscriptions_per_association` | 8 | Data-profile subscriptions one association may hold (1..1024) |
| `idle_timeout_ms` | 30000 | Close quiet associations (1 s..1 h) |
| `setup_timeout_ms` | 5000 | Close associations that never complete E3 setup |
| `maximum_subscriptions_per_dapp` | 16 | Subscriptions per association |
| `ran_identifier` | `ocudu-gnb` | `ranIdentifier` in setup responses (≤64 bytes) |

Enabling `e3ap_sctp` without `portable_e3_data` creates the store with its
defaults; the store is where telemetry and Class-C snapshots are queued for
the E3AP indication drain. Management control actions on E3AP carry the same
authority as `local_e3`.

## `e3dp_sctp`

| Key | Default | Notes |
|-----|---------|-------|
| `bind_address` | `127.0.0.1` | Literal address |
| `port` | `38472` | Port expected by the E3 SCTP reference client |
| `allowed_peers` | empty | Literal peer addresses |
| `maximum_associations` | 16 | Concurrent client associations |
| `idle_timeout_ms` | 30000 | Close quiet associations |

Requires a FlatBuffers-capable build (`flatc` at configure time). Only
`dataSubscriptions` / `dataPoll` are served; RF placement on subscription
records is derived from each cell's uplink carrier.

## Telemetry publication floors

| Key | Default | Stream |
|-----|---------|--------|
| `telemetry_pusch_symbols_min_interval_ms` | 50 | `pusch.symbols` (equalized I/Q per UE) |
| `telemetry_pusch_crc_min_interval_ms` | 0 (every TB) | `pusch.crc` |
| `telemetry_srs_channel_min_interval_ms` | 20 | `srs.channel` |
| `telemetry_pusch_dmrs_min_interval_ms` | 20 | `pusch.dmrs_channel` |
| `telemetry_pusch_grid_min_interval_ms` | 100 | `pusch.grid` (raw REs, largest stream) |
| `telemetry_pusch_symbols_hard_floor_ms` | 10 (set 0 explicitly for full rate) | `pusch.symbols` |
| `telemetry_pusch_crc_hard_floor_ms` | 1 (set 0 explicitly for full rate) | `pusch.crc` |
| `telemetry_srs_channel_hard_floor_ms` | 5 (set 0 explicitly for full rate) | `srs.channel` |
| `telemetry_pusch_dmrs_hard_floor_ms` | 5 (set 0 explicitly for full rate) | `pusch.dmrs_channel` |
| `telemetry_pusch_grid_hard_floor_ms` | 20 | `pusch.grid` — raw-grid captures stage the accelerated grid to the host on the PUSCH thread, hence a default bound |

`*_min_interval_ms` is the **default** per-RNTI floor, used for subscribers
that do not ask for a rate (the dashboard). A subscription may request its own
capture interval (`Telemetry-SubscriptionConfig.minimumIntervalMs`, or the
procedure-level `periodicity`); `0` asks for every event. The producer runs at
the fastest interval among its active subscribers, and the E3AP carrier thins
each subscription back to its own interval, so a full-rate recorder does not
change what a dashboard on the same stream receives. The default and requested
floors are applied **per RNTI**; the configured `*_hard_floor_ms` is enforced
as an **aggregate per-stream bound**: the stream as a whole never publishes
more often than the hard floor, regardless of how many UEs are active, and no
subscriber can request below it. Both values are advertised per stream in the
setup response (`minimumIntervalUs`, `hardFloorIntervalUs`). The floors are
applied **before** any capture work (GPU copies): an idle stream costs one
atomic acquire load per event, an active stream three atomic loads plus the
timestamp check. Set a hard floor on production cells where an operator must
be able to cap the telemetry load regardless of what subscribers request.
When a telemetry sink exists it replaces `expert_phy.zmq_diag_endpoint` as the
upper-PHY diagnostics consumer.

## Class-C process (flat `class_c_process_*` keys)

| Key | Default | Notes |
|-----|---------|-------|
| `class_c_process_ring_slots` | 4 (2..64) | Shared-memory slots per isolated Class-C producer route |
| `class_c_process_payload_capacity_bytes` | 4 MiB (64 KiB..64 MiB) | Max metadata + tensor bytes copied into one route slot |
| `class_c_process_startup_timeout_ms` | 1000 | Worker readiness bound |
| `class_c_process_stop_timeout_ms` | 1000 | Grace period before kill |
| `class_c_process_restart_backoff_ms` | 100 | Delay between restart attempts |
| `class_c_process_heartbeat_period_ms` | 1000 | Authenticated heartbeat challenge interval |
| `class_c_process_heartbeat_timeout_ms` | 3000 | Max heartbeat response time |
| `class_c_process_management_timeout_ms` | 1000 | Max time for one configuration/model operation |
| `class_c_process_max_address_space_bytes` | 0 (inherit host) | Irrevocable RLIMIT_AS before route transfer |
| `class_c_process_max_core_file_bytes` | 0 (dumps disabled) | Irrevocable RLIMIT_CORE |
| `class_c_process_require_seccomp_filter` | false | Require seccomp filter mode before route admission |
| `class_c_process_max_open_files` | 512 (16..1048576) | Irrevocable RLIMIT_NOFILE |
| `class_c_process_max_restart_attempts` | 3 (0..100) | Supervised restart budget |

Workers must set `no_new_privs` before route transfer. See process hardening in
`dapp_operations`.

## Portable E3 data (`portable_e3_data` block)

Requires at least one carrier (`local_e3`, `e3ap_sctp`, or `e3dp_sctp`); the
SCTP carriers auto-create it with defaults when absent. Used by independent
consumers (`ref_*_e3_cpu`, dashboards), not by high-rate SHM/CUDA routes.

| Key | Default | Notes |
|-----|---------|-------|
| `maximum_subscriptions` | 32 (1..32) | Live portable data subscriptions |
| `maximum_retained_bytes` | 128 MiB (1 MiB..1 GiB) | Total preallocated payload queue bytes |
| `maximum_object_bytes` | 4 MiB (1 B..64 MiB) | Largest tensor admitted to E3 chunking; also bounds the largest E3AP indication payload |
| `maximum_metadata_bytes` | 64 KiB (1 KiB..64 KiB) | Largest metadata envelope |
| `minimum_chunk_bytes` | 4 KiB (1 KiB..64 KiB) | Smallest negotiable tensor chunk |
| `maximum_chunk_bytes` | 64 KiB (1 KiB..64 KiB) | Largest negotiable tensor chunk |
| `minimum_lease_us` | 100000 (1..60000000) | Shortest subscription liveness lease |
| `maximum_lease_us` | 10000000 (1..60000000) | Longest subscription liveness lease |

## Quiet reservation duty (flat `quiet_*` keys)

| Key | Default | Meaning |
|-----|---------|---------|
| `quiet_max_retained_prb_slots` | 512 | Outstanding quiet PRB-slot volume per cell |
| `quiet_max_prbs_per_slot` | 16 | Per-slot suppress cap |
| `quiet_duty_window_slots` | 100 | Rolling slot window for duty accounting |
| `quiet_max_prb_slots_per_window` | 64 | Max quiet PRB-slot volume in any rolling window |
| `quiet_max_source_prb_slots_per_window` | 64 | Per-dApp-instance window cap (multi-source fairness) |

Denials return bypass + `policyRejection` incidents; they do not open the dApp
breaker by themselves.

## Package signatures

```yaml
package_signature_mode: required
package_trust_store: /etc/ocudu/dapp-trust.json
minimum_trust_policy_epoch: 1
```

`allow_unsigned` admits a **missing** sidecar only; present bad signatures fail
closed. Tools: `ocudu-dapp-package keygen|sign|trust|inspect`.

## Minimal examples

**Lab unsigned receiver:**

```yaml
dapp:
  catalog_root: /opt/ocudu/dapps
  native_preflight_helper: /opt/ocudu/bin/ocudu-dapp-preflight  # absolute path, required
  package_signature_mode: allow_unsigned
  minimum_trust_policy_epoch: 0     # must be 0 when no trust store is configured
  package_manifests:
    - my_receiver_cuda.manifest.json
  local_e3:
    socket_path: /run/ocudu/dapp-management.sock
    request_timeout_ms: 1000
```

**Reference cell with E3AP telemetry for an out-of-process dashboard:**

```yaml
dapp:
  catalog_root: /opt/ocudu/dapps
  native_preflight_helper: /opt/ocudu/bin/ocudu-dapp-preflight  # absolute path, required
  package_signature_mode: allow_unsigned    # lab posture; see the signing tutorial
  minimum_trust_policy_epoch: 0
  package_manifests: [ ... ]
  local_e3:
    socket_path: /run/ocudu/reference-cell/e3.sock
    request_timeout_ms: 3000
  e3ap_sctp:
    bind_address: 127.0.0.1
    port: 36423
    ran_identifier: ocudu-reference-cell
  e3dp_sctp:
    bind_address: 127.0.0.1
    port: 38472
  telemetry_pusch_symbols_min_interval_ms: 50
```

**Declarative dual start:**

```yaml
dapp:
  catalog_root: /opt/ocudu/dapps
  native_preflight_helper: /opt/ocudu/bin/ocudu-dapp-preflight  # absolute path, required
  package_signature_mode: allow_unsigned
  minimum_trust_policy_epoch: 0     # must be 0 when no trust store is configured
  package_manifests:
    - my_receiver_cuda.manifest.json
    - ref_spectrumd_cpu.manifest.json
  initial_instances:
    - package_manifest: my_receiver_cuda.manifest.json
      placement: in_process
      backend: cuda
      target: shadow
      lifecycle_timeout_ms: 1000
    - package_manifest: ref_spectrumd_cpu.manifest.json
      placement: out_of_process
      backend: cpu
      target: active
      lifecycle_timeout_ms: 2000
  local_e3:
    socket_path: /run/ocudu/dapp-management.sock
    request_timeout_ms: 1000
```

## Related

- [quickstart.md](quickstart.md)
- [e3_management.md](e3_management.md)
- Doxygen `dapp_operations`, `dapp_release_provisioning`
