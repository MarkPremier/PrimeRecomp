/* Window Manager.
 *
 * Windows are CGrafPorts whose PixMap aliases the screen with bounds offset so
 * local (0,0) is the content's top-left. Overlap is handled with a per-pixel
 * ownership map of the screen (front-most window wins), consulted by every
 * QuickDraw pixel write through a window port. Exposed areas get update events,
 * as on a real Mac; the desktop repaints itself.
 *
 * Frames are drawn natively in System 7 style for the standard procIDs.
 */
#include <windows.h>

#include <algorithm>
#include <vector>

#include "pt/host.h"
#include "pt/qd.h"
#include "pt/resources.h"
#include "pt/runtime.h"
#include "pt/ui.h"

namespace pt::wm {

using namespace pt::qd;

namespace wr {   /* WindowRecord after the port */
enum : uint32_t {
    windowKind = 108, visible = 110, hilited = 111, goAwayFlag = 112, spareFlag = 113, strucRgn = 114,
    contRgn = 118, updateRgn = 122, windowDefProc = 126, dataHandle = 130, titleHandle = 134, titleWidth = 138,
    controlList = 140, nextWindow = 144, windowPic = 148, refCon = 152, size = 156,
};
}

struct Win {
    uint32_t w;
    int proc;
    uint8_t id;
    bool owns_storage;
};
static std::vector<Win> g_wins;          /* front first */
static uint8_t g_owner[640 * 480];        /* 0 = desktop, else window id */
static uint8_t g_next_id = 1;
static std::vector<uint32_t> g_activates; /* windows needing activate events (front first) */
static uint32_t g_deactivate = 0;
static uint32_t g_wmgr_port = 0;
static Rect g_desk_dirty{0, 0, 0, 0};

static Win *find(uint32_t w) {
    for (auto &x : g_wins)
        if (x.w == w) return &x;
    return nullptr;
}

static Rect content_global(uint32_t w) { return region_bbox(ld32(w + wr::contRgn)); }
static Rect struct_global(uint32_t w) { return region_bbox(ld32(w + wr::strucRgn)); }

bool occlusion_for(uint32_t port, const uint8_t *&map, uint8_t &owner) {
    Win *x = find(port);
    if (!x) return false;
    map = g_owner;
    owner = x->id;
    return true;
}

static bool has_title(int proc) { int v = proc & ~3; return v == 0 || v == 4 || v == 8 || proc == 5 || v == 16; }

static Rect struct_of(int proc, const Rect &c) {
    switch (proc) {
    case 1: return {(int16_t)(c.top - 8), (int16_t)(c.left - 8), (int16_t)(c.bottom + 8), (int16_t)(c.right + 8)};
    case 2: return {(int16_t)(c.top - 1), (int16_t)(c.left - 1), (int16_t)(c.bottom + 1), (int16_t)(c.right + 1)};
    case 3: return {(int16_t)(c.top - 1), (int16_t)(c.left - 1), (int16_t)(c.bottom + 3), (int16_t)(c.right + 3)};
    default:
        if (has_title(proc)) return {(int16_t)(c.top - 19), (int16_t)(c.left - 1), (int16_t)(c.bottom + 2), (int16_t)(c.right + 2)};
        return {(int16_t)(c.top - 1), (int16_t)(c.left - 1), (int16_t)(c.bottom + 1), (int16_t)(c.right + 1)};
    }
}

static void recompute_occlusion() {
    memset(g_owner, 0, sizeof g_owner);
    for (auto it = g_wins.rbegin(); it != g_wins.rend(); ++it) {
        if (!ld8(it->w + wr::visible)) continue;
        Rect s = rect_sect(struct_global(it->w), {0, 0, 480, 640});
        for (int y = s.top; y < s.bottom; y++) memset(g_owner + y * 640 + s.left, it->id, s.right - s.left);
    }
    st32(0x09D6, g_wins.empty() ? 0 : g_wins.front().w);   /* WindowList */
}

/* ---------- desktop ---------- */
uint32_t wmgr_port();
static void paint_desktop(const Rect &r) {
    uint32_t p = wmgr_port();
    uint32_t saved = current_port();
    set_current_port(p);
    Surface s = surface_of_port(p);
    Rect c = rect_sect(r, {0, 0, 480, 640});
    uint32_t gray = rgb_to_pixel(s, {0x8888, 0x8888, 0x8888}), dark = rgb_to_pixel(s, {0x4444, 0x4444, 0x4444});
    for (int y = c.top; y < c.bottom; y++)
        for (int x = c.left; x < c.right; x++)
            if (!g_owner[y * 640 + x]) put_pixel_raw(s, x, y, ((x + y) & 1) ? dark : gray);
    int mbar = (int16_t)ld16(0x0BAA);
    if (mbar > 0 && c.top < mbar) {
        uint32_t white = rgb_to_pixel(s, {0xFFFF, 0xFFFF, 0xFFFF}), black = rgb_to_pixel(s, {0, 0, 0});
        for (int y = c.top; y < std::min<int>(mbar, c.bottom); y++)
            for (int x = c.left; x < c.right; x++)
                if (!g_owner[y * 640 + x]) put_pixel_raw(s, x, y, y == mbar - 1 ? black : white);
    }
    set_current_port(saved);
}

uint32_t wmgr_port() {
    if (!g_wmgr_port) {
        g_wmgr_port = sys_alloc(port::size, 4);
        init_port(g_wmgr_port, true, 0);
        st32(0x0D2C, g_wmgr_port);   /* WMgrCPort */
    }
    return g_wmgr_port;
}

/* ---------- frames ---------- */
static void hline(const Surface &s, int y, int x0, int x1, uint32_t v) { for (int x = x0; x < x1; x++) put_pixel_raw(s, x, y, v); }
static void vline(const Surface &s, int x, int y0, int y1, uint32_t v) { for (int y = y0; y < y1; y++) put_pixel_raw(s, x, y, v); }
static void frame(const Surface &s, const Rect &r, uint32_t v) {
    hline(s, r.top, r.left, r.right, v); hline(s, r.bottom - 1, r.left, r.right, v);
    vline(s, r.left, r.top, r.bottom, v); vline(s, r.right - 1, r.top, r.bottom, v);
}

static void draw_frame(const Win &x) {
    uint32_t w = x.w;
    if (!ld8(w + wr::visible)) return;
    uint32_t p = wmgr_port();
    Surface s = surface_of_port(p);
    s.occl = g_owner;
    s.owner = x.id;
    uint32_t black = rgb_to_pixel(s, {0, 0, 0}), white = rgb_to_pixel(s, {0xFFFF, 0xFFFF, 0xFFFF});
    Rect c = content_global(w), st = struct_global(w);
    auto px = [&](int xx, int yy, uint32_t v) { if (xx >= 0 && xx < 640 && yy >= 0 && yy < 480 && g_owner[yy * 640 + xx] == x.id) put_pixel_raw(s, xx, yy, v); };
    auto fr = [&](Rect r, uint32_t v) {
        for (int xx = r.left; xx < r.right; xx++) { px(xx, r.top, v); px(xx, r.bottom - 1, v); }
        for (int yy = r.top; yy < r.bottom; yy++) { px(r.left, yy, v); px(r.right - 1, yy, v); }
    };
    bool active = ld8(w + wr::hilited) != 0;
    switch (x.proc) {
    case 1:
        for (int i = 0; i < 8; i++) fr({(int16_t)(st.top + i), (int16_t)(st.left + i), (int16_t)(st.bottom - i), (int16_t)(st.right - i)},
                                       (i == 0 || (i >= 3 && i <= 5)) ? black : white);
        break;
    case 2: fr(st, black); break;
    case 3:
        fr({st.top, st.left, (int16_t)(st.bottom - 2), (int16_t)(st.right - 2)}, black);
        for (int i = 0; i < 2; i++) {
            for (int xx = st.left + 2; xx < st.right; xx++) px(xx, st.bottom - 1 - i, black);
            for (int yy = st.top + 2; yy < st.bottom; yy++) px(st.right - 1 - i, yy, black);
        }
        break;
    default: {
        Rect outer{st.top, st.left, (int16_t)(st.bottom - 1), (int16_t)(st.right - 1)};
        fr(outer, black);
        for (int xx = outer.left + 1; xx <= outer.right; xx++) px(xx, outer.bottom, black);
        for (int yy = outer.top + 1; yy <= outer.bottom; yy++) px(outer.right, yy, black);
        if (has_title(x.proc)) {
            int ty0 = outer.top + 1, ty1 = c.top - 1;
            for (int yy = ty0; yy < ty1; yy++)
                for (int xx = outer.left + 1; xx < outer.right - 1; xx++)
                    px(xx, yy, active && ((yy - ty0) % 2 == 1) && yy > ty0 + 1 && yy < ty1 - 2 ? black : white);
            for (int xx = outer.left; xx < outer.right; xx++) px(xx, c.top - 1, black);
            /* title text */
            uint32_t th = ld32(w + wr::titleHandle);
            if (th && ld32(th)) {
                uint32_t tp = ld32(th);
                uint32_t saved = current_port();
                set_current_port(p);
                st16(p + port::txFont, 0); st16(p + port::txSize, 12); st8(p + port::txFace, 0); st16(p + port::txMode, 0);
                int tw = text_width(p, g_mem + tp + 1, g_mem[tp]);
                int tx = (outer.left + outer.right - tw) / 2;
                for (int yy = ty0 + 1; yy < ty1 - 1; yy++)
                    for (int xx = tx - 6; xx < tx + tw + 6; xx++) px(xx, yy, white);
                st16(p + port::pnLoc, (uint16_t)(ty1 - 5)); st16(p + port::pnLoc + 2, (uint16_t)tx);
                /* draw via a temporary occlusion-aware surface: use the window id */
                draw_text(p, g_mem + tp + 1, g_mem[tp]);
                st16(p + port::txMode, 1);
                set_current_port(saved);
            }
        }
    }
    }
}

static void erase_content(const Win &x, const Rect &global) {
    /* PaintOne: fill the exposed content with the window's background colour */
    Surface s = surface_of_port(x.w);
    uint32_t bk = rgb_to_pixel(s, port_bk(x.w));
    Rect c = rect_sect(global, {0, 0, 480, 640});
    for (int y = c.top; y < c.bottom; y++)
        for (int xx = c.left; xx < c.right; xx++)
            if (g_owner[y * 640 + xx] == x.id) put_pixel_raw(s, xx, y, bk);
}

static void invalidate_global(const Rect &r) {
    /* windows get update regions (and their exposed content erased); uncovered desktop repaints */
    for (auto &x : g_wins) {
        if (!ld8(x.w + wr::visible)) continue;
        Rect c = rect_sect(content_global(x.w), r);
        if (rect_empty(c)) continue;
        erase_content(x, c);
        uint32_t ur = ld32(x.w + wr::updateRgn);
        set_region(ur, rect_union(region_bbox(ur), c));
    }
    g_desk_dirty = rect_union(g_desk_dirty, r);
}

static void redraw_all_frames() {
    for (auto &x : g_wins) draw_frame(x);
}

static void restack(const Rect &exposed) {
    recompute_occlusion();
    invalidate_global(exposed);
    paint_desktop(g_desk_dirty);
    g_desk_dirty = {0, 0, 0, 0};
    redraw_all_frames();
}

static void set_window_geometry(uint32_t w, const Rect &content) {
    Win *x = find(w);
    int proc = x ? x->proc : 0;
    set_region(ld32(w + wr::contRgn), content);
    set_region(ld32(w + wr::strucRgn), struct_of(proc, content));
    uint32_t pmp = port_pixmap(w);
    int16_t h = (int16_t)(content.right - content.left), v = (int16_t)(content.bottom - content.top);
    Rect pr = read_rect(w + port::portRect);
    /* keep the window's local origin; the pixmap bounds place it on screen */
    write_rect(pmp + pm::bounds, {(int16_t)(pr.top - content.top), (int16_t)(pr.left - content.left),
                                  (int16_t)(pr.top - content.top + 480), (int16_t)(pr.left - content.left + 640)});
    write_rect(w + port::portRect, {pr.top, pr.left, (int16_t)(pr.top + v), (int16_t)(pr.left + h)});
    set_region(ld32(w + port::visRgn), read_rect(w + port::portRect));
}

static void activate(uint32_t w) {
    for (auto &x : g_wins) st8(x.w + wr::hilited, x.w == w);
    g_activates.push_back(w);
}

uint32_t new_window(uint32_t storage, const Rect &bounds, const std::string &title, bool visible, int proc,
                    uint32_t behind, bool goaway, uint32_t refcon, uint32_t size) {
    uint32_t w = storage ? storage : guest_alloc(size, true);
    memset(g_mem + w, 0, size);
    init_port(w, true, 0);
    st16(w + wr::windowKind, 8);
    st8(w + wr::visible, visible);
    st8(w + wr::goAwayFlag, goaway);
    st32(w + wr::strucRgn, new_region({0, 0, 0, 0}));
    st32(w + wr::contRgn, new_region({0, 0, 0, 0}));
    st32(w + wr::updateRgn, new_region({0, 0, 0, 0}));
    st32(w + wr::windowDefProc, (uint32_t)proc);
    uint32_t th = handle_new((uint32_t)title.size() + 1, false);
    write_pstr(ld32(th), title);
    st32(w + wr::titleHandle, th);
    st32(w + wr::refCon, refcon);
    write_rect(w + port::portRect, {0, 0, (int16_t)(bounds.bottom - bounds.top), (int16_t)(bounds.right - bounds.left)});
    Win x{w, proc, g_next_id++, storage == 0};
    if (!g_next_id) g_next_id = 1;
    if (behind == 0xFFFFFFFFu || behind == 0) {
        if (behind == 0) g_wins.push_back(x); else g_wins.insert(g_wins.begin(), x);
    } else {
        auto it = std::find_if(g_wins.begin(), g_wins.end(), [&](const Win &a) { return a.w == behind; });
        g_wins.insert(it == g_wins.end() ? g_wins.end() : it + 1, x);
    }
    set_window_geometry(w, bounds);
    if (visible) {
        if (g_wins.front().w == w) activate(w);
        restack(struct_global(w));
    }
    logf(LOG_GFX, "NewWindow %08X proc %d [%d,%d,%d,%d] \"%s\"\n", w, proc, bounds.left, bounds.top, bounds.right,
         bounds.bottom, title.c_str());
    return w;
}

void dispose_window(uint32_t w) {
    Win *x = find(w);
    if (!x) return;
    Rect st = struct_global(w);
    bool was_front = !g_wins.empty() && g_wins.front().w == w;
    bool owns = x->owns_storage;
    g_wins.erase(std::remove_if(g_wins.begin(), g_wins.end(), [&](const Win &a) { return a.w == w; }), g_wins.end());
    g_activates.erase(std::remove(g_activates.begin(), g_activates.end(), w), g_activates.end());
    restack(st);
    if (was_front && !g_wins.empty()) activate(g_wins.front().w);
    if (owns) guest_free(w);
}

bool is_window(uint32_t w) { return find(w) != nullptr; }
uint32_t front_window() {
    for (auto &x : g_wins)
        if (ld8(x.w + wr::visible)) return x.w;
    return 0;
}

void select_window(uint32_t w) {
    auto it = std::find_if(g_wins.begin(), g_wins.end(), [&](const Win &a) { return a.w == w; });
    if (it == g_wins.end()) return;
    Win x = *it;
    g_wins.erase(it);
    g_wins.insert(g_wins.begin(), x);
    activate(w);
    restack(struct_global(w));
}

void show_window(uint32_t w, bool show) {
    if (!find(w) || (ld8(w + wr::visible) != 0) == show) return;
    st8(w + wr::visible, show);
    if (show && front_window() == w) activate(w);
    restack(struct_global(w));
}

void move_window(uint32_t w, int h, int v) {
    Rect c = content_global(w), old = struct_global(w);
    Rect n{(int16_t)v, (int16_t)h, (int16_t)(v + c.bottom - c.top), (int16_t)(h + c.right - c.left)};
    set_window_geometry(w, n);
    /* a hidden window covers nothing: moving it uncovers nothing either (the game moves its
     * dialogs before showing them; erasing there left a black band over the game) */
    if (ld8(w + wr::visible)) restack(rect_union(old, struct_global(w)));
}

void size_window(uint32_t w, int width, int height) {
    Rect c = content_global(w), old = struct_global(w);
    set_window_geometry(w, {c.top, c.left, (int16_t)(c.top + height), (int16_t)(c.left + width)});
    if (ld8(w + wr::visible)) restack(rect_union(old, struct_global(w)));
}

bool pending_window_event(uint16_t mask, host::MacEvent &e, bool remove) {
    e = {};
    e.when = ticks();
    host::mouse_global(e.v, e.h);
    e.modifiers = host::modifiers();
    if ((mask & (1 << 8)) && g_deactivate) {
        e.what = 8; e.message = g_deactivate; e.modifiers &= ~1;
        if (remove) g_deactivate = 0;
        return true;
    }
    if ((mask & (1 << 8)) && !g_activates.empty()) {
        e.what = 8; e.message = g_activates.front(); e.modifiers |= 1;   /* activeFlag */
        if (remove) g_activates.erase(g_activates.begin());
        return true;
    }
    if (mask & (1 << 6)) {
        for (auto &x : g_wins) {
            if (!ld8(x.w + wr::visible)) continue;
            if (!rect_empty(region_bbox(ld32(x.w + wr::updateRgn)))) {
                e.what = 6; e.message = x.w;
                return true;
            }
        }
    }
    return false;
}

int find_window(int v, int h, uint32_t &out) {
    out = 0;
    if (v < (int16_t)ld16(0x0BAA)) return 1;   /* inMenuBar */
    for (auto &x : g_wins) {
        if (!ld8(x.w + wr::visible)) continue;
        Rect c = content_global(x.w), s = struct_global(x.w);
        if (v >= c.top && v < c.bottom && h >= c.left && h < c.right) { out = x.w; return 3; }
        if (v >= s.top && v < s.bottom && h >= s.left && h < s.right) {
            out = x.w;
            if (has_title(x.proc) && v < c.top) {
                if (ld8(x.w + wr::goAwayFlag) && h >= s.left + 8 && h < s.left + 19) return 6;
                return 4;
            }
            return 3;
        }
    }
    return 0;
}

bool begin_update(uint32_t w) {
    if (!find(w)) return false;
    Rect u = region_bbox(ld32(w + wr::updateRgn));
    Rect cr = region_bbox(ld32(w + wr::contRgn));
    Rect pr = read_rect(w + port::portRect);
    Rect local{(int16_t)(u.top - cr.top + pr.top), (int16_t)(u.left - cr.left + pr.left), (int16_t)(u.bottom - cr.top + pr.top),
               (int16_t)(u.right - cr.left + pr.left)};
    set_region(ld32(w + port::visRgn), rect_empty(u) ? Rect{0, 0, 0, 0} : rect_sect(pr, local));
    set_region(ld32(w + wr::updateRgn), {0, 0, 0, 0});
    return !rect_empty(u);
}
void end_update(uint32_t w) {
    if (!find(w)) return;
    set_region(ld32(w + port::visRgn), read_rect(w + port::portRect));
}

}  // namespace pt::wm

