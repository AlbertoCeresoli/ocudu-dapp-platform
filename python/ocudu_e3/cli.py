# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear
"""Command-line front end: ``python3 -m ocudu_e3 <command>``."""

from __future__ import annotations

import argparse
import json
import sys
import time
from typing import Optional

from .e3ap import E3Client, E3Error, SubscriptionConfig
from .schema import E3AP_DEFAULT_PORT, TELEMETRY_IDS
from .telemetry import ClassCPortableRecord, GnbMetricsJsonRecord, PuschCrcRecord, PuschSymbolsRecord


def _client(args) -> E3Client:
    return E3Client(args.host, args.port, dapp_name=args.dapp_name, schema_dir=args.schema_dir, timeout=args.timeout)


def cmd_catalog(args) -> int:
    with _client(args) as client:
        setup = client.setup()
        print(f"RAN {setup.ran_identifier} (E3AP {setup.protocol_version}) assigned dApp id {setup.dapp_id}")
        for function in setup.ran_functions.values():
            print(f"- RAN function {function.ran_function_id}: {function.name} {function.version}")
            if function.control_ids:
                print(f"    control identifiers: {function.control_ids}")
            for stream in function.streams:
                interval = f", default floor {stream.minimum_interval_us / 1000:.0f} ms" if stream.minimum_interval_us else ""
                interval += f", hard floor {stream.hard_floor_interval_us / 1000:.0f} ms" if stream.hard_floor_interval_us else ""
                kind = "class-C snapshot" if stream.class_c_portable else "telemetry record"
                print(f"    telemetry {stream.telemetry_id:>2} {stream.name:<20} {kind} (interface 0x{stream.interface_id:08x}{interval})")
                print(f"       {stream.description}")
    return 0


def _summarize(indication) -> str:
    record = indication.record
    prefix = f"[{indication.stream}] cell {indication.cell_id} seq {indication.sequence} dropped {indication.dropped_objects}"
    if isinstance(record, PuschSymbolsRecord):
        sinr = f"{record.sinr_db:.1f} dB" if record.sinr_db is not None else "n/a"
        return f"{prefix} rnti 0x{record.rnti:04x} sfn {record.radio_slot.sfn}.{record.radio_slot.slot} prb {record.nof_prb} mod {record.modulation_bits} sinr {sinr} samples {len(record.equalized_symbols) // 8}"
    if isinstance(record, PuschCrcRecord):
        return f"{prefix} rnti 0x{record.rnti:04x} sfn {record.radio_slot.sfn}.{record.radio_slot.slot} crc {'ok' if record.crc_ok else 'KO'}"
    if isinstance(record, GnbMetricsJsonRecord):
        return f"{prefix} json {len(record.json_bytes)} bytes"
    if isinstance(record, ClassCPortableRecord):
        return f"{prefix} interface 0x{record.interface_id:08x} metadata {len(record.metadata)} B tensor {len(record.tensor)} B"
    return f"{prefix} {type(record).__name__} rnti 0x{getattr(record, 'rnti', 0):04x}"


def cmd_subscribe(args) -> int:
    configs = []
    if args.rnti:
        rntis = [int(value, 0) for value in args.rnti]
        configs = [SubscriptionConfig(stream, rnti_filter=rntis, cell_id=args.cell) for stream in args.stream]
    elif args.cell is not None:
        configs = [SubscriptionConfig(stream, cell_id=args.cell) for stream in args.stream]
    with _client(args) as client:
        client.setup()
        subscription = client.subscribe(args.stream, configs=configs, periodicity_ms=args.periodicity_ms)
        print(f"subscription {subscription.subscription_id} -> telemetry {subscription.telemetry_ids}", file=sys.stderr)
        deadline = None if args.duration is None else time.monotonic() + args.duration
        count = 0
        for indication in client.indications(timeout=1.0, stop=lambda: deadline is not None and time.monotonic() >= deadline):
            count += 1
            if args.json:
                record = indication.record
                payload = {
                    "stream": indication.stream,
                    "cell_id": indication.cell_id,
                    "sequence": indication.sequence,
                    "dropped_objects": indication.dropped_objects,
                    "record": {k: v for k, v in vars(record).items() if not isinstance(v, (bytes, bytearray))},
                }
                print(json.dumps(payload, default=str))
            else:
                print(_summarize(indication))
            if args.maximum and count >= args.maximum:
                break
        if not args.no_release:
            try:
                client.release()
            except E3Error:
                pass
    return 0


def _print_json(value) -> None:
    print(json.dumps(value, indent=2, sort_keys=True, default=str))


def cmd_inventory(args) -> int:
    with _client(args) as client:
        client.setup()
        _print_json(client.inventory())
    return 0


def cmd_health(args) -> int:
    with _client(args) as client:
        client.setup()
        _print_json(client.health())
    return 0


def cmd_metrics(args) -> int:
    with _client(args) as client:
        client.setup()
        _print_json(client.metrics(args.instance, schema=args.schema))
    return 0


def make_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="ocudu_e3", description="OCUDU E3AP client over SCTP")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=E3AP_DEFAULT_PORT)
    parser.add_argument("--schema-dir", default=None, help="directory holding the E3 ASN.1 modules")
    parser.add_argument("--dapp-name", default="ocudu-e3-cli")
    parser.add_argument("--timeout", type=float, default=3.0)
    commands = parser.add_subparsers(dest="command", required=True)

    commands.add_parser("catalog", help="run E3 setup and list RAN functions and streams").set_defaults(func=cmd_catalog)

    subscribe = commands.add_parser("subscribe", help="subscribe to telemetry and print indications")
    subscribe.add_argument("stream", nargs="+", choices=sorted(TELEMETRY_IDS), help="telemetry stream names")
    subscribe.add_argument("--rnti", action="append", help="restrict per-UE streams to this RNTI (repeatable)")
    subscribe.add_argument("--cell", type=int, default=None)
    subscribe.add_argument("--periodicity-ms", type=int, default=None,
                           help="requested per-RNTI capture interval; 0 = every event (bounded by the advertised hard floor)")
    subscribe.add_argument("--duration", type=float, default=None, help="stop after this many seconds")
    subscribe.add_argument("--maximum", type=int, default=0, help="stop after this many indications")
    subscribe.add_argument("--json", action="store_true", help="print one JSON object per indication")
    subscribe.add_argument("--no-release", action="store_true")
    subscribe.set_defaults(func=cmd_subscribe)

    commands.add_parser("inventory", help="dApp inventory over the management RAN function").set_defaults(func=cmd_inventory)
    commands.add_parser("health", help="aggregate dApp health").set_defaults(func=cmd_health)
    metrics = commands.add_parser("metrics", help="metrics of one dApp instance")
    metrics.add_argument("instance", help="instance identifier (32 hex digits)")
    metrics.add_argument("--schema", action="store_true")
    metrics.set_defaults(func=cmd_metrics)
    return parser


def main(argv: Optional[list] = None) -> int:
    args = make_parser().parse_args(argv)
    try:
        return args.func(args)
    except E3Error as error:
        print(f"ocudu_e3: {error}", file=sys.stderr)
        return 2
    except KeyboardInterrupt:
        return 130
