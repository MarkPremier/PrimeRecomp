/* Widescreen: a wider 3D view, the rest of the game as it was.
 *
 * The Mac screen stays 640x480 (menus, dialogs and the HUD are laid out for it); the host
 * presents host::side_width() extra columns either side. In the game, with Window Size "Full
 * Screen" or "100%" at high resolution, calculate_destination_frame widens the 3D view's
 * rectangle by those columns each side. Everything sized from it follows: the world GWorld,
 * the view's screen_width, the renderer. update_screen's CopyBits to the 640-wide window clips
 * to the middle; the columns it can't show are copied from the GWorld into the host's sides.
 *
 * The field of view: initialize_view_data maps the view's width onto a cone with
 * asin(width * sin(half_cone) / standard_width), which suits 4:3 but bends a wider view. Here
 * the middle 640 columns keep the original's scale exactly and the cone is widened to the
 * extra columns (horizontal-plus), as Aleph One does with tan/atan.
 */
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "image_info.h"
#include "ppc_funcs.h"
#include "pt/host.h"
#include "pt/ppc.h"
#include "pt/qd.h"
#include "pt/runtime.h"

namespace {

using namespace pt;

/* Prime Target v1.2 */
constexpr uint32_t kScreenMode = 0x011458F4;   /* screen_mode: +0 size (0 full screen, 1 100%), +2 acceleration,
                                                   +4 high resolution */
constexpr uint32_t kWorldPixels = 0x01145914;  /* GWorldPtr world_pixels */
constexpr uint32_t kGameStateOff = 0x3FB4 - 8; /* get_game_state: lha 8(r2 - 0x3FB4) */
constexpr int kGameInProgress = 7;

/* view_data (initialize_view_data) */
constexpr uint32_t kVwEdges = 0x00, kVwHalfCone = 0x30, kVwHalfVCone = 0x32, kVwFov = 0x3C, kVwStdWidth = 0x40,
                   kVwWidth = 0x44, kVwHorizScale = 0x4C, kVwVertScale = 0x50, kVwHalfWidth = 0x54,
                   kVwHalfHeight = 0x58, kVwW2sX = 0x5C, kVwW2sY = 0x60, kVwRealW2sX = 0x74, kVwRealW2sY = 0x78;

bool g_captured = false;
bool g_hi_captured = false;   /* the host's high-resolution frame is this frame's 3D view */

int ext() { return host::side_width(); }

bool widened(int size) {
    return ext() > 0 && (size == 0 || size == 1) && ld8(kScreenMode + 4) != 0 && ld16(kScreenMode + 2) == 0;
}

/* High resolution (option hires = the 3D view's height in pixels, e.g. 1080; 0 or absent: off).
 * Window Size "Full Screen" only, with high resolution on and no acceleration. The world bitmap
 * and view are made hires_h() tall and as wide as the presented frame's shape (widescreen or
 * 4:3); the Mac screen keeps a scaled-down copy (for dialogs over the game), and the host
 * presents the full-size picture while it is current. */
int g_hires_h = -1;   /* the option, read once; set_hires_lines changes it */
int hires_h() {
    if (g_hires_h < 0) {
        const char *v = option("PT_HIRES");
        g_hires_h = v ? atoi(v) : 0;
        if (g_hires_h <= 480) g_hires_h = 0;
        if (g_hires_h > 4096) g_hires_h = 4096;
    }
    return g_hires_h;
}
int hires_w() {
    int w = (int)((double)hires_h() * (640 + 2 * ext()) / 480 + 0.5);
    return w > 4096 ? 4096 : w;
}
bool hires(int size) {
    return hires_h() > 0 && size == 0 && ld8(kScreenMode + 4) != 0 && ld16(kScreenMode + 2) == 0;
}

/* calculate_destination_frame's callers that place the world on the Mac screen (the rest size
 * the world bitmap and the view, or draw into the bitmap) */
bool frame_on_mac_screen(uint32_t lr) {
    switch (lr) {
    case 0x01041A64:   /* update_screen: CopyBits destination */
    case 0x0103EFC4:   /* darken_world_window */
    case 0x01040CCC:   /* validate_world_window */
    case 0x01040DBC:   /* blackout_world_window */
    case 0x010411E8:   /* DrawItemValkyrie */
    case 0x010413FC:   /* DrawMovieValkyrie */
        return true;
    }
    return false;
}

int game_state() { return (int16_t)ld16(PT_IMAGE_TOC - kGameStateOff); }

bool sides_live() {
    return g_captured && game_state() == kGameInProgress && widened((int16_t)ld16(kScreenMode));
}

bool hires_live() {
    return g_hi_captured && game_state() == kGameInProgress && hires((int16_t)ld16(kScreenMode));
}

/* calculate_destination_frame's rectangle for a size, as the game computes it */
void dest_frame(int size, int16_t &top, int16_t &left, int16_t &bottom, int16_t &right) {
    static const int16_t dims[4][2] = {{640, 480}, {640, 320}, {480, 240}, {320, 160}};
    int s = size < 0 || size > 3 ? 1 : size;
    int w = dims[s][0], h = dims[s][1];
    left = 0; top = 0; right = (int16_t)w; bottom = (int16_t)h;
    if (s) { left += (int16_t)((640 - w) / 2); right += (int16_t)((640 - w) / 2); top += (int16_t)((320 - h) / 2); bottom += (int16_t)((320 - h) / 2); }
    if (widened(size)) { left -= (int16_t)ext(); right += (int16_t)ext(); }
}

/* copy the GWorld's columns outside the 640-wide window into the host's sides */
void capture_sides() {
    g_captured = false;
    g_hi_captured = false;
    int size = (int16_t)ld16(kScreenMode);
    if (!widened(size)) return;
    uint32_t gw = ld32(kWorldPixels);
    if (!gw) return;
    qd::Surface s = qd::surface_of_port(gw);
    int16_t top, left, bottom, right;
    dest_frame(size, top, left, bottom, right);
    int sw = s.bounds.right - s.bounds.left, sh = s.bounds.bottom - s.bounds.top;
    bool hi = hires(size) && sw == hires_w() && sh == hires_h();
    if (!hi && (sw != right - left || sh != bottom - top)) return;
    uint32_t lut[256];
    if (s.depth == 8) {
        /* the world's pixels are the screen's colour indexes (update_screen copies them as they
         * are), whatever the GWorld's own table says */
        std::vector<qd::RGB> pal = qd::screen_palette();
        for (int i = 0; i < 256; i++) {
            qd::RGB c = i < (int)pal.size() ? pal[i] : qd::RGB{0, 0, 0};
            lut[i] = 0xFF000000u | (uint32_t)(c.r >> 8) << 16 | (uint32_t)(c.g >> 8) << 8 | (c.b >> 8);
        }
    } else if (s.depth != 16 && s.depth != 32) {
        return;
    }
    int e = ext();
    uint8_t mr[256], mg[256], mb[256];   /* direct pixels go through the display's ramp (fades) */
    if (s.depth != 8) qd::direct_maps(s.depth, mr, mg, mb);
    auto pixel = [&](int gx, int gy) -> uint32_t {
        const uint8_t *p = g_mem + s.base + (uint32_t)gy * s.row_bytes;
        if (s.depth == 8) return lut[p[gx]];
        if (s.depth == 16) {
            uint32_t v = (uint32_t)p[2 * gx] << 8 | p[2 * gx + 1];
            return 0xFF000000u | (uint32_t)mr[(v >> 10) & 31] << 16 | (uint32_t)mg[(v >> 5) & 31] << 8 | mb[v & 31];
        }
        return 0xFF000000u | (uint32_t)mr[p[4 * gx + 1]] << 16 | (uint32_t)mg[p[4 * gx + 2]] << 8 | mb[p[4 * gx + 3]];
    };
    if (hi) {
        /* the whole view at full size for the host (its guest pixels; the host converts them as
         * it presents); the sides, shown under dialogs, sample it as the copy on the Mac screen does */
        int bpp = s.depth / 8;
        uint8_t *out = host::hires_frame(sw, sh, s.depth);
        for (int y = 0; y < sh; y++)
            memcpy(out + (size_t)y * sw * bpp, g_mem + s.base + (uint32_t)y * s.row_bytes, (size_t)sw * bpp);
        g_hi_captured = true;
        int fw = right - left, fh = bottom - top;
        if (e) {
            for (int y = 0; y < 480; y++) {
                uint32_t *row = host::side_row(y);
                int gy = (int)((int64_t)(y - top) * sh / fh);
                for (int i = 0; i < e; i++) {
                    row[i] = pixel((int)((int64_t)(-e + i - left) * sw / fw), gy);
                    row[e + i] = pixel((int)((int64_t)(640 + i - left) * sw / fw), gy);
                }
            }
        }
        g_captured = true;
        return;
    }
    for (int y = 0; y < 480; y++) {
        uint32_t *row = host::side_row(y);
        if (y < top || y >= bottom) {
            for (int i = 0; i < 2 * e; i++) row[i] = 0;   /* background: the host fills it with black as displayed */
            continue;
        }
        int gy = y - top;
        for (int i = 0; i < e; i++) {
            row[i] = pixel(-e + i - left, gy);          /* screen x = -e + i */
            row[e + i] = pixel(640 + i - left, gy);     /* screen x = 640 + i */
        }
    }
    g_captured = true;
}

/* darken_world_window (under a dialog) paints a 50% black stipple; blackout paints black */
void shade_sides(bool black) {
    g_hi_captured = false;   /* the host shows the Mac screen's copy, shaded by the game */
    int e = ext();
    if (!e || !g_captured) return;
    for (int y = 0; y < 480; y++) {
        uint32_t *row = host::side_row(y);
        for (int i = 0; i < 2 * e; i++) {
            int x = i < e ? i - e : 640 + i - e;
            if (black || ((x + y) & 1)) row[i] = 0;
        }
    }
}

}  // namespace

