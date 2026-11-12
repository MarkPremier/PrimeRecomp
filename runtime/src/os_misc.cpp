/* Operating-system odds and ends: Gestalt, SysEnvirons, time, string utilities,
 * Process Manager, low-memory accessors.
 *
 * The emulated machine is a Power Macintosh 7500-class system running Mac OS
 * 7.6.1 with QuickTime 2.5 - a typical 1996 target for Prime Target.
 */
#include <windows.h>

#include <cctype>
#include <ctime>

#include "pt/runtime.h"

using namespace pt;

static const uint16_t kSystemVersion = 0x0761;

/* ---------- Gestalt ---------- */
static bool gestalt_value(uint32_t sel, uint32_t &out) {
    switch (sel) {
    case 'sysv': out = kSystemVersion; return true;
    case 'sysa': out = 2; return true;                 /* gestaltPowerPC */
    case 'cput': out = 0x0104; return true;            /* gestaltCPU604 */
    case 'proc': out = 3; return true;                 /* gestalt68020 class for 68k emulator */
    case 'fpu ': out = 0; return true;
    case 'mach': out = 68; return true;                /* gestaltPowerMac7500 */
    case 'qd  ': out = 0x0230; return true;            /* Color QuickDraw 32-bit, v2.3 */
    case 'qdrw': out = 0x0F; return true;
    case 'qtim': out = 0x02508000; return true;        /* QuickTime 2.5 */
    case 'evnt': out = 0x07; return true;
    case 'snd ': out = 0x00000F3F; return true;        /* stereo, 16-bit, input, play+record, mixer */
    case 'sm  ': out = 0x0300; return true;            /* Sound Manager 3.x */
    case 'smvr': out = 0x03208000; return true;
    case 'ram ': out = 64u << 20; return true;
    case 'lram': out = 64u << 20; return true;
    case 'vm  ': out = 0; return true;                 /* virtual memory off */
    case 'mmu ': out = 3; return true;
    case 'fs  ': out = 0x1E; return true;              /* FSSpec calls, HFS, folder manager */
    case 'fold': out = 1; return true;
    case 'alis': out = 1; return true;
    case 'hdwr': out = 0x80; return true;
    case 'os  ': out = 0x3E; return true;              /* launch, temp mem, process mgr */
    case 'appr': out = 0; return false;
    case 'dict': out = 0; return false;
    case 'drag': out = 0; return false;
    case 'tmgr': out = 3; return true;                 /* extended Time Manager */
    case 'kbd ': out = 5; return true;
    case 'atlk': out = 0; return false;                /* AppleTalk: report absent */
    case 'cfrg': out = 1; return true;
    case 'mixd': out = 1; return true;
    case 'ppcf': out = 1; return true;
    default: return false;
    }
}

HLE(Gestalt) {
    uint32_t sel = ARG(0), out = 0;
    char s[5] = {(char)(sel >> 24), (char)(sel >> 16), (char)(sel >> 8), (char)sel, 0};
    if (gestalt_value(sel, out)) {
        if (ARG(1)) st32(ARG(1), out);
        logf(LOG_HLE, "  Gestalt('%s') = %08X\n", s, out);
        RET(0);
    } else {
        logf(LOG_UNIMPL | LOG_HLE, "  Gestalt('%s') undefined\n", s);
        RET(-5551); /* gestaltUndefSelectorErr */
    }
}

HLE(SysEnvirons) {
    uint32_t rec = ARG(1);
    st16(rec + 0, 2);              /* environsVersion */
    st16(rec + 2, 21);             /* machineType: envMachUnknown-ish for PPC */
    st16(rec + 4, kSystemVersion); /* systemVersion */
    st16(rec + 6, 3);              /* processor: env68020 (emulated) */
    st8(rec + 8, 1);               /* hasFPU */
    st8(rec + 9, 1);               /* hasColorQD */
    st16(rec + 10, 5);             /* keyBoardType: Apple Extended */
    st16(rec + 12, 0);             /* atDrvrVersNum */
    st16(rec + 14, -1);            /* sysVRefNum */
    RET(0);
}

