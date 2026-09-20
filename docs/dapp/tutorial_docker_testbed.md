<!-- SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc. -->
<!-- SPDX-License-Identifier: BSD-3-Clause-Clear -->

# Tutorial 0: the zero-hardware testbed — every tutorial without a radio

You can follow the entire tutorial series with **no gNB, no radio, and no
GPU**: the sibling `ocudu-docker/` repository builds the two public repos into
a reproducible container image and ships a **loopback E3 agent** that serves
synthetic telemetry (PUSCH symbols/CRC, SRS, gNB state) over the same E3AP
and E3DP wire protocols a live cell uses. One build command from clone, then
minutes to your first live subscription.

## 1. Build the image (once)

```bash
mkdir ocudu-workspace && cd ocudu-workspace
git clone --recurse-submodules https://gitlab.com/ocudu/work_groups/wg2_ai_ran/ocudu-dapp-platform.git ocudu
git clone https://gitlab.com/ocudu/work_groups/wg2_ai_ran/ocudu-dapp-sdk.git
git clone https://gitlab.com/ocudu/work_groups/wg2_ai_ran/ocudu-dapp-quickstart.git ocudu-docker
ocudu-docker/scripts/build.sh          # or: OCUDU_BUILD_VARIANT=cpu ...
```

The platform repo must sit in a directory named `ocudu` and the testbed in
`ocudu-docker` — the docker build and the cross-repo documentation links
depend on those directory names.

This compiles the OCUDU host (dApp runtime + E3 + gNB + tools) and the full
CPU SDK, **running every release gate during the build** — a green image is a
validated platform, not just compiled bits. See `ocudu-docker/README.md` for
build arguments and the CUDA variant.

## 2. Start the loopback E3 agent

```bash
sudo modprobe sctp    # once per boot; containers use the host kernel's SCTP
docker compose -f ocudu-docker/docker-compose.yml up e3-agent
```

The agent listens on E3AP `sctp/36423` and the FlatBuffers data profile on
`sctp/38472`, publishing synthetic multi-UE telemetry every 20 ms.

> **Two gotchas.** The agent exits when its stdin closes — compose handles
> this with `tty: true`; if you run the binary by hand, hold stdin open
> (`sleep 300 | e3ap_test_server ... &`). And if a real gNB is already
> listening on 36423 on this host, remap the published port in compose first.

## 3. Point the tutorial commands at it

```bash
export PYTHONPATH=ocudu/python
export OCUDU_E3_SCHEMA_DIR=ocudu/schemas/e3

# Discover what the "cell" serves
python3 -m ocudu_e3 --port 36423 catalog

# Watch live (synthetic) CRC outcomes across UEs
python3 -m ocudu_e3 --port 36423 subscribe pusch.crc --maximum 10

# The same management surface dappctl drives on a real cell
python3 ocudu/apps/tools/dappctl/ocudu_dappctl.py --sctp 127.0.0.1:36423 inventory
```

Everything runs inside the image too (`docker run --rm -it
ocudu-dapp-release:cpu-local`), where the environment above is preset.

## 4. What works here, and what needs a real cell

| Works against the loopback agent | Needs a live gNB |
|---|---|
| E3AP setup, telemetry catalog, subscriptions (`pusch.symbols`, `pusch.crc`, `srs.channel`, gNB state) | Real PHY data, real UEs, real KPIs |
| E3DP data-profile create/poll flow on 38472 | Class-A inline processing (CUDA L1) |
| Management wire path: `inventory`, `codec-self-test` | Package admission, lifecycle, breakers |
| Writing/testing a standalone Class-C consumer end to end | Class-B scheduler effects on traffic |
| The MCP server's connection + observation tools | Acting on a cell that pushes back |

The loopback agent is a *wire-faithful* peer: a consumer you build against it
(the [standalone Class-C tutorial](tutorial_class_c_standalone.md)) connects
to a real cell unchanged — only the endpoint differs.

## 5. Re-run the gates whenever you change something

```bash
docker compose -f ocudu-docker/docker-compose.yml run --rm tests
```

That replays the host/E3/SDK test suites inside the image — the same gates
the image build enforces, kept handy for iterating.

## Got a radio?

Radio support is built into the same images — the UHD driver for a
**split-8 USRP** (B200/B210), **split-7.2 O-RU** Open Fronthaul, and the DPDK
fronthaul backend — with example gNB services and configs ready to run; see
the testbed README's "Run with a radio" section.

## Next steps

- [Install & load dApps from the gNB YAML](tutorial_install_and_yaml.md) —
  the operator path, whose lab quickstart needs no keys.
- [Write a standalone Class-C dApp](tutorial_class_c_standalone.md) — build
  your first dApp against this very testbed.
- [Telemetry deep-dive & data capture](tutorial_telemetry_capture.md) — every
  stream, floors, and recording.
