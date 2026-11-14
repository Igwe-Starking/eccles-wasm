/*
    ECCLES WASM - implementation
    Streaming loader + validating translator + interpreter + hardware backends.
    Layout of this file:
      1. state, arena, error handling      4. interpreter
      2. streaming reader                  5. public runtime API, file loading
      3. section loader + translator       6. hardware backends + import table
*/

#if (defined(__unix__) || defined(__APPLE__)) && !defined(ESP_PLATFORM) && !defined(ARDUINO) \
    && !defined(__ZEPHYR__) && !defined(_POSIX_C_SOURCE)
  #define _POSIX_C_SOURCE 200809L
#endif

#include "eccles_wasm.h"
#include <string.h>
#if !defined(ECCLES_WASM_NO_FLOAT)
  #include <math.h>      /* sqrt/sqrtf only */
#endif
#if ECCLES_WASM_USE_HEAP
  #include <stdlib.h>
#endif

#if defined(ECCLES_WASM_DEBUG)
  #ifndef ECCLES_WASM_LOG_LINE
    #include <stdio.h>
    #define ECCLES_WASM_LOG_LINE(msg) printf("[eccles_wasm] %s\n", (msg))
  #endif
#else
  #ifndef ECCLES_WASM_LOG_LINE
    #define ECCLES_WASM_LOG_LINE(msg) ((void)0)
  #endif
#endif

#if defined(__GNUC__) || defined(__clang__)
  #define W_ALIGN8 __attribute__((aligned(8)))
  #define W_UNLIKELY(x) __builtin_expect(!!(x), 0)
  #define W_MAYBE_UNUSED __attribute__((unused))
#else
  #define W_ALIGN8
  #define W_UNLIKELY(x) (x)
  #define W_MAYBE_UNUSED
#endif

#define W_NIL 0xFFFFFFFFu
#define W_PAGE 65536u

/* ===================================================================== *
 *  1. state
 * ===================================================================== */

/* ---- fused ("super") instructions ------------------------------------------------------------
   The translator folds the most common instruction sequences of compiled code into one handler:
     local.get a; local.get b; <i32 op>        -> FLL   (also FLC with a constant, FSC/FSL with one stack operand)
     <alu op>; local.set d                      -> FLLS / FLCS   (result goes straight to the local)
     <i32 compare>; br_if / if                  -> BSS / BLC / BLL (compare and branch in one dispatch)
     local.get a; i32.load                      -> OP_LLD32 / OP_LLD8U      local.get/i32.const; local.set -> OP_MOVE / OP_SETC
   They are produced and consumed only inside this file; nothing about them is visible in the module format. */
#define W_ALU_LIST(X) \
    X(ADD, 0x6A, a + b) X(SUB, 0x6B, a - b) X(MUL, 0x6C, a * b) X(AND, 0x71, a & b) X(OR, 0x72, a | b) \
    X(XOR, 0x73, a ^ b) X(SHL, 0x74, a << (b & 31)) X(SHRS, 0x75, (uint32_t)((int32_t)a >> (b & 31))) \
    X(SHRU, 0x76, a >> (b & 31))
#define W_CMP_LIST(X) \
    X(EQ, 0x46, a == b) X(NE, 0x47, a != b) X(LTS, 0x48, (int32_t)a < (int32_t)b) X(LTU, 0x49, a < b) \
    X(GTS, 0x4A, (int32_t)a > (int32_t)b) X(GTU, 0x4B, a > b) X(LES, 0x4C, (int32_t)a <= (int32_t)b) \
    X(LEU, 0x4D, a <= b) X(GES, 0x4E, (int32_t)a >= (int32_t)b) X(GEU, 0x4F, a >= b)
#define W_FUSE_LIST(X) W_ALU_LIST(X) W_CMP_LIST(X)
#define W_NALU 9

enum {
    W_FUSED_BASE = 0x200,
#define X(n, o, e) FLL_##n, FLC_##n, FSC_##n, FSL_##n,
    W_FUSE_LIST(X)
#undef X
#define X(n, o, e) FLLS_##n, FLCS_##n,
    W_ALU_LIST(X)
#undef X
#define X(n, o, e) BSS_##n, BLC_##n, BLL_##n,
    W_CMP_LIST(X)
#undef X
    OP_MOVE, OP_SETC, OP_LLD32, OP_LLD8U,
    W_FUSED_END
};
static int w_fuse_idx(uint32_t op) {                 /* i32 ALU/compare opcode -> index in W_FUSE_LIST, or -1 */
    int i = 0;
#define X(n, o, e) if (op == (o)) return i; i++;
    W_FUSE_LIST(X)
#undef X
    return -1;
}

typedef struct {
    const uint16_t   *code;     /* translated body; NULL for host functions */
    eccles_wasm_host_fn host;
    uint16_t type;              /* canonical type id */
    uint16_t nparams;
    uint16_t nlocals;           /* locals beyond the parameters */
    uint16_t max_h;             /* peak slots (params + locals + operands) */
    uint8_t  nresults;
} w_func_t;

typedef struct { const char *name; uint16_t func; } w_export_t;
typedef struct { const uint16_t *pc; uint64_t *fp; const w_func_t *f; } w_frame_t;
typedef struct { const char *name; eccles_wasm_host_fn fn; const char *sig; } w_host_t;

typedef struct { uint32_t *elems; uint32_t size; } w_table_t;      /* elements: 0 = null, else function index + 1 (funcref) */
typedef struct { const uint32_t *items; uint32_t len; } w_eseg_t;    /* passive element segment (len 0 = dropped) */

/* translator state (load time only) */
typedef struct { uint32_t patch, loop_pc, else_slot; uint16_t base; uint8_t np, nr, kind, reach; } w_ctl_t;
enum { K_FUNC, K_BLOCK, K_LOOP, K_IF, K_ELSE };
typedef struct {
    uint16_t *code;
    uint32_t n, cap, h, maxh, dead_depth;
    uint16_t nloc;
    uint8_t  nctl, dead;
    w_ctl_t  ctl[ECCLES_WASM_MAX_CTL];
    /* peephole window: the last (up to) two operand-producing instructions, and the last instruction */
    uint32_t hpos[2], hend[2], hval[2]; uint8_t hkind[2], nh;
    uint32_t lpos, lend; uint8_t lkind, lidx;
} w_trans_t;
enum { HK_LGET = 1, HK_ICONST = 2, LK_CMP_SS = 1, LK_CMP_LC = 2, LK_CMP_LL = 3, LK_ALU_LC = 4, LK_ALU_LL = 5 };

/* streaming reader (load time only) */
typedef struct {
    eccles_wasm_read_fn fn;
    void *ctx;
    const uint8_t *p, *e, *start;
    uint32_t pos0;
    uint8_t  mem_src, eof;
    uint8_t  chunk[ECCLES_WASM_CHUNK_SIZE];
} w_reader_t;

/* One complete, independent instance. Lives at the start of its own arena. */
struct eccles_wasm_vm_s {
    /* arena */
    uint8_t *base, *end, *lo, *hi, *hi_init;
    uint8_t  inited, loaded, running;
    void    *heap_block;            /* non-NULL: free() this on destroy */
    void    *user;
    /* diagnostics */
    eccles_wasm_error_t err;
    uint32_t err_info;
    char     err_str[ECCLES_WASM_MAX_NAME + 1];
    /* module */
    uint16_t ntypes, nfuncs, nimports, nglobals, nexports, start;
    uint8_t  has_mem, code_seen;
    uint8_t *types;                 /* concatenated records: np, nr, params.., results.. */
    uint16_t *type_off, *type_canon;
    w_func_t *funcs;
    uint64_t *globals;
    w_table_t *tables; uint16_t ntables;
    w_eseg_t *eseg; uint16_t neseg;
    uint16_t nglobals_imp;
    struct { const char *name; uint64_t v; } hglob[ECCLES_WASM_MAX_HOST_GLOBALS]; uint8_t nhglob;
    w_export_t *exports;
    uint8_t *mem;
    uint32_t mem_bytes, mem_pages, code_bytes;
    struct w_seg { const uint8_t *p; uint32_t len; } *segs;   /* passive data segments (len 0 = dropped) */
    uint16_t nseg;
    /* runtime */
    uint64_t *stk;
    w_frame_t *frames;
    uint32_t fuel_budget;
    uint8_t  exit_req;
    int32_t  exit_code;
    uint32_t wasi_last; uint64_t wasi_acc;
    /* per-instance host functions */
    w_host_t host[ECCLES_WASM_MAX_HOST_FUNCS];
    uint8_t  nhost;
    /* load-time scratch */
    eccles_wasm_fs_read_fn fs_rd; void *fs_h;
    w_reader_t rd;
    w_trans_t  tr;
};

/* chip-level state shared by every instance (set once at start-up) */
static eccles_wasm_hal_t g_hal_user;
static eccles_wasm_fs_open_fn  g_fs_open;
static eccles_wasm_fs_read_fn  g_fs_read;
static eccles_wasm_fs_close_fn g_fs_close;

#if !ECCLES_WASM_USE_HEAP
static uint8_t g_arena[ECCLES_WASM_ARENA_SIZE] W_ALIGN8;
#endif

static void w_fail(eccles_wasm_vm_t *vm, eccles_wasm_error_t e, uint32_t info) {
    if (vm->err == ECCLES_WASM_OK) { vm->err = e; vm->err_info = info; }
}

/* ---- arena: bump from both ends ------------------------------------- */
static void *w_alo(eccles_wasm_vm_t *vm, size_t size, size_t align) {
    uintptr_t p = ((uintptr_t)vm->lo + (align - 1)) & ~(uintptr_t)(align - 1);
    if (vm->err != ECCLES_WASM_OK) return NULL;
    if (p + size > (uintptr_t)vm->hi || p + size < p) {
        w_fail(vm, ECCLES_WASM_ERR_NO_MEMORY, (uint32_t)(p + size - (uintptr_t)vm->lo)); return NULL;
    }
    vm->lo = (uint8_t *)(p + size);
    return (void *)p;
}
static void *w_ahi(eccles_wasm_vm_t *vm, size_t size, size_t align) {
    uintptr_t p;
    if (vm->err != ECCLES_WASM_OK) return NULL;
    if (size > (size_t)(vm->hi - vm->lo)) { w_fail(vm, ECCLES_WASM_ERR_NO_MEMORY, (uint32_t)size); return NULL; }
    p = ((uintptr_t)vm->hi - size) & ~(uintptr_t)(align - 1);
    if (p < (uintptr_t)vm->lo) { w_fail(vm, ECCLES_WASM_ERR_NO_MEMORY, (uint32_t)size); return NULL; }
    vm->hi = (uint8_t *)p;
    return (void *)p;
}

/* ===================================================================== *
 *  2. streaming reader
 * ===================================================================== */


static uint32_t rd_pos(eccles_wasm_vm_t *vm) { return vm->rd.pos0 + (uint32_t)(vm->rd.p - vm->rd.start); }

static int rd_fill(eccles_wasm_vm_t *vm) {
    int n;
    if (vm->rd.mem_src || !vm->rd.fn || vm->rd.eof) return 0;
    vm->rd.pos0 += (uint32_t)(vm->rd.e - vm->rd.start);
    n = vm->rd.fn(vm->rd.ctx, vm->rd.chunk, sizeof vm->rd.chunk);
    if (n < 0 || n > (int)sizeof vm->rd.chunk) { w_fail(vm, ECCLES_WASM_ERR_IO, rd_pos(vm)); n = 0; }
    if (n == 0) vm->rd.eof = 1;
    vm->rd.start = vm->rd.p = vm->rd.chunk; vm->rd.e = vm->rd.chunk + n;
    return n > 0;
}
static int rd_u8(eccles_wasm_vm_t *vm) {
    if (vm->rd.p >= vm->rd.e && !rd_fill(vm)) { w_fail(vm, ECCLES_WASM_ERR_MALFORMED, rd_pos(vm)); return 0; }
    return *vm->rd.p++;
}
static uint32_t rd_u32(eccles_wasm_vm_t *vm) {
    uint32_t r = 0; unsigned s = 0; int b;
    for (;;) {
        b = rd_u8(vm);
        r |= (uint32_t)(b & 0x7F) << s;
        if (!(b & 0x80)) break;
        s += 7;
        if (s > 28 || vm->err) { w_fail(vm, ECCLES_WASM_ERR_MALFORMED, rd_pos(vm)); return 0; }
    }
    if (s == 28 && (b & 0x70)) w_fail(vm, ECCLES_WASM_ERR_MALFORMED, rd_pos(vm));
    return r;
}
static int64_t rd_s64(eccles_wasm_vm_t *vm) {
    uint64_t r = 0; unsigned s = 0; int b;
    do {
        b = rd_u8(vm);
        r |= (uint64_t)(b & 0x7F) << s;
        s += 7;
    } while ((b & 0x80) && s < 70 && !vm->err);
    if (b & 0x80) w_fail(vm, ECCLES_WASM_ERR_MALFORMED, rd_pos(vm));
    if (s < 64 && (b & 0x40)) r |= ~(uint64_t)0 << s;
    return (int64_t)r;
}
static int32_t rd_s32(eccles_wasm_vm_t *vm) {
    int64_t v = rd_s64(vm);
    if (v < -2147483648LL || v > 2147483647LL) { w_fail(vm, ECCLES_WASM_ERR_MALFORMED, rd_pos(vm)); return 0; }
    return (int32_t)v;
}
#if !defined(ECCLES_WASM_NO_FLOAT)
static uint64_t rd_le(eccles_wasm_vm_t *vm, unsigned n) {
    uint64_t v = 0; unsigned i;
    for (i = 0; i < n; i++) v |= (uint64_t)rd_u8(vm) << (8 * i);
    return v;
}
#endif
static void rd_skip(eccles_wasm_vm_t *vm, uint32_t n) {
    while (n && !vm->err) {
        uint32_t avail = (uint32_t)(vm->rd.e - vm->rd.p), t;
        if (!avail) { if (!rd_fill(vm)) { w_fail(vm, ECCLES_WASM_ERR_MALFORMED, rd_pos(vm)); return; } continue; }
        t = n < avail ? n : avail; vm->rd.p += t; n -= t;
    }
}
static void rd_bytes(eccles_wasm_vm_t *vm, uint8_t *dst, uint32_t n) {
    while (n && !vm->err) {
        uint32_t avail = (uint32_t)(vm->rd.e - vm->rd.p), t;
        if (!avail) { if (!rd_fill(vm)) { w_fail(vm, ECCLES_WASM_ERR_MALFORMED, rd_pos(vm)); return; } continue; }
        t = n < avail ? n : avail; memcpy(dst, vm->rd.p, t); dst += t; vm->rd.p += t; n -= t;
    }
}
/* read a name; copy at most cap-1 bytes (NUL-terminated) into dst (may be NULL) */
static uint32_t rd_name(eccles_wasm_vm_t *vm, char *dst, uint32_t cap) {
    uint32_t len = rd_u32(vm), keep = 0;
    if (dst && cap) { keep = len < cap - 1 ? len : cap - 1; rd_bytes(vm, (uint8_t *)dst, keep); dst[keep] = 0; }
    rd_skip(vm, len - keep);
    return len;
}

/* ===================================================================== *
 *  3. module loader + translator
 * ===================================================================== */

/* internal opcodes (wasm opcodes 0x00..0xC4 are used as-is) */
enum {
    OP_SAT0 = 0x100,                       /* 0x100..0x107 trunc_sat */
    OP_MEMCOPY = 0x10A, OP_MEMFILL = 0x10B, OP_MEMINIT = 0x10C, OP_DATADROP = 0x10D, OP_TABINIT = 0x10E, OP_ELEMDROP = 0x10F,
    OP_TABCOPY = 0x120, OP_TABGROW, OP_TABSIZE, OP_TABFILL, OP_RETCALL, OP_RETCALL_IND,
    OP_BR = 0x110, OP_BR_FIX, OP_BR_IF, OP_BR_IF_FIX, OP_BR_TABLE, OP_JZ
};

static int w_find_host(eccles_wasm_vm_t *vm, const char *name, eccles_wasm_host_fn *fn, const char **sig);
#if defined(ECCLES_WASM_LENIENT_IMPORTS)
static eccles_wasm_status_t w_trap_stub(eccles_wasm_vm_t *vm, uint64_t *s) { (void)vm; (void)s; return ECCLES_WASM_FAILED; }
#endif

static const uint8_t *w_rec(eccles_wasm_vm_t *vm, uint32_t type_index) { return vm->types + vm->type_off[type_index]; }

static int w_valtype_ok(int t) {
    if (t == 0x7F || t == 0x7E || t == 0x70 || t == 0x6F) return 1;       /* i32 i64 funcref externref */
#if !defined(ECCLES_WASM_NO_FLOAT)
    if (t == 0x7D || t == 0x7C) return 1;
#endif
    return 0;
}
static int w_vt(eccles_wasm_vm_t *vm) {                      /* read + validate a value type */
    int t = rd_u8(vm);
    if (!vm->err && !w_valtype_ok(t)) { w_fail(vm, ECCLES_WASM_ERR_UNSUPPORTED_FEATURE, (uint32_t)t); return 0; }
    return t;
}

/* "i(ii)" -> type record [np, nr, params..., results...]; returns length or 0 */
static uint32_t w_sig(const char *s, uint8_t *out, uint32_t cap) {
    uint32_t np = 0, k;
    static const char L[] = "iIfF"; static const uint8_t V[] = {0x7F, 0x7E, 0x7D, 0x7C};
    const char *p, *r = s;
    if (!s || !*s || s[1] != '(') return 0;
    for (p = s + 2; *p && *p != ')'; p++, np++) {
        for (k = 0; k < 4 && L[k] != *p; k++) {}
        if (k == 4 || 2 + np + 1 >= cap) return 0;
        out[2 + np] = V[k];
    }
    if (*p != ')' || p[1] != 0) return 0;
    out[0] = (uint8_t)np; out[1] = 0;
    if (*r != 'v') {
        for (k = 0; k < 4 && L[k] != *r; k++) {}
        if (k == 4) return 0;
        out[2 + np] = V[k]; out[1] = 1;
    }
    return 2 + np + out[1];
}


static void t_bad(eccles_wasm_vm_t *vm) { w_fail(vm, ECCLES_WASM_ERR_MALFORMED, rd_pos(vm)); }
static void t_emit(eccles_wasm_vm_t *vm, uint32_t w) {
    if (vm->tr.n >= vm->tr.cap) { w_fail(vm, ECCLES_WASM_ERR_NO_MEMORY, (uint32_t)(((uintptr_t)(vm->tr.code + vm->tr.n) - (uintptr_t)vm->lo) + 2)); return; }
    vm->tr.code[vm->tr.n++] = (uint16_t)w;
}
static void t_emit32(eccles_wasm_vm_t *vm, uint32_t v) { t_emit(vm, v & 0xFFFFu); t_emit(vm, v >> 16); }
static uint32_t t_get32(eccles_wasm_vm_t *vm, uint32_t at) { return (uint32_t)vm->tr.code[at] | ((uint32_t)vm->tr.code[at + 1] << 16); }
static void t_put32(eccles_wasm_vm_t *vm, uint32_t at, uint32_t v) { vm->tr.code[at] = (uint16_t)v; vm->tr.code[at + 1] = (uint16_t)(v >> 16); }
static void t_pop(eccles_wasm_vm_t *vm, uint32_t k) {
    if (vm->tr.nctl == 0 || vm->tr.h < (uint32_t)vm->tr.ctl[vm->tr.nctl - 1].base + k) { t_bad(vm); return; }
    vm->tr.h -= k;
}
static void t_push(eccles_wasm_vm_t *vm, uint32_t k) {
    vm->tr.h += k;
    if (vm->tr.h > vm->tr.maxh) vm->tr.maxh = vm->tr.h;
    if (vm->tr.h > ECCLES_WASM_VALUE_STACK_SLOTS) w_fail(vm, ECCLES_WASM_ERR_LIMIT, vm->tr.h);
}
static void t_resolve(eccles_wasm_vm_t *vm, uint32_t head, uint32_t target) {
    while (head != W_NIL) {
        uint32_t nxt = t_get32(vm, head);
        t_put32(vm, head, target - head);            /* two's complement relative offset */
        head = nxt;
    }
}
/* emit the 32-bit jump slot for a branch to frame f */
static void t_target(eccles_wasm_vm_t *vm, w_ctl_t *f) {
    uint32_t s = vm->tr.n;
    if (f->kind == K_LOOP) t_emit32(vm, f->loop_pc - s);
    else { t_emit32(vm, f->patch); f->patch = s; }
}
static int t_fuse_branch(eccles_wasm_vm_t *vm, int invert);
static void t_branch(eccles_wasm_vm_t *vm, uint32_t depth, int cond) {
    w_ctl_t *f; uint32_t keep; int fix;
    if (depth >= vm->tr.nctl) { t_bad(vm); return; }
    f = &vm->tr.ctl[vm->tr.nctl - 1 - depth];
    keep = (f->kind == K_LOOP) ? f->np : f->nr;
    if (vm->tr.h < (uint32_t)f->base + keep) { t_bad(vm); return; }
    fix = (vm->tr.h != (uint32_t)f->base + keep);
    if (cond && !fix && t_fuse_branch(vm, 0)) { t_target(vm, f); return; }
    t_emit(vm, (cond ? OP_BR_IF : OP_BR) + (uint32_t)fix);
    t_target(vm, f);
    if (fix) { t_emit(vm, keep); t_emit(vm, f->base); }
}
static int t_blocktype(eccles_wasm_vm_t *vm, uint8_t *np, uint8_t *nr) {
    int64_t v = rd_s64(vm);
    if (vm->err) return 0;
    if (v == -64) { *np = 0; *nr = 0; return 1; }
    if (v == -16 || v == -17) { *np = 0; *nr = 1; return 1; }                      /* funcref / externref result */
    if (v >= -4 && v <= -1) {
        if (!w_valtype_ok((int)(v + 0x80))) { w_fail(vm, ECCLES_WASM_ERR_UNSUPPORTED_FEATURE, (uint32_t)(v + 0x80)); return 0; }
        *np = 0; *nr = 1; return 1;
    }
    if (v >= 0 && v < vm->ntypes) { const uint8_t *r = w_rec(vm, (uint32_t)v); *np = r[0]; *nr = r[1]; return 1; }
    w_fail(vm, v < 0 ? ECCLES_WASM_ERR_UNSUPPORTED_FEATURE : ECCLES_WASM_ERR_MALFORMED, rd_pos(vm));
    return 0;
}

#if defined(ECCLES_WASM_NO_FLOAT)
static int t_is_float(uint32_t op) {
    return op == 0x2A || op == 0x2B || op == 0x38 || op == 0x39 || op == 0x43 || op == 0x44 ||
           (op >= 0x5B && op <= 0x66) || (op >= 0x8B && op <= 0xA6) ||
           (op >= 0xA8 && op <= 0xAB) || (op >= 0xAE && op <= 0xBF);
}
#endif
static int t_unary(uint32_t op) {
    return op == 0x45 || op == 0x50 || (op >= 0x67 && op <= 0x69) || (op >= 0x79 && op <= 0x7B) ||
           (op >= 0x8B && op <= 0x91) || (op >= 0x99 && op <= 0x9F) || op >= 0xA7;
}


/* ---- peephole fusion (see the fused-instruction notes at the top of the file) ---- */
static void t_barrier(eccles_wasm_vm_t *vm) { vm->tr.nh = 0; vm->tr.lkind = 0; }
static void t_hist_push(eccles_wasm_vm_t *vm, uint8_t kind, uint32_t val, uint32_t pos) {
    w_trans_t *t = &vm->tr;
    if (t->nh >= 1 && t->hend[t->nh - 1] == pos) {                 /* contiguous with the previous one */
        if (t->nh == 2) { t->hpos[0] = t->hpos[1]; t->hend[0] = t->hend[1]; t->hkind[0] = t->hkind[1]; t->hval[0] = t->hval[1]; }
        else t->nh = 2;
        t->hpos[1] = pos; t->hend[1] = t->n; t->hkind[1] = kind; t->hval[1] = val;
    } else { t->nh = 1; t->hpos[0] = pos; t->hend[0] = t->n; t->hkind[0] = kind; t->hval[0] = val; }
    t->lkind = 0;
}
static int t_hist_top(const w_trans_t *t, uint8_t kind) { return t->nh >= 1 && t->hend[t->nh - 1] == t->n && t->hkind[t->nh - 1] == kind; }

static void t_emit_bin(eccles_wasm_vm_t *vm, uint32_t op, int idx) {     /* an i32 ALU/compare op, fused if possible */
    w_trans_t *t = &vm->tr; int cmp = idx >= W_NALU; uint32_t pos = t->n;
    if (t->nh == 2 && t->hend[1] == t->n && t->hend[0] == t->hpos[1]) {
        if (t->hkind[0] == HK_LGET && t->hkind[1] == HK_LGET) {
            uint32_t a = t->hval[0], b = t->hval[1];
            t->n = pos = t->hpos[0];
            t_emit(vm, (uint32_t)FLL_ADD + (uint32_t)idx * 4u); t_emit(vm, a); t_emit(vm, b);
            t->lpos = pos; t->lend = t->n; t->lkind = cmp ? LK_CMP_LL : LK_ALU_LL; t->lidx = (uint8_t)idx; t->nh = 0; return;
        }
        if (t->hkind[0] == HK_LGET && t->hkind[1] == HK_ICONST) {
            uint32_t a = t->hval[0], c = t->hval[1];
            t->n = pos = t->hpos[0];
            t_emit(vm, (uint32_t)FLC_ADD + (uint32_t)idx * 4u); t_emit(vm, a); t_emit32(vm, c);
            t->lpos = pos; t->lend = t->n; t->lkind = cmp ? LK_CMP_LC : LK_ALU_LC; t->lidx = (uint8_t)idx; t->nh = 0; return;
        }
    }
    if (t_hist_top(t, HK_ICONST)) {
        uint32_t c = t->hval[t->nh - 1]; t->n = t->hpos[t->nh - 1];
        t_emit(vm, (uint32_t)FSC_ADD + (uint32_t)idx * 4u); t_emit32(vm, c); t->lkind = 0; t->nh = 0; return;
    }
    if (t_hist_top(t, HK_LGET)) {
        uint32_t a = t->hval[t->nh - 1]; t->n = t->hpos[t->nh - 1];
        t_emit(vm, (uint32_t)FSL_ADD + (uint32_t)idx * 4u); t_emit(vm, a); t->lkind = 0; t->nh = 0; return;
    }
    t_emit(vm, op);
    t->lpos = pos; t->lend = t->n; t->lkind = cmp ? LK_CMP_SS : 0; t->lidx = (uint8_t)idx; t->nh = 0;
}

/* compare (+ optionally inverted) folded into the conditional branch that follows it */
static int t_fuse_branch(eccles_wasm_vm_t *vm, int invert) {
    static const uint8_t inv[10] = {1, 0, 8, 9, 6, 7, 4, 5, 2, 3};
    w_trans_t *t = &vm->tr; uint32_t pos, a = 0, c0 = 0, c1 = 0; int ci, variant;
    if (t->lkind < LK_CMP_SS || t->lkind > LK_CMP_LL || t->lend != t->n) return 0;
    ci = t->lidx - W_NALU; if (invert) ci = inv[ci];
    pos = t->lpos;
    if (t->lkind == LK_CMP_LL) { a = t->code[pos + 1]; c0 = t->code[pos + 2]; variant = 2; }
    else if (t->lkind == LK_CMP_LC) { a = t->code[pos + 1]; c0 = t->code[pos + 2]; c1 = t->code[pos + 3]; variant = 1; }
    else variant = 0;
    t->n = pos;
    t_emit(vm, (uint32_t)BSS_EQ + (uint32_t)ci * 3u + (uint32_t)variant);
    if (variant == 2) { t_emit(vm, a); t_emit(vm, c0); }
    else if (variant == 1) { t_emit(vm, a); t_emit(vm, c0); t_emit(vm, c1); }
    t->lkind = 0; t->nh = 0;
    return 1;
}

/* local.set: fold with the producer if we can */
static void t_emit_set(eccles_wasm_vm_t *vm, uint32_t dst) {
    w_trans_t *t = &vm->tr; uint32_t pos;
    if (t->lend == t->n && (t->lkind == LK_ALU_LL || t->lkind == LK_ALU_LC)) {
        uint32_t a = t->code[t->lpos + 1], c0 = t->code[t->lpos + 2], c1 = t->code[t->lpos + 3];
        int ll = t->lkind == LK_ALU_LL; uint32_t idx = t->lidx;
        t->n = t->lpos;
        t_emit(vm, (uint32_t)FLLS_ADD + idx * 2u + (ll ? 0u : 1u)); t_emit(vm, a); t_emit(vm, c0);
        if (!ll) t_emit(vm, c1);
        t_emit(vm, dst); t->lkind = 0; t->nh = 0; return;
    }
    if (t_hist_top(t, HK_LGET)) {
        uint32_t a = t->hval[t->nh - 1]; t->n = t->hpos[t->nh - 1];
        t_emit(vm, OP_MOVE); t_emit(vm, a); t_emit(vm, dst); t->lkind = 0; t->nh = 0; return;
    }
    if (t_hist_top(t, HK_ICONST)) {
        uint32_t c = t->hval[t->nh - 1]; t->n = t->hpos[t->nh - 1];
        t_emit(vm, OP_SETC); t_emit(vm, dst); t_emit32(vm, c); t->lkind = 0; t->nh = 0; return;
    }
    pos = t->n; (void)pos;
    t_emit(vm, 0x21); t_emit(vm, dst); t->lkind = 0; t->nh = 0;
}

