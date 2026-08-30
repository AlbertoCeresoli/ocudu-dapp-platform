#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear

import argparse
import hashlib
import json
import os
import shutil
import stat
import subprocess
import tempfile
from pathlib import Path

import jsonschema


PACKAGE_ID = "00112233-4455-6677-8899-aabbccddeeff"


def run(tool: Path, *arguments: object, success: bool = True) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(
        [str(tool), *(str(item) for item in arguments)],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if (result.returncode == 0) != success:
        raise AssertionError(
            f"unexpected command result ({result.returncode}): {' '.join(result.args)}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    return result


def controlled_write(path: Path, content: str, mode: int = 0o444) -> None:
    if path.exists() or path.is_symlink():
        path.chmod(0o644, follow_symlinks=False)
    path.write_text(content, encoding="utf-8")
    path.chmod(mode)


def manifest(artifact: Path) -> str:
    document = {
        "schema_version": 1,
        "package_id": PACKAGE_ID,
        "global_name": "org.deepsig.ocudu.test.valid",
        "vendor": "DeepSig",
        "version": "1.0.0",
        "artifact": artifact.name,
        "sha256": hashlib.sha256(artifact.read_bytes()).hexdigest(),
        "placements": ["in_process"],
        "backends": ["cpu"],
        "interfaces": [
            {
                "interface_id": 0x1234,
                "major": 1,
                "minor": 0,
                "class": "C",
                "placements": ["in_process"],
                "threading": "reentrant_shared",
                "capabilities": 0,
            }
        ],
    }
    return json.dumps(document, sort_keys=True, separators=(",", ":")) + "\n"


def build_trust(
    tool: Path,
    output: Path,
    public_keys: list[Path],
    minimum: int = 11,
    epoch: int = 4,
    revoked: list[str] | None = None,
    success: bool = True,
) -> subprocess.CompletedProcess[str]:
    arguments: list[object] = ["trust", "--output", output, "--policy-epoch", epoch]
    for key in public_keys:
        arguments.extend(("--public-key", key))
    arguments.extend(("--package", f"{PACKAGE_ID}:{minimum}"))
    for key_id in revoked or []:
        arguments.extend(("--revoke-key-id", key_id))
    return run(tool, *arguments, success=success)


def inspect(
    tool: Path,
    catalog: Path,
    trust: Path,
    minimum_epoch: int = 4,
    allow_unsigned: bool = False,
    manifest_name: str = "package.json",
    success: bool = True,
) -> subprocess.CompletedProcess[str]:
    arguments: list[object] = [
        "inspect",
        "--catalog-root",
        catalog,
        "--manifest",
        manifest_name,
        "--trust-store",
        trust,
        "--minimum-policy-epoch",
        minimum_epoch,
    ]
    if allow_unsigned:
        arguments.append("--allow-unsigned")
    return run(tool, *arguments, success=success)


def key_id(output: str) -> str:
    for line in output.splitlines():
        if line.startswith("key-id="):
            value = line.removeprefix("key-id=")
            if len(value) == 64:
                return value
    raise AssertionError(f"tool did not publish a key ID: {output}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--tool", type=Path, required=True)
    parser.add_argument("--artifact", type=Path, required=True)
    parser.add_argument("--signature-schema", type=Path, required=True)
    parser.add_argument("--trust-schema", type=Path, required=True)
    args = parser.parse_args()

    signature_validator = jsonschema.Draft202012Validator(json.loads(args.signature_schema.read_text()))
    trust_validator = jsonschema.Draft202012Validator(json.loads(args.trust_schema.read_text()))
    signature_validator.check_schema(signature_validator.schema)
    trust_validator.check_schema(trust_validator.schema)

    with tempfile.TemporaryDirectory(prefix="ocudu-dapp-signing-") as temporary:
        root = Path(temporary)
        catalog = root / "catalog"
        catalog.mkdir()
        artifact = catalog / args.artifact.name
        shutil.copy2(args.artifact, artifact)
        artifact.chmod(0o555)
        artifact_bytes = artifact.read_bytes()
        manifest_path = catalog / "package.json"
        controlled_write(manifest_path, manifest(artifact))
        original_manifest = manifest_path.read_text()
        sidecar = Path(f"{manifest_path}.sig.json")

        private1, public1 = root / "key1.pem", root / "key1.pub.pem"
        private2, public2 = root / "key2.pem", root / "key2.pub.pem"
        generated1 = run(args.tool, "keygen", "--private-key", private1, "--public-key", public1)
        generated2 = run(args.tool, "keygen", "--private-key", private2, "--public-key", public2)
        key1, key2 = key_id(generated1.stdout), key_id(generated2.stdout)
        assert key1 != key2
        assert stat.S_IMODE(private1.stat().st_mode) == 0o600
        assert stat.S_IMODE(public1.stat().st_mode) == 0o444
        run(args.tool, "keygen", "--private-key", private1, "--public-key", root / "unused.pub", success=False)

        signed = run(
            args.tool,
            "sign",
            "--catalog-root",
            catalog,
            "--manifest",
            manifest_path.name,
            "--private-key",
            private1,
            "--package-sequence",
            11,
        )
        assert f"key-id={key1}" in signed.stdout
        signature_validator.validate(json.loads(sidecar.read_text()))
        run(
            args.tool,
            "sign",
            "--catalog-root",
            catalog,
            "--manifest",
            manifest_path.name,
            "--private-key",
            private1,
            "--package-sequence",
            11,
            success=False,
        )

        trust1 = root / "trust1.json"
        build_trust(args.tool, trust1, [public1])
        trust_validator.validate(json.loads(trust1.read_text()))
        inspected = inspect(args.tool, catalog, trust1)
        assert "signature=verified" in inspected.stdout
        assert f"key-id={key1}" in inspected.stdout
        build_trust(args.tool, trust1, [public1], success=False)

        controlled_write(manifest_path, original_manifest + " ")
        inspect(args.tool, catalog, trust1, success=False)
        controlled_write(manifest_path, original_manifest)

        artifact.chmod(0o755)
        artifact.write_bytes(artifact_bytes + b"tamper")
        artifact.chmod(0o555)
        inspect(args.tool, catalog, trust1, success=False)
        artifact.chmod(0o755)
        artifact.write_bytes(artifact_bytes)
        artifact.chmod(0o555)

        saved_sidecar = sidecar.read_text()
        sidecar.unlink()
        inspect(args.tool, catalog, trust1, success=False)
        unsigned = inspect(args.tool, catalog, trust1, allow_unsigned=True)
        assert "signature=unsigned" in unsigned.stdout
        controlled_write(sidecar, "not-json\n")
        inspect(args.tool, catalog, trust1, allow_unsigned=True, success=False)

        controlled_write(sidecar, saved_sidecar)
        trust_unknown = root / "trust-unknown.json"
        build_trust(args.tool, trust_unknown, [public2])
        inspect(args.tool, catalog, trust_unknown, success=False)

        sidecar.unlink()
        run(
            args.tool,
            "sign",
            "--catalog-root",
            catalog,
            "--manifest",
            manifest_path.name,
            "--private-key",
            private2,
            "--package-sequence",
            12,
        )
        trust_rotation = root / "trust-rotation.json"
        build_trust(args.tool, trust_rotation, [public1, public2])
        rotated = inspect(args.tool, catalog, trust_rotation)
        assert f"key-id={key2}" in rotated.stdout and "sequence=12" in rotated.stdout

        trust_revoked = root / "trust-revoked.json"
        build_trust(args.tool, trust_revoked, [public1, public2], revoked=[key2])
        inspect(args.tool, catalog, trust_revoked, success=False)
        trust_floor = root / "trust-floor.json"
        build_trust(args.tool, trust_floor, [public1, public2], minimum=13)
        inspect(args.tool, catalog, trust_floor, success=False)
        inspect(args.tool, catalog, trust_rotation, minimum_epoch=5, success=False)

        duplicate_key = root / "trust-duplicate-key.json"
        build_trust(args.tool, duplicate_key, [public1, public1], success=False)
        duplicate_package = root / "trust-duplicate-package.json"
        run(
            args.tool,
            "trust",
            "--output",
            duplicate_package,
            "--policy-epoch",
            4,
            "--public-key",
            public1,
            "--package",
            f"{PACKAGE_ID}:11",
            "--package",
            f"{PACKAGE_ID}:12",
            success=False,
        )

        outside = root / "outside"
        outside.mkdir()
        outside_artifact = outside / artifact.name
        shutil.copy2(artifact, outside_artifact)
        outside_artifact.chmod(0o555)
        outside_manifest = outside / "package.json"
        controlled_write(outside_manifest, manifest(outside_artifact))
        run(
            args.tool,
            "sign",
            "--catalog-root",
            outside,
            "--manifest",
            outside_manifest.name,
            "--private-key",
            private1,
            "--package-sequence",
            11,
        )
        os.symlink(outside, catalog / "linked")
        inspect(args.tool, catalog, trust1, manifest_name="linked/package.json", success=False)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
