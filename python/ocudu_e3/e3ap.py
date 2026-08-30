# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear
"""E3AP-OCUDU-V1 client: setup, subscriptions, indications, and control.

Typical use::

    from ocudu_e3 import E3Client, SubscriptionConfig

    with E3Client("127.0.0.1", 36423, dapp_name="my-dapp") as client:
        setup = client.setup()
        sub = client.subscribe(["pusch.symbols", "pusch.crc"],
                               configs=[SubscriptionConfig("pusch.symbols", rnti_filter=[0x4601])])
        for indication in client.indications(timeout=1.0):
            print(indication.stream, indication.record)

Every E3 PDU is one SCTP message. Indications and control outcomes larger than
one fragment are reassembled here; callers only ever see complete objects.
"""

from __future__ import annotations

import threading
from collections import deque
from dataclasses import dataclass, field
from typing import Callable, Dict, Iterable, Iterator, List, Optional, Sequence, Tuple, Union

from . import management as mgmt
from .schema import (
    E3AP_DEFAULT_PORT,
    E3AP_MAX_FRAGMENT_BYTES,
    E3AP_MAX_PAYLOAD_BYTES,
    MANAGEMENT_CONTROL_REQUEST,
    RAN_FUNCTION_MANAGEMENT,
    RAN_FUNCTION_TELEMETRY,
    TELEMETRY_IDS,
    E3Codec,
    load_codec,
    read_u64,
    u64,
)
from .sctp import SctpAssociation, SctpTransportError
from .telemetry import TelemetryIndication, decode_indication


class E3Error(Exception):
    """Base class for E3 client failures."""


class E3TransportError(E3Error):
    """The SCTP association failed or was closed."""


class E3ProtocolError(E3Error):
    """The agent refused a procedure or violated the protocol."""

    def __init__(self, message: str, cause: Optional[str] = None, detail: Optional[str] = None):
        super().__init__(message if not detail else f"{message}: {detail}")
        self.cause = cause
        self.detail = detail


@dataclass
class TelemetryStream:
    telemetry_id: int
    name: str
    description: str
    class_c_portable: bool
    interface_id: int
    minimum_interval_us: Optional[int]
    cell_ids: List[int]
    #: Floor no subscriber may request below; ``None`` means full rate is available.
    hard_floor_interval_us: Optional[int] = None


@dataclass
class RanFunction:
    ran_function_id: int
    name: str
    version: str
    telemetry_ids: List[int]
    control_ids: List[int]
    streams: List[TelemetryStream] = field(default_factory=list)
    schema_module: Optional[str] = None
    maximum_object_bytes: Optional[int] = None
    maximum_subscriptions: Optional[int] = None


@dataclass
class SetupResult:
    dapp_id: int
    ran_identifier: str
    protocol_version: Optional[str]
    ran_functions: Dict[int, RanFunction]
    maximum_payload_bytes: int
    maximum_fragment_bytes: int

    @property
    def telemetry(self) -> Optional[RanFunction]:
        return self.ran_functions.get(RAN_FUNCTION_TELEMETRY)

    @property
    def management(self) -> Optional[RanFunction]:
        return self.ran_functions.get(RAN_FUNCTION_MANAGEMENT)


@dataclass
class SubscriptionConfig:
    """Per-telemetry filter, sampling, rate, and drop policy (``Telemetry-SubscriptionConfig``).

    ``minimum_interval_ms`` is the capture interval requested from the producer:
    ``None`` keeps the stream's advertised default floor, a value lowers or
    raises this subscription's rate, and ``0`` asks for every event (full
    rate). The producer honours the fastest active request down to the
    advertised hard floor; other subscribers on the same stream keep their own
    rate because the carrier thins per subscription.
    """

    telemetry: Union[int, str]
    cell_id: Optional[int] = None
    sample_every_n: int = 1
    drop_policy: str = "coalesceLatest"  # dropNewest | dropOldest | coalesceLatest
    minimum_interval_ms: Optional[int] = None
    rnti_filter: Sequence[int] = ()

    def to_asn(self) -> dict:
        body = {"telemetryId": _telemetry_id(self.telemetry)}
        if self.cell_id is not None:
            body["cellId"] = int(self.cell_id)
        if self.sample_every_n != 1:
            body["sampleEveryN"] = int(self.sample_every_n)
        if self.drop_policy != "coalesceLatest":
            body["dropPolicy"] = self.drop_policy
        if self.minimum_interval_ms is not None:
            body["minimumIntervalMs"] = int(self.minimum_interval_ms)
        if self.rnti_filter:
            body["rntiFilter"] = [int(r) for r in self.rnti_filter]
        return body


