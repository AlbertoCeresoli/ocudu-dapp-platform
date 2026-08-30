<!-- SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc. -->
<!-- SPDX-License-Identifier: BSD-3-Clause-Clear -->

# dApp API and platform compatibility

Working version **v1**. The public module boundary is the versioned C dApp
ABI v1 (`include/ocudu/dapp/abi/v1`, `use_cases/v1`, `process/v1`); the
management, telemetry, E3AP, manifest, signature, and FlatBuffers schemas
carry their own v1 versions. Existing layouts and meanings are never
reinterpreted.

## ABI v1 stability rules

Every public struct opens with `struct_size`, `abi_major`, `abi_minor`
(`abi/v1/base.h`). Compatible evolution uses, in order of preference:

1. **Reserved slots** with defined zero semantics: fields end in `reserved`
   words that hosts and modules must leave zero when unused, so a
   zero-initialized struct from an older peer reads as "unspecified".
2. **`struct_size` growth**: new tail fields are read only after checking
   `struct_size` covers them (e.g. the scheduler candidate's
   `OCUDU_DAPP_SCHEDULER_CANDIDATE_BLER_COUNT_SIZE_V1` gate before
   `bler_nof_observations`; validators compare against
   `CANDIDATE_BASE_SIZE_V1`, never `offsetof(reserved)`).
3. **Known-bits flag masks**: outputs may set only defined bits
   (e.g. `OCUDU_DAPP_RECEIVER_KNOWN_OUTPUT_FLAGS_V1`), so new bits can be
   added without old hosts misreading them.
4. **Parallel versioned entry points** when none of the above fits.

Worked reserved-slot examples (all still ABI v1, `use_cases/v1/pusch_metadata.h`):

| Extension | Mechanism |
|-----------|-----------|
| DM-RS symbol mask | `dmrs.reserved[0]` carries an optional OFDM-symbol bitmask (bit *s* set ⇒ symbol *s* has DM-RS); zero means "not provided", packages may ignore it and use coordinates |
| PUSCH scalars | `pusch.reserved[0..2]`: optional C-RNTI, subcarrier spacing in Hz, full carrier grid width |
| DC subcarrier | `pusch.reserved[3]` is the transmitter DC grid index **encoded + 1**, so a zero-filled slot from an older host reads as "unknown"; `OCUDU_DAPP_PUSCH_DC_OVERLAP_V1` flags overlap with the grant |
| Resident scheduler noise | Channel-estimator `output.reserved[0..3]` implement a request/acknowledge handshake (`OCUDU_DAPP_CHANNEL_ESTIMATOR_SCHED_NOISE_*_V1`); older peers leave the words zero and the base contract is unchanged |

**Breaking changes** — anything that reinterprets an existing field, changes a
layout id's element order, narrows an admitted profile, tightens validation on
previously accepted output, or removes a defined enumerator — require a new
interface or ABI major, a formal release, and a dual-run migration story. Do
not silently widen admitted profiles (e.g. layers > 2, transform precoding)
without a new profile contract.

## Telemetry packed-header versioning

Store records (`include/ocudu/dapp/e3/telemetry_types.h`) open with a 16-byte
packed header: magic `"OTE3"`, `version`, `kind`, `monotonic_ns`. Version 2
applies **only** to the PUSCH-symbols kind — the v1 record followed by
`symbol_flags` and the shared 92-byte grant block — and every other kind stays
version 1. Decoders accept versions 1 and 2 and must key parsing on
(`kind`, `version`); sizes are locked by `static_assert`:

| Record | v1 | v2 |
|--------|----|----|
| `pusch_symbols` | 56 B | 152 B |
| `pusch_crc` / `srs_channel` / `pusch_dmrs_channel` | 26 / 66 / 70 B | — |
| `pusch_grid` (includes grant block) | 120 B | — |
| `gnb_metrics_json` | 16 B (header only) | — |

Adding fields to a kind means a new header version for that kind with the old
layout as a strict prefix; per-UE records keep the RNTI immediately after the
header so the agent can filter without a codec.

## ASN.1 extension policy (E3 schemas)

Every SEQUENCE and ENUMERATED in `schemas/e3/*.asn1` carries an extension
marker (`...`). Compatible additions go **after** the marker so v1 APER
decoders skip them (e.g. `DataSubscriptionRecord.rfCenterFrequencyHz` /
`cellFrequencyOffsetHz`, `PuschSymbolsRecord.grant`/`crcOk`). Constraints on
existing fields are never tightened or loosened; text stays bounded UTF-8
`OCTET STRING` (never `UTF8String`) and 64-bit integers stay 8-byte
network-order `OCTET STRING`, because PER encoders disagree on the
alternatives. One deliberate exception: `Telemetry-Object.puschGrid` was added
as a **root** alternative, not an extension addition, because PER wraps
extension additions in an open type whose length is fragmented above 16 KiB —
unimplemented by several decoders — and grid captures are routinely larger.
Clients built on the previous cut must recompile the modules to read it.

## Toolkit and platform qualification

The companion `OCUDUDapp` toolkit versions independently (SemVer); its
installed C++ helpers are a source authoring surface, not a cross-toolchain
binary ABI — rebuild packages for the target compiler, standard library, CUDA
toolkit, driver, and GPU architecture. Release qualification is tiered:
**NVIDIA GB10 / aarch64** requires full CPU/CUDA tests, the ABI matrix,
sanitizers, performance, and live OTA qualification; **GH200 / aarch64** and
**RTX 4090 / x86-64** require native build, correctness, sanitizers, and
performance, with OTA when hardware is available. "Build supported" and "OTA
qualified" are separate claims, and the ABI compiler fingerprint covers LP64 C
layout across GCC and Clang only — not 32-bit, C++ standard-library,
CUDA-driver, or performance compatibility. See
[release_checklist.md](release_checklist.md) and
[known_limitations.md](known_limitations.md).
