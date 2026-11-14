/* Native replacements for game functions (tools/recomp.py HOOKS).
 *
 * pt_hook_test_mouse: Prime Target's "mouse controls view" turned mouse movement into
 * key presses (full-speed turn or nothing, 30 times a second). Marathon 2's engine
 * underneath still understands its own smooth mouse look: absolute yaw (action flag
 * bit 0, the turn in bits 1..7) and absolute pitch (bit 8, bits 9..13; its decoding is
 * fixed by a patch in recomp.py). This sends the mouse's actual motion through those.
 * "Mouse controls movement" keeps the original code. Options (primetarget.ini or PT_*
 * environment variables): original_mouse = 1 keeps the original for both modes;
 * mouse_speed scales turning and looking (default 1); mouse_invert = 1 makes pushing the
 * mouse forward look down; mouse_lock_y = 1 leaves looking up and down to the keys.
 */
#include <cmath>
#include <deque>
#include <cstdlib>

#include "pt/host.h"
#include "pt/ppc.h"
#include "pt/runtime.h"

namespace {

constexpr uint32_t kLastTick = 0x01100D64;   /* test_mouse's globals (TOC-0x729C/-0x7298/-0x7294) */
constexpr uint32_t kMode = 0x01100D68;       /* 1: mouse controls view */
constexpr uint32_t kFire = 0x00100000;       /* left trigger, as the original sets on Button() */
constexpr uint32_t kYawBits = 0x000000FE, kPitchBits = 0x00003E00;

float env_float(const char *name, float def) {
    const char *v = pt::option(name);
    return v && *v ? (float)atof(v) : def;
}

/* whole units to send this tick; the remainder carries over so slow movement still turns */
int take(float &acc, int lo, int hi) {
    int n = (int)acc;
    if (n < lo) n = lo;
    if (n > hi) n = hi;
    acc -= (float)n;
    if (acc > 4 * hi) acc = 4.0f * hi;   /* don't let a huge flick run on for long */
    if (acc < 4 * lo) acc = 4.0f * lo;
    return n;
}

}  // namespace

static bool option_on(const char *name) {
    const char *v = pt::option(name);
    return v && *v == '1';
}

namespace {
float g_speed = 1.0f, g_yaw_acc = 0, g_pitch_acc = 0, g_pitch_scale = 0.5f;
bool g_view_mode = false;
/* Mouse turns sent with each tick's action flags that the world has not run yet (interp.cpp):
 * facing change, and pitch change (the physics applies an absolute pitch p as p >> 1). */
struct Sent { int yaw, pitch; };
std::deque<Sent> g_unsimulated;
bool g_last_moved = false;
}  // namespace

namespace pt {
/* the world ran n ticks: their queued mouse turns are now part of the player's facing */
void mouse_ticks_simulated(int n) {
    while (n-- > 0 && !g_unsimulated.empty()) {
        g_last_moved = g_unsimulated.front().yaw || g_unsimulated.front().pitch;
        g_unsimulated.pop_front();
    }
}
/* For frames between ticks: the mouse turn and look (angle units) on their way but not yet in
 * the player's facing and pitch. False when the mouse isn't looking, so the frame blends. */
bool mouse_pending(float &yaw, float &pitch) {
    if (!g_view_mode) return false;
    int dx, dy;
    host::peek_mouse_delta(dx, dy);
    yaw = g_yaw_acc + dx * g_speed;
    pitch = g_pitch_acc - dy * g_pitch_scale * 0.5f;
    for (auto &s : g_unsimulated) {
        yaw += (float)s.yaw;
        pitch += (float)s.pitch;
    }
    return yaw != 0 || pitch != 0 || g_last_moved;
}
}  // namespace pt

