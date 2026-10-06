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
import socket
import subprocess
import sys
import textwrap
import xml.etree.ElementTree as ET
from pathlib import Path

from check_dapp_interop_gate import REQUIRED
from libe3_issue_coverage import BASELINE, TRACKER, issue_coverage

REPO = Path(__file__).resolve().parents[1]
GATE = REPO / "scripts" / "check_dapp_interop_gate.py"
PINNED = REPO / "python" / "tests" / "fixtures" / "libe3_vectors.json"
PATCHES = REPO / "tests" / "interop" / "libe3" / "patches"
PROCEDURES = ["SetupRequest", "SetupResponse", "SubscriptionRequest", "SubscriptionResponse", "SubscriptionDelete",
              "IndicationMessage", "DAppControlAction", "DAppReport", "XAppControlAction", "ReleaseMessage", "MessageAck"]
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
    (r"^dapp_e3ap_libe3\.e2e_sctp$", "End to end against the upstream libe3 library, SCTP link layer"),
    (r"^dapp_e3ap_libe3\.e2e_zmq$", "End to end against the upstream libe3 library, ZMQ link layer (ipc and tcp)"),
    (r"^dapp_e3ap_libe3\.interop$", "Schema-level interoperability and Python (ocudu_e3.libe3) against libe3's encoder"),
    (r"^dapp_e3ap_v2_codec\.", "E3AP-OCUDU-V2 APER codec (libe3's module + OCUDU additions; APER = Protobuf)"),
    (r"^dapp_e3ap_libe3_codec\.", "libe3 profile of the E3AP-OCUDU-V2 APER codec against libe3's own vectors"),
    (r"^dapp_e3ap_protobuf_codec\.", "E3AP Protobuf codec (hand-written; libe3 and OCUDU profiles)"),
    (r"^dapp_e3ap_protobuf_conformance\.", "E3AP Protobuf codec against libprotobuf (libe3 and OCUDU schemas)"),
    (r"^dapp_e3ap_sctp_server\..*libe3", "E3AP SCTP agent, libe3 profile"),
    (r"^dapp_e3ap_sctp_server\..*protobuf", "E3AP SCTP agent, Protobuf peers (libe3 and OCUDU profiles)"),
    (r"^dapp_e3ap_sctp_server\.", "E3AP SCTP agent, OCUDU profile and shared regions"),
    (r"^dapp_e3ap_codec\.|^dapp_e3_aper_codec\.|^dapp_e3_python_codec\.", "Native E3AP and service-model codecs"),
    (r"cross_language|python_codec", "Cross-language wire checks (Python client against the C++ agent)"),
    (r"shared_region", "Shared-memory region validation"),
    (r"telemetry_isolation|portable_data_store", "Telemetry store and producer isolation"),
]
OUTPUT_TESTS = re.compile(r"libe3|cross_language|shared_region\.validation|protobuf_conformance\.random")
ENCODINGS = [("asn1", "APER"), ("protobuf", "Protobuf")]
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
        binary = build / "tests/unittests/dapp" / ("interop/libe3" if name.startswith("libe3_") else "") / name
        binaries[name] = (dt.datetime.fromtimestamp(binary.stat().st_mtime).isoformat(timespec="minutes")
                          if binary.is_file() else "not built")
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
        "vector_source": json.loads(PINNED.read_text()).get("source", "unknown"),
        "cmake_options": {k: cache_value(build, k) for k in ("CMAKE_BUILD_TYPE", "CMAKE_CXX_COMPILER", "ENABLE_E3",
                                                             "ENABLE_E3_ASN1_CODEC", "OCUDU_DAPP_LIBE3_INTEROP",
                                                             "OCUDU_LIBE3_GIT_TAG", "OCUDU_LIBE3_SOURCE_DIR",
                                                             "OCUDU_LIBE3_EFFECTIVE_VERSION",
                                                             "OCUDU_LIBE3_EFFECTIVE_REVISION")},
        "toolchain": {
            "cmake": sh("cmake", "--version").splitlines()[0] if sh("cmake", "--version") else "",
            "python": platform.python_version(),
            "asn1tools": sh(sys.executable, "-c", "import asn1tools; print(asn1tools.__version__)"),
            "protoc": sh("protoc", "--version") if shutil.which("protoc") else "not found",
            "pdflatex": "available" if shutil.which("pdflatex") else "not found",
        },
        "binaries": binaries,
    }


