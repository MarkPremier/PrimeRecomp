/* Smooth frames between the engine's 30 Hz world ticks (stage 1: the camera).
 *
 * The world, and the view in it, advances 30 times a second; the game draws once
 * per tick (idle_game_state: update_world, then render_screen if it advanced).
 * Like Aleph One, this keeps the tick rate and draws extra frames in between:
 * the camera (position, polygon, facing, pitch) of the last few ticks is kept, and
 * each frame shows it blended at a moment 1.5 ticks behind a steady 33 ms grid of
 * when the ticks were due (not when each happened to be processed, which jitters).
 * Extra frames are drawn with the tail of render_screen (render_view +
 * update_screen) and the blended camera only in world_view; the real frame runs
 * render_screen itself with the blended camera briefly in the player record.
 * Monsters and other objects still move at 30 Hz.
 *
 * Options: interpolate = 0 turns it off (the original 30 fps); max_fps caps the
 * frame rate (default: the display's refresh rate).
 */
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <vector>

#include "image_info.h"
#include "ppc_funcs.h"
#include "pt/host.h"
#include "pt/ppc.h"
#include "pt/runtime.h"

namespace pt {   /* hooks.cpp: mouse turns on their way to the world */
void mouse_ticks_simulated(int n);
bool mouse_pending(float &yaw, float &pitch);
void widescreen_update(PPCContext *c);   /* widescreen.cpp */
}

