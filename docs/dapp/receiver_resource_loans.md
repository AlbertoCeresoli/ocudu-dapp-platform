# Host resource loans for deferred full receivers

Keeping an uplink slot alive does not by itself keep a PUSCH demodulator
dependency alive. The processor borrows that dependency from a separate pool;
it owns demodulator buffers, CUDA streams and the resident dApp adapter. A
deferred receiver must retain both ownership chains until safe retirement.

## Production provider

`pusch_processor_impl::concurrent_dependencies_pool_type` uses the existing
`bounded_rc_object_pool`, whose control blocks are allocated during pool
construction. The normal processing path holds its original move-only lease.
Only an explicit `clone()` adds a consumer; the final reference returns the
entry to the pool without destroying it.

During `process_data`, `pooled_pusch_resource_provider` is constructed from the
actual dependency lease, its issuing pool and the originating PUSCH notifier.
It is available through `pusch_demodulator::configuration::resource_owner`
only for the synchronous duration of `demodulate()`. It checks the lease's
actual parent-pool identity before acquiring anything. A missing pool/entry,
wrong parent pool, or missing original processor/slot activity returns an
empty loan. There is no dummy ownership fallback.

Acquisition retains three things:

1. The actual dependency pool's shared lifetime.
2. The original processor/slot activity.
3. A clone of the actual dependency entry.

Destruction reverses that order: return the dependency first, allow original
processor/slot reuse second, release the parent pool last. The parent pool
therefore cannot disappear while an entry is being returned. Separate loans
may finish on different threads; returning the original stack lease is not
enough to make the dependency reusable.

The owning `pusch_resource_loan` has 64 bytes of inline owner storage and is
move-only. Its holder machinery forbids heap allocation. The production
retained bundle uses pre-existing shared/intrusive control blocks; it does not
create a new `shared_ptr` control block per grant. Costs still include atomic
reference operations and the bounded pool scan. Moving the dependency pool
from unique to reference-counted leases also adds reference operations on the
normal path; no target-device speedup or latency bound is claimed.

## Native completion adapter

`native_receiver_resident_adapter::submit` now accepts this move-only loan,
instead of requiring an allocating shared-pointer wrapper around a move-only
dependency. Before-dispatch rejection preserves the caller's loan. Accepted
or uncertain native submissions move it into a fixed record with the original
worker and receipt. Completion inspection alone does not release it.

After all consumers have completed/fenced, release or successful quarantine
retirement moves it into `native_receiver_reclaimed::tensors`. Moving is not
reclamation: the receiving control/reclamation path must explicitly destroy
or reset it when safe. An underlying owner's destructor may release a pool or
CUDA resources, so a type-erased holder is not a promise of constant-time
destruction. Never destroy an unretired loan simply because a deadline expired.

The provider pointer must never be retained or passed through a module's C
interface. Acquire while it is valid, then retain the returned owning loan.
The real processor supplies that provider. The CUDA resident adapter now uses
it for a completion-required native target, retaining dependency-owned metadata
and carrying the original acceptance descriptor through the GPU demodulator.
Host completion capability is still disabled: private workspace provisioning,
error recovery and target-device qualification remain incomplete.

The normal consumer-return hook runs inside `pusch_processor_impl::process_data`,
after the synchronous resident decoder and CPU diagnostic callbacks return.
It queries all supplied host streams before releasing the receipt. During this
call the original stack dependency/activity still exists, and native hook clear
waits for the resident pending state before dropping its original worker owner.
Consequently the normal loan/worker reference drops do not perform final CUDA
owner destruction on L1. This reasoning depends on that production call order;
an arbitrary caller-provided holder does not establish the same property.

If a stream is pending, the loan stays retained. Deactivation's control barrier
can retry after CPU consumers return and streams complete. Partial submissions
use control-only retirement. There is not yet an independently running active-
instance reclaimer for this slow/error path; bounded retained entries can reduce
available capacity. Repeated activation after a poisoned adapter also needs a
qualified reset/replacement policy. Do not advertise capability before these
paths and the actual context/resource provider are ready.

The GPU demodulator calls the resident adapter's `begin_grant()` before any
per-grant buffer reset, including for ordinary baseline/CE/EQ processing.
Pending receiver ownership refuses reuse without altering its saved completion
identity. Once reclamation is complete, that boundary clears the old neural
selection and metric flags; the previous descriptor remains invalid until then.
This avoids poisoning a conventional grant after a live receiver switch. It
does not release a loan early or poll slow consumers on the allocation path.

## What this does not provide

- Authentic ownership does not prove CUDA completion, numerical validity or
  accurate SINR. Existing acceptance and consumer-fence checks still apply.
- The loan does not validate arbitrary tensor extents supplied by a caller.
  The production buffer producer must construct descriptors from the retained
  dependency's actual allocations and bounded capacities.
- It does not provision the learned module's private workspace, prepare stream
  or graph in the host's borrowed CUDA context. Those cold resources need their
  own instance/worker lifetime through retirement.
- Stack metadata cannot escape via a loan. The real receiver binding now keeps
  its preparation, input/output and metric metadata in the retained adapter.
- Pool exhaustion is not a radio decoding failure. Before activation, route
  deferred-resource pressure through software-discard reporting, not fabricated
  CRC/BLER, and retain unfenceable work without allowing unsafe overwrite.

The full host must be rebuilt consistently: internal C++ configuration, pool
and adapter types changed. Public dApp C tables, E3 and FAPI wire layouts did
not change. Keep receiver-completion capability disabled until the actual
demodulator binding, private workspace provisioning, retirement and selected
physical metrics are integrated and target CUDA timing/OTA tests pass.

## Tests

`pusch_resource_loan_test` instantiates the actual PUSCH dependency type/pool
with CPU test doubles for its algorithms, and the actual upper notifier and
slot state machine. It covers fixed-size move-only retention, provider/stack
lease exit, two consumers with cross-thread final release, wrong-parent and
missing ownership rejection, parent-pool destruction ordering, and 1,000
repeated generations. This is not whole-processor or GPU execution.

The vendor-side native qualification additionally sends genuinely pooled
test tensors through the loaded C completion table, actual masked worker,
native adapter and actual resident decoder with CPU-modeled CUDA/LDPC. Both a
normal grant and a partial-submit error retain their tensor pool entry and
original slot until explicit reclamation. See its qualification report for
executed results and live-cell observation limits.
