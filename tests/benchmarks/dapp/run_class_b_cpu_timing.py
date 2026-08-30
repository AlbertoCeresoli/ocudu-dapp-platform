#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear

"""Run the fixed OCUDU Class-B CPU feature-to-commit timing qualification."""

from __future__ import annotations

import argparse
import csv
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import shlex
import subprocess
import sys
import time


POLICY_SCHEMA = "ocudu.dapp.class-b-cpu-timing-policy.v1"
REPORT_SCHEMA = "ocudu.dapp.class-b-cpu-timing-report.v1"
SAMPLE_SCHEMA = "ocudu.dapp.class-b-cpu-sample.v1"
DEADLINE_NS = 100_000
REPETITIONS = 1000
WARMUPS = 100

PROFILES = {
    "dl_20mhz_c16": ("dl", 16, 51, False),
    "dl_100mhz_c1": ("dl", 1, 273, False),
    "dl_100mhz_c16": ("dl", 16, 273, False),
    "dl_100mhz_c64": ("dl", 64, 273, False),
    "ul_20mhz_c16_context": ("ul", 16, 51, True),
    "ul_100mhz_c1": ("ul", 1, 273, False),
    "ul_100mhz_c16": ("ul", 16, 273, False),
    "ul_100mhz_c64": ("ul", 64, 273, False),
    "ul_100mhz_c64_context": ("ul", 64, 273, True),
}

REQUIRED_COLUMNS = {
    "schema",
    "scenario",
    "sample",
    "direction",
    "candidates",
    "prbs",
    "interference",
    "avoidance_output",
    "conventional_ns",
    "feature_ns",
    "dapp_ns",
    "validation_ns",
    "commit_ns",
    "total_ns",
    "outcome",
    "deadline_miss",
}
PHASES = ("conventional", "feature", "dapp", "validation", "commit", "total", "combined")
OUTCOMES = {"accepted", "evaluation_accepted", "fallback", "validation_rejection"}


class ProtocolError(RuntimeError):
    """Qualification evidence is structurally invalid."""


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds").replace("+00:00", "Z")


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def artifact(path: Path) -> dict:
    return {"path": str(path.resolve()), "sha256": sha256(path)}


def integer(row: dict[str, str], field: str) -> int:
    try:
        value = int(row[field])
    except (KeyError, ValueError) as error:
        raise ProtocolError(f"invalid integer field {field!r}: {row.get(field)!r}") from error
    if value < 0:
        raise ProtocolError(f"negative integer field {field!r}")
    return value


def affinity_text(cpus: set[int]) -> str:
    if not cpus:
        return ""
    ranges = []
    start = previous = min(cpus)
    for cpu in sorted(cpus)[1:]:
        if cpu == previous + 1:
            previous = cpu
            continue
        ranges.append(str(start) if start == previous else f"{start}-{previous}")
        start = previous = cpu
    ranges.append(str(start) if start == previous else f"{start}-{previous}")
    return ",".join(ranges)


def scheduler_name(value: int) -> str:
    names = {
        getattr(os, "SCHED_FIFO", 1): "fifo",
        getattr(os, "SCHED_RR", 2): "round-robin",
        getattr(os, "SCHED_BATCH", 3): "batch",
        getattr(os, "SCHED_IDLE", 5): "idle",
        getattr(os, "SCHED_OTHER", 0): "other",
    }
    return names.get(value, f"unknown-{value}")


def host_manifest() -> dict:
    governors = {}
    for path in sorted(Path("/sys/devices/system/cpu").glob("cpu[0-9]*/cpufreq/scaling_governor")):
        try:
            governors[path.parts[-3]] = path.read_text(encoding="utf-8").strip()
        except OSError:
            governors[path.parts[-3]] = "unreadable"
    cpuinfo = Path("/proc/cpuinfo")
    return {
        "hostname": platform.node(),
        "machine": platform.machine(),
        "kernel": platform.release(),
        "python": platform.python_version(),
        "scheduler_policy": scheduler_name(os.sched_getscheduler(0)),
        "scheduler_priority": os.sched_getparam(0).sched_priority,
        "cpu_affinity": affinity_text(set(os.sched_getaffinity(0))),
        "cpu_governors": governors,
        "cpuinfo_sha256": sha256(cpuinfo) if cpuinfo.exists() else "unavailable",
    }


