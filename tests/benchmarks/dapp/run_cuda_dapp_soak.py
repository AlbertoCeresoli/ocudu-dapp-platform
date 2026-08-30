#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear

"""Run a bounded real-DU CUDA dApp endurance and resource-growth campaign."""

from __future__ import annotations

import argparse
import importlib.util
import json
import os
from pathlib import Path
import shutil
import shlex
import subprocess
import sys
import time


REPORT_SCHEMA = "ocudu.dapp.cuda-loaded-l1-soak-report.v1"
CATALOG_MANIFESTS = (
    "ref_receiver_cuda.manifest.json",
    "ref_srs_isac_cuda.manifest.json",
    "ref_spectrumd_cuda.manifest.json",
    "ref_srs_isacd_cuda.manifest.json",
)


def load_module(filename: str, name: str):
    path = Path(__file__).with_name(filename)
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load protocol module: {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


contention = load_module("run_class_a_cuda_contention.py", "ocudu_cuda_contention")
qualification = contention.qualification
ProtocolError = qualification.ProtocolError


def artifact(path: Path) -> dict:
    resolved = contention.resolve_file(path)
    return {"path": str(resolved), "sha256": qualification.sha256(resolved)}


def catalog_provenance(catalog: Path) -> tuple[list[dict], dict[str, Path]]:
    records = []
    workers = {}
    for filename in CATALOG_MANIFESTS:
        manifest_path = catalog / filename
        try:
            manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
            artifact_path = (catalog / manifest["artifact"]).resolve(strict=True)
            declared_hash = manifest["sha256"]
            package_id = manifest["package_id"]
            global_name = manifest["global_name"]
        except (OSError, KeyError, json.JSONDecodeError) as error:
            raise ProtocolError(f"cannot read soak package manifest {manifest_path}: {error}") from error
        actual_hash = qualification.sha256(artifact_path)
        if actual_hash != declared_hash:
            raise ProtocolError(f"soak package hash mismatch: {artifact_path}")
        records.append(
            {
                "manifest": artifact(manifest_path),
                "package_id": package_id,
                "global_name": global_name,
                "artifact": {"path": str(artifact_path), "sha256": actual_hash},
            }
        )
        if filename == "ref_spectrumd_cuda.manifest.json":
            workers["spectrum_worker"] = artifact_path
        elif filename == "ref_srs_isacd_cuda.manifest.json":
            workers["srs_worker"] = artifact_path
    if set(workers) != {"spectrum_worker", "srs_worker"}:
        raise ProtocolError("CUDA Class-C workers are absent from the soak catalog")
    return records, workers


def status_fields(pid: int) -> dict[str, int]:
    try:
        lines = Path(f"/proc/{pid}/status").read_text(encoding="utf-8").splitlines()
        values = {line.split(":", 1)[0]: line.split(":", 1)[1].strip() for line in lines if ":" in line}
        rss_bytes = int(values["VmRSS"].split()[0]) * 1024
        threads = int(values["Threads"])
        fds = sum(1 for _ in Path(f"/proc/{pid}/fd").iterdir())
    except (OSError, KeyError, ValueError) as error:
        raise ProtocolError(f"cannot sample process {pid}: {error}") from error
    return {"rss_bytes": rss_bytes, "threads": threads, "file_descriptors": fds}


def nvidia_query(nvidia_smi: Path, fields: str) -> list[str]:
    try:
        completed = subprocess.run(
            [
                str(nvidia_smi),
                fields,
                "--format=csv,noheader,nounits",
            ],
            text=True,
            capture_output=True,
            timeout=5,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        raise ProtocolError(f"cannot query NVIDIA device state: {error}") from error
    if completed.returncode != 0:
        raise ProtocolError(f"NVIDIA device-state query failed: {completed.stderr.strip()}")
    return completed.stdout.splitlines()


def gpu_inventory(nvidia_smi: Path) -> list[dict[str, str]]:
    rows = nvidia_query(nvidia_smi, "--query-gpu=index,name,uuid,pci.bus_id,driver_version")
    inventory = []
    for line in rows:
        fields = [field.strip() for field in line.split(",")]
        if len(fields) != 5 or any(not field for field in fields):
            raise ProtocolError(f"invalid NVIDIA GPU inventory row: {line!r}")
        inventory.append(dict(zip(("index", "name", "uuid", "pci_bus_id", "driver_version"), fields)))
    if not inventory:
        raise ProtocolError("NVIDIA GPU inventory is empty")
    return inventory


def gpu_memory(nvidia_smi: Path, pids: set[int]) -> tuple[dict[int, int], dict[int, set[str]]]:
    rows = nvidia_query(nvidia_smi, "--query-compute-apps=gpu_uuid,pid,used_gpu_memory")
    observed = {pid: 0 for pid in pids}
    devices = {pid: set() for pid in pids}
    found = set()
    for line in rows:
        fields = [field.strip() for field in line.split(",")]
        if len(fields) != 3:
            raise ProtocolError(f"invalid per-process GPU memory row: {line!r}")
        try:
            pid = int(fields[1])
            memory_bytes = int(fields[2]) * 1024 * 1024
        except ValueError as error:
            raise ProtocolError(f"invalid per-process GPU memory row: {line!r}") from error
        if pid in pids:
            observed[pid] += memory_bytes
            devices[pid].add(fields[0])
            found.add(pid)
    if found != pids:
        raise ProtocolError(f"GPU memory query did not contain expected PIDs: {sorted(pids - found)}")
    return observed, devices


def verify_executable(pid: int, expected: Path, role: str) -> None:
    try:
        observed = Path(os.readlink(f"/proc/{pid}/exe")).resolve(strict=True)
    except OSError as error:
        raise ProtocolError(f"cannot identify {role} process {pid}: {error}") from error
    if observed != expected.resolve(strict=True):
        raise ProtocolError(f"unexpected {role} executable for PID {pid}: {observed}")


def collect_samples(
    load_process, pids: dict[str, int], duration_seconds: int, interval_seconds: int, nvidia_smi: Path
) -> list[dict]:
    started = time.monotonic()
    deadline = started + duration_seconds
    samples = []
    while True:
        if load_process.poll() is not None:
            raise ProtocolError(f"DU contention driver exited during soak with status {load_process.returncode}")
        processes = {role: status_fields(pid) for role, pid in pids.items()}
        device_memory, devices = gpu_memory(nvidia_smi, set(pids.values()))
        for role, pid in pids.items():
            processes[role]["gpu_memory_bytes"] = device_memory[pid]
        samples.append(
            {
                "elapsed_ns": int((time.monotonic() - started) * 1_000_000_000),
                "processes": processes,
                "gpu_uuids": {role: sorted(devices[pid]) for role, pid in pids.items()},
            }
        )
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            return samples
        time.sleep(min(interval_seconds, remaining))


def process_summary(samples: list[dict], role: str, pid: int, bounds: dict) -> tuple[dict, list[str]]:
    if len(samples) < 2:
        raise ProtocolError("soak must contain at least two resource samples")
    values = [sample["processes"][role] for sample in samples]
    initial = values[0]
    final = values[-1]
    peaks = {field: max(item[field] for item in values) for field in initial}
    growth = {field: peaks[field] - initial[field] for field in initial}
    reasons = []
    initial_devices = samples[0]["gpu_uuids"][role]
    if not initial_devices or any(sample["gpu_uuids"][role] != initial_devices for sample in samples):
        reasons.append(f"{role} GPU device set was empty or changed during soak")
    bound_role = "du" if role == "du" else "worker"
    limits = {
        "rss_bytes": bounds[f"maximum_{bound_role}_rss_growth_bytes"],
        "gpu_memory_bytes": bounds[f"maximum_{bound_role}_gpu_memory_growth_bytes"],
        "file_descriptors": bounds["maximum_fd_growth"],
        "threads": bounds["maximum_thread_growth"],
    }
    for field, limit in limits.items():
        if growth[field] > limit:
            reasons.append(f"{role} {field} growth {growth[field]} exceeds bound {limit}")
    return {
        "pid": pid,
        "initial": initial,
        "final": final,
        "peak": peaks,
        "peak_growth": growth,
        "bounds": limits,
        "gpu_uuids": initial_devices,
    }, reasons


def write_samples(path: Path, samples: list[dict]) -> None:
    with path.open("w", encoding="utf-8") as output:
        for sample in samples:
            output.write(json.dumps(sample, sort_keys=True, separators=(",", ":")) + "\n")


def validate_arguments(args: argparse.Namespace) -> dict:
    if not 1 <= args.duration_seconds <= 604800:
        raise ProtocolError("duration-seconds must be in [1, 604800]")
    if not 1 <= args.sample_interval_seconds <= min(60, args.duration_seconds):
        raise ProtocolError("sample-interval-seconds must be in [1, min(60, duration)]")
    if not 1 <= args.restart_cycles <= 100:
        raise ProtocolError("restart-cycles must be in [1, 100]")
    if not 1 <= args.ready_timeout_seconds <= 86400:
        raise ProtocolError("ready-timeout-seconds must be in [1, 86400]")
    if not 1 <= args.load_shutdown_timeout_seconds <= 86400:
        raise ProtocolError("load-shutdown-timeout-seconds must be in [1, 86400]")
    if not isinstance(args.bounds_rationale, str) or not args.bounds_rationale.strip():
        raise ProtocolError("bounds-rationale must be nonempty")
    if "replace" in args.bounds_rationale.lower():
        raise ProtocolError("replace the example bounds rationale before use")
    numeric = {
        "maximum_du_rss_growth_bytes": args.maximum_du_rss_growth_mib * 1024 * 1024,
        "maximum_worker_rss_growth_bytes": args.maximum_worker_rss_growth_mib * 1024 * 1024,
        "maximum_du_gpu_memory_growth_bytes": args.maximum_du_gpu_memory_growth_mib * 1024 * 1024,
        "maximum_worker_gpu_memory_growth_bytes": args.maximum_worker_gpu_memory_growth_mib * 1024 * 1024,
        "maximum_fd_growth": args.maximum_fd_growth,
        "maximum_thread_growth": args.maximum_thread_growth,
    }
    if any(value < 0 for value in numeric.values()):
        raise ProtocolError("soak resource-growth bounds must be nonnegative")
    return numeric


def run(args: argparse.Namespace) -> tuple[dict, int]:
    bounds = validate_arguments(args)
    args.load_driver = contention.resolve_file(args.load_driver)
    args.odu = contention.resolve_file(args.odu, executable=True)
    args.client = contention.resolve_file(args.client)
    args.e3_schema = contention.resolve_file(args.e3_schema)
    args.preflight_helper = contention.resolve_file(args.preflight_helper, executable=True)
    args.nvidia_smi = contention.resolve_file(args.nvidia_smi, executable=True)
    args.catalog = args.catalog.resolve(strict=True)
    if not args.catalog.is_dir():
        raise ProtocolError(f"dApp catalog is not a directory: {args.catalog}")
    packages, worker_executables = catalog_provenance(args.catalog)
    devices = gpu_inventory(args.nvidia_smi)
    args.output_dir = args.output_dir.resolve()
    args.output_dir.mkdir(parents=True, exist_ok=False, mode=0o700)
    args.load_hold_timeout_seconds = args.duration_seconds + 600

    ready_path = args.output_dir / "load-ready.json"
    release_path = args.output_dir / "load-release"
    result_path = args.output_dir / "load-result.json"
    load_log_path = args.output_dir / "load-driver.log"
    samples_path = args.output_dir / "resource-samples.jsonl"
    command = contention.load_command(args, ready_path, release_path, result_path)
    runner_path = Path(__file__).resolve(strict=True)
    contention_path = runner_path.with_name("run_class_a_cuda_contention.py")
    qualification_path = runner_path.with_name("run_class_a_cuda_qualification.py")
    report = {
        "SPDX-FileCopyrightText": "Copyright (C) 2026 DeepSig Inc.",
        "SPDX-License-Identifier": "BSD-3-Clause-Clear",
        "schema": REPORT_SCHEMA,
        "created_at": qualification.utc_now(),
        "status": "invalid",
        "host_manifest": qualification.host_manifest(),
        "gpu_inventory": devices,
        "tooling": {
            "runner": artifact(runner_path),
            "contention_protocol": artifact(contention_path),
            "qualification_protocol": artifact(qualification_path),
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
            "cell_count": 2 if getattr(args, "multicell_cuda", False) else 1,
            "configured_pusch_concurrency": 2 if getattr(args, "multicell_cuda", False) else 0,
            "duration_seconds": args.duration_seconds,
            "sample_interval_seconds": args.sample_interval_seconds,
            "restart_cycles_before_soak": args.restart_cycles,
            "release_bounds_approved": args.release_bounds_approved,
            "bounds_rationale": args.bounds_rationale,
            **bounds,
        },
    }
    invalid = False
    failed = False
    load_process = None
    ready = None
    samples = []
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
            ready = contention.wait_ready(load_process, ready_path, args.ready_timeout_seconds)
            pids = sorted(
                contention.validate_ready(
                    ready,
                    args.restart_cycles,
                    contention.READY_SCHEMA,
                    expected_cell_count,
                    expected_pusch_concurrency,
                )
            )
            du_pid = ready["du_pid"]
            instance_metrics = ready["instances"]
            process_pids = {
                "du": du_pid,
                "spectrum_worker": instance_metrics["spectrum_process"]["metrics"]["process.pid"],
                "srs_worker": instance_metrics["srs_isac_process"]["metrics"]["process.pid"],
            }
            if set(process_pids.values()) != set(pids):
                raise ProtocolError("soak process-role mapping does not match the validated load PIDs")
            verify_executable(du_pid, args.odu, "DU")
            for role, executable in worker_executables.items():
                verify_executable(process_pids[role], executable, role.replace("_", " "))
            report["load"].update(
                status="running",
                ready={"path": ready_path.name, "sha256": qualification.sha256(ready_path)},
                process_pids=process_pids,
            )
            samples = collect_samples(
                load_process,
                process_pids,
                args.duration_seconds,
                args.sample_interval_seconds,
                args.nvidia_smi,
            )
            write_samples(samples_path, samples)
        except (OSError, ProtocolError) as error:
            report["load"]["reasons"].append(str(error))
            invalid = True
        finally:
            if load_process is not None:
                exit_code, stop_error = contention.stop_load(
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
                        contention.validate_result(
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
    if samples_path.exists():
        report["samples"] = {"path": samples_path.name, "sha256": qualification.sha256(samples_path)}
    if samples and ready is not None:
        pids = report["load"]["process_pids"]
        process_reports = {}
        resource_reasons = []
        for role in ("du", "spectrum_worker", "srs_worker"):
            process_reports[role], process_reasons = process_summary(samples, role, pids[role], bounds)
            resource_reasons.extend(process_reasons)
        admission_reasons = []
        if not args.release_bounds_approved:
            admission_reasons.append("operator has not approved the resource-growth bounds for release admission")
        reasons = resource_reasons + admission_reasons
        report["summary"] = {
            "sample_count": len(samples),
            "observed_duration_ns": samples[-1]["elapsed_ns"],
            "engineering_endurance_passed": not resource_reasons,
            "release_admission_passed": not reasons,
            "processes": process_reports,
            "reasons": reasons,
        }
        failed = bool(reasons)
    else:
        report["summary"] = {
            "sample_count": 0,
            "observed_duration_ns": 0,
            "engineering_endurance_passed": False,
            "release_admission_passed": False,
            "processes": {},
            "reasons": [],
        }
        invalid = True

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
    parser.add_argument("--load-driver", type=Path, required=True)
    parser.add_argument("--odu", type=Path, required=True)
    parser.add_argument("--client", type=Path, required=True)
    parser.add_argument("--e3-schema", type=Path, required=True)
    parser.add_argument("--catalog", type=Path, required=True)
    parser.add_argument("--preflight-helper", type=Path, required=True)
    parser.add_argument("--nvidia-smi", type=Path)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--duration-seconds", type=int, required=True)
    parser.add_argument("--sample-interval-seconds", type=int, default=5)
    parser.add_argument("--restart-cycles", type=int, default=3)
    parser.add_argument("--multicell-cuda", action="store_true")
    parser.add_argument("--maximum-du-rss-growth-mib", type=int, required=True)
    parser.add_argument("--maximum-worker-rss-growth-mib", type=int, required=True)
    parser.add_argument("--maximum-du-gpu-memory-growth-mib", type=int, required=True)
    parser.add_argument("--maximum-worker-gpu-memory-growth-mib", type=int, required=True)
    parser.add_argument("--maximum-fd-growth", type=int, required=True)
    parser.add_argument("--maximum-thread-growth", type=int, required=True)
    parser.add_argument("--bounds-rationale", required=True)
    parser.add_argument("--release-bounds-approved", action="store_true")
    parser.add_argument("--ready-timeout-seconds", type=int, default=120)
    parser.add_argument("--load-shutdown-timeout-seconds", type=int, default=180)
    args = parser.parse_args()
    if args.nvidia_smi is None:
        discovered = shutil.which("nvidia-smi")
        if discovered is None:
            parser.error("--nvidia-smi is required when nvidia-smi is not on PATH")
        args.nvidia_smi = Path(discovered)
    report_path = args.output_dir / "soak-report.json"
    try:
        report, status = run(args)
    except (OSError, ProtocolError) as error:
        print(f"CUDA dApp soak protocol failed: {error}", file=sys.stderr)
        return 1
    report_path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"soak_status={report['status']} report={report_path}")
    return status


if __name__ == "__main__":
    sys.exit(main())
