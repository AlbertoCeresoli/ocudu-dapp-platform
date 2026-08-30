#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear

"""Run and validate repeated OCUDU Class-A CUDA tail qualification trials."""

from __future__ import annotations

import argparse
import csv
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import shlex
import subprocess
import sys
import time


REPORT_SCHEMA = "ocudu.class-a-cuda-qualification-report.v1"
POLICY_SCHEMA = "ocudu.class-a-cuda-qualification-policy.v1"
PRBS = (25, 106, 273)
LAYERS = (1, 2)
MODULATIONS = (2, 4, 6, 8)
SCOPES = {"submit", "completion", "device-pipeline"}
REPETITIONS = 1000
WARMUPS = 100
SYMBOLS = 12
CE_DEADLINE_US = 100
DEMODULATOR_DEADLINE_US = 150
PREFLIGHT_TIMEOUT_MS = 500
MINIMUM_TRIALS = 3

ENVIRONMENT_COLUMNS = (
    "gpu_monitor_available",
    "gpu_observation_healthy",
    "foreign_compute_processes",
    "foreign_compute_pids",
    "gpu_policy",
    "host",
    "machine",
    "gpu",
    "scheduler_policy",
    "scheduler_priority",
    "cpu_affinity",
    "pusch_stream_priority",
    "package_filter",
    "preflight_timeout_ms",
    "build_hash",
    "build_mode",
    "cuda_device",
    "cuda_driver_version",
    "cuda_runtime_version",
    "gpu_pci_bus_id",
    "nvml_driver_version",
    "gpu_uuid",
    "gpu_temperature_min_c",
    "gpu_temperature_max_c",
    "gpu_sm_clock_min_mhz",
    "gpu_sm_clock_max_mhz",
    "gpu_memory_clock_min_mhz",
    "gpu_memory_clock_max_mhz",
    "gpu_power_min_mw",
    "gpu_power_max_mw",
    "gpu_power_limit_mw",
    "gpu_persistence_mode",
    "gpu_compute_mode",
    "gpu_performance_state_best",
    "gpu_performance_state_worst",
)

REQUIRED_COLUMNS = set(ENVIRONMENT_COLUMNS) | {
    "path",
    "scope",
    "prbs",
    "layers",
    "pusch_lanes",
    "symbols",
    "warmups_per_phase",
    "modulation_order",
    "samples",
    "mean_ns",
    "p50_ns",
    "p90_ns",
    "p95_ns",
    "p99_ns",
    "p999_ns",
    "max_ns",
    "pipeline_deadline_ns",
    "pipeline_misses",
    "dapp_deadline_ns",
    "path_expected_invocations",
    "path_accepted_invocations",
    "path_production_deadline_misses",
}


class ProtocolError(RuntimeError):
    """The evidence is malformed or does not cover the fixed protocol."""


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds").replace("+00:00", "Z")


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def integer(row: dict[str, str], field: str) -> int:
    try:
        return int(row[field])
    except (KeyError, ValueError) as error:
        raise ProtocolError(f"invalid integer field {field!r}: {row.get(field)!r}") from error


