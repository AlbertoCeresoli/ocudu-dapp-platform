#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear

"""Cross-decode C++ and Python dappctl requests with an independent APER peer."""

import argparse
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import threading

import asn1tools

TRANSACTION = "000102030405060708090a0b0c0d0e0f"
INSTANCE = "101112131415161718191a1b1c1d1e1f"
PACKAGE = "202122232425262728292a2b2c2d2e2f"
JOB = "303132333435363738393a3b3c3d3e3f"
SUBSCRIPTION = "404142434445464748494a4b4c4d4e4f"


def validate_config(operation):
    assert operation[0] == "configSet"
    values = {entry["key"]: entry["value"] for entry in operation[1]["entries"]}
    assert values[b"enabled"] == ("booleanValue", True)
    assert values[b"offset"] == ("signedValue", (-2).to_bytes(8, "big", signed=True))
    assert values[b"limit"] == ("unsignedValue", (9).to_bytes(8, "big"))
    assert values[b"gain"] == ("realValue", 1.5)
    assert values[b"label"] == ("stringValue", b"test")
    assert values[b"blob"] == ("bytesValue", b"\xaa\x55")


def validate_load(operation):
    assert operation[0] == "lifecycle"
    assert operation[1]["action"] == "load"
    assert operation[1]["placement"] == "supervisedProcess"
    assert operation[1]["backend"] == "cuda"


def validate_quiet_reservations(operation):
    assert operation[0] == "quietReservations"
    assert operation[1]["cellId"] == 2
    assert operation[1]["maximumRecords"] == 7


def validate_data_subscription(operation):
    assert operation[0] == "dataSubscriptions"
    value = operation[1]
    assert value["action"] == "create"
    assert value["subscriptionId"] == bytes.fromhex(SUBSCRIPTION)
    assert value["interfaceId"] == 0x3001
    assert value["cellId"] == 2
    assert value["sampleEveryN"] == 3
    assert value["queueDepthChunks"] == 7
    assert value["maximumChunkBytes"] == 4096
    assert value["leaseUs"] == 500000
    assert value["dropPolicy"] == "coalesceLatest"


def validate_data_poll(operation):
    assert operation[0] == "dataPoll"
    assert operation[1]["subscriptionId"] == bytes.fromhex(SUBSCRIPTION)
    assert operation[1]["maximumChunks"] == 5


def validate_ue_release(operation):
    assert operation[0] == "ranControl"
    assert operation[1]["action"] == "releaseUeToIdle"
    assert operation[1]["servingPci"] == 1
    assert operation[1]["rnti"] == 0x4601


def validate_catalog_admit(operation):
    assert operation == ("catalogAdmit", {"packageId": bytes.fromhex(PACKAGE)})


def validate_pusch_route_replace(operation):
    assert operation == ("puschRouting", {
        "action": "replace",
        "defaultTarget": ("conventional", None),
        "entries": [
            {"rnti": 0x4601, "target": ("instance", bytes.fromhex(PACKAGE))},
            {"cellIndex": 2, "rnti": 0x4602, "target": ("primary", None)},
        ],
    })


def validate_pusch_route_show(operation):
    assert operation == ("puschRouting", {"action": "query"})


def run_peer(codec, socket_path, command, expected_choice, validator=None):
    listener = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    listener.bind(socket_path)
    Path(socket_path).chmod(0o600)
    listener.listen(1)
    failure = []

    def serve():
        try:
            connection, _ = listener.accept()
            with connection:
                request = codec.decode("ManagementRequest", connection.recv(2 * 1024 * 1024))
                assert request["header"]["transactionId"] == bytes.fromhex(TRANSACTION)
                assert request["operation"][0] == expected_choice
                if validator is not None:
                    validator(request["operation"])
                response = codec.encode(
                    "ManagementResponse",
                    {"transactionId": request["header"]["transactionId"], "code": "success"},
                )
                assert connection.send(response) == len(response)
        except BaseException as error:  # Propagate assertion and transport failures to the test thread.
            failure.append(error)
        finally:
            listener.close()

    worker = threading.Thread(target=serve)
    worker.start()
    completed = subprocess.run(command, text=True, capture_output=True, timeout=10, check=False)
    worker.join(timeout=10)
    if worker.is_alive():
        raise TimeoutError("independent APER peer did not terminate")
    if failure:
        raise failure[0]
    assert completed.returncode == 0, (completed.stdout, completed.stderr)
    assert "success" in completed.stdout or "status=ok" in completed.stdout


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cpp-client", required=True)
    parser.add_argument("--python-client", required=True)
    parser.add_argument("--schema", required=True)
    args = parser.parse_args()
    codec = asn1tools.compile_files(args.schema, "per")

    cases = [
        (("catalog", "admit", "--package", PACKAGE,
          "--expected-inventory-generation", "4", "--dry-run"), "catalogAdmit", validate_catalog_admit),
        (("pusch-route", "replace", "--default", "conventional", "--entry", f"0x4601={PACKAGE}",
          "--entry", "0x4602@2=primary", "--dry-run"), "puschRouting", validate_pusch_route_replace),
        (("pusch-route", "show"), "puschRouting", validate_pusch_route_show),
        (("inventory",), "inventory", None),
        (
            ("load", "--package", PACKAGE, "--placement", "process", "--backend", "cuda", "--dry-run"),
            "lifecycle",
            validate_load,
        ),
        (("config", "set", "--instance", INSTANCE, "--config-generation", "7",
          "--set", "enabled=bool:true", "--set", "offset=i64:-2", "--set", "limit=u64:9",
          "--set", "gain=f64:1.5", "--set", "label=str:test", "--set", "blob=hex:aa55"),
         "configSet", validate_config),
        (("incidents", "list", "--instance", INSTANCE), "incidents", None),
        (("jobs", "cancel", "--job", JOB), "jobs", None),
        (("quiet-reservations", "--instance", INSTANCE, "--cell", "2", "--limit", "7"),
         "quietReservations", validate_quiet_reservations),
        (("data", "subscribe", "--subscription", SUBSCRIPTION, "--interface", "0x3001",
          "--cell", "2", "--sample-every", "3", "--queue-depth", "7",
          "--chunk-bytes", "4096", "--lease-us", "500000",
          "--drop-policy", "coalesce-latest"),
         "dataSubscriptions", validate_data_subscription),
        (("data", "poll", "--subscription", SUBSCRIPTION, "--limit", "5"),
         "dataPoll", validate_data_poll),
        (("model", "validate", "--instance", INSTANCE, "--model", "receiver",
          "--model-generation", "3"), "model", None),
        (("ue-release", "--pci", "1", "--rnti", str(0x4601), "--dry-run"),
         "ranControl", validate_ue_release),
    ]

    with tempfile.TemporaryDirectory(prefix="ocudu-e3-cli-") as directory:
        for client_name, client_prefix in (
            ("cpp", [args.cpp_client]),
            ("python", [sys.executable, args.python_client, "--schema", args.schema]),
        ):
            for index, (arguments, choice, validator) in enumerate(cases):
                socket_path = str(Path(directory) / f"{client_name}-{index}.sock")
                command = client_prefix + [
                    "--socket",
                    socket_path,
                    "--transaction",
                    TRANSACTION,
                    *arguments,
                ]
                run_peer(codec, socket_path, command, choice, validator)


if __name__ == "__main__":
    main()
