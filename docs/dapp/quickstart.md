<!-- SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc. -->
<!-- SPDX-License-Identifier: BSD-3-Clause-Clear -->

# OCUDU dApp Quickstart (Git Markdown)

Guided path: the [step-by-step tutorial series](README.md#start-here) walks
every workflow this reference covers.

Working version **v1**. This is the Git-rendered host-tree walkthrough. Matching
Doxygen: page `dapp_quickstart` (`docs/doxygen/dapp_quickstart.dox`). The
standalone SDK ships the same workflow with scaffold detail in
`ocudu-dapp-sdk/docs/ocudu_dapp_quickstart.md`.

## Mental model

```text
out-of-tree package  →  catalog (artifact + manifest-v1 + SPDX)
                     →  optional sign / trust
                     →  DU YAML package_manifests [+ initial_instances]
                     →  E3: load → prepare → arm → shadow → activate
                     →  metrics / health / incidents
```

| Class | Role | Placement |
|-------|------|-----------|
| A | Resident inline L1 | In-process only |
| B | Bounded RT scheduler decision | In-process |
| C | Async observation | Native or supervised process |

Rules: no peer dApp calls; E3 never supplies paths/argv/routes; conventional
fallback remains; production signatures required by default.

## Build OCUDU with plugins

```bash
cmake -S . -B build-dapp -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=$HOME/ocudu-install \
  -DENABLE_DAPP=ON -DENABLE_E3=ON -DENABLE_CUDA=ON
cmake --build build-dapp -j && cmake --install build-dapp
```

Build the SDK against the install prefix; run `ctest` there. Scaffold a Class-A
CUDA package with `tools/scaffold_class_a_cuda.py`, or copy Class B/C references.
Certify Class-A CUDA with `certify_class_a_cuda` before catalog staging.

## Catalog and YAML

```bash
cp my_pkg.so my_pkg.manifest.json my_pkg.spdx.json /opt/ocudu/dapps/
```

```yaml
dapp:
  catalog_root: /opt/ocudu/dapps
  native_preflight_helper: /opt/ocudu/bin/ocudu-dapp-preflight  # absolute path, required
  package_signature_mode: allow_unsigned   # lab only; production: required + trust
  minimum_trust_policy_epoch: 0     # must be 0 when no trust store is configured
  package_manifests:
    - my_pkg.manifest.json
  local_e3:
    socket_path: /run/ocudu/dapp-management.sock
    request_timeout_ms: 1000
```

Full keys: [yaml_configuration.md](yaml_configuration.md). `catalog_root` and
`native_preflight_helper` are required whenever a `dapp:` block exists. Signing:
`dapp_release_provisioning` Doxygen / `ocudu-dapp-package`.

Optional `initial_instances` start shadow/active instances before E3 ingress
(fail-closed reverse rollback).

## Run and verify

Enable the E3AP SCTP endpoint next to the local socket to use the Python
clients without root:

```yaml
dapp:
  catalog_root: /opt/ocudu/dapps
  package_signature_mode: allow_unsigned    # lab posture; see the signing tutorial
  minimum_trust_policy_epoch: 0
  native_preflight_helper: /usr/local/bin/ocudu-dapp-preflight
  package_manifests: [ ... ]
  local_e3:
    socket_path: /run/ocudu/dapp-management.sock
  e3ap_sctp:
    bind_address: 127.0.0.1
    port: 36423
```

> **Security note:** `dapp.e3ap_sctp` has **no authentication** — any peer that
> can reach the port (subject only to the literal `allowed_peers` address list)
> gets full management authority: UE release/handover, RF gain changes, and
> dApp lifecycle control. Keep it on loopback or behind a network boundary; see
> [security_model.md](security_model.md).

Then launch the gNB with that YAML and verify:

```bash
# Unit and integration coverage for the runtime and the E3 carriers
ctest --test-dir <ocudu-build> -R dapp     # dapp_runtime_test, dapp_composition_test,
                                           # dapp_e3_python_codec.golden, dapp_e3ap_python.cross_language, ...

# Discover RAN functions and telemetry streams over E3AP
export OCUDU_E3_SCHEMA_DIR=schemas/e3
python3 -m ocudu_e3 catalog

# Management inventory over the same SCTP endpoint
python3 apps/tools/dappctl/ocudu_dappctl.py --sctp 127.0.0.1:36423 inventory
```

## Enable / disable (E3)

```bash
export OCUDU_DAPP_E3_SOCKET=/run/ocudu/dapp-management.sock
python3 apps/tools/dappctl/ocudu_dappctl.py --socket $OCUDU_DAPP_E3_SOCKET \
  --schema schemas/e3/e3sm-ocudu-dapp-mgmt-v1.asn1 inventory

# load → prepare → arm → shadow → activate (carry instance generation each step)
# deactivate → unload to disable
```

Mutations are async jobs. See [e3_management.md](e3_management.md).

## Monitor and verify

```text
metrics schema/get · health · incidents list · jobs list · quiet-reservations
```

Working: package in inventory; active state; invoke/publication counters advance
without spike fallbacks for in-profile traffic; process heartbeats healthy.
Containment: failure gallery ctest and conventional fallback.

### Class-A equalizer health under UL traffic

When an **equalizer** package is active (not only CE or full receiver), check:

| Signal | Expected when healthy |
|--------|------------------------|
| `class_a.fallback.*` | Near zero for admitted shapes (spikes ⇒ soft fallback) |
| `class_a.completion.deadline_misses` | Soft GPU completion budget; distinct from breaker-open |
| Scheduler UE `pusch_snr_db` | **Present** under UL (not blank/`n/a`) after equalizer-dApp success |
| UL MCS / CRC | Link adaptation moves with SNR; all-ACK at low MCS with missing SNR is a host CSI bug |

**Why SNR matters:** after a successful equalizer-dApp inline path the resident
CE stub leaves CSI empty and the host skips conventional E2E SINR D2H. The host
derives SINR from **mean post-EQ noise** written by the package
(`SINR_dB = -10 log10(mean σ²)`), then publishes it on CRC for the scheduler.
Packages that fill valid post-EQ noise need no ABI change.

**Optional DMRS hint:** host may set `dmrs.reserved[0]` to an OFDM-symbol
bitmask (bit *s* set if symbol *s* has DMRS). Packages may ignore it and use
coordinates. Layout remains ABI v1.

**Optional DC hint:** when the UE signals a transmitter DC location inside the
carrier, the host sets `pusch.reserved[3]`
(`OCUDU_DAPP_PUSCH_RESERVED_DC_SUBCARRIER_PLUS_ONE_V1`) to that grid
subcarrier + 1 (zero = unknown) and raises `OCUDU_DAPP_PUSCH_DC_OVERLAP_V1`
when it falls inside the grant. The pilot on that RE is LO leakage, not
channel; the SDK's Class-A CUDA contract excludes it from estimation and
erases the data RE. Layout remains ABI v1.

**Default soft wall budgets** (overridable in tests): demodulator **2000 µs**,
channel estimator **800 µs** (the CE completion measurement includes
same-stream host consumer work after the dApp returns, so the budget allows
one 30 kHz slot plus scheduling jitter). One divergence: the resident CUDA
adapter's `create_pusch_resident_dapp` factory defaults its CE deadline
parameter to **100 µs**; the production upper-PHY factory path always passes
the 800 µs budget explicitly
(`include/ocudu/phy/upper/dapp/upper_phy_dapp_hook_provider.h`,
`pusch_dapp_demodulator.h`, `pusch_resident_dapp.h`). Recoverable misses latch
a worker after 8 consecutive observations. Soft deadline ≠ 5G slot clock; see
[e3_system_design.md](e3_system_design.md) § Class A.

Activate/deactivate A/B (same catalog, lifecycle only) is the supported way to
compare equalizer-dApp vs conventional path without changing YAML OLLA tables.

## Related

- [e3_system_design.md](e3_system_design.md) · [e3_wire.md](e3_wire.md)
- Operator policy Doxygen: `dapp_operations`
- Metrics: `dapp_observability`
- SDK Class-A authoring (post-EQ noise, reserved[0]): `ocudu-dapp-sdk/docs/class_a_cuda_authoring.md`
- SDK docs: `ocudu-dapp-sdk/docs/README.md`
