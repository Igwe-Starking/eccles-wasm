Guest (wasm) sources of the eccles-wasm test suite.  Build everything:  ./build_all.sh  (needs clang with the wasm32 target, wasm-ld, python3), then python3 build_deliver.py -> deliver/wasm.h

module   source (build flags)                        sections / exports
hw       src/hw.c (+hw_body.c)                       t_info t_time t_system t_gpio t_pwm_irq t_adc_dac t_uart t_i2c t_spi
math     src/math.c (+math_body.c)                   t_core t_i32 t_i64 t_f32 t_f64
i32      src/eint.c  -DP_I32                         t_e_i32     (tables: edge_i32_gen.inc)
i64a     src/eint.c  -DP_I64A                        t_e_i64a    (edge_i64a_gen.inc)
i64b     src/eint.c  -DP_I64B                        t_e_i64b    (edge_i64b_gen.inc)
misc     src/eint.c  -DP_MISC                        t_e_crypto t_e_prime t_e_mem + trap_* (sha_gen.inc chacha_gen.inc mr_gen.inc)
f32      src/efloat.c -DP_F32                        t_e_f32     (edge_f32_gen.inc)
f64      src/efloat.c -DP_F64                        t_e_f64     (edge_f64_gen.inc)
conv     src/efloat.c -DP_CONV                       t_e_conv t_e_alg
trunc    gen_trunc.py (hand-assembled)               raw float->int opcodes
priv     gen_priv.py  (hand-assembled)               p_<import> wrappers, add, ping, poke, peek
atomx    gen_atomx.py (hand-assembled, + src/atomx_cases.inc)
atomns   gen_atomx.py (non-shared memory variant)
thr      src/thr.c (build_thr.sh)                    t_th_* thread sections
edge2    gen_edge2.py (51 hand-assembled modules, src/edge2.inc)
bench    src/bench.c + kern.inc (8 kernels);  bench2: src/bench2.c + kern2.inc (43 kernels)
Common header: src/common.h and rt/sdk/c/eccles_wasm_runtime.h.  Expected values come from the independent Python generators (gen_*.py).
