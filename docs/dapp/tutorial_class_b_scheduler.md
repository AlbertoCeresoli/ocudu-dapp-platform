<!-- SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc. -->
<!-- SPDX-License-Identifier: BSD-3-Clause-Clear -->

# Tutorial: Class-B scheduler control — bounded real-time RAN steering

Class B lets a dApp advise the MAC scheduler **inside the slot decision**,
under a hard time budget and an operator-owned authority model. This
tutorial covers the contract, the safety machinery, the reference scheduler,
and the evaluation-first workflow that takes a scheduler dApp from admitted
to actively steering a cell.

Reference docs: the SDK's
[`class_b_scheduler_baseline.md`](https://gitlab.com/ocudu/work_groups/wg2_ai_ran/ocudu-dapp-sdk/-/blob/main/docs/class_b_scheduler_baseline.md)
and [`class_b_accelerator_authoring.md`](https://gitlab.com/ocudu/work_groups/wg2_ai_ran/ocudu-dapp-sdk/-/blob/main/docs/class_b_accelerator_authoring.md),
[yaml_configuration.md](yaml_configuration.md),
[operations_cookbook.md](operations_cookbook.md) § scheduler tuning.

## 1. The contract in one paragraph

Per scheduling decision the host hands the dApp a snapshot of every
**candidate** UE — pending bytes, head-of-line delay, average rate, CQI,
SINR, BLER (64-decision window), power headroom, the host's own OLLA offset,
last outcome/MCS, MCS-table context — plus, when available, the per-PRB
**interference context** published by a Class-C sensing dApp and the free-VRB
map. The dApp answers with **intents**: priority, PRB limits/preference,
recommended/maximum MCS, layers, TPC, beam, and an optional per-PRB
**avoid mask**. Intents are *advice*: the host validates every field against
the operator's allow-flags and commits or discards them atomically.

**The budget is ~100 µs for the complete boundary** — feature preparation,
your callback, validation, and commit. It is a host default, deliberately
not a YAML knob. Overruns are handled at four checkpoints: too late before
invoke → skip; late or invalid after invoke → discard; late after commit →
**explicit rollback** of everything applied. In every case the conventional
scheduler result stands — it is computed first and is always the fallback.
Repeated deadline misses open the instance's circuit breaker (8 consecutive
observations), which routes conventionally until an operator `breaker-probe`.

## 2. The authority model: everything defaults to observe-only

All authority is opt-in YAML, default **off**:

```yaml
dapp:
  scheduler_evaluation_only: true      # measure without authority (start here)
  scheduler_allow_mcs: true            # each 'allow' admits one intent class
  scheduler_allow_prb_limits: true
  scheduler_allow_prb_preference: true
  scheduler_allow_tpc: true
  scheduler_allow_layers: true
  # scheduler_allow_prb_assignment: true   # needs the free-VRB snapshot
  max_scheduler_ul_prbs_avoided: 8     # 0 (default) disables avoid-mask authority
  scheduler_max_mcs_delta_from_host: 4 # bound how far MCS advice may stray
```

Two distinct safety layers, often confused:

- **`scheduler_evaluation_only`** runs the dApp *after* the conventional
  policy on every decision — full invoke, full validation, incidents and
  breakers live — then **discards the output** and counts it as
  `class_b.policy.evaluation_accepted`. This is how you measure a scheduler
  dApp's behavior and timing under real load with zero authority. (It is not
  lifecycle `shadow`; a Class-B instance must be `active` to be invoked at
  all.)
- **The allow-flags** bound what an *authoritative* dApp may change. Unset
  capability = that intent class is silently unavailable to the dApp, which
  is told so via `allowed_output_flags`.

## 3. The reference scheduler: five stages, all shipped off

`org.ocudu.reference.scheduler.cpu` (interface `0x00010006`) is a working
five-stage pipeline — **OLLA → RANK → MCS → PACK → TPC** — whose shipping
defaults are deliberately host-equivalent: every stage is `off` /
`conventional_passthrough`, so activating it with every allow-flag set
**changes nothing on air** until you enable a stage. Its knobs are a typed
E3 config schema (tune live, no restart):

| Key | What it does |
|---|---|
| `rank_mode`, `olla_mode`, `mcs_mode`, `pack_mode`, `tpc_mode` | enable each stage |
| `olla_target_bler` (default 0.1), `olla_step_db`, `olla_max_offset_db` | outer-loop link adaptation |
| `tpc_deadband_db`, `tpc_step_hi_db` | closed-loop power control shaping |
| `pf_fairness_coeff`, `backlog_weight`, `scheduling_request_bonus`, `consecutive_winner_penalty` | ranking behavior |
| `enable_prb_avoidance`, `prb_avoidance_threshold` | act on the sensing dApp's interference map |
| `emit_only_allowed` (default true) | never emit an intent class the host didn't allow |

