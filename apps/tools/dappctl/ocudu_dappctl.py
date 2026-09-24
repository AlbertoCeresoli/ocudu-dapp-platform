#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear

"""Management and portable Class-C client for the embedded OCUDU E3 dApp agent."""

import argparse
import json
import math
import os
from pathlib import Path
import secrets
import socket
import struct
import sys

MAX_MESSAGE_SIZE = 2 * 1024 * 1024
MAX_ARTIFACT_SIZE = 1024 * 1024
SCHEMA_NAME = "e3sm-ocudu-dapp-mgmt-v1.asn1"


# CLI spellings accepted by `lifecycle <action>` (aliases map onto the wire names).
LIFECYCLE_ACTIONS = {
    "prepare", "arm", "shadow", "enter-shadow", "activate", "enable",
    "deactivate", "disable", "retry", "retry-quiescence", "probe",
    "breaker-probe", "unload",
}


def unsigned64(value):
    if not 0 <= value <= (1 << 64) - 1:
        raise ValueError("value is outside the uint64 range")
    return value.to_bytes(8, "big")


def identifier(text):
    if any(character not in "0123456789abcdefABCDEF-" for character in text):
        raise argparse.ArgumentTypeError("identifier must be hexadecimal")
    try:
        value = bytes.fromhex(text.replace("-", ""))
    except ValueError as error:
        raise argparse.ArgumentTypeError("identifier must be hexadecimal") from error
    if len(value) != 16:
        raise argparse.ArgumentTypeError("identifier must contain exactly 16 bytes")
    return value


def positive(value):
    parsed = int(value, 0)
    if not 0 < parsed <= (1 << 64) - 1:
        raise argparse.ArgumentTypeError("value must be in [1, 2^64-1]")
    return parsed


def bounded_uint64(value):
    parsed = int(value, 0)
    if not 0 <= parsed <= (1 << 64) - 1:
        raise argparse.ArgumentTypeError("value must be in [0, 2^64-1]")
    return parsed


def bounded_integer(minimum, maximum):
    def parse(value):
        parsed = int(value, 0)
        if not minimum <= parsed <= maximum:
            raise argparse.ArgumentTypeError(
                f"value must be in [{minimum}, {maximum}]"
            )
        return parsed

    return parse


def route_target(text):
    """``primary``, ``conventional`` or an instance identifier (a PuschRouteTarget)."""
    if text in ("primary", "conventional"):
        return (text, None)
    return ("instance", identifier(text))


def route_entry(text):
    """``RNTI[@CELL]=TARGET``: RNTI and cell accept 0x-prefixed hexadecimal or decimal."""
    ue, separator, target = text.partition("=")
    if not separator:
        raise argparse.ArgumentTypeError("route entries are RNTI[@CELL]=TARGET")
    rnti_text, _, cell_text = ue.partition("@")
    try:
        rnti = int(rnti_text, 0)
        cell = int(cell_text, 0) if cell_text else None
    except ValueError as error:
        raise argparse.ArgumentTypeError("RNTI and cell must be integers") from error
    if not 1 <= rnti <= 0xFFEF or (cell is not None and not 0 <= cell <= 0xFFFE):
        raise argparse.ArgumentTypeError("RNTI must be in 1..0xFFEF and the cell in 0..65534")
    entry = {"rnti": rnti, "target": route_target(target)}
    if cell is not None:
        entry["cellIndex"] = cell
    return entry