/* ---------- time ---------- */
HLE(TickCount) { RET(ticks()); }
HLE(LMGetTime) { RET(0); }

static uint32_t mac_seconds_now() {
    /* seconds since 1904-01-01 local time */
    time_t t = time(nullptr);
    struct tm lt;
    localtime_s(&lt, &t);
    time_t local = _mkgmtime(&lt);
    return (uint32_t)((int64_t)local + 2082844800ll);
}
HLE(GetDateTime) { if (ARG(0)) st32(ARG(0), mac_seconds_now()); }
HLE(Delay) {
    uint32_t start = ticks();
    Sleep(ARG(0) * 1000 / 60);
    if (ARG(1)) st32(ARG(1), start + ARG(0));
}
HLE(SysBeep) { MessageBeep(MB_OK); }

/* ---------- strings ---------- */
HLE(p2cstr) {
    uint32_t a = ARG(0);
    uint8_t n = g_mem[a];
    memmove(g_mem + a, g_mem + a + 1, n);
    g_mem[a + n] = 0;
    RET(a);
}
HLE(c2pstr) {
    uint32_t a = ARG(0);
    size_t n = strlen((char *)g_mem + a);
    if (n > 255) n = 255;
    memmove(g_mem + a + 1, g_mem + a, n);
    g_mem[a] = (uint8_t)n;
    RET(a);
}
HLE(NumToString) {
    char buf[16];
    snprintf(buf, sizeof buf, "%d", (int32_t)ARG(0));
    write_pstr(ARG(1), buf);
}
HLE(StringToNum) {
    std::string s = read_pstr(ARG(0));
    st32(ARG(1), (uint32_t)strtol(s.c_str(), nullptr, 10));
}

static int mac_casecmp(const std::string &a, const std::string &b, bool caseSens) {
    size_t n = a.size() < b.size() ? a.size() : b.size();
    for (size_t i = 0; i < n; i++) {
        int x = (uint8_t)a[i], y = (uint8_t)b[i];
        if (!caseSens) { x = toupper(x); y = toupper(y); }
        if (x != y) return x < y ? -1 : 1;
    }
    return a.size() == b.size() ? 0 : (a.size() < b.size() ? -1 : 1);
}
HLE(EqualString) {
    /* Boolean EqualString(ConstStr255Param a, ConstStr255Param b, Boolean caseSens, Boolean diacSens) */
    RET(mac_casecmp(read_pstr(ARG(0)), read_pstr(ARG(1)), ARG(2) & 0xFF) == 0);
}
HLE(IUCompString) { RET((uint32_t)(int32_t)mac_casecmp(read_pstr(ARG(0)), read_pstr(ARG(1)), false)); }

/* ---------- Script Manager (Roman script only) ---------- */
HLE(GetScriptManagerVariable) { RET(0); }
HLE(GetScriptVariable) { RET(0); }
HLE(KeyTranslate) {
    /* UInt32 KeyTranslate(const void *transData, UInt16 keycode, UInt32 *state) - handled in events.cpp */
    RET(0);
}

/* ---------- debugger ---------- */
HLE(DebugStr) { logf(LOG_UNIMPL | LOG_HLE, "DebugStr: %s\n", read_pstr(ARG(0)).c_str()); }

/* ---------- low memory ---------- */
HLE(LMGetMBarHeight) { RET(ld16(0x0BAA)); }
HLE(LMSetMBarHeight) { st16(0x0BAA, ARG(0)); }
HLE(GetMBarHeight) { RET(ld16(0x0BAA)); }
HLE(LMGetCurApName) { RET(0x0910); }
HLE(LMGetCurDirStore) { RET(ld32(0x0398)); }
HLE(LMGetGrayRgn) { RET(ld32(0x09EE)); }
HLE(LMGetTEScrpLength) { RET(ld16(0x0AB0)); }
HLE(LMSetTEScrpLength) { st16(0x0AB0, ARG(0)); }
