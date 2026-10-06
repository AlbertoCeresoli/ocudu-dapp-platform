#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear
"""Report integrity: known differences are informational, failed/missing checks are not."""
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
import check_dapp_interop_gate as gate
import libe3_interop_report as report
from libe3_issue_coverage import ISSUES, SCTP, ZMQ, TRACKER, issue_coverage


def passed(names):
    return [{"name": name, "status": "passed", "time": 0.0, "output": "", "category": "fixture"}
            for name in sorted(names)]


class IssueCoverageTest(unittest.TestCase):
    def test_every_ocudu_tracker_item_and_related_lifetime_is_listed_once(self):
        expected = {81, 85, 87, 88, 89, 90, 93, 95, 96, 97, 98, 99, 100, 101, 102, *range(104, 117)}
        self.assertEqual({item["number"] for item in ISSUES}, expected)
        self.assertEqual(len(ISSUES), len(expected))
        for item in ISSUES:
            self.assertTrue(item["note"])
            self.assertEqual(item["url"], f"https://github.com/wineslab/libe3/issues/{item['number']}")

    def test_no_tests_does_not_claim_runtime_coverage(self):
        for item in issue_coverage([]):
            self.assertIn(item["evidence_result"], ("NOT RUN", "DOCUMENTATION"))

    def test_unrelated_green_test_cannot_qualify_any_issue(self):
        self.assertEqual(issue_coverage(passed({"unrelated"})), issue_coverage([]))

    def test_missing_sctp_is_reported_without_failing_zmq_profile(self):
        tests = passed(gate.REQUIRED["zmq"])
        self.assertEqual(report.gate_verdict(tests, 0, "zmq"), "PASSED")
        rows = {r["number"]: r for r in issue_coverage(tests)}
        self.assertEqual(rows[104]["evidence_result"], "NOT RUN")
        self.assertEqual(rows[81]["evidence_result"], "PARTIAL")
        layers = report.link_layers(tests, [], "zmq", False)
        self.assertTrue(all(r["verdict"] == "SKIPPED" for r in layers if r["layer"] == "SCTP association"))

    def test_known_differences_do_not_fail_a_passing_gate(self):
        tests = passed(gate.REQUIRED["host"] | {name for item in ISSUES for name in item["tests"]})
        rows = issue_coverage(tests)
        self.assertTrue(any(r["disposition"] == "REMAINING-DIFFERENCE" for r in rows))
        self.assertTrue(all(r["evidence_result"] in ("TESTED", "DOCUMENTATION") for r in rows))
        self.assertEqual(report.gate_verdict(tests, 0, "host"), "PASSED")

    def test_failed_check_is_not_hidden_by_known_difference(self):
        tests = [{"name": SCTP, "status": "failed"}]
        rows = {r["number"]: r for r in issue_coverage(tests)}
        self.assertEqual(rows[104]["disposition"], "REMAINING-DIFFERENCE")
        self.assertEqual(rows[104]["evidence_result"], "FAILED")
        self.assertEqual(report.gate_verdict(tests, 0, "host"), "FAILED")

    def test_missing_required_tests_or_failed_gate_cannot_pass(self):
        self.assertEqual(report.gate_verdict([], 0, "host"), "FAILED")
        self.assertEqual(report.gate_verdict(passed({SCTP}), 0, "host"), "FAILED")
        self.assertEqual(report.gate_verdict(passed(gate.REQUIRED["host"]), 1, "host"), "FAILED")

    def test_skipped_required_test_fails_selected_profile(self):
        tests = passed(gate.REQUIRED["zmq"])
        tests[0]["status"] = "skipped"
        self.assertEqual(report.gate_verdict(tests, 0, "zmq"), "FAILED")

    def test_all_issue_links_and_exact_evidence_are_in_both_reports(self):
        tests = passed(gate.REQUIRED["zmq"])
        ctx = {"tests": tests, "issues": issue_coverage(tests), "coverage": [], "summaries": [],
               "link_layers": [], "gate_log": "", "verdict": "PASSED", "profile": "zmq", "kernel_sctp": False,
               "provenance": {"generated": "test", "host": "test", "ocudu_commit": "test",
                              "ocudu_branch": "test", "ocudu_dirty": "no", "submodules": {}, "patches": [],
                              "vector_source": "fixture", "cmake_options": {}, "toolchain": {}, "binaries": {}}}
        for rendered in (report.render_markdown(ctx), report.render_latex(ctx)):
            self.assertIn(TRACKER, rendered)
            for item in ISSUES:
                self.assertIn(item["url"], rendered)
                for name in item["tests"]:
                    self.assertIn(name, rendered)
            self.assertIn("REMAINING-DIFFERENCE", rendered)


