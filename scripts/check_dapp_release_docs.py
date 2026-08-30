#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear

"""Validate the public dApp/E3 documentation set and its relative links."""

from __future__ import annotations

import pathlib
import re
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]
DOCS = ROOT / "docs/dapp"
REQUIRED = {
    "README.md",
    "quickstart.md",
    "yaml_configuration.md",
    "e3_management.md",
    "e3_wire.md",
    "e3_system_design.md",
    "extension_model.md",
    "security_model.md",
    "api_compatibility.md",
    "release_checklist.md",
    "known_limitations.md",
}
LINK_PATTERN = re.compile(r"(?<!!)\[[^]]*\]\(([^)]+)\)")


def main() -> int:
    errors: list[str] = []
    for name in sorted(REQUIRED):
        path = DOCS / name
        if not path.is_file():
            errors.append(f"missing dApp release document: {name}")
            continue
        text = path.read_text(encoding="utf-8")
        if "SPDX-License-Identifier:" not in text:
            errors.append(f"missing SPDX identifier: docs/dapp/{name}")
        for match in LINK_PATTERN.finditer(text):
            target = match.group(1).strip().split(maxsplit=1)[0].strip("<>")
            if not target or target.startswith(("#", "http://", "https://", "mailto:")):
                continue
            target_path = target.split("#", 1)[0].split("?", 1)[0]
            if not target_path:
                continue
            resolved = (path.parent / target_path).resolve()
            if ROOT != resolved and ROOT not in resolved.parents:
                # Workspace-sibling link (e.g. ../../../ocudu-dapp-sdk/...):
                # only checkable when that repo is checked out next to this
                # one. Verify it when the sibling is present; skip it in a
                # standalone checkout, which cannot vouch for other repos.
                try:
                    sibling = ROOT.parent / resolved.relative_to(ROOT.parent).parts[0]
                except ValueError:
                    continue
                if sibling.is_dir() and not resolved.exists():
                    errors.append(f"broken sibling link: docs/dapp/{name} -> {target}")
                continue
            if not resolved.exists():
                errors.append(f"broken relative link: docs/dapp/{name} -> {target}")

    if errors:
        print("\n".join(sorted(set(errors))), file=sys.stderr)
        return 1
    print(f"dApp release documentation checks passed ({len(REQUIRED)} documents)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
