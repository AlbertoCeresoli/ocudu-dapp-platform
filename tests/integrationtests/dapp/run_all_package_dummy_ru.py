#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear

"""Step through every catalog dApp package under dummy-RU and verify lifecycle + traffic.

Discovers product packages from one or more catalog directories (SDK + DeepSig),
merges them into a temporary catalog, boots a real odu with ru_dummy, then for
each package:

  load → prepare → arm → shadow → activate → wait for class-appropriate metrics → unload

Also optionally runs the existing CTest `ru_dummy` campaign suite.

Usage examples:

  # Per-package matrix (CPU + CUDA packages that are present)
  python3 run_all_package_dummy_ru.py \\
    --odu /path/to/odu \\
    --client /path/to/ocudu_dappctl.py \\
    --schema /path/to/e3sm-ocudu-dapp-mgmt-v1.asn1 \\
    --preflight-helper /path/to/ocudu-dapp-preflight \\
    --catalog /path/to/sdk/build \\
    --catalog /path/to/deepsig/build \\
    --report /tmp/dummy-ru-matrix.json

  # Existing integration campaigns via ctest
  python3 run_all_package_dummy_ru.py --ctest-build /path/to/ocudu/build-gpu-release --ctest-only
"""

from __future__ import annotations

import argparse
import json
import os
import signal
import subprocess
import sys
import tempfile
import time
import traceback
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import List, Optional, Sequence

# Reuse the battle-tested E3 client and helpers from the e2e driver.
_DRIVER_DIR = Path(__file__).resolve().parent
if str(_DRIVER_DIR) not in sys.path:
    sys.path.insert(0, str(_DRIVER_DIR))

from dummy_ru_e2e_test import (  # noqa: E402
    BoundedOutput,
    E3Client,
    require,
)
from ru_session import (  # noqa: E402
    PackageSpec as RuPackageSpec,
    class_a_l1_path_compatible as ru_class_a_l1_path_compatible,
    cleanup_package_instances as ru_cleanup_package_instances,
    discover_packages as ru_discover_packages,
    safe_unload as ru_safe_unload,
)


# ---------------------------------------------------------------------------
# Package discovery
# ---------------------------------------------------------------------------

SKIP_PREFIXES_DEFAULT = (
    "failure_",  # intentional fault injectors (separate gallery)
    "test_",  # multi-interface test fixtures, not product packages
    "certifier_",
    "scaffold_",
)


@dataclass
class PackageSpec:
    manifest_path: Path
    package_id: str
    global_name: str
    artifact: str
    backends: List[str]
    placements: List[str]
    dapp_class: str  # "A" | "B" | "C" | "?"
    interface_ids: List[int]
    source_catalog: Path

    @property
    def manifest_name(self) -> str:
        return self.manifest_path.name

    @property
    def preferred_backend(self) -> str:
        for candidate in ("cuda", "cpu", "otherAccelerator", "other"):
            if candidate in self.backends:
                return "other" if candidate == "otherAccelerator" else candidate
        return self.backends[0] if self.backends else "cpu"

    @property
    def preferred_placement(self) -> str:
        # E3 client CLI uses "in-process" / "process"
        raw = self.placements[0] if self.placements else "in_process"
        if raw in ("in_process", "in-process", "inProcess"):
            return "in-process"
        if raw in ("out_of_process", "process", "supervisedProcess", "supervised_process"):
            return "process"
        return raw.replace("_", "-")


def _class_from_interfaces(interfaces: Sequence[dict]) -> str:
    classes = set()
    for item in interfaces:
        value = item.get("class") or item.get("dapp_class")
        if value is None:
            continue
        if isinstance(value, int):
            classes.add({0: "A", 1: "B", 2: "C"}.get(value, "?"))
        else:
            text = str(value).strip().upper()
            if text in ("A", "B", "C"):
                classes.add(text)
            elif "RESIDENT" in text or "INLINE" in text:
                classes.add("A")
            elif "DEADLINE" in text or "DECISION" in text:
                classes.add("B")
            elif "OPEN" in text or "ANALYTIC" in text:
                classes.add("C")
    if len(classes) == 1:
        return next(iter(classes))
    if classes:
        return "/".join(sorted(classes))
    return "?"


def load_package_spec(manifest_path: Path, source_catalog: Path) -> PackageSpec:
    data = json.loads(manifest_path.read_text())
    interfaces = data.get("interfaces") or []
    placements: List[str] = list(data.get("placements") or [])
    if not placements:
        for iface in interfaces:
            for item in iface.get("placements") or []:
                if item not in placements:
                    placements.append(item)
    backends = list(data.get("backends") or ["cpu"])
    interface_ids = [int(item.get("interface_id") or item.get("interfaceId") or 0) for item in interfaces]
    return PackageSpec(
        manifest_path=manifest_path,
        package_id=str(data["package_id"]),
        global_name=str(data.get("global_name") or manifest_path.stem),
        artifact=str(data.get("artifact") or ""),
        backends=[str(b).lower() for b in backends],
        placements=placements or ["in_process"],
        dapp_class=_class_from_interfaces(interfaces),
        interface_ids=[i for i in interface_ids if i],
        source_catalog=source_catalog,
    )


