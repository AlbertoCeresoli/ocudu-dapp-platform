#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear
"""End-to-end OCUDU <-> libe3 interoperability, with the real upstream library.

Two checks, each driven by a binary that links upstream libe3 only (built by
``tests/interop/libe3`` when ``OCUDU_DAPP_LIBE3_INTEROP=ON``):

* ``libe3_interop_dapp`` (libe3 dApp role) attaches to ``e3ap_test_server`` and runs
  setup, subscription, indications, a management control action, a report (with its
  libe3 >= 0.2 sequenceId), an xApp control action and its re-issue, subscription
  delete and release -- once per transport:

  - ``sctp``: ``E3LinkLayer::SCTP_ASSOCIATION`` (our patch to libe3) against the
    agent's SCTP carrier. Skipped, not failed, when the kernel cannot open an SCTP
    socket (no ``sctp`` module, e.g. in containers).
  - ``zmq-ipc`` / ``zmq-tcp``: libe3's own, unpatched ``E3LinkLayer::ZMQ``
    connector against the agent's libe3 ZMQ carrier. Needs only libzmq, so it runs
    everywhere the build has ZeroMQ.

  ``--transports`` (or ``OCUDU_LIBE3_TRANSPORTS``) restricts the set. Every transport
  runs once per encoding: libe3 ``EncodingFormat::ASN1`` (APER) and
  ``EncodingFormat::PROTOBUF`` (libe3 built with ``LIBE3_ENABLE_PROTOBUF``), restricted
  by ``--encodings`` (or ``OCUDU_LIBE3_ENCODINGS``). The SCTP agent detects the
  encoding from the setup request; the ZMQ agent is configured with it, as libe3 is.
* ``libe3_interop_vectors`` encodes every procedure with libe3's encoders and
  decodes what asn1tools (vendored libe3 module) emits; the encode output must
  also match the pinned ``fixtures/libe3_vectors.json`` (APER and Protobuf) so
  drift is visible.

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
import queue
import random
import socket
import tempfile
import threading
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
TRANSPORTS = os.environ.get("OCUDU_LIBE3_TRANSPORTS", "sctp,zmq-ipc,zmq-tcp")
ENCODINGS = os.environ.get("OCUDU_LIBE3_ENCODINGS", "asn1,protobuf")

# The skip conditions below are evaluated when the classes are defined, so the
# command line has to be read before that point, not in __main__.
_REMAINING_ARGV = [sys.argv[0]]
if __name__ == "__main__":
    _parser = argparse.ArgumentParser()
    _parser.add_argument("--server", help="path to e3ap_test_server")
    _parser.add_argument("--dapp", help="path to libe3_interop_dapp")
    _parser.add_argument("--vectors", help="path to libe3_interop_vectors")
    _parser.add_argument("--transports", help="comma list of sctp, zmq-ipc, zmq-tcp")
    _parser.add_argument("--encodings", help="comma list of asn1, protobuf")
    _args, _rest = _parser.parse_known_args()
    _REMAINING_ARGV += _rest
    SERVER = _args.server or SERVER
    DAPP = _args.dapp or DAPP
    VECTORS = _args.vectors or VECTORS
    TRANSPORTS = _args.transports or TRANSPORTS
    ENCODINGS = _args.encodings or ENCODINGS
LIBE3_MODULE = REPO / "schemas" / "e3" / "interop" / "libe3" / "e3ap-1.0.0.asn1"
PINNED = HERE / "fixtures" / "libe3_vectors.json"


def kernel_has_sctp() -> bool:
    """True when this kernel can open an SCTP socket (module loaded or loadable)."""
    try:
        probe = socket.socket(socket.AF_INET, socket.SOCK_STREAM, socket.IPPROTO_SCTP)
    except OSError:
        return False
    probe.close()
    return True


def free_zmq_tcp_base() -> int:
    """A base port with base, base + 1 and base + 9 free on loopback (libe3's 9990/9991/9999 layout)."""
    for _ in range(200):
        base = random.randint(20000, 60000)
        sockets = []
        try:
            for offset in (0, 1, 9):
                probe = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                probe.bind(("127.0.0.1", base + offset))
                sockets.append(probe)
            return base
        except OSError:
            continue
        finally:
            for probe in sockets:
                probe.close()
    raise RuntimeError("no free port triplet for the libe3 ZMQ TCP carrier")


class _AgentProcess:
    """e3ap_test_server with its stdout drained on a thread: replies ("xapp sent") and decoded reports
    ("report <dApp> <RAN function> <sequenceId> <bytes>") arrive interleaved."""

    def __init__(self, transport: str, catalog: str, encoding: str = "asn1"):
        command = [SERVER, "--catalog", catalog, "--period-ms", "5"]
        if transport != "sctp":
            command += ["--zmq-encoding", encoding]
        self.endpoint_args: list[str] = []
        self.ipc_dir = None
        if transport == "zmq-ipc":
            # Unix socket paths are limited to 107 bytes: keep the directory short.
            self.ipc_dir = tempfile.mkdtemp(prefix="e3z", dir="/tmp")
            command += ["--zmq-ipc-dir", self.ipc_dir]
            self.endpoint_args = ["--transport", "zmq-ipc", "--ipc-dir", self.ipc_dir]
        elif transport == "zmq-tcp":
            base = free_zmq_tcp_base()
            command += ["--zmq-tcp-port", str(base)]
            self.endpoint_args = ["--transport", "zmq-tcp", "--port", str(base)]
        self.process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=subprocess.PIPE, text=True)
        self.lines: queue.Queue[str] = queue.Queue()
        self.reports: list[list[int]] = []
        port = 0
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            line = self.process.stdout.readline()
            if not line:
                break
            if line.startswith("e3ap_port"):
                port = int(line.split()[1])
            elif line.startswith("ready"):
                break
        else:
            line = ""
        if not line.startswith("ready"):
            self.process.kill()
            raise RuntimeError(f"test agent ({transport}) did not start: " + self.process.stderr.read())
        if transport == "sctp":
            self.endpoint_args = ["--transport", "sctp", "--port", str(port)]
        self.endpoint_args += ["--encoding", encoding]
        self.reader = threading.Thread(target=self._drain, daemon=True)
        self.reader.start()

    def _drain(self):
        for line in self.process.stdout:
            line = line.strip()
            if line.startswith("report "):
                self.reports.append([int(field) for field in line.split()[1:]])
            else:
                self.lines.put(line)

    def command(self, text: str, timeout: float = 10.0) -> str:
        self.process.stdin.write(text + "\n")
        self.process.stdin.flush()
        return self.lines.get(timeout=timeout)

    def close(self):
        try:
            self.process.stdin.write("quit\n")
            self.process.stdin.flush()
            self.process.wait(timeout=10)
        except Exception:
            self.process.kill()
        if self.ipc_dir:
            subprocess.run(["rm", "-rf", self.ipc_dir], check=False)


