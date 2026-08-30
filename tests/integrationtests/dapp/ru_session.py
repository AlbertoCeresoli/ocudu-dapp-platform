#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear
"""Shared dummy-RU session helpers for package matrix / e2e drivers.

Keeps lifecycle traffic scripts concise without changing host behavior.
"""

from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path
from typing import List, Optional, Sequence

from dummy_ru_e2e_test import E3Client, require  # noqa: F401 — re-export


SKIP_PREFIXES_DEFAULT = (
    "failure_",
    "test_",
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
    dapp_class: str
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
        raw = self.placements[0] if self.placements else "in_process"
        if raw in ("in_process", "in-process", "inProcess"):
            return "in-process"
        if raw in ("out_of_process", "process", "supervisedProcess", "supervised_process"):
            return "process"
        return raw


def class_from_interfaces(interfaces: Sequence[dict]) -> str:
    classes = set()
    for iface in interfaces or []:
        raw = str(iface.get("dappClass") or iface.get("class") or "").lower()
        if "a" in raw or raw in ("1", "resident", "inline"):
            classes.add("A")
        elif "b" in raw or raw in ("2", "decision"):
            classes.add("B")
        elif "c" in raw or raw in ("3", "openloop", "open_loop"):
            classes.add("C")
    if not classes:
        return "?"
    return "".join(sorted(classes))


def load_package_spec(manifest_path: Path, source_catalog: Path) -> PackageSpec:
    data = json.loads(manifest_path.read_text(encoding="utf-8"))
    interfaces = data.get("interfaces") or []
    iface_ids = []
    for iface in interfaces:
        iid = iface.get("interfaceId") or iface.get("id")
        if isinstance(iid, str) and iid.startswith("0x"):
            iface_ids.append(int(iid, 16))
        elif isinstance(iid, int):
            iface_ids.append(iid)
    backends = list(data.get("backends") or ["cpu"])
    placements = list(data.get("placements") or data.get("placement") or ["in_process"])
    if isinstance(placements, str):
        placements = [placements]
    return PackageSpec(
        manifest_path=manifest_path,
        package_id=str(data.get("packageId") or data.get("id") or ""),
        global_name=str(data.get("globalName") or data.get("name") or manifest_path.stem),
        artifact=str(data.get("artifact") or data.get("so") or f"{manifest_path.stem}.so"),
        backends=[str(b) for b in backends],
        placements=[str(p) for p in placements],
        dapp_class=class_from_interfaces(interfaces) if interfaces else str(data.get("dappClass") or "?"),
        interface_ids=iface_ids,
        source_catalog=source_catalog,
    )


def discover_packages(
    catalogs: Sequence[Path],
    *,
    skip_prefixes: Sequence[str] = SKIP_PREFIXES_DEFAULT,
    include_failures: bool = False,
    include_test_fixtures: bool = False,
) -> List[PackageSpec]:
    found: List[PackageSpec] = []
    for catalog in catalogs:
        catalog = Path(catalog)
        if not catalog.is_dir():
            continue
        for manifest in sorted(catalog.glob("*.manifest.json")):
            stem = manifest.name.replace(".manifest.json", "")
            if not include_failures and stem.startswith("failure_"):
                continue
            if not include_test_fixtures and stem.startswith("test_"):
                continue
            if any(stem.startswith(p) for p in skip_prefixes) and not (
                include_failures and stem.startswith("failure_")
            ):
                # still skip certifier/scaffold unless explicitly wanted
                if stem.startswith(("certifier_", "scaffold_")):
                    continue
                if stem.startswith("test_") and not include_test_fixtures:
                    continue
                if stem.startswith("failure_") and not include_failures:
                    continue
            try:
                found.append(load_package_spec(manifest, catalog))
            except Exception:
                continue
    return found


def safe_unload(client: E3Client, instance_id: str) -> None:
    """Unload with fresh generation (Class-A traffic can advance gen under load)."""
    try:
        generation = client.instance_generation(instance_id)
    except Exception:
        return
    try:
        client.unload(instance_id, generation)
    except Exception:
        pass


def cleanup_package_instances(client: E3Client, package_id: str) -> None:
    inv = client.request("inventory")["payload"]["inventory"]
    for item in inv.get("instances") or []:
        if item.get("packageId") != package_id:
            continue
        state = item.get("state")
        if state in {"unloaded", "unloading"}:
            continue
        safe_unload(client, item["instanceId"])


def class_a_l1_path_compatible(spec: PackageSpec, *, cuda_l1_active: bool) -> Optional[str]:
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
