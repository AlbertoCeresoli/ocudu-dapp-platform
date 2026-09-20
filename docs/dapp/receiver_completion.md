# Deferred native receiver completion

The optional C extension in `use_cases/v1/receiver_completion.h` separates
enqueue success from permission to accept an asynchronous receiver result.
It is for resident Class-A full receivers. It is not an E3 transport, a tensor
allocator, a CUDA context provider, or a replacement for stream ordering.

## Negotiation and compatibility

Existing module and receiver v1 layouts are unchanged. A receiver requiring
this extension advertises `OCUDU_DAPP_RECEIVER_CAP_COMPLETION_V1` together with
the resident-accelerator capability and exports
`ocudu_dapp_query_receiver_completion_v1`. Its **ordinary module query** must
reject hosts without `OCUDU_DAPP_HOST_CAP_RECEIVER_COMPLETION_V1`: an older
loader does not know to look for the extension. Its invoke-only callback must
decline without queuing work.

The loader independently checks the host capability, required symbol, version,
table size, callbacks and reserved fields. Legacy receivers continue through
their existing interface. Capability advertisement is a promise that the
actual host PHY and resource-lifetime adapter are integrated, not merely that
this header or loader support exists. The production host does not yet
advertise it. Do not set the bit just to make a package load.

## Ownership and ordering

1. The host obtains authentic tensor loans and retains the exact input/output
   storage, metadata, stream/context resources and issuing worker.
2. `invoke_receiver_deferred` returns a bounded host ticket. The module receipt
   remains private to that worker. The hook overload returns a shared owner of
   the original worker before releasing publication protection.
3. After backend ordering is satisfied, `inspect_receiver` checks the original
   invocation and receipt. Only OK with `ready=1, valid=1` permits acceptance.
   Inspection does not acknowledge or release storage.
4. The [resident decoder acceptance boundary](resident_receiver_acceptance.md)
   must use that result before committing HARQ/CRC or publishing payload, UCI,
   CSI and optional constellation data. Failure is not a fabricated radio CRC.
5. After **every** consumer has finished or fenced, `release_receiver` may
   acknowledge the receipt. Only then can the caller retire its global loans
   and reclaim its resource owner, on the appropriate reclamation path.

No step looks up the currently active module to complete an earlier grant.
Worker generation and sequence reject stale tickets after slot reuse. A worker
has four fixed pending records and a try-only admission gate; overflow returns
RESOURCE without queuing. Completion callbacks remain available after normal
admission closes or the incident breaker opens. Shared ownership still has
reference-count cost; this is not a measured latency guarantee.

## Failure and retirement

The extension defines BYPASS, UNSUPPORTED, INVALID_INPUT and RESOURCE from
invoke as **nothing enqueued**. Partial or uncertain submission must return
DEVICE_ERROR. Exceptions, malformed output and failed post-enqueue validation
retain a host ticket and set `invoke_fallback=false`. The caller must not
overwrite the possibly in-flight buffers with a conventional receiver.

RESOURCE from inspect/release retains ownership. Inspection can be retried
when not ready; an acceptance attempt after a decoder fence must fail closed
if it cannot establish validity. Once inspected conclusively, a ticket cannot
be accepted again. Release success acknowledges even a rejected result; other
statuses retain its record. Invalid/missing receipts require worker retirement.

Control-only `retire_receiver` closes admission and retries cleanup after all
consumers are withdrawn/fenced, including partial submissions without usable
receipts. It does not promise real-time wall-clock duration. Only successful
retirement invalidates all records and permits normal worker destruction.

Destroying a native worker with pending records deliberately retains its
implementation, library and instance-activity token for process lifetime,
incrementing `quarantined_native_receiver_worker_count()`. This is an ownership
violation fail-safe, **not** normal reclamation. It does not retain arbitrary
caller-owned tensors by itself. A correct host owner must keep those loans
alive and complete explicit retirement; repeated violations can exhaust
resources and must be surfaced operationally.

## Current integration boundary

`native_receiver_resident_adapter` in `phy/upper/dapp` now implements the host
native-C-to-resident-decoder binding. It submits through the production
`pusch_dapp_demodulator_target::invoke_receiver_deferred` and real native hook,
retains the issuing worker and caller-provided tensor owner in four fixed
records, and installs the trusted acceptance callback on the exact codeword.
It verifies the contiguous F16 tensor/descriptor mapping and rejects malformed
or already-bound descriptors before dispatch. Its monotonic publication
generation also protects slot/address reuse across worker replacements.

