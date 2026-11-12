/* Font Manager and QuickDraw text: bitmap fonts (NFNT/FONT) found via FOND
 * family resources in the System file and font suitcases. */
#include <map>
#include <vector>

#include "pt/qd.h"
#include "pt/resources.h"
#include "pt/ui.h"

namespace pt::qd {

struct Strike {
    bool valid = false;
    int first = 0, last = 0, widMax = 0, kernMax = 0, ascent = 0, descent = 0, leading = 0, height = 0;
    int row_bytes = 0;
    std::vector<uint8_t> bits;          /* height rows of row_bytes */
    std::vector<uint16_t> loc;          /* bit offsets, last-first+3 entries */
    std::vector<uint16_t> ow;           /* offset/width, last-first+3 entries */
    bool synth_bold = false, synth_italic = false;
};

static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

static bool parse_font(const std::vector<uint8_t> &d, Strike &s) {
    if (d.size() < 26) return false;
    s.first = be16(&d[2]);
    s.last = be16(&d[4]);
    s.widMax = (int16_t)be16(&d[6]);
    s.kernMax = (int16_t)be16(&d[8]);
    s.height = be16(&d[14]);
    int owtloc = be16(&d[16]);
    s.ascent = be16(&d[18]);
    s.descent = be16(&d[20]);
    s.leading = be16(&d[22]);
    s.row_bytes = be16(&d[24]) * 2;
    size_t bits_end = 26 + (size_t)s.row_bytes * s.height;
    int n = s.last - s.first + 3;
    size_t ow_off = 16 + (size_t)owtloc * 2;
    if (bits_end + 2 * n > d.size() || ow_off + 2 * n > d.size()) return false;
    s.bits.assign(d.begin() + 26, d.begin() + bits_end);
    for (int i = 0; i < n; i++) {
        s.loc.push_back(be16(&d[bits_end + 2 * i]));
        s.ow.push_back(be16(&d[ow_off + 2 * i]));
    }
    s.valid = true;
    return true;
}

/* family 0 is the system font (Chicago), 1 the application font (Geneva) */
static int real_family(int fam) { return fam == 0 ? 0 : fam == 1 ? 3 : fam; }

static const Strike &strike_for(int family, int size, int face) {
    static std::map<uint32_t, Strike> cache;
    if (size <= 0) size = 12;
    family = real_family(family);
    uint32_t key = (uint32_t)family << 16 | (uint32_t)size << 4 | (face & 3);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    Strike s;
    std::vector<uint8_t> data;
    int want_style = face & 3;   /* bold | italic */
    /* several files may define the family (the System's FOND can point at ROM strikes);
     * use the best-matching strike that actually exists */
    int best_score = 1 << 30, best_style = 0;
    std::vector<uint8_t> best;
    for (auto &fond : res::get_bytes_all('FOND', (int16_t)family)) {
        if (fond.size() <= 54) continue;
        int n = (int16_t)be16(&fond[52]) + 1;
        for (int i = 0; i < n && 54 + 6 * i + 6 <= (int)fond.size(); i++) {
            int fsize = be16(&fond[54 + 6 * i]), fstyle = be16(&fond[56 + 6 * i]), fid = (int16_t)be16(&fond[58 + 6 * i]);
            if (fsize == 0) continue;   /* outline font */
            int score = std::abs(fsize - size) * 8 + (fstyle == want_style ? 0 : fstyle == 0 ? 2 : 6);
            if (score >= best_score) continue;
            if (!res::get_bytes('NFNT', (int16_t)fid, data) && !res::get_bytes('FONT', (int16_t)fid, data)) continue;
            best_score = score;
            best_style = fstyle;
            best = data;
        }
    }
    if (!best.empty()) {
        parse_font(best, s);
        s.synth_bold = (want_style & 1) && !(best_style & 1);
        s.synth_italic = (want_style & 2) && !(best_style & 2);
    }
    if (!s.valid && res::get_bytes('FONT', (int16_t)(family * 128 + size), data)) parse_font(data, s);
    if (!s.valid && family != 3) {
        logf(LOG_GFX, "font family %d size %d missing; using Geneva\n", family, size);
        Strike g = strike_for(3, size, face);
        return cache[key] = g;
    }
    return cache[key] = s;
}

static const Strike &port_strike(uint32_t port) {
    return strike_for((int16_t)ld16(port + port::txFont), (int16_t)ld16(port + port::txSize), ld8(port + port::txFace));
}

static int glyph_index(const Strike &s, uint8_t ch) {
    int i = ch - s.first;
    if (ch < s.first || ch > s.last || s.ow[i] == 0xFFFF) i = s.last - s.first + 1;   /* missing glyph */
    return i;
}

int text_width(uint32_t port, const uint8_t *t, int len) {
    const Strike &s = port_strike(port);
    if (!s.valid) return len * 6;
    int w = 0;
    for (int i = 0; i < len; i++) {
        int g = glyph_index(s, t[i]);
        w += (s.ow[g] & 0xFF) + (s.synth_bold ? 1 : 0);
        if (t[i] == ' ') w += (int)((int32_t)ld32(port + port::spExtra) >> 16);
    }
    return w;
}

void font_info(uint32_t port, int &ascent, int &descent, int &widMax, int &leading) {
    const Strike &s = port_strike(port);
    ascent = s.ascent;
    descent = s.descent;
    widMax = s.widMax + (s.synth_bold ? 1 : 0);
    leading = s.leading;
}

void draw_text(uint32_t port, const uint8_t *t, int len) {
    const Strike &s = port_strike(port);
    int h = (int16_t)ld16(port + port::pnLoc + 2), v = (int16_t)ld16(port + port::pnLoc);
    if (!s.valid) { st16(port + port::pnLoc + 2, (uint16_t)(h + len * 6)); return; }
    Surface surf = surface_of_port(port);
    Rect clip = rect_sect(port_clip(port), surf.bounds);
    uint32_t fg = rgb_to_pixel(surf, port_fg(port)), bk = rgb_to_pixel(surf, port_bk(port));
    int mode = (int16_t)ld16(port + port::txMode);
    uint8_t face = ld8(port + port::txFace);
    int top = v - s.ascent;
    int start_h = h;
    for (int i = 0; i < len; i++) {
        int g = glyph_index(s, t[i]);
        int x0 = s.loc[g], x1 = s.loc[g + 1];
        int off = (s.ow[g] >> 8) + s.kernMax, adv = (s.ow[g] & 0xFF) + (s.synth_bold ? 1 : 0);
        if (mode == 0) {   /* srcCopy: background box */
            for (int y = 0; y < s.height; y++)
                for (int x = 0; x < adv; x++) {
                    int px = h + x, py = top + y;
                    if (px >= clip.left && px < clip.right && py >= clip.top && py < clip.bottom)
                        put_pixel(surf, px - surf.bounds.left, py - surf.bounds.top, bk);
                }
        }
        for (int y = 0; y < s.height; y++) {
            int slant = s.synth_italic ? (s.ascent - y) / 2 : 0;
            const uint8_t *row = &s.bits[(size_t)y * s.row_bytes];
            for (int bx = x0; bx < x1; bx++) {
                if (!((row[bx >> 3] >> (7 - (bx & 7))) & 1)) continue;
                for (int b = 0; b <= (s.synth_bold ? 1 : 0); b++) {
                    int px = h + off + (bx - x0) + b + slant, py = top + y;
                    if (px < clip.left || px >= clip.right || py < clip.top || py >= clip.bottom) continue;
                    int lx = px - surf.bounds.left, ly = py - surf.bounds.top;
                    if (mode == 2 || mode == 10) put_pixel(surf, lx, ly, get_pixel(surf, lx, ly) ^ (surf.depth >= 32 ? 0xFFFFFF : (1u << surf.depth) - 1));
                    else if (mode == 3 || mode == 11) put_pixel(surf, lx, ly, bk);
                    else put_pixel(surf, lx, ly, fg);
                }
            }
        }
        h += adv;
        if (t[i] == ' ') h += (int)((int32_t)ld32(port + port::spExtra) >> 16);
    }
    if (face & 4) {   /* underline */
        int y = v + 1;
        for (int x = start_h; x < h; x++)
            if (x >= clip.left && x < clip.right && y >= clip.top && y < clip.bottom)
                put_pixel(surf, x - surf.bounds.left, y - surf.bounds.top, fg);
    }
    st16(port + port::pnLoc + 2, (uint16_t)h);
}

}  // namespace pt::qd

