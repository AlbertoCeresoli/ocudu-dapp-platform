# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear
"""Schema discovery and APER codec construction."""

from __future__ import annotations

import os
import sys
from pathlib import Path
from typing import Iterable, Sequence

MANAGEMENT_SCHEMA = "e3sm-ocudu-dapp-mgmt-v1.asn1"
E3AP_SCHEMA = "e3ap-ocudu-v1.asn1"
TELEMETRY_SCHEMA = "e3sm-ocudu-telemetry-v1.asn1"
MANAGEMENT_CARRIER_SCHEMA = "e3sm-ocudu-dapp-mgmt-carrier-v1.asn1"
ALL_SCHEMAS = (E3AP_SCHEMA, TELEMETRY_SCHEMA, MANAGEMENT_CARRIER_SCHEMA, MANAGEMENT_SCHEMA)

RAN_FUNCTION_TELEMETRY = 10
RAN_FUNCTION_MANAGEMENT = 11
MANAGEMENT_CONTROL_REQUEST = 1

TELEMETRY_IDS = {
    "pusch.symbols": 1,
    "pusch.crc": 2,
    "srs.channel": 3,
    "pusch.dmrs_channel": 4,
    "gnb.metrics_json": 5,
    "classc.spectrum": 6,
    "classc.srs_isac": 7,
    "pusch.grid": 8,
}
TELEMETRY_NAMES = {value: key for key, value in TELEMETRY_IDS.items()}

# Store interface identifiers used by the pull path (dataSubscriptions/dataPoll).
STORE_INTERFACES = {
    "pusch.symbols": 0x00020001,
    "pusch.crc": 0x00020002,
    "srs.channel": 0x00020003,
    "pusch.dmrs_channel": 0x00020004,
    "gnb.metrics_json": 0x00020005,
    "classc.spectrum": 0x00010001,
    "classc.srs_isac": 0x00010002,
    "pusch.grid": 0x00020006,
}

E3AP_DEFAULT_PORT = 36423
E3AP_MAX_FRAGMENT_BYTES = 65536
E3AP_MAX_PAYLOAD_BYTES = 16 * 1024 * 1024


def _candidate_directories(explicit: str | os.PathLike[str] | None) -> Iterable[Path]:
    if explicit:
        yield Path(explicit)
    env = os.environ.get("OCUDU_E3_SCHEMA_DIR")
    if env:
        yield Path(env)
    legacy = os.environ.get("OCUDU_DAPP_E3_SCHEMA")
    if legacy:
        yield Path(legacy).parent
    here = Path(__file__).resolve()
    # Source tree: <repo>/python/ocudu_e3/schema.py -> <repo>/schemas/e3
    yield here.parents[2] / "schemas" / "e3"
    # Installed tree: <prefix>/share/ocudu/dapp/python/ocudu_e3 -> <prefix>/share/ocudu/dapp/schema
    yield here.parents[2] / "schema"
    yield Path(sys.prefix) / "share" / "ocudu" / "dapp" / "schema"
    yield Path("/usr/local/share/ocudu/dapp/schema")
    yield Path("/usr/share/ocudu/dapp/schema")
    # pip wheel: synced copies shipped inside the package (see schema/README.md).
    # Deliberately last so a source checkout's schemas/e3 wins over the copies.
    yield here.parent / "schema"


def locate_schemas(explicit_dir: str | os.PathLike[str] | None = None) -> list[Path]:
    """Return the four E3 ASN.1 module paths, searching the usual locations.

    The search order is: the explicit directory, ``OCUDU_E3_SCHEMA_DIR``, the
    directory of ``OCUDU_DAPP_E3_SCHEMA``, the source tree relative to this
    package, and the installed ``share/ocudu/dapp/schema`` prefixes.
    """
    for directory in _candidate_directories(explicit_dir):
        paths = [directory / name for name in ALL_SCHEMAS]
        if all(path.is_file() for path in paths):
            return paths
    raise FileNotFoundError(
        "cannot locate the OCUDU E3 ASN.1 modules; set OCUDU_E3_SCHEMA_DIR to the schemas/e3 directory"
    )


class E3Codec:
    """Thin wrapper over an ``asn1tools`` APER compilation of the E3 modules."""

    def __init__(self, paths: Sequence[Path]):
        try:
            import asn1tools
        except ImportError as error:  # pragma: no cover - environment dependent
            raise RuntimeError("the OCUDU E3 client requires the asn1tools package") from error
        self.paths = [Path(path) for path in paths]
        self._codec = asn1tools.compile_files([str(path) for path in self.paths], "per")

    def encode(self, type_name: str, value) -> bytes:
        return self._codec.encode(type_name, value)

    def decode(self, type_name: str, data: bytes):
        return self._codec.decode(type_name, data)

    # E3AP envelope helpers -------------------------------------------------

    def encode_pdu(self, pdu_id: int, message_name: str, body: dict, timestamp_ns: int | None = None) -> bytes:
        """Encode one E3-PDU; ``timestamp_ns`` fills the optional libe3-style envelope timestamp."""
        pdu = {"id": pdu_id, "msg": (message_name, body)}
        if timestamp_ns is not None:
            pdu["timestamp"] = int(timestamp_ns)
        return self.encode("E3-PDU", pdu)

    def decode_pdu(self, data: bytes) -> tuple[int, str, dict]:
        pdu_id, _timestamp, name, body = self.decode_pdu_with_timestamp(data)
        return pdu_id, name, body

    def decode_pdu_with_timestamp(self, data: bytes) -> tuple[int, int | None, str, dict]:
        """Decode one E3-PDU returning (id, timestamp_ns or None, message name, body)."""
        decoded = self.decode("E3-PDU", data)
        name, body = decoded["msg"]
        timestamp = decoded.get("timestamp")
        return int(decoded["id"]), (int(timestamp) if timestamp is not None else None), name, body


def load_codec(schema_dir: str | os.PathLike[str] | None = None) -> E3Codec:
    """Locate and compile the E3 schemas."""
    return E3Codec(locate_schemas(schema_dir))


def u64(value: int) -> bytes:
    """Fixed-width network-order unsigned integer used by the schemas."""
    return int(value).to_bytes(8, "big", signed=False)


def read_u64(value: bytes) -> int:
    return int.from_bytes(value, "big", signed=False)


def read_i64(value: bytes) -> int:
    return int.from_bytes(value, "big", signed=True)
