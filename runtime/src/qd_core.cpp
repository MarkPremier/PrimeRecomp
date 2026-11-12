/* Color QuickDraw core: graphics devices, ports, GWorlds, colour tables,
 * regions (as rectangles), patterns, transfer modes and the shape primitives. */
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <unordered_map>

#include "pt/qd.h"

namespace pt::qd {

using namespace pt;

uint32_t g_qd_theport = 0;
const uint8_t kBlackPat[8] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
const uint8_t kWhitePat[8] = {0, 0, 0, 0, 0, 0, 0, 0};
static const uint8_t kGrayPat[8] = {0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55};
static const uint8_t kLtGrayPat[8] = {0x88, 0x22, 0x88, 0x22, 0x88, 0x22, 0x88, 0x22};
static const uint8_t kDkGrayPat[8] = {0x77, 0xDD, 0x77, 0xDD, 0x77, 0xDD, 0x77, 0xDD};

static uint32_t g_cur_device = 0, g_main_device = 0;
static uint32_t g_fallback_port = 0;   /* used while thePort is still NULL */
static uint32_t g_seed = 1024;
static uint32_t g_gray_rgn = 0;

static const int kScreenW = 640, kScreenH = 480;
static uint32_t g_fb = 0;               /* framebuffer (sized for 32 bpp) */

uint32_t next_seed() { return ++g_seed; }

/* ---------- standard 8-bit palette (Apple's default 256-colour CLUT) ---------- */
const std::vector<RGB> &standard_clut8() {
    static std::vector<RGB> t;
    if (t.empty()) {
        static const uint8_t ramp[10] = {0xEE, 0xDD, 0xBB, 0xAA, 0x88, 0x77, 0x55, 0x44, 0x22, 0x11};
        for (int i = 0; i < 215; i++) {
            uint16_t r = (uint16_t)((5 - i / 36) * 0x3333), g = (uint16_t)((5 - (i / 6) % 6) * 0x3333),
                     b = (uint16_t)((5 - i % 6) * 0x3333);
            t.push_back({r, g, b});
        }
        for (int c = 0; c < 4; c++)
            for (int i = 0; i < 10; i++) {
                uint16_t v = (uint16_t)(ramp[i] * 0x101);
                t.push_back(c == 0 ? RGB{v, 0, 0} : c == 1 ? RGB{0, v, 0} : c == 2 ? RGB{0, 0, v} : RGB{v, v, v});
            }
        t.push_back({0, 0, 0});
    }
    return t;
}

/* ---------- colour tables ---------- */
uint32_t new_ctab(const std::vector<RGB> &colors, uint32_t seed) {
    uint32_t h = handle_new(8 + 8 * (uint32_t)colors.size(), true);
    uint32_t p = ld32(h);
    st32(p, seed ? seed : next_seed());
    st16(p + 4, 0);
    st16(p + 6, (uint16_t)(colors.size() - 1));
    for (size_t i = 0; i < colors.size(); i++) {
        uint32_t e = p + 8 + 8 * (uint32_t)i;
        st16(e, (uint16_t)i);
        st16(e + 2, colors[i].r); st16(e + 4, colors[i].g); st16(e + 6, colors[i].b);
    }
    return h;
}

std::vector<RGB> read_ctab(uint32_t h) {
    std::vector<RGB> out;
    if (!h || !ld32(h)) return out;
    uint32_t p = ld32(h);
    int n = (int16_t)ld16(p + 6) + 1;
    bool device = (ld16(p + 4) & 0x8000) != 0;  /* ctFlags high bit: indices are implicit */
    out.resize(n > 0 && n <= 256 ? 256 > n ? n : 256 : 0);
    for (int i = 0; i < n && i < 256; i++) {
        uint32_t e = p + 8 + 8 * i;
        int idx = device ? i : (ld16(e) & 0xFF);
        if (idx >= (int)out.size()) out.resize(idx + 1);
        out[idx] = {ld16(e + 2), ld16(e + 4), ld16(e + 6)};
    }
    return out;
}

uint32_t ctab_seed(uint32_t h) { return h && ld32(h) ? ld32(ld32(h)) : 0; }

uint32_t color_to_index(uint32_t ctab, const RGB &c) {
    static std::map<std::pair<uint32_t, uint64_t>, uint8_t> cache;
    uint32_t seed = ctab_seed(ctab);
    uint64_t key = ((uint64_t)c.r << 32) | ((uint64_t)c.g << 16) | c.b;
    auto it = cache.find({seed ^ ctab, key});
    if (it != cache.end()) return it->second;
    auto t = read_ctab(ctab);
    if (t.empty()) return 0;
    int best = 0;
    int64_t bd = INT64_MAX;
    for (size_t i = 0; i < t.size(); i++) {
        int64_t dr = (int64_t)t[i].r - c.r, dg = (int64_t)t[i].g - c.g, db = (int64_t)t[i].b - c.b;
        int64_t d = dr * dr + dg * dg + db * db;
        if (d < bd) { bd = d; best = (int)i; }
    }
    if (cache.size() > 200000) cache.clear();
    cache[{seed ^ ctab, key}] = (uint8_t)best;
    return (uint32_t)best;
}

uint32_t rgb_to_pixel(const Surface &s, const RGB &c) {
    switch (s.depth) {
    case 1: return (c.r + c.g + c.b) / 3 < 0x8000 ? 1 : 0;
    case 16: return ((c.r >> 11) << 10) | ((c.g >> 11) << 5) | (c.b >> 11);
    case 32: return ((uint32_t)(c.r >> 8) << 16) | ((uint32_t)(c.g >> 8) << 8) | (c.b >> 8);
    default: return color_to_index(s.ctab, c) & ((1u << s.depth) - 1);
    }
}

RGB pixel_to_rgb(const Surface &s, uint32_t v) {
    switch (s.depth) {
    case 1: return v ? RGB{0, 0, 0} : RGB{0xFFFF, 0xFFFF, 0xFFFF};
    case 16: {
        auto x = [](uint32_t f) { return (uint16_t)((f << 11) | (f << 6) | (f << 1) | (f >> 4)); };
        return {x((v >> 10) & 31), x((v >> 5) & 31), x(v & 31)};
    }
    case 32: return {(uint16_t)(((v >> 16) & 0xFF) * 0x101), (uint16_t)(((v >> 8) & 0xFF) * 0x101), (uint16_t)((v & 0xFF) * 0x101)};
    default: {
        auto t = read_ctab(s.ctab);
        return v < t.size() ? t[v] : RGB{0, 0, 0};
    }
    }
}

/* ---------- regions: a 10-byte rectangular region per handle ---------- */
uint32_t new_region(const Rect &r) {
    uint32_t h = handle_new(10, true);
    set_region(h, r);
    return h;
}
Rect region_bbox(uint32_t h) {
    if (!h || !ld32(h)) return {0, 0, 0, 0};
    return read_rect(ld32(h) + 2);
}
void set_region(uint32_t h, const Rect &r) {
    if (!h || !ld32(h)) return;
    Rect n = rect_empty(r) ? Rect{0, 0, 0, 0} : r;
    st16(ld32(h), 10);
    write_rect(ld32(h) + 2, n);
}

/* ---------- devices ---------- */
static uint32_t make_pixmap_handle(uint32_t base, int depth, const Rect &bounds, uint32_t ctab) {
    uint32_t h = handle_new(pm::size, true);
    uint32_t p = ld32(h);
    int w = bounds.right - bounds.left;
    uint32_t rb = ((uint32_t)w * depth + 31) / 32 * 4;
    st32(p + pm::baseAddr, base);
    st16(p + pm::rowBytes, (uint16_t)(rb | 0x8000));
    write_rect(p + pm::bounds, bounds);
    st16(p + pm::pmVersion, 0);
    st32(p + pm::hRes, 72 << 16);
    st32(p + pm::vRes, 72 << 16);
    st16(p + pm::pixelType, depth > 8 ? 16 : 0);
    st16(p + pm::pixelSize, (uint16_t)depth);
    st16(p + pm::cmpCount, depth > 8 ? 3 : 1);
    st16(p + pm::cmpSize, depth == 16 ? 5 : depth == 32 ? 8 : depth);
    st32(p + pm::pmTable, ctab);
    return h;
}

static void set_pixmap_depth(uint32_t p, int depth, uint32_t ctab) {
    Rect b = read_rect(p + pm::bounds);
    uint32_t rb = ((uint32_t)(b.right - b.left) * depth + 31) / 32 * 4;
    st16(p + pm::rowBytes, (uint16_t)(rb | 0x8000));
    st16(p + pm::pixelType, depth > 8 ? 16 : 0);
    st16(p + pm::pixelSize, (uint16_t)depth);
    st16(p + pm::cmpCount, depth > 8 ? 3 : 1);
    st16(p + pm::cmpSize, depth == 16 ? 5 : depth == 32 ? 8 : depth);
    st32(p + pm::pmTable, ctab);
}

static std::set<uint32_t> g_screen_pixmaps;   /* PixMap* copies that alias the framebuffer */

static void ensure_devices() {
    if (g_main_device) return;
    g_fb = sys_alloc(kScreenW * kScreenH * 4, 4096);
    Rect r{0, 0, kScreenH, kScreenW};
    uint32_t ctab = new_ctab(standard_clut8(), 8);
    st16(ld32(ctab) + 4, 0x8000);  /* device colour table */
    uint32_t pmh = make_pixmap_handle(g_fb, 8, r, ctab);
    g_screen_pixmaps.insert(ld32(pmh));
    uint32_t gdh = handle_new(gd::size, true);
    uint32_t g = ld32(gdh);
    st16(g + gd::gdRefNum, (uint16_t)-55);
    st16(g + gd::gdType, 0);                   /* clutType */
    uint32_t itab = handle_new(16, true);
    st32(g + gd::gdITable, itab);
    st16(g + gd::gdResPref, 4);
    st16(g + gd::gdFlags, 0x8000 | 0x2000 | 0x0800 | 0x0400 | 0x0001);
    st32(g + gd::gdPMap, pmh);
    write_rect(g + gd::gdRect, r);
    st32(g + gd::gdMode, 0x80);
    g_main_device = g_cur_device = gdh;
    st32(0x08A4, gdh);   /* MainDevice */
    st32(0x08A8, gdh);   /* DeviceList */
    st32(0x0CC8, gdh);   /* TheGDevice */
    g_gray_rgn = new_region({20, 0, kScreenH, kScreenW});
    st32(0x09EE, g_gray_rgn);
}

uint32_t main_device() { ensure_devices(); return g_main_device; }
uint32_t current_device() { ensure_devices(); return g_cur_device; }
void set_current_device(uint32_t gdh) { if (gdh) { g_cur_device = gdh; st32(0x0CC8, gdh); } }

Screen screen() {
    ensure_devices();
    uint32_t p = ld32(ld32(ld32(g_main_device) + gd::gdPMap));
    return {g_fb, kScreenW, kScreenH, (int)ld16(p + pm::pixelSize), (uint32_t)(ld16(p + pm::rowBytes) & 0x3FFF)};
}

std::vector<RGB> screen_palette() {
    ensure_devices();
    uint32_t p = ld32(ld32(ld32(g_main_device) + gd::gdPMap));
    return read_ctab(ld32(p + pm::pmTable));
}

void screen_changed() {}

/* ---------- ports ---------- */
uint32_t current_port() {
    uint32_t p = g_qd_theport ? ld32(g_qd_theport) : 0;
    return p ? p : g_fallback_port;
}
void set_current_port(uint32_t port) {
    if (g_qd_theport) st32(g_qd_theport, port);
    st32(0x0D82 /* unused low-mem slot mirrors thePort for debugging */, port);
}

bool is_color_port(uint32_t port) { return (ld16(port + port::portVersion) & 0xC000) == 0xC000; }

uint32_t port_pixmap(uint32_t port) {
    if (!is_color_port(port)) return 0;
    uint32_t h = ld32(port + port::portPixMap);
    return h ? ld32(h) : 0;
}

Surface surface_of_bitmap(uint32_t a, bool is_pixmap) {
    Surface s;
    s.base = ld32(a + pm::baseAddr);
    uint16_t rb = ld16(a + pm::rowBytes);
    s.row_bytes = rb & 0x3FFF;
    s.bounds = read_rect(a + pm::bounds);
    if (is_pixmap || (rb & 0x8000)) {
        s.depth = ld16(a + pm::pixelSize);
        s.ctab = ld32(a + pm::pmTable);
        s.color_port = true;
    } else {
        s.depth = 1;
    }
    return s;
}

}  // namespace pt::qd
namespace pt::wm { bool occlusion_for(uint32_t port, const uint8_t *&map, uint8_t &owner); }
namespace pt::qd {

Surface surface_of_port(uint32_t port) {
    Surface s;
    if (is_color_port(port)) {
        s = surface_of_bitmap(port_pixmap(port), true);
        s.color_port = true;
    } else {
        s = surface_of_bitmap(port + port::portBits, false);
    }
    if (s.base == g_fb) wm::occlusion_for(port, s.occl, s.owner);
    return s;
}

Rect port_clip(uint32_t port) {
    Rect r = read_rect(port + port::portRect);
    r = rect_sect(r, region_bbox(ld32(port + port::visRgn)));
    r = rect_sect(r, region_bbox(ld32(port + port::clipRgn)));
    return r;
}

static uint32_t new_pixpat(const uint8_t pat[8]) {
    /* PixPat: patType(2) patMap(4) patData(4) patXData(4) patXValid(2) patXMap(4) pat1Data(8) */
    uint32_t h = handle_new(28, true);
    st16(ld32(h), 0);
    memcpy(g_mem + ld32(h) + 20, pat, 8);
    return h;
}

void init_port(uint32_t port, bool color, uint32_t pmh_or_bitmap) {
    ensure_devices();
    memset(g_mem + port, 0, port::size);
    Rect scr{0, 0, kScreenH, kScreenW};
    if (color) {
        uint32_t pmh = pmh_or_bitmap;
        if (!pmh) {
            /* copy of the current device's pixmap, as OpenCPort does */
            uint32_t src = ld32(ld32(ld32(g_cur_device) + gd::gdPMap));
            pmh = handle_new(pm::size, false);
            memcpy(g_mem + ld32(pmh), g_mem + src, pm::size);
            if (ld32(src + pm::baseAddr) == g_fb) g_screen_pixmaps.insert(ld32(pmh));
        }
        st32(port + port::portPixMap, pmh);
        st16(port + port::portVersion, 0xC000);
        Rect b = read_rect(ld32(pmh) + pm::bounds);
        write_rect(port + port::portRect, b);
        scr = b;
        st32(port + port::bkPixPat, new_pixpat(kWhitePat));
        st32(port + port::pnPixPat, new_pixpat(kBlackPat));
        st32(port + port::fillPixPat, new_pixpat(kBlackPat));
        st16(port + port::rgbFgColor, 0); st16(port + port::rgbFgColor + 2, 0); st16(port + port::rgbFgColor + 4, 0);
        st16(port + port::rgbBkColor, 0xFFFF); st16(port + port::rgbBkColor + 2, 0xFFFF); st16(port + port::rgbBkColor + 4, 0xFFFF);
    } else {
        /* BitMap port on the screen (1-bit view) */
        Screen sc = screen();
        st32(port + port::portBits, sc.base);
        st16(port + port::portBits + 4, (uint16_t)sc.row_bytes);
        write_rect(port + port::portBits + 6, scr);
        write_rect(port + port::portRect, scr);
        memcpy(g_mem + port + port::bkPat, kWhitePat, 8);
        memcpy(g_mem + port + port::fillPat, kBlackPat, 8);
        memcpy(g_mem + port + port::pnPat, kBlackPat, 8);
    }
    st32(port + port::visRgn, new_region(scr));
    st32(port + port::clipRgn, new_region({-32768, -32768, 32767, 32767}));
    st32(port + port::pnSize, 0x00010001);
    st16(port + port::pnMode, 8);   /* patCopy */
    st16(port + port::txMode, 1);   /* srcOr */
    st32(port + port::fgColor, 33); /* blackColor */
    st32(port + port::bkColor, 30); /* whiteColor */
    st16(port + port::colrBit, 0);
}

static void ensure_fallback_port() {
    if (!g_fallback_port) {
        g_fallback_port = sys_alloc(port::size, 4);
        init_port(g_fallback_port, true, 0);
    }
}

RGB port_fg(uint32_t port) {
    if (is_color_port(port))
        return {ld16(port + port::rgbFgColor), ld16(port + port::rgbFgColor + 2), ld16(port + port::rgbFgColor + 4)};
    return ld32(port + port::fgColor) == 30 ? RGB{0xFFFF, 0xFFFF, 0xFFFF} : RGB{0, 0, 0};
}
RGB port_bk(uint32_t port) {
    if (is_color_port(port))
        return {ld16(port + port::rgbBkColor), ld16(port + port::rgbBkColor + 2), ld16(port + port::rgbBkColor + 4)};
    return ld32(port + port::bkColor) == 33 ? RGB{0, 0, 0} : RGB{0xFFFF, 0xFFFF, 0xFFFF};
}

/* ---------- pixels ---------- */
uint32_t get_pixel(const Surface &s, int x, int y) {
    uint32_t row = s.base + (uint32_t)y * s.row_bytes;
    switch (s.depth) {
    case 1: return (g_mem[row + (x >> 3)] >> (7 - (x & 7))) & 1;
    case 2: return (g_mem[row + (x >> 2)] >> (6 - 2 * (x & 3))) & 3;
    case 4: return (g_mem[row + (x >> 1)] >> (x & 1 ? 0 : 4)) & 15;
    case 8: return g_mem[row + x];
    case 16: return ld16(row + 2 * x);
    default: return ld32(row + 4 * x) & 0xFFFFFF;
    }
}

void put_pixel_raw(const Surface &s, int x, int y, uint32_t v) {
    uint32_t row = s.base + (uint32_t)y * s.row_bytes;
    switch (s.depth) {
    case 1: {
        uint8_t &b = g_mem[row + (x >> 3)];
        uint8_t m = (uint8_t)(0x80 >> (x & 7));
        b = v ? (b | m) : (b & ~m);
        break;
    }
    case 2: { uint8_t &b = g_mem[row + (x >> 2)]; int sh = 6 - 2 * (x & 3); b = (uint8_t)((b & ~(3 << sh)) | ((v & 3) << sh)); break; }
    case 4: { uint8_t &b = g_mem[row + (x >> 1)]; int sh = x & 1 ? 0 : 4; b = (uint8_t)((b & ~(15 << sh)) | ((v & 15) << sh)); break; }
    case 8: g_mem[row + x] = (uint8_t)v; break;
    case 16: st16(row + 2 * x, v); break;
    default: st32(row + 4 * x, v); break;
    }
}

void pattern_of(uint32_t a, uint8_t pat[8]) { memcpy(pat, g_mem + a, 8); }

static void port_pattern(uint32_t port, uint32_t field_color, uint32_t field_bw, uint8_t pat[8]) {
    if (is_color_port(port)) {
        uint32_t h = ld32(port + field_color);
        if (h && ld32(h)) { memcpy(pat, g_mem + ld32(h) + 20, 8); return; }
        memcpy(pat, kBlackPat, 8);
        return;
    }
    memcpy(pat, g_mem + port + field_bw, 8);
}

/* transfer mode applied to one pixel */
static inline uint32_t apply_mode(int mode, bool bit, uint32_t dst, uint32_t fg, uint32_t bk, uint32_t allones) {
    switch (mode & 7) {
    case 0: return bit ? fg : bk;                  /* copy */
    case 1: return bit ? fg : dst;                 /* or */
    case 2: return bit ? (dst ^ allones) : dst;    /* xor: invert */
    case 3: return bit ? bk : dst;                 /* bic */
    case 4: return bit ? bk : fg;                  /* notCopy */
    case 5: return bit ? dst : fg;                 /* notOr */
    case 6: return bit ? dst : (dst ^ allones);    /* notXor */
    default: return bit ? dst : bk;                /* notBic */
    }
}

void fill_rect(uint32_t port, const Rect &r, const uint8_t pat[8], int mode, bool) {
    Surface s = surface_of_port(port);
    Rect c = rect_sect(r, port_clip(port));
    c = rect_sect(c, s.bounds);
    if (rect_empty(c)) return;
    uint32_t fg = rgb_to_pixel(s, port_fg(port)), bk = rgb_to_pixel(s, port_bk(port));
    uint32_t allones = s.depth >= 32 ? 0xFFFFFF : (1u << s.depth) - 1;
    int m = mode >= 8 && mode < 16 ? mode - 8 : (mode < 8 ? mode : 0);
    bool solid = memcmp(pat, kBlackPat, 8) == 0 && m == 0;
    for (int y = c.top; y < c.bottom; y++) {
        int py = y - s.bounds.top;
        uint8_t row = pat[py & 7];
        for (int x = c.left; x < c.right; x++) {
            int px = x - s.bounds.left;
            if (solid) { put_pixel(s, px, py, fg); continue; }
            bool bit = (row >> (7 - (px & 7))) & 1;
            uint32_t d = (m == 0 || m == 4) ? 0 : get_pixel(s, px, py);
            put_pixel(s, px, py, apply_mode(m, bit, d, fg, bk, allones));
        }
    }
}

void fill_rect_solid(uint32_t port, const Rect &r, const RGB &col) {
    Surface s = surface_of_port(port);
    Rect c = rect_sect(rect_sect(r, port_clip(port)), s.bounds);
    uint32_t v = rgb_to_pixel(s, col);
    for (int y = c.top; y < c.bottom; y++)
        for (int x = c.left; x < c.right; x++) put_pixel(s, x - s.bounds.left, y - s.bounds.top, v);
}

/* span fill used by ovals and polygons: [x0, x1) on row y */
static void fill_span(uint32_t port, int y, int x0, int x1, const uint8_t pat[8], int mode) {
    if (x1 > x0) fill_rect(port, {(int16_t)y, (int16_t)x0, (int16_t)(y + 1), (int16_t)x1}, pat, mode);
}

/* ---------- GWorlds ---------- */
struct GWorldInfo { uint32_t pixels; uint32_t gdh; int depth; };
static std::unordered_map<uint32_t, GWorldInfo> g_gworlds;   /* port -> info */

static int32_t make_gworld(uint32_t &out_port, int depth, const Rect &bounds, uint32_t ctab) {
    ensure_devices();
    if (depth == 0) depth = screen().depth;
    int w = bounds.right - bounds.left, h = bounds.bottom - bounds.top;
    if (w <= 0 || h <= 0) return -50;
    uint32_t rb = ((uint32_t)w * depth + 31) / 32 * 4;
    uint32_t pixels = guest_alloc(rb * h, true);
    if (!pixels) return -108;
    uint32_t tab;
    if (depth <= 8) {
        if (ctab) {
            uint32_t sz = handle_size(ctab);
            tab = handle_new(sz, false);
            memcpy(g_mem + ld32(tab), g_mem + ld32(ctab), sz);
        } else if (depth == 8) {
            /* default: the screen's current table */
            uint32_t sp = ld32(ld32(ld32(g_main_device) + gd::gdPMap));
            uint32_t st = ld32(sp + pm::pmTable);
            tab = new_ctab(read_ctab(st), ctab_seed(st));
        } else {
            std::vector<RGB> gray;
            int n = 1 << depth;
            for (int i = 0; i < n; i++) { uint16_t v = (uint16_t)(0xFFFF - i * 0xFFFF / (n - 1)); gray.push_back({v, v, v}); }
            tab = new_ctab(gray);
        }
    } else {
        tab = new_ctab({{0, 0, 0}});
    }
    uint32_t pmh = make_pixmap_handle(pixels, depth, bounds, tab);
    uint32_t gdh = handle_new(gd::size, true);
    uint32_t g = ld32(gdh);
    st16(g + gd::gdType, depth > 8 ? 2 : 0);
    st32(g + gd::gdITable, handle_new(16, true));
    st16(g + gd::gdFlags, 0x0001 | 0x0400);
    st32(g + gd::gdPMap, pmh);
    write_rect(g + gd::gdRect, bounds);
    uint32_t port = guest_alloc(port::size + 64, true);
    init_port(port, true, pmh);
    st16(port + port::portVersion, 0xC001);   /* GWorld flag */
    g_gworlds[port] = {pixels, gdh, depth};
    out_port = port;
    logf(LOG_GFX, "NewGWorld %dx%d depth %d -> port %08X pixels %08X\n", w, h, depth, port, pixels);
    return 0;
}

static void dispose_gworld(uint32_t port) {
    auto it = g_gworlds.find(port);
    if (it == g_gworlds.end()) return;
    guest_free(it->second.pixels);
    g_gworlds.erase(it);
    guest_free(port);
}

void set_device_entries(uint32_t gdh, int16_t start, int16_t count, uint32_t tbl) {
    uint32_t pmp = ld32(ld32(ld32(gdh) + gd::gdPMap));
    uint32_t ct = ld32(pmp + pm::pmTable);
    if (!ct) return;
    uint32_t p = ld32(ct);
    int n = (int16_t)ld16(p + 6) + 1;
    for (int i = 0; i <= count; i++) {
        uint32_t e = tbl + 8 * i;
        int idx = start >= 0 ? start + i : (ld16(e) & 0xFF);
        if (idx < 0 || idx >= n) continue;
        uint32_t d = p + 8 + 8 * idx;
        st16(d, (uint16_t)idx);
        st16(d + 2, ld16(e + 2)); st16(d + 4, ld16(e + 4)); st16(d + 6, ld16(e + 6));
    }
    st32(p, next_seed());
}

bool is_gworld(uint32_t port) { return g_gworlds.count(port) != 0; }
uint32_t gworld_device(uint32_t port) {
    auto it = g_gworlds.find(port);
    return it == g_gworlds.end() ? 0 : it->second.gdh;
}

}  // namespace pt::qd