/* calculate_destination_frame(short size, Boolean high_resolution, Rect *frame) */
extern "C" int pt_hook_calc_dest_frame(PPCContext *c) {
    int size = (int16_t)c->r[3];
    uint32_t rect = c->r[5];
    uint32_t lr = c->lr;
    fn_0103E9B0_orig(c);
    if (hires(size) && !frame_on_mac_screen(lr)) {
        st16(rect + 0, 0);
        st16(rect + 2, 0);
        st16(rect + 4, (uint16_t)hires_h());
        st16(rect + 6, (uint16_t)hires_w());
    } else if (widened(size)) {
        st16(rect + 2, (uint16_t)((int16_t)ld16(rect + 2) - ext()));
        st16(rect + 6, (uint16_t)((int16_t)ld16(rect + 6) + ext()));
    }
    return 1;
}

/* initialize_view_data(struct view_data *view): wider than 640, keep the scale, widen the cone */
extern "C" int pt_hook_init_view_data(PPCContext *c) {
    uint32_t v = c->r[3];
    fn_01031528_orig(c);
    int width = (int32_t)ld32(v + kVwWidth), std_width = (int32_t)ld32(v + kVwStdWidth);
    if (width <= 640 || std_width <= 0) return 1;
    const double two_pi = 8.0 * atan(1.0);
    double half_cone = (int32_t)ld32(v + kVwFov) * (two_pi / 360.0) / 2;
    /* At high resolution the view is k times the original's 480 lines; the game makes the
     * standard width twice the height (k x 960), so the 640 columns it stands for are 640k. */
    int height = (int32_t)ld32(v + 0x48);
    double k = height > 480 ? height / 480.0 : 1.0;
    double s = 640.0 * k * sin(half_cone) / std_width;
    if (s >= 1.0) return 1;
    double adjusted0 = asin(s);                          /* the original's cone for 640 columns */
    double world_to_screen = 320.0 * k / tan(adjusted0); /* ... and its scale, kept */
    double adjusted = atan((width / 2) / world_to_screen);
    int hs = (int32_t)ld32(v + kVwHorizScale), vs = (int32_t)ld32(v + kVwVertScale);
    if (hs <= 0 || vs <= 0) return 1;
    int16_t w2s_x = (int16_t)(world_to_screen / hs + 0.5), w2s_y = (int16_t)(world_to_screen / vs + 0.5);
    st16(v + kVwHalfCone, (uint16_t)(int16_t)(adjusted * 512 / two_pi + 1.0));
    st32(v + kVwW2sX, (uint32_t)(int32_t)w2s_x);
    st32(v + kVwRealW2sX, (uint32_t)(int32_t)w2s_x);
    st32(v + kVwW2sY, (uint32_t)(int32_t)w2s_y);
    st32(v + kVwRealW2sY, (uint32_t)(int32_t)w2s_y);
    int half_h = (int32_t)ld32(v + kVwHalfHeight);
    st16(v + kVwHalfVCone, (uint16_t)(int16_t)(512 * atan((double)half_h * vs / world_to_screen) / two_pi + 1.0));
    int half_w = (int32_t)ld32(v + kVwHalfWidth);
    st16(v + kVwEdges + 0, (uint16_t)w2s_x);
    st16(v + kVwEdges + 2, (uint16_t)(-half_w));
    st16(v + kVwEdges + 4, (uint16_t)(-w2s_x));
    st16(v + kVwEdges + 6, (uint16_t)(-half_w));
    static int logged_width = 0;
    if (width != logged_width) {
        logged_width = width;
        logf(LOG_GFX, "widescreen: view %dx%d, half cone %d (scale %d as at 640)\n", width,
             (int32_t)ld32(v + 0x48), (int16_t)ld16(v + kVwHalfCone), w2s_x);
    }
    return 1;
}

