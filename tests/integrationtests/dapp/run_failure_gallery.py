#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear

"""Run and validate the live-DU dApp failure gallery."""

import argparse
import json
import jsonschema
import os
from pathlib import Path
import subprocess
import sys
import tempfile


REPORT_SCHEMA = "ocudu.dapp.failure-gallery-report.v1"
SCENARIOS = {
    "authority_conflict",
    "native_exception",
    "native_invalid_output",
    "native_deadline_fallback",
    "process_heartbeat_recovery",
    "process_exit_recovery",
    "process_invalid_output",
}
MANIFESTS = {
    "failure_channel_estimator_exception_cpu.manifest.json",
    "failure_channel_estimator_invalid_cpu.manifest.json",
    "failure_channel_estimator_deadline_cpu.manifest.json",
    "failure_spectrumd_invalid_cpu.manifest.json",
}


class ProtocolError(RuntimeError):
    pass


def require(condition, detail):
    if not condition:
        raise ProtocolError(detail)


def positive_integer(value, detail):
    require(isinstance(value, int) and not isinstance(value, bool) and value > 0, detail)
    return value


def validate_hash(value, detail):
    require(isinstance(value, str) and len(value) == 64, detail)
    try:
        bytes.fromhex(value)
    except ValueError as error:
        raise ProtocolError(detail) from error


def incident_causes(evidence):
    incidents = evidence.get("incidents")
    require(isinstance(incidents, list) and incidents, "scenario has no incident evidence")
    return {record.get("cause") for record in incidents}


def validate_report(report, schema=None):
    if schema is not None:
        try:
            jsonschema.Draft202012Validator(schema).validate(report)
        except jsonschema.ValidationError as error:
            raise ProtocolError(f"failure-gallery report violates its JSON Schema: {error.message}") from error
    require(isinstance(report, dict), "failure-gallery report is not an object")
    require(report.get("schema") == REPORT_SCHEMA, "failure-gallery report schema is unsupported")
    require(report.get("SPDX-License-Identifier") == "BSD-3-Clause-Clear", "report license is invalid")
    require(report.get("status") == "passed", "failure-gallery campaign did not pass")

    du = report.get("du")
    require(isinstance(du, dict), "report is missing DU provenance")
    positive_integer(du.get("pid"), "report DU PID is invalid")
    require(isinstance(du.get("path"), str) and du["path"], "report DU path is invalid")
    validate_hash(du.get("sha256"), "report DU hash is invalid")

    catalog = report.get("catalog")
    require(isinstance(catalog, dict), "report is missing catalog provenance")
    manifests = catalog.get("manifests")
    require(isinstance(manifests, dict) and set(manifests) == MANIFESTS, "report manifest set is incomplete")
    for digest in manifests.values():
        validate_hash(digest, "report manifest hash is invalid")

    scenarios = report.get("scenarios")
    require(isinstance(scenarios, list), "report scenarios are missing")
    by_name = {scenario.get("name"): scenario for scenario in scenarios if isinstance(scenario, dict)}
    require(set(by_name) == SCENARIOS and len(scenarios) == len(SCENARIOS), "report scenario set is incomplete")
    for name, scenario in by_name.items():
        require(scenario.get("status") == "passed", f"scenario did not pass: {name}")
        require(isinstance(scenario.get("instance_id"), str) and scenario["instance_id"], f"missing instance: {name}")
        require(isinstance(scenario.get("evidence"), dict), f"missing evidence: {name}")

    conflict = by_name["authority_conflict"]["evidence"]
    require(conflict.get("result", {}).get("code") == "lifecycleFailed", "authority conflict was not rejected")

    exception = by_name["native_exception"]["evidence"]
    positive_integer(exception.get("fallback_count"), "native exception fallback was not counted")
    positive_integer(exception.get("breaker_bypasses"), "native exception breaker bypass was not counted")
    require(exception.get("breaker_state") == 1, "native exception did not leave its breaker open")
    require("standardException" in incident_causes(exception), "native exception incident is missing")

    invalid = by_name["native_invalid_output"]["evidence"]
    positive_integer(invalid.get("fallback_count"), "native invalid-output fallback was not counted")
    require("invalidOutput" in incident_causes(invalid), "native invalid-output incident is missing")

    deadline = by_name["native_deadline_fallback"]["evidence"]
    positive_integer(deadline.get("fallback_count"), "native deadline fallback was not counted")
    require("deadlineMiss" in incident_causes(deadline), "native deadline incident is missing")

    heartbeat = by_name["process_heartbeat_recovery"]["evidence"]
    positive_integer(heartbeat.get("heartbeat_losses"), "heartbeat loss was not counted")
    require(heartbeat.get("stopped_pid") != heartbeat.get("restarted_pid"), "heartbeat recovery reused its PID")
    require("heartbeatLoss" in incident_causes(heartbeat), "heartbeat-loss incident is missing")

    process_exit = by_name["process_exit_recovery"]["evidence"]
    positive_integer(process_exit.get("successful_restarts"), "process exit was not restarted")
    require(process_exit.get("killed_pid") != process_exit.get("restarted_pid"), "process restart reused its PID")
    require("processExit" in incident_causes(process_exit), "process-exit incident is missing")

    process_invalid = by_name["process_invalid_output"]["evidence"]
    positive_integer(process_invalid.get("results_rejected"), "invalid process result was not rejected")
    require(process_invalid.get("results_accepted") == 0, "invalid process result gained authority")
    require("invalidOutput" in incident_causes(process_invalid), "process invalid-output incident is missing")

    health = report.get("health")
    require(isinstance(health, dict), "report health summary is missing")
    positive_integer(health.get("standardExceptions"), "health omitted the native exception")
    positive_integer(health.get("deadlineMisses"), "health omitted the native deadline")
    positive_integer(health.get("invalidOutputs"), "health omitted invalid output")
    teardown = report.get("teardown")
    require(isinstance(teardown, dict) and teardown.get("live_instances") == 0, "gallery teardown is incomplete")
    limitations = report.get("limitations")
    require(isinstance(limitations, list) and len(limitations) >= 2, "report limitations are incomplete")