static void w_translate(eccles_wasm_vm_t *vm, uint32_t fi, uint32_t body_end) {
    w_func_t *F = &vm->funcs[fi];
    uint32_t nl, i, extra = 0;
    uintptr_t p;
    int done = 0;

    nl = rd_u32(vm);
    for (i = 0; i < nl && !vm->err; i++) {
        uint32_t c = rd_u32(vm);
        (void)w_vt(vm);
        extra += c;
        if (c > 0xFFFFu || extra > 0xFFFFu - F->nparams) { w_fail(vm, ECCLES_WASM_ERR_LIMIT, extra); return; }
    }
    if (vm->err) return;

    p = ((uintptr_t)vm->lo + 1) & ~(uintptr_t)1;
    vm->tr.code = (uint16_t *)p;
    vm->tr.cap = p < (uintptr_t)vm->hi ? (uint32_t)(((uintptr_t)vm->hi - p) / 2) : 0;
    vm->tr.n = 0; vm->tr.h = vm->tr.maxh = (uint32_t)F->nparams + extra; vm->tr.nloc = (uint16_t)vm->tr.h;
    vm->tr.nctl = 1; vm->tr.dead = 0; vm->tr.dead_depth = 0; vm->tr.nh = 0; vm->tr.lkind = 0;
    if (vm->tr.h > ECCLES_WASM_VALUE_STACK_SLOTS) { w_fail(vm, ECCLES_WASM_ERR_LIMIT, vm->tr.h); return; }
    vm->tr.ctl[0].patch = W_NIL; vm->tr.ctl[0].loop_pc = 0; vm->tr.ctl[0].else_slot = W_NIL;
    vm->tr.ctl[0].base = (uint16_t)vm->tr.h; vm->tr.ctl[0].np = 0; vm->tr.ctl[0].nr = F->nresults;
    vm->tr.ctl[0].kind = K_FUNC; vm->tr.ctl[0].reach = 0;

    while (!done) {
        uint32_t op;
        if (vm->err) return;
        if (rd_pos(vm) > body_end) { t_bad(vm); return; }
        op = (uint32_t)rd_u8(vm);
        if (vm->err) return;
        switch (op) {
        case 0x00: if (!vm->tr.dead) { t_emit(vm, 0x00); vm->tr.dead = 1; vm->tr.dead_depth = 0; } break;
        case 0x01: break;
        case 0x02: case 0x03: case 0x04: {
            uint8_t np, nr; w_ctl_t *c;
            if (!t_blocktype(vm, &np, &nr)) break;
            if (vm->tr.dead) { vm->tr.dead_depth++; break; }
            if (op != 0x04) t_barrier(vm);
            if (op == 0x04) t_pop(vm, 1);
            if (vm->err) return;
            if (vm->tr.nctl >= ECCLES_WASM_MAX_CTL) { w_fail(vm, ECCLES_WASM_ERR_LIMIT, vm->tr.nctl); return; }
            if (vm->tr.h < (uint32_t)vm->tr.ctl[vm->tr.nctl - 1].base + np) { t_bad(vm); return; }
            c = &vm->tr.ctl[vm->tr.nctl++];
            c->patch = W_NIL; c->else_slot = W_NIL; c->np = np; c->nr = nr; c->reach = 0;
            c->kind = (uint8_t)(op == 0x02 ? K_BLOCK : op == 0x03 ? K_LOOP : K_IF);
            c->base = (uint16_t)(vm->tr.h - np);
            c->loop_pc = vm->tr.n;
            if (op == 0x04) {
                if (!t_fuse_branch(vm, 1)) t_emit(vm, OP_JZ);
                c->else_slot = vm->tr.n; t_emit32(vm, 0); t_barrier(vm);
            }
            break; }
        case 0x05: {                                              /* else */
            w_ctl_t *f;
            if (vm->tr.dead && vm->tr.dead_depth) break;
            t_barrier(vm);
            f = &vm->tr.ctl[vm->tr.nctl - 1];
            if (f->kind != K_IF) { t_bad(vm); return; }
            if (!vm->tr.dead) {
                if (vm->tr.h != (uint32_t)f->base + f->nr) { t_bad(vm); return; }
                t_emit(vm, OP_BR); t_target(vm, f); f->reach = 1;
            }
            t_put32(vm, f->else_slot, vm->tr.n - f->else_slot);
            f->kind = K_ELSE; vm->tr.h = (uint32_t)f->base + f->np; vm->tr.dead = 0;
            break; }
        case 0x0B: {                                              /* end */
            w_ctl_t *f;
            if (vm->tr.dead && vm->tr.dead_depth) { vm->tr.dead_depth--; break; }
            t_barrier(vm);
            f = &vm->tr.ctl[vm->tr.nctl - 1];
            if (!vm->tr.dead) {
                if (vm->tr.h != (uint32_t)f->base + f->nr) { t_bad(vm); return; }
                f->reach = 1;
            }
            if (f->kind == K_IF) {
                if (f->np != f->nr) { t_bad(vm); return; }
                t_put32(vm, f->else_slot, vm->tr.n - f->else_slot); f->reach = 1;
            }
            if (f->kind != K_LOOP) {
                if (f->patch != W_NIL) f->reach = 1;
                t_resolve(vm, f->patch, vm->tr.n);
            }
            if (f->kind == K_FUNC) { t_emit(vm, 0x0F); vm->tr.nctl = 0; done = 1; break; }
            vm->tr.h = (uint32_t)f->base + f->nr; if (vm->tr.h > vm->tr.maxh) vm->tr.maxh = vm->tr.h;
            vm->tr.dead = !f->reach; vm->tr.dead_depth = 0; vm->tr.nctl--;
            break; }
        case 0x0C: { uint32_t d = rd_u32(vm); if (vm->tr.dead) break; t_branch(vm, d, 0); vm->tr.dead = 1; vm->tr.dead_depth = 0; break; }
        case 0x0D: { uint32_t d = rd_u32(vm); if (vm->tr.dead) break; t_pop(vm, 1); if (!vm->err) t_branch(vm, d, 1); break; }
        case 0x0E: {                                              /* br_table */
            uint32_t n = rd_u32(vm), k;
            if (vm->err) return;
            if (n >= 0xFFFFu) { w_fail(vm, ECCLES_WASM_ERR_LIMIT, n); return; }
            if (!vm->tr.dead) { t_pop(vm, 1); t_emit(vm, OP_BR_TABLE); t_emit(vm, n); }
            for (k = 0; k <= n && !vm->err; k++) {
                uint32_t d = rd_u32(vm);
                if (vm->tr.dead || vm->err) continue;
                if (d >= vm->tr.nctl) { t_bad(vm); return; }
                {
                    w_ctl_t *f = &vm->tr.ctl[vm->tr.nctl - 1 - d];
                    uint32_t keep = (f->kind == K_LOOP) ? f->np : f->nr;
                    if (vm->tr.h < (uint32_t)f->base + keep) { t_bad(vm); return; }
                    t_target(vm, f); t_emit(vm, keep); t_emit(vm, f->base);
                }
            }
            if (!vm->tr.dead) { vm->tr.dead = 1; vm->tr.dead_depth = 0; }
            break; }
        case 0x0F:
            if (vm->tr.dead) break;
            if (vm->tr.h < (uint32_t)vm->tr.ctl[0].base + F->nresults) { t_bad(vm); return; }
            t_emit(vm, 0x0F); vm->tr.dead = 1; vm->tr.dead_depth = 0; break;
        case 0x10: {
            uint32_t idx = rd_u32(vm);
            if (vm->tr.dead) break;
            if (idx >= vm->nfuncs) { t_bad(vm); return; }
            t_pop(vm, vm->funcs[idx].nparams); t_push(vm, vm->funcs[idx].nresults);
            t_emit(vm, 0x10); t_emit(vm, idx);
            break; }
        case 0x11: case 0x13: {                                    /* call_indirect / return_call_indirect */
            uint32_t ti = rd_u32(vm), tab = rd_u32(vm);
            if (vm->err) return;
            if (vm->tr.dead) break;
            if (ti >= vm->ntypes || tab >= vm->ntables) { t_bad(vm); return; }
            { const uint8_t *r = w_rec(vm, ti); t_pop(vm, 1u + r[0]); if (op == 0x11) t_push(vm, r[1]); }
            t_emit(vm, op == 0x11 ? 0x11u : (uint32_t)OP_RETCALL_IND); t_emit(vm, vm->type_canon[ti]); t_emit(vm, tab);
            if (op == 0x13) { vm->tr.dead = 1; vm->tr.dead_depth = 0; }
            break; }
        case 0x12: {                                               /* return_call */
            uint32_t idx = rd_u32(vm);
            if (vm->tr.dead) break;
            if (idx >= vm->nfuncs) { t_bad(vm); return; }
            t_pop(vm, vm->funcs[idx].nparams);
            t_emit(vm, OP_RETCALL); t_emit(vm, idx);
            vm->tr.dead = 1; vm->tr.dead_depth = 0;
            break; }
        case 0x25: case 0x26: {                                    /* table.get / table.set */
            uint32_t ti = rd_u32(vm);
            if (vm->tr.dead) break;
            if (ti >= vm->ntables) { t_bad(vm); return; }
            if (op == 0x25) { t_pop(vm, 1); t_push(vm, 1); } else t_pop(vm, 2);
            t_emit(vm, op); t_emit(vm, ti);
            break; }
        case 0xD0: (void)rd_u8(vm); if (!vm->tr.dead) { t_push(vm, 1); t_emit(vm, 0x41); t_emit32(vm, 0); } break;   /* ref.null */
        case 0xD1: if (!vm->tr.dead) { t_pop(vm, 1); t_push(vm, 1); t_emit(vm, 0x50); } break;                      /* ref.is_null == i64.eqz */
        case 0xD2: {                                               /* ref.func */
            uint32_t idx = rd_u32(vm);
            if (vm->tr.dead) break;
            if (idx >= vm->nfuncs) { t_bad(vm); return; }
            t_push(vm, 1); t_emit(vm, 0x41); t_emit32(vm, idx + 1u);
            break; }
        case 0x1A: if (!vm->tr.dead) { t_pop(vm, 1); t_emit(vm, 0x1A); } break;
        case 0x1C: { uint32_t n = rd_u32(vm), k; for (k = 0; k < n && !vm->err; k++) (void)w_vt(vm); }
            /* fall through */
        case 0x1B: if (!vm->tr.dead) { t_pop(vm, 3); t_push(vm, 1); t_emit(vm, 0x1B); } break;
        case 0x20: case 0x21: case 0x22: {
            uint32_t idx = rd_u32(vm);
            if (vm->tr.dead) break;
            if (idx >= vm->tr.nloc) { t_bad(vm); return; }
            if (op == 0x20) t_push(vm, 1); else if (op == 0x21) t_pop(vm, 1);
            else if (vm->tr.h < (uint32_t)vm->tr.ctl[vm->tr.nctl - 1].base + 1) { t_bad(vm); return; }
            if (op == 0x20) { uint32_t p0 = vm->tr.n; t_emit(vm, op); t_emit(vm, idx); t_hist_push(vm, HK_LGET, idx, p0); }
            else if (op == 0x21) t_emit_set(vm, idx);
            else { t_emit(vm, op); t_emit(vm, idx); }
            break; }
        case 0x23: case 0x24: {
            uint32_t idx = rd_u32(vm);
            if (vm->tr.dead) break;
            if (idx >= vm->nglobals) { t_bad(vm); return; }
            if (op == 0x23) t_push(vm, 1); else t_pop(vm, 1);
            t_emit(vm, op); t_emit(vm, idx);
            break; }
        case 0x3F: case 0x40:
            if (rd_u8(vm) != 0) { t_bad(vm); return; }
            if (vm->tr.dead) break;
            if (!vm->has_mem) { t_bad(vm); return; }
            if (op == 0x3F) t_push(vm, 1); else { t_pop(vm, 1); t_push(vm, 1); }
            t_emit(vm, op);
            break;
        case 0x41: { int32_t v = rd_s32(vm); if (vm->tr.dead) break; { uint32_t p0 = vm->tr.n; t_push(vm, 1); t_emit(vm, 0x41); t_emit32(vm, (uint32_t)v); t_hist_push(vm, HK_ICONST, (uint32_t)v, p0); } break; }
        case 0x42: { uint64_t v = (uint64_t)rd_s64(vm); if (vm->tr.dead) break; t_push(vm, 1); t_emit(vm, 0x42); t_emit32(vm, (uint32_t)v); t_emit32(vm, (uint32_t)(v >> 32)); break; }
#if !defined(ECCLES_WASM_NO_FLOAT)
        case 0x43: { uint32_t v = (uint32_t)rd_le(vm, 4); if (vm->tr.dead) break; t_push(vm, 1); t_emit(vm, 0x43); t_emit32(vm, v); break; }
        case 0x44: { uint64_t v = rd_le(vm, 8); if (vm->tr.dead) break; t_push(vm, 1); t_emit(vm, 0x44); t_emit32(vm, (uint32_t)v); t_emit32(vm, (uint32_t)(v >> 32)); break; }
#endif
        case 0xFC: {
            uint32_t sub = rd_u32(vm);
            if (vm->err) return;
            if (sub <= 7) {
#if defined(ECCLES_WASM_NO_FLOAT)
                w_fail(vm, ECCLES_WASM_ERR_UNSUPPORTED_FEATURE, 0xFC00u | sub); return;
#else
                if (vm->tr.dead) break;
                t_pop(vm, 1); t_push(vm, 1); t_emit(vm, OP_SAT0 + sub);
#endif
            } else if (sub == 10) {
                if (rd_u8(vm) != 0 || rd_u8(vm) != 0) { t_bad(vm); return; }
                if (vm->tr.dead) break;
                if (!vm->has_mem) { t_bad(vm); return; }
                t_pop(vm, 3); t_emit(vm, OP_MEMCOPY);
            } else if (sub == 11) {
                if (rd_u8(vm) != 0) { t_bad(vm); return; }
                if (vm->tr.dead) break;
                if (!vm->has_mem) { t_bad(vm); return; }
                t_pop(vm, 3); t_emit(vm, OP_MEMFILL);
            } else if (sub >= 12 && sub <= 17) {                         /* table.init/elem.drop/copy/grow/size/fill */
                uint32_t a1 = rd_u32(vm), a2 = 0;
                if (sub == 12 || sub == 14) a2 = rd_u32(vm);
                if (vm->err) return;
                if (vm->tr.dead) break;
                if (sub == 12) {                                     /* a1 = elem segment, a2 = table */
                    if (a1 >= vm->neseg || a2 >= vm->ntables) { t_bad(vm); return; }
                    t_pop(vm, 3); t_emit(vm, OP_TABINIT); t_emit(vm, a1); t_emit(vm, a2);
                } else if (sub == 13) {
                    if (a1 >= vm->neseg) { t_bad(vm); return; }
                    t_emit(vm, OP_ELEMDROP); t_emit(vm, a1);
                } else if (sub == 14) {                              /* a1 = dst table, a2 = src table */
                    if (a1 >= vm->ntables || a2 >= vm->ntables) { t_bad(vm); return; }
                    t_pop(vm, 3); t_emit(vm, OP_TABCOPY); t_emit(vm, a1); t_emit(vm, a2);
                } else {
                    if (a1 >= vm->ntables) { t_bad(vm); return; }
                    if (sub == 15) { t_pop(vm, 2); t_push(vm, 1); t_emit(vm, OP_TABGROW); }
                    else if (sub == 16) { t_push(vm, 1); t_emit(vm, OP_TABSIZE); }
                    else { t_pop(vm, 3); t_emit(vm, OP_TABFILL); }
                    t_emit(vm, a1);
                }
            } else if (sub == 8) {                                      /* memory.init dataidx 0 */
                uint32_t di = rd_u32(vm);
                if (rd_u8(vm) != 0) { t_bad(vm); return; }
                if (vm->tr.dead) break;
                if (!vm->has_mem || di >= vm->nseg) { t_bad(vm); return; }
                t_pop(vm, 3); t_emit(vm, OP_MEMINIT); t_emit(vm, di);
            } else if (sub == 9) {                                      /* data.drop dataidx */
                uint32_t di = rd_u32(vm);
                if (vm->tr.dead) break;
                if (di >= vm->nseg) { t_bad(vm); return; }
                t_emit(vm, OP_DATADROP); t_emit(vm, di);
            } else { w_fail(vm, ECCLES_WASM_ERR_UNSUPPORTED_FEATURE, 0xFC00u | sub); return; }
            break; }
        default:
            if (op >= 0x28 && op <= 0x3E) {
                uint32_t off;
#if defined(ECCLES_WASM_NO_FLOAT)
                if (t_is_float(op)) { w_fail(vm, ECCLES_WASM_ERR_UNSUPPORTED_FEATURE, op); return; }
#endif
                (void)rd_u32(vm); off = rd_u32(vm);
                if (vm->tr.dead) break;
                if (!vm->has_mem) { t_bad(vm); return; }
                if (op <= 0x35) { t_pop(vm, 1); t_push(vm, 1); } else t_pop(vm, 2);
                if ((op == 0x28 || op == 0x2D) && t_hist_top(&vm->tr, HK_LGET)) {
                    uint32_t a = vm->tr.hval[vm->tr.nh - 1]; vm->tr.n = vm->tr.hpos[vm->tr.nh - 1];
                    t_emit(vm, op == 0x28 ? OP_LLD32 : OP_LLD8U); t_emit(vm, a); t_emit32(vm, off);
                    vm->tr.nh = 0; vm->tr.lkind = 0;
                } else { t_emit(vm, op); t_emit32(vm, off); }
            } else if (op >= 0x45 && op <= 0xC4) {
#if defined(ECCLES_WASM_NO_FLOAT)
                if (t_is_float(op)) { w_fail(vm, ECCLES_WASM_ERR_UNSUPPORTED_FEATURE, op); return; }
#endif
                if (vm->tr.dead) break;
                if (t_unary(op)) { t_pop(vm, 1); t_push(vm, 1); } else { t_pop(vm, 2); t_push(vm, 1); }
                { int fx = w_fuse_idx(op);
                  if (fx >= 0) t_emit_bin(vm, op, fx);
                  else if (op < 0xBC || op > 0xBF) t_emit(vm, op);       /* reinterpret = no-op on raw slots */ }
            } else { w_fail(vm, ECCLES_WASM_ERR_UNSUPPORTED_FEATURE, op); return; }
            break;
        }
    }
    if (vm->err) return;
    if (rd_pos(vm) != body_end) { t_bad(vm); return; }
    if (vm->tr.maxh > ECCLES_WASM_VALUE_STACK_SLOTS) { w_fail(vm, ECCLES_WASM_ERR_LIMIT, vm->tr.maxh); return; }
    F->code = vm->tr.code; F->nlocals = (uint16_t)extra; F->max_h = (uint16_t)vm->tr.maxh;
    vm->lo = (uint8_t *)(vm->tr.code + vm->tr.n);
    vm->code_bytes += vm->tr.n * 2u;
}

/* ---- constant expressions ---- */
static uint64_t w_const_expr(eccles_wasm_vm_t *vm, uint32_t visible) {   /* incl. extended-const arithmetic */
    uint64_t st[8]; int n = 0;
    for (;;) {
        int op = rd_u8(vm);
        if (vm->err) return 0;
        if (op == 0x0B) break;
        if (n >= 8) { w_fail(vm, ECCLES_WASM_ERR_LIMIT, 8); return 0; }
        switch (op) {
        case 0x41: st[n++] = (uint32_t)rd_s32(vm); break;
        case 0x42: st[n++] = (uint64_t)rd_s64(vm); break;
#if !defined(ECCLES_WASM_NO_FLOAT)
        case 0x43: st[n++] = rd_le(vm, 4); break;
        case 0x44: st[n++] = rd_le(vm, 8); break;
#endif
        case 0x23: { uint32_t g = rd_u32(vm); if (g >= visible) { t_bad(vm); return 0; } st[n++] = vm->globals[g]; break; }
        case 0xD0: (void)rd_u8(vm); st[n++] = 0; break;                                   /* ref.null */
        case 0xD2: { uint32_t f = rd_u32(vm); if (f >= vm->nfuncs) { t_bad(vm); return 0; } st[n++] = (uint64_t)f + 1u; break; }
        case 0x6A: case 0x6B: case 0x6C: {
            uint32_t b, a; if (n < 2) { t_bad(vm); return 0; }
            b = (uint32_t)st[--n]; a = (uint32_t)st[--n];
            st[n++] = (uint32_t)(op == 0x6A ? a + b : op == 0x6B ? a - b : a * b); break; }
        case 0x7C: case 0x7D: case 0x7E: {
            uint64_t b, a; if (n < 2) { t_bad(vm); return 0; }
            b = st[--n]; a = st[--n];
            st[n++] = op == 0x7C ? a + b : op == 0x7D ? a - b : a * b; break; }
        default: w_fail(vm, ECCLES_WASM_ERR_UNSUPPORTED_FEATURE, (uint32_t)op); return 0;
        }
    }
    if (n != 1) { t_bad(vm); return 0; }
    return st[0];
}

/* ---- sections ---- */
static void sec_type(eccles_wasm_vm_t *vm) {
    uint32_t n = rd_u32(vm), i, used = 0;
    uint8_t *pool;
    if (vm->err) return;
    if (n > 0xFFFFu) { w_fail(vm, ECCLES_WASM_ERR_LIMIT, n); return; }
    vm->type_off = (uint16_t *)w_alo(vm, n * 2u + 2u, 2);
    vm->type_canon = (uint16_t *)w_alo(vm, n * 2u + 2u, 2);
    pool = vm->types = (uint8_t *)w_alo(vm, 0, 1);
    for (i = 0; i < n && !vm->err; i++) {
        uint32_t np, nr, k; uint8_t *rec;
        if (rd_u8(vm) != 0x60) { t_bad(vm); return; }
        np = rd_u32(vm); if (np > 200) { w_fail(vm, ECCLES_WASM_ERR_LIMIT, np); return; }
        if (vm->err) return;
        rec = (uint8_t *)w_alo(vm, 2 + np, 1); if (!rec) return;
        rec[0] = (uint8_t)np;
        for (k = 0; k < np; k++) rec[2 + k] = (uint8_t)w_vt(vm);
        nr = rd_u32(vm); if (nr > 16) { w_fail(vm, ECCLES_WASM_ERR_LIMIT, nr); return; }
        if (vm->err || !w_alo(vm, nr, 1)) return;
        rec[1] = (uint8_t)nr;
        for (k = 0; k < nr; k++) rec[2 + np + k] = (uint8_t)w_vt(vm);
        vm->type_off[i] = (uint16_t)used;
        if (used + 2 + np + nr > 0xFFFFu) { w_fail(vm, ECCLES_WASM_ERR_LIMIT, used); return; }
        used += 2 + np + nr;
        { uint32_t j; vm->type_canon[i] = (uint16_t)i;
          for (j = 0; j < i; j++) { const uint8_t *a = pool + vm->type_off[j];
              if (a[0] == rec[0] && a[1] == rec[1] && memcmp(a, rec, 2u + rec[0] + rec[1]) == 0) { vm->type_canon[i] = (uint16_t)j; break; } } }
    }
    vm->ntypes = (uint16_t)n;
}

static void w_alloc_mem(eccles_wasm_vm_t *vm, uint32_t flags, uint32_t mn) {
    uint64_t want;
    if (vm->has_mem) { w_fail(vm, ECCLES_WASM_ERR_UNSUPPORTED_FEATURE, 0x3002u); return; }     /* multi-memory */
    if (flags > 1) { w_fail(vm, ECCLES_WASM_ERR_UNSUPPORTED_FEATURE, 0x3100u | flags); return; }
    if (mn > 65536u) { t_bad(vm); return; }
    want = (uint64_t)mn * W_PAGE;
#if defined(ECCLES_WASM_MEM_LIMIT_BYTES)
    if (want > (uint64_t)(ECCLES_WASM_MEM_LIMIT_BYTES)) want = (uint64_t)(ECCLES_WASM_MEM_LIMIT_BYTES);
#endif
    if (want > 0xFFFFFFF0u) { w_fail(vm, ECCLES_WASM_ERR_NO_MEMORY, 0xFFFFFFFFu); return; }
    vm->mem = (uint8_t *)w_ahi(vm, (size_t)want, 16);
    if (!vm->mem) return;
    memset(vm->mem, 0, (size_t)want);
    vm->mem_bytes = (uint32_t)want; vm->mem_pages = mn; vm->has_mem = 1;
}
static void w_alloc_table(eccles_wasm_vm_t *vm, w_table_t *t, uint32_t flags, uint32_t mn) {
    if (flags > 1) { w_fail(vm, ECCLES_WASM_ERR_UNSUPPORTED_FEATURE, 0x2100u | flags); return; }
    if (mn > 0x7FFFFFu) { w_fail(vm, ECCLES_WASM_ERR_LIMIT, mn); return; }
    t->elems = (uint32_t *)w_alo(vm, (size_t)mn * 4u + 4u, 4);
    if (!t->elems) return;
    memset(t->elems, 0, (size_t)mn * 4u);
    t->size = mn;
}
static int w_find_global(eccles_wasm_vm_t *vm, const char *name, uint64_t *v) {
    uint8_t i;
    for (i = 0; i < vm->nhglob; i++) if (strcmp(vm->hglob[i].name, name) == 0) { *v = vm->hglob[i].v; return 1; }
    return 0;
}

static void sec_import(eccles_wasm_vm_t *vm) {
    uint32_t n = rd_u32(vm), i, nf = 0;
    char name[ECCLES_WASM_MAX_NAME + 1];
    if (vm->err) return;
    if (n > 0xFFFFu) { w_fail(vm, ECCLES_WASM_ERR_LIMIT, n); return; }
    vm->funcs = (w_func_t *)w_alo(vm, n * sizeof(w_func_t) + 8, 8);
    if (!vm->funcs) return;
    for (i = 0; i < n && !vm->err; i++) {
        uint32_t fl, ti; int kind; eccles_wasm_host_fn fn = 0; const char *sig = 0; w_func_t *f; const uint8_t *rec;
        (void)rd_name(vm, NULL, 0);
        fl = rd_name(vm, name, sizeof name);
        kind = rd_u8(vm);
        if (vm->err) return;
        if (kind == 1) {                                        /* imported table: owned by this instance */
            int rt = rd_u8(vm); uint32_t flags, mn; w_table_t *tb;
            flags = rd_u32(vm); mn = rd_u32(vm); if (flags & 1) (void)rd_u32(vm);
            if (vm->err) return;
            if (rt != 0x70 && rt != 0x6F) { w_fail(vm, ECCLES_WASM_ERR_UNSUPPORTED_FEATURE, 0x2000u | (uint32_t)rt); return; }
            if (!vm->tables) { vm->tables = (w_table_t *)w_alo(vm, (n + 1u) * sizeof(w_table_t), sizeof(void *)); if (!vm->tables) return; }
            tb = &vm->tables[vm->ntables]; tb->elems = NULL; tb->size = 0;
            w_alloc_table(vm, tb, flags, mn); vm->ntables++;
            continue;
        }
        if (kind == 2) {                                        /* imported memory: allocated here, same as a defined one */
            uint32_t flags = rd_u32(vm), mn = rd_u32(vm);
            if (flags & 1) (void)rd_u32(vm);
            if (vm->err) return;
            w_alloc_mem(vm, flags, mn);
            continue;
        }
        if (kind == 3) {                                        /* imported global: value from eccles_wasm_vm_set_global() or 0 */
            uint64_t v = 0;
            (void)w_vt(vm); (void)rd_u8(vm);
            if (vm->err) return;
            if (!vm->globals) { vm->globals = (uint64_t *)w_alo(vm, (n + 1u) * 8u, 8); if (!vm->globals) return; }
            if (fl < sizeof name) (void)w_find_global(vm, name, &v);
            vm->globals[vm->nglobals_imp++] = v;
            continue;
        }
        if (kind != 0) { w_fail(vm, ECCLES_WASM_ERR_UNSUPPORTED_FEATURE, 0x1000u | (uint32_t)kind); return; }
        ti = rd_u32(vm);
        if (vm->err) return;
        if (ti >= vm->ntypes) { t_bad(vm); return; }
        if (fl >= sizeof name || !w_find_host(vm, name, &fn, &sig)) {
#if defined(ECCLES_WASM_LENIENT_IMPORTS)
            fn = w_trap_stub; sig = 0;
#else
            memcpy(vm->err_str, name, sizeof name);
            w_fail(vm, ECCLES_WASM_ERR_IMPORT, i); return;
#endif
        }
        rec = w_rec(vm, ti);
        if (sig) {
            uint8_t want[2 + 200 + 16]; uint32_t wl = w_sig(sig, want, sizeof want);
            if (wl != 2u + rec[0] + rec[1] || memcmp(want, rec, wl) != 0) {
                memcpy(vm->err_str, name, sizeof name);
                w_fail(vm, ECCLES_WASM_ERR_SIGNATURE, i); return;
            }
        }
        f = &vm->funcs[nf++];
        f->code = NULL; f->host = fn; f->type = vm->type_canon[ti]; f->nparams = rec[0]; f->nresults = rec[1];
        f->nlocals = 0; f->max_h = 0;
    }
    vm->nimports = vm->nfuncs = (uint16_t)nf;
    vm->nglobals = vm->nglobals_imp;
}

