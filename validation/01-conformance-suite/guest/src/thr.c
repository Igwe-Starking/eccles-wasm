/* thr.c - wasi-threads style guest: spawn sibling instances over ONE shared linear memory and hammer atomics.
 * Build: clang --target=wasm32 -matomics -mbulk-memory -Wl,--shared-memory,--import-memory? (see build_thr.sh)
 * The host installs the spawner (FreeRTOS tasks on the ESP32, pthreads on the PC). Threads never print. */
#include "common.h"
#define MAXTH 6
#define STK   2048
__attribute__((import_module("wasi"), import_name("thread-spawn"))) int thread_spawn(void *start_arg);

#ifdef EW_NATIVE_TEST
#endif
enum { J_COUNT32 = 1, J_COUNT64, J_LOCK, J_STACK, J_RING_PROD, J_RING_CONS, J_TRAP, J_WAITER, J_SETTER, J_TIME, J_MIX, J_DENY, J_NOP };
struct job { uint32_t kind, a, b, c; };
static struct job jobs[MAXTH];
static uint8_t  stacks[MAXTH][STK] __attribute__((aligned(16)));

/* shared state, every access either atomic or lock-protected */
static volatile uint32_t done_cnt, started_cnt, bad_cnt;
static volatile uint32_t c32;  static volatile uint64_t c64 __attribute__((aligned(8)));
static volatile uint32_t lockw; static volatile uint32_t plain_ctr;           /* plain_ctr only touched under lockw */
static volatile uint32_t ring[64]; static volatile uint32_t rhead, rtail;      /* SPSC ring */
static volatile uint32_t flag_w __attribute__((aligned(8))); static volatile uint32_t wres;
static volatile uint32_t tid_seen[MAXTH + 1];

#define ALD(p)        __atomic_load_n(&(p), __ATOMIC_SEQ_CST)
#define AST(p, v)     __atomic_store_n(&(p), (v), __ATOMIC_SEQ_CST)
#define AADD(p, v)    __atomic_fetch_add(&(p), (v), __ATOMIC_SEQ_CST)

static void lock(void) { uint32_t e, spins = 0; do { e = 0; if (++spins > 3000000u) { AADD(bad_cnt, 1); return; } } while (!__atomic_compare_exchange_n(&lockw, &e, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)); }
static void unlock(void) { __atomic_store_n(&lockw, 0, __ATOMIC_RELEASE); }

NOINL static uint32_t stack_job(uint32_t tid, uint32_t seed, uint32_t rounds) {     /* private stack must survive other threads running */
    volatile uint32_t buf[96]; uint32_t bad = 0, r, i;
    for (r = 0; r < rounds; r++) {
        for (i = 0; i < 96; i++) buf[i] = (seed * 2654435761u) ^ (i * 40503u) ^ (tid << 24) ^ r;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        for (i = 0; i < 96; i++) if (buf[i] != ((seed * 2654435761u) ^ (i * 40503u) ^ (tid << 24) ^ r)) bad++;
    }
    return bad;
}
NOINL static void divzero(uint32_t z) { volatile uint32_t x = 100; volatile uint32_t d = z; AST(bad_cnt, ALD(bad_cnt)); x = x / d; (void)x; }

