<!-- SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc. -->
<!-- SPDX-License-Identifier: BSD-3-Clause-Clear -->

# Proposing a new interface, stream, or extension

The point of this platform is that **interface consensus can form in working
code, in the open, before standards bodies formalize it**. That only works
if there is a clear, fair path for anyone — vendor, operator, researcher —
to propose a new use-case contract, telemetry stream, or service-model
extension. This page is that path: the registries, the evolution rules the
code already enforces, and what a good proposal contains.

## 1. What can be extended

| Extension | Lives in | Example precedent |
|---|---|---|
| A new **use-case contract** (Class A/B/C interface) | `include/ocudu/dapp/use_cases/v1/` + a reference module in the SDK | scheduler `0x00010006` |
| A new **telemetry stream** | telemetry service model (ASN.1) + publisher + stream registry | `pusch.grid` (id 8) |
| A new **management operation** | `e3sm-ocudu-dapp-mgmt-v1.asn1` (extension arm) + host service | `ranControl`, `cellInfo` |
| A new **context/output schema** for an existing contract | schema-id'd struct under `use_cases/v1/` | quiet-reservation context |
| Conventions (config keys, metric names) | docs + reference usage | vendor-prefixed metrics |

## 2. The identifier registries

Identifiers are 32-bit and live in **separate namespaces disambiguated by
the field they travel in** — an interface id and a context schema id may
share a numeric value without colliding, but *within* each namespace,
allocation must be coordinated here:

| Namespace | Range in use | Registry (source of truth) |
|---|---|---|
| Use-case **interface ids** | `0x00010001`–`0x00010006` | `include/ocudu/dapp/use_cases/v1/*.h` |
| Telemetry-stream **interface ids** | `0x00020001`–`0x00020006` | `include/ocudu/dapp/e3/telemetry_types.h` |
| **Telemetry ids** (small ints, wire) | 1–8 | `e3ap_protocol.h` / `default_telemetry_streams()` |
| **Context / layout / output schema ids** | per-contract, `0x0001xxxx` | the owning contract's header |

Process rules for allocation (project convention):

- **Next-free within the block**, allocated in the proposal MR itself — the
  merged header *is* the registry entry; there is no separate list to drift.
- **Never reuse or renumber** a shipped id, even for an abandoned
  experiment.
- For pre-consensus experimentation, use ids at or above `0x7F000000` in the
  relevant namespace and mark them experimental in the header comment; they
  carry no compatibility promise and must be re-allocated to the real block
  on graduation.
- Names follow the established shapes: reverse-DNS `global_name` for
  packages, `family.metric_name` with a vendor prefix for metrics and config
  keys (`acme.rx.confidence`), `family.stream` for telemetry streams.

## 3. The evolution rules your proposal must obey

These are enforced by the codebase and CI, not aspirations — see
[api_compatibility.md](api_compatibility.md) for the full text:

- **ABI v1 structs are frozen in layout and meaning.** Every public struct
  opens `struct_size / abi_major / abi_minor`; evolution happens through
  trailing `reserved` slots **with defined zero semantics** (a zero-filled
  slot from an older peer must mean "not provided", never a valid value —
  see the DC-subcarrier `+1` encoding for the worked pattern). Consumed
  reserved slots get named size constants, never `offsetof` games.
- **Never repurpose.** A change that alters an existing field's meaning is a
  new interface id or a new ABI major — which requires a formal release and
  a dual-run migration story. Prefer a reserved slot or a new id.
- **ASN.1 schemas evolve through their extension markers** (every SEQUENCE
  and ENUMERATED carries one). Existing constraints are never tightened or
  loosened; text stays bounded `OCTET STRING`; payloads that can exceed
  ~16 KiB go in as **root** CHOICE alternatives, not extension additions
  (PER fragments large open types — `pusch.grid` is the precedent).
- **Real-time discipline is part of the contract.** A new stream's producer
  must never block (bounded queues, counted drops, floors checked before
  capture work); a new contract's host boundary needs an explicit time
  budget and a conventional fallback. "What happens when the dApp is late,
  wrong, or absent" is a design input, not an afterthought.
- **Host authority is the security model.** New capabilities that *change*
  RAN behavior arrive operator-gated (allow-flags, budgets, dry-run) and
  observable (metrics, incidents). E3 selects from host-validated options;
  it never supplies code, paths, or unchecked parameters.

## 4. What a proposal contains

Working code beats position papers — that's the thesis. A complete proposal
is one merge request (plus an issue for discussion) containing:

1. **The problem**: the AI-RAN use case, why existing contracts can't carry
   it, and who else plausibly needs it (one vendor's private need is a
   vendor header, not a platform interface).
2. **The contract**: draft `use_cases/v1/` header and/or ASN.1 delta,
   following §2–§3, with the failure/fallback semantics written down.
3. **A reference implementation**: a working SDK module or example client —
   CPU-runnable so every reviewer can execute it (CUDA variants welcome
   alongside, per the backend-neutral rule).
4. **Certification vectors** for a new Class-A/B contract (extend the
   offline certifier), or a loopback-agent producer for a new stream so it's
   exercisable radio-free.
5. **Observability**: the metrics/incidents the new surface emits, and the
   telemetry needed to judge it in production.
6. **Docs**: a capabilities.md entry, wire notes if the schema changed, and
   a tutorial section if the workflow is new.
7. **Evidence**: for real-time paths, measured boundary timings on a
   reference platform; for streams, rate/volume characteristics and the
   proposed default floors.

## 5. The path to consensus (and to the SDOs)

1. **Issue** — sketch the use case and claim identifiers provisionally.
2. **Draft MR** — the seven items above; CI gates (ABI fingerprint matrix,
   codec round-trips, release-docs, sanitizers) must pass like any change.
3. **Experimental landing** — merged under an experimental id when at least
   the reference implementation and vectors are in; marked clearly; may
   change without notice.
4. **Graduation to v1** — after a second independent implementation or
   deployment report validates the contract, the id moves into the stable
   block and the freeze rules of §3 apply forever after.
5. **Upstream** — graduated interfaces, with their running references and
   deployment evidence, are the material the project carries into O-RAN /
   3GPP discussions. The reference implementation is the contribution.

Maintainer review balances one bias explicitly: **additions are cheap,
promises are forever**. Expect pushback on anything that constrains future
evolution, and none on experiments cleanly marked as such.

## 6. Worked precedents to copy

- *A full contract*: the scheduler (`0x00010006`) — capability-gated
  authority, evaluation mode, timing decomposition metrics, a five-stage
  reference with everything shipped off.
- *A heavyweight stream*: `pusch.grid` — root-alternative encoding, a
  default hard floor justified in-source, grant parameters for offline use.
- *A reserved-slot extension*: the DM-RS symbol mask and DC-subcarrier
  fields — new information for new peers, invisible to old ones.
