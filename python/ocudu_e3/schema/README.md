<!-- SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc. -->
<!-- SPDX-License-Identifier: BSD-3-Clause-Clear -->

# Synced schema copies

The `.asn1` modules in this directory are **synced copies of `schemas/e3`**
at the repository root, checked in so that pip wheels of `ocudu-e3` ship the
schemas as `ocudu_e3/schema/*.asn1`. Do not edit them here; edit
`schemas/e3/` and re-copy. A source checkout still wins: the schema search
order in `ocudu_e3/schema.py` tries this directory last.
