#!/usr/bin/env python3
"""Regenerates the computed-goto dispatch table inside w_exec() from its CASE(...) labels.
   python3 tests/gen_dispatch.py          rewrite src/eccles_wasm.c
   python3 tests/gen_dispatch.py --check  exit 1 if the table is stale (used by `make check-gen`)"""
import re, sys, os
path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src", "eccles_wasm.c")
s = open(path).read()
a = s.index("static eccles_wasm_error_t w_exec(")
b = s.index("\ntrap:\n    return err;", a)
body = s[a:b]
# walk the body line by line so entries inside "#if !defined(ECCLES_WASM_NO_FLOAT)" stay guarded
out, seen, guard = [], set(), False
for line in body.split("\n"):
    st = line.strip()
    if st.startswith("#if") and "NO_FLOAT" in st:
        guard = True; continue
    if st.startswith("#endif") and guard:
        guard = False; continue
    for m in re.finditer(r'CASE\(([^)#]+)\)', line):
        t = m.group(1).strip()
        if t in seen: continue
        seen.add(t); out.append((t, guard))
lines, cur = [], False
for t, g in out:
    if g != cur:
        lines.append("#if !defined(ECCLES_WASM_NO_FLOAT)" if g else "#endif"); cur = g
    lines.append("        [%s] = &&L_%s," % (t, t))
if cur: lines.append("#endif")
toks = [t for t, _ in out]
begin = "        /* BEGIN GENERATED DISPATCH (python3 tests/gen_dispatch.py) */\n"
end = "        /* END GENERATED DISPATCH */\n"
i = s.index(begin) + len(begin); j = s.index(end)
new = s[:i] + "\n".join(lines) + "\n" + s[j:]
if "--check" in sys.argv:
    sys.exit(0 if new == s else 1)
open(path, "w").write(new)
print("dispatch table: %d entries" % len(toks))
