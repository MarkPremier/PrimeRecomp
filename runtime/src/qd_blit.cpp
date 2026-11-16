/* CopyBits and friends. */
#include <algorithm>
#include <map>

#include "pt/host.h"
#include "pt/qd.h"

namespace pt::qd {

static bool same_colors(const Surface &a, const Surface &b) {
    if (a.depth != b.depth) return false;
    if (a.depth > 8) return true;
    if (a.ctab == b.ctab || ctab_seed(a.ctab) == ctab_seed(b.ctab)) return true;
    auto ta = read_ctab(a.ctab), tb = read_ctab(b.ctab);
    if (ta.size() != tb.size()) return false;
    for (size_t i = 0; i < ta.size(); i++)
        if (ta[i].r != tb[i].r || ta[i].g != tb[i].g || ta[i].b != tb[i].b) return false;
    return true;
}

void copy_bits(const Surface &src, const Surface &dst, Rect sr, Rect dr, int mode, const Rect &clip_dst,
               const Surface *mask) {
    (void)mask;
    if (rect_empty(sr) || rect_empty(dr)) return;
    Rect c = rect_sect(rect_sect(dr, clip_dst), dst.bounds);
    if (rect_empty(c)) return;
    int sw = sr.right - sr.left, sh = sr.bottom - sr.top, dw = dr.right - dr.left, dh = dr.bottom - dr.top;
    bool scaled = sw != dw || sh != dh;
    int m = mode & 0x3F;               /* drop ditherCopy (64) */
    bool transparent = m == 36;
    if (m >= 8 && m < 32) m &= 7;      /* pattern modes are invalid here; treat as source modes */
    if (m >= 32 && !transparent) m = 0; /* arithmetic modes approximated by srcCopy */

    uint32_t port = current_port();
    RGB fgc = port ? port_fg(port) : RGB{0, 0, 0}, bkc = port ? port_bk(port) : RGB{0xFFFF, 0xFFFF, 0xFFFF};

    /* fast path: plain copy, same pixel format, no scaling */
    if (m == 0 && !scaled && !transparent && src.depth >= 8 && same_colors(src, dst)) {
        int bpp = src.depth / 8;
        for (int y = c.top; y < c.bottom; y++) {
            int sy = y - dr.top + sr.top - src.bounds.top, sx = c.left - dr.left + sr.left - src.bounds.left;
            if (sy < 0 || sx < 0) continue;
            uint32_t s = src.base + (uint32_t)sy * src.row_bytes + (uint32_t)sx * bpp;
            int py = y - dst.bounds.top, px0 = c.left - dst.bounds.left, w = c.right - c.left;
            uint32_t d = dst.base + (uint32_t)py * dst.row_bytes + (uint32_t)px0 * bpp;
            if (!dst.occl || py < 0 || py >= 480) {
                memmove(g_mem + d, g_mem + s, (size_t)w * bpp);
                continue;
            }
            /* copy only the runs of pixels this window owns on screen */
            const uint8_t *own = dst.occl + py * 640;
            int x = 0;
            while (x < w) {
                int gx = px0 + x;
                bool mine = gx < 0 || gx >= 640 || own[gx] == dst.owner;
                int run = 1;
                while (x + run < w) {
                    int g2 = px0 + x + run;
                    bool m2 = g2 < 0 || g2 >= 640 || own[g2] == dst.owner;
                    if (m2 != mine) break;
                    run++;
                }
                if (mine) memmove(g_mem + d + (uint32_t)x * bpp, g_mem + s + (uint32_t)x * bpp, (size_t)run * bpp);
                x += run;
            }
        }
        return;
    }

    /* fast path: plain scaled copy, same pixel format (the high-resolution world shrunk onto the
     * Mac screen): nearest pixel, as the general path picks it */
    if (m == 0 && scaled && !transparent && src.depth >= 8 && same_colors(src, dst)) {
        int bpp = src.depth / 8;
        std::vector<int> col((size_t)(c.right - c.left));
        for (int x = c.left; x < c.right; x++)
            col[x - c.left] = sr.left + (int)((int64_t)(x - dr.left) * sw / dw) - src.bounds.left;
        for (int y = c.top; y < c.bottom; y++) {
            int sy = sr.top + (int)((int64_t)(y - dr.top) * sh / dh) - src.bounds.top;
            if (sy < 0 || sy >= src.bounds.bottom - src.bounds.top) continue;
            const uint8_t *srow = g_mem + src.base + (uint32_t)sy * src.row_bytes;
            int py = y - dst.bounds.top;
            uint8_t *drow = g_mem + dst.base + (uint32_t)py * dst.row_bytes;
            const uint8_t *own = dst.occl && py >= 0 && py < 480 ? dst.occl + py * 640 : nullptr;
            for (int x = c.left; x < c.right; x++) {
                int sx = col[x - c.left], px = x - dst.bounds.left;
                if (sx < 0 || sx >= src.bounds.right - src.bounds.left) continue;
                if (own && px >= 0 && px < 640 && own[px] != dst.owner) continue;
                if (bpp == 4) memcpy(drow + px * 4, srow + sx * 4, 4);
                else if (bpp == 2) memcpy(drow + px * 2, srow + sx * 2, 2);
                else drow[px] = srow[sx];
            }
        }
        return;
    }

    /* general path: source pixel -> RGB -> destination pixel */
    std::vector<uint32_t> lut;
    bool src_indexed = src.depth <= 8;
    if (src_indexed) {
        int n = 1 << src.depth;
        lut.resize(n);
        if (src.depth == 1) {
            uint32_t f = rgb_to_pixel(dst, fgc), b = rgb_to_pixel(dst, bkc);
            lut[0] = b;
            lut[1] = f;
        } else {
            auto t = read_ctab(src.ctab);
            bool same = same_colors(src, dst);
            for (int i = 0; i < n; i++) lut[i] = same ? (uint32_t)i : rgb_to_pixel(dst, i < (int)t.size() ? t[i] : RGB{0, 0, 0});
        }
    }
    std::map<uint32_t, uint32_t> direct_cache;
    /* direct -> indexed: a 15-bit RGB inverse table per destination colour table */
    static std::vector<uint8_t> inv;
    static uint32_t inv_key = 0;
    bool use_inv = !src_indexed && dst.depth == 8;
    if (use_inv) {
        uint32_t key = ctab_seed(dst.ctab) ^ dst.ctab;
        if (inv.empty() || key != inv_key) {
            inv.assign(32768, 0);
            for (uint32_t i = 0; i < 32768; i++) {
                RGB c{(uint16_t)(((i >> 10) & 31) * 0x0842), (uint16_t)(((i >> 5) & 31) * 0x0842), (uint16_t)((i & 31) * 0x0842)};
                inv[i] = (uint8_t)color_to_index(dst.ctab, c);
            }
            inv_key = key;
        }
    }
    uint32_t allones = dst.depth >= 32 ? 0xFFFFFF : (1u << dst.depth) - 1;
    uint32_t dst_bk = rgb_to_pixel(dst, bkc), dst_fg = rgb_to_pixel(dst, fgc);
    uint32_t src_bk = src_indexed ? (src.depth == 1 ? 0 : color_to_index(src.ctab, bkc)) : rgb_to_pixel(src, bkc);

    for (int y = c.top; y < c.bottom; y++) {
        int sy = sr.top + (scaled ? (int)((int64_t)(y - dr.top) * sh / dh) : y - dr.top) - src.bounds.top;
        if (sy < 0) continue;
        for (int x = c.left; x < c.right; x++) {
            int sx = sr.left + (scaled ? (int)((int64_t)(x - dr.left) * sw / dw) : x - dr.left) - src.bounds.left;
            if (sx < 0) continue;
            uint32_t sp = get_pixel(src, sx, sy);
            if (transparent && sp == src_bk) continue;
            uint32_t dp;
            if (src_indexed) dp = lut[sp];
            else if (use_inv) {
                uint32_t r5, g5, b5;
                if (src.depth == 16) { r5 = (sp >> 10) & 31; g5 = (sp >> 5) & 31; b5 = sp & 31; }
                else { r5 = (sp >> 19) & 31; g5 = (sp >> 11) & 31; b5 = (sp >> 3) & 31; }
                dp = inv[r5 << 10 | g5 << 5 | b5];
            } else {
                auto it = direct_cache.find(sp);
                if (it != direct_cache.end()) dp = it->second;
                else dp = direct_cache[sp] = rgb_to_pixel(dst, pixel_to_rgb(src, sp));
            }
            int px = x - dst.bounds.left, py = y - dst.bounds.top;
            if (m == 0 || transparent) { put_pixel(dst, px, py, dp); continue; }
            /* boolean source modes: a source pixel "is set" when it is not the background */
            bool set = src.depth == 1 ? sp != 0 : sp != src_bk;
            uint32_t d = get_pixel(dst, px, py);
            uint32_t out = d;
            switch (m & 7) {
            case 1: out = set ? dp : d; break;                     /* srcOr */
            case 2: out = set ? d ^ allones : d; break;            /* srcXor */
            case 3: out = set ? dst_bk : d; break;                 /* srcBic */
            case 4: out = set ? dst_bk : dst_fg; break;            /* notSrcCopy */
            case 5: out = set ? d : dst_fg; break;                 /* notSrcOr */
            case 6: out = set ? d : d ^ allones; break;            /* notSrcXor */
            case 7: out = set ? d : dst_bk; break;                 /* notSrcBic */
            }
            put_pixel(dst, px, py, out);
        }
    }
}

/* resolve a CopyBits argument: BitMap*, PixMap*, or &CGrafPort.portBits (a PixMapHandle) */
Surface surface_from_bits_arg(uint32_t a, uint32_t &pixmap_out) {
    uint16_t rb = ld16(a + 4);
    if ((rb & 0xC000) == 0xC000) {          /* CGrafPort: baseAddr field holds the PixMapHandle */
        pixmap_out = ld32(ld32(a));
        return surface_of_bitmap(pixmap_out, true);
    }
    pixmap_out = (rb & 0x8000) ? a : 0;
    return surface_of_bitmap(a, (rb & 0x8000) != 0);
}

}  // namespace pt::qd

