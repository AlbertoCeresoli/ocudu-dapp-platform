# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear
"""Offline unit tests for the ocudu_e3 Python client.

Runs with no server and no network (``python3 test_offline_units.py`` or via
``python3 -m unittest``): fragment reassembly, the packed telemetry store
layouts, the management builders and ``normalize()``, and Class-C FlatBuffers
decoding from envelopes built with the vendored bindings.
"""

from __future__ import annotations

import math
import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import ocudu_e3  # noqa: E402
from ocudu_e3 import management as mgmt  # noqa: E402
from ocudu_e3.e3ap import E3ProtocolError, _FragmentAssembly  # noqa: E402
from ocudu_e3.schema import STORE_INTERFACES  # noqa: E402
from ocudu_e3.telemetry import (  # noqa: E402
    _DMRS,
    _GRANT,
    _GRID,
    _HEADER,
    _MAGIC,
    _PUSCH_CRC,
    _PUSCH_SYMBOLS,
    _SRS,
    _SRS_PILOTS,
    _SYMBOLS_V2_TAIL,
    PuschCrcRecord,
    PuschDmrsChannelRecord,
    PuschGridRecord,
    PuschSymbolsRecord,
    SrsChannelRecord,
    decode_store_publication,
)


class FragmentAssemblyTests(unittest.TestCase):
    """_FragmentAssembly reassembles indication/control fragments and rejects abuse."""

    BOUND = 64

    def setUp(self):
        self.assembly = _FragmentAssembly(self.BOUND)

    def test_single_fragment_passthrough(self):
        self.assertEqual(self.assembly.add(("k",), b"abc", 0, 1, 3), b"abc")
        self.assertEqual(self.assembly.add(("k",), b"abc", 0, 1, None), b"abc")
        self.assertEqual(self.assembly.add(("k",), b"abc", 0, 0, None), b"abc")

    def test_single_fragment_length_mismatch_rejected(self):
        with self.assertRaises(E3ProtocolError):
            self.assembly.add(("k",), b"abc", 0, 1, 4)

    def test_in_order_multi_fragment_reassembly(self):
        key = (7, 12)
        self.assertIsNone(self.assembly.add(key, b"aa", 0, 3, 6))
        self.assertIsNone(self.assembly.add(key, b"bb", 1, 3, 6))
        self.assertEqual(self.assembly.add(key, b"cc", 2, 3, 6), b"aabbcc")
        # The state is consumed: replaying a middle fragment has no first fragment.
        with self.assertRaises(E3ProtocolError):
            self.assembly.add(key, b"bb", 1, 3, 6)

    def test_interleaved_keys_do_not_mix(self):
        self.assertIsNone(self.assembly.add((1, 1), b"AA", 0, 2, 4))
        self.assertIsNone(self.assembly.add((2, 9), b"xx", 0, 2, 4))
        self.assertEqual(self.assembly.add((1, 1), b"BB", 1, 2, 4), b"AABB")
        self.assertEqual(self.assembly.add((2, 9), b"yy", 1, 2, 4), b"xxyy")

    def test_out_of_order_fragment_rejected_and_state_dropped(self):
        key = ("ooo",)
        self.assertIsNone(self.assembly.add(key, b"aa", 0, 3, 6))
        with self.assertRaises(E3ProtocolError):
            self.assembly.add(key, b"cc", 2, 3, 6)
        # The partial buffer was discarded, so the correct next index now fails too.
        with self.assertRaises(E3ProtocolError):
            self.assembly.add(key, b"bb", 1, 3, 6)

    def test_fragment_count_change_rejected(self):
        key = ("count",)
        self.assertIsNone(self.assembly.add(key, b"aa", 0, 3, 6))
        with self.assertRaises(E3ProtocolError):
            self.assembly.add(key, b"bb", 1, 4, 6)

    def test_payload_bytes_mismatch_rejected(self):
        key = ("len",)
        self.assertIsNone(self.assembly.add(key, b"aa", 0, 2, 5))
        with self.assertRaises(E3ProtocolError):
            self.assembly.add(key, b"bb", 1, 2, 5)

    def test_oversize_total_rejected_on_first_fragment(self):
        with self.assertRaises(E3ProtocolError):
            self.assembly.add(("big",), b"x", 0, 2, self.BOUND + 1)

    def test_missing_payload_bytes_rejected_on_first_fragment(self):
        with self.assertRaises(E3ProtocolError):
            self.assembly.add(("nolen",), b"x", 0, 2, None)

    def test_lying_total_cannot_smuggle_past_the_bound(self):
        key = ("smuggle",)
        self.assertIsNone(self.assembly.add(key, b"a" * 40, 0, 2, 60))
        with self.assertRaises(E3ProtocolError):
            self.assembly.add(key, b"b" * 40, 1, 2, 60)

    def test_missing_first_fragment_rejected(self):
        with self.assertRaises(E3ProtocolError):
            self.assembly.add(("orphan",), b"aa", 1, 3, 6)


# ---------------------------------------------------------------------------
# Packed telemetry store layouts
# ---------------------------------------------------------------------------

# Static sizes pinned in ocudu/include/ocudu/dapp/e3/telemetry_types.h:254-262.
SIZEOF_HEADER = 16
SIZEOF_PUSCH_SYMBOLS_V1 = 56
SIZEOF_GRANT = 92
SIZEOF_PUSCH_SYMBOLS_V2 = 152
SIZEOF_PUSCH_GRID = 120
SIZEOF_PUSCH_CRC = 26
SIZEOF_SRS = 66
SIZEOF_DMRS = 70

GRANT_PRB_MASK = bytes([0x0F]) + bytes(34)  # CRBs 0..3 allocated

GRANT_VALUES = dict(
    harq_id=5,
    rv=2,
    new_data=1,
    ldpc_base_graph=1,
    n_id=123,
    bwp_start_rb=4,
    bwp_size_rb=51,
    dmrs_symbol_mask=0x2104,
    dmrs_scrambling_id=301,
    dc_position=200,
    nof_harq_ack_bits=2,
    nof_csi_part1_bits=11,
    n_rs_id=17,
    tbs_bytes=3824,
    tbs_lbrm_bytes=25344,
    mcs_index=16,
    mcs_table=1,
    start_symbol=2,
    nof_symbols=12,
    dmrs_type=1,
    nof_cdm_groups_without_data=2,
    nof_rx_ports=4,
    nof_layers=2,
    cyclic_prefix=0,
    alpha_scaling=0.5,
    beta_offset_harq_ack=2.0,
    beta_offset_csi_part1=6.25,
    beta_offset_csi_part2=1.125,
)


