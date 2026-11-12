/* Guest address space and the Mac Memory Manager.
 *
 * The whole 4 GB guest space is reserved up front; only the regions in
 * layout.h are committed, so a stray guest access faults and is reported
 * with its guest address by the exception handler in main.cpp.
 */
#include <windows.h>

#include <cstdarg>
#include <map>
#include <unordered_map>

#include "pt/runtime.h"

uint8_t *g_mem = nullptr;

namespace pt {

int16_t g_mem_err = 0;

static void commit(uint32_t base, uint32_t size) {
    if (!VirtualAlloc(g_mem + base, size, MEM_COMMIT, PAGE_READWRITE))
        fatal("cannot commit guest region %08X+%X", base, size);
}

/* ---------- heap: first-fit with coalescing, metadata kept host-side ---------- */
static std::map<uint32_t, uint32_t> g_free;            /* addr -> size */
static std::unordered_map<uint32_t, uint32_t> g_used;  /* addr -> size */
static const uint32_t kAlign = 16;

void mem_init() {
    g_mem = (uint8_t *)VirtualAlloc(nullptr, 0x100000000ull, MEM_RESERVE, PAGE_NOACCESS);
    if (!g_mem) fatal("cannot reserve 4 GB guest address space");
    commit(PT_LOWMEM_BASE, PT_LOWMEM_SIZE);
    commit(PT_CODE_BASE, PT_IMAGE_REGION_END - PT_CODE_BASE);
    commit(PT_HEAP_BASE, PT_HEAP_END - PT_HEAP_BASE);
    commit(PT_SYSTEM_BASE, PT_SYSTEM_END - PT_SYSTEM_BASE);
    commit(PT_STACK_BASE, PT_STACK_TOP - PT_STACK_BASE);
    g_free[PT_HEAP_BASE] = PT_HEAP_END - PT_HEAP_BASE;
}

uint32_t guest_alloc(uint32_t size, bool clear) {
    uint32_t need = (size + kAlign - 1) & ~(kAlign - 1);
    if (need == 0) need = kAlign;
    for (auto it = g_free.begin(); it != g_free.end(); ++it) {
        if (it->second >= need) {
            uint32_t addr = it->first, left = it->second - need;
            g_free.erase(it);
            if (left) g_free[addr + need] = left;
            g_used[addr] = need;
            if (clear) memset(g_mem + addr, 0, need);
            logf(LOG_MEM, "alloc %u -> %08X\n", size, addr);
            return addr;
        }
    }
    logf(LOG_MEM, "alloc %u FAILED\n", size);
    return 0;
}

void guest_free(uint32_t addr) {
    auto u = g_used.find(addr);
    if (u == g_used.end()) {
        logf(LOG_MEM, "free of unknown block %08X ignored\n", addr);
        return;
    }
    uint32_t size = u->second;
    g_used.erase(u);
    auto next = g_free.lower_bound(addr);
    if (next != g_free.end() && addr + size == next->first) {
        size += next->second;
        next = g_free.erase(next);
    }
    if (next != g_free.begin()) {
        auto prev = std::prev(next);
        if (prev->first + prev->second == addr) {
            prev->second += size;
            return;
        }
    }
    g_free[addr] = size;
}

uint32_t guest_block_size(uint32_t addr) {
    auto u = g_used.find(addr);
    return u == g_used.end() ? 0 : u->second;
}

bool guest_resize(uint32_t addr, uint32_t size) {
    auto u = g_used.find(addr);
    if (u == g_used.end()) return false;
    uint32_t need = (size + kAlign - 1) & ~(kAlign - 1);
    if (need == 0) need = kAlign;
    uint32_t have = u->second;
    if (need <= have) {
        if (need < have) {
            u->second = need;
            g_used[addr + need] = have - need;  /* hand the tail back through free() */
            guest_free(addr + need);
        }
        return true;
    }
    auto f = g_free.find(addr + have);
    if (f == g_free.end() || have + f->second < need) return false;
    uint32_t extra = need - have, fsize = f->second;
    g_free.erase(f);
    if (fsize > extra) g_free[addr + need] = fsize - extra;
    u->second = need;
    return true;
}

uint32_t guest_heap_free_bytes() {
    uint32_t t = 0;
    for (auto &f : g_free) t += f.second;
    return t;
}
uint32_t guest_heap_largest_free() {
    uint32_t m = 0;
    for (auto &f : g_free) m = f.second > m ? f.second : m;
    return m;
}

static uint32_t g_sys_next = PT_SYSTEM_BASE;
uint32_t sys_alloc(uint32_t size, uint32_t align) {
    g_sys_next = (g_sys_next + align - 1) & ~(align - 1);
    uint32_t a = g_sys_next;
    g_sys_next += size;
    if (g_sys_next > PT_SYSTEM_END) fatal("system region exhausted");
    memset(g_mem + a, 0, size);
    return a;
}

/* ---------- strings ---------- */
std::string read_pstr(uint32_t a) {
    if (!a) return {};
    return std::string((const char *)g_mem + a + 1, g_mem[a]);
}
std::string read_cstr(uint32_t a) {
    if (!a) return {};
    return std::string((const char *)g_mem + a);
}
void write_pstr(uint32_t a, const std::string &s) {
    size_t n = s.size() > 255 ? 255 : s.size();
    g_mem[a] = (uint8_t)n;
    memcpy(g_mem + a + 1, s.data(), n);
}
uint32_t make_pstr_guest(const std::string &s) {
    uint32_t a = sys_alloc(256, 2);
    write_pstr(a, s);
    return a;
}

/* ---------- handles ---------- */
struct HandleInfo {
    uint8_t state;  /* Mac HGetState bits: 0x80 locked, 0x40 purgeable, 0x20 resource */
};
static std::unordered_map<uint32_t, HandleInfo> g_handles;  /* master pointer addr -> info */
static std::unordered_map<uint32_t, uint32_t> g_block_owner; /* block -> master pointer */
static std::unordered_map<uint32_t, uint32_t> g_logical_size;  /* block -> requested size */

static uint32_t new_master_pointer() { return sys_alloc(4, 4); }

uint32_t handle_from_block(uint32_t block) {
    uint32_t mp = new_master_pointer();
    st32(mp, block);
    g_handles[mp] = HandleInfo{0};
    if (block) g_block_owner[block] = mp;
    return mp;
}

uint32_t handle_new(uint32_t size, bool clear) {
    uint32_t block = guest_alloc(size ? size : 1, clear);
    if (!block) return 0;
    uint32_t h = handle_from_block(block);
    g_logical_size[block] = size;  /* GetHandleSize returns what was asked for */
    logf(LOG_MEM, "NewHandle %u -> %08X (block %08X)\n", size, h, block);
    return h;
}

void handle_dispose(uint32_t h) {
    auto it = g_handles.find(h);
    if (it == g_handles.end()) return;
    uint32_t block = ld32(h);
    if (block) {
        g_block_owner.erase(block);
        g_logical_size.erase(block);
        guest_free(block);
    }
    st32(h, 0);
    g_handles.erase(it);
}

uint32_t handle_size(uint32_t h) {
    uint32_t block = ld32(h);
    if (!block) return 0;
    auto l = g_logical_size.find(block);
    return l != g_logical_size.end() ? l->second : guest_block_size(block);
}

bool handle_set_size(uint32_t h, uint32_t size) {
    uint32_t block = ld32(h);
    if (block && guest_resize(block, size)) {
        g_logical_size[block] = size;
        return true;
    }
    /* move it (a real Mac may move unlocked handles) */
    uint32_t nb = guest_alloc(size ? size : 1, false);
    if (!nb) return false;
    if (block) {
        uint32_t old = handle_size(h);
        memcpy(g_mem + nb, g_mem + block, old < size ? old : size);
        g_block_owner.erase(block);
        g_logical_size.erase(block);
        guest_free(block);
    }
    st32(h, nb);
    g_block_owner[nb] = h;
    g_logical_size[nb] = size;
    return true;
}

uint8_t handle_get_state(uint32_t h) {
    auto it = g_handles.find(h);
    return it == g_handles.end() ? 0 : it->second.state;
}
void handle_set_state(uint32_t h, uint8_t s) {
    auto it = g_handles.find(h);
    if (it != g_handles.end()) it->second.state = s;
}
uint32_t handle_recover(uint32_t block) {
    auto it = g_block_owner.find(block);
    return it == g_block_owner.end() ? 0 : it->second;
}

}  // namespace pt

