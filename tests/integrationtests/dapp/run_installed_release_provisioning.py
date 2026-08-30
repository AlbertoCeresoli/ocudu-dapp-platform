#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear

"""Exercise the installed v1 package tool through rollover and live admission."""

import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import stat
import subprocess
import tempfile

import jsonschema


MANIFESTS = (
    "ref_spectrum_cpu.manifest.json",
    "ref_srs_isac_cpu.manifest.json",
    "ref_spectrumd_cpu.manifest.json",
    "ref_srs_isacd_cpu.manifest.json",
    "ref_spectrum_observerd_cpu.manifest.json",
    "ref_scheduler_cpu.manifest.json",
)
INITIAL_EPOCH = 41
OVERLAP_EPOCH = 42
FINAL_EPOCH = 43
INITIAL_SEQUENCE = 101
ROTATED_SEQUENCE = 201
SCHEMAS = (
    "package-signature-v1.schema.json",
    "package-trust-policy-v1.schema.json",
    "package-manifest-v1.schema.json",
    "package-spdx-sbom-profile-v1.schema.json",
    "release-provisioning-report-v1.schema.json",
)


def require(condition: bool, detail: str) -> None:
    if not condition:
        raise RuntimeError(detail)


def command(*arguments: object, success: bool = True) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(
        [str(argument) for argument in arguments],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if (result.returncode == 0) != success:
        raise RuntimeError(
            f"unexpected command result ({result.returncode}): {' '.join(result.args)}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    return result


def load_json(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def parse_key_id(output: str) -> str:
    for line in output.splitlines():
        if line.startswith("key-id=") and len(line.removeprefix("key-id=")) == 64:
            return line.removeprefix("key-id=")
    raise RuntimeError(f"package tool did not return a key ID:\n{output}")


def installed_record(prefix: Path, path: Path) -> dict:
    mode = stat.S_IMODE(path.stat().st_mode)
    require(path.is_file() and not path.is_symlink(), f"installed file is not regular: {path}")
    require((mode & 0o022) == 0, f"installed file is group/world writable: {path}")
    return {
        "relative_path": path.relative_to(prefix).as_posix(),
        "sha256": sha256(path),
        "mode": f"{mode:04o}",
    }


def stage_catalog(source: Path, destination: Path, validators: dict[str, jsonschema.Validator]) -> list[dict]:
    destination.mkdir(mode=0o700)
    packages = []
    copied = set()
    for manifest_name in MANIFESTS:
        source_manifest = source / manifest_name
        require(source_manifest.is_file(), f"SDK manifest is missing: {source_manifest}")
        manifest = load_json(source_manifest)
        validators["manifest"].validate(manifest)
        require(manifest["schema_version"] == 1, f"manifest is not v1: {source_manifest}")
        require(manifest["provenance"]["format"] == "spdx-2.3-json", f"unexpected SBOM format: {source_manifest}")
        artifact_name = manifest["artifact"]
        sbom_name = manifest["provenance"]["sbom"]
        source_artifact = source / artifact_name
        source_sbom = source / sbom_name
        require(source_artifact.is_file(), f"SDK artifact is missing: {source_artifact}")
        require(source_sbom.is_file(), f"SDK SBOM is missing: {source_sbom}")
        validators["sbom"].validate(load_json(source_sbom))
        require(sha256(source_artifact) == manifest["sha256"], f"artifact digest mismatch: {source_artifact}")
        require(sha256(source_sbom) == manifest["provenance"]["sha256"], f"SBOM digest mismatch: {source_sbom}")
        for source_path in (source_manifest, source_artifact, source_sbom):
            if source_path.name in copied:
                continue
            target = destination / source_path.name
            shutil.copy2(source_path, target)
            mode = stat.S_IMODE(source_path.stat().st_mode) & ~0o022
            target.chmod(mode)
            copied.add(source_path.name)
        packages.append(
            {
                "package_id": manifest["package_id"],
                "manifest": manifest_name,
                "manifest_sha256": sha256(source_manifest),
                "artifact_sha256": sha256(source_artifact),
                "sbom_sha256": sha256(source_sbom),
            }
        )
    return packages


def sign_catalog(tool: Path, catalog: Path, private_key: Path, first_sequence: int, signature_validator) -> None:
    for offset, manifest_name in enumerate(MANIFESTS):
        command(
            tool,
            "sign",
            "--catalog-root",
            catalog,
            "--manifest",
            manifest_name,
            "--private-key",
            private_key,
            "--package-sequence",
            first_sequence + offset,
        )
        signature_validator.validate(load_json(catalog / f"{manifest_name}.sig.json"))


def make_trust(
    tool: Path,
    output: Path,
    epoch: int,
    public_keys: tuple[Path, ...],
    packages: list[dict],
    first_sequence: int,
    trust_validator,
    revoked_key: str | None = None,
) -> None:
    arguments: list[object] = [tool, "trust", "--output", output, "--policy-epoch", epoch]
    for public_key in public_keys:
        arguments.extend(("--public-key", public_key))
    for offset, package in enumerate(packages):
        arguments.extend(("--package", f"{package['package_id']}:{first_sequence + offset}"))
    if revoked_key:
        arguments.extend(("--revoke-key-id", revoked_key))
    command(*arguments)
    trust_validator.validate(load_json(output))


def inspect_catalog(tool: Path, catalog: Path, trust: Path, epoch: int, success: bool = True) -> str:
    evidence = []
    for manifest_name in MANIFESTS:
        result = command(
            tool,
            "inspect",
            "--catalog-root",
            catalog,
            "--manifest",
            manifest_name,
            "--trust-store",
            trust,
            "--minimum-policy-epoch",
            epoch,
            success=success,
        )
        text = result.stdout if success else result.stderr
        evidence.append(text)
        if success:
            require("signature=verified" in text, f"signature was not verified:\n{text}")
            require("manifest-schema-version=1" in text, f"manifest v1 was not reported:\n{text}")
            require("sbom=verified" in text, f"SBOM was not verified:\n{text}")
    return "\n".join(evidence)


def write_report(path: Path, report: dict, validator) -> None:
    validator.validate(report)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f".{path.name}.{os.getpid()}.tmp")
    temporary.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    temporary.chmod(0o444)
    os.replace(temporary, path)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cmake-command", type=Path, required=True)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--source-catalog", type=Path, required=True)
    parser.add_argument("--driver", type=Path, required=True)
    parser.add_argument("--odu", type=Path, required=True)
    parser.add_argument("--client", type=Path, required=True)
    parser.add_argument("--e3-schema", type=Path, required=True)
    parser.add_argument("--preflight-helper", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="ocudu-dapp-release-provisioning-") as temporary:
        root = Path(temporary)
        install = root / "install"
        command(args.cmake_command, "--install", args.build_dir, "--prefix", install, "--component", "dapp_sdk")
        tool = install / "bin" / "ocudu-dapp-package"
        schema_dir = install / "share" / "ocudu" / "dapp" / "schema"
        installed_files = [tool, *(schema_dir / name for name in SCHEMAS)]
        for path in installed_files:
            require(path.is_file(), f"dapp_sdk component did not install {path}")
        require(os.access(tool, os.X_OK), f"installed package tool is not executable: {tool}")
        records = [installed_record(install, path) for path in installed_files]

        validators = {
            "signature": jsonschema.Draft202012Validator(load_json(schema_dir / SCHEMAS[0])),
            "trust": jsonschema.Draft202012Validator(load_json(schema_dir / SCHEMAS[1])),
            "manifest": jsonschema.Draft202012Validator(load_json(schema_dir / SCHEMAS[2])),
            "sbom": jsonschema.Draft202012Validator(load_json(schema_dir / SCHEMAS[3])),
            "report": jsonschema.Draft202012Validator(
                load_json(schema_dir / SCHEMAS[4]), format_checker=jsonschema.FormatChecker()
            ),
        }
        for validator in validators.values():
            validator.check_schema(validator.schema)

        old_catalog, new_catalog, low_catalog = root / "catalog-old", root / "catalog-new", root / "catalog-low"
        packages = stage_catalog(args.source_catalog, old_catalog, validators)
        stage_catalog(args.source_catalog, new_catalog, validators)
        stage_catalog(args.source_catalog, low_catalog, validators)

        old_private, old_public = root / "old.pem", root / "old.pub.pem"
        new_private, new_public = root / "new.pem", root / "new.pub.pem"
        old_key_id = parse_key_id(
            command(tool, "keygen", "--private-key", old_private, "--public-key", old_public).stdout
        )
        new_key_id = parse_key_id(
            command(tool, "keygen", "--private-key", new_private, "--public-key", new_public).stdout
        )
        require(old_key_id != new_key_id, "rollover generated duplicate key identities")
        require(stat.S_IMODE(old_private.stat().st_mode) == 0o600, "old private key mode is not 0600")
        require(stat.S_IMODE(new_private.stat().st_mode) == 0o600, "new private key mode is not 0600")

        sign_catalog(tool, old_catalog, old_private, INITIAL_SEQUENCE, validators["signature"])
        sign_catalog(tool, new_catalog, new_private, ROTATED_SEQUENCE, validators["signature"])
        sign_catalog(tool, low_catalog, new_private, INITIAL_SEQUENCE, validators["signature"])

        initial_trust = root / "trust-initial.json"
        overlap_trust = root / "trust-overlap.json"
        final_trust = root / "trust-final.json"
        make_trust(
            tool, initial_trust, INITIAL_EPOCH, (old_public,), packages, INITIAL_SEQUENCE, validators["trust"]
        )
        make_trust(
            tool,
            overlap_trust,
            OVERLAP_EPOCH,
            (old_public, new_public),
            packages,
            INITIAL_SEQUENCE,
            validators["trust"],
        )
        make_trust(
            tool,
            final_trust,
            FINAL_EPOCH,
            (old_public, new_public),
            packages,
            ROTATED_SEQUENCE,
            validators["trust"],
            revoked_key=old_key_id,
        )

        inspect_catalog(tool, old_catalog, initial_trust, INITIAL_EPOCH)
        inspect_catalog(tool, old_catalog, overlap_trust, OVERLAP_EPOCH)
        inspect_catalog(tool, new_catalog, overlap_trust, OVERLAP_EPOCH)
        inspect_catalog(tool, new_catalog, final_trust, FINAL_EPOCH)
        revoked = inspect_catalog(tool, old_catalog, final_trust, FINAL_EPOCH, success=False)
        require("revoked" in revoked, f"old key rejection was not attributable to revocation:\n{revoked}")
        floored = inspect_catalog(tool, low_catalog, final_trust, FINAL_EPOCH, success=False)
        require("below the trusted minimum" in floored, f"sequence rejection was not attributable:\n{floored}")
        rollback = inspect_catalog(tool, new_catalog, overlap_trust, FINAL_EPOCH, success=False)
        require("epoch is below" in rollback, f"policy rollback rejection was not attributable:\n{rollback}")

        old_private.unlink()
        new_private.unlink()
        require(
            not old_private.exists() and not new_private.exists(),
            "offline private keys were retained after signing",
        )

        command(
            "python3",
            args.driver,
            "--odu",
            args.odu,
            "--client",
            args.client,
            "--schema",
            args.e3_schema,
            "--catalog",
            new_catalog,
            "--preflight-helper",
            args.preflight_helper,
            "--package-trust-store",
            final_trust,
            "--minimum-trust-policy-epoch",
            FINAL_EPOCH,
        )

        report = {
            "SPDX-FileCopyrightText": "Copyright (C) 2026 DeepSig Inc.",
            "SPDX-License-Identifier": "BSD-3-Clause-Clear",
            "schema": "ocudu.dapp.release-provisioning-report.v1",
            "status": "passed",
            "published_utc": datetime.datetime.now(datetime.timezone.utc).isoformat().replace("+00:00", "Z"),
            "platform": {
                "system": platform.system(),
                "architecture": platform.machine(),
                "kernel_release": platform.release(),
            },
            "installation": {"component": "dapp_sdk", "files": records},
            "catalog": {
                "package_count": len(packages),
                "manifest_schema_version": 1,
                "spdx_format": "spdx-2.3-json",
                "packages": packages,
            },
            "keys": {
                "algorithm": "ecdsa-p256-sha256",
                "old_key_id": old_key_id,
                "new_key_id": new_key_id,
                "private_key_mode": "0600",
                "private_keys_retained": False,
            },
            "phases": [
                {
                    "name": "initial_old_key_admission",
                    "status": "passed",
                    "evidence": "epoch 41 admitted all six old-key packages",
                },
                {
                    "name": "overlap_old_catalog_admission",
                    "status": "passed",
                    "evidence": "epoch 42 admitted the old catalog during overlap",
                },
                {
                    "name": "overlap_new_catalog_admission",
                    "status": "passed",
                    "evidence": "epoch 42 admitted the higher-sequence new-key catalog",
                },
                {
                    "name": "old_key_revocation_rejection",
                    "status": "passed",
                    "evidence": "epoch 43 rejected every old-key signature as revoked",
                },
                {
                    "name": "sequence_floor_rejection",
                    "status": "passed",
                    "evidence": "epoch 43 rejected every below-floor new-key signature",
                },
                {
                    "name": "policy_epoch_rollback_rejection",
                    "status": "passed",
                    "evidence": "minimum epoch 43 rejected the epoch 42 policy",
                },
                {
                    "name": "final_catalog_live_lifecycle",
                    "status": "passed",
                    "evidence": "ordinary dummy-RU bootstrap and E3 lifecycle admitted the rotated catalog",
                },
            ],
            "limitations": [
                "This report qualifies only the recorded platform and installed build; "
                "it is not a multi-platform release claim.",
                "The v1 exercise uses temporary file-backed keys; production custody "
                "requires an approved offline facility and two-person operator procedure.",
                "The test proves software ceremony semantics, not organizational authorization "
                "or hardware-security-module integration.",
            ],
        }
        write_report(args.output, report, validators["report"])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
