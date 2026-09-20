# CUDA dApp post-equalization metric reduction

The inline and deferred PUSCH dApp reporting paths now enqueue
`pusch_e2e_dapp_noise_async` on the producer compute stream, before the existing
completion event. The reducer reads the qualified per-RE noise output, excludes
nonfinite/nonpositive values and erasures at or above 1e5, then transfers only
an eight-byte sum/count into preallocated pinned storage. Managed readback uses
the existing context aliases. No new hot-path allocation or synchronization
is added by this API.

After the existing fence, `pusch_e2e_dapp_noise_get_mean` provides the mean or
NAN for unavailable data. `post_eq_sinr_db_from_mean_noise` applies the existing
unit-signal convention and optional explicit calibration offset. Empty grants
clear the count; failures invalidate that grant's reporting, with no fallback
to a previous DMRS result. Do not consume a result after an unsuccessful
enqueue or before completion. Borrowed input/context lifetimes and same-device,
serialized context use are caller responsibilities.

The context reuses at most 512 bytes of its existing partial-noise workspace.
Two bounded kernels handle up to 183,456 RE/layer samples. Compensated FP32
local sums and an FP64 tree retain numerical agreement with the former CPU
reduction, without requiring full-tensor D2H or a host vector resize.

This is transport/reduction plumbing, **not a noise estimator**. Receiver
metric validity still requires correctly normalized physical disturbance.
Learned logits/decoding calibration cannot simply be relabeled SINR. Missing
EPRE/RSRP/TA/CFO/EVM are not restored by this change.

Build the explicit `pusch_dapp_noise_test` and
`pusch_dapp_noise_fault_test` targets. With `CUDA_VISIBLE_DEVICES` empty, run
the former with `--precheck` and the latter without arguments. They are also
registered as no-GPU CTests when `BUILD_OCUDU_PHY_CUDA_TESTS` is enabled.
The numerical GPU test requires explicit `--execute`; do not run it unguarded
on a device shared with a live modem.

Current GB10 evidence: 110 pinned/managed cases passed with less than 2e-6 dB
error versus the former CPU calculation. SM89/90/121 build/precheck passed;
other devices were not executed. Both changed library targets compile.
The complete notifier/decoder integration is not runtime-qualified yet.

A 45-second shared-GPU screen retained four UEs but recorded 54 native
real-time-failure messages. Therefore no sustained latency-isolation or OTA
performance claim is warranted. Keep the loaded modem unchanged until that
behavior and the full receiver's physical measurements are qualified.