/* ======================= Memory Manager HLE ======================= */
using namespace pt;

static void set_mem_err(PPCContext *, int16_t e) { g_mem_err = e; st16(0x0220, (uint16_t)e); }

HLE(NewPtr) {
    uint32_t p = guest_alloc(ARG(0), false);
    set_mem_err(c, p ? 0 : -108);
    RET(p);
}
HLE(NewPtrClear) {
    uint32_t p = guest_alloc(ARG(0), true);
    set_mem_err(c, p ? 0 : -108);
    RET(p);
}
HLE(NewPtrSysClear) {
    uint32_t p = guest_alloc(ARG(0), true);
    set_mem_err(c, p ? 0 : -108);
    RET(p);
}
HLE(DisposePtr) {
    guest_free(ARG(0));
    set_mem_err(c, 0);
}
HLE(SetPtrSize) {
    bool ok = guest_resize(ARG(0), ARG(1));
    set_mem_err(c, ok ? 0 : -108);
}
HLE(NewHandle) {
    uint32_t h = handle_new(ARG(0), false);
    set_mem_err(c, h ? 0 : -108);
    RET(h);
}
HLE(DisposeHandle) {
    handle_dispose(ARG(0));
    set_mem_err(c, 0);
}
HLE(GetHandleSize) {
    RET(ARG(0) ? handle_size(ARG(0)) : 0);
    set_mem_err(c, 0);
}
HLE(SetHandleSize) {
    bool ok = handle_set_size(ARG(0), ARG(1));
    set_mem_err(c, ok ? 0 : -108);
}
HLE(HLock) { handle_set_state(ARG(0), handle_get_state(ARG(0)) | 0x80); set_mem_err(c, 0); }
HLE(HUnlock) { handle_set_state(ARG(0), handle_get_state(ARG(0)) & ~0x80); set_mem_err(c, 0); }
HLE(HNoPurge) { handle_set_state(ARG(0), handle_get_state(ARG(0)) & ~0x40); set_mem_err(c, 0); }
HLE(HGetState) { RET((int8_t)handle_get_state(ARG(0))); set_mem_err(c, 0); }
HLE(HSetState) { handle_set_state(ARG(0), (uint8_t)ARG(1)); set_mem_err(c, 0); }
HLE(MoveHHi) { set_mem_err(c, 0); }
/* MoveHHi + HLock; blocks never move here, so it is just a lock (used by v1.2) */
HLE(HLockHi) { handle_set_state(ARG(0), handle_get_state(ARG(0)) | 0x80); set_mem_err(c, 0); }
HLE(MaxApplZone) {}
HLE(MoreMasters) {}
HLE(MemError) { RET((int16_t)g_mem_err); }
HLE(FreeMem) { RET(guest_heap_free_bytes()); }
HLE(ApplicationZone) { RET(0x00000300u); /* opaque zone pointer */ }
HLE(GetApplLimit) { RET(PT_HEAP_END); }
HLE(BlockMove) { memmove(g_mem + ARG(1), g_mem + ARG(0), ARG(2)); }
HLE(BlockMoveData) { memmove(g_mem + ARG(1), g_mem + ARG(0), ARG(2)); }
HLE(HandToHand) {
    /* pascal OSErr HandToHand(Handle *theHndl) */
    uint32_t src = ld32(ARG(0));
    uint32_t size = handle_size(src);
    uint32_t h = handle_new(size, false);
    if (!h) { RET(-108); return; }
    memcpy(g_mem + ld32(h), g_mem + ld32(src), size);
    st32(ARG(0), h);
    RET(0);
}
HLE(PtrToHand) {
    /* OSErr PtrToHand(const void *srcPtr, Handle *dstHndl, long size) */
    uint32_t h = handle_new(ARG(2), false);
    if (!h) { RET(-108); return; }
    memcpy(g_mem + ld32(h), g_mem + ARG(0), ARG(2));
    st32(ARG(1), h);
    RET(0);
}
