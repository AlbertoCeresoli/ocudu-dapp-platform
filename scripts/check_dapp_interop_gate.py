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
    # SCTP-capable runners: everything, both libe3 link layers, both libe3 encodings.
    "host": {
        "dapp_e3ap_libe3.e2e_sctp", "dapp_e3ap_libe3.e2e_zmq", "dapp_e3ap_libe3.interop",
        "dapp_e3ap_python.cross_language", "dapp_e3_shared_region.validation",
        "dapp_e3ap_libe3_codec.decodes_and_reproduces_vectors_from_the_upstream_libe3_encoder",
        "dapp_e3ap_sctp_server.serves_every_encoding_and_profile_on_one_port",
        # Protobuf: libe3's ProtobufE3Encoder vectors against the hand-written codec, and the codec against
        # libprotobuf on both schemas (only built with libprotobuf, so a build without it fails here).
        "dapp_e3ap_protobuf_codec.decodes_and_reproduces_vectors_from_the_upstream_libe3_protobuf_encoder",
        "dapp_e3ap_protobuf_conformance.ocudu_schema_is_a_wire_superset_of_the_libe3_schema",
        "dapp_e3ap_protobuf_conformance.random_pdus_encode_exactly_as_libprotobuf_serialises_them",
        # E3AP-OCUDU-V2: one protocol with libe3, the same in APER and Protobuf.
        "dapp_e3ap_v2_codec.aper_and_protobuf_carry_the_same_protocol_in_both_profiles",
        "dapp_e3ap_v2_codec.every_ocudu_addition_round_trips_and_libe3_peers_see_the_libe3_message",
        "portable_data_store_test.fanout_copy_budget_drops_whole_objects_and_rotates_admission",
        "portable_data_store_test.concurrent_removal_and_replacement_do_not_reclaim_a_borrowed_producer_slot",
        "dapp_telemetry_isolation.no_producer_allocation",
        "dapp_e3_cli.cross_language",
        "dapp_package_signature.runtime_policy_reload_is_monotonic_and_keeps_original_immutable",
        "dapp_package_signature.runtime_catalog_verifies_then_admits_without_loading_and_replays_transactions",
        "dapp_package_signature.special_files_never_wait_for_a_fifo_writer",
    },
    # Any runner (no kernel SCTP needed): libe3 interop over the ZMQ link layer (APER and Protobuf),
    # libe3's encoder vectors against our codecs, and the schema-level checks.
    "zmq": {
        "dapp_e3ap_libe3.e2e_zmq", "dapp_e3ap_libe3.interop",
        "dapp_e3ap_libe3_codec.decodes_and_reproduces_vectors_from_the_upstream_libe3_encoder",
        "dapp_e3ap_libe3_codec.round_trips_every_procedure_of_the_libe3_module",
        "dapp_e3ap_libe3_codec.refuses_values_the_libe3_module_cannot_carry",
        # Protobuf: libe3's ProtobufE3Encoder vectors against the hand-written codec, and the codec against
        # libprotobuf on both schemas (only built with libprotobuf, so a build without it fails here).
        "dapp_e3ap_protobuf_codec.decodes_and_reproduces_vectors_from_the_upstream_libe3_protobuf_encoder",
        "dapp_e3ap_protobuf_conformance.ocudu_schema_is_a_wire_superset_of_the_libe3_schema",
        "dapp_e3ap_protobuf_conformance.random_pdus_encode_exactly_as_libprotobuf_serialises_them",
        # E3AP-OCUDU-V2: one protocol with libe3, the same in APER and Protobuf.
        "dapp_e3ap_v2_codec.aper_and_protobuf_carry_the_same_protocol_in_both_profiles",
        "dapp_e3ap_v2_codec.every_ocudu_addition_round_trips_and_libe3_peers_see_the_libe3_message",
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
    selections = {
        "host": ["-R", r"^dapp_e3|^dapp_portable_data_store|^portable_data_store_test|^dapp_telemetry_isolation|^dapp_package_signature",
                 "-E", "by_reference_delivery_is_faster_than_in_band"],
        # Only what runs without kernel SCTP: the agent's SCTP carrier tests stay in the host profile.
        "zmq": ["-R", r"^dapp_e3ap_libe3\.(e2e_zmq|interop)$|^dapp_e3ap_libe3_codec\.|^dapp_e3ap_protobuf_(codec|conformance)\.|^dapp_e3ap_v2_codec\."],
        "sdk": [],
    }
    selection = selections[args.profile]
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