class _LibE3DAppAgainstOcuduAgent:
    """The libe3 dApp against the agent over one transport (see the subclasses)."""

    TRANSPORT = ""
    ENCODING = "asn1"

    @classmethod
    def setUpClass(cls):
        cls.tempdir = tempfile.TemporaryDirectory()
        cls.agent = _AgentProcess(cls.TRANSPORT, cls.tempdir.name, cls.ENCODING)

    @classmethod
    def tearDownClass(cls):
        cls.agent.close()
        cls.tempdir.cleanup()

    def start_dapp(self, *extra: str) -> subprocess.Popen:
        return subprocess.Popen([DAPP, *self.agent.endpoint_args, *extra], stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, text=True)

    def run_dapp(self, *extra: str, xapp: bytes | None = None, xapp_sequence: int = 0) -> dict:
        """Run the libe3 dApp to completion. With ``xapp`` the agent is told, as soon as the dApp
        announces its identifier, to queue that xApp control action to it (RAN function 11)."""
        return self.finish_dapp(self.start_dapp(*extra), xapp=xapp, xapp_sequence=xapp_sequence, args=extra)

    def finish_dapp(self, process: subprocess.Popen, *, xapp: bytes | None = None, xapp_sequence: int = 0,
                    args=()) -> dict:
        records = []
        for line in process.stdout:
            if not line.startswith("{"):
                continue
            record = json.loads(line)
            records.append(record)
            if record.get("event") == "setup" and xapp is not None:
                reply = self.agent.command(f"xapp {record['dapp_id']} 11 {xapp.hex()} {xapp_sequence}")
                self.assertEqual(reply, "xapp sent")
        stderr = process.communicate(timeout=60)[1]
        summaries = [record for record in records if "event" not in record]
        self.assertTrue(summaries, "dApp printed no JSON summary: " + stderr[-2000:])
        summary = summaries[-1]
        summary["_exit"] = process.returncode
        # scripts/libe3_interop_report.py collects what the real libe3 dApp reported per session.
        record = os.environ.get("OCUDU_LIBE3_E2E_SUMMARY")
        if record:
            with open(record, "a", encoding="utf-8") as sink:
                sink.write(json.dumps({"test": f"{self.TRANSPORT}/{self.ENCODING}:{self._testMethodName}",
                                       "transport": self.TRANSPORT, "args": list(args), **summary}) + "\n")
        return summary

    def test_full_session(self):
        codec = load_codec(os.environ.get("OCUDU_E3_SCHEMA_DIR"))
        control = codec.encode("ManagementRequest", management.inventory_request())
        before = len(self.agent.reports)
        summary = self.run_dapp("--telemetry", "2", "--indications", "3", "--control-hex", control.hex(),
                                "--expect-xapp", "1", "--reissue", "1", "--report-sequence", "4242",
                                xapp=control, xapp_sequence=3000000000)
        self.assertEqual(summary["transport"], self.TRANSPORT)
        self.assertEqual(summary["encoding"], self.ENCODING)
        self.assertTrue(summary["setup"], summary)
        self.assertGreaterEqual(summary["dapp_id"], 1)
        self.assertLessEqual(summary["dapp_id"], 100, "this RAN numbers libe3 dApps 1..100 (the 0.2 wire allows 1..65535)")
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
        self.assertEqual(summary["xapp_controls"], 1, "the xApp control action must reach the libe3 dApp")
        # The xApp control is a real management request, so the dApp's re-issue of it is accepted.
        self.assertEqual(summary["xapp_control_hex"], control.hex())
        self.assertEqual(summary["xapp_ran_function"], 11)
        # libe3 >= 0.2 E3-SequenceID end to end: the report's id reaches the RAN, the xApp control's id
        # (beyond the old 1..1000 range) reaches the dApp, and the dApp's re-issue carrying it is acked.
        self.assertEqual(summary["xapp_sequence"], 3000000000)
        self.assertEqual(summary["reissue_sequence"], 3000000000)
        self.assertEqual(summary["reissue_ack"], "positive")
        reports = self.agent.reports[before:]
        self.assertIn([summary["dapp_id"], 10, 4242, 3], reports, reports)
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


