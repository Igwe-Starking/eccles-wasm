#!/usr/bin/env python3
"""Fails if the C or Rust guest SDK disagrees with the runtime's import table (names + argument counts)."""
import os, re, sys
root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
src = open(os.path.join(root, "src", "eccles_wasm.c")).read()
# {"name", hw_x, "i(iii)"}
rt = {}
for m in re.finditer(r'\{"(\w+)",\s*(?:hw|wasi)_\w+,\s*"([ivIfF])\(([iIfF]*)\)"\}', src):
    rt[m.group(1)] = len(m.group(3))
hw = {k: v for k, v in rt.items() if k not in ("fd_write", "proc_exit", "args_sizes_get", "environ_sizes_get", "args_get", "environ_get", "clock_time_get", "random_get", "sched_yield")}
bad = 0
def compare(label, found):
    global bad
    for n, a in sorted(found.items()):
        if n not in hw: print("%s: import '%s' is not provided by the runtime" % (label, n)); bad += 1
        elif hw[n] != a: print("%s: '%s' takes %d args in the SDK but %d in the runtime" % (label, n, a, hw[n])); bad += 1
    for n in sorted(set(hw) - set(found)):
        print("%s: runtime import '%s' has no SDK declaration" % (label, n)); bad += 1
def nargs(arglist):
    a = arglist.strip()
    return 0 if a in ("", "void") else a.count(",") + 1
# C header
h = open(os.path.join(root, "sdk", "c", "eccles_wasm_runtime.h")).read()
c = {}
for m in re.finditer(r'EW_IMPORT\("(\w+)"\)\s+int\s+\w+\(([^)]*)\);', h):
    c[m.group(1)] = nargs(m.group(2))
compare("C SDK", c)
# Rust crate
r = open(os.path.join(root, "sdk", "rust", "src", "lib.rs")).read()
blk = r[r.index("extern \"C\" {"):r.index("\n    }\n}", r.index("extern \"C\" {"))]
rs = {}
pending = None
for line in blk.split("\n"):
    ln = re.search(r'#\[link_name = "(\w+)"\]', line)
    if ln: pending = ln.group(1); continue
    m = re.search(r'pub fn (\w+)\(([^)]*)\) -> i32;', line)
    if m:
        rs[pending or m.group(1)] = nargs(m.group(2)); pending = None
compare("Rust SDK", rs)
print("SDK check: %d hardware imports in runtime, C SDK declares %d, Rust SDK declares %d, %d problem(s)" % (len(hw), len(c), len(rs), bad))
sys.exit(1 if bad else 0)