using namespace pt;
using namespace pt::qd;

HLE(CopyBits) {
    /* CopyBits(const BitMap *src, const BitMap *dst, const Rect *srcRect, const Rect *dstRect, short mode, RgnHandle mask) */
    uint32_t spm, dpm;
    Surface s = surface_from_bits_arg(ARG(0), spm);
    Surface d = surface_from_bits_arg(ARG(1), dpm);
    Rect sr = read_rect(ARG(2)), dr = read_rect(ARG(3));
    Rect clip = d.bounds;
    uint32_t port = current_port();
    if (port) {
        bool dst_is_port = is_color_port(port) ? (dpm && dpm == port_pixmap(port)) || ld32(ARG(1)) == ld32(port + port::portPixMap)
                                               : ARG(1) == port + port::portBits || d.base == ld32(port + port::portBits);
        if (dst_is_port) clip = rect_sect(clip, port_clip(port));
    }
    if (ARG(5)) clip = rect_sect(clip, region_bbox(ARG(5)));
    logf(LOG_GFX, "CopyBits %08X(%d) -> %08X(%d) [%d,%d,%d,%d]->[%d,%d,%d,%d] mode %d\n", s.base, s.depth, d.base,
         d.depth, sr.left, sr.top, sr.right, sr.bottom, dr.left, dr.top, dr.right, dr.bottom, (int16_t)ARG(4));
    copy_bits(s, d, sr, dr, (int16_t)ARG(4), clip);
    /* a finished 3D view (or any large picture) reaching the screen: show it now rather than
     * at the next throttled present, so frames arrive at an even 30 per second */
    if (d.base == screen().base && (dr.right - dr.left) * (dr.bottom - dr.top) >= 640 * 120) host::present(true);
}
