# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear
"""Python client for the OCUDU E3 interface.

The package speaks E3AP-OCUDU-V2 (libe3 E3AP 1.0.0 with OCUDU extension additions, ASN.1 APER
over SCTP) to the embedded OCUDU
E3 agent: setup, telemetry subscriptions with fragmented indications, and the
dApp management service model carried as control actions. It also decodes the
packed telemetry metadata used by the pull-based ``dataPoll`` path so the same
records are available to clients of the local management socket.

Only the Python standard library and ``asn1tools`` are required; ``numpy`` is
optional and used to expose complex sample vectors as arrays.
"""

from .e3ap import (
    E3Client,
    E3Error,
    E3ProtocolError,
    E3Subscription,
    E3TransportError,
    RanFunction,
    SetupResult,
    SubscriptionConfig,
    TelemetryStream,
)
from .schema import (
    RAN_FUNCTION_MANAGEMENT,
    RAN_FUNCTION_TELEMETRY,
    TELEMETRY_IDS,
    TELEMETRY_NAMES,
    E3Codec,
    load_codec,
    locate_schemas,
)
try:  # Class-C decoding needs the optional ``flatbuffers`` runtime.
    from .classc import SpectrumSnapshot, SrsIsacSnapshot, decode_spectrum, decode_srs_isac
except ImportError:  # pragma: no cover - flatbuffers not installed
    SpectrumSnapshot = SrsIsacSnapshot = None  # type: ignore[assignment]

    def decode_spectrum(*_args, **_kwargs):  # type: ignore[no-redef]
        raise RuntimeError("ocudu_e3.classc requires the 'flatbuffers' package")

    decode_srs_isac = decode_spectrum  # type: ignore[assignment]
from .telemetry import (
    ClassCPortableRecord,
    GnbMetricsJsonRecord,
    PuschCrcRecord,
    PuschDmrsChannelRecord,
    PuschGrantParameters,
    PuschGridRecord,
    PuschSymbolsRecord,
    RadioSlot,
    SrsChannelRecord,
    SrsConfigRecord,
    SrsDerivedMapping,
    SrsResourceParameters,
    TaCommandRecord,
    TelemetryIndication,
    decode_indication,
    decode_store_publication,
)

__all__ = [
    "SpectrumSnapshot",
    "decode_spectrum",
    "SrsIsacSnapshot",
    "decode_srs_isac",
    "E3Client",
    "E3Error",
    "E3ProtocolError",
    "E3Subscription",
    "E3TransportError",
    "RanFunction",
    "SetupResult",
    "SubscriptionConfig",
    "TelemetryStream",
    "RAN_FUNCTION_MANAGEMENT",
    "RAN_FUNCTION_TELEMETRY",
    "TELEMETRY_IDS",
    "TELEMETRY_NAMES",
    "E3Codec",
    "load_codec",
    "locate_schemas",
    "ClassCPortableRecord",
    "GnbMetricsJsonRecord",
    "PuschCrcRecord",
    "PuschDmrsChannelRecord",
    "PuschSymbolsRecord",
    "PuschGrantParameters",
    "PuschGridRecord",
    "RadioSlot",
    "SrsChannelRecord",
    "SrsConfigRecord",
    "SrsDerivedMapping",
    "SrsResourceParameters",
    "TaCommandRecord",
    "TelemetryIndication",
    "decode_indication",
    "decode_store_publication",
]

__version__ = "0.1.0"