/* ======================================================================== */
using namespace pt;
using namespace pt::qd;

static uint32_t cur() {
    uint32_t p = current_port();
    if (!p) fatal("QuickDraw call with no current port");
    return p;
}

/* ---------- initialisation ---------- */
HLE(InitGraf) {
    /* InitGraf(&qd.thePort): QDGlobals lie *below* thePort */
    uint32_t tp = ARG(0);
    g_qd_theport = tp;
    Screen sc = screen();
    memcpy(g_mem + tp - 8, kWhitePat, 8);       /* white */
    memcpy(g_mem + tp - 16, kBlackPat, 8);      /* black */
    memcpy(g_mem + tp - 24, kGrayPat, 8);       /* gray */
    memcpy(g_mem + tp - 32, kLtGrayPat, 8);     /* ltGray */
    memcpy(g_mem + tp - 40, kDkGrayPat, 8);     /* dkGray */
    /* arrow cursor (68 bytes) at tp-108: data, mask, hotSpot */
    static const uint16_t arrow[16] = {0x0000, 0x4000, 0x6000, 0x7000, 0x7800, 0x7C00, 0x7E00, 0x7F00,
                                       0x7F80, 0x7C00, 0x6C00, 0x4600, 0x0600, 0x0300, 0x0300, 0x0000};
    static const uint16_t amask[16] = {0xC000, 0xE000, 0xF000, 0xF800, 0xFC00, 0xFE00, 0xFF00, 0xFF80,
                                       0xFFC0, 0xFFE0, 0xFE00, 0xEF00, 0xCF00, 0x8780, 0x0780, 0x0380};
    for (int i = 0; i < 16; i++) { st16(tp - 108 + 2 * i, arrow[i]); st16(tp - 108 + 32 + 2 * i, amask[i]); }
    st32(tp - 108 + 64, 0x00010001);
    uint32_t sb = tp - 122;                      /* screenBits */
    st32(sb, sc.base);
    st16(sb + 4, (uint16_t)sc.row_bytes);
    write_rect(sb + 6, {0, 0, (int16_t)sc.height, (int16_t)sc.width});
    st32(tp - 126, 1);                           /* randSeed */
    ensure_fallback_port();
    st32(tp, g_fallback_port);
}

