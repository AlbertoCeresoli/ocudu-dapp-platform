<!-- SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc. -->
<!-- SPDX-License-Identifier: BSD-3-Clause-Clear -->

# Capstone: the whole loop, one story

Every tutorial in this set is one station on a single journey: **improve a
live cell with a learned algorithm, safely, and keep improving it**. This
page walks that journey once, end to end, as a story with links — a fictional
vendor, *AcmeRF*, ships a learned uplink model into a running gNB and closes
the continual-learning loop. Each step is a few lines here and a full
tutorial elsewhere.

```
capture → train → package → admit → shadow → stage model → activate
   ↑                                                          │
   └── monitor (agent) ← compare A/B windows ← ... ← rollback if wrong
```

## Act 0 — a place to practice

Before touching a radio, AcmeRF builds the platform in a container and runs
every command in this story against the loopback E3 agent.
→ [Tutorial 0: the zero-hardware testbed](tutorial_docker_testbed.md)

## Act 1 — get the data

On a lab cell, they open the floor on the stream they need and record real
over-the-air uplink: equalized symbols with **complete grant parameters and
the gNB's own CRC verdicts** — a self-labelling dataset.

```bash
python3 record_telemetry.py pusch.symbols pusch.crc --rnti 0x4601 --output ue.h5
python3 export_pusch_burst.py ue.h5 --all --only-complete --output bursts/
```

The exported bursts replay offline through the gNB's own receiver chain
(`ocudu-pusch-replay`), giving them a ground-truth referee for every
algorithm iteration — capture once, iterate offline for weeks.
→ [Tutorial 8: telemetry & data capture](tutorial_telemetry_capture.md)

## Act 2 — train (your world, not ours)

Training happens in AcmeRF's own stack. Two platform facts shape it: the
management-path model artifact is bounded at 1 MiB — so large weights ship
out of band and the staged artifact is a small validated descriptor — and
whatever they train must honor the invocation contract (validate-or-bypass,
enqueue-and-return, no allocation on the hot path).
→ [Tutorial 9 §3](tutorial_model_operations.md) for the artifact pattern.

## Act 3 — become a package

One scaffold command gives them a self-contained vendor repo — the frozen
C ABI is the only coupling to the host; their release cadence is their own.
They replace `algorithm.cu`, keep the contract, and **certify offline**
against deterministic vectors before anyone else sees the code.
→ [Tutorial 1: a new vendor repo](https://gitlab.com/ocudu/work_groups/wg2_ai_ran/ocudu-dapp-sdk/-/blob/main/docs/tutorial_new_vendor_repo.md)

## Act 4 — onto the operator's cell

The operator drops the bundle into the catalog. In the lab, no keys —
`package_signature_mode: allow_unsigned` and go; for production they'll come
back for signatures, SBOMs, and a trust policy with sequence floors.
→ [Tutorial 2: install & YAML](tutorial_install_and_yaml.md), then
[Tutorial 5: signing & trust](tutorial_signing_and_trust.md)

## Act 5 — shadow first, always

```
load → prepare → arm → enter-shadow
```

The instance is warm, configured, model-loadable — and carries **zero
traffic**. Every generation-guarded step is reversible, and the breaker +
bypass machinery stands behind whatever happens next.
→ [Tutorial 3: dappctl operations](tutorial_dappctl_operations.md)

## Act 6 — the model goes live

Into the shadow instance: `stage → validate → warm`, each off the hot path,
each CAS-guarded. Then the moment of truth — `activate` is an atomic slot
publication, and `rollback` retains the previous model, so the revert path
is as safe as the deploy path. They compare interleaved KPI windows
(per-RNTI CRC/MCS over thousands of TBs) between old and new.
→ [Tutorial 9: model operations](tutorial_model_operations.md)

## Act 7 — hand the watch to an agent

A standing MCP loop reads the KPIs and the dApp's own published metrics
every few minutes, dry-runs any proposed adjustment, and flags regressions —
the "operations copilot" from a single prompt. AcmeRF's dApp exposed its
confidence score as a metric, so the agent watches *that* too.
→ [Tutorial 4: MCP monitoring](https://gitlab.com/ocudu/work_groups/wg2_ai_ran/ocudu-dapp-sdk/-/blob/main/docs/tutorial_mcp_monitoring.md)
and [Tutorial 6: exposing KPIs](tutorial_dapp_kpis.md)

## Act 8 — and around again

The cell keeps capturing; AcmeRF keeps training; new model generations flow
through the same stage→activate ladder — each one validated, warm before
live, and one `rollback` from undone. That loop — data to model to cell to
data — running under operator authority on an open platform, is the whole
point.

## When it goes wrong (it will)

Breaker open? Generation conflict? Silent telemetry? Every failure mode in
this story has a runbook entry.
→ [Tutorial 11: troubleshooting](tutorial_troubleshooting.md)

## The other two roads

This story rode Class A (inline model). The same journey runs on the other
two contracts: a **standalone Class-C** sensor is the fastest first mile
([Tutorial 7](tutorial_class_c_standalone.md)), and **Class-B** scheduler
control follows the same shadow-first, evaluation-first discipline
([Tutorial 10](tutorial_class_b_scheduler.md)). New words along the way live
in the [glossary](glossary.md).
