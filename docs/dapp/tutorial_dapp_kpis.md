<!-- SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc. -->
<!-- SPDX-License-Identifier: BSD-3-Clause-Clear -->

# Tutorial: exposing dApp KPIs and querying them over E3

A dApp can publish its own **metrics** — invocation counts, model confidence,
detection rates, custom quality figures — and every one of them becomes
queryable over E3: from `dappctl`, from the Python `ocudu_e3` client, from a
dashboard, or from an LLM agent via the MCP server. This tutorial shows both
halves: how a vendor **declares and reports** metrics in a dApp (authoring),
and how an operator **reads** them over each E3 client (operations).

This is the metric surface distinct from the built-in **telemetry streams**
(`pusch.symbols`, `srs.channel`, …); those are PHY-produced data feeds
([e3_wire.md](e3_wire.md)). The metrics here are whatever *your dApp* chooses
to expose about *itself*.

## How a dApp exposes metrics (author side)

Metrics are part of the module's `management` vtable — two callbacks:

- **`describe_metrics`** returns the catalog: for each metric a `name`,
  `description`, `unit`, `value_kind`, and a `flags` word marking it a
  **counter** (monotonic) or a **gauge** (point-in-time).
- **`read_metrics`** returns the current sample values (with a monotonic
  timestamp), read on demand.

Values are typed (`ocudu_dapp_value_v1`): bool / int64 / uint64 / double /
string / bytes. A minimal pair, from the reference (`reference/common/
module_support.cpp`), exposes an invocation counter and a layer-count gauge:

```c
// describe_metrics: declare the catalog
descriptors[0].name        = "dapp.invoke.count";
descriptors[0].description = "Accepted data-path invocations";
descriptors[0].unit        = "invocations";
descriptors[0].value_kind  = OCUDU_DAPP_VALUE_UINT64_V1;
descriptors[0].flags       = OCUDU_DAPP_METRIC_COUNTER_V1;   // vs _GAUGE_V1

// read_metrics: report the values now
samples[0].name  = "dapp.invoke.count";
samples[0].value = counter_value(state.invocation_count.load());
samples[0].monotonic_timestamp_ns = host.monotonic_ns(host.host_context);
```

Practical guidance:

- **Use the SDK helpers.** `stateful_module_context` + the reference
  `describe_metrics`/`read_metrics` give you the invocation counter for free;
  add your own by extending both functions in your `module_support.cpp`. Keep
  the two in sync (a name in `read_metrics` must appear in `describe_metrics`).
- **Read must be cheap and non-blocking.** `read_metrics` is called from the
  management plane, off the real-time path — but keep it to atomic loads of
  values you already maintain (as the reference does). Don't compute on read.
- **Name with a vendor prefix.** Metric names are a flat string namespace
  today; prefix yours (e.g. `acme.rx.confidence`) so two vendors' metrics on
  the same cell never collide.
- **Counter vs gauge matters to dashboards.** Mark monotonic totals as
  counters and instantaneous readings as gauges; downstream tooling (rate
  computation, alerting) relies on the flag.

Once your package declares metrics, nothing else is needed — the host wires
`describe_metrics`/`read_metrics` into the E3 management surface automatically.

## Reading metrics over E3 (operator side)

The same metrics reach you through every E3 client. All of these work over the
local socket or over E3AP/SCTP.

### With `dappctl`

```bash
SOCK=/run/ocudu/dapp-management.sock
# the metric catalog (names, units, counter/gauge) …
python3 apps/tools/dappctl/ocudu_dappctl.py --socket $SOCK metrics schema  --instance <id>
# … and the current values
python3 apps/tools/dappctl/ocudu_dappctl.py --socket $SOCK metrics get --instance <id>
```

### With the Python `ocudu_e3` client

> **Running `ocudu_e3`:** it is not pip-installed yet, so put the client on the
> path first: `export PYTHONPATH=/path/to/ocudu/python` and
> `export OCUDU_E3_SCHEMA_DIR=/path/to/ocudu/schemas/e3` (see `ocudu/python/README.md`).

