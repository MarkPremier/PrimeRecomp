/* Prime Target native runtime: entry point.
 *
 * Environment:
 *   PT_GAME_DIR   folder holding the extracted volume (default: <exe>/game)
 *   PT_LOG        comma list: hle,unimpl,mem,file,res,gfx,snd,evt,all (default: unimpl)
 */
#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "image_info.h"
#include "ppc_funcs.h"
#include "pt/host.h"
#include "pt/resources.h"
#include "pt/runtime.h"
#include "pt/vfs.h"

namespace pt {
namespace snd { void init(); }
namespace setup { bool import_game(const std::filesystem::path &root); }
void start_interrupt_clock();

/* the low-memory globals Mac code commonly reads directly */
static void lowmem_init() {
    st16(0x0BAA, 20);           /* MBarHeight */
    st32(0x020C, 0);            /* Time */
    st16(0x0220, 0);            /* MemErr */
    st8(0x0210, 0);             /* SysPPtr / misc */
    st16(0x028E, 0x3FFF);       /* ROM85: Color QuickDraw present (high bit clear) */
}

void dispatch_init();
std::string load_game_fragment(const std::vector<uint8_t> &blob);
const char *func_name(uint32_t addr);

unsigned g_log_mask = LOG_UNIMPL;
static FILE *g_log = stderr;

void logf(unsigned ch, const char *fmt, ...) {
    if (!(g_log_mask & ch)) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fflush(g_log);
}

static PPCContext *g_main_ctx = nullptr;   /* the guest's registers, for crash reports */

static void dump_trace() {
    if (g_main_ctx) {
        fprintf(stderr, "registers (as last stored):");
        for (int i = 0; i < 32; i++) fprintf(stderr, "%s r%d=%08X", i % 8 ? "" : "\n ", i, g_main_ctx->r[i]);
        fprintf(stderr, "\n  lr=%08X\n", g_main_ctx->lr);
    }
    fprintf(stderr, "recent function entries (oldest first):\n");
    for (unsigned i = 0; i < 32; i++) {
        uint32_t a = g_trace_ring[(g_trace_pos - 32 + i) & 255];
        if (a) fprintf(stderr, "  %08X %s\n", a, func_name(a));
    }
}

[[noreturn]] void fatal(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "FATAL: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    dump_trace();
    fflush(stderr);
    if (!getenv("PT_RUN_SECONDS")) {   /* players launch from Explorer and never see stderr */
        char msg[1024];
        va_start(ap, fmt);
        vsnprintf(msg, sizeof msg, fmt, ap);
        va_end(ap);
        MessageBoxA(nullptr, msg, "Prime Target", MB_OK | MB_ICONERROR);
    }
    ExitProcess(1);
}

static std::string exe_dir() {
    char buf[MAX_PATH];
    GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string s(buf);
    return s.substr(0, s.find_last_of("\\/"));
}

static std::string env_or(const char *name, const std::string &def) {
    const char *v = getenv(name);
    return v && *v ? std::string(v) : def;
}

/* UTF-8 path of the extracted game volume */
std::string data_root() {
    static std::string root;
    if (!root.empty()) return root;
    if (const char *v = getenv("PT_GAME_DIR"); v && *v) return root = v;
    wchar_t buf[MAX_PATH * 4];
    GetModuleFileNameW(nullptr, buf, MAX_PATH * 4);
    std::filesystem::path home = std::filesystem::path(buf).parent_path(), dir = home;
    /* <exe>/game, or a game folder in a parent (running from build/msvc/Release in the repo) */
    for (int i = 0; i < 5 && !dir.empty(); i++, dir = dir.parent_path()) {
        std::error_code ec;
        if (std::filesystem::is_directory(dir / "game" / "Prime Target", ec)) return root = (dir / "game").u8string();
        if (dir == dir.parent_path()) break;
    }
    /* first run: copy the game off the player's CD image into <exe>/game */
    if (!setup::import_game(home)) ExitProcess(0);
    return root = (home / "game").u8string();
}

uint32_t ticks() { return (uint32_t)(GetTickCount64() * 60 / 1000); }

/* the game's own application file (data fork) holds the PowerPC fragment */
static void load_fragment() {
    vfs::Entry e;
    if (!vfs::lookup(vfs::app_vref(), vfs::app_dir(), vfs::app_name(), e))
        fatal("The game files were not found in %s (the application \"%s\" is missing).", data_root().c_str(),
              vfs::mac_to_utf8(vfs::app_name()).c_str());
    std::vector<uint8_t> blob;
    if (FILE *f = _wfopen(e.host.c_str(), L"rb")) {
        fseek(f, 0, SEEK_END);
        blob.resize((size_t)ftell(f));
        fseek(f, 0, SEEK_SET);
        if (!blob.empty() && fread(blob.data(), 1, blob.size(), f) != blob.size()) blob.clear();
        fclose(f);
    }
    if (blob.empty()) fatal("cannot read %s", e.host.u8string().c_str());
    std::string why = load_game_fragment(blob);
    if (!why.empty()) fatal("%s: %s", e.host.u8string().c_str(), why.c_str());
}

static void parse_log_env() {
    std::string v = env_or("PT_LOG", "unimpl");
    g_log_mask = 0;
    size_t pos = 0;
    while (pos <= v.size()) {
        size_t comma = v.find(',', pos);
        std::string tok = v.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        static const struct { const char *n; unsigned m; } names[] = {
            {"hle", LOG_HLE}, {"unimpl", LOG_UNIMPL}, {"mem", LOG_MEM}, {"file", LOG_FILE}, {"res", LOG_RES},
            {"gfx", LOG_GFX}, {"snd", LOG_SND}, {"evt", LOG_EVT}, {"all", ~0u}};
        for (auto &n : names)
            if (tok == n.n) g_log_mask |= n.m;
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
}

static LONG WINAPI on_fault(EXCEPTION_POINTERS *ep) {
    auto *er = ep->ExceptionRecord;
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2) {
        uintptr_t host = (uintptr_t)er->ExceptionInformation[1];
        uintptr_t base = (uintptr_t)g_mem;
        if (host >= base && host < base + 0x100000000ull)
            fprintf(stderr, "FATAL: guest %s of unmapped address %08X\n",
                    er->ExceptionInformation[0] ? "write" : "read", (uint32_t)(host - base));
        else
            fprintf(stderr, "FATAL: host access violation at %p\n", (void *)host);
    } else {
        fprintf(stderr, "FATAL: exception %08lX\n", er->ExceptionCode);
    }
    dump_trace();
    fflush(stderr);
    ExitProcess(2);
}

}  // namespace pt

using namespace pt;

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    parse_log_env();
    AddVectoredExceptionHandler(1, on_fault);
    mem_init();
    logf(LOG_HLE | LOG_UNIMPL, "game data: %s\n", data_root().c_str());
    vfs::init();
    load_fragment();
    dispatch_init();
    hle_bind_imports();
    lowmem_init();
    res::init();
    host::init();
    init_widescreen();
    snd::init();
    start_interrupt_clock();

    static PPCContext ctx;
    PPCContext *c = &ctx;
    g_main_ctx = c;
    c->r[1] = PT_STACK_TOP - 1024;
    c->r[2] = ld32(PT_IMAGE_MAIN_TVECTOR + 4);
    ppc_call_indirect(c, ld32(PT_IMAGE_MAIN_TVECTOR));
    logf(LOG_HLE | LOG_UNIMPL, "__start returned\n");
    return 0;
}

HLE(ExitToShell) {
    (void)c;
    logf(LOG_HLE | LOG_UNIMPL, "ExitToShell\n");
    fflush(stderr);
    ExitProcess(0);
}
