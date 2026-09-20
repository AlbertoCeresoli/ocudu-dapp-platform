#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear
"""OCUDU <-> libe3 interoperability report.

Runs the fail-closed interop gate (scripts/check_dapp_interop_gate.py) against a build configured with
OCUDU_DAPP_LIBE3_INTEROP=ON and renders what it proved: provenance (commits, submodules, patches,
toolchain), every test with status, time and captured output, the coverage of libe3's E3AP procedures,
the sessions the real libe3 dApp reported, and the pinned byte-exact vectors. Markdown is always written;
a PDF when pdflatex is available. The exit code is the gate's, so CI can keep publishing the report of a
failed run.
"""
from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import textwrap
import xml.etree.ElementTree as ET
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
GATE = REPO / "scripts" / "check_dapp_interop_gate.py"
PINNED = REPO / "python" / "tests" / "fixtures" / "libe3_vectors.json"
PATCHES = REPO / "tests" / "interop" / "libe3" / "patches"
PROCEDURES = ["SetupRequest", "SetupResponse", "SubscriptionRequest", "SubscriptionResponse", "SubscriptionDelete",
              "IndicationMessage", "DAppControlAction", "DAppReport", "XAppControlAction", "ReleaseMessage", "MessageAck"]
# asn1tools and asn1c disagree on constrained UTF8String, the documented divergence (test_libe3_interop.py).
UTF8_DIVERGENCE = {"SetupRequest", "SetupResponse"}
# Which summary flags of the end-to-end session exercise which procedures on the wire.
SESSION_STEPS = [
    ("setup", "setup completed", ["SetupRequest", "SetupResponse"]),
    ("subscribed", "subscription accepted", ["SubscriptionRequest", "SubscriptionResponse"]),
    ("indications", "indications received", ["IndicationMessage"]),
    ("control_ack", "control action acknowledged", ["DAppControlAction", "MessageAck"]),
    ("report_ack", "report acknowledged", ["DAppReport", "MessageAck"]),
    ("xapp_controls", "xApp control action delivered to the dApp", ["XAppControlAction"]),
    ("unsubscribed", "subscription delete confirmed", ["SubscriptionDelete", "SubscriptionResponse"]),
    ("released", "release accepted", ["ReleaseMessage"]),
]
CATEGORIES = [
    (r"^dapp_e3ap_libe3\.e2e$", "End to end against the upstream libe3 library"),
    (r"^dapp_e3ap_libe3\.interop$", "Schema-level interoperability (asn1tools on the vendored libe3 module)"),
    (r"^dapp_e3ap_libe3_codec\.", "libe3 APER codec (generated from the upstream module)"),
    (r"^dapp_e3ap_sctp_server\..*libe3", "E3AP SCTP agent, libe3 dialect"),
    (r"^dapp_e3ap_sctp_server\.", "E3AP SCTP agent, native dialect and shared regions"),
    (r"^dapp_e3ap_codec\.|^dapp_e3_aper_codec\.|^dapp_e3_python_codec\.", "Native E3AP and service-model codecs"),
    (r"cross_language|python_codec", "Cross-language wire checks (Python client against the C++ agent)"),
    (r"shared_region", "Shared-memory region validation"),
    (r"telemetry_isolation|portable_data_store", "Telemetry store and producer isolation"),
]
OUTPUT_TESTS = re.compile(r"libe3|cross_language|shared_region\.validation")
MAX_OUTPUT_LINES = 90


def sh(*args: str, cwd: Path | None = None) -> str:
    try:
        return subprocess.check_output(list(args), cwd=cwd, text=True, stderr=subprocess.DEVNULL).strip()
    except (OSError, subprocess.CalledProcessError):
        return ""


def cache_value(build: Path, key: str) -> str:
    cache = build / "CMakeCache.txt"
    if not cache.exists():
        return ""
    match = re.search(rf"^{re.escape(key)}:[A-Z]+=(.*)$", cache.read_text(), re.M)
    return match.group(1).strip() if match else ""


