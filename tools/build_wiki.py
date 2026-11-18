#!/usr/bin/env python3
"""build_wiki.py - expand wiki-src/*.md into wiki/*.md and one ECCLES_WASM_WIKI.txt.

Markers inside wiki-src pages:
    {{include path/to/file}}     the file, in a fenced code block (language from the extension)
    {{raw path/to/file}}         the file pasted as it is (for markdown tables)
    {{output NAME}}              the captured output of examples/wiki/host/NAME.c (run examples/wiki/run.sh first)
    {{version}}                  the library version

    python3 tools/build_wiki.py           write wiki/ and ECCLES_WASM_WIKI.txt
    python3 tools/build_wiki.py --check   exit 1 when wiki/ is out of date
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LANG = {".c": "c", ".h": "c", ".cpp": "cpp", ".ino": "cpp", ".rs": "rust", ".sh": "bash", ".py": "python",
        ".ini": "ini", ".txt": "", ".yml": "yaml", ".json": "json", ".cmake": "cmake", ".conf": "", ".md": "markdown"}


def version():
    for line in open(os.path.join(ROOT, "src", "eccles_wasm.h")):
        m = re.search(r'ECCLES_WASM_VERSION_STRING\s+"([^"]+)"', line)
        if m:
            return m.group(1)
    return "?"


def expand(text, page):
    def inc(m):
        rel = m.group(1).strip()
        path = os.path.join(ROOT, rel)
        if not os.path.isfile(path):
            sys.exit("%s: missing include %s" % (page, rel))
        body = open(path).read().rstrip("\n")
        lang = LANG.get(os.path.splitext(rel)[1], "")
        return "```%s\n%s\n```\n*(file: `%s`)*" % (lang, body, rel)

    def out(m):
        path = os.path.join(ROOT, "build", "wiki", "out", m.group(1).strip() + ".txt")
        if not os.path.isfile(path):
            sys.exit("%s: no captured output for %s (run: make wiki-examples)" % (page, m.group(1)))
        return "```text\n%s\n```" % open(path).read().rstrip("\n")

    def raw(m):
        path = os.path.join(ROOT, m.group(1).strip())
        if not os.path.isfile(path):
            sys.exit("%s: missing raw include %s" % (page, m.group(1).strip()))
        return open(path).read().rstrip("\n")

    text = re.sub(r"\{\{raw ([^}]+)\}\}", raw, text)
    text = re.sub(r"\{\{include ([^}]+)\}\}", inc, text)
    text = re.sub(r"\{\{output ([^}]+)\}\}", out, text)
    return text.replace("{{version}}", version())


def build():
    src = os.path.join(ROOT, "wiki-src")
    pages = sorted(f for f in os.listdir(src) if f.endswith(".md"))
    result = {}
    for f in pages:
        result[f] = expand(open(os.path.join(src, f)).read(), f)
    order = [p for p in open(os.path.join(src, "ORDER")).read().split() if p]
    txt = ["ECCLES WASM %s - COMPLETE WIKI\n%s\n" % (version(), "=" * 60)]
    toc = ["CONTENTS\n"]
    for i, p in enumerate(order, 1):
        toc.append("  %2d. %s" % (i, p.replace("-", " ")))
    txt.append("\n".join(toc) + "\n")
    for p in order:
        body = result.get(p + ".md")
        if body is None:
            sys.exit("ORDER lists %s but wiki-src/%s.md does not exist" % (p, p))
        txt.append("\n\n%s\n%s\n%s\n\n%s" % ("#" * 78, "## PAGE: " + p.replace("-", " "), "#" * 78, body))
    result["__txt__"] = "\n".join(txt) + "\n"
    return result


def coverage(pages):
    """every public function in eccles_wasm.h must be documented; every eccles_wasm_* name in the wiki must exist."""
    hdr = open(os.path.join(ROOT, "src", "eccles_wasm.h")).read()
    code = open(os.path.join(ROOT, "src", "eccles_wasm.c")).read()
    declared = set(re.findall(r"\b(eccles_wasm_[a-z_0-9]+)\s*\(", hdr))
    declared = {n for n in declared if not n.endswith("_fn") and not n.startswith("eccles_wasm__")}
    text = "\n".join(v for k, v in pages.items() if k != "__txt__")
    problems = []
    for n in sorted(declared):
        stem = re.sub(r"^eccles_wasm_(vm_|sim_)?", "", n)
        if n in text or ("_" + stem) in text or ("`" + stem) in text:
            continue
        problems.append("undocumented in wiki: " + n)
    known = set(re.findall(r"\beccles_wasm_[a-z_0-9]+", hdr + code))
    prose = re.sub(r"\]\([^)]*\)", "]", text)
    for m in sorted(set(re.finditer(r"\beccles_wasm_[a-z_0-9]+", prose)), key=lambda m: m.group(0)):
        n = m.group(0)
        tail = prose[m.end():m.end() + 2]
        if tail.startswith("*") or tail.startswith(".h") or n.endswith("_") or n == "eccles_wasm_runtime":
            continue
        if n not in known:
            problems.append("wiki mentions a name that does not exist: " + n)
    return problems


def main():
    res = build()
    probs = coverage(res)
    if probs:
        print("\n".join(probs))
        return 1
    outputs = {os.path.join(ROOT, "wiki", k): v for k, v in res.items() if k != "__txt__"}
    outputs[os.path.join(ROOT, "ECCLES_WASM_WIKI.txt")] = res["__txt__"]
    if "--check" in sys.argv:
        bad = [p for p, v in outputs.items() if not os.path.isfile(p) or open(p).read() != v]
        for p in bad:
            print("out of date:", os.path.relpath(p, ROOT))
        return 1 if bad else 0
    os.makedirs(os.path.join(ROOT, "wiki"), exist_ok=True)
    for p, v in outputs.items():
        open(p, "w").write(v)
    print("wrote %d wiki pages + ECCLES_WASM_WIKI.txt" % (len(outputs) - 1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
