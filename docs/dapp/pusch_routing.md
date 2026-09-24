<!-- SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc. -->
<!-- SPDX-License-Identifier: BSD-3-Clause-Clear -->

# Per-UE routing of Class-A PUSCH dApps

Several channel-estimator, equalizer and receiver dApps can be active at once,
and each UE's PUSCH grants are served by the one its entry in a host route
table names — or by the stock receiver. The table is set over E3 at control
rates (every second or 100 ms is typical); the choice for every grant is a
lock-free lookup in the PUSCH worker. The dApps themselves are unchanged: a
receiver does not know it is routed, and every package that runs today runs the
same way behind a route.

Typical uses:

- **A/B of two receivers on live UEs**, e.g. a new neural receiver for half the
  UEs and the current one for the rest, both active at once.
- **Link-dependent selection**: a controller watches each UE's SINR over E3
  telemetry and sends low-SINR UEs to a neural receiver and the rest to the
  stock receiver, or to a second neural receiver. The SDK ships this controller
  as an example (`examples/e3_receiver_routing`).
- **Per-UE opt-out**: keep a receiver active for the cell but send one UE to
  the stock receiver.

## Model

A **route** is one of:

| Route | Meaning |
|-------|---------|
| `primary` | The behaviour of a host without routing: the first instance of each hook (slot 0), with the usual rules — an active receiver or equalizer replaces estimation, an equalizer and a receiver both on slot 0 fall back to the stock receiver |
| `conventional` | The stock PUSCH receiver for this UE, whatever dApps are active |
| an instance ID | Exactly that Class-A instance, on the hook it occupies (channel estimator, equalizer or receiver); no other dApp runs for the grant |

The **table** holds a default route and up to 512 **entries**. An entry is a
C-RNTI, optionally scoped to one DU cell (the `cellId` of `cell-info`), and a
route. A grant looks up its exact (cell, RNTI) entry, then its any-cell RNTI
entry, then the default. An empty table routes every UE `primary`: a host with
routing enabled and no table behaves exactly like a host without routing.

Entries name **instances**, not hook slots. The host resolves an instance to
the slot it occupies whenever it activates or deactivates, so:

- an entry keeps working if its instance is deactivated and reactivated, even
  on another slot;
- while its instance is not active, the entry sends the UE to the **stock
  receiver** and counts the grant in `unavailableLookups` — it never reaches
  another instance that later takes the same slot;
- a new entry must name an instance that is active on a routable hook
  (`notFound` otherwise), which catches mistyped IDs.

## Configuration

```yaml
dapp:
  pusch_route_slots: 2   # instances each of CE / EQ / RX can hold at once (1..8, default 1)
```

`pusch_route_slots` is the number of hook points the host creates for each
Class-A PUSCH hook. With the default `1` there is one instance per hook, as
before, and the table can still route UEs between an active channel
estimator, equalizer, receiver and the stock receiver. With `2`, two receivers
(or two equalizers, or two channel estimators) can be active at the same time;
activation takes the lowest free slot and is refused with the usual
`already owned by another instance` conflict once every slot is taken.
Spectrum, SRS and scheduler hooks are not routed and keep one point.

Each slot is a complete hook point with its own per-lane workers, so memory and
worker threads grow with the number of instances you actually activate, not
with the slot count. Every slot of a hook has the same backend policy as slot 0:
on a CUDA L1 every Class-A slot accepts only CUDA packages, on a CPU L1 only CPU
packages. Routing therefore never mixes CPU and CUDA receivers within a cell.

## Setting the table over E3

The `puschRouting` management operation (`E3SM-OCUDU-DAPP-MGMT-V1`) takes an
action and answers with the resulting table, synchronously, over the local
socket or E3AP SCTP:

| Action | Takes | Effect |
|--------|-------|--------|
| `query` | nothing | Returns the table |
| `replace` | optional default (absent: `primary`), entries | Sets the default and every entry in one step |
| `update` | entries | Inserts or overwrites the given UEs; an entry routed `primary` removes that UE |
| `clear` | nothing | Removes every entry and resets the default to `primary` |

`dryRun` validates a `replace` or `update` without applying it. A request is
validated as a whole before anything changes; a rejected request leaves the
table as it was. The response lists the default, the entries (sorted by cell,
then RNTI, any-cell entries last), which instance occupies which hook slot, the
table generation (incremented by every change, including (de)activations) and
`unavailableLookups`.

### From the command line

