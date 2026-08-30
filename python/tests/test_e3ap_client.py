# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear
"""Cross-language test: the Python E3AP client against the C++ loopback agent.

Run directly (``python3 test_e3ap_client.py --server <e3ap_test_server>``) or
through CTest. Without ``--server`` the offline codec tests still run and the
live tests are skipped.
"""

from __future__ import annotations

import argparse
import os
import struct
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import ocudu_e3  # noqa: E402
from ocudu_e3 import E3Client, E3ProtocolError, SubscriptionConfig  # noqa: E402
from ocudu_e3.telemetry import (  # noqa: E402
    GnbMetricsJsonRecord,
    PuschCrcRecord,
    PuschSymbolsRecord,
    SrsChannelRecord,
    decode_store_publication,
)

SERVER = os.environ.get("OCUDU_E3AP_TEST_SERVER")
# APER bytes of {id 1, setupRequest{"1.0.0","x","0","v"}}; the C++ codec test pins the same vector.
SETUP_GOLDEN_HEX = "00000080312e302e30007800300076"


class OfflineCodecTests(unittest.TestCase):
    def setUp(self):
        self.codec = ocudu_e3.load_codec()

    def test_setup_request_golden_vector(self):
        # Fixed APER bytes for a minimal setup request; changes here mean the wire changed.
        encoded = self.codec.encode_pdu(
            1, "setupRequest", {"e3apProtocolVersion": b"1.0.0", "dAppName": b"x", "dAppVersion": b"0", "vendor": b"v"}
        )
        self.assertEqual(encoded.hex(), SETUP_GOLDEN_HEX)
        pdu_id, name, body = self.codec.decode_pdu(encoded)
        self.assertEqual((pdu_id, name), (1, "setupRequest"))
        self.assertEqual(body["dAppName"], b"x")

    def test_telemetry_indication_round_trip(self):
        payload = self.codec.encode(
            "Telemetry-IndicationData",
            {
                "telemetryId": 1,
                "cellId": 0,
                "sequence": (9).to_bytes(8, "big"),
                "routeEpoch": (3).to_bytes(8, "big"),
                "monotonicNs": (7).to_bytes(8, "big"),
                "droppedObjects": bytes(8),
                "object": (
                    "puschSymbols",
                    {
                        "rnti": 0x4601,
                        "radioSlot": {"sfn": 100, "slot": 3, "numerology": 1},
                        "nofPrb": 2,
                        "nofDataSymbols": 1,
                        "nofLayers": 1,
                        "modulationBits": 2,
                        "sinrDb": 12.5,
                        "equalizedSymbols": struct.pack("<4f", 1.0, -1.0, 0.5, 0.25),
                    },
                ),
            },
        )
        indication = ocudu_e3.decode_indication(self.codec, payload)
        self.assertEqual(indication.stream, "pusch.symbols")
        self.assertEqual(indication.sequence, 9)
        record = indication.record
        self.assertIsInstance(record, PuschSymbolsRecord)
        self.assertEqual(record.sinr_db, 12.5)
        self.assertIsNone(record.epre_db)
        samples = list(record.samples())
        self.assertEqual(len(samples), 2)
        self.assertAlmostEqual(complex(samples[0]).real, 1.0)
        self.assertAlmostEqual(complex(samples[1]).imag, 0.25)

    def test_store_metadata_layout_matches_the_host_records(self):
        header = struct.pack("<IHHQ", 0x3345544F, 1, 2, 123)
        body = struct.pack("<HHBBB3x", 0x4601, 10, 5, 1, 1)
        record = decode_store_publication(0x00020002, header + body, b"\x01")
        self.assertIsInstance(record, PuschCrcRecord)
        self.assertEqual(record.rnti, 0x4601)
        self.assertEqual(record.radio_slot.sfn, 10)
        self.assertTrue(record.crc_ok)
        with self.assertRaises(ValueError):
            decode_store_publication(0x00020002, header[:-1], b"\x01")
        portable = decode_store_publication(0x00010001, b"OCC3", b"\x00" * 16)
        self.assertEqual(portable.interface_id, 0x00010001)


