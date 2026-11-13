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
float g_speed = 1.0f, g_yaw_acc = 0, g_pitch_acc = 0;
bool g_view_mode = false;
/* turns sent with each tick's action flags that the world has not run yet (interp.cpp) */
std::deque<int> g_unsimulated;
int g_last_simulated = 0;
}  // namespace

namespace pt {
/* the world ran n ticks: their queued mouse turns are now part of the player's facing */
void mouse_ticks_simulated(int n) {
    while (n-- > 0 && !g_unsimulated.empty()) {
        g_last_simulated = g_unsimulated.front();
        g_unsimulated.pop_front();
    }
}
/* For frames between ticks: the mouse turn (angle units) that is on its way but not yet in
 * the player's facing. False when the mouse isn't turning, so the frame blends instead. */
bool mouse_pending_yaw(float &units) {
    if (!g_view_mode) return false;
    int dx, dy;
    host::peek_mouse_delta(dx, dy);
    units = g_yaw_acc + dx * g_speed;
    for (int y : g_unsimulated) units += (float)y;
    return units != 0 || g_last_simulated != 0;
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

    static int yaw = 0, pitch = 0;
    static bool fire = false;
    uint32_t now = ticks();
    if ((int32_t)ld32(kLastTick) < (int32_t)now) {   /* once per tick, as the original */
        st32(kLastTick, now);
        int dx, dy;
        host::take_mouse_delta(dx, dy);
        g_yaw_acc += dx * speed;                        /* angle units (512 per turn) per pixel */
        if (!lock_y) g_pitch_acc += -dy * speed * 0.5f * invert;   /* mouse forward (dy < 0) looks up */
        yaw = take(g_yaw_acc, -64, 63);
        pitch = take(g_pitch_acc, -16, 15);
        fire = host::mouse_down();
        g_unsimulated.push_back(yaw);
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
