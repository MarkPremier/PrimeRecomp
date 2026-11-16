/* Runtime services shared by the HLE (Toolbox) modules. */
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include "pt/ppc.h"
#include "pt/layout.h"

namespace pt {

/* ---------- logging ---------- */
enum LogCh : unsigned {
    LOG_HLE = 1u << 0,      /* every Toolbox call */
    LOG_UNIMPL = 1u << 1,   /* calls with no implementation */
    LOG_MEM = 1u << 2,
    LOG_FILE = 1u << 3,
    LOG_RES = 1u << 4,
    LOG_GFX = 1u << 5,
    LOG_SND = 1u << 6,
    LOG_EVT = 1u << 7,
};
extern unsigned g_log_mask;
void logf(unsigned ch, const char *fmt, ...);
/* port option: environment variable env_name (e.g. "PT_MOUSE_SPEED"), else primetarget.ini's
 * mouse_speed beside the program; nullptr when unset (settings.cpp) */
const char *option(const char *env_name);
void set_option(const char *env_name, const char *value);   /* and save it to primetarget.ini */
/* high frame rate (interp.cpp): the in-game "Smooth Motion" setting */
bool interpolation_enabled();
void set_interpolation(bool on);
void init_widescreen();   /* widescreen.cpp: the host shows the widened 3D view beside the screen */
bool widescreen_enabled();
void set_widescreen(bool on);
int hires_lines();              /* the hires option: the 3D view's height in Full Screen, 0 = the original 480 */
void set_hires_lines(int lines);   /* saves it and rebuilds the view if a game is in progress */

/* ---------- guest memory ---------- */
void mem_init();
uint32_t guest_alloc(uint32_t size, bool clear);   /* 0 on failure */
void guest_free(uint32_t addr);
uint32_t guest_block_size(uint32_t addr);           /* 0 if not a live block */
bool guest_resize(uint32_t addr, uint32_t size);     /* in place only */
uint32_t guest_heap_free_bytes();
uint32_t guest_heap_largest_free();

/* system-region bump allocator for objects the runtime owns */
uint32_t sys_alloc(uint32_t size, uint32_t align = 4);

/* host <-> guest helpers */
inline uint8_t *gptr(uint32_t a) { return g_mem + a; }
std::string read_pstr(uint32_t a);                  /* Pascal string */
std::string read_cstr(uint32_t a);
void write_pstr(uint32_t a, const std::string &s);  /* writes length byte + chars */
uint32_t make_pstr_guest(const std::string &s);      /* allocates in system region */

/* ---------- Memory Manager state ---------- */
extern int16_t g_mem_err;
uint32_t handle_new(uint32_t size, bool clear);      /* returns handle (0 on failure) */
uint32_t handle_from_block(uint32_t block);          /* wrap an existing block */
void handle_dispose(uint32_t h);
uint32_t handle_size(uint32_t h);
bool handle_set_size(uint32_t h, uint32_t size);
uint8_t handle_get_state(uint32_t h);
void handle_set_state(uint32_t h, uint8_t s);
uint32_t handle_recover(uint32_t block);

/* ---------- HLE registry ---------- */
using HLEFunc = void (*)(PPCContext *c);
struct HLERegistrar {
    HLERegistrar(const char *name, HLEFunc fn);
};
void hle_bind_imports();
void hle_dispatch(PPCContext *c, uint32_t import_index);

/* call back into guest code (UPPs, callbacks): saves/restores the ABI state */
uint32_t guest_call(PPCContext *c, uint32_t tvector_or_code, const uint32_t *args, int nargs);

/* ---------- misc ---------- */
uint32_t ticks();          /* 60 Hz Mac ticks since start */
std::string data_root();   /* host folder holding the extracted game volume */
[[noreturn]] void fatal(const char *fmt, ...);

}  // namespace pt

/* convenience for HLE bodies */
#define ARG(n) (c->r[3 + (n)])
#define RET(v) (c->r[3] = (uint32_t)(v))

#define HLE(name)                                                         \
    static void hle_##name(PPCContext *c);                                \
    static pt::HLERegistrar hle_reg_##name(#name, hle_##name);            \
    static void hle_##name(PPCContext *c)
