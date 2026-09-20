# Resident receiver result acceptance

This is an internal C++ host boundary for asynchronous receiver results. It
does not change the dApp v1 C ABI, add a private FAPI wire value, register a
neural package or qualify its radio performance. The native module/receipt
adapter and resource-pool owner still need to supply the check described here.
The currently running cell has not been rebuilt or changed by this work.

Follow-on development: a vendor-side design note describes a native host
adapter connecting the real masked worker's receipt to this check. It retains
receipts through downstream use and tests the actual decoder path. Production
resource-pool/LCM integration is still required; this does not make the running
package use that adapter. The follow-on [native C completion extension](receiver_completion.md)
provides loader/worker/hook support, but does not yet connect the production
PUSCH consumer and pool owner or advertise support on the running host.
The generic host `native_receiver_resident_adapter` now binds native receipts
to the actual decoder check with retained caller-provided tensor owners; see
the completion guide for its connected CPU qualification and remaining
production pool/continuation work.

The [PUSCH processing-lifetime loan](pusch_processing_lifetime.md) now retains
the real wrapper through processing and notifier diagnostic tails. It is a
host ownership prerequisite, not a CUDA fence or a complete resource provider.

## Why a stream fence is insufficient

### Failure before decoder submission

The accelerated demodulator now preserves explicit overwrite prohibition from
an inline invocation, separately from whether it produced usable LLRs. A
selected module's failed continuation also prohibits conventional overwrite.
After successfully draining the producer and transfer streams, it marks the
input discarded and publishes neither a resident descriptor nor host softbits,
an end-codeword marker, candidate CSI or symbols. The processor handles this
before symbol collection and decoder submission.

`pusch_decoder::discard_unsubmitted_input()` is exclusive producer-side
cancellation of a configured collecting input with zero submitted softbits
and zero available codeblocks. It clears the TB, unlocks the HARQ buffer,
returns to idle and notifies `input_valid=false` once. It does not invoke a
decoder or post-decode callback. Because no decoder consumed candidate input,
it preserves HARQ/CRC state as it stood after `new_data()` setup, including
valid previous-transmission combining data. Idle/partially submitted input is
rejected without mutation. The processor settles supported pending UCI as
unknown and clears diagnostics before that terminal notification.

This is not asynchronous GPU cancellation. The caller must establish safe
producer completion separately. If stream drain fails, production still stops
fatally before releasing uncertain storage; a real bounded quarantine and
resource-retirement owner remains required. Native completion tickets also
require their own original-worker retirement, not just stream synchronization.
Completion-required packages therefore remain disabled. Full host rebuild,
actual CUDA error/latency qualification and deployment are still outstanding.
The tests and the interrupted/recovery qualification record are kept with the
vendor receiver package.

### Acceptance after submitted decoding

A completed accelerator stream establishes memory ordering. It does not prove
that the receiver's metadata, feature construction, inference and output-gather
checks passed. Previously, `pusch_decoder_impl::try_decode_resident` committed
the backend CRC and exported failed-block softbits into host HARQ storage before
its deferred completion callback. Some resident UCI could also be notified
before decode. Suppressing a late constellation did not protect those effects.

The new ordering is:

1. The resident decoder backend finishes and fences its work.
2. The host validates the exact input's required completion receipt.
3. The bounded completion callback receives the acceptance decision. Only an
   accepted input may publish deferred CSI, UCI and equalized symbols.
4. Accepted inputs follow the existing CRC/HARQ path. Rejected inputs clear
   their TB bytes, full codeblock softbuffer footprints and CB CRCs, without
   importing the rejected backend's HARQ softbits.
5. The result is notified with explicit `input_valid` status.

The backend may already have written provisional TB bytes before step 2. Those
are caller-owned staging bytes, not deliverable payload before notification;
rejection erases them. This mechanism does not prevent decoder compute on an
invalid input and does not make a malformed device pointer safe to submit.
Descriptor admission and authentic buffer ownership remain prerequisites.

## Host-owned completion contract

`resident_softbit_buffer` has four optional fields:

| Field | Meaning |
| --- | --- |
| `acceptance_required` | The producer requires per-invocation asynchronous acceptance. |
| `completion_context` | Host-owned state retaining the original receipt and issuing worker. |
| `accept_completion` | Bounded, nonblocking, `noexcept` host function checking that receipt and exact codeword. |
| `completion_generation` | Host publication generation, checked by the adapter to reject stale descriptors after fixed-slot reuse. |

Conventional producers leave all four empty. Required acceptance with a
missing context or callback rejects. A callback/context supplied while the
requirement flag is clear also rejects, rather than silently ignoring it.
An unexpected generation on a conventional descriptor also rejects. Required
producers define their generation checks in the host validator; this preserves
the existing callback contract while allowing exact fixed-slot publications.
The validator must check the original invocation, generations, worker and
stream, not whichever dApp happens to be active when decoding completes.

These pointers are installed by a trusted native host adapter, never read from
E3, untrusted package data or a module-returned arbitrary function address.
The owner must retain the issuing worker/module, callback state and global
tensor leases through `try_decode_resident()` returning and all other outstanding
consumers. A worker-local receipt acknowledgment is not a global tensor release.
Early decoder rejection, cancellation and unbinding need explicit retirement;
this header does not implement that resource/lifetime owner.

