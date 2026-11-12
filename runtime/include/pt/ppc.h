/* PowerPC guest state and memory accessors used by the recompiled code.
 *
 * The guest is a 32-bit big-endian PowerPC (603/604 class, no AltiVec).
 * Guest memory is one 4 GB host reservation: guest address A lives at
 * g_mem + A, so every 32-bit address is representable and wraps naturally.
 */
#ifndef PT_PPC_H
#define PT_PPC_H

#include <stdint.h>
#include <string.h>
#include <math.h>

#ifdef _MSC_VER
#include <stdlib.h>
#include <intrin.h>
#define PT_BSWAP16(x) _byteswap_ushort(x)
#define PT_BSWAP32(x) _byteswap_ulong(x)
#define PT_BSWAP64(x) _byteswap_uint64(x)
#define PT_NOINLINE __declspec(noinline)
#define PT_NORETURN __declspec(noreturn)
#else
#define PT_BSWAP16(x) __builtin_bswap16(x)
#define PT_BSWAP32(x) __builtin_bswap32(x)
#define PT_BSWAP64(x) __builtin_bswap64(x)
#define PT_NOINLINE __attribute__((noinline))
#define PT_NORETURN __attribute__((noreturn))
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct CRField {
    uint8_t lt, gt, eq, so;
} CRField;

typedef struct PPCContext {
    uint32_t r[32];
    double f[32];
    CRField cr[8];
    uint32_t lr, ctr;
    uint32_t xer_so, xer_ov, xer_ca;
    uint32_t fpscr;
} PPCContext;

typedef void (*PPCFunc)(PPCContext *c);

extern uint8_t *g_mem;

/* ---- memory ---- */
static inline uint8_t  ld8(uint32_t a)  { return g_mem[a]; }
static inline uint16_t ld16(uint32_t a) { uint16_t v; memcpy(&v, g_mem + a, 2); return PT_BSWAP16(v); }
static inline uint32_t ld32(uint32_t a) { uint32_t v; memcpy(&v, g_mem + a, 4); return PT_BSWAP32(v); }
static inline void st8(uint32_t a, uint32_t v)  { g_mem[a] = (uint8_t)v; }
static inline void st16(uint32_t a, uint32_t v) { uint16_t x = PT_BSWAP16((uint16_t)v); memcpy(g_mem + a, &x, 2); }
static inline void st32(uint32_t a, uint32_t v) { uint32_t x = PT_BSWAP32(v); memcpy(g_mem + a, &x, 4); }
static inline uint16_t ld16br(uint32_t a) { uint16_t v; memcpy(&v, g_mem + a, 2); return v; }
static inline uint32_t ld32br(uint32_t a) { uint32_t v; memcpy(&v, g_mem + a, 4); return v; }
static inline void st16br(uint32_t a, uint32_t v) { uint16_t x = (uint16_t)v; memcpy(g_mem + a, &x, 2); }
static inline void st32br(uint32_t a, uint32_t v) { memcpy(g_mem + a, &v, 4); }

static inline double ldf32(uint32_t a) { uint32_t v = ld32(a); float f; memcpy(&f, &v, 4); return (double)f; }
static inline double ldf64(uint32_t a) { uint64_t v; memcpy(&v, g_mem + a, 8); v = PT_BSWAP64(v); double d; memcpy(&d, &v, 8); return d; }
static inline void stf32(uint32_t a, double d) { float f = (float)d; uint32_t v; memcpy(&v, &f, 4); st32(a, v); }
static inline void stf64(uint32_t a, double d) { uint64_t v; memcpy(&v, &d, 8); v = PT_BSWAP64(v); memcpy(g_mem + a, &v, 8); }

/* ---- integer helpers ---- */
static inline uint32_t rotl32(uint32_t v, uint32_t n) { n &= 31; return n ? (v << n) | (v >> (32 - n)) : v; }

static inline void cr_set_signed(PPCContext *c, int f, int32_t a, int32_t b) {
    c->cr[f].lt = a < b; c->cr[f].gt = a > b; c->cr[f].eq = a == b; c->cr[f].so = (uint8_t)c->xer_so;
}
static inline void cr_set_unsigned(PPCContext *c, int f, uint32_t a, uint32_t b) {
    c->cr[f].lt = a < b; c->cr[f].gt = a > b; c->cr[f].eq = a == b; c->cr[f].so = (uint8_t)c->xer_so;
}
static inline void cr0_rec(PPCContext *c, uint32_t v) { cr_set_signed(c, 0, (int32_t)v, 0); }

