#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear

"""Qualify full-width CUDA Class-C process throughput under a real DU load."""

from __future__ import annotations

import argparse
import importlib.util
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import time


REPORT_SCHEMA = "ocudu.dapp.class-c-cuda-throughput-report.v1"
POLICY_SCHEMA = "ocudu.dapp.class-c-cuda-throughput-policy.v1"
READY_SCHEMA = "ocudu.dapp.class-c-cuda-throughput-load-ready.v1"
RESULT_SCHEMA = "ocudu.dapp.class-c-cuda-throughput-load-result.v1"
PACKAGE_MANIFESTS = (
    "ref_receiver_cuda.manifest.json",
    "ref_srs_isac_cuda.manifest.json",
    "ref_spectrumd_cuda.manifest.json",
    "ref_srs_isacd_cuda.manifest.json",
)
ROUTES = {
    "spectrum": ("spectrum_process", "process.results_accepted"),
    "srs_isac": ("srs_isac_process", "process.results_busy"),
}
COUNTERS = {
    "publications": "class_c.route.publications",
    "payload_bytes": "class_c.route.payload_bytes",
    "backpressure_drops": "class_c.route.drops.backpressure",
    "invalid_drops": "class_c.route.drops.invalid",
    "encode_failures": "class_c.route.failures.encode",
    "notification_failures": "class_c.route.failures.notification",
    "concurrent_drops": "class_c.route.drops.concurrent",
    "snapshot_count": "class_c.copy.snapshot.count",
    "snapshot_bytes": "class_c.copy.snapshot.bytes",
    "lease_count": "class_c.copy.lease.count",
    "export_count": "class_c.copy.export.count",
    "export_bytes": "class_c.copy.export.bytes",
    "e3_payload_count": "class_c.copy.e3_payload.count",
    "e3_payload_bytes": "class_c.copy.e3_payload.bytes",
    "export_slot_reservation_failures": "class_c.export_slots.reservation_failures",
}