The completion notification uses `unique_function<void(bool), 128, true>`.
Its captures cannot spill to a heap allocation; an oversized capture fails to
compile. The current PUSCH callback fits. The decoder moves the callback out
before invoking it and clears its per-grant state in `new_data`. This is a
storage bound, not a measured full-PUSCH latency improvement.

## Rejection and reporting semantics

`pusch_decoder_result::input_valid=false` distinguishes software-invalid input
from a measured radio CRC failure. TB CRC is forced false locally to prevent
payload delivery, and the PHY notifier drops candidate CSI and CRC/symbol
diagnostics. Resident HARQ-ACK/CSI-Part1 notifications are settled as unknown,
using their configured lengths and no candidate payload bits. Resident CSI
Part2 is not supported by this rejection path or the current resident plan.

The PHY-to-FAPI translator withholds **both** CRC and RX-data indications for
the software-invalid input. It does not encode a private CRC status in FAPI.
Existing missing-feedback/HARQ timeout behavior therefore applies; this can
delay recovery and must be exercised before deployment. Current scheduler
source routes that timeout to its timeout/aggregate accounting, not the
received-CRC OLLA update. It can still affect timeout/loss counters: absence of
a radio CRC observation is not absence of a failed transmission.

The PUSCH performance metric carries `input_valid` too. Its aggregator retains
all processing/CPU time, but excludes software-invalid inputs from radio BLER
and last-valid SINR/EVM updates. `get_nof_software_discards()` counts them
separately. Exporting this new counter into every management schema is not
implemented here. Existing valid-input counters remain on the normal path;
there is no added atomic increment for each accepted radio observation.

## Qualification and remaining work

`pusch_resident_acceptance_test` compiles the **actual** `ENABLE_CUDA` decoder
control flow, real CPU segmentation/CRC implementations and FAPI translator
against a CPU fake resident backend. It must not link CUDA libraries. Coverage:

- QPSK, 16/64/256-QAM metadata; single- and multi-codeblock grants.
- All accepted/rejected and backend-CRC-pass/fail combinations.
- Validation after backend fencing and before softbit export/notification.
- Rejected TB/CB CRC suppression and complete HARQ-footprint erasure, preserving
  the unused capacity tail.
- Required/missing/ambiguous completion fields and fixed callback storage.
- Unknown UCI, discarded CSI, FAPI suppression and separate software-failure
  metrics with retained processing time.

The CMake target is CPU-only even though it exercises the resident branch.
An independent runner, kept with the vendor receiver package, drives it.
It pins compiler-discovered local dependencies and linked inputs, checks the
real PUSCH processor's callback at compile time, runs ASan/UBSan tests, verifies
that the test executable has no CUDA dependency, and requires an independent
cell watch. Existing static libraries are not rebuilt with sanitizers; coverage
must not be described as a fully instrumented gNB or a CUDA numerical test.

Before live activation: wire the genuine masked receipt/resource adapter;
exercise worker replacement and cancellation; qualify legacy and selected
scheduler metrics, CUDA math and full-PUSCH latency; build the complete host
consistently with these C++ types; then perform guarded OTA comparisons.
Do not mix old/new host objects or infer OTA safety from CPU fakes. The previous
conditional 64-QAM simulated sensitivity gain remains separate evidence.

### Completed CPU qualification: 157603

All **37 tests** passed with ASan/UBSan, including 32 QAM/codeblock/acceptance/CRC
combinations and five contract, notifier, FAPI and metric tests. The real PUSCH
processor also passed an `ENABLE_CUDA` syntax check, so its actual callback
captures satisfy the compile-time storage limit. This is direct compiler/test
evidence; the new CMake target has not yet been exercised through CTest.

Independent verification checks 220 compiler-discovered local source/input
pins and the executable, test JSON, protocol and completed cell watch. Artifact
prefix: parent-workspace `artifacts/joint_rx_smoke/resident_acceptance_157603`.
Report SHA-256:
`d8e41982b31b8a349a5afadb2dcc7bc069d449e143d7f1bc1cb23aec78afc359`.

The 270.235-s watch passed with gNB PID 1421677/start ticks 23627096 and the
same four RNTIs 4601/4602/4607/4817. The child exited at 47.763 s, followed by
about 222 s of observation. Two late messages and seven UL discards were
recorded: one discard in the prelaunch gap, zero late/discard messages during
the child, and two late/six discarded messages after exit. No burst threshold,
RLF or identity-change failure was observed. Do not attribute the post-exit
messages to inference; no new inference was running on the cell.

Earlier attempts 157601/157602 are retained as failed build qualifications.
The first exposed an incorrect `unique_function` emptiness check plus warning
flags unlike the repository's build; the second exposed standalone factory
link omissions. The final harness uses the real generic CPU implementations
directly and the repository's warning family. Each retry had a fresh 65-s
read-only cell anchor. These corrections do not turn the earlier attempts into
passing runs. The unrelated pre-existing worktree changes remain untouched.

The non-CUDA `pusch_decoder_impl.cpp` and `pusch_processor_impl.cpp` branches
also passed a separate `-fsyntax-only` compiler invocation with warnings as
errors. Its independently checked `resident_acceptance_cpu_157603.watch.jsonl`
passed for 90.157 s with the same PID/UE identities; the compiler exited at
2.326 s. There were zero late messages and five UL discards (three prelaunch,
two after the compiler exited). This is CPU build compatibility evidence,
not a full gNB link, CTest execution or a new CPU OTA comparison.