/* update_screen(): the world is about to be copied to the window, which presents it; take the
 * columns beside it first so they show in the same frame (after, they lagged a frame and the
 * view broke at the join while moving) */
extern "C" int pt_hook_update_screen(PPCContext *c) {
    using clk = std::chrono::steady_clock;
    auto t0 = clk::now();
    capture_sides();
    auto t1 = clk::now();
    fn_01041A24_orig(c);
    auto t2 = clk::now();
    if (g_hi_captured) {   /* PT_LOG=gfx: what high resolution costs besides drawing */
        static double cap = 0, copy = 0;
        static int n = 0;
        cap += std::chrono::duration<double, std::milli>(t1 - t0).count();
        copy += std::chrono::duration<double, std::milli>(t2 - t1).count();
        if (++n == 600) {
            logf(LOG_GFX, "hires: capture %.2f ms, update_screen %.2f ms (the shrunk copy and the present it triggers; mean of %d)\n", cap / n, copy / n, n);
            cap = copy = 0;
            n = 0;
        }
    }
    return 1;
}

extern "C" int pt_hook_darken_world_window(PPCContext *c) {
    fn_0103EF64_orig(c);
    shade_sides(false);
    return 1;
}

extern "C" int pt_hook_blackout_world_window(PPCContext *c) {
    fn_01040D64_orig(c);
    shade_sides(true);
    return 1;
}