static inline void cr_set_fp(PPCContext *c, int f, double a, double b) {
    int un = (a != a) || (b != b);
    c->cr[f].lt = !un && a < b; c->cr[f].gt = !un && a > b; c->cr[f].eq = !un && a == b; c->cr[f].so = (uint8_t)un;
}

static inline uint8_t *cr_bit_ptr(PPCContext *c, int bit) {
    CRField *f = &c->cr[bit >> 2];
    switch (bit & 3) { case 0: return &f->lt; case 1: return &f->gt; case 2: return &f->eq; default: return &f->so; }
}

static inline uint32_t mfcr(PPCContext *c) {
    uint32_t v = 0;
    for (int i = 0; i < 8; i++)
        v |= (uint32_t)((c->cr[i].lt << 3) | (c->cr[i].gt << 2) | (c->cr[i].eq << 1) | c->cr[i].so) << (28 - 4 * i);
    return v;
}
static inline void mtcrf(PPCContext *c, uint32_t mask, uint32_t v) {
    for (int i = 0; i < 8; i++) {
        if (mask & (0x80u >> i)) {
            uint32_t n = (v >> (28 - 4 * i)) & 0xF;
            c->cr[i].lt = (n >> 3) & 1; c->cr[i].gt = (n >> 2) & 1; c->cr[i].eq = (n >> 1) & 1; c->cr[i].so = n & 1;
        }
    }
}

static inline uint32_t cntlzw(uint32_t v) {
#ifdef _MSC_VER
    unsigned long i; return _BitScanReverse(&i, v) ? 31 - i : 32;
#else
    return v ? (uint32_t)__builtin_clz(v) : 32;
#endif
}

static inline uint32_t divw(uint32_t a, uint32_t b) {
    if (b == 0 || (a == 0x80000000u && b == 0xFFFFFFFFu)) return 0; /* undefined on PPC; 603/604 give 0 or -1 */
    return (uint32_t)((int32_t)a / (int32_t)b);
}
static inline uint32_t divwu(uint32_t a, uint32_t b) { return b ? a / b : 0; }

/* sraw/srawi set CA when the result is negative and any 1 bits were shifted out */
static inline uint32_t sraw(PPCContext *c, uint32_t v, uint32_t n) {
    n &= 63;
    if (n >= 32) { c->xer_ca = (v >> 31); return (int32_t)v < 0 ? 0xFFFFFFFFu : 0; }
    uint32_t r = (uint32_t)((int32_t)v >> n);
    c->xer_ca = ((int32_t)v < 0) && n && (v & ((1u << n) - 1));
    return r;
}

/* fctiw/fctiwz: saturating conversion; result lands in the low word of the FPR */
static inline double fctiw_bits(double d, int truncate) {
    int32_t r;
    if (d != d) r = (int32_t)0x80000000;
    else if (d >= 2147483647.0) r = 0x7FFFFFFF;
    else if (d <= -2147483648.0) r = (int32_t)0x80000000;
    else r = truncate ? (int32_t)d : (int32_t)nearbyint(d);
    uint64_t bits = 0xFFF8000000000000ull | (uint32_t)r;
    double out; memcpy(&out, &bits, 8); return out;
}
static inline uint64_t f2bits(double d) { uint64_t v; memcpy(&v, &d, 8); return v; }
static inline double bits2f(uint64_t v) { double d; memcpy(&d, &v, 8); return d; }

/* ---- function-entry trace ring (read by the crash handler) ---- */
extern uint32_t g_trace_ring[256];
extern unsigned g_trace_pos;
extern uint8_t g_fn_watch_enabled;
void pt_fn_watch(PPCContext *c, uint32_t addr);
#define PT_FN_ENTER(addr) (g_trace_ring[g_trace_pos++ & 255] = (addr), g_fn_watch_enabled ? pt_fn_watch(c, (addr)) : (void)0)

/* ---- control transfer (implemented in the runtime) ---- */
void ppc_call_indirect(PPCContext *c, uint32_t target);
PT_NORETURN void ppc_fatal(const char *fmt, ...);
void ppc_trap(PPCContext *c, uint32_t pc);

#ifdef __cplusplus
}
#endif
#endif