/* ======================================================================== */
using namespace pt;
using namespace pt::qd;
using namespace pt::wm;

HLE(InitWindows) {
    uint32_t p = wmgr_port();
    st32(0x09D6, 0);
    paint_desktop({0, 0, 480, 640});
    set_current_port(p);
}

HLE(NewWindow) {
    /* WindowPtr NewWindow(void *storage, const Rect *bounds, ConstStr255Param title, Boolean visible,
     *                     short procID, WindowPtr behind, Boolean goAwayFlag, long refCon) */
    RET(new_window(ARG(0), read_rect(ARG(1)), read_pstr(ARG(2)), ARG(3) & 0xFF, (int16_t)ARG(4), ARG(5), ARG(6) & 0xFF,
                   ARG(7), wr::size));
}

HLE(GetNewCWindow) {
    /* WindowPtr GetNewCWindow(short windowID, void *storage, WindowPtr behind) */
    std::vector<uint8_t> d;
    if (!res::get_bytes('WIND', (int16_t)ARG(0), d) || d.size() < 19) { RET(0); return; }
    auto be16 = [&](int o) { return (int16_t)(d[o] << 8 | d[o + 1]); };
    Rect b{be16(0), be16(2), be16(4), be16(6)};
    int proc = be16(8);
    bool vis = be16(10) != 0, goaway = be16(12) != 0;
    uint32_t refcon = (uint32_t)(d[14] << 24 | d[15] << 16 | d[16] << 8 | d[17]);
    std::string title((const char *)&d[19], d[18]);
    RET(new_window(ARG(1), b, title, vis, proc, ARG(2), goaway, refcon, wr::size));
}

