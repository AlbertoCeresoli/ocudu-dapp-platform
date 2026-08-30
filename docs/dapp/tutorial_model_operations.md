<!-- SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc. -->
<!-- SPDX-License-Identifier: BSD-3-Clause-Clear -->

# Tutorial: neural-model operations — stage, validate, warm, activate, roll back

A Class-A dApp that carries a model (a neural receiver, learned CE/EQ, a
demapper table) can swap that model **live, without touching the instance's
lifecycle**, through six generation-guarded operations:

```
stage → validate → warm → activate      (the forward ladder)
rollback                                 (atomic revert to the previous model)
retire                                   (free a slot; the active model refuses)
```

This is the hands-on half of the continual-learning loop: capture data,
fine-tune offline or online, and publish the new model into the running cell
with an atomic, reversible activation. Reference docs:
[e3_management.md](e3_management.md) § model, the SDK's
[`class_a_cuda_authoring.md`](https://gitlab.com/ocudu/work_groups/wg2_ai_ran/ocudu-dapp-sdk/-/blob/main/docs/class_a_cuda_authoring.md),
and [`run_examples.md`](https://gitlab.com/ocudu/work_groups/wg2_ai_ran/ocudu-dapp-sdk/-/blob/main/docs/run_examples.md) §1.6.

## 1. What each step means (as the reference receiver implements it)

The CUDA receiver reference (`ref_receiver_cuda`, interface `0x00010005`) is
the worked model-management demo. Its "model" is deliberately simple — a
264-byte constellation-level table, not neural weights — so the *machinery*
is what you study:

| Op | What the host guarantees | What the reference does |
|---|---|---|
| `stage` | artifact bounds checked, CAS on model generation | copies the blob into the staged slot |
| `validate` | off the hot path, failures are attributable incidents | decodes + checks the artifact (magic, sizes, finite values, level separation) |
| `warm` | off the hot path | allocates and uploads **device state** into the spare slot; refuses if that slot is still the rollback target or still fenced by in-flight work |
| `activate` | generation published atomically | lock-free atomic slot exchange — the next invocation uses the new model |
| `rollback` | previous model retained | atomic swap back (itself reversible) |
| `retire` | active model refuses retirement | frees a slot once no queued work references it |

Two invariants to internalize: **all allocation happens in `warm`, never in
the invocation path**, and **`activate` is a publication, not a load** — by
the time you activate, the model is resident, validated, and warm.

## 2. Driving it with `dappctl`

```bash
DCTL="python3 apps/tools/dappctl/ocudu_dappctl.py --sctp 127.0.0.1:36423"

# One-time: build a demo artifact for the reference receiver
$SDK_BUILD/make_receiver_cuda_model /tmp/receiver-model.bin 0.75

# Read the instance generation (CAS guard for every op)
$DCTL inventory     # note the instance's generation G

$DCTL model stage    --instance $I --expected-generation $G \
      --model receiver-levels --version 1 --model-generation 1 \
      --artifact /tmp/receiver-model.bin
$DCTL model validate --instance $I --expected-generation $G2 \
      --model receiver-levels --model-generation 2
$DCTL model warm     --instance $I --expected-generation $G3 \
      --model receiver-levels --model-generation 3
$DCTL model activate --instance $I --expected-generation $G4 \
      --model receiver-levels --model-generation 4
# and if the KPIs disagree with the lab:
$DCTL model rollback --instance $I --expected-generation $G5 \
      --model receiver-levels --model-generation 5
```

**The generation discipline (this is the part people get wrong):**

- `--expected-generation` is the **instance** generation and
  `--model-generation` is the **model** generation. Both are compare-and-swap
  guards; both advance on every committed op. A `conflict` means your view is
  stale — someone (or your last op) advanced it. Re-read and retry.
- Model ops are **asynchronous**: the response is `accepted` + a job id, and
  the *new* generations come back on the job result. Follow up with:

  ```bash
  $DCTL jobs get --job <job-id>     # resourceGeneration = new model generation
  ```

- The current model generation is **not** shown in `inventory` — track it
  from job results (or start from 1: a fresh instance's model generation is 1
  and advances exactly once per committed op).
- Retries are safe: the host caches transactions, so re-issuing the identical
  request returns the original result instead of double-applying.

Dry-run works on every action (`--dry-run`): full validation, no mutation.
Note the surface: model ops live in **dappctl** (Python and C++); the
`ocudu_e3` library and the MCP server do not expose them today.

## 3. Real weights and the 1 MiB truth

The management-path artifact is bounded at **1 MiB** end to end (schema,
codec, host, clients). A constellation table fits; a real neural-receiver
weight set does not. The intended pattern for large models:

1. Deliver the weights **out of band** (your model store, a volume the dApp
   can read, your packaging pipeline).
2. Use the model operations as the **generation-guarded control signal**: the
   staged "artifact" is a small descriptor (path/URI/hash/config) your
   `validate` resolves and checks, your `warm` loads to the device, and
   `activate` publishes — keeping CAS, atomic activation, rollback, and the
   incident/audit trail without pushing gigabytes through E3.

That is also the shape of **online/streaming updates**: there is no delta or
chunking support in the protocol, but the full cycle can run repeatedly —
"publish a new generation every N minutes" is exactly what the
generation counter is for.

## 4. A/B testing: two honest patterns

**Model swap in place (one instance).** Stage → validate → warm on the live
instance, `activate`, compare KPI windows before/after, `rollback` if worse.
Rollback is atomic and retains the previous model, so the revert path is as
safe as the deploy path. Use the measurement discipline from the
[operations cookbook](operations_cookbook.md): interleave A/B arms in
alternating windows, let link adaptation settle after each switch, compare
CRC/MCS per RNTI over thousands of TBs, not seconds.

**Package-level A/B (two instances).** For comparing *implementations* (not
just weights), load the new package alongside the old, walk it to `shadow`,
then `activate` new / `deactivate` old — the
[dappctl tutorial's A/B workflow](tutorial_dappctl_operations.md#3-swap-a-package-for-a-new-version-the-ab-workflow).
One caveat stated plainly: a **shadow instance carries zero traffic**, so it
produces no comparable link KPIs — shadow is where you stage, configure, and
model-load safely; the KPI comparison happens across activation windows.

## 5. Implementing `manage_model` in your own dApp

Declare the capability bit (`OCUDU_DAPP_MANAGEMENT_MODELS_V1`) and implement
the single `manage_model` callback. The SDK gives you the whole state machine:

- `stateful_module_context` + the reference `module_support` implement the
  three-slot store (staged / active / previous), the CAS checks, the phase
  ladder (can't validate what isn't staged, can't activate what isn't warm),
  and the generation bump — you supply the *meaning* of validate/warm for
  your artifact.
- The CUDA receiver reference adds the production patterns worth copying
  verbatim: device-state slots with **per-lane completion fences** (a slot is
  only reusable when no queued kernel can still read it), atomic activation,
  and rollback that never frees memory the GPU might touch.
- `certify_class_a_cuda` drives the full matrix offline — two complete
  ladders, rollback, retire, and "retiring the active model must conflict" —
  so your implementation is proven before it meets an operator.

## 6. Exercising it without a radio

The model path itself needs a running dApp host but no radio: the CPU
reference packages accept the full ladder (the platform's own E2E harness
stages a plain byte fixture through a Class-C process instance in shadow and
walks it to generation 5). What a radio adds is only the thing that matters
last: **meaningful KPIs after activate**. Practice the ladder and the
generation discipline anywhere; judge models on the live cell.

See the whole journey in one story: [the capstone](tutorial_capstone.md).

## Next

- [Telemetry & data capture](tutorial_telemetry_capture.md) — the data half
  of the training loop.
- [dApp KPIs over E3](tutorial_dapp_kpis.md) — publish the metrics your A/B
  windows compare.
- [MCP monitoring](https://gitlab.com/ocudu/work_groups/wg2_ai_ran/ocudu-dapp-sdk/-/blob/main/docs/tutorial_mcp_monitoring.md) —
  agent loops that watch a model's health after activation.