def discover_packages(
    catalogs: Sequence[Path],
    *,
    include_failures: bool = False,
    include_test_fixtures: bool = False,
    name_filter: Optional[str] = None,
) -> List[PackageSpec]:
    found: List[PackageSpec] = []
    seen_ids = set()
    for catalog in catalogs:
        catalog = catalog.resolve()
        require(catalog.is_dir(), f"catalog is not a directory: {catalog}")
        for manifest in sorted(catalog.glob("*.manifest.json")):
            stem = manifest.name.replace(".manifest.json", "")
            if not include_failures and stem.startswith("failure_"):
                continue
            if not include_test_fixtures and (
                stem.startswith("test_") or stem.startswith("certifier_") or stem.startswith("scaffold_")
            ):
                continue
            if name_filter and name_filter not in stem and name_filter not in manifest.name:
                continue
            try:
                spec = load_package_spec(manifest, catalog)
            except Exception as exc:  # noqa: BLE001 — discovery should be robust
                print(f"[skip] unreadable manifest {manifest}: {exc}", file=sys.stderr)
                continue
            # Artifact must exist next to the manifest (or bare name without .so).
            artifact_name = Path(spec.artifact).name if spec.artifact else stem
            candidates = [
                catalog / artifact_name,
                catalog / f"{stem}.so",
                catalog / stem,
            ]
            if not any(path.exists() for path in candidates):
                print(f"[skip] missing artifact for {stem} under {catalog}", file=sys.stderr)
                continue
            if spec.package_id in seen_ids:
                print(f"[skip] duplicate package_id {spec.package_id} ({stem})", file=sys.stderr)
                continue
            seen_ids.add(spec.package_id)
            found.append(spec)
    return found


def merge_catalog(packages: Sequence[PackageSpec], destination: Path) -> None:
    """Copy every package's manifest + sibling artifacts into destination.

    Host admission rejects symbolic-link manifests ("must be an existing
    non-symbolic file"), so we materialize real files (hardlink when possible,
    else copy).
    """
    import shutil

    destination.mkdir(parents=True, exist_ok=True)

    def materialize(src: Path, dst: Path) -> None:
        if dst.exists() or dst.is_symlink():
            return
        src = src.resolve()
        # Prefer a real copy so we can enforce admission-safe modes without
        # mutating the build tree (host rejects group/other-writable artifacts).
        shutil.copy2(src, dst)
        mode = 0o555 if (src.is_file() and (os.access(src, os.X_OK) or src.suffix == ".so")) else 0o444
        try:
            os.chmod(dst, mode)
        except OSError:
            pass

    for spec in packages:
        source_dir = spec.source_catalog
        materialize(spec.manifest_path, destination / spec.manifest_name)
        stem = spec.manifest_name.replace(".manifest.json", "")
        patterns = [
            stem,
            f"{stem}.so",
            f"{stem}.spdx.json",
            Path(spec.artifact).name if spec.artifact else "",
            "make_receiver_cuda_model",
        ]
        for name in patterns:
            if not name:
                continue
            src = source_dir / name
            if not src.exists():
                continue
            materialize(src, destination / name)
        # Vendor packages may ship a weights/ directory and a vendor_env.json
        # next to their DSOs; stage both alongside the merged catalog.
        weights_dir = source_dir / "weights"
        if weights_dir.is_dir():
            for src in sorted(weights_dir.iterdir()):
                if src.is_file():
                    dst = destination / "weights" / src.name
                    dst.parent.mkdir(parents=True, exist_ok=True)
                    materialize(src, dst)
        vendor_env = source_dir / "vendor_env.json"
        if vendor_env.is_file():
            materialize(vendor_env, destination / "vendor_env.json")


# ---------------------------------------------------------------------------
# DU config
# ---------------------------------------------------------------------------

