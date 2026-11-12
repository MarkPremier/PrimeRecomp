/* DrawPicture: PICT version 1 and 2 decoding.
 * Implements the bitmap opcodes (BitsRect/Rgn, PackBitsRect/Rgn, DirectBitsRect/Rgn)
 * and skips the rest correctly; Prime Target's pictures are all bitmap pictures. */
#include <vector>

#include "pt/qd.h"

namespace pt::qd {

struct Reader {
    const uint8_t *p, *end;
    bool ok() const { return p < end; }
    uint8_t u8() { return p < end ? *p++ : 0; }
    uint16_t u16() { uint16_t v = (uint16_t)(u8() << 8); return (uint16_t)(v | u8()); }
    uint32_t u32() { uint32_t v = (uint32_t)u16() << 16; return v | u16(); }
    Rect rect() { Rect r; r.top = (int16_t)u16(); r.left = (int16_t)u16(); r.bottom = (int16_t)u16(); r.right = (int16_t)u16(); return r; }
    void skip(size_t n) { p = (size_t)(end - p) < n ? end : p + n; }
};

static void unpack_bits(Reader &rd, size_t packed_len, std::vector<uint8_t> &out, size_t want) {
    const uint8_t *stop = rd.p + packed_len;
    if (stop > rd.end) stop = rd.end;
    out.clear();
    while (rd.p < stop && out.size() < want) {
        int8_t n = (int8_t)rd.u8();
        if (n >= 0) { for (int i = 0; i <= n && rd.p < stop; i++) out.push_back(rd.u8()); }
        else if (n != -128) { uint8_t v = rd.u8(); for (int i = 0; i < 1 - n; i++) out.push_back(v); }
    }
    rd.p = stop;
    out.resize(want, 0);
}

/* 16-bit PackBits (packType 3): runs of words */
static void unpack_words(Reader &rd, size_t packed_len, std::vector<uint8_t> &out, size_t want) {
    const uint8_t *stop = rd.p + packed_len;
    if (stop > rd.end) stop = rd.end;
    out.clear();
    while (rd.p < stop && out.size() < want) {
        int8_t n = (int8_t)rd.u8();
        if (n >= 0) { for (int i = 0; i <= n; i++) { out.push_back(rd.u8()); out.push_back(rd.u8()); } }
        else if (n != -128) { uint8_t a = rd.u8(), b = rd.u8(); for (int i = 0; i < 1 - n; i++) { out.push_back(a); out.push_back(b); } }
    }
    rd.p = stop;
    out.resize(want, 0);
}

static void skip_region(Reader &rd) { uint16_t sz = rd.u16(); rd.skip(sz >= 2 ? sz - 2 : 0); }

struct PictCtx {
    Rect frame;
    Rect dst;
    uint32_t port;
};

static Rect map_rect(const PictCtx &pc, const Rect &r) {
    int fw = pc.frame.right - pc.frame.left, fh = pc.frame.bottom - pc.frame.top;
    int dw = pc.dst.right - pc.dst.left, dh = pc.dst.bottom - pc.dst.top;
    if (fw <= 0 || fh <= 0) return r;
    auto mx = [&](int x) { return (int16_t)(pc.dst.left + (int64_t)(x - pc.frame.left) * dw / fw); };
    auto my = [&](int y) { return (int16_t)(pc.dst.top + (int64_t)(y - pc.frame.top) * dh / fh); };
    return {my(r.top), mx(r.left), my(r.bottom), mx(r.right)};
}

/* decode a bitmap opcode and blit it into the current port */
static void bits_op(Reader &rd, const PictCtx &pc, uint16_t op) {
    bool direct = op == 0x9A || op == 0x9B;
    bool packed = op != 0x90 && op != 0x91;
    bool has_rgn = op & 1;
    if (direct) rd.u32();   /* baseAddr placeholder 0x000000FF */
    uint16_t rbf = rd.u16();
    uint32_t row_bytes = rbf & 0x3FFF;
    bool is_pixmap = direct || (rbf & 0x8000);
    Rect bounds = rd.rect();
    int depth = 1, pack_type = 0, cmp_count = 1;
    std::vector<RGB> colors;
    if (is_pixmap) {
        rd.u16(); pack_type = rd.u16(); rd.u32(); rd.u32(); rd.u32();  /* version, packType, packSize, hRes, vRes */
        rd.u16(); depth = rd.u16(); cmp_count = rd.u16(); rd.u16();     /* pixelType, pixelSize, cmpCount, cmpSize */
        rd.u32(); rd.u32(); rd.u32();                                   /* planeBytes, pmTable, pmReserved */
        if (!direct) {
            rd.u32();
            uint16_t flags = rd.u16();
            int n = (int16_t)rd.u16() + 1;
            colors.assign(256, RGB{0, 0, 0});
            for (int i = 0; i < n; i++) {
                uint16_t idx = rd.u16();
                RGB c{rd.u16(), rd.u16(), rd.u16()};
                int at = (flags & 0x8000) ? i : (idx & 0xFF);
                if (at < 256) colors[at] = c;
            }
            colors.resize((size_t)1 << (depth > 8 ? 8 : depth));
        }
    }
    Rect sr = rd.rect(), dr = rd.rect();
    int mode = (int16_t)rd.u16();
    if (has_rgn) skip_region(rd);

    int w = bounds.right - bounds.left, h = bounds.bottom - bounds.top;
    if (w <= 0 || h <= 0) return;
    int out_depth = depth == 24 ? 32 : depth;
    uint32_t out_rb = ((uint32_t)w * out_depth + 31) / 32 * 4;
    uint32_t buf = guest_alloc(out_rb * h, true);
    if (!buf) return;
    std::vector<uint8_t> row;
    for (int y = 0; y < h; y++) {
        uint32_t dst = buf + (uint32_t)y * out_rb;
        if (!packed || row_bytes < 8 || pack_type == 1) {
            row.assign(rd.p, rd.p + std::min<size_t>(row_bytes, rd.end - rd.p));
            rd.skip(row_bytes);
            row.resize(row_bytes, 0);
        } else {
            size_t n = row_bytes > 250 ? rd.u16() : rd.u8();
            if (direct && depth == 32 && pack_type != 2) unpack_bits(rd, n, row, (size_t)w * cmp_count);
            else if (direct && depth == 16) unpack_words(rd, n, row, row_bytes);
            else unpack_bits(rd, n, row, row_bytes);
        }
        if (direct && depth == 32) {
            /* component planes: [A] R G B, each w bytes */
            int planes = pack_type == 2 ? 0 : cmp_count;
            for (int x = 0; x < w; x++) {
                uint32_t r, g, b;
                if (planes >= 3) {
                    int o = planes == 4 ? w : 0;
                    r = row[o + x]; g = row[o + w + x]; b = row[o + 2 * w + x];
                } else {
                    r = row[3 * x]; g = row[3 * x + 1]; b = row[3 * x + 2];
                }
                st32(dst + 4 * x, (r << 16) | (g << 8) | b);
            }
        } else {
            memcpy(g_mem + dst, row.data(), std::min<size_t>(out_rb, row.size()));
        }
        if (!rd.ok() && y + 1 < h) break;
    }
    Surface s;
    s.base = buf;
    s.row_bytes = out_rb;
    s.depth = out_depth;
    s.bounds = bounds;
    s.color_port = is_pixmap;
    uint32_t tab = 0;
    if (is_pixmap && out_depth <= 8) { tab = new_ctab(colors); s.ctab = tab; }
    Surface d = surface_of_port(pc.port);
    copy_bits(s, d, sr, map_rect(pc, dr), mode, port_clip(pc.port));
    if (tab) handle_dispose(tab);
    guest_free(buf);
}

void draw_picture(uint32_t pic, const Rect &dst) {
    if (!pic || !ld32(pic)) return;
    uint32_t base = ld32(pic);
    uint32_t size = handle_size(pic);
    Reader rd{g_mem + base, g_mem + base + size};
    rd.u16();
    PictCtx pc{rd.rect(), dst, current_port()};
    bool v2 = false;
    if (rd.p[0] == 0x11 && rd.p[1] == 0x01) { rd.skip(2); }          /* version 1 */
    else if (rd.p[0] == 0x00 && rd.p[1] == 0x11) { rd.skip(4); v2 = true; }
    while (rd.ok()) {
        if (v2 && ((rd.p - (g_mem + base)) & 1)) rd.u8();
        uint16_t op = v2 ? rd.u16() : rd.u8();
        switch (op) {
        case 0x0000: break;                                    /* NOP */
        case 0x0001: skip_region(rd); break;                   /* ClipRgn */
        case 0x0003: case 0x0004: case 0x0005: case 0x0008: case 0x000D: case 0x0015: case 0x0016: rd.skip(2); break;
        case 0x0006: case 0x0007: case 0x000B: case 0x000C: case 0x000E: case 0x000F: rd.skip(4); break;
        case 0x0002: case 0x0009: case 0x000A: case 0x0010: rd.skip(8); break;
        case 0x0011: rd.skip(v2 ? 2 : 1); break;
        case 0x001A: case 0x001B: case 0x001D: case 0x001F: rd.skip(6); break;
        case 0x001C: case 0x001E: break;
        case 0x0020: rd.skip(8); break;
        case 0x0021: rd.skip(4); break;
        case 0x0022: rd.skip(6); break;
        case 0x0023: rd.skip(2); break;
        case 0x0028: { rd.skip(4); uint8_t n = rd.u8(); rd.skip(n); break; }
        case 0x0029: case 0x002A: { rd.skip(1); uint8_t n = rd.u8(); rd.skip(n); break; }
        case 0x002B: { rd.skip(2); uint8_t n = rd.u8(); rd.skip(n); break; }
        case 0x002C: case 0x002D: case 0x002E: { uint16_t n = rd.u16(); rd.skip(n); break; }
        case 0x0030: case 0x0031: case 0x0032: case 0x0033: case 0x0034:
        case 0x0040: case 0x0041: case 0x0042: case 0x0043: case 0x0044:
        case 0x0050: case 0x0051: case 0x0052: case 0x0053: case 0x0054: rd.skip(8); break;
        case 0x0038: case 0x0039: case 0x003A: case 0x003B: case 0x003C:
        case 0x0048: case 0x0049: case 0x004A: case 0x004B: case 0x004C:
        case 0x0058: case 0x0059: case 0x005A: case 0x005B: case 0x005C: break;
        case 0x0060: case 0x0061: case 0x0062: case 0x0063: case 0x0064: rd.skip(12); break;
        case 0x0068: case 0x0069: case 0x006A: case 0x006B: case 0x006C: rd.skip(4); break;
        case 0x0070: case 0x0071: case 0x0072: case 0x0073: case 0x0074:
        case 0x0080: case 0x0081: case 0x0082: case 0x0083: case 0x0084: skip_region(rd); break;
        case 0x0090: case 0x0091: case 0x0098: case 0x0099: case 0x009A: case 0x009B: bits_op(rd, pc, op); break;
        case 0x00A0: rd.skip(2); break;
        case 0x00A1: { rd.skip(2); uint16_t n = rd.u16(); rd.skip(n); break; }
        case 0x00FF: return;
        case 0x0C00: rd.skip(24); break;
        default:
            if (op >= 0x0100 && op <= 0x7FFF) rd.skip((op >> 8) * 2);
            else if (op >= 0x8000 && op <= 0x80FF) break;
            else if (op >= 0x8100) { uint32_t n = rd.u32(); rd.skip(n); }
            else { logf(LOG_UNIMPL, "DrawPicture: unhandled opcode %04X\n", op); return; }
        }
    }
}

}  // namespace pt::qd

using namespace pt;
using namespace pt::qd;

HLE(DrawPicture) {
    Rect r = read_rect(ARG(1));
    logf(LOG_GFX, "DrawPicture %08X into [%d,%d,%d,%d]\n", ARG(0), r.left, r.top, r.right, r.bottom);
    draw_picture(ARG(0), r);
}