def packed_grant(flags: int) -> bytes:
    """Pack a pusch_grant_metadata_v1 block with GRANT_VALUES and the given flags."""
    g = GRANT_VALUES
    return _GRANT.pack(
        g["harq_id"], g["rv"], g["new_data"], g["ldpc_base_graph"],
        g["n_id"], g["bwp_start_rb"], g["bwp_size_rb"], g["dmrs_symbol_mask"], g["dmrs_scrambling_id"],
        g["dc_position"], g["nof_harq_ack_bits"], g["nof_csi_part1_bits"], g["n_rs_id"],
        g["tbs_bytes"], g["tbs_lbrm_bytes"],
        flags, g["mcs_index"], g["mcs_table"], g["start_symbol"], g["nof_symbols"],
        g["dmrs_type"], g["nof_cdm_groups_without_data"], g["nof_rx_ports"], g["nof_layers"], g["cyclic_prefix"],
        g["alpha_scaling"], g["beta_offset_harq_ack"], g["beta_offset_csi_part1"], g["beta_offset_csi_part2"],
        GRANT_PRB_MASK,
    )


def header(kind: int, version: int = 1, monotonic_ns: int = 777) -> bytes:
    return _HEADER.pack(_MAGIC, version, kind, monotonic_ns)


class PackedStoreLayoutTests(unittest.TestCase):
    """The Python struct layouts mirror the packed C++ records byte for byte."""

    def test_struct_sizes_match_the_host_static_asserts(self):
        self.assertEqual(_HEADER.size, SIZEOF_HEADER)
        self.assertEqual(_HEADER.size + _PUSCH_SYMBOLS.size, SIZEOF_PUSCH_SYMBOLS_V1)
        self.assertEqual(_GRANT.size, SIZEOF_GRANT)
        self.assertEqual(
            _HEADER.size + _PUSCH_SYMBOLS.size + _SYMBOLS_V2_TAIL.size + _GRANT.size,
            SIZEOF_PUSCH_SYMBOLS_V2,
        )
        self.assertEqual(_HEADER.size + _GRID.size + _GRANT.size, SIZEOF_PUSCH_GRID)
        self.assertEqual(_HEADER.size + _PUSCH_CRC.size, SIZEOF_PUSCH_CRC)
        self.assertEqual(_HEADER.size + _SRS.size, SIZEOF_SRS)
        self.assertEqual(_HEADER.size + _DMRS.size, SIZEOF_DMRS)

    # -- pusch_symbols v2 + grant ------------------------------------------
    def _symbols_v2(self, symbol_flags: int, grant_flags: int = 0x1F, sinr: float = 21.5) -> bytes:
        v1_body = _PUSCH_SYMBOLS.pack(
            0x4601, 100, 3, 1,  # rnti, sfn, slot, numerology
            52, 12, 1, 4, 0,    # nof_prb, nof_data_symbols, nof_layers, modulation_bits, reserved
            sinr, -10.0, -12.5, 0.25, 0.5, -25.0, 0.625,
        )
        metadata = header(kind=1, version=2) + v1_body + _SYMBOLS_V2_TAIL.pack(symbol_flags) + packed_grant(grant_flags)
        self.assertEqual(len(metadata), SIZEOF_PUSCH_SYMBOLS_V2)
        return metadata

    def test_pusch_symbols_v2_grant_round_trip(self):
        tensor = struct.pack("<8f", 1.0, -1.0, 0.5, 0.25, -0.5, 0.75, 2.0, -2.0)
        record = decode_store_publication(
            STORE_INTERFACES["pusch.symbols"], self._symbols_v2(0x08 | 0x01 | 0x02), tensor
        )
        self.assertIsInstance(record, PuschSymbolsRecord)
        self.assertEqual(record.rnti, 0x4601)
        self.assertEqual((record.radio_slot.sfn, record.radio_slot.slot, record.radio_slot.numerology), (100, 3, 1))
        self.assertEqual(record.nof_prb, 52)
        self.assertEqual(record.nof_data_symbols, 12)
        self.assertEqual(record.nof_layers, 1)
        self.assertEqual(record.modulation_bits, 4)
        self.assertEqual(record.sinr_db, 21.5)
        self.assertEqual(record.epre_db, -10.0)
        self.assertEqual(record.rsrp_db, -12.5)
        self.assertEqual(record.evm, 0.25)
        self.assertEqual(record.time_alignment_us, 0.5)
        self.assertEqual(record.cfo_hz, -25.0)
        self.assertEqual(record.target_code_rate, 0.625)
        self.assertEqual(record.equalized_symbols, tensor)
        self.assertIs(record.crc_ok, True)
        self.assertFalse(record.symbols_truncated)
        grant = record.grant
        self.assertIsNotNone(grant)
        for name, expected in GRANT_VALUES.items():
            self.assertEqual(getattr(grant, name), expected, f"grant field {name}")
        self.assertEqual(grant.prb_mask, GRANT_PRB_MASK)
        self.assertEqual(grant.prbs(), [0, 1, 2, 3])
        # flags = 0x1F: mcs known, transform precoding, n_scid, dc present, csi part 2 expected
        self.assertTrue(grant.transform_precoding)
        self.assertTrue(grant.n_scid)
        self.assertTrue(grant.csi_part2_expected)
        self.assertEqual(grant.mcs_table_name, "qam256")

    def test_pusch_symbols_v2_grant_optional_fields_absent_when_flags_clear(self):
        record = decode_store_publication(
            STORE_INTERFACES["pusch.symbols"], self._symbols_v2(0x08, grant_flags=0x00), b""
        )
        grant = record.grant
        self.assertIsNotNone(grant)
        self.assertIsNone(grant.mcs_index)
        self.assertIsNone(grant.mcs_table)
        self.assertIsNone(grant.mcs_table_name)
        self.assertIsNone(grant.dc_position)
        self.assertIsNone(grant.n_rs_id)
        self.assertFalse(grant.transform_precoding)
        self.assertFalse(grant.n_scid)
        self.assertFalse(grant.csi_part2_expected)

    def test_pusch_symbols_v2_flags_without_grant_or_crc(self):
        # symbol_flags: only bit2 (symbols_truncated); no crc verdict, no grant.
        record = decode_store_publication(STORE_INTERFACES["pusch.symbols"], self._symbols_v2(0x04), b"")
        self.assertIsNone(record.grant)
        self.assertIsNone(record.crc_ok)
        self.assertTrue(record.symbols_truncated)
        # crc known (bit0) but failed (bit1 clear)
        record = decode_store_publication(STORE_INTERFACES["pusch.symbols"], self._symbols_v2(0x01), b"")
        self.assertIs(record.crc_ok, False)

    def test_pusch_symbols_non_finite_measurements_become_none(self):
        record = decode_store_publication(
            STORE_INTERFACES["pusch.symbols"], self._symbols_v2(0x08, sinr=float("nan")), b""
        )
        self.assertIsNone(record.sinr_db)
        self.assertEqual(record.epre_db, -10.0)

    # -- pusch_crc (26 B) ---------------------------------------------------
    def test_pusch_crc_round_trip(self):
        metadata = header(kind=2) + _PUSCH_CRC.pack(0x4602, 512, 7, 2, 1)
        self.assertEqual(len(metadata), SIZEOF_PUSCH_CRC)
        record = decode_store_publication(STORE_INTERFACES["pusch.crc"], metadata, b"\x01")
        self.assertIsInstance(record, PuschCrcRecord)
        self.assertEqual(record.rnti, 0x4602)
        self.assertEqual((record.radio_slot.sfn, record.radio_slot.slot, record.radio_slot.numerology), (512, 7, 2))
        self.assertIs(record.crc_ok, True)
        metadata = header(kind=2) + _PUSCH_CRC.pack(0x4602, 512, 7, 2, 0)
        self.assertIs(decode_store_publication(STORE_INTERFACES["pusch.crc"], metadata, b"\x00").crc_ok, False)

    # -- srs (66 B) ---------------------------------------------------------
    def test_srs_round_trip(self):
        body = _SRS.pack(
            0x17, 900, 15, 1,          # rnti, sfn, slot, numerology
            816,                        # nof_subcarriers
            4, 4, 2, 3, 1, 12, 2, 2, 5, 1, 7, 1,  # 12 u8 fields
            30, 500, 10, 1, 3, 12, 6,   # 7 u16 fields
            -70.5, -72.0, 0.125, float("nan"),  # rsrp, epre, noise variance, time alignment
        )
        metadata = header(kind=3) + body
        self.assertEqual(len(metadata), SIZEOF_SRS)
        tensor = struct.pack("<4f", 1.0, 0.0, 0.0, -1.0)
        record = decode_store_publication(STORE_INTERFACES["srs.channel"], metadata, tensor)
        self.assertIsInstance(record, SrsChannelRecord)
        self.assertEqual(record.rnti, 0x17)
        self.assertEqual((record.radio_slot.sfn, record.radio_slot.slot, record.radio_slot.numerology), (900, 15, 1))
        self.assertEqual(record.nof_subcarriers, 816)
        self.assertEqual(record.comb_size, 4)
        self.assertEqual(record.nof_rx_ports, 4)
        self.assertEqual(record.nof_tx_ports, 2)
        self.assertEqual(record.rx_port_index, 3)
        self.assertEqual(record.tx_port_index, 1)
        self.assertEqual(record.start_symbol, 12)
        self.assertEqual(record.nof_symbols, 2)
        self.assertEqual(record.comb_offset, 2)
        self.assertEqual(record.cyclic_shift, 5)
        self.assertEqual(record.bandwidth_index, 1)
        self.assertEqual(record.frequency_position, 7)
        self.assertEqual(record.frequency_hopping, 1)
        self.assertEqual(record.mapping_initial_subcarrier, 30)
        self.assertEqual(record.sequence_id, 500)
        self.assertEqual(record.sequence_group, 10)
        self.assertEqual(record.sequence_number, 1)
        self.assertEqual(record.cyclic_shift_index, 3)
        self.assertEqual(record.cyclic_shift_maximum, 12)
        self.assertEqual(record.frequency_shift, 6)
        self.assertEqual(record.rsrp_db, -70.5)
        self.assertEqual(record.epre_db, -72.0)
        self.assertEqual(record.noise_variance, 0.125)
        self.assertIsNone(record.time_alignment_ns)  # NaN sanitized to None
        self.assertEqual(record.coefficients, tensor)
        samples = list(record.samples())
        self.assertEqual(complex(samples[0]), 1.0 + 0.0j)
        self.assertEqual(complex(samples[1]), -1.0j)
        self.assertEqual(record.received_pilots, b"")

    # -- srs v2 (70 B): the dApp input rides after the estimate ---------------
    def test_srs_v2_splits_estimate_pilots_and_reference(self):
        body = _SRS.pack(0x17, 900, 15, 1, 2, 4, 1, 1, 0, 0, 12, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 12, 0,
                         -70.0, -72.0, 0.1, 150.0) + _SRS_PILOTS.pack(2, 2, 0)
        metadata = header(kind=3) + body
        estimate = struct.pack("<4f", 1.0, 0.0, 0.0, 1.0)
        pilots = struct.pack("<8f", 1.0, 0.0, 0.0, 1.0, -1.0, 0.0, 0.0, -1.0)
        reference = struct.pack("<4f", 1.0, 0.0, 0.0, -1.0)
        record = decode_store_publication(STORE_INTERFACES["srs.channel"], metadata, estimate + pilots + reference)
        self.assertEqual(record.coefficients, estimate)
        self.assertEqual(record.received_pilots, pilots)
        self.assertEqual(record.reference_symbols, reference)
        self.assertEqual([complex(value) for value in record.pilots()], [1, 1j, -1, -1j])
        self.assertEqual([complex(value) for value in record.reference()], [1, -1j])
        with self.assertRaises(ValueError):
            decode_store_publication(STORE_INTERFACES["srs.channel"], metadata + b"\x00", estimate)

    # -- dmrs (70 B) --------------------------------------------------------
    def test_dmrs_round_trip(self):
        body = _DMRS.pack(
            0x4603, 33, 4, 0,           # rnti, sfn, slot, numerology
            10, 20, 120,                # start_prb, nof_prb, nof_subcarriers
            2, 0, 14, 4, 2, 1, 0, 1, 2, 1, 1, 0,  # 12 u8 fields (incl. reserved)
            611,                        # scrambling_id
            18.5, -9.0, -11.0, 0.0625, -0.25, 40.0, 0.5,
        )
        metadata = header(kind=4) + body
        self.assertEqual(len(metadata), SIZEOF_DMRS)
        record = decode_store_publication(STORE_INTERFACES["pusch.dmrs_channel"], metadata, b"")
        self.assertIsInstance(record, PuschDmrsChannelRecord)
        self.assertEqual(record.rnti, 0x4603)
        self.assertEqual((record.radio_slot.sfn, record.radio_slot.slot, record.radio_slot.numerology), (33, 4, 0))
        self.assertEqual(record.start_prb, 10)
        self.assertEqual(record.nof_prb, 20)
        self.assertEqual(record.nof_subcarriers, 120)
        self.assertEqual(record.dmrs_symbol_index, 2)
        self.assertEqual(record.start_symbol, 0)
        self.assertEqual(record.nof_symbols, 14)
        self.assertEqual(record.nof_rx_ports, 4)
        self.assertEqual(record.nof_tx_layers, 2)
        self.assertEqual(record.rx_port_index, 1)
        self.assertEqual(record.tx_layer_index, 0)
        self.assertEqual(record.dmrs_type, 1)
        self.assertEqual(record.nof_cdm_groups_without_data, 2)
        self.assertTrue(record.transform_precoding)
        self.assertTrue(record.n_scid)
        self.assertEqual(record.scrambling_id, 611)
        self.assertEqual(record.sinr_db, 18.5)
        self.assertEqual(record.epre_db, -9.0)
        self.assertEqual(record.rsrp_db, -11.0)
        self.assertEqual(record.noise_variance, 0.0625)
        self.assertEqual(record.time_alignment_us, -0.25)
        self.assertEqual(record.cfo_hz, 40.0)
        self.assertEqual(record.target_code_rate, 0.5)

    # -- grid (120 B) -------------------------------------------------------
    def test_pusch_grid_round_trip(self):
        metadata = header(kind=6) + _GRID.pack(0x4601, 33, 4, 1, 52, 4, 0, 14) + packed_grant(0x1F)
        self.assertEqual(len(metadata), SIZEOF_PUSCH_GRID)
        tensor = struct.pack("<4f", 0.5, -0.5, 1.5, -1.5)
        record = decode_store_publication(STORE_INTERFACES["pusch.grid"], metadata, tensor)
        self.assertIsInstance(record, PuschGridRecord)
        self.assertEqual(record.rnti, 0x4601)
        self.assertEqual((record.radio_slot.sfn, record.radio_slot.slot, record.radio_slot.numerology), (33, 4, 1))
        self.assertEqual(record.nof_prb, 52)
        self.assertEqual(record.nof_rx_ports, 4)
        self.assertEqual(record.start_symbol, 0)
        self.assertEqual(record.nof_symbols, 14)
        self.assertEqual(record.resource_elements, tensor)
        for name, expected in GRANT_VALUES.items():
            self.assertEqual(getattr(record.grant, name), expected, f"grid grant field {name}")

    def test_truncated_metadata_and_bad_header_rejected(self):
        good = header(kind=2) + _PUSCH_CRC.pack(1, 2, 3, 0, 1)
        # Truncated header -> ValueError; truncated body -> struct.error.
        with self.assertRaises(ValueError):
            decode_store_publication(STORE_INTERFACES["pusch.crc"], good[: _HEADER.size - 1], b"")
        with self.assertRaises(struct.error):
            decode_store_publication(STORE_INTERFACES["pusch.crc"], good[:-1], b"")
        bad_magic = b"\x00" * 4 + good[4:]
        with self.assertRaises(ValueError):
            decode_store_publication(STORE_INTERFACES["pusch.crc"], bad_magic, b"")
        bad_kind = header(kind=3) + good[_HEADER.size:]
        with self.assertRaises(ValueError):
            decode_store_publication(STORE_INTERFACES["pusch.crc"], bad_kind, b"")