def load_policy(path: Path) -> dict:
    try:
        policy = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ProtocolError(f"cannot read policy {path}: {error}") from error
    required = {
        "SPDX-FileCopyrightText",
        "SPDX-License-Identifier",
        "schema",
        "platform_id",
        "deadline_ns",
        "repetitions",
        "warmups",
        "maximum_p999_ns",
        "maximum_deadline_miss_rate",
        "maximum_fallback_rate",
        "maximum_validation_rejection_rate",
        "required_scheduler_policy",
        "minimum_scheduler_priority",
        "required_cpu_affinity",
        "cpu_frequency_policy",
        "release_controls_approved",
        "controls_rationale",
    }
    optional = {"operator_notes"}
    if missing := sorted(required - policy.keys()):
        raise ProtocolError(f"policy is missing fields: {', '.join(missing)}")
    if unknown := sorted(policy.keys() - required - optional):
        raise ProtocolError(f"policy has unknown fields: {', '.join(unknown)}")
    fixed = {
        "SPDX-License-Identifier": "BSD-3-Clause-Clear",
        "schema": POLICY_SCHEMA,
        "deadline_ns": DEADLINE_NS,
        "repetitions": REPETITIONS,
        "warmups": WARMUPS,
        "maximum_p999_ns": DEADLINE_NS,
        "maximum_deadline_miss_rate": 0,
        "maximum_fallback_rate": 0,
        "maximum_validation_rejection_rate": 0,
    }
    for field, expected in fixed.items():
        if policy[field] != expected:
            raise ProtocolError(f"policy field {field!r} must be {expected!r}")
    for field in ("SPDX-FileCopyrightText", "platform_id", "required_cpu_affinity", "cpu_frequency_policy", "controls_rationale"):
        if not isinstance(policy[field], str) or not policy[field]:
            raise ProtocolError(f"policy field {field!r} must be a nonempty string")
    if policy["required_scheduler_policy"] not in {"fifo", "round-robin", "batch", "idle", "other"}:
        raise ProtocolError("required_scheduler_policy is not recognized")
    if not isinstance(policy["minimum_scheduler_priority"], int) or policy["minimum_scheduler_priority"] < 0:
        raise ProtocolError("minimum_scheduler_priority must be a nonnegative integer")
    if not isinstance(policy["release_controls_approved"], bool):
        raise ProtocolError("release_controls_approved must be a boolean")
    return policy


def verify_module(reference_dir: Path) -> tuple[Path, Path, dict]:
    manifest_path = reference_dir / "ref_scheduler_cpu.manifest.json"
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ProtocolError(f"cannot read scheduler manifest: {error}") from error
    if manifest.get("global_name") != "org.ocudu.reference.scheduler.cpu":
        raise ProtocolError("scheduler manifest has the wrong global name")
    artifact_path = reference_dir / str(manifest.get("artifact", ""))
    if not artifact_path.is_file() or manifest.get("sha256") != sha256(artifact_path):
        raise ProtocolError("scheduler artifact does not match its manifest digest")
    return manifest_path, artifact_path, manifest


def load_csv(path: Path) -> list[dict[str, str]]:
    try:
        with path.open(newline="", encoding="utf-8") as source:
            reader = csv.DictReader(source)
            if reader.fieldnames is None or set(reader.fieldnames) != REQUIRED_COLUMNS or len(reader.fieldnames) != len(REQUIRED_COLUMNS):
                raise ProtocolError("raw CSV header does not exactly match the v1 sample schema")
            rows = list(reader)
    except OSError as error:
        raise ProtocolError(f"cannot read raw CSV: {error}") from error
    if len(rows) != len(PROFILES) * REPETITIONS:
        raise ProtocolError(f"raw CSV has {len(rows)} rows; expected {len(PROFILES) * REPETITIONS}")
    return rows


def percentile(values: list[int], fraction: float) -> int:
    ordered = sorted(values)
    return ordered[max(0, math.ceil(fraction * len(ordered)) - 1)]


def distribution(values: list[int]) -> dict:
    return {
        "min": min(values),
        "p50": percentile(values, 0.50),
        "p90": percentile(values, 0.90),
        "p99": percentile(values, 0.99),
        "p999": percentile(values, 0.999),
        "max": max(values),
    }