_HAVE_AGENT = bool(SERVER and DAPP)
_WANTED = {name.strip() for name in TRANSPORTS.split(",") if name.strip()}
_WANTED_ENCODINGS = {name.strip() for name in ENCODINGS.split(",") if name.strip()}


@unittest.skipUnless(_HAVE_AGENT, "needs --server and --dapp (or the OCUDU_* environment)")
@unittest.skipUnless("sctp" in _WANTED, "sctp not in --transports")
@unittest.skipUnless(kernel_has_sctp(), "kernel has no SCTP support (load the sctp module); ZMQ legs still run")
class LibE3OverSctp(_LibE3DAppAgainstOcuduAgent, unittest.TestCase):
    TRANSPORT = "sctp"


@unittest.skipUnless(_HAVE_AGENT, "needs --server and --dapp (or the OCUDU_* environment)")
@unittest.skipUnless(kernel_has_sctp(), "kernel has no SCTP support (load the sctp module); ZMQ legs still run")
class LibE3ProtobufOverSctp(_LibE3DAppAgainstOcuduAgent, unittest.TestCase):
    TRANSPORT = "sctp"
    ENCODING = "protobuf"


class _ZmqLeg(_LibE3DAppAgainstOcuduAgent):
    def test_two_concurrent_libe3_dapps_share_the_zmq_sockets(self):
        """Both sessions ride the same PUB/SUB pair: the carrier routes by the dApp identifier in each
        PDU and every PDU names its dApp, so neither session sees the other's traffic as its own."""
        first = self.start_dapp("--indications", "3", "--report-sequence", "11")
        second = self.start_dapp("--indications", "3", "--report-sequence", "22")
        one = self.finish_dapp(first)
        two = self.finish_dapp(second)
        self.assertTrue(one["ok"] and two["ok"], (one, two))
        self.assertNotEqual(one["dapp_id"], two["dapp_id"])
        self.assertIn([one["dapp_id"], 10, 11, 3], self.agent.reports)
        self.assertIn([two["dapp_id"], 10, 22, 3], self.agent.reports)