# ---------------------------------------------------------------------------
# Management builders and normalize()
# ---------------------------------------------------------------------------


class ManagementBuilderTests(unittest.TestCase):
    codec = None

    @classmethod
    def setUpClass(cls):
        cls.codec = ocudu_e3.load_codec()

    def _assert_encodes(self, request: dict, expected_operation: str):
        """The request has the builder shape and survives an APER round trip."""
        self.assertEqual(set(request) - {"header", "operation"}, set())
        self.assertIsInstance(request["header"]["transactionId"], bytes)
        self.assertEqual(len(request["header"]["transactionId"]), 16)
        kind, _body = request["operation"]
        self.assertEqual(kind, expected_operation)
        encoded = self.codec.encode("ManagementRequest", request)
        decoded = self.codec.decode("ManagementRequest", encoded)
        self.assertEqual(decoded["operation"][0], expected_operation)
        self.assertEqual(decoded["header"]["transactionId"], request["header"]["transactionId"])
        return decoded

    def test_inventory_request(self):
        request = mgmt.inventory_request(include_packages=False, include_instances=False)
        self.assertEqual(request["operation"][1], {"includePackages": False, "includeInstances": False})
        self._assert_encodes(request, "inventory")
        self.assertEqual(mgmt.inventory_request()["operation"][1], {})

    def test_health_request(self):
        request = mgmt.health_request()
        self.assertIsNone(request["operation"][1])
        self._assert_encodes(request, "health")

    def test_metrics_request(self):
        instance = bytes(range(16))
        request = mgmt.metrics_request(instance, schema=True)
        self.assertEqual(request["header"]["instanceId"], instance)
        self.assertEqual(request["operation"][1], {"kind": "schema"})
        self._assert_encodes(request, "metrics")
        self.assertEqual(mgmt.metrics_request(instance)["operation"][1], {"kind": "snapshot"})

    def test_incidents_request(self):
        request = mgmt.incidents_request(instance_id=b"\x02" * 16, after=5, limit=10)
        body = request["operation"][1]
        self.assertEqual(body["maximumRecords"], 10)
        self.assertEqual(body["afterIncidentId"], (5).to_bytes(8, "big"))
        self.assertEqual(request["header"]["instanceId"], b"\x02" * 16)
        self._assert_encodes(request, "incidents")

    def test_lifecycle_request(self):
        request = mgmt.lifecycle_request("activate", b"\x03" * 16, expected_generation=3, timeout_us=2_000_000)
        body = request["operation"][1]
        self.assertEqual(body["action"], "activate")
        self.assertEqual(body["timeoutUs"], 2_000_000)
        self.assertEqual(request["header"]["expectedInstanceGeneration"], (3).to_bytes(8, "big"))
        self._assert_encodes(request, "lifecycle")

    def test_load_request(self):
        request = mgmt.load_request(b"\x04" * 16, placement="supervisedProcess", backend="cuda")
        body = request["operation"][1]
        self.assertEqual(body["action"], "load")
        self.assertEqual(body["packageId"], b"\x04" * 16)
        self.assertEqual(body["placement"], "supervisedProcess")
        self.assertEqual(body["backend"], "cuda")
        self._assert_encodes(request, "lifecycle")

    def test_jobs_request(self):
        self.assertEqual(mgmt.jobs_request()["operation"][1], {"action": "list"})
        job = b"\x05" * 16
        self.assertEqual(mgmt.jobs_request(job)["operation"][1], {"action": "get", "jobId": job})
        request = mgmt.jobs_request(job, cancel=True)
        self.assertEqual(request["operation"][1], {"action": "cancel", "jobId": job})
        self._assert_encodes(request, "jobs")

    def test_cell_info_request(self):
        request = mgmt.cell_info_request()
        self.assertEqual(request["operation"][1], {})
        self._assert_encodes(request, "cellInfo")

    def test_ran_control_request(self):
        request = mgmt.ran_control_request(
            "handoverUe", serving_pci=1, rnti=0x4601, target_pci=2, dry_run=True
        )
        body = request["operation"][1]
        self.assertEqual(body, {"action": "handoverUe", "servingPci": 1, "rnti": 0x4601, "targetPci": 2})
        self.assertIs(request["header"]["dryRun"], True)
        self._assert_encodes(request, "ranControl")
        gain = mgmt.ran_control_request("setTxGain", port_id=3, gain_db=-6.5)
        self.assertEqual(gain["operation"][1]["portId"], 3)
        self.assertEqual(gain["operation"][1]["gainDb"], -6.5)
        self._assert_encodes(gain, "ranControl")

    def test_config_request(self):
        request = mgmt.config_request(b"\x06" * 16, schema=True)
        self.assertEqual(request["operation"][1], {"kind": "schema"})
        self._assert_encodes(request, "configQuery")
        self.assertEqual(mgmt.config_request(b"\x06" * 16)["operation"][1], {"kind": "effective"})

    def test_parse_and_display_identifier(self):
        text = "00010203-0405-0607-0809-0a0b0c0d0e0f"
        self.assertEqual(mgmt.parse_identifier(text), bytes(range(16)))
        self.assertEqual(mgmt.parse_identifier(text.replace("-", "")), bytes(range(16)))
        self.assertEqual(mgmt.display_identifier(bytes(range(16))), text)
        with self.assertRaises(ValueError):
            mgmt.parse_identifier("abcd")


