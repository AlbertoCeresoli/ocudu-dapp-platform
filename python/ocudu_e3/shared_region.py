# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear
"""Reader for by-reference E3AP delivery (E3-SharedRegion / E3-TensorDescriptor).

Mirrors ``include/ocudu/dapp/e3/shared_region_layout.h``: a 64-byte region
header followed by rows of ``row_bytes``; each row starts with a 64-byte row
header whose first field is a seqlock generation (odd while the agent writes,
even when complete). ``read`` copies the tensor and rejects an observed
generation change; ``view`` hands out a zero-copy memoryview for callers that
check the generation themselves. This is a trusted local-writer protocol, not
protection against a backing-file owner that truncates or corrupts a live map,
and not an asynchronous GPU lease.
"""
from __future__ import annotations

import mmap
import os
import re
import stat
import struct
from dataclasses import dataclass, field
from typing import List, Optional, Union

REGION_MAGIC = 0x314D48533345434F  # "OCE3SHM1"
REGION_HEADER = struct.Struct("<QIIIIQQQ16x")  # magic, version, header_bytes, row_header_bytes, nof_rows, row_bytes, write_index, generation
ROW_HEADER = struct.Struct("<QQQHHIQQ16x")  # generation, sequence, byte_length, telemetry_id, cell_id, reserved, realtime_ns, route_epoch
REGION_NO_ROW = 0xFFFFFFFFFFFFFFFF
ELEMENT_BYTES = {"int8": 1, "uint8": 1, "int16": 2, "uint16": 2, "int32": 4,
                 "fp16": 2, "bf16": 2, "fp32": 4, "complexInt16": 4,
                 "complexBf16": 4, "complexFp16": 4, "complexFp32": 8}


class SharedRegionError(RuntimeError):
    pass


@dataclass
class SharedRegion:
    """Decoded E3-SharedRegion."""

    memory_domain: str
    region_name: str
    region_bytes: int
    header_bytes: int
    row_bytes: int
    nof_rows: int
    layout_version: int

    @classmethod
    def from_asn(cls, body: dict) -> "SharedRegion":
        name = body["regionName"]
        return cls(
            memory_domain=body["memoryDomain"],
            region_name=name.decode("utf-8") if isinstance(name, (bytes, bytearray)) else str(name),
            region_bytes=int.from_bytes(body["regionBytes"], "big"),
            header_bytes=int(body["headerBytes"]),
            row_bytes=int.from_bytes(body["rowBytes"], "big"),
            nof_rows=int(body["nofRows"]),
            layout_version=int(body["layoutVersion"]),
        )


@dataclass
class PosixSharedLocation:
    region_name: str
    row_index: int
    byte_offset: int


@dataclass
class AcceleratorIpcLocation:
    device_ordinal: int
    allocation_id: int
    memory_handle: bytes
    ready_event_handle: Optional[bytes]
    byte_offset: int


