# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear
"""Typed views of OCUDU-TELEMETRY-SM indications and packed store metadata.

Complex sample vectors are delivered as little-endian binary32 ``(re, im)``
pairs. :meth:`PuschSymbolsRecord.samples` and friends return ``numpy``
arrays when ``numpy`` is installed and plain lists of Python complex numbers
otherwise.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field
from typing import Optional, Union

from .schema import E3Codec, STORE_INTERFACES, TELEMETRY_NAMES, read_u64

try:  # optional acceleration
    import numpy as _np
except ImportError:  # pragma: no cover - numpy is optional
    _np = None


def _complex_view(payload: bytes):
    if len(payload) % 8 != 0:
        raise ValueError("complex sample payload must hold whole binary32 pairs")
    if _np is not None:
        return _np.frombuffer(payload, dtype="<c8")
    count = len(payload) // 4
    floats = struct.unpack(f"<{count}f", payload)
    return [complex(floats[i], floats[i + 1]) for i in range(0, count, 2)]


@dataclass(frozen=True)
class RadioSlot:
    sfn: int
    slot: int
    numerology: int


@dataclass
class PuschGrantParameters:
    """Everything needed to regenerate the DM-RS, descramble, de-multiplex UCI and LDPC-decode a grant."""

    harq_id: int
    n_id: int
    tbs_bytes: int
    rv: int
    new_data: bool
    ldpc_base_graph: int
    tbs_lbrm_bytes: int
    mcs_index: Optional[int]
    mcs_table: Optional[int]  # 0 qam64, 1 qam256, 2 qam64LowSe (TS 38.214 5.1.3.1)
    transform_precoding: bool
    n_rs_id: Optional[int]
    cyclic_prefix: int  # 0 normal, 1 extended
    start_symbol: int
    nof_symbols: int
    dmrs_symbol_mask: int
    dmrs_type: int
    nof_cdm_groups_without_data: int
    dmrs_scrambling_id: int
    n_scid: bool
    bwp_start_rb: int
    bwp_size_rb: int
    prb_mask: bytes  # 35 octets, CRB mask, LSB-first per byte
    nof_rx_ports: int
    nof_layers: int
    dc_position: Optional[int]
    nof_harq_ack_bits: int
    nof_csi_part1_bits: int
    csi_part2_expected: bool
    alpha_scaling: float
    beta_offset_harq_ack: float
    beta_offset_csi_part1: float
    beta_offset_csi_part2: float

    def prbs(self):
        """Allocated CRB indices."""
        return [i for i in range(275) if self.prb_mask[i // 8] >> (i % 8) & 1]

    @property
    def mcs_table_name(self) -> Optional[str]:
        return {0: "qam64", 1: "qam256", 2: "qam64LowSe"}.get(self.mcs_table) if self.mcs_table is not None else None


@dataclass
class PuschSymbolsRecord:
    rnti: int
    radio_slot: RadioSlot
    nof_prb: int
    nof_data_symbols: int
    nof_layers: int
    modulation_bits: int
    sinr_db: Optional[float]
    epre_db: Optional[float]
    rsrp_db: Optional[float]
    evm: Optional[float]
    time_alignment_us: Optional[float]
    cfo_hz: Optional[float]
    target_code_rate: Optional[float]
    equalized_symbols: bytes = field(repr=False)
    #: Grant parameters (agents from 2026-08 onwards); ``None`` for legacy records.
    grant: Optional[PuschGrantParameters] = None
    #: The gNB's transport-block CRC verdict for this grant, when known.
    crc_ok: Optional[bool] = None
    #: True when the capture is not a complete codeword (clipped or DM-RS symbols carry data).
    symbols_truncated: bool = False

    def samples(self):
        """Equalized data symbols as complex values (symbol-major, RE, layer)."""
        return _complex_view(self.equalized_symbols)


@dataclass
class PuschGridRecord:
    """Raw pre-equalization resource elements for one grant, DM-RS included."""

    rnti: int
    radio_slot: RadioSlot
    nof_prb: int
    nof_rx_ports: int
    start_symbol: int
    nof_symbols: int
    grant: PuschGrantParameters
    resource_elements: bytes = field(repr=False)

    def samples(self):
        """All resource elements: receive-port-major, then OFDM symbol, then subcarrier."""
        return _complex_view(self.resource_elements)

    def grid(self):
        """numpy array shaped (nof_rx_ports, nof_symbols, nof_prb * 12)."""
        if _np is None:
            raise RuntimeError("numpy is required for PuschGridRecord.grid()")
        return _np.asarray(self.samples()).reshape(self.nof_rx_ports, self.nof_symbols, self.nof_prb * 12)


@dataclass
class PuschCrcRecord:
    rnti: int
    radio_slot: RadioSlot
    crc_ok: bool


@dataclass
class SrsChannelRecord:
    rnti: int
    radio_slot: RadioSlot
    nof_subcarriers: int
    comb_size: int
    nof_rx_ports: int
    nof_tx_ports: int
    rx_port_index: int
    tx_port_index: int
    start_symbol: int
    nof_symbols: int
    rsrp_db: Optional[float]
    epre_db: Optional[float]
    noise_variance: Optional[float]
    time_alignment_ns: Optional[float]
    mapping_initial_subcarrier: Optional[int]
    comb_offset: Optional[int]
    cyclic_shift: Optional[int]
    sequence_id: Optional[int]
    sequence_group: Optional[int]
    sequence_number: Optional[int]
    cyclic_shift_index: Optional[int]
    cyclic_shift_maximum: Optional[int]
    bandwidth_index: Optional[int]
    frequency_position: Optional[int]
    frequency_shift: Optional[int]
    frequency_hopping: Optional[int]
    coefficients: bytes = field(repr=False)

    def samples(self):
        return _complex_view(self.coefficients)


@dataclass
class PuschDmrsChannelRecord:
    rnti: int
    radio_slot: RadioSlot
    start_prb: int
    nof_prb: int
    nof_subcarriers: int
    dmrs_symbol_index: int
    start_symbol: int
    nof_symbols: int
    nof_rx_ports: int
    nof_tx_layers: int
    rx_port_index: int
    tx_layer_index: int
    dmrs_type: int
    nof_cdm_groups_without_data: int
    transform_precoding: bool
    scrambling_id: int
    n_scid: bool
    sinr_db: Optional[float]
    epre_db: Optional[float]
    rsrp_db: Optional[float]
    noise_variance: Optional[float]
    time_alignment_us: Optional[float]
    cfo_hz: Optional[float]
    target_code_rate: Optional[float]
    estimates: bytes = field(repr=False)

    def samples(self):
        return _complex_view(self.estimates)


@dataclass
class GnbMetricsJsonRecord:
    json_bytes: bytes = field(repr=False)

    def text(self) -> str:
        return self.json_bytes.decode("utf-8")

    def json(self):
        import json as _json

        return _json.loads(self.json_bytes)


@dataclass
class ClassCPortableRecord:
    interface_id: int
    metadata: bytes = field(repr=False)
    tensor: bytes = field(repr=False)


TelemetryRecord = Union[
    PuschSymbolsRecord,
    PuschCrcRecord,
    SrsChannelRecord,
    PuschDmrsChannelRecord,
    GnbMetricsJsonRecord,
    ClassCPortableRecord,
    PuschGridRecord,
]


@dataclass
class TelemetryIndication:
    telemetry_id: int
    cell_id: int
    sequence: int
    route_epoch: int
    monotonic_ns: int
    dropped_objects: int
    record: TelemetryRecord

    @property
    def stream(self) -> str:
        return TELEMETRY_NAMES.get(self.telemetry_id, f"telemetry-{self.telemetry_id}")


# ---------------------------------------------------------------------------
# ASN.1 decoding
# ---------------------------------------------------------------------------


def _slot(value: dict) -> RadioSlot:
    return RadioSlot(int(value["sfn"]), int(value["slot"]), int(value["numerology"]))


def _record_from_asn(kind: str, body: dict) -> TelemetryRecord:
    if kind == "puschSymbols":
        return PuschSymbolsRecord(
            rnti=body["rnti"],
            radio_slot=_slot(body["radioSlot"]),
            nof_prb=body["nofPrb"],
            nof_data_symbols=body["nofDataSymbols"],
            nof_layers=body["nofLayers"],
            modulation_bits=body["modulationBits"],
            sinr_db=body.get("sinrDb"),
            epre_db=body.get("epreDb"),
            rsrp_db=body.get("rsrpDb"),
            evm=body.get("evm"),
            time_alignment_us=body.get("timeAlignmentUs"),
            cfo_hz=body.get("cfoHz"),
            target_code_rate=body.get("targetCodeRate"),
            equalized_symbols=body["equalizedSymbols"],
            grant=_grant_from_asn(body["grant"]) if "grant" in body else None,
            crc_ok=bool(body["crcOk"]) if "crcOk" in body else None,
            symbols_truncated=bool(body.get("symbolsTruncated", False)),
        )
    if kind == "puschGrid":
        return PuschGridRecord(
            rnti=body["rnti"],
            radio_slot=_slot(body["radioSlot"]),
            nof_prb=body["nofPrb"],
            nof_rx_ports=body["nofRxPorts"],
            start_symbol=body["startSymbol"],
            nof_symbols=body["nofSymbols"],
            grant=_grant_from_asn(body["grant"]),
            resource_elements=body["resourceElements"],
        )
    if kind == "puschCrc":
        return PuschCrcRecord(rnti=body["rnti"], radio_slot=_slot(body["radioSlot"]), crc_ok=bool(body["crcOk"]))
    if kind == "srsChannel":
        return SrsChannelRecord(
            rnti=body["rnti"],
            radio_slot=_slot(body["radioSlot"]),
            nof_subcarriers=body["nofSubcarriers"],
            comb_size=body["combSize"],
            nof_rx_ports=body["nofRxPorts"],
            nof_tx_ports=body["nofTxPorts"],
            rx_port_index=body["rxPortIndex"],
            tx_port_index=body["txPortIndex"],
            start_symbol=body["startSymbol"],
            nof_symbols=body["nofSymbols"],
            rsrp_db=body.get("rsrpDb"),
            epre_db=body.get("epreDb"),
            noise_variance=body.get("noiseVariance"),
            time_alignment_ns=body.get("timeAlignmentNs"),
            mapping_initial_subcarrier=body.get("mappingInitialSubcarrier"),
            comb_offset=body.get("combOffset"),
            cyclic_shift=body.get("cyclicShift"),
            sequence_id=body.get("sequenceId"),
            sequence_group=body.get("sequenceGroup"),
            sequence_number=body.get("sequenceNumber"),
            cyclic_shift_index=body.get("cyclicShiftIndex"),
            cyclic_shift_maximum=body.get("cyclicShiftMaximum"),
            bandwidth_index=body.get("bandwidthIndex"),
            frequency_position=body.get("frequencyPosition"),
            frequency_shift=body.get("frequencyShift"),
            frequency_hopping=body.get("frequencyHopping"),
            coefficients=body["coefficients"],
        )
    if kind == "puschDmrsChannel":
        return PuschDmrsChannelRecord(
            rnti=body["rnti"],
            radio_slot=_slot(body["radioSlot"]),
            start_prb=body["startPrb"],
            nof_prb=body["nofPrb"],
            nof_subcarriers=body["nofSubcarriers"],
            dmrs_symbol_index=body["dmrsSymbolIndex"],
            start_symbol=body["startSymbol"],
            nof_symbols=body["nofSymbols"],
            nof_rx_ports=body["nofRxPorts"],
            nof_tx_layers=body["nofTxLayers"],
            rx_port_index=body["rxPortIndex"],
            tx_layer_index=body["txLayerIndex"],
            dmrs_type=body["dmrsType"],
            nof_cdm_groups_without_data=body["nofCdmGroupsWithoutData"],
            transform_precoding=bool(body["transformPrecoding"]),
            scrambling_id=body["scramblingId"],
            n_scid=bool(body["nScid"]),
            sinr_db=body.get("sinrDb"),
            epre_db=body.get("epreDb"),
            rsrp_db=body.get("rsrpDb"),
            noise_variance=body.get("noiseVariance"),
            time_alignment_us=body.get("timeAlignmentUs"),
            cfo_hz=body.get("cfoHz"),
            target_code_rate=body.get("targetCodeRate"),
            estimates=body["estimates"],
        )
    if kind == "gnbMetricsJson":
        return GnbMetricsJsonRecord(json_bytes=body["json"])
    if kind == "classCPortable":
        return ClassCPortableRecord(interface_id=body["interfaceId"], metadata=body.get("metadata", b""), tensor=body["tensor"])
    raise ValueError(f"unknown telemetry object {kind}")


def _grant_from_asn(body: dict) -> PuschGrantParameters:
    return PuschGrantParameters(
        harq_id=body["harqId"],
        n_id=body["nId"],
        tbs_bytes=body["tbsBytes"],
        rv=body["rvIndex"],
        new_data=bool(body["newData"]),
        ldpc_base_graph=body["ldpcBaseGraph"],
        tbs_lbrm_bytes=body["tbsLbrmBytes"],
        mcs_index=body.get("mcsIndex"),
        mcs_table=body.get("mcsTable"),
        transform_precoding=bool(body["transformPrecoding"]),
        n_rs_id=body.get("nRsId"),
        cyclic_prefix=body["cyclicPrefix"],
        start_symbol=body["startSymbol"],
        nof_symbols=body["nofSymbols"],
        dmrs_symbol_mask=body["dmrsSymbolMask"],
        dmrs_type=body["dmrsType"],
        nof_cdm_groups_without_data=body["nofCdmGroupsWithoutData"],
        dmrs_scrambling_id=body["dmrsScramblingId"],
        n_scid=bool(body["nScid"]),
        bwp_start_rb=body["bwpStartRb"],
        bwp_size_rb=body["bwpSizeRb"],
        prb_mask=bytes(body["prbMask"]),
        nof_rx_ports=body["nofRxPorts"],
        nof_layers=body["nofLayers"],
        dc_position=body.get("dcPosition"),
        nof_harq_ack_bits=body["nofHarqAckBits"],
        nof_csi_part1_bits=body["nofCsiPart1Bits"],
        csi_part2_expected=bool(body["csiPart2Expected"]),
        alpha_scaling=float(body["alphaScaling"]),
        beta_offset_harq_ack=float(body["betaOffsetHarqAck"]),
        beta_offset_csi_part1=float(body["betaOffsetCsiPart1"]),
        beta_offset_csi_part2=float(body["betaOffsetCsiPart2"]),
    )


def decode_indication(codec: E3Codec, payload: bytes) -> TelemetryIndication:
    """Decode one reassembled ``Telemetry-IndicationData`` payload."""
    decoded = codec.decode("Telemetry-IndicationData", payload)
    kind, body = decoded["object"]
    return TelemetryIndication(
        telemetry_id=int(decoded["telemetryId"]),
        cell_id=int(decoded["cellId"]),
        sequence=read_u64(decoded["sequence"]),
        route_epoch=read_u64(decoded["routeEpoch"]),
        monotonic_ns=read_u64(decoded["monotonicNs"]),
        dropped_objects=read_u64(decoded["droppedObjects"]),
        record=_record_from_asn(kind, body),
    )


# ---------------------------------------------------------------------------
# Packed store metadata (pull path: dataSubscriptions / dataPoll)
# ---------------------------------------------------------------------------

_HEADER = struct.Struct("<IHHQ")  # magic, version, kind, monotonic_ns
_MAGIC = 0x3345544F
_SLOT = struct.Struct("<HBB")
_PUSCH_SYMBOLS = struct.Struct("<H" + "HBB" + "HBBBB" + "7f")
_PUSCH_CRC = struct.Struct("<H" + "HBB" + "B3x")
_SRS = struct.Struct("<H" + "HBB" + "H" + "12B" + "7H" + "4f")
_DMRS = struct.Struct("<H" + "HBB" + "HHH" + "12B" + "H" + "7f")
_GRANT = struct.Struct("<4B" + "9H" + "2I" + "10B" + "4f" + "35s" + "x")  # 92 bytes
_SYMBOLS_V2_TAIL = struct.Struct("<B3x")  # symbol_flags + reserved, followed by _GRANT
_GRID = struct.Struct("<H" + "HBB" + "H" + "BBBx")  # rnti, slot, nof_prb, rx ports, start, nof symbols; then _GRANT


def _grant_from_packed(body: bytes, offset: int = 0) -> PuschGrantParameters:
    f = _GRANT.unpack_from(body, offset)
    flags = f[15]
    return PuschGrantParameters(
        harq_id=f[0], rv=f[1], new_data=bool(f[2]), ldpc_base_graph=f[3],
        n_id=f[4], bwp_start_rb=f[5], bwp_size_rb=f[6], dmrs_symbol_mask=f[7], dmrs_scrambling_id=f[8],
        dc_position=f[9] if flags & 0x08 else None, nof_harq_ack_bits=f[10], nof_csi_part1_bits=f[11],
        n_rs_id=f[12] if flags & 0x02 else None, tbs_bytes=f[13], tbs_lbrm_bytes=f[14],
        mcs_index=f[16] if flags & 0x01 else None, mcs_table=f[17] if flags & 0x01 else None,
        start_symbol=f[18], nof_symbols=f[19], dmrs_type=f[20], nof_cdm_groups_without_data=f[21],
        nof_rx_ports=f[22], nof_layers=f[23], cyclic_prefix=f[24],
        alpha_scaling=f[25], beta_offset_harq_ack=f[26], beta_offset_csi_part1=f[27], beta_offset_csi_part2=f[28],
        prb_mask=f[29], transform_precoding=bool(flags & 0x02), n_scid=bool(flags & 0x04),
        csi_part2_expected=bool(flags & 0x10),
    )

_INTERFACE_TO_KIND = {
    STORE_INTERFACES["pusch.symbols"]: 1,
    STORE_INTERFACES["pusch.crc"]: 2,
    STORE_INTERFACES["srs.channel"]: 3,
    STORE_INTERFACES["pusch.dmrs_channel"]: 4,
    STORE_INTERFACES["gnb.metrics_json"]: 5,
    STORE_INTERFACES["pusch.grid"]: 6,
}


def _finite(value: float) -> Optional[float]:
    return value if value == value and value not in (float("inf"), float("-inf")) else None


def decode_store_publication(interface_id: int, metadata: bytes, tensor: bytes) -> TelemetryRecord:
    """Decode a portable-store publication (metadata + reassembled tensor).

    Telemetry interfaces carry the packed ``telemetry_metadata_header`` record;
    Class-C interfaces are returned as :class:`ClassCPortableRecord`.
    """
    kind = _INTERFACE_TO_KIND.get(interface_id)
    if kind is None:
        return ClassCPortableRecord(interface_id=interface_id, metadata=metadata, tensor=tensor)
    if len(metadata) < _HEADER.size:
        raise ValueError("telemetry metadata is shorter than its header")
    magic, version, header_kind, _monotonic = _HEADER.unpack_from(metadata, 0)
    if magic != _MAGIC or version not in (1, 2) or header_kind != kind:
        raise ValueError("telemetry metadata header is invalid")
    body = metadata[_HEADER.size :]
    if kind == 1:
        fields = _PUSCH_SYMBOLS.unpack_from(body, 0)
        extra = body[_PUSCH_SYMBOLS.size:]
        grant = crc_ok = None
        truncated = False
        if len(extra) >= _SYMBOLS_V2_TAIL.size + _GRANT.size:
            (symbol_flags,) = _SYMBOLS_V2_TAIL.unpack_from(extra, 0)
            if symbol_flags & 0x08:
                grant = _grant_from_packed(extra, _SYMBOLS_V2_TAIL.size)
            if symbol_flags & 0x01:
                crc_ok = bool(symbol_flags & 0x02)
            truncated = bool(symbol_flags & 0x04)
        return PuschSymbolsRecord(
            rnti=fields[0],
            radio_slot=RadioSlot(*fields[1:4]),
            nof_prb=fields[4],
            nof_data_symbols=fields[5],
            nof_layers=fields[6],
            modulation_bits=fields[7],
            sinr_db=_finite(fields[9]),
            epre_db=_finite(fields[10]),
            rsrp_db=_finite(fields[11]),
            evm=_finite(fields[12]),
            time_alignment_us=_finite(fields[13]),
            cfo_hz=_finite(fields[14]),
            target_code_rate=_finite(fields[15]),
            equalized_symbols=tensor,
            grant=grant,
            crc_ok=crc_ok,
            symbols_truncated=truncated,
        )
    if kind == 6:
        fields = _GRID.unpack_from(body, 0)
        return PuschGridRecord(
            rnti=fields[0],
            radio_slot=RadioSlot(*fields[1:4]),
            nof_prb=fields[4],
            nof_rx_ports=fields[5],
            start_symbol=fields[6],
            nof_symbols=fields[7],
            grant=_grant_from_packed(body, _GRID.size),
            resource_elements=tensor,
        )
    if kind == 2:
        fields = _PUSCH_CRC.unpack(body)
        return PuschCrcRecord(rnti=fields[0], radio_slot=RadioSlot(*fields[1:4]), crc_ok=bool(fields[4]))
    if kind == 3:
        f = _SRS.unpack(body)
        u8 = f[5:17]
        u16 = f[17:24]
        return SrsChannelRecord(
            rnti=f[0],
            radio_slot=RadioSlot(*f[1:4]),
            nof_subcarriers=f[4],
            comb_size=u8[0],
            nof_rx_ports=u8[1],
            nof_tx_ports=u8[2],
            rx_port_index=u8[3],
            tx_port_index=u8[4],
            start_symbol=u8[5],
            nof_symbols=u8[6],
            comb_offset=u8[7],
            cyclic_shift=u8[8],
            bandwidth_index=u8[9],
            frequency_position=u8[10],
            frequency_hopping=u8[11],
            mapping_initial_subcarrier=u16[0],
            sequence_id=u16[1],
            sequence_group=u16[2],
            sequence_number=u16[3],
            cyclic_shift_index=u16[4],
            cyclic_shift_maximum=u16[5],
            frequency_shift=u16[6],
            rsrp_db=_finite(f[24]),
            epre_db=_finite(f[25]),
            noise_variance=_finite(f[26]),
            time_alignment_ns=_finite(f[27]),
            coefficients=tensor,
        )
    if kind == 4:
        f = _DMRS.unpack(body)
        u8 = f[7:19]
        return PuschDmrsChannelRecord(
            rnti=f[0],
            radio_slot=RadioSlot(*f[1:4]),
            start_prb=f[4],
            nof_prb=f[5],
            nof_subcarriers=f[6],
            dmrs_symbol_index=u8[0],
            start_symbol=u8[1],
            nof_symbols=u8[2],
            nof_rx_ports=u8[3],
            nof_tx_layers=u8[4],
            rx_port_index=u8[5],
            tx_layer_index=u8[6],
            dmrs_type=u8[7],
            nof_cdm_groups_without_data=u8[8],
            transform_precoding=bool(u8[9]),
            n_scid=bool(u8[10]),
            scrambling_id=f[19],
            sinr_db=_finite(f[20]),
            epre_db=_finite(f[21]),
            rsrp_db=_finite(f[22]),
            noise_variance=_finite(f[23]),
            time_alignment_us=_finite(f[24]),
            cfo_hz=_finite(f[25]),
            target_code_rate=_finite(f[26]),
            estimates=tensor,
        )
    return GnbMetricsJsonRecord(json_bytes=tensor)