def typed_entry(text):
    try:
        key, encoded = text.split("=", 1)
        kind, raw = encoded.split(":", 1)
    except ValueError as error:
        raise argparse.ArgumentTypeError(
            "entry must have key=bool|i64|u64|f64|str|hex:value form"
        ) from error
    if not key:
        raise argparse.ArgumentTypeError("configuration key cannot be empty")
    try:
        if kind == "bool":
            if raw not in ("true", "false", "1", "0"):
                raise ValueError("boolean must be true, false, 1, or 0")
            value = ("booleanValue", raw in ("true", "1"))
        elif kind == "i64":
            integer = int(raw, 0)
            if not -(1 << 63) <= integer < (1 << 63):
                raise ValueError("signed value is outside the int64 range")
            value = ("signedValue", integer.to_bytes(8, "big", signed=True))
        elif kind == "u64":
            value = ("unsignedValue", unsigned64(int(raw, 0)))
        elif kind == "f64":
            number = float(raw)
            if not math.isfinite(number):
                raise ValueError("floating-point value must be finite")
            value = ("realValue", number)
        elif kind == "str":
            value = ("stringValue", raw.encode("utf-8"))
        elif kind == "hex":
            value = ("bytesValue", bytes.fromhex(raw))
        else:
            raise ValueError("unknown value kind")
    except (UnicodeError, ValueError) as error:
        raise argparse.ArgumentTypeError(str(error)) from error
    return {"key": key.encode("utf-8"), "value": value}


def add_instance_options(parser, mutation=False):
    parser.add_argument("--instance", required=True, type=identifier)
    if mutation:
        parser.add_argument("--expected-generation", type=positive)
        parser.add_argument("--dry-run", action="store_true")


def request_header(args, transaction, instance_id=None, mutation=False):
    result = {"transactionId": transaction, "dryRun": bool(mutation and args.dry_run)}
    if instance_id is not None:
        result["instanceId"] = instance_id
    expected_generation = getattr(args, "expected_generation", None)
    if mutation and expected_generation is not None:
        result["expectedInstanceGeneration"] = unsigned64(expected_generation)
    return result


def read_artifact(path):
    if path is None:
        return b""
    try:
        size = path.stat().st_size
        if size > MAX_ARTIFACT_SIZE:
            raise ValueError("model artifact exceeds the 1 MiB management bound")
        return path.read_bytes()
    except OSError as error:
        raise ValueError(f"cannot read model artifact: {error}") from error


