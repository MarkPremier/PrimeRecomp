/* Cinepak ('cvid') video decoder.
 *
 * Bitstream as documented by the MultimediaWiki and implemented by FFmpeg's
 * cinepak.c: frames of horizontal strips; each strip carries V4/V1 codebooks
 * (full or partial updates, colour or greyscale) and 4x4 block vectors (intra,
 * inter with skip flags, or V1-only). Output is 0x00RRGGBB.
 */
#include "pt/cinepak.h"

#include <algorithm>
#include <cstring>

namespace pt::cvid {

static inline uint32_t rb16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }
static inline uint32_t rb24(const uint8_t *p) { return (uint32_t)p[0] << 16 | p[1] << 8 | p[2]; }
static inline uint32_t rb32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static inline int clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

void Decoder::reset(int w, int h) {
    width_ = w;
    height_ = h;
    frame_.assign((size_t)w * h, 0);
    strips_.clear();
}

void Decoder::decode_codebook(Entry *cb, int id, const uint8_t *d, const uint8_t *end) {
    int n = (id & 0x04) ? 4 : 6;
    uint32_t flag = 0, mask = 0;
    for (int i = 0; i < 256; i++) {
        if ((id & 0x01) && !(mask >>= 1)) {
            if (d + 4 > end) break;
            flag = rb32(d);
            d += 4;
            mask = 0x80000000u;
        }
        if (!(id & 0x01) || (flag & mask)) {
            if (d + n > end) break;
            int u = 0, v = 0;
            if (n == 6) { u = (int8_t)d[4]; v = (int8_t)d[5]; }
            for (int k = 0; k < 4; k++) {
                int y = d[k];
                int r = clamp8(y + 2 * v), g = clamp8(y - u / 2 - v), b = clamp8(y + 2 * u);
                cb[i].rgb[k] = (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
            }
            d += n;
        }
    }
}

void Decoder::decode_vectors(Strip &s, int id, const uint8_t *d, const uint8_t *end) {
    uint32_t flag = 0, mask = 0;
    for (int y = s.y1; y < s.y2; y += 4) {
        for (int x = s.x1; x < s.x2; x += 4) {
            if ((id & 0x01) && !(mask >>= 1)) {
                if (d + 4 > end) return;
                flag = rb32(d);
                d += 4;
                mask = 0x80000000u;
            }
            if ((id & 0x01) && !(flag & mask)) continue;   /* inter: block unchanged */
            if (!(id & 0x02) && !(mask >>= 1)) {
                if (d + 4 > end) return;
                flag = rb32(d);
                d += 4;
                mask = 0x80000000u;
            }
            uint32_t px[16];
            if ((id & 0x02) || (~flag & mask)) {
                if (d >= end) return;
                const Entry &e = s.v1[*d++];
                for (int r = 0; r < 4; r++)
                    for (int c = 0; c < 4; c++) px[r * 4 + c] = e.rgb[(r >> 1) * 2 + (c >> 1)];
            } else {
                if (d + 4 > end) return;
                const Entry *q[4] = {&s.v4[d[0]], &s.v4[d[1]], &s.v4[d[2]], &s.v4[d[3]]};
                d += 4;
                for (int r = 0; r < 4; r++)
                    for (int c = 0; c < 4; c++) {
                        const Entry &e = *q[(r >> 1) * 2 + (c >> 1)];
                        px[r * 4 + c] = e.rgb[(r & 1) * 2 + (c & 1)];
                    }
            }
            for (int r = 0; r < 4; r++) {
                int yy = y + r;
                if (yy >= height_) break;
                for (int c = 0; c < 4; c++) {
                    int xx = x + c;
                    if (xx < width_) frame_[(size_t)yy * width_ + xx] = px[r * 4 + c];
                }
            }
        }
    }
}

bool Decoder::decode(const uint8_t *data, size_t size) {
    if (size < 10) return false;
    const uint8_t *end = data + size;
    int flags = data[0];
    size_t frame_size = rb24(data + 1);
    if (frame_size && frame_size < size) end = data + frame_size;
    int strips = (int)rb16(data + 8);
    data += 10;
    if ((int)strips_.size() < strips) strips_.resize(strips);
    int y0 = 0;
    for (int i = 0; i < strips && data + 12 <= end; i++) {
        Strip &s = strips_[i];
        size_t ssize = rb24(data + 1);
        int top = (int)rb16(data + 4), bottom = (int)rb16(data + 8);
        s.x1 = 0;
        s.x2 = width_;
        if (top == 0) { s.y1 = y0; s.y2 = y0 + bottom; }
        else { s.y1 = top; s.y2 = bottom; }
        s.y2 = std::min(s.y2, height_);
        if (ssize < 12 || data + ssize > end) ssize = (size_t)(end - data);
        if (i > 0 && !(flags & 0x01)) {   /* codebooks carry over from the previous strip */
            memcpy(s.v4, strips_[i - 1].v4, sizeof s.v4);
            memcpy(s.v1, strips_[i - 1].v1, sizeof s.v1);
        }
        const uint8_t *p = data + 12, *se = data + ssize;
        while (p + 4 <= se) {
            int id = p[0];
            size_t csize = rb24(p + 1);
            if (csize < 4 || p + csize > se) break;
            const uint8_t *cd = p + 4, *ce = p + csize;
            switch (id) {
            case 0x20: case 0x21: case 0x24: case 0x25: decode_codebook(s.v4, id, cd, ce); break;
            case 0x22: case 0x23: case 0x26: case 0x27: decode_codebook(s.v1, id, cd, ce); break;
            case 0x30: case 0x31: case 0x32: decode_vectors(s, id, cd, ce); break;
            default: break;
            }
            p += csize;
        }
        y0 = s.y2;
        data += ssize;
    }
    return true;
}

}  // namespace pt::cvid
