#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear
"""Fail-closed CTest gate: required tests must exist, execute, and not skip.

The comparative timing benchmark has its own qualification step; this gate
does not turn noisy single-host performance ordering into a conformance rule.
"""
import argparse
import json
from pathlib import Path
import re
import subprocess
import sys
import xml.etree.ElementTree as ET

REQUIRED = {
    "host": {
        "dapp_e3ap_libe3.e2e", "dapp_e3ap_libe3.interop",
        "dapp_e3ap_python.cross_language", "dapp_e3_shared_region.validation",
        "dapp_e3ap_libe3_codec.decodes_and_reproduces_vectors_from_the_upstream_libe3_encoder",
        "portable_data_store_test.fanout_copy_budget_drops_whole_objects_and_rotates_admission",
        "portable_data_store_test.concurrent_removal_and_replacement_do_not_reclaim_a_borrowed_producer_slot",
        "dapp_telemetry_isolation.no_producer_allocation",
    },
    "sdk": {
        "contract.ref_channel_estimator_cpu", "contract.ref_equalizer_cpu",
        "contract.ref_receiver_cpu", "contract.ref_scheduler_cpu",
        "portable_e3.spectrum", "portable_e3.srs_isac", "install.public_consumer",
        "abi.public_v1_compiler_matrix",
    },
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--profile", choices=REQUIRED, default="host")
    args = parser.parse_args()
    build = args.build.resolve()
    base = ["ctest", "--test-dir", str(build)]
    selection = (["-R", r"^dapp_e3|^dapp_portable_data_store|^portable_data_store_test|^dapp_telemetry_isolation",
                  "-E", "by_reference_delivery_is_faster_than_in_band"] if args.profile == "host" else [])
    inventory = json.loads(subprocess.check_output(base + selection + ["--show-only=json-v1"], text=True))
    tests = {test["name"] for test in inventory["tests"]}
    missing = REQUIRED[args.profile] - tests
    if missing:
        raise RuntimeError("required interop tests missing (check dependencies/options/build): " + ", ".join(sorted(missing)))
    report = build / "dapp-interop.xml"
    command = base + selection + ["-j1", "--no-tests=error", "--verbose", "--output-junit", str(report)]
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=600)
    (build / "dapp-interop.log").write_text(result.stdout)
    print(result.stdout)
    if result.returncode:
        raise RuntimeError(f"interop CTest failed ({result.returncode})")
    cases = ET.parse(report).getroot().findall(".//testcase")
    executed = {case.attrib.get("name") for case in cases}
    if tests != executed:
        raise RuntimeError("CTest report does not match selected test inventory")
    if (any(case.find("skipped") is not None or case.attrib.get("status") in ("notrun", "disabled") for case in cases)
            or re.search(r"\[\s*SKIPPED\s*\]|OK \(skipped=|\*\*\*Skipped", result.stdout)):
        raise RuntimeError("interop gate contained skipped tests")
    print(f"Qualified {len(cases)} {args.profile} tests without skips")


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, subprocess.SubprocessError, OSError, ValueError, ET.ParseError) as error:
        print(f"INTEROP GATE FAILED: {error}", file=sys.stderr)
        sys.exit(1)
