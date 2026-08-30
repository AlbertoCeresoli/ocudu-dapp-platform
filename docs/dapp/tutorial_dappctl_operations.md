<!-- SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc. -->
<!-- SPDX-License-Identifier: BSD-3-Clause-Clear -->

# Tutorial: operating dApps with `dappctl`

Once packages are in the catalog (see the
[install/YAML tutorial](tutorial_install_and_yaml.md)), this covers the daily
operator loop: **load → prepare → arm → shadow → activate**, swapping a
package for a new version, deactivating and unloading, and dealing with
circuit breakers. Everything here works identically over the local UNIX socket
or over E3AP/SCTP.

Reference: [e3_management.md](e3_management.md) (full operation catalog and the
generation model). Recipes: [operations_cookbook.md](operations_cookbook.md).

## The two clients

```bash
# C++ (built at apps/tools/dappctl)
ocudu-dappctl <command> ...

# Python (same surface; needs asn1tools). Pick a transport:
python3 apps/tools/dappctl/ocudu_dappctl.py --socket /run/ocudu/dapp-management.sock <command> ...
python3 apps/tools/dappctl/ocudu_dappctl.py --sctp 127.0.0.1:36423 <command> ...
```

The examples below use the Python client with `--socket`; substitute `--sctp
host:port` for remote management. Defaults: `--socket` falls back to
`OCUDU_DAPP_E3_SOCKET` or `/run/ocudu/dapp-management.sock`; `--sctp` falls back to
`OCUDU_DAPP_E3_SCTP`.

## The lifecycle, in one picture

```
        load                prepare      arm       enter-shadow   activate
DISCOVERED ─▶ LOADED ─▶ PREPARED ─▶ ARMED ─▶ SHADOW ─────────▶ ACTIVE
                │            │         │        │                  │
              unload    (rollback via deactivate / retry-quiescence)
                                                        deactivate │
                                     INACTIVE ◀──────────────────  ▼
```

- **prepare** does the heavy one-time work (device state, weight upload).
- **arm** wires the hooks but leaves the gate disabled.
- **enter-shadow** admits invocations that are *measured but never applied* —
  the counterfactual-evidence state. Configuration and model operations are
  legal here, so this is where you warm a model against live traffic shape
  with zero exposure.
- **activate** flips the gate; the dApp now affects the radio.
- **deactivate** returns to `INACTIVE`; **unload** removes it.