def provenance(build: Path) -> dict:
    binaries = {}
    for name in ("e3ap_test_server", "dapp_runtime_test", "dapp_telemetry_allocation_test", "libe3_interop_dapp",
                 "libe3_interop_vectors"):
        found = sorted(build.rglob(name))
        binaries[name] = (dt.datetime.fromtimestamp(found[0].stat().st_mtime).isoformat(timespec="minutes")
                          if found else "not built")
    submodules = {}
    for line in sh("git", "submodule", "status", "third_party/libe3", "third_party/asn1c", cwd=REPO).splitlines():
        parts = line.split()
        if len(parts) >= 2:
            submodules[parts[1]] = parts[0].lstrip("-+") + (f" ({parts[2].strip('()')})" if len(parts) > 2 else "")
    return {
        "generated": dt.datetime.now().astimezone().isoformat(timespec="seconds"),
        "host": f"{platform.node()} {platform.system()} {platform.release()} {platform.machine()}",
        "ocudu_commit": sh("git", "rev-parse", "HEAD", cwd=REPO),
        "ocudu_branch": sh("git", "rev-parse", "--abbrev-ref", "HEAD", cwd=REPO),
        "ocudu_dirty": "yes" if sh("git", "status", "--porcelain", "--untracked-files=no", cwd=REPO) else "no",
        "submodules": submodules,
        "patches": sorted(p.name for p in PATCHES.glob("*.patch")),
        "cmake_options": {k: cache_value(build, k) for k in ("CMAKE_BUILD_TYPE", "CMAKE_CXX_COMPILER", "ENABLE_E3",
                                                             "ENABLE_E3_ASN1_CODEC", "OCUDU_DAPP_LIBE3_INTEROP",
                                                             "OCUDU_LIBE3_GIT_TAG")},
        "toolchain": {
            "cmake": sh("cmake", "--version").splitlines()[0] if sh("cmake", "--version") else "",
            "python": platform.python_version(),
            "asn1tools": sh(sys.executable, "-c", "import asn1tools; print(asn1tools.__version__)"),
            "pdflatex": "available" if shutil.which("pdflatex") else "not found",
        },
        "binaries": binaries,
    }


def run_gate(build: Path, summary_file: Path) -> tuple[int, str]:
    env = dict(os.environ, OCUDU_LIBE3_E2E_SUMMARY=str(summary_file))
    if summary_file.exists():
        summary_file.unlink()
    completed = subprocess.run([sys.executable, str(GATE), "--build", str(build)], env=env, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, text=True)
    return completed.returncode, completed.stdout


def load_tests(build: Path) -> list[dict]:
    report = build / "dapp-interop.xml"
    if not report.exists():
        return []
    tests = []
    for case in ET.parse(report).getroot().findall(".//testcase"):
        name = case.attrib.get("name", "")
        failure = case.find("failure")
        status = ("skipped" if case.find("skipped") is not None else "failed" if failure is not None
                  else "passed" if case.attrib.get("status") == "run" else case.attrib.get("status", "unknown"))
        out = case.find("system-out")
        tests.append({"name": name, "status": status, "time": float(case.attrib.get("time", 0.0)),
                      "output": (out.text or "") if out is not None else "",
                      "category": next((label for pattern, label in CATEGORIES if re.search(pattern, name)),
                                       "Other E3 host tests")})
    return tests


def load_summaries(summary_file: Path) -> list[dict]:
    if not summary_file.exists():
        return []
    return [json.loads(line) for line in summary_file.read_text().splitlines() if line.strip()]


def coverage(tests: list[dict], summaries: list[dict]) -> list[dict]:
    passed = {t["name"] for t in tests if t["status"] == "passed"}
    vectors = {v["name"]: v for v in json.loads(PINNED.read_text())["vectors"]} if PINNED.exists() else {}
    codec_ok = "dapp_e3ap_libe3_codec.decodes_and_reproduces_vectors_from_the_upstream_libe3_encoder" in passed
    e2e_ok = "dapp_e3ap_libe3.e2e" in passed
    full = [s for s in summaries if s.get("test") == "test_full_session_over_one_sctp_association"]
    on_wire: set[str] = set()
    for s in full:
        for key, _, procs in SESSION_STEPS:
            value = s.get(key)
            if value and value != "negative":
                on_wire.update(procs)
    rows = []
    for name in PROCEDURES:
        rows.append({
            "procedure": name,
            "vector": f"{len(vectors[name]['hex']) // 2} B" if name in vectors else "none",
            "codec": "yes" if codec_ok and name in vectors else "no",
            "asn1tools": ("documented divergence" if name in UTF8_DIVERGENCE else "yes" if e2e_ok else "no"),
            "session": "yes" if name in on_wire else ("codec and vectors only" if e2e_ok else "no"),
        })
    return rows


