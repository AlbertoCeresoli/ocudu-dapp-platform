#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear
"""End-to-end OCUDU <-> libe3 interoperability, with the real upstream library.

Two checks, each driven by a binary that links upstream libe3 only (built by
``tests/interop/libe3`` when ``OCUDU_DAPP_LIBE3_INTEROP=ON``):

* ``libe3_interop_dapp`` (libe3 dApp role over ``E3LinkLayer::SCTP_ASSOCIATION``)
  attaches to ``e3ap_test_server`` and runs setup, subscription, indications,
  a management control action, a report, subscription delete and release.
* ``libe3_interop_vectors`` encodes every procedure with libe3's encoder and
  decodes what asn1tools (vendored libe3 module) emits; the encode output must
  also match the pinned ``fixtures/libe3_vectors.json`` so drift is visible.

Run::

    python3 python/tests/test_libe3_e2e.py --server build/tests/unittests/dapp/e3ap_test_server \\
        --dapp build/tests/interop/libe3/libe3_interop_dapp \\
        --vectors build/tests/interop/libe3/libe3_interop_vectors
"""
from __future__ import annotations

import argparse
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import time
import unittest

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parents[1]
sys.path.insert(0, str(REPO / "python"))

import asn1tools  # noqa: E402
from ocudu_e3 import management  # noqa: E402
from ocudu_e3.schema import load_codec  # noqa: E402

SERVER = os.environ.get("OCUDU_E3AP_TEST_SERVER")
DAPP = os.environ.get("OCUDU_LIBE3_INTEROP_DAPP")
VECTORS = os.environ.get("OCUDU_LIBE3_INTEROP_VECTORS")

# The skip conditions below are evaluated when the classes are defined, so the
# command line has to be read before that point, not in __main__.
_REMAINING_ARGV = [sys.argv[0]]
if __name__ == "__main__":
    _parser = argparse.ArgumentParser()
    _parser.add_argument("--server", help="path to e3ap_test_server")
    _parser.add_argument("--dapp", help="path to libe3_interop_dapp")
    _parser.add_argument("--vectors", help="path to libe3_interop_vectors")
    _args, _rest = _parser.parse_known_args()
    _REMAINING_ARGV += _rest
    SERVER = _args.server or SERVER
    DAPP = _args.dapp or DAPP
    VECTORS = _args.vectors or VECTORS
LIBE3_MODULE = REPO / "schemas" / "e3" / "interop" / "libe3" / "e3ap-1.0.0.asn1"
PINNED = HERE / "fixtures" / "libe3_vectors.json"


