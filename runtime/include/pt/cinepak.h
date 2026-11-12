#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace pt::cvid {

class Decoder {
public:
    void reset(int width, int height);
    bool decode(const uint8_t *data, size_t size);
    const std::vector<uint32_t> &frame() const { return frame_; }   /* 0x00RRGGBB, width*height */
    int width() const { return width_; }
    int height() const { return height_; }

private:
    struct Entry { uint32_t rgb[4]; };
    struct Strip { Entry v4[256]{}, v1[256]{}; int x1 = 0, y1 = 0, x2 = 0, y2 = 0; };
    void decode_codebook(Entry *cb, int id, const uint8_t *d, const uint8_t *end);
    void decode_vectors(Strip &s, int id, const uint8_t *d, const uint8_t *end);
    int width_ = 0, height_ = 0;
    std::vector<uint32_t> frame_;
    std::vector<Strip> strips_;
};

}  // namespace pt::cvid