def esc(text: str) -> str:
    return re.sub(r"([&%$#_{}~^\\])", lambda m: {"\\": r"\textbackslash{}", "~": r"\textasciitilde{}",
                                                  "^": r"\textasciicircum{}"}.get(m.group(1), "\\" + m.group(1)), text)


def brief_args(args: list[str]) -> str:
    """Command-line arguments with long hex payloads abbreviated to their length."""
    return " ".join(f"<{len(a) // 2} B>" if len(a) > 24 and re.fullmatch(r"[0-9a-fA-F]+", a) else a for a in args) or "-"


def path(text: str) -> str:
    """A long identifier that may break at punctuation (url package)."""
    return r"\path{" + text.replace("{", "").replace("}", "") + "}"


def clip(output: str) -> str:
    lines = [textwrap.shorten(line, 112, placeholder=" ...") for line in output.splitlines() if line.strip()]
    if len(lines) > MAX_OUTPUT_LINES:
        lines = lines[:MAX_OUTPUT_LINES] + [f"... {len(lines) - MAX_OUTPUT_LINES} more lines in dapp-interop.log"]
    return "\n".join(lines)


def render_markdown(ctx: dict) -> str:
    p, tests = ctx["provenance"], ctx["tests"]
    counts = {k: sum(t["status"] == k for t in tests) for k in ("passed", "failed", "skipped")}
    out = [f"# OCUDU <-> libe3 interoperability report", "",
           f"**Result: {ctx['verdict']}** ({counts['passed']} passed, {counts['failed']} failed, "
           f"{counts['skipped']} skipped of {len(tests)} required host tests). Generated {p['generated']} on {p['host']}.",
           "", "## Provenance", "",
           f"- ocudu `{p['ocudu_commit']}` ({p['ocudu_branch']}, uncommitted changes: {p['ocudu_dirty']})"]
    out += [f"- {k} `{v}`" for k, v in p["submodules"].items()]
    out += [f"- libe3 patches applied: {', '.join(p['patches']) or 'none'}",
            "- CMake: " + ", ".join(f"{k}={v}" for k, v in p["cmake_options"].items() if v),
            "- Toolchain: " + ", ".join(f"{k} {v}" for k, v in p["toolchain"].items() if v),
            "- Binaries: " + ", ".join(f"{k} {v}" for k, v in p["binaries"].items()), "",
            "## libe3 E3AP 1.0.0 procedure coverage", "",
            "| procedure | pinned libe3 vector | OCUDU codec decodes and reproduces | asn1tools <-> libe3 | on the wire in the E2E session |",
            "|---|---|---|---|---|"]
    out += [f"| {r['procedure']} | {r['vector']} | {r['codec']} | {r['asn1tools']} | {r['session']} |" for r in ctx["coverage"]]
    out += ["", "## End-to-end sessions reported by the libe3 dApp", ""]
    for s in ctx["summaries"]:
        out.append(f"- `{s['test']}` args `{brief_args(s['args'])}`: " + ", ".join(
            f"{k}={s[k]}" for k in ("dapp_id", "ran_identifier", "ran_functions", "telemetry_ids", "control_ids",
                                    "subscription_id", "indications", "indication_bytes", "control_ack", "report_ack",
                                    "xapp_controls", "xapp_control_hex", "unsubscribed", "released", "ok") if k in s))
    out += ["", "## Tests", "", "| test | category | status | s |", "|---|---|---|---|"]
    out += [f"| {t['name']} | {t['category']} | {t['status']} | {t['time']:.2f} |" for t in tests]
    out += ["", "## Captured output of the interoperability tests", ""]
    for t in tests:
        if OUTPUT_TESTS.search(t["name"]) and t["output"].strip():
            out += [f"### {t['name']} ({t['status']})", "", "```", clip(t["output"]), "```", ""]
    out += ["## Gate log (tail)", "", "```", "\n".join(ctx["gate_log"].splitlines()[-40:]), "```", ""]
    return "\n".join(out)


