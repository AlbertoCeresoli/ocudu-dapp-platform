#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear

"""Hardware-independent tests for the Class-C CUDA throughput protocol."""

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
    spec = importlib.util.spec_from_file_location("class_c_cuda_throughput", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class ThroughputProtocolTest(unittest.TestCase):
    @staticmethod
    def policy(approved=True):
        return {
            "SPDX-FileCopyrightText": "Copyright (C) 2026 DeepSig Inc.",
            "SPDX-License-Identifier": "BSD-3-Clause-Clear",
            "schema": runner.POLICY_SCHEMA,
            "platform_id": "test-platform",
            "channel_bandwidth_mhz": 100,
            "common_scs_khz": 30,
            "time_scaling": 1,
            "srs_period_ms": 20,
            "duration_seconds": 30,
            "restart_cycles_before_measurement": 3,
            "minimum_spectrum_tensor_bytes": 183456,
            "minimum_srs_tensor_bytes": 1024,
            "minimum_spectrum_publications_per_second": 1,
            "minimum_srs_publications_per_second": 0.1,
            "minimum_spectrum_export_gbps": 0.0001,
            "minimum_srs_export_gbps": 0.000001,
            "maximum_backpressure_drop_rate": 0.0,
            "maximum_export_slot_high_watermark_ratio": 1.0,
            "release_controls_approved": approved,
            "controls_rationale": "test controls",
        }

    @staticmethod
    def route_metrics(exports, tensor_bytes, results, pid, progress=0):
        count = exports + progress
        metrics = {
            "class_c.route.publications": count,
            "class_c.route.payload_bytes": count * 512,
            "class_c.route.drops.backpressure": 0,
            "class_c.route.drops.invalid": 0,
            "class_c.route.failures.encode": 0,
            "class_c.route.failures.notification": 0,
            "class_c.route.drops.concurrent": 0,
            "class_c.copy.snapshot.count": 0,
            "class_c.copy.snapshot.bytes": 0,
            "class_c.copy.lease.count": 0,
            "class_c.copy.export.count": count,
            "class_c.copy.export.bytes": count * tensor_bytes,
            "class_c.copy.e3_payload.count": 0,
            "class_c.copy.e3_payload.bytes": 0,
            "class_c.export_slots.capacity": 4,
            "class_c.export_slots.in_use": 0,
            "class_c.export_slots.high_watermark": 2,
            "class_c.export_slots.reservation_failures": 0,
            "process.exits": 3,
            "process.successful_restarts": 3,
            "process.pid": pid,
            "process.result_protocol_failures": 0,
            "process.results_accepted": 0,
            "process.results_busy": 0,
        }
        metrics[results] = count
        return metrics

    @classmethod
    def record(cls, result=False):
        progress = 200 if result else 0
        return {
            "SPDX-FileCopyrightText": "Copyright (C) 2026 DeepSig Inc.",
            "SPDX-License-Identifier": "BSD-3-Clause-Clear",
            "schema": runner.RESULT_SCHEMA if result else runner.READY_SCHEMA,
            "published_monotonic_ns": 31_000_000_000 if result else 1_000_000_000,
            "du_pid": 4242,
            "restart_cycles": 3,
            "load_profile": {
                "channel_bandwidth_mhz": 100,
                "common_scs_khz": 30,
                "time_scaling": 1,
                "srs_period_ms": 20,
            },
            "instances": {
                "spectrum_process": {
                    "instance_id": "spectrum",
                    "metrics": cls.route_metrics(
                        10, 183456, "process.results_accepted", 4343, progress
                    ),
                },
                "srs_isac_process": {
                    "instance_id": "srs",
                    "metrics": cls.route_metrics(
                        5, 1024, "process.results_busy", 4444, progress // 10
                    ),
                },
            },
        }

    @staticmethod
    def write_catalog(root):
        packages = (
            ("ref_receiver_cuda.manifest.json", "ref_receiver_cuda.so"),
            ("ref_srs_isac_cuda.manifest.json", "ref_srs_isac_cuda.so"),
            ("ref_spectrumd_cuda.manifest.json", "ref_spectrumd_cuda"),
            ("ref_srs_isacd_cuda.manifest.json", "ref_srs_isacd_cuda"),
        )
        for index, (manifest_name, artifact_name) in enumerate(packages):
            payload = f"artifact:{artifact_name}".encode()
            (root / artifact_name).write_bytes(payload)
            (root / manifest_name).write_text(
                json.dumps(
                    {
                        "artifact": artifact_name,
                        "sha256": hashlib.sha256(payload).hexdigest(),
                        "package_id": f"package-{index}",
                        "global_name": f"global-{index}",
                    }
                ),
                encoding="utf-8",
            )

    def test_route_analysis_reports_exact_rate_and_pressure(self):
        ready = self.record()["instances"]["spectrum_process"]
        result = self.record(True)["instances"]["spectrum_process"]
        result["metrics"]["class_c.route.publications"] += 300
        summary, reasons = runner.analyze_route("spectrum", ready, result, 30_000_000_000, self.policy())
        self.assertEqual(reasons, [])
        self.assertEqual(summary["counter_deltas"]["export_count"], 200)
        self.assertEqual(summary["tensor_bytes_per_export"], 183456)
        self.assertAlmostEqual(summary["publications_per_second"], 200 / 30)
        self.assertEqual(summary["export_slot_high_watermark_ratio"], 0.5)

    def test_unexpected_host_snapshot_is_invalid(self):
        ready = self.record()["instances"]["spectrum_process"]
        result = self.record(True)["instances"]["spectrum_process"]
        result["metrics"]["class_c.copy.snapshot.count"] = 1
        with self.assertRaisesRegex(runner.ProtocolError, "unexpected copy/lease path"):
            runner.analyze_route("spectrum", ready, result, 30_000_000_000, self.policy())

    def test_backpressure_and_threshold_shortfall_fail(self):
        ready = self.record()["instances"]["srs_isac_process"]
        result = self.record(True)["instances"]["srs_isac_process"]
        result["metrics"]["class_c.route.drops.backpressure"] = 1
        result["metrics"]["class_c.export_slots.reservation_failures"] = 1
        policy = self.policy()
        policy["minimum_srs_publications_per_second"] = 1000
        _, reasons = runner.analyze_route("srs_isac", ready, result, 30_000_000_000, policy)
        self.assertTrue(any("publications/s" in reason for reason in reasons))
        self.assertTrue(any("drop rate" in reason for reason in reasons))

    def test_complete_campaign_emits_hashed_passing_report(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self.write_catalog(root)
            policy_path = root / "policy.json"
            policy_path.write_text(json.dumps(self.policy()), encoding="utf-8")

            class FakeLoad:
                def __init__(fake_self, command, **_):
                    fake_self.pid = 4242
                    fake_self.returncode = None
                    Path(command[command.index("--contention-ready-file") + 1]).write_text(
                        json.dumps(self.record()), encoding="utf-8"
                    )
                    fake_self.result_path = Path(command[command.index("--contention-result-file") + 1])

                def poll(fake_self):
                    return fake_self.returncode

                def wait(fake_self, timeout=None):
                    del timeout
                    fake_self.result_path.write_text(json.dumps(self.record(True)), encoding="utf-8")
                    fake_self.returncode = 0
                    return 0

            arguments = SimpleNamespace(
                policy=policy_path,
                policy_schema=Path(__file__),
                report_schema=Path(__file__),
                load_driver=Path(__file__),
                odu=Path(sys.executable),
                client=Path(__file__),
                e3_schema=Path(__file__),
                catalog=root,
                preflight_helper=Path(sys.executable),
                nvidia_smi=Path(sys.executable),
                output_dir=root / "evidence",
                ready_timeout_seconds=5,
                load_shutdown_timeout_seconds=5,
            )
            test_policy = self.policy()
            test_policy["duration_seconds"] = 0
            with mock.patch.object(runner.subprocess, "Popen", side_effect=FakeLoad), mock.patch.object(
                runner, "gpu_inventory", return_value=[{"uuid": "GPU-test"}]
            ), mock.patch.object(
                runner,
                "gpu_client_pids",
                side_effect=[set(), {4242, 4343, 4444}, {4242, 4343, 4444}],
            ), mock.patch.object(
                runner.qualification, "host_manifest", return_value={}
            ), mock.patch.object(
                runner, "load_policy", return_value=test_policy
            ):
                report, status = runner.run(arguments)

            self.assertEqual(status, 0)
            self.assertEqual(report["status"], "passed")
            self.assertEqual(report["load"]["status"], "passed")
            self.assertEqual(set(report["routes"]), {"spectrum", "srs_isac"})
            self.assertEqual(len(report["load"]["packages"]), 4)
            self.assertIn("sha256", report["load"]["result"])


if __name__ == "__main__":
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--runner", required=True)
    options, remaining = parser.parse_known_args()
    runner = load_runner(options.runner)
    unittest.main(argv=[sys.argv[0], *remaining])