HLE(OpenCPort) { init_port(ARG(0), true, 0); set_current_port(ARG(0)); }
HLE(SetPort) { set_current_port(ARG(0)); }
HLE(GetPort) { st32(ARG(0), current_port()); }

/* ---------- devices ---------- */
HLE(GetMainDevice) { RET(main_device()); }
HLE(GetDeviceList) { RET(main_device()); }
HLE(GetNextDevice) { RET(ld32(ld32(ARG(0)) + gd::gdNextGD)); }
HLE(GetMaxDevice) { RET(main_device()); }
HLE(GetGDevice) { RET(current_device()); }
HLE(SetGDevice) { set_current_device(ARG(0)); }
HLE(DisposeGDevice) { (void)c; }
HLE(TestDeviceAttribute) {
    uint32_t g = ld32(ARG(0));
    RET((ld16(g + gd::gdFlags) >> (ARG(1) & 15)) & 1);
}
HLE(HasDepth) {
    /* short HasDepth(GDHandle, short depth, short whichFlags, short flags): returns mode or 0 */
    int d = (int16_t)ARG(1);
    RET(d == 1 || d == 8 || d == 16 || d == 32 ? 0x80 + (d == 8 ? 3 : d == 16 ? 4 : d == 32 ? 5 : 0) : 0);
}
HLE(SetDepth) {
    /* OSErr SetDepth(GDHandle gd, short depth, short whichFlags, short flags) */
    /* the depth argument may be a pixel depth or a video mode (0x80 + n) */
    static const int kModeDepth[6] = {1, 2, 4, 8, 16, 32};
    int arg = (int16_t)ARG(1);
    int depth = arg >= 0x80 && arg < 0x86 ? kModeDepth[arg - 0x80] : arg ? arg : 8;
    uint32_t gdh = ARG(0);
    uint32_t g = ld32(gdh);
    uint32_t pmp = ld32(ld32(g + gd::gdPMap));
    uint32_t tab = ld32(pmp + pm::pmTable);
    if (depth == 8) {
        uint32_t nt = new_ctab(standard_clut8(), 8);
        st16(ld32(nt) + 4, 0x8000);
        tab = nt;
    }
    set_pixmap_depth(pmp, depth, tab);
    st16(g + gd::gdType, depth > 8 ? 2 : 0);
    for (uint32_t p : g_screen_pixmaps) {
        Rect b = read_rect(p + pm::bounds);
        set_pixmap_depth(p, depth, tab);
        write_rect(p + pm::bounds, b);
        uint32_t rb = (uint32_t)kScreenW * depth / 8;
        st16(p + pm::rowBytes, (uint16_t)(rb | 0x8000));
    }
    if (g_qd_theport) st16(g_qd_theport - 122 + 4, (uint16_t)(kScreenW * depth / 8));
    memset(g_mem + g_fb, 0, kScreenW * kScreenH * 4);
    logf(LOG_GFX, "SetDepth %d\n", depth);
    RET(0);
}
HLE(SetEntries) {
    /* SetEntries(short start, short count, CSpecArray aTable): modifies the current device's CLUT */
    set_device_entries(current_device(), (int16_t)ARG(0), (int16_t)ARG(1), ARG(2));
}
HLE(CTabChanged) { if (ARG(0) && ld32(ARG(0))) st32(ld32(ARG(0)), next_seed()); }
HLE(GetCTSeed) { RET(next_seed()); }
HLE(MakeITable) { (void)c; }
HLE(DisposeCTable) { handle_dispose(ARG(0)); }
HLE(GetGrayRgn) { main_device(); RET(ld32(0x09EE)); }

