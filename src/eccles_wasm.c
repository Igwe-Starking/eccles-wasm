//created by Nwobodo Ecclesiastes Chidera on Jun 18 2025
//this is the day the project started, it took me about 4 months to get everything working
//
//eccles_wasm.c the implementation, i know the file is huge, i will split it into seperate files
//(loader / interpreter / backends) when i have time
//TODO: split this file, its getting to long to scroll
//FIXME: some comments here are outdated, i keep changing the code and forgetting to update them

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
//#include <stdint.h> //already in the header dont include again
//#include <stdio.h> //only for printf debugging, remove before release
#include <string.h>
#if !defined(ECCLES_WASM_NO_FLOAT)
  #include <math.h>      /* sqrt/sqrtf only */
#endif
#if ECCLES_WASM_USE_HEAP
  #include <stdlib.h>
#endif

//debug logging, if you define ECCLES_WASM_DEBUG it prints to the console
//i use it alot when the translator refuses a module and i dont know why
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

//W_NIL marks a slot that has no patch yet (end of a patch list)
#define W_NIL 0xFFFFFFFFu
#define W_PAGE 65536u
//#define W_PAGE 64*1024 //no brackets, broke inside a #if so i wrote the number directly

/* ===================================================================== *
 *  1. state
 * ===================================================================== */

//a function can be a wasm function or a host function (a c function from the user), both use this struct
//if code is NULL then its a host function
//NOTE: keep nparams and nlocals 16bit, i tried 32bit and the struct got bigger on esp32 for no good reason
typedef struct {
    const uint16_t   *code;     /* translated body; NULL for host functions */
    eccles_wasm_host_fn host;
    uint16_t type;              /* canonical type id */
    uint16_t nparams;
    uint16_t nlocals;           /* locals beyond the parameters */
    uint16_t max_h;             /* peak slots (params + locals + operands) */
    uint8_t  nresults;
    //uint8_t isHost; //not needed we just check code == NULL
} w_func_t;

typedef struct { const char *name; uint16_t func; } w_export_t;
typedef struct { const uint16_t *pc; uint64_t *fp; const w_func_t *f; } w_frame_t;
typedef struct { const char *name; eccles_wasm_host_fn fn; const char *sig; } w_host_t;

//G is the global state of the whole runtime, yes its a global and yes its single instance, i know global state is ugly
//but on a microcontroller with 20kb of ram i dont want to pass a handle around everywhere, maybe i will add a handle api later
//NOTE: dont reorder the arena pointers, w_setup depends on them
static struct {
    /* arena */
    uint8_t *base, *end, *lo, *hi, *hi_init;
    uint8_t  inited, loaded, running, own_heap;
    /* diagnostics */
    eccles_wasm_error_t err;
    uint32_t err_info;
    char     err_str[ECCLES_WASM_MAX_NAME + 1];
    /* module */
    uint16_t ntypes, nfuncs, nimports, nglobals, nexports, tsize, start;
    uint8_t  has_mem, has_table, code_seen;
    uint8_t *types;                 /* concatenated records: np, nr, params.., results.. */
    uint16_t *type_off, *type_canon;
    w_func_t *funcs;
    uint64_t *globals;
    uint16_t *table;
    w_export_t *exports;
    uint8_t *mem;
    uint32_t mem_bytes, mem_pages, code_bytes;
    /* runtime */
    uint64_t *stk;
    w_frame_t *frames;
    uint32_t fuel_budget;
    uint8_t  exit_req;
    int32_t  exit_code;
    /* hooks */
    eccles_wasm_hal_t hal;
    eccles_wasm_fs_open_fn fs_open;
    eccles_wasm_fs_read_fn fs_read;
    eccles_wasm_fs_close_fn fs_close;
    w_host_t host[ECCLES_WASM_MAX_HOST_FUNCS];
    uint8_t  nhost;
} G;

#if !ECCLES_WASM_USE_HEAP
static uint8_t g_arena[ECCLES_WASM_ARENA_SIZE] W_ALIGN8;
#endif

//records the FIRST error only, later errors are ignored because they are usually just a result of the first one
static void w_fail(eccles_wasm_error_t e, uint32_t info) {
    if (G.err == ECCLES_WASM_OK) { G.err = e; G.err_info = info; }
}

/* ---- arena: bump from both ends ------------------------------------- */
//low allocator, grows up from the start of the arena. used for module data (types, funcs, translated code)
//NOTE: the overflow check (p + size < p) is for 32bit targets where the pointer can wrap around, found this the hard way
//we dont free anything here, the whole module is dropped at once by w_module_reset()
static void *w_alo(size_t size, size_t align) {
    uintptr_t p = ((uintptr_t)G.lo + (align - 1)) & ~(uintptr_t)(align - 1);
    if (G.err != ECCLES_WASM_OK) return NULL;
    if (p + size > (uintptr_t)G.hi || p + size < p) {
        w_fail(ECCLES_WASM_ERR_NO_MEMORY, (uint32_t)(p + size - (uintptr_t)G.lo)); return NULL;
    }
    G.lo = (uint8_t *)(p + size);
    return (void *)p;
}
//high allocator, grows down from the end of the arena. value stack, call stack and linear memory live here
static void *w_ahi(size_t size, size_t align) {
    uintptr_t p;
    if (G.err != ECCLES_WASM_OK) return NULL;
    if (size > (size_t)(G.hi - G.lo)) { w_fail(ECCLES_WASM_ERR_NO_MEMORY, (uint32_t)size); return NULL; }
    p = ((uintptr_t)G.hi - size) & ~(uintptr_t)(align - 1);
    //p = (uintptr_t)G.hi - size; //old version, forgot the alignment. wasm memory must be 8 aligned or u64 loads crash on cortex m0
    if (p < (uintptr_t)G.lo) { w_fail(ECCLES_WASM_ERR_NO_MEMORY, (uint32_t)size); return NULL; }
    G.hi = (uint8_t *)p;
    return (void *)p;
}

/* ===================================================================== *
 *  2. streaming reader
 * ===================================================================== */

static struct {
    eccles_wasm_read_fn fn;
    void *ctx;
    const uint8_t *p, *e, *start;
    uint32_t pos0;
    uint8_t  mem_src, eof;
    uint8_t  chunk[ECCLES_WASM_CHUNK_SIZE];
} R;

//current position in the .wasm file (not in the chunk), used for error info so you know where the module is bad
static uint32_t rd_pos(void) { return R.pos0 + (uint32_t)(R.p - R.start); }

//refills the chunk buffer from the stream, returns 0 when there is nothing more to read
static int rd_fill(void) {
    int n;
    if (R.mem_src || !R.fn || R.eof) return 0;
    //ECCLES_WASM_LOG_LINE("rd_fill: nothing more to read");
    R.pos0 += (uint32_t)(R.e - R.start);
    n = R.fn(R.ctx, R.chunk, sizeof R.chunk);
    if (n < 0 || n > (int)sizeof R.chunk) { w_fail(ECCLES_WASM_ERR_IO, rd_pos()); n = 0; }
    if (n == 0) R.eof = 1;
    R.start = R.p = R.chunk; R.e = R.chunk + n;
    return n > 0;
}
static int rd_u8(void) {
    if (R.p >= R.e && !rd_fill()) { w_fail(ECCLES_WASM_ERR_MALFORMED, rd_pos()); return 0; }
    return *R.p++;
}
//reads a LEB128 unsigned int (max 5 bytes). NOTE: wasm uses leb128 everywhere so this function is very hot while loading
static uint32_t rd_u32(void) {
    uint32_t r = 0; unsigned s = 0; int b;
    for (;;) {
        b = rd_u8();
        r |= (uint32_t)(b & 0x7F) << s;
        if (!(b & 0x80)) break;
        s += 7;
        if (s > 28 || G.err) { w_fail(ECCLES_WASM_ERR_MALFORMED, rd_pos()); return 0; }
    }
    if (s == 28 && (b & 0x70)) w_fail(ECCLES_WASM_ERR_MALFORMED, rd_pos());
    return r;
}
//signed leb128 for 64bit, the sign extension part confused me for a whole weekend
static int64_t rd_s64(void) {
    uint64_t r = 0; unsigned s = 0; int b;
    do {
        b = rd_u8();
        r |= (uint64_t)(b & 0x7F) << s;
        s += 7;
    } while ((b & 0x80) && s < 70 && !G.err);
    if (b & 0x80) w_fail(ECCLES_WASM_ERR_MALFORMED, rd_pos());
    if (s < 64 && (b & 0x40)) r |= ~(uint64_t)0 << s;
    return (int64_t)r;
}
//signed leb128 for 32bit
static int32_t rd_s32(void) {
    int64_t v = rd_s64();
    if (v < -2147483648LL || v > 2147483647LL) { w_fail(ECCLES_WASM_ERR_MALFORMED, rd_pos()); return 0; }
    return (int32_t)v;
}
#if !defined(ECCLES_WASM_NO_FLOAT)
static uint64_t rd_le(unsigned n) {
    uint64_t v = 0; unsigned i;
    for (i = 0; i < n; i++) v |= (uint64_t)rd_u8() << (8 * i);
    return v;
}
#endif
//skip n bytes without storing them (custom sections and so on)
static void rd_skip(uint32_t n) {
    while (n && !G.err) {
        uint32_t avail = (uint32_t)(R.e - R.p), t;
        if (!avail) { if (!rd_fill()) { w_fail(ECCLES_WASM_ERR_MALFORMED, rd_pos()); return; } continue; }
        t = n < avail ? n : avail; R.p += t; n -= t;
    }
}
//copy n bytes into dst, used for the header and the data segments
static void rd_bytes(uint8_t *dst, uint32_t n) {
    while (n && !G.err) {
        uint32_t avail = (uint32_t)(R.e - R.p), t;
        if (!avail) { if (!rd_fill()) { w_fail(ECCLES_WASM_ERR_MALFORMED, rd_pos()); return; } continue; }
        t = n < avail ? n : avail; memcpy(dst, R.p, t); dst += t; R.p += t; n -= t;
    }
}
/* read a name; copy at most cap-1 bytes (NUL-terminated) into dst (may be NULL) */
//reads a length prefixed name into dst, cap is the size of dst
static uint32_t rd_name(char *dst, uint32_t cap) {
    uint32_t len = rd_u32(), keep = 0;
    if (dst && cap) { keep = len < cap - 1 ? len : cap - 1; rd_bytes((uint8_t *)dst, keep); dst[keep] = 0; }
    rd_skip(len - keep);
    return len;
}

/* ===================================================================== *
 *  3. module loader + translator
 * ===================================================================== */

/* internal opcodes (wasm opcodes 0x00..0xC4 are used as-is) */
//internal opcodes that dont exist in wasm, we add them for the resolved branches and bulk memory
//the normal wasm opcodes 0x00 - 0xC4 are used as they are
enum {
    OP_SAT0 = 0x100,                       /* 0x100..0x107 trunc_sat */
    OP_MEMCOPY = 0x10A, OP_MEMFILL = 0x10B,
    OP_BR = 0x110, OP_BR_FIX, OP_BR_IF, OP_BR_IF_FIX, OP_BR_TABLE, OP_JZ
    //TAILCALL: internal versions of return_call / return_call_indirect. DISABLED, see the note above w_translate
    //OP_RETURN_CALL = 0x112, OP_RETURN_CALL_IND = 0x113,
};

static int w_find_host(const char *name, eccles_wasm_host_fn *fn, const char **sig);
#if defined(ECCLES_WASM_LENIENT_IMPORTS)
static eccles_wasm_status_t w_trap_stub(uint64_t *s) { (void)s; return ECCLES_WASM_FAILED; }
#endif

//returns the record of a type (np, nr, params.., results..)
static const uint8_t *w_rec(uint32_t type_index) { return G.types + G.type_off[type_index]; }

//only i32 i64 f32 f64 are valid here (no v128, no reference types), with NO_FLOAT the float types are rejected also
static int w_valtype_ok(int t) {
    if (t == 0x7F || t == 0x7E) return 1;
#if !defined(ECCLES_WASM_NO_FLOAT)
    if (t == 0x7D || t == 0x7C) return 1;
#endif
    return 0;
}
static int w_vt(void) {                      /* read + validate a value type */
    int t = rd_u8();
    if (!G.err && !w_valtype_ok(t)) { w_fail(ECCLES_WASM_ERR_UNSUPPORTED_FEATURE, (uint32_t)t); return 0; }
    return t;
}

/* "i(ii)" -> type record [np, nr, params..., results...]; returns length or 0 */
//parses the signature string of a host function (look at the wasi table for an example "i()") into a type record
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

/* ---- translator state ---- */
//control stack entry, one for every open block/loop/if. patch is the head of the list of branches waiting for the end label
typedef struct { uint32_t patch, loop_pc, else_slot; uint16_t base; uint8_t np, nr, kind, reach; } w_ctl_t;
enum { K_FUNC, K_BLOCK, K_LOOP, K_IF, K_ELSE };
static struct {
    uint16_t *code;
    uint32_t n, cap, h, maxh, dead_depth;
    uint16_t nloc;
    uint8_t  nctl, dead;
    w_ctl_t  ctl[ECCLES_WASM_MAX_CTL];
} T;