namespace pt::qd {
void te_text_box(uint32_t port, uint32_t text, int len, const Rect &box, int just) {
    int a, d, w, l;
    font_info(port, a, d, w, l);
    int line_h = a + d + l;
    int y = box.top + a;
    int i = 0;
    uint32_t clip = ld32(port + port::clipRgn);
    Rect old_clip = region_bbox(clip);
    set_region(clip, rect_sect(old_clip, box));
    while (i < len && y - a < box.bottom) {
        int start = i, last_space = -1, width = box.right - box.left;
        while (i < len && g_mem[text + i] != '\r') {
            if (g_mem[text + i] == ' ') last_space = i;
            if (text_width(port, g_mem + text + start, i - start + 1) > width) {
                if (last_space > start) i = last_space;
                break;
            }
            i++;
        }
        int n = i - start;
        int lw = text_width(port, g_mem + text + start, n);
        int x = just == 1 ? box.left + (width - lw) / 2 : just == -1 ? box.right - lw : box.left;
        st16(port + port::pnLoc, (uint16_t)y);
        st16(port + port::pnLoc + 2, (uint16_t)x);
        draw_text(port, g_mem + text + start, n);
        if (i < len && (g_mem[text + i] == '\r' || g_mem[text + i] == ' ')) i++;
        y += line_h;
    }
    set_region(clip, old_clip);
}
}  // namespace pt::qd