static void sec_function(eccles_wasm_vm_t *vm) {
    uint32_t n = rd_u32(vm), i, total;
    w_func_t *nf;
    if (vm->err) return;
    total = (uint32_t)vm->nimports + n;
    if (total > 0xFFFEu) { w_fail(vm, ECCLES_WASM_ERR_LIMIT, total); return; }
    nf = (w_func_t *)w_alo(vm, total * sizeof(w_func_t) + 8, 8);
    if (!nf) return;
    if (vm->nimports) memcpy(nf, vm->funcs, vm->nimports * sizeof(w_func_t));
    vm->funcs = nf;
    for (i = 0; i < n && !vm->err; i++) {
        uint32_t ti = rd_u32(vm); const uint8_t *rec; w_func_t *f;
        if (vm->err) return;
        if (ti >= vm->ntypes) { t_bad(vm); return; }
        rec = w_rec(vm, ti); f = &vm->funcs[vm->nimports + i];
        f->code = NULL; f->host = 0; f->type = vm->type_canon[ti]; f->nparams = rec[0]; f->nresults = rec[1];
        f->nlocals = 0; f->max_h = 0;
    }
    vm->nfuncs = (uint16_t)total;
}

static void sec_table(eccles_wasm_vm_t *vm) {
    uint32_t n = rd_u32(vm), i, total;
    w_table_t *nt;
    if (vm->err) return;
    total = (uint32_t)vm->ntables + n;
    if (total > 0xFFu) { w_fail(vm, ECCLES_WASM_ERR_LIMIT, total); return; }
    nt = (w_table_t *)w_alo(vm, (total + 1u) * sizeof(w_table_t), sizeof(void *));
    if (!nt) return;
    if (vm->ntables) memcpy(nt, vm->tables, vm->ntables * sizeof(w_table_t));
    for (i = 0; i < n && !vm->err; i++) {
        int rt = rd_u8(vm); uint32_t flags = rd_u32(vm), mn = rd_u32(vm);
        if (flags & 1) (void)rd_u32(vm);
        if (vm->err) return;
        if (rt != 0x70 && rt != 0x6F) { w_fail(vm, ECCLES_WASM_ERR_UNSUPPORTED_FEATURE, 0x2000u | (uint32_t)rt); return; }
        nt[vm->ntables + i].elems = NULL; nt[vm->ntables + i].size = 0;
        w_alloc_table(vm, &nt[vm->ntables + i], flags, mn);
    }
    vm->tables = nt; vm->ntables = (uint16_t)total;
}

static void sec_memory(eccles_wasm_vm_t *vm) {
    uint32_t n = rd_u32(vm), flags, mn;
    if (vm->err) return;
    if (n + (vm->has_mem ? 1u : 0u) > 1u) { w_fail(vm, ECCLES_WASM_ERR_UNSUPPORTED_FEATURE, 0x3000u | n); return; }
    if (n == 0) return;
    flags = rd_u32(vm); mn = rd_u32(vm); if (flags & 1) (void)rd_u32(vm);
    if (vm->err) return;
    w_alloc_mem(vm, flags, mn);
}

static void sec_global(eccles_wasm_vm_t *vm) {
    uint32_t n = rd_u32(vm), i, base = vm->nglobals_imp; uint64_t *ng;
    if (vm->err) return;
    if (n + base > 0xFFFEu) { w_fail(vm, ECCLES_WASM_ERR_LIMIT, n + base); return; }
    ng = (uint64_t *)w_alo(vm, (n + base) * 8u + 8u, 8);
    if (!ng) return;
    if (base) memcpy(ng, vm->globals, base * 8u);
    vm->globals = ng;
    for (i = 0; i < n && !vm->err; i++) {
        (void)w_vt(vm); (void)rd_u8(vm);
        vm->globals[base + i] = w_const_expr(vm, base + i);
    }
    vm->nglobals = (uint16_t)(n + base);
}

static void sec_export(eccles_wasm_vm_t *vm) {
    uint32_t n = rd_u32(vm), i;
    if (vm->err) return;
    if (n > 0xFFFEu) { w_fail(vm, ECCLES_WASM_ERR_LIMIT, n); return; }
    vm->exports = (w_export_t *)w_alo(vm, n * sizeof(w_export_t) + 8, sizeof(void *));
    if (!vm->exports) return;
    for (i = 0; i < n && !vm->err; i++) {
        uint32_t len = rd_u32(vm), idx; int kind; uint8_t *save = vm->lo; char *nm;
        if (vm->err) return;
        nm = (char *)w_alo(vm, (size_t)len + 1, 1);
        if (!nm) return;
        rd_bytes(vm, (uint8_t *)nm, len); nm[len] = 0;
        kind = rd_u8(vm); idx = rd_u32(vm);
        if (vm->err) return;
        if (kind == 0) {
            if (idx >= vm->nfuncs) { t_bad(vm); return; }
            vm->exports[vm->nexports].name = nm; vm->exports[vm->nexports].func = (uint16_t)idx; vm->nexports++;
        } else vm->lo = save;
    }
}

static void sec_start(eccles_wasm_vm_t *vm) {
    uint32_t idx = rd_u32(vm);
    if (vm->err) return;
    if (idx >= vm->nfuncs || vm->funcs[idx].nparams || vm->funcs[idx].nresults) { t_bad(vm); return; }
    vm->start = (uint16_t)idx;
}

static void sec_element(eccles_wasm_vm_t *vm) {            /* all eight element-segment encodings */
    uint32_t n = rd_u32(vm), i;
    if (vm->err) return;
    if (n > 0xFFFEu) { w_fail(vm, ECCLES_WASM_ERR_LIMIT, n); return; }
    vm->eseg = (w_eseg_t *)w_alo(vm, (n + 1u) * sizeof(w_eseg_t), sizeof(void *));
    if (!vm->eseg) return;
    for (i = 0; i < n; i++) { vm->eseg[i].items = NULL; vm->eseg[i].len = 0; }
    vm->neseg = (uint16_t)n;
    for (i = 0; i < n && !vm->err; i++) {
        uint32_t flags = rd_u32(vm), tab = 0, off = 0, cnt, k; int passive, decl, exprs; uint32_t *items = NULL;
        if (vm->err) return;
        if (flags > 7) { w_fail(vm, ECCLES_WASM_ERR_UNSUPPORTED_FEATURE, 0x9000u | flags); return; }
        passive = (flags & 3) == 1; decl = (flags & 3) == 3; exprs = (flags & 4) != 0;
        if (!(flags & 1)) {                                         /* active: [tableidx] offset */
            if (flags & 2) tab = rd_u32(vm);
            off = (uint32_t)w_const_expr(vm, vm->nglobals);
            if (vm->err) return;
            if (tab >= vm->ntables) { t_bad(vm); return; }
        }
        if (flags & 3) (void)rd_u8(vm);                              /* elemkind / reftype */
        cnt = rd_u32(vm);
        if (vm->err) return;
        if (!(flags & 1) && (uint64_t)off + cnt > vm->tables[tab].size) { t_bad(vm); return; }
        if (passive) { items = (uint32_t *)w_alo(vm, (size_t)cnt * 4u + 4u, 4); if (!items) return; }
        for (k = 0; k < cnt && !vm->err; k++) {
            uint32_t v;
            if (exprs) v = (uint32_t)w_const_expr(vm, vm->nglobals);
            else { uint32_t fx = rd_u32(vm); if (fx >= vm->nfuncs) { t_bad(vm); return; } v = fx + 1u; }
            if (passive) items[k] = v;
            else if (!decl) vm->tables[tab].elems[off + k] = v;
        }
        if (passive) { vm->eseg[i].items = items; vm->eseg[i].len = cnt; }
    }
}

static void sec_code(eccles_wasm_vm_t *vm) {
    uint32_t n = rd_u32(vm), i;
    if (vm->err) return;
    if (n != (uint32_t)(vm->nfuncs - vm->nimports)) { t_bad(vm); return; }
    vm->code_seen = 1;
    for (i = 0; i < n && !vm->err; i++) {
        uint32_t size = rd_u32(vm), end;
        if (vm->err) return;
        end = rd_pos(vm) + size;
        w_translate(vm, (uint32_t)vm->nimports + i, end);
    }
}

static int w_alloc_segs(eccles_wasm_vm_t *vm, uint32_t n) {
    uint32_t i;
    if (n > 0xFFFEu) { w_fail(vm, ECCLES_WASM_ERR_LIMIT, n); return 0; }
    vm->segs = (struct w_seg *)w_alo(vm, (n + 1u) * sizeof(struct w_seg), sizeof(void *));
    if (!vm->segs) return 0;
    for (i = 0; i < n; i++) { vm->segs[i].p = NULL; vm->segs[i].len = 0; }
    vm->nseg = (uint16_t)n;
    return 1;
}
static void sec_datacount(eccles_wasm_vm_t *vm) {
    uint32_t n = rd_u32(vm);
    if (!vm->err && n) (void)w_alloc_segs(vm, n);
}

static void sec_data(eccles_wasm_vm_t *vm) {
    uint32_t n = rd_u32(vm), i;
    if (vm->err) return;
    if (vm->nseg && n != vm->nseg) { t_bad(vm); return; }              /* must agree with the data count section */
    for (i = 0; i < n && !vm->err; i++) {
        uint32_t flags = rd_u32(vm), off, len; uint64_t e;
        if (vm->err) return;
        if (flags == 1) {                                         /* passive: keep the bytes for memory.init */
            uint8_t *dst;
            len = rd_u32(vm);
            if (vm->err) return;
            if (!vm->segs && !w_alloc_segs(vm, n)) return;
            dst = (uint8_t *)w_alo(vm, len, 1);
            if (!dst) return;
            rd_bytes(vm, dst, len);
            vm->segs[i].p = dst; vm->segs[i].len = len;
            continue;
        }
        if (flags == 2) { if (rd_u32(vm) != 0) { t_bad(vm); return; } }
        else if (flags != 0) { w_fail(vm, ECCLES_WASM_ERR_UNSUPPORTED_FEATURE, 0xB000u | flags); return; }
        off = (uint32_t)w_const_expr(vm, vm->nglobals);
        len = rd_u32(vm);
        if (vm->err) return;
        e = (uint64_t)off + len;
        if (!vm->has_mem && len) { t_bad(vm); return; }
        if (e > vm->mem_bytes) {
            if (e <= (uint64_t)vm->mem_pages * W_PAGE) w_fail(vm, ECCLES_WASM_ERR_NO_MEMORY, (uint32_t)e);
            else t_bad(vm);
            return;
        }
        rd_bytes(vm, vm->mem + off, len);
    }
}

static void w_load_module(eccles_wasm_vm_t *vm) {
    uint8_t hdr[8]; int last = 0, rank;
    static const uint8_t magic[8] = {0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00};
    vm->start = 0xFFFFu;
    rd_bytes(vm, hdr, 8);
    if (vm->err) return;
    if (memcmp(hdr, magic, 8) != 0) { t_bad(vm); return; }
    for (;;) {
        uint32_t id, size, end;
        if (vm->rd.p >= vm->rd.e && !rd_fill(vm)) break;                 /* clean EOF between sections */
        id = (uint32_t)rd_u8(vm); size = rd_u32(vm);
        if (vm->err) return;
        end = rd_pos(vm) + size;
        if (id == 0) { rd_skip(vm, size); if (vm->err) return; continue; }
        switch (id) {
        case 1: rank = 1; break;  case 2: rank = 2; break;  case 3: rank = 3; break;
        case 4: rank = 4; break;  case 5: rank = 5; break;  case 6: rank = 6; break;
        case 7: rank = 7; break;  case 8: rank = 8; break;  case 9: rank = 9; break;
        case 12: rank = 10; break; case 10: rank = 11; break; case 11: rank = 12; break;
        default: t_bad(vm); return;
        }
        if (rank <= last) { t_bad(vm); return; }
        last = rank;
        switch (id) {
        case 1: sec_type(vm); break;     case 2: sec_import(vm); break;  case 3: sec_function(vm); break;
        case 4: sec_table(vm); break;    case 5: sec_memory(vm); break;  case 6: sec_global(vm); break;
        case 7: sec_export(vm); break;   case 8: sec_start(vm); break;   case 9: sec_element(vm); break;
        case 12: sec_datacount(vm); break;
        case 10: sec_code(vm); break;    case 11: sec_data(vm); break;
        }
        if (vm->err) return;
        if (rd_pos(vm) != end) { t_bad(vm); return; }
    }
    if (vm->err) return;
    if (vm->nfuncs > vm->nimports && !vm->code_seen) { t_bad(vm); return; }
}

/* ===================================================================== *
 *  4. interpreter
 * ===================================================================== */

#if (ECCLES_WASM_YIELD_INTERVAL) != 0 && !defined(ECCLES_WASM_YIELD)
  #error "eccles_wasm: ECCLES_WASM_YIELD_INTERVAL is set but ECCLES_WASM_YIELD() is not defined."
#endif

#if ECCLES_WASM_BIG_ENDIAN
  static uint16_t le16(uint16_t v) { return (uint16_t)((v << 8) | (v >> 8)); }
  static uint32_t le32(uint32_t v) { return ((uint32_t)le16((uint16_t)v) << 16) | le16((uint16_t)(v >> 16)); }
  static uint64_t le64(uint64_t v) { return ((uint64_t)le32((uint32_t)v) << 32) | le32((uint32_t)(v >> 32)); }
#else
  #define le16(v) (v)
  #define le32(v) (v)
  #define le64(v) (v)
#endif

static uint32_t w_clz32(uint32_t x) {
    if (!x) return 32;
#if defined(__GNUC__) || defined(__clang__)
    return (uint32_t)__builtin_clz(x);
#else
    { uint32_t n = 0; while (!(x & 0x80000000u)) { x <<= 1; n++; } return n; }
#endif
}
static uint32_t w_ctz32(uint32_t x) {
    if (!x) return 32;
#if defined(__GNUC__) || defined(__clang__)
    return (uint32_t)__builtin_ctz(x);
#else
    { uint32_t n = 0; while (!(x & 1u)) { x >>= 1; n++; } return n; }
#endif
}
static uint32_t w_pop32(uint32_t x) {
    x = x - ((x >> 1) & 0x55555555u);
    x = (x & 0x33333333u) + ((x >> 2) & 0x33333333u);
    x = (x + (x >> 4)) & 0x0F0F0F0Fu;
    return (x * 0x01010101u) >> 24;
}
static uint32_t w_clz64(uint64_t x) { return (x >> 32) ? w_clz32((uint32_t)(x >> 32)) : 32 + w_clz32((uint32_t)x); }
static uint32_t w_ctz64(uint64_t x) { return (uint32_t)x ? w_ctz32((uint32_t)x) : 32 + w_ctz32((uint32_t)(x >> 32)); }
static uint32_t w_pop64(uint64_t x) { return w_pop32((uint32_t)x) + w_pop32((uint32_t)(x >> 32)); }

#if !defined(ECCLES_WASM_NO_FLOAT)
static uint32_t fb32(float f)    { uint32_t u; memcpy(&u, &f, 4); return u; }
static float    bf32(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static uint64_t fb64(double f)   { uint64_t u; memcpy(&u, &f, 8); return u; }
static double   bf64(uint64_t u) { double f; memcpy(&f, &u, 8); return f; }

static float f32_zero_like(float x) { return (fb32(x) & 0x80000000u) ? bf32(0x80000000u) : 0.0f; }
static double f64_zero_like(double x) { return (fb64(x) >> 63) ? bf64((uint64_t)1 << 63) : 0.0; }
static float f32_trunc(float x) {
    float t; if (!(x > -8388608.0f && x < 8388608.0f)) return x;
    t = (float)(int32_t)x; return t == 0.0f ? f32_zero_like(x) : t;
}
static float f32_floor(float x) {
    float t; if (!(x > -8388608.0f && x < 8388608.0f)) return x;
    t = (float)(int32_t)x; if (t > x) t -= 1.0f; return t == 0.0f ? f32_zero_like(x) : t;
}
static float f32_ceil(float x) {
    float t; if (!(x > -8388608.0f && x < 8388608.0f)) return x;
    t = (float)(int32_t)x; if (t < x) t += 1.0f; return t == 0.0f ? f32_zero_like(x) : t;
}
static float f32_nearest(float x) {
    volatile float v; float t;
    if (!(x > -8388608.0f && x < 8388608.0f)) return x;
    t = x < 0 ? -x : x; v = t + 8388608.0f; t = v - 8388608.0f;
    return bf32((fb32(t) & 0x7FFFFFFFu) | (fb32(x) & 0x80000000u));
}
static double f64_trunc(double x) {
    double t; if (!(x > -4503599627370496.0 && x < 4503599627370496.0)) return x;
    t = (double)(int64_t)x; return t == 0.0 ? f64_zero_like(x) : t;
}
static double f64_floor(double x) {
    double t; if (!(x > -4503599627370496.0 && x < 4503599627370496.0)) return x;
    t = (double)(int64_t)x; if (t > x) t -= 1.0; return t == 0.0 ? f64_zero_like(x) : t;
}
static double f64_ceil(double x) {
    double t; if (!(x > -4503599627370496.0 && x < 4503599627370496.0)) return x;
    t = (double)(int64_t)x; if (t < x) t += 1.0; return t == 0.0 ? f64_zero_like(x) : t;
}
static double f64_nearest(double x) {
    volatile double v; double t;
    if (!(x > -4503599627370496.0 && x < 4503599627370496.0)) return x;
    t = x < 0 ? -x : x; v = t + 4503599627370496.0; t = v - 4503599627370496.0;
    return bf64((fb64(t) & 0x7FFFFFFFFFFFFFFFull) | (fb64(x) & 0x8000000000000000ull));
}
static float f32_min(float a, float b) {
    if (a != a) return a;
    if (b != b) return b;
    if (a == b) return bf32(fb32(a) | fb32(b));
    return a < b ? a : b;
}
static float f32_max(float a, float b) {
    if (a != a) return a;
    if (b != b) return b;
    if (a == b) return bf32(fb32(a) & fb32(b));
    return a > b ? a : b;
}
static double f64_min(double a, double b) {
    if (a != a) return a;
    if (b != b) return b;
    if (a == b) return bf64(fb64(a) | fb64(b));
    return a < b ? a : b;
}
static double f64_max(double a, double b) {
    if (a != a) return a;
    if (b != b) return b;
    if (a == b) return bf64(fb64(a) & fb64(b));
    return a > b ? a : b;
}
#endif

#define W_NOPS 0x290
typedef char w_nops_check[(W_FUSED_END <= W_NOPS) ? 1 : -1];   /* the dispatch table must cover every fused op */
#define W_TBL(n, o, e)  [FLL_##n] = &&L_FLL_##n, [FLC_##n] = &&L_FLC_##n, [FSC_##n] = &&L_FSC_##n, [FSL_##n] = &&L_FSL_##n,
#define W_TBLS(n, o, e) [FLLS_##n] = &&L_FLLS_##n, [FLCS_##n] = &&L_FLCS_##n,
#define W_TBLB(n, o, e) [BSS_##n] = &&L_BSS_##n, [BLC_##n] = &&L_BLC_##n, [BLL_##n] = &&L_BLL_##n,
#define W_FUSED_TABLE W_FUSE_LIST(W_TBL) W_ALU_LIST(W_TBLS) W_CMP_LIST(W_TBLB) \
    [OP_MOVE] = &&L_OP_MOVE, [OP_SETC] = &&L_OP_SETC, [OP_LLD32] = &&L_OP_LLD32, [OP_LLD8U] = &&L_OP_LLD8U,
/* Dispatch: computed goto (GCC/Clang "labels as values") is selected automatically; define
   ECCLES_WASM_NO_COMPUTED_GOTO to force a portable switch. Both run the same handler bodies. */
#if (defined(__GNUC__) || defined(__clang__)) && !defined(ECCLES_WASM_NO_COMPUTED_GOTO)
  #define W_CG 1
  #define CASE(x)       L_##x:
  #define CASE_DEFAULT  L_default:
  #define NEXT          goto *disp[*pc++]
  #define W_REDISPATCH  goto *disp[*pc++]
#else
  #define W_CG 0
  #define CASE(x)       case x:
  #define CASE_DEFAULT  default:
  #define NEXT          break
  #define W_REDISPATCH  continue
#endif

#if ECCLES_WASM_FUEL
  #if ECCLES_WASM_YIELD_INTERVAL
    #define W_TICK_YIELD() if ((++ticks & ((ECCLES_WASM_YIELD_INTERVAL) - 1u)) == 0) { ECCLES_WASM_YIELD(); }
  #else
    #define W_TICK_YIELD()
  #endif
  #define TICK() do { if (fuel && --fuel == 0) TRAP(ECCLES_WASM_TRAP_FUEL); W_TICK_YIELD() } while (0)
#else
  #define TICK() ((void)0)
#endif

#define TRAP(e)   do { err = (e); goto trap; } while (0)
#define IMM16()   (*pc++)
#define IMM32()   (pc += 2, (uint32_t)pc[-2] | ((uint32_t)pc[-1] << 16))
#define OFF32(p)  ((int32_t)((uint32_t)(p)[0] | ((uint32_t)(p)[1] << 16)))
#define BIN32(E)  { uint32_t b = (uint32_t)sp[-1], a = (uint32_t)sp[-2]; sp--; sp[-1] = (uint32_t)(E); NEXT; }
#define BIN64(E)  { uint64_t b = sp[-1], a = sp[-2]; sp--; sp[-1] = (uint64_t)(E); NEXT; }
#define UN32(E)   { uint32_t a = (uint32_t)sp[-1]; sp[-1] = (uint32_t)(E); NEXT; }
#define UN64(E)   { uint64_t a = sp[-1]; sp[-1] = (uint64_t)(E); NEXT; }
#define FBIN32(E) { float b = bf32((uint32_t)sp[-1]), a = bf32((uint32_t)sp[-2]); sp--; sp[-1] = fb32(E); NEXT; }
#define FCMP32(E) { float b = bf32((uint32_t)sp[-1]), a = bf32((uint32_t)sp[-2]); sp--; sp[-1] = (E) ? 1u : 0u; NEXT; }
#define FUN32(E)  { float a = bf32((uint32_t)sp[-1]); sp[-1] = fb32(E); NEXT; }
#define FBIN64(E) { double b = bf64(sp[-1]), a = bf64(sp[-2]); sp--; sp[-1] = fb64(E); NEXT; }
#define FCMP64(E) { double b = bf64(sp[-1]), a = bf64(sp[-2]); sp--; sp[-1] = (E) ? 1u : 0u; NEXT; }
#define FUN64(E)  { double a = bf64(sp[-1]); sp[-1] = fb64(E); NEXT; }
#define MEMOFF(n) uint32_t off_ = IMM32(); uint64_t ea = (uint64_t)(uint32_t)sp[-1] + off_; \
                  if (W_UNLIKELY(ea + (n) > memsz)) TRAP(ECCLES_WASM_TRAP_MEM_OOB)
#define STOREP(n) uint32_t off_ = IMM32(); uint64_t val = sp[-1]; uint64_t ea = (uint64_t)(uint32_t)sp[-2] + off_; \
                  sp -= 2; if (W_UNLIKELY(ea + (n) > memsz)) TRAP(ECCLES_WASM_TRAP_MEM_OOB)
#define FIX(keep, nb) { uint64_t *d_ = fp + (nb), *s_ = sp - (keep); \
                  if (d_ != s_) { uint32_t k_; for (k_ = 0; k_ < (keep); k_++) d_[k_] = s_[k_]; } sp = d_ + (keep); }

#if W_CG && defined(__GNUC__)
  #pragma GCC diagnostic push
  #pragma GCC diagnostic ignored "-Wpedantic"
  #pragma GCC diagnostic ignored "-Woverride-init"
