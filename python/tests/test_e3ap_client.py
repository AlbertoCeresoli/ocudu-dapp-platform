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
import socket
import struct
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import Mock, patch
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
# E3AP-OCUDU-V2 APER bytes of {id 1, setupRequest{"1.0.0","x","0","v"}}: a setup without OCUDU
# additions is libe3's setup, bit for bit (checked against the libe3 module below).
SETUP_GOLDEN_HEX = "00000100312e302e30007800300076"
# The same PDU with the envelope timestamp 1756800000000000000 ns (libe3's root field).
SETUP_TS_GOLDEN_HEX = "4000081861684ac56800000100312e302e30007800300076"


class ClientNegotiationTests(unittest.TestCase):
    def client(self):
        with patch("ocudu_e3.e3ap.SctpAssociation"):
            return E3Client()

    def test_setup_requires_exact_version_and_explicit_profile_grant(self):
        accepted = {"responseCode": "positive", "e3apProtocolVersion": b"1.0.0", "ocuduProfile": 1,
                    "dAppIdentifier": 101, "ranIdentifier": b"test"}
        for key, values in (("e3apProtocolVersion", (None, b"1.", b"1.1.0", b"2.0.0")),
                            ("ocuduProfile", (None, 0, 2, 255))):
            for value in values:
                with self.subTest(key=key, value=value), self.client() as client:
                    body = {**accepted, key: value}
                    client._send = Mock()
                    client._wait_for = Mock(return_value=(1, "setupResponse", body))
                    with self.assertRaises(E3ProtocolError):
                        client.setup()
                    self.assertIsNone(client.dapp_id)
        with self.client() as client:
            client._send = Mock()
            client._wait_for = Mock(return_value=(1, "setupResponse", accepted))
            self.assertEqual(client.setup().dapp_id, 101)

    def test_expired_subscription_delete_releases_local_reader(self):
        with self.client() as client:
            client.dapp_id = 101
            subscription = Mock()
            client.subscriptions[7] = subscription
            client._send = Mock()
            client._wait_for = Mock(return_value=(1, "messageAck", {"responseCode": "negative", "cause": "notFound"}))
            client.unsubscribe(7)
            subscription.region_reader.close.assert_called_once()
            self.assertNotIn(7, client.subscriptions)
            with self.assertRaises(E3ProtocolError):
                client.unsubscribe(8)

    def test_close_and_release_close_shared_readers(self):
        from ocudu_e3.e3ap import E3TransportError

        for operation in ("close", "release", "failed_release"):
            with self.subTest(operation=operation):
                client = self.client()
                client.dapp_id = 101
                subscription = Mock()
                client.subscriptions[7] = subscription
                client._send = Mock()
                client._wait_for = Mock(return_value=(1, "messageAck", {"responseCode": "positive"}))
                if operation == "failed_release":
                    client._send.side_effect = E3TransportError("peer went away")
                    client.release()
                else:
                    getattr(client, operation)()
                subscription.region_reader.close.assert_called_once()
                self.assertFalse(client.subscriptions)
                self.assertIsNone(client.dapp_id)