The target's completion-aware entry is separate from ordinary `invoke`.
Targets which only implement ordinary invocation return UNSUPPORTED without
calling it; a missing receiver returns BYPASS. The native target forwards the
ticket and original-worker owner from the native hook without manufacturing a
receipt. Its implementation is built separately from the CPU demodulator
decorator so this exact production routing can be linked into CPU fault tests.
The CUDA resident adapter now routes completion-required targets through this
entry with stable dependency-owned metadata and a resource loan. Ordinary
receivers retain their ordinary invocation. The target's routing bit is advisory:
the native worker independently checks its actual contract during invocation,
including when activation changes between the routing check and dispatch.

Release is explicitly after decoder return **and** all diagnostic/other
consumers have finished/fenced. Owners move into empty reclamation outputs;
final resource destruction must run on the designated control/reclamation path.
The real processor's normal consumer-return hook may drop additional references
only while its original dependency/activity and the native hook's original
worker owner remain held; see [resource-loan ordering](receiver_resource_loans.md).
Failed
partial submissions retain owners until control retirement. Admission can
close without disabling outstanding acceptance. An unretired adapter destructor
terminates rather than silently freeing in-flight storage. This adapter accepts
complete codewords only; a compacted/changed output needs a continuation loan.

A vendor-side qualification, kept with the receiver package rather than
in this repository, qualifies a connected native
hook -> loaded C table -> vendor worker -> host adapter -> actual
decoder path using CPU-modeled CUDA/LDPC operations. Unlike the previous
qualification, native-C dispatch and decoder acceptance are now exercised in
one chain. The loan in that test owns the actual test tensor allocation, not
just an unrelated sentinel. This still does not supply the production pool
lease or authenticate a caller's arbitrary shared owner in that qualification.

The later [resource-loan provider](receiver_resource_loans.md) retains the
actual reference-counted dependency entry, its verified parent pool and the
original slot activity. The production processor passes a borrowed provider
through its demodulator configuration. Native completion records now accept
its bounded move-only loan directly. The actual GPU demodulator preserves that
adapter's acceptance identity only for the same pointer, count, stream, event,
scrambling identity and buffer index. It must not turn a mismatched/compacted
codeword into an unguarded conventional descriptor. The processor calls the
consumer-return hook after synchronous resident decoder return. Native clear
waits through that boundary and all supplied GPU streams before final release.
Learned-worker private arena/context provisioning and complete slow/error-path
reclamation remain required; production completion capability stays disabled.

### Grant transitions and pending consumers

`pusch_resident_dapp::begin_grant()` is called by the real GPU demodulator
before resetting host vectors or uploading/reusing device storage, regardless
of whether the new grant uses a neural receiver, CE, EQ or the baseline.
Pending receiver ownership refuses admission without resetting the old state.
The caller marks the new input as a software discard and returns before tensor
reuse. This defensive check does not replace the dependency loan, which normally
prevents that demodulator from being selected again while a consumer owns it.
`enable_symbol_capture()` only requests capture for the next demodulation; it
does not clear the previous capture before admission. The accepted demodulation
resets capture descriptors/vectors together with the other per-grant state.

After safe completion, the old neural descriptor remains invalid until this
explicit new-grant boundary. The boundary clears its completion selection and
per-grant symbol/noise flags so a conventional codeword can pass unchanged.
Resetting only inside `invoke_receiver()` is insufficient: an inactive receiver
or an EQ/CE-only grant never calls that method. Clearing the selection during
retirement would instead risk publishing the old codeword without its required
acceptance check. Neither completion nor admission is a GPU fence by itself.

The resident adapter also checks for pending work at the beginning of its
destructor, before freeing any CUDA event or staging buffer. An ownership
violation terminates before destruction; this is a fail-safe, not qualified
production recovery. Do not describe it as crash-free quarantine.

The native loader/worker/hook support and sibling SDK authoring helper are
implemented. A vendor C-table adapter around a production masked receiver exists
worker. Cross-repository CPU fault qualification uses independently loaded
test DSOs and a CPU model of CUDA calls. Those fixtures are not deployable
neural packages and cannot qualify GPU math or timing.

The [PUSCH processing-lifetime loan](pusch_processing_lifetime.md) retains
the actual wrapper through process/decoder and notifier diagnostic tails;
terminal MAC notification alone no longer releases that pool entry. Its
focused CPU qualification also covers the real logging/metrics forwarders.
This host loan does not replace the global resource owner or stream fences.

Production private-resource provisioning, qualification of the integrated
original-owner/acceptance path, UCI-compaction continuation ownership, selected physical
CSI and target-GPU/full-PUSCH latency remain required before activation.
Neither a receipt nor a populated dashboard metric establishes radio gain.
