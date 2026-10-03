#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear
"""OCUDU <-> libe3 E3AP alignment gate.

OCUDU's E3AP module, ``E3AP-OCUDU-V2`` (``schemas/e3/e3ap-ocudu-v2.asn1``), is the
vendored upstream libe3 ``e3ap-1.0.0.asn1`` (``schemas/e3/interop/libe3``) with
OCUDU extension additions. This gate holds it to that, with no documented
divergences to hide behind:

* structure: every libe3 type appears in V2 with the identical root (members,
  types, constraints, optionality, tags, extension markers); OCUDU adds only
  extension additions and new types;
* bytes: a V2 PDU without additions is the libe3 PDU (every pinned libe3 vector
  re-encodes identically through OCUDU's own Python codec), and a libe3 decoder
  reads an OCUDU-profile PDU as the libe3 message.

Run standalone or through ctest (``dapp_e3ap_libe3.interop``):

    OCUDU_E3_SCHEMA_DIR=schemas/e3 python3 python/tests/test_libe3_interop.py
"""
from __future__ import annotations

import json
import os
import pathlib
import sys
import unittest

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parents[1]
sys.path.insert(0, str(REPO / "python"))

import asn1tools  # noqa: E402

LIBE3_MODULE = REPO / "schemas" / "e3" / "interop" / "libe3" / "e3ap-1.0.0.asn1"
OCUDU_SCHEMA_DIR = pathlib.Path(os.environ.get("OCUDU_E3_SCHEMA_DIR", REPO / "schemas" / "e3"))
OCUDU_MODULE = OCUDU_SCHEMA_DIR / "e3ap-ocudu-v2.asn1"
VECTORS = HERE / "fixtures" / "libe3_vectors.json"


def _root(definition):
    """A parsed type with every member list cut at its extension marker (kept)."""
    if isinstance(definition, dict):
        result = {key: _root(value) for key, value in definition.items()}
        members = result.get("members")
        if isinstance(members, list) and None in members:
            result["members"] = members[: members.index(None) + 1]
        return result
    if isinstance(definition, list):
        return [_root(item) for item in definition]
    return definition


class StructuralAlignment(unittest.TestCase):
    """E3AP-OCUDU-V2's root is libe3's module, type for type."""

    @classmethod
    def setUpClass(cls):
        cls.libe3 = asn1tools.parse_files([str(LIBE3_MODULE)])["E3"]["types"]
        cls.ocudu = asn1tools.parse_files([str(OCUDU_MODULE)])["E3AP-OCUDU-V2"]["types"]

    def test_every_libe3_type_has_the_identical_root_in_v2(self):
        self.assertEqual(len(self.libe3), 19)
        for name, definition in self.libe3.items():
            with self.subTest(name):
                self.assertIn(name, self.ocudu)
                self.assertEqual(_root(self.ocudu[name]), _root(definition))

    def test_ocudu_additions_sit_only_behind_libe3_extension_markers(self):
        for name, definition in self.libe3.items():
            members = definition.get("members")
            if not isinstance(members, list):
                continue
            ocudu_members = self.ocudu[name]["members"]
            with self.subTest(name):
                # Same root, then only additions: never a second marker or a root member after them.
                self.assertEqual(_root(ocudu_members[: len(members)]), _root(members))
                self.assertNotIn(None, ocudu_members[len(members):])

    def test_only_the_pdu_choice_gains_a_message(self):
        choice = next(member for member in self.ocudu["E3-PDU"]["members"] if member and member["name"] == "msg")
        additions = choice["members"][choice["members"].index(None) + 1:]
        self.assertEqual([member["name"] for member in additions], ["controlResponse"])


class ByteAlignment(unittest.TestCase):
    """The same bytes both ways, through OCUDU's own Python codec (ocudu_e3)."""

    @classmethod
    def setUpClass(cls):
        from ocudu_e3.libe3 import load_libe3_codec
        from ocudu_e3.schema import load_codec

        cls.ocudu = load_codec(OCUDU_SCHEMA_DIR)
        cls.libe3 = load_libe3_codec(OCUDU_SCHEMA_DIR)
        cls.document = json.loads(VECTORS.read_text())

    def test_ocudu_codec_reproduces_every_libe3_vector(self):
        for vector in self.document["vectors"]:
            with self.subTest(vector["name"]):
                encoded = bytes.fromhex(vector["hex"])
                decoded = self.ocudu.decode("E3-PDU", encoded)
                self.assertEqual(self.ocudu.encode("E3-PDU", decoded).hex(), vector["hex"])

    def test_a_libe3_decoder_reads_ocudu_profile_pdus_as_the_libe3_message(self):
        cases = {
            "setupRequest": ({"e3apProtocolVersion": b"1.0.0", "dAppName": b"x", "dAppVersion": b"0", "vendor": b"v"},
                             {"ocuduProfile": 1, "dAppInstance": bytes(16)}),
            "messageAck": ({"requestId": 1, "responseCode": "negative"},
                           {"cause": "setupRequired", "detail": b"complete setup first"}),
            "indicationMessage": ({"dAppIdentifier": 101, "ranFunctionIdentifier": 10, "protocolData": b"\x01"},
                                  {"subscriptionId": 4, "telemetryIdentifier": 2, "payloadBytes": 1}),
            "subscriptionRequest": ({"dAppIdentifier": 101, "ranFunctionIdentifier": 10,
                                     "telemetryIdentifierList": [2], "controlIdentifierList": [], "periodicity": 10},
                                    {"subscriptionData": b"\x00", "deliveryMode": "byReference",
                                     "subscriptionTimeLong": 86400}),
        }
        for name, (root, additions) in cases.items():
            with self.subTest(name):
                ocudu = self.ocudu.encode("E3-PDU", {"id": 7, "timestamp": 1, "msg": (name, {**root, **additions})})
                plain = self.ocudu.encode("E3-PDU", {"id": 7, "timestamp": 1, "msg": (name, root)})
                seen = self.libe3.decode("E3-PDU", ocudu)
                self.assertEqual(seen, self.libe3.decode("E3-PDU", plain))
                self.assertEqual(self.libe3.encode("E3-PDU", seen), plain)