def analyze_rows(rows: list[dict[str, str]], policy: dict) -> tuple[list[dict], dict, list[str]]:
    grouped: dict[str, list[dict[str, str]]] = {}
    for row in rows:
        if row.get("schema") != SAMPLE_SCHEMA or row.get("scenario") not in PROFILES:
            raise ProtocolError("raw CSV contains an unknown schema or profile")
        grouped.setdefault(row["scenario"], []).append(row)
    if set(grouped) != set(PROFILES):
        raise ProtocolError("raw CSV does not contain the exact fixed profile matrix")

    profiles = []
    all_reasons = []
    total_misses = total_fallbacks = total_rejections = 0
    for name in PROFILES:
        direction, candidates, prbs, interference = PROFILES[name]
        profile_rows = grouped[name]
        if len(profile_rows) != REPETITIONS or {integer(row, "sample") for row in profile_rows} != set(range(REPETITIONS)):
            raise ProtocolError(f"profile {name} has missing, duplicate, or excess samples")
        phase_values = {phase: [] for phase in PHASES}
        outcomes = {outcome: 0 for outcome in sorted(OUTCOMES)}
        misses = avoidance_outputs = 0
        for row in profile_rows:
            expected = {
                "direction": direction,
                "candidates": candidates,
                "prbs": prbs,
                "interference": int(interference),
            }
            for field, value in expected.items():
                observed = row[field] if isinstance(value, str) else integer(row, field)
                if observed != value:
                    raise ProtocolError(f"profile {name} changes fixed field {field!r}")
            if row["outcome"] not in OUTCOMES:
                raise ProtocolError(f"profile {name} has unknown outcome {row['outcome']!r}")
            outcomes[row["outcome"]] += 1
            miss = integer(row, "deadline_miss")
            avoidance = integer(row, "avoidance_output")
            if miss not in (0, 1) or avoidance not in (0, 1):
                raise ProtocolError("boolean sample fields must be zero or one")
            misses += miss
            avoidance_outputs += avoidance
            values = {phase: integer(row, f"{phase}_ns") for phase in PHASES if phase not in {"combined"}}
            values["combined"] = values["conventional"] + values["total"]
            if values["total"] < sum(values[phase] for phase in ("feature", "dapp", "validation", "commit")):
                raise ProtocolError(f"profile {name} has total time smaller than its measured phases")
            if values["total"] > DEADLINE_NS and miss == 0:
                raise ProtocolError(f"profile {name} hides a complete-boundary deadline miss")
            for phase, value in values.items():
                phase_values[phase].append(value)
        if interference and outcomes["accepted"] == REPETITIONS and avoidance_outputs != REPETITIONS:
            raise ProtocolError(f"profile {name} did not exercise PRB-mask output on every accepted sample")
        if not interference and avoidance_outputs != 0:
            raise ProtocolError(f"profile {name} emitted avoidance output without interference context")

        reasons = []
        miss_rate = misses / REPETITIONS
        fallback_rate = outcomes["fallback"] / REPETITIONS
        rejection_rate = outcomes["validation_rejection"] / REPETITIONS
        phase_summary = {phase: distribution(values) for phase, values in phase_values.items()}
        if phase_summary["total"]["p999"] > policy["maximum_p999_ns"]:
            reasons.append(f"total P99.9 {phase_summary['total']['p999']} ns exceeds {policy['maximum_p999_ns']} ns")
        if miss_rate > policy["maximum_deadline_miss_rate"]:
            reasons.append(f"deadline miss rate {miss_rate:.9f} exceeds policy")
        if fallback_rate > policy["maximum_fallback_rate"]:
            reasons.append(f"fallback rate {fallback_rate:.9f} exceeds policy")
        if rejection_rate > policy["maximum_validation_rejection_rate"]:
            reasons.append(f"validation rejection rate {rejection_rate:.9f} exceeds policy")
        if outcomes["evaluation_accepted"] != 0:
            reasons.append("authoritative qualification contains evaluation-only outputs")
        profiles.append(
            {
                "name": name,
                "direction": direction,
                "candidates": candidates,
                "prbs": prbs,
                "interference": interference,
                "samples": REPETITIONS,
                "avoidance_outputs": avoidance_outputs,
                "outcomes": outcomes,
                "phases": phase_summary,
                "deadline_miss_rate": miss_rate,
                "reasons": reasons,
            }
        )
        all_reasons.extend(f"{name}: {reason}" for reason in reasons)
        total_misses += misses
        total_fallbacks += outcomes["fallback"]
        total_rejections += outcomes["validation_rejection"]

    summary = {
        "rows": len(rows),
        "deadline_misses": total_misses,
        "fallbacks": total_fallbacks,
        "validation_rejections": total_rejections,
        "worst_total_p999_ns": max(item["phases"]["total"]["p999"] for item in profiles),
        "reasons": all_reasons,
    }
    return profiles, summary, all_reasons


def control_reasons(policy: dict, host: dict) -> list[str]:
    reasons = []
    if host["scheduler_policy"] != policy["required_scheduler_policy"]:
        reasons.append(
            f"scheduler policy {host['scheduler_policy']} does not match {policy['required_scheduler_policy']}"
        )
    if host["scheduler_priority"] < policy["minimum_scheduler_priority"]:
        reasons.append("scheduler priority is below the policy minimum")
    if host["cpu_affinity"] != policy["required_cpu_affinity"]:
        reasons.append(f"CPU affinity {host['cpu_affinity']} does not match {policy['required_cpu_affinity']}")
    if not policy["release_controls_approved"]:
        reasons.append("release controls are not operator-approved")
    return reasons


