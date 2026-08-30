#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear

"""Sign an independently built SDK catalog, then run its ordinary DU lifecycle."""

import argparse
import json
from pathlib import Path
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
POLICY_EPOCH = 7
FIRST_SEQUENCE = 101


def require(condition, detail):
    if not condition:
        raise RuntimeError(detail)


def command(*arguments, capture=False):
    result = subprocess.run(
        [str(argument) for argument in arguments],
        text=True,
        stdout=subprocess.PIPE if capture else None,
        stderr=subprocess.PIPE if capture else None,
        check=False,
    )
    require(
        result.returncode == 0,
        f"command failed ({result.returncode}): {' '.join(result.args)}\n"
        f"stdout:\n{result.stdout or ''}\nstderr:\n{result.stderr or ''}",
    )
    return result


def command_must_fail(*arguments):
    result = subprocess.run(
        [str(argument) for argument in arguments],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    require(result.returncode != 0, f"command unexpectedly succeeded: {' '.join(result.args)}")
    return result


def load_json(path):
    return json.loads(path.read_text(encoding="utf-8"))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--driver", type=Path, required=True)
    parser.add_argument("--package-tool", type=Path, required=True)
    parser.add_argument("--source-catalog", type=Path, required=True)
    parser.add_argument("--odu", type=Path, required=True)
    parser.add_argument("--client", type=Path, required=True)
    parser.add_argument("--schema", type=Path, required=True)
    parser.add_argument("--preflight-helper", type=Path, required=True)
    parser.add_argument("--signature-schema", type=Path, required=True)
    parser.add_argument("--trust-schema", type=Path, required=True)
    parser.add_argument("--manifest-schema", type=Path, required=True)
    parser.add_argument("--sbom-schema", type=Path, required=True)
    parser.add_argument("--initial-startup", action="store_true")
    args = parser.parse_args()

    signature_validator = jsonschema.Draft202012Validator(load_json(args.signature_schema))
    trust_validator = jsonschema.Draft202012Validator(load_json(args.trust_schema))
    manifest_validator = jsonschema.Draft202012Validator(load_json(args.manifest_schema))
    sbom_validator = jsonschema.Draft202012Validator(load_json(args.sbom_schema))
    signature_validator.check_schema(signature_validator.schema)
    trust_validator.check_schema(trust_validator.schema)
    manifest_validator.check_schema(manifest_validator.schema)
    sbom_validator.check_schema(sbom_validator.schema)

    with tempfile.TemporaryDirectory(prefix="ocudu-dapp-signed-catalog-") as temporary:
        root = Path(temporary)
        catalog = root / "catalog"
        catalog.mkdir(mode=0o700)
        package_floors = []

        for manifest_name in MANIFESTS:
            source_manifest = args.source_catalog / manifest_name
            require(source_manifest.is_file(), f"SDK manifest is missing: {source_manifest}")
            document = load_json(source_manifest)
            artifact_name = document.get("artifact")
            package_id = document.get("package_id")
            provenance = document.get("provenance")
            require(isinstance(artifact_name, str), f"manifest has no artifact: {source_manifest}")
            require(isinstance(package_id, str), f"manifest has no package ID: {source_manifest}")
            require(document.get("schema_version") == 1, f"manifest is not schema v1: {source_manifest}")
            require(isinstance(provenance, dict), f"manifest has no provenance: {source_manifest}")
            sbom_name = provenance.get("sbom")
            require(isinstance(sbom_name, str), f"manifest has no SBOM: {source_manifest}")
            source_artifact = args.source_catalog / artifact_name
            source_sbom = args.source_catalog / sbom_name
            require(source_artifact.is_file(), f"SDK artifact is missing: {source_artifact}")
            require(source_sbom.is_file(), f"SDK SBOM is missing: {source_sbom}")
            manifest_validator.validate(document)
            sbom_validator.validate(load_json(source_sbom))
            shutil.copy2(source_manifest, catalog / manifest_name)
            shutil.copy2(source_artifact, catalog / artifact_name)
            shutil.copy2(source_sbom, catalog / sbom_name)
            (catalog / manifest_name).chmod(0o444)
            (catalog / sbom_name).chmod(0o444)
            artifact_mode = stat.S_IMODE(source_artifact.stat().st_mode)
            (catalog / artifact_name).chmod(artifact_mode & ~0o022)
            package_floors.append((package_id, FIRST_SEQUENCE + len(package_floors)))

        private_key = root / "operator-signing-key.pem"
        public_key = root / "operator-signing-key.pub.pem"
        command(
            args.package_tool,
            "keygen",
            "--private-key",
            private_key,
            "--public-key",
            public_key,
            capture=True,
        )

        for manifest_name, (_, sequence) in zip(MANIFESTS, package_floors):
            command(
                args.package_tool,
                "sign",
                "--catalog-root",
                catalog,
                "--manifest",
                manifest_name,
                "--private-key",
                private_key,
                "--package-sequence",
                sequence,
                capture=True,
            )
            signature_validator.validate(load_json(catalog / f"{manifest_name}.sig.json"))

        trust_store = root / "package-trust-policy.json"
        trust_arguments = [
            args.package_tool,
            "trust",
            "--output",
            trust_store,
            "--policy-epoch",
            POLICY_EPOCH,
            "--public-key",
            public_key,
        ]
        for package_id, sequence in package_floors:
            trust_arguments.extend(("--package", f"{package_id}:{sequence}"))
        command(*trust_arguments, capture=True)
        trust_validator.validate(load_json(trust_store))

        for manifest_name in MANIFESTS:
            inspection = command(
                args.package_tool,
                "inspect",
                "--catalog-root",
                catalog,
                "--manifest",
                manifest_name,
                "--trust-store",
                trust_store,
                "--minimum-policy-epoch",
                POLICY_EPOCH,
                capture=True,
            )
            require(
                "manifest-schema-version=1" in inspection.stdout,
                f"package inspection lost manifest v1:\n{inspection.stdout}",
            )
            require("sbom=verified" in inspection.stdout, f"package inspection did not verify the SBOM:\n{inspection.stdout}")

        tamper_manifest = load_json(catalog / MANIFESTS[0])
        tamper_sbom = catalog / tamper_manifest["provenance"]["sbom"]
        original_sbom = tamper_sbom.read_bytes()
        tamper_sbom.chmod(0o644)
        tamper_sbom.write_bytes(original_sbom + b" ")
        tamper_sbom.chmod(0o444)
        failure = command_must_fail(
            args.package_tool,
            "inspect",
            "--catalog-root",
            catalog,
            "--manifest",
            MANIFESTS[0],
            "--trust-store",
            trust_store,
            "--minimum-policy-epoch",
            POLICY_EPOCH,
        )
        require("SBOM SHA-256" in failure.stderr, "SBOM tamper rejection was not attributable")
        tamper_sbom.chmod(0o644)
        tamper_sbom.write_bytes(original_sbom)
        tamper_sbom.chmod(0o444)

        driver_arguments = [
            "python3",
            args.driver,
            "--odu",
            args.odu,
            "--client",
            args.client,
            "--schema",
            args.schema,
            "--catalog",
            catalog,
            "--preflight-helper",
            args.preflight_helper,
            "--package-trust-store",
            trust_store,
            "--minimum-trust-policy-epoch",
            POLICY_EPOCH,
        ]
        if args.initial_startup:
            driver_arguments.append("--initial-startup")
        command(*driver_arguments)


if __name__ == "__main__":
    main()