class UpstreamEncoderVectors(unittest.TestCase):
    """Bytes produced by libe3's own encoder (Asn1E3Encoder, asn1c) against Python.

    ``ocudu_e3.libe3`` compiles the vendored grammar the way asn1c encodes it, so a
    Python peer must reproduce every libe3 vector byte for byte -- all eleven
    procedures, including the two that carry text. The vectors are pinned in
    fixtures/libe3_vectors.json by scripts/regen_libe3_vectors.py.
    """

    VECTORS = HERE / "fixtures" / "libe3_vectors.json"

    @classmethod
    def setUpClass(cls):
        import json

        from ocudu_e3.libe3 import Libe3Codec, load_libe3_codec

        cls.codec = Libe3Codec(OCUDU_SCHEMA_DIR)
        cls.raw = load_libe3_codec(OCUDU_SCHEMA_DIR)
        cls.strict = asn1tools.compile_files([str(LIBE3_MODULE)], "per")
        cls.document = json.loads(cls.VECTORS.read_text())

    def test_one_vector_per_libe3_procedure(self):
        names = [vector["name"] for vector in self.document["vectors"]]
        expected = ["SetupRequest", "SetupResponse", "SubscriptionRequest", "SubscriptionDelete",
                    "SubscriptionResponse", "IndicationMessage", "DAppControlAction", "DAppReport",
                    "XAppControlAction", "ReleaseMessage", "MessageAck"]
        self.assertEqual(names, expected)

    # Why ocudu_e3.libe3 exists: asn1c (libe3's and OCUDU's generator) treats the
    # SIZE constraint of libe3's UTF8String fields as PER-visible and writes their
    # length as a constrained bit field; a strict X.691 encoder (plain asn1tools)
    # does not, because UTF8String is not a known-multiplier type, and writes an
    # octet-aligned length determinant. Plain asn1tools therefore cannot read
    # libe3's setup PDUs. Pinned so the reason for the asn1c-equivalent compile (used by
    # ocudu_e3 for E3AP-OCUDU-V2 too) stays checked.
    STRICT_X691_DIVERGENT = {"SetupRequest", "SetupResponse"}

    def test_a_strict_x691_encoder_cannot_read_libe3_text_pdus(self):
        divergent = set()
        for vector in self.document["vectors"]:
            try:
                decoded = self.strict.decode("E3-PDU", bytes.fromhex(vector["hex"]))
                if self.strict.encode("E3-PDU", decoded).hex() != vector["hex"]:
                    divergent.add(vector["name"])
            except asn1tools.DecodeError:
                divergent.add(vector["name"])
        self.assertEqual(divergent, self.STRICT_X691_DIVERGENT)

    def test_python_reproduces_every_libe3_vector_byte_for_byte(self):
        for vector in self.document["vectors"]:
            with self.subTest(vector["name"]):
                encoded = bytes.fromhex(vector["hex"])
                decoded = self.codec.decode(encoded)
                self.assertEqual(decoded["id"], self.document["message_id"])
                self.assertEqual(decoded["timestamp"], self.document["timestamp"])
                fields = vector["fields"]
                self.assertEqual(decoded["msg"][0].lower(), fields["name"].lower())
                body = decoded["msg"][1]
                for key, value in fields.items():
                    if key in ("name", "id", "timestamp"):
                        continue
                    self.assertIn(key, body, f"{vector['name']}.{key}")
                    got = body[key]
                    if isinstance(got, bytes):
                        got = got.hex()
                    if key == "ranFunctionList":
                        got = [
                            {
                                "ranFunctionIdentifier": item["ranFunctionIdentifier"],
                                "telemetryIdentifierList": item["telemetryIdentifierList"],
                                "controlIdentifierList": item["controlIdentifierList"],
                                "ranFunctionData": item["ranFunctionData"].hex(),
                            }
                            for item in got
                        ]
                    self.assertEqual(got, value, f"{vector['name']}.{key}")
                self.assertEqual(self.codec.encode(decoded).hex(), vector["hex"], vector["name"])

    def test_text_fields_are_utf8_strings_and_malformed_utf8_is_rejected(self):
        setup = {"id": 1, "msg": ("setupRequest", {"e3apProtocolVersion": "1.0.0", "dAppName": "dApp-\u00e9",
                                                     "dAppVersion": "0.2.1", "vendor": "v"})}
        self.assertEqual(self.codec.decode(self.codec.encode(setup))["msg"][1]["dAppName"], "dApp-\u00e9")
        bad = self.raw.encode("E3-PDU", {"id": 1, "msg": ("setupRequest", {
            "e3apProtocolVersion": b"1.0.0", "dAppName": b"\xff\xfe", "dAppVersion": b"0", "vendor": b"v"})})
        with self.assertRaises(UnicodeDecodeError):
            self.codec.decode(bad)


if __name__ == "__main__":
    unittest.main(verbosity=2)