#endif
static eccles_wasm_error_t w_exec(eccles_wasm_vm_t *vm, const w_func_t *f0) {
    const w_func_t *const funcs = vm->funcs;
    const w_func_t *cur, *f;
    const uint16_t *pc;
    uint64_t *fp, *sp;
    w_frame_t *fr = vm->frames;
    w_frame_t *const frend = vm->frames + ECCLES_WASM_CALL_DEPTH;
    uint64_t *const stk_end = vm->stk + ECCLES_WASM_VALUE_STACK_SLOTS;
    uint8_t *const mem = vm->mem;
    const uint32_t memsz = vm->mem_bytes;
    eccles_wasm_error_t err = ECCLES_WASM_OK;
#if ECCLES_WASM_FUEL
    uint32_t fuel = vm->fuel_budget;
  #if ECCLES_WASM_YIELD_INTERVAL
    uint32_t ticks = 0;
  #endif
#endif

    fp = vm->stk;
    if (fp + f0->max_h > stk_end) return ECCLES_WASM_TRAP_STACK_OVERFLOW;
    sp = fp + f0->nparams;
    { uint64_t *ze = fp + f0->nparams + f0->nlocals; while (sp < ze) *sp++ = 0; }
    cur = f0; pc = f0->code;

#if W_CG
    static const void *const disp[W_NOPS] = {
        [0 ... W_NOPS - 1] = &&L_default,
        /* BEGIN GENERATED DISPATCH (python3 tests/gen_dispatch.py) */
        [0x00] = &&L_0x00,
        [OP_BR] = &&L_OP_BR,
        [OP_BR_FIX] = &&L_OP_BR_FIX,
        [OP_BR_IF] = &&L_OP_BR_IF,
        [OP_BR_IF_FIX] = &&L_OP_BR_IF_FIX,
        [OP_JZ] = &&L_OP_JZ,
        [OP_BR_TABLE] = &&L_OP_BR_TABLE,
        [0x0F] = &&L_0x0F,
        [0x10] = &&L_0x10,
        [0x11] = &&L_0x11,
        [OP_RETCALL] = &&L_OP_RETCALL,
        [OP_RETCALL_IND] = &&L_OP_RETCALL_IND,
        [0x25] = &&L_0x25,
        [0x26] = &&L_0x26,
        [OP_TABSIZE] = &&L_OP_TABSIZE,
        [OP_TABGROW] = &&L_OP_TABGROW,
        [OP_TABFILL] = &&L_OP_TABFILL,
        [OP_TABCOPY] = &&L_OP_TABCOPY,
        [OP_TABINIT] = &&L_OP_TABINIT,
        [OP_ELEMDROP] = &&L_OP_ELEMDROP,
        [0x1A] = &&L_0x1A,
        [0x1B] = &&L_0x1B,
        [0x20] = &&L_0x20,
        [0x21] = &&L_0x21,
        [0x22] = &&L_0x22,
        [0x23] = &&L_0x23,
        [0x24] = &&L_0x24,
        [0x28] = &&L_0x28,
        [0x2A] = &&L_0x2A,
        [0x29] = &&L_0x29,
        [0x2B] = &&L_0x2B,
        [0x2C] = &&L_0x2C,
        [0x2D] = &&L_0x2D,
        [0x2E] = &&L_0x2E,
        [0x2F] = &&L_0x2F,
        [0x30] = &&L_0x30,
        [0x31] = &&L_0x31,
        [0x32] = &&L_0x32,
        [0x33] = &&L_0x33,
        [0x34] = &&L_0x34,
        [0x35] = &&L_0x35,
        [0x36] = &&L_0x36,
        [0x38] = &&L_0x38,
        [0x37] = &&L_0x37,
        [0x39] = &&L_0x39,
        [0x3A] = &&L_0x3A,
        [0x3C] = &&L_0x3C,
        [0x3B] = &&L_0x3B,
        [0x3D] = &&L_0x3D,
        [0x3E] = &&L_0x3E,
        [0x3F] = &&L_0x3F,
        [0x40] = &&L_0x40,
        [OP_MEMCOPY] = &&L_OP_MEMCOPY,
        [OP_MEMFILL] = &&L_OP_MEMFILL,
        [OP_MEMINIT] = &&L_OP_MEMINIT,
        [OP_DATADROP] = &&L_OP_DATADROP,
        [0x41] = &&L_0x41,
        [0x43] = &&L_0x43,
        [0x42] = &&L_0x42,
        [0x44] = &&L_0x44,
        [0x45] = &&L_0x45,
        [0x46] = &&L_0x46,
        [0x47] = &&L_0x47,
        [0x48] = &&L_0x48,
        [0x49] = &&L_0x49,
        [0x4A] = &&L_0x4A,
        [0x4B] = &&L_0x4B,
        [0x4C] = &&L_0x4C,
        [0x4D] = &&L_0x4D,
        [0x4E] = &&L_0x4E,
        [0x4F] = &&L_0x4F,
        [0x50] = &&L_0x50,
        [0x51] = &&L_0x51,
        [0x52] = &&L_0x52,
        [0x53] = &&L_0x53,
        [0x54] = &&L_0x54,
        [0x55] = &&L_0x55,
        [0x56] = &&L_0x56,
        [0x57] = &&L_0x57,
        [0x58] = &&L_0x58,
        [0x59] = &&L_0x59,
        [0x5A] = &&L_0x5A,
#if !defined(ECCLES_WASM_NO_FLOAT)
        [0x5B] = &&L_0x5B,
        [0x5C] = &&L_0x5C,
        [0x5D] = &&L_0x5D,
        [0x5E] = &&L_0x5E,
        [0x5F] = &&L_0x5F,
        [0x60] = &&L_0x60,
        [0x61] = &&L_0x61,
        [0x62] = &&L_0x62,
        [0x63] = &&L_0x63,
        [0x64] = &&L_0x64,
        [0x65] = &&L_0x65,
        [0x66] = &&L_0x66,
#endif
        [0x67] = &&L_0x67,
        [0x68] = &&L_0x68,
        [0x69] = &&L_0x69,
        [0x6A] = &&L_0x6A,
        [0x6B] = &&L_0x6B,
        [0x6C] = &&L_0x6C,
        [0x6D] = &&L_0x6D,
        [0x6E] = &&L_0x6E,
        [0x6F] = &&L_0x6F,
        [0x70] = &&L_0x70,
        [0x71] = &&L_0x71,
        [0x72] = &&L_0x72,
        [0x73] = &&L_0x73,
        [0x74] = &&L_0x74,
        [0x75] = &&L_0x75,
        [0x76] = &&L_0x76,
        [0x77] = &&L_0x77,
        [0x78] = &&L_0x78,
        [0x79] = &&L_0x79,
        [0x7A] = &&L_0x7A,
        [0x7B] = &&L_0x7B,
        [0x7C] = &&L_0x7C,
        [0x7D] = &&L_0x7D,
        [0x7E] = &&L_0x7E,
        [0x7F] = &&L_0x7F,
        [0x80] = &&L_0x80,
        [0x81] = &&L_0x81,
        [0x82] = &&L_0x82,
        [0x83] = &&L_0x83,
        [0x84] = &&L_0x84,
        [0x85] = &&L_0x85,
        [0x86] = &&L_0x86,
        [0x87] = &&L_0x87,
        [0x88] = &&L_0x88,
        [0x89] = &&L_0x89,
        [0x8A] = &&L_0x8A,
#if !defined(ECCLES_WASM_NO_FLOAT)
        [0x8B] = &&L_0x8B,
        [0x8C] = &&L_0x8C,
        [0x8D] = &&L_0x8D,
        [0x8E] = &&L_0x8E,
        [0x8F] = &&L_0x8F,
        [0x90] = &&L_0x90,
        [0x91] = &&L_0x91,
        [0x92] = &&L_0x92,
        [0x93] = &&L_0x93,
        [0x94] = &&L_0x94,
        [0x95] = &&L_0x95,
        [0x96] = &&L_0x96,
        [0x97] = &&L_0x97,
        [0x98] = &&L_0x98,
        [0x99] = &&L_0x99,
        [0x9A] = &&L_0x9A,
        [0x9B] = &&L_0x9B,
        [0x9C] = &&L_0x9C,
        [0x9D] = &&L_0x9D,
        [0x9E] = &&L_0x9E,
        [0x9F] = &&L_0x9F,
        [0xA0] = &&L_0xA0,
        [0xA1] = &&L_0xA1,
        [0xA2] = &&L_0xA2,
        [0xA3] = &&L_0xA3,
        [0xA4] = &&L_0xA4,
        [0xA5] = &&L_0xA5,
        [0xA6] = &&L_0xA6,
#endif
        [0xA7] = &&L_0xA7,
        [0xAC] = &&L_0xAC,
        [0xAD] = &&L_0xAD,
        [0xC0] = &&L_0xC0,
        [0xC1] = &&L_0xC1,
        [0xC2] = &&L_0xC2,
        [0xC3] = &&L_0xC3,
        [0xC4] = &&L_0xC4,
#if !defined(ECCLES_WASM_NO_FLOAT)
        [0xA8] = &&L_0xA8,
        [0xA9] = &&L_0xA9,
        [0xAA] = &&L_0xAA,
        [0xAB] = &&L_0xAB,
        [0xAE] = &&L_0xAE,
        [0xAF] = &&L_0xAF,
        [0xB0] = &&L_0xB0,
        [0xB1] = &&L_0xB1,
        [0xB2] = &&L_0xB2,
        [0xB3] = &&L_0xB3,
        [0xB4] = &&L_0xB4,
        [0xB5] = &&L_0xB5,
        [0xB6] = &&L_0xB6,
        [0xB7] = &&L_0xB7,
        [0xB8] = &&L_0xB8,
        [0xB9] = &&L_0xB9,
        [0xBA] = &&L_0xBA,
        [0xBB] = &&L_0xBB,
        [0x100] = &&L_0x100,
        [0x101] = &&L_0x101,
        [0x102] = &&L_0x102,
        [0x103] = &&L_0x103,
        [0x104] = &&L_0x104,
        [0x105] = &&L_0x105,
        [0x106] = &&L_0x106,
        [0x107] = &&L_0x107,
#endif
        [OP_MOVE] = &&L_OP_MOVE,
        [OP_SETC] = &&L_OP_SETC,
        [OP_LLD32] = &&L_OP_LLD32,
        [OP_LLD8U] = &&L_OP_LLD8U,
        /* END GENERATED DISPATCH */
        W_FUSED_TABLE
    };
    NEXT;
    {
#else
    for (;;) {
        switch (*pc++) {
#endif
        CASE(0x00) TRAP(ECCLES_WASM_TRAP_UNREACHABLE);
        /* ---- control ---- */
        CASE(OP_BR)     { int32_t o = OFF32(pc); if (o < 0) TICK(); pc += o; NEXT; }
        CASE(OP_BR_FIX) { const uint16_t *s = pc; int32_t o = OFF32(s); uint32_t keep = s[2], nb = s[3];
                          FIX(keep, nb) if (o < 0) TICK(); pc = s + o; NEXT; }
        CASE(OP_BR_IF)  { const uint16_t *s = pc;
                          if ((uint32_t)*--sp) { int32_t o = OFF32(s); if (o < 0) TICK(); pc = s + o; } else pc += 2;
                          NEXT; }
        CASE(OP_BR_IF_FIX) { const uint16_t *s = pc;
                          if ((uint32_t)*--sp) { int32_t o = OFF32(s); uint32_t keep = s[2], nb = s[3];
                              FIX(keep, nb) if (o < 0) TICK(); pc = s + o; } else pc += 4;
                          NEXT; }
        CASE(OP_JZ)     { const uint16_t *s = pc; if (!(uint32_t)*--sp) pc = s + OFF32(s); else pc += 2; NEXT; }
        CASE(OP_BR_TABLE) {
            uint32_t n = pc[0], idx = (uint32_t)*--sp; const uint16_t *e; int32_t o; uint32_t keep, nb;
            if (idx > n) idx = n;
            e = pc + 1 + idx * 4u; o = OFF32(e); keep = e[2]; nb = e[3];
            FIX(keep, nb) if (o < 0) TICK(); pc = e + o; NEXT; }
        CASE(0x0F) do_return: {                                     /* return / function end */
            uint32_t nr = cur->nresults, k; uint64_t *src = sp - nr;
            for (k = 0; k < nr; k++) fp[k] = src[k];
            sp = fp + nr;
            if (fr == vm->frames) goto done;
            fr--; pc = fr->pc; fp = fr->fp; cur = fr->f;
            NEXT; }
        CASE(0x10) f = &funcs[IMM16()]; goto do_call;
        CASE(0x11) {
            uint32_t ti = IMM16(), tb = IMM16(), ix = (uint32_t)*--sp; const w_table_t *tab = &vm->tables[tb]; uint32_t e;
            if (ix >= tab->size) TRAP(ECCLES_WASM_TRAP_UNDEFINED_ELEMENT);
            e = tab->elems[ix];
            if (e == 0) TRAP(ECCLES_WASM_TRAP_UNDEFINED_ELEMENT);
            f = &funcs[e - 1];
            if (f->type != ti) TRAP(ECCLES_WASM_TRAP_INDIRECT_TYPE);
            goto do_call; }
        CASE(OP_RETCALL) f = &funcs[IMM16()]; goto do_tailcall;
        CASE(OP_RETCALL_IND) {
            uint32_t ti = IMM16(), tb = IMM16(), ix = (uint32_t)*--sp; const w_table_t *tab = &vm->tables[tb]; uint32_t e;
            if (ix >= tab->size) TRAP(ECCLES_WASM_TRAP_UNDEFINED_ELEMENT);
            e = tab->elems[ix];
            if (e == 0) TRAP(ECCLES_WASM_TRAP_UNDEFINED_ELEMENT);
            f = &funcs[e - 1];
            if (f->type != ti) TRAP(ECCLES_WASM_TRAP_INDIRECT_TYPE);
            goto do_tailcall; }
        /* ---- reference types / tables ---- */
        CASE(0x25) { const w_table_t *t = &vm->tables[IMM16()]; uint32_t i = (uint32_t)sp[-1];
            if (i >= t->size) TRAP(ECCLES_WASM_TRAP_TABLE_OOB);
            sp[-1] = t->elems[i]; NEXT; }
        CASE(0x26) { w_table_t *t = &vm->tables[IMM16()]; uint32_t i = (uint32_t)sp[-2];
            if (i >= t->size) TRAP(ECCLES_WASM_TRAP_TABLE_OOB);
            t->elems[i] = (uint32_t)sp[-1]; sp -= 2; NEXT; }
        CASE(OP_TABSIZE) *sp++ = vm->tables[IMM16()].size; NEXT;
        CASE(OP_TABGROW) { uint32_t ti = IMM16(), n = (uint32_t)sp[-1]; sp--; sp[-1] = (n == 0) ? vm->tables[ti].size : 0xFFFFFFFFu; NEXT; }   /* growth always fails (spec-legal) */
        CASE(OP_TABFILL) { w_table_t *t = &vm->tables[IMM16()]; uint64_t d = (uint32_t)sp[-3], n = (uint32_t)sp[-1], k; uint32_t v = (uint32_t)sp[-2];
            sp -= 3; if (d + n > t->size) TRAP(ECCLES_WASM_TRAP_TABLE_OOB);
            for (k = 0; k < n; k++) t->elems[d + k] = v;
            NEXT; }
        CASE(OP_TABCOPY) { w_table_t *dt = &vm->tables[IMM16()], *st = &vm->tables[IMM16()];
            uint64_t d = (uint32_t)sp[-3], so = (uint32_t)sp[-2], n = (uint32_t)sp[-1]; sp -= 3;
            if (d + n > dt->size || so + n > st->size) TRAP(ECCLES_WASM_TRAP_TABLE_OOB);
            if (n) memmove(dt->elems + d, st->elems + so, (size_t)n * 4u);
            NEXT; }
        CASE(OP_TABINIT) { const w_eseg_t *sg = &vm->eseg[IMM16()]; w_table_t *t = &vm->tables[IMM16()];
            uint64_t d = (uint32_t)sp[-3], so = (uint32_t)sp[-2], n = (uint32_t)sp[-1]; sp -= 3;
            if (so + n > sg->len || d + n > t->size) TRAP(ECCLES_WASM_TRAP_TABLE_OOB);
            if (n) memcpy(t->elems + d, sg->items + so, (size_t)n * 4u);
            NEXT; }
        CASE(OP_ELEMDROP) vm->eseg[IMM16()].len = 0; NEXT;
        CASE(0x1A) sp--; NEXT;
        CASE(0x1B) { uint32_t c = (uint32_t)sp[-1]; sp -= 2; if (!c) sp[-1] = sp[0]; NEXT; }
        /* ---- variables ---- */
        CASE(0x20) *sp++ = fp[IMM16()]; NEXT;
        CASE(0x21) fp[IMM16()] = *--sp; NEXT;
        CASE(0x22) fp[IMM16()] = sp[-1]; NEXT;
        CASE(0x23) *sp++ = vm->globals[IMM16()]; NEXT;
        CASE(0x24) vm->globals[IMM16()] = *--sp; NEXT;
        /* ---- memory ---- */
        CASE(0x28) CASE(0x2A) { MEMOFF(4); uint32_t v; memcpy(&v, mem + ea, 4); sp[-1] = le32(v); NEXT; }
        CASE(0x29) CASE(0x2B) { MEMOFF(8); uint64_t v; memcpy(&v, mem + ea, 8); sp[-1] = le64(v); NEXT; }
        CASE(0x2C) { MEMOFF(1); sp[-1] = (uint32_t)(int32_t)(int8_t)mem[ea]; NEXT; }
        CASE(0x2D) { MEMOFF(1); sp[-1] = mem[ea]; NEXT; }
        CASE(0x2E) { MEMOFF(2); uint16_t v; memcpy(&v, mem + ea, 2); sp[-1] = (uint32_t)(int32_t)(int16_t)le16(v); NEXT; }
        CASE(0x2F) { MEMOFF(2); uint16_t v; memcpy(&v, mem + ea, 2); sp[-1] = le16(v); NEXT; }
        CASE(0x30) { MEMOFF(1); sp[-1] = (uint64_t)(int64_t)(int8_t)mem[ea]; NEXT; }
        CASE(0x31) { MEMOFF(1); sp[-1] = mem[ea]; NEXT; }
        CASE(0x32) { MEMOFF(2); uint16_t v; memcpy(&v, mem + ea, 2); sp[-1] = (uint64_t)(int64_t)(int16_t)le16(v); NEXT; }
        CASE(0x33) { MEMOFF(2); uint16_t v; memcpy(&v, mem + ea, 2); sp[-1] = le16(v); NEXT; }
        CASE(0x34) { MEMOFF(4); uint32_t v; memcpy(&v, mem + ea, 4); sp[-1] = (uint64_t)(int64_t)(int32_t)le32(v); NEXT; }
        CASE(0x35) { MEMOFF(4); uint32_t v; memcpy(&v, mem + ea, 4); sp[-1] = le32(v); NEXT; }
        CASE(0x36) CASE(0x38) { STOREP(4); uint32_t v = le32((uint32_t)val); memcpy(mem + ea, &v, 4); NEXT; }
        CASE(0x37) CASE(0x39) { STOREP(8); uint64_t v = le64(val); memcpy(mem + ea, &v, 8); NEXT; }
        CASE(0x3A) CASE(0x3C) { STOREP(1); mem[ea] = (uint8_t)val; NEXT; }
        CASE(0x3B) CASE(0x3D) { STOREP(2); uint16_t v = le16((uint16_t)val); memcpy(mem + ea, &v, 2); NEXT; }
        CASE(0x3E) { STOREP(4); uint32_t v = le32((uint32_t)val); memcpy(mem + ea, &v, 4); NEXT; }
        CASE(0x3F) *sp++ = vm->mem_pages; NEXT;
        CASE(0x40) sp[-1] = ((uint32_t)sp[-1] == 0) ? vm->mem_pages : 0xFFFFFFFFu; NEXT;
        CASE(OP_MEMCOPY) {
            uint64_t d = (uint32_t)sp[-3], s = (uint32_t)sp[-2], n = (uint32_t)sp[-1]; sp -= 3;
            if (d + n > memsz || s + n > memsz) TRAP(ECCLES_WASM_TRAP_MEM_OOB);
            if (n) memmove(mem + d, mem + s, (size_t)n);
            NEXT; }
        CASE(OP_MEMFILL) {
            uint64_t d = (uint32_t)sp[-3], n = (uint32_t)sp[-1]; uint8_t v = (uint8_t)sp[-2]; sp -= 3;
            if (d + n > memsz) TRAP(ECCLES_WASM_TRAP_MEM_OOB);
            if (n) memset(mem + d, v, (size_t)n);
            NEXT; }
        CASE(OP_MEMINIT) {
            uint32_t di = IMM16(); uint64_t d = (uint32_t)sp[-3], so = (uint32_t)sp[-2], n = (uint32_t)sp[-1]; sp -= 3;
            if (so + n > vm->segs[di].len || d + n > memsz) TRAP(ECCLES_WASM_TRAP_MEM_OOB);
            if (n) memcpy(mem + d, vm->segs[di].p + so, (size_t)n);
            NEXT; }
        CASE(OP_DATADROP) vm->segs[IMM16()].len = 0; NEXT;
        /* ---- constants ---- */
        CASE(0x41) CASE(0x43) *sp++ = IMM32(); NEXT;
        CASE(0x42) CASE(0x44) { uint64_t lo = IMM32(); uint64_t hi = IMM32(); *sp++ = lo | (hi << 32); NEXT; }
        /* ---- i32 compare ---- */
        CASE(0x45) UN32(a == 0)
        CASE(0x46) BIN32(a == b)
        CASE(0x47) BIN32(a != b)
        CASE(0x48) BIN32((int32_t)a < (int32_t)b)
        CASE(0x49) BIN32(a < b)
        CASE(0x4A) BIN32((int32_t)a > (int32_t)b)
        CASE(0x4B) BIN32(a > b)
        CASE(0x4C) BIN32((int32_t)a <= (int32_t)b)
        CASE(0x4D) BIN32(a <= b)
        CASE(0x4E) BIN32((int32_t)a >= (int32_t)b)
        CASE(0x4F) BIN32(a >= b)
        /* ---- i64 compare ---- */
        CASE(0x50) UN64(a == 0)
        CASE(0x51) BIN64(a == b)
        CASE(0x52) BIN64(a != b)
        CASE(0x53) BIN64((int64_t)a < (int64_t)b)
        CASE(0x54) BIN64(a < b)
        CASE(0x55) BIN64((int64_t)a > (int64_t)b)
        CASE(0x56) BIN64(a > b)
        CASE(0x57) BIN64((int64_t)a <= (int64_t)b)
        CASE(0x58) BIN64(a <= b)
        CASE(0x59) BIN64((int64_t)a >= (int64_t)b)
        CASE(0x5A) BIN64(a >= b)
#if !defined(ECCLES_WASM_NO_FLOAT)
        CASE(0x5B) FCMP32(a == b)
        CASE(0x5C) FCMP32(a != b)
        CASE(0x5D) FCMP32(a < b)
        CASE(0x5E) FCMP32(a > b)
        CASE(0x5F) FCMP32(a <= b)
        CASE(0x60) FCMP32(a >= b)
        CASE(0x61) FCMP64(a == b)
        CASE(0x62) FCMP64(a != b)
        CASE(0x63) FCMP64(a < b)
        CASE(0x64) FCMP64(a > b)
        CASE(0x65) FCMP64(a <= b)
        CASE(0x66) FCMP64(a >= b)
#endif
        /* ---- i32 arithmetic ---- */
        CASE(0x67) UN32(w_clz32(a))
        CASE(0x68) UN32(w_ctz32(a))
        CASE(0x69) UN32(w_pop32(a))
        CASE(0x6A) BIN32(a + b)
        CASE(0x6B) BIN32(a - b)
        CASE(0x6C) BIN32(a * b)
        CASE(0x6D) { int32_t b = (int32_t)sp[-1], a = (int32_t)sp[-2];
            if (!b) TRAP(ECCLES_WASM_TRAP_DIV_ZERO);
            if (a == INT32_MIN && b == -1) TRAP(ECCLES_WASM_TRAP_INT_OVERFLOW);
            sp--; sp[-1] = (uint32_t)(a / b); NEXT; }
        CASE(0x6E) { uint32_t b = (uint32_t)sp[-1], a = (uint32_t)sp[-2];
            if (!b) TRAP(ECCLES_WASM_TRAP_DIV_ZERO);
            sp--; sp[-1] = a / b; NEXT; }
        CASE(0x6F) { int32_t b = (int32_t)sp[-1], a = (int32_t)sp[-2];
            if (!b) TRAP(ECCLES_WASM_TRAP_DIV_ZERO);
            sp--; sp[-1] = (b == -1) ? 0u : (uint32_t)(a % b); NEXT; }
        CASE(0x70) { uint32_t b = (uint32_t)sp[-1], a = (uint32_t)sp[-2];
            if (!b) TRAP(ECCLES_WASM_TRAP_DIV_ZERO);
            sp--; sp[-1] = a % b; NEXT; }
        CASE(0x71) BIN32(a & b)
        CASE(0x72) BIN32(a | b)
        CASE(0x73) BIN32(a ^ b)
        CASE(0x74) BIN32(a << (b & 31))
        CASE(0x75) BIN32((uint32_t)((int32_t)a >> (b & 31)))
        CASE(0x76) BIN32(a >> (b & 31))
        CASE(0x77) BIN32((a << (b & 31)) | (a >> ((32 - (b & 31)) & 31)))
        CASE(0x78) BIN32((a >> (b & 31)) | (a << ((32 - (b & 31)) & 31)))
        /* ---- i64 arithmetic ---- */
        CASE(0x79) UN64(w_clz64(a))
        CASE(0x7A) UN64(w_ctz64(a))
        CASE(0x7B) UN64(w_pop64(a))
        CASE(0x7C) BIN64(a + b)
        CASE(0x7D) BIN64(a - b)
        CASE(0x7E) BIN64(a * b)
        CASE(0x7F) { int64_t b = (int64_t)sp[-1], a = (int64_t)sp[-2];
            if (!b) TRAP(ECCLES_WASM_TRAP_DIV_ZERO);
            if (a == INT64_MIN && b == -1) TRAP(ECCLES_WASM_TRAP_INT_OVERFLOW);
            sp--; sp[-1] = (uint64_t)(a / b); NEXT; }
        CASE(0x80) { uint64_t b = sp[-1], a = sp[-2];
            if (!b) TRAP(ECCLES_WASM_TRAP_DIV_ZERO);
            sp--; sp[-1] = a / b; NEXT; }
        CASE(0x81) { int64_t b = (int64_t)sp[-1], a = (int64_t)sp[-2];
            if (!b) TRAP(ECCLES_WASM_TRAP_DIV_ZERO);
            sp--; sp[-1] = (b == -1) ? 0u : (uint64_t)(a % b); NEXT; }
        CASE(0x82) { uint64_t b = sp[-1], a = sp[-2];
            if (!b) TRAP(ECCLES_WASM_TRAP_DIV_ZERO);
            sp--; sp[-1] = a % b; NEXT; }
        CASE(0x83) BIN64(a & b)
        CASE(0x84) BIN64(a | b)
        CASE(0x85) BIN64(a ^ b)
        CASE(0x86) BIN64(a << (b & 63))
        CASE(0x87) BIN64((uint64_t)((int64_t)a >> (b & 63)))
        CASE(0x88) BIN64(a >> (b & 63))
        CASE(0x89) BIN64((a << (b & 63)) | (a >> ((64 - (b & 63)) & 63)))
        CASE(0x8A) BIN64((a >> (b & 63)) | (a << ((64 - (b & 63)) & 63)))
#if !defined(ECCLES_WASM_NO_FLOAT)
        /* ---- f32 ---- */
        CASE(0x8B) sp[-1] = (uint32_t)sp[-1] & 0x7FFFFFFFu; NEXT;
        CASE(0x8C) sp[-1] = (uint32_t)sp[-1] ^ 0x80000000u; NEXT;
        CASE(0x8D) FUN32(f32_ceil(a))
        CASE(0x8E) FUN32(f32_floor(a))
        CASE(0x8F) FUN32(f32_trunc(a))
        CASE(0x90) FUN32(f32_nearest(a))
        CASE(0x91) FUN32(sqrtf(a))
        CASE(0x92) FBIN32(a + b)
        CASE(0x93) FBIN32(a - b)
        CASE(0x94) FBIN32(a * b)
        CASE(0x95) FBIN32(a / b)
        CASE(0x96) FBIN32(f32_min(a, b))
        CASE(0x97) FBIN32(f32_max(a, b))
        CASE(0x98) { uint32_t b = (uint32_t)sp[-1], a = (uint32_t)sp[-2]; sp--; sp[-1] = (a & 0x7FFFFFFFu) | (b & 0x80000000u); NEXT; }
        /* ---- f64 ---- */
        CASE(0x99) sp[-1] &= 0x7FFFFFFFFFFFFFFFull; NEXT;
        CASE(0x9A) sp[-1] ^= 0x8000000000000000ull; NEXT;
        CASE(0x9B) FUN64(f64_ceil(a))
        CASE(0x9C) FUN64(f64_floor(a))
        CASE(0x9D) FUN64(f64_trunc(a))
        CASE(0x9E) FUN64(f64_nearest(a))
        CASE(0x9F) FUN64(sqrt(a))
        CASE(0xA0) FBIN64(a + b)
        CASE(0xA1) FBIN64(a - b)
        CASE(0xA2) FBIN64(a * b)
        CASE(0xA3) FBIN64(a / b)
        CASE(0xA4) FBIN64(f64_min(a, b))
        CASE(0xA5) FBIN64(f64_max(a, b))
        CASE(0xA6) { uint64_t b = sp[-1], a = sp[-2]; sp--; sp[-1] = (a & 0x7FFFFFFFFFFFFFFFull) | (b & 0x8000000000000000ull); NEXT; }
#endif
        /* ---- conversions ---- */
        CASE(0xA7) UN64((uint32_t)a)
        CASE(0xAC) sp[-1] = (uint64_t)(int64_t)(int32_t)(uint32_t)sp[-1]; NEXT;
        CASE(0xAD) sp[-1] = (uint32_t)sp[-1]; NEXT;
        CASE(0xC0) sp[-1] = (uint32_t)(int32_t)(int8_t)(uint8_t)sp[-1]; NEXT;
        CASE(0xC1) sp[-1] = (uint32_t)(int32_t)(int16_t)(uint16_t)sp[-1]; NEXT;
        CASE(0xC2) sp[-1] = (uint64_t)(int64_t)(int8_t)(uint8_t)sp[-1]; NEXT;
        CASE(0xC3) sp[-1] = (uint64_t)(int64_t)(int16_t)(uint16_t)sp[-1]; NEXT;
        CASE(0xC4) sp[-1] = (uint64_t)(int64_t)(int32_t)(uint32_t)sp[-1]; NEXT;
#if !defined(ECCLES_WASM_NO_FLOAT)
        CASE(0xA8) CASE(0xA9) CASE(0xAA) CASE(0xAB) {
            double d = (pc[-1] < 0xAA) ? (double)bf32((uint32_t)sp[-1]) : bf64(sp[-1]);
            if (d != d) TRAP(ECCLES_WASM_TRAP_BAD_CONVERSION);
            if (pc[-1] & 1) { if (d <= -1.0 || d >= 4294967296.0) TRAP(ECCLES_WASM_TRAP_INT_OVERFLOW); sp[-1] = (uint32_t)d; }
            else { if (d <= -2147483649.0 || d >= 2147483648.0) TRAP(ECCLES_WASM_TRAP_INT_OVERFLOW); sp[-1] = (uint32_t)(int32_t)d; }
            NEXT; }
        CASE(0xAE) CASE(0xAF) CASE(0xB0) CASE(0xB1) {
            double d = (pc[-1] < 0xB0) ? (double)bf32((uint32_t)sp[-1]) : bf64(sp[-1]);
            if (d != d) TRAP(ECCLES_WASM_TRAP_BAD_CONVERSION);
            if (pc[-1] & 1) { if (d <= -1.0 || d >= 18446744073709551616.0) TRAP(ECCLES_WASM_TRAP_INT_OVERFLOW); sp[-1] = (uint64_t)d; }
            else { if (d < -9223372036854775808.0 || d >= 9223372036854775808.0) TRAP(ECCLES_WASM_TRAP_INT_OVERFLOW); sp[-1] = (uint64_t)(int64_t)d; }
            NEXT; }
        CASE(0xB2) sp[-1] = fb32((float)(int32_t)(uint32_t)sp[-1]); NEXT;
        CASE(0xB3) sp[-1] = fb32((float)(uint32_t)sp[-1]); NEXT;
        CASE(0xB4) sp[-1] = fb32((float)(int64_t)sp[-1]); NEXT;
        CASE(0xB5) sp[-1] = fb32((float)sp[-1]); NEXT;
        CASE(0xB6) sp[-1] = fb32((float)bf64(sp[-1])); NEXT;
        CASE(0xB7) sp[-1] = fb64((double)(int32_t)(uint32_t)sp[-1]); NEXT;
        CASE(0xB8) sp[-1] = fb64((double)(uint32_t)sp[-1]); NEXT;
        CASE(0xB9) sp[-1] = fb64((double)(int64_t)sp[-1]); NEXT;
        CASE(0xBA) sp[-1] = fb64((double)sp[-1]); NEXT;
        CASE(0xBB) sp[-1] = fb64((double)bf32((uint32_t)sp[-1])); NEXT;
        CASE(0x100) CASE(0x101) CASE(0x102) CASE(0x103)
        CASE(0x104) CASE(0x105) CASE(0x106) CASE(0x107) {
            uint32_t sub = pc[-1] - OP_SAT0; uint64_t r;
            double d = (sub & 2) ? bf64(sp[-1]) : (double)bf32((uint32_t)sp[-1]);
            if (d != d) r = 0;
            else if (sub & 1) {
                if (sub < 4) r = d <= 0 ? 0u : d >= 4294967296.0 ? 0xFFFFFFFFu : (uint32_t)d;
                else r = d <= 0 ? 0u : d >= 18446744073709551616.0 ? ~(uint64_t)0 : (uint64_t)d;
            } else {
                if (sub < 4) r = (uint32_t)(d <= -2147483648.0 ? INT32_MIN : d >= 2147483648.0 ? INT32_MAX : (int32_t)d);
                else r = (uint64_t)(d <= -9223372036854775808.0 ? INT64_MIN : d >= 9223372036854775808.0 ? INT64_MAX : (int64_t)d);
            }
            sp[-1] = r; NEXT; }
#endif

        /* ---- fused instructions (generated from W_*_LIST; see the notes at the top of the file) ---- */
#define IMM32AT(q) ((uint32_t)(q)[0] | ((uint32_t)(q)[1] << 16))
#define X(n, o, e) \
        CASE(FLL_##n) { uint32_t a = (uint32_t)fp[pc[0]], b = (uint32_t)fp[pc[1]]; pc += 2; *sp++ = (uint32_t)(e); NEXT; } \
        CASE(FLC_##n) { uint32_t a = (uint32_t)fp[pc[0]], b = IMM32AT(pc + 1); pc += 3; *sp++ = (uint32_t)(e); NEXT; } \
        CASE(FSC_##n) { uint32_t a = (uint32_t)sp[-1], b = IMM32AT(pc); pc += 2; sp[-1] = (uint32_t)(e); NEXT; } \
        CASE(FSL_##n) { uint32_t a = (uint32_t)sp[-1], b = (uint32_t)fp[pc[0]]; pc += 1; sp[-1] = (uint32_t)(e); NEXT; }
        W_FUSE_LIST(X)
#undef X
#define X(n, o, e) \
        CASE(FLLS_##n) { uint32_t a = (uint32_t)fp[pc[0]], b = (uint32_t)fp[pc[1]]; fp[pc[2]] = (uint32_t)(e); pc += 3; NEXT; } \
        CASE(FLCS_##n) { uint32_t a = (uint32_t)fp[pc[0]], b = IMM32AT(pc + 1); fp[pc[3]] = (uint32_t)(e); pc += 4; NEXT; }
        W_ALU_LIST(X)
#undef X
#define X(n, o, e) \
        CASE(BSS_##n) { uint32_t b = (uint32_t)sp[-1], a = (uint32_t)sp[-2]; const uint16_t *s_ = pc; sp -= 2; \
            if (e) { int32_t o_ = OFF32(s_); if (o_ < 0) TICK(); pc = s_ + o_; } else pc += 2; NEXT; } \
        CASE(BLC_##n) { uint32_t a = (uint32_t)fp[pc[0]], b = IMM32AT(pc + 1); const uint16_t *s_ = pc + 3; \
            if (e) { int32_t o_ = OFF32(s_); if (o_ < 0) TICK(); pc = s_ + o_; } else pc += 5; NEXT; } \
        CASE(BLL_##n) { uint32_t a = (uint32_t)fp[pc[0]], b = (uint32_t)fp[pc[1]]; const uint16_t *s_ = pc + 2; \
            if (e) { int32_t o_ = OFF32(s_); if (o_ < 0) TICK(); pc = s_ + o_; } else pc += 4; NEXT; }
        W_CMP_LIST(X)
#undef X
        CASE(OP_MOVE) { fp[pc[1]] = fp[pc[0]]; pc += 2; NEXT; }
        CASE(OP_SETC) { fp[pc[0]] = IMM32AT(pc + 1); pc += 3; NEXT; }
        CASE(OP_LLD32) { uint64_t ea = (uint64_t)(uint32_t)fp[pc[0]] + IMM32AT(pc + 1); uint32_t v; pc += 3;
            if (W_UNLIKELY(ea + 4 > memsz)) TRAP(ECCLES_WASM_TRAP_MEM_OOB);
            memcpy(&v, mem + ea, 4); *sp++ = le32(v); NEXT; }
        CASE(OP_LLD8U) { uint64_t ea = (uint64_t)(uint32_t)fp[pc[0]] + IMM32AT(pc + 1); pc += 3;
            if (W_UNLIKELY(ea + 1 > memsz)) TRAP(ECCLES_WASM_TRAP_MEM_OOB);
            *sp++ = mem[ea]; NEXT; }
        CASE_DEFAULT TRAP(ECCLES_WASM_TRAP_UNREACHABLE);
#if !W_CG
        }
        continue;
#endif

    do_tailcall:                                            /* return_call / return_call_indirect: reuse the frame */
        if (!f->code) {                                     /* host callee: call it, then return its results */
            uint64_t *args = sp - f->nparams;
            if (f->host(vm, args) != ECCLES_WASM_SUCCESSFUL) TRAP(ECCLES_WASM_TRAP_HOST);
            sp = args + f->nresults;
            goto do_return;
        }
        TICK();
        {
            uint64_t *src = sp - f->nparams, *ze; uint32_t k;
            if (fp + f->max_h > stk_end) TRAP(ECCLES_WASM_TRAP_STACK_OVERFLOW);
            for (k = 0; k < f->nparams; k++) fp[k] = src[k];
            cur = f; pc = f->code;
            sp = fp + f->nparams; ze = sp + f->nlocals;
            while (sp < ze) *sp++ = 0;
        }
        W_REDISPATCH;

    do_call:
        if (!f->code) {                                         /* host function */
            uint64_t *args = sp - f->nparams;
            if (f->host(vm, args) != ECCLES_WASM_SUCCESSFUL) TRAP(ECCLES_WASM_TRAP_HOST);
            sp = args + f->nresults;
            W_REDISPATCH;
        }
        TICK();
        if (fr == frend) TRAP(ECCLES_WASM_TRAP_CALL_DEPTH);
        {
            uint64_t *nfp = sp - f->nparams, *ze;
            if (nfp + f->max_h > stk_end) TRAP(ECCLES_WASM_TRAP_STACK_OVERFLOW);
            fr->pc = pc; fr->fp = fp; fr->f = cur; fr++;
            fp = nfp; cur = f; pc = f->code;
            ze = fp + f->nparams + f->nlocals;
            while (sp < ze) *sp++ = 0;
        }
        W_REDISPATCH;
#if !W_CG
    }
#else
    }
#endif
done:
    return ECCLES_WASM_OK;
trap:
    return err;
}
#if W_CG && defined(__GNUC__)
  #pragma GCC diagnostic pop
#endif

/* ===================================================================== *
 *  5. public runtime API
 * ===================================================================== */

static void w_clear_err(eccles_wasm_vm_t *vm) { vm->err = ECCLES_WASM_OK; vm->err_info = 0; vm->err_str[0] = 0; }

static eccles_wasm_status_t w_status_of(eccles_wasm_error_t e) {
    if (e == ECCLES_WASM_OK) return ECCLES_WASM_SUCCESSFUL;
    if (e == ECCLES_WASM_ERR_UNSUPPORTED_FEATURE || e == ECCLES_WASM_ERR_IMPORT || e == ECCLES_WASM_ERR_NO_FS)
        return ECCLES_WASM_UNSUPPORTED;
    return ECCLES_WASM_FAILED;
}

static void w_module_reset(eccles_wasm_vm_t *vm) {
    vm->ntypes = vm->nfuncs = vm->nimports = vm->nglobals = vm->nexports = 0; vm->nglobals_imp = 0; vm->ntables = 0; vm->neseg = 0; vm->tables = NULL; vm->eseg = NULL;
    vm->start = 0xFFFFu;
    vm->has_mem = vm->code_seen = vm->loaded = vm->exit_req = 0;
    vm->types = NULL; vm->type_off = vm->type_canon = NULL; vm->funcs = NULL; vm->globals = NULL;
    vm->exports = NULL; vm->mem = NULL; vm->segs = NULL; vm->nseg = 0;
    vm->mem_bytes = vm->mem_pages = vm->code_bytes = 0;
    vm->lo = vm->base; vm->hi = vm->hi_init;
}

eccles_wasm_vm_t *eccles_wasm_vm_create(void *mem, size_t size) {
    uintptr_t b = ((uintptr_t)mem + 7u) & ~(uintptr_t)7u;
    size_t adj = (size_t)(b - (uintptr_t)mem), hdr = (sizeof(struct eccles_wasm_vm_s) + 7u) & ~(size_t)7u;
    eccles_wasm_vm_t *vm;
    if (!mem || size < adj + hdr + 4096u) return NULL;
    vm = (eccles_wasm_vm_t *)b;
    memset(vm, 0, sizeof *vm);
    size = (size - adj - hdr) & ~(size_t)7u;
    vm->base = (uint8_t *)b + hdr; vm->end = vm->base + size; vm->lo = vm->base; vm->hi = vm->end;
    vm->stk = (uint64_t *)w_ahi(vm, ECCLES_WASM_VALUE_STACK_SLOTS * sizeof(uint64_t), 8);
    vm->frames = (w_frame_t *)w_ahi(vm, ECCLES_WASM_CALL_DEPTH * sizeof(w_frame_t), 8);
    if (vm->err) return NULL;
    vm->hi_init = vm->hi;
    w_module_reset(vm);
    vm->inited = 1;
    return vm;
}

#if ECCLES_WASM_USE_HEAP
eccles_wasm_vm_t *eccles_wasm_vm_create_heap(size_t size) {
    void *m = malloc(size); eccles_wasm_vm_t *vm;
    if (!m) return NULL;
    vm = eccles_wasm_vm_create(m, size);
    if (!vm) { free(m); return NULL; }
    vm->heap_block = m;
    return vm;
}
#endif
void eccles_wasm_vm_destroy(eccles_wasm_vm_t *vm) {
#if ECCLES_WASM_USE_HEAP
    if (vm && vm->heap_block) { void *m = vm->heap_block; vm->inited = 0; free(m); }
#else
    if (vm) vm->inited = 0;
#endif
}
void eccles_wasm_vm_set_user(eccles_wasm_vm_t *vm, void *user) { vm->user = user; }
void *eccles_wasm_vm_user(const eccles_wasm_vm_t *vm) { return vm->user; }

eccles_wasm_status_t eccles_wasm_vm_unload(eccles_wasm_vm_t *vm) {
    if (vm->inited) w_module_reset(vm);
    return ECCLES_WASM_SUCCESSFUL;
}

static eccles_wasm_error_t w_invoke(eccles_wasm_vm_t *vm, const w_func_t *f, const uint64_t *args, uint32_t nargs,
                                    uint64_t *res, uint32_t maxres) {
    eccles_wasm_error_t e; uint32_t i;
    if (vm->running) return ECCLES_WASM_ERR_BUSY;
    for (i = 0; i < nargs; i++) vm->stk[i] = args[i];
    vm->running = 1; vm->exit_req = 0;
    if (!f->code) e = (f->host(vm, vm->stk) == ECCLES_WASM_SUCCESSFUL) ? ECCLES_WASM_OK : ECCLES_WASM_TRAP_HOST;
    else e = w_exec(vm, f);
    vm->running = 0;
    if (e == ECCLES_WASM_TRAP_HOST && vm->exit_req) e = ECCLES_WASM_OK;       /* WASI proc_exit */
    if (e == ECCLES_WASM_OK && res && !vm->exit_req)
        for (i = 0; i < f->nresults && i < maxres; i++) res[i] = vm->stk[i];
    return e;
}

static eccles_wasm_status_t w_finish_load(eccles_wasm_vm_t *vm) {
    vm->rd.fn = NULL; vm->rd.ctx = NULL;
    if (!vm->err && !vm->loaded) {
        vm->loaded = 1;
#if ECCLES_WASM_AUTORUN_START
        if (vm->start != 0xFFFFu) {
            eccles_wasm_error_t e = w_invoke(vm, &vm->funcs[vm->start], NULL, 0, NULL, 0);
            if (e != ECCLES_WASM_OK) w_fail(vm, e, 0);
        }
#endif
    }
    if (vm->err) {
        ECCLES_WASM_LOG_LINE(eccles_wasm_error_name(vm->err));
        vm->loaded = 0;
        { eccles_wasm_error_t keep = vm->err; uint32_t info = vm->err_info; char s[ECCLES_WASM_MAX_NAME + 1];
          memcpy(s, vm->err_str, sizeof s); w_module_reset(vm);
          vm->err = keep; vm->err_info = info; memcpy(vm->err_str, s, sizeof s); }
        return w_status_of(vm->err);
    }
    return ECCLES_WASM_SUCCESSFUL;
}

static eccles_wasm_status_t w_begin_load(eccles_wasm_vm_t *vm) {
    if (!vm->inited) return ECCLES_WASM_FAILED;
    if (vm->running) { w_clear_err(vm); w_fail(vm, ECCLES_WASM_ERR_BUSY, 0); return ECCLES_WASM_FAILED; }
    w_clear_err(vm); w_module_reset(vm);
    vm->rd.eof = 0; vm->rd.pos0 = 0;
    return ECCLES_WASM_SUCCESSFUL;
}

eccles_wasm_status_t eccles_wasm_vm_load_buffer(eccles_wasm_vm_t *vm, const void *wasm, size_t len) {
    if (w_begin_load(vm) != ECCLES_WASM_SUCCESSFUL) return ECCLES_WASM_FAILED;
    if (!wasm || !len) { w_fail(vm, ECCLES_WASM_ERR_BAD_ARG, 0); return ECCLES_WASM_FAILED; }
    vm->rd.mem_src = 1; vm->rd.fn = NULL; vm->rd.start = vm->rd.p = (const uint8_t *)wasm; vm->rd.e = vm->rd.p + len;
    w_load_module(vm);
    return w_finish_load(vm);
}

eccles_wasm_status_t eccles_wasm_vm_load_stream(eccles_wasm_vm_t *vm, eccles_wasm_read_fn read, void *ctx) {
    if (w_begin_load(vm) != ECCLES_WASM_SUCCESSFUL) return ECCLES_WASM_FAILED;
    if (!read) { w_fail(vm, ECCLES_WASM_ERR_BAD_ARG, 0); return ECCLES_WASM_FAILED; }
    vm->rd.mem_src = 0; vm->rd.fn = read; vm->rd.ctx = ctx; vm->rd.start = vm->rd.p = vm->rd.e = vm->rd.chunk;
    w_load_module(vm);
    return w_finish_load(vm);
}

/* ---- file systems ---- */
#if ECCLES_WASM_FS == ECCLES_WASM_FS_STDIO
  #include <stdio.h>
  static void *w_fs_open(const char *p) { return (void *)fopen(p, "rb"); }
  static int w_fs_read(void *f, uint8_t *b, size_t n) {
      size_t r = fread(b, 1, n, (FILE *)f);
      return (r == 0 && ferror((FILE *)f)) ? -1 : (int)r;
  }
  static void w_fs_close(void *f) { fclose((FILE *)f); }
  #define W_HAVE_FS 1
#elif ECCLES_WASM_FS == ECCLES_WASM_FS_FATFS
  #include "ff.h"
  static FIL g_fil;
  static void *w_fs_open(const char *p) { return f_open(&g_fil, p, FA_READ) == FR_OK ? (void *)&g_fil : NULL; }
  static int w_fs_read(void *f, uint8_t *b, size_t n) { UINT br = 0; return f_read((FIL *)f, b, (UINT)n, &br) == FR_OK ? (int)br : -1; }
  static void w_fs_close(void *f) { (void)f_close((FIL *)f); }
  #define W_HAVE_FS 1
#elif ECCLES_WASM_FS == ECCLES_WASM_FS_LITTLEFS
  #include "lfs.h"
  static lfs_file_t g_lfile;
  static void *w_fs_open(const char *p) { return lfs_file_open(ECCLES_WASM_LFS_PTR, &g_lfile, p, LFS_O_RDONLY) >= 0 ? (void *)&g_lfile : NULL; }
  static int w_fs_read(void *f, uint8_t *b, size_t n) { return (int)lfs_file_read(ECCLES_WASM_LFS_PTR, (lfs_file_t *)f, b, (lfs_size_t)n); }
  static void w_fs_close(void *f) { (void)lfs_file_close(ECCLES_WASM_LFS_PTR, (lfs_file_t *)f); }
  #define W_HAVE_FS 1
#elif ECCLES_WASM_FS == ECCLES_WASM_FS_ZEPHYR
  #include <zephyr/fs/fs.h>
  static struct fs_file_t g_zfile;
  static void *w_fs_open(const char *p) { fs_file_t_init(&g_zfile); return fs_open(&g_zfile, p, FS_O_READ) == 0 ? (void *)&g_zfile : NULL; }
  static int w_fs_read(void *f, uint8_t *b, size_t n) { return (int)fs_read((struct fs_file_t *)f, b, n); }
  static void w_fs_close(void *f) { (void)fs_close((struct fs_file_t *)f); }
  #define W_HAVE_FS 1
#endif

static int w_fs_trampoline(void *ctx, uint8_t *buf, size_t max) { eccles_wasm_vm_t *v = (eccles_wasm_vm_t *)ctx; return v->fs_rd(v->fs_h, buf, max); }

void eccles_wasm_set_fs(eccles_wasm_fs_open_fn o, eccles_wasm_fs_read_fn r, eccles_wasm_fs_close_fn c) {
    g_fs_open = o; g_fs_read = r; g_fs_close = c;
}

eccles_wasm_status_t eccles_wasm_vm_load(eccles_wasm_vm_t *vm, const char *path) {
    eccles_wasm_fs_open_fn o = g_fs_open; eccles_wasm_fs_read_fn r = g_fs_read; eccles_wasm_fs_close_fn c = g_fs_close;
    void *h; eccles_wasm_status_t st;
    w_clear_err(vm);
    if (!path) { w_fail(vm, ECCLES_WASM_ERR_BAD_ARG, 0); return ECCLES_WASM_FAILED; }
    if (!o || !r) {
#if defined(W_HAVE_FS)
        o = w_fs_open; r = w_fs_read; c = w_fs_close;
#else
        w_fail(vm, ECCLES_WASM_ERR_NO_FS, 0); return ECCLES_WASM_UNSUPPORTED;
#endif
    }
    h = o(path);
    if (!h) { w_fail(vm, ECCLES_WASM_ERR_IO, 0); return ECCLES_WASM_FAILED; }
    vm->fs_rd = r; vm->fs_h = h;
    st = eccles_wasm_vm_load_stream(vm, w_fs_trampoline, vm);
    if (c) c(h);
    return st;
}

/* ---- calling into the guest ---- */
bool eccles_wasm_vm_has_export(eccles_wasm_vm_t *vm, const char *name) {
    uint32_t i;
    if (!vm->loaded || !name) return false;
    for (i = 0; i < vm->nexports; i++) if (strcmp(vm->exports[i].name, name) == 0) return true;
    return false;
}

eccles_wasm_status_t eccles_wasm_vm_call(eccles_wasm_vm_t *vm, const char *name, const uint64_t *args, uint32_t nargs,
                                      uint64_t *results, uint32_t max_results) {
    uint32_t i; const w_func_t *f = NULL; eccles_wasm_error_t e;
    if (vm->running) { w_fail(vm, ECCLES_WASM_ERR_BUSY, 0); return ECCLES_WASM_FAILED; }
    w_clear_err(vm);
    if (!vm->loaded) { w_fail(vm, ECCLES_WASM_ERR_NOT_LOADED, 0); return ECCLES_WASM_FAILED; }
    if (!name || (nargs && !args)) { w_fail(vm, ECCLES_WASM_ERR_BAD_ARG, 0); return ECCLES_WASM_FAILED; }
    for (i = 0; i < vm->nexports; i++) if (strcmp(vm->exports[i].name, name) == 0) { f = &vm->funcs[vm->exports[i].func]; break; }
    if (!f) { w_fail(vm, ECCLES_WASM_ERR_NO_EXPORT, 0); return ECCLES_WASM_FAILED; }
    if (nargs != f->nparams) { w_fail(vm, ECCLES_WASM_ERR_SIGNATURE, f->nparams); return ECCLES_WASM_FAILED; }
    e = w_invoke(vm, f, args, nargs, results, max_results);
    if (e != ECCLES_WASM_OK) { w_fail(vm, e, 0); return ECCLES_WASM_FAILED; }
    return (vm->exit_req && vm->exit_code != 0) ? ECCLES_WASM_FAILED : ECCLES_WASM_SUCCESSFUL;
}

eccles_wasm_status_t eccles_wasm_vm_run(eccles_wasm_vm_t *vm, const char *name) { return eccles_wasm_vm_call(vm, name, NULL, 0, NULL, 0); }
void eccles_wasm_vm_set_fuel(eccles_wasm_vm_t *vm, uint32_t budget) { vm->fuel_budget = budget; }

uint8_t *eccles_wasm_vm_mem(eccles_wasm_vm_t *vm, uint32_t addr, uint32_t len) {
    if (!vm->loaded || !vm->mem || (uint64_t)addr + len > vm->mem_bytes) return NULL;
    return vm->mem + addr;
}
uint32_t eccles_wasm_vm_mem_size(const eccles_wasm_vm_t *vm) { return vm->loaded ? vm->mem_bytes : 0; }

eccles_wasm_error_t eccles_wasm_vm_last_error(const eccles_wasm_vm_t *vm) { return vm->err; }
uint32_t eccles_wasm_vm_last_error_info(const eccles_wasm_vm_t *vm) { return vm->err_info; }
const char *eccles_wasm_vm_last_error_str(const eccles_wasm_vm_t *vm) { return vm->err_str; }
bool eccles_wasm_vm_exited(const eccles_wasm_vm_t *vm, int32_t *code) { if (vm->exit_req && code) *code = vm->exit_code; return vm->exit_req != 0; }

const char *eccles_wasm_error_name(eccles_wasm_error_t e) {
    static const char *const n[] = {
        "ok", "not initialised", "no module loaded", "busy (re-entrant call)", "out of arena memory",
        "module exceeds a configured limit", "I/O error", "no filesystem backend", "malformed module",
        "unsupported WebAssembly feature", "unresolved import", "signature mismatch", "no such export",
        "bad argument", "trap: unreachable", "trap: out-of-bounds memory access", "trap: integer divide by zero",
        "trap: integer overflow", "trap: invalid conversion to integer", "trap: undefined table element",
        "trap: indirect call type mismatch", "trap: value stack overflow", "trap: call stack overflow",
        "trap: instruction budget exhausted", "trap: host function failed", "trap: out-of-bounds table access"
    };
    return (unsigned)e < sizeof n / sizeof n[0] ? n[e] : "unknown error";
}

eccles_wasm_stats_t eccles_wasm_vm_get_stats(const eccles_wasm_vm_t *vm) {
    eccles_wasm_stats_t s; memset(&s, 0, sizeof s);
    if (!vm->inited) return s;
    s.arena_size = (uint32_t)(vm->end - vm->base);
    s.arena_used = (uint32_t)((vm->lo - vm->base) + (vm->end - vm->hi));
    s.memory_bytes = vm->mem_bytes; s.memory_pages = vm->mem_pages; s.code_bytes = vm->code_bytes;
    s.functions = vm->nfuncs; s.imports = vm->nimports; s.exports = vm->nexports; s.loaded = vm->loaded;
    return s;
}

eccles_wasm_status_t eccles_wasm_vm_register(eccles_wasm_vm_t *vm, const char *name, eccles_wasm_host_fn fn, const char *sig) {
    uint8_t tmp[2 + 200 + 16]; uint8_t i;
    if (!name || !fn || !w_sig(sig, tmp, sizeof tmp)) return ECCLES_WASM_FAILED;
    for (i = 0; i < vm->nhost; i++) if (strcmp(vm->host[i].name, name) == 0) { vm->host[i].fn = fn; vm->host[i].sig = sig; return ECCLES_WASM_SUCCESSFUL; }
    if (vm->nhost >= ECCLES_WASM_MAX_HOST_FUNCS) return ECCLES_WASM_FAILED;
    vm->host[vm->nhost].name = name; vm->host[vm->nhost].fn = fn; vm->host[vm->nhost].sig = sig; vm->nhost++;
    return ECCLES_WASM_SUCCESSFUL;
}
eccles_wasm_status_t eccles_wasm_vm_set_global(eccles_wasm_vm_t *vm, const char *name, uint64_t value) {
    uint8_t i;
    if (!name) return ECCLES_WASM_FAILED;
    for (i = 0; i < vm->nhglob; i++) if (strcmp(vm->hglob[i].name, name) == 0) { vm->hglob[i].v = value; return ECCLES_WASM_SUCCESSFUL; }
    if (vm->nhglob >= ECCLES_WASM_MAX_HOST_GLOBALS) return ECCLES_WASM_FAILED;
    vm->hglob[vm->nhglob].name = name; vm->hglob[vm->nhglob].v = value; vm->nhglob++;
    return ECCLES_WASM_SUCCESSFUL;
}
void eccles_wasm_vm_unregister_all(eccles_wasm_vm_t *vm) { vm->nhost = 0; }


/* ---- the simple, handle-free API: everything operates on one default instance ---- */
static eccles_wasm_vm_t *g_def;
static eccles_wasm_vm_t *w_def(void) {
    if (!g_def) {
#if ECCLES_WASM_USE_HEAP
        g_def = eccles_wasm_vm_create_heap(ECCLES_WASM_ARENA_SIZE);
#else
        g_def = eccles_wasm_vm_create(g_arena, sizeof g_arena);
#endif
    }
    return g_def;
}
eccles_wasm_vm_t *eccles_wasm_default_vm(void) { return w_def(); }
eccles_wasm_status_t eccles_wasm_init(void) { return w_def() ? ECCLES_WASM_SUCCESSFUL : ECCLES_WASM_FAILED; }
eccles_wasm_status_t eccles_wasm_init_with(void *mem, size_t size) {
    eccles_wasm_vm_t *v = eccles_wasm_vm_create(mem, size);
    if (!v) return ECCLES_WASM_FAILED;
    if (g_def) eccles_wasm_vm_destroy(g_def);
    g_def = v; return ECCLES_WASM_SUCCESSFUL;
}
#define W_DEF(fail) eccles_wasm_vm_t *vm = w_def(); if (!vm) return (fail)
eccles_wasm_status_t eccles_wasm_load(const char *p) { W_DEF(ECCLES_WASM_FAILED); return eccles_wasm_vm_load(vm, p); }
eccles_wasm_status_t eccles_wasm_load_buffer(const void *d, size_t n) { W_DEF(ECCLES_WASM_FAILED); return eccles_wasm_vm_load_buffer(vm, d, n); }
eccles_wasm_status_t eccles_wasm_load_stream(eccles_wasm_read_fn r, void *c) { W_DEF(ECCLES_WASM_FAILED); return eccles_wasm_vm_load_stream(vm, r, c); }
eccles_wasm_status_t eccles_wasm_unload(void) { W_DEF(ECCLES_WASM_FAILED); return eccles_wasm_vm_unload(vm); }
eccles_wasm_status_t eccles_wasm_call(const char *n, const uint64_t *a, uint32_t na, uint64_t *r, uint32_t nr) {
    W_DEF(ECCLES_WASM_FAILED); return eccles_wasm_vm_call(vm, n, a, na, r, nr);
}
eccles_wasm_status_t eccles_wasm_run(const char *n) { W_DEF(ECCLES_WASM_FAILED); return eccles_wasm_vm_run(vm, n); }
bool eccles_wasm_has_export(const char *n) { W_DEF(false); return eccles_wasm_vm_has_export(vm, n); }
void eccles_wasm_set_fuel(uint32_t b) { eccles_wasm_vm_t *vm = w_def(); if (vm) eccles_wasm_vm_set_fuel(vm, b); }
uint8_t *eccles_wasm_mem(uint32_t a, uint32_t l) { W_DEF(NULL); return eccles_wasm_vm_mem(vm, a, l); }
uint32_t eccles_wasm_mem_size(void) { W_DEF(0); return eccles_wasm_vm_mem_size(vm); }
eccles_wasm_error_t eccles_wasm_last_error(void) { W_DEF(ECCLES_WASM_ERR_NO_MEMORY); return eccles_wasm_vm_last_error(vm); }
uint32_t eccles_wasm_last_error_info(void) { W_DEF(0); return eccles_wasm_vm_last_error_info(vm); }
const char *eccles_wasm_last_error_str(void) { W_DEF(""); return eccles_wasm_vm_last_error_str(vm); }
bool eccles_wasm_exited(int32_t *c) { W_DEF(false); return eccles_wasm_vm_exited(vm, c); }
eccles_wasm_stats_t eccles_wasm_get_stats(void) {
    eccles_wasm_vm_t *vm = w_def(); eccles_wasm_stats_t z; memset(&z, 0, sizeof z);
    return vm ? eccles_wasm_vm_get_stats(vm) : z;
}
eccles_wasm_status_t eccles_wasm_register(const char *n, eccles_wasm_host_fn f, const char *sig) { W_DEF(ECCLES_WASM_FAILED); return eccles_wasm_vm_register(vm, n, f, sig); }
void eccles_wasm_unregister_all(void) { eccles_wasm_vm_t *vm = w_def(); if (vm) eccles_wasm_vm_unregister_all(vm); }

void eccles_wasm_hal_reset(void) { memset(&g_hal_user, 0, sizeof g_hal_user); }
void eccles_wasm_hal_override(const eccles_wasm_hal_t *o) {
    typedef void (*vfn)(void);
    vfn *dst = (vfn *)(void *)&g_hal_user; const vfn *src = (const vfn *)(const void *)o; size_t i;
    if (!o) return;
    for (i = 0; i < sizeof g_hal_user / sizeof(vfn); i++) if (src[i]) dst[i] = src[i];
}

/* ===================================================================== *
 *  6. hardware backends
 *
 *  Every backend implements some subset of the plat_* functions and
 *  #defines W_HAS_<name> for each one; the stubs further down return
 *  UNSUPPORTED for the rest, so a backend only writes what its chip can do.
 * ===================================================================== */

#if defined(__GNUC__) || defined(__clang__)
  #pragma GCC diagnostic push
  #pragma GCC diagnostic ignored "-Wunused-parameter"
  #pragma GCC diagnostic ignored "-Wunused-function"
#endif

#define W_OK  ECCLES_WASM_SUCCESSFUL
#define W_BAD ECCLES_WASM_FAILED
#define W_NS  ECCLES_WASM_UNSUPPORTED
typedef eccles_wasm_status_t w_st;

/* Cortex-M software reset without needing CMSIS headers */
#if defined(__ARM_ARCH_6M__) || defined(__ARM_ARCH_7M__) || defined(__ARM_ARCH_7EM__) || \
    defined(__ARM_ARCH_8M_MAIN__) || defined(__ARM_ARCH_8M_BASE__)
  #define W_CORTEX_M 1
  static void w_cortexm_reset(void) {
      __asm volatile ("dsb 0xF" ::: "memory");
      *(volatile uint32_t *)0xE000ED0Cu = 0x05FA0004u;
      for (;;) {}
  }
#endif

/* shared pin-interrupt counters (ESP-IDF / Pico) */
#if ECCLES_WASM_PLATFORM == ECCLES_WASM_PLAT_ESP_IDF || ECCLES_WASM_PLATFORM == ECCLES_WASM_PLAT_PICO
static struct { int32_t pin; uint8_t edge; volatile uint32_t count; } w_irq[ECCLES_WASM_MAX_IRQ];
static int w_irq_slot(uint32_t pin, int alloc) {
    int i, freeslot = -1;
    for (i = 0; i < ECCLES_WASM_MAX_IRQ; i++) {
        if (w_irq[i].pin == (int32_t)pin && w_irq[i].edge) return i;
        if (!w_irq[i].edge && freeslot < 0) freeslot = i;
    }
    return alloc ? freeslot : -1;
}
#endif

/* =================================================================== HOST */
#if ECCLES_WASM_PLATFORM == ECCLES_WASM_PLAT_HOST
#include <stdio.h>
#if defined(_WIN32)
  #include <windows.h>
#else
  #include <time.h>
#endif
#if defined(ECCLES_WASM_THREADSAFE) && !defined(_WIN32) && !defined(ECCLES_WASM_USER_LOCK)
  #include <pthread.h>
  static pthread_mutex_t w_host_mx = PTHREAD_MUTEX_INITIALIZER;
  void eccles_wasm_host_lock(void)   { pthread_mutex_lock(&w_host_mx); }
  void eccles_wasm_host_unlock(void) { pthread_mutex_unlock(&w_host_mx); }
#endif

#define SIM_PINS 64
#define SIM_PORTS 2
#define SIM_BUF 256
static uint8_t  sim_mode[SIM_PINS], sim_level[SIM_PINS], sim_irq_edge[SIM_PINS];
static uint32_t sim_adc[SIM_PINS], sim_duty[SIM_PINS], sim_freq[SIM_PINS], sim_irq_cnt[SIM_PINS];
static uint8_t  sim_rx[SIM_PORTS][SIM_BUF], sim_tx[SIM_PORTS][SIM_BUF];
static uint32_t sim_rxn[SIM_PORTS], sim_txn[SIM_PORTS];
static uint8_t  sim_i2c_w[2][64], sim_i2c_r[2][64];
static uint32_t sim_i2c_wn[2], sim_i2c_rn[2], sim_i2c_addr[2];
static uint8_t  sim_reset_flag;
static uint64_t sim_vt_us;
static uint32_t sim_rng = 0x2545F491u;

static uint64_t sim_now_us(void) {
#if defined(ECCLES_WASM_HOST_VIRTUAL_TIME)
    { uint64_t v; ECCLES_WASM_HW_LOCK(); v = sim_vt_us; ECCLES_WASM_HW_UNLOCK(); return v; }
#elif defined(_WIN32)
    LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c);
    return (uint64_t)(c.QuadPart * 1000000ll / f.QuadPart) + sim_vt_us;
#else
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u + sim_vt_us;
#endif
}
static void sim_wait_us(uint32_t us) {
#if defined(ECCLES_WASM_HOST_VIRTUAL_TIME)
    ECCLES_WASM_HW_LOCK(); sim_vt_us += us; ECCLES_WASM_HW_UNLOCK();
#elif defined(_WIN32)
    Sleep((us + 999u) / 1000u);
#else
    struct timespec ts; ts.tv_sec = us / 1000000u; ts.tv_nsec = (long)(us % 1000000u) * 1000l;
    nanosleep(&ts, NULL);
#endif
}

static w_st plat_gpio_mode(uint32_t pin, uint32_t mode) {
    if (pin >= SIM_PINS || mode > ECCLES_WASM_PIN_OPEN_DRAIN) return W_BAD;
    sim_mode[pin] = (uint8_t)mode; return W_OK;
}
static w_st plat_gpio_write(uint32_t pin, uint32_t level) {
    if (pin >= SIM_PINS) return W_BAD;
    if (sim_mode[pin] != ECCLES_WASM_PIN_OUTPUT && sim_mode[pin] != ECCLES_WASM_PIN_OPEN_DRAIN) return W_BAD;
    sim_level[pin] = level ? 1 : 0; return W_OK;
}
static w_st plat_gpio_read(uint32_t pin, uint32_t *level) {
    if (pin >= SIM_PINS) return W_BAD;
    *level = sim_level[pin]; return W_OK;
}
static w_st plat_pwm(uint32_t pin, uint32_t freq, uint32_t duty) {
    if (pin >= SIM_PINS || duty > 65535u || !freq) return W_BAD;
    sim_mode[pin] = ECCLES_WASM_PIN_OUTPUT; sim_duty[pin] = duty; sim_freq[pin] = freq; return W_OK;
}
static w_st plat_analog(uint32_t pin, uint32_t v) {
    if (v > ECCLES_WASM_ANALOG_MAX) return W_BAD;
    return plat_pwm(pin, ECCLES_WASM_ANALOG_FREQ_HZ, (uint32_t)(((uint64_t)v * 65535u) / ECCLES_WASM_ANALOG_MAX));
}
static w_st plat_adc(uint32_t pin, uint32_t *v) { if (pin >= SIM_PINS) return W_BAD; *v = sim_adc[pin]; return W_OK; }
static w_st plat_millis(uint32_t *v) { *v = (uint32_t)(sim_now_us() / 1000u); return W_OK; }
static w_st plat_micros(uint32_t *v) { *v = (uint32_t)sim_now_us(); return W_OK; }
static w_st plat_sleep_ms(uint32_t ms) { while (ms >= 1000u) { sim_wait_us(1000000u); ms -= 1000u; } sim_wait_us(ms * 1000u); return W_OK; }
static w_st plat_sleep_us(uint32_t us) { sim_wait_us(us); return W_OK; }
static w_st plat_yield(void) { return W_OK; }
static w_st plat_deep_sleep(uint32_t ms) { return plat_sleep_ms(ms); }
static w_st plat_reset(void) { sim_reset_flag = 1; return W_OK; }
static w_st plat_random(uint32_t *v) { sim_rng ^= sim_rng << 13; sim_rng ^= sim_rng >> 17; sim_rng ^= sim_rng << 5; *v = sim_rng; return W_OK; }
static w_st plat_wdt_feed(void) { return W_OK; }
static w_st plat_log(const uint8_t *b, uint32_t n) { fwrite(b, 1, n, stdout); fflush(stdout); return W_OK; }
static w_st plat_irq_attach(uint32_t pin, uint32_t edge) {
    if (pin >= SIM_PINS || edge < 1 || edge > 3) return W_BAD;
    sim_irq_edge[pin] = (uint8_t)edge; sim_irq_cnt[pin] = 0; return W_OK;
}
static w_st plat_irq_count(uint32_t pin, uint32_t *c) { if (pin >= SIM_PINS || !sim_irq_edge[pin]) return W_BAD; *c = sim_irq_cnt[pin]; return W_OK; }
static w_st plat_irq_clear(uint32_t pin) { if (pin >= SIM_PINS || !sim_irq_edge[pin]) return W_BAD; sim_irq_cnt[pin] = 0; return W_OK; }
static w_st plat_irq_detach(uint32_t pin) { if (pin >= SIM_PINS) return W_BAD; sim_irq_edge[pin] = 0; return W_OK; }
static w_st plat_uart_begin(uint32_t p, uint32_t baud, int32_t tx, int32_t rx) { return (p < SIM_PORTS && baud) ? W_OK : W_BAD; }
static w_st plat_uart_write(uint32_t p, const uint8_t *b, uint32_t n) {
    if (p >= SIM_PORTS) return W_BAD;
    if (sim_txn[p] + n > SIM_BUF) return W_BAD;
    memcpy(sim_tx[p] + sim_txn[p], b, n); sim_txn[p] += n; return W_OK;
}
static w_st plat_uart_read(uint32_t p, uint8_t *b, uint32_t n, uint32_t to, uint32_t *got) {
    uint32_t k;
    if (p >= SIM_PORTS) return W_BAD;
    k = n < sim_rxn[p] ? n : sim_rxn[p];
    memcpy(b, sim_rx[p], k); memmove(sim_rx[p], sim_rx[p] + k, sim_rxn[p] - k); sim_rxn[p] -= k; *got = k;
    return W_OK;
}
static w_st plat_uart_end(uint32_t p) { return p < SIM_PORTS ? W_OK : W_BAD; }
static w_st plat_i2c_begin(uint32_t b, int32_t sda, int32_t scl, uint32_t f) { return b < 2 ? W_OK : W_BAD; }
static w_st plat_i2c_write(uint32_t b, uint32_t a, const uint8_t *d, uint32_t n) {
    if (b >= 2 || a > 0x7F || n > 64) return W_BAD;
    memcpy(sim_i2c_w[b], d, n); sim_i2c_wn[b] = n; sim_i2c_addr[b] = a; return W_OK;
}
static w_st plat_i2c_read(uint32_t b, uint32_t a, uint8_t *d, uint32_t n) {
    uint32_t i;
    if (b >= 2 || a > 0x7F) return W_BAD;
    for (i = 0; i < n; i++) d[i] = i < sim_i2c_rn[b] ? sim_i2c_r[b][i] : 0;
    sim_i2c_addr[b] = a; return W_OK;
}
static w_st plat_i2c_write_read(uint32_t b, uint32_t a, const uint8_t *t, uint32_t tn, uint8_t *r, uint32_t rn) {
    w_st s = plat_i2c_write(b, a, t, tn);
    return s == W_OK ? plat_i2c_read(b, a, r, rn) : s;
}
static w_st plat_spi_begin(uint32_t b, int32_t a, int32_t m, int32_t i, uint32_t f, uint32_t mode) { return (b < 2 && mode < 4) ? W_OK : W_BAD; }
static w_st plat_spi_transfer(uint32_t b, const uint8_t *t, uint8_t *r, uint32_t n) {
    uint32_t i;
    if (b >= 2) return W_BAD;
    for (i = 0; i < n; i++) { uint8_t v = t ? t[i] : 0; if (r) r[i] = v; }      /* loopback */
    return W_OK;
}
static w_st plat_spi_end(uint32_t b) { return b < 2 ? W_OK : W_BAD; }
#define W_HAS_gpio_mode 1
#define W_HAS_gpio_write 1
#define W_HAS_gpio_read 1
#define W_HAS_analog 1
#define W_HAS_pwm 1
#define W_HAS_adc 1
#define W_HAS_millis 1
#define W_HAS_micros 1
#define W_HAS_sleep_ms 1
#define W_HAS_sleep_us 1
#define W_HAS_yield 1
#define W_HAS_deep_sleep 1
#define W_HAS_reset 1
#define W_HAS_random 1
#define W_HAS_wdt_feed 1
#define W_HAS_log 1
#define W_HAS_irq_attach 1
#define W_HAS_irq_count 1
#define W_HAS_irq_clear 1
#define W_HAS_irq_detach 1
#define W_HAS_uart_begin 1
#define W_HAS_uart_write 1
#define W_HAS_uart_read 1
#define W_HAS_uart_end 1
#define W_HAS_i2c_begin 1
#define W_HAS_i2c_write 1
#define W_HAS_i2c_read 1
#define W_HAS_i2c_write_read 1
#define W_HAS_spi_begin 1
#define W_HAS_spi_transfer 1
#define W_HAS_spi_end 1

void eccles_wasm_sim_reset(void) {
    memset(sim_mode, 0, sizeof sim_mode); memset(sim_level, 0, sizeof sim_level); memset(sim_irq_edge, 0, sizeof sim_irq_edge);
    memset(sim_adc, 0, sizeof sim_adc); memset(sim_duty, 0, sizeof sim_duty); memset(sim_freq, 0, sizeof sim_freq);
    memset(sim_irq_cnt, 0, sizeof sim_irq_cnt); memset(sim_rxn, 0, sizeof sim_rxn); memset(sim_txn, 0, sizeof sim_txn);
    memset(sim_i2c_wn, 0, sizeof sim_i2c_wn); memset(sim_i2c_rn, 0, sizeof sim_i2c_rn); memset(sim_i2c_addr, 0, sizeof sim_i2c_addr);
    sim_reset_flag = 0;
}
uint32_t eccles_wasm_sim_pin_mode(uint32_t p) { return p < SIM_PINS ? sim_mode[p] : 0; }
uint32_t eccles_wasm_sim_pin_level(uint32_t p) { return p < SIM_PINS ? sim_level[p] : 0; }
void eccles_wasm_sim_set_input(uint32_t p, uint32_t l) {
    if (p >= SIM_PINS) return;
    l = l ? 1 : 0;
    if (sim_irq_edge[p] && sim_level[p] != l) eccles_wasm_sim_irq_fire(p, l ? ECCLES_WASM_EDGE_RISING : ECCLES_WASM_EDGE_FALLING);
    sim_level[p] = (uint8_t)l;
}
void eccles_wasm_sim_set_adc(uint32_t p, uint32_t v) { if (p < SIM_PINS) sim_adc[p] = v; }
uint32_t eccles_wasm_sim_pwm_duty(uint32_t p) { return p < SIM_PINS ? sim_duty[p] : 0; }
uint32_t eccles_wasm_sim_pwm_freq(uint32_t p) { return p < SIM_PINS ? sim_freq[p] : 0; }
void eccles_wasm_sim_irq_fire(uint32_t p, uint32_t edge) { if (p < SIM_PINS && (sim_irq_edge[p] & edge)) sim_irq_cnt[p]++; }
void eccles_wasm_sim_uart_feed(uint32_t p, const uint8_t *d, uint32_t n) {
    if (p >= SIM_PORTS) return;
    if (sim_rxn[p] + n > SIM_BUF) n = SIM_BUF - sim_rxn[p];
    memcpy(sim_rx[p] + sim_rxn[p], d, n); sim_rxn[p] += n;
}
uint32_t eccles_wasm_sim_uart_take(uint32_t p, uint8_t *o, uint32_t max) {
    uint32_t k;
    if (p >= SIM_PORTS) return 0;
    k = max < sim_txn[p] ? max : sim_txn[p];
    memcpy(o, sim_tx[p], k); memmove(sim_tx[p], sim_tx[p] + k, sim_txn[p] - k); sim_txn[p] -= k; return k;
}
void eccles_wasm_sim_i2c_set_response(uint32_t b, const uint8_t *d, uint32_t n) {
    if (b >= 2) return;
    if (n > 64) n = 64;
    memcpy(sim_i2c_r[b], d, n); sim_i2c_rn[b] = n;
}
uint32_t eccles_wasm_sim_i2c_last_write(uint32_t b, uint8_t *o, uint32_t max) {
    uint32_t k;
    if (b >= 2) return 0;
    k = max < sim_i2c_wn[b] ? max : sim_i2c_wn[b]; memcpy(o, sim_i2c_w[b], k); return k;
}
uint32_t eccles_wasm_sim_i2c_last_addr(uint32_t b) { return b < 2 ? sim_i2c_addr[b] : 0; }
bool eccles_wasm_sim_reset_requested(void) { return sim_reset_flag != 0; }

/* ============================================================== ARDUINO */
#elif ECCLES_WASM_PLATFORM == ECCLES_WASM_PLAT_ARDUINO
#include <Arduino.h>
#if defined(ARDUINO_ARCH_ESP32)
  #include <esp_system.h>
  #include <esp_sleep.h>
#endif
static w_st plat_gpio_mode(uint32_t pin, uint32_t mode) {
    switch (mode) {
    case ECCLES_WASM_PIN_INPUT:  pinMode((uint8_t)pin, INPUT); return W_OK;
    case ECCLES_WASM_PIN_OUTPUT: pinMode((uint8_t)pin, OUTPUT); return W_OK;
    case ECCLES_WASM_PIN_INPUT_PULLUP: pinMode((uint8_t)pin, INPUT_PULLUP); return W_OK;
#if defined(INPUT_PULLDOWN)
    case ECCLES_WASM_PIN_INPUT_PULLDOWN: pinMode((uint8_t)pin, INPUT_PULLDOWN); return W_OK;
#endif
#if defined(OUTPUT_OPEN_DRAIN)
    case ECCLES_WASM_PIN_OPEN_DRAIN: pinMode((uint8_t)pin, OUTPUT_OPEN_DRAIN); return W_OK;
#endif
    default: return mode > ECCLES_WASM_PIN_OPEN_DRAIN ? W_BAD : W_NS;
    }
}
static w_st plat_gpio_write(uint32_t pin, uint32_t l) { digitalWrite((uint8_t)pin, l ? HIGH : LOW); return W_OK; }
static w_st plat_gpio_read(uint32_t pin, uint32_t *l) { *l = digitalRead((uint8_t)pin) ? 1u : 0u; return W_OK; }
static w_st plat_analog(uint32_t pin, uint32_t v) {
    if (v > ECCLES_WASM_ANALOG_MAX) return W_BAD;
    analogWrite((uint8_t)pin, (int)(((uint32_t)v * 255u) / ECCLES_WASM_ANALOG_MAX)); return W_OK;
}
static w_st plat_adc(uint32_t pin, uint32_t *v) { *v = (uint32_t)analogRead((uint8_t)pin); return W_OK; }
static w_st plat_millis(uint32_t *v) { *v = (uint32_t)millis(); return W_OK; }
static w_st plat_micros(uint32_t *v) { *v = (uint32_t)micros(); return W_OK; }
static w_st plat_sleep_ms(uint32_t ms) { delay(ms); return W_OK; }
static w_st plat_sleep_us(uint32_t us) { delayMicroseconds(us); return W_OK; }
static w_st plat_yield(void) { yield(); return W_OK; }
#if defined(ARDUINO_ARCH_ESP32)
static w_st plat_deep_sleep(uint32_t ms) { esp_deep_sleep((uint64_t)ms * 1000u); return W_OK; }
static w_st plat_reset(void) { esp_restart(); return W_OK; }
static w_st plat_random(uint32_t *v) { *v = esp_random(); return W_OK; }
#define W_HAS_deep_sleep 1
#define W_HAS_reset 1
#define W_HAS_random 1
#elif defined(W_CORTEX_M)
static w_st plat_reset(void) { w_cortexm_reset(); return W_OK; }
#define W_HAS_reset 1
#endif
#define W_HAS_gpio_mode 1
#define W_HAS_gpio_write 1
#define W_HAS_gpio_read 1
#define W_HAS_analog 1
#define W_HAS_adc 1
#define W_HAS_millis 1
#define W_HAS_micros 1
#define W_HAS_sleep_ms 1
#define W_HAS_sleep_us 1
#define W_HAS_yield 1

/* ============================================================== ESP-IDF */
#elif ECCLES_WASM_PLATFORM == ECCLES_WASM_PLAT_ESP_IDF
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/uart.h"
#include "driver/i2c.h"
#include "driver/spi_master.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_sleep.h"
#include "esp_attr.h"
#include "esp_rom_sys.h"
#include "esp_task_wdt.h"
#include "esp_idf_version.h"
#if defined(__has_include)
  #if __has_include("esp_random.h")
    #include "esp_random.h"
  #endif
  #if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0) && __has_include("esp_adc/adc_oneshot.h")
    #include "esp_adc/adc_oneshot.h"
    #define W_ESP_ADC 1
  #endif
#endif

static w_st plat_gpio_mode(uint32_t pin, uint32_t mode) {
    gpio_num_t g = (gpio_num_t)pin;
    if (!GPIO_IS_VALID_GPIO(g)) return W_BAD;
    gpio_reset_pin(g);
    switch (mode) {
    case ECCLES_WASM_PIN_INPUT:  gpio_set_direction(g, GPIO_MODE_INPUT); gpio_set_pull_mode(g, GPIO_FLOATING); break;
    case ECCLES_WASM_PIN_INPUT_PULLUP:   gpio_set_direction(g, GPIO_MODE_INPUT); gpio_set_pull_mode(g, GPIO_PULLUP_ONLY); break;
    case ECCLES_WASM_PIN_INPUT_PULLDOWN: gpio_set_direction(g, GPIO_MODE_INPUT); gpio_set_pull_mode(g, GPIO_PULLDOWN_ONLY); break;
    case ECCLES_WASM_PIN_OUTPUT: gpio_set_direction(g, GPIO_MODE_INPUT_OUTPUT); break;
    case ECCLES_WASM_PIN_OPEN_DRAIN: gpio_set_direction(g, GPIO_MODE_INPUT_OUTPUT_OD); break;
    default: return W_BAD;
    }
    return W_OK;
}
static w_st plat_gpio_write(uint32_t pin, uint32_t l) { return gpio_set_level((gpio_num_t)pin, l ? 1 : 0) == ESP_OK ? W_OK : W_BAD; }
static w_st plat_gpio_read(uint32_t pin, uint32_t *l) {
    if (!GPIO_IS_VALID_GPIO((gpio_num_t)pin)) return W_BAD;
    *l = (uint32_t)gpio_get_level((gpio_num_t)pin); return W_OK;
}

/* PWM: one LEDC timer shared by every PWM pin (changing the frequency changes it for all) */
#define W_PWM_CH 8
static int32_t w_pwm_pin[W_PWM_CH];
static uint8_t w_pwm_init;
static uint32_t w_pwm_hz;
static w_st plat_pwm(uint32_t pin, uint32_t hz, uint32_t duty16) {
    int i, ch = -1; uint32_t duty;
    if (!GPIO_IS_VALID_OUTPUT_GPIO((gpio_num_t)pin) || !hz || duty16 > 65535u) return W_BAD;
    if (!w_pwm_init) {
        ledc_timer_config_t t; memset(&t, 0, sizeof t);
        t.speed_mode = LEDC_LOW_SPEED_MODE; t.duty_resolution = LEDC_TIMER_10_BIT; t.timer_num = LEDC_TIMER_0;
        t.freq_hz = hz; t.clk_cfg = LEDC_AUTO_CLK;
        if (ledc_timer_config(&t) != ESP_OK) return W_BAD;
        for (i = 0; i < W_PWM_CH; i++) w_pwm_pin[i] = -1;
        w_pwm_init = 1; w_pwm_hz = hz;
    } else if (hz != w_pwm_hz) {
        if (ledc_set_freq(LEDC_LOW_SPEED_MODE, LEDC_TIMER_0, hz) != ESP_OK) return W_BAD;
        w_pwm_hz = hz;
    }
    for (i = 0; i < W_PWM_CH; i++) if (w_pwm_pin[i] == (int32_t)pin) { ch = i; break; }
    if (ch < 0) {
        ledc_channel_config_t c; memset(&c, 0, sizeof c);
        for (i = 0; i < W_PWM_CH; i++) if (w_pwm_pin[i] < 0) { ch = i; break; }
        if (ch < 0) return W_BAD;
        c.gpio_num = (int)pin; c.speed_mode = LEDC_LOW_SPEED_MODE; c.channel = (ledc_channel_t)ch;
        c.timer_sel = LEDC_TIMER_0; c.duty = 0; c.hpoint = 0;
        if (ledc_channel_config(&c) != ESP_OK) return W_BAD;
        w_pwm_pin[ch] = (int32_t)pin;
    }
    duty = (duty16 * 1023u) / 65535u;
    if (ledc_set_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)ch, duty) != ESP_OK) return W_BAD;
    return ledc_update_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)ch) == ESP_OK ? W_OK : W_BAD;
}
static w_st plat_analog(uint32_t pin, uint32_t v) {
    if (v > ECCLES_WASM_ANALOG_MAX) return W_BAD;
    return plat_pwm(pin, ECCLES_WASM_ANALOG_FREQ_HZ, (uint32_t)(((uint64_t)v * 65535u) / ECCLES_WASM_ANALOG_MAX));
}
#if defined(W_ESP_ADC)
static w_st plat_adc(uint32_t pin, uint32_t *v) {
    static adc_oneshot_unit_handle_t h[2]; adc_unit_t unit; adc_channel_t ch; int raw = 0, u;
    if (adc_oneshot_io_to_channel((int)pin, &unit, &ch) != ESP_OK) return W_BAD;
    u = (unit == ADC_UNIT_1) ? 0 : 1;
    if (!h[u]) {
        adc_oneshot_unit_init_cfg_t ic; memset(&ic, 0, sizeof ic); ic.unit_id = unit;
        if (adc_oneshot_new_unit(&ic, &h[u]) != ESP_OK) return W_BAD;
    }
    {
        adc_oneshot_chan_cfg_t cc; memset(&cc, 0, sizeof cc);
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 2, 0)
        cc.atten = ADC_ATTEN_DB_12;
#else
        cc.atten = ADC_ATTEN_DB_11;
#endif
        cc.bitwidth = ADC_BITWIDTH_DEFAULT;
        if (adc_oneshot_config_channel(h[u], ch, &cc) != ESP_OK) return W_BAD;
    }
    if (adc_oneshot_read(h[u], ch, &raw) != ESP_OK) return W_BAD;
    *v = (uint32_t)raw; return W_OK;
}
#define W_HAS_adc 1
#endif
static w_st plat_millis(uint32_t *v) { *v = (uint32_t)(esp_timer_get_time() / 1000); return W_OK; }
static w_st plat_micros(uint32_t *v) { *v = (uint32_t)esp_timer_get_time(); return W_OK; }
static w_st plat_sleep_ms(uint32_t ms) {
    TickType_t t = pdMS_TO_TICKS(ms);
    if (ms && !t) t = 1;
    vTaskDelay(t); return W_OK;
}
static w_st plat_sleep_us(uint32_t us) {
    if (us >= 2000u) return plat_sleep_ms(us / 1000u);
    esp_rom_delay_us(us); return W_OK;
}
static w_st plat_yield(void) { taskYIELD(); return W_OK; }
static w_st plat_deep_sleep(uint32_t ms) { esp_deep_sleep((uint64_t)ms * 1000u); return W_OK; }
static w_st plat_reset(void) { esp_restart(); return W_OK; }
static w_st plat_random(uint32_t *v) { *v = esp_random(); return W_OK; }
static w_st plat_wdt_feed(void) { (void)esp_task_wdt_reset(); return W_OK; }
static w_st plat_log(const uint8_t *b, uint32_t n) { fwrite(b, 1, n, stdout); return W_OK; }

