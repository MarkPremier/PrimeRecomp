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

int ext() { return host::side_width(); }

bool widened(int size) {
    return ext() > 0 && (size == 0 || size == 1) && ld8(kScreenMode + 4) != 0 && ld16(kScreenMode + 2) == 0;
}

int game_state() { return (int16_t)ld16(PT_IMAGE_TOC - kGameStateOff); }

bool sides_live() {
    return g_captured && game_state() == kGameInProgress && widened((int16_t)ld16(kScreenMode));
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
    int size = (int16_t)ld16(kScreenMode);
    if (!widened(size)) return;
    uint32_t gw = ld32(kWorldPixels);
    if (!gw) return;
    qd::Surface s = qd::surface_of_port(gw);
    int16_t top, left, bottom, right;
    dest_frame(size, top, left, bottom, right);
    if (s.bounds.right - s.bounds.left != right - left || s.bounds.bottom - s.bounds.top != bottom - top) return;
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
    for (int y = 0; y < 480; y++) {
        uint32_t *row = host::side_row(y);
        if (y < top || y >= bottom) {
            for (int i = 0; i < 2 * e; i++) row[i] = 0xFF000000u;
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
    int e = ext();
    if (!e || !g_captured) return;
    for (int y = 0; y < 480; y++) {
        uint32_t *row = host::side_row(y);
        for (int i = 0; i < 2 * e; i++) {
            int x = i < e ? i - e : 640 + i - e;
            if (black || ((x + y) & 1)) row[i] = 0xFF000000u;
        }
    }
}

}  // namespace

/* calculate_destination_frame(short size, Boolean high_resolution, Rect *frame) */
extern "C" int pt_hook_calc_dest_frame(PPCContext *c) {
    int size = (int16_t)c->r[3];
    uint32_t rect = c->r[5];
    fn_0103E9B0_orig(c);
    if (widened(size)) {
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
    double s = 640.0 * sin(half_cone) / std_width;
    if (s >= 1.0) return 1;
    double adjusted0 = asin(s);                          /* the original's cone for 640 columns */
    double world_to_screen = 320.0 / tan(adjusted0);     /* ... and its scale, kept */
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
    capture_sides();
    fn_01041A24_orig(c);
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
void init_widescreen() { host::set_sides_live(sides_live); }

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