@unittest.skipUnless(SERVER and DAPP, "needs --server and --dapp (or the OCUDU_* environment)")
class LibE3DAppAgainstOcuduAgent(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tempdir = tempfile.TemporaryDirectory()
        cls.process = subprocess.Popen(
            [SERVER, "--catalog", cls.tempdir.name, "--period-ms", "5"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        cls.e3ap_port = 0
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            line = cls.process.stdout.readline()
            if not line:
                break
            if line.startswith("e3ap_port"):
                cls.e3ap_port = int(line.split()[1])
            elif line.startswith("ready"):
                break
        if cls.e3ap_port == 0:
            cls.process.kill()
            raise RuntimeError("test agent did not report its E3AP port: " + cls.process.stderr.read())

    @classmethod
    def tearDownClass(cls):
        try:
            cls.process.stdin.write("quit\n")
            cls.process.stdin.flush()
            cls.process.wait(timeout=10)
        except Exception:
            cls.process.kill()
        cls.tempdir.cleanup()

    def run_dapp(self, *extra: str) -> dict:
        completed = subprocess.run(
            [DAPP, "--port", str(self.e3ap_port), *extra],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=60,
        )
        lines = [line for line in completed.stdout.splitlines() if line.startswith("{")]
        self.assertTrue(lines, "dApp printed no JSON summary: " + completed.stderr[-2000:])
        summary = json.loads(lines[-1])
        summary["_exit"] = completed.returncode
        return summary

    def test_full_session_over_one_sctp_association(self):
        codec = load_codec(os.environ.get("OCUDU_E3_SCHEMA_DIR"))
        control = codec.encode("ManagementRequest", management.inventory_request())
        summary = self.run_dapp("--telemetry", "2", "--indications", "3", "--control-hex", control.hex())
        self.assertTrue(summary["setup"], summary)
        self.assertGreaterEqual(summary["dapp_id"], 1)
        self.assertLessEqual(summary["dapp_id"], 100, "libe3 dApp identifiers are 1..100")
        self.assertEqual(summary["ran_identifier"], "ocudu-e3-test-agent")
        self.assertEqual(summary["ran_functions"], [10, 11])
        self.assertEqual(summary["telemetry_ids"], [1, 2, 3, 4, 5, 6, 7, 8])
        self.assertEqual(summary["control_ids"], [1])
        self.assertTrue(summary["subscribed"], summary)
        self.assertLessEqual(summary["subscription_id"], 100)
        self.assertGreaterEqual(summary["indications"], 3)
        for size in summary["indication_bytes"]:
            self.assertGreater(size, 0)
            self.assertLessEqual(size, 32768)
        self.assertEqual(summary["control_ack"], "positive", "management inventory query over libe3")
        self.assertEqual(summary["report_ack"], "positive")
        self.assertTrue(summary["unsubscribed"], "delete confirmed by a SubscriptionResponse")
        self.assertTrue(summary["released"])
        self.assertTrue(summary["ok"])
        self.assertEqual(summary["_exit"], 0)

    def test_two_libe3_dapps_in_a_row_get_fresh_identifiers(self):
        first = self.run_dapp("--indications", "1")
        second = self.run_dapp("--indications", "1")
        self.assertTrue(first["ok"] and second["ok"], (first, second))
        # The released identifier is returned to the pool, so both sessions see
        # the same (top of pool) identifier; what matters is that setup
        # succeeded twice without leaking one.
        self.assertEqual(first["dapp_id"], second["dapp_id"])

    def test_catalog_admission_extension_over_real_libe3_control_action(self):
        # The positive carrier ACK means the bounded management job was
        # accepted, not that an unsigned/unconfigured package was admitted.
        codec = load_codec(os.environ.get("OCUDU_E3_SCHEMA_DIR"))
        request = management.catalog_admit_request(bytes.fromhex("00112233445566778899aabbccddeeff"), dry_run=True)
        control = codec.encode("ManagementRequest", request)
        summary = self.run_dapp("--indications", "1", "--control-hex", control.hex())
        self.assertTrue(summary["setup"], summary)
        self.assertEqual(summary["control_ack"], "positive", summary)
        self.assertTrue(summary["released"], summary)
        self.assertTrue(summary["ok"], summary)


@unittest.skipUnless(VECTORS, "needs --vectors (or OCUDU_LIBE3_INTEROP_VECTORS)")
class LibE3EncoderAgainstAsn1tools(unittest.TestCase):
    """The upstream encoder binary versus asn1tools on the vendored module."""

    @classmethod
    def setUpClass(cls):
        cls.libe3 = asn1tools.compile_files([str(LIBE3_MODULE)], "per")
        cls.pinned = json.loads(PINNED.read_text())

    def encode_all(self) -> list[dict]:
        output = subprocess.run([VECTORS, "encode"], stdout=subprocess.PIPE, text=True, check=True).stdout
        return [json.loads(line) for line in output.splitlines() if line.strip()]

    def decode_one(self, hex_text: str) -> dict:
        output = subprocess.run([VECTORS, "decode", hex_text], stdout=subprocess.PIPE, text=True).stdout
        return json.loads(output.strip() or '{"error": "no output"}')

    def test_pinned_vectors_are_current(self):
        fresh = {row["name"]: row["hex"] for row in self.encode_all()}
        pinned = {row["name"]: row["hex"] for row in self.pinned["vectors"]}
        self.assertEqual(fresh, pinned, "regenerate fixtures/libe3_vectors.json and e3ap_libe3_vectors.inc")

    def test_libe3_decodes_what_asn1tools_encodes(self):
        """The nine procedures without UTF8String cross from asn1tools to libe3
        field for field; SetupRequest/SetupResponse are the documented
        asn1tools divergence (see test_libe3_interop.py)."""
        for vector in self.pinned["vectors"]:
            if vector["name"] in ("SetupRequest", "SetupResponse"):
                continue
            with self.subTest(vector["name"]):
                decoded = self.libe3.decode("E3-PDU", bytes.fromhex(vector["hex"]))
                re_encoded = self.libe3.encode("E3-PDU", decoded)
                seen = self.decode_one(re_encoded.hex())
                self.assertNotIn("error", seen, vector["name"])
                self.assertEqual(seen["id"], self.pinned["message_id"])
                self.assertEqual(seen["timestamp"], self.pinned["timestamp"])
                self.assertEqual(seen, vector["fields"])

    def test_libe3_rejects_ocudu_native_setup_bytes(self):
        """The two modules are not confusable at setup: OCUDU's native setup
        request is not a libe3 PDU (the agent relies on the converse too)."""
        ocudu = load_codec(os.environ.get("OCUDU_E3_SCHEMA_DIR"))
        native = ocudu.encode_pdu(1, "setupRequest", {"e3apProtocolVersion": b"1.0.0", "dAppName": b"x", "dAppVersion": b"0", "vendor": b"v"})
        seen = self.decode_one(native.hex())
        self.assertTrue("error" in seen or seen.get("name") != "SetupRequest", seen)


if __name__ == "__main__":
    unittest.main(argv=_REMAINING_ARGV, verbosity=2)