static void IRAM_ATTR w_isr(void *arg) { w_irq[(uintptr_t)arg].count++; }
static w_st plat_irq_attach(uint32_t pin, uint32_t edge) {
    static uint8_t svc; int s = w_irq_slot(pin, 1); gpio_int_type_t t;
    if (s < 0 || !GPIO_IS_VALID_GPIO((gpio_num_t)pin)) return W_BAD;
    t = edge == ECCLES_WASM_EDGE_RISING ? GPIO_INTR_POSEDGE : edge == ECCLES_WASM_EDGE_FALLING ? GPIO_INTR_NEGEDGE : GPIO_INTR_ANYEDGE;
    if (!svc) { esp_err_t e = gpio_install_isr_service(0); if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return W_BAD; svc = 1; }
    gpio_set_intr_type((gpio_num_t)pin, t);
    w_irq[s].pin = (int32_t)pin; w_irq[s].edge = (uint8_t)edge; w_irq[s].count = 0;
    if (gpio_isr_handler_add((gpio_num_t)pin, w_isr, (void *)(uintptr_t)s) != ESP_OK) { w_irq[s].edge = 0; return W_BAD; }
    gpio_intr_enable((gpio_num_t)pin);
    return W_OK;
}
static w_st plat_irq_count(uint32_t pin, uint32_t *c) { int s = w_irq_slot(pin, 0); if (s < 0) return W_BAD; *c = w_irq[s].count; return W_OK; }
static w_st plat_irq_clear(uint32_t pin) { int s = w_irq_slot(pin, 0); if (s < 0) return W_BAD; w_irq[s].count = 0; return W_OK; }
static w_st plat_irq_detach(uint32_t pin) {
    int s = w_irq_slot(pin, 0);
    if (s < 0) return W_BAD;
    gpio_isr_handler_remove((gpio_num_t)pin); w_irq[s].edge = 0; return W_OK;
}