def run_gate(build: Path, summary_file: Path, profile: str) -> tuple[int, str]:
    env = dict(os.environ, OCUDU_LIBE3_E2E_SUMMARY=str(summary_file))
    if summary_file.exists():
        summary_file.unlink()
    completed = subprocess.run([sys.executable, str(GATE), "--build", str(build), "--profile", profile], env=env,
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    return completed.returncode, completed.stdout


def load_tests(build: Path) -> list[dict]:
    report = build / "dapp-interop.xml"
    if not report.exists():
        return []
    tests = []
    for case in ET.parse(report).getroot().findall(".//testcase"):
        name = case.attrib.get("name", "")
        status = ("failed" if case.find("failure") is not None or case.find("error") is not None
                  else "skipped" if case.find("skipped") is not None
                  else "passed" if case.attrib.get("status", "run") == "run"
                  else case.attrib.get("status", "unknown"))
        out = case.find("system-out")
        tests.append({"name": name, "status": status, "time": float(case.attrib.get("time", 0.0)),
                      "output": plain(out.text or "") if out is not None else "",
                      "category": next((label for pattern, label in CATEGORIES if re.search(pattern, name)),
                                       "Other E3 host tests")})
    return tests


def load_summaries(summary_file: Path) -> list[dict]:
    if not summary_file.exists():
        return []
    return [json.loads(line) for line in summary_file.read_text().splitlines() if line.strip()]


def coverage(tests: list[dict], summaries: list[dict]) -> list[dict]:
    passed = {t["name"] for t in tests if t["status"] == "passed"}
    pinned = json.loads(PINNED.read_text()) if PINNED.exists() else {}
    vectors = {v["name"]: v for v in pinned.get("vectors", [])}
    protobuf_vectors = {v["name"]: v for v in pinned.get("protobuf_vectors", [])}
    codec_ok = "dapp_e3ap_libe3_codec.decodes_and_reproduces_vectors_from_the_upstream_libe3_encoder" in passed
    protobuf_ok = ("dapp_e3ap_protobuf_codec.decodes_and_reproduces_vectors_from_the_upstream_libe3_protobuf_encoder"
                   in passed)
    e2e_ok = bool({"dapp_e3ap_libe3.e2e_sctp", "dapp_e3ap_libe3.e2e_zmq"} & passed)
    # Which link layers carried each procedure in a full session ("<transport>:test_full_session").
    on_wire: dict[str, set[str]] = {}
    for s in summaries:
        if not str(s.get("test", "")).endswith(":test_full_session"):
            continue
        for key, _, procs in SESSION_STEPS:
            value = s.get(key)
            if value and value != "negative":
                for proc in procs:
                    on_wire.setdefault(proc, set()).add(f"{s.get('transport', '?')}/{s.get('encoding', 'asn1')}")
    rows = []
    for name in PROCEDURES:
        rows.append({
            "procedure": name,
            "vector": f"{len(vectors[name]['hex']) // 2} B" if name in vectors else "none",
            "codec": "yes" if codec_ok and name in vectors else "no",
            "protobuf": (f"yes ({len(protobuf_vectors[name]['hex']) // 2} B)" if protobuf_ok and name in protobuf_vectors
                         else "no"),
            # Python (ocudu_e3.libe3, asn1c-equivalent) reproduces libe3's vector and libe3 decodes Python's bytes.
            "asn1tools": "yes" if e2e_ok and "dapp_e3ap_libe3.interop" in passed else "no",
            "session": (", ".join(sorted(on_wire[name])) if name in on_wire
                        else ("codec and vectors only" if e2e_ok else "no")),
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


ANSI = re.compile(r"\x1b\[[0-9;?]*[ -/]*[@-~]|\x1b[@-Z\\-_]")
CONTROL = re.compile(r"[\x00-\x08\x0b-\x1f\x7f]")


def plain(text: str) -> str:
    """Captured output without ANSI colour sequences and control characters (CI runners colour
    gtest output; pdflatex rejects the escape byte)."""
    return CONTROL.sub("", ANSI.sub("", text))


def clip(output: str) -> str:
    lines = [textwrap.shorten(line, 112, placeholder=" ...") for line in plain(output).splitlines() if line.strip()]
    if len(lines) > MAX_OUTPUT_LINES:
        lines = lines[:MAX_OUTPUT_LINES] + [f"... {len(lines) - MAX_OUTPUT_LINES} more lines in dapp-interop.log"]
    return "\n".join(lines)


def kernel_has_sctp() -> bool:
    try:
        probe = socket.socket(socket.AF_INET, socket.SOCK_STREAM, socket.IPPROTO_SCTP)
    except OSError:
        return False
    probe.close()
    return True


def gate_verdict(tests: list[dict], returncode: int, profile: str) -> str:
    """Known interoperability differences are not failures; missing required evidence is."""
    passed = {t["name"] for t in tests if t["status"] == "passed"}
    return ("PASSED" if returncode == 0 and tests and all(t["status"] == "passed" for t in tests)
            and REQUIRED[profile] <= passed else "FAILED")


def link_layers(tests: list[dict], summaries: list[dict], profile: str, kernel_sctp: bool) -> list[dict]:
    """One row per libe3 link layer: what ran against the OCUDU agent, and if not, why."""
    status = {t["name"]: t["status"] for t in tests}
    sessions = {}
    for s in summaries:
        transport = s.get("transport")
        if transport:
            entry = sessions.setdefault((transport, s.get("encoding", "asn1")), [0, 0])
            entry[0] += 1
            entry[1] += 1 if s.get("ok") else 0

    def row(name, libe3_side, agent_side, test, transport, encoding, label):
        state = status.get(test)
        if state is None:
            reason = (("this runner's kernel has no SCTP (zmq gate profile)" if not kernel_sctp
                       else "zmq gate profile selected") if profile == "zmq" and test.endswith("sctp") else "not run")
            verdict = "SKIPPED" if profile == "zmq" and test.endswith("sctp") else "NOT RUN"
        else:
            verdict, reason = state.upper(), ""
        done, ok = sessions.get((transport, encoding), [0, 0])
        if verdict == "PASSED" and not done:
            # The suite passed but this encoding never ran in it (libe3 built without Protobuf).
            verdict, reason = "NOT RUN", f"no {label} session recorded"
        return {"layer": name, "encoding": label, "libe3": libe3_side, "agent": agent_side, "verdict": verdict,
                "sessions": f"{ok}/{done} sessions ok" if done else "-", "reason": reason}

    layers = [
        ("SCTP association", "E3LinkLayer::SCTP_ASSOCIATION (OCUDU patch 0001)", "SCTP carrier, port 36423",
         "dapp_e3ap_libe3.e2e_sctp", "sctp"),
        ("ZMQ over ipc://", "E3LinkLayer::ZMQ, stock connector", "libe3 ZMQ carrier (REP/PUB/SUB)",
         "dapp_e3ap_libe3.e2e_zmq", "zmq-ipc"),
        ("ZMQ over tcp://", "E3LinkLayer::ZMQ, stock connector", "libe3 ZMQ carrier (REP/PUB/SUB)",
         "dapp_e3ap_libe3.e2e_zmq", "zmq-tcp"),
    ]
    return [row(name, libe3_side, agent_side, test, transport, encoding, label)
            for name, libe3_side, agent_side, test, transport in layers for encoding, label in ENCODINGS]


def render_markdown(ctx: dict) -> str:
    p, tests = ctx["provenance"], ctx["tests"]
    counts = {k: sum(t["status"] == k for t in tests) for k in ("passed", "failed", "skipped")}
    out = [f"# OCUDU <-> libe3 interoperability report", "",
           f"**Result: {ctx['verdict']}** ({counts['passed']} passed, {counts['failed']} failed, "
           f"{counts['skipped']} skipped of {len(tests)} required tests, gate profile `{ctx['profile']}`). "
           f"Generated {p['generated']} on {p['host']} (kernel SCTP: {'yes' if ctx['kernel_sctp'] else 'no'}).",
           "", "## Link layers", "",
           "| link layer | encoding | libe3 side | OCUDU agent side | result | full sessions | note |",
           "|---|---|---|---|---|---|---|"]
    out += [f"| {r['layer']} | {r['encoding']} | {r['libe3']} | {r['agent']} | **{r['verdict']}** | {r['sessions']} "
            f"| {r['reason']} |" for r in ctx["link_layers"]]
    out += ["", "## libe3 issue #84: item-by-item comparison", "",
            f"Tracking [wineslab/libe3#84]({TRACKER}). {BASELINE}", "",
            "Test success describes the stated scope, not closure of the upstream issue. Remaining differences, "
            "documentation-only items and untested transports do **not** fail CI. Regressions in required checks do.", "",
            "`TESTED` means all named evidence passed; `PARTIAL`/`NOT RUN` means some/all evidence is absent or skipped. "
            "SCTP evidence requires the host profile. APER uses a matched compiler; SCTP uses patch 0001, not stock libe3 SCTP.", "",
            "| issue | topic | disposition | evidence this run | remaining difference / qualification |",
            "|---|---|---|---|---|"]
    out += [f"| [#{r['number']}]({r['url']}) | {r['topic']} | {r['disposition']} | {r['evidence_result']} | {r['note']} |"
            for r in ctx["issues"]]
    out += ["", "### Exact evidence for each issue", ""]
    for r in ctx["issues"]:
        out.append(f"- [#{r['number']}]({r['url']}): " + ("; ".join(
            f"`{e['test']}` ({e['status']})" for e in r["evidence"]) or "documentation review only; no runtime check"))
    out += ["", "## Provenance", "",
           f"- ocudu `{p['ocudu_commit']}` ({p['ocudu_branch']}, uncommitted changes: {p['ocudu_dirty']})"]
    out += [f"- {k} `{v}`" for k, v in p["submodules"].items()]
    out += [f"- libe3 patches applied: {', '.join(p['patches']) or 'none'}",
            f"- Pinned vector source: {p['vector_source']}",
            "- CMake: " + ", ".join(f"{k}={v}" for k, v in p["cmake_options"].items() if v),
            "- Toolchain: " + ", ".join(f"{k} {v}" for k, v in p["toolchain"].items() if v),
            "- Binaries: " + ", ".join(f"{k} {v}" for k, v in p["binaries"].items()), "",
            "## libe3 E3AP 1.0.0 procedure coverage", "",
            "| procedure | pinned libe3 APER vector | OCUDU APER codec decodes and reproduces | OCUDU Protobuf codec reproduces libe3's Protobuf vector | Python (ocudu_e3.libe3) <-> libe3 | on the wire in the E2E session (link layer/encoding) |",
            "|---|---|---|---|---|---|"]
    out += [f"| {r['procedure']} | {r['vector']} | {r['codec']} | {r['protobuf']} | {r['asn1tools']} | {r['session']} |"
            for r in ctx["coverage"]]
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
         r"\setlength{\tabcolsep}{3.5pt}",
         r"\title{\vspace{-1cm}OCUDU $\leftrightarrow$ libe3 interoperability report}",
         r"\author{Generated by \texttt{scripts/libe3\_interop\_report.py}}", r"\date{" + esc(p["generated"]) + "}",
         r"\begin{document}\maketitle",
         r"\section*{Result: \textcolor{" + colour + "}{" + ctx["verdict"] + "}}",
         f"{counts['passed']} passed, {counts['failed']} failed, {counts['skipped']} skipped of {len(tests)} required host "
         f"tests (the fail-closed gate \\texttt{{check\\_dapp\\_interop\\_gate.py}}: every required test must exist, run and "
         f"not skip; profile \\texttt{{{esc(ctx['profile'])}}}). Host {esc(p['host'])}, kernel SCTP: "
         f"{'yes' if ctx['kernel_sctp'] else 'no'}.",
         r"\Needspace*{10\baselineskip}\section*{Link layers}",
         r"Each libe3 link layer exercised end to end between the real upstream libe3 dApp and the OCUDU E3 agent.",
         r"\small\begin{longtable}{@{}>{\raggedright\arraybackslash}p{2.2cm}>{\raggedright\arraybackslash}p{1.4cm}"
         r">{\raggedright\arraybackslash}p{3.6cm}>{\raggedright\arraybackslash}p{3.0cm}>{\raggedright\arraybackslash}p{1.6cm}"
         r">{\raggedright\arraybackslash}p{1.7cm}>{\raggedright\arraybackslash}p{2.3cm}@{}}\toprule",
         r"link layer & encoding & libe3 side & OCUDU agent side & result & full sessions & note\\\midrule\endhead"]
    L += [" & ".join([esc(r["layer"]), esc(r["encoding"]), " ".join(path(word) for word in r["libe3"].split()), esc(r["agent"]),
                      r"\textbf{\textcolor{" + ("green!50!black" if r["verdict"] == "PASSED" else
                                                 "orange!80!black" if r["verdict"] == "SKIPPED" else "red!70!black")
                      + "}{" + r["verdict"] + "}}", esc(r["sessions"]), esc(r["reason"])]) + r"\\"
          for r in ctx["link_layers"]]
    L += [r"\bottomrule\end{longtable}\normalsize",
          r"\section*{libe3 issue \#84: item-by-item comparison}",
          r"Tracker: \url{" + TRACKER + "}. " + esc(BASELINE),
          "Remaining differences and absent optional transport coverage are informational, not CI failures. "
          "TESTED means all listed evidence passed, not that the upstream issue is closed. PARTIAL/NOT RUN marks "
          "missing or skipped evidence. APER uses a matched compiler; SCTP uses local patch 0001."]
    for r in ctx["issues"]:
        L += [r"\Needspace*{5\baselineskip}\subsection*{\href{" + r["url"] + "}{\\#" + str(r["number"])
              + "}: " + esc(r["topic"]) + "}",
              esc(r["disposition"] + "; evidence: " + r["evidence_result"] + ". " + r["note"])]
        if r["evidence"]:
            L += [r"{\small " + r"\par ".join(path(e["test"]) + " (" + esc(e["status"]) + ")"
                                              for e in r["evidence"]) + "}"]
    L += [
         r"\Needspace*{8\baselineskip}\section*{Provenance}", r"\begin{itemize}\raggedright\setlength{\itemsep}{0pt}",
         r"\item ocudu \texttt{" + esc(p["ocudu_commit"]) + "} (" + esc(p["ocudu_branch"]) + ", uncommitted changes: "
         + p["ocudu_dirty"] + ")"]
    L += [r"\item " + esc(k) + r" \texttt{" + esc(v) + "}" for k, v in p["submodules"].items()]
    L += [r"\item libe3 patches applied: " + (", ".join(path(x) for x in p["patches"]) or "none"),
          r"\item Pinned vector source: " + esc(p["vector_source"]),
          r"\item CMake: " + ", ".join(esc(k) + "=" + path(v) for k, v in p["cmake_options"].items() if v),
          r"\item Toolchain: " + ", ".join(esc(f"{k} {v}") for k, v in p["toolchain"].items() if v),
          r"\item Binaries (build time): " + ", ".join(esc(k) + " " + esc(v) for k, v in p["binaries"].items()),
          r"\end{itemize}",
          r"\Needspace*{12\baselineskip}\section*{libe3 E3AP 1.0.0 procedure coverage}",
          r"Every procedure of the upstream module: the pinned vector is what libe3's own encoder produced; the OCUDU "
          r"libe3 codec must decode it and reproduce the bytes; the Python codec (ocudu\_e3.libe3, the vendored grammar "
          r"compiled the way asn1c encodes constrained UTF8String) must reproduce it and libe3 must decode what Python "
          r"emits, field for field; the Protobuf column says the hand-written OCUDU Protobuf codec decodes libe3's own "
          r"ProtobufE3Encoder output and reproduces it byte for byte (that codec is further checked against libprotobuf "
          r"on random PDUs of both the libe3 schema and the OCUDU superset schema); the last column says over which link "
          r"layer and encoding the real libe3 dApp exchanged the procedure with the OCUDU agent in the end-to-end session.",
          r"\small\begin{longtable}{@{}>{\raggedright\arraybackslash}p{3.1cm}>{\raggedright\arraybackslash}p{1.3cm}"
          r">{\raggedright\arraybackslash}p{1.6cm}>{\raggedright\arraybackslash}p{1.8cm}>{\raggedright\arraybackslash}p{1.6cm}"
          r">{\raggedright\arraybackslash}p{6.4cm}@{}}\toprule",
          r"procedure & APER vector & APER codec reproduces & Protobuf codec reproduces & Python $\leftrightarrow$ libe3 & "
          r"on the wire (E2E, link layer/encoding)\\\midrule\endhead"]
    L += [" & ".join(esc(r[k]) for k in ("procedure", "vector", "codec", "protobuf", "asn1tools", "session")) + r"\\"
          for r in ctx["coverage"]]
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
    parser.add_argument("--profile", choices=("auto", "host", "zmq"), default="auto",
                        help="auto detects kernel SCTP; host requires SCTP and ZMQ; zmq needs no kernel SCTP")
    args = parser.parse_args()
    kernel_sctp = kernel_has_sctp()
    if args.profile == "auto":
        args.profile = "host" if kernel_sctp else "zmq"
    build = args.build.resolve()
    out = (args.out or build / "dapp-interop-report").resolve()
    out.mkdir(parents=True, exist_ok=True)
    summary_file = out / "libe3_e2e_sessions.jsonl"
    if args.no_run:
        rc, log = 0, (build / "dapp-interop.log").read_text() if (build / "dapp-interop.log").exists() else ""
    else:
        rc, log = run_gate(build, summary_file, args.profile)
    try:
        tests, summaries = load_tests(build), load_summaries(summary_file)
    except (ET.ParseError, ValueError, OSError) as error:
        # Preserve a useful failed-run artifact even when the test process left
        # truncated JUnit or session output behind.
        tests, summaries, rc = [], [], 1
        log += f"\nCannot load current test evidence: {error}\n"
    ctx = {"provenance": provenance(build), "tests": tests, "summaries": summaries,
           "gate_log": plain(log), "verdict": gate_verdict(tests, rc, args.profile), "issues": issue_coverage(tests)}
    ctx["coverage"] = coverage(tests, ctx["summaries"])
    ctx["profile"] = args.profile
    ctx["kernel_sctp"] = kernel_sctp
    ctx["link_layers"] = link_layers(tests, ctx["summaries"], args.profile, ctx["kernel_sctp"])
    (out / "libe3_interop_report.md").write_text(render_markdown(ctx))
    (out / "libe3_interop_report.tex").write_text(render_latex(ctx))
    (out / "libe3_issue_coverage.json").write_text(json.dumps(
        {"tracker": TRACKER, "audit_baseline": BASELINE, "profile": args.profile, "verdict": ctx["verdict"],
         "provenance": ctx["provenance"], "issues": ctx["issues"]}, indent=2) + "\n")
    # Keep the current gate failure, including preflight diagnostics, with the artifact.
    (out / "dapp-interop.log").write_text(plain(log))
    for name in ("dapp-interop.xml",):
        if (build / name).exists():
            if build != out:
                shutil.copy2(build / name, out / name)
        else:
            (out / name).unlink(missing_ok=True)
    (out / "libe3_interop_report.pdf").unlink(missing_ok=True)
    pdf = "not rendered (pdflatex not found)"
    if shutil.which("pdflatex"):
        for _ in range(2):
            subprocess.run(["pdflatex", "-interaction=nonstopmode", "-halt-on-error", "libe3_interop_report.tex"], cwd=out,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        pdf = str(out / "libe3_interop_report.pdf") if (out / "libe3_interop_report.pdf").exists() else "pdflatex failed (see .log)"
        for suffix in (".aux", ".out"):
            (out / f"libe3_interop_report{suffix}").unlink(missing_ok=True)
    print(f"libe3 interop report: {ctx['verdict']}, {len(tests)} tests; markdown {out / 'libe3_interop_report.md'}; pdf {pdf}")
    return rc or (0 if ctx["verdict"] == "PASSED" else 1)


if __name__ == "__main__":
    sys.exit(main())
