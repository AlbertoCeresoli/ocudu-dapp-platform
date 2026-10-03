#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear
"""Regenerate the pinned libe3 wire vectors from the real upstream encoder.

Runs ``libe3_interop_vectors encode`` (upstream libe3's own Asn1E3Encoder, and its
ProtobufE3Encoder when libe3 was built with Protobuf), decodes every vector back with
``libe3_interop_vectors decode`` for the field view, and rewrites

* ``python/tests/fixtures/libe3_vectors.json`` (name, hex, decoded fields; APER under
  ``vectors``, Protobuf under ``protobuf_vectors``),
* ``tests/unittests/dapp/e3ap_libe3_vectors.inc`` (name, APER hex for the C++ codec test), and
* ``tests/unittests/dapp/e3ap_libe3_protobuf_vectors.inc`` (name, Protobuf hex).

Both encodings carry the same content, so libe3 must decode them to the same fields.

Run it after bumping ``third_party/libe3`` (a wire change shows up as a diff in both files)::

    python3 scripts/regen_libe3_vectors.py --vectors build/tests/unittests/dapp/interop/libe3/libe3_interop_vectors
"""
from __future__ import annotations

import argparse
import json
import pathlib
import subprocess

REPO = pathlib.Path(__file__).resolve().parents[1]
FIXTURE = REPO / "python" / "tests" / "fixtures" / "libe3_vectors.json"
INCLUDE = REPO / "tests" / "unittests" / "dapp" / "e3ap_libe3_vectors.inc"
PROTOBUF_INCLUDE = REPO / "tests" / "unittests" / "dapp" / "e3ap_libe3_protobuf_vectors.inc"


def libe3_vectors(tool: str, encoding: str) -> list[dict]:
    encoded = subprocess.run([tool, "encode", encoding], capture_output=True, text=True, check=True).stdout
    vectors = []
    for row in (json.loads(line) for line in encoded.splitlines() if line.strip()):
        decoded = subprocess.run([tool, "decode", row["hex"], encoding], capture_output=True, text=True,
                                 check=True).stdout
        fields = json.loads(decoded)
        if "error" in fields:
            raise SystemExit(f"libe3 cannot decode its own {encoding} {row['name']} vector: {fields['error']}")
        vectors.append({"name": row["name"], "hex": row["hex"], "fields": fields})
    return vectors


def write_include(path: pathlib.Path, vectors: list[dict], what: str) -> None:
    lines = [f"// Generated from python/tests/fixtures/libe3_vectors.json ({what}) by scripts/regen_libe3_vectors.py."]
    lines += [f'{{"{vector["name"]}", "{vector["hex"]}"}},' for vector in vectors]
    path.write_text("\n".join(lines) + "\n")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--vectors", required=True, help="path to libe3_interop_vectors")
    args = parser.parse_args()

    libe3 = REPO / "third_party" / "libe3"
    version = (libe3 / "VERSION").read_text().strip()
    commit = subprocess.run(["git", "-C", str(libe3), "rev-parse", "--short", "HEAD"], capture_output=True,
                            text=True, check=True).stdout.strip()
    asn1c = subprocess.run(["git", "-C", str(REPO / "third_party" / "asn1c"), "rev-parse", "--short", "HEAD"],
                           capture_output=True, text=True, check=False).stdout.strip() or "unknown"

    vectors = libe3_vectors(args.vectors, "asn1")
    protobuf = libe3_vectors(args.vectors, "protobuf")
    for aper, proto in zip(vectors, protobuf, strict=True):
        if aper["fields"] != proto["fields"]:
            raise SystemExit(f"libe3 decodes its APER and Protobuf {aper['name']} vectors differently")
    first = vectors[0]["fields"]
    FIXTURE.write_text(json.dumps({
        "source": f"libe3 {version} (wineslab/libe3 {commit}) Asn1E3Encoder and ProtobufE3Encoder via "
                  f"tests/interop/libe3/libe3_interop_vectors encode; asn1c mouse07410 {asn1c}",
        "message_id": first["id"],
        "timestamp": first["timestamp"],
        "vectors": vectors,
        "protobuf_vectors": protobuf,
    }, indent=1) + "\n")
    write_include(INCLUDE, vectors, "APER; see e3ap_libe3_codec_test.cpp")
    write_include(PROTOBUF_INCLUDE, protobuf, "Protobuf; see e3ap_protobuf_codec_test.cpp")
    print(f"{len(vectors)} APER + {len(protobuf)} Protobuf vectors from libe3 {version} ({commit}) -> "
          f"{FIXTURE.relative_to(REPO)}, {INCLUDE.relative_to(REPO)}, {PROTOBUF_INCLUDE.relative_to(REPO)}")


if __name__ == "__main__":
    main()
