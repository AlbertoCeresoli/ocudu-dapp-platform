# Optional common pilot feedback for CUDA PUSCH receivers

This experimental host facility supplies conventional pilot-derived CSI when
a full receiver replaces channel estimation, equalization and demapping but
does not supply an authoritative post-equalization noise measurement. It is
not an algorithm-gain claim or a substitute for receiver qualification.

## Configuration

Set `OCUDU_PUSCH_COMMON_PILOT_CSI=1` before starting the gNB to opt in. The
default is off. For a comparison using this statistic, explicitly select
`pusch_sinr_calc_method: channel_estimator` in the existing expert PHY
configuration. The environment flag does not change that selection, activate
a package, apply a SINR offset or claim that the receiver supplied noise.

Keep the selected method fixed between conventional and replacement-receiver
arms. Changing the method itself may change link adaptation or power control;
measure a new conventional baseline after changing it. A lower BLER caused by
lower MCS is not by itself a receiver improvement.

Admission currently supports SISO, type-1 DMRS, two DMRS symbols and 30-kHz
SCS with the primitive's preallocated bounds. Unsupported full-receiver
grants remain on the conventional CUDA path. Other unsupported conventional
profiles keep their existing feedback. Count these separately from actual
replacement invocations in performance qualification.

Host pre-admission now reports exactly one
`class_a.fallback.unsupported.metadata` event for a selected full receiver
whose common-pilot profile is unsupported, without invoking the module.
`mode_with_pilot_admission` snapshots selection once; inactive/CE/EQ/conflict
and supported profiles do not acquire this fallback reason. The reason shares
the existing metadata-admission counter; it is not a new per-cause counter.
Always reconcile actual invocation counts too. No public C ABI or E3 wire
change is involved.

## Measurement and lifetime contract

The common stage uses known transmitted DMRS and observed received pilots.
An explicitly known in-grant DC pilot is excluded from estimation, smoothing
and measurement counts; it is not a zero-valued observation. Unallocated REs
are not assumed to be noise-only. Conventional residual/noise conventions and
their calibration limitations remain unchanged.

`pusch_e2e_validate_pilot_metrics` checks admission without CUDA submission or
state mutation. It is not a persistent admission lease or a completion token.
The caller must retain the same handle, configuration and input grid.

After conventional data processing, the host uses
`pusch_e2e_refresh_pilot_metrics_with_dc`, preserving that grant's post-EQ/EVM
accumulators. After inline replacement, it uses
`pusch_e2e_process_pilot_metrics_with_dc`, clearing stale data statistics.
Explicitly authoritative dApp noise can subsequently populate post-EQ SINR.
Neither path changes already-produced LLRs or optional equalized symbols.

Pilot work is queued before the direct-grid loan's completion event is
committed. The existing checked readback and successful completion fence
cover the results. The resident estimator stub starts each grant with empty
CSI. Measurement failure suppresses the new statistics rather than reporting
old values or retrying the decoded data on CPU. Missing pilot, post-EQ and EVM
sources remain distinct; the notifier must never relabel one as another.

The path adds no per-grant allocation, CUDA graph, tensor transport over E3
or explicit blocking synchronization. It still adds GPU work and small
readbacks using existing buffers. First-use costs, stream contention and
end-to-end deadlines need actual-target measurement.

## Tests and current qualification boundary

`pusch_metric_provenance_test` checks source separation through the real
processor notifier adapter, including absent completion and both notification
forms. `pusch_pilot_metrics_fault_test` checks admission, preserving-layout
corruption and reset failures before allocation or kernel work.
`pusch_metric_readback_fault_test` covers failed submission/completion.

The explicit `pusch_pilot_metrics_test --execute-dc` device test compares
pilot arithmetic with an independent CPU reference and checks exact data
output/statistic preservation while poisoning erased DC samples. It requires
an anchored external live-cell guard. Do not run it unguarded beside an
operational cell. These primitive and notifier tests do not execute the
complete production host/decoder pipeline or qualify OTA sensitivity,
all-profile support, deadline isolation or receiver lifecycle recovery.

`pusch_resident_admission_test` exercises all selection/requirement/profile
combinations and repeated changing grants/deactivation on CPU. The isolated
`pusch_e2e_pipeline_test` has explicit `--sinr-method channel_estimator` and
`--require-pilot-csi` checks after the real decoder, with TA/CFO in its sample
records. Its `--dapp-receiver-completion` option sets the host capability only
on its own socket-free test service; the ordinary service/gNB default stays
off. Baseline CRC disagreement now also fails the exit status and JSON verdict.

The first guarded full-host smoke (164501) was stopped during CUDA factory
startup, before any decoded frame. It did not qualify the adapter or receiver.
The test now propagates the configured PRB/layer/port bounds into its frontend
factories and records finer cold subphases. Internal production GPU working
sets still include fixed maximum-capacity allocations; this is not proof of
bounded startup latency. Do not repeat the original probe unchanged or use
primitive passes as authorization for live receiver promotion.