def render_latex(ctx: dict) -> str:
    p, tests = ctx["provenance"], ctx["tests"]
    counts = {k: sum(t["status"] == k for t in tests) for k in ("passed", "failed", "skipped")}
    colour = "green!50!black" if ctx["verdict"] == "PASSED" else "red!70!black"
    L = [r"\documentclass[10pt,a4paper]{article}", r"\usepackage[margin=1.8cm]{geometry}",
         r"\usepackage{booktabs,longtable,xcolor,array,url,needspace}", r"\usepackage[T1]{fontenc}", r"\usepackage{hyperref}",
         r"\urlstyle{tt}\sloppy",
         r"\hypersetup{colorlinks=true,linkcolor=blue!50!black,urlcolor=blue!50!black}",
         r"\setlength{\parskip}{4pt}\setlength{\parindent}{0pt}",
         r"\title{\vspace{-1cm}OCUDU $\leftrightarrow$ libe3 interoperability report}",
         r"\author{Generated by \texttt{scripts/libe3\_interop\_report.py}}", r"\date{" + esc(p["generated"]) + "}",
         r"\begin{document}\maketitle",
         r"\section*{Result: \textcolor{" + colour + "}{" + ctx["verdict"] + "}}",
         f"{counts['passed']} passed, {counts['failed']} failed, {counts['skipped']} skipped of {len(tests)} required host "
         f"tests (the fail-closed gate \\texttt{{check\\_dapp\\_interop\\_gate.py}}: every required test must exist, run and "
         f"not skip). Host {esc(p['host'])}.",
         r"\Needspace*{8\baselineskip}\section*{Provenance}", r"\begin{itemize}\raggedright\setlength{\itemsep}{0pt}",
         r"\item ocudu \texttt{" + esc(p["ocudu_commit"]) + "} (" + esc(p["ocudu_branch"]) + ", uncommitted changes: "
         + p["ocudu_dirty"] + ")"]
    L += [r"\item " + esc(k) + r" \texttt{" + esc(v) + "}" for k, v in p["submodules"].items()]
    L += [r"\item libe3 patches applied: " + (", ".join(path(x) for x in p["patches"]) or "none"),
          r"\item CMake: " + ", ".join(esc(k) + "=" + path(v) for k, v in p["cmake_options"].items() if v),
          r"\item Toolchain: " + ", ".join(esc(f"{k} {v}") for k, v in p["toolchain"].items() if v),
          r"\item Binaries (build time): " + ", ".join(esc(k) + " " + esc(v) for k, v in p["binaries"].items()),
          r"\end{itemize}",
          r"\Needspace*{12\baselineskip}\section*{libe3 E3AP 1.0.0 procedure coverage}",
          r"Every procedure of the upstream module: the pinned vector is what libe3's own encoder produced; the OCUDU "
          r"libe3 codec must decode it and reproduce the bytes; asn1tools on the vendored grammar must cross to libe3 "
          r"field for field (SetupRequest/SetupResponse carry constrained UTF8String, the documented asn1tools/asn1c "
          r"divergence); the last column says whether the real libe3 dApp exchanged the procedure with the OCUDU agent "
          r"in the end-to-end session.",
          r"\small\begin{longtable}{@{}lllll@{}}\toprule",
          r"procedure & pinned vector & codec decodes + reproduces & asn1tools $\leftrightarrow$ libe3 & on the wire (E2E)\\\midrule\endhead"]
    L += [" & ".join(esc(r[k]) for k in ("procedure", "vector", "codec", "asn1tools", "session")) + r"\\" for r in ctx["coverage"]]
    L += [r"\bottomrule\end{longtable}\normalsize", r"\Needspace*{8\baselineskip}\section*{End-to-end sessions reported by the libe3 dApp}"]
    if ctx["summaries"]:
        L += [r"\small\begin{longtable}{@{}>{\raggedright\arraybackslash}p{4.4cm}>{\raggedright\arraybackslash}p{11.6cm}@{}}\toprule session & what the libe3 dApp reported\\\midrule\endhead"]
        for s in ctx["summaries"]:
            fields = ", ".join(f"{k}={s[k]}" for k in ("dapp_id", "ran_identifier", "ran_functions", "telemetry_ids",
                                                       "control_ids", "subscription_id", "indications", "indication_bytes",
                                                       "control_ack", "report_ack", "xapp_controls", "xapp_control_hex",
                                                       "unsubscribed", "released", "ok") if k in s)
            L.append(path(s["test"].replace("test_", "")) + r"\newline args: "
                     + " ".join(r"\texttt{" + esc(a) + "}" for a in brief_args(s["args"]).split())
                     + " & " + esc(fields) + r"\\")
        L += [r"\bottomrule\end{longtable}\normalsize"]
    else:
        L.append("No session summaries were recorded (the end-to-end test did not run).")
    L += [r"\Needspace*{8\baselineskip}\section*{Tests}", r"\small\begin{longtable}{@{}>{\raggedright\arraybackslash}p{8.4cm}>{\raggedright\arraybackslash}p{5.4cm}lr@{}}\toprule test & category & status & s\\\midrule\endhead"]
    for t in tests:
        col = {"passed": "green!50!black", "failed": "red!70!black"}.get(t["status"], "orange!80!black")
        L.append(path(t["name"]) + " & " + esc(t["category"]) + r" & \textcolor{" + col + "}{" + t["status"]
                 + "} & " + f"{t['time']:.2f}" + r"\\")
    L += [r"\bottomrule\end{longtable}\normalsize", r"\Needspace*{8\baselineskip}\section*{Captured output of the interoperability tests}",
          r"Full output of every test is in \texttt{dapp-interop.log} beside this report; long outputs are clipped here."]
    for t in tests:
        if OUTPUT_TESTS.search(t["name"]) and t["output"].strip():
            L += [r"\Needspace*{6\baselineskip}\subsection*{" + path(t["name"]) + " (" + t["status"] + ")}", r"{\scriptsize\begin{verbatim}",
                  clip(t["output"]), r"\end{verbatim}}"]
    L += [r"\Needspace*{6\baselineskip}\section*{Gate log (tail)}", r"{\scriptsize\begin{verbatim}", "\n".join(ctx["gate_log"].splitlines()[-40:]),
          r"\end{verbatim}}", r"\end{document}"]
    return "\n".join(L)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build", type=Path, required=True, help="build tree configured with OCUDU_DAPP_LIBE3_INTEROP=ON")
    parser.add_argument("--out", type=Path, help="output directory (default <build>/dapp-interop-report)")
    parser.add_argument("--no-run", action="store_true", help="render from the last gate run instead of running it")
    args = parser.parse_args()
    build = args.build.resolve()
    out = (args.out or build / "dapp-interop-report").resolve()
    out.mkdir(parents=True, exist_ok=True)
    summary_file = out / "libe3_e2e_sessions.jsonl"
    if args.no_run:
        rc, log = 0, (build / "dapp-interop.log").read_text() if (build / "dapp-interop.log").exists() else ""
    else:
        rc, log = run_gate(build, summary_file)
    tests = load_tests(build)
    ctx = {"provenance": provenance(build), "tests": tests, "summaries": load_summaries(summary_file),
           "gate_log": log, "verdict": "PASSED" if rc == 0 and tests and all(t["status"] == "passed" for t in tests) else "FAILED"}
    ctx["coverage"] = coverage(tests, ctx["summaries"])
    (out / "libe3_interop_report.md").write_text(render_markdown(ctx))
    (out / "libe3_interop_report.tex").write_text(render_latex(ctx))
    for name in ("dapp-interop.xml", "dapp-interop.log"):
        if (build / name).exists():
            shutil.copy2(build / name, out / name)
    pdf = "not rendered (pdflatex not found)"
    if shutil.which("pdflatex"):
        for _ in range(2):
            subprocess.run(["pdflatex", "-interaction=nonstopmode", "-halt-on-error", "libe3_interop_report.tex"], cwd=out,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        pdf = str(out / "libe3_interop_report.pdf") if (out / "libe3_interop_report.pdf").exists() else "pdflatex failed (see .log)"
        for suffix in (".aux", ".out"):
            (out / f"libe3_interop_report{suffix}").unlink(missing_ok=True)
    print(f"libe3 interop report: {ctx['verdict']}, {len(tests)} tests; markdown {out / 'libe3_interop_report.md'}; pdf {pdf}")
    return rc


if __name__ == "__main__":
    sys.exit(main())