namespace {

using clock_type = std::chrono::steady_clock;

/* Prime Target v1.2 addresses (render_screen's own reads) */
constexpr uint32_t kWorldViewPtr = 0x01145918;   /* struct view_data *world_view */
constexpr uint32_t kBitmapPtr = 0x01145910;      /* bitmap the 3D view is drawn into */
constexpr uint32_t kPlayerPtr = 0x01145730;      /* current_player */
constexpr uint32_t kDynamicWorldPtr = 0x011444B4;
constexpr uint32_t kScreenMode = 0x011458F4;     /* screen_mode struct (not a pointer); +2: 0 normal, 1 Valkyrie */
constexpr uint32_t kRenderView = 0x0103177C, kUpdateScreen = 0x01041A24, kFindPolygon = 0x01014484;

/* player_data / view_data fields */
constexpr uint32_t kPlCamXY = 0x30, kPlCamZ = 0x34, kPlCamPoly = 0x36, kPlYaw = 0x3C, kPlPitch = 0x3E;
/* variables.step_phase (fixed, 0..1): the walking bob; calculate_weapon_position_for_idle sways
 * the weapon in hand by it, so it is blended too or the gun bobs at 30 Hz in a smooth view */
constexpr uint32_t kPlStepPhase = 0x144;
constexpr uint32_t kVwOriginXY = 0x18, kVwOriginZ = 0x1C, kVwYaw = 0x2A, kVwPitch = 0x2C, kVwTicks = 0x34,
                   kVwPoly = 0x38;
/* update_view_data: dtanpitch and the vertical clipping edges made from it; the trig tables */
constexpr uint32_t kVwTopEdge = 0x12, kVwBottomEdge = 0x16, kVwHalfHeight = 0x58, kVwW2sY = 0x60, kVwDtanPitch = 0x64;
constexpr uint32_t kSineTablePtr = 0x0114DB78, kCosineTablePtr = 0x0114DB7C;

/* the heartbeat: install_timer_task(30) primes its Time Manager task every 1000/30 = 33 ms */
constexpr double kTick = 0.033;
constexpr int kPitchLimit = 64;   /* the physics keeps pitch within +-64 (448..64 as angles) */

struct Camera {
    int16_t x = 0, y = 0, z = 0, poly = -1, yaw = 0, pitch = 0;
    double pitchf = 0;   /* blended frames: the pitch between whole angles (signed); pitch = its floor */
    int32_t step = 0;
    uint32_t tick = 0;
};

Camera g_prev, g_cur;
bool g_pitchf_active = false;   /* a smooth-motion frame is drawing: pt_hook_update_view_data uses g_pitchf */
double g_pitchf = 0;
int32_t g_last_dtan = 0;        /* for PT_FRAMELOG: the vertical shift the last frame drew with */
bool g_have = false;
clock_type::time_point g_tick_time, g_last_frame;

/* When was world tick k due? base + k * kTick, base tracking the earliest each tick has been
 * processed (ticks are noticed between frames, so a little late and unevenly). Blending on
 * this grid rather than on when each tick happened to be processed keeps motion even. */
double g_base = 0;
bool g_have_base = false;

double seconds(clock_type::time_point t) { return std::chrono::duration<double>(t.time_since_epoch()).count(); }

void track_tick(uint32_t tick, clock_type::time_point at, bool snap) {
    double obs = seconds(at) - tick * kTick;
    if (!g_have_base || snap || obs < g_base || obs - g_base > 2 * kTick) g_base = obs;   /* (re)lock */
    else g_base += 0.02 * (obs - g_base);                                                 /* drift */
    g_have_base = true;
}

/* how far (0..1) from the previous tick's camera to the last one to show at time `at`: one
 * and a half ticks behind the grid: ticks are processed up to ~10 ms after they are due, and
 * the half tick keeps the shown moment short of the newest tick until it arrives (without it,
 * frames held still and then jumped). Mouse turning doesn't wait for this; see blend(). */
constexpr double kDelayTicks = 1.5;

/* The world as the renderer sees it, per tick (stage 2, as Aleph One's interpolated_world):
 * platform floor/ceiling heights (polygons), the lines' adjacent heights the renderer clips
 * with, wall texture offsets that ride platforms (sides' primary y0), and object positions. */
constexpr uint32_t kPolygonsPtr = 0x011444A4, kLinesPtr = 0x0114449C, kSidesPtr = 0x011444A0, kObjectsPtr = 0x011444B0;
constexpr uint32_t kDwLineCount = 0x22, kDwSideCount = 0x24, kDwPolygonCount = 0x26;
constexpr int kMaxObjects = 384;   /* MAXIMUM_OBJECTS_PER_MAP (recalculate_map_counts); the count field is only set for saving */
constexpr uint32_t kPolyStride = 0x84, kPolyFloor = 0x30, kPolyCeiling = 0x32;
constexpr uint32_t kLineStride = 0x20, kLineFloor = 0x08, kLineCeiling = 0x0A;
constexpr uint32_t kSideStride = 0x46, kSideY0 = 0x06;
constexpr uint32_t kObjStride = 0x32, kObjFlags = 0x0E;   /* location x,y,z at +0; flags: 0x8000 used, & 7 owner */
constexpr int kOwnerProjectile = 3;
constexpr int kSpeedLimit = 512, kProjectileSpeedLimit = 1024;   /* world units per tick, as Aleph One */

struct WorldSnap {
    std::vector<int16_t> poly, line, side, obj;   /* poly/line: 2 per entry; obj: x, y, z, flags */
};

void snapshot(WorldSnap &w) {
    using namespace pt;
    uint32_t dw = ld32(kDynamicWorldPtr);
    auto grab = [](std::vector<int16_t> &out, uint32_t base_ptr, int count, uint32_t stride,
                   std::initializer_list<uint32_t> fields) {
        out.clear();
        uint32_t base = ld32(base_ptr);
        if (!base || count <= 0 || count > 32768) return;
        out.reserve(count * fields.size());
        for (int i = 0; i < count; i++)
            for (uint32_t f : fields) out.push_back((int16_t)ld16(base + i * stride + f));
    };
    grab(w.poly, kPolygonsPtr, (int16_t)ld16(dw + kDwPolygonCount), kPolyStride, {kPolyFloor, kPolyCeiling});
    grab(w.line, kLinesPtr, (int16_t)ld16(dw + kDwLineCount), kLineStride, {kLineFloor, kLineCeiling});
    grab(w.side, kSidesPtr, (int16_t)ld16(dw + kDwSideCount), kSideStride, {kSideY0});
    grab(w.obj, kObjectsPtr, kMaxObjects, kObjStride, {0, 2, 4, kObjFlags});
}

/* the cameras (and worlds) of the last few ticks, oldest first */
Camera g_hist[4];
WorldSnap g_whist[4];
int g_nhist = 0;

void remember(const Camera &cam, bool snap) {
    if (snap) g_nhist = 0;
    if (g_nhist == 4) {
        for (int i = 0; i < 3; i++) g_hist[i] = g_hist[i + 1];
        std::rotate(g_whist, g_whist + 1, g_whist + 4);
        g_nhist = 3;
    }
    snapshot(g_whist[g_nhist]);
    if (snap) {
        const WorldSnap &w = g_whist[g_nhist];
        int used = 0;
        for (size_t i = 0; i + 3 < w.obj.size(); i += 4) used += ((uint16_t)w.obj[i + 3] & 0x8000) != 0;
        pt::logf(pt::LOG_GFX, "interp: world %zu polygons, %zu lines, %zu sides, %d of %zu objects in use\n",
                 w.poly.size() / 2, w.line.size() / 2, w.side.size(), used, w.obj.size() / 4);
    }
    g_hist[g_nhist++] = cam;
}

/* the two remembered ticks either side of the moment to show at `at`, and how far between */
void bracket(clock_type::time_point at, int &ia, int &ib, double &f) {
    double shown = (seconds(at) - g_base) / kTick - kDelayTicks;   /* in ticks */
    ia = ib = 0;
    f = 1.0;
    for (int i = g_nhist - 2; i >= 0; i--)
        if (shown >= g_hist[i].tick || i == 0) {
            ia = i;
            ib = i + 1;
            f = (shown - g_hist[i].tick) / (double)(g_hist[i + 1].tick - g_hist[i].tick);
            f = f < 0 ? 0 : f > 1 ? 1 : f;
            return;
        }
}
void bracket(clock_type::time_point at, Camera &a, Camera &b, double &f) {
    int ia, ib;
    bracket(at, ia, ib, f);
    a = g_hist[ia];
    b = g_hist[ib];
}

/* Put the world blended at `at` into guest memory for one frame; restore_world() undoes it.
 * Only values that differ from the live (newest) state are written. */
struct Undo { uint32_t addr; uint16_t value; };
std::vector<Undo> g_undo;

void put16(uint32_t addr, int v) {
    g_undo.push_back({addr, ld16(addr)});
    st16(addr, (uint16_t)(int16_t)v);
}

void blend_world(clock_type::time_point at) {
    using namespace pt;
    g_undo.clear();
    if (g_nhist < 2) return;
    int ia, ib;
    double t;
    bracket(at, ia, ib, t);
    const WorldSnap &A = g_whist[ia], &B = g_whist[ib], &L = g_whist[g_nhist - 1];
    auto lerp16 = [](int a, int b, double f) { return a + (int)lround((b - a) * f); };
    auto heights = [&](const std::vector<int16_t> &a, const std::vector<int16_t> &b, const std::vector<int16_t> &l,
                       uint32_t base_ptr, uint32_t stride, std::initializer_list<uint32_t> fields) {
        size_t nf = fields.size();
        if (a.size() != l.size() || b.size() != l.size() || !nf) return;
        uint32_t base = ld32(base_ptr);
        if (!base) return;
        for (size_t i = 0; i < l.size(); i++) {
            if (a[i] == l[i] && b[i] == l[i]) continue;
            int v = lerp16(a[i], b[i], t);
            if (v != l[i]) put16(base + (uint32_t)(i / nf) * stride + *(fields.begin() + i % nf), v);
        }
    };
    heights(A.poly, B.poly, L.poly, kPolygonsPtr, kPolyStride, {kPolyFloor, kPolyCeiling});
    heights(A.line, B.line, L.line, kLinesPtr, kLineStride, {kLineFloor, kLineCeiling});
    heights(A.side, B.side, L.side, kSidesPtr, kSideStride, {kSideY0});
    /* objects: those in all three ticks, moving no faster than a limit (else they teleported,
     * or the slot was reused); their polygon stays the live one, as in Aleph One */
    if (A.obj.size() == L.obj.size() && B.obj.size() == L.obj.size()) {
        uint32_t base = ld32(kObjectsPtr);
        for (size_t i = 0; base && i + 3 < L.obj.size(); i += 4) {
            uint16_t fa = (uint16_t)A.obj[i + 3], fb = (uint16_t)B.obj[i + 3], fl = (uint16_t)L.obj[i + 3];
            if (!(fa & fb & fl & 0x8000) || (fa & 7) != (fl & 7) || (fb & 7) != (fl & 7)) continue;
            if (A.obj[i] == L.obj[i] && A.obj[i + 1] == L.obj[i + 1] && A.obj[i + 2] == L.obj[i + 2] &&
                B.obj[i] == L.obj[i] && B.obj[i + 1] == L.obj[i + 1] && B.obj[i + 2] == L.obj[i + 2])
                continue;   /* not moving */
            int limit = (fl & 7) == kOwnerProjectile ? kProjectileSpeedLimit : kSpeedLimit;
            auto far = [&](const std::vector<int16_t> &p, const std::vector<int16_t> &q, int lim) {
                return std::abs(p[i] - q[i]) + std::abs(p[i + 1] - q[i + 1]) > lim || std::abs(p[i + 2] - q[i + 2]) > lim;
            };
            if (far(A.obj, B.obj, limit) || far(B.obj, L.obj, limit * 3)) continue;
            uint32_t o = base + (uint32_t)(i / 4) * kObjStride;
            for (int k = 0; k < 3; k++) {
                int v = lerp16(A.obj[i + k], B.obj[i + k], t);
                if (v != L.obj[i + k]) put16(o + 2 * k, v);
            }
        }
    }
}

size_t g_blended_values = 0;   /* for the log: world values blended in the last frame */

void restore_world() {
    g_blended_values += g_undo.size();
    for (auto it = g_undo.rbegin(); it != g_undo.rend(); ++it) st16(it->addr, it->value);
    g_undo.clear();
}
uint32_t g_tv_render_view, g_tv_update_screen, g_tv_find_polygon, g_points;

int g_enabled = -1;   /* from the interpolate option; Preferences > Graphics > Smooth Motion */

bool enabled() {
    if (g_enabled < 0) {
        const char *v = pt::option("PT_INTERPOLATE");
        g_enabled = !(v && *v == '0');
    }
    return g_enabled != 0;
}

double frame_interval() {
    static const double iv = [] {
        const char *v = pt::option("PT_MAX_FPS");
        int fps = v ? atoi(v) : 0;
        if (fps <= 0) fps = pt::host::refresh_rate();
        if (fps < 31) fps = 60;
        return 1.0 / fps;
    }();
    return iv;
}

uint32_t tvector(uint32_t code) {
    uint32_t tv = pt::sys_alloc(8);
    st32(tv, code);
    st32(tv + 4, PT_IMAGE_TOC);
    return tv;
}

Camera read_camera() {
    using namespace pt;
    uint32_t p = ld32(kPlayerPtr);
    Camera c;
    c.x = (int16_t)ld16(p + kPlCamXY);
    c.y = (int16_t)ld16(p + kPlCamXY + 2);
    c.z = (int16_t)ld16(p + kPlCamZ);
    c.poly = (int16_t)ld16(p + kPlCamPoly);
    c.yaw = (int16_t)ld16(p + kPlYaw);
    c.pitch = (int16_t)ld16(p + kPlPitch);
    c.step = (int32_t)ld32(p + kPlStepPhase);
    c.tick = ld32(ld32(kDynamicWorldPtr));
    return c;
}

/* PT_FRAMELOG=file: one line per drawn frame (time shown, time presented, camera) for checking pacing */
void frame_log(char kind, clock_type::time_point at, const Camera &cam) {
    static FILE *f = [] { const char *p = getenv("PT_FRAMELOG"); return p ? fopen(p, "w") : nullptr; }();
    if (!f) return;
    fprintf(f, "%c %.5f %.5f %d %d %d %d %u %d %d\n", kind, seconds(at), seconds(clock_type::now()), cam.x, cam.y, cam.yaw,
            cam.pitch, g_cur.tick, cam.step, g_last_dtan);
}

int lerp(int a, int b, double t) { return a + (int)((b - a) * t + (b >= a ? 0.5 : -0.5)); }

/* the step phase goes round 0..0x10000; blend across the wrap, and don't blend a jump */
int32_t lerp_phase(int32_t a, int32_t b, double t) {
    if (a < 0 || a > 0x10000 || b < 0 || b > 0x10000) return b;
    int32_t d = b - a;
    if (d < -0x8000) d += 0x10000;
    if (d > 0x8000) d -= 0x10000;
    if (d > 0x4000 || d < -0x4000) return b;
    return (int32_t)(a + (int32_t)lround(d * t)) & 0xFFFF;
}

/* angles (facing, and pitch: 504 is 8 below level) are 0..511; blend the short way round */
int lerp_angle(int a, int b, double t) {
    if (a < 0 || a >= 512 || b < 0 || b >= 512) return b;
    int d = b - a;
    if (d > 256) d -= 512;
    if (d < -256) d += 512;
    return (a + (int)(d * t + (d >= 0 ? 0.5 : -0.5))) & 511;
}

}  // namespace