namespace {
bool g_view_dirty = false;   /* the width changed: the game's view needs rebuilding */
}

namespace pt {
void init_widescreen() {
    host::set_sides_live(sides_live);
    host::set_hires_live(hires_live);
    if (hires_h()) logf(LOG_GFX, "hires: 3D view %d lines tall in Full Screen\n", hires_h());
}

/* Preferences > Graphics > Widescreen */
bool widescreen_enabled() {
    const char *v = option("PT_WIDESCREEN");
    return !(v && (strcmp(v, "0") == 0 || strcmp(v, "off") == 0));
}
void set_widescreen(bool on) {
    const char *cur = option("PT_WIDESCREEN");
    std::string keep = cur && widescreen_enabled() ? cur : "auto";   /* keep a chosen shape such as 16:9 */
    set_option("PT_WIDESCREEN", on ? keep.c_str() : "0");
    host::reconfigure_width();
    g_view_dirty = true;
    logf(LOG_GFX, "widescreen %s\n", on ? "on" : "off");
}

/* Preferences > Graphics > Window Size "Full Screen 1080p" / "1440p" (dialogs.cpp) */
int hires_lines() { return hires_h(); }
void set_hires_lines(int lines) {
    if (lines <= 480) lines = 0;
    if (lines == hires_h()) return;
    set_option("PT_HIRES", std::to_string(lines).c_str());
    g_hires_h = lines;
    g_hi_captured = false;
    g_view_dirty = true;
    logf(LOG_GFX, "hires %d\n", lines);
}

/* from the main loop (interp.cpp's idle hook): once in a game, let the game's own
 * change_screen_mode rebuild the world GWorld and view at the new width */
void widescreen_update(PPCContext *c) {
    if (!g_view_dirty || game_state() != kGameInProgress) return;
    g_view_dirty = false;
    static uint32_t tv = 0;
    if (!tv) {
        tv = sys_alloc(8);
        st32(tv, 0x0103ECCC);   /* change_screen_mode(struct screen_mode_data *, Boolean redraw) */
        st32(tv + 4, PT_IMAGE_TOC);
    }
    uint32_t args[2] = {kScreenMode, 0};
    guest_call(c, tv, args, 2);
}
}  // namespace pt