HLE(DisposeWindow) { dispose_window(ARG(0)); }
HLE(CloseWindow) { dispose_window(ARG(0)); }
HLE(ShowWindow) { show_window(ARG(0), true); }
HLE(SelectWindow) { select_window(ARG(0)); }
HLE(FrontWindow) { RET(front_window()); }
HLE(MoveWindow) { move_window(ARG(0), (int16_t)ARG(1), (int16_t)ARG(2)); if (ARG(3) & 0xFF) select_window(ARG(0)); }
HLE(SizeWindow) { size_window(ARG(0), (int16_t)ARG(1), (int16_t)ARG(2)); }
HLE(GrowWindow) { RET(0); }
HLE(DrawGrowIcon) { (void)c; }
HLE(GetWRefCon) { RET(ld32(ARG(0) + wr::refCon)); }
HLE(SetWRefCon) { st32(ARG(0) + wr::refCon, ARG(1)); }
HLE(GetWTitle) {
    uint32_t th = ld32(ARG(0) + wr::titleHandle);
    write_pstr(ARG(1), th ? read_pstr(ld32(th)) : std::string());
}
HLE(SetWTitle) {
    uint32_t th = ld32(ARG(0) + wr::titleHandle);
    std::string t = read_pstr(ARG(1));
    handle_set_size(th, (uint32_t)t.size() + 1);
    write_pstr(ld32(th), t);
    restack(region_bbox(ld32(ARG(0) + wr::strucRgn)));
}