def build_request(args, transaction):
    if args.operation == "catalog-admit":
        header = request_header(args, transaction, mutation=True)
        if args.expected_inventory_generation is not None:
            header["expectedInventoryGeneration"] = unsigned64(args.expected_inventory_generation)
        return {
            "header": header,
            "operation": ("catalogAdmit", {"packageId": args.package}),
        }
    if args.operation == "inventory":
        return {
            "header": request_header(args, transaction),
            "operation": (
                "inventory",
                {
                    "includePackages": not args.omit_packages,
                    "includeInstances": not args.omit_instances,
                },
            ),
        }
    if args.operation == "load":
        header = request_header(args, transaction, mutation=True)
        if args.expected_inventory_generation is not None:
            header["expectedInventoryGeneration"] = unsigned64(
                args.expected_inventory_generation
            )
        return {
            "header": header,
            "operation": (
                "lifecycle",
                {
                    "action": "load",
                    "packageId": args.package,
                    "placement": {
                        "in-process": "inProcess",
                        "process": "supervisedProcess",
                    }[args.placement],
                    "backend": {
                        "cpu": "cpu",
                        "cuda": "cuda",
                        "other": "otherAccelerator",
                    }[args.backend],
                    "timeoutUs": 1_000_000,
                },
            ),
        }
    if args.operation == "lifecycle":
        action = {
            "shadow": "enterShadow",
            "enter-shadow": "enterShadow",
            "enable": "activate",
            "disable": "deactivate",
            "retry": "retryQuiescence",
            "retry-quiescence": "retryQuiescence",
            "probe": "breakerProbe",
            "breaker-probe": "breakerProbe",
        }.get(args.action, args.action)
        allowed = {
            "prepare",
            "arm",
            "enterShadow",
            "activate",
            "deactivate",
            "retryQuiescence",
            "unload",
            "breakerProbe",
        }
        if action not in allowed:
            raise ValueError("unknown lifecycle action")
        return {
            "header": request_header(args, transaction, args.instance, True),
            "operation": (
                "lifecycle",
                {"action": action, "timeoutUs": args.timeout_us},
            ),
        }
    if args.operation in ("config-get", "config-schema"):
        return {
            "header": request_header(args, transaction, args.instance),
            "operation": (
                "configQuery",
                {"kind": "effective" if args.operation == "config-get" else "schema"},
            ),
        }
    if args.operation == "config-set":
        boundary = {
            "immediate": "immediate",
            "invocation": "invocation",
            "slot": "slot",
            "restart": "restart",
        }[args.boundary]
        return {
            "header": request_header(args, transaction, args.instance, True),
            "operation": (
                "configSet",
                {
                    "expectedConfigGeneration": unsigned64(args.config_generation),
                    "boundary": boundary,
                    "entries": args.entries,
                },
            ),
        }
    if args.operation in ("metrics-get", "metrics-schema"):
        return {
            "header": request_header(args, transaction, args.instance),
            "operation": (
                "metrics",
                {"kind": "snapshot" if args.operation == "metrics-get" else "schema"},
            ),
        }
    if args.operation == "incidents-list":
        header = request_header(args, transaction, args.instance)
        query = {"action": "list", "maximumRecords": args.limit}
        if args.cause is not None:
            query["cause"] = args.cause
        if args.after is not None:
            query["afterIncidentId"] = unsigned64(args.after)
        return {"header": header, "operation": ("incidents", query)}
    if args.operation == "incidents-clear":
        return {
            "header": request_header(args, transaction),
            "operation": (
                "incidents",
                {
                    "action": "clearThrough",
                    "afterIncidentId": unsigned64(args.through),
                    "maximumRecords": 100,
                },
            ),
        }
    if args.operation == "health":
        return {
            "header": request_header(args, transaction),
            "operation": ("health", None),
        }
    if args.operation.startswith("jobs-"):
        action = args.operation.removeprefix("jobs-")
        query = {"action": action}
        if action != "list":
            query["jobId"] = args.job
        return {
            "header": request_header(args, transaction),
            "operation": ("jobs", query),
        }
    if args.operation == "quiet-reservations":
        query = {"maximumRecords": args.limit}
        if args.cell is not None:
            query["cellId"] = args.cell
        return {
            "header": request_header(args, transaction, args.instance),
            "operation": ("quietReservations", query),
        }
    if args.operation == "data-subscribe":
        query = {
            "action": "create",
            "subscriptionId": args.subscription,
            "interfaceId": args.interface,
            "sampleEveryN": args.sample_every,
            "queueDepthChunks": args.queue_depth,
            "maximumChunkBytes": args.chunk_bytes,
            "leaseUs": args.lease_us,
            "dropPolicy": {
                "drop-newest": "dropNewest",
                "drop-oldest": "dropOldest",
                "coalesce-latest": "coalesceLatest",
            }[args.drop_policy],
        }
        if args.cell is not None:
            query["cellId"] = args.cell
        return {
            "header": request_header(args, transaction),
            "operation": ("dataSubscriptions", query),
        }
    if args.operation == "data-list":
        return {
            "header": request_header(args, transaction),
            "operation": ("dataSubscriptions", {"action": "list"}),
        }
    if args.operation == "data-remove":
        return {
            "header": request_header(args, transaction),
            "operation": (
                "dataSubscriptions",
                {"action": "remove", "subscriptionId": args.subscription},
            ),
        }
    if args.operation == "data-poll":
        return {
            "header": request_header(args, transaction),
            "operation": (
                "dataPoll",
                {
                    "subscriptionId": args.subscription,
                    "maximumChunks": args.limit,
                },
            ),
        }
    if args.operation == "ue-release":
        return {
            "header": request_header(args, transaction, mutation=True),
            "operation": (
                "ranControl",
                {
                    "action": "releaseUeToIdle",
                    "servingPci": args.pci,
                    "rnti": args.rnti,
                },
            ),
        }
    if args.operation == "ue-handover":
        return {
            "header": request_header(args, transaction, mutation=True),
            "operation": (
                "ranControl",
                {"action": "handoverUe", "servingPci": args.pci, "rnti": args.rnti, "targetPci": args.target_pci},
            ),
        }
    if args.operation == "rf-gain":
        return {
            "header": request_header(args, transaction, mutation=True),
            "operation": (
                "ranControl",
                {
                    "action": "setTxGain" if args.direction == "tx" else "setRxGain",
                    "servingPci": 0,
                    "rnti": 0,
                    "portId": args.port,
                    "gainDb": float(args.gain_db),
                },
            ),
        }
    if args.operation == "cell-info":
        return {"header": request_header(args, transaction), "operation": ("cellInfo", {})}
    if args.operation == "pusch-route":
        action = {"show": "query", "set": "update", "replace": "replace", "clear": "clear"}[args.action]
        if action in ("query", "clear") and (args.entry or args.default):
            raise ValueError(f"pusch-route {args.action} takes no entries or default")
        if action == "update" and (args.default or not args.entry):
            raise ValueError("pusch-route set takes one or more --entry and no --default")
        body = {"action": action}
        if args.default:
            body["defaultTarget"] = route_target(args.default)
        if args.entry:
            body["entries"] = args.entry
        return {
            "header": request_header(args, transaction, mutation=action != "query"),
            "operation": ("puschRouting", body),
        }
    if args.operation == "model":
        return {
            "header": request_header(args, transaction, args.instance, True),
            "operation": (
                "model",
                {
                    "action": args.action,
                    "modelId": args.model.encode("utf-8"),
                    "modelVersion": args.version.encode("utf-8"),
                    "expectedModelGeneration": unsigned64(args.model_generation),
                    "artifact": read_artifact(args.artifact),
                },
            ),
        }
    raise ValueError("no management operation selected")


