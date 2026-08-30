<!-- SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc. -->
<!-- SPDX-License-Identifier: BSD-3-Clause-Clear -->

# Tutorial: signing, SBOMs, and key management

This is the **production trust** guide. The
[install tutorial](tutorial_install_and_yaml.md) shows how to run a dApp with
no keys at all (`package_signature_mode: allow_unsigned`); this page is what
you graduate to when the gNB must refuse anything it cannot cryptographically
verify. It covers what actually gets signed, the SBOM, building and
distributing a trust policy, rotation and revocation, verifying before deploy,
and key custody.

Tool: `ocudu-dapp-package` (`apps/tools/dapp_package`). Schemas:
`schemas/dapp/package-{manifest,signature,trust-policy,spdx-sbom-profile}-v1.schema.json`.
Design rationale: [security_model.md](security_model.md).

## What a "package" is, and what gets signed

A production dApp bundle is four files:

| File | Contents |
|------|----------|
| `pkg.so` | the artifact (ELF64 shared object) |
| `pkg.manifest.json` | identity + **SHA-256 of the artifact** + **SHA-256 of the SBOM** (`provenance.sha256`) + interfaces/backends/placement |
| `pkg.spdx.json` | the SPDX 2.3 SBOM |
| `pkg.manifest.json.sig.json` | the detached signature sidecar |

The key insight: **one signature covers all three.** The signature is over a
domain-separated digest of the *manifest*, and the manifest already contains
the artifact hash and the SBOM hash. So verifying the manifest signature, then
re-hashing the artifact and SBOM against the manifest, transitively
authenticates the whole bundle. Concretely the signed digest is:

```
SHA256( "OCUDU-DAPP-MANIFEST-SIGNATURE-V1\n" || sha256(manifest) || be64(package_sequence) )
```

signed with **ECDSA P-256 / SHA-256**. The `package_sequence` is folded in so a
signature is bound to a monotonic sequence number (rollback protection, below).

## The SBOM is a first-class, verified artifact

The SPDX SBOM is not decorative — the host verifies it. It must be SPDX 2.3
JSON describing **exactly one package and exactly one file**, where:

- the package `name`/`versionInfo`/`supplier` equal the manifest's
  `name`/`version`/`vendor`;
- the single file's `fileName` is the catalog-relative artifact path and its
  `SHA256` equals the admitted artifact digest;
- a `DESCRIBES` relationship ties them together.

This aligns with where regulation (EU CRA, US EO 14028) is pushing the
industry: the SBOM travels with the artifact, is bound to it by hash, and is
checked at admission. The SDK build emits it automatically alongside the
manifest; you normally don't hand-write it.

> **Note:** the SBOM is only verified when the manifest declares `provenance`.
> If you want to *guarantee* every package carries a verified SBOM, treat that
> as an operator policy (require the `provenance` block in your intake) — there
> is no gNB-side "require SBOM" switch today.

## 1. Vendor: generate a signing key (once)

```bash
ocudu-dapp-package keygen --private-key vendor.key --public-key vendor.pub
```