NOINL static void body(uint32_t tid, uint32_t slot) {
    struct job j = jobs[slot]; uint32_t i;
    AST(tid_seen[slot], tid);
    AADD(started_cnt, 1);
    switch (j.kind) {
    case J_COUNT32: for (i = 0; i < j.a; i++) AADD(c32, 1); break;
    case J_COUNT64: for (i = 0; i < j.a; i++) __atomic_fetch_add(&c64, 0x100000001ull, __ATOMIC_SEQ_CST); break;
    case J_LOCK:    for (i = 0; i < j.a; i++) { lock(); plain_ctr = plain_ctr + 1; unlock(); } break;
    case J_STACK:   AADD(bad_cnt, stack_job(tid, j.b, j.a)); break;
    case J_RING_PROD: {
        uint32_t w = 0;
        for (i = 0; i < j.a; i++) {
            while (ALD(rhead) - ALD(rtail) >= 64) { ew_yield(); if (++w > 4000000u) { AADD(bad_cnt, 1); goto out_p; } }
            ring[ALD(rhead) & 63] = i * 7u + 3u; AADD(rhead, 1);
        }
        out_p: ; } break;
    case J_RING_CONS: {
        uint64_t sum = 0; uint32_t bad = 0, w = 0;
        for (i = 0; i < j.a; i++) {
            while (ALD(rhead) == ALD(rtail)) { ew_yield(); if (++w > 4000000u) { AADD(bad_cnt, 1); goto out_c; } }
            if (ring[ALD(rtail) & 63] != i * 7u + 3u) bad++;
            sum += ring[ALD(rtail) & 63]; AADD(rtail, 1);
        }
        out_c: AADD(c32, (uint32_t)sum); AADD(bad_cnt, bad); } break;
    case J_TRAP:    divzero(0); break;                       /* traps inside the sibling instance */
    case J_WAITER:  AST(wres, __builtin_wasm_memory_atomic_wait32((int *)&flag_w, 0, 2000000000LL) + 100u); break;   /* ns: 2 s cap */
    case J_SETTER:  ew_sleep(j.a); AST(flag_w, 1); __builtin_wasm_memory_atomic_notify((int *)&flag_w, 1); break;
    case J_TIME:    { uint32_t a = ew_micros(); ew_sleep(j.a); AADD(c32, (ew_micros() - a) / 1000u >= j.a - 1 ? 1u : 0u); } break;
    case J_MIX:     for (i = 0; i < j.a; i++) { AADD(c32, 3); __atomic_fetch_sub(&c32, 1, __ATOMIC_SEQ_CST); __atomic_fetch_xor(&c64, 1, __ATOMIC_SEQ_CST); __atomic_fetch_xor(&c64, 1, __ATOMIC_SEQ_CST); } break;
    case J_DENY:    { uint32_t v = 0; int st = ew_millis_(&v); AADD(c32, st == EW_DENIED ? 1u : 0u); AADD(c64, st == EW_OK ? 1u : 0u); } break;
    default: break;
    }
    AADD(done_cnt, 1);
}

/* wasi-threads entry: runs in the sibling instance, which has its OWN globals (so its own __stack_pointer copy) */
__attribute__((export_name("wasi_thread_start"), noinline)) void wasi_thread_start(int tid, int slot) {
    uint32_t sp = (uint32_t)(uintptr_t)&stacks[(uint32_t)slot][STK];
    __asm__ volatile("local.get %0\nglobal.set __stack_pointer" :: "r"(sp));
    body((uint32_t)tid, (uint32_t)slot);
}

static void reset_state(void) {
    done_cnt = started_cnt = bad_cnt = 0; c32 = 0; c64 = 0; lockw = 0; plain_ctr = 0; rhead = rtail = 0; flag_w = 0; wres = 0;
    for (int i = 0; i <= MAXTH; i++) tid_seen[i] = 0;
}
/* spawn nthreads jobs; returns number of threads actually spawned; waits (bounded) for all that started */
static int spawn_all(int n) {
    int ok = 0, i;
    for (i = 0; i < n; i++) { int tid = thread_spawn((void *)(uintptr_t)i); if (tid > 0) ok++; else break; }
    return ok;
}
static uint32_t nowms(void) { return ew_millis(); }
static int join(int n, uint32_t timeout_ms) {
    uint32_t t0 = nowms();
    while (ALD(done_cnt) < (uint32_t)n) { if (nowms() - t0 > timeout_ms) return 0; ew_sleep(1); }
    return 1;
}

/* ============================================================ test sections ============================================================ */
static void setjob(int slot, uint32_t kind, uint32_t a, uint32_t b) { jobs[slot].kind = kind; jobs[slot].a = a; jobs[slot].b = b; }
static int launch_x(int n, uint32_t timeout_ms, int expect_done, int strict) {
    int ok = spawn_all(n);
    int joined = join(expect_done < 0 ? ok : expect_done, timeout_ms);
    if (strict && ok != n) rep(K_FAIL, "spawn", "spawned %d of %d threads", ok, n);
    if (!joined) rep(K_FAIL, "join", "timeout after %u ms: done=%u started=%u", (unsigned)timeout_ms, (unsigned)ALD(done_cnt), (unsigned)ALD(started_cnt));
    return ok;
}
static int launch(int n, uint32_t timeout_ms, int expect_done) { return launch_x(n, timeout_ms, expect_done, 1); }
static void check_tids(int n, const char *what) {
    int i, j, bad = 0;
    for (i = 0; i < n; i++) { if (ALD(tid_seen[i]) == 0) bad++; for (j = i + 1; j < n; j++) if (ALD(tid_seen[i]) == ALD(tid_seen[j])) bad++; }
    chk_i32(what, bad, 0);
}