static w_st plat_uart_begin(uint32_t port, uint32_t baud, int32_t tx, int32_t rx) {
    uart_config_t c; memset(&c, 0, sizeof c);
    if (port >= UART_NUM_MAX || !baud) return W_BAD;
    c.baud_rate = (int)baud; c.data_bits = UART_DATA_8_BITS; c.parity = UART_PARITY_DISABLE;
    c.stop_bits = UART_STOP_BITS_1; c.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    if (uart_driver_install((uart_port_t)port, 512, 0, 0, NULL, 0) != ESP_OK) return W_BAD;
    if (uart_param_config((uart_port_t)port, &c) != ESP_OK) return W_BAD;
    if (uart_set_pin((uart_port_t)port, tx < 0 ? UART_PIN_NO_CHANGE : tx, rx < 0 ? UART_PIN_NO_CHANGE : rx,
                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK) return W_BAD;
    return W_OK;
}
static w_st plat_uart_write(uint32_t port, const uint8_t *b, uint32_t n) {
    return uart_write_bytes((uart_port_t)port, (const char *)b, n) == (int)n ? W_OK : W_BAD;
}
static w_st plat_uart_read(uint32_t port, uint8_t *b, uint32_t n, uint32_t to, uint32_t *got) {
    int r = uart_read_bytes((uart_port_t)port, b, n, pdMS_TO_TICKS(to));
    if (r < 0) return W_BAD;
    *got = (uint32_t)r; return W_OK;
}
static w_st plat_uart_end(uint32_t port) { return uart_driver_delete((uart_port_t)port) == ESP_OK ? W_OK : W_BAD; }

static w_st plat_i2c_begin(uint32_t bus, int32_t sda, int32_t scl, uint32_t f) {
    i2c_config_t c; memset(&c, 0, sizeof c);
    if (bus >= I2C_NUM_MAX || sda < 0 || scl < 0) return W_BAD;
    c.mode = I2C_MODE_MASTER; c.sda_io_num = sda; c.scl_io_num = scl;
    c.sda_pullup_en = GPIO_PULLUP_ENABLE; c.scl_pullup_en = GPIO_PULLUP_ENABLE; c.master.clk_speed = f ? f : 100000u;
    if (i2c_param_config((i2c_port_t)bus, &c) != ESP_OK) return W_BAD;
    return i2c_driver_install((i2c_port_t)bus, I2C_MODE_MASTER, 0, 0, 0) == ESP_OK ? W_OK : W_BAD;
}
static w_st plat_i2c_write(uint32_t bus, uint32_t a, const uint8_t *b, uint32_t n) {
    return i2c_master_write_to_device((i2c_port_t)bus, (uint8_t)a, b, n, pdMS_TO_TICKS(1000)) == ESP_OK ? W_OK : W_BAD;
}
static w_st plat_i2c_read(uint32_t bus, uint32_t a, uint8_t *b, uint32_t n) {
    return i2c_master_read_from_device((i2c_port_t)bus, (uint8_t)a, b, n, pdMS_TO_TICKS(1000)) == ESP_OK ? W_OK : W_BAD;
}
static w_st plat_i2c_write_read(uint32_t bus, uint32_t a, const uint8_t *t, uint32_t tn, uint8_t *r, uint32_t rn) {
    return i2c_master_write_read_device((i2c_port_t)bus, (uint8_t)a, t, tn, r, rn, pdMS_TO_TICKS(1000)) == ESP_OK ? W_OK : W_BAD;
}

#define W_SPI_MAX 4000u
static spi_device_handle_t w_spi_dev[2];
static w_st plat_spi_begin(uint32_t bus, int32_t sck, int32_t mosi, int32_t miso, uint32_t f, uint32_t mode) {
    spi_bus_config_t b; spi_device_interface_config_t d; spi_host_device_t host;
    if (bus > 1 || mode > 3 || sck < 0) return W_BAD;
    host = bus == 0 ? SPI2_HOST : SPI3_HOST;
    memset(&b, 0, sizeof b); memset(&d, 0, sizeof d);
    b.mosi_io_num = mosi; b.miso_io_num = miso; b.sclk_io_num = sck; b.quadwp_io_num = -1; b.quadhd_io_num = -1;
    b.max_transfer_sz = W_SPI_MAX;
    d.clock_speed_hz = (int)(f ? f : 1000000u); d.mode = (uint8_t)mode; d.spics_io_num = -1; d.queue_size = 1;
    if (spi_bus_initialize(host, &b, SPI_DMA_CH_AUTO) != ESP_OK) return W_BAD;
    return spi_bus_add_device(host, &d, &w_spi_dev[bus]) == ESP_OK ? W_OK : W_BAD;
}
static w_st plat_spi_transfer(uint32_t bus, const uint8_t *tx, uint8_t *rx, uint32_t n) {
    uint32_t off = 0;
    if (bus > 1 || !w_spi_dev[bus]) return W_BAD;
    while (off < n) {
        uint32_t k = n - off < W_SPI_MAX ? n - off : W_SPI_MAX; spi_transaction_t t; memset(&t, 0, sizeof t);
        t.length = k * 8u; t.tx_buffer = tx ? tx + off : NULL; t.rx_buffer = rx ? rx + off : NULL;
        if (spi_device_polling_transmit(w_spi_dev[bus], &t) != ESP_OK) return W_BAD;
        off += k;
    }
    return W_OK;
}
static w_st plat_spi_end(uint32_t bus) {
    if (bus > 1 || !w_spi_dev[bus]) return W_BAD;
    spi_bus_remove_device(w_spi_dev[bus]); w_spi_dev[bus] = NULL;
    return spi_bus_free(bus == 0 ? SPI2_HOST : SPI3_HOST) == ESP_OK ? W_OK : W_BAD;
}
#define W_HAS_gpio_mode 1
#define W_HAS_gpio_write 1
#define W_HAS_gpio_read 1
#define W_HAS_analog 1
#define W_HAS_pwm 1
#define W_HAS_millis 1
#define W_HAS_micros 1
#define W_HAS_sleep_ms 1
#define W_HAS_sleep_us 1
#define W_HAS_yield 1
#define W_HAS_deep_sleep 1
#define W_HAS_reset 1
#define W_HAS_random 1
#define W_HAS_wdt_feed 1
#define W_HAS_log 1
#define W_HAS_irq_attach 1
#define W_HAS_irq_count 1
#define W_HAS_irq_clear 1
#define W_HAS_irq_detach 1
#define W_HAS_uart_begin 1
#define W_HAS_uart_write 1
#define W_HAS_uart_read 1
#define W_HAS_uart_end 1
#define W_HAS_i2c_begin 1
#define W_HAS_i2c_write 1
#define W_HAS_i2c_read 1
#define W_HAS_i2c_write_read 1
#define W_HAS_spi_begin 1
#define W_HAS_spi_transfer 1
#define W_HAS_spi_end 1

/* ================================================================== PICO */
#elif ECCLES_WASM_PLATFORM == ECCLES_WASM_PLAT_PICO
#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/rand.h"
#include "hardware/gpio.h"
#include "hardware/adc.h"
#include "hardware/pwm.h"
#include "hardware/uart.h"
#include "hardware/i2c.h"
#include "hardware/spi.h"
#include "hardware/watchdog.h"
#include "hardware/clocks.h"

static w_st plat_gpio_mode(uint32_t pin, uint32_t mode) {
    if (pin >= NUM_BANK0_GPIOS) return W_BAD;
    gpio_init(pin);
    switch (mode) {
    case ECCLES_WASM_PIN_INPUT:  gpio_set_dir(pin, GPIO_IN); gpio_disable_pulls(pin); break;
    case ECCLES_WASM_PIN_INPUT_PULLUP:   gpio_set_dir(pin, GPIO_IN); gpio_pull_up(pin); break;
    case ECCLES_WASM_PIN_INPUT_PULLDOWN: gpio_set_dir(pin, GPIO_IN); gpio_pull_down(pin); break;
    case ECCLES_WASM_PIN_OUTPUT: gpio_set_dir(pin, GPIO_OUT); break;
    case ECCLES_WASM_PIN_OPEN_DRAIN: return W_NS;
    default: return W_BAD;
    }
    return W_OK;
}
static w_st plat_gpio_write(uint32_t pin, uint32_t l) { if (pin >= NUM_BANK0_GPIOS) return W_BAD; gpio_put(pin, l ? 1 : 0); return W_OK; }
static w_st plat_gpio_read(uint32_t pin, uint32_t *l) { if (pin >= NUM_BANK0_GPIOS) return W_BAD; *l = gpio_get(pin) ? 1u : 0u; return W_OK; }
static w_st plat_pwm(uint32_t pin, uint32_t hz, uint32_t duty16) {
    uint slice; float div; uint32_t wrap; uint32_t clk;
    if (pin >= NUM_BANK0_GPIOS || !hz || duty16 > 65535u) return W_BAD;
    clk = clock_get_hz(clk_sys);
    gpio_set_function(pin, GPIO_FUNC_PWM);
    slice = pwm_gpio_to_slice_num(pin);
    div = (float)clk / ((float)hz * 65536.0f); if (div < 1.0f) div = 1.0f;
    if (div > 255.9f) return W_BAD;
    wrap = (uint32_t)((float)clk / (div * (float)hz)); if (wrap < 2u) return W_BAD;
    wrap -= 1u; if (wrap > 65535u) wrap = 65535u;
    pwm_set_clkdiv(slice, div); pwm_set_wrap(slice, (uint16_t)wrap);
    pwm_set_gpio_level(pin, (uint16_t)(((uint64_t)duty16 * (wrap + 1u)) >> 16));
    pwm_set_enabled(slice, true);
    return W_OK;
}
static w_st plat_analog(uint32_t pin, uint32_t v) {
    if (v > ECCLES_WASM_ANALOG_MAX) return W_BAD;
    return plat_pwm(pin, ECCLES_WASM_ANALOG_FREQ_HZ, (uint32_t)(((uint64_t)v * 65535u) / ECCLES_WASM_ANALOG_MAX));
}
static w_st plat_adc(uint32_t pin, uint32_t *v) {
    static uint8_t init;
    if (pin < 26u || pin > 29u) return W_BAD;
    if (!init) { adc_init(); init = 1; }
    adc_gpio_init(pin); adc_select_input(pin - 26u);
    *v = adc_read(); return W_OK;
}
static w_st plat_millis(uint32_t *v) { *v = (uint32_t)(time_us_64() / 1000u); return W_OK; }
static w_st plat_micros(uint32_t *v) { *v = time_us_32(); return W_OK; }
static w_st plat_sleep_ms(uint32_t ms) { sleep_ms(ms); return W_OK; }
static w_st plat_sleep_us(uint32_t us) { sleep_us(us); return W_OK; }
static w_st plat_yield(void) { tight_loop_contents(); return W_OK; }
static w_st plat_reset(void) { watchdog_reboot(0, 0, 0); for (;;) tight_loop_contents(); return W_OK; }
static w_st plat_random(uint32_t *v) { *v = get_rand_32(); return W_OK; }
static w_st plat_wdt_feed(void) { watchdog_update(); return W_OK; }
static w_st plat_log(const uint8_t *b, uint32_t n) { fwrite(b, 1, n, stdout); fflush(stdout); return W_OK; }

static void w_gpio_cb(uint gpio, uint32_t events) {
    int i;
    for (i = 0; i < ECCLES_WASM_MAX_IRQ; i++)
        if (w_irq[i].edge && w_irq[i].pin == (int32_t)gpio) w_irq[i].count++;
}
static w_st plat_irq_attach(uint32_t pin, uint32_t edge) {
    int s = w_irq_slot(pin, 1); uint32_t ev = 0;
    if (s < 0 || pin >= NUM_BANK0_GPIOS) return W_BAD;
    if (edge & ECCLES_WASM_EDGE_RISING) ev |= GPIO_IRQ_EDGE_RISE;
    if (edge & ECCLES_WASM_EDGE_FALLING) ev |= GPIO_IRQ_EDGE_FALL;
    w_irq[s].pin = (int32_t)pin; w_irq[s].edge = (uint8_t)edge; w_irq[s].count = 0;
    gpio_set_irq_enabled_with_callback(pin, ev, true, &w_gpio_cb);
    return W_OK;
}
static w_st plat_irq_count(uint32_t pin, uint32_t *c) { int s = w_irq_slot(pin, 0); if (s < 0) return W_BAD; *c = w_irq[s].count; return W_OK; }
static w_st plat_irq_clear(uint32_t pin) { int s = w_irq_slot(pin, 0); if (s < 0) return W_BAD; w_irq[s].count = 0; return W_OK; }
static w_st plat_irq_detach(uint32_t pin) {
    int s = w_irq_slot(pin, 0);
    if (s < 0) return W_BAD;
    gpio_set_irq_enabled(pin, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, false); w_irq[s].edge = 0; return W_OK;
}

static uart_inst_t *w_uart(uint32_t p) { return p == 0 ? uart0 : p == 1 ? uart1 : NULL; }
static i2c_inst_t *w_i2c(uint32_t b) { return b == 0 ? i2c0 : b == 1 ? i2c1 : NULL; }
static spi_inst_t *w_spi(uint32_t b) { return b == 0 ? spi0 : b == 1 ? spi1 : NULL; }
static w_st plat_uart_begin(uint32_t p, uint32_t baud, int32_t tx, int32_t rx) {
    uart_inst_t *u = w_uart(p);
    if (!u || !baud) return W_BAD;
    uart_init(u, baud);
    if (tx >= 0) gpio_set_function((uint)tx, GPIO_FUNC_UART);
    if (rx >= 0) gpio_set_function((uint)rx, GPIO_FUNC_UART);
    return W_OK;
}
static w_st plat_uart_write(uint32_t p, const uint8_t *b, uint32_t n) { uart_inst_t *u = w_uart(p); if (!u) return W_BAD; uart_write_blocking(u, b, n); return W_OK; }
static w_st plat_uart_read(uint32_t p, uint8_t *b, uint32_t n, uint32_t to, uint32_t *got) {
    uart_inst_t *u = w_uart(p); uint32_t k = 0;
    if (!u) return W_BAD;
    while (k < n && uart_is_readable_within_us(u, to * 1000u)) b[k++] = uart_getc(u);
    *got = k; return W_OK;
}
static w_st plat_uart_end(uint32_t p) { uart_inst_t *u = w_uart(p); if (!u) return W_BAD; uart_deinit(u); return W_OK; }
static w_st plat_i2c_begin(uint32_t b, int32_t sda, int32_t scl, uint32_t f) {
    i2c_inst_t *i = w_i2c(b);
    if (!i || sda < 0 || scl < 0) return W_BAD;
    i2c_init(i, f ? f : 100000u);
    gpio_set_function((uint)sda, GPIO_FUNC_I2C); gpio_set_function((uint)scl, GPIO_FUNC_I2C);
    gpio_pull_up((uint)sda); gpio_pull_up((uint)scl);
    return W_OK;
}
static w_st plat_i2c_write(uint32_t b, uint32_t a, const uint8_t *d, uint32_t n) {
    i2c_inst_t *i = w_i2c(b); if (!i) return W_BAD;
    return i2c_write_blocking(i, (uint8_t)a, d, n, false) == (int)n ? W_OK : W_BAD;
}
static w_st plat_i2c_read(uint32_t b, uint32_t a, uint8_t *d, uint32_t n) {
    i2c_inst_t *i = w_i2c(b); if (!i) return W_BAD;
    return i2c_read_blocking(i, (uint8_t)a, d, n, false) == (int)n ? W_OK : W_BAD;
}
static w_st plat_i2c_write_read(uint32_t b, uint32_t a, const uint8_t *t, uint32_t tn, uint8_t *r, uint32_t rn) {
    i2c_inst_t *i = w_i2c(b); if (!i) return W_BAD;
    if (i2c_write_blocking(i, (uint8_t)a, t, tn, true) != (int)tn) return W_BAD;      /* repeated start */
    return i2c_read_blocking(i, (uint8_t)a, r, rn, false) == (int)rn ? W_OK : W_BAD;
}
static w_st plat_spi_begin(uint32_t b, int32_t sck, int32_t mosi, int32_t miso, uint32_t f, uint32_t mode) {
    spi_inst_t *s = w_spi(b);
    if (!s || mode > 3) return W_BAD;
    spi_init(s, f ? f : 1000000u);
    spi_set_format(s, 8, (spi_cpol_t)((mode >> 1) & 1), (spi_cpha_t)(mode & 1), SPI_MSB_FIRST);
    if (sck >= 0) gpio_set_function((uint)sck, GPIO_FUNC_SPI);
    if (mosi >= 0) gpio_set_function((uint)mosi, GPIO_FUNC_SPI);
    if (miso >= 0) gpio_set_function((uint)miso, GPIO_FUNC_SPI);
    return W_OK;
}
static w_st plat_spi_transfer(uint32_t b, const uint8_t *tx, uint8_t *rx, uint32_t n) {
    spi_inst_t *s = w_spi(b); static const uint8_t zeros[32];
    if (!s) return W_BAD;
    if (tx && rx) spi_write_read_blocking(s, tx, rx, n);
    else if (tx) spi_write_blocking(s, tx, n);
    else if (rx) { uint32_t o = 0; while (o < n) { uint32_t k = n - o < 32u ? n - o : 32u; spi_write_read_blocking(s, zeros, rx + o, k); o += k; } }
    return W_OK;
}
static w_st plat_spi_end(uint32_t b) { spi_inst_t *s = w_spi(b); if (!s) return W_BAD; spi_deinit(s); return W_OK; }
#define W_HAS_gpio_mode 1
#define W_HAS_gpio_write 1
#define W_HAS_gpio_read 1
#define W_HAS_analog 1
#define W_HAS_pwm 1
#define W_HAS_adc 1
#define W_HAS_millis 1
#define W_HAS_micros 1
#define W_HAS_sleep_ms 1
#define W_HAS_sleep_us 1
#define W_HAS_yield 1
#define W_HAS_reset 1
#define W_HAS_random 1
#define W_HAS_wdt_feed 1
#define W_HAS_log 1
#define W_HAS_irq_attach 1
#define W_HAS_irq_count 1
#define W_HAS_irq_clear 1
#define W_HAS_irq_detach 1
#define W_HAS_uart_begin 1
#define W_HAS_uart_write 1
#define W_HAS_uart_read 1
#define W_HAS_uart_end 1
#define W_HAS_i2c_begin 1
#define W_HAS_i2c_write 1
#define W_HAS_i2c_read 1
#define W_HAS_i2c_write_read 1
#define W_HAS_spi_begin 1
#define W_HAS_spi_transfer 1
#define W_HAS_spi_end 1

/* ============================================================= STM32 HAL */
#elif ECCLES_WASM_PLATFORM == ECCLES_WASM_PLAT_STM32_HAL
#ifndef ECCLES_WASM_STM32_HAL_HEADER
  #define ECCLES_WASM_STM32_HAL_HEADER "main.h"
#endif
#include ECCLES_WASM_STM32_HAL_HEADER

static GPIO_TypeDef *w_port(uint32_t pin) {
    static GPIO_TypeDef *const ports[] = {
#ifdef GPIOA
        GPIOA,
#else
        0,
#endif
#ifdef GPIOB
        GPIOB,
#else
        0,
#endif
#ifdef GPIOC
        GPIOC,
#else
        0,
#endif
#ifdef GPIOD
        GPIOD,
#else
        0,
#endif
#ifdef GPIOE
        GPIOE,
#else
        0,
#endif
#ifdef GPIOF
        GPIOF,
#else
        0,
#endif
#ifdef GPIOG
        GPIOG,
#else
        0,
#endif
#ifdef GPIOH
        GPIOH,
#else
        0,
#endif
    };
    return (pin >> 4) < sizeof ports / sizeof ports[0] ? ports[pin >> 4] : 0;
}
static void w_port_clk(uint32_t idx) {
    switch (idx) {
#ifdef GPIOA
    case 0: __HAL_RCC_GPIOA_CLK_ENABLE(); break;
#endif
#ifdef GPIOB
    case 1: __HAL_RCC_GPIOB_CLK_ENABLE(); break;
#endif
#ifdef GPIOC
    case 2: __HAL_RCC_GPIOC_CLK_ENABLE(); break;
#endif
#ifdef GPIOD
    case 3: __HAL_RCC_GPIOD_CLK_ENABLE(); break;
#endif
#ifdef GPIOE
    case 4: __HAL_RCC_GPIOE_CLK_ENABLE(); break;
#endif
#ifdef GPIOF
    case 5: __HAL_RCC_GPIOF_CLK_ENABLE(); break;
#endif
#ifdef GPIOG
    case 6: __HAL_RCC_GPIOG_CLK_ENABLE(); break;
#endif
#ifdef GPIOH
    case 7: __HAL_RCC_GPIOH_CLK_ENABLE(); break;
#endif
    default: break;
    }
}
static w_st plat_gpio_mode(uint32_t pin, uint32_t mode) {
    GPIO_TypeDef *p = w_port(pin); GPIO_InitTypeDef g; memset(&g, 0, sizeof g);
    if (!p) return W_BAD;
    w_port_clk(pin >> 4);
    g.Pin = (uint16_t)(1u << (pin & 15u)); g.Speed = GPIO_SPEED_FREQ_LOW; g.Pull = GPIO_NOPULL;
    switch (mode) {
    case ECCLES_WASM_PIN_INPUT: g.Mode = GPIO_MODE_INPUT; break;
    case ECCLES_WASM_PIN_INPUT_PULLUP: g.Mode = GPIO_MODE_INPUT; g.Pull = GPIO_PULLUP; break;
    case ECCLES_WASM_PIN_INPUT_PULLDOWN: g.Mode = GPIO_MODE_INPUT; g.Pull = GPIO_PULLDOWN; break;
    case ECCLES_WASM_PIN_OUTPUT: g.Mode = GPIO_MODE_OUTPUT_PP; break;
    case ECCLES_WASM_PIN_OPEN_DRAIN: g.Mode = GPIO_MODE_OUTPUT_OD; break;
    default: return W_BAD;
    }
    HAL_GPIO_Init(p, &g); return W_OK;
}
static w_st plat_gpio_write(uint32_t pin, uint32_t l) {
    GPIO_TypeDef *p = w_port(pin); if (!p) return W_BAD;
    HAL_GPIO_WritePin(p, (uint16_t)(1u << (pin & 15u)), l ? GPIO_PIN_SET : GPIO_PIN_RESET); return W_OK;
}
static w_st plat_gpio_read(uint32_t pin, uint32_t *l) {
    GPIO_TypeDef *p = w_port(pin); if (!p) return W_BAD;
    *l = HAL_GPIO_ReadPin(p, (uint16_t)(1u << (pin & 15u))) == GPIO_PIN_SET ? 1u : 0u; return W_OK;
}
static w_st plat_millis(uint32_t *v) { *v = HAL_GetTick(); return W_OK; }
static w_st plat_micros(uint32_t *v) {                 /* SysTick-based, works on every Cortex-M */
    uint32_t ms, val, load;
    do { ms = HAL_GetTick(); val = SysTick->VAL; } while (ms != HAL_GetTick());
    load = SysTick->LOAD + 1u;
    *v = ms * 1000u + ((load - val) * 1000u) / load; return W_OK;
}
static w_st plat_sleep_ms(uint32_t ms) { HAL_Delay(ms); return W_OK; }
static w_st plat_sleep_us(uint32_t us) {
    uint32_t t0, t; plat_micros(&t0);
    do { plat_micros(&t); } while ((uint32_t)(t - t0) < us);
    return W_OK;
}
static w_st plat_yield(void) { return W_OK; }
static w_st plat_reset(void) { NVIC_SystemReset(); return W_OK; }
#define W_HAS_gpio_mode 1
#define W_HAS_gpio_write 1
#define W_HAS_gpio_read 1
#define W_HAS_millis 1
#define W_HAS_micros 1
#define W_HAS_sleep_ms 1
#define W_HAS_sleep_us 1
#define W_HAS_yield 1
#define W_HAS_reset 1
#if defined(ECCLES_WASM_STM32_UART_HANDLES) && defined(HAL_UART_MODULE_ENABLED)
static UART_HandleTypeDef *w_uart_h(uint32_t p) {
    static UART_HandleTypeDef *const h[] = ECCLES_WASM_STM32_UART_HANDLES;
    return p < sizeof h / sizeof h[0] ? h[p] : 0;
}
static w_st plat_uart_begin(uint32_t p, uint32_t baud, int32_t tx, int32_t rx) { return w_uart_h(p) ? W_OK : W_BAD; }
static w_st plat_uart_write(uint32_t p, const uint8_t *b, uint32_t n) {
    UART_HandleTypeDef *h = w_uart_h(p); if (!h) return W_BAD;
    return HAL_UART_Transmit(h, (uint8_t *)b, (uint16_t)n, 1000) == HAL_OK ? W_OK : W_BAD;
}
static w_st plat_uart_read(uint32_t p, uint8_t *b, uint32_t n, uint32_t to, uint32_t *got) {
    UART_HandleTypeDef *h = w_uart_h(p); uint32_t k = 0; if (!h) return W_BAD;
    while (k < n && HAL_UART_Receive(h, &b[k], 1, to) == HAL_OK) k++;
    *got = k; return W_OK;
}
static w_st plat_uart_end(uint32_t p) { return w_uart_h(p) ? W_OK : W_BAD; }
#define W_HAS_uart_begin 1
#define W_HAS_uart_write 1
#define W_HAS_uart_read 1
#define W_HAS_uart_end 1
#endif
#if defined(ECCLES_WASM_STM32_I2C_HANDLES) && defined(HAL_I2C_MODULE_ENABLED)
static I2C_HandleTypeDef *w_i2c_h(uint32_t b) {
    static I2C_HandleTypeDef *const h[] = ECCLES_WASM_STM32_I2C_HANDLES;
    return b < sizeof h / sizeof h[0] ? h[b] : 0;
}
static w_st plat_i2c_begin(uint32_t b, int32_t sda, int32_t scl, uint32_t f) { return w_i2c_h(b) ? W_OK : W_BAD; }
static w_st plat_i2c_write(uint32_t b, uint32_t a, const uint8_t *d, uint32_t n) {
    I2C_HandleTypeDef *h = w_i2c_h(b); if (!h) return W_BAD;
    return HAL_I2C_Master_Transmit(h, (uint16_t)(a << 1), (uint8_t *)d, (uint16_t)n, 1000) == HAL_OK ? W_OK : W_BAD;
}
static w_st plat_i2c_read(uint32_t b, uint32_t a, uint8_t *d, uint32_t n) {
    I2C_HandleTypeDef *h = w_i2c_h(b); if (!h) return W_BAD;
    return HAL_I2C_Master_Receive(h, (uint16_t)(a << 1), d, (uint16_t)n, 1000) == HAL_OK ? W_OK : W_BAD;
}
/* NOTE: the polling HAL API cannot issue a repeated start - this is write, STOP, read. */
static w_st plat_i2c_write_read(uint32_t b, uint32_t a, const uint8_t *t, uint32_t tn, uint8_t *r, uint32_t rn) {
    w_st s = plat_i2c_write(b, a, t, tn);
    return s == W_OK ? plat_i2c_read(b, a, r, rn) : s;
}
#define W_HAS_i2c_begin 1
#define W_HAS_i2c_write 1
#define W_HAS_i2c_read 1
#define W_HAS_i2c_write_read 1
#endif
#if defined(ECCLES_WASM_STM32_SPI_HANDLES) && defined(HAL_SPI_MODULE_ENABLED)
static SPI_HandleTypeDef *w_spi_h(uint32_t b) {
    static SPI_HandleTypeDef *const h[] = ECCLES_WASM_STM32_SPI_HANDLES;
    return b < sizeof h / sizeof h[0] ? h[b] : 0;
}
static w_st plat_spi_begin(uint32_t b, int32_t a, int32_t m, int32_t i, uint32_t f, uint32_t mode) { return w_spi_h(b) ? W_OK : W_BAD; }
static w_st plat_spi_transfer(uint32_t b, const uint8_t *tx, uint8_t *rx, uint32_t n) {
    SPI_HandleTypeDef *h = w_spi_h(b); HAL_StatusTypeDef s; static uint8_t dummy[16];
    if (!h) return W_BAD;
    if (tx && rx) s = HAL_SPI_TransmitReceive(h, (uint8_t *)tx, rx, (uint16_t)n, 1000);
    else if (tx) s = HAL_SPI_Transmit(h, (uint8_t *)tx, (uint16_t)n, 1000);
    else if (rx) s = HAL_SPI_Receive(h, rx, (uint16_t)n, 1000);
    else s = HAL_OK;
    (void)dummy; return s == HAL_OK ? W_OK : W_BAD;
}
static w_st plat_spi_end(uint32_t b) { return w_spi_h(b) ? W_OK : W_BAD; }
#define W_HAS_spi_begin 1
#define W_HAS_spi_transfer 1
#define W_HAS_spi_end 1
#endif

/* ============================================================== ZEPHYR */
#elif ECCLES_WASM_PLATFORM == ECCLES_WASM_PLAT_ZEPHYR
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/printk.h>
#if defined(CONFIG_REBOOT)
  #include <zephyr/sys/reboot.h>
#endif
#if defined(CONFIG_ENTROPY_GENERATOR) || defined(CONFIG_TEST_RANDOM_GENERATOR) || defined(CONFIG_XOSHIRO_RANDOM_GENERATOR)
  #include <zephyr/random/random.h>
  #define W_ZEPHYR_RNG 1
#endif

static const struct device *w_zport(uint32_t pin) {
    static const struct device *const ports[] = {
#if DT_NODE_HAS_STATUS(DT_NODELABEL(gpio0), okay)
        DEVICE_DT_GET(DT_NODELABEL(gpio0)),
#else
        NULL,
#endif
#if DT_NODE_HAS_STATUS(DT_NODELABEL(gpio1), okay)
        DEVICE_DT_GET(DT_NODELABEL(gpio1)),
#else
        NULL,
#endif
#if DT_NODE_HAS_STATUS(DT_NODELABEL(gpio2), okay)
        DEVICE_DT_GET(DT_NODELABEL(gpio2)),
#else
        NULL,
#endif
#if DT_NODE_HAS_STATUS(DT_NODELABEL(gpio3), okay)
        DEVICE_DT_GET(DT_NODELABEL(gpio3)),
#else
        NULL,
#endif
    };
    const struct device *d = (pin >> 5) < sizeof ports / sizeof ports[0] ? ports[pin >> 5] : NULL;
    return (d && device_is_ready(d)) ? d : NULL;
}
static w_st plat_gpio_mode(uint32_t pin, uint32_t mode) {
    const struct device *d = w_zport(pin); gpio_flags_t f;
    if (!d) return W_BAD;
    switch (mode) {
    case ECCLES_WASM_PIN_INPUT: f = GPIO_INPUT; break;
    case ECCLES_WASM_PIN_INPUT_PULLUP: f = GPIO_INPUT | GPIO_PULL_UP; break;
    case ECCLES_WASM_PIN_INPUT_PULLDOWN: f = GPIO_INPUT | GPIO_PULL_DOWN; break;
    case ECCLES_WASM_PIN_OUTPUT: f = GPIO_OUTPUT | GPIO_INPUT; break;
    case ECCLES_WASM_PIN_OPEN_DRAIN: f = GPIO_OUTPUT | GPIO_INPUT | GPIO_OPEN_DRAIN; break;
    default: return W_BAD;
    }
    return gpio_pin_configure(d, (gpio_pin_t)(pin & 31u), f) == 0 ? W_OK : W_BAD;
}
static w_st plat_gpio_write(uint32_t pin, uint32_t l) {
    const struct device *d = w_zport(pin); if (!d) return W_BAD;
    return gpio_pin_set_raw(d, (gpio_pin_t)(pin & 31u), l ? 1 : 0) == 0 ? W_OK : W_BAD;
}
static w_st plat_gpio_read(uint32_t pin, uint32_t *l) {
    const struct device *d = w_zport(pin); int r; if (!d) return W_BAD;
    r = gpio_pin_get_raw(d, (gpio_pin_t)(pin & 31u)); if (r < 0) return W_BAD;
    *l = (uint32_t)r; return W_OK;
}
static w_st plat_millis(uint32_t *v) { *v = k_uptime_get_32(); return W_OK; }
static w_st plat_micros(uint32_t *v) { *v = (uint32_t)k_ticks_to_us_floor64(k_uptime_ticks()); return W_OK; }
static w_st plat_sleep_ms(uint32_t ms) { k_msleep((int32_t)ms); return W_OK; }
static w_st plat_sleep_us(uint32_t us) { k_busy_wait(us); return W_OK; }
static w_st plat_yield(void) { k_yield(); return W_OK; }
#if defined(CONFIG_REBOOT)
static w_st plat_reset(void) { sys_reboot(SYS_REBOOT_COLD); return W_OK; }
#define W_HAS_reset 1
#endif
#if defined(W_ZEPHYR_RNG)
static w_st plat_random(uint32_t *v) { *v = sys_rand32_get(); return W_OK; }
#define W_HAS_random 1
#endif
static w_st plat_log(const uint8_t *b, uint32_t n) { uint32_t i; for (i = 0; i < n; i++) printk("%c", b[i]); return W_OK; }
#define W_HAS_gpio_mode 1
#define W_HAS_gpio_write 1
#define W_HAS_gpio_read 1
#define W_HAS_millis 1
#define W_HAS_micros 1
#define W_HAS_sleep_ms 1
#define W_HAS_sleep_us 1
#define W_HAS_yield 1
#define W_HAS_log 1

#endif /* platform */

/* ---- stubs: UNSUPPORTED for everything a backend didn't provide ---- */
#define W_STUB(name, params) static w_st plat_##name params { return W_NS; }
#ifndef W_HAS_gpio_mode
W_STUB(gpio_mode, (uint32_t pin, uint32_t mode))
#endif
#ifndef W_HAS_gpio_write
W_STUB(gpio_write, (uint32_t pin, uint32_t level))
#endif
#ifndef W_HAS_gpio_read
W_STUB(gpio_read, (uint32_t pin, uint32_t *level))
#endif
#ifndef W_HAS_analog
W_STUB(analog, (uint32_t pin, uint32_t value))
#endif
#ifndef W_HAS_pwm
W_STUB(pwm, (uint32_t pin, uint32_t hz, uint32_t duty))
#endif
#ifndef W_HAS_adc
W_STUB(adc, (uint32_t pin, uint32_t *value))
#endif
#ifndef W_HAS_dac
W_STUB(dac, (uint32_t pin, uint32_t value))
#endif
#ifndef W_HAS_millis
W_STUB(millis, (uint32_t *v))
#endif
#ifndef W_HAS_micros
W_STUB(micros, (uint32_t *v))
#endif
#ifndef W_HAS_sleep_ms
W_STUB(sleep_ms, (uint32_t ms))
#endif
#ifndef W_HAS_sleep_us
W_STUB(sleep_us, (uint32_t us))
#endif
#ifndef W_HAS_yield
W_STUB(yield, (void))
#endif
#ifndef W_HAS_deep_sleep
W_STUB(deep_sleep, (uint32_t ms))
#endif
#ifndef W_HAS_reset
W_STUB(reset, (void))
#endif
#ifndef W_HAS_random
W_STUB(random, (uint32_t *v))
#endif
#ifndef W_HAS_wdt_feed
W_STUB(wdt_feed, (void))
#endif
#ifndef W_HAS_log
W_STUB(log, (const uint8_t *b, uint32_t n))
#endif
#ifndef W_HAS_irq_attach
W_STUB(irq_attach, (uint32_t pin, uint32_t edge))
#endif
#ifndef W_HAS_irq_count
W_STUB(irq_count, (uint32_t pin, uint32_t *c))
#endif
#ifndef W_HAS_irq_clear
W_STUB(irq_clear, (uint32_t pin))
#endif
#ifndef W_HAS_irq_detach
W_STUB(irq_detach, (uint32_t pin))
#endif
#ifndef W_HAS_uart_begin
W_STUB(uart_begin, (uint32_t p, uint32_t baud, int32_t tx, int32_t rx))
#endif
#ifndef W_HAS_uart_write
W_STUB(uart_write, (uint32_t p, const uint8_t *b, uint32_t n))
#endif
#ifndef W_HAS_uart_read
W_STUB(uart_read, (uint32_t p, uint8_t *b, uint32_t n, uint32_t to, uint32_t *got))
#endif
#ifndef W_HAS_uart_end
W_STUB(uart_end, (uint32_t p))
#endif
#ifndef W_HAS_i2c_begin
W_STUB(i2c_begin, (uint32_t b, int32_t sda, int32_t scl, uint32_t f))
#endif
#ifndef W_HAS_i2c_write
W_STUB(i2c_write, (uint32_t b, uint32_t a, const uint8_t *d, uint32_t n))
#endif
#ifndef W_HAS_i2c_read
W_STUB(i2c_read, (uint32_t b, uint32_t a, uint8_t *d, uint32_t n))
#endif
#ifndef W_HAS_i2c_write_read
W_STUB(i2c_write_read, (uint32_t b, uint32_t a, const uint8_t *t, uint32_t tn, uint8_t *r, uint32_t rn))
#endif
#ifndef W_HAS_spi_begin
W_STUB(spi_begin, (uint32_t b, int32_t sck, int32_t mosi, int32_t miso, uint32_t f, uint32_t mode))
#endif
#ifndef W_HAS_spi_transfer
W_STUB(spi_transfer, (uint32_t b, const uint8_t *tx, uint8_t *rx, uint32_t n))
#endif
#ifndef W_HAS_spi_end
W_STUB(spi_end, (uint32_t b))
#endif

static const eccles_wasm_hal_t g_plat = {
    plat_gpio_mode, plat_gpio_write, plat_gpio_read, plat_analog, plat_pwm, plat_adc, plat_dac,
    plat_millis, plat_micros, plat_sleep_ms, plat_sleep_us, plat_yield, plat_deep_sleep, plat_reset,
    plat_random, plat_wdt_feed, plat_log, plat_irq_attach, plat_irq_count, plat_irq_clear, plat_irq_detach,
    plat_uart_begin, plat_uart_write, plat_uart_read, plat_uart_end,
    plat_i2c_begin, plat_i2c_write, plat_i2c_read, plat_i2c_write_read,
    plat_spi_begin, plat_spi_transfer, plat_spi_end
};

/* ===================================================================== *
 *  public hardware API (dispatch: run-time override first, then backend)
 * ===================================================================== */

#define W_HAL(f) (g_hal_user.f ? g_hal_user.f : g_plat.f)
#define W_HWL(expr) { w_st r_; ECCLES_WASM_HW_LOCK(); r_ = (expr); ECCLES_WASM_HW_UNLOCK(); return r_; }

w_st eccles_wasm_mode(uint32_t pin, uint32_t mode) W_HWL(W_HAL(gpio_mode)(pin, mode))
w_st eccles_wasm_write(uint32_t pin, uint32_t lvl) W_HWL(W_HAL(gpio_write)(pin, lvl))
w_st eccles_wasm_read(uint32_t pin, uint32_t *lvl) W_HWL(W_HAL(gpio_read)(pin, lvl))
w_st eccles_wasm_enable(uint32_t pin) {
    w_st r_; ECCLES_WASM_HW_LOCK();
    r_ = (W_HAL(gpio_mode)(pin, ECCLES_WASM_PIN_OUTPUT) == W_BAD) ? W_BAD : W_HAL(gpio_write)(pin, 1);
    ECCLES_WASM_HW_UNLOCK(); return r_;
}
w_st eccles_wasm_disable(uint32_t pin) {
    w_st r_; ECCLES_WASM_HW_LOCK();
    r_ = (W_HAL(gpio_mode)(pin, ECCLES_WASM_PIN_OUTPUT) == W_BAD) ? W_BAD : W_HAL(gpio_write)(pin, 0);
    ECCLES_WASM_HW_UNLOCK(); return r_;
}
w_st eccles_wasm_toggle(uint32_t pin) {
    uint32_t l = 0; w_st s; ECCLES_WASM_HW_LOCK();
    s = W_HAL(gpio_read)(pin, &l);
    if (s == W_OK) s = W_HAL(gpio_write)(pin, l ? 0u : 1u);
    ECCLES_WASM_HW_UNLOCK(); return s;
}
w_st eccles_wasm_analog(uint32_t pin, uint32_t v) W_HWL(W_HAL(analog)(pin, v))
w_st eccles_wasm_pwm(uint32_t pin, uint32_t hz, uint32_t duty) W_HWL(W_HAL(pwm)(pin, hz, duty))
w_st eccles_wasm_adc(uint32_t pin, uint32_t *v) W_HWL(W_HAL(adc)(pin, v))
w_st eccles_wasm_dac(uint32_t pin, uint32_t v) W_HWL(W_HAL(dac)(pin, v))
w_st eccles_wasm_sleep(uint32_t ms)                 { return W_HAL(sleep_ms)(ms); }
w_st eccles_wasm_sleep_us(uint32_t us) {
    w_st s = W_HAL(sleep_us)(us);
    if (s == W_NS) {                                         /* no delay primitive: spin on micros() */
        uint32_t t0, t;
        if (W_HAL(micros)(&t0) != W_OK) return W_NS;
        do { (void)W_HAL(micros)(&t); } while ((uint32_t)(t - t0) < us);
        return W_OK;
    }
    return s;
}
w_st eccles_wasm_millis(uint32_t *v)                { return W_HAL(millis)(v); }
w_st eccles_wasm_micros(uint32_t *v)                { return W_HAL(micros)(v); }
w_st eccles_wasm_yield(void)                        { return W_HAL(yield)(); }
w_st eccles_wasm_deep_sleep(uint32_t ms)            { return W_HAL(deep_sleep)(ms); }
w_st eccles_wasm_reset(void)                        { return W_HAL(reset)(); }
w_st eccles_wasm_wdt_feed(void)                     { return W_HAL(wdt_feed)(); }
w_st eccles_wasm_random(uint32_t *v) W_HWL(W_HAL(random)(v))
w_st eccles_wasm_log(const uint8_t *b, uint32_t n) W_HWL(W_HAL(log)(b, n))
w_st eccles_wasm_irq_attach(uint32_t pin, uint32_t e) W_HWL(W_HAL(irq_attach)(pin, e))
w_st eccles_wasm_irq_count(uint32_t pin, uint32_t *c) W_HWL(W_HAL(irq_count)(pin, c))
w_st eccles_wasm_irq_clear(uint32_t pin) W_HWL(W_HAL(irq_clear)(pin))
w_st eccles_wasm_irq_detach(uint32_t pin) W_HWL(W_HAL(irq_detach)(pin))
w_st eccles_wasm_uart_begin(uint32_t p, uint32_t baud, int32_t tx, int32_t rx) W_HWL(W_HAL(uart_begin)(p, baud, tx, rx))
w_st eccles_wasm_uart_write(uint32_t p, const uint8_t *b, uint32_t n) W_HWL(W_HAL(uart_write)(p, b, n))
w_st eccles_wasm_uart_read(uint32_t p, uint8_t *b, uint32_t n, uint32_t to, uint32_t *got) { return W_HAL(uart_read)(p, b, n, to, got); }
w_st eccles_wasm_uart_end(uint32_t p) W_HWL(W_HAL(uart_end)(p))
w_st eccles_wasm_i2c_begin(uint32_t b, int32_t sda, int32_t scl, uint32_t f) W_HWL(W_HAL(i2c_begin)(b, sda, scl, f))
w_st eccles_wasm_i2c_write(uint32_t b, uint32_t a, const uint8_t *d, uint32_t n) W_HWL(W_HAL(i2c_write)(b, a, d, n))
w_st eccles_wasm_i2c_read(uint32_t b, uint32_t a, uint8_t *d, uint32_t n) W_HWL(W_HAL(i2c_read)(b, a, d, n))
w_st eccles_wasm_i2c_write_read(uint32_t b, uint32_t a, const uint8_t *t, uint32_t tn, uint8_t *r, uint32_t rn) W_HWL(W_HAL(i2c_write_read)(b, a, t, tn, r, rn))
w_st eccles_wasm_spi_begin(uint32_t b, int32_t sck, int32_t mosi, int32_t miso, uint32_t f, uint32_t m) W_HWL(W_HAL(spi_begin)(b, sck, mosi, miso, f, m))
w_st eccles_wasm_spi_transfer(uint32_t b, const uint8_t *t, uint8_t *r, uint32_t n) W_HWL(W_HAL(spi_transfer)(b, t, r, n))
w_st eccles_wasm_spi_end(uint32_t b) W_HWL(W_HAL(spi_end)(b))

#if defined(__GNUC__) || defined(__clang__)
  #pragma GCC diagnostic pop
#endif

/* ===================================================================== *
 *  guest-facing import table
 * ===================================================================== */

typedef struct { const char *name; eccles_wasm_host_fn fn; const char *sig; } w_builtin_t;

#define A(i)  ((uint32_t)s[i])
#define SA(i) ((int32_t)(uint32_t)s[i])
#define RET(st) do { s[0] = (uint64_t)(st); return ECCLES_WASM_SUCCESSFUL; } while (0)
static void w_put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static W_MAYBE_UNUSED void w_put64(uint8_t *p, uint64_t v) { w_put32(p, (uint32_t)v); w_put32(p + 4, (uint32_t)(v >> 32)); }
static W_MAYBE_UNUSED uint32_t w_get32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

#if !defined(ECCLES_WASM_NO_BUILTIN_IMPORTS)
#define HW1(name, call) static eccles_wasm_status_t hw_##name(eccles_wasm_vm_t *vm, uint64_t *s) { (void)vm; RET(call); }
#define HWOUT(name, outi, call) static eccles_wasm_status_t hw_##name(eccles_wasm_vm_t *vm, uint64_t *s) { \
    uint32_t v = 0; uint8_t *p = eccles_wasm_vm_mem(vm, A(outi), 4); w_st st; \
    if (!p) { RET(W_BAD); } \
    st = (call); if (st == W_OK) { w_put32(p, v); } RET(st); }

