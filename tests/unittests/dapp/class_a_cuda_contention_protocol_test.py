#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear

"""Hardware-independent tests for the loaded-L1 CUDA contention protocol."""

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
    spec = importlib.util.spec_from_file_location("class_a_contention", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class ContentionProtocolTest(unittest.TestCase):
    @staticmethod
    def policy():
        return {
            "schema": runner.qualification.POLICY_SCHEMA,
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

    @staticmethod
    def environment(pids="4242;4343;4444"):
        return {
            "gpu_monitor_available": "1",
            "gpu_observation_healthy": "1",
            "foreign_compute_processes": str(len(pids.split(";"))),
            "foreign_compute_pids": pids,
            "gpu_policy": "contention",
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

    def rows(self, lanes=1):
        rows = []
        signatures = (
            ("accelerated-conventional", 2),
            ("org.test.channel-estimator", 2),
            ("org.test.receiver-qpsk", 2),
            ("org.test.receiver-qam16", 4),
            ("org.test.receiver-qam64", 6),
            ("org.test.receiver-qam256", 8),
        )
        for prbs in runner.qualification.PRBS:
            for layers in runner.qualification.LAYERS:
                for path, modulation in signatures:
                    conventional = path == "accelerated-conventional"
                    for scope in runner.qualification.SCOPES:
                        rows.append(
                            {
                                **self.environment(),
                                "path": path,
                                "scope": scope,
                                "prbs": str(prbs),
                                "layers": str(layers),
                                "pusch_lanes": str(lanes),
                                "symbols": str(runner.qualification.SYMBOLS),
                                "warmups_per_phase": str(runner.qualification.WARMUPS),
                                "modulation_order": str(modulation),
                                "samples": str(runner.qualification.REPETITIONS * lanes),
                                "mean_ns": "70000",
                                "p50_ns": "68000",
                                "p90_ns": "90000",
                                "p95_ns": "100000",
                                "p99_ns": "120000",
                                "p999_ns": "140000",
                                "max_ns": "145000",
                                "pipeline_deadline_ns": str(
                                    runner.qualification.DEMODULATOR_DEADLINE_US * 1000
                                ),
                                "pipeline_misses": "0",
                                "dapp_deadline_ns": "0" if conventional else "150000",
                                "path_expected_invocations": "0" if conventional else str(3 * lanes * 1100),
                                "path_accepted_invocations": "0" if conventional else str(3 * lanes * 1100),
                                "path_production_deadline_misses": "0",
                            }
                        )
        return rows

    @staticmethod
    def ready(snapshot_count=0, schema=None, progress=0):
        return {
            "SPDX-FileCopyrightText": "Copyright (C) 2026 DeepSig Inc.",
            "SPDX-License-Identifier": "BSD-3-Clause-Clear",
            "schema": schema or runner.READY_SCHEMA,
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
                        "class_c.copy.snapshot.count": snapshot_count,
                        "process.results_accepted": 8 + progress,
                        "process.exits": 3,
                        "process.successful_restarts": 3,
                        "process.pid": 4343,
                        "host.input.cell_bitmap": 1,
                    },
                },
            },
        }

    def test_contention_command_preserves_strict_gate_without_raw_trace(self):
        command = runner.qualification.benchmark_command(
            Path("/benchmark"), Path("/results.csv"), 2, gpu_policy="contention"
        )
        self.assertIn("-G", command)
        self.assertIn("-X", command)
        self.assertNotIn("-Q", command)

    def test_ready_record_rejects_host_snapshot(self):
        with self.assertRaisesRegex(runner.ProtocolError, "host snapshot"):
            runner.validate_ready(self.ready(snapshot_count=1), 3)

    def test_multicell_ready_requires_exact_cells_and_configured_workers(self):
        ready = self.ready()
        ready["load_profile"] = {"cell_count": 2, "configured_pusch_concurrency": 2}
        for instance in ready["instances"].values():
            instance["metrics"]["host.input.cell_bitmap"] = 3
        ready["instances"]["receiver"]["metrics"]["host.native_workers"] = 4
        ready["instances"]["receiver"]["metrics"]["host.native_workers_active"] = 2
        self.assertEqual(
            runner.validate_ready(ready, 3, expected_cell_count=2, expected_pusch_concurrency=2),
            {4242, 4343, 4444},
        )
        ready["instances"]["spectrum_process"]["metrics"]["host.input.cell_bitmap"] = 1
        with self.assertRaisesRegex(runner.ProtocolError, "exact 2-cell coverage"):
            runner.validate_ready(ready, 3)

    def test_requested_multicell_topology_rejects_a_single_cell_record(self):
        with self.assertRaisesRegex(runner.ProtocolError, "requested topology"):
            runner.validate_ready(self.ready(), 3, expected_cell_count=2, expected_pusch_concurrency=2)

    def test_only_declared_load_clients_are_accepted(self):
        environment = self.environment("4242;4343;4444;9999")
        with self.assertRaisesRegex(runner.ProtocolError, "do not match"):
            runner.validate_known_clients(environment, {4242, 4343, 4444})

    def test_contention_policy_requires_an_observed_load(self):
        rows = self.rows()
        for row in rows:
            row["foreign_compute_processes"] = "0"
            row["foreign_compute_pids"] = "none"
        _, _, reasons, _ = runner.qualification.validate_rows(rows, 1, gpu_policy="contention")
        self.assertIn("contention load was not observed", reasons[0])

    def test_final_record_requires_progress(self):
        with self.assertRaisesRegex(runner.ProtocolError, "made no progress"):
            runner.validate_result(self.ready(schema=runner.RESULT_SCHEMA), self.ready(), 3)

    def test_repeated_campaign_emits_hashed_passing_report(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            policy_path = root / "policy.json"
            policy_path.write_text(json.dumps(self.policy()), encoding="utf-8")

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
                        json.dumps(self.ready(schema=runner.RESULT_SCHEMA, progress=10)), encoding="utf-8"
                    )
                    fake_self.returncode = 0
                    return 0

            def benchmark(command, **_):
                lanes = int(command[command.index("-N") + 1])
                output = Path(command[command.index("-O") + 1])
                with output.open("w", newline="", encoding="utf-8") as destination:
                    writer = csv.DictWriter(
                        destination, fieldnames=sorted(runner.qualification.REQUIRED_COLUMNS)
                    )
                    writer.writeheader()
                    writer.writerows(self.rows(lanes))
                return SimpleNamespace(returncode=0, stdout="synthetic contention benchmark\n")

            arguments = SimpleNamespace(
                benchmark=Path(sys.executable),
                load_driver=Path(__file__),
                odu=Path(sys.executable),
                client=Path(__file__),
                e3_schema=Path(__file__),
                catalog=root,
                preflight_helper=Path(sys.executable),
                policy=policy_path,
                output_dir=root / "evidence",
                trials=3,
                lane_counts=[1],
                restart_cycles=3,
                ready_timeout_seconds=5,
                load_hold_timeout_seconds=30,
                load_shutdown_timeout_seconds=5,
                benchmark_timeout_seconds=5,
            )
            with mock.patch.object(runner.qualification, "host_manifest", return_value={}), mock.patch.object(
                runner.subprocess, "Popen", side_effect=FakeLoad
            ), mock.patch.object(runner.subprocess, "run", side_effect=benchmark):
                report, status = runner.run(arguments)

            self.assertEqual(status, 0)
            self.assertEqual(report["status"], "passed")
            self.assertEqual(report["load"]["expected_gpu_client_pids"], [4242, 4343, 4444])
            self.assertEqual(report["summary"]["passed_runs"], 3)
            self.assertTrue(all("sha256" in item["csv"] for item in report["runs"]))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--runner", required=True)
    options, remaining = parser.parse_known_args()
    runner = load_runner(options.runner)
    unittest.main(argv=[sys.argv[0], *remaining])
