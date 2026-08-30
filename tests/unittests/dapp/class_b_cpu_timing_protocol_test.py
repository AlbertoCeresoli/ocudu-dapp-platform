#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear

"""Hardware-independent tests for the Class-B CPU timing protocol."""

import argparse
import csv
import hashlib
import importlib.util
import json
from pathlib import Path
from types import SimpleNamespace
import sys
import tempfile
import unittest
from unittest import mock

import jsonschema


def load_runner(path):
    spec = importlib.util.spec_from_file_location("class_b_cpu_timing", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class ClassBTimingProtocolTest(unittest.TestCase):
    @staticmethod
    def policy(approved=True):
        host = runner.host_manifest()
        return {
            "SPDX-FileCopyrightText": "Copyright (C) 2026 DeepSig Inc.",
            "SPDX-License-Identifier": "BSD-3-Clause-Clear",
            "schema": runner.POLICY_SCHEMA,
            "platform_id": "unit-test",
            "deadline_ns": runner.DEADLINE_NS,
            "repetitions": runner.REPETITIONS,
            "warmups": runner.WARMUPS,
            "maximum_p999_ns": runner.DEADLINE_NS,
            "maximum_deadline_miss_rate": 0,
            "maximum_fallback_rate": 0,
            "maximum_validation_rejection_rate": 0,
            "required_scheduler_policy": host["scheduler_policy"],
            "minimum_scheduler_priority": host["scheduler_priority"],
            "required_cpu_affinity": host["cpu_affinity"],
            "cpu_frequency_policy": "unit-test",
            "release_controls_approved": approved,
            "controls_rationale": "hardware-independent protocol test",
        }

    @staticmethod
    def rows():
        rows = []
        for name, (direction, candidates, prbs, interference) in runner.PROFILES.items():
            for sample in range(runner.REPETITIONS):
                rows.append(
                    {
                        "schema": runner.SAMPLE_SCHEMA,
                        "scenario": name,
                        "sample": str(sample),
                        "direction": direction,
                        "candidates": str(candidates),
                        "prbs": str(prbs),
                        "interference": str(int(interference)),
                        "avoidance_output": str(int(interference)),
                        "conventional_ns": "300",
                        "feature_ns": "100",
                        "dapp_ns": "200",
                        "validation_ns": "50",
                        "commit_ns": "25",
                        "total_ns": "500",
                        "outcome": "accepted",
                        "deadline_miss": "0",
                    }
                )
        return rows

    @staticmethod
    def write_rows(path, rows):
        fields = [
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
        ]
        with path.open("w", newline="", encoding="utf-8") as output:
            writer = csv.DictWriter(output, fieldnames=fields)
            writer.writeheader()
            writer.writerows(rows)

    def test_fixed_matrix_passes_and_reports_all_phases(self):
        profiles, summary, reasons = runner.analyze_rows(self.rows(), self.policy())
        self.assertEqual(reasons, [])
        self.assertEqual(len(profiles), 9)
        self.assertEqual(summary["rows"], 9000)
        self.assertEqual(summary["worst_total_p999_ns"], 500)
        self.assertEqual(set(profiles[0]["phases"]), set(runner.PHASES))

    def test_missing_profile_is_invalid_evidence(self):
        rows = [row for row in self.rows() if row["scenario"] != "dl_20mhz_c16"]
        with self.assertRaisesRegex(runner.ProtocolError, "exact fixed profile matrix"):
            runner.analyze_rows(rows, self.policy())

    def test_deadline_and_fallback_are_threshold_failures(self):
        rows = self.rows()
        rows[0]["outcome"] = "fallback"
        rows[0]["deadline_miss"] = "1"
        rows[0]["total_ns"] = str(runner.DEADLINE_NS + 1)
        _, summary, reasons = runner.analyze_rows(rows, self.policy())
        self.assertEqual(summary["deadline_misses"], 1)
        self.assertEqual(summary["fallbacks"], 1)
        self.assertTrue(any("deadline miss rate" in reason for reason in reasons))
        self.assertTrue(any("fallback rate" in reason for reason in reasons))

    def test_complete_campaign_is_hashed_and_schema_valid(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            benchmark = root / "benchmark"
            benchmark.write_bytes(b"benchmark")
            module = root / "ref_scheduler_cpu.so"
            module.write_bytes(b"module")
            manifest = root / "ref_scheduler_cpu.manifest.json"
            manifest.write_text(
                json.dumps(
                    {
                        "global_name": "org.ocudu.reference.scheduler.cpu",
                        "artifact": module.name,
                        "sha256": hashlib.sha256(module.read_bytes()).hexdigest(),
                    }
                ),
                encoding="utf-8",
            )
            policy_path = root / "policy.json"
            policy_path.write_text(json.dumps(self.policy()), encoding="utf-8")

            def execute(command, **_):
                raw_path = Path(command[command.index("-o") + 1])
                self.write_rows(raw_path, self.rows())
                return SimpleNamespace(returncode=0, stdout="benchmark complete\n")

            arguments = SimpleNamespace(
                benchmark=benchmark,
                reference_dir=root,
                policy=policy_path,
                policy_schema=policy_schema,
                report_schema=report_schema,
                output_dir=root / "evidence",
                timeout_seconds=10,
            )
            with mock.patch.object(runner.subprocess, "run", side_effect=execute):
                report, status = runner.run(arguments)

            self.assertEqual(status, 0)
            self.assertEqual(report["status"], "passed")
            self.assertEqual(report["summary"]["rows"], 9000)
            self.assertEqual(report["tooling"]["module_artifact"]["sha256"], hashlib.sha256(b"module").hexdigest())
            schema = json.loads(report_schema.read_text(encoding="utf-8"))
            jsonschema.Draft202012Validator(schema).validate(report)

    def test_malformed_campaign_is_invalid_and_schema_valid(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            benchmark = root / "benchmark"
            benchmark.write_bytes(b"benchmark")
            module = root / "ref_scheduler_cpu.so"
            module.write_bytes(b"module")
            (root / "ref_scheduler_cpu.manifest.json").write_text(
                json.dumps(
                    {
                        "global_name": "org.ocudu.reference.scheduler.cpu",
                        "artifact": module.name,
                        "sha256": hashlib.sha256(module.read_bytes()).hexdigest(),
                    }
                ),
                encoding="utf-8",
            )
            policy_path = root / "policy.json"
            policy_path.write_text(json.dumps(self.policy()), encoding="utf-8")

            def execute(command, **_):
                self.write_rows(Path(command[command.index("-o") + 1]), self.rows()[:-1])
                return SimpleNamespace(returncode=0, stdout="incomplete benchmark\n")

            arguments = SimpleNamespace(
                benchmark=benchmark,
                reference_dir=root,
                policy=policy_path,
                policy_schema=policy_schema,
                report_schema=report_schema,
                output_dir=root / "evidence",
                timeout_seconds=10,
            )
            with mock.patch.object(runner.subprocess, "run", side_effect=execute):
                report, status = runner.run(arguments)

            self.assertEqual(status, 1)
            self.assertEqual(report["status"], "invalid")
            self.assertIn("expected 9000", report["summary"]["reasons"][0])
            schema = json.loads(report_schema.read_text(encoding="utf-8"))
            jsonschema.Draft202012Validator(schema).validate(report)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--runner", required=True)
    parser.add_argument("--policy-schema", required=True)
    parser.add_argument("--report-schema", required=True)
    options, remaining = parser.parse_known_args()
    runner = load_runner(options.runner)
    policy_schema = Path(options.policy_schema)
    report_schema = Path(options.report_schema)
    unittest.main(argv=[sys.argv[0], *remaining])
