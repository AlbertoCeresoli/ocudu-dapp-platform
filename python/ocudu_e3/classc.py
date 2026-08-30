# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear
"""Class-C publication ("OCC3") decoding for ``classc.spectrum`` / ``classc.srs_isac``.

A Class-C telemetry record carries the producer's FlatBuffers
``PublicationEnvelope`` (file identifier ``OCC3``, schema
``schemas/dapp/class-c-publication-v1.fbs``) as metadata and the complete
tensor as a raw byte blob. Nothing about the tensor is re-encoded on the wire:
E3AP places it in an APER ``OCTET STRING`` (a length prefix followed by the
bytes), so decoding is one slice plus a ``numpy.frombuffer``.

The bindings under ``_classc_fbs`` are generated with
``flatc --python schemas/dapp/class-c-publication-v1.fbs``; regenerate them
when the schema changes.
"""

from __future__ import annotations

import os
import sys
from dataclasses import dataclass
from typing import Optional

import numpy as np

_BINDINGS = os.path.join(os.path.dirname(__file__), "_classc_fbs")
if _BINDINGS not in sys.path:
    sys.path.insert(0, _BINDINGS)

from Ocudu.Dapp.ClassC.V1.ElementType import ElementType  # noqa: E402
from Ocudu.Dapp.ClassC.V1.PublicationEnvelope import PublicationEnvelope  # noqa: E402
from Ocudu.Dapp.ClassC.V1.PublicationKind import PublicationKind  # noqa: E402
from Ocudu.Dapp.ClassC.V1.PublicationMetadata import PublicationMetadata  # noqa: E402
from Ocudu.Dapp.ClassC.V1.SpectrumMetadata import SpectrumMetadata  # noqa: E402
from Ocudu.Dapp.ClassC.V1.SrsIsacMetadata import SrsIsacMetadata  # noqa: E402

UL_USE_NAMES = {
    0: "unknown", 1: "planned_empty", 2: "pusch", 3: "pucch", 4: "srs",
    5: "prach_or_common", 6: "reference_signal", 7: "reserved_quiet",
}
GRANULARITY_NAMES = {0: "none", 1: "prb", 2: "symbol_prb", 3: "resource_element"}

_ELEMENT_BYTES = {
    ElementType.Int8: 1, ElementType.Uint8: 1, ElementType.Int16: 2, ElementType.Uint16: 2,
    ElementType.Int32: 4, ElementType.Fp16: 2, ElementType.Bf16: 2, ElementType.Fp32: 4,
    ElementType.ComplexInt16: 4, ElementType.ComplexBf16: 4, ElementType.ComplexFp16: 4,
    ElementType.ComplexFp32: 8,
}


def bf16_pairs_to_complex64(raw: bytes) -> np.ndarray:
    """Complex bfloat16 (re, im) pairs -> complex64. bf16 is the top half of an IEEE binary32."""
    halves = np.frombuffer(raw, dtype="<u2").astype(np.uint32) << 16
    floats = halves.view(np.float32)
    return floats[0::2] + 1j * floats[1::2]


@dataclass
class SpectrumSnapshot:
    """One decoded ``classc.spectrum`` publication."""

    publication_sequence: int
    sfn: int
    slot: int
    numerology: int
    cell_id: int
    observation_time_ns: int
    reference_frequency_hz: int
    bandwidth_hz: int
    rx_ports: int
    symbols: int
    subcarriers: int
    scale: float
    plan_generation: int
    occupancy_granularity: str
    occupancy_complete: bool
    #: Expected uplink use per (symbol, PRB) as ``UlUseCategory`` codes; ``None`` when the
    #: producer had no plan. See :data:`UL_USE_NAMES`.
    occupancy: Optional[np.ndarray]
    #: Resource grid shaped (rx_ports, symbols, subcarriers), complex64, already divided by ``scale``.
    grid: np.ndarray

    @property
    def nof_prb(self) -> int:
        return self.subcarriers // 12

    @property
    def subcarrier_spacing_hz(self) -> int:
        return 15_000 << self.numerology

    def prb_center_frequencies_hz(self) -> np.ndarray:
        """Absolute centre frequency of each PRB (reference frequency is the carrier centre)."""
        prb_bw = 12 * self.subcarrier_spacing_hz
        first = self.reference_frequency_hz - self.bandwidth_hz / 2 + prb_bw / 2
        return first + prb_bw * np.arange(self.nof_prb)

    def prb_power(self) -> np.ndarray:
        """Mean |x|^2 per (symbol, PRB), summed over receive ports; shape (symbols, nof_prb)."""
        power = (np.abs(self.grid) ** 2).sum(axis=0)  # (symbols, subcarriers)
        return power[:, : self.nof_prb * 12].reshape(self.symbols, self.nof_prb, 12).mean(axis=2)