namespace {

void ensure_tvectors() {
    if (g_tv_render_view) return;
    g_tv_render_view = tvector(kRenderView);
    g_tv_update_screen = tvector(kUpdateScreen);
    g_tv_find_polygon = tvector(kFindPolygon);
    g_points = pt::sys_alloc(8);
}

/* The camera to show at time `at`. Every frame, the real one drawn as a tick arrives and those
 * between, comes from here, so the view moves steadily instead of jumping ahead and back. */
Camera blend(PPCContext *c, clock_type::time_point at) {
    using namespace pt;
    Camera a, b;
    double t;
    bracket(at, a, b, t);
    Camera cam = b;
    cam.x = (int16_t)lerp(a.x, b.x, t);
    cam.y = (int16_t)lerp(a.y, b.y, t);
    cam.z = (int16_t)lerp(a.z, b.z, t);
    cam.step = lerp_phase(a.step, b.step, t);
    /* Mouse look isn't blended (that would show it late): show the facing and pitch the world
     * will reach once it runs the mouse movement already on its way. Keys blend. */
    float pend_yaw, pend_pitch;
    if (mouse_pending(pend_yaw, pend_pitch) && g_cur.yaw >= 0 && g_cur.yaw < 512) {
        cam.yaw = (int16_t)((g_cur.yaw + (int)lroundf(pend_yaw)) & 511);
        double p = (g_cur.pitch >= 256 ? g_cur.pitch - 512 : g_cur.pitch) + (double)pend_pitch;   /* 504: 8 below level */
        cam.pitchf = p < -kPitchLimit ? -kPitchLimit : p > kPitchLimit ? kPitchLimit : p;
    } else {
        cam.yaw = (int16_t)lerp_angle(a.yaw, b.yaw, t);
        auto signed_pitch = [](int v) { return v >= 256 ? v - 512 : v; };   /* an angle: just below level is 511 */
        int pa = signed_pitch(a.pitch), pb = signed_pitch(b.pitch);
        cam.pitchf = pa + (pb - pa) * t;
    }
    cam.pitch = (int16_t)((int)std::floor(cam.pitchf) & 511);   /* the rest is drawn by pt_hook_update_view_data */
    /* the renderer starts from the polygon holding the eye */
    if (cam.x == a.x && cam.y == a.y) {
        cam.poly = a.poly;
    } else if (cam.x != b.x || cam.y != b.y) {
        ensure_tvectors();
        st16(g_points, (uint16_t)b.x);
        st16(g_points + 2, (uint16_t)b.y);
        st16(g_points + 4, (uint16_t)cam.x);
        st16(g_points + 6, (uint16_t)cam.y);
        uint32_t args[3] = {g_points, g_points + 4, (uint32_t)(int32_t)b.poly};
        int16_t poly = (int16_t)guest_call(c, g_tv_find_polygon, args, 3);
        if (poly < 0) { cam.x = b.x; cam.y = b.y; cam.z = b.z; cam.poly = b.poly; }
        else cam.poly = poly;
    }
    return cam;
}

}  // namespace