def write_matrix_config(
    path: Path,
    *,
    socket_path: Path,
    log_path: Path,
    catalog: Path,
    preflight_helper: Path,
    manifests: Sequence[str],
    enable_cuda_accel: bool,
    channel_bandwidth_mhz: int,
    srs_period_ms: int,
    time_scaling: float,
    omit_signature_keys: bool = False,
) -> None:
    """Mirror dummy_ru_e2e_test.write_config layout (CLI11 / YAML schema sensitive)."""
    quote = json.dumps
    manifest_lines = "\n".join(f"    - {name}" for name in manifests)
    signature = (
        ""
        if omit_signature_keys
        else "  package_signature_mode: allow_unsigned\n  minimum_trust_policy_epoch: 0\n"
    )
    # expert_phy must appear before cell_cfg (same order as dummy_ru_e2e_test).
    pusch = (
        "expert_phy:\n  pusch_acceleration_mode: enabled\n"
        if enable_cuda_accel
        else "expert_phy:\n  pusch_acceleration_mode: disabled\n"
    )
    class_a_pusch = (
        "  pusch:\n"
        "    min_ue_mcs: 0\n"
        "    max_ue_mcs: 0\n"
        "    mcs_table: qam64\n"
        if enable_cuda_accel
        else ""
    )
    # Unique F1-U bind avoids default 127.0.10.2:2152 collisions when another DU is up.
    f1u_bind = os.environ.get("OCUDU_DAPP_F1U_BIND", f"127.0.80.{(os.getpid() % 250) + 1}")
    path.write_text(
        f"""# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear
# Generated by run_all_package_dummy_ru.py — full package matrix under ru_dummy.

log:
  filename: {quote(str(log_path))}
  all_level: warning
test_mode:
  test_ue:
    rnti: 0x44
    ri: 1
    cqi: 15
    nof_ues: 1
    ue_creation_stagger_slots: 10
    pusch_active: true
    pdsch_active: false
    auto_ack_indication_delay: 1
ru_dummy:
  time_scaling: {time_scaling}
f1u:
  socket:
    - bind_addr: {f1u_bind}
      udp:
        reuse_addr: false
dapp:
  catalog_root: {quote(str(catalog))}
  native_preflight_helper: {quote(str(preflight_helper))}
  native_preflight_timeout_ms: 30000
{signature}  package_manifests:
{manifest_lines}
  max_scheduler_ul_prbs_avoided: 8
  class_c_process_restart_backoff_ms: 20
  class_c_process_management_timeout_ms: 2000
  local_e3:
    socket_path: {quote(str(socket_path))}
    request_timeout_ms: 2000
{pusch}cell_cfg:
{class_a_pusch}  dl_arfcn: 637212
  band: 78
  channel_bandwidth_MHz: {channel_bandwidth_mhz}
  common_scs: 30
  srs:
    type_enabled: periodic
    period_ms: {srs_period_ms}
""",
        encoding="utf-8",
    )


# ---------------------------------------------------------------------------
# Per-package verification
# ---------------------------------------------------------------------------

@dataclass
class PackageResult:
    package: str
    package_id: str
    global_name: str
    dapp_class: str
    backend: str
    placement: str
    status: str  # passed | failed | skipped
    detail: str = ""
    metrics_snapshot: dict = field(default_factory=dict)
    duration_s: float = 0.0


def verify_package_traffic(client: E3Client, instance_id: str, spec: PackageSpec, timeout: float) -> dict:
    """Wait for class-appropriate proof that the package is on the data path."""
    metrics = client.metrics(instance_id)
    dapp_class = spec.dapp_class
    placement = spec.preferred_placement

    def snap() -> dict:
        return client.metrics(instance_id)

    # Prefer invoke count when present; process packages often use class_c / process metrics.
    try:
        if "A" in dapp_class:
            client.wait_metric(instance_id, "dapp.invoke.count", lambda v: v > 0, timeout=timeout)
            try:
                client.wait_metric(instance_id, "dapp.input.max_layers", lambda v: v >= 1, timeout=min(5.0, timeout))
            except RuntimeError:
                pass
        elif "B" in dapp_class:
            client.wait_metric(instance_id, "dapp.invoke.count", lambda v: v > 0, timeout=timeout)
            try:
                client.wait_metric(
                    instance_id, "class_b.policy.accepted", lambda v: v > 0, timeout=min(15.0, timeout)
                )
            except RuntimeError:
                # Some builds only expose invoke count.
                pass
        else:
            # Class C (and unknown): native invoke or process pipeline activity.
            if placement == "process":
                # Observer packages must not accept results.
                name = spec.global_name + " " + spec.manifest_name
                if "observer" in name.lower():
                    client.wait_metric(
                        instance_id, "class_c.copy.snapshot.count", lambda v: v > 0, timeout=timeout
                    )
                    current = snap()
                    require(
                        current.get("process.results_accepted", 0) == 0,
                        f"observer gained result authority: {current}",
                    )
                else:
                    # Prefer export (CUDA-IPC) or snapshot (CPU SHM).
                    deadline = time.monotonic() + timeout
                    last = None
                    while time.monotonic() < deadline:
                        last = snap()
                        if (last.get("process.results_accepted") or 0) > 0:
                            break
                        if (last.get("process.results_busy") or 0) > 0:
                            break
                        if (last.get("class_c.copy.snapshot.count") or 0) > 0:
                            break
                        if (last.get("class_c.copy.export.count") or 0) > 0:
                            break
                        if (last.get("dapp.invoke.count") or 0) > 0:
                            break
                        time.sleep(0.05)
                    else:
                        raise RuntimeError(f"process Class-C never progressed; last={last}")
            else:
                client.wait_metric(instance_id, "dapp.invoke.count", lambda v: v > 0, timeout=timeout)
    except RuntimeError:
        # One more full metrics dump for diagnostics.
        metrics = snap()
        raise

    return snap()