```bash
# Python client, over E3AP SCTP or the local socket
ocudu_dappctl.py --sctp 127.0.0.1:36423 pusch-route show
ocudu_dappctl.py --sctp 127.0.0.1:36423 pusch-route set \
    --entry 0x4601=<receiver-A-instance-id> --entry 0x4602=conventional
ocudu_dappctl.py --sctp 127.0.0.1:36423 pusch-route replace --default conventional \
    --entry 0x4601=<receiver-A-instance-id> --entry 0x4603@1=<receiver-B-instance-id>
ocudu_dappctl.py --sctp 127.0.0.1:36423 pusch-route set --entry 0x4601=primary   # remove one UE
ocudu_dappctl.py --sctp 127.0.0.1:36423 pusch-route clear

# C++ client, local socket: same subcommand and flags
ocudu-dappctl --socket /run/ocudu/dapp-management.sock pusch-route show
```

`--entry` is `RNTI[@CELL]=TARGET`; RNTI and cell take decimal or `0x`
hexadecimal; a target is `primary`, `conventional` or an instance ID. Instance
IDs come from `inventory`.

### From Python

```python
from ocudu_e3 import E3Client, management

with E3Client("127.0.0.1", 36423, dapp_name="router") as client:
    client.setup()
    table = client.pusch_routing("replace", [
        management.pusch_route_entry(0x4601, receiver_a),        # instance ID (UUID text or bytes)
        management.pusch_route_entry(0x4602, "conventional"),
        management.pusch_route_entry(0x4603, receiver_b, cell_index=1),
    ], default="primary")
    print(table["payload"]["puschRouting"])
```

`E3Client.pusch_routing(action, entries, default, dry_run=...)` wraps
`management.pusch_routing_request`. A controller that re-sends a small
`update` every 100 ms to 1 s is well within the design point; each change
republishes one bounded snapshot and never blocks a grant.

## How a grant is dispatched

For every PUSCH grant the PHY resolves the route once, from the grant's DU cell
and C-RNTI, before it decides which dApp mode serves it:

- **CUDA resident path** (`pusch_demodulator_gpu_impl`): the grant's route is
  selected on the lane's resident adapter right after the grant starts;
  the mode (CE / EQ / RX / stock), the invocation, its fallback accounting and
  its asynchronous deadline report all use the instance on that route. A
  completion observed after later grants were routed elsewhere is still
  attributed to the instance that served it.
- **CPU path**: the host demodulator and channel-estimator decorators resolve
  the route at the start of the grant and use the routed view for the whole
  grant. The channel-estimator configuration carries the RNTI for this purpose.

Both paths also fill the optional C-RNTI field of the dApp's PUSCH metadata
(`OCUDU_DAPP_PUSCH_RESERVED_RNTI_V1`), so CPU and CUDA dApps see the same
attribution.

The lookup reads one of two fixed-size compiled snapshots under a per-snapshot
reader count: no lock, no allocation, two binary searches over at most 512
entries. A writer rebuilds only the snapshot no reader holds and publishes it
with one atomic store, so a grant always sees one complete table. Every failure
of a routed instance — bypass, invalid output, deadline, breaker — falls back to
the stock receiver for that grant exactly as it does without routing, and is
counted in that instance's own metrics and incidents.

## Observability

- `pusch-route show` / `puschRouting query`: the table, the slot bindings and
  `unavailableLookups`.
- Each instance's `metrics get` counts the grants it served
  (`dapp.invoke.count`) and its fallbacks (`class_a.fallback.*`); with routing
  these are exactly the grants of the UEs routed to it.
- E3 telemetry (`pusch.symbols`, `pusch.crc`, `gnb.metrics_json`) is per RNTI,
  so a controller can measure each UE on the receiver serving it.

## Limits

- 512 entries; a full table encodes below one 16 KiB APER open-type fragment,
  which every client decoder (including `asn1tools`) handles.
- 1 to 8 slots per hook (`pusch_route_slots`).
- An instance implementing several routable hooks is routed by its deepest one
  (receiver, then equalizer, then channel estimator).
- Routing selects among instances; it does not load or activate them. Load and
  activate each instance with the usual lifecycle, then route UEs to it.

Implementation: `include/ocudu/dapp/runtime/pusch_route_table.h` (table),
`native_hook_registry` (slot groups and binding notifications),
`native_pusch_dapp_demodulator_target.cpp` and `pusch_dapp_channel_estimator.cpp`
(routed targets), `pusch_resident_dapp.cpp` (CUDA route selection). Tests:
`dapp_pusch_route_table.*`, `dapp_native_hook_registry.route_table_slots_*`,
`dapp_demodulator.each_grant_is_served_*`, `dapp_e3_aper_codec.pusch_routing_*`,
`dapp_e3_management_service.pusch_routing_*`,
`pusch_resident_dapp_adapter.the_selected_route_*` and the Python
`test_pusch_routing_round_trips_and_validates_over_sctp`.
