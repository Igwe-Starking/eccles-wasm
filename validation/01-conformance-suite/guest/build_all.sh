#!/bin/bash
set -e
cd "$(dirname "$0")"
mkdir -p out deliver
rm -f out/*.wasm
./build.sh hw    hw.c
./build.sh math  math.c
./build.sh i32   eint.c   -DP_I32
./build.sh i64a  eint.c   -DP_I64A
./build.sh i64b  eint.c   -DP_I64B
./build.sh misc  eint.c   -DP_MISC
./build.sh f32   efloat.c -DP_F32
./build.sh f64   efloat.c -DP_F64
./build.sh conv  efloat.c -DP_CONV
./build.sh bench bench.c
./build_thr.sh
python3 gen_trunc.py >/dev/null
python3 gen_priv.py  >/dev/null
python3 gen_atomx.py >/dev/null
python3 gen_edge2.py >/dev/null
ls -la out/*.wasm | awk '{print $5, $9}'