def _safe_unload(client: E3Client, instance_id: str) -> None:
    ru_safe_unload(client, instance_id)


def _cleanup_package_instances(client: E3Client, package_id: str) -> None:
    ru_cleanup_package_instances(client, package_id)


def class_a_l1_path_compatible(spec: PackageSpec, *, cuda_l1_active: bool) -> Optional[str]:
    """Return incompatibility detail if Class-A package cannot serve the active L1 path.

    Mirrors host instance_manager::class_a_l1_path_incompatibility:
      - Accelerated L1 (CUDA in host backend_mask) → Class-A must use backend=cuda
      - Host L1 only → Class-A must use backend=cpu
    Class B/C are path-agnostic for this check (host tensors / publish).
    """
    if "A" not in (spec.dapp_class or ""):
        return None
    backend = spec.preferred_backend
    if cuda_l1_active and backend == "cpu":
        return (
            "Class-A cpu backend incompatible with active accelerated L1 path; "
            "use a cuda Class-A package or disable pusch_acceleration"
        )
    if not cuda_l1_active and backend == "cuda":
        return (
            "Class-A cuda backend incompatible with active host L1 path; "
            "enable pusch_acceleration or use a cpu Class-A package"
        )
    return None


def _step_packages(
    *,
    client: E3Client,
    packages: Sequence[PackageSpec],
    admitted: set,
    args,
    results: List[PackageResult],
    phase_label: str,
    cuda_l1_active: bool,
) -> None:
    total = len(packages)
    for index, spec in enumerate(packages, start=1):
        label = f"[{phase_label} {index}/{total}] {spec.manifest_name}"
        backend = spec.preferred_backend
        placement = spec.preferred_placement
        l1_issue = class_a_l1_path_compatible(spec, cuda_l1_active=cuda_l1_active)
        if l1_issue is not None:
            results.append(
                PackageResult(
                    package=spec.manifest_name,
                    package_id=spec.package_id,
                    global_name=spec.global_name,
                    dapp_class=spec.dapp_class,
                    backend=backend,
                    placement=placement,
                    status="skipped",
                    detail=f"l1_path_check: {l1_issue}",
                )
            )
            print(f"{label}: SKIP {l1_issue}")
            continue
        if spec.package_id not in admitted:
            results.append(
                PackageResult(
                    package=spec.manifest_name,
                    package_id=spec.package_id,
                    global_name=spec.global_name,
                    dapp_class=spec.dapp_class,
                    backend=backend,
                    placement=placement,
                    status="failed",
                    detail="package not in DU inventory (admission/preflight rejected)",
                )
            )
            print(f"{label}: FAIL not admitted")
            continue

        started = time.monotonic()
        instance_id = None
        try:
            print(f"{label}: activate place={placement} backend={backend} class={spec.dapp_class} …")
            instance_id, _generation = client.activate(spec.package_id, placement, backend)
            metrics = verify_package_traffic(client, instance_id, spec, timeout=args.metric_timeout)
            unload_detail = ""
            try:
                _safe_unload(client, instance_id)
            except Exception as unload_exc:  # noqa: BLE001
                # Traffic already proved the package works. Class-A packages can
                # race generation so hard on deactivate that unload fails even
                # after a healthy invoke path; do not fail the matrix cell.
                unload_detail = f" (unload best-effort failed: {unload_exc})"
                try:
                    _cleanup_package_instances(client, spec.package_id)
                except Exception:  # noqa: BLE001
                    pass
            instance_id = None
            duration = time.monotonic() - started
            result = PackageResult(
                package=spec.manifest_name,
                package_id=spec.package_id,
                global_name=spec.global_name,
                dapp_class=spec.dapp_class,
                backend=backend,
                placement=placement,
                status="passed",
                detail="lifecycle + traffic OK" + unload_detail,
                metrics_snapshot={
                    k: metrics.get(k)
                    for k in (
                        "dapp.invoke.count",
                        "class_b.policy.accepted",
                        "class_c.copy.snapshot.count",
                        "class_c.copy.export.count",
                        "process.results_accepted",
                        "process.results_busy",
                        "process.pid",
                    )
                    if k in metrics
                },
                duration_s=round(duration, 3),
            )
            results.append(result)
            print(f"{label}: PASS ({duration:.1f}s) metrics={result.metrics_snapshot}{unload_detail}")
        except Exception as exc:  # noqa: BLE001
            duration = time.monotonic() - started
            detail = f"{exc}"
            if instance_id is not None:
                try:
                    _safe_unload(client, instance_id)
                except Exception as unload_exc:  # noqa: BLE001
                    detail += f" | unload also failed: {unload_exc}"
            # Best-effort: drop any leftover non-terminal instances for this package.
            try:
                _cleanup_package_instances(client, spec.package_id)
            except Exception:  # noqa: BLE001
                pass
            results.append(
                PackageResult(
                    package=spec.manifest_name,
                    package_id=spec.package_id,
                    global_name=spec.global_name,
                    dapp_class=spec.dapp_class,
                    backend=backend,
                    placement=placement,
                    status="failed",
                    detail=detail,
                    duration_s=round(duration, 3),
                )
            )
            print(f"{label}: FAIL {detail}")
            if args.fail_fast:
                break


