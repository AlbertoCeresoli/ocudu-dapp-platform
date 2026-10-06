# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear
"""Evidence map for the OCUDU section of wineslab/libe3#84 (reviewed 2026-10-06).

Disposition describes interoperability scope, not test success. Known differences
are informational and must never change the CI exit code. Evidence is resolved by
exact CTest name against this run, never inferred from a generic green codec test.
"""

TRACKER = "https://github.com/wineslab/libe3/issues/84"
BASELINE = "Tracker audit: OCUDU 38fc00bb526c and libe3 152ea05 (0.2.1); reviewed 2026-10-06."


def server(name):
    return "dapp_e3ap_sctp_server." + name


def v2(name):
    return "dapp_e3ap_v2_codec." + name


def issue(number, topic, disposition, tests, note):
    return {"number": number, "url": f"https://github.com/wineslab/libe3/issues/{number}",
            "topic": topic, "disposition": disposition, "tests": tests, "note": note}


SCHEMA = "dapp_e3ap_libe3.interop"
SCTP = "dapp_e3ap_libe3.e2e_sctp"
ZMQ = "dapp_e3ap_libe3.e2e_zmq"
APER = "dapp_e3ap_libe3_codec.decodes_and_reproduces_vectors_from_the_upstream_libe3_encoder"
PROTOBUF = "dapp_e3ap_protobuf_codec.decodes_and_reproduces_vectors_from_the_upstream_libe3_protobuf_encoder"
BOUNDS = "dapp_e3ap_libe3_codec.refuses_values_the_libe3_module_cannot_carry"
NEGATIVE = server("rejects_unknown_subscription_ids_without_leaking_partial_admission")
GRANTS = server("repeat_subscriptions_report_grants_and_preserve_existing_ids_at_capacity")

