/* Import binding: every PEF import gets a fake TVector whose code address
 * dispatches here, to a registered HLE implementation or a logging stub. */
#include <windows.h>
#include <mmsystem.h>

#include <cstdarg>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "pt/runtime.h"

extern "C" const char *const ppc_import_names[];
extern "C" const unsigned ppc_import_count;

namespace pt {

static std::map<std::string, HLEFunc> &registry() {
    static std::map<std::string, HLEFunc> r;
    return r;
}

HLERegistrar::HLERegistrar(const char *name, HLEFunc fn) { registry()[name] = fn; }

static std::vector<HLEFunc> g_bound;
static std::vector<unsigned> g_call_counts;

void hle_bind_imports() {
    g_bound.assign(ppc_import_count, nullptr);
    g_call_counts.assign(ppc_import_count, 0);
    unsigned implemented = 0;
    for (unsigned i = 0; i < ppc_import_count; i++) {
        /* TVector {code, toc} for import i */
        st32(PT_IMPORT_TVEC_BASE + 8 * i, PT_IMPORT_CODE_BASE + 4 * i);
        st32(PT_IMPORT_TVEC_BASE + 8 * i + 4, 0);
        auto it = registry().find(ppc_import_names[i]);
        if (it != registry().end()) {
            g_bound[i] = it->second;
            implemented++;
        }
    }
    logf(LOG_HLE | LOG_UNIMPL, "HLE: %u of %u imports implemented\n", implemented, ppc_import_count);
}

namespace timer { void poll(PPCContext *c); }
namespace host { void pump(); }
namespace snd { void poll(PPCContext *c); }

static PPCContext *g_cur_ctx = nullptr;
PPCContext *current_ctx() { return g_cur_ctx; }

/* "interrupt" work, run on the game thread at safe points */
static void service_interrupts(PPCContext *c) {
    timer::poll(c);
    snd::poll(c);
    static uint64_t last_pump = 0;
    uint64_t now = GetTickCount64();
    if (now - last_pump >= 2) { last_pump = now; host::pump(); }
}

void start_interrupt_clock() {
    timeBeginPeriod(1);
    CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
        for (;;) { Sleep(1); g_irq_pending = 1; }
    }, nullptr, 0, nullptr);
}

void hle_dispatch(PPCContext *c, uint32_t idx) {
    g_call_counts[idx]++;
    g_cur_ctx = c;
    service_interrupts(c);   /* every Toolbox call is also a safe point */
    const char *name = ppc_import_names[idx];
    if (g_bound[idx]) {
        logf(LOG_HLE, "%-24s r3=%08X r4=%08X r5=%08X r6=%08X  LR=%08X\n", name, c->r[3], c->r[4], c->r[5],
             c->r[6], c->lr);
        g_bound[idx](c);
        return;
    }
    static std::set<unsigned> warned;
    if (warned.insert(idx).second || (g_log_mask & LOG_HLE))
        logf(LOG_UNIMPL | LOG_HLE, "UNIMPLEMENTED %-20s r3=%08X r4=%08X r5=%08X  LR=%08X\n", name, c->r[3],
             c->r[4], c->r[5], c->lr);
    c->r[3] = 0; /* noErr / NULL */
}

}  // namespace pt

extern "C" void ppc_irq_poll(PPCContext *c) {
    g_irq_pending = 0;
    static bool busy = false;   /* guest code run by the poll must not re-enter it */
    if (busy) return;
    busy = true;
    pt::service_interrupts(c);
    busy = false;
}