@unittest.skipUnless(SERVER, "set OCUDU_E3AP_TEST_SERVER or pass --server to run live tests")
class LiveAgentTests(unittest.TestCase):
    process: subprocess.Popen
    e3ap_port: int
    e3dp_port: int

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
        cls.e3dp_port = 0
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            line = cls.process.stdout.readline()
            if not line:
                break
            if line.startswith("e3ap_port"):
                cls.e3ap_port = int(line.split()[1])
            elif line.startswith("e3dp_port"):
                cls.e3dp_port = int(line.split()[1])
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

    def client(self) -> E3Client:
        return E3Client("127.0.0.1", self.e3ap_port, dapp_name="python-test", timeout=5.0)

    def test_setup_lists_streams_and_management(self):
        with self.client() as client:
            setup = client.setup()
            self.assertEqual(setup.ran_identifier, "ocudu-e3-test-agent")
            self.assertIsNotNone(setup.telemetry)
            names = {stream.name for stream in setup.telemetry.streams}
            self.assertEqual(names, set(ocudu_e3.TELEMETRY_IDS))
            self.assertIsNotNone(setup.management)
            self.assertEqual(setup.management.schema_module, "E3SM-OCUDU-DAPP-MGMT-V1")
            self.assertEqual(setup.management.control_ids, [1])

    def test_subscription_streams_records_and_honors_rnti_filter(self):
        with self.client() as client:
            client.setup()
            subscription = client.subscribe(
                ["pusch.symbols", "pusch.crc", "srs.channel", "gnb.metrics_json"],
                configs=[SubscriptionConfig("pusch.symbols", rnti_filter=[0x4602])],
            )
            self.assertEqual(len(subscription.telemetry_ids), 4)
            seen = {}
            deadline = time.monotonic() + 8
            while time.monotonic() < deadline and len(seen) < 4:
                indication = client.next_indication(timeout=1.0)
                if indication is None:
                    continue
                seen.setdefault(indication.stream, indication)
                if indication.stream == "pusch.symbols":
                    self.assertEqual(indication.record.rnti, 0x4602, "RNTI filter must drop 0x4601")
            self.assertEqual(set(seen), {"pusch.symbols", "pusch.crc", "srs.channel", "gnb.metrics_json"})
            symbols = seen["pusch.symbols"].record
            self.assertIsInstance(symbols, PuschSymbolsRecord)
            self.assertEqual(symbols.nof_prb, 52)
            self.assertEqual(len(symbols.equalized_symbols), 52 * 12 * 12 * 8)
            self.assertIsInstance(seen["pusch.crc"].record, PuschCrcRecord)
            srs = seen["srs.channel"].record
            self.assertIsInstance(srs, SrsChannelRecord)
            self.assertAlmostEqual(srs.rsrp_db, -70.0)
            metrics = seen["gnb.metrics_json"].record
            self.assertIsInstance(metrics, GnbMetricsJsonRecord)
            self.assertEqual(metrics.json()["ue_list"][0]["ue_container"]["rnti"], 17921)
            client.unsubscribe(subscription)
            client.release()

    def test_management_over_control_actions(self):
        with self.client() as client:
            client.setup()
            inventory = client.inventory()
            self.assertEqual(inventory["code"], "success")
            self.assertIn("inventory", inventory["payload"])
            health = client.health()
            self.assertEqual(health["code"], "success")
            self.assertIn("total", health["payload"]["health"])
            with self.assertRaises(E3ProtocolError):
                client.subscribe([999])


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", default=None, help="path to e3ap_test_server")
    parser.add_argument("unittest_args", nargs="*")
    args = parser.parse_args()
    global SERVER
    if args.server:
        SERVER = args.server
        os.environ["OCUDU_E3AP_TEST_SERVER"] = args.server
    # Re-evaluate the skip decorator now that SERVER may be set.
    if SERVER:
        LiveAgentTests.__unittest_skip__ = False
    program = unittest.main(argv=[sys.argv[0]] + args.unittest_args, exit=False, verbosity=2)
    return 0 if program.result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
