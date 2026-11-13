/* Fallback bitmap fonts when no Mac OS System file is available.
 *
 * The classic Mac fonts (Chicago, Geneva, Helvetica...) are Apple's and are
 * not shipped. Without the player's own System file, each strike is built at
 * runtime by rendering a similar Windows font without anti-aliasing, at the
 * same pixel size (a 12-point Mac font is 12 pixels per em at 72 dpi), into
 * the NFNT layout the text code already draws from. Nothing is cached on disk.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <vector>

#include "pt/runtime.h"

namespace pt::qd {

struct GdiStrike {
    int first = 0, last = 0, widMax = 0, kernMax = 0, ascent = 0, descent = 0, leading = 0, height = 0;
    int row_bytes = 0;
    std::vector<uint8_t> bits;
    std::vector<uint16_t> loc, ow;
};

/* Mac font family -> similar Windows face */
static const wchar_t *face_for(int family, bool &force_bold) {
    force_bold = false;
    switch (family) {
    case 0: force_bold = true; return L"Tahoma";   /* Chicago: a heavy system sans */
    case 2: case 20: return L"Times New Roman";    /* New York, Times */
    case 4: return L"Lucida Console";              /* Monaco */
    case 16: return L"Palatino Linotype";          /* Palatino */
    case 21: return L"Arial";                      /* Helvetica */
    case 22: return L"Courier New";                /* Courier */
    default: return L"Tahoma";                     /* Geneva and anything else */
    }
}

bool gdi_strike(int family, int size, bool bold, bool italic, GdiStrike &out) {
    bool force_bold;
    const wchar_t *face = face_for(family, force_bold);
    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) return false;
    HFONT font = CreateFontW(-size, 0, 0, 0, (bold || force_bold) ? FW_BOLD : FW_NORMAL, italic, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY,
                             DEFAULT_PITCH, face);
    HGDIOBJ old_font = SelectObject(dc, font);
    TEXTMETRICW tm{};
    GetTextMetricsW(dc, &tm);
    const int pad = size + 4, cw = size * 4 + 16, ch = tm.tmHeight;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = cw;
    bi.bmiHeader.biHeight = -ch;   /* top-down */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void *pixels = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!bmp || ch <= 0) {
        SelectObject(dc, old_font);
        DeleteObject(font);
        DeleteDC(dc);
        return false;
    }
    HGDIOBJ old_bmp = SelectObject(dc, bmp);
    SetBkMode(dc, OPAQUE);
    SetBkColor(dc, RGB(255, 255, 255));
    SetTextColor(dc, RGB(0, 0, 0));
    SetTextAlign(dc, TA_TOP | TA_LEFT);
    const uint32_t *px = (const uint32_t *)pixels;

    struct Glyph { int left = 0, width = 0, adv = 0; std::vector<uint8_t> ink; bool present = false; };
    std::vector<Glyph> glyphs(257);   /* 0..255, then the missing-glyph box */
    for (int c = 0; c < 256; c++) {
        Glyph &g = glyphs[c];
        if (c < 0x20 || c == 0x7F) continue;
        char mc = (char)c;
        wchar_t wc = 0;
        if (MultiByteToWideChar(10000, 0, &mc, 1, &wc, 1) != 1) continue;   /* code page 10000: Mac Roman */
        SIZE sz{};
        GetTextExtentPoint32W(dc, &wc, 1, &sz);
        RECT r{0, 0, cw, ch};
        ExtTextOutW(dc, pad, 0, ETO_OPAQUE, &r, &wc, 1, nullptr);
        GdiFlush();
        int ink_l = cw, ink_r = -1;
        for (int y = 0; y < ch; y++)
            for (int x = 0; x < cw; x++)
                if ((px[y * cw + x] & 0xFFFFFF) != 0xFFFFFF) {
                    ink_l = std::min(ink_l, x);
                    ink_r = std::max(ink_r, x);
                }
        int l = ink_r < 0 ? pad : std::min(pad, ink_l);
        int r_edge = ink_r < 0 ? pad : ink_r + 1;
        g.left = l - pad;
        g.width = r_edge - l;
        g.adv = std::min<int>(sz.cx, 254);
        g.ink.assign((size_t)g.width * ch, 0);
        for (int y = 0; y < ch; y++)
            for (int x = 0; x < g.width; x++)
                g.ink[(size_t)y * g.width + x] = (px[y * cw + l + x] & 0xFFFFFF) != 0xFFFFFF;
        g.present = true;
    }
    /* missing glyph: an empty box, as the Mac draws */
    {
        Glyph &g = glyphs[256];
        int w = std::max(3, size / 2), top = std::max(0, (int)tm.tmAscent - size * 2 / 3);
        g.adv = w + 1;
        g.width = w;
        g.ink.assign((size_t)w * ch, 0);
        for (int y = top; y < tm.tmAscent; y++)
            for (int x = 0; x < w; x++)
                if (y == top || y == tm.tmAscent - 1 || x == 0 || x == w - 1) g.ink[(size_t)y * w + x] = 1;
        g.present = true;
    }
    SelectObject(dc, old_bmp);
    DeleteObject(bmp);
    SelectObject(dc, old_font);
    DeleteObject(font);
    DeleteDC(dc);

    out = GdiStrike{};
    out.first = 0;
    out.last = 255;
    out.ascent = tm.tmAscent;
    out.descent = tm.tmDescent;
    out.leading = tm.tmExternalLeading;
    out.height = ch;
    int kern = 0, total = 0;
    for (auto &g : glyphs)
        if (g.present) {
            kern = std::min(kern, g.left);
            total += g.width;
            out.widMax = std::max(out.widMax, g.adv);
        }
    out.kernMax = kern;
    out.row_bytes = (total + 15) / 16 * 2;
    out.bits.assign((size_t)out.row_bytes * ch, 0);
    int x = 0;
    for (int c = 0; c <= 256; c++) {
        const Glyph &g = glyphs[c];
        out.loc.push_back((uint16_t)x);
        if (!g.present) {
            out.ow.push_back(0xFFFF);
            continue;
        }
        out.ow.push_back((uint16_t)(std::min(g.left - kern, 255) << 8 | g.adv));
        for (int y = 0; y < ch; y++)
            for (int i = 0; i < g.width; i++)
                if (g.ink[(size_t)y * g.width + i]) out.bits[(size_t)y * out.row_bytes + ((x + i) >> 3)] |= 0x80 >> ((x + i) & 7);
        x += g.width;
    }
    out.loc.push_back((uint16_t)x);   /* end of the missing glyph */
    out.ow.push_back(0xFFFF);
    logf(LOG_GFX, "font: family %d size %d%s%s from Windows font (ascent %d descent %d)\n", family, size,
         bold ? " bold" : "", italic ? " italic" : "", out.ascent, out.descent);
    return x < 65536;
}

}  // namespace pt::qd
