<!-- SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc. -->
<!-- SPDX-License-Identifier: BSD-3-Clause-Clear -->

# Optional full-receiver symbol output

The full-receiver v1 interface still requires LLRs, not intermediate channel
estimates or equalized symbols. Direct-bit receivers may never form symbols.
For constellation diagnostics, a host can now request a caller-owned CF32
`[data_re, layer]` tensor in the append-only `equalized_symbols` tail of
`ocudu_dapp_receiver_metrics_v1`.

This is an optional output of the existing receiver invocation, not a second
callback. It shares the same grant, selected-RE ordering, stream and completion
lease as the LLRs, so the host does not need to reinvoke the receiver or retain
its private intermediate tensors to obtain a constellation.

## Contract

- The typed metadata schema remains `OCUDU_DAPP_METADATA_RECEIVER_METRICS_V1`.
  Its existing noise request and reserved prefix keep their offsets.
- Both the metadata byte extent and its `struct_size` must cover
  `OCUDU_DAPP_RECEIVER_EQUALIZED_SYMBOLS_MIN_SIZE_V1` before inspecting the tail.
  Older, shorter metadata remains valid. A zero symbol tensor `struct_size`
  means no symbol request.
- Use `OCUDU_DAPP_LAYOUT_PUSCH_EQUALIZED_DATA_RE_LAYER_V1`, layout version 1,
  in exactly the same selected-RE/layer order as the receiver LLRs.
- Return unsliced estimates in the transmitted QAM symbol domain, not hard
  decisions or normalized neural features. Finite zero erasures are permitted.
  The output must not alias inputs, LLRs, noise, metadata or private workspace.
- A receiver may decline even a well-formed request without failing the LLR
  operation. Only set `OCUDU_DAPP_RECEIVER_EQUALIZED_SYMBOLS_VALID_V1` after
  writing/enqueueing the complete requested tensor. Never substitute a new
  pointer or change the caller's tensor descriptor or lease.
- Clear output flags on each invocation. A clear symbol-valid flag means
  **no current constellation**, not permission to reuse prior/baseline symbols.
- CUDA writes use the inherited invocation stream. The bit certifies output
  delivery, not GPU completion; existing completion fences still govern reads,
  buffer reuse, and unload. Do not synchronize the L1 thread for diagnostics.

The symbol-valid and post-EQ-noise-valid flags are independent. Symbols do not
automatically grant scheduler SINR authority. In particular, decoding/LLR
calibration variance must not silently become physical scheduler noise.
All four validity combinations are supported: neither output, noise only,
symbols only, or both. A supplied noise buffer is a request, not an obligation
to assert its validity when returning symbols.

## Host and reference implementations

The host CPU and CUDA adapters request symbols when capture is enabled. The
CPU path exposes its current finite symbol buffer; the CUDA path feeds the
existing bounded asynchronous constellation tap. Missing output yields an
empty capture. No new E3 tensor transport or management operation is needed.
The SDK CPU/CUDA reference receivers export their existing equalizer result;
the receiver algorithm, LLRs and noise calculation are unchanged.

## Validation and deployment status

Focused CPU checks cover genuine legacy-sized metadata, all four QAM orders,
unchanged LLR/noise outputs, stale validity, malformed/aliased buffers and host
capture enable/disable/decline. CUDA source and regression tests are also
provided. Cold compilation is not GPU numerical, latency or OTA qualification.
The running gNB is not updated simply by editing these headers: rebuild the
host and applicable packages, then validate GPU execution and fresh dashboard
captures before claiming deployment or OTA gains.