def load_policy(path: Path) -> dict:
    try:
        policy = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ProtocolError(f"cannot read policy {path}: {error}") from error
    required = {
        "schema",
        "platform_id",
        "scheduler_policy",
        "minimum_scheduler_priority",
        "cpu_affinity",
        "pusch_stream_priority",
        "gpu_clock_policy",
        "gpu_power_policy",
        "maximum_gpu_temperature_c",
        "release_controls_approved",
    }
    missing = sorted(required - policy.keys())
    if missing:
        raise ProtocolError(f"policy is missing fields: {', '.join(missing)}")
    if policy["schema"] != POLICY_SCHEMA:
        raise ProtocolError(f"unsupported policy schema: {policy['schema']!r}")
    required_strings = (
        "platform_id",
        "scheduler_policy",
        "cpu_affinity",
        "pusch_stream_priority",
        "gpu_clock_policy",
        "gpu_power_policy",
    )
    if not all(isinstance(policy[field], str) and policy[field] for field in required_strings):
        raise ProtocolError("policy string fields must be nonempty")
    if not isinstance(policy["minimum_scheduler_priority"], int) or policy["minimum_scheduler_priority"] < 0:
        raise ProtocolError("minimum_scheduler_priority must be a nonnegative integer")
    if not isinstance(policy["maximum_gpu_temperature_c"], int) or not 1 <= policy["maximum_gpu_temperature_c"] <= 120:
        raise ProtocolError("maximum_gpu_temperature_c must be in [1, 120]")
    if not isinstance(policy["release_controls_approved"], bool):
        raise ProtocolError("release_controls_approved must be a boolean operator attestation")
    if policy["scheduler_policy"] not in {"fifo", "round-robin", "batch", "idle", "other"}:
        raise ProtocolError("scheduler_policy is not recognized")
    optional_strings = {
        "SPDX-FileCopyrightText",
        "SPDX-License-Identifier",
        "$schema",
        "expected_machine",
        "expected_gpu",
        "required_gpu_persistence_mode",
        "required_gpu_compute_mode",
        "operator_notes",
    }
    optional_integers = {"minimum_gpu_sm_clock_mhz", "expected_gpu_power_limit_mw"}
    known = required | optional_strings | optional_integers
    unknown = sorted(policy.keys() - known)
    if unknown:
        raise ProtocolError(f"policy has unknown fields: {', '.join(unknown)}")
    if any(field in policy and not isinstance(policy[field], str) for field in optional_strings):
        raise ProtocolError("optional policy string fields must be strings")
    if any(field in policy and not isinstance(policy[field], int) for field in optional_integers):
        raise ProtocolError("optional policy numeric fields must be integers")
    if policy.get("minimum_gpu_sm_clock_mhz", 0) < 0 or policy.get("expected_gpu_power_limit_mw", -1) < -1:
        raise ProtocolError("optional policy numeric fields are outside their allowed range")
    if policy.get("SPDX-License-Identifier", "BSD-3-Clause-Clear") != "BSD-3-Clause-Clear":
        raise ProtocolError("policy SPDX license identifier must be BSD-3-Clause-Clear")
    if policy.get("required_gpu_persistence_mode", "enabled") not in {"enabled", "disabled", "unavailable"}:
        raise ProtocolError("required_gpu_persistence_mode is not recognized")
    if policy.get("required_gpu_compute_mode", "default") not in {
        "default", "exclusive-thread", "exclusive-process", "prohibited", "unavailable"
    }:
        raise ProtocolError("required_gpu_compute_mode is not recognized")
    deployment_values = (
        policy["platform_id"],
        policy["cpu_affinity"],
        policy["gpu_clock_policy"],
        policy["gpu_power_policy"],
    )
    if any("replace" in value.lower() for value in deployment_values):
        raise ProtocolError("replace every deployment-specific example policy value before use")
    return policy


def load_csv(path: Path) -> list[dict[str, str]]:
    try:
        with path.open(newline="", encoding="utf-8") as source:
            reader = csv.DictReader(source)
            if reader.fieldnames is None or len(reader.fieldnames) != len(set(reader.fieldnames)):
                raise ProtocolError("CSV has no header or has duplicate columns")
            missing = sorted(REQUIRED_COLUMNS - set(reader.fieldnames))
            if missing:
                raise ProtocolError(f"CSV is missing columns: {', '.join(missing)}")
            rows = list(reader)
    except OSError as error:
        raise ProtocolError(f"cannot read CSV {path}: {error}") from error
    if not rows:
        raise ProtocolError("CSV contains no measurements")
    return rows