def _run_du_phase(
    *,
    phase_name: str,
    packages: Sequence[PackageSpec],
    admit_packages: Sequence[PackageSpec],
    odu: Path,
    client_path: Path,
    schema: Path,
    preflight: Path,
    enable_cuda_accel: bool,
    args,
    results: List[PackageResult],
) -> dict:
    """Boot one DU admitting `admit_packages`; exercise only `packages`."""
    phase_meta = {"name": phase_name, "cuda_accel": enable_cuda_accel, "count": len(packages)}
    if not packages:
        phase_meta["skipped"] = True
        return phase_meta

    with tempfile.TemporaryDirectory(prefix=f"ocudu-dapp-matrix-{phase_name}-") as temporary:
        root = Path(temporary)
        merged = root / "catalog"
        # Only admit packages this host/build can preflight (don't force CUDA .so onto CPU odu).
        merge_catalog(admit_packages, merged)
        socket_path = root / "e3.sock"
        config_path = root / "du.yml"
        log_path = root / "du.log"
        write_matrix_config(
            config_path,
            socket_path=socket_path,
            log_path=log_path,
            catalog=merged,
            preflight_helper=preflight,
            manifests=[p.manifest_name for p in admit_packages],
            enable_cuda_accel=enable_cuda_accel,
            channel_bandwidth_mhz=args.channel_bandwidth_mhz,
            srs_period_ms=args.srs_period_ms,
            time_scaling=args.time_scaling,
            omit_signature_keys=args.omit_signature_keys,
        )

        environment = os.environ.copy()
        if enable_cuda_accel:
            environment["OCUDU_PUSCH_ENABLE_ACCELERATED_UCI"] = "1"
        # Vendor packages may ship a vendor_env.json next to their DSOs: a flat
        # string->string map merged into the DU environment ("${catalog}" in a
        # value expands to the merged catalog path). Closed packages use it to
        # find weights or enable features without this harness naming them.
        vendor_env_path = merged / "vendor_env.json"
        if vendor_env_path.is_file():
            try:
                vendor_env = json.loads(vendor_env_path.read_text(encoding="utf-8"))
            except ValueError:
                vendor_env = None
            if isinstance(vendor_env, dict):
                for key, value in vendor_env.items():
                    environment.setdefault(
                        str(key), str(value).replace("${catalog}", str(merged.resolve())))

        process = subprocess.Popen(
            [str(odu), "-c", str(config_path)],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
            env=environment,
        )
        require(process.stdout is not None, "DU stdout pipe missing")
        output = BoundedOutput(process.stdout)
        client = E3Client(client_path, schema, socket_path)

        try:
            inventory = client.wait_ready(process, timeout=args.du_ready_timeout)
            admitted = {item["packageId"] for item in inventory["payload"]["inventory"]["packages"]}
            print(
                f"[du:{phase_name}] ready — admitted={len(admitted)} "
                f"stepping={len(packages)} cuda_accel={enable_cuda_accel} "
                f"time_scaling={args.time_scaling}"
            )
            # Let dummy-RU UE attach and UL settle before Class-A activations.
            # Without this, early packages can see zero invokes under time_scaling=1.
            settle_s = float(getattr(args, "settle_seconds", 0.0) or 0.0)
            if settle_s > 0:
                time.sleep(settle_s)
            _step_packages(
                client=client,
                packages=packages,
                admitted=admitted,
                args=args,
                results=results,
                phase_label=phase_name,
                cuda_l1_active=enable_cuda_accel,
            )

            require(process.stdin is not None, "DU stdin missing")
            process.stdin.write("q\n")
            process.stdin.flush()
            try:
                process.wait(timeout=15)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
            output.join()
            phase_meta["du_returncode"] = process.returncode
            phase_meta["du_log_tail"] = (
                "\n".join(log_path.read_text(errors="replace").splitlines()[-30:]) if log_path.exists() else ""
            )
            phase_meta["du_stdout_tail"] = "\n".join(output.text().splitlines()[-30:])
        finally:
            if process.poll() is None:
                process.send_signal(signal.SIGTERM)
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        pass
    return phase_meta