/* ---------- GWorlds ---------- */
HLE(NewGWorld) {
    /* QDErr NewGWorld(GWorldPtr *offscreenGWorld, short depth, const Rect *boundsRect, CTabHandle cTable, GDHandle aGDevice, GWorldFlags flags) */
    uint32_t port = 0;
    int32_t err = make_gworld(port, (int16_t)ARG(1), read_rect(ARG(2)), ARG(3));
    if (!err) st32(ARG(0), port);
    RET(err);
}
HLE(UpdateGWorld) {
    /* GWorldFlags UpdateGWorld(GWorldPtr *gw, short depth, const Rect *bounds, CTabHandle, GDHandle, GWorldFlags) */
    uint32_t old = ld32(ARG(0));
    Rect nb = read_rect(ARG(2));
    int depth = (int16_t)ARG(1);
    if (old && is_gworld(old)) {
        Surface s = surface_of_port(old);
        Rect ob = s.bounds;
        if ((depth == 0 || depth == s.depth) && ob.right - ob.left == nb.right - nb.left && ob.bottom - ob.top == nb.bottom - nb.top) {
            RET(0);
            return;
        }
    }
    uint32_t port = 0;
    int32_t err = make_gworld(port, depth, nb, ARG(3));
    if (err) { RET(err); return; }
    if (old) dispose_gworld(old);
    st32(ARG(0), port);
    RET(1 << 1); /* reallocPix */
}
HLE(DisposeGWorld) { dispose_gworld(ARG(0)); }
HLE(GetGWorld) {
    st32(ARG(0), current_port());
    st32(ARG(1), current_device());
}
HLE(SetGWorld) {
    set_current_port(ARG(0));
    uint32_t gdh = gworld_device(ARG(0));
    set_current_device(gdh ? gdh : (ARG(1) ? ARG(1) : main_device()));
}
HLE(GetGWorldPixMap) { RET(ld32(ARG(0) + port::portPixMap)); }
HLE(LockPixels) { RET(1); }
HLE(UnlockPixels) { (void)c; }
HLE(GetPixBaseAddr) { RET(ld32(ld32(ARG(0)) + pm::baseAddr)); }
HLE(NewPixMap) { RET(make_pixmap_handle(0, 8, {0, 0, 0, 0}, 0)); }

