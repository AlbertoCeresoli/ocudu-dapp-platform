# Vendored libe3 E3AP module

`e3ap-1.0.0.asn1` is the E3AP grammar of the Northeastern University
[libe3](https://github.com/wineslab/libe3) project, copied verbatim from
`messages/asn1/V1/e3ap-1.0.0.asn1` at commit `f2329e0` (release tag `0.2.1`,
2026-09-22), Apache-2.0 (see the file header). Only the three-line
provenance comment at the top was added. Do not edit it; re-vendor it.

It is the root of OCUDU's own module: `schemas/e3/e3ap-ocudu-v2.asn1`
(E3AP-OCUDU-V2) is this grammar with OCUDU extension additions behind its
markers, compiled by `asn1c` with `-fprefix=E3V2_` into the codec behind
`e3ap_v2_codec` / `e3ap_libe3_codec`. This copy is the reference it is checked
against:

- `python/tests/test_libe3_interop.py` requires every type here to have the
  identical root in E3AP-OCUDU-V2 (asn1tools structural comparison) and
  OCUDU's codec to reproduce libe3's own encoder vectors byte for byte;
- `ocudu_e3.libe3` compiles it (asn1c-equivalently) as a libe3 decoder for the
  Python tests.

`e3ap-1.0.0.proto` is libe3's Protobuf schema, likewise vendored verbatim
(three provenance comment lines added) and checked against the submodule at
configure time; `schemas/e3/protobuf/e3ap-ocudu-v2.proto` is its wire superset.

The schemas are unchanged in release **0.2.2**, commit
`840b00f7583ee22160bc9b000aa067de9793df2c` (2026-10-05), now selected by
`third_party/libe3`. Configure-time checks compare both vendored schemas to
that checkout. Historical copy provenance above is retained.

The end-to-end tests under `tests/interop/libe3` build that 0.2.2 library
(plus the patches in that directory), run its dApp role against the OCUDU
agent, and recheck the 0.2.1 fixtures against the current encoder. See the
[interop report guide](../../../../tests/interop/libe3/README.md) for
compiler/transport qualifications and the itemized libe3 #84 comparison.