def run(args):
    output = args.output.resolve()
    require(output.parent.is_dir(), f"output parent is missing: {output.parent}")
    with tempfile.TemporaryDirectory(prefix="ocudu-dapp-failure-gallery-") as temporary:
        report_path = Path(temporary) / "report.json"
        command = [
            sys.executable,
            str(args.driver.resolve()),
            "--odu",
            str(args.odu.resolve()),
            "--client",
            str(args.client.resolve()),
            "--schema",
            str(args.schema.resolve()),
            "--catalog",
            str(args.catalog.resolve()),
            "--preflight-helper",
            str(args.preflight_helper.resolve()),
            "--failure-gallery",
            "--failure-gallery-report",
            str(report_path),
        ]
        completed = subprocess.run(command, text=True, capture_output=True, timeout=args.timeout_seconds)
        require(completed.returncode == 0, completed.stderr or completed.stdout or "failure-gallery driver failed")
        try:
            report = json.loads(report_path.read_text(encoding="utf-8"))
            report_schema = json.loads(args.report_schema.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as error:
            raise ProtocolError(f"cannot read failure-gallery report: {error}") from error
        validate_report(report, report_schema)
        temporary_output = output.with_name(f".{output.name}.{os.getpid()}.tmp")
        temporary_output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        os.replace(temporary_output, output)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--driver", type=Path, required=True)
    parser.add_argument("--odu", type=Path, required=True)
    parser.add_argument("--client", type=Path, required=True)
    parser.add_argument("--schema", type=Path, required=True)
    parser.add_argument("--catalog", type=Path, required=True)
    parser.add_argument("--preflight-helper", type=Path, required=True)
    parser.add_argument("--report-schema", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--timeout-seconds", type=int, default=120)
    args = parser.parse_args()
    try:
        require(1 <= args.timeout_seconds <= 3600, "timeout must be in [1, 3600] seconds")
        run(args)
    except (ProtocolError, OSError, subprocess.TimeoutExpired) as error:
        print(f"failure-gallery error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