def decode_spectrum(metadata: bytes, tensor: bytes) -> SpectrumSnapshot:
    """Decode a ``classc.spectrum`` record (``ClassCPortableRecord.metadata`` / ``.tensor``)."""
    if not PublicationEnvelope.PublicationEnvelopeBufferHasIdentifier(metadata, 0):
        raise ValueError("Class-C metadata does not carry the OCC3 identifier")
    envelope = PublicationEnvelope.GetRootAs(metadata, 0)
    if envelope.PublicationKind() != PublicationKind.Spectrum:
        raise ValueError(f"publication kind {envelope.PublicationKind()} is not Spectrum")
    if envelope.MetadataType() != PublicationMetadata.SpectrumMetadata:
        raise ValueError("publication metadata is not SpectrumMetadata")
    meta = SpectrumMetadata()
    meta.Init(envelope.Metadata().Bytes, envelope.Metadata().Pos)
    context = envelope.Context()
    radio = context.RadioTime()
    descriptor = envelope.Tensor()
    if descriptor is None:
        raise ValueError("spectrum publication has no tensor descriptor")

    rx_ports, symbols, subcarriers = int(meta.RxPorts()), int(meta.Symbols()), int(meta.Subcarriers())
    element = descriptor.ElementType()
    offset = int(descriptor.ByteOffset())
    length = int(descriptor.ByteLength()) or len(tensor) - offset
    payload = tensor[offset : offset + length]
    expected = rx_ports * symbols * subcarriers * _ELEMENT_BYTES.get(element, 4)
    if len(payload) != expected:
        raise ValueError(f"tensor holds {len(payload)} bytes, expected {expected} for {rx_ports}x{symbols}x{subcarriers}")
    if element == ElementType.ComplexBf16:
        grid = bf16_pairs_to_complex64(payload)
    elif element == ElementType.ComplexFp32:
        grid = np.frombuffer(payload, dtype="<c8")
    elif element == ElementType.ComplexInt16:
        pairs = np.frombuffer(payload, dtype="<i2").astype(np.float32)
        grid = pairs[0::2] + 1j * pairs[1::2]
    else:
        raise ValueError(f"unsupported spectrum element type {element}")
    grid = grid.reshape(rx_ports, symbols, subcarriers)
    scale = float(meta.Scale()) or 1.0
    if scale != 1.0:
        grid = grid / scale

    occupancy = None
    if not meta.OccupancyAnnotationsIsNone() and meta.OccupancyAnnotationsLength():
        annotations = meta.OccupancyAnnotationsAsNumpy()
        nof_prb = subcarriers // 12
        if meta.OccupancyGranularity() == 2 and annotations.size == symbols * nof_prb:
            occupancy = annotations.reshape(symbols, nof_prb)
        elif meta.OccupancyGranularity() == 1 and annotations.size == nof_prb:
            occupancy = np.broadcast_to(annotations, (symbols, nof_prb))
        else:
            occupancy = annotations

    return SpectrumSnapshot(
        publication_sequence=int(envelope.PublicationSequence()),
        sfn=int(radio.Sfn()),
        slot=int(radio.Slot()),
        numerology=int(radio.Numerology()),
        cell_id=int(context.CellId()),
        observation_time_ns=int(context.ObservationTimeNs()),
        reference_frequency_hz=int(context.ReferenceFrequencyHz()),
        bandwidth_hz=int(meta.BandwidthHz()),
        rx_ports=rx_ports,
        symbols=symbols,
        subcarriers=subcarriers,
        scale=scale,
        plan_generation=int(meta.PlanGeneration()),
        occupancy_granularity=GRANULARITY_NAMES.get(int(meta.OccupancyGranularity()), "unknown"),
        occupancy_complete=bool(meta.OccupancyComplete()),
        occupancy=occupancy,
        grid=grid,
    )