def validate_rows(
    rows: list[dict[str, str]], lanes: int, gpu_policy: str = "exclusive"
) -> tuple[dict[str, str], tuple, list[str], dict]:
    if gpu_policy not in {"exclusive", "contention"}:
        raise ProtocolError(f"unsupported GPU policy: {gpu_policy!r}")
    environment = {field: rows[0][field] for field in ENVIRONMENT_COLUMNS}
    for field, expected in environment.items():
        if any(row[field] != expected for row in rows):
            raise ProtocolError(f"environment field {field!r} changes within one run")

    fixed = {
        "pusch_lanes": lanes,
        "symbols": SYMBOLS,
        "warmups_per_phase": WARMUPS,
        "samples": REPETITIONS * lanes,
        "pipeline_deadline_ns": DEMODULATOR_DEADLINE_US * 1000,
        "preflight_timeout_ms": PREFLIGHT_TIMEOUT_MS,
    }
    for field, expected in fixed.items():
        if any(integer(row, field) != expected for row in rows):
            raise ProtocolError(f"fixed field {field!r} is not uniformly {expected}")
    for field, expected in (("gpu_policy", gpu_policy), ("package_filter", "all"), ("build_mode", "Release")):
        if any(row[field] != expected for row in rows):
            raise ProtocolError(f"fixed field {field!r} is not uniformly {expected!r}")

    keys = [
        (row["path"], row["scope"], integer(row, "prbs"), integer(row, "layers"), integer(row, "modulation_order"))
        for row in rows
    ]
    if len(keys) != len(set(keys)):
        raise ProtocolError("CSV contains duplicate path/scope/shape rows")
    if {integer(row, "prbs") for row in rows} != set(PRBS) or {integer(row, "layers") for row in rows} != set(LAYERS):
        raise ProtocolError("CSV does not cover the fixed PRB/layer envelope")
    if {row["scope"] for row in rows} != SCOPES:
        raise ProtocolError("CSV contains missing or non-qualifying scopes")

    groups: dict[tuple, set[str]] = {}
    for row in rows:
        group = (row["path"], integer(row, "prbs"), integer(row, "layers"), integer(row, "modulation_order"))
        groups.setdefault(group, set()).add(row["scope"])
    if any(scopes != SCOPES for scopes in groups.values()):
        raise ProtocolError("each path/shape/modulation must contain exactly the three qualifying scopes")

    conventional = [group for group in groups if group[0] == "accelerated-conventional"]
    expected_conventional = {("accelerated-conventional", prbs, layers, 2) for prbs in PRBS for layers in LAYERS}
    if set(conventional) != expected_conventional:
        raise ProtocolError("conventional baseline does not cover every fixed shape exactly once")
    resident_signatures = {
        (row["path"], integer(row, "modulation_order"))
        for row in rows
        if row["path"] != "accelerated-conventional"
    }
    if not resident_signatures:
        raise ProtocolError("CSV contains no resident dApp path")
    for prbs in PRBS:
        for layers in LAYERS:
            observed = {
                (row["path"], integer(row, "modulation_order"))
                for row in rows
                if row["path"] != "accelerated-conventional"
                and integer(row, "prbs") == prbs
                and integer(row, "layers") == layers
            }
            if observed != resident_signatures:
                raise ProtocolError(f"resident package matrix changes at {prbs} PRBs/{layers} layers")

    reasons = []
    monitor_ok = environment["gpu_monitor_available"] == "1" and environment["gpu_observation_healthy"] == "1"
    if not monitor_ok:
        reasons.append("GPU process observation was unavailable or unhealthy")
    foreign_processes = integer(environment, "foreign_compute_processes")
    if gpu_policy == "exclusive" and foreign_processes != 0:
        reasons.append("foreign GPU compute clients were observed")
    if gpu_policy == "contention" and foreign_processes == 0:
        reasons.append("the declared GPU contention load was not observed")

    resident_invocations = 0
    production_misses = 0
    pipeline_misses = 0
    worst = None
    for row in rows:
        conventional_path = row["path"] == "accelerated-conventional"
        expected_invocations = integer(row, "path_expected_invocations")
        accepted_invocations = integer(row, "path_accepted_invocations")
        row_production_misses = integer(row, "path_production_deadline_misses")
        if conventional_path:
            if expected_invocations != 0 or accepted_invocations != 0 or integer(row, "dapp_deadline_ns") != 0:
                raise ProtocolError("conventional path contains resident admission counters")
        else:
            required_invocations = 3 * lanes * (WARMUPS + REPETITIONS)
            if expected_invocations != required_invocations:
                raise ProtocolError("resident expected invocation count does not match the fixed protocol")
            if integer(row, "dapp_deadline_ns") not in (CE_DEADLINE_US * 1000, DEMODULATOR_DEADLINE_US * 1000):
                raise ProtocolError("resident dApp deadline is outside the v1 protocol")
            if row["scope"] == "device-pipeline":
                resident_invocations += accepted_invocations
                production_misses += row_production_misses
                if accepted_invocations != expected_invocations:
                    reasons.append(f"{row['path']} did not admit every invocation")
                if row_production_misses:
                    reasons.append(f"{row['path']} recorded {row_production_misses} production deadline misses")
        if row["scope"] == "device-pipeline":
            misses = integer(row, "pipeline_misses")
            pipeline_misses += misses
            if integer(row, "p999_ns") > integer(row, "pipeline_deadline_ns"):
                reasons.append(
                    f"{row['path']} {row['prbs']}PRB/{row['layers']}L/"
                    f"{row['modulation_order']}Qm exceeded P99.9"
                )
            candidate = (integer(row, "p999_ns"), row)
            if worst is None or candidate[0] > worst[0]:
                worst = candidate

    worst_row = worst[1]
    summary = {
        "measurement_rows": len(rows),
        "device_pipeline_rows": sum(row["scope"] == "device-pipeline" for row in rows),
        "resident_accepted_invocations": resident_invocations,
        "pipeline_deadline_misses": pipeline_misses,
        "production_deadline_misses": production_misses,
        "worst_device_pipeline": {
            field: int(worst_row[field]) if field not in ("path",) else worst_row[field]
            for field in (
                "path", "prbs", "layers", "modulation_order", "p50_ns", "p90_ns", "p99_ns", "p999_ns", "max_ns"
            )
        },
    }
    return environment, tuple(sorted(keys)), sorted(set(reasons)), summary