def load_sibling(filename: str, name: str):
    path = Path(__file__).with_name(filename)
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load protocol helper: {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


contention = load_sibling("run_class_a_cuda_contention.py", "ocudu_cuda_contention")
qualification = contention.qualification
ProtocolError = qualification.ProtocolError


def artifact(path: Path) -> dict:
    return {"path": str(path), "sha256": qualification.sha256(path)}


def load_policy(path: Path) -> dict:
    try:
        policy = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ProtocolError(f"cannot read throughput policy: {error}") from error
    required = {
        "SPDX-FileCopyrightText",
        "SPDX-License-Identifier",
        "schema",
        "platform_id",
        "channel_bandwidth_mhz",
        "common_scs_khz",
        "time_scaling",
        "srs_period_ms",
        "duration_seconds",
        "restart_cycles_before_measurement",
        "minimum_spectrum_tensor_bytes",
        "minimum_srs_tensor_bytes",
        "minimum_spectrum_publications_per_second",
        "minimum_srs_publications_per_second",
        "minimum_spectrum_export_gbps",
        "minimum_srs_export_gbps",
        "maximum_backpressure_drop_rate",
        "maximum_export_slot_high_watermark_ratio",
        "release_controls_approved",
        "controls_rationale",
    }
    if not isinstance(policy, dict) or not required.issubset(policy):
        raise ProtocolError("throughput policy is missing required fields")
    if set(policy) - required - {"operator_notes"}:
        raise ProtocolError("throughput policy contains unsupported fields")
    if policy["schema"] != POLICY_SCHEMA:
        raise ProtocolError("unsupported throughput policy schema")
    if policy["SPDX-License-Identifier"] != "BSD-3-Clause-Clear":
        raise ProtocolError("throughput policy has the wrong SPDX license")
    expected = {"channel_bandwidth_mhz": 100, "common_scs_khz": 30, "time_scaling": 1}
    for field, value in expected.items():
        if policy[field] != value:
            raise ProtocolError(f"{field} must equal {value} for the full-width v1 protocol")
    integer_ranges = {
        "duration_seconds": (30, 3600),
        "restart_cycles_before_measurement": (1, 100),
        "minimum_spectrum_tensor_bytes": (183456, 1 << 40),
        "minimum_srs_tensor_bytes": (1, 1 << 40),
    }
    for field, (minimum, maximum) in integer_ranges.items():
        value = policy[field]
        if isinstance(value, bool) or not isinstance(value, int) or not minimum <= value <= maximum:
            raise ProtocolError(f"invalid policy field {field}")
    if policy["srs_period_ms"] not in {5, 10, 20, 40, 80, 160, 320, 640, 1280}:
        raise ProtocolError("unsupported SRS period in throughput policy")
    for field in (
        "minimum_spectrum_publications_per_second",
        "minimum_srs_publications_per_second",
        "minimum_spectrum_export_gbps",
        "minimum_srs_export_gbps",
    ):
        value = policy[field]
        if isinstance(value, bool) or not isinstance(value, (int, float)) or value < 0:
            raise ProtocolError(f"invalid policy field {field}")
    for field in ("maximum_backpressure_drop_rate", "maximum_export_slot_high_watermark_ratio"):
        value = policy[field]
        if isinstance(value, bool) or not isinstance(value, (int, float)) or not 0 <= value <= 1:
            raise ProtocolError(f"invalid policy field {field}")
    if policy["maximum_export_slot_high_watermark_ratio"] == 0:
        raise ProtocolError("export-slot high-watermark ratio must be positive")
    if not isinstance(policy["release_controls_approved"], bool):
        raise ProtocolError("release_controls_approved must be boolean")
    if not all(isinstance(policy[field], str) and policy[field] for field in ("platform_id", "controls_rationale")):
        raise ProtocolError("policy identity and controls rationale must be nonempty")
    return policy


def catalog_provenance(root: Path) -> list[dict]:
    packages = []
    for filename in PACKAGE_MANIFESTS:
        manifest = root / filename
        try:
            parsed = json.loads(manifest.read_text(encoding="utf-8"))
            binary = (root / parsed["artifact"]).resolve(strict=True)
        except (OSError, KeyError, TypeError, json.JSONDecodeError) as error:
            raise ProtocolError(f"cannot read package manifest {manifest}: {error}") from error
        if binary.parent != root.resolve():
            raise ProtocolError(f"package artifact leaves catalog: {binary}")
        if qualification.sha256(binary) != parsed.get("sha256"):
            raise ProtocolError(f"package artifact hash mismatch: {binary}")
        if not all(isinstance(parsed.get(field), str) and parsed[field] for field in ("package_id", "global_name")):
            raise ProtocolError(f"package identity is missing: {manifest}")
        packages.append(
            {
                "manifest": artifact(manifest),
                "package_id": parsed["package_id"],
                "global_name": parsed["global_name"],
                "artifact": artifact(binary),
            }
        )
    return packages


def gpu_inventory(executable: Path) -> list[dict]:
    fields = "index,name,uuid,pci.bus_id,driver_version"
    completed = subprocess.run(
        [str(executable), f"--query-gpu={fields}", "--format=csv,noheader,nounits"],
        text=True,
        capture_output=True,
        timeout=10,
    )
    if completed.returncode != 0:
        raise ProtocolError(f"GPU inventory failed: {completed.stderr or completed.stdout}")
    devices = []
    for line in completed.stdout.splitlines():
        values = [value.strip() for value in line.split(",")]
        if len(values) != 5 or not all(values):
            raise ProtocolError(f"malformed GPU inventory row: {line!r}")
        devices.append(dict(zip(("index", "name", "uuid", "pci_bus_id", "driver_version"), values)))
    if not devices:
        raise ProtocolError("GPU inventory is empty")
    return devices


def gpu_client_pids(executable: Path) -> set[int]:
    completed = subprocess.run(
        [str(executable), "--query-compute-apps=pid", "--format=csv,noheader,nounits"],
        text=True,
        capture_output=True,
        timeout=10,
    )
    if completed.returncode != 0:
        raise ProtocolError(f"GPU process query failed: {completed.stderr or completed.stdout}")
    try:
        return {int(line.strip()) for line in completed.stdout.splitlines() if line.strip()}
    except ValueError as error:
        raise ProtocolError("GPU process query returned a malformed PID") from error


def validate_profile(payload: dict, policy: dict) -> dict:
    profile = payload.get("load_profile")
    expected = {
        "channel_bandwidth_mhz": policy["channel_bandwidth_mhz"],
        "common_scs_khz": policy["common_scs_khz"],
        "time_scaling": policy["time_scaling"],
        "srs_period_ms": policy["srs_period_ms"],
    }
    if profile != expected:
        raise ProtocolError(f"load profile does not match throughput policy: {profile!r}")
    return profile


def validate_load_record(payload: dict, restart_cycles: int, expected_schema: str) -> set[int]:
    if payload.get("schema") != expected_schema:
        raise ProtocolError(f"unsupported Class-C load schema: {payload.get('schema')!r}")
    if payload.get("SPDX-License-Identifier") != "BSD-3-Clause-Clear":
        raise ProtocolError("Class-C load record has the wrong SPDX license")
    if payload.get("restart_cycles") != restart_cycles:
        raise ProtocolError("Class-C load restart count does not match the campaign")
    du_pid = payload.get("du_pid")
    instances = payload.get("instances")
    required = {instance_name for instance_name, _ in ROUTES.values()}
    if not isinstance(du_pid, int) or du_pid <= 0 or not isinstance(instances, dict) or set(instances) != required:
        raise ProtocolError("Class-C load record has an invalid process or instance set")
    worker_pids = set()
    for route, (instance_name, result_metric) in ROUTES.items():
        instance = instances[instance_name]
        if not isinstance(instance, dict) or not isinstance(instance.get("instance_id"), str):
            raise ProtocolError(f"{route} load instance identity is invalid")
        if contention.metric(instance, "class_c.copy.export.count") == 0:
            raise ProtocolError(f"{route} load instance has no exports")
        if contention.metric(instance, "class_c.copy.export.bytes") == 0:
            raise ProtocolError(f"{route} load instance has no export bytes")
        if contention.metric(instance, "class_c.copy.snapshot.count") != 0:
            raise ProtocolError(f"{route} load instance used a host snapshot")
        if contention.metric(instance, result_metric) == 0:
            raise ProtocolError(f"{route} worker has no source-valid results")
        if contention.metric(instance, "process.exits") != restart_cycles or contention.metric(
            instance, "process.successful_restarts"
        ) != restart_cycles:
            raise ProtocolError(f"{route} restart accounting does not match the campaign")
        if contention.metric(instance, "process.result_protocol_failures") != 0:
            raise ProtocolError(f"{route} worker emitted an invalid result")
        worker_pids.add(contention.metric(instance, "process.pid"))
    if 0 in worker_pids or du_pid in worker_pids or len(worker_pids) != len(ROUTES):
        raise ProtocolError("Class-C load worker PIDs are invalid or aliased")
    return {du_pid, *worker_pids}


def validate_final_record(result: dict, ready: dict, restart_cycles: int) -> None:
    if validate_load_record(result, restart_cycles, RESULT_SCHEMA) != validate_load_record(
        ready, restart_cycles, READY_SCHEMA
    ):
        raise ProtocolError("Class-C load PIDs changed during measurement")
    if result["du_pid"] != ready["du_pid"]:
        raise ProtocolError("Class-C load DU identity changed during measurement")
    for _, (instance_name, _) in ROUTES.items():
        before = ready["instances"][instance_name]
        after = result["instances"][instance_name]
        if before["instance_id"] != after["instance_id"]:
            raise ProtocolError(f"Class-C instance identity changed: {instance_name}")
        if contention.metric(after, "class_c.copy.export.count") <= contention.metric(
            before, "class_c.copy.export.count"
        ):
            raise ProtocolError(f"Class-C export made no progress: {instance_name}")


def load_command(args, ready_path: Path, release_path: Path, result_path: Path) -> list[str]:
    return [
        sys.executable,
        str(args.load_driver),
        "--odu",
        str(args.odu),
        "--client",
        str(args.client),
        "--schema",
        str(args.e3_schema),
        "--catalog",
        str(args.catalog),
        "--preflight-helper",
        str(args.preflight_helper),
        "--cuda-class-c",
        "--cuda-class-c-throughput",
        "--cuda-contention-cycles",
        str(args.restart_cycles),
        "--contention-ready-file",
        str(ready_path),
        "--contention-release-file",
        str(release_path),
        "--contention-result-file",
        str(result_path),
        "--contention-hold-timeout-seconds",
        str(args.load_hold_timeout_seconds),
        "--channel-bandwidth-mhz",
        str(args.channel_bandwidth_mhz),
        "--srs-period-ms",
        str(args.srs_period_ms),
        "--time-scaling",
        str(args.time_scaling),
    ]


def counter_delta(before: dict, after: dict, metric_name: str) -> int:
    first = contention.metric(before, metric_name)
    last = contention.metric(after, metric_name)
    if last < first:
        raise ProtocolError(f"metric regressed during throughput measurement: {metric_name}")
    return last - first


def analyze_route(route: str, before: dict, after: dict, elapsed_ns: int, policy: dict) -> tuple[dict, list[str]]:
    instance_name, result_metric = ROUTES[route]
    deltas = {name: counter_delta(before, after, metric_name) for name, metric_name in COUNTERS.items()}
    deltas["results"] = counter_delta(before, after, result_metric)
    if deltas["publications"] == 0 or deltas["export_count"] == 0 or deltas["export_bytes"] == 0:
        raise ProtocolError(f"{route} route made no measurable export progress")
    if deltas["publications"] < deltas["export_count"]:
        raise ProtocolError(f"{route} exported more tensors than it published")
    if deltas["export_bytes"] % deltas["export_count"] != 0:
        raise ProtocolError(f"{route} export size changed within the fixed-shape campaign")
    for field in ("snapshot_count", "snapshot_bytes", "lease_count", "e3_payload_count", "e3_payload_bytes"):
        if deltas[field] != 0:
            raise ProtocolError(f"{route} used an unexpected copy/lease path: {field}")
    capacity = contention.metric(after, "class_c.export_slots.capacity")
    in_use = contention.metric(after, "class_c.export_slots.in_use")
    high_watermark = contention.metric(after, "class_c.export_slots.high_watermark")
    if capacity < 2 or in_use > capacity or not 1 <= high_watermark <= capacity:
        raise ProtocolError(f"{route} export-slot gauges are inconsistent")
    seconds = elapsed_ns / 1_000_000_000
    publications_per_second = deltas["export_count"] / seconds
    export_bytes_per_second = deltas["export_bytes"] / seconds
    export_gbps = export_bytes_per_second * 8 / 1_000_000_000
    export_attempts = deltas["export_count"] + deltas["export_slot_reservation_failures"]
    drop_rate = deltas["export_slot_reservation_failures"] / export_attempts if export_attempts else 0.0
    tensor_bytes = deltas["export_bytes"] // deltas["export_count"]
    ratio = high_watermark / capacity
    prefix = "spectrum" if route == "spectrum" else "srs"
    reasons = []
    thresholds = (
        (tensor_bytes, policy[f"minimum_{prefix}_tensor_bytes"], "tensor bytes"),
        (publications_per_second, policy[f"minimum_{prefix}_publications_per_second"], "publications/s"),
        (export_gbps, policy[f"minimum_{prefix}_export_gbps"], "export Gb/s"),
    )
    for observed, required, label in thresholds:
        if observed < required:
            reasons.append(f"{route} {label} {observed:.9g} is below required {required:.9g}")
    if drop_rate > policy["maximum_backpressure_drop_rate"]:
        reasons.append(
            f"{route} backpressure drop rate {drop_rate:.9g} exceeds "
            f"{policy['maximum_backpressure_drop_rate']:.9g}"
        )
    if ratio > policy["maximum_export_slot_high_watermark_ratio"]:
        reasons.append(
            f"{route} export-slot high-watermark ratio {ratio:.9g} exceeds "
            f"{policy['maximum_export_slot_high_watermark_ratio']:.9g}"
        )
    for field in (
        "invalid_drops",
        "encode_failures",
        "notification_failures",
        "concurrent_drops",
        "export_slot_reservation_failures",
    ):
        if deltas[field] != 0:
            reasons.append(f"{route} recorded {deltas[field]} {field.replace('_', ' ')}")
    if deltas["results"] == 0:
        reasons.append(f"{route} worker returned no source-valid results")
    return {
        "counter_deltas": deltas,
        "tensor_bytes_per_export": tensor_bytes,
        "publications_per_second": publications_per_second,
        "export_bytes_per_second": export_bytes_per_second,
        "export_gbps": export_gbps,
        "backpressure_drop_rate": drop_rate,
        "export_slots_capacity": capacity,
        "export_slots_in_use_final": in_use,
        "export_slots_high_watermark": high_watermark,
        "export_slot_high_watermark_ratio": ratio,
        "reasons": reasons,
    }, reasons


def run(args: argparse.Namespace) -> tuple[dict, int]:
    args.load_driver = contention.resolve_file(args.load_driver)
    args.odu = contention.resolve_file(args.odu, executable=True)
    args.client = contention.resolve_file(args.client)
    args.e3_schema = contention.resolve_file(args.e3_schema)
    args.preflight_helper = contention.resolve_file(args.preflight_helper, executable=True)
    args.nvidia_smi = contention.resolve_file(args.nvidia_smi, executable=True)
    args.report_schema = contention.resolve_file(args.report_schema)
    args.policy_schema = contention.resolve_file(args.policy_schema)
    args.policy = contention.resolve_file(args.policy)
    args.catalog = args.catalog.resolve(strict=True)
    if not args.catalog.is_dir():
        raise ProtocolError(f"dApp catalog is not a directory: {args.catalog}")
    policy = load_policy(args.policy)
    args.restart_cycles = policy["restart_cycles_before_measurement"]
    args.load_hold_timeout_seconds = policy["duration_seconds"] + 600
    args.channel_bandwidth_mhz = policy["channel_bandwidth_mhz"]
    args.srs_period_ms = policy["srs_period_ms"]
    args.time_scaling = policy["time_scaling"]
    args.output_dir = args.output_dir.resolve()
    args.output_dir.mkdir(parents=True, exist_ok=False, mode=0o700)

    ready_path = args.output_dir / "load-ready.json"
    release_path = args.output_dir / "load-release"
    result_path = args.output_dir / "load-result.json"
    log_path = args.output_dir / "load-driver.log"
    command = load_command(args, ready_path, release_path, result_path)
    runner_path = Path(__file__).resolve(strict=True)
    contention_path = runner_path.with_name("run_class_a_cuda_contention.py")
    packages = catalog_provenance(args.catalog)
    report = {
        "SPDX-FileCopyrightText": "Copyright (C) 2026 DeepSig Inc.",
        "SPDX-License-Identifier": "BSD-3-Clause-Clear",
        "schema": REPORT_SCHEMA,
        "created_at": qualification.utc_now(),
        "status": "invalid",
        "policy_artifact": artifact(args.policy),
        "policy": policy,
        "host_manifest": qualification.host_manifest(),
        "gpu_inventory": gpu_inventory(args.nvidia_smi),
        "tooling": {
            "runner": artifact(runner_path),
            "contention_protocol": artifact(contention_path),
            "report_schema": artifact(args.report_schema),
            "policy_schema": artifact(args.policy_schema),
        },
        "load": {
            "odu": artifact(args.odu),
            "driver": artifact(args.load_driver),
            "client": artifact(args.client),
            "e3_schema": artifact(args.e3_schema),
            "preflight_helper": artifact(args.preflight_helper),
            "nvidia_smi": artifact(args.nvidia_smi),
            "packages": packages,
            "command": shlex.join(command),
            "status": "invalid",
            "reasons": [],
        },
        "protocol": {
            "observed_duration_ns": 0,
            "load_profile": {
                "channel_bandwidth_mhz": policy["channel_bandwidth_mhz"],
                "common_scs_khz": policy["common_scs_khz"],
                "time_scaling": policy["time_scaling"],
                "srs_period_ms": policy["srs_period_ms"],
            },
            "restart_cycles_before_measurement": args.restart_cycles,
        },
        "routes": {},
        "summary": {"total_export_bytes": 0, "aggregate_export_gbps": 0.0, "reasons": []},
    }
    invalid = False
    failed = False
    process = None
    ready = None
    observed_after = None
    baseline_pids = gpu_client_pids(args.nvidia_smi)
    report["load"]["baseline_gpu_client_pids"] = sorted(baseline_pids)

    with log_path.open("w", encoding="utf-8") as output:
        try:
            process = subprocess.Popen(command, stdout=output, stderr=subprocess.STDOUT, text=True, start_new_session=True)
            ready = contention.wait_ready(process, ready_path, args.ready_timeout_seconds)
            expected_pids = validate_load_record(ready, args.restart_cycles, READY_SCHEMA)
            validate_profile(ready, policy)
            observed_before = gpu_client_pids(args.nvidia_smi)
            if baseline_pids & expected_pids:
                raise ProtocolError("a load process PID was already present in the baseline GPU inventory")
            if observed_before != baseline_pids | expected_pids:
                raise ProtocolError(
                    f"GPU clients before measurement {sorted(observed_before)} do not match stable baseline "
                    f"{sorted(baseline_pids)} plus load {sorted(expected_pids)}"
                )
            report["load"].update(
                status="running",
                ready={"path": ready_path.name, "sha256": qualification.sha256(ready_path)},
                expected_gpu_client_pids=sorted(expected_pids),
                observed_gpu_client_pids_before=sorted(observed_before),
            )
            deadline = time.monotonic() + policy["duration_seconds"]
            while time.monotonic() < deadline:
                if process.poll() is not None:
                    raise ProtocolError(f"DU load exited during throughput measurement with status {process.returncode}")
                time.sleep(min(0.1, max(0.0, deadline - time.monotonic())))
            observed_after = gpu_client_pids(args.nvidia_smi)
            if observed_after != baseline_pids | expected_pids:
                raise ProtocolError(
                    f"GPU clients after measurement {sorted(observed_after)} do not match stable baseline "
                    f"{sorted(baseline_pids)} plus load {sorted(expected_pids)}"
                )
            report["load"]["observed_gpu_client_pids_after"] = sorted(observed_after)
        except (OSError, ProtocolError) as error:
            report["load"]["reasons"].append(str(error))
            invalid = True
        finally:
            if process is not None:
                exit_code, stop_error = contention.stop_load(process, release_path, args.load_shutdown_timeout_seconds)
                report["load"]["exit_code"] = exit_code
                if stop_error is not None:
                    report["load"]["reasons"].append(stop_error)
                    invalid = True
                if exit_code != 0:
                    report["load"]["reasons"].append(f"DU load exited with status {exit_code}")
                    invalid = True

    if ready is not None and result_path.exists():
        try:
            result = json.loads(result_path.read_text(encoding="utf-8"))
            validate_final_record(result, ready, args.restart_cycles)
            validate_profile(result, policy)
            elapsed_ns = result["published_monotonic_ns"] - ready["published_monotonic_ns"]
            if elapsed_ns < policy["duration_seconds"] * 1_000_000_000:
                raise ProtocolError("observed throughput interval is shorter than requested")
            if observed_after is None:
                raise ProtocolError("GPU clients were not observed at the end of measurement")
            report["load"].update(
                result={"path": result_path.name, "sha256": qualification.sha256(result_path)},
            )
            report["protocol"]["observed_duration_ns"] = elapsed_ns
            reasons = []
            total_bytes = 0
            for route, (instance_name, _) in ROUTES.items():
                summary, route_reasons = analyze_route(
                    route,
                    ready["instances"][instance_name],
                    result["instances"][instance_name],
                    elapsed_ns,
                    policy,
                )
                report["routes"][route] = summary
                reasons.extend(route_reasons)
                total_bytes += summary["counter_deltas"]["export_bytes"]
            if not policy["release_controls_approved"]:
                reasons.append("operator has not approved the full-width throughput controls for release admission")
            report["summary"] = {
                "total_export_bytes": total_bytes,
                "aggregate_export_gbps": total_bytes * 8 / elapsed_ns,
                "reasons": reasons,
            }
            failed = bool(reasons)
            if not invalid:
                report["load"]["status"] = "passed"
        except (KeyError, TypeError, OSError, json.JSONDecodeError, ProtocolError) as error:
            report["load"]["reasons"].append(f"invalid final load record: {error}")
            invalid = True
    else:
        report["load"]["reasons"].append("DU load produced no final throughput record")
        invalid = True

    report["load"]["log"] = {"path": log_path.name, "sha256": qualification.sha256(log_path)}
    report["finished_at"] = qualification.utc_now()
    if invalid:
        report["status"] = "invalid"
        return report, 1
    if failed:
        report["status"] = "failed"
        return report, 2
    report["status"] = "passed"
    return report, 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--policy", type=Path, required=True)
    parser.add_argument("--policy-schema", type=Path, required=True)
    parser.add_argument("--report-schema", type=Path, required=True)
    parser.add_argument("--load-driver", type=Path, required=True)
    parser.add_argument("--odu", type=Path, required=True)
    parser.add_argument("--client", type=Path, required=True)
    parser.add_argument("--e3-schema", type=Path, required=True)
    parser.add_argument("--catalog", type=Path, required=True)
    parser.add_argument("--preflight-helper", type=Path, required=True)
    parser.add_argument("--nvidia-smi", type=Path)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--ready-timeout-seconds", type=int, default=180)
    parser.add_argument("--load-shutdown-timeout-seconds", type=int, default=180)
    args = parser.parse_args()
    if args.nvidia_smi is None:
        found = shutil.which("nvidia-smi")
        if found is None:
            parser.error("--nvidia-smi is required when nvidia-smi is not on PATH")
        args.nvidia_smi = Path(found)
    report_path = args.output_dir / "throughput-report.json"
    try:
        report, status = run(args)
    except (OSError, ProtocolError) as error:
        print(f"Class-C CUDA throughput protocol failed: {error}", file=sys.stderr)
        return 1
    report_path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"throughput_status={report['status']} report={report_path}")
    return status


if __name__ == "__main__":
    sys.exit(main())