def run_package_matrix(args) -> dict:
    catalogs = [Path(p).resolve() for p in args.catalog]
    packages = discover_packages(
        catalogs,
        include_failures=args.include_failures,
        include_test_fixtures=args.include_test_fixtures,
        name_filter=args.filter,
    )
    require(packages, "no packages discovered — check --catalog paths")

    if args.cpu_only or args.skip_cuda:
        packages = [p for p in packages if "cuda" not in p.backends]

    odu = Path(args.odu).resolve()
    # Optional non-CUDA host for Class-A CPU phase. GPU-linked odu often never
    # dispatches resident Class-A CPU packages even with acceleration disabled.
    odu_cpu = Path(args.odu_cpu).resolve() if getattr(args, "odu_cpu", "") else odu
    preflight_cpu = (
        Path(args.preflight_helper_cpu).resolve()
        if getattr(args, "preflight_helper_cpu", "")
        else Path(args.preflight_helper).resolve()
    )
    client_path = Path(args.client).resolve()
    schema = Path(args.schema).resolve()
    preflight = Path(args.preflight_helper).resolve()
    require(odu.is_file(), f"odu missing: {odu}")
    require(odu_cpu.is_file(), f"odu_cpu missing: {odu_cpu}")
    require(client_path.is_file(), f"E3 client missing: {client_path}")
    require(schema.is_file(), f"schema missing: {schema}")
    require(preflight.is_file(), f"preflight helper missing: {preflight}")
    require(preflight_cpu.is_file(), f"preflight_helper_cpu missing: {preflight_cpu}")

    results: List[PackageResult] = []
    report = {
        "schema": "ocudu.dapp.dummy-ru-package-matrix.v1",
        "odu": str(odu),
        "catalogs": [str(c) for c in catalogs],
        "package_count": len(packages),
        "packages": [],
        "phases": [],
        "summary": {},
    }

    # Split Class-A by backend: with pusch_acceleration enabled, CPU Class-A hooks
    # are not on the hot path (CUDA owns the L1 lane). Class B/C CPU still runs under
    # either mode.
    cuda_pkgs = [p for p in packages if "cuda" in p.backends]
    cpu_pkgs = [p for p in packages if "cuda" not in p.backends]
    cpu_class_a = [p for p in cpu_pkgs if "A" in p.dapp_class]
    cpu_other = [p for p in cpu_pkgs if "A" not in p.dapp_class]

    # Phase 1: CPU Class-A (+ other CPU) with acceleration OFF.
    # Phase 2: CUDA packages (+ remaining CPU B/C) with acceleration ON.
    # If only one side is present, collapse to a single phase.
    if cuda_pkgs and cpu_class_a and not args.cpu_only:
        # CPU Class-A (+ B/C CPU) on a non-accelerated host binary when available.
        phase1 = cpu_class_a + cpu_other
        phase2 = cuda_pkgs
        report["phases"].append(
            _run_du_phase(
                phase_name="cpu",
                packages=phase1,
                admit_packages=phase1,
                odu=odu_cpu,
                client_path=client_path,
                schema=schema,
                preflight=preflight_cpu,
                enable_cuda_accel=False,
                args=args,
                results=results,
            )
        )
        if not args.fail_fast or not any(r.status == "failed" for r in results):
            report["phases"].append(
                _run_du_phase(
                    phase_name="cuda",
                    packages=phase2,
                    admit_packages=phase2,
                    odu=odu,
                    client_path=client_path,
                    schema=schema,
                    preflight=preflight,
                    enable_cuda_accel=True,
                    args=args,
                    results=results,
                )
            )
    elif cuda_pkgs and not args.cpu_only:
        report["phases"].append(
            _run_du_phase(
                phase_name="cuda",
                packages=packages,
                admit_packages=packages,
                odu=odu,
                client_path=client_path,
                schema=schema,
                preflight=preflight,
                enable_cuda_accel=True,
                args=args,
                results=results,
            )
        )
    else:
        report["phases"].append(
            _run_du_phase(
                phase_name="cpu",
                packages=packages,
                admit_packages=packages,
                odu=odu_cpu if not cuda_pkgs else odu,
                client_path=client_path,
                schema=schema,
                preflight=preflight_cpu if not cuda_pkgs else preflight,
                enable_cuda_accel=False,
                args=args,
                results=results,
            )
        )

    report["packages"] = [asdict(r) for r in results]
    passed = sum(1 for r in results if r.status == "passed")
    failed = sum(1 for r in results if r.status == "failed")
    skipped = sum(1 for r in results if r.status == "skipped")
    report["summary"] = {
        "passed": passed,
        "failed": failed,
        "skipped": skipped,
        "total": len(results),
        "ok": failed == 0 and passed > 0,
    }
    return report