def validate_policy(policy: dict, environment: dict[str, str]) -> list[str]:
    reasons = []
    if not policy["release_controls_approved"]:
        reasons.append("operator has not approved the CPU/GPU controls for release admission")
    exact = {
        "scheduler_policy": policy["scheduler_policy"],
        "cpu_affinity": policy["cpu_affinity"],
        "pusch_stream_priority": policy["pusch_stream_priority"],
    }
    for field, expected in exact.items():
        if environment[field] != expected:
            reasons.append(f"{field}={environment[field]!r}, expected {expected!r}")
    if integer(environment, "scheduler_priority") < policy["minimum_scheduler_priority"]:
        reasons.append("scheduler priority is below policy minimum")
    maximum_temperature = integer(environment, "gpu_temperature_max_c")
    if maximum_temperature < 0:
        reasons.append("GPU temperature telemetry is unavailable")
    elif maximum_temperature > policy["maximum_gpu_temperature_c"]:
        reasons.append("GPU temperature exceeded policy maximum")
    optional_exact = {
        "machine": "expected_machine",
        "gpu": "expected_gpu",
        "gpu_persistence_mode": "required_gpu_persistence_mode",
        "gpu_compute_mode": "required_gpu_compute_mode",
    }
    for field, policy_field in optional_exact.items():
        if policy_field in policy and environment[field] != policy[policy_field]:
            reasons.append(f"{field}={environment[field]!r}, expected {policy[policy_field]!r}")
    numeric_minima = {"gpu_sm_clock_min_mhz": "minimum_gpu_sm_clock_mhz"}
    for field, policy_field in numeric_minima.items():
        if policy_field in policy and integer(environment, field) < policy[policy_field]:
            reasons.append(f"{field} is below policy minimum")
    if (
        "expected_gpu_power_limit_mw" in policy
        and integer(environment, "gpu_power_limit_mw") != policy["expected_gpu_power_limit_mw"]
    ):
        reasons.append("GPU enforced power limit does not match policy")
    return reasons


def host_manifest() -> dict:
    cpu_model = "unavailable"
    try:
        for line in Path("/proc/cpuinfo").read_text(encoding="utf-8").splitlines():
            if line.lower().startswith(("model name", "model")) and ":" in line:
                cpu_model = line.split(":", 1)[1].strip()
                break
    except OSError:
        pass
    return {
        "python": platform.python_version(),
        "kernel": platform.release(),
        "operating_system": platform.platform(),
        "cpu_model": cpu_model,
        "logical_cpus": os.cpu_count(),
    }