@dataclass
class TensorDescriptor:
    """Decoded E3-TensorDescriptor."""

    memory_domain: str
    location: Union[PosixSharedLocation, AcceleratorIpcLocation]
    byte_length: int
    generation: int
    element_type: Optional[str] = None
    dimensions: List[int] = field(default_factory=list)
    byte_strides: List[int] = field(default_factory=list)
    layout_id: Optional[int] = None
    lease_expiry_ns: Optional[int] = None

    @classmethod
    def from_asn(cls, body: dict) -> "TensorDescriptor":
        kind, where = body["location"]
        if kind == "posixShared":
            name = where["regionName"]
            location: Union[PosixSharedLocation, AcceleratorIpcLocation] = PosixSharedLocation(
                region_name=name.decode("utf-8") if isinstance(name, (bytes, bytearray)) else str(name),
                row_index=int(where["rowIndex"]),
                byte_offset=int.from_bytes(where["byteOffset"], "big"),
            )
        elif kind == "acceleratorIpc":
            location = AcceleratorIpcLocation(
                device_ordinal=int(where["deviceOrdinal"]),
                allocation_id=int.from_bytes(where["allocationId"], "big"),
                memory_handle=bytes(where["memoryHandle"]),
                ready_event_handle=bytes(where["readyEventHandle"]) if "readyEventHandle" in where else None,
                byte_offset=int.from_bytes(where["byteOffset"], "big"),
            )
        else:
            raise SharedRegionError(f"unknown tensor location {kind}")
        return cls(
            memory_domain=body["memoryDomain"],
            location=location,
            byte_length=int.from_bytes(body["byteLength"], "big"),
            generation=int.from_bytes(body["generation"], "big"),
            element_type=body.get("elementType"),
            dimensions=[int(item) for item in body.get("dimensions", [])],
            byte_strides=[int.from_bytes(item, "big") for item in body.get("byteStrides", [])],
            layout_id=int(body["layoutId"]) if "layoutId" in body else None,
            lease_expiry_ns=int.from_bytes(body["leaseExpiryNs"], "big") if "leaseExpiryNs" in body else None,
        )


