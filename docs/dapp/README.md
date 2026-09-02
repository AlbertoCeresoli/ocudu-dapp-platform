<!-- SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc. -->
<!-- SPDX-License-Identifier: BSD-3-Clause-Clear -->

# OCUDU dApp Runtime documentation

Public dApp ABI and schemas **v1**. Canonical host repository:
<https://gitlab.com/ocudu/work_groups/wg2_ai_ran/ocudu-dapp-platform>. This directory is the **Git-native**
(Markdown) entry point for the optional dApp Runtime and embedded E3 management
plane. Doxygen pages under [`../doxygen/`](../doxygen/) build HTML in the same
style as other OCUDU architecture notes.

## Start here

**Read the papers first?** The quickstart repository ships two preview papers: the [architecture and tutorial paper](https://gitlab.com/ocudu/work_groups/wg2_ai_ran/ocudu-dapp-quickstart/-/blob/main/papers/ocudu_dapp_tutorial.pdf) and the [measured interface study](https://gitlab.com/ocudu/work_groups/wg2_ai_ran/ocudu-dapp-quickstart/-/blob/main/papers/ocudu_dapp_interface_study.pdf).

**New to the platform? Follow the step-by-step tutorials** — a guided path
from an empty vendor repo to an LLM agent tuning a live cell. No radio needed:
the sibling `ocudu-docker/` repo builds everything into a container and ships
a loopback E3 agent the tutorial commands can target.

```bash
# clone the set side by side; the platform repo must be cloned as ocudu/
mkdir ocudu-workspace && cd ocudu-workspace
git clone https://gitlab.com/ocudu/work_groups/wg2_ai_ran/ocudu-dapp-platform.git ocudu
git clone https://gitlab.com/ocudu/work_groups/wg2_ai_ran/ocudu-dapp-sdk.git
git clone https://gitlab.com/ocudu/work_groups/wg2_ai_ran/ocudu-dapp-quickstart.git ocudu-docker
```

| # | Tutorial | For | Covers |
|---|----------|-----|--------|
| 0 | [The zero-hardware testbed](tutorial_docker_testbed.md) | everyone | build the platform in docker; loopback E3 agent; run the tutorial commands radio-free (see its works-here table) |
| 1 | [Stand up a new vendor dApp repo](https://gitlab.com/ocudu/work_groups/wg2_ai_ran/ocudu-dapp-sdk/-/blob/main/docs/tutorial_new_vendor_repo.md) | vendors | scaffold → build → certify (no keys needed) |
| 2 | [Install & load dApps from the gNB YAML](tutorial_install_and_yaml.md) | operators / SIs | lab quickstart (no keys), catalog, `initial_instances`, admission |
| 3 | [Operate dApps with `dappctl`](tutorial_dappctl_operations.md) | operators | lifecycle, version swap / A/B, deactivate/unload, circuit breakers |
| 4 | [Monitor & tune over the MCP server](https://gitlab.com/ocudu/work_groups/wg2_ai_ran/ocudu-dapp-sdk/-/blob/main/docs/tutorial_mcp_monitoring.md) | operators / researchers | ask the gNB questions and run KPI-driven LLM tuning loops |
| 5 | [Signing, SBOMs & key management](tutorial_signing_and_trust.md) | vendors + operators | production trust: what gets signed, the SBOM, trust policy, rotation/revocation, custody |
| 6 | [Exposing dApp KPIs & querying over E3](tutorial_dapp_kpis.md) | vendors + operators | declare metrics in a dApp; read them via dappctl, `ocudu_e3`, a dashboard, or the MCP agent |
| 7 | [Your first dApp: standalone Class-C over E3](tutorial_class_c_standalone.md) | developers | Python (or C++) out-of-process dApp: subscribe, decode numpy snapshots, act back — no CUDA, no signing |
| 8 | [Telemetry deep-dive & data capture](tutorial_telemetry_capture.md) | researchers | all 8 streams, floors & rate model, HDF5 capture, offline PUSCH replay |
| 9 | [Neural-model operations](tutorial_model_operations.md) | vendors + operators | stage → validate → warm → activate → rollback, generation discipline, live A/B |
| 10 | [Class-B scheduler control](tutorial_class_b_scheduler.md) | vendors + operators | the ~100 µs contract, allow-flags, evaluation-first ladder, sensing→avoidance |
| 11 | [Troubleshooting runbook](tutorial_troubleshooting.md) | everyone | symptom → cause → fix, from real failure modes |
| 12 | [Capstone: the whole loop](tutorial_capstone.md) | everyone | one story through all of it: capture → train → package → shadow → activate → monitor → rollback |

New words along the way: the **[glossary](glossary.md)**. Want to extend the
platform itself — a new contract, stream, or operation? Read
**[proposing_extensions.md](proposing_extensions.md)**: the registries, the
evolution rules, and the working-code-first path to consensus.

**Or start with [quickstart.md](quickstart.md)** — it walks the whole loop
(build → package → catalog → YAML → enable over E3 → run and verify) and is the
authoritative copy of the Class-A CSI / DMRS / wall-budget notes that the other
guides reference. The remaining guides are references for one aspect each:

| Guide | Audience |
|-------|----------|
| [quickstart.md](quickstart.md) | **Start here.** End-to-end: package → catalog → enable/disable → monitor → run and verify |
| [capabilities.md](capabilities.md) | The complete capability reference: Classes A/B/C, all telemetry streams, every management operation, carriers and clients |
| [operations_cookbook.md](operations_cookbook.md) | Verified operator recipes: record/replay, A/B via lifecycle, scheduler tuning, RAN control, sensing, channel emulation |
| [extension_model.md](extension_model.md) | Algorithm authors: packaging, placement, and the host boundary |
| [yaml_configuration.md](yaml_configuration.md) | Complete `dapp:` configuration reference |
| [e3_management.md](e3_management.md) | E3 management operations and client usage |
| [e3_wire.md](e3_wire.md) | Carriers (local socket, E3AP over SCTP, E3 data profile), APER layout, telemetry streams, interop notes |
| [e3_system_design.md](e3_system_design.md) | Runtime architecture, timing classes, composition |
| [security_model.md](security_model.md) | Trust, containment, admission, and class-specific failure behavior |
| [api_compatibility.md](api_compatibility.md) | ABI/schema stability and platform qualification |
| [release_checklist.md](release_checklist.md) | Required public-release evidence and gates |
| [known_limitations.md](known_limitations.md) | Explicit limitations and pending platform evidence |

## Doxygen pages (HTML via `cmake --target doxygen`)

The Markdown set in this directory is the normative, Git-rendered prose; the
Doxygen pages are condensed HTML companions of the same material that link into
the API reference (`\ref ocudu::dapp::...`). When they disagree, the Markdown
and the code win; each `.dox` page names its Markdown companion at the top.

| Page | File |
|------|------|
| `dapp_quickstart` | `dapp_quickstart.dox` |
| `dapp_operations` | `dapp_operations.dox` |
| `dapp_observability` | `dapp_observability.dox` |
| `dapp_release_provisioning` | `dapp_release_provisioning.dox` |
| `e3_management` | `e3_management.dox` |
| `e3_wire` | `e3_wire.dox` |
| `e3_system_design` | `e3_system_design.dox` |

```bash
cmake --build <ocudu-build> --target doxygen
# open <build>/docs/doxygen/html/index.html
# or: docker compose -f docs/docker-compose.yml up
```

## Normative schemas

| Schema | Role |
|--------|------|
| `schemas/e3/e3sm-ocudu-dapp-mgmt-v1.asn1` | Management service model (APER) |
| `schemas/e3/e3ap-ocudu-v1.asn1` | E3AP envelope for the SCTP carrier (APER) |
| `schemas/e3/e3sm-ocudu-telemetry-v1.asn1` | Telemetry service model: PUSCH/SRS/DM-RS records, gNB JSON, Class-C snapshots |
| `schemas/e3/e3sm-ocudu-dapp-mgmt-carrier-v1.asn1` | Management RAN-function capability advertisement |
| `schemas/dapp/e3-data-profile-v1.fbs` | FlatBuffers data profile served to the E3 SCTP reference client |
| `schemas/dapp/package-manifest-v1.schema.json` | Package manifest |
| `schemas/dapp/package-signature-v1.schema.json` | Detached signature sidecar |
| `schemas/dapp/package-trust-policy-v1.schema.json` | Offline trust policy |
| `schemas/dapp/package-spdx-sbom-profile-v1.schema.json` | SBOM profile |
| `schemas/dapp/class-c-publication-v1.fbs` | Class-C process metadata |
| `schemas/dapp/*-report-v1.schema.json` | Qualification / evidence reports |

## Python client

`python/ocudu_e3` is a standard-library plus `asn1tools` client for the E3AP
carrier (setup, telemetry subscriptions with fragment reassembly, management
control) and a decoder for the packed telemetry records of the pull path. The
`E3Client` class also exposes the management operations directly —
`inventory()`, `health()`, `metrics()`, `incidents()`, `cell_info()` (served
cells and RAN-control capabilities) — and `ocudu_e3.management` builds
`ranControl` requests (UE release/handover, RF gain). Run
`python3 -m ocudu_e3 --help`; see [`../../python/README.md`](../../python/README.md).

## Out-of-tree SDK

Authors build packages against the installed `OCUDUDAppSDK` package using the
standalone **ocudu-dapp-sdk** repository:

- the six working reference dApps are intended to be extended with stronger algorithms;
- keep the reference lifecycle, validation, memory/stream, metrics, and host-authority shell;
- replace the CE/EQ/RX, scheduler, spectrum, or SRS/ISAC algorithm core;
- Markdown: `docs/ocudu_dapp_quickstart.md`, class authoring guides
- Doxygen: `docs/doxygen/` (`cmake --target doxygen` in the SDK build)

## Tools

| Tool | Role |
|------|------|
| `ocudu-dappctl` | C++ local E3 client |
| `apps/tools/dappctl/ocudu_dappctl.py` | Python/JSON E3 client; local socket (`--socket`, default `/run/ocudu/dapp-management.sock`, env `OCUDU_DAPP_E3_SOCKET`) or E3AP SCTP (`--sctp host:port`, env `OCUDU_DAPP_E3_SCTP`); `codec-self-test` verifies the APER codec against independent golden vectors |
| `ocudu-dapp-package` | Offline keygen / sign / trust / inspect |
| `ocudu-dapp-preflight` | Disposable native preflight helper |

Build flags: `-DENABLE_DAPP=ON` and typically `-DENABLE_E3=ON`. Without a
`dapp:` YAML block the modem path remains unchanged even when the feature is
compiled in. See [quickstart.md](quickstart.md) § Run and verify for a minimal
enable-and-check recipe (`ctest -R dapp`, `python3 -m ocudu_e3 catalog`,
`ocudu_dappctl.py --sctp 127.0.0.1:36423 inventory`).

## Class-A equalizer CSI and DMRS notes (host)

See [quickstart.md](quickstart.md) § Class-A equalizer health under UL traffic —
the authoritative copy of the post-EQ SINR, `dmrs.reserved[0]` and soft
wall-budget notes (demodulator 2000 µs, channel estimator 800 µs).

Architecture decision log: `dapp-architecture-design/design_refinements.md` (§2026-07-16).

## Offline PUSCH replay

`ocudu-pusch-replay` (`apps/tools/pusch_replay`) decodes a PUSCH burst recorded over E3 (`pusch.symbols` with its `PuschGrantParameters`) using the gNB's own demapper, descrambler, UL-SCH demultiplexer and LDPC decoder and compares the transport-block CRC with the verdict the gNB reported. Bursts are exported from a recorder HDF5 with the SDK's `export_pusch_burst.py`. See `e3_wire.md`, section "Decoding recorded PUSCH bursts offline".
