<!-- SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc. -->
<!-- SPDX-License-Identifier: BSD-3-Clause-Clear -->

# Tutorial: installing dApps and loading them from the gNB YAML

This is the **operator's** side of the story: you have received one or more
signed dApp bundles from vendors (see the SDK's
[new-vendor-repo tutorial](https://gitlab.com/ocudu/work_groups/wg2_ai_ran/ocudu-dapp-sdk/-/blob/main/docs/tutorial_new_vendor_repo.md)),
and you want to admit them on your gNB. It covers the catalog, the trust
policy, the `dapp:` YAML block, and bringing instances up either at startup or
later over E3.

Full key reference: [yaml_configuration.md](yaml_configuration.md). Command
reference: [e3_management.md](e3_management.md). Operating a running instance:
[dappctl operations tutorial](tutorial_dappctl_operations.md).

## The mental model

```
vendor bundle (.so + manifest + SBOM + .sig)   →  YOU copy into the catalog
trust policy (your keys, epochs, seq floors)    →  YOU own it, offline
gnb.yml  dapp:  catalog_root + package_manifests →  names manifests, never paths on the wire
  [+ initial_instances]                          →  optional: bring up at startup
E3 / dappctl: load → prepare → arm → shadow → activate
```

Two invariants worth internalizing:
- **The catalog is named only in YAML.** E3 can *select* from it but can never
  supply a path, argv, or artifact. This keeps "load arbitrary code" off the
  network entirely.
- **Telemetry hard floors default non-zero.** Full-rate capture needs an
  explicit `telemetry_<stream>_hard_floor_ms: 0`.

## 0. Lab quickstart (no keys)

> **No gNB or radio at hand?** The sibling `ocudu-docker/` repository builds
> `ocudu` + `ocudu-dapp-sdk` into a reproducible container and ships a
> loopback E3 agent (`docker compose up e3-agent`) that serves synthetic
> telemetry on E3AP `sctp/36423` — every E3 command in these tutorials can be
> pointed at it instead of a live cell.

**You do not need signatures, keys, or a trust policy to get a dApp running.**
For a lab or bring-up, set `package_signature_mode: allow_unsigned` and load
the vendor's three files directly:

```bash
# 1. Drop the bundle in a catalog (no .sig needed)
sudo mkdir -p /opt/ocudu/dapps
sudo cp acme_eq.so acme_eq.manifest.json acme_eq.spdx.json /opt/ocudu/dapps/
```

```yaml
# 2. Minimal gnb.yml — lab only
dapp:
  catalog_root: /opt/ocudu/dapps
  native_preflight_helper: /opt/ocudu/bin/ocudu-dapp-preflight  # absolute path, required
  package_signature_mode: allow_unsigned    # forgives a MISSING signature
  minimum_trust_policy_epoch: 0     # must be 0 when no trust store is configured
  package_manifests:
    - acme_eq.manifest.json
  local_e3:
    socket_path: /run/ocudu/dapp-management.sock
```

```bash
# 3. Start the gNB, confirm it was admitted, and load it
python3 apps/tools/dappctl/ocudu_dappctl.py --socket /run/ocudu/dapp-management.sock inventory
```

That's the whole loop without any crypto. `allow_unsigned` forgives a
*missing* signature sidecar — a *present but invalid* one still fails, so it's
safe against accidental tampering but is **not** a production posture.

When you're ready for production (`package_signature_mode: required`, real
keys, trust policy, SBOM verification), that's its own topic — see the
**[Signing, SBOMs, and key management tutorial](tutorial_signing_and_trust.md)**.
The rest of this page covers the catalog, YAML, and instance bring-up that
apply either way; §2 (trust policy) is the production path.

## 1. Stage the catalog

Copy each vendor bundle — the shared object, its manifest, its SPDX SBOM, and
(for production) its detached signature — into one catalog root:

```bash
sudo mkdir -p /opt/ocudu/dapps
sudo cp acme_eq.so acme_eq.manifest.json acme_eq.spdx.json acme_eq.manifest.json.sig.json \
        /opt/ocudu/dapps/
# repeat for other vendors / packages
```

Everything is admitted **relative to `catalog_root`**; the artifact path
inside the manifest and the SBOM's file entry are catalog-relative, and the
loader refuses symlinks or paths that escape the root.

## 2. Build the trust policy (production)

For production you hold the trust decision, not the vendor: an offline policy
file names each vendor's public key with a per-package sequence floor, and
`package_signature_mode: required` enforces it. That whole workflow —
`keygen`/`sign`/`trust`/`inspect`, what the signature covers, SBOM
verification, rotation and revocation, and key custody — has its own guide:

> **→ [Signing, SBOMs, and key management](tutorial_signing_and_trust.md)**

The short version for this page: produce the policy, point YAML at it with
`package_trust_store`, and set `package_signature_mode: required` (§3).

## 3. The `dapp:` YAML block

Minimum production shape:

```yaml
dapp:
  catalog_root: /opt/ocudu/dapps
  native_preflight_helper: /opt/ocudu/bin/ocudu-dapp-preflight  # absolute path, required
  package_signature_mode: required          # lab: allow_unsigned
  package_trust_store: /etc/ocudu/dapp-trust.json   # required when mode is 'required'
  package_manifests:
    - acme_eq.manifest.json
    - acme_spectrum.manifest.json

  # Optional: management endpoints
  local_e3:
    socket_path: /run/ocudu/dapp-management.sock   # SO_PEERCRED-checked, 0600
  e3ap_sctp:
    bind_address: 127.0.0.1     # loopback default IS the security boundary today
    port: 36423
    allowed_peers: []           # empty = admit all; set literal peer addresses to restrict
```

> **Security note (read before non-loopback binds).** E3AP has no TLS/DTLS or
> token authentication in this release, and `allowed_peers: []` admits every
> peer that can reach the port. The same association carries load/unload,
> config, and RAN control. Keep `bind_address` on loopback, or place an
> external boundary (IPsec/VPN/network policy) in front, until you have set a
> real `allowed_peers` list. See [security_model.md](security_model.md).

`package_signature_mode: allow_unsigned` is a **lab-only** convenience: it
forgives a *missing* signature sidecar, but a present-but-bad one always
fails.

## 4a. Bring instances up at startup (declarative)

Add `initial_instances[]` to have the DU load, prepare, arm, shadow, and (if
`target: active`) activate packages during startup — reversing the whole set
if any transition fails:

```yaml
dapp:
  catalog_root: /opt/ocudu/dapps
  native_preflight_helper: /opt/ocudu/bin/ocudu-dapp-preflight  # absolute path, required
  package_signature_mode: required
  package_trust_store: /etc/ocudu/dapp-trust.json
  package_manifests:
    - acme_eq.manifest.json
    - acme_spectrum.manifest.json
  initial_instances:
    - package_manifest: acme_eq.manifest.json
      placement: in_process          # Class A/B must be in_process
      backend: cuda                  # cpu | cuda | other; must match the manifest
      target: shadow                 # shadow = warmed, zero traffic exposure
      lifecycle_timeout_ms: 2000
    - package_manifest: acme_spectrum.manifest.json
      placement: out_of_process      # Class C may be supervised
      backend: cpu
      target: active
      lifecycle_timeout_ms: 2000
```

Every `initial_instances` entry must name a manifest already listed in
`package_manifests`. **`target: shadow` is the safe default for anything you
intend to A/B or validate** — a shadow instance accepts configuration and
model operations before it ever touches traffic; you activate it later over
E3 when the evidence looks right.

## 4b. Or start empty and load over E3 (interactive)

Leave `initial_instances` out entirely, start the gNB, and drive everything
with `dappctl` or the Python client. This is the usual path for experiments
and A/B tests:

```bash
sudo systemctl start ocudu-gnb    # or however you launch the gNB

# discover what the catalog offers and what is loaded
python3 apps/tools/dappctl/ocudu_dappctl.py \
  --socket /run/ocudu/dapp-management.sock inventory
```

Then follow the [dappctl operations tutorial](tutorial_dappctl_operations.md)
to load, arm, shadow, activate, swap versions, and manage breakers.

## 5. Confirm admission

> **Running `ocudu_e3`:** it is not pip-installed yet, so put the client on the
> path first: `export PYTHONPATH=/path/to/ocudu/python` and
> `export OCUDU_E3_SCHEMA_DIR=/path/to/ocudu/schemas/e3` (see `ocudu/python/README.md`).

```bash
# over the local socket (same-host) …
python3 apps/tools/dappctl/ocudu_dappctl.py --socket /run/ocudu/dapp-management.sock inventory
# … or over E3AP/SCTP
python3 -m ocudu_e3 --port 36423 catalog
```

`inventory` shows each admitted package with its verification status
(`sbom_verified`, signature status) and each live instance's state. If a
package is missing, check: catalog path containment, the signature/trust
policy (in `required` mode), and that the manifest is listed in
`package_manifests`.

## Troubleshooting quick table

| Symptom | Likely cause |
|---------|--------------|
| Package not in `inventory` | Not listed in `package_manifests`, or catalog path/symlink rejected |
| `required`-mode admission fails | Missing/invalid signature, key not in trust policy, or no sequence floor for the package |
| Instance won't reach `active` | Backend mismatch vs manifest, wrong placement for the class, or a failed preflight |
| Telemetry seems throttled | Non-zero hard-floor defaults — set `telemetry_<stream>_hard_floor_ms: 0` |
| Nothing reachable over E3AP | `bind_address` is loopback (intended) or `allowed_peers` excludes you |

Next: [operating a loaded dApp with `dappctl`](tutorial_dappctl_operations.md).
