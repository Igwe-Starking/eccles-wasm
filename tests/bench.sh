#!/usr/bin/env bash
# usage: tests/bench.sh <wsrun-binary> ; prints best-of-3 wall ms for each benchmark
BIN=${1:-build/wsrun}
run() { name=$1; arg=$2; best=999999
  for i in 1 2 3; do s=$(date +%s%N); $BIN build/bench/$name.wasm run $arg >/tmp/bench.out; e=$(date +%s%N); ms=$(( (e-s)/1000000 )); [ $ms -lt $best ] && best=$ms; done
  printf "%-8s %6d ms   -> %s\n" $name $best "$(cut -c1-30 /tmp/bench.out)"; }
run fib 22        # fib(34) ~ 18M calls (hex arg)
run loop 5f5e100  # 100M iterations
run sieve 100000  # 1M-byte sieve
run mix64 2faf080 # 50M iterations
