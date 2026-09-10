# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear
"""Builders and normalizers for E3SM-OCUDU-DAPP-MGMT-V1 messages.

These helpers produce the ``ManagementRequest`` dictionaries accepted by
:class:`ocudu_e3.E3Codec` and flatten decoded ``ManagementResponse`` values
into JSON-friendly Python objects (identifiers as UUID-style strings, fixed
width counters as integers, bounded UTF-8 fields as ``str``).
"""

from __future__ import annotations

import secrets
from typing import Optional

from .schema import u64

IDENTIFIER_FIELDS = {
    "transactionId",
    "instanceId",
    "packageId",
    "jobId",
    "requestId",
    "reservationId",
    "sourceInstanceId",
    "subscriptionId",
}
TEXT_FIELDS = {
    "plmn",
    "globalName",
    "vendor",
    "version",
    "incompatibility",
    "lastError",
    "key",
    "name",
    "description",
    "unit",
    "detail",
    "sanitizedMessage",
    "errorText",
    "stringValue",
    "peerAddress",
    "dappName",
}
COUNTER_FIELDS = {
    "dlCenterHz",
    "ulCenterHz",
    "nrCellIdentity",
    "generation",
    "lastIncidentId",
    "appGeneration",
    "modelGeneration",
    "invocationSequence",
    "exceptionTypeHash",
    "observedLatencyNs",
    "deadlineNs",
    "timestampNs",
    "inventoryGeneration",
    "instanceGeneration",
    "resourceGeneration",
    "submittedNs",
    "startedNs",
    "completedNs",
    "clearedCount",
    "total",
    "standardExceptions",
    "unknownExceptions",
    "deadlineMisses",
    "invalidInputs",
    "invalidOutputs",
    "deviceErrors",
    "resourceExhaustions",
    "quiescenceTimeouts",
    "breakerOpened",
    "fallbacks",
    "quarantined",
    "historyEvictions",
    "sourceRingDrops",
    "reservationGeneration",
    "sourceGeneration",
    "sourceSequence",
    "expiryMonotonicNs",
    "leaseExpiryNs",
    "queuedChunks",
    "publishedObjects",
    "publishedTensorBytes",
    "droppedObjects",
    "busyDrops",
    "sampledObjects",
    "publicationSequence",
    "routeEpoch",
    "tensorSize",
    "rfCenterFrequencyHz",
    "associationId",
    "openedNs",
    "controlRequests",
}
SIGNED_FIELDS = {"signedValue", "cellFrequencyOffsetHz"}

SUCCESS_CODES = ("success", "dryRunSuccess", "accepted")


def new_transaction() -> bytes:
    """Fresh 16-byte transaction identifier."""
    return secrets.token_bytes(16)


def parse_identifier(text: str) -> bytes:
    """Accept 32 hex digits, optionally with UUID dashes."""
    compact = text.replace("-", "")
    if len(compact) != 32:
        raise ValueError("identifiers are 16 bytes (32 hex digits)")
    return bytes.fromhex(compact)


def display_identifier(value: bytes) -> str:
    compact = value.hex()
    return f"{compact[:8]}-{compact[8:12]}-{compact[12:16]}-{compact[16:20]}-{compact[20:]}"


def header(transaction: Optional[bytes] = None, instance_id: Optional[bytes] = None, **extra) -> dict:
    result = {"transactionId": transaction or new_transaction()}
    if instance_id is not None:
        result["instanceId"] = instance_id
    if "expected_instance_generation" in extra and extra["expected_instance_generation"] is not None:
        result["expectedInstanceGeneration"] = u64(extra["expected_instance_generation"])
    if "expected_inventory_generation" in extra and extra["expected_inventory_generation"] is not None:
        result["expectedInventoryGeneration"] = u64(extra["expected_inventory_generation"])
    if extra.get("dry_run"):
        result["dryRun"] = True
    return result


def inventory_request(include_packages: bool = True, include_instances: bool = True, transaction=None) -> dict:
    body = {}
    if not include_packages:
        body["includePackages"] = False
    if not include_instances:
        body["includeInstances"] = False
    return {"header": header(transaction), "operation": ("inventory", body)}


def health_request(transaction=None) -> dict:
    return {"header": header(transaction), "operation": ("health", None)}


def catalog_admit_request(package_id: bytes, *, expected_inventory_generation=None,
                          dry_run: bool = False, transaction=None) -> dict:
    """Admit a locally staged signed UUID bundle. Never load or activate it."""
    if not isinstance(package_id, bytes) or len(package_id) != 16 or not any(package_id):
        raise ValueError("package ID must be 16 nonzero identifier bytes")
    return {
        "header": header(transaction, expected_inventory_generation=expected_inventory_generation, dry_run=dry_run),
        "operation": ("catalogAdmit", {"packageId": package_id}),
    }


