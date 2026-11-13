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
    pitch = (g_pitch_acc - dy * g_pitch_scale) * 0.5f;
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
        g_pitch_acc += -dy * g_pitch_scale;             /* mouse forward (dy < 0) looks up */
        yaw = take(g_yaw_acc, -64, 63);
        pitch = take(g_pitch_acc, -16, 15);
        fire = host::mouse_down();
        g_unsimulated.push_back({yaw, pitch >> 1});
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