@dataclass
class E3Subscription:
    subscription_id: int
    ran_function_id: int
    telemetry_ids: List[int]


def _text(value) -> str:
    """Bounded UTF-8 fields travel as OCTET STRING; decode them for callers."""
    if value is None:
        return ""
    if isinstance(value, (bytes, bytearray)):
        return bytes(value).decode("utf-8", "replace")
    return str(value)


def _telemetry_id(value: Union[int, str]) -> int:
    if isinstance(value, int):
        return value
    try:
        return TELEMETRY_IDS[value]
    except KeyError as error:
        raise ValueError(f"unknown telemetry stream {value!r}; known: {sorted(TELEMETRY_IDS)}") from error


class _FragmentAssembly:
    """Reassembles fragments keyed by (subscription, sequence) or by PDU id."""

    def __init__(self, maximum_bytes: int):
        self.maximum_bytes = maximum_bytes
        self._partial: Dict[Tuple, Tuple[bytearray, int, int]] = {}

    def add(self, key: Tuple, fragment: bytes, index: int, count: int, total: Optional[int]) -> Optional[bytes]:
        if count <= 1 and index == 0:
            if total is not None and total != len(fragment):
                raise E3ProtocolError("single-fragment payload length mismatch")
            return bytes(fragment)
        if index == 0:
            if total is None or total > self.maximum_bytes:
                raise E3ProtocolError("fragmented payload misses a valid payloadBytes")
            self._partial[key] = (bytearray(), 0, count)
        state = self._partial.get(key)
        if state is None:
            raise E3ProtocolError("fragment arrived without its first fragment")
        buffer, expected_index, expected_count = state
        if index != expected_index or count != expected_count:
            del self._partial[key]
            raise E3ProtocolError("fragments arrived out of order")
        buffer.extend(fragment)
        if len(buffer) > self.maximum_bytes:
            del self._partial[key]
            raise E3ProtocolError("reassembled payload exceeds the bound")
        if index + 1 == count:
            del self._partial[key]
            if total is not None and len(buffer) != total:
                raise E3ProtocolError("reassembled payload length mismatch")
            return bytes(buffer)
        self._partial[key] = (buffer, index + 1, count)
        return None


