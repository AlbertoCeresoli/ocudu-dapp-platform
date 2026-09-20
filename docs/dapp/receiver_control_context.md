# CUDA receiver control context

The completion-capable full receiver borrows the PHY execution context. The
management thread cannot assume that context is current when it creates or
retires a worker. `native_control_context` is a host-private C++ lifetime and
cold-callback boundary; it changes neither the public C receiver tables nor E3.

## Binding and ownership

`pusch_demodulator_gpu_impl::install_resident_dapp()` captures the context of
its actual, non-null execution stream before installing a resident adapter.
Capture requires the calling thread's current context to match that stream,
and requires it to be the already-active device primary context. A device
ordinal alone is not sufficient. Capture does not initialize CUDA, select a
device, create a context, reset a device or synchronize the stream.

The retained primary-context reference is held by the hook lane and, before
the first fallible worker-create callback, by the native instance. Workers
also retain it. An instance cannot mix different context identities. Partial
creation failure does not discard this ownership: module quiescence may still
need to clean up allocations made before the failure.

The retained context is pushed only around cold module/worker lifecycle
callbacks. The caller's previous context is checked and restored, including
when a module callback returns an error or throws. Per-grant invocation,
receipt inspection/release and metric snapshots do not acquire this context
wrapper. This design does not add a context switch to every inference.

## Failure contract

Missing or mismatched context fails before worker creation. An ambiguous
push/pop or restoration failure poisons the context owner; subsequent cold
calls fail closed. An uncertain reference is retained until process exit,
not reset or released speculatively. A failed primary retain does not prove
ownership even if a driver supplied an output handle.

Worker and instance quarantine retain the library and context together.
Normal cleanup releases the primary reference only after the associated
worker/module lifetime is finished. External primary-context reset remains
forbidden while any host or module user is alive. Retaining a context does
not protect it from another caller explicitly resetting it.

This is not an automatic recovery mechanism. Bounded quarantine admission,
independent active-instance reclamation, driver release-failure observability
and poisoned-worker replacement remain separate hardening work. Do not
enable completion-capable receivers solely because this boundary exists.

## Qualification boundary

A focused vendor-side qualification test, kept with the receiver package,
compiles the
actual context owner, native instance/worker/slot and resident adapter. Its
CUDA driver is CPU-modeled. The tests cover absent/foreign/same prior contexts,
callback errors and exceptions, capture failures, matching/mismatched stream
ownership, poisoned push/pop, mixed-instance rejection and all four QAM
metadata paths without per-grant context pushes. The actual GPU demodulator
installation caller and CUDA adapter test translation unit are syntax-checked,
not executed on a device by this qualification.

Actual target-device lifecycle isolation, full-shape warmup, CUDA numerical
parity and whole-PUSCH latency require separate measurements. A passing
CPU-modeled test does not qualify activation or an OTA performance gain.

Related: [completion contract](receiver_completion.md),
[resource loans](receiver_resource_loans.md). CUDA primary-context ownership
semantics are documented in the
[NVIDIA Driver API](https://docs.nvidia.com/cuda/cuda-driver-api/group__CUDA__PRIMARY__CTX.html).