extern "C" int pt_hook_test_mouse(PPCContext *c) {
    using namespace pt;
    static const bool original = option_on("PT_ORIGINAL_MOUSE");
    static const bool lock_y = option_on("PT_MOUSE_LOCK_Y");
    static const float speed = g_speed = env_float("PT_MOUSE_SPEED", 1.0f);
    static const float invert = option_on("PT_MOUSE_INVERT") ? -1.0f : 1.0f;
    g_view_mode = !original && (int16_t)ld16(kMode) == 1;
    if (!g_view_mode) {
        g_unsimulated.clear();
        return 0;
    }
    /* Mouse movement made while the game wasn't reading the mouse (menus, Preferences, loading,
     * the prologue) doesn't count: sent at once it turned the player half round at the start of
     * a level. The original recentred the pointer on entering a level to the same effect. */
    static uint32_t last_call = 0;
    uint32_t t_now = ticks();
    if (!last_call || t_now - last_call > 15) {   /* over a quarter of a second (ticks are 1/60 s) */
        int dx, dy;
        host::take_mouse_delta(dx, dy);
        g_yaw_acc = g_pitch_acc = 0;
        g_unsimulated.clear();
    }
    last_call = t_now;

    static int yaw = 0, pitch = 0;
    static bool fire = false;
    uint32_t now = ticks();
    if ((int32_t)ld32(kLastTick) < (int32_t)now) {   /* once per tick, as the original */
        st32(kLastTick, now);
        int dx, dy;
        host::take_mouse_delta(dx, dy);
        g_pitch_scale = lock_y ? 0.0f : speed * 0.5f * invert;
        g_yaw_acc += dx * speed;                        /* angle units (512 per turn) per pixel */
        /* Pitch in the player's own units: the physics applies the sent value p as p >> 1, so
         * send twice a whole number. Sending odd values lost the half (+1 became 0, -1 became
         * -1): slow looks up stalled while the frames between ticks showed the half and took it
         * back, a vertical judder, and up and down ran at different speeds. */
        g_pitch_acc += -dy * g_pitch_scale * 0.5f;      /* mouse forward (dy < 0) looks up */
        yaw = take(g_yaw_acc, -64, 63);
        int look = take(g_pitch_acc, -8, 7);
        pitch = 2 * look;
        fire = host::mouse_down();
        g_unsimulated.push_back({yaw, look});
        if (g_unsimulated.size() > 4) g_unsimulated.pop_front();   /* never let it drift far */
    }
    /* Merge into the keyboard's flags. A mouse turn replaces the turn keys' bits, and sends an
     * absolute pitch too (Marathon 2's mouse did): a turn alone puts the physics on its keyboard
     * path, which levels the view. When the mouse isn't looking and look keys are held, the keys
     * keep the pitch. */
    uint32_t af = c->r[3], cur = ld32(af), f = fire ? kFire : 0;
    bool keys_look = (cur & 0x0E00) != 0;   /* look up / down / ahead */
    if (yaw || pitch) {
        cur &= ~kYawBits;
        f |= 1u | (uint32_t)(yaw + 64) << 1;
        if (pitch || !keys_look) {
            cur &= ~kPitchBits;
            f |= 0x100u | (uint32_t)(pitch + 16) << 9;
        }
    }
    st32(af, cur | f);
    return 1;
}

/* ---------- per-level revisit records ----------
 * Prime Target remembers the objects of each level you leave (SaveLevel) and puts them back when
 * you return (goto_level -> PlaceRevisitedLevelObjects). The records are a static array of 28,
 * one per campaign level (0x24B4 bytes each, from TOC-0x3F3C), but the Map has 40 levels: the
 * last 12 are network maps the vidmaster's level select can still start. For those the game read
 * other variables as a record (level 29 placed a monster of type 0x9BDA and crashed) and would
 * save over them on leaving. Levels past the array place their objects fresh and aren't saved. */
#include "image_info.h"
#include "ppc_funcs.h"

namespace {
constexpr uint32_t kRevisitBase = PT_IMAGE_TOC - 0x3F3C, kRevisitSize = 0x24B4;
constexpr int kRevisitRecords = 28;
}

/* PlaceRevisitedLevelObjects(record *) */
extern "C" int pt_hook_place_revisited(PPCContext *c) {
    uint32_t rec = c->r[3];
    if (rec >= kRevisitBase && (rec - kRevisitBase) / kRevisitSize < (uint32_t)kRevisitRecords) return 0;
    pt::logf(pt::LOG_GFX, "level beyond the revisit records: placing its objects afresh\n");
    fn_010269A0(c);   /* place_initial_objects */
    return 1;
}

/* SaveLevel(short level_index) */
extern "C" int pt_hook_save_level(PPCContext *c) {
    int16_t level = (int16_t)c->r[3];
    return level < 0 || level >= kRevisitRecords;   /* skip: no record to keep it in */
}
