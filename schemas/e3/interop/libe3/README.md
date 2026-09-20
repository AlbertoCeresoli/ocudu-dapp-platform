# Vendored libe3 E3AP module

`e3ap-1.0.0.asn1` is the E3AP grammar of the Northeastern University
[libe3](https://github.com/wineslab/libe3) project, copied verbatim from
`messages/asn1/V1/e3ap-1.0.0.asn1` at commit `6f44b6f` (2026-09-01, library
version 0.1.3), Apache-2.0 (see the file header). Only the three-line
provenance comment at the top was added. Do not edit it; re-vendor it.

It is compiled twice in this tree:

- by `asn1c` with `-fprefix=LIBE3_` into `ocudu_dapp_libe3_asn1`, the codec
  behind `e3ap_libe3_codec` and the libe3 dialect of the E3AP SCTP agent
  (`lib/dapp/e3/e3ap_libe3_codec.cpp`);
- by `asn1tools` in `python/tests/test_libe3_interop.py`, the structural and
  byte-level gate against `e3ap-ocudu-v1.asn1`.

The end-to-end interoperability tests under `tests/interop/libe3` build the
upstream library itself (same commit, plus the patches in that directory) and
run its dApp role against the OCUDU agent.