@unittest.skipUnless(_HAVE_AGENT, "needs --server and --dapp (or the OCUDU_* environment)")
@unittest.skipUnless("zmq-ipc" in _WANTED, "zmq-ipc not in --transports")
class LibE3OverZmqIpc(_ZmqLeg, unittest.TestCase):
    TRANSPORT = "zmq-ipc"


@unittest.skipUnless(_HAVE_AGENT, "needs --server and --dapp (or the OCUDU_* environment)")
@unittest.skipUnless("zmq-tcp" in _WANTED, "zmq-tcp not in --transports")
class LibE3OverZmqTcp(_ZmqLeg, unittest.TestCase):
    TRANSPORT = "zmq-tcp"


@unittest.skipUnless(_HAVE_AGENT, "needs --server and --dapp (or the OCUDU_* environment)")
class LibE3ProtobufOverZmqIpc(_ZmqLeg, unittest.TestCase):
    TRANSPORT = "zmq-ipc"
    ENCODING = "protobuf"


@unittest.skipUnless(_HAVE_AGENT, "needs --server and --dapp (or the OCUDU_* environment)")
class LibE3ProtobufOverZmqTcp(_ZmqLeg, unittest.TestCase):
    TRANSPORT = "zmq-tcp"
    ENCODING = "protobuf"


@unittest.skipUnless(VECTORS, "needs --vectors (or OCUDU_LIBE3_INTEROP_VECTORS)")
class LibE3EncoderAgainstAsn1tools(unittest.TestCase):
    """The upstream encoder binary versus asn1tools on the vendored module."""

    @classmethod
    def setUpClass(cls):
        from ocudu_e3.libe3 import load_libe3_codec

        # libe3's grammar compiled the way asn1c encodes it (see ocudu_e3/libe3.py).
        cls.libe3 = load_libe3_codec(LIBE3_MODULE.parents[2])
        cls.pinned = json.loads(PINNED.read_text())

    def encode_all(self, encoding: str = "asn1") -> list[dict]:
        output = subprocess.run([VECTORS, "encode", encoding], stdout=subprocess.PIPE, text=True, check=True).stdout
        return [json.loads(line) for line in output.splitlines() if line.strip()]

    def decode_one(self, hex_text: str, encoding: str = "asn1") -> dict:
        output = subprocess.run([VECTORS, "decode", hex_text, encoding], stdout=subprocess.PIPE, text=True).stdout
        return json.loads(output.strip() or '{"error": "no output"}')

    def test_pinned_vectors_are_current(self):
        fresh = {row["name"]: row["hex"] for row in self.encode_all()}
        pinned = {row["name"]: row["hex"] for row in self.pinned["vectors"]}
        self.assertEqual(fresh, pinned, "regenerate fixtures/libe3_vectors.json and e3ap_libe3_vectors.inc")

    def test_libe3_decodes_what_python_encodes(self):
        """All eleven procedures cross from Python (ocudu_e3.libe3) to the real libe3
        decoder field for field, the two text-carrying setup PDUs included."""
        for vector in self.pinned["vectors"]:
            with self.subTest(vector["name"]):
                decoded = self.libe3.decode("E3-PDU", bytes.fromhex(vector["hex"]))
                re_encoded = self.libe3.encode("E3-PDU", decoded)
                seen = self.decode_one(re_encoded.hex())
                self.assertNotIn("error", seen, vector["name"])
                self.assertEqual(seen["id"], self.pinned["message_id"])
                self.assertEqual(seen["timestamp"], self.pinned["timestamp"])
                self.assertEqual(seen, vector["fields"])

    def test_libe3_reads_ocudu_native_pdus(self):
        """E3AP-OCUDU-V2 is libe3's module plus extension additions: the real libe3 decoder reads OCUDU's
        own setup request (OCUDU profile requested, instance identity attached) as that setup."""
        ocudu = load_codec(os.environ.get("OCUDU_E3_SCHEMA_DIR"))
        body = {"e3apProtocolVersion": b"1.0.0", "dAppName": b"x", "dAppVersion": b"0", "vendor": b"v"}
        native = ocudu.encode_pdu(1, "setupRequest", {**body, "ocuduProfile": 1, "dAppInstance": bytes(range(16))},
                                  timestamp_ns=1756800000000000000)
        seen = self.decode_one(native.hex())
        self.assertEqual(seen, {"name": "SetupRequest", "id": 1, "timestamp": 1756800000000000000,
                                "e3apProtocolVersion": "1.0.0", "dAppName": "x", "dAppVersion": "0", "vendor": "v"})
        report = ocudu.encode_pdu(9, "dAppReport", {"dAppIdentifier": 101, "ranFunctionIdentifier": 10,
                                                     "sequenceId": 4242, "reportData": b"\x01"})
        self.assertEqual(self.decode_one(report.hex())["sequenceId"], 4242)