HW1(enable, eccles_wasm_enable(A(0)))
HW1(disable, eccles_wasm_disable(A(0)))
HW1(toggle, eccles_wasm_toggle(A(0)))
HW1(mode, eccles_wasm_mode(A(0), A(1)))
HW1(write, eccles_wasm_write(A(0), A(1)))
HWOUT(read, 1, eccles_wasm_read(A(0), &v))
HW1(analog, eccles_wasm_analog(A(0), A(1)))
HW1(pwm, eccles_wasm_pwm(A(0), A(1), A(2)))
HWOUT(adc, 1, eccles_wasm_adc(A(0), &v))
HW1(dac, eccles_wasm_dac(A(0), A(1)))
HW1(sleep, eccles_wasm_sleep(A(0)))
HW1(sleep_us, eccles_wasm_sleep_us(A(0)))
HWOUT(millis, 0, eccles_wasm_millis(&v))
HWOUT(micros, 0, eccles_wasm_micros(&v))
HW1(yield, eccles_wasm_yield())
HW1(deep_sleep, eccles_wasm_deep_sleep(A(0)))
HW1(reset, eccles_wasm_reset())
HW1(wdt_feed, eccles_wasm_wdt_feed())
HWOUT(random, 0, eccles_wasm_random(&v))
HW1(irq_attach, eccles_wasm_irq_attach(A(0), A(1)))
HWOUT(irq_count, 1, eccles_wasm_irq_count(A(0), &v))
HW1(irq_clear, eccles_wasm_irq_clear(A(0)))
HW1(irq_detach, eccles_wasm_irq_detach(A(0)))
HW1(uart_begin, eccles_wasm_uart_begin(A(0), A(1), SA(2), SA(3)))
HW1(uart_end, eccles_wasm_uart_end(A(0)))
HW1(i2c_begin, eccles_wasm_i2c_begin(A(0), SA(1), SA(2), A(3)))
HW1(spi_begin, eccles_wasm_spi_begin(A(0), SA(1), SA(2), SA(3), A(4), A(5)))
HW1(spi_end, eccles_wasm_spi_end(A(0)))