namespace pt {
bool interpolation_enabled() { return enabled(); }
void set_interpolation(bool on) {
    g_enabled = on;
    set_option("PT_INTERPOLATE", on ? "1" : "0");
    logf(LOG_GFX, "interp: smooth motion %s\n", on ? "on" : "off");
}
}  // namespace pt

/* every render_screen. After a world tick: remember where the camera was and is, and draw
 * this real frame with the blended camera put in the player record for the call. */
extern "C" int pt_hook_render_screen(PPCContext *c) {
    using namespace pt;
    uint32_t p = ld32(kPlayerPtr);
    if ((int16_t)c->r[3] <= 0 || !p) return 0;
    Camera now = read_camera();
    /* a jump (teleport, new level) starts the history again rather than blending across it:
     * faster than Aleph One's half a world unit a tick */
    int dt = (int)(now.tick - g_cur.tick);
    bool snap = !g_have || now.tick < g_cur.tick || dt > 4 ||
                std::abs(now.x - g_cur.x) + std::abs(now.y - g_cur.y) > kSpeedLimit * dt ||
                std::abs(now.z - g_cur.z) > kSpeedLimit * dt;
    mouse_ticks_simulated((int16_t)c->r[3]);
    g_prev = snap ? now : g_cur;
    g_cur = now;
    g_have = true;
    g_tick_time = g_last_frame = clock_type::now();
    track_tick(now.tick, g_tick_time, snap);
    remember(now, snap);
    if (!enabled()) return 0;

    host::pump();   /* the newest mouse motion, as for the frames between */
    Camera cam = blend(c, g_tick_time);
    uint8_t saved[0x10];
    memcpy(saved, g_mem + p + kPlCamXY, sizeof saved);   /* +0x30 .. +0x3F */
    st16(p + kPlCamXY, (uint16_t)cam.x);
    st16(p + kPlCamXY + 2, (uint16_t)cam.y);
    st16(p + kPlCamZ, (uint16_t)cam.z);
    st16(p + kPlCamPoly, (uint16_t)cam.poly);
    st16(p + kPlYaw, (uint16_t)cam.yaw);
    st16(p + kPlPitch, (uint16_t)cam.pitch);
    uint32_t step = ld32(p + kPlStepPhase);
    st32(p + kPlStepPhase, (uint32_t)cam.step);
    blend_world(g_tick_time);
    g_pitchf_active = true;
    g_pitchf = cam.pitchf;
    fn_010403D8_orig(c);
    g_pitchf_active = false;
    restore_world();
    frame_log('R', g_tick_time, cam);
    memcpy(g_mem + p + kPlCamXY, saved, sizeof saved);
    st32(p + kPlStepPhase, step);
    return 1;
}

