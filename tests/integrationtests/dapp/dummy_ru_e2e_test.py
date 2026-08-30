#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear

"""Run real OCUDU DU/dummy-RU lifecycle and Class-C recovery coverage."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import tempfile
import threading
import time


SPECTRUM_NATIVE = "8f0d0001-4f9c-4e31-8c00-000000000001"
SRS_ISAC_NATIVE = "8f0d0001-4f9c-4e31-8c00-000000000002"
CHANNEL_ESTIMATOR_CPU = "8f0d0001-4f9c-4e31-8c00-000000000003"
EQUALIZER_CPU = "8f0d0001-4f9c-4e31-8c00-000000000004"
RECEIVER_CPU = "8f0d0001-4f9c-4e31-8c00-000000000005"
SCHEDULER_NATIVE = "8f0d0001-4f9c-4e31-8c00-000000000006"
SPECTRUM_PROCESS = "8f0d0001-4f9c-4e31-8c00-000000000101"
SRS_ISAC_PROCESS = "8f0d0001-4f9c-4e31-8c00-000000000102"
SPECTRUM_OBSERVER_PROCESS = "8f0d0001-4f9c-4e31-8c00-000000000103"
CHANNEL_ESTIMATOR_CUDA = "8f0d0001-4f9c-4e31-8c00-000000000203"
EQUALIZER_CUDA = "8f0d0001-4f9c-4e31-8c00-000000000204"
RECEIVER_CUDA = "8f0d0001-4f9c-4e31-8c00-000000000205"
SPECTRUM_CUDA_NATIVE = "8f0d0001-4f9c-4e31-8c00-000000000201"
SRS_ISAC_CUDA_NATIVE = "8f0d0001-4f9c-4e31-8c00-000000000202"
SPECTRUM_CUDA_PROCESS = "8f0d0001-4f9c-4e31-8c00-000000000301"
SRS_ISAC_CUDA_PROCESS = "8f0d0001-4f9c-4e31-8c00-000000000302"
FAILURE_CE_EXCEPTION = "8f0d0001-4f9c-4e31-8c00-000000000401"
FAILURE_CE_INVALID = "8f0d0001-4f9c-4e31-8c00-000000000402"
FAILURE_CE_DEADLINE = "8f0d0001-4f9c-4e31-8c00-000000000403"
FAILURE_SPECTRUM_PROCESS_INVALID = "8f0d0001-4f9c-4e31-8c00-000000000404"
TERMINAL_JOBS = {"succeeded", "failed", "cancelled"}
CONTENTION_READY_SCHEMA = "ocudu.dapp.cuda-contention-load-ready.v1"
CONTENTION_RESULT_SCHEMA = "ocudu.dapp.cuda-contention-load-result.v1"
CLASS_C_THROUGHPUT_READY_SCHEMA = "ocudu.dapp.class-c-cuda-throughput-load-ready.v1"
CLASS_C_THROUGHPUT_RESULT_SCHEMA = "ocudu.dapp.class-c-cuda-throughput-load-result.v1"
FAILURE_GALLERY_SCHEMA = "ocudu.dapp.failure-gallery-report.v1"


def require(condition, detail):
    if not condition:
        raise RuntimeError(detail)


def unsigned_value(sample):
    encoded = sample["value"]["unsignedValue"]
    return int(encoded.removeprefix("hex:"), 16)


def fixed_unsigned(value):
    if isinstance(value, int):
        return value
    require(isinstance(value, str) and value.startswith("hex:"), f"invalid fixed-width unsigned value: {value}")
    return int(value.removeprefix("hex:"), 16)


class ManagementJobError(RuntimeError):
    """Failed E3 management job with structured result fields for lifecycle races."""

    def __init__(self, job):
        self.job = job if isinstance(job, dict) else {}
        self.result = self.job.get("result") if isinstance(self.job.get("result"), dict) else {}
        super().__init__(f"management job failed: {job}")

    @property
    def instance_generation(self):
        value = self.result.get("instanceGeneration")
        if value is None:
            return None
        try:
            return fixed_unsigned(value)
        except Exception:
            return None

    @property
    def instance_state(self):
        # Prefer the instance state carried on the result (not the job state).
        state = self.result.get("state")
        return state if isinstance(state, str) else None

    @property
    def detail(self):
        return self.result.get("detail") or self.job.get("detail") or ""


def parse_lifecycle_conflict(error):
    """Extract (generation, state, detail) from a lifecycle failure.

    Prefers structured ManagementJobError fields; falls back to regex on the
    string form of older/unexpected exceptions.
    """
    if isinstance(error, ManagementJobError):
        return error.instance_generation, error.instance_state, error.detail or str(error)

    message = str(error)
    generation = None
    state = None
    if not message:
        return generation, state, message
    # Prefer nested result.state over the job-level state ("failed").
    match = re.search(
        r"['\"]result['\"]\s*:\s*\{[^}]*?['\"]state['\"]\s*:\s*['\"]([A-Za-z]+)['\"]",
        message,
    )
    if match:
        state = match.group(1)
    match = re.search(r"['\"]instanceGeneration['\"]\s*:\s*(\d+)", message)
    if match:
        generation = int(match.group(1))
    else:
        match = re.search(r"['\"]instanceGeneration['\"]\s*:\s*['\"]hex:([0-9a-fA-F]+)['\"]", message)
        if match:
            generation = int(match.group(1), 16)
    if state is None:
        # Last resort: any state token (may be the job state "failed").
        match = re.search(r"['\"]state['\"]\s*:\s*['\"]([A-Za-z]+)['\"]", message)
        if match:
            state = match.group(1)
    return generation, state, message


# Lifecycle states where activate is already complete (host naming variants).
_ACTIVE_STATES = {"active"}
# States where deactivate is effectively done / not needed.
_INACTIVE_STATES = {"inactive", "unloaded", "unloading", "loaded", "prepared", "armed", "shadow"}
# States where unload is complete.
_UNLOADED_STATES = {"unloaded"}


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


class BoundedOutput:
    def __init__(self, stream, capacity=400):
        self._stream = stream
        self._capacity = capacity
        self._lines = []
        self._lock = threading.Lock()
        self._thread = threading.Thread(target=self._read, daemon=True)
        self._thread.start()

    def _read(self):
        for line in self._stream:
            with self._lock:
                self._lines.append(line.rstrip())
                if len(self._lines) > self._capacity:
                    del self._lines[: len(self._lines) - self._capacity]

    def text(self):
        with self._lock:
            return "\n".join(self._lines)

    def join(self):
        self._thread.join(timeout=2)


class E3Client:
    def __init__(self, executable, schema, socket_path):
        self._base = [
            "python3",
            str(executable),
            "--socket",
            str(socket_path),
            "--schema",
            str(schema),
            "--timeout-ms",
            "2000",
        ]

    def request(self, *arguments):
        completed = subprocess.run(self._base + list(arguments), text=True, capture_output=True, timeout=5)
        require(completed.returncode == 0, f"E3 client failed: {completed.stderr or completed.stdout}")
        response = json.loads(completed.stdout)
        require(
            response.get("code") in {"success", "dryRunSuccess", "accepted"},
            f"E3 request was rejected: {response}",
        )
        return response

    def wait_ready(self, process, timeout=15):
        deadline = time.monotonic() + timeout
        last_error = "local E3 endpoint did not become ready"
        while time.monotonic() < deadline:
            require(process.poll() is None, f"DU exited during startup with status {process.returncode}")
            try:
                return self.request("inventory")
            except (RuntimeError, subprocess.TimeoutExpired, json.JSONDecodeError) as error:
                last_error = str(error)
                time.sleep(0.05)
        raise RuntimeError(last_error)

    def wait_job(self, job_id, timeout=10, expect_success=True):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            response = self.request("jobs", "get", "--job", job_id)
            jobs = response["payload"]["jobs"]["jobs"]
            require(len(jobs) == 1, f"job query returned {len(jobs)} records")
            job = jobs[0]
            if job["state"] in TERMINAL_JOBS:
                result = job.get("result", {})
                if expect_success:
                    if job["state"] != "succeeded" or result.get("code") not in {"success", "dryRunSuccess"}:
                        raise ManagementJobError(job)
                else:
                    require(job["state"] == "failed", f"management job unexpectedly succeeded: {job}")
                    require(
                        result.get("code") not in {"success", "dryRunSuccess"},
                        f"failed management job returned success: {job}",
                    )
                return result
            time.sleep(0.02)
        raise RuntimeError(f"management job {job_id} did not complete")

    def mutation(self, *arguments):
        response = self.request(*arguments)
        require(response["code"] == "accepted" and "jobId" in response, f"mutation was not queued: {response}")
        return self.wait_job(response["jobId"])

    def failed_mutation(self, *arguments):
        response = self.request(*arguments)
        require(response["code"] == "accepted" and "jobId" in response, f"mutation was not queued: {response}")
        return self.wait_job(response["jobId"], expect_success=False)

    def load(self, package_id, placement, backend="cpu"):
        result = self.mutation(
            "load", "--package", package_id, "--placement", placement, "--backend", backend
        )
        return result["instanceId"], fixed_unsigned(result["instanceGeneration"])

    def lifecycle(self, instance_id, generation, action):
        """Run a lifecycle mutation, tolerating Class-A generation races.

        Incident refresh can advance instance generation between the client
        reading inventory and the host applying the mutation. Retry with the
        generation carried in the conflict result; if the desired state is
        already reached (activate→active, deactivate→inactive, unload→unloaded),
        treat the mutation as successful.
        """
        generation = fixed_unsigned(generation)
        last_error = None
        for attempt in range(12):
            try:
                result = self.mutation(
                    "lifecycle",
                    action,
                    "--instance",
                    instance_id,
                    "--expected-generation",
                    str(generation),
                    "--timeout-us",
                    "5000000",
                )
                require(result["instanceId"] == instance_id, "lifecycle result changed instance identity")
                return fixed_unsigned(result["instanceGeneration"])
            except (RuntimeError, ManagementJobError) as error:
                last_error = error
                conflict_gen, conflict_state, detail = parse_lifecycle_conflict(error)
                message = detail or str(error)

                def desired_reached(state_name):
                    if not state_name:
                        return False
                    if action == "activate":
                        return state_name in _ACTIVE_STATES
                    if action == "deactivate":
                        return state_name in _INACTIVE_STATES | _UNLOADED_STATES
                    if action == "unload":
                        return state_name in _UNLOADED_STATES
                    # prepare/arm/shadow: never treat a conflict as success by state alone.
                    return False

                if desired_reached(conflict_state):
                    if conflict_gen is not None:
                        return conflict_gen
                    return self.instance_generation(instance_id)

                raced = "generation mismatch" in message or "illegal in current state" in message
                if not raced:
                    raise

                try:
                    live_gen, live_state = self.instance_state(instance_id)
                    if desired_reached(live_state):
                        return live_gen
                    generation = conflict_gen if conflict_gen is not None else live_gen
                except Exception:
                    if conflict_gen is None:
                        raise
                    generation = conflict_gen
                time.sleep(0.03 * (attempt + 1))
                continue
        raise last_error

    def activate(self, package_id, placement, backend="cpu"):
        instance_id, generation = self.shadow(package_id, placement, backend)
        generation = self.lifecycle(instance_id, generation, "activate")
        return instance_id, generation

    def shadow(self, package_id, placement, backend="cpu"):
        instance_id, generation = self.load(package_id, placement, backend)
        for action in ("prepare", "arm", "shadow"):
            generation = self.lifecycle(instance_id, generation, action)
        return instance_id, generation

    def fail_activation(self, instance_id, generation):
        result = self.failed_mutation(
            "lifecycle",
            "activate",
            "--instance",
            instance_id,
            "--expected-generation",
            str(fixed_unsigned(generation)),
            "--timeout-us",
            "2000000",
        )
        require(result["instanceId"] == instance_id, "failed activation changed instance identity")
        return result

    def fail_lifecycle(self, instance_id, generation, action):
        result = self.failed_mutation(
            "lifecycle",
            action,
            "--instance",
            instance_id,
            "--expected-generation",
            str(fixed_unsigned(generation)),
            "--timeout-us",
            "2000000",
        )
        require(result["instanceId"] == instance_id, "failed lifecycle operation changed instance identity")
        return result

    def unload(self, instance_id, generation):
        """Deactivate and unload, racing Class-A incident generation advances.

        While a Class-A package is still bound it may keep recording non-observed
        incidents that advance instance generation under refresh(). Deactivate
        therefore needs more aggressive refresh-and-retry than other lifecycle
        steps. Once deactivate lands, unload is comparatively quiet.
        """
        generation = fixed_unsigned(generation)
        # Deactivate first with extended retries; treat "already inactive" as OK
        # via lifecycle()'s desired-state recovery.
        last_error = None
        for attempt in range(40):
            try:
                generation = self.lifecycle(instance_id, generation, "deactivate")
                last_error = None
                break
            except (RuntimeError, ManagementJobError) as error:
                last_error = error
                conflict_gen, conflict_state, _ = parse_lifecycle_conflict(error)
                if conflict_state in _INACTIVE_STATES | _UNLOADED_STATES:
                    generation = conflict_gen if conflict_gen is not None else generation
                    last_error = None
                    break
                try:
                    generation = (
                        conflict_gen
                        if conflict_gen is not None
                        else self.instance_generation(instance_id)
                    )
                except Exception:
                    if conflict_gen is not None:
                        generation = conflict_gen
                    else:
                        raise
                time.sleep(0.05)
        if last_error is not None:
            raise last_error

        for attempt in range(20):
            try:
                generation = self.lifecycle(instance_id, generation, "unload")
                return
            except (RuntimeError, ManagementJobError) as error:
                last_error = error
                conflict_gen, conflict_state, _ = parse_lifecycle_conflict(error)
                if conflict_state in _UNLOADED_STATES:
                    return
                # Instance already gone from inventory.
                try:
                    generation = (
                        conflict_gen
                        if conflict_gen is not None
                        else self.instance_generation(instance_id)
                    )
                except Exception:
                    return
                time.sleep(0.05)
        if last_error is not None:
            raise last_error

    def metrics(self, instance_id):
        response = self.request("metrics", "get", "--instance", instance_id)
        samples = response["payload"]["metrics"]["samples"]
        return {sample["name"]: unsigned_value(sample) for sample in samples}

    def config(self, instance_id):
        response = self.request("config", "get", "--instance", instance_id)
        return response["payload"]["config"]

    def set_enabled(self, instance_id, config_generation, enabled):
        result = self.mutation(
            "config",
            "set",
            "--instance",
            instance_id,
            "--config-generation",
            str(config_generation),
            "--set",
            f"enabled=bool:{str(enabled).lower()}",
        )
        return result["resourceGeneration"], result["instanceGeneration"]

    def model(self, instance_id, model_generation, action, artifact=None, dry_run=False):
        arguments = [
            "model",
            action,
            "--instance",
            instance_id,
            "--model",
            "dummyru-model",
            "--version",
            "1",
            "--model-generation",
            str(model_generation),
        ]
        if artifact is not None:
            arguments += ["--artifact", str(artifact)]
        if dry_run:
            arguments.append("--dry-run")
        result = self.mutation(*arguments)
        return result["resourceGeneration"], result["instanceGeneration"]

    def instance_generation(self, instance_id):
        return self.instance_state(instance_id)[0]

    def instance_state(self, instance_id):
        """Return (generation:int, state:str) for a live instance."""
        instances = self.request("inventory")["payload"]["inventory"]["instances"]
        matching = [instance for instance in instances if instance["instanceId"] == instance_id]
        require(len(matching) == 1, f"instance {instance_id} is absent or duplicated")
        return fixed_unsigned(matching[0]["generation"]), matching[0].get("state") or ""

    def wait_metric(self, instance_id, name, predicate, timeout=10):
        deadline = time.monotonic() + timeout
        last = None
        while time.monotonic() < deadline:
            last = self.metrics(instance_id).get(name)
            if last is not None and predicate(last):
                return last
            time.sleep(0.05)
        raise RuntimeError(f"metric {name} did not satisfy predicate; last value was {last}")


def write_config(
    path,
    socket_path,
    log_path,
    catalog,
    preflight_helper,
    multicell_cpu,
    multicell_cuda,
    failure_gallery,
    initial_startup,
    cuda_class_a,
    cuda_class_c,
    channel_bandwidth_mhz,
    srs_period_ms,
    time_scaling,
    package_trust_store,
    minimum_trust_policy_epoch,
):
    quote = json.dumps
    multicell = multicell_cpu or multicell_cuda
    log_level = "error" if multicell or failure_gallery else "warning"
    ue_creation_stagger_slots = 320 if multicell else 10
    cpu_class_a_manifests = (
        "    - ref_channel_estimator_cpu.manifest.json\n"
        "    - ref_equalizer_cpu.manifest.json\n"
        "    - ref_receiver_cpu.manifest.json\n"
        if multicell_cpu
        else ""
    )
    cuda_manifest = (
        "    - ref_channel_estimator_cuda.manifest.json\n"
        "    - ref_equalizer_cuda.manifest.json\n"
        "    - ref_receiver_cuda.manifest.json\n"
        if cuda_class_a
        else ""
    )
    cuda_class_c_manifests = (
        "    - ref_spectrum_cuda.manifest.json\n"
        "    - ref_srs_isac_cuda.manifest.json\n"
        "    - ref_spectrumd_cuda.manifest.json\n"
        "    - ref_srs_isacd_cuda.manifest.json\n"
        if cuda_class_c
        else ""
    )
    failure_manifests = (
        "    - failure_channel_estimator_exception_cpu.manifest.json\n"
        "    - failure_channel_estimator_invalid_cpu.manifest.json\n"
        "    - failure_channel_estimator_deadline_cpu.manifest.json\n"
        "    - failure_spectrumd_invalid_cpu.manifest.json\n"
        if failure_gallery
        else ""
    )
    pusch_acceleration_profile = ""
    if cuda_class_a or cuda_class_c:
        pusch_acceleration_profile = "expert_phy:\n  pusch_acceleration_mode: enabled\n"
    elif multicell_cpu or failure_gallery:
        pusch_acceleration_profile = "expert_phy:\n  pusch_acceleration_mode: disabled\n"
    cuda_multicell_concurrency = (
        "expert_execution:\n"
        "  threads:\n"
        "    upper_phy:\n"
        "      max_pusch_and_srs_concurrency: 2\n"
        if multicell_cuda
        else ""
    )
    class_a_pusch_profile = (
        "  pusch:\n"
        "    min_ue_mcs: 0\n"
        "    max_ue_mcs: 0\n"
        "    mcs_table: qam64\n"
        if cuda_class_a
        else ""
    )
    cell_overrides = (
        "cells:\n"
        "  - pci: 1\n"
        "    sector_id: 0\n"
        "  - pci: 2\n"
        "    sector_id: 1\n"
        if multicell
        else ""
    )
    signature_config = (
        f"  package_signature_mode: required\n"
        f"  package_trust_store: {quote(str(package_trust_store))}\n"
        f"  minimum_trust_policy_epoch: {minimum_trust_policy_epoch}\n"
        if package_trust_store
        else "  package_signature_mode: allow_unsigned\n  minimum_trust_policy_epoch: 0\n"
    )
    initial_instances = (
        "  initial_instances:\n"
        "    - package_manifest: ref_spectrum_cpu.manifest.json\n"
        "      placement: in_process\n"
        "      backend: cpu\n"
        "      target: active\n"
        "      lifecycle_timeout_ms: 2000\n"
        "    - package_manifest: ref_srs_isacd_cpu.manifest.json\n"
        "      placement: out_of_process\n"
        "      backend: cpu\n"
        "      target: shadow\n"
        "      lifecycle_timeout_ms: 2000\n"
        if initial_startup
        else ""
    )
    path.write_text(
        f"""# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear

log:
  filename: {quote(str(log_path))}
  all_level: {log_level}
test_mode:
  test_ue:
    rnti: 0x44
    ri: 1
    cqi: 15
    nof_ues: 1
    ue_creation_stagger_slots: {ue_creation_stagger_slots}
    pusch_active: true
    pdsch_active: false
    auto_ack_indication_delay: 1
ru_dummy:
  time_scaling: {time_scaling}
dapp:
  catalog_root: {quote(str(catalog))}
  native_preflight_helper: {quote(str(preflight_helper))}
{signature_config}  package_manifests:
    - ref_spectrum_cpu.manifest.json
    - ref_srs_isac_cpu.manifest.json
    - ref_spectrumd_cpu.manifest.json
    - ref_srs_isacd_cpu.manifest.json
    - ref_spectrum_observerd_cpu.manifest.json
    - ref_scheduler_cpu.manifest.json
{cpu_class_a_manifests}{cuda_manifest}{cuda_class_c_manifests}{failure_manifests}{initial_instances}  max_scheduler_ul_prbs_avoided: 8
  class_c_process_restart_backoff_ms: 20
  class_c_process_management_timeout_ms: 2000
  local_e3:
    socket_path: {quote(str(socket_path))}
    request_timeout_ms: 2000
{pusch_acceleration_profile}{cuda_multicell_concurrency}cell_cfg:
{class_a_pusch_profile}  dl_arfcn: 637212
  band: 78
  channel_bandwidth_MHz: {channel_bandwidth_mhz}
  common_scs: 30
  srs:
    type_enabled: periodic
    period_ms: {srs_period_ms}
{cell_overrides}
""",
        encoding="utf-8",
    )


def assert_inventory(client, expected_packages, expected_instances, expected_states=None):
    inventory = client.request("inventory")["payload"]["inventory"]
    packages = {item["packageId"] for item in inventory["packages"]}
    require(packages == expected_packages, f"unexpected package inventory: {packages}")
    instances = inventory["instances"]
    require(len(instances) == expected_instances, f"unexpected instance inventory: {instances}")
    if expected_states is not None:
        states = {instance["state"] for instance in instances}
        require(states == expected_states, f"unexpected instance states: {instances}")


def require_worker_child(worker_pid, du_pid, expected_executable):
    status = Path(f"/proc/{worker_pid}/status").read_text(encoding="utf-8")
    parent = next((line.split()[1] for line in status.splitlines() if line.startswith("PPid:")), None)
    require(parent == str(du_pid), f"reported worker PID {worker_pid} is not a DU child")
    executable = Path(os.readlink(f"/proc/{worker_pid}/exe")).resolve()
    require(executable == expected_executable.resolve(), f"unexpected worker executable: {executable}")


def publish_json_atomic(path, payload):
    require(path.parent.is_dir(), f"contention-record parent directory is missing: {path.parent}")
    temporary = path.with_name(f".{path.name}.{os.getpid()}.tmp")
    temporary.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    os.replace(temporary, path)


def publish_contention_record(path, schema, process, restart_cycles, instances, load_profile):
    publish_json_atomic(
        path,
        {
            "SPDX-FileCopyrightText": "Copyright (C) 2026 DeepSig Inc.",
            "SPDX-License-Identifier": "BSD-3-Clause-Clear",
            "schema": schema,
            "published_monotonic_ns": time.monotonic_ns(),
            "du_pid": process.pid,
            "restart_cycles": restart_cycles,
            "load_profile": load_profile,
            "instances": instances,
        },
    )


def wait_for_contention_release(path, process, timeout_seconds):
    deadline = time.monotonic() + timeout_seconds
    while time.monotonic() < deadline:
        require(process.poll() is None, f"DU exited while contention load was held: {process.returncode}")
        if path.exists():
            return
        time.sleep(0.05)
    raise RuntimeError(f"contention release was not received within {timeout_seconds} seconds")


def run_multicell_cpu_campaign(client, process, catalog):
    expected_cell_bitmap = 0b11
    packages = {
        SPECTRUM_NATIVE,
        SRS_ISAC_NATIVE,
        CHANNEL_ESTIMATOR_CPU,
        EQUALIZER_CPU,
        RECEIVER_CPU,
        SCHEDULER_NATIVE,
        SPECTRUM_PROCESS,
        SRS_ISAC_PROCESS,
        SPECTRUM_OBSERVER_PROCESS,
    }
    assert_inventory(client, packages, 0)
    instance_ids = set()

    def remember(instance_id):
        require(instance_id not in instance_ids, f"instance identity was reused: {instance_id}")
        instance_ids.add(instance_id)

    def activate(package_id, placement):
        instance_id, generation = client.activate(package_id, placement)
        remember(instance_id)
        return instance_id, generation

    def require_both_cells(instance_id, timeout=15):
        return client.wait_metric(
            instance_id,
            "host.input.cell_bitmap",
            lambda value: value == expected_cell_bitmap,
            timeout=timeout,
        )

    for package_id in (CHANNEL_ESTIMATOR_CPU, EQUALIZER_CPU, RECEIVER_CPU):
        instance_id, generation = activate(package_id, "in-process")
        client.wait_metric(instance_id, "dapp.invoke.count", lambda value: value > 0)
        client.wait_metric(instance_id, "dapp.input.max_layers", lambda value: value == 1)
        require_both_cells(instance_id, timeout=30)
        rejected = client.fail_lifecycle(instance_id, generation - 1, "deactivate")
        require(rejected["code"] == "conflict", f"stale Class-A generation was not rejected: {rejected}")
        require(
            rejected["instanceGeneration"] >= generation,
            f"stale Class-A request regressed the current generation: {rejected}",
        )
        client.unload(instance_id, rejected["instanceGeneration"])

    scheduler_id, _ = activate(SCHEDULER_NATIVE, "in-process")
    client.wait_metric(scheduler_id, "dapp.invoke.count", lambda value: value > 0)
    client.wait_metric(scheduler_id, "class_b.policy.accepted", lambda value: value > 0)
    require_both_cells(scheduler_id)

    srs_native_id, srs_native_generation = activate(SRS_ISAC_NATIVE, "in-process")
    client.wait_metric(srs_native_id, "dapp.invoke.count", lambda value: value > 0)
    require_both_cells(srs_native_id)
    client.unload(srs_native_id, srs_native_generation)

    srs_process_id, _ = activate(SRS_ISAC_PROCESS, "process")
    client.wait_metric(srs_process_id, "class_c.copy.snapshot.count", lambda value: value > 0)
    client.wait_metric(srs_process_id, "process.results_busy", lambda value: value > 0)
    require_both_cells(srs_process_id)
    srs_metrics = client.metrics(srs_process_id)
    require(srs_metrics["process.result_protocol_failures"] == 0, "SRS process emitted an invalid result")
    require_worker_child(srs_metrics["process.pid"], process.pid, catalog / "ref_srs_isacd_cpu")
    srs_progress = srs_metrics["class_c.copy.snapshot.count"]

    observer_id, observer_generation = activate(SPECTRUM_OBSERVER_PROCESS, "process")
    client.wait_metric(observer_id, "class_c.copy.snapshot.count", lambda value: value > 0)
    require_both_cells(observer_id)
    observer_metrics = client.metrics(observer_id)
    require(observer_metrics["process.results_accepted"] == 0, "observer gained result authority")
    require(observer_metrics["process.result_protocol_failures"] == 0, "observer emitted a prohibited result")
    require_worker_child(observer_metrics["process.pid"], process.pid, catalog / "ref_spectrum_observerd_cpu")
    require(
        observer_metrics["process.pid"] != srs_metrics["process.pid"],
        "distinct SRS and spectrum observers shared one process unexpectedly",
    )
    observers = [(observer_id, observer_generation)]

    native_id, native_generation = activate(SPECTRUM_NATIVE, "in-process")
    client.wait_metric(native_id, "dapp.invoke.count", lambda value: value > 0)
    require_both_cells(native_id)

    conflicting_id, conflicting_generation = client.shadow(SPECTRUM_PROCESS, "process")
    remember(conflicting_id)
    rejected = client.fail_activation(conflicting_id, conflicting_generation)
    require(rejected["code"] == "lifecycleFailed", f"unexpected authority rejection: {rejected}")
    require(
        "another authority is active" in rejected.get("detail", ""),
        f"authority rejection was not diagnostic: {rejected}",
    )
    client.unload(conflicting_id, rejected["instanceGeneration"])

    observer_progress = {
        instance_id: client.metrics(instance_id)["class_c.copy.snapshot.count"]
        for instance_id, _ in observers
    }
    client.unload(native_id, native_generation)

    process_id, process_generation = activate(SPECTRUM_PROCESS, "process")
    accepted = client.wait_metric(process_id, "process.results_accepted", lambda value: value > 0)
    client.wait_metric(process_id, "class_c.copy.snapshot.count", lambda value: value > 0)
    require_both_cells(process_id)
    process_metrics = client.metrics(process_id)
    worker_pid = process_metrics["process.pid"]
    require_worker_child(worker_pid, process.pid, catalog / "ref_spectrumd_cpu")
    scheduler_before_restart = client.metrics(scheduler_id)["dapp.invoke.count"]
    os.kill(worker_pid, signal.SIGKILL)
    client.wait_metric(process_id, "process.exits", lambda value: value >= 1)
    client.wait_metric(process_id, "process.successful_restarts", lambda value: value >= 1)
    restarted_pid = client.wait_metric(
        process_id,
        "process.pid",
        lambda value: value > 0 and value != worker_pid,
    )
    require_worker_child(restarted_pid, process.pid, catalog / "ref_spectrumd_cpu")
    client.wait_metric(process_id, "process.results_accepted", lambda value: value > accepted)
    client.wait_metric(scheduler_id, "dapp.invoke.count", lambda value: value > scheduler_before_restart)
    restarted_metrics = client.metrics(process_id)
    require(
        restarted_metrics["process.result_protocol_failures"] == 0,
        "restarted spectrum worker emitted an invalid control result",
    )
    restart_rejected_results = restarted_metrics["process.results_rejected"]

    restarted_generation = client.instance_generation(process_id)
    require(restarted_generation > process_generation, "process restart did not advance instance generation")
    stale = client.fail_lifecycle(process_id, process_generation, "deactivate")
    require(stale["code"] == "conflict", f"stale post-restart generation was not rejected: {stale}")
    require(
        stale["instanceGeneration"] == restarted_generation,
        f"stale request did not report the current generation: {stale}",
    )
    client.unload(process_id, restarted_generation)

    repeated_id, repeated_generation = activate(SPECTRUM_PROCESS, "process")
    client.wait_metric(repeated_id, "process.results_accepted", lambda value: value > 0)
    require_both_cells(repeated_id)
    repeated_metrics = client.metrics(repeated_id)
    require_worker_child(repeated_metrics["process.pid"], process.pid, catalog / "ref_spectrumd_cpu")
    require(repeated_metrics["process.results_rejected"] == 0, "fresh spectrum worker result was rejected")
    client.unload(repeated_id, repeated_generation)

    for observer_id, observer_generation in observers:
        client.wait_metric(
            observer_id,
            "class_c.copy.snapshot.count",
            lambda value, before=observer_progress[observer_id]: value > before,
        )
        observer_metrics = client.metrics(observer_id)
        require(observer_metrics["process.results_accepted"] == 0, "observer gained authority during handoff")
        require(observer_metrics["process.results_rejected"] == 0, "observer result was rejected unexpectedly")
        require(observer_metrics["process.result_protocol_failures"] == 0, "observer violated its capability set")
        require(observer_metrics["process.successful_restarts"] == 0, "observer restarted unexpectedly")
        client.unload(observer_id, observer_generation)

    client.wait_metric(
        srs_process_id,
        "class_c.copy.snapshot.count",
        lambda value: value > srs_progress,
    )
    srs_metrics = client.metrics(srs_process_id)
    require(srs_metrics["process.results_rejected"] == 0, "SRS result was rejected unexpectedly")
    require(srs_metrics["process.result_protocol_failures"] == 0, "SRS observer failed during coexistence")
    require(srs_metrics["process.successful_restarts"] == 0, "SRS observer restarted unexpectedly")
    client.unload(srs_process_id, client.instance_generation(srs_process_id))
    client.unload(scheduler_id, client.instance_generation(scheduler_id))
    assert_inventory(client, packages, len(instance_ids), {"unloaded"})
    inventory_ids = {
        instance["instanceId"]
        for instance in client.request("inventory")["payload"]["inventory"]["instances"]
    }
    require(inventory_ids == instance_ids, "final inventory lost or introduced an instance tombstone")
    jobs = client.request("jobs", "list")["payload"]["jobs"]["jobs"]
    require(all(job["state"] in TERMINAL_JOBS for job in jobs), "management job remained live at teardown")
    incidents = client.request("incidents", "list", "--instance", process_id, "--limit", "16")
    incident_records = incidents["payload"]["incidents"]["incidents"]
    require(
        any(record["cause"] == "processExit" for record in incident_records),
        "process restart was not represented by an E3 incident",
    )
    rejected_incidents = sum(
        record["recurrenceCount"] for record in incident_records if record["cause"] == "invalidOutput"
    )
    require(
        rejected_incidents == restart_rejected_results,
        "restart result rejections were not represented exactly by E3 incidents",
    )
    health = client.request("health")["payload"]["health"]
    require(
        health["deviceErrors"] == 0
        and health["invalidInputs"] == 0
        and health["invalidOutputs"] == restart_rejected_results
        and health["standardExceptions"] == 0
        and health["unknownExceptions"] == 0,
        f"multicell CPU campaign reported an unsafe failure: {health}",
    )


def run_failure_gallery_campaign(client, process, catalog, report_path):
    packages = {
        SPECTRUM_NATIVE,
        SRS_ISAC_NATIVE,
        SCHEDULER_NATIVE,
        SPECTRUM_PROCESS,
        SRS_ISAC_PROCESS,
        SPECTRUM_OBSERVER_PROCESS,
        FAILURE_CE_EXCEPTION,
        FAILURE_CE_INVALID,
        FAILURE_CE_DEADLINE,
        FAILURE_SPECTRUM_PROCESS_INVALID,
    }
    assert_inventory(client, packages, 0)
    scenarios = []

    def incident_records(instance_id):
        response = client.request("incidents", "list", "--instance", instance_id, "--limit", "128")
        return response["payload"]["incidents"]["incidents"]

    def incidents_with_cause(instance_id, cause):
        return [record for record in incident_records(instance_id) if record["cause"] == cause]

    def record(name, instance_id, evidence):
        scenarios.append({"name": name, "status": "passed", "instance_id": instance_id, "evidence": evidence})

    # Authority conflicts fail before publication changes and preserve the active path.
    authority_id, authority_generation = client.activate(SPECTRUM_NATIVE, "in-process")
    progress = client.wait_metric(authority_id, "dapp.invoke.count", lambda value: value > 0)
    conflicting_id, conflicting_generation = client.shadow(SPECTRUM_PROCESS, "process")
    rejected = client.fail_activation(conflicting_id, conflicting_generation)
    require(rejected["code"] == "lifecycleFailed", f"unexpected authority conflict result: {rejected}")
    require("another authority is active" in rejected.get("detail", ""), "authority conflict lacked diagnosis")
    client.wait_metric(authority_id, "dapp.invoke.count", lambda value: value > progress)
    record(
        "authority_conflict",
        conflicting_id,
        {"result": rejected, "preserved_authority_instance_id": authority_id},
    )
    client.unload(conflicting_id, rejected["instanceGeneration"])
    client.unload(authority_id, authority_generation)

    native_failures = (
        (
            "native_exception",
            FAILURE_CE_EXCEPTION,
            "class_a.fallback.exception",
            "standardException",
        ),
        (
            "native_invalid_output",
            FAILURE_CE_INVALID,
            "class_a.fallback.invalid_output",
            "invalidOutput",
        ),
        (
            "native_deadline_fallback",
            FAILURE_CE_DEADLINE,
            "class_a.fallback.deadline",
            "deadlineMiss",
        ),
    )
    for name, package_id, metric_name, cause in native_failures:
        instance_id, generation = client.activate(package_id, "in-process")
        fallback_count = client.wait_metric(instance_id, metric_name, lambda value: value > 0, timeout=20)
        breaker_state = client.wait_metric(instance_id, "host.breaker_state", lambda value: value == 1, timeout=20)
        breaker_bypasses = client.wait_metric(
            instance_id, "class_a.fallback.breaker_open", lambda value: value > 0, timeout=20
        )
        records = incidents_with_cause(instance_id, cause)
        require(records, f"{name} was not represented by an E3 incident")
        if name == "native_exception":
            require(records[0]["action"] == "breakerOpened", f"native exception did not open breaker: {records}")
            require(
                "intentional failure-gallery" in records[0].get("sanitizedMessage", ""),
                f"native exception message was not preserved safely: {records[0]}",
            )
        record(
            name,
            instance_id,
            {
                "fallback_metric": metric_name,
                "fallback_count": fallback_count,
                "breaker_state": breaker_state,
                "breaker_bypasses": breaker_bypasses,
                "incidents": records,
            },
        )
        client.unload(instance_id, client.instance_generation(instance_id))

    # A stopped worker exercises authenticated heartbeat detection; a later
    # hard exit proves the distinct process-exit path on the recovered worker.
    process_id, process_generation = client.activate(SPECTRUM_PROCESS, "process")
    accepted = client.wait_metric(process_id, "process.results_accepted", lambda value: value > 0)
    first_pid = client.wait_metric(process_id, "process.pid", lambda value: value > 0)
    require_worker_child(first_pid, process.pid, catalog / "ref_spectrumd_cpu")
    os.kill(first_pid, signal.SIGSTOP)
    heartbeat_losses = client.wait_metric(process_id, "process.heartbeat_losses", lambda value: value > 0, timeout=20)
    first_restarts = client.wait_metric(process_id, "process.successful_restarts", lambda value: value >= 1, timeout=20)
    second_pid = client.wait_metric(
        process_id, "process.pid", lambda value: value > 0 and value != first_pid, timeout=20
    )
    require_worker_child(second_pid, process.pid, catalog / "ref_spectrumd_cpu")
    client.wait_metric(process_id, "process.results_accepted", lambda value: value > accepted)
    heartbeat_incidents = incidents_with_cause(process_id, "heartbeatLoss")
    require(heartbeat_incidents, "heartbeat loss was not represented by an E3 incident")
    record(
        "process_heartbeat_recovery",
        process_id,
        {
            "stopped_pid": first_pid,
            "restarted_pid": second_pid,
            "heartbeat_losses": heartbeat_losses,
            "successful_restarts": first_restarts,
            "incidents": heartbeat_incidents,
        },
    )

    os.kill(second_pid, signal.SIGKILL)
    second_restarts = client.wait_metric(
        process_id, "process.successful_restarts", lambda value: value >= 2, timeout=20
    )
    third_pid = client.wait_metric(
        process_id, "process.pid", lambda value: value > 0 and value != second_pid, timeout=20
    )
    require_worker_child(third_pid, process.pid, catalog / "ref_spectrumd_cpu")
    exit_incidents = incidents_with_cause(process_id, "processExit")
    require(exit_incidents, "hard process exit was not represented by an E3 incident")
    record(
        "process_exit_recovery",
        process_id,
        {
            "killed_pid": second_pid,
            "restarted_pid": third_pid,
            "successful_restarts": second_restarts,
            "incidents": exit_incidents,
        },
    )
    client.unload(process_id, client.instance_generation(process_id))

    invalid_id, invalid_generation = client.activate(FAILURE_SPECTRUM_PROCESS_INVALID, "process")
    rejected_results = client.wait_metric(invalid_id, "process.results_rejected", lambda value: value > 0, timeout=20)
    invalid_incidents = incidents_with_cause(invalid_id, "invalidOutput")
    require(invalid_incidents, "invalid process result was not represented by an E3 incident")
    invalid_metrics = client.metrics(invalid_id)
    require(invalid_metrics["process.results_accepted"] == 0, "invalid process result gained authority")
    record(
        "process_invalid_output",
        invalid_id,
        {
            "results_rejected": rejected_results,
            "results_accepted": invalid_metrics["process.results_accepted"],
            "incidents": invalid_incidents,
        },
    )
    client.unload(invalid_id, client.instance_generation(invalid_id))

    jobs = client.request("jobs", "list")["payload"]["jobs"]["jobs"]
    require(all(job["state"] in TERMINAL_JOBS for job in jobs), "failure-gallery management job remained live")
    inventory = client.request("inventory")["payload"]["inventory"]
    require(all(item["state"] == "unloaded" for item in inventory["instances"]), "gallery left a live instance")
    report = {
        "SPDX-FileCopyrightText": "Copyright (C) 2026 DeepSig Inc.",
        "SPDX-License-Identifier": "BSD-3-Clause-Clear",
        "schema": FAILURE_GALLERY_SCHEMA,
        "status": "passed",
        "published_monotonic_ns": time.monotonic_ns(),
        "du": {
            "pid": process.pid,
            "path": str(Path(process.args[0]).resolve()),
            "sha256": sha256(Path(process.args[0])),
        },
        "catalog": {
            "path": str(catalog),
            "manifests": {
                filename: sha256(catalog / filename)
                for filename in (
                    "failure_channel_estimator_exception_cpu.manifest.json",
                    "failure_channel_estimator_invalid_cpu.manifest.json",
                    "failure_channel_estimator_deadline_cpu.manifest.json",
                    "failure_spectrumd_invalid_cpu.manifest.json",
                )
            },
        },
        "scenarios": scenarios,
        "health": client.request("health")["payload"]["health"],
        "teardown": {
            "terminal_jobs": len(jobs),
            "unloaded_instances": len(inventory["instances"]),
            "live_instances": 0,
        },
        "limitations": [
            "native deadline fallback is synchronous; asynchronous completion has deterministic component coverage",
            "dummy RU proves containment and management visibility, not production latency or RF correctness",
        ],
    }
    require(len(scenarios) == 7, f"failure gallery produced {len(scenarios)} scenarios instead of seven")
    publish_json_atomic(report_path, report)


def run(args):
    catalog = Path(args.catalog).resolve()
    preflight_helper = Path(args.preflight_helper).resolve()
    require(preflight_helper.is_file(), f"native preflight helper is missing: {preflight_helper}")
    required = {
        "ref_spectrum_cpu.manifest.json",
        "ref_srs_isac_cpu.manifest.json",
        "ref_spectrumd_cpu.manifest.json",
        "ref_srs_isacd_cpu.manifest.json",
        "ref_spectrum_observerd_cpu.manifest.json",
        "ref_scheduler_cpu.manifest.json",
    }
    if args.multicell_cpu:
        required.update(
            {
                "ref_channel_estimator_cpu.manifest.json",
                "ref_equalizer_cpu.manifest.json",
                "ref_receiver_cpu.manifest.json",
            }
        )
    if args.cuda_class_a:
        required.update(
            {
                "ref_channel_estimator_cuda.manifest.json",
                "ref_equalizer_cuda.manifest.json",
                "ref_receiver_cuda.manifest.json",
            }
        )
    if args.cuda_class_c:
        required.update(
            {
                "ref_spectrum_cuda.manifest.json",
                "ref_srs_isac_cuda.manifest.json",
                "ref_spectrumd_cuda.manifest.json",
                "ref_srs_isacd_cuda.manifest.json",
            }
        )
    if args.failure_gallery:
        required.update(
            {
                "failure_channel_estimator_exception_cpu.manifest.json",
                "failure_channel_estimator_invalid_cpu.manifest.json",
                "failure_channel_estimator_deadline_cpu.manifest.json",
                "failure_spectrumd_invalid_cpu.manifest.json",
            }
        )
    for filename in required:
        require((catalog / filename).is_file(), f"missing reference package manifest: {filename}")

    ready_path = Path(args.contention_ready_file).resolve() if args.contention_ready_file else None
    release_path = Path(args.contention_release_file).resolve() if args.contention_release_file else None
    result_path = Path(args.contention_result_file).resolve() if args.contention_result_file else None
    gallery_report_path = Path(args.failure_gallery_report).resolve() if args.failure_gallery_report else None
    if gallery_report_path is not None:
        require(not gallery_report_path.exists(), f"failure-gallery report already exists: {gallery_report_path}")
        require(gallery_report_path.parent.is_dir(), "failure-gallery report parent directory is missing")
    if ready_path is not None:
        require(not ready_path.exists(), f"contention-ready file already exists: {ready_path}")
        require(not release_path.exists(), f"contention-release file already exists: {release_path}")
        require(not result_path.exists(), f"contention-result file already exists: {result_path}")
        require(len({ready_path, release_path, result_path}) == 3, "contention coordination paths must be distinct")

    hold_cuda_class_c = args.cuda_contention or args.cuda_class_c_throughput
    with tempfile.TemporaryDirectory(prefix="ocudu-dapp-dummyru-") as temporary:
        root = Path(temporary)
        socket_path = root / "e3.sock"
        config_path = root / "du.yml"
        log_path = root / "du.log"
        write_config(
            config_path,
            socket_path,
            log_path,
            catalog,
            preflight_helper,
            args.multicell_cpu,
            args.multicell_cuda,
            args.failure_gallery,
            args.initial_startup,
            args.cuda_class_a,
            args.cuda_class_c,
            args.channel_bandwidth_mhz,
            args.srs_period_ms,
            args.time_scaling,
            Path(args.package_trust_store).resolve() if args.package_trust_store else None,
            args.minimum_trust_policy_epoch,
        )
        load_profile = {
            "cell_count": 2 if args.multicell_cuda else 1,
            "configured_pusch_concurrency": 2 if args.multicell_cuda else 0,
            "channel_bandwidth_mhz": args.channel_bandwidth_mhz,
            "common_scs_khz": 30,
            "time_scaling": args.time_scaling,
            "srs_period_ms": args.srs_period_ms,
        }
        environment = os.environ.copy()
        if args.cuda_class_a:
            environment["OCUDU_PUSCH_ENABLE_ACCELERATED_UCI"] = "1"
        process = subprocess.Popen(
            [str(Path(args.odu).resolve()), "-c", str(config_path)],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
            env=environment,
        )
        require(process.stdout is not None, "DU output pipe was not created")
        output = BoundedOutput(process.stdout)
        client = E3Client(Path(args.client).resolve(), Path(args.schema).resolve(), socket_path)
        try:
            initial_inventory = client.wait_ready(process)
            if args.package_trust_store:
                package_records = initial_inventory["payload"]["inventory"]["packages"]
                require(package_records, "signed catalog inventory is empty")
                for package in package_records:
                    require(package.get("signatureStatus") == "verified", f"package is not verified: {package}")
                    require(
                        fixed_unsigned(package.get("manifestSchemaVersion")) == 1,
                        f"package manifest schema is not v1: {package}",
                    )
                    sbom_digest = package.get("sbomSha256", "")
                    require(
                        isinstance(sbom_digest, str)
                        and sbom_digest.startswith("hex:")
                        and len(sbom_digest.removeprefix("hex:")) == 64,
                        f"package SBOM identity is invalid: {package}",
                    )
                    signing_key = package.get("signingKeyId", "")
                    require(
                        isinstance(signing_key, str)
                        and signing_key.startswith("hex:")
                        and len(signing_key.removeprefix("hex:")) == 64,
                        f"package signer identity is invalid: {package}",
                    )
                    require(fixed_unsigned(package.get("signatureSequence")) > 0, "package sequence is zero")
                    require(
                        fixed_unsigned(package.get("trustPolicyEpoch")) >= args.minimum_trust_policy_epoch,
                        "package trust-policy epoch is below the configured minimum",
                    )
            if args.initial_startup:
                packages = {
                    SPECTRUM_NATIVE,
                    SRS_ISAC_NATIVE,
                    SCHEDULER_NATIVE,
                    SPECTRUM_PROCESS,
                    SRS_ISAC_PROCESS,
                    SPECTRUM_OBSERVER_PROCESS,
                }
                assert_inventory(client, packages, 2, {"active", "shadow"})
                instances = initial_inventory["payload"]["inventory"]["instances"]
                spectrum = next(item for item in instances if item["packageId"] == SPECTRUM_NATIVE)
                srs_process = next(item for item in instances if item["packageId"] == SRS_ISAC_PROCESS)
                client.wait_metric(spectrum["instanceId"], "dapp.invoke.count", lambda value: value > 0)
                require(
                    client.metrics(srs_process["instanceId"])["class_c.route.publications"] == 0,
                    "shadow startup process published input before activation",
                )
                process_config = client.config(srs_process["instanceId"])
                require(process_config["generation"] == 1, f"unexpected startup config: {process_config}")
                config_generation, process_generation = client.set_enabled(
                    srs_process["instanceId"], process_config["generation"], False
                )
                require(config_generation == 2, "startup config disable did not advance its generation")
                config_generation, process_generation = client.set_enabled(
                    srs_process["instanceId"], config_generation, True
                )
                require(config_generation == 3, "startup config enable did not advance its generation")

                model_path = root / "startup-model.bin"
                model_path.write_bytes(b"OCUDU startup model fixture")
                model_generation = 1
                for action in ("stage", "validate", "warm", "activate"):
                    model_generation, process_generation = client.model(
                        srs_process["instanceId"],
                        model_generation,
                        action,
                        model_path if action == "stage" else None,
                    )
                require(model_generation == 5, "startup model lifecycle did not reach generation five")
                require(
                    client.metrics(srs_process["instanceId"])["class_c.route.publications"] == 0,
                    "shadow startup process published while management state was staged",
                )
                process_generation = client.lifecycle(srs_process["instanceId"], process_generation, "activate")
                client.wait_metric(
                    srs_process["instanceId"], "class_c.copy.snapshot.count", lambda value: value > 0
                )
                client.wait_metric(srs_process["instanceId"], "process.results_busy", lambda value: value > 0)
                client.unload(spectrum["instanceId"], spectrum["generation"])
                client.unload(srs_process["instanceId"], process_generation)
                assert_inventory(client, packages, 2, {"unloaded"})
                require(process.stdin is not None, "DU input pipe was not created")
                process.stdin.write("q\n")
                process.stdin.flush()
                process.wait(timeout=10)
                output.join()
                require(process.returncode == 0, f"DU did not shut down cleanly: {process.returncode}\n{output.text()}")
                return
            if args.failure_gallery:
                run_failure_gallery_campaign(client, process, catalog, gallery_report_path)
                require(process.stdin is not None, "DU input pipe was not created")
                process.stdin.write("q\n")
                process.stdin.flush()
                process.wait(timeout=10)
                output.join()
                require(
                    process.returncode == 0,
                    f"DU did not shut down cleanly: {process.returncode}\n{output.text()}",
                )
                log = log_path.read_text(errors="replace") if log_path.exists() else ""
                require(
                    "==== DU started ===" in output.text() or "==== DU started ===" in log,
                    "DU startup banner missing",
                )
                return
            if args.multicell_cpu:
                run_multicell_cpu_campaign(client, process, catalog)
                require(process.stdin is not None, "DU input pipe was not created")
                process.stdin.write("q\n")
                process.stdin.flush()
                process.wait(timeout=10)
                output.join()
                require(
                    process.returncode == 0,
                    f"DU did not shut down cleanly: {process.returncode}\n{output.text()}",
                )
                log = log_path.read_text(errors="replace") if log_path.exists() else ""
                require(
                    "==== DU started ===" in output.text() or "==== DU started ===" in log,
                    "DU startup banner missing",
                )
                return
            packages = {
                SPECTRUM_NATIVE,
                SRS_ISAC_NATIVE,
                SCHEDULER_NATIVE,
                SPECTRUM_PROCESS,
                SRS_ISAC_PROCESS,
                SPECTRUM_OBSERVER_PROCESS,
            }
            if args.cuda_class_a:
                packages.update({CHANNEL_ESTIMATOR_CUDA, EQUALIZER_CUDA, RECEIVER_CUDA})
            if args.cuda_class_c:
                packages.update(
                    {SPECTRUM_CUDA_NATIVE, SRS_ISAC_CUDA_NATIVE, SPECTRUM_CUDA_PROCESS, SRS_ISAC_CUDA_PROCESS}
                )
            assert_inventory(client, packages, 0)

            contention_receiver = None
            expected_cell_bitmap = 0b11 if args.multicell_cuda else 0b1

            def require_expected_cells(instance_id, timeout=30):
                return client.wait_metric(
                    instance_id,
                    "host.input.cell_bitmap",
                    lambda value: value == expected_cell_bitmap,
                    timeout=timeout,
                )

            cuda_process_exit_count = 0
            cuda_srs_process_exit_count = 0
            if args.cuda_class_a:
                for package_id in (CHANNEL_ESTIMATOR_CUDA, EQUALIZER_CUDA, RECEIVER_CUDA):
                    class_a_id, class_a_generation = client.activate(package_id, "in-process", "cuda")
                    try:
                        invocations = client.wait_metric(class_a_id, "dapp.invoke.count", lambda value: value > 0)
                        client.wait_metric(class_a_id, "dapp.input.max_layers", lambda value: value == 1)
                        require_expected_cells(class_a_id)
                        native_workers = client.wait_metric(
                            class_a_id,
                            "host.native_workers",
                            lambda value: value == 4 if args.multicell_cuda else value > 0,
                        )
                        active_native_workers = client.wait_metric(
                            class_a_id,
                            "host.native_workers_active",
                            lambda value: value == 2 if args.multicell_cuda else value > 0,
                        )
                        require(
                            not args.multicell_cuda or (native_workers == 4 and active_native_workers == 2),
                            f"CUDA Class-A package {package_id} did not bind four contexts and advance two workers",
                        )
                    except RuntimeError as error:
                        raise RuntimeError(
                            f"CUDA Class-A package {package_id} was not invoked: {error}; "
                            f"metrics={client.metrics(class_a_id)}"
                        ) from error
                    if package_id == RECEIVER_CUDA:
                        model_tool = catalog / "make_receiver_cuda_model"
                        model_path = root / "receiver-model.bin"
                        require(model_tool.is_file(), f"receiver model generator is missing: {model_tool}")
                        generated = subprocess.run(
                            [str(model_tool), str(model_path), "0.75"], text=True, capture_output=True, timeout=5
                        )
                        require(
                            generated.returncode == 0,
                            f"receiver model generation failed: {generated.stderr or generated.stdout}",
                        )
                        model_generation = 1
                        for action in ("stage", "validate", "warm", "activate"):
                            model_generation, class_a_generation = client.model(
                                class_a_id,
                                model_generation,
                                action,
                                model_path if action == "stage" else None,
                            )
                        require(model_generation == 5, "CUDA receiver model lifecycle did not reach generation five")
                        client.wait_metric(class_a_id, "dapp.invoke.count", lambda value: value > invocations)
                    class_a_generation = client.instance_generation(class_a_id)
                    if args.cuda_contention and package_id == RECEIVER_CUDA:
                        contention_receiver = (class_a_id, class_a_generation)
                    else:
                        client.unload(class_a_id, class_a_generation)

            if args.cuda_class_c:
                cuda_srs_id, cuda_srs_generation = client.activate(SRS_ISAC_CUDA_NATIVE, "in-process", "cuda")
                client.wait_metric(cuda_srs_id, "dapp.invoke.count", lambda value: value > 0)
                require_expected_cells(cuda_srs_id)
                client.unload(cuda_srs_id, cuda_srs_generation)

                cuda_srs_process_id, cuda_srs_process_generation = client.activate(
                    SRS_ISAC_CUDA_PROCESS, "process", "cuda"
                )
                srs_export_count = client.wait_metric(
                    cuda_srs_process_id, "class_c.copy.export.count", lambda value: value > 0
                )
                srs_result_count = client.wait_metric(
                    cuda_srs_process_id, "process.results_busy", lambda value: value > 0
                )
                require_expected_cells(cuda_srs_process_id)
                cuda_srs_process_metrics = client.metrics(cuda_srs_process_id)
                require(
                    cuda_srs_process_metrics["class_c.copy.export.bytes"] > 0,
                    "CUDA process SRS exported no device bytes",
                )
                require(
                    cuda_srs_process_metrics["class_c.copy.snapshot.count"] == 0,
                    "CUDA process SRS unexpectedly used the host snapshot path",
                )
                require(
                    cuda_srs_process_metrics["process.result_protocol_failures"] == 0,
                    "CUDA process SRS emitted an invalid result",
                )
                cuda_srs_worker_pid = cuda_srs_process_metrics["process.pid"]
                require_worker_child(cuda_srs_worker_pid, process.pid, catalog / "ref_srs_isacd_cuda")
                for cycle in range(1, args.cuda_contention_cycles + 1):
                    previous_pid = cuda_srs_worker_pid
                    os.kill(previous_pid, signal.SIGKILL)
                    client.wait_metric(cuda_srs_process_id, "process.exits", lambda value: value >= cycle)
                    client.wait_metric(
                        cuda_srs_process_id, "process.successful_restarts", lambda value: value >= cycle
                    )
                    cuda_srs_worker_pid = client.wait_metric(
                        cuda_srs_process_id, "process.pid", lambda value: value > 0 and value != previous_pid
                    )
                    require_worker_child(cuda_srs_worker_pid, process.pid, catalog / "ref_srs_isacd_cuda")
                    srs_export_count = client.wait_metric(
                        cuda_srs_process_id,
                        "class_c.copy.export.count",
                        lambda value: value > srs_export_count,
                    )
                    srs_result_count = client.wait_metric(
                        cuda_srs_process_id, "process.results_busy", lambda value: value > srs_result_count
                    )
                    require_expected_cells(cuda_srs_process_id)
                cuda_srs_incidents = client.request(
                    "incidents", "list", "--instance", cuda_srs_process_id, "--limit", "128"
                )
                cuda_srs_process_exit_count = sum(
                    record["recurrenceCount"]
                    for record in cuda_srs_incidents["payload"]["incidents"]["incidents"]
                    if record["cause"] == "processExit"
                )
                require(
                    cuda_srs_process_exit_count == args.cuda_contention_cycles,
                    f"expected {args.cuda_contention_cycles} CUDA SRS process exits, "
                    f"got {cuda_srs_process_exit_count}",
                )
                if not hold_cuda_class_c:
                    client.unload(
                        cuda_srs_process_id, client.instance_generation(cuda_srs_process_id)
                    )

                receiver_before = 0
                if contention_receiver is not None:
                    receiver_before = client.metrics(contention_receiver[0])["dapp.invoke.count"]
                cuda_native_id, cuda_native_generation = client.activate(
                    SPECTRUM_CUDA_NATIVE, "in-process", "cuda"
                )
                client.wait_metric(cuda_native_id, "dapp.invoke.count", lambda value: value > 0)
                require_expected_cells(cuda_native_id)
                if contention_receiver is not None:
                    client.wait_metric(
                        contention_receiver[0], "dapp.invoke.count", lambda value: value > receiver_before
                    )
                client.unload(cuda_native_id, cuda_native_generation)

                receiver_before_process = 0
                if contention_receiver is not None:
                    receiver_before_process = client.metrics(contention_receiver[0])["dapp.invoke.count"]
                cuda_process_id, cuda_process_generation = client.activate(
                    SPECTRUM_CUDA_PROCESS, "process", "cuda"
                )
                export_count = client.wait_metric(
                    cuda_process_id, "class_c.copy.export.count", lambda value: value > 0
                )
                accepted_count = client.wait_metric(
                    cuda_process_id, "process.results_accepted", lambda value: value > 0
                )
                require_expected_cells(cuda_process_id)
                cuda_process_metrics = client.metrics(cuda_process_id)
                require(
                    cuda_process_metrics["class_c.copy.export.bytes"] > 0,
                    "CUDA process spectrum exported no device bytes",
                )
                require(
                    cuda_process_metrics["class_c.copy.snapshot.count"] == 0,
                    "CUDA process spectrum unexpectedly used the host snapshot path",
                )
                cuda_worker_pid = cuda_process_metrics["process.pid"]
                require_worker_child(cuda_worker_pid, process.pid, catalog / "ref_spectrumd_cuda")
                for cycle in range(1, args.cuda_contention_cycles + 1):
                    previous_pid = cuda_worker_pid
                    os.kill(previous_pid, signal.SIGKILL)
                    client.wait_metric(cuda_process_id, "process.exits", lambda value: value >= cycle)
                    client.wait_metric(
                        cuda_process_id, "process.successful_restarts", lambda value: value >= cycle
                    )
                    cuda_worker_pid = client.wait_metric(
                        cuda_process_id, "process.pid", lambda value: value > 0 and value != previous_pid
                    )
                    require_worker_child(cuda_worker_pid, process.pid, catalog / "ref_spectrumd_cuda")
                    export_count = client.wait_metric(
                        cuda_process_id, "class_c.copy.export.count", lambda value: value > export_count
                    )
                    accepted_count = client.wait_metric(
                        cuda_process_id, "process.results_accepted", lambda value: value > accepted_count
                    )
                    require_expected_cells(cuda_process_id)
                cuda_incidents = client.request(
                    "incidents", "list", "--instance", cuda_process_id, "--limit", "128"
                )
                cuda_process_exit_count = sum(
                    record["recurrenceCount"]
                    for record in cuda_incidents["payload"]["incidents"]["incidents"]
                    if record["cause"] == "processExit"
                )
                require(
                    cuda_process_exit_count == args.cuda_contention_cycles,
                    f"expected {args.cuda_contention_cycles} CUDA process exits, got {cuda_process_exit_count}",
                )

                if contention_receiver is not None:
                    receiver_after_churn = client.wait_metric(
                        contention_receiver[0], "dapp.invoke.count", lambda value: value > receiver_before_process
                    )
                    require(
                        receiver_after_churn > receiver_before_process,
                        "Class-A receiver stopped during CUDA Class-C process churn",
                    )

                if ready_path is not None:
                    held_instances = {
                        "srs_isac_process": {
                            "instance_id": cuda_srs_process_id,
                            "metrics": client.metrics(cuda_srs_process_id),
                        },
                        "spectrum_process": {
                            "instance_id": cuda_process_id,
                            "metrics": client.metrics(cuda_process_id),
                        },
                    }
                    if contention_receiver is not None:
                        held_instances["receiver"] = {
                            "instance_id": contention_receiver[0],
                            "metrics": client.metrics(contention_receiver[0]),
                        }
                    ready_schema = (
                        CONTENTION_READY_SCHEMA
                        if args.cuda_contention
                        else CLASS_C_THROUGHPUT_READY_SCHEMA
                    )
                    publish_contention_record(
                        ready_path,
                        ready_schema,
                        process,
                        args.cuda_contention_cycles,
                        held_instances,
                        load_profile,
                    )
                    wait_for_contention_release(release_path, process, args.contention_hold_timeout_seconds)
                    final_instances = {}
                    for name, instance in held_instances.items():
                        after = client.metrics(instance["instance_id"])
                        progress_metric = (
                            "dapp.invoke.count" if name == "receiver" else "class_c.copy.export.count"
                        )
                        require(
                            after[progress_metric] > instance["metrics"][progress_metric],
                            f"CUDA contention instance stopped while held: {name}",
                        )
                        require(
                            after["host.input.cell_bitmap"] == expected_cell_bitmap,
                            f"CUDA contention instance lost exact cell coverage while held: {name}",
                        )
                        final_instances[name] = {"instance_id": instance["instance_id"], "metrics": after}
                    process_after = client.metrics(cuda_process_id)
                    require(
                        process_after["process.pid"] == cuda_worker_pid,
                        "CUDA process worker restarted unexpectedly during the contention measurement",
                    )
                    srs_process_after = client.metrics(cuda_srs_process_id)
                    require(
                        srs_process_after["process.pid"] == cuda_srs_worker_pid,
                        "CUDA SRS process worker restarted unexpectedly during the contention measurement",
                    )
                    result_schema = (
                        CONTENTION_RESULT_SCHEMA
                        if args.cuda_contention
                        else CLASS_C_THROUGHPUT_RESULT_SCHEMA
                    )
                    publish_contention_record(
                        result_path,
                        result_schema,
                        process,
                        args.cuda_contention_cycles,
                        final_instances,
                        load_profile,
                    )

                cuda_process_generation = client.instance_generation(cuda_process_id)
                client.unload(cuda_process_id, cuda_process_generation)
                if hold_cuda_class_c:
                    client.unload(
                        cuda_srs_process_id, client.instance_generation(cuda_srs_process_id)
                    )

                if contention_receiver is not None:
                    receiver_generation = client.instance_generation(contention_receiver[0])
                    client.unload(contention_receiver[0], receiver_generation)
                    contention_receiver = None

            srs_native_id, srs_native_generation = client.activate(SRS_ISAC_NATIVE, "in-process")
            client.wait_metric(srs_native_id, "dapp.invoke.count", lambda value: value > 0)
            client.unload(srs_native_id, srs_native_generation)

            srs_process_id, srs_process_generation = client.activate(SRS_ISAC_PROCESS, "process")
            client.wait_metric(srs_process_id, "class_c.copy.snapshot.count", lambda value: value > 0)
            client.wait_metric(srs_process_id, "process.results_busy", lambda value: value > 0)
            srs_process_metrics = client.metrics(srs_process_id)
            require(
                srs_process_metrics["process.results_accepted"] == 0,
                "SRS process result was accepted without a configured report sink",
            )
            require(
                srs_process_metrics["process.result_protocol_failures"] == 0,
                "SRS process emitted an invalid result",
            )
            require_worker_child(
                srs_process_metrics["process.pid"], process.pid, catalog / "ref_srs_isacd_cpu"
            )
            client.unload(srs_process_id, srs_process_generation)

            scheduler_id, scheduler_generation = client.activate(SCHEDULER_NATIVE, "in-process")
            native_id, native_generation = client.activate(SPECTRUM_NATIVE, "in-process")
            observer_id, observer_generation = client.activate(SPECTRUM_OBSERVER_PROCESS, "process")
            client.wait_metric(scheduler_id, "dapp.invoke.count", lambda value: value > 0)
            client.wait_metric(scheduler_id, "class_b.policy.invocations", lambda value: value > 0)
            client.wait_metric(scheduler_id, "class_b.policy.accepted", lambda value: value > 0)
            require(
                client.metrics(scheduler_id)["class_b.policy.evaluation_accepted"] == 0,
                "authoritative scheduler unexpectedly ran in evaluation-only mode",
            )
            client.wait_metric(native_id, "dapp.invoke.count", lambda value: value > 0)
            client.wait_metric(observer_id, "class_c.copy.snapshot.count", lambda value: value > 0)
            observer_metrics = client.metrics(observer_id)
            require(observer_metrics["process.results_accepted"] == 0, "observer unexpectedly published a result")
            require(
                observer_metrics["process.result_protocol_failures"] == 0,
                "observer emitted a prohibited control result",
            )
            require(observer_metrics["process.successful_restarts"] == 0, "observer restarted unexpectedly")
            observer_pid = observer_metrics["process.pid"]
            require_worker_child(observer_pid, process.pid, catalog / "ref_spectrum_observerd_cpu")
            health = client.request("health")["payload"]["health"]
            if args.cuda_class_a:
                require(
                    health["total"]
                    == health["deadlineMisses"] + cuda_process_exit_count + cuda_srs_process_exit_count
                    and health["fallbacks"] <= health["deadlineMisses"],
                    f"CUDA health contained an unexplained non-deadline failure: {health}",
                )
                require(
                    health["deviceErrors"] == 0
                    and health["invalidInputs"] == 0
                    and health["invalidOutputs"] == 0
                    and health["standardExceptions"] == 0
                    and health["unknownExceptions"] == 0,
                    f"CUDA Class-A health reported an unsafe failure: {health}",
                )
            elif args.cuda_class_c:
                require(
                    health["total"]
                    == cuda_process_exit_count + cuda_srs_process_exit_count + health["invalidOutputs"]
                    and health["deviceErrors"] == 0
                    and health["invalidInputs"] == 0
                    and health["standardExceptions"] == 0
                    and health["unknownExceptions"] == 0,
                    f"CUDA Class-C health contained an unexplained failure: {health}",
                )
            else:
                require(
                    health["total"] == 0 and health["deadlineMisses"] == 0,
                    f"unexpected native health: {health}",
                )

            conflicting_id, conflicting_generation = client.shadow(SPECTRUM_PROCESS, "process")
            rejected = client.fail_activation(conflicting_id, conflicting_generation)
            require(rejected["code"] == "lifecycleFailed", f"unexpected authority rejection: {rejected}")
            require(
                "another authority is active" in rejected.get("detail", ""),
                f"authority rejection was not diagnostic: {rejected}",
            )
            client.unload(conflicting_id, rejected["instanceGeneration"])
            client.unload(native_id, native_generation)

            process_id, process_generation = client.activate(SPECTRUM_PROCESS, "process")
            observer_before = client.metrics(observer_id)["class_c.copy.snapshot.count"]
            client.wait_metric(observer_id, "class_c.copy.snapshot.count", lambda value: value > observer_before)
            observer_metrics = client.metrics(observer_id)
            require(observer_metrics["process.results_accepted"] == 0, "observer gained result authority")
            require(observer_metrics["process.result_protocol_failures"] == 0, "observer violated its capability set")
            config = client.config(process_id)
            require(config["generation"] == 1, f"unexpected initial process config: {config}")
            config_generation, process_generation = client.set_enabled(process_id, 1, False)
            require(config_generation == 2, "disabling process dApp did not advance config generation")
            config_generation, process_generation = client.set_enabled(process_id, 2, True)
            require(config_generation == 3, "reenabling process dApp did not advance config generation")

            model_path = root / "model.bin"
            model_path.write_bytes(b"OCUDU dApp model fixture")
            model_generation = 1
            for action in ("stage", "validate", "warm", "activate"):
                model_generation, process_generation = client.model(
                    process_id,
                    model_generation,
                    action,
                    model_path if action == "stage" else None,
                )
            require(model_generation == 5, "process model lifecycle did not reach generation five")

            accepted_before = client.wait_metric(process_id, "process.results_accepted", lambda value: value > 0)
            client.wait_metric(process_id, "class_c.copy.snapshot.count", lambda value: value > 0)
            client.wait_metric(process_id, "class_c.copy.snapshot.bytes", lambda value: value > 0)
            scheduler_before = client.metrics(scheduler_id)["dapp.invoke.count"]
            worker_pid = client.wait_metric(process_id, "process.pid", lambda value: value > 0)
            require_worker_child(worker_pid, process.pid, catalog / "ref_spectrumd_cpu")
            os.kill(worker_pid, signal.SIGKILL)
            client.wait_metric(process_id, "process.exits", lambda value: value >= 1)
            client.wait_metric(process_id, "process.successful_restarts", lambda value: value >= 1)
            client.wait_metric(process_id, "process.management_replays", lambda value: value == 6)
            client.wait_metric(process_id, "process.pid", lambda value: value > 0 and value != worker_pid)
            client.wait_metric(process_id, "process.results_accepted", lambda value: value > accepted_before)
            client.wait_metric(scheduler_id, "dapp.invoke.count", lambda value: value > scheduler_before)
            incidents = client.request("incidents", "list", "--instance", process_id, "--limit", "16")
            records = incidents["payload"]["incidents"]["incidents"]
            process_exit_reported = any(record["cause"] == "processExit" for record in records)
            require(process_exit_reported, "process exit incident was not reported")

            restarted_generation = client.instance_generation(process_id)
            require(restarted_generation > process_generation, "worker restart did not advance instance generation")
            process_generation = restarted_generation
            config = client.config(process_id)
            require(config["generation"] == 3, f"process config generation was not replayed: {config}")
            require(
                config["entries"][0]["value"]["booleanValue"] is True,
                f"process enabled state was not replayed: {config}",
            )
            model_generation, process_generation = client.model(
                process_id, 5, "retire", dry_run=True
            )
            require(model_generation == 5, "dry-run model probe changed the replayed generation")
            client.unload(process_id, process_generation)
            client.unload(observer_id, observer_generation)
            client.unload(scheduler_id, scheduler_generation)
            expected_instances = 7 + (3 if args.cuda_class_a else 0) + (4 if args.cuda_class_c else 0)
            assert_inventory(client, packages, expected_instances, {"unloaded"})
        except Exception as error:
            log = log_path.read_text(errors="replace") if log_path.exists() else ""
            detail = f"{error}\n--- DU output ---\n{output.text()}\n--- DU log ---\n{log}"
            raise RuntimeError(detail) from error
        finally:
            if process.poll() is None and process.stdin is not None:
                try:
                    process.stdin.write("q\n")
                    process.stdin.flush()
                except BrokenPipeError:
                    pass
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
            output.join()
        require(process.returncode == 0, f"DU did not shut down cleanly: {process.returncode}\n{output.text()}")
        log = log_path.read_text(errors="replace") if log_path.exists() else ""
        require("==== DU started ===" in output.text() or "==== DU started ===" in log, "DU startup banner missing")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--odu", required=True)
    parser.add_argument("--client", required=True)
    parser.add_argument("--schema", required=True)
    parser.add_argument("--catalog", required=True)
    parser.add_argument("--preflight-helper", required=True)
    parser.add_argument("--multicell-cpu", action="store_true")
    parser.add_argument("--multicell-cuda", action="store_true")
    parser.add_argument("--failure-gallery", action="store_true")
    parser.add_argument("--failure-gallery-report")
    parser.add_argument("--initial-startup", action="store_true")
    parser.add_argument("--cuda-class-a", action="store_true")
    parser.add_argument("--cuda-class-c", action="store_true")
    parser.add_argument("--cuda-contention", action="store_true")
    parser.add_argument("--cuda-class-c-throughput", action="store_true")
    parser.add_argument("--cuda-contention-cycles", type=int, default=1)
    parser.add_argument("--contention-ready-file")
    parser.add_argument("--contention-release-file")
    parser.add_argument("--contention-result-file")
    parser.add_argument("--contention-hold-timeout-seconds", type=int, default=1800)
    parser.add_argument("--channel-bandwidth-mhz", type=int, default=20)
    parser.add_argument("--srs-period-ms", type=int, default=20)
    parser.add_argument("--time-scaling", type=int, default=10)
    parser.add_argument("--package-trust-store")
    parser.add_argument("--minimum-trust-policy-epoch", type=int, default=0)
    args = parser.parse_args()
    coordination = (
        bool(args.contention_ready_file),
        bool(args.contention_release_file),
        bool(args.contention_result_file),
    )
    require(not args.cuda_contention or (args.cuda_class_a and args.cuda_class_c),
            "--cuda-contention requires --cuda-class-a and --cuda-class-c")
    require(
        not args.multicell_cpu
        or not (args.cuda_class_a or args.cuda_class_c or args.cuda_contention or args.cuda_class_c_throughput),
        "--multicell-cpu is a CPU-only correctness campaign",
    )
    require(
        not args.multicell_cuda
        or (args.cuda_class_a and args.cuda_class_c and args.cuda_contention and not args.cuda_class_c_throughput),
        "--multicell-cuda requires the Class-A/Class-C CUDA contention campaign",
    )
    require(not (args.multicell_cpu and args.multicell_cuda), "CPU and CUDA multicell modes are mutually exclusive")
    require(
        args.failure_gallery == bool(args.failure_gallery_report),
        "--failure-gallery and --failure-gallery-report must be provided together",
    )
    require(
        not args.failure_gallery
        or not (
            args.multicell_cpu
            or args.multicell_cuda
            or args.cuda_class_a
            or args.cuda_class_c
            or args.cuda_contention
            or args.cuda_class_c_throughput
        ),
        "--failure-gallery is an independent CPU containment campaign",
    )
    require(
        not args.initial_startup
        or not (
            args.multicell_cpu
            or args.multicell_cuda
            or args.failure_gallery
            or args.cuda_class_a
            or args.cuda_class_c
            or args.cuda_contention
            or args.cuda_class_c_throughput
        ),
        "--initial-startup is an independent CPU lifecycle campaign",
    )
    require(
        not args.cuda_class_c_throughput or (args.cuda_class_c and not args.cuda_contention),
        "--cuda-class-c-throughput requires --cuda-class-c and excludes --cuda-contention",
    )
    require(
        all(coordination) or not any(coordination),
        "contention ready, release, and result files must be provided together",
    )
    require(
        not args.contention_ready_file or args.cuda_contention or args.cuda_class_c_throughput,
        "contention coordination requires a CUDA hold mode",
    )
    require(1 <= args.cuda_contention_cycles <= 100, "--cuda-contention-cycles must be in [1, 100]")
    require(
        args.channel_bandwidth_mhz in {20, 40, 50, 60, 80, 90, 100},
        "--channel-bandwidth-mhz is not a supported 30-kHz FR1 profile",
    )
    require(args.srs_period_ms in {5, 10, 20, 40, 80, 160, 320, 640, 1280}, "unsupported SRS period")
    require(args.time_scaling in {1, 10}, "--time-scaling must be 1 or 10")
    require(
        bool(args.package_trust_store) == (args.minimum_trust_policy_epoch > 0),
        "signed catalog requires both --package-trust-store and a positive --minimum-trust-policy-epoch",
    )
    require(
        1 <= args.contention_hold_timeout_seconds <= 86400,
        "--contention-hold-timeout-seconds must be in [1, 86400]",
    )
    run(args)


if __name__ == "__main__":
    main()