def make_parser():
    parser = argparse.ArgumentParser(
        description="Control and inspect the embedded OCUDU E3 dApp agent"
    )
    parser.add_argument(
        "--socket",
        default=os.environ.get("OCUDU_DAPP_E3_SOCKET", "/run/ocudu/dapp-management.sock"),
        help="Local credentialed E3 management socket (env OCUDU_DAPP_E3_SOCKET)",
    )
    parser.add_argument(
        "--sctp",
        default=os.environ.get("OCUDU_DAPP_E3_SCTP", ""),
        metavar="HOST:PORT",
        help="Reach the agent through the E3AP SCTP endpoint instead of the local socket",
    )
    parser.add_argument("--dapp-name", default="ocudu-dappctl", help="dApp name announced in E3 setup (SCTP only)")
    parser.add_argument("--server-uid", type=int, default=os.geteuid(), help="Expected uid of the local socket peer")
    parser.add_argument("--timeout-ms", type=positive, default=1000, help="Per-request timeout in milliseconds")
    parser.add_argument("--transaction", type=identifier, help="Explicit 16-byte hex transaction id (random when omitted)")
    parser.add_argument("--schema", type=Path, help="Override the E3 management APER schema file")
    commands = parser.add_subparsers(required=True)

    inventory = commands.add_parser("inventory")
    inventory.add_argument("--omit-packages", action="store_true")
    inventory.add_argument("--omit-instances", action="store_true")
    inventory.set_defaults(operation="inventory")

    catalog = commands.add_parser("catalog", help="Admit a locally staged signed package without restarting")
    catalog_commands = catalog.add_subparsers(required=True)
    admit = catalog_commands.add_parser("admit", help="Verify a staged UUID bundle; never load or activate it")
    admit.add_argument("--package", required=True, type=identifier)
    admit.add_argument("--expected-inventory-generation", type=positive)
    admit.add_argument("--dry-run", action="store_true")
    admit.set_defaults(operation="catalog-admit", expected_generation=None)

    load = commands.add_parser("load")
    load.add_argument("--package", required=True, type=identifier)
    load.add_argument("--placement", choices=("in-process", "process"), default="in-process")
    load.add_argument("--backend", choices=("cpu", "cuda", "other"), default="cpu")
    load.add_argument("--expected-inventory-generation", type=positive)
    load.add_argument("--dry-run", action="store_true")
    load.set_defaults(operation="load", expected_generation=None)

    lifecycle = commands.add_parser("lifecycle")
    lifecycle.add_argument(
        "action",
        choices=sorted(LIFECYCLE_ACTIONS),
        help="Lifecycle transition: prepare, arm, enter-shadow/shadow, activate/enable, "
        "deactivate/disable, retry-quiescence/retry, breaker-probe/probe, unload",
    )
    add_instance_options(lifecycle, True)
    lifecycle.add_argument("--timeout-us", type=bounded_uint64, default=1_000_000)
    lifecycle.set_defaults(operation="lifecycle")

    config = commands.add_parser("config").add_subparsers(required=True)
    for name in ("get", "schema"):
        command = config.add_parser(name)
        add_instance_options(command)
        command.set_defaults(operation=f"config-{name}")
    config_set = config.add_parser("set")
    add_instance_options(config_set, True)
    config_set.add_argument("--config-generation", type=positive, required=True)
    config_set.add_argument(
        "--boundary", choices=("immediate", "invocation", "slot", "restart"), default="invocation"
    )
    config_set.add_argument("--set", dest="entries", action="append", type=typed_entry, required=True)
    config_set.set_defaults(operation="config-set")

    metrics = commands.add_parser("metrics").add_subparsers(required=True)
    for name in ("get", "schema"):
        command = metrics.add_parser(name)
        add_instance_options(command)
        command.set_defaults(operation=f"metrics-{name}")

    incidents = commands.add_parser("incidents").add_subparsers(required=True)
    incident_list = incidents.add_parser("list")
    incident_list.add_argument("--instance", type=identifier)
    incident_list.add_argument(
        "--cause",
        choices=(
            "returnedError",
            "standardException",
            "unknownException",
            "invalidInput",
            "invalidOutput",
            "deadlineMiss",
            "deviceError",
            "resourceExhausted",
            "processExit",
            "heartbeatLoss",
            "leaseExpired",
            "quiescenceTimeout",
            "securityViolation",
            "policyRejection",
        ),
    )
    incident_list.add_argument("--after", type=bounded_uint64)
    incident_list.add_argument("--limit", type=bounded_integer(1, 1024), default=100)
    incident_list.set_defaults(operation="incidents-list")
    incident_clear = incidents.add_parser("clear")
    incident_clear.add_argument("--through", type=bounded_uint64, required=True)
    incident_clear.set_defaults(operation="incidents-clear")

    commands.add_parser("health").set_defaults(operation="health")

    jobs = commands.add_parser("jobs").add_subparsers(required=True)
    jobs.add_parser("list").set_defaults(operation="jobs-list")
    for name in ("get", "cancel"):
        command = jobs.add_parser(name)
        command.add_argument("--job", type=identifier, required=True)
        command.set_defaults(operation=f"jobs-{name}")

    quiet_reservations = commands.add_parser(
        "quiet-reservations", help="List authoritative bounded quiet reservations"
    )
    quiet_reservations.add_argument("--instance", type=identifier)
    quiet_reservations.add_argument("--cell", type=bounded_integer(0, 65535))
    quiet_reservations.add_argument("--limit", type=bounded_integer(1, 1024), default=100)
    quiet_reservations.set_defaults(operation="quiet-reservations")

    data = commands.add_parser(
        "data", help="Manage the bounded portable Class-C E3 payload profile"
    ).add_subparsers(required=True)
    data_subscribe = data.add_parser("subscribe")
    data_subscribe.add_argument("--subscription", type=identifier, required=True)
    data_subscribe.add_argument(
        "--interface", type=bounded_integer(1, (1 << 32) - 1), required=True
    )
    data_subscribe.add_argument("--cell", type=bounded_integer(0, 65535))
    data_subscribe.add_argument(
        "--sample-every", type=bounded_integer(1, 65535), default=1
    )
    data_subscribe.add_argument(
        "--queue-depth", type=bounded_integer(1, 1024), default=64
    )
    data_subscribe.add_argument(
        "--chunk-bytes", type=bounded_integer(1024, 65536), default=65536
    )
    data_subscribe.add_argument(
        "--lease-us", type=bounded_integer(1, 60000000), default=1000000
    )
    data_subscribe.add_argument(
        "--drop-policy",
        choices=("drop-newest", "drop-oldest", "coalesce-latest"),
        default="drop-newest",
    )
    data_subscribe.set_defaults(operation="data-subscribe")
    data.add_parser("list").set_defaults(operation="data-list")
    data_remove = data.add_parser("remove")
    data_remove.add_argument("--subscription", type=identifier, required=True)
    data_remove.set_defaults(operation="data-remove")
    data_poll = data.add_parser("poll")
    data_poll.add_argument("--subscription", type=identifier, required=True)
    data_poll.add_argument("--limit", type=bounded_integer(1, 8), default=8)
    data_poll.set_defaults(operation="data-poll")

    ue_release = commands.add_parser(
        "ue-release", help="Queue an RRC release-to-idle through the E3 RAN control authority"
    )
    ue_release.add_argument("--pci", type=bounded_integer(0, 1007), required=True)
    ue_release.add_argument("--rnti", type=bounded_integer(1, 65535), required=True)
    ue_release.add_argument("--dry-run", action="store_true")
    ue_release.set_defaults(operation="ue-release")

    handover = commands.add_parser("ue-handover", help="Queue a handover of a UE to another cell through E3")
    handover.add_argument("--pci", type=bounded_integer(0, 1007), required=True)
    handover.add_argument("--rnti", type=bounded_integer(1, 65535), required=True)
    handover.add_argument("--target-pci", type=bounded_integer(0, 1007), required=True)
    handover.add_argument("--dry-run", action="store_true")
    handover.set_defaults(operation="ue-handover")

    gain = commands.add_parser("rf-gain", help="Set a radio-unit TX or RX gain through E3")
    gain.add_argument("--direction", choices=("tx", "rx"), default="rx")
    gain.add_argument("--port", type=bounded_integer(0, 63), default=0)
    gain.add_argument("--gain-db", type=float, required=True)
    gain.add_argument("--dry-run", action="store_true")
    gain.set_defaults(operation="rf-gain")

    cell_info = commands.add_parser("cell-info", help="Describe the served cells and RAN control capabilities")
    cell_info.set_defaults(operation="cell-info")

    route = commands.add_parser(
        "pusch-route",
        help="Show or change the per-UE route of Class-A PUSCH grants between dApp instances",
        description="Route each UE's PUSCH grants to one channel-estimator, equalizer or receiver instance, "
        "or to the stock receiver. A target is primary (the first instance of each hook, as without routing), "
        "conventional (the stock receiver) or an instance identifier. set updates the named UEs (a primary "
        "target removes a UE's entry); replace sets the default and every entry; clear resets the table.",
    )
    route.add_argument("action", choices=("show", "set", "replace", "clear"))
    route.add_argument("--entry", type=route_entry, action="append", default=[], metavar="RNTI[@CELL]=TARGET")
    route.add_argument("--default", metavar="TARGET", help="Route of UEs without an entry (replace only)")
    route.add_argument("--dry-run", action="store_true")
    route.set_defaults(operation="pusch-route")

    model = commands.add_parser("model")
    model.add_argument("action", choices=("stage", "validate", "warm", "activate", "rollback", "retire"))
    add_instance_options(model, True)
    model.add_argument("--model", required=True)
    model.add_argument("--version", default="")
    model.add_argument("--model-generation", type=positive, required=True)
    model.add_argument("--artifact", type=Path)
    model.set_defaults(operation="model")

    commands.add_parser("codec-self-test", help="Verify independent APER golden vectors").set_defaults(
        operation="codec-self-test"
    )
    return parser