/* ---------- regions ---------- */
HLE(NewRgn) { RET(new_region({0, 0, 0, 0})); }
HLE(DisposeRgn) { handle_dispose(ARG(0)); }
HLE(SetRectRgn) { set_region(ARG(0), {(int16_t)ARG(2), (int16_t)ARG(1), (int16_t)ARG(4), (int16_t)ARG(3)}); }
HLE(RectRgn) { set_region(ARG(0), read_rect(ARG(1))); }
HLE(CopyRgn) { set_region(ARG(1), region_bbox(ARG(0))); }
HLE(SectRgn) { set_region(ARG(2), rect_sect(region_bbox(ARG(0)), region_bbox(ARG(1)))); }
HLE(UnionRgn) { set_region(ARG(2), rect_union(region_bbox(ARG(0)), region_bbox(ARG(1)))); }
HLE(EmptyRgn) { RET(rect_empty(region_bbox(ARG(0)))); }
static uint32_t g_open_rgn = 0;
static Rect g_open_rgn_box{0, 0, 0, 0};
HLE(OpenRgn) { g_open_rgn = 1; g_open_rgn_box = {0, 0, 0, 0}; }
HLE(CloseRgn) { set_region(ARG(0), g_open_rgn_box); g_open_rgn = 0; }
HLE(FillRgn) {
    uint8_t pat[8];
    pattern_of(ARG(1), pat);
    fill_rect(cur(), region_bbox(ARG(0)), pat, 8);
}