HLE(FindWindow) {
    /* short FindWindow(Point thePoint, WindowPtr *theWindow) */
    uint32_t w;
    int part = find_window((int16_t)(ARG(0) >> 16), (int16_t)ARG(0), w);
    st32(ARG(1), w);
    RET(part);
}

HLE(DragWindow) {
    /* void DragWindow(WindowPtr, Point startPt, const Rect *boundsRect) */
    uint32_t w = ARG(0);
    int sv = (int16_t)(ARG(1) >> 16), sh = (int16_t)ARG(1);
    Rect cr = region_bbox(ld32(w + wr::contRgn));
    while (host::mouse_down()) { host::pump(); Sleep(5); }
    int16_t v, h;
    host::mouse_global(v, h);
    if (v != sv || h != sh) move_window(w, cr.left + h - sh, cr.top + v - sv);
    select_window(w);
}

HLE(BeginUpdate) { begin_update(ARG(0)); }
HLE(EndUpdate) {
    uint32_t w = ARG(0);
    set_region(ld32(w + port::visRgn), read_rect(w + port::portRect));
}
static Rect local_to_global_rect(uint32_t w, const Rect &r) {
    Rect c = region_bbox(ld32(w + wr::contRgn));
    Rect pr = read_rect(w + port::portRect);
    return {(int16_t)(r.top - pr.top + c.top), (int16_t)(r.left - pr.left + c.left), (int16_t)(r.bottom - pr.top + c.top),
            (int16_t)(r.right - pr.left + c.left)};
}
HLE(InvalRect) {
    uint32_t w = current_port();
    if (!is_window(w)) return;
    uint32_t ur = ld32(w + wr::updateRgn);
    set_region(ur, rect_union(region_bbox(ur), rect_sect(local_to_global_rect(w, read_rect(ARG(0))), region_bbox(ld32(w + wr::contRgn)))));
}
HLE(ValidRect) {
    uint32_t w = current_port();
    if (!is_window(w)) return;
    uint32_t ur = ld32(w + wr::updateRgn);
    Rect g = local_to_global_rect(w, read_rect(ARG(0)));
    Rect u = region_bbox(ur);
    /* only clear when the valid rect covers the whole update area (rectangular regions) */
    if (g.top <= u.top && g.left <= u.left && g.bottom >= u.bottom && g.right >= u.right) set_region(ur, {0, 0, 0, 0});
}