class SharedRegionReader:
    """Read-only mapping of one agent-owned POSIX shared-memory region."""

    def __init__(self, region: SharedRegion, shm_dir: str = "/dev/shm", *, maximum_bytes: int = 256 * 1024 * 1024):
        self.region = region
        self._map = None
        self._fd = -1
        if (not re.fullmatch(r"/[A-Za-z0-9_.-]{1,62}", region.region_name)
                or region.region_name in ("/.", "/..")):
            raise SharedRegionError("region name must be a single canonical POSIX shm component")
        if (region.memory_domain != "posixShared" or region.layout_version != 1
                or region.header_bytes != REGION_HEADER.size or not 1 <= region.nof_rows <= 65535
                or region.row_bytes < ROW_HEADER.size + 64 or region.row_bytes % 64
                or region.region_bytes != region.header_bytes + region.nof_rows * region.row_bytes
                or region.region_bytes > maximum_bytes):
            raise SharedRegionError("invalid or oversized shared region layout")
        path = os.path.join(shm_dir, region.region_name[1:])
        try:
            self._fd = os.open(path, os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW)
            info = os.fstat(self._fd)
            if not stat.S_ISREG(info.st_mode) or info.st_size != region.region_bytes:
                raise SharedRegionError("shared region backing file size/type mismatch")
            self._map = mmap.mmap(self._fd, region.region_bytes, mmap.MAP_SHARED, mmap.PROT_READ)
            magic, version, header_bytes, row_header_bytes, nof_rows, row_bytes, _write, _gen = REGION_HEADER.unpack_from(
                self._map, 0
            )
            if (magic != REGION_MAGIC or version != region.layout_version
                    or header_bytes != region.header_bytes or row_header_bytes != ROW_HEADER.size
                    or row_bytes != region.row_bytes or nof_rows != region.nof_rows):
                raise SharedRegionError(f"{path} layout does not match the advertised E3-SharedRegion")
            self.row_header_bytes = row_header_bytes
        except Exception:
            self.close()
            raise

    def close(self) -> None:
        if getattr(self, "_map", None) is not None:
            self._map.close()
            self._map = None
        if getattr(self, "_fd", -1) >= 0:
            os.close(self._fd)
            self._fd = -1

    def __enter__(self) -> "SharedRegionReader":
        return self

    def __exit__(self, *_exc) -> None:
        self.close()

    @property
    def header(self) -> dict:
        magic, version, header_bytes, row_header_bytes, nof_rows, row_bytes, write_index, generation = (
            REGION_HEADER.unpack_from(self._map, 0)
        )
        return {
            "version": version,
            "nof_rows": nof_rows,
            "row_bytes": row_bytes,
            "write_index": None if write_index == REGION_NO_ROW else write_index,
            "generation": generation,
        }

    def row_header(self, row_index: int) -> dict:
        offset = self._row_offset(row_index)
        generation, sequence, byte_length, telemetry_id, cell_id, _r, realtime_ns, route_epoch = ROW_HEADER.unpack_from(
            self._map, offset
        )
        return {
            "generation": generation,
            "sequence": sequence,
            "byte_length": byte_length,
            "telemetry_id": telemetry_id,
            "cell_id": cell_id,
            "realtime_ns": realtime_ns,
            "route_epoch": route_epoch,
        }

    def _row_generation(self, row_index: int) -> int:
        offset = self._row_offset(row_index)
        return struct.unpack_from("<Q", self._map, offset)[0]

    def _row_offset(self, row_index: int) -> int:
        if self._map is None or not 0 <= row_index < self.region.nof_rows:
            raise SharedRegionError("closed region or invalid row index")
        return self.region.header_bytes + self.region.row_bytes * row_index

    def _validate_descriptor(self, descriptor: TensorDescriptor) -> PosixSharedLocation:
        location = descriptor.location
        if descriptor.memory_domain != "posixShared" or not isinstance(location, PosixSharedLocation):
            raise SharedRegionError("descriptor does not point into a POSIX shared region")
        if location.region_name != self.region.region_name:
            raise SharedRegionError("descriptor names another region")
        start = self._row_offset(location.row_index)
        end = location.byte_offset + descriptor.byte_length
        if (location.byte_offset != start + self.row_header_bytes or descriptor.byte_length <= 0
                or end > start + self.region.row_bytes):
            raise SharedRegionError("descriptor must point into the nominated row's payload")
        if not 0 < descriptor.generation < 2**64 or descriptor.generation % 2:
            raise SharedRegionError("descriptor must name a committed even generation")
        if (len(descriptor.dimensions) > 8 or any(d <= 0 for d in descriptor.dimensions)
                or (descriptor.byte_strides and len(descriptor.byte_strides) != len(descriptor.dimensions))
                or any(s <= 0 for s in descriptor.byte_strides)):
            raise SharedRegionError("invalid tensor shape or strides")
        if descriptor.dimensions:
            item_bytes = ELEMENT_BYTES.get(descriptor.element_type)
            if item_bytes is None:
                raise SharedRegionError("shaped tensor needs a supported element type")
            if descriptor.byte_strides:
                extent = item_bytes + sum((d - 1) * s for d, s in
                                          zip(descriptor.dimensions, descriptor.byte_strides))
            else:
                extent = item_bytes
                for dimension in descriptor.dimensions:
                    extent *= dimension
            if extent > descriptor.byte_length:
                raise SharedRegionError("tensor shape or strides exceed the descriptor payload")
        return location

    def view(self, descriptor: TensorDescriptor) -> memoryview:
        """Borrow bytes only until overwrite; this is NOT a lease for asynchronous work.

        Release the view before closing the region. Prefer read() for a checked
        copy; view callers must recheck generation after consuming the bytes.
        """
        location = self._validate_descriptor(descriptor)
        if self._row_generation(location.row_index) != descriptor.generation:
            raise SharedRegionError("row was overwritten")
        if self.row_header(location.row_index)["byte_length"] != descriptor.byte_length:
            raise SharedRegionError("row length does not match the descriptor")
        end = location.byte_offset + descriptor.byte_length
        return memoryview(self._map)[location.byte_offset : end]

    def read(self, descriptor: TensorDescriptor) -> Optional[bytes]:
        """Copy the tensor; returns None when the row was overwritten meanwhile (torn read)."""
        location = self._validate_descriptor(descriptor)
        if self._row_generation(location.row_index) != descriptor.generation:
            return None
        # A changing row may expose a new length: discard, do not misinterpret it.
        if self.row_header(location.row_index)["byte_length"] != descriptor.byte_length:
            return None
        data = bytes(self._map[location.byte_offset : location.byte_offset + descriptor.byte_length])
        if self._row_generation(location.row_index) != descriptor.generation:
            return None
        return data