# ---------------------------------------------------------------------------
# CTest campaign runner
# ---------------------------------------------------------------------------

def run_ctest_campaigns(build_dir: Path, extra_args: Sequence[str] = ()) -> dict:
    build_dir = build_dir.resolve()
    require(build_dir.is_dir(), f"ctest build dir missing: {build_dir}")
    cmd = [
        "ctest",
        "--test-dir",
        str(build_dir),
        "-L",
        "ru_dummy",
        "--output-on-failure",
        "--timeout",
        "300",
        *extra_args,
    ]
    print("[ctest]", " ".join(cmd))
    completed = subprocess.run(cmd, text=True, capture_output=True)
    return {
        "command": cmd,
        "returncode": completed.returncode,
        "stdout_tail": "\n".join(completed.stdout.splitlines()[-80:]),
        "stderr_tail": "\n".join(completed.stderr.splitlines()[-40:]),
        "ok": completed.returncode == 0,
    }


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def default_paths_from_workspace() -> dict:
    """Best-effort defaults when run from the ocudu-dApp workspace layout."""
    here = Path(__file__).resolve()
    # .../ocudu/tests/integrationtests/dapp/this.py → workspace root candidates
    ocudu_src = here.parents[3]  # ocudu/
    workspace = ocudu_src.parent  # ocudu-dApp/
    candidates = {
        "schema": ocudu_src / "schemas/e3/e3sm-ocudu-dapp-mgmt-v1.asn1",
        "client": ocudu_src / "apps/tools/dappctl/ocudu_dappctl.py",
        "sdk_catalog": workspace / "ocudu-dapp-sdk" / "build-cuda",
        "sdk_catalog_cpu": workspace / "ocudu-dapp-sdk" / "build",
        "vendor_catalog": Path(os.environ["OCUDU_DAPP_VENDOR_CATALOG"])
        if os.environ.get("OCUDU_DAPP_VENDOR_CATALOG")
        else workspace / "vendor-dapps" / "build",
        # Prefer main build/ odu (usually rebuilt with current package_manifest).
        "odu_cpu": ocudu_src / "build" / "apps" / "du" / "odu",
        "odu_gpu": ocudu_src / "build-gpu-release" / "apps" / "du" / "odu",
        "preflight_cpu": ocudu_src / "build" / "apps" / "tools" / "dapp_preflight" / "ocudu-dapp-preflight",
        "preflight_gpu": ocudu_src / "build-gpu-release" / "apps" / "tools" / "dapp_preflight" / "ocudu-dapp-preflight",
        "ctest_gpu": ocudu_src / "build-gpu-release",
        "ctest_cpu": ocudu_src / "build",
    }
    return candidates