class NormalizeTests(unittest.TestCase):
    def test_normalize_representative_response(self):
        response = {
            "transactionId": bytes(range(16)),
            "code": "success",
            "payload": (
                "cellInfo",
                {
                    "cells": [
                        {
                            "plmn": b"00101",
                            "pci": 1,
                            "rfCenterFrequencyHz": (3_500_000_000).to_bytes(8, "big"),
                            "cellFrequencyOffsetHz": (-120).to_bytes(8, "big", signed=True),
                            "nrCellIdentity": (0x12345).to_bytes(8, "big"),
                            "vendor": b"DeepSig",
                            "opaque": b"\xde\xad",
                            "flags": (b"\xa0", 3),  # BIT STRING '101'B
                        }
                    ],
                },
            ),
        }
        normalized = mgmt.normalize(response)
        self.assertEqual(normalized["transactionId"], "00010203-0405-0607-0809-0a0b0c0d0e0f")
        self.assertEqual(normalized["code"], "success")
        cell = normalized["payload"]["cellInfo"]["cells"][0]
        self.assertEqual(cell["plmn"], "00101")
        self.assertEqual(cell["pci"], 1)
        self.assertEqual(cell["rfCenterFrequencyHz"], 3_500_000_000)
        self.assertEqual(cell["cellFrequencyOffsetHz"], -120)  # SIGNED_FIELDS decode
        self.assertEqual(cell["nrCellIdentity"], 0x12345)
        self.assertEqual(cell["vendor"], "DeepSig")
        self.assertEqual(cell["opaque"], "hex:dead")
        self.assertEqual(cell["flags"], 0b101)

    def test_normalize_signed_field_in_list_context(self):
        normalized = mgmt.normalize({"signedValue": (-7).to_bytes(8, "big", signed=True)})
        self.assertEqual(normalized["signedValue"], -7)

    def test_unwrap_typed_value(self):
        self.assertEqual(mgmt.unwrap_typed_value(("signedValue", (-5).to_bytes(8, "big", signed=True))), -5)
        self.assertEqual(mgmt.unwrap_typed_value(("unsignedValue", (9).to_bytes(8, "big"))), 9)
        self.assertEqual(mgmt.unwrap_typed_value(("stringValue", b"hi")), "hi")
        self.assertEqual(mgmt.unwrap_typed_value(("bytesValue", b"\x01\x02")), "hex:0102")
        self.assertEqual(mgmt.unwrap_typed_value(42), 42)