static eccles_wasm_status_t hw_log(eccles_wasm_vm_t *vm, uint64_t *s) {
    uint8_t *p = eccles_wasm_vm_mem(vm, A(0), A(1)); if (!p) RET(W_BAD);
    RET(eccles_wasm_log(p, A(1)));
}
static eccles_wasm_status_t hw_uart_write(eccles_wasm_vm_t *vm, uint64_t *s) {
    uint8_t *p = eccles_wasm_vm_mem(vm, A(1), A(2)); if (!p) RET(W_BAD);
    RET(eccles_wasm_uart_write(A(0), p, A(2)));
}
static eccles_wasm_status_t hw_uart_read(eccles_wasm_vm_t *vm, uint64_t *s) {
    uint8_t *p = eccles_wasm_vm_mem(vm, A(1), A(2)), *g = eccles_wasm_vm_mem(vm, A(4), 4); uint32_t got = 0; w_st st;
    if (!p || !g) RET(W_BAD);
    st = eccles_wasm_uart_read(A(0), p, A(2), A(3), &got);
    if (st == W_OK) w_put32(g, got);
    RET(st);
}
static eccles_wasm_status_t hw_i2c_write(eccles_wasm_vm_t *vm, uint64_t *s) {
    uint8_t *p = eccles_wasm_vm_mem(vm, A(2), A(3)); if (!p) RET(W_BAD);
    RET(eccles_wasm_i2c_write(A(0), A(1), p, A(3)));
}
static eccles_wasm_status_t hw_i2c_read(eccles_wasm_vm_t *vm, uint64_t *s) {
    uint8_t *p = eccles_wasm_vm_mem(vm, A(2), A(3)); if (!p) RET(W_BAD);
    RET(eccles_wasm_i2c_read(A(0), A(1), p, A(3)));
}
static eccles_wasm_status_t hw_i2c_write_read(eccles_wasm_vm_t *vm, uint64_t *s) {
    uint8_t *t = eccles_wasm_vm_mem(vm, A(2), A(3)), *r = eccles_wasm_vm_mem(vm, A(4), A(5));
    if (!t || !r) RET(W_BAD);
    RET(eccles_wasm_i2c_write_read(A(0), A(1), t, A(3), r, A(5)));
}
static eccles_wasm_status_t hw_spi_transfer(eccles_wasm_vm_t *vm, uint64_t *s) {   /* tx/rx address 0 = "none" */
    uint8_t *t = A(1) ? eccles_wasm_vm_mem(vm, A(1), A(3)) : NULL, *r = A(2) ? eccles_wasm_vm_mem(vm, A(2), A(3)) : NULL;
    if ((A(1) && !t) || (A(2) && !r)) RET(W_BAD);
    RET(eccles_wasm_spi_transfer(A(0), t, r, A(3)));
}

static const w_builtin_t g_hw[] = {
    {"enable", hw_enable, "i(i)"}, {"disable", hw_disable, "i(i)"}, {"toggle", hw_toggle, "i(i)"},
    {"mode", hw_mode, "i(ii)"}, {"write", hw_write, "i(ii)"}, {"read", hw_read, "i(ii)"},
    {"analog", hw_analog, "i(ii)"}, {"pwm", hw_pwm, "i(iii)"}, {"adc", hw_adc, "i(ii)"}, {"dac", hw_dac, "i(ii)"},
    {"sleep", hw_sleep, "i(i)"}, {"sleep_us", hw_sleep_us, "i(i)"}, {"millis", hw_millis, "i(i)"},
    {"micros", hw_micros, "i(i)"}, {"yield", hw_yield, "i()"}, {"deep_sleep", hw_deep_sleep, "i(i)"},
    {"reset", hw_reset, "i()"}, {"wdt_feed", hw_wdt_feed, "i()"}, {"random", hw_random, "i(i)"},
    {"log", hw_log, "i(ii)"},
    {"irq_attach", hw_irq_attach, "i(ii)"}, {"irq_count", hw_irq_count, "i(ii)"},
    {"irq_clear", hw_irq_clear, "i(i)"}, {"irq_detach", hw_irq_detach, "i(i)"},
    {"uart_begin", hw_uart_begin, "i(iiii)"}, {"uart_write", hw_uart_write, "i(iii)"},
    {"uart_read", hw_uart_read, "i(iiiii)"}, {"uart_end", hw_uart_end, "i(i)"},
    {"i2c_begin", hw_i2c_begin, "i(iiii)"}, {"i2c_write", hw_i2c_write, "i(iiii)"},
    {"i2c_read", hw_i2c_read, "i(iiii)"}, {"i2c_write_read", hw_i2c_write_read, "i(iiiiii)"},
    {"spi_begin", hw_spi_begin, "i(iiiiii)"}, {"spi_transfer", hw_spi_transfer, "i(iiii)"}, {"spi_end", hw_spi_end, "i(i)"}
};
#endif /* builtin imports */

#if ECCLES_WASM_WASI
/* ---- minimal WASI: enough for printf()/exit() style C and Rust guests ---- */
#define WASI_OK 0u
#define WASI_EBADF 8u
#define WASI_ENOSYS 52u
static eccles_wasm_status_t wasi_fd_write(eccles_wasm_vm_t *vm, uint64_t *s) {
    uint32_t fd = A(0), iov = A(1), n = A(2), i, total = 0; uint8_t *out = eccles_wasm_vm_mem(vm, A(3), 4);
    if (!out) RET(28u);                                               /* EINVAL */
    if (fd != 1 && fd != 2) RET(WASI_EBADF);
    for (i = 0; i < n; i++) {
        uint8_t *e = eccles_wasm_vm_mem(vm, iov + i * 8u, 8), *d; uint32_t len;
        if (!e) RET(28u);
        len = w_get32(e + 4); d = eccles_wasm_vm_mem(vm, w_get32(e), len);
        if (!d && len) RET(28u);
        if (len) (void)eccles_wasm_log(d, len);
        total += len;
    }
    w_put32(out, total); RET(WASI_OK);
}
static eccles_wasm_status_t wasi_proc_exit(eccles_wasm_vm_t *vm, uint64_t *s) {
    vm->exit_req = 1; vm->exit_code = (int32_t)A(0); (void)s; return ECCLES_WASM_FAILED;   /* unwinds the guest */
}
static eccles_wasm_status_t wasi_sizes_get(eccles_wasm_vm_t *vm, uint64_t *s) {            /* args_sizes_get / environ_sizes_get: none */
    uint8_t *a = eccles_wasm_vm_mem(vm, A(0), 4), *b = eccles_wasm_vm_mem(vm, A(1), 4);
    if (!a || !b) RET(28u);
    w_put32(a, 0); w_put32(b, 0); RET(WASI_OK);
}
static eccles_wasm_status_t wasi_get(eccles_wasm_vm_t *vm, uint64_t *s) { (void)vm; RET(WASI_OK); }  /* args_get / environ_get: nothing to copy */
static eccles_wasm_status_t wasi_clock_time_get(eccles_wasm_vm_t *vm, uint64_t *s) {       /* (id, precision i64, out) -> ns since boot */
    uint32_t now; uint8_t *out = eccles_wasm_vm_mem(vm, A(2), 8);
    if (!out) RET(28u);
    if (eccles_wasm_micros(&now) != W_OK) RET(WASI_ENOSYS);
    vm->wasi_acc += (uint32_t)(now - vm->wasi_last); vm->wasi_last = now;   /* extend the 32-bit counter */
    w_put64(out, vm->wasi_acc * 1000u); RET(WASI_OK);
}
static eccles_wasm_status_t wasi_random_get(eccles_wasm_vm_t *vm, uint64_t *s) {
    uint8_t *p = eccles_wasm_vm_mem(vm, A(0), A(1)); uint32_t i = 0, v;
    if (!p && A(1)) RET(28u);
    while (i < A(1)) {
        uint32_t k;
        if (eccles_wasm_random(&v) != W_OK) RET(WASI_ENOSYS);
        for (k = 0; k < 4 && i < A(1); k++, i++) p[i] = (uint8_t)(v >> (8 * k));
    }
    RET(WASI_OK);
}
static eccles_wasm_status_t wasi_sched_yield(eccles_wasm_vm_t *vm, uint64_t *s) { (void)vm; (void)eccles_wasm_yield(); RET(WASI_OK); }
static const w_builtin_t g_wasi[] = {
    {"fd_write", wasi_fd_write, "i(iiii)"}, {"proc_exit", wasi_proc_exit, "v(i)"},
    {"args_sizes_get", wasi_sizes_get, "i(ii)"}, {"environ_sizes_get", wasi_sizes_get, "i(ii)"},
    {"args_get", wasi_get, "i(ii)"}, {"environ_get", wasi_get, "i(ii)"},
    {"clock_time_get", wasi_clock_time_get, "i(iIi)"}, {"random_get", wasi_random_get, "i(ii)"},
    {"sched_yield", wasi_sched_yield, "i()"}
};
#endif

static int w_find_host(eccles_wasm_vm_t *vm, const char *name, eccles_wasm_host_fn *fn, const char **sig) {
    size_t i;
    if (strncmp(name, "eccles_wasm_", 12) == 0) name += 12;
    for (i = 0; i < vm->nhost; i++)
        if (strcmp(vm->host[i].name, name) == 0) { *fn = vm->host[i].fn; *sig = vm->host[i].sig; return 1; }
#if !defined(ECCLES_WASM_NO_BUILTIN_IMPORTS)
    for (i = 0; i < sizeof g_hw / sizeof g_hw[0]; i++)
        if (strcmp(g_hw[i].name, name) == 0) { *fn = g_hw[i].fn; *sig = g_hw[i].sig; return 1; }
#endif
#if ECCLES_WASM_WASI
    for (i = 0; i < sizeof g_wasi / sizeof g_wasi[0]; i++)
        if (strcmp(g_wasi[i].name, name) == 0) { *fn = g_wasi[i].fn; *sig = g_wasi[i].sig; return 1; }
#endif
    return 0;
}
