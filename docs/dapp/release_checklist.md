<!-- SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc. -->
<!-- SPDX-License-Identifier: BSD-3-Clause-Clear -->

# dApp and E3 release checklist

1. Freeze compatible host, SDK, ABI, schema, and generated-code revisions.
2. Run formatting, header/license, documentation-link, Doxygen-warning, schema-generation, and clean-install consumer gates.
3. Run GCC/Clang ABI fingerprints, CPU and CUDA unit/integration suites, ASan, UBSan, and parser mutation/fuzz targets.
4. Exercise package hash/SBOM/signature/trust/revocation, malformed package input, native preflight isolation, and failure gallery.
5. Qualify Class A correctness and complete-boundary deadline behavior; qualify Class B policy authority and timing; qualify Class C bounded throughput, overload, process death, and restart.
6. Run live attach/traffic plus dApp load, enable/disable, shadow/active transition, rollback, unload, and sustained soak. Separate RF/UE instability from dApp health evidence.
7. Record the GB10 primary report and GH200/RTX 4090 native reports, identifying unavailable OTA hardware explicitly.
8. Review public extension, security, compatibility, operator, and known-limitations documentation.
9. Release source and documentation from clean default-branch commits. Use simple protected tags; no special preview branch or prebuilt binary release is required.

A required failing gate blocks the tag. A waiver records its owner, risk, rationale, and follow-up issue in the release evidence.

The required cross-repository jobs run `scripts/check_dapp_interop_gate.py`:
real patched-libe3 E2E, native/Python wire compatibility, SHM validation,
telemetry isolation, and the SDK built against that host's installed SDK.
Missing or skipped required tests fail the job. Record both full commit IDs.
The SDK job requires a paired host commit containing this gate; update its host
lock (or explicitly set `OCUDU_HOST_REF`) after the host changes are committed
and published. Never waive the job merely because the old host pin lacks it.
Run the comparative delivery benchmark separately under a controlled load;
functional conformance does not certify timing, CUDA, or OTA performance.

## E3 SCTP carriers and telemetry

| Gate | Evidence |
|------|----------|
| ASN.1 modules validate with both toolchains | `asn1tools parse` at configure time; asn1c APER generation for `E3-PDU`, `Telemetry-*`, `Mgmt-RanFunctionData` and the libe3 `E3-PDU` with the pinned `third_party/asn1c` (bumping that submodule regenerates the codecs and requires re-pinning `python/tests/fixtures/libe3_vectors.json` and the golden vectors) |
| Codec round trips and cross-language golden vectors | `dapp_runtime_test --gtest_filter='dapp_e3ap_codec.*'` and `python/tests/test_e3ap_client.py` pin identical setup bytes |
| Producer never blocks | `dapp_telemetry_publisher.*` (gate before capture, per-RNTI floors, drop accounting) |
| E3AP procedures | `dapp_e3ap_sctp_server.*`: setup/refusal, fragmented indications with RNTI filter, management as control action, setup-required, garbage close, idle/setup timeouts |
| Data-profile compatibility | `dapp_e3dp_sctp_server.*`: reference-client create/renew/conflict/poll/remove flow and semantic-error handling |
| Python client against the loopback agent | `ctest -R dapp_e3ap_python.cross_language` |
| Live observer | `python3 -m ocudu_e3 subscribe pusch.crc gnb.metrics_json` against the reference cell; dashboard `/api/health` reports `e3_stream.connected` |
| Documentation | `e3_wire.md`, `e3_management.md`, `yaml_configuration.md`, `known_limitations.md` mention every new key, stream, bound and limitation |