There is no "MCS cap" key in the reference — the operator-side bound is the
host's `scheduler_max_mcs_delta_from_host`; a custom stage can emit
`maximum_mcs` intents for a hard cap.

## 4. From admitted to steering: the evaluation-first ladder

**Step 1 — evaluate with zero authority.** YAML:
`scheduler_evaluation_only: true` *plus* the allow-flags you eventually
intend (so the package exercises its full intent surface). Restart the DU,
then:

```bash
DCTL="python3 apps/tools/dappctl/ocudu_dappctl.py --sctp 127.0.0.1:36423"
$DCTL load --package <scheduler-package-id> --placement in-process --backend cpu
$DCTL lifecycle prepare --instance $I --expected-generation $G
$DCTL lifecycle arm --instance $I --expected-generation $G2
$DCTL lifecycle enter-shadow --instance $I --expected-generation $G3
$DCTL lifecycle activate --instance $I --expected-generation $G4
```

**Step 2 — watch the evaluation KPIs** (`metrics get`):
`class_b.policy.invocations` advancing; `evaluation_accepted` tracking it;
`fallbacks`, `validation_rejections`, `deadline_misses` at or near zero; and
`dapp_time_ns` comfortably inside the 100 µs boundary under *your* cell's
load. The qualification bar: zero validation rejections and every deadline
miss investigated **before** granting authority.

**Step 3 — tune live over E3** (async: follow the job):

```bash
$DCTL config get --instance $I                      # schema + generation
$DCTL config set --instance $I --config-generation $N \
     --set "olla_mode=str:contained_baseline" --set "olla_target_bler=f64:0.05"
$DCTL jobs get --job <job-id>                        # the real outcome
```

**Step 4 — grant authority, one stage at a time.** Flip
`scheduler_evaluation_only: false`, restart, re-activate — then enable
**one** stage, measure, and only then the next. Judge each step by *cell*
KPIs (attach/RRC stability, per-RNTI CRC/MCS/throughput over thousands of
TBs), not only the dApp's own counters: clean `class_b.policy.*` metrics
prove the dApp behaved, not that the RF configuration is healthy.

**Rollback** is boring by design: `deactivate` → `unload` (or drop the
manifest and restart). The conventional scheduler is always underneath.

## 5. Composition: sensing in, quiet out

Class-B closes the loop with Class-C sensing **without dApps ever calling
each other**:

- A spectrum dApp publishes a per-PRB **interference map** into a host-owned,
  provenance-checked cache; the scheduler boundary *borrows* an immutable
  snapshot per decision (lease, 250 ms max residence) and exposes it as the
  candidate context. The reference scheduler's `enable_prb_avoidance` +
  `prb_avoidance_threshold` turn it into avoid-mask intents, bounded by
  `max_scheduler_ul_prbs_avoided`.
- **Quiet-period reservations** run through a host authority with hard
  budgets (lead time, horizon, duration, PRBs/slot, duty cycle): a sensing
  dApp *requests*, the host *grants and clamps*, and granted windows apply
  to UL scheduling even if the requesting dApp dies — or if no scheduler
  dApp is bound at all. Inspect live: `$DCTL quiet-reservations --cell 0`.
  (Reservations are created by in-process/supervised dApps through the host
  API; E3 clients observe them.)

## 6. KPI reference

`class_b.policy.` … `invocations`, `accepted`, `evaluation_accepted`,
`fallbacks`, `validation_rejections`, `deadline_misses`, plus the timing
decomposition `conventional_time_ns` / `feature_time_ns` / `dapp_time_ns` /
`validation_time_ns` / `commit_time_ns` / `total_time_ns`. Healthy
authoritative operation: `accepted ≈ invocations`, the rest ≈ 0, and the
time split showing *your* code (not the boundary) owning the budget.

## 7. No radio required to start

The full Class-B lifecycle — load, evaluate, authority, config tuning,
restart-survival — runs against the **dummy-RU** end-to-end harness with no
radio, and the reference pipeline's algorithm stages are covered by offline
SDK tests (`ctest -R reference_scheduler`). What the live cell adds is the
part that matters last: real candidate features and the KPI evidence for
granting authority.

See the whole journey in one story: [the capstone](tutorial_capstone.md).

## Next

- [Standalone Class-C dApp](tutorial_class_c_standalone.md) — build the
  sensing side of the composition.
- [dappctl operations](tutorial_dappctl_operations.md) — the lifecycle and
  breaker mechanics used here.
- [MCP monitoring](https://gitlab.com/ocudu/work_groups/wg2_ai_ran/ocudu-dapp-sdk/-/blob/main/docs/tutorial_mcp_monitoring.md) —
  agent loops on the same KPIs.
