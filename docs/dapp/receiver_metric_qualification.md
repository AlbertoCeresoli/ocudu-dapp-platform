<!-- SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc. -->
<!-- SPDX-License-Identifier: BSD-3-Clause-Clear -->

# Receiver metric and adaptation qualification

## Production feedback

Native CUDA CE/EQ/RX output is reduced on its inherited producer stream into
the existing fixed-size SINR/EVM accumulator. The baseline and external path
use the same unit-average-power QAM slicer for 4/16/64/256-QAM EVM. This is
decision-directed EVM, not known-transmitted-symbol error; incorrect decisions
can look artificially close to a different constellation point.
Matching the calculation does not establish estimator calibration or make EVM
independent of modulation. For algorithm attribution, stratify by modulation,
allocation width and channel condition; a mixed-MCS EVM mean is descriptive only.

There is no full post-EQ noise download, host-vector resize, new stream,
per-grant allocation, or per-MCS/RB CUDA graph. Existing checked asynchronous
readback and completion fences protect pinned or managed host mailboxes.
Failed submissions do not publish a previous grant's values. Optional RX
symbols/noise produce zero observation counts when absent. Pilot SINR,
EPRE/RSRP/TA/CFO from a previous baseline invocation are not published for a
current external receiver.

Post-EQ SINR remains `-10 log10(mean(valid noise))`, with existing intentional
erasure sentinels excluded. It feeds normal CRC/CSI link adaptation and power
control. EVM does not replace post-EQ SINR. The legacy explicit calibration
override remains supported, but no offset is introduced by this change.

## Scheduler-window telemetry

The E3 metrics producer and dashboard preserve these additive fields:

- `report_sequence`, `report_period_ms`: per-cell report identity and duration.
  Sequence resets on host restart. Deduplicate by host epoch, cell, UE and
  sequence; count gaps explicitly. Polling the same report twice is not two
  observations.
- `ul_first_tx_ok/nok`, `ul_retx_ok/nok`: actual HARQ-matched CRC outcomes,
  classified using retransmission count before updating HARQ state, not RV.
  Unmatched/time-out observations in legacy counters are not invented as
  first transmissions. Compute BLER from summed counts, not mean percentages.
- `ul_olla_eligible_ok/nok`, `ul_olla_skipped`: the controller's actual eligibility
  decision. Eligible updates may still have zero effect at an MCS/offset limit.
  This is observational: the host's existing treatment of retransmission CRCs
  is unchanged.
- `ul_olla_offset_db`, `ul_olla_enabled`, `ul_olla_target_bler`,
  `ul_olla_max_offset_db`, `ul_olla_at_lower_limit/upper_limit`: state at the last
  matched CRC. The offset is not an interval average; zero eligible observations
  means the current interval cannot establish fresh adaptation behavior.
- `ul_prb_symbols`, `ul_retx_prb_symbols`: exact scheduled PRBs times allocated
  OFDM symbols, including DMRS/overhead. These are not payload-only RE counts.
- `ul_mac_delivered_bytes`: successful MAC TB bytes, including padding. Divide
  summed bytes by summed PRB-symbols for a MAC resource-efficiency measure, not
  application spectral efficiency. CRC delivery and allocation may straddle a
  window; use long windows and settling/drain intervals.

Counters are scalar scheduler-thread updates, without hot-path logging,
allocation or transport calls. JSON serialization stays in the existing
lower-rate producer. Old producers yield missing dashboard fields, not zeros.

## A/B protocol

Keep RF/channel/scheduler/BLER targets identical. Warm the instance once, then
use its supported soft-enable setting where available to avoid repeated CUDA
worker construction/destruction. Keep UE population and offered packet rate
fixed within each comparison. Record source/instance/config generations.
Audit actual sender counts as well as requested pacing: a single ping process
can send far below its nominal rate on a high-RTT path. Changes in achieved
offered load must not be attributed to receiver capacity.

Use OFF/ON/OFF windows with settling, a bounded load ramp and immediate
rollback on persistent UE loss, materially worse CRC/drop behavior or L1
errors. Do not equate offered ping rate, theoretical PHY rate, MAC TB rate and
received application payload. Record actual ping replies/loss and bytes as
well as the independent MAC counters. If queues do not remain loaded, call
the result load-limited rather than a capacity measurement.

Closed-loop transmit-power control can hold reported SINR at its target even
with an improved receiver. Inspect OLLA state and eligible outcomes before
expecting a higher MCS. A 2% configured BLER target is not a 10% target.

Required checks include CPU QAM/invalid-noise fixtures, actual CUDA reduction
through pinned and managed readback (all QAMs, changing extents, absent optional
outputs, stale sentinels), scheduler count/reset/eligibility tests, dashboard
E3 merge tests, and a live matched-source comparison. A build alone is not an
OTA latency or performance qualification.

Focused CPU checks (after building the corresponding test targets):

```sh
ctest --test-dir build-cpu --output-on-failure -R 'scheduler_metrics_handler_test|ue_link_adaptation_controller|pusch_equalized_metrics|pusch_metric_provenance'
```

The explicit CUDA `pusch_metric_readback_test --execute` includes the external
reduction cases along with same-stream/side-stream readback. Its
`--external-only` mode runs the reduction matrix through the real side-stream
readback, with both pinned and managed mailboxes. These modes create a CUDA
context: run on a dedicated test GPU or while the radio has already been
deliberately drained, not as an unannounced live-radio precheck. Neither mode
replaces the full PUSCH latency simulator or loaded-L1 tail qualification.