# All 27 entries explicitly associated with OCUDU by #84, plus related lifetime
# issue #95. Do not silently remove an unresolved item when changing the tests.
ISSUES = [
    issue(81, "CI transport execution", "PROFILE-QUALIFIED", [SCTP, ZMQ],
          "Host profile exercises real SCTP and ZMQ. ZMQ-only runners do not qualify SCTP; absence is reported."),
    issue(85, "Encodings and links", "REMAINING-DIFFERENCE",
          [server("serves_every_encoding_and_profile_on_one_port"), SCTP, ZMQ],
          "OCUDU serves APER/Protobuf profiles and both carriers. JSON and a mixed-encoding upstream RAN are not qualified."),
    issue(87, "Service-model identity", "REMAINING-DIFFERENCE",
          [server("completes_setup_and_advertises_ran_functions")],
          "OCUDU advertises its catalogs on functions 10/11. Numeric IDs are not a cross-vendor schema registry; inspect function data."),
    issue(88, "Granted values", "REMAINING-DIFFERENCE", [GRANTS, ZMQ],
          "APER/OCUDU profiles report admitted lists; libe3 Protobuf has no granted-list fields. Effective periodicity is not returned; upstream API visibility is not proved."),
    issue(89, "Version/profile acceptance", "ALIGNED",
          [server("rejects_unknown_protocol_and_profile_versions_before_allocating_an_id"),
           server("rejected_setup_does_not_pin_the_proposed_profile"), APER, PROTOBUF],
          "Exact protocol 1.0.0 and optional OCUDU profile 1; rejected proposals do not bind a session. No arbitrary 1.x acceptance."),
    issue(90, "Disconnect notification", "REMAINING-DIFFERENCE",
          [server("enforces_setup_before_procedures_and_releases_cleanly"),
           server("closes_associations_that_never_complete_setup_or_speak_garbage")],
          "Host close/release behavior is tested. Server-initiated ReleaseMessage and stock libe3 reacting to abrupt loss remain unqualified."),
    issue(93, "Session cleanup", "PROFILE-QUALIFIED",
          [server("quiet_subscription_survives_idle_until_expiry_or_peer_disconnect"),
           server("reclaims_the_slot_of_a_peer_that_vanishes_with_output_pending")],
          "SCTP quiet subscriptions survive; disconnect and stalled output reclaim state. This does not prove ZMQ vanished-peer detection."),
    issue(95, "Subscription lifetime (related)", "REMAINING-DIFFERENCE",
          [server("subscription_expiry_reclaims_shared_memory_and_admission_budget"),
           server("finite_subscription_expires_despite_continuous_store_lease_renewal")],
          "OCUDU honors positive lifetime; zero/absent is unlimited. Store renewal cannot extend it. Upstream RAN semantics remain separate."),
    issue(96, "Unknown selections", "ALIGNED", [NEGATIVE],
          "Both encodings/profiles reject unknown functions, telemetry/control selections, duplicates and wrong dApp IDs without partial-store leaks."),
    issue(97, "Repeated subscriptions", "REMAINING-DIFFERENCE",
          [GRANTS, server("subscription_wrap_never_replaces_a_live_libe3_subscription")],
          "OCUDU creates distinct bounded subscriptions; capacity rejection and wrap preserve existing ones. It does not adopt upstream ID reuse."),
    issue(98, "Periodicity units/range", "REMAINING-DIFFERENCE",
          [server("normalizes_periodicity_units_before_applying_defaults_and_hard_floors"),
           server("hard_floor_and_millisecond_overrides_survive_store_lease_recovery"),
           v2("wider_subscription_ranges_ride_in_additions_and_root_stays_libe3")],
          "libe3 profile uses microseconds; OCUDU profile 1 retains milliseconds. Floors and defaults apply; the libe3 APER 0..1000 bound remains."),
    issue(99, "Identifier allocation/ranges", "REMAINING-DIFFERENCE",
          [server("native_and_libe3_identifiers_remain_disjoint_during_churn"),
           server("subscription_wrap_never_replaces_a_live_libe3_subscription"), BOUNDS, NEGATIVE],
          "Host reserves dApp IDs 1..100 for libe3 and accepts full-width message IDs. Upstream allocation/request-ID limits are not changed."),
    issue(100, "Negative-response detail", "REMAINING-DIFFERENCE", [NEGATIVE],
          "OCUDU peers receive cause/detail. libe3 peers receive a valid negative response without OCUDU-only diagnostic fields."),
    issue(101, "Interop documentation", "DOCUMENTED", [],
          "tests/interop/libe3/README.md describes 0.2.2, local patches, profiles and this report. Documentation review is not a runtime test."),
    issue(102, "Conformance fixtures", "ALIGNED", [APER, PROTOBUF, SCHEMA, ZMQ],
          "Eleven procedures in both encodings: pinned vectors plus current libe3 encoder/decoder execution, with the compiler qualification below."),
    issue(104, "SCTP association topology", "REMAINING-DIFFERENCE", [SCTP],
          "Test uses OCUDU patch 0001, a single SCTP association. Stock libe3's three-channel connector is not compatible/qualified."),
    issue(105, "SCTP framing", "REMAINING-DIFFERENCE", [SCTP],
          "Patched association connector uses one PDU per SCTP message. Stock length-prefixed SCTP framing is not exercised."),
    issue(106, "SCTP PPID/streams", "REMAINING-DIFFERENCE", [SCTP],
          "Patched connector interoperates with the host. Arbitrary PPID/stream combinations and stock connector metadata are not qualified."),
    issue(107, "Transport tuning/admission", "REMAINING-DIFFERENCE",
          [SCTP, server("streams_multi_megabyte_indications_without_delayed_sack_stalls"), GRANTS],
          "Loopback IPv4, bulk transfer and bounded admission are checked. IPv6, remote networks and performance parity are not qualified."),
    issue(108, "Text representation", "ALIGNED", [SCHEMA, APER],
          "V2 roots match libe3 including UTF8String; Python checks valid/nonvalid UTF-8. APER compiler caveat remains under #109."),
    issue(109, "ASN.1 compiler/runtime", "REMAINING-DIFFERENCE", [SCHEMA, ZMQ, SCTP],
          "libe3 is built with OCUDU's pinned asn1c and patch 0002. Separate processes prevent runtime collisions; stock-compiler APER is not qualified."),
    issue(110, "Payload size/fragmentation", "REMAINING-DIFFERENCE",
          [BOUNDS, server("drops_indications_the_libe3_module_cannot_carry"),
           server("streams_fragmented_telemetry_indications_to_subscribers")],
          "libe3 profile keeps its 32 KiB/no-fragment limit; larger native payloads are not silently sent on that profile."),
    issue(111, "Control outcomes", "ALIGNED",
          [server("serves_libe3_module_peers_on_the_same_port_beside_native_ones"),
           server("carries_management_requests_as_control_actions"), SCTP, ZMQ],
          "libe3 receives one MessageAck; OCUDU profile may additionally carry a service-model ControlResponse."),
    issue(112, "Optional OCUDU fields", "PROFILE-QUALIFIED",
          [v2("every_ocudu_addition_round_trips_and_libe3_peers_see_the_libe3_message"),
           server("delivers_large_tensors_by_reference_through_a_shared_region"), "dapp_e3_shared_region.validation"],
          "Extensions round-trip and libe3 sees the base message. SHM safety is OCUDU-profile coverage, not upstream SHM interoperability."),
    issue(113, "Schema bounds", "ALIGNED",
          [SCHEMA, BOUNDS, v2("wider_subscription_ranges_ride_in_additions_and_root_stays_libe3")],
          "Root constraints match libe3; larger OCUDU ranges use additions. Boundary cases do not exhaust every possible value."),
    issue(114, "Envelope timestamp", "ALIGNED", [SCHEMA, APER, PROTOBUF,
          server("stamps_outbound_pdus_with_the_realtime_clock")],
          "Current roots/fixtures agree on envelope placement; emitted timestamps are checked against the realtime clock."),
    issue(115, "Response-code extensibility", "ALIGNED", [SCHEMA, APER, PROTOBUF],
          "The current root uses libe3's positive/negative enumeration. A hypothetical third response code is not supported."),
    issue(116, "Sequence IDs", "ALIGNED",
          [v2("sequence_identifiers_are_mandatory_on_reports_and_xapp_controls"), SCTP, ZMQ],
          "Report/control sequence IDs are mandatory; the real peer session carries a sequence ID above the legacy 1000 limit."),
]


def issue_coverage(tests):
    statuses = {test["name"]: test["status"] for test in tests}
    rows = []
    for item in ISSUES:
        evidence = [{"test": name, "status": statuses.get(name, "not-run")} for name in item["tests"]]
        states = [entry["status"] for entry in evidence]
        if not states:
            result = "DOCUMENTATION"
        elif any(state in ("failed", "error") for state in states):
            result = "FAILED"
        elif all(state == "passed" for state in states):
            result = "TESTED"
        elif any(state == "passed" for state in states):
            result = "PARTIAL"
        else:
            result = "NOT RUN"
        rows.append({**item, "evidence": evidence, "evidence_result": result})
    return rows
