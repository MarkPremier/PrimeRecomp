/* Indirect control transfer, guest callbacks, traps, timebase. */
#include <windows.h>

#include <cstdarg>
#include <string>
#include <vector>

#include "image_info.h"
#include "ppc_funcs.h"
#include "pt/runtime.h"

extern "C" const char *const ppc_import_names[];
extern "C" const unsigned ppc_import_count;

uint32_t g_trace_ring[256];
unsigned g_trace_pos;
uint8_t g_fn_watch_enabled;

static std::vector<PPCFunc> g_direct;  /* one slot per code word */
static std::vector<uint8_t> g_watch;   /* PT_WATCH: functions to log on entry */

extern "C" void pt_fn_watch(PPCContext *c, uint32_t addr) {
    uint32_t i = (addr - PT_IMAGE_CODE_BASE) / 4;
    if (i >= g_watch.size() || !g_watch[i]) return;
    const char *name = "?";
    for (unsigned k = 0; k < ppc_func_count; k++)
        if (ppc_func_table[k].addr == addr) { name = ppc_func_table[k].name; break; }
    fprintf(stderr, "-> %s(%08X, %08X, %08X, %08X) LR=%08X\n", name, c->r[3], c->r[4], c->r[5], c->r[6], c->lr);
}

namespace pt {

void dispatch_init() {
    g_direct.assign(PT_IMAGE_CODE_SIZE / 4, nullptr);
    for (unsigned i = 0; i < ppc_func_count; i++)
        g_direct[(ppc_func_table[i].addr - PT_IMAGE_CODE_BASE) / 4] = ppc_func_table[i].fn;
    if (const char *w = getenv("PT_WATCH")) {
        g_watch.assign(PT_IMAGE_CODE_SIZE / 4, 0);
        std::string list(w);
        size_t p = 0;
        while (p <= list.size()) {
            size_t e = list.find(',', p);
            std::string name = list.substr(p, e == std::string::npos ? std::string::npos : e - p);
            for (unsigned i = 0; i < ppc_func_count; i++)
                if (name == ppc_func_table[i].name || name == "*") {
                    g_watch[(ppc_func_table[i].addr - PT_IMAGE_CODE_BASE) / 4] = 1;
                    g_fn_watch_enabled = 1;
                }
            if (e == std::string::npos) break;
            p = e + 1;
        }
    }
}

const char *func_name(uint32_t addr) {
    /* nearest function at or below addr */
    const char *best = "?";
    uint32_t best_addr = 0;
    for (unsigned i = 0; i < ppc_func_count; i++) {
        uint32_t a = ppc_func_table[i].addr;
        if (a <= addr && a >= best_addr) {
            best_addr = a;
            best = ppc_func_table[i].name;
        }
    }
    return best;
}

uint32_t guest_call(PPCContext *c, uint32_t tvector, const uint32_t *args, int nargs) {
    PPCContext saved = *c;
    uint32_t code = ld32(tvector), toc = ld32(tvector + 4);
    c->r[1] = (saved.r[1] - 256) & ~15u;  /* fresh frame below the caller's */
    st32(c->r[1], saved.r[1]);             /* back chain */
    for (int i = 0; i < nargs && i < 8; i++) c->r[3 + i] = args[i];
    c->r[2] = toc;
    c->lr = 0;
    ppc_call_indirect(c, code);
    uint32_t result = c->r[3];
    *c = saved;
    return result;
}

}  // namespace pt

extern "C" void ppc_call_indirect(PPCContext *c, uint32_t target) {
    if (target - PT_IMAGE_CODE_BASE < PT_IMAGE_CODE_SIZE) {
        PPCFunc fn = g_direct[(target - PT_IMAGE_CODE_BASE) / 4];
        if (!fn) ppc_fatal("indirect branch to %08X, inside %s, is not a function start", target, pt::func_name(target));
        fn(c);
        return;
    }
    uint32_t idx = (target - PT_IMPORT_CODE_BASE) / 4;
    if (target >= PT_IMPORT_CODE_BASE && idx < ppc_import_count && (target & 3) == 0) {
        pt::hle_dispatch(c, idx);
        return;
    }
    ppc_fatal("indirect branch to unmapped address %08X (LR %08X)", target, c->lr);
}

extern "C" void ppc_fatal(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    pt::fatal("%s", buf);
}

extern "C" void ppc_trap(PPCContext *c, uint32_t pc) {
    ppc_fatal("guest trap at %08X in %s (LR %08X)", pc, pt::func_name(pc), c->lr);
}

static uint64_t timebase() {
    static LARGE_INTEGER freq, start;
    if (!freq.QuadPart) {
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&start);
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    /* 601-class timebase: 7.8336 MHz (bus/4 on a 7600-era machine is close enough) */
    return (uint64_t)((double)(now.QuadPart - start.QuadPart) * 7833600.0 / (double)freq.QuadPart);
}
extern "C" uint32_t ppc_timebase_lo(void) { return (uint32_t)timebase(); }
extern "C" uint32_t ppc_timebase_hi(void) { return (uint32_t)(timebase() >> 32); }