# ---------------------------------------------------------------------------
# Class-C FlatBuffers decoding
# ---------------------------------------------------------------------------

try:
    import flatbuffers  # noqa: F401
    import numpy as np

    from ocudu_e3 import classc  # adds _classc_fbs to sys.path
    from Ocudu.Dapp.ClassC.V1 import (  # noqa: E402
        ElementType,
        Id128,
        ObservationContext,
        PublicationEnvelope,
        PublicationKind,
        PublicationMetadata,
        RadioTime,
        SpectrumMetadata,
        SrsIsacMetadata,
        TensorDescriptor,
    )

    HAVE_CLASSC = True
except ImportError:  # pragma: no cover - optional dependency missing
    HAVE_CLASSC = False


def _build_context(builder, *, sfn, slot, numerology, cell_id, obs_ns, ref_hz, ue_token_low=None):
    ObservationContext.Start(builder)
    ObservationContext.AddRadioTime(
        builder, RadioTime.CreateRadioTime(builder, sfn, slot, numerology, 0x3FFF)
    )
    ObservationContext.AddCellId(builder, cell_id)
    ObservationContext.AddObservationTimeNs(builder, obs_ns)
    if ue_token_low is not None:
        ObservationContext.AddUeToken(builder, Id128.CreateId128(builder, ue_token_low, 0))
    ObservationContext.AddReferenceFrequencyHz(builder, ref_hz)
    return ObservationContext.End(builder)


