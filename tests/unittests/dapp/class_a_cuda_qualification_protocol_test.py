#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear

"""Unit tests for the hardware-independent Class-A qualification protocol."""

import argparse
import csv
import importlib.util
import json
from pathlib import Path
from types import SimpleNamespace
import sys
import tempfile
import unittest
from unittest import mock


def load_runner(path):
    spec = importlib.util.spec_from_file_location("class_a_qualification", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class QualificationProtocolTest(unittest.TestCase):
    def environment(self):
        return {
            "gpu_monitor_available": "1",
            "gpu_observation_healthy": "1",
            "foreign_compute_processes": "0",
            "foreign_compute_pids": "none",
            "gpu_policy": "exclusive",
            "host": "test-host",
            "machine": "aarch64",
            "gpu": "Test GPU",
            "scheduler_policy": "fifo",
            "scheduler_priority": "20",
            "cpu_affinity": "2-3",
            "pusch_stream_priority": "high",
            "package_filter": "all",
            "preflight_timeout_ms": "500",
            "build_hash": "0123abcd",
            "build_mode": "Release",
            "cuda_device": "0",
            "cuda_driver_version": "13000",
            "cuda_runtime_version": "13000",
            "gpu_pci_bus_id": "0000:01:00.0",
            "nvml_driver_version": "580.95.05",
            "gpu_uuid": "GPU-test",
            "gpu_temperature_min_c": "42",
            "gpu_temperature_max_c": "63",
            "gpu_sm_clock_min_mhz": "900",
            "gpu_sm_clock_max_mhz": "1800",
            "gpu_memory_clock_min_mhz": "1000",
            "gpu_memory_clock_max_mhz": "2000",
            "gpu_power_min_mw": "5000",
            "gpu_power_max_mw": "45000",
            "gpu_power_limit_mw": "60000",
            "gpu_persistence_mode": "enabled",
            "gpu_compute_mode": "default",
            "gpu_performance_state_best": "0",
            "gpu_performance_state_worst": "8",
        }

    def rows(self, lanes=2):
        rows = []
        signatures = (
            ("accelerated-conventional", 2),
            ("org.test.channel-estimator", 2),
            ("org.test.receiver-qpsk", 2),
            ("org.test.receiver-qam16", 4),
            ("org.test.receiver-qam64", 6),
            ("org.test.receiver-qam256", 8),
        )
        for prbs in runner.PRBS:
            for layers in runner.LAYERS:
                for path, modulation in signatures:
                    conventional = path == "accelerated-conventional"
                    for scope in runner.SCOPES:
                        rows.append(
                            {
                                **self.environment(),
                                "path": path,
                                "scope": scope,
                                "prbs": str(prbs),
                                "layers": str(layers),
                                "pusch_lanes": str(lanes),
                                "symbols": str(runner.SYMBOLS),
                                "warmups_per_phase": str(runner.WARMUPS),
                                "modulation_order": str(modulation),
                                "samples": str(runner.REPETITIONS * lanes),
                                "mean_ns": "70000",
                                "p50_ns": "68000",
                                "p90_ns": "90000",
                                "p95_ns": "100000",
                                "p99_ns": "120000",
                                "p999_ns": "140000",
                                "max_ns": "145000",
                                "pipeline_deadline_ns": str(runner.DEMODULATOR_DEADLINE_US * 1000),
                                "pipeline_misses": "0",
                                "dapp_deadline_ns": "0" if conventional else "150000",
                                "path_expected_invocations": "0" if conventional else str(3 * lanes * 1100),
                                "path_accepted_invocations": "0" if conventional else str(3 * lanes * 1100),
                                "path_production_deadline_misses": "0",
                            }
                        )
        return rows

    def policy(self):
        return {
            "schema": runner.POLICY_SCHEMA,
            "platform_id": "test-platform",
            "scheduler_policy": "fifo",
            "minimum_scheduler_priority": 10,
            "cpu_affinity": "2-3",
            "pusch_stream_priority": "high",
            "gpu_clock_policy": "locked by test operator",
            "gpu_power_policy": "fixed 60 W limit",
            "maximum_gpu_temperature_c": 80,
            "release_controls_approved": True,
            "expected_machine": "aarch64",
            "expected_gpu": "Test GPU",
            "required_gpu_persistence_mode": "enabled",
            "expected_gpu_power_limit_mw": 60000,
        }

    def test_complete_matrix_passes(self):
        environment, signature, reasons, summary = runner.validate_rows(self.rows(), 2)
        self.assertTrue(signature)
        self.assertEqual(reasons, [])
        self.assertEqual(runner.validate_policy(self.policy(), environment), [])
        self.assertEqual(summary["production_deadline_misses"], 0)

    def test_tail_and_production_misses_fail_closed(self):
        rows = self.rows()
        target = next(
            row
            for row in rows
            if row["scope"] == "device-pipeline" and row["path"] != "accelerated-conventional"
        )
        target["p999_ns"] = "150001"
        target["pipeline_misses"] = "1"
        target["path_production_deadline_misses"] = "1"
        _, _, reasons, summary = runner.validate_rows(rows, 2)
        self.assertTrue(any("exceeded P99.9" in reason for reason in reasons))
        self.assertTrue(any("production deadline misses" in reason for reason in reasons))
        self.assertEqual(summary["pipeline_deadline_misses"], 1)

    def test_missing_shape_is_invalid_evidence(self):
        rows = [row for row in self.rows() if row["prbs"] != "273"]
        with self.assertRaisesRegex(runner.ProtocolError, "fixed PRB/layer envelope"):
            runner.validate_rows(rows, 2)

    def test_environment_change_is_invalid_evidence(self):
        rows = self.rows()
        rows[-1]["build_hash"] = "different"
        with self.assertRaisesRegex(runner.ProtocolError, "changes within one run"):
            runner.validate_rows(rows, 2)

    def test_policy_mismatch_fails(self):
        environment, _, _, _ = runner.validate_rows(self.rows(), 2)
        policy = self.policy()
        policy["cpu_affinity"] = "4-5"
        policy["maximum_gpu_temperature_c"] = 60
        reasons = runner.validate_policy(policy, environment)
        self.assertTrue(any("cpu_affinity" in reason for reason in reasons))
        self.assertTrue(any("temperature" in reason for reason in reasons))

    def test_unavailable_optional_telemetry_remains_explicit(self):
        rows = self.rows()
        for row in rows:
            row["gpu_memory_clock_min_mhz"] = "-1"
            row["gpu_memory_clock_max_mhz"] = "-1"
            row["gpu_power_limit_mw"] = "-1"
        environment, _, reasons, _ = runner.validate_rows(rows, 2)
        self.assertEqual(reasons, [])
        self.assertEqual(environment["gpu_memory_clock_min_mhz"], "-1")
        self.assertEqual(environment["gpu_power_limit_mw"], "-1")

    def test_command_cannot_relax_qualification(self):
        command = runner.benchmark_command(Path("/benchmark"), Path("/result.csv"), 2)
        self.assertIn("-G", command)
        self.assertNotIn("-X", command)
        self.assertNotIn("-Q", command)
        self.assertEqual(command[command.index("-C") + 1], "100")
        self.assertEqual(command[command.index("-D") + 1], "150")

    def test_repeated_runner_emits_hashed_pass_report(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            policy_path = root / "policy.json"
            policy_path.write_text(json.dumps(self.policy()), encoding="utf-8")

            def benchmark(command, **_):
                lanes = int(command[command.index("-N") + 1])
                output = Path(command[command.index("-O") + 1])
                rows = self.rows(lanes)
                with output.open("w", newline="", encoding="utf-8") as destination:
                    writer = csv.DictWriter(destination, fieldnames=sorted(runner.REQUIRED_COLUMNS))
                    writer.writeheader()
                    writer.writerows(rows)
                return SimpleNamespace(returncode=0, stdout="synthetic qualifying benchmark\n")

            arguments = SimpleNamespace(
                benchmark=Path(sys.executable),
                policy=policy_path,
                output_dir=root / "evidence",
                trials=3,
                lane_counts=[1],
                timeout_seconds=5,
            )
            with mock.patch.object(runner, "host_manifest", return_value={}), mock.patch.object(
                runner.subprocess, "run", side_effect=benchmark
            ):
                report, status = runner.run(arguments)
            self.assertEqual(status, 0)
            self.assertEqual(report["status"], "passed")
            self.assertEqual(report["summary"]["passed_runs"], 3)
            self.assertTrue(all("sha256" in item["csv"] for item in report["runs"]))

    def test_unapproved_controls_fail_without_invalidating_evidence(self):
        environment, _, benchmark_reasons, _ = runner.validate_rows(self.rows(), 2)
        policy = self.policy()
        policy["release_controls_approved"] = False
        self.assertEqual(benchmark_reasons, [])
        self.assertIn("not approved", runner.validate_policy(policy, environment)[0])


if __name__ == "__main__":
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--runner", required=True)
    options, remaining = parser.parse_known_args()
    runner = load_runner(options.runner)
    unittest.main(argv=[sys.argv[0], *remaining])