EW_EXPORT("t_th_basic") void t_th_basic(void) {
    int i;
    head("THREADS 1: shared memory, atomic counters (4 sibling instances running in parallel)");
    chk_i32("memory.size (pages) of the shared memory", WASM_MEM_SIZE(), 1);
    chk_i32("memory.grow(1) on shared memory with max == initial", WASM_MEM_GROW(1), -1);
    reset_state(); for (i = 0; i < 4; i++) setjob(i, J_COUNT32, 5000, 0);
    TIC(); launch(4, 20000, -1);
    rep(K_INFO, "timing", "4 threads x 5000 atomic i32 adds: %u us", (unsigned)TOC());
    chk_u32("i32 atomic add, 4 x 5000", ALD(c32), 20000u);
    chk_u32("threads started", ALD(started_cnt), 4u); chk_u32("threads finished", ALD(done_cnt), 4u);
    check_tids(4, "thread ids: nonzero and pairwise distinct (mismatches)");
    reset_state(); for (i = 0; i < 4; i++) setjob(i, J_COUNT64, 2500, 0);
    TIC(); launch(4, 20000, -1);
    rep(K_INFO, "timing", "4 threads x 2500 atomic i64 adds: %u us", (unsigned)TOC());
    chk_u64("i64 atomic add of 0x100000001, 4 x 2500", (uint64_t)ALD(c64), 10000ull * 0x100000001ull);
    reset_state(); for (i = 0; i < 4; i++) setjob(i, J_MIX, 1500, 0);
    launch(4, 20000, -1);
    chk_u32("mixed add/sub on i32 (net +2 per iteration), 4 x 1500", ALD(c32), 4u * 1500u * 2u);
    chk_u64("i64 xor applied an even number of times", (uint64_t)ALD(c64), 0ull);
}
EW_EXPORT("t_th_lock") void t_th_lock(void) {
    int i;
    head("THREADS 2: compare-exchange spin lock protecting a NON-atomic counter, and stack isolation");
    reset_state(); for (i = 0; i < 4; i++) setjob(i, J_LOCK, 1500, 0);
    TIC(); launch(4, 30000, -1);
    rep(K_INFO, "timing", "4 threads x 1500 locked plain increments: %u us", (unsigned)TOC());
    chk_u32("plain counter under cmpxchg lock, 4 x 1500 (lost update = broken mutual exclusion)", plain_ctr, 6000u);
    reset_state(); for (i = 0; i < 4; i++) setjob(i, J_STACK, 120, 0x1000u + (uint32_t)i * 977u);
    launch(4, 30000, -1);
    chk_u32("private-stack corruption words (4 threads x 120 rounds x 96 words)", ALD(bad_cnt), 0u);
}
EW_EXPORT("t_th_ring") void t_th_ring(void) {
    head("THREADS 3: single-producer / single-consumer ring buffer on two threads");
    reset_state(); setjob(0, J_RING_PROD, 2000, 0); setjob(1, J_RING_CONS, 2000, 0);
    TIC(); launch(2, 40000, -1);
    rep(K_INFO, "timing", "2000 items through a 64-slot ring: %u us", (unsigned)TOC());
    chk_u32("consumer: items out of order or lost", ALD(bad_cnt), 0u);
    chk_u32("consumer: sum of 7*i+3 for i<2000", ALD(c32), 13999000u);
}
EW_EXPORT("t_th_wait") void t_th_wait(void) {
    uint32_t t0, el; int r;
    head("THREADS 4: memory.atomic.wait / notify (polling implementation)");
    reset_state(); setjob(0, J_WAITER, 0, 0); setjob(1, J_SETTER, 40, 0);
    t0 = nowms(); launch(2, 8000, -1); el = nowms() - t0;
    chk_u32("waiter result code (100 + 0 = woken by the store)", ALD(wres), 100u);
    chk_bool("woken no earlier than the 40 ms the setter slept", el >= 39u, "elapsed >= 39 ms");
    chk_bool("woken long before the waiter's 2 s timeout", el < 1500u, "elapsed < 1500 ms");
    ew_printf("        measured %u ms\n", (unsigned)el);
    reset_state(); t0 = nowms();
    r = __builtin_wasm_memory_atomic_wait32((int *)&flag_w, 0, 5000000LL);      /* 5 ms, nobody wakes it */
    el = nowms() - t0;
    chk_i32("wait32 with nobody to wake it (2 = timed out)", r, 2);
    chk_bool("timeout not shorter than requested", el >= 4u, "elapsed >= 4 ms");
    ew_printf("        measured %u ms\n", (unsigned)el);
}
EW_EXPORT("t_th_trap") void t_th_trap(void) {
    int i;
    head("THREADS 5: one sibling traps (divide by zero) while others keep working");
    reset_state(); setjob(0, J_TRAP, 0, 0); setjob(1, J_COUNT32, 3000, 0); setjob(2, J_COUNT32, 3000, 0);
    launch(3, 20000, 2);
    for (i = 0; i < 400 && ALD(started_cnt) < 3; i++) ew_sleep(1);
    chk_u32("threads that started (trapping one included)", ALD(started_cnt), 3u);
    chk_u32("threads that finished normally", ALD(done_cnt), 2u);
    chk_u32("healthy siblings' atomic adds (2 x 3000)", ALD(c32), 6000u);
}
EW_EXPORT("t_th_time") void t_th_time(void) {
    int i; uint32_t t0, el;
    head("THREADS 6: three threads sleeping 25 ms in parallel, then 6 threads oversubscribing the cores");
    reset_state(); for (i = 0; i < 3; i++) setjob(i, J_TIME, 25, 0);
    t0 = nowms(); launch(3, 8000, -1); el = nowms() - t0;
    chk_u32("every thread measured >= 24 ms for sleep(25)", ALD(c32), 3u);
    chk_bool("3 parallel 25 ms sleeps took well under 3 x 25 ms x 4", el < 300u, "elapsed < 300 ms");
    ew_printf("        measured %u ms\n", (unsigned)el);
    reset_state(); for (i = 0; i < MAXTH; i++) setjob(i, J_COUNT32, 500, 0);
    launch(MAXTH, 30000, -1);
    chk_u32("6 concurrent threads x 500 atomic adds", ALD(c32), 3000u);
    chk_u32("6 threads finished", ALD(done_cnt), 6u);
}
/* the host limits the spawner to `allowed` threads; the guest asks for 8 and must see exactly that many succeed */
EW_EXPORT("t_th_limit") uint32_t t_th_limit(uint32_t allowed) {
    int ok, i;
    head("THREADS 7: spawner refuses after N threads (thread-spawn must return a negative value)");
    reset_state(); for (i = 0; i < MAXTH; i++) setjob(i, J_COUNT32, 100, 0);
    ok = launch_x(MAXTH, 20000, -1, 0);
    ew_printf("[INFO] spawner granted %d of %d requested threads\n", ok, MAXTH);
    chk_i32("threads granted", ok, (int32_t)allowed);
    chk_u32("their adds (granted x 100)", ALD(c32), allowed * 100u);
    return (uint32_t)ok;
}
/* with no PERM_THREADS (or no spawner) thread-spawn returns -1 and runs nothing */
EW_EXPORT("t_th_denied") int32_t t_th_denied(void) {
    int r;
    reset_state(); setjob(0, J_COUNT32, 10, 0);
    r = thread_spawn((void *)0);
    ew_printf("[INFO] thread-spawn returned %d\n", r);
    return r;
}

/* children call millis(): with PERM_TIME they get status OK, without it DENIED. Returns the number that were denied. */
EW_EXPORT("t_th_priv") uint32_t t_th_priv(uint32_t expect_denied) {
    int i;
    head("THREADS 8: sibling privilege (children inherit the parent's policy unless the host narrows it)");
    reset_state(); for (i = 0; i < 3; i++) setjob(i, J_DENY, 0, 0);
    launch(3, 20000, -1);
    chk_u32("children whose millis() was DENIED", ALD(c32), expect_denied);
    chk_u64("children whose millis() succeeded", (uint64_t)ALD(c64), 3u - expect_denied);
    return ALD(c32);
}