def _build_tensor_descriptor(builder, element_type, byte_length):
    TensorDescriptor.Start(builder)
    TensorDescriptor.AddElementType(builder, element_type)
    TensorDescriptor.AddByteOffset(builder, 0)
    TensorDescriptor.AddByteLength(builder, byte_length)
    return TensorDescriptor.End(builder)


def _finish_envelope(builder, *, kind, metadata_type, metadata_off, context_off, tensor_off, sequence):
    PublicationEnvelope.Start(builder)
    PublicationEnvelope.AddPublicationKind(builder, kind)
    PublicationEnvelope.AddPublicationSequence(builder, sequence)
    PublicationEnvelope.AddContext(builder, context_off)
    PublicationEnvelope.AddTensor(builder, tensor_off)
    PublicationEnvelope.AddMetadataType(builder, metadata_type)
    PublicationEnvelope.AddMetadata(builder, metadata_off)
    envelope = PublicationEnvelope.End(builder)
    builder.Finish(envelope, file_identifier=b"OCC3")
    return bytes(builder.Output())


def _float32_to_bf16_bytes(values: "np.ndarray") -> bytes:
    """Truncate float32 to bfloat16 (the top half of the binary32)."""
    return ((values.astype(np.float32).view("<u4") >> 16).astype("<u2")).tobytes()