/* ---------- rectangles / points ---------- */
HLE(SetRect) { write_rect(ARG(0), {(int16_t)ARG(2), (int16_t)ARG(1), (int16_t)ARG(4), (int16_t)ARG(3)}); }
HLE(OffsetRect) {
    Rect r = read_rect(ARG(0));
    int16_t dh = (int16_t)ARG(1), dv = (int16_t)ARG(2);
    write_rect(ARG(0), {(int16_t)(r.top + dv), (int16_t)(r.left + dh), (int16_t)(r.bottom + dv), (int16_t)(r.right + dh)});
}
HLE(InsetRect) {
    Rect r = read_rect(ARG(0));
    int16_t dh = (int16_t)ARG(1), dv = (int16_t)ARG(2);
    Rect n{(int16_t)(r.top + dv), (int16_t)(r.left + dh), (int16_t)(r.bottom - dv), (int16_t)(r.right - dh)};
    if (rect_empty(n)) n = {0, 0, 0, 0};
    write_rect(ARG(0), n);
}
HLE(SectRect) {
    Rect r = rect_sect(read_rect(ARG(0)), read_rect(ARG(1)));
    write_rect(ARG(2), r);
    RET(!rect_empty(r));
}
HLE(PtInRect) {
    /* Boolean PtInRect(Point pt, const Rect *r): Point passed by value as (v<<16)|h */
    int16_t v = (int16_t)(ARG(0) >> 16), h = (int16_t)ARG(0);
    Rect r = read_rect(ARG(1));
    RET(v >= r.top && v < r.bottom && h >= r.left && h < r.right);
}
HLE(SetPt) { st16(ARG(0), (uint16_t)ARG(2)); st16(ARG(0) + 2, (uint16_t)ARG(1)); }

static void port_offset(uint32_t port, int &dh, int &dv) {
    Surface s = surface_of_port(port);
    dh = -s.bounds.left;
    dv = -s.bounds.top;
}
HLE(LocalToGlobal) {
    int dh, dv;
    port_offset(cur(), dh, dv);
    st16(ARG(0), (uint16_t)((int16_t)ld16(ARG(0)) + dv));
    st16(ARG(0) + 2, (uint16_t)((int16_t)ld16(ARG(0) + 2) + dh));
}
HLE(GlobalToLocal) {
    int dh, dv;
    port_offset(cur(), dh, dv);
    st16(ARG(0), (uint16_t)((int16_t)ld16(ARG(0)) - dv));
    st16(ARG(0) + 2, (uint16_t)((int16_t)ld16(ARG(0) + 2) - dh));
}
HLE(SetOrigin) {
    uint32_t p = cur();
    Rect pr = read_rect(p + port::portRect);
    int16_t dh = (int16_t)(ARG(0) - pr.left), dv = (int16_t)(ARG(1) - pr.top);
    auto shift = [&](uint32_t a) {
        Rect r = read_rect(a);
        write_rect(a, {(int16_t)(r.top + dv), (int16_t)(r.left + dh), (int16_t)(r.bottom + dv), (int16_t)(r.right + dh)});
    };
    shift(p + port::portRect);
    if (is_color_port(p)) shift(port_pixmap(p) + pm::bounds);
    else shift(p + port::portBits + 6);
    uint32_t vr = ld32(p + port::visRgn);
    if (vr) { Rect b = region_bbox(vr); set_region(vr, {(int16_t)(b.top + dv), (int16_t)(b.left + dh), (int16_t)(b.bottom + dv), (int16_t)(b.right + dh)}); }
}
HLE(ClipRect) { set_region(ld32(cur() + port::clipRgn), read_rect(ARG(0))); }

