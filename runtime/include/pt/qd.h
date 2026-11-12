/* Color QuickDraw internals shared by the qd_*.cpp modules.
 * Record layouts follow Inside Macintosh: Imaging With QuickDraw (1994). */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "pt/runtime.h"

namespace pt::qd {

struct Rect { int16_t top, left, bottom, right; };
inline Rect read_rect(uint32_t a) {
    return {(int16_t)ld16(a), (int16_t)ld16(a + 2), (int16_t)ld16(a + 4), (int16_t)ld16(a + 6)};
}
inline void write_rect(uint32_t a, const Rect &r) {
    st16(a, (uint16_t)r.top); st16(a + 2, (uint16_t)r.left); st16(a + 4, (uint16_t)r.bottom); st16(a + 6, (uint16_t)r.right);
}
inline bool rect_empty(const Rect &r) { return r.bottom <= r.top || r.right <= r.left; }
inline Rect rect_sect(const Rect &a, const Rect &b) {
    Rect r{a.top > b.top ? a.top : b.top, a.left > b.left ? a.left : b.left,
           a.bottom < b.bottom ? a.bottom : b.bottom, a.right < b.right ? a.right : b.right};
    if (rect_empty(r)) r = {0, 0, 0, 0};
    return r;
}
inline Rect rect_union(const Rect &a, const Rect &b) {
    if (rect_empty(a)) return b;
    if (rect_empty(b)) return a;
    return {a.top < b.top ? a.top : b.top, a.left < b.left ? a.left : b.left,
            a.bottom > b.bottom ? a.bottom : b.bottom, a.right > b.right ? a.right : b.right};
}

struct RGB { uint16_t r, g, b; };

/* ---------- record offsets ---------- */
namespace port {   /* CGrafPort / GrafPort (108 bytes) */
enum : uint32_t {
    device = 0, portBits = 2, portPixMap = 2, portVersion = 6, grafVars = 8, chExtra = 12, pnLocHFrac = 14,
    portRect = 16, visRgn = 24, clipRgn = 28, bkPat = 32, bkPixPat = 32, rgbFgColor = 36, rgbBkColor = 42,
    fillPat = 40, pnLoc = 48, pnSize = 52, pnMode = 56, pnPat = 58, pnPixPat = 58, fillPixPat = 62, pnVis = 66,
    txFont = 68, txFace = 70, txMode = 72, txSize = 74, spExtra = 76, fgColor = 80, bkColor = 84, colrBit = 88,
    patStretch = 90, picSave = 92, rgnSave = 96, polySave = 100, grafProcs = 104, size = 108,
};
}
namespace pm {     /* PixMap (50 bytes) */
enum : uint32_t {
    baseAddr = 0, rowBytes = 4, bounds = 6, pmVersion = 14, packType = 16, packSize = 18, hRes = 22, vRes = 26,
    pixelType = 30, pixelSize = 32, cmpCount = 34, cmpSize = 36, planeBytes = 38, pmTable = 42, pmReserved = 46, size = 50,
};
}
namespace gd {     /* GDevice (62 bytes) */
enum : uint32_t {
    gdRefNum = 0, gdID = 2, gdType = 4, gdITable = 6, gdResPref = 10, gdSearchProc = 12, gdCompProc = 16,
    gdFlags = 20, gdPMap = 22, gdRefCon = 26, gdNextGD = 30, gdRect = 34, gdMode = 42, gdCCBytes = 46,
    gdCCDepth = 48, gdCCXData = 50, gdCCXMask = 54, gdReserved = 58, size = 62,
};
}

/* A drawing target resolved from a port: where pixels live and how they map. */
struct Surface {
    uint32_t base = 0;
    uint32_t row_bytes = 0;
    int depth = 1;        /* 1, 8, 16, 32 (2/4 drawn as 8 is not supported) */
    Rect bounds{};        /* local coordinates of pixel (0,0) is bounds.topLeft */
    uint32_t ctab = 0;    /* CTabHandle for indexed depths (0 for 1-bit / direct) */
    bool color_port = false;
    const uint8_t *occl = nullptr;   /* screen ownership map (window ports only) */
    uint8_t owner = 0;               /* this window's id in the map */
};

/* ---------- state ---------- */
extern uint32_t g_qd_theport;    /* guest address of qd.thePort */
uint32_t current_port();
void set_current_port(uint32_t port);
uint32_t current_device();       /* GDHandle */
void set_current_device(uint32_t gdh);
uint32_t main_device();
void set_device_entries(uint32_t gdh, int16_t start, int16_t count, uint32_t colorspecs);

/* ---------- ports / surfaces ---------- */
bool is_color_port(uint32_t port);
Surface surface_of_port(uint32_t port);
Surface surface_of_bitmap(uint32_t bitmap_or_pixmap, bool is_pixmap);   /* BitMap* or PixMap* */
uint32_t port_pixmap(uint32_t port);       /* PixMap* (dereferenced) or 0 */
Rect port_clip(uint32_t port);             /* portRect ∩ visRgn ∩ clipRgn, local coords */
void init_port(uint32_t port, bool color, uint32_t pixmap_handle_or_bitmap_src);

/* ---------- colour ---------- */
uint32_t new_ctab(const std::vector<RGB> &colors, uint32_t seed = 0);   /* CTabHandle */
std::vector<RGB> read_ctab(uint32_t ctab_handle);
uint32_t ctab_seed(uint32_t ctab_handle);
uint32_t next_seed();
uint32_t color_to_index(uint32_t ctab_handle, const RGB &c);
uint32_t rgb_to_pixel(const Surface &s, const RGB &c);                 /* pixel value for depth */
RGB pixel_to_rgb(const Surface &s, uint32_t pixel);
RGB port_fg(uint32_t port);
RGB port_bk(uint32_t port);
const std::vector<RGB> &standard_clut8();

/* ---------- regions (bounding-box approximation) ---------- */
uint32_t new_region(const Rect &r);
Rect region_bbox(uint32_t rgn_handle);
void set_region(uint32_t rgn_handle, const Rect &r);

/* ---------- pixel access ---------- */
uint32_t get_pixel(const Surface &s, int x, int y);   /* x,y in surface-local coords */
void put_pixel_raw(const Surface &s, int x, int y, uint32_t v);
inline bool pixel_visible(const Surface &s, int x, int y) {
    return !s.occl || (unsigned)x >= 640 || (unsigned)y >= 480 || s.occl[y * 640 + x] == s.owner;
}
inline void put_pixel(const Surface &s, int x, int y, uint32_t v) {
    if (pixel_visible(s, x, y)) put_pixel_raw(s, x, y, v);
}

/* pattern fill of a rect (local coords) with transfer mode, clipped */
void fill_rect(uint32_t port, const Rect &r, const uint8_t pat[8], int mode, bool use_fill_colors = false);
void fill_rect_solid(uint32_t port, const Rect &r, const RGB &c);
void pattern_of(uint32_t addr, uint8_t pat[8]);   /* 8-byte Pattern */
extern const uint8_t kBlackPat[8];
extern const uint8_t kWhitePat[8];

/* ---------- blit ---------- */
void copy_bits(const Surface &src, const Surface &dst, Rect sr, Rect dr, int mode, const Rect &clip_dst,
               const Surface *mask = nullptr);

/* ---------- pictures / text ---------- */
void draw_picture(uint32_t pic_handle, const Rect &dst);
void draw_text(uint32_t port, const uint8_t *text, int len);
int text_width(uint32_t port, const uint8_t *text, int len);
void font_info(uint32_t port, int &ascent, int &descent, int &widMax, int &leading);

/* ---------- screen ---------- */
struct Screen {
    uint32_t base;
    int width, height, depth;
    uint32_t row_bytes;
};
Screen screen();
void screen_changed();            /* depth or CLUT changed */
std::vector<RGB> screen_palette();

}  // namespace pt::qd
