<!-- SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc. -->
<!-- SPDX-License-Identifier: BSD-3-Clause-Clear -->

# dApp extension and packaging model

Working version **v1**. This guide describes what a dApp package *is*, what it
may and may not do, and how the out-of-tree SDK relates to the host tree.
Capabilities of each class: [capabilities.md](capabilities.md); lifecycle and
operations: [e3_management.md](e3_management.md); trust details:
[security_model.md](security_model.md).

## Package anatomy

A package is a native shared object plus sidecar metadata staged under the
operator-controlled `catalog_root` and named — by catalog-relative path only —
in DU YAML (`package_manifests`). The artifact exports one entry point,
`ocudu_dapp_query_v1` (`include/ocudu/dapp/abi/v1/module.h`), which fills an
`ocudu_dapp_module_v1`: identity strings, capability bits, the interface
descriptor table, the lifecycle callbacks
(`create`/`configure`/`prepare`/`warmup`/`start`/`stop`/`quiesce`/`destroy`),
and the optional management vtable (config, metrics, models).

### Manifest v1 (`schemas/dapp/package-manifest-v1.schema.json`)

| Field | Meaning |
|-------|---------|
| `schema_version` | Constant `1` |
| `package_id` | UUID; the stable identity referenced by trust policy and E3 |
| `global_name`, `vendor`, `version` | Human identity (bounded strings) |
| `artifact` | Catalog-relative path to the shared object — absolute paths, `..` segments, and `//` are rejected by the schema pattern |
| `sha256` | **Artifact content hash**; admission fails if the file does not match |
| `placements` | Exactly one of `in_process` / `out_of_process` |
| `backends` | 1..3 of `cpu` / `cuda` / `other_accelerator` |
| `interfaces` | 1..64 records: `interface_id`, `major`/`minor`, `class` (`A`/`B`/`C`), per-interface `placements`, `threading` (`per_worker` / `reentrant_shared` / `serialized`), optional `binding` (`required`/`optional`), `capabilities` bits, and — for Class-A PUSCH interfaces — the declarative `pusch_profile` (max PRB/rx ports/layers, modulation and DM-RS masks, flags) the host enforces before dispatch |
| `provenance` | SBOM binding: catalog-relative `sbom` path, `format` `spdx-2.3-json`, and the SBOM's own `sha256` |

The manifest is read as a bounded, non-writable JSON file below the catalog
root (`include/ocudu/dapp/runtime/package_manifest.h`); what it declares must
match what `ocudu_dapp_query_v1` reports, or admission records the
incompatibility in inventory.

### Signature, trust store, and policy epochs

A detached sidecar (`schemas/dapp/package-signature-v1.schema.json`) signs the
manifest with `ecdsa-p256-sha256`: `key_id`, a monotonically checked
`package_sequence`, `manifest_sha256`, and the signature. Because the manifest
embeds the artifact and SBOM hashes, one signature transitively binds the
whole package. The offline trust policy
(`schemas/dapp/package-trust-policy-v1.schema.json`) carries a `policy_epoch`,
the trusted keys (SPKI), revoked key ids, and a per-package
`minimum_sequence` floor for rollback protection. DU YAML selects
`package_signature_mode` (`required` in production), the
`package_trust_store` path, and `minimum_trust_policy_epoch`; `allow_unsigned`
admits only a *missing* sidecar — a present bad signature always fails closed.
Tooling: `ocudu-dapp-package keygen|sign|trust|inspect`.

## What a package may and may not do

- **No peer calls.** dApps never call dApps. Composition happens through
  host-owned caches: a Class-C spectrum result reaches the Class-B scheduler
  as a provenance-stamped host context, never as a direct call.
- **No paths, argv, or routes over E3.** E3 references only package ids
  already admitted from DU configuration; it cannot introduce filesystem
  artifacts, and supervised workers receive no caller argv.
- **Conventional fallback is guaranteed.** Every Class-A/B seam retains the
  stock implementation; any error status, deadline miss, invalid output, or
  open breaker returns authority to it with attributable counters. A package
  can never make the radio path unavailable by failing.
- **Host owns memory and streams.** Tensors are caller-owned views; resident
  CUDA modules enqueue only on the inherited `execution_stream` and allocate
  nothing during invocation. Selecting a CUDA backend means the algorithm
  stays on CUDA — there is no hidden CPU fallback inside a package.
- **Bounded authority.** Class-B intents are validated against host allow
  flags and guardrails; Class-C quiet requests are advice until the host
  reservation authority grants them.

## Placement and threading

Placements (`abi/v1/base.h`): `in_process` (mandatory for Class A and
latency-critical Class B) and `out_of_process` (Class C under the supervised
process route, traded for bounded transport cost and stronger fault
isolation). Threading models declared per interface: `per_worker` (the host
calls `create_worker_context` per lane and never shares contexts),
`reentrant_shared` (one context, concurrent calls), and `serialized` (one
call at a time). Admission runs the disposable **native preflight** helper
before the host `dlopen`s anything; hot tensors never travel over E3.

## Reference cores and the SDK

OCUDU supplies the host seams; the standalone **ocudu-dapp-sdk** builds
against the installed `OCUDUDAppSDK` package and ships complete reference
dApps intended to be extended: keep the tested shell — ABI discovery,
lifecycle, worker state, tensor validation, memory/stream discipline, metrics,
configuration, model activation, signatures/SBOM, fallback, and teardown —
and replace the algorithm core: CE and noise estimation, MIMO equalization
and SINR reporting, joint receiver/demapping, scheduler
ranking/MCS/OLLA/TPC policy, spectrum detection/classification, or SRS/ISAC
estimation and learned inference. The SDK's C++ helpers are a *source*
authoring surface, not a cross-toolchain binary ABI: rebuild for the target
compiler, standard library, CUDA toolkit, driver, and GPU architecture
([api_compatibility.md](api_compatibility.md)).

A faster kernel benchmark does not qualify an extension. Validate the complete
integration boundary: completion latency, malformed input, concurrency,
enable/disable, fallback, rollback, unload, sustained traffic, and OTA
goodput/BLER against both the unchanged reference and the conventional host
path ([release_checklist.md](release_checklist.md)).