```bash
# schema (descriptors) for one instance …
python3 -m ocudu_e3 --port 36423 metrics <instance-hex> --schema
# … or the samples
python3 -m ocudu_e3 --port 36423 metrics <instance-hex>
```

`E3Client.metrics()` also returns them programmatically, which is the hook for
a **dashboard**: poll `metrics()` per instance on your own interval and render.
Because reads are cheap and off the RT path, a dashboard scraping every few
seconds costs the radio nothing. (There is no built-in Prometheus/OTLP export
yet; a thin exporter over `ocudu_e3.metrics()` is the usual bridge — see
[known_limitations.md](known_limitations.md).)

### With an LLM agent over MCP

The [MCP server](https://gitlab.com/ocudu/work_groups/wg2_ai_ran/ocudu-dapp-sdk/-/tree/main/examples/e3_mcp_server) exposes the
metric surface as the `get_dapp_metrics` tool, alongside `get_dapp_health`
(breaker/consecutive-failure rollup) and `get_dapp_inventory`. So an agent can
be asked, in plain language:

> *"Read `get_dapp_metrics` for the receiver instance. Is `acme.rx.confidence`
> trending down over the last few samples? If so, check `get_dapp_health` for
> an open breaker and summarize."*

This is exactly the input a **KPI-tuning loop** runs on — see the
[MCP monitoring tutorial](https://gitlab.com/ocudu/work_groups/wg2_ai_ran/ocudu-dapp-sdk/-/blob/main/docs/tutorial_mcp_monitoring.md),
where dApp metrics drive the observe → reason → dry-run → confirm cycle.

## The two metric paths (and which one to use)

There are two ways values move, and it's worth knowing which is live:

| Path | Direction | Status |
|------|-----------|--------|
| **Pull** — `describe_metrics` / `read_metrics` on the `management` vtable | host reads the dApp on demand, string-named | **This is the path.** Wired to E3, queryable everywhere above. |
| **Push** — `host_api.metric_add(metric_id, value)` | dApp pushes to a host sink, uint32-id'd | Present in the ABI but the production host does not populate the sink today; there is no id→name mapping. Prefer pull. |

Author your KPIs through `describe_metrics`/`read_metrics`. It's the path that
actually surfaces over E3, and the one dashboards and the MCP server read.

## Worked example: a detection-rate KPI

A Class-C emitter-detector dApp wants to expose how often it's firing. In
`describe_metrics` it adds a gauge:

```c
descriptors[n].name        = "acme.spectrum.detections_per_min";
descriptors[n].description = "Emitter detections in the trailing minute";
descriptors[n].unit        = "detections/min";
descriptors[n].value_kind  = OCUDU_DAPP_VALUE_DOUBLE_V1;
descriptors[n].flags       = OCUDU_DAPP_METRIC_GAUGE_V1;
```

and in `read_metrics` reports the current windowed rate it already maintains.
The operator then watches it live:

```bash
python3 -m ocudu_e3 --port 36423 metrics <instance-hex>        # one look
watch -n5 'python3 -m ocudu_e3 --port 36423 metrics <hex>'      # a poor-man's dashboard
```

or an agent loop flags when the rate spikes and correlates it with the
scheduler's quiet-reservation activity — all from the same E3 surface.

## See also

- [e3_management.md](e3_management.md) — the full management operation catalog,
  including the metrics operations.
- [tutorial_dappctl_operations.md](tutorial_dappctl_operations.md) — where
  `metrics schema` / `metrics get` sits in the operator loop.
- [../../../ocudu-dapp-sdk/docs/tutorial_mcp_monitoring.md](https://gitlab.com/ocudu/work_groups/wg2_ai_ran/ocudu-dapp-sdk/-/blob/main/docs/tutorial_mcp_monitoring.md)
  — driving KPI-based tuning loops from dApp metrics.
- [../../../ocudu-dapp-sdk/docs/package_config.md](https://gitlab.com/ocudu/work_groups/wg2_ai_ran/ocudu-dapp-sdk/-/blob/main/docs/package_config.md)
  — the `make_config_management` helper that also carries your metric callbacks.