Every mutating command supports `--dry-run` (validate, don't commit) and
optimistic concurrency via `--expected-generation`.

## 1. Load and drive to active

```bash
# See the catalog + what is loaded (packages carry hex IDs)
python3 .../ocudu_dappctl.py --socket $SOCK inventory

# Load a package into an instance (in-process CUDA here)
python3 .../ocudu_dappctl.py --socket $SOCK load \
  --package <package-id-hex> --placement in-process --backend cuda
#   → returns accepted + jobId; poll it (see §5) — jobs are async

# Walk the lifecycle (each takes --instance <id>)
python3 .../ocudu_dappctl.py --socket $SOCK lifecycle prepare      --instance <id>
python3 .../ocudu_dappctl.py --socket $SOCK lifecycle arm          --instance <id>
python3 .../ocudu_dappctl.py --socket $SOCK lifecycle enter-shadow --instance <id>
# … observe in shadow (metrics, evaluation-mode counters) …
python3 .../ocudu_dappctl.py --socket $SOCK lifecycle activate     --instance <id>
```

Accepted `lifecycle` actions (aliases in parentheses): `prepare`, `arm`,
`enter-shadow` (`shadow`), `activate` (`enable`), `deactivate` (`disable`),
`retry-quiescence` (`retry`), `unload`, `breaker-probe` (`probe`).

## 2. Configure and read metrics

```bash
# Typed config set (schema-checked); --boundary controls when it takes effect
python3 .../ocudu_dappctl.py --socket $SOCK config set --instance <id> \
  --config-generation <gen-from-config-get> \
  --set some_key=i64:42 --set another=bool:true --boundary invocation

# Read back the effective config / schema
python3 .../ocudu_dappctl.py --socket $SOCK config get    --instance <id>
python3 .../ocudu_dappctl.py --socket $SOCK config schema --instance <id>

# Metrics (schema, then samples)
python3 .../ocudu_dappctl.py --socket $SOCK metrics schema  --instance <id>
python3 .../ocudu_dappctl.py --socket $SOCK metrics get --instance <id>
```

## 3. Swap a package for a new version (the A/B workflow)

The safe pattern is **shadow the new version alongside the old active one,
compare, then flip** — this is exactly how the over-the-air receiver A/B
comparisons are run.

```bash
# 1. Stage the new bundle in the catalog and add it to package_manifests
#    (see the install tutorial) — then load it as a SECOND instance:
python3 .../ocudu_dappctl.py --socket $SOCK load \
  --package <new-version-id> --placement in-process --backend cuda
python3 .../ocudu_dappctl.py --socket $SOCK lifecycle prepare      --instance <new>
python3 .../ocudu_dappctl.py --socket $SOCK lifecycle arm          --instance <new>
python3 .../ocudu_dappctl.py --socket $SOCK lifecycle enter-shadow --instance <new>

# 2. Compare: read metrics from both instances over the same window
python3 .../ocudu_dappctl.py --socket $SOCK metrics get --instance <old>
python3 .../ocudu_dappctl.py --socket $SOCK metrics get --instance <new>

# 3. Cut over: activate the new, deactivate the old
python3 .../ocudu_dappctl.py --socket $SOCK lifecycle activate   --instance <new>
python3 .../ocudu_dappctl.py --socket $SOCK lifecycle deactivate --instance <old>
python3 .../ocudu_dappctl.py --socket $SOCK lifecycle unload     --instance <old>
```

If the new version regresses, the reverse is one command each — deactivate the
new, re-activate the old. For a **model** swap within one instance (rather than
a whole package), use the model operations (stage → validate → warm → activate
→ rollback) documented in [e3_management.md](e3_management.md); rollback
retains the previous model, so reverting is atomic.

## 4. Deactivate and unload cleanly

```bash
python3 .../ocudu_dappctl.py --socket $SOCK lifecycle deactivate --instance <id>
python3 .../ocudu_dappctl.py --socket $SOCK lifecycle unload     --instance <id>
```

`unload` proves quiescence first (in-flight invocations drained). If the
module doesn't drain within the deadline it is **quarantined** rather than
force-removed — `retry-quiescence` is the operator escape hatch to attempt the
drain again:

```bash
python3 .../ocudu_dappctl.py --socket $SOCK lifecycle retry-quiescence --instance <id>
```

## 5. Watch and reset circuit breakers

A dApp that faults (`EXCEPTION`/`DEVICE_ERROR`/`INTERNAL_ERROR` — immediate) or
repeatedly misses (8 consecutive recoverable failures, incl. deadline misses)
trips its **circuit breaker**: the host bypasses to the conventional path and
records the reason. Breakers are **per lane** plus one per-instance lifecycle
breaker.

```bash
# Health rollup: reports 'open' if the lifecycle or ANY lane breaker is open,
# plus the worst consecutive-failure count
python3 .../ocudu_dappctl.py --socket $SOCK health

# Incident history — the 'breakerOpened' cause tells you what tripped
python3 .../ocudu_dappctl.py --socket $SOCK incidents list --limit 50
```

> **Important today:** an open breaker does **not** change the instance's
> reported lifecycle state for native dApps — a fully-bypassing Class-A
> instance still shows `ACTIVE` in `inventory`. Use `health` (not `inventory`)
> to see that a breaker is open.

To reset after you've addressed the cause, the `breaker-probe` action
**force-closes every breaker on the instance at once** (there is no per-lane
reset today):

```bash
python3 .../ocudu_dappctl.py --socket $SOCK lifecycle breaker-probe --instance <id>
# then clear the incident history if you want a clean slate
python3 .../ocudu_dappctl.py --socket $SOCK incidents clear --through <last-incident-id>
```

## 6. Jobs, health, and RAN control

```bash
# Mutations return jobIds and complete asynchronously — poll them
python3 .../ocudu_dappctl.py --socket $SOCK jobs list
python3 .../ocudu_dappctl.py --socket $SOCK jobs get --job <job-id>

# Served cells and RAN-control capabilities
python3 .../ocudu_dappctl.py --socket $SOCK cell-info

# Bounded RAN controls (host-validated; support --dry-run)
python3 .../ocudu_dappctl.py --socket $SOCK ue-release --pci <pci> --rnti <rnti> --dry-run
python3 .../ocudu_dappctl.py --socket $SOCK ue-handover --pci <pci> --rnti <rnti> --target-pci <t>
```

## Command cheat-sheet

| Goal | Command |
|------|---------|
| See catalog + instances | `inventory` |
| Load a package | `load --package <id> --placement … --backend …` |
| Walk lifecycle | `lifecycle {prepare,arm,enter-shadow,activate,deactivate,unload} --instance <id>` |
| Configure | `config set --instance <id> --config-generation <g> --set k=kind:v` / `config get` / `config schema` |
| Metrics | `metrics schema` / `metrics get --instance <id>` |
| Breaker status | `health` (not `inventory`) |
| Reset breakers | `lifecycle breaker-probe --instance <id>` |
| Incidents | `incidents list` / `incidents clear --through <last-incident-id>` |
| Async jobs | `jobs list` / `jobs get --job <id>` |
| Validate without committing | add `--dry-run` |

Next: [driving all of this from an LLM agent over MCP](https://gitlab.com/ocudu/work_groups/wg2_ai_ran/ocudu-dapp-sdk/-/blob/main/docs/tutorial_mcp_monitoring.md).