@unittest.skipUnless(HAVE_CLASSC, "flatbuffers and numpy are required for Class-C tests")
class ClassCDecodeTests(unittest.TestCase):
    RX, SYMBOLS, SUBCARRIERS = 2, 2, 24  # 2 PRBs
    SCALE = 2.0

    def _spectrum_envelope(self):
        n = self.RX * self.SYMBOLS * self.SUBCARRIERS
        grid = (np.arange(n, dtype=np.float32) - 3.0) + 1j * (np.arange(n, dtype=np.float32) * 0.5)
        grid = grid.astype(np.complex64)
        tensor = (grid * self.SCALE).astype(np.complex64).tobytes()

        builder = flatbuffers.Builder(1024)
        occupancy = builder.CreateByteVector(bytes([2, 3, 4, 0]))  # symbol_prb: 2 symbols x 2 PRBs
        SpectrumMetadata.Start(builder)
        SpectrumMetadata.AddBandwidthHz(builder, 20_000_000)
        SpectrumMetadata.AddRxPorts(builder, self.RX)
        SpectrumMetadata.AddSymbols(builder, self.SYMBOLS)
        SpectrumMetadata.AddSubcarriers(builder, self.SUBCARRIERS)
        SpectrumMetadata.AddScale(builder, self.SCALE)
        SpectrumMetadata.AddOccupancyAnnotations(builder, occupancy)
        SpectrumMetadata.AddPlanGeneration(builder, 9)
        SpectrumMetadata.AddOccupancyGranularity(builder, 2)  # symbol_prb
        SpectrumMetadata.AddOccupancyComplete(builder, True)
        meta = SpectrumMetadata.End(builder)
        descriptor = _build_tensor_descriptor(builder, ElementType.ElementType.ComplexFp32, len(tensor))
        context = _build_context(
            builder, sfn=200, slot=5, numerology=1, cell_id=3, obs_ns=123_456_789, ref_hz=3_500_000_000
        )
        metadata = _finish_envelope(
            builder,
            kind=PublicationKind.PublicationKind.Spectrum,
            metadata_type=PublicationMetadata.PublicationMetadata.SpectrumMetadata,
            metadata_off=meta,
            context_off=context,
            tensor_off=descriptor,
            sequence=42,
        )
        return metadata, tensor, grid

    def test_decode_spectrum(self):
        metadata, tensor, grid = self._spectrum_envelope()
        snapshot = classc.decode_spectrum(metadata, tensor)
        self.assertEqual(snapshot.publication_sequence, 42)
        self.assertEqual((snapshot.sfn, snapshot.slot, snapshot.numerology), (200, 5, 1))
        self.assertEqual(snapshot.cell_id, 3)
        self.assertEqual(snapshot.observation_time_ns, 123_456_789)
        self.assertEqual(snapshot.reference_frequency_hz, 3_500_000_000)
        self.assertEqual(snapshot.bandwidth_hz, 20_000_000)
        self.assertEqual((snapshot.rx_ports, snapshot.symbols, snapshot.subcarriers), (2, 2, 24))
        self.assertEqual(snapshot.scale, self.SCALE)
        self.assertEqual(snapshot.plan_generation, 9)
        self.assertEqual(snapshot.occupancy_granularity, "symbol_prb")
        self.assertTrue(snapshot.occupancy_complete)
        self.assertEqual(snapshot.nof_prb, 2)
        self.assertEqual(snapshot.subcarrier_spacing_hz, 30_000)
        self.assertEqual(snapshot.grid.shape, (2, 2, 24))
        self.assertEqual(snapshot.grid.dtype, np.complex64)
        # The decoder divides the scaled tensor back down.
        np.testing.assert_allclose(snapshot.grid.reshape(-1), grid, rtol=1e-6)
        self.assertIsNotNone(snapshot.occupancy)
        self.assertEqual(snapshot.occupancy.shape, (2, 2))
        np.testing.assert_array_equal(snapshot.occupancy, np.array([[2, 3], [4, 0]]))
        self.assertEqual(snapshot.prb_power().shape, (2, 2))
        centers = snapshot.prb_center_frequencies_hz()
        self.assertEqual(centers.shape, (2,))
        self.assertAlmostEqual(centers[1] - centers[0], 12 * 30_000)

    def test_decode_spectrum_rejects_bad_identifier_and_kind(self):
        with self.assertRaises(ValueError):
            classc.decode_spectrum(b"\x00" * 32, b"")
        metadata, tensor, _grid = self._spectrum_envelope()
        with self.assertRaises(ValueError):
            classc.decode_srs_isac(metadata, tensor)  # spectrum envelope is not SrsIsac

    def test_decode_spectrum_rejects_tensor_size_mismatch(self):
        metadata, tensor, _grid = self._spectrum_envelope()
        with self.assertRaises(ValueError):
            classc.decode_spectrum(metadata, tensor[:-8])

    def test_decode_spectrum_with_a_grid_window(self):
        # [srs-sharing] A region-cropped tensor holds only the window; the metadata still describes the full grid.
        metadata, _tensor, grid = self._spectrum_envelope()
        full = grid.reshape(self.RX, self.SYMBOLS, self.SUBCARRIERS)
        window = ocudu_e3.GridWindow(first_symbol=1, nof_symbols=1, first_subcarrier=12, nof_subcarriers=12)
        cropped = (full[:, 1:2, 12:24] * self.SCALE).astype(np.complex64).tobytes()
        snapshot = classc.decode_spectrum(metadata, cropped, window)
        self.assertEqual(snapshot.grid.shape, (2, 1, 12))
        self.assertEqual((snapshot.first_symbol, snapshot.first_subcarrier), (1, 12))
        self.assertEqual((snapshot.symbols, snapshot.subcarriers), (2, 24))  # source grid
        np.testing.assert_allclose(snapshot.grid, full[:, 1:2, 12:24], rtol=1e-6)
        self.assertEqual(snapshot.prb_power().shape, (1, 1))
        with self.assertRaises(ValueError):
            classc.decode_spectrum(metadata, cropped[:-8], window)
        with self.assertRaises(ValueError):
            outside = ocudu_e3.GridWindow(first_symbol=1, nof_symbols=2, first_subcarrier=0, nof_subcarriers=12)
            classc.decode_spectrum(metadata, cropped, outside)

    def test_decode_srs_isac_bf16(self):
        rx, tx, points = 2, 1, 8
        n = rx * tx * points
        # Values exactly representable in bfloat16 so the round trip is exact.
        re = np.array([1.0, -2.0, 0.5, 4.0, -0.25, 3.0, -1.5, 8.0] * (n // 8), dtype=np.float32)
        im = np.array([0.5, 1.0, -4.0, 0.125, 2.0, -8.0, 0.75, -1.0] * (n // 8), dtype=np.float32)
        interleaved = np.empty(2 * n, dtype=np.float32)
        interleaved[0::2] = re
        interleaved[1::2] = im
        tensor = _float32_to_bf16_bytes(interleaved)

        builder = flatbuffers.Builder(1024)
        SrsIsacMetadata.Start(builder)
        SrsIsacMetadata.AddSrsResourceId(builder, 3)
        SrsIsacMetadata.AddSrsConfigId(builder, 1)
        SrsIsacMetadata.AddRxPorts(builder, rx)
        SrsIsacMetadata.AddTxPorts(builder, tx)
        SrsIsacMetadata.AddCombSize(builder, 4)
        SrsIsacMetadata.AddCombOffset(builder, 1)
        SrsIsacMetadata.AddFirstSubcarrier(builder, 24)
        SrsIsacMetadata.AddSubcarrierSpacingHz(builder, 30_000)
        SrsIsacMetadata.AddFrequencyPoints(builder, points)
        SrsIsacMetadata.AddNoiseVariance(builder, 0.125)
        SrsIsacMetadata.AddRsrp(builder, -70.5)
        SrsIsacMetadata.AddEpre(builder, -72.0)
        SrsIsacMetadata.AddTimingAdvanceNs(builder, -40)
        SrsIsacMetadata.AddCfoHz(builder, 120)
        meta = SrsIsacMetadata.End(builder)
        descriptor = _build_tensor_descriptor(builder, ElementType.ElementType.ComplexBf16, len(tensor))
        context = _build_context(
            builder, sfn=10, slot=2, numerology=1, cell_id=1, obs_ns=55, ref_hz=3_600_000_000,
            ue_token_low=0xABCD4601,
        )
        metadata = _finish_envelope(
            builder,
            kind=PublicationKind.PublicationKind.SrsIsac,
            metadata_type=PublicationMetadata.PublicationMetadata.SrsIsacMetadata,
            metadata_off=meta,
            context_off=context,
            tensor_off=descriptor,
            sequence=7,
        )
        snapshot = classc.decode_srs_isac(metadata, tensor)
        self.assertEqual(snapshot.publication_sequence, 7)
        self.assertEqual(snapshot.rnti, 0x4601)  # ue_token low word
        self.assertEqual((snapshot.sfn, snapshot.slot, snapshot.numerology), (10, 2, 1))
        self.assertEqual(snapshot.cell_id, 1)
        self.assertEqual(snapshot.observation_time_ns, 55)
        self.assertEqual(snapshot.reference_frequency_hz, 3_600_000_000)
        self.assertEqual(snapshot.srs_resource_id, 3)
        self.assertEqual(snapshot.srs_config_id, 1)
        self.assertEqual((snapshot.rx_ports, snapshot.tx_ports), (2, 1))
        self.assertEqual(snapshot.comb_size, 4)
        self.assertEqual(snapshot.first_subcarrier, 24)
        self.assertEqual(snapshot.subcarrier_spacing_hz, 30_000)
        self.assertEqual(snapshot.frequency_points, points)
        self.assertEqual(snapshot.tone_spacing_hz, 120_000)
        self.assertAlmostEqual(snapshot.noise_variance, 0.125)
        self.assertAlmostEqual(snapshot.rsrp_db, -70.5)
        self.assertAlmostEqual(snapshot.epre_db, -72.0)
        self.assertEqual(snapshot.timing_advance_ns, -40)
        self.assertEqual(snapshot.cfo_hz, 120)
        self.assertEqual(snapshot.h.shape, (2, 1, 8))
        self.assertEqual(snapshot.h.dtype, np.complex64)
        expected = (re + 1j * im).astype(np.complex64).reshape(rx, tx, points)
        np.testing.assert_array_equal(snapshot.h, expected)

    def test_bf16_pairs_to_complex64(self):
        values = np.array([1.5, -0.5, 2.0, 3.0], dtype=np.float32)
        result = classc.bf16_pairs_to_complex64(_float32_to_bf16_bytes(values))
        np.testing.assert_array_equal(result, np.array([1.5 - 0.5j, 2.0 + 3.0j], dtype=np.complex64))



# ---------------------------------------------------------------------------
# srs.config and ta.command
# ---------------------------------------------------------------------------


# [srs-sharing] srs.config / ta.command decoding on the APER push path and the packed pull path.
class SrsConfigAndTaCommandTests(unittest.TestCase):
    codec = None

    @classmethod
    def setUpClass(cls):
        cls.codec = ocudu_e3.load_codec()

    def _indication(self, telemetry_id: int, obj) -> bytes:
        zero = bytes(8)
        return self.codec.encode(
            "Telemetry-IndicationData",
            {"telemetryId": telemetry_id, "cellId": 0, "sequence": zero, "routeEpoch": zero, "monotonicNs": zero,
             "droppedObjects": zero, "object": obj},
        )

    def test_grid_region_subscription_and_window_record(self):
        # [srs-sharing] Telemetry-GridRegion in a subscription config and Telemetry-GridWindow in a Class-C record.
        region = ocudu_e3.GridRegion(slot_period=10, slot_offset=7, first_symbol=13, nof_symbols=1, first_prb=5,
                                     nof_prbs=40)
        config = ocudu_e3.SubscriptionConfig("classc.spectrum", drop_policy="dropOldest", grid_region=region)
        encoded = self.codec.encode("Telemetry-SubscriptionData", {"configs": [config.to_asn()]})
        decoded = self.codec.decode("Telemetry-SubscriptionData", encoded)["configs"][0]
        self.assertEqual(decoded["gridRegion"], {"slotPeriod": 10, "slotOffset": 7, "firstSymbol": 13,
                                                 "nofSymbols": 1, "firstPrb": 5, "nofPrbs": 40})
        with self.assertRaises(ValueError):
            ocudu_e3.GridRegion(slot_period=10, slot_offset=10).to_asn()

        window = {"firstSymbol": 13, "nofSymbols": 1, "firstSubcarrier": 60, "nofSubcarriers": 480}
        payload = self._indication(6, ("classCPortable", {"interfaceId": 0x00010001, "metadata": b"m", "tensor": b"t",
                                                          "gridWindow": window}))
        record = ocudu_e3.telemetry.decode_indication(self.codec, payload).record
        self.assertEqual(record.grid_window, ocudu_e3.GridWindow(13, 1, 60, 480))
        plain = self._indication(6, ("classCPortable", {"interfaceId": 0x00010001, "metadata": b"m", "tensor": b"t"}))
        self.assertIsNone(ocudu_e3.telemetry.decode_indication(self.codec, plain).record.grid_window)

    def test_srs_config_indication(self):
        resource = {"resourceId": 0, "nofPorts": 2, "combSize": 4, "combOffset": 1, "cyclicShift": 0,
                    "startPosition": 0, "nofSymbols": 1, "repetitionFactor": 1, "freqDomainPosition": 0,
                    "freqDomainShift": 5, "cSrs": 11, "bSrs": 0, "bHop": 0, "groupOrSequenceHopping": 0,
                    "sequenceId": 1, "periodSlots": 20, "offsetSlots": 7, "bwpStartRb": 0, "bwpSizeRb": 51,
                    "derived": {"startSymbol": 13, "sequenceLength": 120, "mappingInitialSubcarrier": 61,
                                "sequenceGroup": 1, "sequenceNumber": 0, "cyclicShiftMaximum": 12}}
        payload = self._indication(9, ("srsConfig", {"rnti": 0x4602, "event": 2, "configEpoch": 7, "ueIndex": 1,
                                                     "numerology": 1, "nTaOffset": 25600, "resource": resource}))
        indication = ocudu_e3.telemetry.decode_indication(self.codec, payload)
        self.assertEqual(indication.stream, "srs.config")
        record = indication.record
        self.assertIsInstance(record, ocudu_e3.SrsConfigRecord)
        self.assertEqual((record.rnti, record.event_name, record.config_epoch), (0x4602, "reconfigured", 7))
        self.assertEqual((record.resource.period_slots, record.resource.offset_slots), (20, 7))
        self.assertEqual(record.resource.derived.mapping_initial_subcarrier, 61)

        removed = self._indication(9, ("srsConfig", {"rnti": 0x4602, "event": 4, "configEpoch": 8, "ueIndex": 1,
                                                     "numerology": 1, "nTaOffset": 25600}))
        self.assertIsNone(ocudu_e3.telemetry.decode_indication(self.codec, removed).record.resource)

    def test_ta_command_indication(self):
        payload = self._indication(10, ("taCommand", {"rnti": 0x4601, "radioSlot": {"sfn": 5, "slot": 3,
                                                                                    "numerology": 1},
                                                      "source": 1, "tagId": 0, "taCommand": 3846}))
        indication = ocudu_e3.telemetry.decode_indication(self.codec, payload)
        self.assertEqual(indication.stream, "ta.command")
        self.assertEqual((indication.record.source_name, indication.record.ta_command), ("rar", 3846))

    def test_store_publications(self):
        from ocudu_e3.telemetry import _SRS_CONFIG, _TA_COMMAND, decode_store_publication

        self.assertEqual(_HEADER.size + _SRS_CONFIG.size, 64)
        self.assertEqual(_HEADER.size + _TA_COMMAND.size, 26)
        body = _SRS_CONFIG.pack(0x4601, 1, 0x03, 3, 0, 1, 0, 2, 4, 0, 0, 0, 1, 1, 0, 11, 0, 0, 0,
                                5, 1, 20, 7, 25600, 0, 51, 13, 12, 120, 60, 1, 0)
        record = decode_store_publication(STORE_INTERFACES["srs.config"], _HEADER.pack(_MAGIC, 1, 7, 0) + body, b"\x01")
        self.assertEqual((record.config_epoch, record.n_ta_offset), (3, 25600))
        self.assertEqual(record.resource.derived.sequence_length, 120)
        body = _TA_COMMAND.pack(0x4601, 5, 3, 1, 2, 0, 33)
        record = decode_store_publication(STORE_INTERFACES["ta.command"], _HEADER.pack(_MAGIC, 1, 8, 0) + body, b"\x02")
        self.assertEqual((record.source_name, record.ta_command, record.radio_slot.slot), ("mac_ce", 33, 3))


if __name__ == "__main__":
    unittest.main(verbosity=2)