/* update_view_data(view), at the start of render_view: the renderer's one use of the pitch is
 * here, as dtanpitch = world_to_screen_y * sine[pitch] / cosine[pitch] (+0x64) and the vertical
 * clipping edges made from it (+0x12, +0x16). The pitch is a whole angle (0.7 degrees: about 5
 * lines on a 480-line view), so slow looking up or down moved in steps. For smooth-motion frames
 * the shift is taken between the two whole pitches either side, from the game's own tables, so
 * whole pitches draw exactly as before. */
extern "C" int pt_hook_update_view_data(PPCContext *c) {
    using namespace pt;
    uint32_t v = c->r[3];
    fn_01031AD4_orig(c);
    g_last_dtan = (int32_t)ld32(v + kVwDtanPitch);
    if (!g_pitchf_active || v != ld32(kWorldViewPtr)) return 1;
    int n = (int)std::floor(g_pitchf);
    double f = g_pitchf - n;
    if (f < 1e-4 || (int16_t)ld16(v + kVwPitch) != (int16_t)(n & 511)) return 1;
    uint32_t sine = ld32(kSineTablePtr), cosine = ld32(kCosineTablePtr);
    int32_t w2s = (int32_t)ld32(v + kVwW2sY);
    auto shift = [&](int angle) {   /* as update_view_data computes it (divw truncates) */
        angle &= 511;
        int32_t s = (int32_t)ld32(sine + 4 * angle), co = (int32_t)ld32(cosine + 4 * angle);
        return co ? (w2s * s) / co : 0;
    };
    int32_t d0 = shift(n), d1 = shift(n + 1);
    if ((int32_t)ld32(v + kVwDtanPitch) != d0) return 1;   /* not the pitch we set: leave it */
    int32_t d = (int32_t)lround(d0 + (d1 - d0) * f);
    int32_t half_h = (int32_t)ld32(v + kVwHalfHeight);
    st32(v + kVwDtanPitch, (uint32_t)d);
    g_last_dtan = d;
    st16(v + kVwTopEdge, (uint16_t)(int16_t)(-(half_h + d)));
    st16(v + kVwBottomEdge, (uint16_t)(int16_t)(-half_h + d));
    return 1;
}