@unittest.skipUnless(VECTORS, "needs --vectors (or OCUDU_LIBE3_INTEROP_VECTORS)")
class LibE3ProtobufVectors(unittest.TestCase):
    """libe3's ProtobufE3Encoder against the pinned vectors (which OCUDU's hand-written codec
    reproduces byte for byte in e3ap_protobuf_codec_test), and against its own APER encoding."""

    @classmethod
    def setUpClass(cls):
        cls.pinned = json.loads(PINNED.read_text())

    def run_tool(self, *args: str) -> str:
        return subprocess.run([VECTORS, *args], stdout=subprocess.PIPE, text=True, check=True).stdout

    def test_pinned_protobuf_vectors_are_current(self):
        rows = [json.loads(line) for line in self.run_tool("encode", "protobuf").splitlines() if line.strip()]
        fresh = {row["name"]: row["hex"] for row in rows}
        pinned = {row["name"]: row["hex"] for row in self.pinned["protobuf_vectors"]}
        self.assertEqual(fresh, pinned, "regenerate fixtures/libe3_vectors.json and e3ap_libe3_protobuf_vectors.inc")

    def test_both_libe3_encodings_carry_the_same_fields(self):
        aper = {row["name"]: row["fields"] for row in self.pinned["vectors"]}
        for vector in self.pinned["protobuf_vectors"]:
            with self.subTest(vector["name"]):
                seen = json.loads(self.run_tool("decode", vector["hex"], "protobuf"))
                self.assertEqual(seen, vector["fields"])
                self.assertEqual(seen, aper[vector["name"]])


if __name__ == "__main__":
    if _WANTED == {"sctp"} and not kernel_has_sctp():
        # CTest SKIP_RETURN_CODE: the SCTP leg cannot run on this kernel; the ZMQ leg still does.
        print("SKIPPED: this kernel cannot open SCTP sockets (no sctp module); run the e2e_zmq leg instead")
        sys.exit(77)
    # Load only what was asked for: a transport that was not requested is not a skipped test (the CI gate
    # fails closed on skips), it is simply not part of this run.
    selected = []
    if _HAVE_AGENT:
        legs = (LibE3OverSctp, LibE3OverZmqIpc, LibE3OverZmqTcp,
                LibE3ProtobufOverSctp, LibE3ProtobufOverZmqIpc, LibE3ProtobufOverZmqTcp)
        selected += [cls for cls in legs if cls.TRANSPORT in _WANTED and cls.ENCODING in _WANTED_ENCODINGS]
    if VECTORS:
        selected.append(LibE3EncoderAgainstAsn1tools)
        if "protobuf" in _WANTED_ENCODINGS:
            selected.append(LibE3ProtobufVectors)
    if not selected:
        print("nothing to run: give --server/--dapp and/or --vectors")
        sys.exit(2)
    loader = unittest.defaultTestLoader
    suite = unittest.TestSuite(loader.loadTestsFromTestCase(cls) for cls in selected)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    sys.exit(0 if result.wasSuccessful() else 1)