def locate_schema(explicit):
    candidates = [explicit, os.environ.get("OCUDU_DAPP_E3_SCHEMA")]
    source_root = Path(__file__).resolve().parents[3]
    candidates.extend(
        (
            source_root / "schemas/e3" / SCHEMA_NAME,
            Path(sys.prefix) / "share/ocudu/dapp/schema" / SCHEMA_NAME,
            Path("/usr/local/share/ocudu/dapp/schema") / SCHEMA_NAME,
            Path("/usr/share/ocudu/dapp/schema") / SCHEMA_NAME,
        )
    )
    for candidate in candidates:
        if candidate and Path(candidate).is_file():
            return Path(candidate)
    raise ValueError("cannot locate the OCUDU E3 management ASN.1 schema; use --schema")


def compile_codec(schema_path):
    try:
        import asn1tools
    except ImportError as error:
        raise ValueError("Python management client requires the asn1tools package") from error
    try:
        return asn1tools.compile_files(str(schema_path), "per")
    except Exception as error:
        raise ValueError(f"cannot compile E3 management schema: {error}") from error


def codec_self_test(codec):
    inventory = bytes.fromhex("00000102030405060708090a0b0c0d0e0f00")
    decoded = codec.decode("ManagementRequest", inventory)
    if codec.encode("ManagementRequest", decoded) != inventory:
        raise ValueError("inventory APER golden vector mismatch")
    config = {
        "header": {
            "transactionId": bytes(range(1, 17)),
            "instanceId": bytes(range(2, 18)),
            "expectedInstanceGeneration": unsigned64(7),
            "dryRun": True,
        },
        "operation": (
            "configSet",
            {
                "expectedConfigGeneration": unsigned64(9),
                "boundary": "slot",
                "entries": [
                    {"key": b"enabled", "value": ("booleanValue", True)},
                    {"key": b"offset", "value": ("signedValue", (-17).to_bytes(8, "big", signed=True))},
                    {"key": b"limit", "value": ("unsignedValue", unsigned64(42))},
                    {"key": b"gain", "value": ("realValue", 1.25)},
                    {"key": b"model", "value": ("stringValue", b"small")},
                    {"key": b"digest", "value": ("bytesValue", bytes((1, 2, 3, 4)))},
                ],
            },
        ),
    }
    expected = bytes.fromhex(
        "340102030405060708090a0b0c0d0e0f1002030405060708090a0b0c0d0e0f1011"
        "00000000000000078c000000000000000940000606656e61626c656408286f666673"
        "657410ffffffffffffffef046c696d697420000000000000002a036761696e300380"
        "fe05046d6f64656c400005736d616c6c05646967657374500401020304"
    )
    if codec.encode("ManagementRequest", config) != expected:
        raise ValueError("typed configuration APER golden vector mismatch")
    print("APER codec self-test passed")


