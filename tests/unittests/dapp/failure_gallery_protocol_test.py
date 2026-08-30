#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear

import argparse
import importlib.util
import json
from pathlib import Path
import unittest


def load_runner(path):
    spec = importlib.util.spec_from_file_location("ocudu_failure_gallery", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load runner: {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class FailureGalleryProtocolTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.runner = load_runner(Path(cls.runner_path))
        cls.schema = json.loads(Path(cls.schema_path).read_text(encoding="utf-8"))

    def report(self):
        incident = lambda cause: [{"cause": cause}]
        return {
            "SPDX-FileCopyrightText": "Copyright (C) 2026 DeepSig Inc.",
            "SPDX-License-Identifier": "BSD-3-Clause-Clear",
            "schema": self.runner.REPORT_SCHEMA,
            "status": "passed",
            "published_monotonic_ns": 1,
            "du": {"pid": 42, "path": "/tmp/odu", "sha256": "12" * 32},
            "catalog": {
                "path": "/tmp/catalog",
                "manifests": {name: "34" * 32 for name in self.runner.MANIFESTS},
            },
            "scenarios": [
                {
                    "name": "authority_conflict",
                    "status": "passed",
                    "instance_id": "a",
                    "evidence": {"result": {"code": "lifecycleFailed"}},
                },
                {
                    "name": "native_exception",
                    "status": "passed",
                    "instance_id": "b",
                    "evidence": {
                        "fallback_count": 1,
                        "breaker_bypasses": 1,
                        "breaker_state": 1,
                        "incidents": incident("standardException"),
                    },
                },
                {
                    "name": "native_invalid_output",
                    "status": "passed",
                    "instance_id": "c",
                    "evidence": {"fallback_count": 1, "incidents": incident("invalidOutput")},
                },
                {
                    "name": "native_deadline_fallback",
                    "status": "passed",
                    "instance_id": "d",
                    "evidence": {"fallback_count": 1, "incidents": incident("deadlineMiss")},
                },
                {
                    "name": "process_heartbeat_recovery",
                    "status": "passed",
                    "instance_id": "e",
                    "evidence": {
                        "heartbeat_losses": 1,
                        "stopped_pid": 10,
                        "restarted_pid": 11,
                        "incidents": incident("heartbeatLoss"),
                    },
                },
                {
                    "name": "process_exit_recovery",
                    "status": "passed",
                    "instance_id": "e",
                    "evidence": {
                        "successful_restarts": 2,
                        "killed_pid": 11,
                        "restarted_pid": 12,
                        "incidents": incident("processExit"),
                    },
                },
                {
                    "name": "process_invalid_output",
                    "status": "passed",
                    "instance_id": "f",
                    "evidence": {
                        "results_rejected": 1,
                        "results_accepted": 0,
                        "incidents": incident("invalidOutput"),
                    },
                },
            ],
            "health": {"standardExceptions": 1, "deadlineMisses": 1, "invalidOutputs": 2},
            "teardown": {"terminal_jobs": 49, "unloaded_instances": 7, "live_instances": 0},
            "limitations": ["one", "two"],
        }

    def test_complete_report_passes(self):
        self.runner.validate_report(self.report(), self.schema)

    def test_missing_scenario_fails(self):
        report = self.report()
        report["scenarios"].pop()
        with self.assertRaisesRegex(self.runner.ProtocolError, "scenario set"):
            self.runner.validate_report(report)

    def test_invalid_process_result_must_not_be_accepted(self):
        report = self.report()
        report["scenarios"][-1]["evidence"]["results_accepted"] = 1
        with self.assertRaisesRegex(self.runner.ProtocolError, "gained authority"):
            self.runner.validate_report(report)

    def test_requested_license_is_enforced(self):
        report = self.report()
        report["SPDX-License-Identifier"] = "BSD-3-Clause"
        with self.assertRaisesRegex(self.runner.ProtocolError, "license"):
            self.runner.validate_report(report)


if __name__ == "__main__":
    arguments = argparse.ArgumentParser()
    arguments.add_argument("--runner", required=True)
    arguments.add_argument("--report-schema", required=True)
    options, remaining = arguments.parse_known_args()
    FailureGalleryProtocolTest.runner_path = options.runner
    FailureGalleryProtocolTest.schema_path = options.report_schema
    unittest.main(argv=[__file__, *remaining])