class ReportIntegrityTest(unittest.TestCase):
    def test_junit_errors_skips_and_missing_status(self):
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory)
            (build / "dapp-interop.xml").write_text(
                '<testsuite><testcase name="ok"/><testcase name="error"><error/></testcase>'
                '<testcase name="skipped"><skipped/></testcase>'
                '<testcase name="failure" status="run"><failure/></testcase></testsuite>')
            self.assertEqual({t["name"]: t["status"] for t in report.load_tests(build)},
                             {"ok": "passed", "error": "failed", "skipped": "skipped", "failure": "failed"})

    def test_preflight_failure_removes_previous_green_results(self):
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory)
            for name in ("dapp-interop.xml", "dapp-interop.log"):
                (build / name).write_text("stale green results")
            with patch.object(sys, "argv", ["gate", "--build", directory]), \
                    patch.object(gate.subprocess, "check_output", return_value=json.dumps({"tests": []})):
                with self.assertRaisesRegex(RuntimeError, "required interop tests missing"):
                    gate.main()
            self.assertFalse((build / "dapp-interop.xml").exists())
            self.assertFalse((build / "dapp-interop.log").exists())

    def test_kernel_probe_handles_unsupported_sctp(self):
        with patch.object(report.socket, "socket", side_effect=OSError("no SCTP")):
            self.assertFalse(report.kernel_has_sctp())

    def test_no_run_with_no_results_is_a_failure_and_still_writes_artifacts(self):
        with tempfile.TemporaryDirectory() as directory:
            with patch.object(sys, "argv", ["report", "--build", directory, "--no-run"]), \
                    patch.object(report, "kernel_has_sctp", return_value=False), \
                    patch.object(report.shutil, "which", return_value=None):
                self.assertEqual(report.main(), 1)
            artifact = json.loads((Path(directory) / "dapp-interop-report/libe3_issue_coverage.json").read_text())
            self.assertEqual(artifact["profile"], "zmq")
            self.assertEqual(artifact["verdict"], "FAILED")
            self.assertEqual(len(artifact["issues"]), len(ISSUES))

    def test_auto_runs_host_with_sctp_and_zmq_without_it(self):
        for supported, expected in ((True, "host"), (False, "zmq")):
            with self.subTest(sctp=supported), tempfile.TemporaryDirectory() as directory:
                def fake_gate(build, summary_file, profile):
                    self.assertEqual(profile, expected)
                    (build / "dapp-interop.xml").write_text('<testsuite>' + ''.join(
                        f'<testcase name="{name}" status="run"/>' for name in gate.REQUIRED[profile]) + '</testsuite>')
                    return 0, "all selected checks passed"

                with patch.object(sys, "argv", ["report", "--build", directory]), \
                        patch.object(report, "kernel_has_sctp", return_value=supported), \
                        patch.object(report, "run_gate", side_effect=fake_gate), \
                        patch.object(report.shutil, "which", return_value=None):
                    self.assertEqual(report.main(), 0)
                artifact = json.loads((Path(directory) / "dapp-interop-report/libe3_issue_coverage.json").read_text())
                self.assertEqual(artifact["profile"], expected)
                self.assertEqual(artifact["verdict"], "PASSED")

    def test_truncated_junit_produces_a_failed_report_not_a_traceback(self):
        with tempfile.TemporaryDirectory() as directory:
            (Path(directory) / "dapp-interop.xml").write_text('<testsuite><testcase')
            with patch.object(sys, "argv", ["report", "--build", directory, "--no-run"]), \
                    patch.object(report, "kernel_has_sctp", return_value=False), \
                    patch.object(report.shutil, "which", return_value=None):
                self.assertEqual(report.main(), 1)
            self.assertIn("Cannot load current test evidence", (
                Path(directory) / "dapp-interop-report/libe3_interop_report.md").read_text())


if __name__ == "__main__":
    unittest.main(verbosity=2)
