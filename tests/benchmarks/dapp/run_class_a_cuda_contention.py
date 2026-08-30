#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear

"""Collect Class-A CUDA tail evidence while a real DU sustains CUDA load."""

from __future__ import annotations

import argparse
import importlib.util
import json
import os
from pathlib import Path
import shlex
import signal
import subprocess
import sys
import time


REPORT_SCHEMA = "ocudu.class-a-cuda-contention-report.v1"
READY_SCHEMA = "ocudu.dapp.cuda-contention-load-ready.v1"
RESULT_SCHEMA = "ocudu.dapp.cuda-contention-load-result.v1"
REQUIRED_LOAD_INSTANCES = {"receiver", "srs_isac_process", "spectrum_process"}


def load_qualification_module():
    path = Path(__file__).with_name("run_class_a_cuda_qualification.py")
    spec = importlib.util.spec_from_file_location("ocudu_class_a_qualification", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load qualification protocol: {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


qualification = load_qualification_module()
ProtocolError = qualification.ProtocolError


def resolve_file(path: Path, executable: bool = False) -> Path:
    try:
        resolved = path.resolve(strict=True)
    except OSError as error:
        raise ProtocolError(f"required path is unavailable: {path}: {error}") from error
    if not resolved.is_file() or (executable and not os.access(resolved, os.X_OK)):
        qualifier = "executable" if executable else "file"
        raise ProtocolError(f"required {qualifier} is unavailable: {resolved}")
    return resolved


def metric(instance: dict, name: str) -> int:
    try:
        value = instance["metrics"][name]
    except (KeyError, TypeError) as error:
        raise ProtocolError(f"load-ready record is missing metric {name!r}") from error
    if not isinstance(value, int) or value < 0:
        raise ProtocolError(f"load-ready metric {name!r} is invalid: {value!r}")
    return value


def validate_ready(
    payload: dict,
    restart_cycles: int,
    expected_schema: str = READY_SCHEMA,
    expected_cell_count: int | None = None,
    expected_pusch_concurrency: int | None = None,
) -> set[int]:
    if payload.get("schema") != expected_schema:
        raise ProtocolError(f"unsupported load-ready schema: {payload.get('schema')!r}")
    if payload.get("SPDX-License-Identifier") != "BSD-3-Clause-Clear":
        raise ProtocolError("load-ready record has the wrong SPDX license")
    if payload.get("restart_cycles") != restart_cycles:
        raise ProtocolError("load-ready restart count does not match the requested campaign")
    du_pid = payload.get("du_pid")
    if not isinstance(du_pid, int) or du_pid <= 0:
        raise ProtocolError("load-ready DU PID is invalid")
    instances = payload.get("instances")
    if not isinstance(instances, dict) or set(instances) != REQUIRED_LOAD_INSTANCES:
        raise ProtocolError("load-ready record does not contain the exact contention instance set")
    if any(
        not isinstance(item, dict) or not isinstance(item.get("instance_id"), str) or not item["instance_id"]
        for item in instances.values()
    ):
        raise ProtocolError("load-ready instance identity is missing")

    profile = payload.get("load_profile")
    if not isinstance(profile, dict):
        raise ProtocolError("load-ready record is missing the load profile")
    cell_count = profile.get("cell_count")
    configured_concurrency = profile.get("configured_pusch_concurrency")
    if isinstance(cell_count, bool) or not isinstance(cell_count, int) or not 1 <= cell_count <= 32:
        raise ProtocolError("load-ready cell count is invalid")
    if (
        isinstance(configured_concurrency, bool)
        or not isinstance(configured_concurrency, int)
        or configured_concurrency < 0
    ):
        raise ProtocolError("load-ready configured PUSCH concurrency is invalid")
    if cell_count > 1 and configured_concurrency == 0:
        raise ProtocolError("multicell load must declare explicit PUSCH concurrency")
    if expected_cell_count is not None and cell_count != expected_cell_count:
        raise ProtocolError("load-ready cell count does not match the requested topology")
    if expected_pusch_concurrency is not None and configured_concurrency != expected_pusch_concurrency:
        raise ProtocolError("load-ready PUSCH concurrency does not match the requested topology")
    expected_cell_bitmap = (1 << cell_count) - 1
    for name, instance in instances.items():
        if metric(instance, "host.input.cell_bitmap") != expected_cell_bitmap:
            raise ProtocolError(f"CUDA contention instance lacks exact {cell_count}-cell coverage: {name}")

    if metric(instances["receiver"], "dapp.invoke.count") == 0:
        raise ProtocolError("resident Class-A receiver has no invocations")
    native_workers = metric(instances["receiver"], "host.native_workers")
    active_native_workers = metric(instances["receiver"], "host.native_workers_active")
    if configured_concurrency > 0 and native_workers != 2 * configured_concurrency:
        raise ProtocolError("resident Class-A receiver does not match configured PUSCH concurrency")
    if configured_concurrency > 0 and active_native_workers != configured_concurrency:
        raise ProtocolError("resident Class-A active workers do not match configured PUSCH concurrency")
    if native_workers == 0:
        raise ProtocolError("resident Class-A receiver has no bound native workers")
    if active_native_workers == 0 or active_native_workers > native_workers:
        raise ProtocolError("resident Class-A active-worker count is invalid")
    srs_process = instances["srs_isac_process"]
    if metric(srs_process, "class_c.copy.export.count") == 0 or metric(
        srs_process, "class_c.copy.export.bytes"
    ) == 0:
        raise ProtocolError("CUDA process SRS route has no device exports")
    if metric(srs_process, "class_c.copy.snapshot.count") != 0:
        raise ProtocolError("CUDA process SRS route used the host snapshot path")
    if metric(srs_process, "process.results_busy") == 0:
        raise ProtocolError("CUDA process SRS worker has no source-valid results")
    if metric(srs_process, "process.result_protocol_failures") != 0:
        raise ProtocolError("CUDA process SRS worker emitted an invalid result")
    if metric(srs_process, "process.exits") != restart_cycles:
        raise ProtocolError("CUDA process SRS exit count does not match the requested churn")
    if metric(srs_process, "process.successful_restarts") != restart_cycles:
        raise ProtocolError("CUDA process SRS restart count does not match the requested churn")
    process = instances["spectrum_process"]
    if metric(process, "class_c.copy.export.count") == 0 or metric(process, "class_c.copy.export.bytes") == 0:
        raise ProtocolError("CUDA process spectrum route has no device exports")
    if metric(process, "class_c.copy.snapshot.count") != 0:
        raise ProtocolError("CUDA process spectrum route used the host snapshot path")
    if metric(process, "process.results_accepted") == 0:
        raise ProtocolError("CUDA process spectrum worker has no accepted results")
    if metric(process, "process.exits") != restart_cycles:
        raise ProtocolError("CUDA process exit count does not match the requested churn")
    if metric(process, "process.successful_restarts") != restart_cycles:
        raise ProtocolError("CUDA process restart count does not match the requested churn")
    worker_pids = {metric(process, "process.pid"), metric(srs_process, "process.pid")}
    if 0 in worker_pids or du_pid in worker_pids or len(worker_pids) != 2:
        raise ProtocolError("CUDA process worker PIDs are invalid or aliased")
    return {du_pid, *worker_pids}


def validate_result(
    payload: dict,
    ready: dict,
    restart_cycles: int,
    expected_cell_count: int | None = None,
    expected_pusch_concurrency: int | None = None,
) -> None:
    if payload.get("load_profile") != ready.get("load_profile"):
        raise ProtocolError("contention load profile changed during measurement")
    if validate_ready(
        payload,
        restart_cycles,
        RESULT_SCHEMA,
        expected_cell_count,
        expected_pusch_concurrency,
    ) != validate_ready(
        ready,
        restart_cycles,
        READY_SCHEMA,
        expected_cell_count,
        expected_pusch_concurrency,
    ):
        raise ProtocolError("contention load PIDs changed during measurement")
    if payload["du_pid"] != ready["du_pid"]:
        raise ProtocolError("contention DU identity changed during measurement")
    before = ready["instances"]
    after = payload["instances"]
    for name in REQUIRED_LOAD_INSTANCES:
        if after[name]["instance_id"] != before[name]["instance_id"]:
            raise ProtocolError(f"contention instance identity changed during measurement: {name}")
    if metric(after["receiver"], "dapp.invoke.count") <= metric(before["receiver"], "dapp.invoke.count"):
        raise ProtocolError("resident Class-A receiver made no progress during measurement")
    for name in ("srs_isac_process", "spectrum_process"):
        if metric(after[name], "class_c.copy.export.count") <= metric(
            before[name], "class_c.copy.export.count"
        ):
            raise ProtocolError(f"CUDA process export made no progress during measurement: {name}")
    if metric(after["srs_isac_process"], "process.results_busy") <= metric(
        before["srs_isac_process"], "process.results_busy"
    ):
        raise ProtocolError("CUDA process SRS results made no progress during measurement")
    if metric(after["spectrum_process"], "process.results_accepted") <= metric(
        before["spectrum_process"], "process.results_accepted"
    ):
        raise ProtocolError("CUDA process spectrum results made no progress during measurement")


def parse_client_pids(text: str) -> set[int]:
    if text == "none":
        return set()
    try:
        result = {int(item) for item in text.split(";")}
    except ValueError as error:
        raise ProtocolError(f"invalid foreign GPU client PID list: {text!r}") from error
    if not result or any(pid <= 0 for pid in result):
        raise ProtocolError(f"invalid foreign GPU client PID list: {text!r}")
    return result


def validate_known_clients(environment: dict[str, str], expected_pids: set[int]) -> None:
    observed = parse_client_pids(environment["foreign_compute_pids"])
    count = qualification.integer(environment, "foreign_compute_processes")
    if count != len(observed):
        raise ProtocolError("foreign GPU client count and PID list disagree")
    if observed != expected_pids:
        raise ProtocolError(
            f"observed GPU contention clients {sorted(observed)} do not match the DU load {sorted(expected_pids)}"
        )


def load_command(
    args: argparse.Namespace, ready_path: Path, release_path: Path, result_path: Path
) -> list[str]:
    command = [
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
        "--cuda-class-a",
        "--cuda-class-c",
        "--cuda-contention",
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
        str(getattr(args, "channel_bandwidth_mhz", 20)),
        "--srs-period-ms",
        str(getattr(args, "srs_period_ms", 20)),
        "--time-scaling",
        str(getattr(args, "time_scaling", 10)),
    ]
    if getattr(args, "multicell_cuda", False):
        command.append("--multicell-cuda")
    return command


def wait_ready(process: subprocess.Popen, path: Path, timeout_seconds: int) -> dict:
    deadline = time.monotonic() + timeout_seconds
    while time.monotonic() < deadline:
        status = process.poll()
        if status is not None:
            raise ProtocolError(f"DU contention driver exited before readiness with status {status}")
        if path.exists():
            try:
                return json.loads(path.read_text(encoding="utf-8"))
            except (OSError, json.JSONDecodeError) as error:
                raise ProtocolError(f"cannot read atomic load-ready record: {error}") from error
        time.sleep(0.05)
    raise ProtocolError(f"DU contention driver was not ready within {timeout_seconds} seconds")


def stop_load(process: subprocess.Popen, release_path: Path, timeout_seconds: int) -> tuple[int, str | None]:
    if process.poll() is None:
        try:
            release_path.write_text("release\n", encoding="utf-8")
        except OSError as error:
            try:
                os.killpg(process.pid, signal.SIGTERM)
                process.wait(timeout=5)
            except (ProcessLookupError, subprocess.TimeoutExpired):
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                process.wait(timeout=5)
            exit_code = process.returncode if process.returncode is not None else 1
            return exit_code, f"cannot release DU contention driver: {error}"
    try:
        return process.wait(timeout=timeout_seconds), None
    except subprocess.TimeoutExpired:
        try:
            os.killpg(process.pid, signal.SIGTERM)
            process.wait(timeout=5)
        except (ProcessLookupError, subprocess.TimeoutExpired):
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.wait(timeout=5)
        return process.returncode if process.returncode is not None else 1, "DU contention driver shutdown timed out"


def validate_arguments(args: argparse.Namespace) -> tuple[int, ...]:
    if args.trials < qualification.MINIMUM_TRIALS:
        raise ProtocolError(f"at least {qualification.MINIMUM_TRIALS} repeated trials are required")
    lanes = tuple(dict.fromkeys(args.lane_counts))
    if not lanes or lanes[0] != 1 or any(value < 1 or value > 64 for value in lanes):
        raise ProtocolError("lane counts must begin with the one-lane baseline and stay in [1, 64]")
    if not 1 <= args.restart_cycles <= 100:
        raise ProtocolError("restart cycles must be in [1, 100]")
    for name in (
        "ready_timeout_seconds",
        "load_hold_timeout_seconds",
        "load_shutdown_timeout_seconds",
        "benchmark_timeout_seconds",
    ):
        if not 1 <= getattr(args, name) <= 86400:
            raise ProtocolError(f"{name.replace('_', '-')} must be in [1, 86400]")
    return lanes


def run(args: argparse.Namespace) -> tuple[dict, int]:
    args.benchmark = resolve_file(args.benchmark, executable=True)
    args.load_driver = resolve_file(args.load_driver)
    args.odu = resolve_file(args.odu, executable=True)
    args.client = resolve_file(args.client)
    args.e3_schema = resolve_file(args.e3_schema)
    args.preflight_helper = resolve_file(args.preflight_helper, executable=True)
    args.catalog = args.catalog.resolve(strict=True)
    if not args.catalog.is_dir():
        raise ProtocolError(f"dApp catalog is not a directory: {args.catalog}")
    policy = qualification.load_policy(args.policy)
    lanes = validate_arguments(args)
    args.output_dir = args.output_dir.resolve()
    args.output_dir.mkdir(parents=True, exist_ok=False, mode=0o700)

    ready_path = args.output_dir / "load-ready.json"
    release_path = args.output_dir / "load-release"
    result_path = args.output_dir / "load-result.json"
    load_log_path = args.output_dir / "load-driver.log"
    command = load_command(args, ready_path, release_path, result_path)
    runner_path = Path(__file__).resolve(strict=True)
    qualification_path = runner_path.with_name("run_class_a_cuda_qualification.py")
    report = {
        "SPDX-FileCopyrightText": "Copyright (C) 2026 DeepSig Inc.",
        "SPDX-License-Identifier": "BSD-3-Clause-Clear",
        "schema": REPORT_SCHEMA,
        "created_at": qualification.utc_now(),
        "status": "invalid",
        "policy": policy,
        "host_manifest": qualification.host_manifest(),
        "tooling": {
            "runner": {"path": str(runner_path), "sha256": qualification.sha256(runner_path)},
            "qualification_protocol": {
                "path": str(qualification_path),
                "sha256": qualification.sha256(qualification_path),
            },
        },
        "benchmark": {"path": str(args.benchmark), "sha256": qualification.sha256(args.benchmark)},
        "load": {
            "driver": {"path": str(args.load_driver), "sha256": qualification.sha256(args.load_driver)},
            "command": shlex.join(command),
            "status": "invalid",
            "reasons": [],
        },
        "protocol": {
            "cell_count": 2 if getattr(args, "multicell_cuda", False) else 1,
            "configured_pusch_concurrency": 2 if getattr(args, "multicell_cuda", False) else 0,
            "trials": args.trials,
            "lane_counts": list(lanes),
            "prbs": list(qualification.PRBS),
            "layers": list(qualification.LAYERS),
            "modulation_orders": list(qualification.MODULATIONS),
            "symbols": qualification.SYMBOLS,
            "repetitions": qualification.REPETITIONS,
            "warmups_per_phase": qualification.WARMUPS,
            "ce_deadline_us": qualification.CE_DEADLINE_US,
            "demodulator_deadline_us": qualification.DEMODULATOR_DEADLINE_US,
            "preflight_timeout_ms": qualification.PREFLIGHT_TIMEOUT_MS,
            "gpu_policy": "contention",
            "restart_cycles_before_measurement": args.restart_cycles,
        },
        "runs": [],
    }
    invalid = False
    failed = False
    expected_signature = None
    expected_identity = None
    expected_load_pids: set[int] = set()
    load_process = None
    ready = None
    expected_cell_count = 2 if getattr(args, "multicell_cuda", False) else 1
    expected_pusch_concurrency = 2 if getattr(args, "multicell_cuda", False) else 0

    with load_log_path.open("w", encoding="utf-8") as load_log:
        try:
            load_process = subprocess.Popen(
                command,
                stdout=load_log,
                stderr=subprocess.STDOUT,
                text=True,
                start_new_session=True,
            )
            ready = wait_ready(load_process, ready_path, args.ready_timeout_seconds)
            expected_load_pids = validate_ready(
                ready, args.restart_cycles, READY_SCHEMA, expected_cell_count, expected_pusch_concurrency
            )
            report["load"].update(
                status="running",
                ready={"path": ready_path.name, "sha256": qualification.sha256(ready_path)},
                expected_gpu_client_pids=sorted(expected_load_pids),
            )

            for trial in range(1, args.trials + 1):
                for lane_count in lanes:
                    stem = f"trial-{trial:02d}-lanes-{lane_count:02d}"
                    csv_path = args.output_dir / f"{stem}.csv"
                    log_path = args.output_dir / f"{stem}.log"
                    benchmark = qualification.benchmark_command(
                        args.benchmark, csv_path, lane_count, gpu_policy="contention"
                    )
                    started = qualification.utc_now()
                    before = time.monotonic()
                    timed_out = False
                    if load_process.poll() is not None:
                        return_code = 1
                        output = f"DU contention driver exited with status {load_process.returncode}\n"
                    else:
                        try:
                            completed = subprocess.run(
                                benchmark,
                                text=True,
                                stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT,
                                timeout=args.benchmark_timeout_seconds,
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
                            output += f"\ncontention benchmark timed out after {args.benchmark_timeout_seconds}s\n"
                        except OSError as error:
                            return_code = 1
                            output = f"contention benchmark could not execute: {error}\n"
                    log_path.write_text(output, encoding="utf-8")
                    record = {
                        "trial": trial,
                        "pusch_lanes": lane_count,
                        "started_at": started,
                        "finished_at": qualification.utc_now(),
                        "duration_seconds": round(time.monotonic() - before, 6),
                        "command": shlex.join(benchmark),
                        "exit_code": return_code,
                        "log": {"path": log_path.name, "sha256": qualification.sha256(log_path)},
                        "status": "invalid",
                        "reasons": [],
                    }
                    if load_process.poll() is not None:
                        record["reasons"].append("DU contention load stopped during measurement")
                        invalid = True
                    elif timed_out:
                        record["reasons"].append("benchmark timed out")
                        invalid = True
                    elif return_code not in (0, 2):
                        record["reasons"].append(f"benchmark infrastructure exit {return_code}")
                        invalid = True
                    elif not csv_path.exists():
                        if return_code == 2:
                            record["status"] = "failed"
                            record["reasons"].append("benchmark refused before producing measurements")
                            failed = True
                        else:
                            record["reasons"].append("successful benchmark produced no CSV")
                            invalid = True
                    else:
                        record["csv"] = {"path": csv_path.name, "sha256": qualification.sha256(csv_path)}
                        try:
                            rows = qualification.load_csv(csv_path)
                            environment, signature, benchmark_reasons, summary = qualification.validate_rows(
                                rows, lane_count, gpu_policy="contention"
                            )
                            validate_known_clients(environment, expected_load_pids)
                            policy_reasons = qualification.validate_policy(policy, environment)
                            reasons = sorted(set(benchmark_reasons + policy_reasons))
                            identity_fields = (
                                "host",
                                "machine",
                                "gpu",
                                "build_hash",
                                "build_mode",
                                "cuda_device",
                                "cuda_driver_version",
                                "cuda_runtime_version",
                                "nvml_driver_version",
                                "gpu_uuid",
                                "gpu_pci_bus_id",
                            )
                            identity = tuple(environment[field] for field in identity_fields)
                            if expected_signature is None:
                                expected_signature = signature
                            elif signature != expected_signature:
                                raise ProtocolError("path/shape matrix changes between repeated runs")
                            if expected_identity is None:
                                expected_identity = identity
                            elif identity != expected_identity:
                                raise ProtocolError("build or hardware identity changes between repeated runs")
                            benchmark_pass = not benchmark_reasons
                            if return_code == 0 and not benchmark_pass:
                                raise ProtocolError("benchmark returned success but report-side gate failed")
                            if return_code == 2 and benchmark_pass:
                                raise ProtocolError("benchmark returned gate failure but report-side gate passed")
                            record.update(
                                status="passed" if not reasons else "failed",
                                reasons=reasons,
                                environment=environment,
                                summary=summary,
                            )
                            failed |= bool(reasons)
                        except ProtocolError as error:
                            record["reasons"].append(str(error))
                            invalid = True
                    report["runs"].append(record)
        except (OSError, ProtocolError) as error:
            report["load"]["reasons"].append(str(error))
            invalid = True
        finally:
            if load_process is not None:
                exit_code, stop_error = stop_load(
                    load_process, release_path, args.load_shutdown_timeout_seconds
                )
                report["load"]["exit_code"] = exit_code
                if stop_error is not None:
                    report["load"]["reasons"].append(stop_error)
                    invalid = True
                if exit_code != 0:
                    report["load"]["reasons"].append(f"DU contention driver exited with status {exit_code}")
                    invalid = True
                elif ready is None:
                    report["load"]["reasons"].append("DU contention driver never published readiness")
                    invalid = True
                elif not result_path.exists():
                    report["load"]["reasons"].append("DU contention driver produced no final load record")
                    invalid = True
                else:
                    try:
                        result = json.loads(result_path.read_text(encoding="utf-8"))
                        validate_result(
                            result,
                            ready,
                            args.restart_cycles,
                            expected_cell_count,
                            expected_pusch_concurrency,
                        )
                        report["load"]["result"] = {
                            "path": result_path.name,
                            "sha256": qualification.sha256(result_path),
                        }
                    except (OSError, json.JSONDecodeError, ProtocolError) as error:
                        report["load"]["reasons"].append(f"invalid final load record: {error}")
                        invalid = True
                if not report["load"]["reasons"]:
                    report["load"]["status"] = "passed"

    report["load"]["log"] = {"path": load_log_path.name, "sha256": qualification.sha256(load_log_path)}
    expected_runs = args.trials * len(lanes)
    if len(report["runs"]) != expected_runs:
        report["load"]["reasons"].append(
            f"campaign produced {len(report['runs'])} of {expected_runs} expected benchmark runs"
        )
        invalid = True
    report["finished_at"] = qualification.utc_now()
    report["summary"] = {
        "expected_runs": expected_runs,
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


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--benchmark", type=Path, required=True)
    parser.add_argument("--load-driver", type=Path, required=True)
    parser.add_argument("--odu", type=Path, required=True)
    parser.add_argument("--client", type=Path, required=True)
    parser.add_argument("--e3-schema", type=Path, required=True)
    parser.add_argument("--catalog", type=Path, required=True)
    parser.add_argument("--preflight-helper", type=Path, required=True)
    parser.add_argument("--policy", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--trials", type=int, default=qualification.MINIMUM_TRIALS)
    parser.add_argument("--lane-counts", type=qualification.parse_lane_counts, default=[1, 2])
    parser.add_argument("--restart-cycles", type=int, default=3)
    parser.add_argument("--multicell-cuda", action="store_true")
    parser.add_argument("--ready-timeout-seconds", type=int, default=120)
    parser.add_argument("--load-hold-timeout-seconds", type=int, default=7200)
    parser.add_argument("--load-shutdown-timeout-seconds", type=int, default=180)
    parser.add_argument("--benchmark-timeout-seconds", type=int, default=1800)
    args = parser.parse_args()
    report_path = args.output_dir / "contention-report.json"
    try:
        report, status = run(args)
    except (OSError, ProtocolError) as error:
        print(f"Class-A CUDA contention protocol failed: {error}", file=sys.stderr)
        return 1
    report_path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"contention_status={report['status']} report={report_path}")
    return status


if __name__ == "__main__":
    sys.exit(main())