IDENTIFIER_FIELDS = {
    "transactionId",
    "instanceId",
    "packageId",
    "jobId",
    "requestId",
    "reservationId",
    "sourceInstanceId",
    "subscriptionId",
    "instance",
}
TEXT_FIELDS = {
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
    "associationId",
    "openedNs",
    "controlRequests",
    "unavailableLookups",
}


def display_identifier(value):
    compact = value.hex()
    return f"{compact[:8]}-{compact[8:12]}-{compact[12:16]}-{compact[16:20]}-{compact[20:]}"


def normalize(value, field=None):
    if isinstance(value, dict):
        return {key: normalize(item, key) for key, item in value.items()}
    if isinstance(value, list):
        return [normalize(item, field) for item in value]
    if isinstance(value, tuple):
        if len(value) == 2 and isinstance(value[0], str):
            return {value[0]: normalize(value[1], value[0])}
        if len(value) == 2 and isinstance(value[0], bytes):
            return int.from_bytes(value[0], "big") >> (len(value[0]) * 8 - value[1])
    if isinstance(value, bytes):
        if field in IDENTIFIER_FIELDS:
            return display_identifier(value)
        if field in COUNTER_FIELDS:
            return int.from_bytes(value, "big")
        if field == "signedValue":
            return int.from_bytes(value, "big", signed=True)
        if field in TEXT_FIELDS:
            try:
                return value.decode("utf-8")
            except UnicodeDecodeError:
                pass
        return "hex:" + value.hex()
    return value