@dataclass
class SrsIsacSnapshot:
    """One decoded ``classc.srs_isac`` publication: the SRS channel estimate per receive/transmit port."""

    publication_sequence: int
    rnti: int  #: sounding UE (ObservationContext.ue_token low word); 0 when the producer did not tag it
    sfn: int
    slot: int
    numerology: int
    cell_id: int
    observation_time_ns: int
    reference_frequency_hz: int
    srs_resource_id: int
    srs_config_id: int
    rx_ports: int
    tx_ports: int
    comb_size: int
    first_subcarrier: int
    subcarrier_spacing_hz: int
    frequency_points: int
    noise_variance: float
    rsrp_db: float
    epre_db: float
    timing_advance_ns: int
    cfo_hz: int
    #: Channel estimate shaped (rx_ports, tx_ports, frequency_points), complex64, one sample per comb subcarrier.
    h: np.ndarray

    @property
    def tone_spacing_hz(self) -> int:
        return self.comb_size * self.subcarrier_spacing_hz


def decode_srs_isac(metadata: bytes, tensor: bytes) -> SrsIsacSnapshot:
    """Decode a ``classc.srs_isac`` record (``ClassCPortableRecord.metadata`` / ``.tensor``)."""
    if not PublicationEnvelope.PublicationEnvelopeBufferHasIdentifier(metadata, 0):
        raise ValueError("Class-C metadata does not carry the OCC3 identifier")
    envelope = PublicationEnvelope.GetRootAs(metadata, 0)
    if envelope.PublicationKind() != PublicationKind.SrsIsac:
        raise ValueError(f"publication kind {envelope.PublicationKind()} is not SrsIsac")
    if envelope.MetadataType() != PublicationMetadata.SrsIsacMetadata:
        raise ValueError("publication metadata is not SrsIsacMetadata")
    meta = SrsIsacMetadata()
    meta.Init(envelope.Metadata().Bytes, envelope.Metadata().Pos)
    context = envelope.Context()
    radio = context.RadioTime()
    descriptor = envelope.Tensor()
    if descriptor is None:
        raise ValueError("SRS-ISAC publication has no tensor descriptor")
    rx, tx, points = int(meta.RxPorts()), int(meta.TxPorts()), int(meta.FrequencyPoints())
    offset = int(descriptor.ByteOffset())
    length = int(descriptor.ByteLength()) or len(tensor) - offset
    payload = tensor[offset : offset + length]
    element = descriptor.ElementType()
    if element == ElementType.ComplexFp32:
        h = np.frombuffer(payload, dtype="<c8")
    elif element == ElementType.ComplexBf16:
        h = bf16_pairs_to_complex64(payload)
    else:
        raise ValueError(f"unsupported SRS-ISAC element type {element}")
    if h.size != rx * tx * points:
        raise ValueError(f"tensor holds {h.size} samples, expected {rx}x{tx}x{points}")
    token = context.UeToken()
    return SrsIsacSnapshot(
        publication_sequence=int(envelope.PublicationSequence()),
        rnti=(int(token.Low()) & 0xFFFF) if token is not None else 0,
        sfn=int(radio.Sfn()),
        slot=int(radio.Slot()),
        numerology=int(radio.Numerology()),
        cell_id=int(context.CellId()),
        observation_time_ns=int(context.ObservationTimeNs()),
        reference_frequency_hz=int(context.ReferenceFrequencyHz()),
        srs_resource_id=int(meta.SrsResourceId()),
        srs_config_id=int(meta.SrsConfigId()),
        rx_ports=rx,
        tx_ports=tx,
        comb_size=int(meta.CombSize()),
        first_subcarrier=int(meta.FirstSubcarrier()),
        subcarrier_spacing_hz=int(meta.SubcarrierSpacingHz()),
        frequency_points=points,
        noise_variance=float(meta.NoiseVariance()),
        rsrp_db=float(meta.Rsrp()),
        epre_db=float(meta.Epre()),
        timing_advance_ns=int(meta.TimingAdvanceNs()),
        cfo_hz=int(meta.CfoHz()),
        h=h.reshape(rx, tx, points),
    )
