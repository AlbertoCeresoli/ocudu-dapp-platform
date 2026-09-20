# PUSCH processing lifetime and receiver completion

A terminal SCH result is a delivery event, not permission to reuse every
processor, tensor or CUDA resource associated with that grant. The notifier
adaptor still has CRC/constellation diagnostics to finish after delivering
the result, and the decoder and its caller may still be on the stack.

## Host processing activity

`pusch_processor_result_notifier::acquire_processing_activity()` returns a
move-only `pusch_processing_activity`. The default implementation returns an
empty activity for unpooled notifiers. A pooled processor returns a loan on
its original wrapper, using a fixed two-pointer token and an atomic count.
There is no allocation, queue, lock acquisition or accelerator call in this
token. Its atomic/reference-count cost still needs target-device measurement.

The wrapper keeps an implicit loan until terminal SCH notification and an
explicit loan through the outer `process()` call. The real processor retains
an activity through `process_data()` and returns its local dependency lease
before releasing that activity. The real notifier adaptor retains an activity
through its post-result diagnostic tail. Logging and metrics decorators
forward acquisition to the originating notifier; callers acquire before a
decorator clears its notification pointer on terminal delivery.

The last release empties the wrapper's unique pool pointer before publishing
the pool entry. It does not touch the wrapper after publication, when another
thread may already be using it. The inner notifier still calls its result
sink before diagnostics. The existing metrics decorator joins SCH with
`process()` return before forwarding it, so synchronous diagnostics can precede
upper-layer delivery in that configuration. Retaining ownership does not change
this existing ordering or add a metrics/diagnostic wait to result delivery.

An asynchronous consumer must acquire an activity **before** scheduling or
terminal result delivery, move it into its continuation, and release it only
after its last use of protected state. Acquiring through an old notifier after
terminal delivery is not supported. The pool itself must outlive all loans.
This does not change the existing terminal-SCH contract or repair UCI-only
completion semantics. UCI-only pool/lifecycle behavior requires separate review.

## Original uplink slot ownership

The outer `detail::pusch_processor_result_notifier_adaptor` also retains a
counted activity. Its terminal result closes an implicit reference, while the
actual `uplink_processor_impl::process_pusch` task keeps an explicit reference
through its processing and trace-scope cleanup. The wrapper holds an upstream
activity obtained from that exact outer notifier, propagating its lifetime
through asynchronous decoder and diagnostic consumers. No active-module lookup
or unrelated shared-pointer sentinel substitutes for this original owner.

Only the outer adaptor's final release calls
`state_machine.on_finish_processing_pdu()`. The slot therefore remains busy
until its task and consumers finish, even after its SCH result has reached the
upper layer. This protects the uplink processor's slot repository, grid and
payload-pool reuse via its existing state machine. Multiple PDUs retain their
own activities; finishing one does not make a still-occupied slot reusable.

On the final wrapper release, both owner members are moved into locals before
the inner pool entry is published. The inner entry returns first; only then
is the original outer-slot activity released. The outer adaptor likewise moves
its callback out before invoking it, permitting immediate adaptor reuse without
the old generation subsequently touching the new callback. Notification's
exchange now clears the actual notifier member, rather than exchanging a
temporary local and leaving the member live.

`pusch_slot_activity` covers the actual outer adaptor and slot state machine,
including propagation through the actual wrapper/notifier/decorator chain,
prompt result and unchanged CSI/payload forwarding, synchronous/asynchronous
tails, a final release on another thread, immediate reentrant configuration,
1,000 repeated slot generations and multiple PDUs. These component tests do
not execute the complete uplink executor task or GPU kernels. The actual task
source is checked in both CPU and CUDA preprocessing configurations.

## Not a CUDA or global resource fence

An activity retains the processor wrapper and, with the propagation above,
the original uplink slot until safe reuse. It does not authenticate or retain
independently allocated output tensors or a GPU context. It does not
wait for a stream, prove inference validity, retire a receiver ticket, or
make uncertain output safe for a conventional overwrite.

The [resource-loan provider](receiver_resource_loans.md) now retains the actual
dependency pool entry and this original activity together. The processor
supplies it during demodulation. The [receiver completion adapter](receiver_completion.md)
still needs that owner bound into the actual accelerated invocation and an
explicit continuation through decoder return and all dependent CUDA/diagnostic
streams. Partial-submission errors
must preserve those owners and honor the prohibition on fallback until safe
retirement. The current accelerated demodulator does not yet complete that
integration. Keep completion capability disabled until it does.

## Compatibility and tests

This adds an internal host C++ virtual method. Rebuild the complete host and
all consumers of that C++ interface consistently before deployment. It does
not alter the public dApp C tables, E3 messages or FAPI wire layout.

`pusch_processing_activity_test` exercises the actual bounded wrapper pool,
notifier adaptor and logging/metrics decorators. It covers synchronous and
asynchronous diagnostic tails, deferred consumers, repeated reuse, a
cross-thread release, default empty loans and move-only exact-once release.
The deferred-consumer tests do not execute CUDA or prove side-stream fencing.

The bounded pool's bitmap rotation also now handles zero rotation without
shifting a 64-bit value by 64. `bounded_object_pool_bitmap_test.cpp`, included
in `object_pool_test`, covers 128 rotation counts with mixed, one-bit, empty
and full patterns. The initial combined qualification found this undefined
behavior despite passing the lifetime assertions; that run remains failed.

The qualification record, legacy-versus-fixed control and deployment
limitations are kept with the vendor receiver package, as is the later
outer-slot qualification; it does not replace the earlier
failed-run history or constitute receiver performance qualification.
