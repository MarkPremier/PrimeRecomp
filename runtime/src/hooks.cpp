/* Native replacements for game functions (tools/recomp.py HOOKS).
 *
 * pt_hook_test_mouse: Prime Target's "mouse controls view" turned mouse movement into
 * key presses (full-speed turn or nothing, 30 times a second). Marathon 2's engine
 * underneath still understands its own smooth mouse look: absolute yaw (action flag
 * bit 0, the turn in bits 1..7) and absolute pitch (bit 8, bits 9..13; its decoding is
 * fixed by a patch in recomp.py). This sends the mouse's actual motion through those.
 * "Mouse controls movement" keeps the original code. PT_ORIGINAL_MOUSE=1 keeps it for
 * both modes; PT_MOUSE_SPEED scales turning and looking (default 1); PT_MOUSE_INVERT=1
 * makes pushing the mouse forward look down.
 */
#include <cmath>
#include <cstdlib>

#include "pt/host.h"
#include "pt/ppc.h"
#include "pt/runtime.h"

namespace {

constexpr uint32_t kLastTick = 0x01100D64;   /* test_mouse's globals (TOC-0x729C/-0x7298/-0x7294) */
constexpr uint32_t kMode = 0x01100D68;       /* 1: mouse controls view */
constexpr uint32_t kFlags = 0x01100D6C;
constexpr uint32_t kFire = 0x00100000;       /* left trigger, as the original sets on Button() */
constexpr uint32_t kYawBits = 0x000000FE, kPitchBits = 0x00003E00;

float env_float(const char *name, float def) {
    const char *v = getenv(name);
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

extern "C" int pt_hook_test_mouse(PPCContext *c) {
    using namespace pt;
    static const bool original = getenv("PT_ORIGINAL_MOUSE") && *getenv("PT_ORIGINAL_MOUSE") == '1';
    static const float speed = env_float("PT_MOUSE_SPEED", 1.0f);
    static const float invert = getenv("PT_MOUSE_INVERT") && *getenv("PT_MOUSE_INVERT") == '1' ? -1.0f : 1.0f;
    if (original || (int16_t)ld16(kMode) != 1) return 0;

    static float yaw_acc = 0, pitch_acc = 0;
    uint32_t now = ticks();
    if ((int32_t)ld32(kLastTick) < (int32_t)now) {   /* once per tick, as the original */
        st32(kLastTick, now);
        int dx, dy;
        host::take_mouse_delta(dx, dy);
        yaw_acc += dx * speed;                        /* angle units (512 per turn) per pixel */
        pitch_acc += -dy * speed * 0.5f * invert;     /* mouse forward (dy < 0) looks up */
        uint32_t flags = 0;
        int yaw = take(yaw_acc, -64, 63);
        int pitch = take(pitch_acc, -16, 15);
        /* Both axes together, as Marathon 2's mouse did: a turn without an absolute pitch
         * sends the physics down its keyboard path, which levels the view. With the mouse
         * still, nothing is set and the keys work as usual. */
        if (yaw || pitch) flags |= 1u | (uint32_t)(yaw + 64) << 1 | 0x100u | (uint32_t)(pitch + 16) << 9;
        if (host::mouse_down()) flags |= kFire;
        st32(kFlags, flags);
    }
    /* merge into the keyboard's flags; an absolute turn or look replaces the keys' bits */
    uint32_t f = ld32(kFlags), af = c->r[3], cur = ld32(af);
    if (f & 1) cur &= ~kYawBits;
    if (f & 0x100) cur &= ~kPitchBits;
    st32(af, cur | f);
    return 1;
}