def exchange_sctp(endpoint, dapp_name, timeout_ms, request):
    """Carry the same ManagementRequest as an E3AP dApp control action over SCTP."""
    candidates = [
        Path(__file__).resolve().parents[3] / "python",  # source checkout
        Path(__file__).resolve().parents[1] / "share" / "ocudu" / "dapp" / "python",  # bin/ + share/ install
        Path(__file__).resolve().parents[2] / "share" / "ocudu" / "dapp" / "python",
    ]
    for python_dir in candidates:
        if (python_dir / "ocudu_e3").is_dir() and str(python_dir) not in sys.path:
            sys.path.insert(0, str(python_dir))
            break
    try:
        import ocudu_e3
    except ImportError as error:
        raise ValueError("the SCTP carrier requires the ocudu_e3 package (python/ocudu_e3)") from error
    host, _, port = endpoint.rpartition(":")
    if not host or not port.isdigit():
        raise ValueError("--sctp expects HOST:PORT")
    with ocudu_e3.E3Client(host, int(port), dapp_name=dapp_name, timeout=timeout_ms / 1000) as client:
        client.setup()
        return client.management(request)


def exchange(codec, path, expected_uid, timeout_ms, request):
    encoded = codec.encode("ManagementRequest", request)
    if len(encoded) > MAX_MESSAGE_SIZE:
        raise ValueError("management request exceeds the 2 MiB transport bound")
    with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as connection:
        connection.settimeout(timeout_ms / 1000)
        connection.connect(path)
        credentials = connection.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12)
        _, peer_uid, _ = struct.unpack("3i", credentials)
        if peer_uid != expected_uid:
            raise PermissionError("local management server credentials rejected")
        if connection.send(encoded) != len(encoded):
            raise OSError("short management request send")
        response, _, flags, _ = connection.recvmsg(MAX_MESSAGE_SIZE + 1, 0, socket.MSG_TRUNC)
        if flags & socket.MSG_TRUNC or len(response) > MAX_MESSAGE_SIZE:
            raise ValueError("management response exceeds the 2 MiB transport bound")
        if not response:
            raise ConnectionError("management peer closed without a response")
    return codec.decode("ManagementResponse", response)


def main():
    parser = make_parser()
    args = parser.parse_args()
    try:
        codec = compile_codec(locate_schema(args.schema))
        if args.operation == "codec-self-test":
            codec_self_test(codec)
            return 0
        transaction = args.transaction or secrets.token_bytes(16)
        request = build_request(args, transaction)
        if args.sctp:
            response = exchange_sctp(args.sctp, args.dapp_name, args.timeout_ms, request)
        else:
            response = exchange(codec, args.socket, args.server_uid, args.timeout_ms, request)
        if response.get("transactionId") != transaction:
            raise ValueError("management response transaction ID mismatch")
        print(json.dumps(normalize(response), indent=2, sort_keys=True))
        # Exit contract (shared with the C++ ocudu-dappctl):
        #   0 = operation accepted, 1 = transport failure, 2 = invalid input,
        #   3 = the agent refused the operation.
        return 0 if response.get("code") in ("success", "dryRunSuccess", "accepted") else 3
    except (ConnectionError, TimeoutError, OSError) as error:
        print(f"ocudu-dappctl.py: transport: {error}", file=sys.stderr)
        return 1
    except Exception as error:
        print(f"ocudu-dappctl.py: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
