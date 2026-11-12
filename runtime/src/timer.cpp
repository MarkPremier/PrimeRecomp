/* Mixed Mode Manager (routine descriptors) and the Time Manager.
 *
 * Routine descriptors: for native PowerPC code a UniversalProcPtr can be the
 * transition vector itself, so NewRoutineDescriptor hands back the proc
 * pointer and CallUniversalProc calls it directly.
 *
 * Time Manager tasks would run at interrupt time on a real Mac. Here they run
 * on the game thread at safe points (every Toolbox call), which keeps guest
 * memory single-threaded. A task is due once its wake-up time has passed;
 * the task usually re-primes itself from inside its own callback.
 */
#include <windows.h>

#include <vector>

#include "pt/runtime.h"

using namespace pt;

/* ---------- Mixed Mode ---------- */
HLE(NewRoutineDescriptor) { RET(ARG(0)); }
HLE(DisposeRoutineDescriptor) { (void)c; }
HLE(CallUniversalProc) {
    /* long CallUniversalProc(UniversalProcPtr proc, ProcInfoType info, ...) - args follow in r5.. */
    uint32_t args[6] = {ARG(2), ARG(3), ARG(4), ARG(5), ARG(6), ARG(7)};
    RET(guest_call(c, ARG(0), args, 6));
}

/* ---------- Time Manager ---------- */
namespace pt::timer {

struct Task {
    uint32_t tm;        /* guest TMTask address */
    bool primed;
    double due_us;      /* host microseconds */
};
static std::vector<Task> g_tasks;
static bool g_in_poll = false;

static double now_us() {
    static LARGE_INTEGER f, s;
    if (!f.QuadPart) { QueryPerformanceFrequency(&f); QueryPerformanceCounter(&s); }
    LARGE_INTEGER n;
    QueryPerformanceCounter(&n);
    return (double)(n.QuadPart - s.QuadPart) * 1e6 / (double)f.QuadPart;
}

static Task *find(uint32_t tm) {
    for (auto &t : g_tasks)
        if (t.tm == tm) return &t;
    return nullptr;
}

/* vertical-blanking tasks (SlotVInstall): VBLTask qLink(0) qType(4) vblAddr(6) vblCount(10) vblPhase(12) */
static std::vector<uint32_t> g_vbls;
static double g_next_vbl = 0;

static void poll_vbl(PPCContext *c, double now) {
    if (g_vbls.empty() || now < g_next_vbl) return;
    g_next_vbl = now + 1e6 / 60.0;
    auto list = g_vbls;
    for (uint32_t t : list) {
        int16_t n = (int16_t)ld16(t + 10);
        if (n <= 0) continue;
        st16(t + 10, (uint16_t)(n - 1));
        if (n - 1 == 0) {
            uint32_t arg = t;
            guest_call(c, ld32(t + 6), &arg, 1);
            if ((int16_t)ld16(t + 10) == 0)
                for (auto it = g_vbls.begin(); it != g_vbls.end(); ++it)
                    if (*it == t) { g_vbls.erase(it); break; }
        }
    }
}

void vbl_install(uint32_t t) { g_vbls.push_back(t); }
void vbl_remove(uint32_t t) {
    for (auto it = g_vbls.begin(); it != g_vbls.end(); ++it)
        if (*it == t) { g_vbls.erase(it); return; }
}

void poll(PPCContext *c) {
    if (g_in_poll) return;
    g_in_poll = true;
    double now = now_us();
    poll_vbl(c, now);
    if (g_tasks.empty()) { g_in_poll = false; return; }
    for (size_t i = 0; i < g_tasks.size(); i++) {
        Task t = g_tasks[i];
        if (!t.primed || now < t.due_us) continue;
        g_tasks[i].primed = false;
        /* TMTask: qLink(0) qType(4) tmAddr(6) tmCount(10) tmWakeUp(14) tmReserved(18) */
        st16(t.tm + 4, ld16(t.tm + 4) & 0x7FFF);  /* clear "active" bit */
        uint32_t proc = ld32(t.tm + 6);
        if (proc) {
            uint32_t arg = t.tm;
            guest_call(c, proc, &arg, 1);
        }
    }
    g_in_poll = false;
}

}  // namespace pt::timer

using namespace pt::timer;

HLE(InsTime) {
    if (!find(ARG(0))) g_tasks.push_back({ARG(0), false, 0});
}
HLE(InsXTime) {
    if (!find(ARG(0))) g_tasks.push_back({ARG(0), false, 0});
}
HLE(RmvTime) {
    for (auto it = g_tasks.begin(); it != g_tasks.end(); ++it)
        if (it->tm == ARG(0)) {
            /* tmCount receives the unused time (we report none) */
            st32(ARG(0) + 10, 0);
            g_tasks.erase(it);
            return;
        }
}
HLE(PrimeTime) {
    /* void PrimeTime(QElemPtr tmTaskPtr, long count): count > 0 ms, count < 0 -us */
    Task *t = find(ARG(0));
    if (!t) return;
    int32_t count = (int32_t)ARG(1);
    double delay = count >= 0 ? count * 1000.0 : -(double)count;
    t->due_us = now_us() + delay;
    t->primed = true;
    st16(ARG(0) + 4, ld16(ARG(0) + 4) | 0x8000);
}

HLE(SlotVInstall) { vbl_install(ARG(0)); RET(0); }
HLE(SlotVRemove) { vbl_remove(ARG(0)); RET(0); }
