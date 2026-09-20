# OCUDU <-> libe3 end-to-end interoperability

This directory proves, with the real upstream library, that a dApp built on
[libe3](https://github.com/wineslab/libe3) (Northeastern University) attaches
to the OCUDU E3AP agent and runs a complete E3AP session, and that the two
ASN.1 encoders agree byte for byte.

## What is built

With `-DOCUDU_DAPP_LIBE3_INTEROP=ON` the build:

1. copies the `third_party/libe3` submodule (github.com/wineslab/libe3,
   pinned; the grammar in `schemas/e3/interop/libe3` is checked against it at
   configure time) into the build tree, or uses
   `-DOCUDU_LIBE3_SOURCE_DIR=<checkout>`, or, without the submodule, fetches
   `OCUDU_LIBE3_GIT_TAG`;
2. applies `patches/*.patch` to that copy (skipping any already present);
3. builds libe3 with `LIBE3_ENABLE_SCTP=ON`, `LIBE3_ENABLE_ZMQ=OFF`, the
   ASN.1 encoder only, against the same `asn1c` OCUDU uses: the
   `third_party/asn1c` submodule (github.com/mouse07410/asn1c, pinned), built
   at configure time by `cmake/modules/OCUDUAsn1c.cmake`;
4. builds `libe3_interop_dapp` and `libe3_interop_vectors`, which link libe3
   and nothing from OCUDU;
5. registers `dapp_e3ap_libe3.e2e` with CTest, which runs
   `python/tests/test_libe3_e2e.py`.

```
git submodule update --init third_party/asn1c third_party/libe3
cmake -S . -B build -DOCUDU_DAPP_LIBE3_INTEROP=ON
cmake --build build --target libe3_interop_dapp libe3_interop_vectors e3ap_test_server
ctest --test-dir build -R 'dapp_e3ap_libe3'
cmake --build build --target libe3_interop_report   # gate + report in build/dapp-interop-report/
```

## The report

`scripts/libe3_interop_report.py --build build` (the `libe3_interop_report`
target) runs the fail-closed gate `scripts/check_dapp_interop_gate.py` and
renders what it proved into `build/dapp-interop-report/`: provenance (OCUDU
commit, libe3 and asn1c submodule commits, patches applied, CMake options,
toolchain, binary build times), the coverage of every libe3 E3AP 1.0.0
procedure (pinned vector, codec reproduction, asn1tools crossing, exchanged
on the wire in the end-to-end session), the sessions the real libe3 dApp
reported, every required test with status and time, and the captured output
of the interoperability tests. Markdown always, PDF when `pdflatex` is on the
path; the `libe3 interoperability report` CI job publishes the directory as an
artifact exposed on the merge request. The exit code is
the gate's.

## The patches

`patches/0002-*.patch` moves libe3's own `build_libe3` asn1c pin from a
2024 commit to the one `third_party/asn1c` pins (two years of APER decoder
and fuzzer fixes) and lists the runtime units that generator emits, so
`libasn1_e3ap` links without compiling codecs out. Both projects then
generate from one `asn1c` commit, which is what makes the byte-exact vector
checks meaningful.

`patches/0001-*.patch` adds `E3LinkLayer::SCTP_ASSOCIATION` and
`SctpAssociationE3Connector` to libe3: one SCTP one-to-one association per
peer carries the setup, inbound and outbound channels, one E3-PDU per SCTP
message with `MSG_EOR` boundaries and no length prefix. This is how E2AP,
F1AP and NGAP frame their PDUs, how the OCUDU agent frames E3AP, and the
transport the operator community asked for on E3. The RAN role serves many
peers on one listener (setup is the first message of a fresh association,
later messages are inbound, outbound messages are broadcast to set-up peers,
as with the ZMQ and POSIX connectors); the dApp role gates `receive()` until
the setup thread has consumed the `SetupResponse`. Send and receive buffers
are preset before `listen()`/`connect()` and delayed SACK is off, so
multi-chunk messages do not stall on the kernel's 200 ms SACK timer.

The patch carries its own tests (`tests/test_role_aware_connector_sctp.cpp`,
`tests/integration/test_role_pair_sctp.cpp`, both skip on hosts without
SCTP) and is intended for submission upstream; it is kept here so the
interoperability tests do not wait on that.

## What the tests assert

`dapp_e3ap_libe3.e2e`:

- a libe3 dApp completes setup with a dApp identifier in 1..100, sees RAN
  functions 10 (telemetry, identifiers 1..8) and 11 (management, control 1);
- subscribes to the PUSCH CRC stream with a plain libe3 subscription request,
  receives indications (each at most 32 KiB), and gets the delete confirmed
  by an `E3-SubscriptionResponse`, which is what libe3's state machine waits
  for;
- sends a management control action (an APER `ManagementRequest` inventory
  query) and gets one positive `E3-MessageAck`; sends a report and gets one;
- releases, after which the agent closes the association;
- two consecutive dApps both succeed (identifiers are returned to the pool);
- `libe3_interop_vectors encode` still matches the pinned
  `python/tests/fixtures/libe3_vectors.json`, and libe3 decodes what
  asn1tools emits for the nine procedures that carry no `UTF8String`.

The pinned vectors are also checked, byte for byte, by the always-on C++ test
`dapp_e3ap_libe3_codec.decodes_and_reproduces_vectors_from_the_upstream_libe3_encoder`
and by `python/tests/test_libe3_interop.py`, so a default build without this
option still gates OCUDU's libe3 codec against libe3's real encoder output.

## Regenerating the vectors

```
build/tests/unittests/dapp/interop/libe3/libe3_interop_vectors encode > /tmp/vectors.jsonl
```

then rebuild `python/tests/fixtures/libe3_vectors.json` (name, hex, and the
`decode` output of each line) and `tests/unittests/dapp/e3ap_libe3_vectors.inc`
from it.