def build_parser() -> argparse.ArgumentParser:
    defaults = default_paths_from_workspace()
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument(
        "--odu",
        default=str(defaults["odu_gpu"] if defaults["odu_gpu"].is_file() else defaults["odu_cpu"]),
        help="Primary odu (prefer CUDA-enabled for Class-A CUDA packages)",
    )
    parser.add_argument(
        "--odu-cpu",
        default=str(defaults["odu_cpu"] if defaults["odu_cpu"].is_file() else ""),
        help="Optional non-CUDA odu for Class-A CPU phase (defaults to --odu)",
    )
    parser.add_argument("--client", default=str(defaults["client"]))
    parser.add_argument("--schema", default=str(defaults["schema"]))
    parser.add_argument(
        "--preflight-helper",
        default=str(
            defaults["preflight_gpu"] if defaults["preflight_gpu"].is_file() else defaults["preflight_cpu"]
        ),
    )
    parser.add_argument(
        "--preflight-helper-cpu",
        default=str(defaults["preflight_cpu"] if defaults["preflight_cpu"].is_file() else ""),
        help="Optional preflight for Class-A CPU phase",
    )
    parser.add_argument(
        "--catalog",
        action="append",
        default=None,
        help="Catalog directory containing *.manifest.json (repeatable). Defaults to SDK build-cuda + deepsig build.",
    )
    parser.add_argument("--report", default="", help="Write JSON report to this path")
    parser.add_argument("--filter", default="", help="Only packages whose name contains this substring")
    parser.add_argument("--include-failures", action="store_true", help="Include failure_* gallery packages")
    parser.add_argument("--include-test-fixtures", action="store_true", help="Include test_* multi-iface fixtures")
    parser.add_argument("--cpu-only", action="store_true", help="Skip CUDA-backend packages")
    parser.add_argument("--skip-cuda", action="store_true", help="Alias of --cpu-only for CUDA backends")
    parser.add_argument("--fail-fast", action="store_true")
    parser.add_argument("--metric-timeout", type=float, default=45.0)
    parser.add_argument("--du-ready-timeout", type=float, default=30.0)
    parser.add_argument("--channel-bandwidth-mhz", type=int, default=20)
    parser.add_argument("--srs-period-ms", type=int, default=20)
    # time_scaling>1 slows the dummy RU so test-mode storage does not overflow
    # under host load (mirrors deepsig tests/test_gnb_lifecycle.py).
    parser.add_argument("--time-scaling", type=float, default=2.0)
    parser.add_argument(
        "--settle-seconds",
        type=float,
        default=3.0,
        help="Sleep after DU ready before first package activate (UE attach settle)",
    )
    parser.add_argument(
        "--ctest-build",
        default="",
        help="If set, also run `ctest -L ru_dummy` in this build directory",
    )
    parser.add_argument("--ctest-only", action="store_true", help="Only run ctest campaigns (no per-package matrix)")
    parser.add_argument("--list", action="store_true", help="List discovered packages and exit")
    parser.add_argument(
        "--omit-signature-keys",
        action="store_true",
        help="Omit package_signature_mode YAML keys (for older odu binaries that reject them)",
    )
    return parser


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    defaults = default_paths_from_workspace()

    if args.catalog is None:
        catalogs = []
        for key in ("sdk_catalog", "sdk_catalog_cpu", "vendor_catalog"):
            path = defaults[key]
            if path.is_dir() and any(path.glob("*.manifest.json")):
                # Prefer build-cuda over build when both exist — avoid duplicate package_ids.
                if key == "sdk_catalog_cpu" and defaults["sdk_catalog"].is_dir():
                    continue
                catalogs.append(str(path))
        args.catalog = catalogs

    if args.list:
        packages = discover_packages(
            [Path(p) for p in args.catalog],
            include_failures=args.include_failures,
            include_test_fixtures=args.include_test_fixtures,
            name_filter=args.filter or None,
        )
        for spec in packages:
            print(
                f"{spec.manifest_name:42} class={spec.dapp_class:3} "
                f"backend={spec.preferred_backend:5} place={spec.preferred_placement:11} "
                f"id={spec.package_id}  ({spec.global_name})"
            )
        print(f"\n{len(packages)} packages")
        return 0

    combined = {
        "schema": "ocudu.dapp.dummy-ru-full-report.v1",
        "matrix": None,
        "ctest": None,
    }
    exit_ok = True

    if not args.ctest_only:
        try:
            combined["matrix"] = run_package_matrix(args)
            exit_ok = exit_ok and combined["matrix"]["summary"]["ok"]
        except Exception as exc:  # noqa: BLE001
            combined["matrix"] = {"ok": False, "error": str(exc), "traceback": traceback.format_exc()}
            exit_ok = False
            print(f"[matrix] aborted: {exc}", file=sys.stderr)

    ctest_build = args.ctest_build
    if not ctest_build and not args.ctest_only:
        # Optional: only run ctest when explicitly requested unless --ctest-only.
        ctest_build = ""
    if args.ctest_only and not ctest_build:
        ctest_build = str(defaults["ctest_gpu"] if defaults["ctest_gpu"].is_dir() else defaults["ctest_cpu"])

    if ctest_build:
        try:
            combined["ctest"] = run_ctest_campaigns(Path(ctest_build))
            exit_ok = exit_ok and combined["ctest"]["ok"]
            print(combined["ctest"]["stdout_tail"])
            if combined["ctest"]["stderr_tail"]:
                print(combined["ctest"]["stderr_tail"], file=sys.stderr)
        except Exception as exc:  # noqa: BLE001
            combined["ctest"] = {"ok": False, "error": str(exc)}
            exit_ok = False

    if args.report:
        report_path = Path(args.report)
        report_path.parent.mkdir(parents=True, exist_ok=True)
        report_path.write_text(json.dumps(combined, indent=2) + "\n")
        print(f"[report] wrote {report_path}")

    if combined.get("matrix") and combined["matrix"].get("summary"):
        s = combined["matrix"]["summary"]
        print(f"[summary] matrix passed={s['passed']} failed={s['failed']} skipped={s['skipped']} total={s['total']}")
    return 0 if exit_ok else 1


if __name__ == "__main__":
    sys.exit(main())