class E3Client:
    """Synchronous E3AP client bound to one SCTP association.

    The client is safe to drive from one thread. :meth:`indications` yields
    telemetry while control procedures issued from the same thread interleave
    transparently because every PDU is matched by type and id.
    """

    def __init__(
        self,
        host: str = "127.0.0.1",
        port: int = E3AP_DEFAULT_PORT,
        *,
        dapp_name: str = "ocudu-e3-python",
        dapp_version: str = "0.1.0",
        vendor: str = "DeepSig",
        codec: Optional[E3Codec] = None,
        schema_dir=None,
        timeout: float = 3.0,
    ):
        self.codec = codec or load_codec(schema_dir)
        self.dapp_name = dapp_name
        self.dapp_version = dapp_version
        self.vendor = vendor
        self.timeout = timeout
        try:
            self._association = SctpAssociation(host, port, timeout=timeout)
        except SctpTransportError as error:
            raise E3TransportError(str(error)) from error
        self._next_id = 1
        self._lock = threading.Lock()
        self.setup_result: Optional[SetupResult] = None
        self.dapp_id: Optional[int] = None
        # Management responses and telemetry share one association. Bound the
        # indications accumulated while a control request is in flight so a
        # stalled peer cannot grow client memory without limit.
        self._pending_indications = deque(maxlen=64)
        self.pending_indication_drops = 0
        self._indication_assembly = _FragmentAssembly(E3AP_MAX_PAYLOAD_BYTES)
        self._control_assembly = _FragmentAssembly(E3AP_MAX_PAYLOAD_BYTES)
        self._fragment_bytes = E3AP_MAX_FRAGMENT_BYTES
        self.subscriptions: Dict[int, E3Subscription] = {}
        self.on_ack: Optional[Callable[[int, dict], None]] = None

    # ------------------------------------------------------------------
    def close(self) -> None:
        self._association.close()

    def __enter__(self) -> "E3Client":
        return self

    def __exit__(self, *_exc) -> None:
        self.close()

    @property
    def connected(self) -> bool:
        return self._association.connected

    def _take_id(self) -> int:
        with self._lock:
            value = self._next_id
            self._next_id = 1 if value == 0xFFFFFFFF else value + 1
            return value

    def _send(self, pdu_id: int, name: str, body: dict) -> None:
        try:
            self._association.send(self.codec.encode_pdu(pdu_id, name, body))
        except SctpTransportError as error:
            raise E3TransportError(str(error)) from error

    def _receive(self, timeout: Optional[float]) -> Optional[Tuple[int, str, dict]]:
        try:
            message = self._association.receive(timeout)
        except SctpTransportError as error:
            raise E3TransportError(str(error)) from error
        if message is None:
            return None
        try:
            return self.codec.decode_pdu(message)
        except Exception as error:  # asn1tools raises its own hierarchy
            raise E3ProtocolError(f"malformed E3-PDU from the agent: {error}") from error

    def _dispatch_indication(self, body: dict) -> None:
        subscription_id = body.get("subscriptionId")
        sequence = read_u64(body["sequence"]) if "sequence" in body else None
        key = (subscription_id, sequence)
        payload = self._indication_assembly.add(
            key,
            body["protocolData"],
            body.get("fragmentIndex", 0),
            body.get("fragmentCount", 1),
            body.get("payloadBytes"),
        )
        if payload is not None:
            if len(self._pending_indications) == self._pending_indications.maxlen:
                self.pending_indication_drops += 1
            self._pending_indications.append(decode_indication(self.codec, payload))

    def _wait_for(self, predicate: Callable[[int, str, dict], bool], timeout: Optional[float]) -> Tuple[int, str, dict]:
        """Receive PDUs until one satisfies the predicate; indications are queued."""
        import time

        deadline = None if timeout is None else time.monotonic() + timeout
        while True:
            remaining = None if deadline is None else max(0.0, deadline - time.monotonic())
            if deadline is not None and remaining == 0.0:
                raise E3TransportError("timed out waiting for the agent")
            pdu = self._receive(remaining)
            if pdu is None:
                raise E3TransportError("timed out waiting for the agent")
            pdu_id, name, body = pdu
            if name == "indicationMessage":
                self._dispatch_indication(body)
                continue
            if name == "messageAck" and self.on_ack is not None and not predicate(pdu_id, name, body):
                self.on_ack(pdu_id, body)
                continue
            if predicate(pdu_id, name, body):
                return pdu
            # Unsolicited procedure PDUs are ignored; the agent never sends them today.

    # ------------------------------------------------------------------
    # Procedures
    # ------------------------------------------------------------------
    def setup(self) -> SetupResult:
        """Run E3 Setup and learn the RAN functions the agent offers."""
        pdu_id = self._take_id()
        self._send(
            pdu_id,
            "setupRequest",
            {
                "e3apProtocolVersion": b"1.0.0",
                "dAppName": self.dapp_name.encode("utf-8"),
                "dAppVersion": self.dapp_version.encode("utf-8"),
                "vendor": self.vendor.encode("utf-8"),
            },
        )
        _, _, body = self._wait_for(lambda i, n, b: n == "setupResponse" and b.get("requestId") == pdu_id, self.timeout)
        if body.get("responseCode") != "positive":
            raise E3ProtocolError("E3 setup refused", body.get("cause"), _text(body.get("detail")))
        functions: Dict[int, RanFunction] = {}
        for entry in body.get("ranFunctionList", []):
            function = RanFunction(
                ran_function_id=entry["ranFunctionIdentifier"],
                name=_text(entry.get("ranFunctionName")),
                version=_text(entry.get("ranFunctionVersion")),
                telemetry_ids=list(entry.get("telemetryIdentifierList", [])),
                control_ids=list(entry.get("controlIdentifierList", [])),
            )
            data = entry.get("ranFunctionData", b"")
            if function.ran_function_id == RAN_FUNCTION_TELEMETRY and data:
                decoded = self.codec.decode("Telemetry-RanFunctionData", data)
                function.maximum_object_bytes = decoded.get("maximumObjectBytes")
                function.maximum_subscriptions = decoded.get("maximumSubscriptions")
                for stream in decoded.get("streams", []):
                    function.streams.append(
                        TelemetryStream(
                            telemetry_id=stream["telemetryId"],
                            name=stream["name"].decode("utf-8", "replace"),
                            description=stream.get("description", b"").decode("utf-8", "replace"),
                            class_c_portable=bool(stream.get("classCPortable")),
                            interface_id=stream["interfaceId"],
                            minimum_interval_us=read_u64(stream["minimumIntervalUs"]) if "minimumIntervalUs" in stream else None,
                            cell_ids=list(stream.get("cellIds", [])),
                            hard_floor_interval_us=read_u64(stream["hardFloorIntervalUs"]) if "hardFloorIntervalUs" in stream else None,
                        )
                    )
            elif function.ran_function_id == RAN_FUNCTION_MANAGEMENT and data:
                decoded = self.codec.decode("Mgmt-RanFunctionData", data)
                function.schema_module = decoded.get("schemaModule", b"").decode("utf-8", "replace")
                function.maximum_object_bytes = decoded.get("maximumBytes")
            functions[function.ran_function_id] = function
        result = SetupResult(
            dapp_id=body["dAppIdentifier"],
            ran_identifier=_text(body["ranIdentifier"]),
            protocol_version=_text(body.get("e3apProtocolVersion")) or None,
            ran_functions=functions,
            maximum_payload_bytes=body.get("maximumPayloadBytes", E3AP_MAX_PAYLOAD_BYTES),
            maximum_fragment_bytes=body.get("maximumFragmentBytes", E3AP_MAX_FRAGMENT_BYTES),
        )
        self._fragment_bytes = result.maximum_fragment_bytes
        self.setup_result = result
        self.dapp_id = result.dapp_id
        return result

    def _require_setup(self) -> int:
        if self.dapp_id is None:
            raise E3ProtocolError("call setup() before other procedures")
        return self.dapp_id

    def subscribe(
        self,
        telemetries: Iterable[Union[int, str]],
        *,
        configs: Sequence[SubscriptionConfig] = (),
        periodicity_ms: Optional[int] = None,
        subscription_time_s: Optional[int] = None,
    ) -> E3Subscription:
        """Subscribe to telemetry streams of RAN function 10."""
        dapp_id = self._require_setup()
        ids = [_telemetry_id(item) for item in telemetries]
        if not ids:
            raise ValueError("at least one telemetry identifier is required")
        body = {
            "dAppIdentifier": dapp_id,
            "ranFunctionIdentifier": RAN_FUNCTION_TELEMETRY,
            "telemetryIdentifierList": ids,
            "controlIdentifierList": [],
        }
        if periodicity_ms is not None:
            body["periodicity"] = int(periodicity_ms)
        if subscription_time_s is not None:
            body["subscriptionTime"] = int(subscription_time_s)
        if configs:
            body["subscriptionData"] = self.codec.encode(
                "Telemetry-SubscriptionData", {"configs": [config.to_asn() for config in configs]}
            )
        pdu_id = self._take_id()
        self._send(pdu_id, "subscriptionRequest", body)
        _, _, response = self._wait_for(
            lambda i, n, b: n == "subscriptionResponse" and b.get("requestId") == pdu_id, self.timeout
        )
        if response.get("responseCode") != "positive":
            raise E3ProtocolError("E3 subscription refused", response.get("cause"), _text(response.get("detail")))
        subscription = E3Subscription(
            subscription_id=response["subscriptionId"],
            ran_function_id=RAN_FUNCTION_TELEMETRY,
            telemetry_ids=list(response.get("telemetryIdentifierList", ids)),
        )
        self.subscriptions[subscription.subscription_id] = subscription
        return subscription

    def unsubscribe(self, subscription: Union[int, E3Subscription]) -> None:
        dapp_id = self._require_setup()
        subscription_id = subscription.subscription_id if isinstance(subscription, E3Subscription) else int(subscription)
        pdu_id = self._take_id()
        self._send(pdu_id, "subscriptionDelete", {"dAppIdentifier": dapp_id, "subscriptionId": subscription_id})
        _, _, ack = self._wait_for(lambda i, n, b: n == "messageAck" and b.get("requestId") == pdu_id, self.timeout)
        if ack.get("responseCode") != "positive":
            raise E3ProtocolError("E3 subscription delete refused", ack.get("cause"), _text(ack.get("detail")))
        self.subscriptions.pop(subscription_id, None)

    def release(self) -> None:
        """Send E3 Release; the agent removes every subscription and closes the association."""
        dapp_id = self._require_setup()
        pdu_id = self._take_id()
        self._send(pdu_id, "releaseMessage", {"dAppIdentifier": dapp_id})
        try:
            self._wait_for(lambda i, n, b: n == "messageAck" and b.get("requestId") == pdu_id, self.timeout)
        except E3TransportError:
            pass
        self.subscriptions.clear()
        self.close()

    # ------------------------------------------------------------------
    # Indications
    # ------------------------------------------------------------------
    def next_indication(self, timeout: Optional[float] = 1.0) -> Optional[TelemetryIndication]:
        """Return the next complete telemetry indication or ``None`` on timeout."""
        import time

        if self._pending_indications:
            return self._pending_indications.popleft()
        deadline = None if timeout is None else time.monotonic() + timeout
        while True:
            remaining = None if deadline is None else max(0.0, deadline - time.monotonic())
            if deadline is not None and remaining == 0.0:
                return None
            pdu = self._receive(remaining)
            if pdu is None:
                return None
            pdu_id, name, body = pdu
            if name == "indicationMessage":
                self._dispatch_indication(body)
                if self._pending_indications:
                    return self._pending_indications.popleft()
            elif name == "messageAck" and self.on_ack is not None:
                self.on_ack(pdu_id, body)
            # Other procedure PDUs outside a pending request are ignored.

    def indications(self, timeout: Optional[float] = 1.0, stop: Optional[Callable[[], bool]] = None) -> Iterator[TelemetryIndication]:
        """Iterate over indications until ``stop()`` returns true or the association closes."""
        while self.connected and (stop is None or not stop()):
            indication = self.next_indication(timeout)
            if indication is not None:
                yield indication

    # ------------------------------------------------------------------
    # Control (management service model)
    # ------------------------------------------------------------------
    def management(self, request: dict, timeout: Optional[float] = None) -> dict:
        """Send one ``ManagementRequest`` as a control action and return the decoded ``ManagementResponse``."""
        dapp_id = self._require_setup()
        timeout = self.timeout if timeout is None else timeout
        payload = self.codec.encode("ManagementRequest", request)
        pdu_id = self._take_id()
        fragments = [payload[i : i + self._fragment_bytes] for i in range(0, len(payload), self._fragment_bytes)]
        for index, fragment in enumerate(fragments):
            body = {
                "dAppIdentifier": dapp_id,
                "ranFunctionIdentifier": RAN_FUNCTION_MANAGEMENT,
                "controlIdentifier": MANAGEMENT_CONTROL_REQUEST,
                "actionData": fragment,
                "payloadBytes": len(payload),
            }
            if len(fragments) > 1:
                body["fragmentIndex"] = index
                body["fragmentCount"] = len(fragments)
            self._send(pdu_id, "dAppControlAction", body)
        _, _, ack = self._wait_for(lambda i, n, b: n == "messageAck" and b.get("requestId") == pdu_id, timeout)
        if ack.get("responseCode") != "positive":
            raise E3ProtocolError("management control refused", ack.get("cause"), _text(ack.get("detail")))
        outcome = None
        while outcome is None:
            _, _, body = self._wait_for(lambda i, n, b: n == "controlResponse" and b.get("requestId") == pdu_id, timeout)
            fragment = body.get("controlOutcome")
            if fragment is None:
                raise E3ProtocolError("management control failed", body.get("cause"), _text(body.get("detail")))
            outcome = self._control_assembly.add(
                ("control", pdu_id),
                fragment,
                body.get("fragmentIndex", 0),
                body.get("fragmentCount", 1),
                body.get("payloadBytes") or None,
            )
        response = self.codec.decode("ManagementResponse", outcome)
        if response.get("transactionId") != request["header"]["transactionId"]:
            raise E3ProtocolError("management response transaction mismatch")
        return response

    # Convenience wrappers ------------------------------------------------
    def inventory(self, **kwargs) -> dict:
        return mgmt.normalize(self.management(mgmt.inventory_request(**kwargs)))

    def health(self) -> dict:
        return mgmt.normalize(self.management(mgmt.health_request()))

    def metrics(self, instance_id: Union[str, bytes], schema: bool = False) -> dict:
        identifier = mgmt.parse_identifier(instance_id) if isinstance(instance_id, str) else instance_id
        return mgmt.normalize(self.management(mgmt.metrics_request(identifier, schema=schema)))

    def incidents(self, **kwargs) -> dict:
        return mgmt.normalize(self.management(mgmt.incidents_request(**kwargs)))

    def cell_info(self) -> dict:
        return mgmt.normalize(self.management(mgmt.cell_info_request()))
