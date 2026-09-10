<!-- SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc. -->
<!-- SPDX-License-Identifier: BSD-3-Clause-Clear -->

# Runtime native-package admission

`catalog admit` adds a signed native package to a running host, or replaces
the catalog record for an **unloaded** package. It never loads a library,
runs CUDA preflight, binds a hook, or activates an instance. Radio operation
and existing active instances continue unchanged.

This is an additive `catalogAdmit` extension of the OCUDU management service
model, carried over the existing credentialed local socket or E3AP SCTP
association. It does not change the dApp C ABI, existing APER alternatives,
or the upstream libe3 envelope. An older host cannot use the new operation:
install a host containing this feature once before using it.

## Stage locally, select by identity over E3

The request carries only a package UUID. The host resolves it to:

```text
<configured catalog_root>/<32 lowercase UUID hex digits>/package.manifest.json
```

For `00112233-4455-6677-8899-aabbccddeeff`, stage the manifest at
`00112233445566778899aabbccddeeff/package.manifest.json`, with its detached
signature at `package.manifest.json.sig.json`. Keep artifact and optional
SPDX paths relative to this manifest, as in startup packaging. The signed
manifest must contain the requested package ID.

Use a local, operator-owned filesystem. Bundle files must be regular,
root/DU-user owned, and not group/other writable. Stage into a temporary
sibling directory, finish signing and inspection, then publish the complete
directory with a same-filesystem rename. Do not overwrite a resident `.so`.
E3 cannot upload bytes or select filesystem/trust-store paths. It cannot
admit unsigned bundles, even when startup allows unsigned lab packages.

The startup-pinned trust-store path must authorize the package ID, signing
key, and minimum package sequence. To authorize a new identity, the local
operator stages a new policy at that same path. A changed policy requires
a strictly larger `policy_epoch`. Keep all previous package floors at least
as high and retain all key revocations. Trust rotation and package admission
commit together; failed admission and dry-run change neither.

```bash
python3 apps/tools/dappctl/ocudu_dappctl.py --sctp 127.0.0.1:36423 inventory

# Substitute the actual generation returned by inventory.
python3 apps/tools/dappctl/ocudu_dappctl.py --sctp 127.0.0.1:36423 \
  catalog admit --package 00112233-4455-6677-8899-aabbccddeeff \
  --expected-inventory-generation 42 --dry-run

# Inspect the returned jobId until the result is dryRunSuccess.
python3 apps/tools/dappctl/ocudu_dappctl.py --sctp 127.0.0.1:36423 \
  jobs get --job <job-id>

# Repeat without --dry-run, using a fresh transaction and current generation.
# Wait for job success, then query inventory. Loading remains separate.
```

The C++ `ocudu-dappctl` has the same command over the local socket. Python
applications may call `E3Client.catalog_admit(package_id, dry_run=True)`.
An `accepted` response means **job queued**, not package admitted.

## Safety and concurrency contract

- Admission uses the bounded, transactional management dispatcher. No catalog
  scan or polling is added to L1.
- Signature, manifest, SPDX (when declared), and artifact validation run on
  the management worker with the instance-manager mutex released. Inventory
  and health readers do not wait behind this file IO. The catalog is capped
  at 256 packages; files retain their existing validation-size bounds.
- Nonblocking opens prevent a substituted FIFO from waiting for a writer
  before type checking. This is not a hard wall-time guarantee for failed
  or slow storage.
- Commit rechecks inventory and policy identity. Concurrent changes cause a
  conflict, not a lost update. Retry conflicts with a **new transaction**;
  the same transaction replays its original result.
- Package-ID/global-name collisions and identity/interface/profile changes
  are refused. Replace implementation/version only after every instance of
  that package is unloaded, including inactive or quarantined residents.
- Package sequences cannot decrease during a host lifetime. Re-signing the
  same artifact at a higher sequence retains that high-water mark. Reusing
  a sequence for changed artifact/version/SBOM content is rejected.
- Tightened trust marks affected catalog entries incompatible and rejects
  subsequent loads/activations. It does **not** forcibly stop existing instances;
  deactivation/quiescence remains an explicit operator action.

## Boundaries

This first version admits **in-process native** packages. New Class-C process
routes still require host-owned factory/topology construction at startup;
they are rejected rather than advertised as usable without a route.

Admission does not remove files/packages, edit YAML, persist desired lifecycle
state, or replace the gNB executable. Add the UUID-relative manifest to the
deployment's startup `package_manifests` for persistence across future restarts.
Maintain the deployment's minimum trust-policy epoch for restart protection.

The existing credential/peer policy applies: SCTP address allowlisting is not
authentication. Keep it on loopback or a protected management network. This
is not a secure remote code-upload API.

Admission executes no package code. Later **load/prepare/activate** can allocate
resources and run native/CUDA lifecycle code; those operations still require
their own latency and OTA qualification.

## Regression coverage

The mandatory host interop gate includes signed admission/dry-run/replay,
sequence/policy rollback, resident-update refusal, FIFO rejection, C++/Python
CLI cross-decoding, and real libe3 control-action transport. Existing
manifest/SPDX tests validate the parser reused by runtime admission.

```bash
python3 scripts/check_dapp_interop_gate.py --build <cpu-test-build>
ctest --test-dir <cpu-test-build> --output-on-failure \
  -R '^dapp_package_manifest|^dapp_package_signature|^dapp_instance_manager'
```

These are radio-free conformance checks, not a loaded-L1 latency certificate.