def run(args) -> tuple[dict, int]:
    for path in (args.benchmark, args.policy, args.policy_schema, args.report_schema):
        if not path.is_file():
            raise ProtocolError(f"required artifact is missing: {path}")
    policy = load_policy(args.policy)
    manifest_path, module_path, _ = verify_module(args.reference_dir)
    args.output_dir.mkdir(parents=True, exist_ok=True)
    raw_path = args.output_dir / "class-b-cpu-raw.csv"
    log_path = args.output_dir / "benchmark.log"
    command = [
        str(args.benchmark.resolve()),
        "-o",
        str(raw_path.resolve()),
        "-R",
        str(REPETITIONS),
        "-W",
        str(WARMUPS),
        "-D",
        str(DEADLINE_NS // 1000),
    ]
    created_at = utc_now()
    started = time.monotonic()
    try:
        completed = subprocess.run(
            command,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=args.timeout_seconds,
            check=False,
        )
        exit_code = completed.returncode
        output = completed.stdout
    except subprocess.TimeoutExpired as error:
        exit_code = 124
        output = error.stdout or ""
        if isinstance(output, bytes):
            output = output.decode(errors="replace")
        output += f"\nbenchmark timed out after {args.timeout_seconds}s\n"
    except OSError as error:
        exit_code = 126
        output = f"benchmark execution failed: {error}\n"
    log_path.write_text(output, encoding="utf-8")
    host = host_manifest()
    report = {
        "SPDX-FileCopyrightText": "Copyright (C) 2026 DeepSig Inc.",
        "SPDX-License-Identifier": "BSD-3-Clause-Clear",
        "schema": REPORT_SCHEMA,
        "created_at": created_at,
        "finished_at": utc_now(),
        "status": "invalid",
        "policy_artifact": artifact(args.policy),
        "policy": policy,
        "host_manifest": host,
        "tooling": {
            "runner": artifact(Path(__file__)),
            "benchmark": artifact(args.benchmark),
            "policy_schema": artifact(args.policy_schema),
            "report_schema": artifact(args.report_schema),
            "module_manifest": artifact(manifest_path),
            "module_artifact": artifact(module_path),
        },
        "run": {
            "command": shlex.join(command),
            "exit_code": exit_code,
            "duration_seconds": round(time.monotonic() - started, 6),
            "raw_csv": artifact(raw_path) if raw_path.is_file() else {"path": str(raw_path.resolve()), "sha256": "0" * 64},
            "log": artifact(log_path),
        },
        "protocol": {
            "deadline_ns": DEADLINE_NS,
            "repetitions": REPETITIONS,
            "warmups": WARMUPS,
            "expected_profiles": list(PROFILES),
            "expected_rows": len(PROFILES) * REPETITIONS,
        },
        "profiles": [],
        "summary": {
            "rows": 0,
            "deadline_misses": 0,
            "fallbacks": 0,
            "validation_rejections": 0,
            "worst_total_p999_ns": 0,
            "reasons": [],
        },
    }
    if exit_code != 0 or not raw_path.is_file():
        report["summary"]["reasons"] = [f"benchmark infrastructure exit {exit_code}"]
        return report, 1
    try:
        profiles, summary, reasons = analyze_rows(load_csv(raw_path), policy)
    except ProtocolError as error:
        report["summary"]["reasons"] = [str(error)]
        return report, 1
    reasons.extend(control_reasons(policy, host))
    summary["reasons"] = reasons
    report["profiles"] = profiles
    report["summary"] = summary
    if reasons:
        report["status"] = "failed"
        return report, 2
    report["status"] = "passed"
    return report, 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--benchmark", type=Path, required=True)
    parser.add_argument("--reference-dir", type=Path, required=True)
    parser.add_argument("--policy", type=Path, required=True)
    parser.add_argument("--policy-schema", type=Path, required=True)
    parser.add_argument("--report-schema", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--timeout-seconds", type=int, default=900)
    args = parser.parse_args()
    report_path = args.output_dir / "timing-report.json"
    try:
        report, status = run(args)
    except (ProtocolError, OSError) as error:
        print(f"Class-B CPU timing protocol failed: {error}", file=sys.stderr)
        return 1
    report_path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"class_b_timing_status={report['status']} report={report_path}")
    return status


if __name__ == "__main__":
    sys.exit(main())