/* ---------- colour state ---------- */
static void set_rgb(uint32_t port, uint32_t field, uint32_t index_field, uint32_t rgbptr) {
    if (is_color_port(port)) {
        memcpy(g_mem + port + field, g_mem + rgbptr, 6);
        RGB c{ld16(rgbptr), ld16(rgbptr + 2), ld16(rgbptr + 4)};
        st32(port + index_field, rgb_to_pixel(surface_of_port(port), c));
    }
}
HLE(RGBForeColor) { set_rgb(cur(), port::rgbFgColor, port::fgColor, ARG(0)); }
HLE(RGBBackColor) { set_rgb(cur(), port::rgbBkColor, port::bkColor, ARG(0)); }
HLE(GetForeColor) { memcpy(g_mem + ARG(0), g_mem + cur() + port::rgbFgColor, 6); }
HLE(GetBackColor) { memcpy(g_mem + ARG(0), g_mem + cur() + port::rgbBkColor, 6); }
static RGB classic_color(uint32_t c) {
    switch (c) {
    case 30: return {0xFFFF, 0xFFFF, 0xFFFF};
    case 69: return {0xFC00, 0xF37D, 0x052F};
    case 137: return {0xF2D7, 0x0856, 0x84EC};
    case 205: return {0xDD6B, 0x08C2, 0x06A2};
    case 273: return {0x0241, 0xAB54, 0xEAFF};
    case 341: return {0x0000, 0x8000, 0x11B0};
    case 409: return {0x0000, 0x0000, 0xD400};
    default: return {0, 0, 0};
    }
}
static void classic(uint32_t port, uint32_t field, uint32_t idx, uint32_t color) {
    st32(port + idx, color);
    if (is_color_port(port)) {
        RGB c = classic_color(color);
        st16(port + field, c.r); st16(port + field + 2, c.g); st16(port + field + 4, c.b);
    }
}
HLE(ForeColor) { classic(cur(), port::rgbFgColor, port::fgColor, ARG(0)); }

/* ---------- pen ---------- */
HLE(PenNormal) {
    uint32_t p = cur();
    st32(p + port::pnSize, 0x00010001);
    st16(p + port::pnMode, 8);
    if (is_color_port(p)) memcpy(g_mem + ld32(ld32(p + port::pnPixPat)) + 20, kBlackPat, 8);
    else memcpy(g_mem + p + port::pnPat, kBlackPat, 8);
}
HLE(PenSize) { st16(cur() + port::pnSize, (uint16_t)ARG(1)); st16(cur() + port::pnSize + 2, (uint16_t)ARG(0)); }
HLE(PenMode) { st16(cur() + port::pnMode, (uint16_t)ARG(0)); }
HLE(PenPat) {
    uint32_t p = cur();
    if (is_color_port(p)) memcpy(g_mem + ld32(ld32(p + port::pnPixPat)) + 20, g_mem + ARG(0), 8);
    else memcpy(g_mem + p + port::pnPat, g_mem + ARG(0), 8);
}

/* ---------- shapes ---------- */
static void pen_pat(uint32_t p, uint8_t pat[8]) { port_pattern(p, port::pnPixPat, port::pnPat, pat); }
static void bk_pat(uint32_t p, uint8_t pat[8]) { port_pattern(p, port::bkPixPat, port::bkPat, pat); }
static int pen_mode(uint32_t p) { return (int16_t)ld16(p + port::pnMode); }

static void track_rgn(const Rect &r) { if (g_open_rgn) g_open_rgn_box = rect_union(g_open_rgn_box, r); }

HLE(EraseRect) {
    uint32_t p = cur();
    uint8_t pat[8];
    bk_pat(p, pat);
    /* erase: pattern bits 0 -> background colour; with the default white bkPat this is a bk fill */
    Rect r = read_rect(ARG(0));
    if (!memcmp(pat, kWhitePat, 8)) fill_rect_solid(p, r, port_bk(p));
    else fill_rect(p, r, pat, 8);
}
HLE(PaintRect) {
    uint32_t p = cur();
    uint8_t pat[8];
    pen_pat(p, pat);
    fill_rect(p, read_rect(ARG(0)), pat, pen_mode(p));
}
static void frame_rect(uint32_t p, const Rect &r) {
    uint8_t pat[8];
    pen_pat(p, pat);
    int pw = (int16_t)ld16(p + port::pnSize + 2), ph = (int16_t)ld16(p + port::pnSize);
    int m = pen_mode(p);
    if (rect_empty(r)) return;
    fill_rect(p, {r.top, r.left, (int16_t)(r.top + ph), r.right}, pat, m);
    fill_rect(p, {(int16_t)(r.bottom - ph), r.left, r.bottom, r.right}, pat, m);
    fill_rect(p, {(int16_t)(r.top + ph), r.left, (int16_t)(r.bottom - ph), (int16_t)(r.left + pw)}, pat, m);
    fill_rect(p, {(int16_t)(r.top + ph), (int16_t)(r.right - pw), (int16_t)(r.bottom - ph), r.right}, pat, m);
}
HLE(FrameRect) { Rect r = read_rect(ARG(0)); track_rgn(r); frame_rect(cur(), r); }

static void oval(uint32_t p, const Rect &r, bool frame, int ow = -1, int oh = -1) {
    uint8_t pat[8];
    pen_pat(p, pat);
    int m = pen_mode(p);
    double w = r.right - r.left, h = r.bottom - r.top;
    if (w <= 0 || h <= 0) return;
    double rx = ow < 0 ? w / 2 : ow / 2.0, ry = oh < 0 ? h / 2 : oh / 2.0;
    int pw = (int16_t)ld16(p + port::pnSize + 2), ph = (int16_t)ld16(p + port::pnSize);
    auto span = [&](int y, double inset, int &x0, int &x1) {
        double cy = y + 0.5;
        double top = r.top + ry, bot = r.bottom - ry;
        double dy = cy < top ? top - cy : cy > bot ? cy - bot : 0;
        double rr = ry - inset, rxx = rx - inset;
        if (dy > rr || rr <= 0) { x0 = x1 = 0; return; }
        double dx = rxx * std::sqrt(1 - (dy * dy) / (rr * rr));
        x0 = (int)std::lround(r.left + inset + (rx - inset) - dx);
        x1 = (int)std::lround(r.right - inset - (rx - inset) + dx);
    };
    for (int y = r.top; y < r.bottom; y++) {
        int x0, x1;
        span(y, 0, x0, x1);
        if (!frame) { fill_span(p, y, x0, x1, pat, m); continue; }
        int i0, i1;
        bool inner = y >= r.top + ph && y < r.bottom - ph;
        span(y, std::max(pw, ph), i0, i1);
        if (!inner || i1 <= i0) fill_span(p, y, x0, x1, pat, m);
        else { fill_span(p, y, x0, std::min(i0, x0 + pw) > x0 ? i0 : x0 + pw, pat, m); fill_span(p, y, std::max(i1, x1 - pw) < x1 ? i1 : x1 - pw, x1, pat, m); }
    }
}
HLE(PaintOval) { oval(cur(), read_rect(ARG(0)), false); }
HLE(FrameOval) { oval(cur(), read_rect(ARG(0)), true); }
HLE(FrameRoundRect) {
    Rect r = read_rect(ARG(0));
    track_rgn(r);
    oval(cur(), r, true, (int16_t)ARG(1), (int16_t)ARG(2));
}