- `vendor.key` is an unencrypted P-256 private key (mode `0600`). **Keep it
  offline.** See [Key custody](#key-custody) below.
- `vendor.pub` is the public key you distribute. Its **`key_id`** is
  `SHA256(SPKI-DER)` — that hash is the stable identity the operator pins.

## 2. Vendor: sign each package

```bash
ocudu-dapp-package sign \
  --catalog-root build/packages/acme_eq \
  --manifest acme_eq.manifest.json \
  --private-key vendor.key \
  --package-sequence 1
```

This writes `acme_eq.manifest.json.sig.json` (mode `0444`, `O_EXCL` so it
never overwrites). The sidecar carries `algorithm`, `key_id`,
`package_sequence`, `manifest_sha256`, and the hex-DER signature. **Bump
`--package-sequence` on every new build of the same package** — the operator's
trust policy enforces a minimum sequence per package, which is how you prevent
an attacker from re-deploying an old, superseded (but validly signed) build.

## 3. Operator: build the trust policy

You (the operator/SI) decide which vendors to trust, offline. One policy file
names every trusted public key and a per-package sequence floor:

```bash
ocudu-dapp-package trust \
  --output /etc/ocudu/dapp-trust.json \
  --policy-epoch 1 \
  --public-key vendorA.pub --public-key vendorB.pub \
  --package 8f0d1c2b3a4e5f60718293a4b5c6d7e8:1 \
  --package 0123456789abcdef0123456789abcdef:1
```

- `--policy-epoch` is a monotonic counter; the DU YAML carries a
  `minimum_trust_policy_epoch` floor, so a policy *older* than the configured
  floor refuses to start (downgrade protection).
- `--public-key` (repeatable) adds trusted keys; `--package id:floor`
  (repeatable) sets the minimum accepted sequence per package. The `id` is the
  **16-byte hex `package-id`** that `sign`/`inspect` print (the manifest's
  UUID; dashes are accepted and ignored) — *not* the reverse-DNS
  `global_name`.
- `--revoke-key-id <key_id>` (repeatable) revokes a key.

Then point YAML at it and require signatures:

```yaml
dapp:
  catalog_root: /opt/ocudu/dapps
  native_preflight_helper: /opt/ocudu/bin/ocudu-dapp-preflight  # absolute path, required
  package_signature_mode: required
  package_trust_store: /etc/ocudu/dapp-trust.json
  package_manifests: [ acme_eq.manifest.json, acme_spectrum.manifest.json ]
```

In `required` mode, a package with **no sequence-floor entry is rejected** —
every admitted package must appear in `--package`.

## 4. Verify before you deploy

`inspect` runs the exact admission checks offline, so you can validate a bundle
against your policy before it touches the gNB:

```bash
ocudu-dapp-package inspect \
  --catalog-root /opt/ocudu/dapps \
  --manifest acme_eq.manifest.json \
  --trust-store /etc/ocudu/dapp-trust.json \
  --minimum-policy-epoch 1
```

It checks path containment, the signature, the trust policy (key present, not
revoked, sequence ≥ floor, epoch ≥ minimum), the manifest shape, the artifact
hash and ELF sanity, and the SBOM binding — the same fail-closed pipeline the
DU runs at load.

## Rotation and revocation

| Operation | How |
|-----------|-----|
| **Rotate a key** | Add the new `--public-key` to the policy, bump `--policy-epoch`, redistribute the policy. (Sign new builds with the new key; old builds signed by the old key still verify until you revoke it.) |
| **Revoke a key** | Add its `key_id` via `--revoke-key-id`, bump the epoch, redistribute. Revocation wins over trust and invalidates **every** past signature by that key. |
| **Block an old build** | Raise that package's `--package id:floor` above the bad sequence, bump the epoch. |

> Two sharp edges to know:
> - Revocation is **absolute** — there is no "trust signatures made before the
>   compromise date." If you must invalidate one bad build but keep the key's
>   good history, prefer raising the sequence floor over revoking the key.
> - The trust policy file itself is authenticated only by **filesystem
>   ownership and mode** (root- or DU-user-owned, not group/other-writable),
>   plus the epoch floor in YAML. There is no policy signature. Protect the
>   file, and change `minimum_trust_policy_epoch` in YAML in lock-step so a
>   swapped-in older policy is rejected.

## Key custody

The tooling currently reads an **unencrypted PEM** private key from disk —
there is no passphrase, HSM, or KMS backend yet. Until there is, treat custody
as an operational discipline:

- Generate and hold `vendor.key` on an **offline** or hardware-protected host;
  never check it into the repo or ship it in CI.
- Distribute only `vendor.pub`; the operator pins its `key_id`.
- Scope keys per vendor, and if you can, per product line — note that **any**
  key in a trust policy can currently sign **any** package (there is no
  key→package delegation), so one compromised key covers everything that
  policy trusts. Minimizing keys per policy limits blast radius.
- Rotate on a schedule and immediately on suspected compromise (revoke + epoch
  bump + redistribute).

## Threat model in one paragraph

With `required` mode and a well-kept policy: the gNB will load only artifacts
whose exact bytes were hashed into a manifest signed by a currently-trusted,
non-revoked key at or above the package's sequence floor, with a matching
verified SBOM, from inside the catalog root — and E3 can never supply an
artifact or path to bypass any of this. The residual trust you are extending
is: (1) the vendors whose keys you added, (2) the filesystem integrity of the
catalog and the policy file, and (3) that the in-process artifact is
non-hostile native code (the admission/preflight/breaker machinery contains
faults, not adversaries — Class-C process placement is the answer when you need
isolation from the code itself). See [security_model.md](security_model.md)
for the full treatment.

## Command reference

| Command | Purpose |
|---------|---------|
| `ocudu-dapp-package keygen --private-key K --public-key P` | Generate a P-256 key pair |
| `ocudu-dapp-package sign --catalog-root R --manifest M --private-key K --package-sequence N` | Write the detached signature sidecar |
| `ocudu-dapp-package trust --output T --policy-epoch E --public-key P... --package id:floor... [--revoke-key-id ID...]` | Build an offline trust policy |
| `ocudu-dapp-package inspect --catalog-root R --manifest M --trust-store T --minimum-policy-epoch E` | Run the admission checks offline |

Back to: [install & load tutorial](tutorial_install_and_yaml.md) ·
[dappctl operations](tutorial_dappctl_operations.md).
