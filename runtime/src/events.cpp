/* Event Manager, cursor calls, Apple Event Manager stubs. */
#include <windows.h>

#include "pt/host.h"
#include "pt/qd.h"
#include "pt/resources.h"
#include "pt/runtime.h"

using namespace pt;

namespace pt::wm {
bool pending_window_event(uint16_t mask, host::MacEvent &e, bool remove);
}

static void write_event(uint32_t a, const host::MacEvent &e) {
    st16(a, e.what);
    st32(a + 2, e.message);
    st32(a + 6, e.when);
    st16(a + 10, (uint16_t)e.v);
    st16(a + 12, (uint16_t)e.h);
    st16(a + 14, e.modifiers);
}

static host::MacEvent null_event() {
    host::MacEvent e{};
    e.what = 0;
    e.when = ticks();
    host::mouse_global(e.v, e.h);
    e.modifiers = host::modifiers();
    return e;
}

static bool get_event(uint16_t mask, host::MacEvent &e, bool include_window_events) {
    host::pump();
    if (host::next_event(mask, e, true)) return true;
    if (include_window_events && wm::pending_window_event(mask, e, true)) return true;
    e = null_event();
    return false;
}

HLE(GetNextEvent) {
    host::MacEvent e;
    bool got = get_event((uint16_t)ARG(0), e, true);
    write_event(ARG(1), e);
    if (got) logf(LOG_EVT, "event %d msg %08X at %d,%d\n", e.what, e.message, e.h, e.v);
    RET(got);
}

HLE(WaitNextEvent) {
    /* Boolean WaitNextEvent(EventMask mask, EventRecord *ev, UInt32 sleep, RgnHandle mouseRgn) */
    host::MacEvent e;
    uint32_t until = ticks() + ARG(2);
    bool got;
    for (;;) {
        got = get_event((uint16_t)ARG(0), e, true);
        if (got || ticks() >= until || ARG(2) == 0) break;
        host::present(false);
        Sleep(1);
    }
    write_event(ARG(1), e);
    if (got) logf(LOG_EVT, "event %d msg %08X at %d,%d\n", e.what, e.message, e.h, e.v);
    RET(got);
}

HLE(GetOSEvent) {
    host::MacEvent e;
    bool got = get_event((uint16_t)ARG(0), e, false);
    write_event(ARG(1), e);
    RET(got);
}

HLE(FlushEvents) { host::flush_events((uint16_t)ARG(0)); }

HLE(GetKeys) {
    host::pump();
    uint8_t k[16];
    host::key_map(k);
    memcpy(g_mem + ARG(0), k, 16);
    if (g_log_mask & LOG_EVT) {
        bool any = false;
        for (int i = 0; i < 16; i++) any |= k[i] != 0;
        if (any) {
            logf(LOG_EVT, "GetKeys:");
            for (int i = 0; i < 128; i++) if ((k[i >> 3] >> (i & 7)) & 1) logf(LOG_EVT, " %02X", i);
            logf(LOG_EVT, " (LR %08X)\n", c->lr);
        }
    }
}

HLE(GetMouse) {
    host::pump();
    int16_t v, h;
    host::mouse_global(v, h);
    uint32_t port = qd::current_port();
    if (port) {
        qd::Surface s = qd::surface_of_port(port);
        v = (int16_t)(v + s.bounds.top);
        h = (int16_t)(h + s.bounds.left);
    }
    st16(ARG(0), (uint16_t)v);
    st16(ARG(0) + 2, (uint16_t)h);
}

HLE(Button) { host::pump(); RET(host::mouse_down()); }
HLE(StillDown) {
    host::pump();
    host::MacEvent e;
    RET(host::mouse_down() && !host::next_event(1 << 2, e, false));
}
HLE(WaitMouseUp) {
    host::pump();
    host::MacEvent e;
    bool up = host::next_event(1 << 2, e, true);
    RET(host::mouse_down() && !up);
}
HLE(SystemTask) { host::pump(); }
HLE(SystemClick) { (void)c; }
HLE(OpenDeskAcc) { RET(0); }

/* KeyTranslate: map a virtual key code (+ modifier bits 8-15) to a character */
static const char kUnshifted[128] = {
    'a', 's', 'd', 'f', 'h', 'g', 'z', 'x', 'c', 'v', 0, 'b', 'q', 'w', 'e', 'r',
    'y', 't', '1', '2', '3', '4', '6', '5', '=', '9', '7', '-', '8', '0', ']', 'o',
    'u', '[', 'i', 'p', 13, 'l', 'j', '\'', 'k', ';', '\\', ',', '/', 'n', 'm', '.',
    9, ' ', '`', 8, 0, 27, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, '.', 0, '*', 0, '+', 0, 27, 0, 0, 0, '/', 3, 0, '-', 0,
    0, '=', '0', '1', '2', '3', '4', '5', '6', '7', 0, '8', '9', 0, 0, 0,
    16, 16, 16, 16, 16, 16, 0, 16, 0, 16, 0, 16, 0, 16, 0, 16,
    0, 16, 5, 1, 11, 127, 16, 4, 16, 12, 16, 28, 29, 31, 30, 0,
};
HLE(KeyTranslate) {
    uint16_t code = (uint16_t)ARG(1);
    uint8_t ch = (uint8_t)kUnshifted[code & 0x7F];
    if ((code & 0x0200) && ch >= 'a' && ch <= 'z') ch = (uint8_t)(ch - 32);
    if (code & 0x80) ch = 0;  /* key up */
    RET(ch);
}

/* ---------- cursor ---------- */
HLE(InitCursor) {
    /* arrow from qd globals */
    if (qd::g_qd_theport) host::set_cursor(g_mem + qd::g_qd_theport - 108);
    host::show_cursor();
}
HLE(SetCursor) { host::set_cursor(g_mem + ARG(0)); }
HLE(HideCursor) { host::hide_cursor(); }
HLE(ShowCursor) { host::show_cursor(); }
HLE(GetCursor) { RET(res::get_handle('CURS', (int16_t)ARG(0))); }

/* ---------- Apple Event Manager: nothing is ever delivered ---------- */
HLE(AEInstallEventHandler) { RET(0); }
HLE(AEProcessAppleEvent) { RET(0); }
HLE(AEGetParamDesc) { RET(-1701); }   /* errAEDescNotFound */
HLE(AECountItems) { if (ARG(1)) st32(ARG(1), 0); RET(0); }
HLE(AEGetNthPtr) { RET(-1701); }
HLE(AEGetAttributePtr) { RET(-1701); }