using namespace pt;
using namespace pt::qd;

static uint32_t tport() { return current_port(); }

HLE(InitFonts) { (void)c; }
HLE(TextFont) { st16(tport() + port::txFont, (uint16_t)ARG(0)); }
HLE(TextFace) { st8(tport() + port::txFace, (uint8_t)ARG(0)); }
HLE(TextSize) { st16(tport() + port::txSize, (uint16_t)ARG(0)); }
HLE(DrawString) { uint32_t s = ARG(0); draw_text(tport(), g_mem + s + 1, g_mem[s]); }
HLE(DrawText) { draw_text(tport(), g_mem + ARG(0) + (int16_t)ARG(1), (int16_t)ARG(2)); }
HLE(TextWidth) { RET(text_width(tport(), g_mem + ARG(0) + (int16_t)ARG(1), (int16_t)ARG(2))); }
HLE(StringWidth) { uint32_t s = ARG(0); RET(text_width(tport(), g_mem + s + 1, g_mem[s])); }
HLE(CharWidth) { uint8_t ch = (uint8_t)ARG(0); RET(text_width(tport(), &ch, 1)); }
HLE(GetFontInfo) {
    int a, d, w, l;
    font_info(tport(), a, d, w, l);
    st16(ARG(0), (uint16_t)a); st16(ARG(0) + 2, (uint16_t)d); st16(ARG(0) + 4, (uint16_t)w); st16(ARG(0) + 6, (uint16_t)l);
}
HLE(TruncText) {
    /* short TruncText(short width, Ptr textPtr, short *length, TruncCode truncWhere) */
    int width = (int16_t)ARG(0);
    uint32_t text = ARG(1);
    int len = (int16_t)ld16(ARG(2));
    if (text_width(tport(), g_mem + text, len) <= width) { RET(0); return; }
    uint8_t ell = 0xC9;   /* '…' in Mac Roman */
    int ew = text_width(tport(), &ell, 1);
    while (len > 0 && text_width(tport(), g_mem + text, len) + ew > width) len--;
    g_mem[text + len] = ell;
    st16(ARG(2), (uint16_t)(len + 1));
    RET(1);
}

HLE(TETextBox) {
    /* void TETextBox(const void *text, long length, const Rect *box, short just) */
    te_text_box(tport(), ARG(0), (int)ARG(1), read_rect(ARG(2)), (int16_t)ARG(3));
}