/* top of the main loop's idle routine: draw a blended frame if one is due */
extern "C" int pt_hook_idle_game_state(PPCContext *c) {
    using namespace pt;
    {   /* Window Size "Full Screen" (screen_mode.size 0): on the Mac the view then filled the
         * monitor, so the window fills it too. Other sizes leave the window alone (shrinking the
         * view with the F-keys shouldn't leave full screen); Alt+Enter switches. */
        static int last = -99;
        int size = (int16_t)ld16(kScreenMode);
        if (size != last) {
            if (size == 0) host::set_fullscreen(true);
            last = size;
        }
    }
    widescreen_update(c);
    if (!enabled() || !g_have) return 0;
    auto now = clock_type::now();
    double since_tick = std::chrono::duration<double>(now - g_tick_time).count();
    double since_frame = std::chrono::duration<double>(now - g_last_frame).count();
    if (since_tick > 2 * kTick) return 0;            /* paused, in a menu, or between levels */
    if (since_frame < frame_interval()) return 0;
    uint32_t wv = ld32(kWorldViewPtr), bitmap = ld32(kBitmapPtr);
    if (!wv || !bitmap || ld16(kScreenMode + 2) != 0) return 0;   /* normal display only */
    ensure_tvectors();
    host::pump();   /* take in the newest mouse motion: frames wait on vsync, not on the event pump */
    Camera cam = blend(c, now);

    /* draw with the blended camera, then put world_view back as render_screen left it */
    uint8_t saved[0x3C];
    memcpy(saved, g_mem + wv, sizeof saved);
    st16(wv + kVwOriginXY, (uint16_t)cam.x);
    st16(wv + kVwOriginXY + 2, (uint16_t)cam.y);
    st16(wv + kVwOriginZ, (uint16_t)cam.z);
    st32(wv + kVwPoly, (uint32_t)(int32_t)cam.poly);
    st16(wv + kVwYaw, (uint16_t)cam.yaw);
    st16(wv + kVwPitch, (uint16_t)cam.pitch);
    st32(wv + kVwTicks, 0);
    uint32_t p = ld32(kPlayerPtr), step = ld32(p + kPlStepPhase);
    st32(p + kPlStepPhase, (uint32_t)cam.step);   /* the weapon's bob */
    blend_world(now);   /* platforms and objects */
    g_pitchf_active = true;
    g_pitchf = cam.pitchf;
    uint32_t args[2] = {wv, bitmap};
    guest_call(c, g_tv_render_view, args, 2);
    g_pitchf_active = false;
    guest_call(c, g_tv_update_screen, nullptr, 0);
    restore_world();
    frame_log('B', now, cam);
    memcpy(g_mem + wv, saved, sizeof saved);
    st32(p + kPlStepPhase, step);
    g_last_frame = now;
    static unsigned frames = 0;
    static uint32_t first_tick = g_cur.tick;
    if (++frames % 300 == 0)
        logf(LOG_GFX, "interp: %u blended frames over %u ticks; camera %d,%d,%d poly %d yaw %d pitch %d; %zu world values\n",
             frames, g_cur.tick - first_tick, g_cur.x, g_cur.y, g_cur.z, g_cur.poly, g_cur.yaw, g_cur.pitch,
             g_blended_values);
        if (frames % 300 == 0) g_blended_values = 0;
    return 0;
}
