#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear

"""Hardware-independent tests for the CUDA dApp loaded-L1 soak protocol."""

import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
from types import SimpleNamespace
import sys
import tempfile
import unittest
from unittest import mock


def load_runner(path):
    spec = importlib.util.spec_from_file_location("cuda_dapp_soak", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class SoakProtocolTest(unittest.TestCase):
    @staticmethod
    def ready(schema=None, progress=0):
        return {
            "SPDX-FileCopyrightText": "Copyright (C) 2026 DeepSig Inc.",
            "SPDX-License-Identifier": "BSD-3-Clause-Clear",
            "schema": schema or runner.contention.READY_SCHEMA,
            "published_monotonic_ns": 123,
            "du_pid": 4242,
            "restart_cycles": 3,
            "load_profile": {"cell_count": 1, "configured_pusch_concurrency": 0},
            "instances": {
                "receiver": {
                    "instance_id": "receiver",
                    "metrics": {
                        "dapp.invoke.count": 10 + progress,
                        "host.input.cell_bitmap": 1,
                        "host.native_workers": 2,
                        "host.native_workers_active": 2,
                    },
                },
                "srs_isac_process": {
                    "instance_id": "srs",
                    "metrics": {
                        "class_c.copy.export.count": 5 + progress,
                        "class_c.copy.export.bytes": 512,
                        "class_c.copy.snapshot.count": 0,
                        "process.results_busy": 4 + progress,
                        "process.result_protocol_failures": 0,
                        "process.exits": 3,
                        "process.successful_restarts": 3,
                        "process.pid": 4444,
                        "host.input.cell_bitmap": 1,
                    },
                },
                "spectrum_process": {
                    "instance_id": "spectrum",
                    "metrics": {
                        "class_c.copy.export.count": 10 + progress,
                        "class_c.copy.export.bytes": 1024,
                        "class_c.copy.snapshot.count": 0,
                        "process.results_accepted": 8 + progress,
                        "process.exits": 3,
                        "process.successful_restarts": 3,
                        "process.pid": 4343,
                        "host.input.cell_bitmap": 1,
                    },
                },
            },
        }

    @staticmethod
    def samples(rss_growth=4096, fd_growth=1, thread_growth=0):
        def process(rss, fds, threads):
            return {
                "rss_bytes": rss,
                "gpu_memory_bytes": rss,
                "file_descriptors": fds,
                "threads": threads,
            }

        return [
            {
                "elapsed_ns": 0,
                "processes": {
                    "du": process(100_000, 20, 10),
                    "spectrum_worker": process(50_000, 8, 2),
                    "srs_worker": process(40_000, 8, 2),
                },
                "gpu_uuids": {
                    "du": ["GPU-test"],
                    "spectrum_worker": ["GPU-test"],
                    "srs_worker": ["GPU-test"],
                },
            },
            {
                "elapsed_ns": 60_000_000_000,
                "processes": {
                    "du": process(100_000 + rss_growth, 20 + fd_growth, 10 + thread_growth),
                    "spectrum_worker": process(50_000 + rss_growth, 8 + fd_growth, 2 + thread_growth),
                    "srs_worker": process(40_000 + rss_growth, 8 + fd_growth, 2 + thread_growth),
                },
                "gpu_uuids": {
                    "du": ["GPU-test"],
                    "spectrum_worker": ["GPU-test"],
                    "srs_worker": ["GPU-test"],
                },
            },
        ]

    @staticmethod
    def bounds():
        return {
            "maximum_du_rss_growth_bytes": 1_000_000,
            "maximum_worker_rss_growth_bytes": 1_000_000,
            "maximum_du_gpu_memory_growth_bytes": 1_000_000,
            "maximum_worker_gpu_memory_growth_bytes": 1_000_000,
            "maximum_fd_growth": 2,
            "maximum_thread_growth": 0,
        }

    @staticmethod
    def write_catalog(root):
        packages = (
            ("ref_receiver_cuda.manifest.json", "ref_receiver_cuda.so", "receiver", "receiver"),
            ("ref_srs_isac_cuda.manifest.json", "ref_srs_isac_cuda.so", "srs", "srs"),
            ("ref_spectrumd_cuda.manifest.json", "ref_spectrumd_cuda", "spectrum", "spectrum"),
            ("ref_srs_isacd_cuda.manifest.json", "ref_srs_isacd_cuda", "srs-process", "srs-process"),
        )
        for manifest_name, artifact_name, package_id, global_name in packages:
            payload = f"artifact:{artifact_name}".encode()
            (root / artifact_name).write_bytes(payload)
            manifest = {
                "artifact": artifact_name,
                "sha256": hashlib.sha256(payload).hexdigest(),
                "package_id": package_id,
                "global_name": global_name,
            }
            (root / manifest_name).write_text(json.dumps(manifest), encoding="utf-8")

    def test_resource_growth_inside_bounds_passes(self):
        summary, reasons = runner.process_summary(self.samples(), "du", 4242, self.bounds())
        self.assertEqual(reasons, [])
        self.assertEqual(summary["peak_growth"]["rss_bytes"], 4096)

    def test_resource_growth_exceeding_bound_fails(self):
        bounds = self.bounds()
        bounds["maximum_fd_growth"] = 0
        _, reasons = runner.process_summary(self.samples(), "spectrum_worker", 4343, bounds)
        self.assertIn("file_descriptors growth", reasons[0])

    def test_catalog_hash_mismatch_is_invalid(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self.write_catalog(root)
            (root / "ref_receiver_cuda.so").write_bytes(b"changed")
            with self.assertRaisesRegex(runner.ProtocolError, "hash mismatch"):
                runner.catalog_provenance(root)

    def test_gpu_memory_requires_both_declared_processes(self):
        completed = SimpleNamespace(returncode=0, stdout="GPU-test, 4242, 100\n", stderr="")
        with mock.patch.object(runner.subprocess, "run", return_value=completed):
            with self.assertRaisesRegex(runner.ProtocolError, "expected PIDs"):
                runner.gpu_memory(Path("/nvidia-smi"), {4242, 4343, 4444})

    def test_complete_campaign_emits_hashed_passing_report(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self.write_catalog(root)

            class FakeLoad:
                def __init__(fake_self, command, **_):
                    fake_self.pid = 4444
                    fake_self.returncode = None
                    ready_path = Path(command[command.index("--contention-ready-file") + 1])
                    ready_path.write_text(json.dumps(self.ready()), encoding="utf-8")
                    fake_self.result_path = Path(command[command.index("--contention-result-file") + 1])

                def poll(fake_self):
                    return fake_self.returncode

                def wait(fake_self, timeout=None):
                    del timeout
                    fake_self.result_path.write_text(
                        json.dumps(
                            self.ready(schema=runner.contention.RESULT_SCHEMA, progress=10)
                        ),
                        encoding="utf-8",
                    )
                    fake_self.returncode = 0
                    return 0

            arguments = SimpleNamespace(
                load_driver=Path(__file__),
                odu=Path(sys.executable),
                client=Path(__file__),
                e3_schema=Path(__file__),
                catalog=root,
                preflight_helper=Path(sys.executable),
                nvidia_smi=Path(sys.executable),
                output_dir=root / "evidence",
                duration_seconds=60,
                sample_interval_seconds=5,
                restart_cycles=3,
                maximum_du_rss_growth_mib=1,
                maximum_worker_rss_growth_mib=1,
                maximum_du_gpu_memory_growth_mib=1,
                maximum_worker_gpu_memory_growth_mib=1,
                maximum_fd_growth=2,
                maximum_thread_growth=0,
                bounds_rationale="test operator bounds",
                release_bounds_approved=True,
                ready_timeout_seconds=5,
                load_shutdown_timeout_seconds=5,
            )
            with mock.patch.object(runner.qualification, "host_manifest", return_value={}), mock.patch.object(
                runner.subprocess, "Popen", side_effect=FakeLoad
            ), mock.patch.object(runner, "verify_executable"), mock.patch.object(
                runner, "collect_samples", return_value=self.samples()
            ), mock.patch.object(
                runner,
                "gpu_inventory",
                return_value=[
                    {
                        "index": "0",
                        "name": "Test GPU",
                        "uuid": "GPU-test",
                        "pci_bus_id": "0000:01:00.0",
                        "driver_version": "1.0",
                    }
                ],
            ):
                report, status = runner.run(arguments)

            self.assertEqual(status, 0)
            self.assertEqual(report["status"], "passed")
            self.assertEqual(report["load"]["status"], "passed")
            self.assertEqual(report["summary"]["sample_count"], 2)
            self.assertTrue(report["summary"]["engineering_endurance_passed"])
            self.assertTrue(report["summary"]["release_admission_passed"])
            self.assertIn("sha256", report["samples"])
            self.assertEqual(len(report["load"]["packages"]), 4)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--runner", required=True)
    options, remaining = parser.parse_known_args()
    runner = load_runner(options.runner)
    unittest.main(argv=[sys.argv[0], *remaining])