class OfflineCodecTests(unittest.TestCase):
    def test_native_wire_is_the_libe3_module_at_every_id_width(self):
        """E3AP-OCUDU-V2 without additions is libe3's E3AP 1.0.0, whatever the id width."""
        from ocudu_e3.libe3 import Libe3Codec

        libe3 = Libe3Codec()
        body = {"e3apProtocolVersion": b"1.0.0", "dAppName": b"x", "dAppVersion": b"0", "vendor": b"v"}
        text = {key: value.decode() for key, value in body.items()}
        for pdu_id in (1, 256, 257, 65536, 65537, 16777216, 16777217, 4294967295):
            for timestamp in (None, 1756800000000000000):
                with self.subTest(pdu_id=pdu_id, timestamp=timestamp):
                    encoded = self.codec.encode_pdu(pdu_id, "setupRequest", body, timestamp_ns=timestamp)
                    reference = {"id": pdu_id, "msg": ("setupRequest", text)}
                    if timestamp is not None:
                        reference["timestamp"] = timestamp
                    self.assertEqual(encoded, libe3.encode(reference))
                    self.assertEqual(libe3.decode(encoded), reference)
        # With the OCUDU profile requested, libe3 still reads the setup (and skips the addition).
        encoded = self.codec.encode_pdu(1, "setupRequest", {**body, "ocuduProfile": 1})
        self.assertEqual(libe3.decode(encoded), {"id": 1, "msg": ("setupRequest", text)})

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

    def test_setup_request_with_timestamp_golden_vector(self):
        encoded = self.codec.encode_pdu(
            1,
            "setupRequest",
            {"e3apProtocolVersion": b"1.0.0", "dAppName": b"x", "dAppVersion": b"0", "vendor": b"v"},
            timestamp_ns=1756800000000000000,
        )
        self.assertEqual(encoded.hex(), SETUP_TS_GOLDEN_HEX)
        pdu_id, timestamp, name, _body = self.codec.decode_pdu_with_timestamp(encoded)
        self.assertEqual((pdu_id, timestamp, name), (1, 1756800000000000000, "setupRequest"))
        _pdu_id, _name, _body = self.codec.decode_pdu(encoded)  # 3-tuple API still works

    def test_tensor_descriptor_round_trip(self):
        from ocudu_e3.shared_region import TensorDescriptor, PosixSharedLocation, AcceleratorIpcLocation

        body = {
            "memoryDomain": "posixShared",
            "location": ("posixShared", {"regionName": b"/ocudu-e3-1-2-3", "rowIndex": 1, "byteOffset": (128).to_bytes(8, "big")}),
            "byteLength": (734208).to_bytes(8, "big"),
            "generation": (4).to_bytes(8, "big"),
            "elementType": "complexFp32",
            "dimensions": [4, 14, 3276],
        }
        encoded = self.codec.encode("E3-TensorDescriptor", body)
        descriptor = TensorDescriptor.from_asn(self.codec.decode("E3-TensorDescriptor", encoded))
        self.assertIsInstance(descriptor.location, PosixSharedLocation)
        self.assertEqual(descriptor.location.row_index, 1)
        self.assertEqual(descriptor.byte_length, 734208)
        self.assertEqual(descriptor.dimensions, [4, 14, 3276])
        gpu = {
            "memoryDomain": "acceleratorIpcExport",
            "location": ("acceleratorIpc", {"deviceOrdinal": 0, "allocationId": (9).to_bytes(8, "big"), "memoryHandle": bytes(range(64)), "byteOffset": (0).to_bytes(8, "big")}),
            "byteLength": (16).to_bytes(8, "big"),
            "generation": (2).to_bytes(8, "big"),
        }
        descriptor = TensorDescriptor.from_asn(self.codec.decode("E3-TensorDescriptor", self.codec.encode("E3-TensorDescriptor", gpu)))
        self.assertIsInstance(descriptor.location, AcceleratorIpcLocation)
        self.assertEqual(descriptor.location.memory_handle, bytes(range(64)))
        self.assertIsNone(descriptor.location.ready_event_handle)

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
    def test_plain_libe3_peer_at_large_ids_gets_the_libe3_profile(self):
        """A peer speaking libe3's module (no OCUDU additions) is served libe3's procedures."""
        from ocudu_e3.libe3 import Libe3Codec

        libe3 = Libe3Codec()
        for pdu_id in (257, 65537, 4294967295):
            with self.subTest(pdu_id=pdu_id), socket.socket(socket.AF_INET, socket.SOCK_STREAM, socket.IPPROTO_SCTP) as peer:
                peer.settimeout(3)
                peer.connect(("127.0.0.1", self.e3ap_port))
                body = {"e3apProtocolVersion": "1.0.0", "dAppName": "libe3-peer", "dAppVersion": "0", "vendor": "v"}
                peer.sendall(libe3.encode({"id": pdu_id, "msg": ("setupRequest", body)}))
                kind, response = libe3.decode(peer.recv(65536))["msg"]
                self.assertEqual(kind, "setupResponse")
                self.assertEqual(response["responseCode"], "positive")
                self.assertEqual(response["requestId"], pdu_id)
                self.assertLessEqual(response["dAppIdentifier"], 100, "libe3-profile identifiers")

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

    def test_agent_stamps_pdus_with_the_realtime_clock(self):
        before = time.time_ns()
        with self.client() as client:
            client.setup()
            self.assertIsNotNone(client.last_agent_timestamp_ns)
            self.assertGreaterEqual(client.last_agent_timestamp_ns, before)
            self.assertLessEqual(client.last_agent_timestamp_ns, time.time_ns())

    def test_dapp_report_is_acknowledged(self):
        with self.client() as client:
            client.setup()
            client.report(1, b"\x01\x02\x03")  # a Spectrum-style northbound report

    def test_by_reference_subscription_delivers_tensors_through_shared_memory(self):
        with self.client() as client:
            client.setup()
            subscription = client.subscribe(
                ["pusch.symbols"], configs=[SubscriptionConfig("pusch.symbols", minimum_interval_ms=0)],
                by_reference=True, minimum_by_reference_bytes=256,
            )
            self.assertIsNotNone(subscription.shared_region)
            self.assertIsNotNone(subscription.region_reader)
            self.assertEqual(subscription.shared_region.memory_domain, "posixShared")
            self.assertGreaterEqual(subscription.shared_region.nof_rows, 1)
            seen = None
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                indication = client.next_indication(timeout=1.0)
                if indication is None:
                    continue
                if isinstance(indication.record, PuschSymbolsRecord) and indication.by_reference is not None:
                    seen = indication
                    break
            self.assertIsNotNone(seen, "no by-reference PUSCH symbol indication within 10 s")
            self.assertEqual(seen.by_reference.element_type, "complexFp32")
            self.assertEqual(seen.by_reference.byte_length, len(seen.record.equalized_symbols))
            self.assertGreater(len(seen.record.equalized_symbols), 256)
            # The row header agrees with the descriptor and the record.
            row = subscription.region_reader.row_header(seen.by_reference.location.row_index)
            self.assertEqual(row["generation"] % 2, 0)
            self.assertEqual(row["telemetry_id"], seen.telemetry_id)
            self.assertEqual(row["byte_length"], seen.by_reference.byte_length)
            self.assertEqual(row["sequence"], seen.sequence)
            self.assertEqual(client.torn_reads, 0)
            client.unsubscribe(subscription)
            self.assertIsNone(subscription.region_reader._map if subscription.region_reader else None)

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

    def test_pusch_routing_round_trips_and_validates_over_sctp(self):
        from ocudu_e3 import management
        with self.client() as client:
            client.setup()
            table = client.pusch_routing()
            self.assertEqual(table["code"], "success")
            state = table["payload"]["puschRouting"]
            self.assertEqual(state["slotsPerHook"], 2)
            self.assertEqual(state["entries"], [])
            self.assertEqual(state["defaultTarget"], {"primary": None})

            unbound = management.pusch_route_entry(0x4601, "00112233-4455-6677-8899-aabbccddeeff")
            self.assertEqual(client.pusch_routing("update", [unbound])["code"], "notFound")

            dry = client.pusch_routing("replace", [management.pusch_route_entry(0x4601, "conventional")], dry_run=True)
            self.assertEqual(dry["code"], "dryRunSuccess")
            self.assertEqual(dry["payload"]["puschRouting"]["entries"], [])

            applied = client.pusch_routing("replace", [
                management.pusch_route_entry(0x4601, "conventional"),
                management.pusch_route_entry(0x4602, "conventional", cell_index=1),
            ], default="conventional")
            self.assertEqual(applied["code"], "success")
            state = applied["payload"]["puschRouting"]
            self.assertEqual(state["defaultTarget"], {"conventional": None})
            # Sorted by cell, then RNTI; entries for every cell (no cellIndex) come last.
            self.assertEqual([entry["rnti"] for entry in state["entries"]], [0x4602, 0x4601])
            self.assertEqual(state["entries"][0]["cellIndex"], 1)
            self.assertNotIn("cellIndex", state["entries"][1])
            self.assertGreater(state["generation"], table["payload"]["puschRouting"]["generation"])

            removed = client.pusch_routing("update", [management.pusch_route_entry(0x4601, "primary")])
            self.assertEqual([entry["rnti"] for entry in removed["payload"]["puschRouting"]["entries"]], [0x4602])
            cleared = client.pusch_routing("clear")
            self.assertEqual(cleared["payload"]["puschRouting"]["entries"], [])
            self.assertEqual(cleared["payload"]["puschRouting"]["defaultTarget"], {"primary": None})

        with self.assertRaises(ValueError):
            management.pusch_routing_request("query", [management.pusch_route_entry(0x4601)])
        with self.assertRaises(ValueError):
            management.pusch_routing_request("update", default="conventional")
        with self.assertRaises(ValueError):
            management.pusch_route_entry(0xFFF0)

    def test_catalog_admission_is_a_job_and_cannot_bypass_missing_trust(self):
        from ocudu_e3 import management
        with self.client() as client:
            client.setup()
            before = client.inventory()["payload"]["inventory"]
            accepted = client.catalog_admit("00112233-4455-6677-8899-aabbccddeeff", dry_run=True)
            self.assertEqual(accepted["code"], "accepted")
            request = {"header": management.header(), "operation": ("jobs", {
                "action": "get", "jobId": management.parse_identifier(accepted["jobId"])})}
            deadline = time.monotonic() + 5
            while True:
                result = client.management(request)["payload"][1]["jobs"][0]
                if result["state"] in ("succeeded", "failed"):
                    break
                self.assertLess(time.monotonic(), deadline)
                time.sleep(0.01)
            self.assertEqual(result["state"], "failed")
            self.assertEqual(result["result"]["code"], "verificationFailed")
            after = client.inventory()["payload"]["inventory"]
            self.assertEqual(before["generation"], after["generation"])
            self.assertEqual(after.get("packages", []), [])


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