//malformed module, saves the file position so you can find the bad byte with a hex editor
//static void t_bad(void) { printf("bad module at %u\n", (unsigned)rd_pos()); w_fail(ECCLES_WASM_ERR_MALFORMED, rd_pos()); } //old debug version
static void t_bad(void) { w_fail(ECCLES_WASM_ERR_MALFORMED, rd_pos()); }
//emit one 16bit word to the translated code. NOTE: we use 16bit words not bytes because of alignment on cortex m0 / esp32
static void t_emit(uint32_t w) {
    if (T.n >= T.cap) { w_fail(ECCLES_WASM_ERR_NO_MEMORY, (uint32_t)(((uintptr_t)(T.code + T.n) - (uintptr_t)G.lo) + 2)); return; }
    T.code[T.n++] = (uint16_t)w;
}
//32bit values are written as two words, low one first
static void t_emit32(uint32_t v) { t_emit(v & 0xFFFFu); t_emit(v >> 16); }
static uint32_t t_get32(uint32_t at) { return (uint32_t)T.code[at] | ((uint32_t)T.code[at + 1] << 16); }
static void t_put32(uint32_t at, uint32_t v) { T.code[at] = (uint16_t)v; T.code[at + 1] = (uint16_t)(v >> 16); }
//t_pop and t_push only track the HEIGHT of the operand stack, not the types. thats why the validation is structural
static void t_pop(uint32_t k) {
    if (T.nctl == 0 || T.h < (uint32_t)T.ctl[T.nctl - 1].base + k) { t_bad(); return; }
    T.h -= k;
}
static void t_push(uint32_t k) {
    T.h += k;
    if (T.h > T.maxh) T.maxh = T.h;
    if (T.h > ECCLES_WASM_VALUE_STACK_SLOTS) w_fail(ECCLES_WASM_ERR_LIMIT, T.h);
}
//patch all the branches waiting for this label
static void t_resolve(uint32_t head, uint32_t target) {
    while (head != W_NIL) {
        uint32_t nxt = t_get32(head);
        t_put32(head, target - head);            /* two's complement relative offset */
        head = nxt;
    }
}
/* emit the 32-bit jump slot for a branch to frame f */
static void t_target(w_ctl_t *f) {
    uint32_t s = T.n;
    if (f->kind == K_LOOP) t_emit32(f->loop_pc - s);
    else { t_emit32(f->patch); f->patch = s; }
}
//emit a branch to the label at depth, the code is a bit messy because of the keep/drop values (FIX)
//TODO: clean up t_branch, too many special cases
static void t_branch(uint32_t depth, int cond) {
    w_ctl_t *f; uint32_t keep; int fix;
    if (depth >= T.nctl) { t_bad(); return; }
    f = &T.ctl[T.nctl - 1 - depth];
    keep = (f->kind == K_LOOP) ? f->np : f->nr;
    if (T.h < (uint32_t)f->base + keep) { t_bad(); return; }
    fix = (T.h != (uint32_t)f->base + keep);
    t_emit((cond ? OP_BR_IF : OP_BR) + (uint32_t)fix);
    t_target(f);
    if (fix) { t_emit(keep); t_emit(f->base); }
}
//block types can be empty, a single valtype, or a type index (multi value)
static int t_blocktype(uint8_t *np, uint8_t *nr) {
    int64_t v = rd_s64();
    if (G.err) return 0;
    if (v == -64) { *np = 0; *nr = 0; return 1; }
    if (v >= -4 && v <= -1) {
        if (!w_valtype_ok((int)(v + 0x80))) { w_fail(ECCLES_WASM_ERR_UNSUPPORTED_FEATURE, (uint32_t)(v + 0x80)); return 0; }
        *np = 0; *nr = 1; return 1;
    }
    if (v >= 0 && v < G.ntypes) { const uint8_t *r = w_rec((uint32_t)v); *np = r[0]; *nr = r[1]; return 1; }
    w_fail(v < 0 ? ECCLES_WASM_ERR_UNSUPPORTED_FEATURE : ECCLES_WASM_ERR_MALFORMED, rd_pos());
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

//the translator: reads ONE function body from the stream and converts it to our internal 16bit code
//this is the biggest function in the file, i know. every wasm opcode has its own case
//
//TAILCALL (return_call 0x12 / return_call_indirect 0x13)  ---  NOT WORKING, COMMENTED OUT EVERYWHERE
//i wanted to support the tail call proposal so recursive guests (state machines, parsers) dont eat the call stack,
//we only have ECCLES_WASM_CALL_DEPTH 32 frames!! this gave me weeks of headache. the translator side is easy but the
//interpreter side (reusing the frame) keeps giving me serious compile frustration and weird crashes, every time i fix
//one thing something else breaks. so i commented everything out and i will come back to it once the problem is resolved
//search for TAILCALL in this file and in eccles_wasm.h, all the pieces are there, just commented
static void w_translate(uint32_t fi, uint32_t body_end) {
    w_func_t *F = &G.funcs[fi];
    uint32_t nl, i, extra = 0;
    uintptr_t p;
    int done = 0;

    //local declarations come first, (count, type) pairs. we only need the total count because all locals start as zero
    nl = rd_u32();
    for (i = 0; i < nl && !G.err; i++) {
        uint32_t c = rd_u32();
        (void)w_vt();
        extra += c;
        if (c > 0xFFFFu || extra > 0xFFFFu - F->nparams) { w_fail(ECCLES_WASM_ERR_LIMIT, extra); return; }
    }
    if (G.err) return;

    //translated code goes right after the used part of the arena and we use ALL the free space as a buffer (T.cap) until the function is done
    p = ((uintptr_t)G.lo + 1) & ~(uintptr_t)1;
    T.code = (uint16_t *)p;
    T.cap = p < (uintptr_t)G.hi ? (uint32_t)(((uintptr_t)G.hi - p) / 2) : 0;
    T.n = 0; T.h = T.maxh = (uint32_t)F->nparams + extra; T.nloc = (uint16_t)T.h;
    T.nctl = 1; T.dead = 0; T.dead_depth = 0;
    if (T.h > ECCLES_WASM_VALUE_STACK_SLOTS) { w_fail(ECCLES_WASM_ERR_LIMIT, T.h); return; }
    T.ctl[0].patch = W_NIL; T.ctl[0].loop_pc = 0; T.ctl[0].else_slot = W_NIL;
    T.ctl[0].base = (uint16_t)T.h; T.ctl[0].np = 0; T.ctl[0].nr = F->nresults;
    T.ctl[0].kind = K_FUNC; T.ctl[0].reach = 0;

    while (!done) {
        uint32_t op;
        if (G.err) return;
        if (rd_pos() > body_end) { t_bad(); return; }
        op = (uint32_t)rd_u8();
        if (G.err) return;
        //printf("op %02x h=%u dead=%d\n", (unsigned)op, (unsigned)T.h, T.dead); //debug, very noisy
        //NOTE: T.dead means we are in unreachable code (after br, return, unreachable), we still parse it but dont emit anything
        switch (op) {
        case 0x00: if (!T.dead) { t_emit(0x00); T.dead = 1; T.dead_depth = 0; } break;
        case 0x01: break;
        case 0x02: case 0x03: case 0x04: {
            uint8_t np, nr; w_ctl_t *c;
            if (!t_blocktype(&np, &nr)) break;
            if (T.dead) { T.dead_depth++; break; }
            if (op == 0x04) t_pop(1);
            if (G.err) return;
            if (T.nctl >= ECCLES_WASM_MAX_CTL) { w_fail(ECCLES_WASM_ERR_LIMIT, T.nctl); return; }
            if (T.h < (uint32_t)T.ctl[T.nctl - 1].base + np) { t_bad(); return; }
            c = &T.ctl[T.nctl++];
            c->patch = W_NIL; c->else_slot = W_NIL; c->np = np; c->nr = nr; c->reach = 0;
            c->kind = (uint8_t)(op == 0x02 ? K_BLOCK : op == 0x03 ? K_LOOP : K_IF);
            c->base = (uint16_t)(T.h - np);
            c->loop_pc = T.n;
            if (op == 0x04) { t_emit(OP_JZ); c->else_slot = T.n; t_emit32(0); }
            break; }
        case 0x05: {                                              /* else */
            w_ctl_t *f;
            if (T.dead && T.dead_depth) break;
            f = &T.ctl[T.nctl - 1];
            if (f->kind != K_IF) { t_bad(); return; }
            if (!T.dead) {
                if (T.h != (uint32_t)f->base + f->nr) { t_bad(); return; }
                t_emit(OP_BR); t_target(f); f->reach = 1;
            }
            t_put32(f->else_slot, T.n - f->else_slot);
            f->kind = K_ELSE; T.h = (uint32_t)f->base + f->np; T.dead = 0;
            break; }
        case 0x0B: {                                              /* end */
            w_ctl_t *f;
            if (T.dead && T.dead_depth) { T.dead_depth--; break; }
            f = &T.ctl[T.nctl - 1];
            if (!T.dead) {
                if (T.h != (uint32_t)f->base + f->nr) { t_bad(); return; }
                f->reach = 1;
            }
            if (f->kind == K_IF) {
                if (f->np != f->nr) { t_bad(); return; }
                t_put32(f->else_slot, T.n - f->else_slot); f->reach = 1;
            }
            if (f->kind != K_LOOP) {
                if (f->patch != W_NIL) f->reach = 1;
                t_resolve(f->patch, T.n);
            }
            if (f->kind == K_FUNC) { t_emit(0x0F); T.nctl = 0; done = 1; break; }
            T.h = (uint32_t)f->base + f->nr; if (T.h > T.maxh) T.maxh = T.h;
            T.dead = !f->reach; T.dead_depth = 0; T.nctl--;
            break; }
        case 0x0C: { uint32_t d = rd_u32(); if (T.dead) break; t_branch(d, 0); T.dead = 1; T.dead_depth = 0; break; }
        case 0x0D: { uint32_t d = rd_u32(); if (T.dead) break; t_pop(1); if (!G.err) t_branch(d, 1); break; }
        case 0x0E: {                                              /* br_table */
            uint32_t n = rd_u32(), k;
            if (G.err) return;
            if (n >= 0xFFFFu) { w_fail(ECCLES_WASM_ERR_LIMIT, n); return; }
            if (!T.dead) { t_pop(1); t_emit(OP_BR_TABLE); t_emit(n); }
            for (k = 0; k <= n && !G.err; k++) {
                uint32_t d = rd_u32();
                if (T.dead || G.err) continue;
                if (d >= T.nctl) { t_bad(); return; }
                {
                    w_ctl_t *f = &T.ctl[T.nctl - 1 - d];
                    uint32_t keep = (f->kind == K_LOOP) ? f->np : f->nr;
                    if (T.h < (uint32_t)f->base + keep) { t_bad(); return; }
                    t_target(f); t_emit(keep); t_emit(f->base);
                }
            }
            if (!T.dead) { T.dead = 1; T.dead_depth = 0; }
            break; }
        case 0x0F:
            if (T.dead) break;
            if (T.h < (uint32_t)T.ctl[0].base + F->nresults) { t_bad(); return; }
            t_emit(0x0F); T.dead = 1; T.dead_depth = 0; break;
        case 0x10: {
            uint32_t idx = rd_u32();
            if (T.dead) break;
            if (idx >= G.nfuncs) { t_bad(); return; }
            t_pop(G.funcs[idx].nparams); t_push(G.funcs[idx].nresults);
            t_emit(0x10); t_emit(idx);
            break; }
        case 0x11: {
            uint32_t ti = rd_u32(), tab = rd_u32();
            if (G.err) return;
            if (tab != 0) { w_fail(ECCLES_WASM_ERR_UNSUPPORTED_FEATURE, op); return; }
            if (T.dead) break;
            if (ti >= G.ntypes || !G.has_table) { t_bad(); return; }
            { const uint8_t *r = w_rec(ti); t_pop(1u + r[0]); t_push(r[1]); }
            t_emit(0x11); t_emit(G.type_canon[ti]);
            break; }
            //TAILCALL: return_call (0x12), same as call but the current frame is reused. NOT FINISHED
            //#if ECCLES_WASM_TAIL_CALLS
            //case 0x12: {
            //    uint32_t idx = rd_u32();
            //    if (T.dead) break;
            //    if (idx >= G.nfuncs) { t_bad(); return; }
            //    //the results of the callee must match OUR results or the module is malformed (we only compare the count for now)
            //    //if (G.funcs[idx].nresults != F->nresults) { t_bad(); return; }
            //    t_pop(G.funcs[idx].nparams);
            //    t_emit(OP_RETURN_CALL); t_emit(idx);
            //    T.dead = 1; T.dead_depth = 0; //code after a tail call is unreachable, same as after return
            //    break; }
            ////TAILCALL: return_call_indirect (0x13)
            //case 0x13: {
            //    uint32_t ti = rd_u32(), tab = rd_u32();
            //    if (G.err) return;
            //    if (tab != 0) { w_fail(ECCLES_WASM_ERR_UNSUPPORTED_FEATURE, op); return; }
            //    if (T.dead) break;
            //    if (ti >= G.ntypes || !G.has_table) { t_bad(); return; }
            //    { const uint8_t *r = w_rec(ti); t_pop(1u + r[0]); }
            //    t_emit(OP_RETURN_CALL_IND); t_emit(G.type_canon[ti]);
            //    T.dead = 1; T.dead_depth = 0;
            //    break; }
            //#endif
        case 0x1A: if (!T.dead) { t_pop(1); t_emit(0x1A); } break;
        case 0x1C: { uint32_t n = rd_u32(), k; for (k = 0; k < n && !G.err; k++) (void)w_vt(); }
            /* fall through */
        case 0x1B: if (!T.dead) { t_pop(3); t_push(1); t_emit(0x1B); } break;
        //local.get / local.set / local.tee
        case 0x20: case 0x21: case 0x22: {
            uint32_t idx = rd_u32();
            if (T.dead) break;
            if (idx >= T.nloc) { t_bad(); return; }
            if (op == 0x20) t_push(1); else if (op == 0x21) t_pop(1);
            else if (T.h < (uint32_t)T.ctl[T.nctl - 1].base + 1) { t_bad(); return; }
            t_emit(op); t_emit(idx);
            break; }
        case 0x23: case 0x24: {
            uint32_t idx = rd_u32();
            if (T.dead) break;
            if (idx >= G.nglobals) { t_bad(); return; }
            if (op == 0x23) t_push(1); else t_pop(1);
            t_emit(op); t_emit(idx);
            break; }
        case 0x3F: case 0x40:
            if (rd_u8() != 0) { t_bad(); return; }
            if (T.dead) break;
            if (!G.has_mem) { t_bad(); return; }
            if (op == 0x3F) t_push(1); else { t_pop(1); t_push(1); }
            t_emit(op);
            break;
        case 0x41: { int32_t v = rd_s32(); if (T.dead) break; t_push(1); t_emit(0x41); t_emit32((uint32_t)v); break; }
        case 0x42: { uint64_t v = (uint64_t)rd_s64(); if (T.dead) break; t_push(1); t_emit(0x42); t_emit32((uint32_t)v); t_emit32((uint32_t)(v >> 32)); break; }
#if !defined(ECCLES_WASM_NO_FLOAT)
        case 0x43: { uint32_t v = (uint32_t)rd_le(4); if (T.dead) break; t_push(1); t_emit(0x43); t_emit32(v); break; }
        case 0x44: { uint64_t v = rd_le(8); if (T.dead) break; t_push(1); t_emit(0x44); t_emit32((uint32_t)v); t_emit32((uint32_t)(v >> 32)); break; }
#endif
        case 0xFC: {
            uint32_t sub = rd_u32();
            if (G.err) return;
            if (sub <= 7) {
#if defined(ECCLES_WASM_NO_FLOAT)
                w_fail(ECCLES_WASM_ERR_UNSUPPORTED_FEATURE, 0xFC00u | sub); return;
#else
                if (T.dead) break;
                t_pop(1); t_push(1); t_emit(OP_SAT0 + sub);
#endif
            } else if (sub == 10) {
                if (rd_u8() != 0 || rd_u8() != 0) { t_bad(); return; }
                if (T.dead) break;
                if (!G.has_mem) { t_bad(); return; }
                t_pop(3); t_emit(OP_MEMCOPY);
            } else if (sub == 11) {
                if (rd_u8() != 0) { t_bad(); return; }
                if (T.dead) break;
                if (!G.has_mem) { t_bad(); return; }
                t_pop(3); t_emit(OP_MEMFILL);
            } else { w_fail(ECCLES_WASM_ERR_UNSUPPORTED_FEATURE, 0xFC00u | sub); return; }
            break; }
        default:
            if (op >= 0x28 && op <= 0x3E) {
                uint32_t off;
#if defined(ECCLES_WASM_NO_FLOAT)
                if (t_is_float(op)) { w_fail(ECCLES_WASM_ERR_UNSUPPORTED_FEATURE, op); return; }
#endif
                (void)rd_u32(); off = rd_u32();
                if (T.dead) break;
                if (!G.has_mem) { t_bad(); return; }
                if (op <= 0x35) { t_pop(1); t_push(1); } else t_pop(2);
                t_emit(op); t_emit32(off);
            } else if (op >= 0x45 && op <= 0xC4) {
#if defined(ECCLES_WASM_NO_FLOAT)
                if (t_is_float(op)) { w_fail(ECCLES_WASM_ERR_UNSUPPORTED_FEATURE, op); return; }
#endif
                if (T.dead) break;
                if (t_unary(op)) { t_pop(1); t_push(1); } else { t_pop(2); t_push(1); }
                if (op < 0xBC || op > 0xBF) t_emit(op);          /* reinterpret = no-op on raw slots */
            } else { w_fail(ECCLES_WASM_ERR_UNSUPPORTED_FEATURE, op); return; }
            break;
        }
    }
    if (G.err) return;
    if (rd_pos() != body_end) { t_bad(); return; }
    if (T.maxh > ECCLES_WASM_VALUE_STACK_SLOTS) { w_fail(ECCLES_WASM_ERR_LIMIT, T.maxh); return; }
    F->code = T.code; F->nlocals = (uint16_t)extra; F->max_h = (uint16_t)T.maxh;
    G.lo = (uint8_t *)(T.code + T.n);
    G.code_bytes += T.n * 2u;
}

/* ---- constant expressions ---- */
//evaluates the constant expressions used by globals, data offsets and element segments
static uint64_t w_const_expr(uint32_t nglob_visible) {
    uint64_t v = 0; int op = rd_u8();
    switch (op) {
    case 0x41: v = (uint32_t)rd_s32(); break;
    case 0x42: v = (uint64_t)rd_s64(); break;
#if !defined(ECCLES_WASM_NO_FLOAT)
    case 0x43: v = rd_le(4); break;
    case 0x44: v = rd_le(8); break;
#endif
    case 0x23: { uint32_t g = rd_u32(); if (g >= nglob_visible) t_bad(); else v = G.globals[g]; break; }
    default: w_fail(G.err ? G.err : ECCLES_WASM_ERR_UNSUPPORTED_FEATURE, (uint32_t)op); return 0;
    }
    if (rd_u8() != 0x0B) t_bad();
    return v;
}

/* ---- sections ---- */
//section 1, the function signatures. we store them as small records (np, nr, params, results) and merge the identical ones
//so call_indirect can compare the type ids with a simple ==
static void sec_type(void) {
    uint32_t n = rd_u32(), i, used = 0;
    uint8_t *pool;
    if (G.err) return;
    if (n > 0xFFFFu) { w_fail(ECCLES_WASM_ERR_LIMIT, n); return; }
    G.type_off = (uint16_t *)w_alo(n * 2u + 2u, 2);
    G.type_canon = (uint16_t *)w_alo(n * 2u + 2u, 2);
    pool = G.types = (uint8_t *)w_alo(0, 1);
    for (i = 0; i < n && !G.err; i++) {
        uint32_t np, nr, k; uint8_t *rec;
        if (rd_u8() != 0x60) { t_bad(); return; }
        np = rd_u32(); if (np > 200) { w_fail(ECCLES_WASM_ERR_LIMIT, np); return; }
        if (G.err) return;
        rec = (uint8_t *)w_alo(2 + np, 1); if (!rec) return;
        rec[0] = (uint8_t)np;
        for (k = 0; k < np; k++) rec[2 + k] = (uint8_t)w_vt();
        nr = rd_u32(); if (nr > 16) { w_fail(ECCLES_WASM_ERR_LIMIT, nr); return; }
        if (G.err || !w_alo(nr, 1)) return;
        rec[1] = (uint8_t)nr;
        for (k = 0; k < nr; k++) rec[2 + np + k] = (uint8_t)w_vt();
        G.type_off[i] = (uint16_t)used;
        if (used + 2 + np + nr > 0xFFFFu) { w_fail(ECCLES_WASM_ERR_LIMIT, used); return; }
        used += 2 + np + nr;
        { uint32_t j; G.type_canon[i] = (uint16_t)i;
          for (j = 0; j < i; j++) { const uint8_t *a = pool + G.type_off[j];
              if (a[0] == rec[0] && a[1] == rec[1] && memcmp(a, rec, 2u + rec[0] + rec[1]) == 0) { G.type_canon[i] = (uint16_t)j; break; } } }
    }
    G.ntypes = (uint16_t)n;
}

//section 2, imports. only function imports are supported (no imported memory/table/globals)
//every import is looked up in the user registered host functions and in our builtin tables
static void sec_import(void) {
    uint32_t n = rd_u32(), i;
    char name[ECCLES_WASM_MAX_NAME + 1];
    if (G.err) return;
    if (n > 0xFFFFu) { w_fail(ECCLES_WASM_ERR_LIMIT, n); return; }
    G.funcs = (w_func_t *)w_alo(n * sizeof(w_func_t) + 8, 8);
    if (!G.funcs) return;
    for (i = 0; i < n && !G.err; i++) {
        uint32_t ml, fl, ti; int kind; eccles_wasm_host_fn fn = 0; const char *sig = 0; w_func_t *f; const uint8_t *rec;
        ml = rd_name(NULL, 0); (void)ml;
        fl = rd_name(name, sizeof name);
        kind = rd_u8();
        if (G.err) return;
        if (kind != 0) { w_fail(ECCLES_WASM_ERR_UNSUPPORTED_FEATURE, 0x1000u | (uint32_t)kind); return; }
        ti = rd_u32();
        if (G.err) return;
        if (ti >= G.ntypes) { t_bad(); return; }
        if (fl >= sizeof name || !w_find_host(name, &fn, &sig)) {
#if defined(ECCLES_WASM_LENIENT_IMPORTS)
            fn = w_trap_stub; sig = 0;
#else
            memcpy(G.err_str, name, sizeof name);
            w_fail(ECCLES_WASM_ERR_IMPORT, i); return;
#endif
        }
        rec = w_rec(ti);
        if (sig) {
            uint8_t want[2 + 200 + 16]; uint32_t wl = w_sig(sig, want, sizeof want);
            if (wl != 2u + rec[0] + rec[1] || memcmp(want, rec, wl) != 0) {
                memcpy(G.err_str, name, sizeof name);
                w_fail(ECCLES_WASM_ERR_SIGNATURE, i); return;
            }
        }
        f = &G.funcs[i];
        f->code = NULL; f->host = fn; f->type = G.type_canon[ti]; f->nparams = rec[0]; f->nresults = rec[1];
        f->nlocals = 0; f->max_h = 0;
    }
    G.nimports = G.nfuncs = (uint16_t)n;
}

//section 3, just the type index of every function defined in the module
static void sec_function(void) {
    uint32_t n = rd_u32(), i, total;
    w_func_t *nf;
    if (G.err) return;
    total = (uint32_t)G.nimports + n;
    if (total > 0xFFFEu) { w_fail(ECCLES_WASM_ERR_LIMIT, total); return; }
    nf = (w_func_t *)w_alo(total * sizeof(w_func_t) + 8, 8);
    if (!nf) return;
    if (G.nimports) memcpy(nf, G.funcs, G.nimports * sizeof(w_func_t));
    G.funcs = nf;
    for (i = 0; i < n && !G.err; i++) {
        uint32_t ti = rd_u32(); const uint8_t *rec; w_func_t *f;
        if (G.err) return;
        if (ti >= G.ntypes) { t_bad(); return; }
        rec = w_rec(ti); f = &G.funcs[G.nimports + i];
        f->code = NULL; f->host = 0; f->type = G.type_canon[ti]; f->nparams = rec[0]; f->nresults = rec[1];
        f->nlocals = 0; f->max_h = 0;
    }
    G.nfuncs = (uint16_t)total;
}

//section 4, we only support ONE table
static void sec_table(void) {
    uint32_t n = rd_u32(), flags, mn, k;
    if (G.err) return;
    if (n > 1) { w_fail(ECCLES_WASM_ERR_UNSUPPORTED_FEATURE, 0x2000u | n); return; }
    if (n == 0) return;
    if (rd_u8() != 0x70) { w_fail(ECCLES_WASM_ERR_UNSUPPORTED_FEATURE, 0x2070u); return; }
    flags = rd_u32(); if (flags > 1) { w_fail(ECCLES_WASM_ERR_UNSUPPORTED_FEATURE, 0x2100u | flags); return; }
    mn = rd_u32(); if (flags) (void)rd_u32();
    if (G.err) return;
    if (mn > 0xFFFEu) { w_fail(ECCLES_WASM_ERR_LIMIT, mn); return; }
    G.table = (uint16_t *)w_alo(mn * 2u + 2u, 2);
    if (!G.table) return;
    for (k = 0; k < mn; k++) G.table[k] = 0xFFFFu;
    G.tsize = (uint16_t)mn; G.has_table = 1;
}

//section 5, linear memory. NOTE: with ECCLES_WASM_MEM_LIMIT_BYTES the guest still sees the real page count but we only back part of it
static void sec_memory(void) {
    uint32_t n = rd_u32(), flags, mn; uint64_t want;
    if (G.err) return;
    if (n > 1) { w_fail(ECCLES_WASM_ERR_UNSUPPORTED_FEATURE, 0x3000u | n); return; }
    if (n == 0) return;
    flags = rd_u32(); if (flags > 1) { w_fail(ECCLES_WASM_ERR_UNSUPPORTED_FEATURE, 0x3100u | flags); return; }
    mn = rd_u32(); if (flags) (void)rd_u32();
    if (G.err) return;
    if (mn > 65536u) { t_bad(); return; }
    want = (uint64_t)mn * W_PAGE;
#if defined(ECCLES_WASM_MEM_LIMIT_BYTES)
    if (want > (uint64_t)(ECCLES_WASM_MEM_LIMIT_BYTES)) want = (uint64_t)(ECCLES_WASM_MEM_LIMIT_BYTES);
#endif
    if (want > 0xFFFFFFF0u) { w_fail(ECCLES_WASM_ERR_NO_MEMORY, 0xFFFFFFFFu); return; }
    G.mem = (uint8_t *)w_ahi((size_t)want, 16);
    if (!G.mem) return;
    memset(G.mem, 0, (size_t)want);
    G.mem_bytes = (uint32_t)want; G.mem_pages = mn; G.has_mem = 1;
}

//section 6, globals
static void sec_global(void) {
    uint32_t n = rd_u32(), i;
    if (G.err) return;
    if (n > 0xFFFEu) { w_fail(ECCLES_WASM_ERR_LIMIT, n); return; }
    G.globals = (uint64_t *)w_alo(n * 8u + 8u, 8);
    if (!G.globals) return;
    for (i = 0; i < n && !G.err; i++) {
        (void)w_vt(); (void)rd_u8();
        G.globals[i] = w_const_expr(i);
    }
    G.nglobals = (uint16_t)n;
}

//section 7, exports. we keep the names so eccles_wasm_call can find them
static void sec_export(void) {
    uint32_t n = rd_u32(), i;
    if (G.err) return;
    if (n > 0xFFFEu) { w_fail(ECCLES_WASM_ERR_LIMIT, n); return; }
    G.exports = (w_export_t *)w_alo(n * sizeof(w_export_t) + 8, sizeof(void *));
    if (!G.exports) return;
    for (i = 0; i < n && !G.err; i++) {
        uint32_t len = rd_u32(), idx; int kind; uint8_t *save = G.lo; char *nm;
        if (G.err) return;
        nm = (char *)w_alo((size_t)len + 1, 1);
        if (!nm) return;
        rd_bytes((uint8_t *)nm, len); nm[len] = 0;
        kind = rd_u8(); idx = rd_u32();
        if (G.err) return;
        if (kind == 0) {
            if (idx >= G.nfuncs) { t_bad(); return; }
            G.exports[G.nexports].name = nm; G.exports[G.nexports].func = (uint16_t)idx; G.nexports++;
        } else G.lo = save;
    }
}

//section 8, the start function (runs at the end of loading if ECCLES_WASM_AUTORUN_START is on)
static void sec_start(void) {
    uint32_t idx = rd_u32();
    if (G.err) return;
    if (idx >= G.nfuncs || G.funcs[idx].nparams || G.funcs[idx].nresults) { t_bad(); return; }
    G.start = (uint16_t)idx;
}

//section 9, element segments, they fill the function table
static void sec_element(void) {
    uint32_t n = rd_u32(), i;
    for (i = 0; i < n && !G.err; i++) {
        uint32_t flags = rd_u32(), off, cnt, k;
        if (G.err) return;
        if (flags != 0) { w_fail(ECCLES_WASM_ERR_UNSUPPORTED_FEATURE, 0x9000u | flags); return; }
        if (!G.has_table) { t_bad(); return; }
        off = (uint32_t)w_const_expr(G.nglobals);
        cnt = rd_u32();
        if (G.err) return;
        if ((uint64_t)off + cnt > G.tsize) { t_bad(); return; }
        for (k = 0; k < cnt && !G.err; k++) {
            uint32_t fx = rd_u32();
            if (fx >= G.nfuncs) { t_bad(); return; }
            G.table[off + k] = (uint16_t)fx;
        }
    }
}

//section 10, the function bodies. every body goes to w_translate
static void sec_code(void) {
    uint32_t n = rd_u32(), i;
    if (G.err) return;
    if (n != (uint32_t)(G.nfuncs - G.nimports)) { t_bad(); return; }
    G.code_seen = 1;
    for (i = 0; i < n && !G.err; i++) {
        uint32_t size = rd_u32(), end;
        if (G.err) return;
        end = rd_pos() + size;
        w_translate((uint32_t)G.nimports + i, end);
    }
}

//section 11, data segments are copied straight into linear memory
//passive segments (memory.init) are not supported, see the flags check below
static void sec_data(void) {
    uint32_t n = rd_u32(), i;
    for (i = 0; i < n && !G.err; i++) {
        uint32_t flags = rd_u32(), off, len; uint64_t e;
        if (G.err) return;
        if (flags == 2) { if (rd_u32() != 0) { t_bad(); return; } }
        else if (flags != 0) { w_fail(ECCLES_WASM_ERR_UNSUPPORTED_FEATURE, 0xB000u | flags); return; }
        off = (uint32_t)w_const_expr(G.nglobals);
        len = rd_u32();
        if (G.err) return;
        e = (uint64_t)off + len;
        if (!G.has_mem && len) { t_bad(); return; }
        if (e > G.mem_bytes) {
            if (e <= (uint64_t)G.mem_pages * W_PAGE) w_fail(ECCLES_WASM_ERR_NO_MEMORY, (uint32_t)e);
            else t_bad();
            return;
        }
        rd_bytes(G.mem + off, len);
    }
}

//the main loader loop. checks the magic number then walks all the sections
//sections must come in the correct order (rank) except custom sections, those we skip
//NOTE: the datacount section (12) is read and ignored
static void w_load_module(void) {
    uint8_t hdr[8]; int last = 0, rank;
    static const uint8_t magic[8] = {0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00};
    G.start = 0xFFFFu;
    rd_bytes(hdr, 8);
    if (G.err) return;
    if (memcmp(hdr, magic, 8) != 0) { t_bad(); return; }
    for (;;) {
        uint32_t id, size, end;
        if (R.p >= R.e && !rd_fill()) break;                 /* clean EOF between sections */
        id = (uint32_t)rd_u8(); size = rd_u32();
        if (G.err) return;
        end = rd_pos() + size;
        if (id == 0) { rd_skip(size); if (G.err) return; continue; }
        switch (id) {
        case 1: rank = 1; break;  case 2: rank = 2; break;  case 3: rank = 3; break;
        case 4: rank = 4; break;  case 5: rank = 5; break;  case 6: rank = 6; break;
        case 7: rank = 7; break;  case 8: rank = 8; break;  case 9: rank = 9; break;
        case 12: rank = 10; break; case 10: rank = 11; break; case 11: rank = 12; break;
        default: t_bad(); return;
        }
        if (rank <= last) { t_bad(); return; }
        last = rank;
        switch (id) {
        case 1: sec_type(); break;     case 2: sec_import(); break;  case 3: sec_function(); break;
        case 4: sec_table(); break;    case 5: sec_memory(); break;  case 6: sec_global(); break;
        case 7: sec_export(); break;   case 8: sec_start(); break;   case 9: sec_element(); break;
        case 12: (void)rd_u32(); break;
        case 10: sec_code(); break;    case 11: sec_data(); break;
        }
        if (G.err) return;
        if (rd_pos() != end) { t_bad(); return; }
    }
    if (G.err) return;
    if (G.nfuncs > G.nimports && !G.code_seen) { t_bad(); return; }
}

/* ===================================================================== *
 *  4. interpreter
 * ===================================================================== */

#if (ECCLES_WASM_YIELD_INTERVAL) != 0 && !defined(ECCLES_WASM_YIELD)
  #error "eccles_wasm: ECCLES_WASM_YIELD_INTERVAL is set but ECCLES_WASM_YIELD() is not defined."
#endif

//wasm memory is little endian. on big endian chips we swap the bytes on every load and store
#if ECCLES_WASM_BIG_ENDIAN
  static uint16_t le16(uint16_t v) { return (uint16_t)((v << 8) | (v >> 8)); }
  static uint32_t le32(uint32_t v) { return ((uint32_t)le16((uint16_t)v) << 16) | le16((uint16_t)(v >> 16)); }
  static uint64_t le64(uint64_t v) { return ((uint64_t)le32((uint32_t)v) << 32) | le32((uint32_t)(v >> 32)); }
#else
  #define le16(v) (v)
  #define le32(v) (v)
  #define le64(v) (v)
#endif

//clz / ctz / popcnt, uses the compiler builtin when there is one, otherwise a plain c version
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
//float <-> bits with memcpy, casting the pointer is undefined behaviour and gcc -O2 really breaks it (learned this the hard way)
static uint32_t fb32(float f)    { uint32_t u; memcpy(&u, &f, 4); return u; }
static float    bf32(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static uint64_t fb64(double f)   { uint64_t u; memcpy(&u, &f, 8); return u; }
static double   bf64(uint64_t u) { double f; memcpy(&f, &u, 8); return f; }

static float f32_zero_like(float x) { return (fb32(x) & 0x80000000u) ? bf32(0x80000000u) : 0.0f; }
static double f64_zero_like(double x) { return (fb64(x) >> 63) ? bf64((uint64_t)1 << 63) : 0.0; }
//we write our own trunc/floor/ceil/nearest because i dont want to link the whole libm on small chips (only sqrt comes from math.h)
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
//wasm min/max have special rules for nan and -0, the c fmin/fmax are not the same thing
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

//fuel = instruction budget. every call and every backward branch costs 1, when it reaches zero we trap with TRAP_FUEL
//this is how we stop infinite loops (a while(1) guest) from freezing the mcu. fuel 0 means unlimited
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

//TRAP jumps to the trap label at the bottom with the error code
#define TRAP(e)   do { err = (e); goto trap; } while (0)
//read immediates from the translated code (pc is a pointer to 16bit words)
#define IMM16()   (*pc++)
#define IMM32()   (pc += 2, (uint32_t)pc[-2] | ((uint32_t)pc[-1] << 16))
#define OFF32(p)  ((int32_t)((uint32_t)(p)[0] | ((uint32_t)(p)[1] << 16)))
//helper macros for binary/unary ops, i got tired of writing the same pop pop push 200 times
//they contain a break so they only work directly inside the switch
//#define BIN32(E) sp[-2] = (uint32_t)(E); sp--; //old version, no a and b temporaries so sp was used twice in the expression
#define BIN32(E)  { uint32_t b = (uint32_t)sp[-1], a = (uint32_t)sp[-2]; sp--; sp[-1] = (uint32_t)(E); break; }
#define BIN64(E)  { uint64_t b = sp[-1], a = sp[-2]; sp--; sp[-1] = (uint64_t)(E); break; }
#define UN32(E)   { uint32_t a = (uint32_t)sp[-1]; sp[-1] = (uint32_t)(E); break; }
#define UN64(E)   { uint64_t a = sp[-1]; sp[-1] = (uint64_t)(E); break; }
#define FBIN32(E) { float b = bf32((uint32_t)sp[-1]), a = bf32((uint32_t)sp[-2]); sp--; sp[-1] = fb32(E); break; }
#define FCMP32(E) { float b = bf32((uint32_t)sp[-1]), a = bf32((uint32_t)sp[-2]); sp--; sp[-1] = (E) ? 1u : 0u; break; }
#define FUN32(E)  { float a = bf32((uint32_t)sp[-1]); sp[-1] = fb32(E); break; }
#define FBIN64(E) { double b = bf64(sp[-1]), a = bf64(sp[-2]); sp--; sp[-1] = fb64(E); break; }
#define FCMP64(E) { double b = bf64(sp[-1]), a = bf64(sp[-2]); sp--; sp[-1] = (E) ? 1u : 0u; break; }
#define FUN64(E)  { double a = bf64(sp[-1]); sp[-1] = fb64(E); break; }
//memory access macros, ea is the effective address (base + offset), everything is checked against memsz
//so a bad guest cant touch our own memory
#define MEMOFF(n) uint32_t off_ = IMM32(); uint64_t ea = (uint64_t)(uint32_t)sp[-1] + off_; \
                  if (W_UNLIKELY(ea + (n) > memsz)) TRAP(ECCLES_WASM_TRAP_MEM_OOB)
#define STOREP(n) uint32_t off_ = IMM32(); uint64_t val = sp[-1]; uint64_t ea = (uint64_t)(uint32_t)sp[-2] + off_; \
                  sp -= 2; if (W_UNLIKELY(ea + (n) > memsz)) TRAP(ECCLES_WASM_TRAP_MEM_OOB)
//FIX moves the kept values down to the block base when a branch leaves a block that has extra values on the stack
#define FIX(keep, nb) { uint64_t *d_ = fp + (nb), *s_ = sp - (keep); \
                  if (d_ != s_) { uint32_t k_; for (k_ = 0; k_ < (keep); k_++) d_[k_] = s_[k_]; } sp = d_ + (keep); }

//the interpreter. one big switch inside a for loop, simple but not the fastest
//TAILCALL / dispatch: i also wanted to try clang's musttail, one small function per opcode and every handler tail calls the next
//     __attribute__((musttail)) return handlers[op](pc, sp, fp, cur);
//it would be faster but musttail is clang only and gcc refuses it, and -Wpedantic complains too. no for now
//#define W_MUSTTAIL __attribute__((musttail))
//#define W_DISPATCH(next) W_MUSTTAIL return next(pc, sp, fp, cur)
static eccles_wasm_error_t w_exec(const w_func_t *f0) {
    const w_func_t *const funcs = G.funcs;
    const w_func_t *cur, *f;
    const uint16_t *pc;
    uint64_t *fp, *sp;
    w_frame_t *fr = G.frames;
    w_frame_t *const frend = G.frames + ECCLES_WASM_CALL_DEPTH;
    uint64_t *const stk_end = G.stk + ECCLES_WASM_VALUE_STACK_SLOTS;
    uint8_t *const mem = G.mem;
    const uint32_t memsz = G.mem_bytes;
    eccles_wasm_error_t err = ECCLES_WASM_OK;
#if ECCLES_WASM_FUEL
    uint32_t fuel = G.fuel_budget;
  #if ECCLES_WASM_YIELD_INTERVAL
    uint32_t ticks = 0;
  #endif
#endif

    fp = G.stk; //value stack starts at the bottom of G.stk
    if (fp + f0->max_h > stk_end) return ECCLES_WASM_TRAP_STACK_OVERFLOW; //the first function doesnt even fit
    sp = fp + f0->nparams;
    { uint64_t *ze = fp + f0->nparams + f0->nlocals; while (sp < ze) *sp++ = 0; }
    cur = f0; pc = f0->code;

    //main loop
    for (;;) {
        uint32_t op = *pc++;
        //printf("pc=%u op=%03x sp=%d\n", (unsigned)(pc - cur->code), (unsigned)op, (int)(sp - fp)); //super slow, leave commented
        switch (op) {
        case 0x00: TRAP(ECCLES_WASM_TRAP_UNREACHABLE);
        /* ---- control ---- */
        //branches, backward branches (o < 0) cost fuel so loops cant run forever
        case OP_BR:     { int32_t o = OFF32(pc); if (o < 0) TICK(); pc += o; break; }
        case OP_BR_FIX: { const uint16_t *s = pc; int32_t o = OFF32(s); uint32_t keep = s[2], nb = s[3];
                          FIX(keep, nb) if (o < 0) TICK(); pc = s + o; break; }
        case OP_BR_IF:  { const uint16_t *s = pc;
                          if ((uint32_t)*--sp) { int32_t o = OFF32(s); if (o < 0) TICK(); pc = s + o; } else pc += 2;
                          break; }
        case OP_BR_IF_FIX: { const uint16_t *s = pc;
                          if ((uint32_t)*--sp) { int32_t o = OFF32(s); uint32_t keep = s[2], nb = s[3];
                              FIX(keep, nb) if (o < 0) TICK(); pc = s + o; } else pc += 4;
                          break; }
        case OP_JZ:     { const uint16_t *s = pc; if (!(uint32_t)*--sp) pc = s + OFF32(s); else pc += 2; break; }
        case OP_BR_TABLE: {
            uint32_t n = pc[0], idx = (uint32_t)*--sp; const uint16_t *e; int32_t o; uint32_t keep, nb;
            if (idx > n) idx = n;
            e = pc + 1 + idx * 4u; o = OFF32(e); keep = e[2]; nb = e[3];
            FIX(keep, nb) if (o < 0) TICK(); pc = e + o; break; }
        case 0x0F: {                                               /* return / function end */
        //fp[0] = sp[-1]; sp = fp + 1; //old return, only one result. multi value broke it
            uint32_t nr = cur->nresults, k; uint64_t *src = sp - nr;
            for (k = 0; k < nr; k++) fp[k] = src[k];
            sp = fp + nr;
            if (fr == G.frames) goto done;
            fr--; pc = fr->pc; fp = fr->fp; cur = fr->f;
            break; }
        case 0x10: f = &funcs[IMM16()]; goto do_call;
        case 0x11: {
            uint32_t ti = IMM16(), ix = (uint32_t)*--sp; uint16_t fi;
            if (ix >= G.tsize) TRAP(ECCLES_WASM_TRAP_UNDEFINED_ELEMENT);
            fi = G.table[ix];
            if (fi == 0xFFFFu) TRAP(ECCLES_WASM_TRAP_UNDEFINED_ELEMENT);
            f = &funcs[fi];
            if (f->type != ti) TRAP(ECCLES_WASM_TRAP_INDIRECT_TYPE);
            goto do_call; }
            //TAILCALL: return_call. NOT WORKING YET, see the note above w_translate
            //the idea: instead of pushing a new frame we move the arguments down to fp and jump to the callee so the call stack never grows
            //#if ECCLES_WASM_TAIL_CALLS
            //case OP_RETURN_CALL: {
            //    f = &funcs[IMM16()];
            //    if (!f->code) { //host function, call it and then do a normal return
            //        uint64_t *args = sp - f->nparams;
            //        if (f->host(args) != ECCLES_WASM_SUCCESSFUL) TRAP(ECCLES_WASM_TRAP_HOST);
            //        sp = args + f->nresults;
            //        //pc = ??? how do we return from here, need to jump into the 0x0F code. goto do_return maybe
            //        continue;
            //    }
            //    {
            //        uint64_t *args = sp - f->nparams, *ze;
            //        uint32_t k;
            //        if (fp + f->max_h > stk_end) TRAP(ECCLES_WASM_TRAP_STACK_OVERFLOW);
            //        for (k = 0; k < f->nparams; k++) fp[k] = args[k]; //args are always above fp so copying forward is safe (i think)
            //        sp = fp + f->nparams;
            //        ze = fp + f->nparams + f->nlocals; while (sp < ze) *sp++ = 0; //zero the locals again
            //        cur = f; pc = f->code;
            //        TICK(); //a tail call still costs fuel, otherwise a tail recursive infinite loop is never stopped
            //        continue;
            //    }
            //}
            //case OP_RETURN_CALL_IND: ... same but f comes from the table like 0x11. TODO
            //#endif
        //drop
        //case 0x1A: sp -= 1; break; //same thing
        case 0x1A: sp--; break;
        //select, c is the condition
        case 0x1B: { uint32_t c = (uint32_t)sp[-1]; sp -= 2; if (!c) sp[-1] = sp[0]; break; }
        /* ---- variables ---- */
        case 0x20: *sp++ = fp[IMM16()]; break;
        case 0x21: fp[IMM16()] = *--sp; break;
        case 0x22: fp[IMM16()] = sp[-1]; break;
        case 0x23: *sp++ = G.globals[IMM16()]; break;
        case 0x24: G.globals[IMM16()] = *--sp; break;
        /* ---- memory ---- */
        //all loads and stores go through memcpy so we dont have alignment problems, le32 etc fix the byte order on big endian chips
        case 0x28: case 0x2A: { MEMOFF(4); uint32_t v; memcpy(&v, mem + ea, 4); sp[-1] = le32(v); break; }
        case 0x29: case 0x2B: { MEMOFF(8); uint64_t v; memcpy(&v, mem + ea, 8); sp[-1] = le64(v); break; }
        case 0x2C: { MEMOFF(1); sp[-1] = (uint32_t)(int32_t)(int8_t)mem[ea]; break; }
        case 0x2D: { MEMOFF(1); sp[-1] = mem[ea]; break; }
        case 0x2E: { MEMOFF(2); uint16_t v; memcpy(&v, mem + ea, 2); sp[-1] = (uint32_t)(int32_t)(int16_t)le16(v); break; }
        case 0x2F: { MEMOFF(2); uint16_t v; memcpy(&v, mem + ea, 2); sp[-1] = le16(v); break; }
        case 0x30: { MEMOFF(1); sp[-1] = (uint64_t)(int64_t)(int8_t)mem[ea]; break; }
        case 0x31: { MEMOFF(1); sp[-1] = mem[ea]; break; }
        case 0x32: { MEMOFF(2); uint16_t v; memcpy(&v, mem + ea, 2); sp[-1] = (uint64_t)(int64_t)(int16_t)le16(v); break; }
        case 0x33: { MEMOFF(2); uint16_t v; memcpy(&v, mem + ea, 2); sp[-1] = le16(v); break; }
        case 0x34: { MEMOFF(4); uint32_t v; memcpy(&v, mem + ea, 4); sp[-1] = (uint64_t)(int64_t)(int32_t)le32(v); break; }
        case 0x35: { MEMOFF(4); uint32_t v; memcpy(&v, mem + ea, 4); sp[-1] = le32(v); break; }
        case 0x36: case 0x38: { STOREP(4); uint32_t v = le32((uint32_t)val); memcpy(mem + ea, &v, 4); break; }
        case 0x37: case 0x39: { STOREP(8); uint64_t v = le64(val); memcpy(mem + ea, &v, 8); break; }
        case 0x3A: case 0x3C: { STOREP(1); mem[ea] = (uint8_t)val; break; }
        case 0x3B: case 0x3D: { STOREP(2); uint16_t v = le16((uint16_t)val); memcpy(mem + ea, &v, 2); break; }
        case 0x3E: { STOREP(4); uint32_t v = le32((uint32_t)val); memcpy(mem + ea, &v, 4); break; }
        //memory.size and memory.grow. grow(0) works, grow(n>0) always returns -1 because the memory has a fixed size (allowed by the spec)
        case 0x3F: *sp++ = G.mem_pages; break;
        case 0x40: sp[-1] = ((uint32_t)sp[-1] == 0) ? G.mem_pages : 0xFFFFFFFFu; break;
        //bulk memory ops, memmove because the regions can overlap
        case OP_MEMCOPY: {
            uint64_t d = (uint32_t)sp[-3], s = (uint32_t)sp[-2], n = (uint32_t)sp[-1]; sp -= 3;
            if (d + n > memsz || s + n > memsz) TRAP(ECCLES_WASM_TRAP_MEM_OOB);
            if (n) memmove(mem + d, mem + s, (size_t)n);
            break; }
        case OP_MEMFILL: {
            uint64_t d = (uint32_t)sp[-3], n = (uint32_t)sp[-1]; uint8_t v = (uint8_t)sp[-2]; sp -= 3;
            if (d + n > memsz) TRAP(ECCLES_WASM_TRAP_MEM_OOB);
            if (n) memset(mem + d, v, (size_t)n);
            break; }
        /* ---- constants ---- */
        case 0x41: case 0x43: *sp++ = IMM32(); break;
        case 0x42: case 0x44: { uint64_t lo = IMM32(); uint64_t hi = IMM32(); *sp++ = lo | (hi << 32); break; }
        /* ---- i32 compare ---- */
        case 0x45: UN32(a == 0)
        case 0x46: BIN32(a == b)
        case 0x47: BIN32(a != b)
        case 0x48: BIN32((int32_t)a < (int32_t)b)
        case 0x49: BIN32(a < b)
        case 0x4A: BIN32((int32_t)a > (int32_t)b)
        case 0x4B: BIN32(a > b)
        case 0x4C: BIN32((int32_t)a <= (int32_t)b)
        case 0x4D: BIN32(a <= b)
        case 0x4E: BIN32((int32_t)a >= (int32_t)b)
        case 0x4F: BIN32(a >= b)
        /* ---- i64 compare ---- */
        case 0x50: UN64(a == 0)
        case 0x51: BIN64(a == b)
        case 0x52: BIN64(a != b)
        case 0x53: BIN64((int64_t)a < (int64_t)b)
        case 0x54: BIN64(a < b)
        case 0x55: BIN64((int64_t)a > (int64_t)b)
        case 0x56: BIN64(a > b)
        case 0x57: BIN64((int64_t)a <= (int64_t)b)
        case 0x58: BIN64(a <= b)
        case 0x59: BIN64((int64_t)a >= (int64_t)b)
        case 0x5A: BIN64(a >= b)
#if !defined(ECCLES_WASM_NO_FLOAT)
        case 0x5B: FCMP32(a == b)
        case 0x5C: FCMP32(a != b)
        case 0x5D: FCMP32(a < b)
        case 0x5E: FCMP32(a > b)
        case 0x5F: FCMP32(a <= b)
        case 0x60: FCMP32(a >= b)
        case 0x61: FCMP64(a == b)
        case 0x62: FCMP64(a != b)
        case 0x63: FCMP64(a < b)
        case 0x64: FCMP64(a > b)
        case 0x65: FCMP64(a <= b)
        case 0x66: FCMP64(a >= b)
#endif
        /* ---- i32 arithmetic ---- */
        case 0x67: UN32(w_clz32(a))
        case 0x68: UN32(w_ctz32(a))
        case 0x69: UN32(w_pop32(a))
        case 0x6A: BIN32(a + b)
        case 0x6B: BIN32(a - b)
        case 0x6C: BIN32(a * b)
        case 0x6D: { int32_t b = (int32_t)sp[-1], a = (int32_t)sp[-2];
            if (!b) TRAP(ECCLES_WASM_TRAP_DIV_ZERO);
            if (a == INT32_MIN && b == -1) TRAP(ECCLES_WASM_TRAP_INT_OVERFLOW);
            sp--; sp[-1] = (uint32_t)(a / b); break; }
        case 0x6E: { uint32_t b = (uint32_t)sp[-1], a = (uint32_t)sp[-2];
            if (!b) TRAP(ECCLES_WASM_TRAP_DIV_ZERO);
            sp--; sp[-1] = a / b; break; }
        case 0x6F: { int32_t b = (int32_t)sp[-1], a = (int32_t)sp[-2];
            if (!b) TRAP(ECCLES_WASM_TRAP_DIV_ZERO);
            sp--; sp[-1] = (b == -1) ? 0u : (uint32_t)(a % b); break; }
        case 0x70: { uint32_t b = (uint32_t)sp[-1], a = (uint32_t)sp[-2];
            if (!b) TRAP(ECCLES_WASM_TRAP_DIV_ZERO);
            sp--; sp[-1] = a % b; break; }
        case 0x71: BIN32(a & b)
        case 0x72: BIN32(a | b)
        case 0x73: BIN32(a ^ b)
        case 0x74: BIN32(a << (b & 31))
        case 0x75: BIN32((uint32_t)((int32_t)a >> (b & 31)))
        case 0x76: BIN32(a >> (b & 31))
        case 0x77: BIN32((a << (b & 31)) | (a >> ((32 - (b & 31)) & 31)))
        case 0x78: BIN32((a >> (b & 31)) | (a << ((32 - (b & 31)) & 31)))
        /* ---- i64 arithmetic ---- */
        case 0x79: UN64(w_clz64(a))
        case 0x7A: UN64(w_ctz64(a))
        case 0x7B: UN64(w_pop64(a))
        case 0x7C: BIN64(a + b)
        case 0x7D: BIN64(a - b)
        case 0x7E: BIN64(a * b)
        case 0x7F: { int64_t b = (int64_t)sp[-1], a = (int64_t)sp[-2];
            if (!b) TRAP(ECCLES_WASM_TRAP_DIV_ZERO);
            if (a == INT64_MIN && b == -1) TRAP(ECCLES_WASM_TRAP_INT_OVERFLOW);
            sp--; sp[-1] = (uint64_t)(a / b); break; }
        case 0x80: { uint64_t b = sp[-1], a = sp[-2];
            if (!b) TRAP(ECCLES_WASM_TRAP_DIV_ZERO);
            sp--; sp[-1] = a / b; break; }
        case 0x81: { int64_t b = (int64_t)sp[-1], a = (int64_t)sp[-2];
            if (!b) TRAP(ECCLES_WASM_TRAP_DIV_ZERO);
            sp--; sp[-1] = (b == -1) ? 0u : (uint64_t)(a % b); break; }
        case 0x82: { uint64_t b = sp[-1], a = sp[-2];
            if (!b) TRAP(ECCLES_WASM_TRAP_DIV_ZERO);
            sp--; sp[-1] = a % b; break; }
        case 0x83: BIN64(a & b)
        case 0x84: BIN64(a | b)
        case 0x85: BIN64(a ^ b)
        case 0x86: BIN64(a << (b & 63))
        case 0x87: BIN64((uint64_t)((int64_t)a >> (b & 63)))
        case 0x88: BIN64(a >> (b & 63))
        case 0x89: BIN64((a << (b & 63)) | (a >> ((64 - (b & 63)) & 63)))
        case 0x8A: BIN64((a >> (b & 63)) | (a << ((64 - (b & 63)) & 63)))
#if !defined(ECCLES_WASM_NO_FLOAT)
        /* ---- f32 ---- */
        case 0x8B: sp[-1] = (uint32_t)sp[-1] & 0x7FFFFFFFu; break;
        case 0x8C: sp[-1] = (uint32_t)sp[-1] ^ 0x80000000u; break;
        case 0x8D: FUN32(f32_ceil(a))
        case 0x8E: FUN32(f32_floor(a))
        case 0x8F: FUN32(f32_trunc(a))
        case 0x90: FUN32(f32_nearest(a))
        case 0x91: FUN32(sqrtf(a))
        case 0x92: FBIN32(a + b)
        case 0x93: FBIN32(a - b)
        case 0x94: FBIN32(a * b)
        case 0x95: FBIN32(a / b)
        case 0x96: FBIN32(f32_min(a, b))
        case 0x97: FBIN32(f32_max(a, b))
        case 0x98: { uint32_t b = (uint32_t)sp[-1], a = (uint32_t)sp[-2]; sp--; sp[-1] = (a & 0x7FFFFFFFu) | (b & 0x80000000u); break; }
        /* ---- f64 ---- */
        case 0x99: sp[-1] &= 0x7FFFFFFFFFFFFFFFull; break;
        case 0x9A: sp[-1] ^= 0x8000000000000000ull; break;
        case 0x9B: FUN64(f64_ceil(a))
        case 0x9C: FUN64(f64_floor(a))
        case 0x9D: FUN64(f64_trunc(a))
        case 0x9E: FUN64(f64_nearest(a))
        case 0x9F: FUN64(sqrt(a))
        case 0xA0: FBIN64(a + b)
        case 0xA1: FBIN64(a - b)
        case 0xA2: FBIN64(a * b)
        case 0xA3: FBIN64(a / b)
        case 0xA4: FBIN64(f64_min(a, b))
        case 0xA5: FBIN64(f64_max(a, b))
        case 0xA6: { uint64_t b = sp[-1], a = sp[-2]; sp--; sp[-1] = (a & 0x7FFFFFFFFFFFFFFFull) | (b & 0x8000000000000000ull); break; }
#endif
        /* ---- conversions ---- */
        case 0xA7: UN64((uint32_t)a)
        case 0xAC: sp[-1] = (uint64_t)(int64_t)(int32_t)(uint32_t)sp[-1]; break;
        case 0xAD: sp[-1] = (uint32_t)sp[-1]; break;
        case 0xC0: sp[-1] = (uint32_t)(int32_t)(int8_t)(uint8_t)sp[-1]; break;
        case 0xC1: sp[-1] = (uint32_t)(int32_t)(int16_t)(uint16_t)sp[-1]; break;
        case 0xC2: sp[-1] = (uint64_t)(int64_t)(int8_t)(uint8_t)sp[-1]; break;
        case 0xC3: sp[-1] = (uint64_t)(int64_t)(int16_t)(uint16_t)sp[-1]; break;
        case 0xC4: sp[-1] = (uint64_t)(int64_t)(int32_t)(uint32_t)sp[-1]; break;
#if !defined(ECCLES_WASM_NO_FLOAT)
        case 0xA8: case 0xA9: case 0xAA: case 0xAB: {
            double d = (op < 0xAA) ? (double)bf32((uint32_t)sp[-1]) : bf64(sp[-1]);
            if (d != d) TRAP(ECCLES_WASM_TRAP_BAD_CONVERSION);
            if (op & 1) { if (d <= -1.0 || d >= 4294967296.0) TRAP(ECCLES_WASM_TRAP_INT_OVERFLOW); sp[-1] = (uint32_t)d; }
            else { if (d <= -2147483649.0 || d >= 2147483648.0) TRAP(ECCLES_WASM_TRAP_INT_OVERFLOW); sp[-1] = (uint32_t)(int32_t)d; }
            break; }
        case 0xAE: case 0xAF: case 0xB0: case 0xB1: {
            double d = (op < 0xB0) ? (double)bf32((uint32_t)sp[-1]) : bf64(sp[-1]);
            if (d != d) TRAP(ECCLES_WASM_TRAP_BAD_CONVERSION);
            if (op & 1) { if (d <= -1.0 || d >= 18446744073709551616.0) TRAP(ECCLES_WASM_TRAP_INT_OVERFLOW); sp[-1] = (uint64_t)d; }
            else { if (d < -9223372036854775808.0 || d >= 9223372036854775808.0) TRAP(ECCLES_WASM_TRAP_INT_OVERFLOW); sp[-1] = (uint64_t)(int64_t)d; }
            break; }
        case 0xB2: sp[-1] = fb32((float)(int32_t)(uint32_t)sp[-1]); break;
        case 0xB3: sp[-1] = fb32((float)(uint32_t)sp[-1]); break;
        case 0xB4: sp[-1] = fb32((float)(int64_t)sp[-1]); break;
        case 0xB5: sp[-1] = fb32((float)sp[-1]); break;
        case 0xB6: sp[-1] = fb32((float)bf64(sp[-1])); break;
        case 0xB7: sp[-1] = fb64((double)(int32_t)(uint32_t)sp[-1]); break;
        case 0xB8: sp[-1] = fb64((double)(uint32_t)sp[-1]); break;
        case 0xB9: sp[-1] = fb64((double)(int64_t)sp[-1]); break;
        case 0xBA: sp[-1] = fb64((double)sp[-1]); break;
        case 0xBB: sp[-1] = fb64((double)bf32((uint32_t)sp[-1])); break;
        case OP_SAT0 + 0: case OP_SAT0 + 1: case OP_SAT0 + 2: case OP_SAT0 + 3:
        case OP_SAT0 + 4: case OP_SAT0 + 5: case OP_SAT0 + 6: case OP_SAT0 + 7: {
            uint32_t sub = op - OP_SAT0; uint64_t r;
            double d = (sub & 2) ? bf64(sp[-1]) : (double)bf32((uint32_t)sp[-1]);
            if (d != d) r = 0;
            else if (sub & 1) {
                if (sub < 4) r = d <= 0 ? 0u : d >= 4294967296.0 ? 0xFFFFFFFFu : (uint32_t)d;
                else r = d <= 0 ? 0u : d >= 18446744073709551616.0 ? ~(uint64_t)0 : (uint64_t)d;
            } else {
                if (sub < 4) r = (uint32_t)(d <= -2147483648.0 ? INT32_MIN : d >= 2147483648.0 ? INT32_MAX : (int32_t)d);
                else r = (uint64_t)(d <= -9223372036854775808.0 ? INT64_MIN : d >= 9223372036854775808.0 ? INT64_MAX : (int64_t)d);
            }
            sp[-1] = r; break; }
#endif
        default: TRAP(ECCLES_WASM_TRAP_UNREACHABLE);
        }
        continue;

    //TAILCALL: this is where i tried to add a do_tail_call: label. gcc says "label at end of compound statement" in c99,
    //and clang -Wpedantic complained about jumping over the initialisation of nfp. wasted 2 days here, gave up
    //do_tail_call:
    do_call:
        if (!f->code) {                                         /* host function */
            uint64_t *args = sp - f->nparams;
            if (f->host(args) != ECCLES_WASM_SUCCESSFUL) TRAP(ECCLES_WASM_TRAP_HOST);
            sp = args + f->nresults;
            continue;
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
    }
//normal exit, the results are already at the bottom of the value stack (G.stk)
done:
    return ECCLES_WASM_OK;
//something went wrong, err has the trap code
trap:
    return err;
}

/* ===================================================================== *
 *  5. public runtime API
 * ===================================================================== */

static void w_clear_err(void) { G.err = ECCLES_WASM_OK; G.err_info = 0; G.err_str[0] = 0; }

//internal error -> the 3 public status values (SUCCESSFUL, FAILED, UNSUPPORTED)
static eccles_wasm_status_t w_status_of(eccles_wasm_error_t e) {
    if (e == ECCLES_WASM_OK) return ECCLES_WASM_SUCCESSFUL;
    if (e == ECCLES_WASM_ERR_UNSUPPORTED_FEATURE || e == ECCLES_WASM_ERR_IMPORT || e == ECCLES_WASM_ERR_NO_FS)
        return ECCLES_WASM_UNSUPPORTED;
    return ECCLES_WASM_FAILED;
}

//drops the loaded module and gives the arena space back (lo goes back to base, hi goes back to just under the stacks)
static void w_module_reset(void) {
    G.ntypes = G.nfuncs = G.nimports = G.nglobals = G.nexports = G.tsize = 0;
    G.start = 0xFFFFu;
    G.has_mem = G.has_table = G.code_seen = G.loaded = G.exit_req = 0;
    G.types = NULL; G.type_off = G.type_canon = NULL; G.funcs = NULL; G.globals = NULL;
    G.table = NULL; G.exports = NULL; G.mem = NULL;
    G.mem_bytes = G.mem_pages = G.code_bytes = 0;
    G.lo = G.base; G.hi = G.hi_init;
}

//sets up the arena. the value stack and the call stack are taken from the TOP first so they are always at the same place
static eccles_wasm_status_t w_setup(void *mem, size_t size, int own_heap) {
    uintptr_t b = ((uintptr_t)mem + 7u) & ~(uintptr_t)7u;
    size_t adj = (size_t)(b - (uintptr_t)mem);
    w_clear_err();
    if (size < adj + 4096u) { w_fail(ECCLES_WASM_ERR_BAD_ARG, (uint32_t)size); return ECCLES_WASM_FAILED; }
    size = (size - adj) & ~(size_t)7u;
#if ECCLES_WASM_USE_HEAP
    if (G.own_heap && G.base) free(G.base);
#endif
    G.own_heap = (uint8_t)own_heap;
    //memset(G.base, 0, size); //zeroing the whole arena was slow on esp32 and we dont need it, removed
    G.base = (uint8_t *)b; G.end = G.base + size; G.lo = G.base; G.hi = G.end;
    G.stk = (uint64_t *)w_ahi(ECCLES_WASM_VALUE_STACK_SLOTS * sizeof(uint64_t), 8);
    G.frames = (w_frame_t *)w_ahi(ECCLES_WASM_CALL_DEPTH * sizeof(w_frame_t), 8);
    if (G.err) { G.inited = 0; return ECCLES_WASM_FAILED; }
    G.hi_init = G.hi;
    w_module_reset();
    G.inited = 1;
    return ECCLES_WASM_SUCCESSFUL;
}

eccles_wasm_status_t eccles_wasm_init_with(void *mem, size_t size) {
    if (!mem) { w_fail(ECCLES_WASM_ERR_BAD_ARG, 0); return ECCLES_WASM_FAILED; }
    return w_setup(mem, size, 0);
}

//NOTE: calling init twice is fine, it does nothing the second time
eccles_wasm_status_t eccles_wasm_init(void) {
    if (G.inited) return ECCLES_WASM_SUCCESSFUL;
#if ECCLES_WASM_USE_HEAP
    {
        //void *m = calloc(1, ECCLES_WASM_ARENA_SIZE); //calloc is slower and we dont need zeroed memory
        void *m = malloc(ECCLES_WASM_ARENA_SIZE);
        if (!m) { w_clear_err(); w_fail(ECCLES_WASM_ERR_NO_MEMORY, ECCLES_WASM_ARENA_SIZE); return ECCLES_WASM_FAILED; }
        { eccles_wasm_status_t s = w_setup(m, ECCLES_WASM_ARENA_SIZE, 1); if (s != ECCLES_WASM_SUCCESSFUL) free(m); return s; }
    }
#else
    return w_setup(g_arena, sizeof g_arena, 0);
#endif
}

eccles_wasm_status_t eccles_wasm_unload(void) {
    if (G.inited) w_module_reset();
    return ECCLES_WASM_SUCCESSFUL;
}

//runs a function from the host side. the arguments are copied to the bottom of the value stack
static eccles_wasm_error_t w_invoke(const w_func_t *f, const uint64_t *args, uint32_t nargs,
                                    uint64_t *res, uint32_t maxres) {
    eccles_wasm_error_t e; uint32_t i;
    if (G.running) return ECCLES_WASM_ERR_BUSY;
    for (i = 0; i < nargs; i++) G.stk[i] = args[i];
    G.running = 1; G.exit_req = 0;
    if (!f->code) e = (f->host(G.stk) == ECCLES_WASM_SUCCESSFUL) ? ECCLES_WASM_OK : ECCLES_WASM_TRAP_HOST;
    else e = w_exec(f);
    G.running = 0;
    if (e == ECCLES_WASM_TRAP_HOST && G.exit_req) e = ECCLES_WASM_OK;       /* WASI proc_exit */
    if (e == ECCLES_WASM_OK && res && !G.exit_req)
        for (i = 0; i < f->nresults && i < maxres; i++) res[i] = G.stk[i];
    return e;
}

//called when the whole module is loaded. runs the start function (if there is one) and cleans up if anything failed
static eccles_wasm_status_t w_finish_load(void) {
    R.fn = NULL; R.ctx = NULL;
    if (!G.err && !G.loaded) {
        G.loaded = 1;
        //printf("loaded ok, code bytes = %u\n", (unsigned)G.code_bytes);
#if ECCLES_WASM_AUTORUN_START
        if (G.start != 0xFFFFu) {
            eccles_wasm_error_t e = w_invoke(&G.funcs[G.start], NULL, 0, NULL, 0);
            if (e != ECCLES_WASM_OK) w_fail(e, 0);
        }
#endif
    }
    if (G.err) {
        ECCLES_WASM_LOG_LINE(eccles_wasm_error_name(G.err));
        G.loaded = 0;
        { eccles_wasm_error_t keep = G.err; uint32_t info = G.err_info; char s[ECCLES_WASM_MAX_NAME + 1];
          memcpy(s, G.err_str, sizeof s); w_module_reset();
          G.err = keep; G.err_info = info; memcpy(G.err_str, s, sizeof s); }
        return w_status_of(G.err);
    }
    return ECCLES_WASM_SUCCESSFUL;
}

static eccles_wasm_status_t w_begin_load(void) {
    if (eccles_wasm_init() != ECCLES_WASM_SUCCESSFUL) return ECCLES_WASM_FAILED;
    if (G.running) { w_clear_err(); w_fail(ECCLES_WASM_ERR_BUSY, 0); return ECCLES_WASM_FAILED; }
    w_clear_err(); w_module_reset();
    R.eof = 0; R.pos0 = 0;
    return ECCLES_WASM_SUCCESSFUL;
}

//load from memory (flash or ram), nothing is copied
eccles_wasm_status_t eccles_wasm_load_buffer(const void *wasm, size_t len) {
    if (w_begin_load() != ECCLES_WASM_SUCCESSFUL) return ECCLES_WASM_FAILED;
    if (!wasm || !len) { w_fail(ECCLES_WASM_ERR_BAD_ARG, 0); return ECCLES_WASM_FAILED; }
    R.mem_src = 1; R.fn = NULL; R.start = R.p = (const uint8_t *)wasm; R.e = R.p + len;
    w_load_module();
    return w_finish_load();
}

//load from anything that can give us bytes. the module is read in small chunks (ECCLES_WASM_CHUNK_SIZE) so it never has to be in ram
eccles_wasm_status_t eccles_wasm_load_stream(eccles_wasm_read_fn read, void *ctx) {
    if (w_begin_load() != ECCLES_WASM_SUCCESSFUL) return ECCLES_WASM_FAILED;
    if (!read) { w_fail(ECCLES_WASM_ERR_BAD_ARG, 0); return ECCLES_WASM_FAILED; }
    R.mem_src = 0; R.fn = read; R.ctx = ctx; R.start = R.p = R.e = R.chunk;
    w_load_module();
    return w_finish_load();
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

//used to pass the file handle to the stream reader. only one file can be open at a time
static struct { eccles_wasm_fs_read_fn rd; void *h; } g_fsctx;
static int w_fs_trampoline(void *ctx, uint8_t *buf, size_t max) { (void)ctx; return g_fsctx.rd(g_fsctx.h, buf, max); }

void eccles_wasm_set_fs(eccles_wasm_fs_open_fn o, eccles_wasm_fs_read_fn r, eccles_wasm_fs_close_fn c) {
    G.fs_open = o; G.fs_read = r; G.fs_close = c;
}

//load a wasm file from the filesystem of the platform (stdio, fatfs, littlefs, zephyr) or from the users own fs hooks (eccles_wasm_set_fs)
eccles_wasm_status_t eccles_wasm_load(const char *path) {
    eccles_wasm_fs_open_fn o = G.fs_open; eccles_wasm_fs_read_fn r = G.fs_read; eccles_wasm_fs_close_fn c = G.fs_close;
    void *h; eccles_wasm_status_t st;
    w_clear_err();
    if (!path) { w_fail(ECCLES_WASM_ERR_BAD_ARG, 0); return ECCLES_WASM_FAILED; }
    if (!o || !r) {
#if defined(W_HAVE_FS)
        o = w_fs_open; r = w_fs_read; c = w_fs_close;
#else
        w_fail(ECCLES_WASM_ERR_NO_FS, 0); return ECCLES_WASM_UNSUPPORTED;
#endif
    }
    h = o(path);
    if (!h) { w_fail(ECCLES_WASM_ERR_IO, 0); return ECCLES_WASM_FAILED; }
    g_fsctx.rd = r; g_fsctx.h = h;
    st = eccles_wasm_load_stream(w_fs_trampoline, NULL);
    if (c) c(h);
    return st;
}

/* ---- calling into the guest ---- */
//linear search, there are only a few exports so a hash table would be overkill
bool eccles_wasm_has_export(const char *name) {
    uint32_t i;
    if (!G.loaded || !name) return false;
    for (i = 0; i < G.nexports; i++) if (strcmp(G.exports[i].name, name) == 0) return true;
    return false;
}

//call an exported function by name. args and results are raw 64bit slots
//the number of args must match exactly or we return FAILED (ERR_SIGNATURE)
eccles_wasm_status_t eccles_wasm_call(const char *name, const uint64_t *args, uint32_t nargs,
                                      uint64_t *results, uint32_t max_results) {
    uint32_t i; const w_func_t *f = NULL; eccles_wasm_error_t e;
    if (G.running) { w_fail(ECCLES_WASM_ERR_BUSY, 0); return ECCLES_WASM_FAILED; }
    w_clear_err();
    if (!G.loaded) { w_fail(ECCLES_WASM_ERR_NOT_LOADED, 0); return ECCLES_WASM_FAILED; }
    if (!name || (nargs && !args)) { w_fail(ECCLES_WASM_ERR_BAD_ARG, 0); return ECCLES_WASM_FAILED; }
    for (i = 0; i < G.nexports; i++) if (strcmp(G.exports[i].name, name) == 0) { f = &G.funcs[G.exports[i].func]; break; }
    if (!f) { w_fail(ECCLES_WASM_ERR_NO_EXPORT, 0); return ECCLES_WASM_FAILED; }
    if (nargs != f->nparams) { w_fail(ECCLES_WASM_ERR_SIGNATURE, f->nparams); return ECCLES_WASM_FAILED; }
    e = w_invoke(f, args, nargs, results, max_results);
    if (e != ECCLES_WASM_OK) { w_fail(e, 0); return ECCLES_WASM_FAILED; }
    return (G.exit_req && G.exit_code != 0) ? ECCLES_WASM_FAILED : ECCLES_WASM_SUCCESSFUL;
}

//shortcut for exports without parameters
eccles_wasm_status_t eccles_wasm_run(const char *name) { return eccles_wasm_call(name, NULL, 0, NULL, 0); }
//budget per call, 0 = unlimited
void eccles_wasm_set_fuel(uint32_t budget) { G.fuel_budget = budget; }

uint8_t *eccles_wasm_mem(uint32_t addr, uint32_t len) {
    if (!G.loaded || !G.mem || (uint64_t)addr + len > G.mem_bytes) return NULL;
    return G.mem + addr;
}
uint32_t eccles_wasm_mem_size(void) { return G.loaded ? G.mem_bytes : 0; }

eccles_wasm_error_t eccles_wasm_last_error(void) { return G.err; }
uint32_t eccles_wasm_last_error_info(void) { return G.err_info; }
const char *eccles_wasm_last_error_str(void) { return G.err_str; }
bool eccles_wasm_exited(int32_t *code) { if (G.exit_req && code) *code = G.exit_code; return G.exit_req != 0; }

const char *eccles_wasm_error_name(eccles_wasm_error_t e) {
    static const char *const n[] = {
        "ok", "not initialised", "no module loaded", "busy (re-entrant call)", "out of arena memory",
        "module exceeds a configured limit", "I/O error", "no filesystem backend", "malformed module",
        "unsupported WebAssembly feature", "unresolved import", "signature mismatch", "no such export",
        "bad argument", "trap: unreachable", "trap: out-of-bounds memory access", "trap: integer divide by zero",
        "trap: integer overflow", "trap: invalid conversion to integer", "trap: undefined table element",
        "trap: indirect call type mismatch", "trap: value stack overflow", "trap: call stack overflow",
        "trap: instruction budget exhausted", "trap: host function failed"
    };
    return (unsigned)e < sizeof n / sizeof n[0] ? n[e] : "unknown error";
}

//how much of the arena is used right now, good for tuning ECCLES_WASM_ARENA_SIZE
eccles_wasm_stats_t eccles_wasm_get_stats(void) {
    eccles_wasm_stats_t s; memset(&s, 0, sizeof s);
    if (!G.inited) return s;
    s.arena_size = (uint32_t)(G.end - G.base);
    s.arena_used = (uint32_t)((G.lo - G.base) + (G.end - G.hi));
    s.memory_bytes = G.mem_bytes; s.memory_pages = G.mem_pages; s.code_bytes = G.code_bytes;
    s.functions = G.nfuncs; s.imports = G.nimports; s.exports = G.nexports; s.loaded = G.loaded;
    return s;
}

//register BEFORE loading the module, the imports are resolved at load time
eccles_wasm_status_t eccles_wasm_register(const char *name, eccles_wasm_host_fn fn, const char *sig) {
    uint8_t tmp[2 + 200 + 16]; uint8_t i;
    if (!name || !fn || !w_sig(sig, tmp, sizeof tmp)) return ECCLES_WASM_FAILED;
    for (i = 0; i < G.nhost; i++) if (strcmp(G.host[i].name, name) == 0) { G.host[i].fn = fn; G.host[i].sig = sig; return ECCLES_WASM_SUCCESSFUL; }
    if (G.nhost >= ECCLES_WASM_MAX_HOST_FUNCS) return ECCLES_WASM_FAILED;
    G.host[G.nhost].name = name; G.host[G.nhost].fn = fn; G.host[G.nhost].sig = sig; G.nhost++;
    return ECCLES_WASM_SUCCESSFUL;
}
void eccles_wasm_unregister_all(void) { G.nhost = 0; }

void eccles_wasm_hal_reset(void) { memset(&G.hal, 0, sizeof G.hal); }
void eccles_wasm_hal_override(const eccles_wasm_hal_t *o) {
    typedef void (*vfn)(void);
    vfn *dst = (vfn *)(void *)&G.hal; const vfn *src = (const vfn *)(const void *)o; size_t i;
    if (!o) return;
    for (i = 0; i < sizeof G.hal / sizeof(vfn); i++) if (src[i]) dst[i] = src[i];
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

//short names for the backend code below
#define W_OK  ECCLES_WASM_SUCCESSFUL
#define W_BAD ECCLES_WASM_FAILED
#define W_NS  ECCLES_WASM_UNSUPPORTED
typedef eccles_wasm_status_t w_st;

//dont ask why i wrote this by hand, i didnt want to include cmsis just for NVIC_SystemReset
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
//find (or allocate) the counter slot of a pin. only ECCLES_WASM_MAX_IRQ pins can be used at the same time
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
//HOST backend is a simulator, it pretends to be a board (pins, adc, uart, i2c) so i can test everything on my pc without hardware
//with ECCLES_WASM_HOST_VIRTUAL_TIME the sleep calls dont really sleep, they just move a fake clock, makes the tests run in ms
#include <stdio.h>
#if defined(_WIN32)
  #include <windows.h>
#else
  #include <time.h>
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

//current time of the simulator in microseconds. with virtual time its just a counter
static uint64_t sim_now_us(void) {
#if defined(ECCLES_WASM_HOST_VIRTUAL_TIME)
    return sim_vt_us;
#elif defined(_WIN32)
    LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c);
    return (uint64_t)(c.QuadPart * 1000000ll / f.QuadPart) + sim_vt_us;
#else
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u + sim_vt_us;
#endif
}
//"sleeps" for us microseconds. virtual time just adds to the counter
static void sim_wait_us(uint32_t us) {
#if defined(ECCLES_WASM_HOST_VIRTUAL_TIME)
    sim_vt_us += us;
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
    //printf("gpio %u = %u\n", (unsigned)pin, (unsigned)level); //debug
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
//ARDUINO backend. Serial/Wire/SPI are c++ classes so a .c file cant call them, thats why there is c++ glue at the bottom of the header
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
//ESP-IDF backend, uses the idf drivers directly (gpio, ledc for pwm, adc oneshot, uart, i2c, spi)
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
//raspberry pi pico sdk backend
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
//STM32 HAL backend. cubemx already configured the peripherals so the begin() functions do nothing, we only use the handles
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
//zephyr backend, only gpio, time, reset, rng and log for now. TODO: add uart and i2c
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

#define W_HAL(f) (G.hal.f ? G.hal.f : g_plat.f)

w_st eccles_wasm_mode(uint32_t pin, uint32_t mode)  { return W_HAL(gpio_mode)(pin, mode); }
w_st eccles_wasm_write(uint32_t pin, uint32_t lvl)  { return W_HAL(gpio_write)(pin, lvl); }
w_st eccles_wasm_read(uint32_t pin, uint32_t *lvl)  { return W_HAL(gpio_read)(pin, lvl); }
w_st eccles_wasm_enable(uint32_t pin) {
    if (W_HAL(gpio_mode)(pin, ECCLES_WASM_PIN_OUTPUT) == W_BAD) return W_BAD;
    return W_HAL(gpio_write)(pin, 1);
}
w_st eccles_wasm_disable(uint32_t pin) {
    if (W_HAL(gpio_mode)(pin, ECCLES_WASM_PIN_OUTPUT) == W_BAD) return W_BAD;
    return W_HAL(gpio_write)(pin, 0);
}
w_st eccles_wasm_toggle(uint32_t pin) {
    uint32_t l = 0; w_st s = W_HAL(gpio_read)(pin, &l);
    if (s != W_OK) return s;
    return W_HAL(gpio_write)(pin, l ? 0u : 1u);
}
w_st eccles_wasm_analog(uint32_t pin, uint32_t v)   { return W_HAL(analog)(pin, v); }
w_st eccles_wasm_pwm(uint32_t pin, uint32_t hz, uint32_t duty) { return W_HAL(pwm)(pin, hz, duty); }
w_st eccles_wasm_adc(uint32_t pin, uint32_t *v)     { return W_HAL(adc)(pin, v); }
w_st eccles_wasm_dac(uint32_t pin, uint32_t v)      { return W_HAL(dac)(pin, v); }
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
w_st eccles_wasm_random(uint32_t *v)                { return W_HAL(random)(v); }
w_st eccles_wasm_log(const uint8_t *b, uint32_t n)  { return W_HAL(log)(b, n); }
w_st eccles_wasm_irq_attach(uint32_t pin, uint32_t e) { return W_HAL(irq_attach)(pin, e); }
w_st eccles_wasm_irq_count(uint32_t pin, uint32_t *c) { return W_HAL(irq_count)(pin, c); }
w_st eccles_wasm_irq_clear(uint32_t pin)            { return W_HAL(irq_clear)(pin); }
w_st eccles_wasm_irq_detach(uint32_t pin)           { return W_HAL(irq_detach)(pin); }
w_st eccles_wasm_uart_begin(uint32_t p, uint32_t baud, int32_t tx, int32_t rx) { return W_HAL(uart_begin)(p, baud, tx, rx); }
w_st eccles_wasm_uart_write(uint32_t p, const uint8_t *b, uint32_t n) { return W_HAL(uart_write)(p, b, n); }
w_st eccles_wasm_uart_read(uint32_t p, uint8_t *b, uint32_t n, uint32_t to, uint32_t *got) { return W_HAL(uart_read)(p, b, n, to, got); }
w_st eccles_wasm_uart_end(uint32_t p)               { return W_HAL(uart_end)(p); }
w_st eccles_wasm_i2c_begin(uint32_t b, int32_t sda, int32_t scl, uint32_t f) { return W_HAL(i2c_begin)(b, sda, scl, f); }
w_st eccles_wasm_i2c_write(uint32_t b, uint32_t a, const uint8_t *d, uint32_t n) { return W_HAL(i2c_write)(b, a, d, n); }
w_st eccles_wasm_i2c_read(uint32_t b, uint32_t a, uint8_t *d, uint32_t n) { return W_HAL(i2c_read)(b, a, d, n); }
w_st eccles_wasm_i2c_write_read(uint32_t b, uint32_t a, const uint8_t *t, uint32_t tn, uint8_t *r, uint32_t rn) { return W_HAL(i2c_write_read)(b, a, t, tn, r, rn); }
w_st eccles_wasm_spi_begin(uint32_t b, int32_t sck, int32_t mosi, int32_t miso, uint32_t f, uint32_t m) { return W_HAL(spi_begin)(b, sck, mosi, miso, f, m); }
w_st eccles_wasm_spi_transfer(uint32_t b, const uint8_t *t, uint8_t *r, uint32_t n) { return W_HAL(spi_transfer)(b, t, r, n); }
w_st eccles_wasm_spi_end(uint32_t b)                { return W_HAL(spi_end)(b); }

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
//little endian helpers for the guest memory. we build the values byte by byte instead of casting the pointer
//because of alignment (cortex m0 hard faults on unaligned access)
static void w_put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static W_MAYBE_UNUSED void w_put64(uint8_t *p, uint64_t v) { w_put32(p, (uint32_t)v); w_put32(p + 4, (uint32_t)(v >> 32)); }
static W_MAYBE_UNUSED uint32_t w_get32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

#if !defined(ECCLES_WASM_NO_BUILTIN_IMPORTS)
#define HW1(name, call) static eccles_wasm_status_t hw_##name(uint64_t *s) { RET(call); }
#define HWOUT(name, outi, call) static eccles_wasm_status_t hw_##name(uint64_t *s) { \
    uint32_t v = 0; uint8_t *p = eccles_wasm_mem(A(outi), 4); w_st st; \
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

static eccles_wasm_status_t hw_log(uint64_t *s) {
    uint8_t *p = eccles_wasm_mem(A(0), A(1)); if (!p) RET(W_BAD);
    RET(eccles_wasm_log(p, A(1)));
}
static eccles_wasm_status_t hw_uart_write(uint64_t *s) {
    uint8_t *p = eccles_wasm_mem(A(1), A(2)); if (!p) RET(W_BAD);
    RET(eccles_wasm_uart_write(A(0), p, A(2)));
}
static eccles_wasm_status_t hw_uart_read(uint64_t *s) {
    uint8_t *p = eccles_wasm_mem(A(1), A(2)), *g = eccles_wasm_mem(A(4), 4); uint32_t got = 0; w_st st;
    if (!p || !g) RET(W_BAD);
    st = eccles_wasm_uart_read(A(0), p, A(2), A(3), &got);
    if (st == W_OK) w_put32(g, got);
    RET(st);
}
static eccles_wasm_status_t hw_i2c_write(uint64_t *s) {
    uint8_t *p = eccles_wasm_mem(A(2), A(3)); if (!p) RET(W_BAD);
    RET(eccles_wasm_i2c_write(A(0), A(1), p, A(3)));
}
static eccles_wasm_status_t hw_i2c_read(uint64_t *s) {
    uint8_t *p = eccles_wasm_mem(A(2), A(3)); if (!p) RET(W_BAD);
    RET(eccles_wasm_i2c_read(A(0), A(1), p, A(3)));
}
static eccles_wasm_status_t hw_i2c_write_read(uint64_t *s) {
    uint8_t *t = eccles_wasm_mem(A(2), A(3)), *r = eccles_wasm_mem(A(4), A(5));
    if (!t || !r) RET(W_BAD);
    RET(eccles_wasm_i2c_write_read(A(0), A(1), t, A(3), r, A(5)));
}
static eccles_wasm_status_t hw_spi_transfer(uint64_t *s) {   /* tx/rx address 0 = "none" */
    uint8_t *t = A(1) ? eccles_wasm_mem(A(1), A(3)) : NULL, *r = A(2) ? eccles_wasm_mem(A(2), A(3)) : NULL;
    if ((A(1) && !t) || (A(2) && !r)) RET(W_BAD);
    RET(eccles_wasm_spi_transfer(A(0), t, r, A(3)));
}

//the hardware import table, name -> function. the guest calls these by name (enable, disable, sleep ...)
//NOTE: the "eccles_wasm_" prefix is optional
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
//dont expect full wasi here, only what printf() and exit() need
#define WASI_OK 0u
#define WASI_EBADF 8u
#define WASI_ENOSYS 52u
//fd_write is what printf() ends up calling in a wasi guest
static eccles_wasm_status_t wasi_fd_write(uint64_t *s) {
    uint32_t fd = A(0), iov = A(1), n = A(2), i, total = 0; uint8_t *out = eccles_wasm_mem(A(3), 4);
    if (!out) RET(28u);                                               /* EINVAL */
    if (fd != 1 && fd != 2) RET(WASI_EBADF);
    for (i = 0; i < n; i++) {
        uint8_t *e = eccles_wasm_mem(iov + i * 8u, 8), *d; uint32_t len;
        if (!e) RET(28u);
        len = w_get32(e + 4); d = eccles_wasm_mem(w_get32(e), len);
        if (!d && len) RET(28u);
        if (len) (void)eccles_wasm_log(d, len);
        total += len;
    }
    w_put32(out, total); RET(WASI_OK);
}
//proc_exit stops the guest. we save the code so the host can read it with eccles_wasm_exited()
static eccles_wasm_status_t wasi_proc_exit(uint64_t *s) {
    G.exit_req = 1; G.exit_code = (int32_t)A(0); (void)s; return ECCLES_WASM_FAILED;   /* unwinds the guest */
}
static eccles_wasm_status_t wasi_sizes_get(uint64_t *s) {            /* args_sizes_get / environ_sizes_get: none */
    uint8_t *a = eccles_wasm_mem(A(0), 4), *b = eccles_wasm_mem(A(1), 4);
    if (!a || !b) RET(28u);
    w_put32(a, 0); w_put32(b, 0); RET(WASI_OK);
}
static eccles_wasm_status_t wasi_get(uint64_t *s) { RET(WASI_OK); }  /* args_get / environ_get: nothing to copy */
static eccles_wasm_status_t wasi_clock_time_get(uint64_t *s) {       /* (id, precision i64, out) -> ns since boot */
    static uint32_t last; static uint64_t acc_us; uint32_t now; uint8_t *out = eccles_wasm_mem(A(2), 8);
    if (!out) RET(28u);
    if (eccles_wasm_micros(&now) != W_OK) RET(WASI_ENOSYS);
    acc_us += (uint32_t)(now - last); last = now;                    /* extend the 32-bit counter */
    w_put64(out, acc_us * 1000u); RET(WASI_OK);
}
static eccles_wasm_status_t wasi_random_get(uint64_t *s) {
    uint8_t *p = eccles_wasm_mem(A(0), A(1)); uint32_t i = 0, v;
    if (!p && A(1)) RET(28u);
    while (i < A(1)) {
        uint32_t k;
        if (eccles_wasm_random(&v) != W_OK) RET(WASI_ENOSYS);
        for (k = 0; k < 4 && i < A(1); k++, i++) p[i] = (uint8_t)(v >> (8 * k));
    }
    RET(WASI_OK);
}
static eccles_wasm_status_t wasi_sched_yield(uint64_t *s) { (void)eccles_wasm_yield(); RET(WASI_OK); }
static const w_builtin_t g_wasi[] = {
    {"fd_write", wasi_fd_write, "i(iiii)"}, {"proc_exit", wasi_proc_exit, "v(i)"},
    {"args_sizes_get", wasi_sizes_get, "i(ii)"}, {"environ_sizes_get", wasi_sizes_get, "i(ii)"},
    {"args_get", wasi_get, "i(ii)"}, {"environ_get", wasi_get, "i(ii)"},
    {"clock_time_get", wasi_clock_time_get, "i(iIi)"}, {"random_get", wasi_random_get, "i(ii)"},
    {"sched_yield", wasi_sched_yield, "i()"}
};
#endif

//search order: host functions registered by the user first, then the builtin hardware table, then wasi
//so the user can override any builtin import
static int w_find_host(const char *name, eccles_wasm_host_fn *fn, const char **sig) {
    size_t i;
    if (strncmp(name, "eccles_wasm_", 12) == 0) name += 12;
    for (i = 0; i < G.nhost; i++)
        if (strcmp(G.host[i].name, name) == 0) { *fn = G.host[i].fn; *sig = G.host[i].sig; return 1; }
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