def metrics_request(instance_id: bytes, schema: bool = False, transaction=None) -> dict:
    return {
        "header": header(transaction, instance_id),
        "operation": ("metrics", {"kind": "schema" if schema else "snapshot"}),
    }


def incidents_request(instance_id: Optional[bytes] = None, after: Optional[int] = None, limit: int = 100, transaction=None) -> dict:
    body = {"maximumRecords": limit}
    if after is not None:
        body["afterIncidentId"] = u64(after)
    return {"header": header(transaction, instance_id), "operation": ("incidents", body)}


def lifecycle_request(action: str, instance_id: bytes, expected_generation: Optional[int] = None, timeout_us: int = 1_000_000, transaction=None) -> dict:
    return {
        "header": header(transaction, instance_id, expected_instance_generation=expected_generation),
        "operation": ("lifecycle", {"action": action, "timeoutUs": timeout_us}),
    }


def load_request(package_id: bytes, placement: str, backend: str, transaction=None) -> dict:
    return {
        "header": header(transaction),
        "operation": ("lifecycle", {"action": "load", "packageId": package_id, "placement": placement, "backend": backend}),
    }


def jobs_request(job_id: Optional[bytes] = None, cancel: bool = False, transaction=None) -> dict:
    body = {"action": "cancel" if cancel else ("get" if job_id else "list")}
    if job_id is not None:
        body["jobId"] = job_id
    return {"header": header(transaction), "operation": ("jobs", body)}


def cell_info_request(transaction=None) -> dict:
    """Served cells (PCI, PLMN/TAC, band, carriers, bandwidth, TDD, PRACH, gains) and RAN-control capabilities."""
    return {"header": header(transaction), "operation": ("cellInfo", {})}


def ran_control_request(action: str, serving_pci: int = 0, rnti: int = 0, *, target_pci: Optional[int] = None,
                        port_id: Optional[int] = None, gain_db: Optional[float] = None, dry_run: bool = False,
                        transaction=None) -> dict:
    """``ranControl``: releaseUeToIdle | handoverUe | setTxGain | setRxGain."""
    body = {"action": action, "servingPci": int(serving_pci), "rnti": int(rnti)}
    if target_pci is not None:
        body["targetPci"] = int(target_pci)
    if port_id is not None:
        body["portId"] = int(port_id)
    if gain_db is not None:
        body["gainDb"] = float(gain_db)
    return {"header": header(transaction, dry_run=dry_run), "operation": ("ranControl", body)}


def config_request(instance_id: bytes, schema: bool = False, transaction=None) -> dict:
    return {
        "header": header(transaction, instance_id),
        "operation": ("configQuery", {"kind": "schema" if schema else "effective"}),
    }


def normalize(value, field: Optional[str] = None):
    """Recursively convert an asn1tools decode result into JSON-friendly data."""
    if isinstance(value, dict):
        return {key: normalize(item, key) for key, item in value.items()}
    if isinstance(value, list):
        return [normalize(item, field) for item in value]
    if isinstance(value, tuple):
        if len(value) == 2 and isinstance(value[0], str):
            return {value[0]: normalize(value[1], value[0])}
        if len(value) == 2 and isinstance(value[0], bytes):  # BIT STRING (bytes, bit_count)
            return int.from_bytes(value[0], "big") >> (len(value[0]) * 8 - value[1])
    if isinstance(value, bytes):
        if field in IDENTIFIER_FIELDS:
            return display_identifier(value)
        if field in COUNTER_FIELDS:
            return int.from_bytes(value, "big")
        if field in SIGNED_FIELDS:
            return int.from_bytes(value, "big", signed=True)
        if field in TEXT_FIELDS:
            try:
                return value.decode("utf-8")
            except UnicodeDecodeError:
                pass
        return "hex:" + value.hex()
    return value


def unwrap_typed_value(value):
    """Flatten a ``TypedValue`` CHOICE into a Python scalar."""
    if isinstance(value, tuple) and len(value) == 2:
        kind, inner = value
        if kind == "signedValue":
            return int.from_bytes(inner, "big", signed=True)
        if kind == "unsignedValue":
            return int.from_bytes(inner, "big")
        if kind == "stringValue":
            return inner.decode("utf-8", "replace")
        if kind == "bytesValue":
            return "hex:" + inner.hex()
        return inner
    return value
