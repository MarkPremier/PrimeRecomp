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
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "image_info.h"
#include "ppc_funcs.h"
#include "pt/host.h"
#include "pt/ppc.h"
#include "pt/runtime.h"

namespace pt {   /* hooks.cpp: mouse turns on their way to the world */
void mouse_ticks_simulated(int n);
bool mouse_pending_yaw(float &units);
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
constexpr uint32_t kVwOriginXY = 0x18, kVwOriginZ = 0x1C, kVwYaw = 0x2A, kVwPitch = 0x2C, kVwTicks = 0x34,
                   kVwPoly = 0x38;

/* the heartbeat: install_timer_task(30) primes its Time Manager task every 1000/30 = 33 ms */
constexpr double kTick = 0.033;

struct Camera {
    int16_t x = 0, y = 0, z = 0, poly = -1, yaw = 0, pitch = 0;
    uint32_t tick = 0;
};

Camera g_prev, g_cur;
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

/* the cameras of the last few ticks, oldest first */
Camera g_hist[4];
int g_nhist = 0;

void remember(const Camera &cam, bool snap) {
    if (snap) g_nhist = 0;
    if (g_nhist == 4) {
        for (int i = 0; i < 3; i++) g_hist[i] = g_hist[i + 1];
        g_nhist = 3;
    }
    g_hist[g_nhist++] = cam;
}

/* the two remembered cameras either side of the moment to show at `at`, and how far between */
void bracket(clock_type::time_point at, Camera &a, Camera &b, double &f) {
    double shown = (seconds(at) - g_base) / kTick - kDelayTicks;   /* in ticks */
    a = b = g_hist[0];
    f = 1.0;
    for (int i = g_nhist - 2; i >= 0; i--)
        if (shown >= g_hist[i].tick || i == 0) {
            a = g_hist[i];
            b = g_hist[i + 1];
            f = (shown - a.tick) / (double)(b.tick - a.tick);
            f = f < 0 ? 0 : f > 1 ? 1 : f;
            return;
        }
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
    c.tick = ld32(ld32(kDynamicWorldPtr));
    return c;
}

int lerp(int a, int b, double t) { return a + (int)((b - a) * t + (b >= a ? 0.5 : -0.5)); }

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
    cam.pitch = (int16_t)lerp_angle(a.pitch, b.pitch, t);   /* an angle too: just below level is 511 */
    /* Mouse turning isn't blended (that would show it late): show the facing the world will
     * reach once it runs the turns already on their way. Keys blend. */
    float pending;
    if (mouse_pending_yaw(pending) && g_cur.yaw >= 0 && g_cur.yaw < 512)
        cam.yaw = (int16_t)((g_cur.yaw + (int)lroundf(pending)) & 511);
    else
        cam.yaw = (int16_t)lerp_angle(a.yaw, b.yaw, t);
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
    bool snap = !g_have || now.tick < g_cur.tick || now.tick - g_cur.tick > 4 ||
                std::abs(now.x - g_cur.x) + std::abs(now.y - g_cur.y) > 2048 || std::abs(now.z - g_cur.z) > 1024;
    mouse_ticks_simulated((int16_t)c->r[3]);
    g_prev = snap ? now : g_cur;
    g_cur = now;
    g_have = true;
    g_tick_time = g_last_frame = clock_type::now();
    track_tick(now.tick, g_tick_time, snap);
    remember(now, snap);
    if (!enabled()) return 0;

    Camera cam = blend(c, g_tick_time);
    uint8_t saved[0x10];
    memcpy(saved, g_mem + p + kPlCamXY, sizeof saved);   /* +0x30 .. +0x3F */
    st16(p + kPlCamXY, (uint16_t)cam.x);
    st16(p + kPlCamXY + 2, (uint16_t)cam.y);
    st16(p + kPlCamZ, (uint16_t)cam.z);
    st16(p + kPlCamPoly, (uint16_t)cam.poly);
    st16(p + kPlYaw, (uint16_t)cam.yaw);
    st16(p + kPlPitch, (uint16_t)cam.pitch);
    fn_010403D8_orig(c);
    memcpy(g_mem + p + kPlCamXY, saved, sizeof saved);
    return 1;
}

/* top of the main loop's idle routine: draw a blended frame if one is due */
extern "C" int pt_hook_idle_game_state(PPCContext *c) {
    using namespace pt;
    if (!enabled() || !g_have) return 0;
    auto now = clock_type::now();
    double since_tick = std::chrono::duration<double>(now - g_tick_time).count();
    double since_frame = std::chrono::duration<double>(now - g_last_frame).count();
    if (since_tick > 2 * kTick) return 0;            /* paused, in a menu, or between levels */
    if (since_frame < frame_interval()) return 0;
    uint32_t wv = ld32(kWorldViewPtr), bitmap = ld32(kBitmapPtr);
    if (!wv || !bitmap || ld16(kScreenMode + 2) != 0) return 0;   /* normal display only */
    ensure_tvectors();
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
    uint32_t args[2] = {wv, bitmap};
    guest_call(c, g_tv_render_view, args, 2);
    guest_call(c, g_tv_update_screen, nullptr, 0);
    memcpy(g_mem + wv, saved, sizeof saved);
    g_last_frame = now;
    static unsigned frames = 0;
    static uint32_t first_tick = g_cur.tick;
    if (++frames % 300 == 0)
        logf(LOG_GFX, "interp: %u blended frames over %u ticks; camera %d,%d,%d poly %d yaw %d pitch %d\n", frames,
             g_cur.tick - first_tick, g_cur.x, g_cur.y, g_cur.z, g_cur.poly, g_cur.yaw, g_cur.pitch);
    return 0;
}