def benchmark_command(
    benchmark: Path, csv_path: Path, lanes: int, gpu_policy: str = "exclusive"
) -> list[str]:
    if gpu_policy not in {"exclusive", "contention"}:
        raise ProtocolError(f"unsupported GPU policy: {gpu_policy!r}")
    command = [
        str(benchmark),
        "-R", str(REPETITIONS),
        "-W", str(WARMUPS),
        "-N", str(lanes),
        "-P", ",".join(map(str, PRBS)),
        "-L", ",".join(map(str, LAYERS)),
        "-M", ",".join(map(str, MODULATIONS)),
        "-S", str(SYMBOLS),
        "-T", str(PREFLIGHT_TIMEOUT_MS),
        "-C", str(CE_DEADLINE_US),
        "-D", str(DEMODULATOR_DEADLINE_US),
        "-G",
    ]
    if gpu_policy == "contention":
        command.append("-X")
    command += ["-O", str(csv_path)]
    return command


def run(args: argparse.Namespace) -> tuple[dict, int]:
    benchmark = args.benchmark.resolve(strict=True)
    if not benchmark.is_file() or not os.access(benchmark, os.X_OK):
        raise ProtocolError(f"benchmark is not executable: {benchmark}")
    policy = load_policy(args.policy)
    if args.trials < MINIMUM_TRIALS:
        raise ProtocolError(f"at least {MINIMUM_TRIALS} repeated trials are required")
    lanes = tuple(dict.fromkeys(args.lane_counts))
    if not lanes or lanes[0] != 1 or any(value < 1 or value > 64 for value in lanes):
        raise ProtocolError("lane counts must begin with the one-lane baseline and stay in [1, 64]")
    args.output_dir.mkdir(parents=True, exist_ok=False, mode=0o700)

    report = {
        "SPDX-FileCopyrightText": "Copyright (C) 2026 DeepSig Inc.",
        "SPDX-License-Identifier": "BSD-3-Clause-Clear",
        "schema": REPORT_SCHEMA,
        "created_at": utc_now(),
        "status": "invalid",
        "policy": policy,
        "host_manifest": host_manifest(),
        "benchmark": {"path": str(benchmark), "sha256": sha256(benchmark)},
        "protocol": {
            "trials": args.trials,
            "lane_counts": list(lanes),
            "prbs": list(PRBS),
            "layers": list(LAYERS),
            "modulation_orders": list(MODULATIONS),
            "symbols": SYMBOLS,
            "repetitions": REPETITIONS,
            "warmups_per_phase": WARMUPS,
            "ce_deadline_us": CE_DEADLINE_US,
            "demodulator_deadline_us": DEMODULATOR_DEADLINE_US,
            "preflight_timeout_ms": PREFLIGHT_TIMEOUT_MS,
            "gpu_policy": "exclusive",
        },
        "runs": [],
    }
    expected_signature = None
    expected_environment_identity = None
    invalid = False
    failed = False

    for trial in range(1, args.trials + 1):
        for lane_count in lanes:
            stem = f"trial-{trial:02d}-lanes-{lane_count:02d}"
            csv_path = args.output_dir / f"{stem}.csv"
            log_path = args.output_dir / f"{stem}.log"
            command = benchmark_command(benchmark, csv_path, lane_count)
            started = utc_now()
            before = time.monotonic()
            timed_out = False
            try:
                completed = subprocess.run(
                    command,
                    text=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                    timeout=args.timeout_seconds,
                    check=False,
                )
                return_code = completed.returncode
                output = completed.stdout
            except subprocess.TimeoutExpired as error:
                timed_out = True
                return_code = 1
                output = error.stdout or ""
                if isinstance(output, bytes):
                    output = output.decode(errors="replace")
                output += f"\nqualification run timed out after {args.timeout_seconds}s\n"
            except OSError as error:
                return_code = 1
                output = f"qualification benchmark could not execute: {error}\n"
            log_path.write_text(output, encoding="utf-8")
            run_record = {
                "trial": trial,
                "pusch_lanes": lane_count,
                "started_at": started,
                "finished_at": utc_now(),
                "duration_seconds": round(time.monotonic() - before, 6),
                "command": shlex.join(command),
                "exit_code": return_code,
                "log": {"path": log_path.name, "sha256": sha256(log_path)},
                "status": "invalid",
                "reasons": [],
            }
            if timed_out:
                run_record["reasons"].append("benchmark timed out")
                invalid = True
            elif return_code not in (0, 2):
                run_record["reasons"].append(f"benchmark infrastructure exit {return_code}")
                invalid = True
            elif not csv_path.exists():
                if return_code == 2:
                    run_record["status"] = "failed"
                    run_record["reasons"].append("benchmark refused qualification before producing measurements")
                    failed = True
                else:
                    run_record["reasons"].append("successful benchmark produced no CSV")
                    invalid = True
            else:
                run_record["csv"] = {"path": csv_path.name, "sha256": sha256(csv_path)}
                try:
                    rows = load_csv(csv_path)
                    environment, signature, benchmark_reasons, summary = validate_rows(rows, lane_count)
                    policy_reasons = validate_policy(policy, environment)
                    reasons = sorted(set(benchmark_reasons + policy_reasons))
                    identity_fields = (
                        "host", "machine", "gpu", "build_hash", "build_mode", "cuda_device", "cuda_driver_version",
                        "cuda_runtime_version", "nvml_driver_version", "gpu_uuid", "gpu_pci_bus_id",
                    )
                    identity = tuple(environment[field] for field in identity_fields)
                    if expected_signature is None:
                        expected_signature = signature
                    elif signature != expected_signature:
                        raise ProtocolError("path/shape matrix changes between repeated runs")
                    if expected_environment_identity is None:
                        expected_environment_identity = identity
                    elif identity != expected_environment_identity:
                        raise ProtocolError("build or hardware identity changes between repeated runs")
                    benchmark_pass = not benchmark_reasons
                    if return_code == 0 and not benchmark_pass:
                        raise ProtocolError("benchmark returned success but report-side gate failed")
                    if return_code == 2 and benchmark_pass:
                        raise ProtocolError("benchmark returned gate failure but report-side gate passed")
                    recomputed_pass = not reasons
                    run_record.update(
                        status="passed" if recomputed_pass else "failed",
                        reasons=reasons,
                        environment=environment,
                        summary=summary,
                    )
                    failed |= not recomputed_pass
                except ProtocolError as error:
                    run_record["reasons"].append(str(error))
                    invalid = True
            report["runs"].append(run_record)

    report["finished_at"] = utc_now()
    report["summary"] = {
        "expected_runs": args.trials * len(lanes),
        "passed_runs": sum(item["status"] == "passed" for item in report["runs"]),
        "failed_runs": sum(item["status"] == "failed" for item in report["runs"]),
        "invalid_runs": sum(item["status"] == "invalid" for item in report["runs"]),
        "resident_accepted_invocations": sum(
            item.get("summary", {}).get("resident_accepted_invocations", 0) for item in report["runs"]
        ),
        "pipeline_deadline_misses": sum(
            item.get("summary", {}).get("pipeline_deadline_misses", 0) for item in report["runs"]
        ),
        "production_deadline_misses": sum(
            item.get("summary", {}).get("production_deadline_misses", 0) for item in report["runs"]
        ),
    }
    if invalid:
        report["status"] = "invalid"
        return report, 1
    if failed:
        report["status"] = "failed"
        return report, 2
    report["status"] = "passed"
    return report, 0


def parse_lane_counts(text: str) -> list[int]:
    try:
        return [int(item) for item in text.split(",") if item]
    except ValueError as error:
        raise argparse.ArgumentTypeError("lane counts must be comma-separated integers") from error


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--benchmark", type=Path, required=True)
    parser.add_argument("--policy", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--trials", type=int, default=MINIMUM_TRIALS)
    parser.add_argument("--lane-counts", type=parse_lane_counts, default=[1, 2])
    parser.add_argument("--timeout-seconds", type=int, default=1800)
    args = parser.parse_args()
    report_path = args.output_dir / "qualification-report.json"
    try:
        report, status = run(args)
    except (ProtocolError, OSError) as error:
        print(f"Class-A CUDA qualification protocol failed: {error}", file=sys.stderr)
        return 1
    report_path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"qualification_status={report['status']} report={report_path}")
    return status


if __name__ == "__main__":
    sys.exit(main())