/* ---------- lines and polygons ---------- */
struct PolyRec { bool open = false; std::vector<std::pair<int, int>> pts; };
static PolyRec g_poly;

static void line_to(uint32_t p, int h1, int v1) {
    int h0 = (int16_t)ld16(p + port::pnLoc + 2), v0 = (int16_t)ld16(p + port::pnLoc);
    st16(p + port::pnLoc, (uint16_t)v1);
    st16(p + port::pnLoc + 2, (uint16_t)h1);
    if (g_poly.open) { g_poly.pts.push_back({h1, v1}); return; }
    track_rgn({(int16_t)std::min(v0, v1), (int16_t)std::min(h0, h1), (int16_t)(std::max(v0, v1) + 1), (int16_t)(std::max(h0, h1) + 1)});
    if ((int16_t)ld16(p + port::pnVis) < 0) return;
    uint8_t pat[8];
    pen_pat(p, pat);
    int m = pen_mode(p);
    int pw = (int16_t)ld16(p + port::pnSize + 2), ph = (int16_t)ld16(p + port::pnSize);
    if (pw <= 0 || ph <= 0) return;
    int dx = std::abs(h1 - h0), dy = -std::abs(v1 - v0), sx = h0 < h1 ? 1 : -1, sy = v0 < v1 ? 1 : -1, err = dx + dy;
    for (;;) {
        fill_rect(p, {(int16_t)v0, (int16_t)h0, (int16_t)(v0 + ph), (int16_t)(h0 + pw)}, pat, m);
        if (h0 == h1 && v0 == v1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; h0 += sx; }
        if (e2 <= dx) { err += dx; v0 += sy; }
    }
}
HLE(MoveTo) {
    uint32_t p = cur();
    st16(p + port::pnLoc, (uint16_t)ARG(1));
    st16(p + port::pnLoc + 2, (uint16_t)ARG(0));
    if (g_poly.open) g_poly.pts.push_back({(int16_t)ARG(0), (int16_t)ARG(1)});
}
HLE(LineTo) { line_to(cur(), (int16_t)ARG(0), (int16_t)ARG(1)); }
HLE(Line) {
    uint32_t p = cur();
    line_to(p, (int16_t)ld16(p + port::pnLoc + 2) + (int16_t)ARG(0), (int16_t)ld16(p + port::pnLoc) + (int16_t)ARG(1));
}

static uint32_t g_poly_handle = 0;
HLE(OpenPoly) {
    g_poly = PolyRec{};
    g_poly.open = true;
    g_poly_handle = handle_new(10, true);
    st16(ld32(g_poly_handle), 10);
    RET(g_poly_handle);
}
HLE(ClosePoly) {
    g_poly.open = false;
    if (!g_poly_handle) return;
    auto &pts = g_poly.pts;
    uint32_t h = g_poly_handle;
    handle_set_size(h, 10 + 4 * (uint32_t)pts.size());
    uint32_t p = ld32(h);
    st16(p, (uint16_t)(10 + 4 * pts.size()));
    Rect bb{32767, 32767, -32768, -32768};
    for (size_t i = 0; i < pts.size(); i++) {
        st16(p + 10 + 4 * (uint32_t)i, (uint16_t)pts[i].second);
        st16(p + 12 + 4 * (uint32_t)i, (uint16_t)pts[i].first);
        bb.top = (int16_t)std::min<int>(bb.top, pts[i].second); bb.bottom = (int16_t)std::max<int>(bb.bottom, pts[i].second);
        bb.left = (int16_t)std::min<int>(bb.left, pts[i].first); bb.right = (int16_t)std::max<int>(bb.right, pts[i].first);
    }
    write_rect(p + 2, pts.empty() ? Rect{0, 0, 0, 0} : bb);
    g_poly_handle = 0;
}
HLE(KillPoly) { handle_dispose(ARG(0)); }

static void poly_points(uint32_t h, std::vector<std::pair<int, int>> &pts) {
    uint32_t p = ld32(h);
    int n = (ld16(p) - 10) / 4;
    for (int i = 0; i < n; i++) pts.push_back({(int16_t)ld16(p + 12 + 4 * i), (int16_t)ld16(p + 10 + 4 * i)});
}
HLE(FillPoly) {
    std::vector<std::pair<int, int>> pts;
    poly_points(ARG(0), pts);
    if (pts.size() < 3) return;
    uint8_t pat[8];
    pattern_of(ARG(1), pat);
    int ymin = INT32_MAX, ymax = INT32_MIN;
    for (auto &q : pts) { ymin = std::min(ymin, q.second); ymax = std::max(ymax, q.second); }
    uint32_t p = cur();
    for (int y = ymin; y < ymax; y++) {
        std::vector<double> xs;
        double cy = y + 0.5;
        for (size_t i = 0; i < pts.size(); i++) {
            auto a = pts[i], b = pts[(i + 1) % pts.size()];
            if ((a.second <= cy && b.second > cy) || (b.second <= cy && a.second > cy))
                xs.push_back(a.first + (cy - a.second) * (b.first - a.first) / (double)(b.second - a.second));
        }
        std::sort(xs.begin(), xs.end());
        for (size_t i = 0; i + 1 < xs.size(); i += 2) fill_span(p, y, (int)std::lround(xs[i]), (int)std::lround(xs[i + 1]), pat, 8);
    }
}
HLE(FramePoly) {
    std::vector<std::pair<int, int>> pts;
    poly_points(ARG(0), pts);
    uint32_t p = cur();
    if (pts.empty()) return;
    st16(p + port::pnLoc, (uint16_t)pts[0].second);
    st16(p + port::pnLoc + 2, (uint16_t)pts[0].first);
    for (size_t i = 1; i < pts.size(); i++) line_to(p, pts[i].first, pts[i].second);
}
