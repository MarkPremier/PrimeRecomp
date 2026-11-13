/* PEF loader: instantiates the game's PowerPC fragment (its application file's
 * data fork) at the fixed guest addresses the recompiled code was made for.
 *
 * A C++ port of tools/pef.py: sections, pattern-initialised data, and the
 * loader section's relocation bytecode ("Mac OS Runtime Architectures",
 * chapter 8). The file's SHA-1 must equal the one the code was translated
 * from, so the result is identical to what the generator saw.
 */
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "image_info.h"
#include "pt/layout.h"
#include "pt/runtime.h"

namespace pt {

/* ---------- SHA-1 (FIPS 180-4) ---------- */
std::string sha1_hex(const uint8_t *data, size_t len) {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    auto rol = [](uint32_t v, int n) { return (v << n) | (v >> (32 - n)); };
    std::vector<uint8_t> m(data, data + len);
    uint64_t bits = (uint64_t)len * 8;
    m.push_back(0x80);
    while (m.size() % 64 != 56) m.push_back(0);
    for (int i = 7; i >= 0; i--) m.push_back((uint8_t)(bits >> (8 * i)));
    for (size_t off = 0; off < m.size(); off += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++)
            w[i] = (uint32_t)m[off + 4 * i] << 24 | (uint32_t)m[off + 4 * i + 1] << 16 |
                   (uint32_t)m[off + 4 * i + 2] << 8 | m[off + 4 * i + 3];
        for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6; }
            uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    char out[41];
    for (int i = 0; i < 5; i++) snprintf(out + 8 * i, 9, "%08x", h[i]);
    return out;
}

namespace {

uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

struct Section {
    uint32_t total, unpacked, packed, offset;
    uint8_t kind;
    uint32_t base = 0;
    bool placed = false;
};

struct Loader {
    const std::vector<uint8_t> &blob;
    std::vector<Section> sec;
    std::vector<uint32_t> import_addr;
    std::string err;

    explicit Loader(const std::vector<uint8_t> &b) : blob(b) {}

    bool fail(const std::string &e) { err = e; return false; }

    uint32_t read_arg(const uint8_t *p, size_t &pos, size_t end) {
        uint32_t v = 0;
        while (pos < end) {
            uint8_t b = p[pos++];
            v = (v << 7) | (b & 0x7F);
            if (!(b & 0x80)) break;
        }
        return v;
    }

    bool unpack_pidata(const uint8_t *p, size_t n, uint32_t dst, uint32_t unpacked) {
        size_t pos = 0;
        uint32_t out = 0;
        auto put = [&](const uint8_t *src, uint32_t len) {
            if (out + len > unpacked) return false;
            if (src) memcpy(g_mem + dst + out, src, len);
            else memset(g_mem + dst + out, 0, len);
            out += len;
            return true;
        };
        while (pos < n) {
            uint8_t op = p[pos++];
            uint32_t opcode = op >> 5, count = op & 0x1F;
            if (count == 0) count = read_arg(p, pos, n);
            if (opcode == 0) {
                if (!put(nullptr, count)) return fail("pidata overflow");
            } else if (opcode == 1) {
                if (pos + count > n || !put(p + pos, count)) return fail("pidata overflow");
                pos += count;
            } else if (opcode == 2) {
                uint32_t repeat = read_arg(p, pos, n);
                if (pos + count > n) return fail("pidata overflow");
                for (uint32_t i = 0; i <= repeat; i++)
                    if (!put(p + pos, count)) return fail("pidata overflow");
                pos += count;
            } else if (opcode == 3 || opcode == 4) {
                uint32_t custom = read_arg(p, pos, n), repeat = read_arg(p, pos, n);
                const uint8_t *common = nullptr;
                if (opcode == 3) {
                    if (pos + count > n) return fail("pidata overflow");
                    common = p + pos;
                    pos += count;
                }
                if (!put(common, count)) return fail("pidata overflow");
                for (uint32_t i = 0; i < repeat; i++) {
                    if (pos + custom > n || !put(p + pos, custom)) return fail("pidata overflow");
                    pos += custom;
                    if (!put(common, count)) return fail("pidata overflow");
                }
            } else {
                return fail("bad pidata opcode");
            }
        }
        if (out != unpacked) return fail("pidata unpacked to the wrong size");
        return true;
    }

    /* relocation state for one target section */
    struct RState { uint32_t addr = 0, imp = 0, C = 0, D = 0; };

    bool run(const std::vector<uint16_t> &ins, size_t pc, size_t stop, Section &t, RState &st) {
        auto add = [&](uint32_t delta) {
            if (st.addr + 4 > t.total) return fail("relocation outside its section");
            uint32_t a = t.base + st.addr;
            st32(a, ld32(a) + delta);
            st.addr += 4;
            return true;
        };
        auto base_of = [&](uint32_t i, uint32_t &out) {
            if (i >= sec.size() || !sec[i].placed) return fail("relocation references an unplaced section");
            out = sec[i].base;
            return true;
        };
        auto imp = [&](uint32_t i, uint32_t &out) {
            if (i >= import_addr.size()) return fail("relocation references a missing import");
            out = import_addr[i];
            return true;
        };
        while (pc < stop) {
            uint16_t w = ins[pc++];
            uint32_t v;
            if (w >> 14 == 0) {                                   /* RelocBySectDWithSkip */
                st.addr += ((w >> 6) & 0xFF) * 4;
                for (int i = 0; i < (w & 0x3F); i++)
                    if (!add(st.D)) return false;
            } else if (w >> 13 == 2) {                            /* RelocRun group */
                int sub = (w >> 9) & 0xF, n = (w & 0x1FF) + 1;
                for (int i = 0; i < n; i++) {
                    switch (sub) {
                    case 0: if (!add(st.C)) return false; break;
                    case 1: if (!add(st.D)) return false; break;
                    case 2: if (!add(st.C) || !add(st.D)) return false; st.addr += 4; break;
                    case 3: if (!add(st.C) || !add(st.D)) return false; break;
                    case 4: if (!add(st.D)) return false; st.addr += 4; break;
                    case 5: if (!imp(st.imp, v) || !add(v)) return false; st.imp++; break;
                    default: return fail("bad RelocRun subopcode");
                    }
                }
            } else if (w >> 13 == 3) {                            /* RelocSmIndex group */
                int sub = (w >> 9) & 0xF;
                uint32_t idx = w & 0x1FF;
                if (sub == 0) { if (!imp(idx, v) || !add(v)) return false; st.imp = idx + 1; }
                else if (sub == 1) { if (!base_of(idx, st.C)) return false; }
                else if (sub == 2) { if (!base_of(idx, st.D)) return false; }
                else if (sub == 3) { if (!base_of(idx, v) || !add(v)) return false; }
                else return fail("bad RelocSmIndex subopcode");
            } else if (w >> 12 == 8) {                            /* RelocIncrPosition */
                st.addr += (w & 0xFFF) + 1;
            } else if (w >> 12 == 9) {                            /* RelocSmRepeat */
                size_t blocks = ((w >> 8) & 0xF) + 1, times = (w & 0xFF) + 1;
                if (pc - 1 < blocks) return fail("bad RelocSmRepeat");
                for (size_t i = 0; i < times; i++)
                    if (!run(ins, pc - 1 - blocks, pc - 1, t, st)) return false;
            } else if (w >> 10 == 0x28) {                         /* RelocSetPosition */
                if (pc >= ins.size()) return fail("truncated relocation");
                st.addr = (uint32_t)(w & 0x3FF) << 16 | ins[pc++];
            } else if (w >> 10 == 0x29) {                         /* RelocLgByImport */
                if (pc >= ins.size()) return fail("truncated relocation");
                uint32_t idx = (uint32_t)(w & 0x3FF) << 16 | ins[pc++];
                if (!imp(idx, v) || !add(v)) return false;
                st.imp = idx + 1;
            } else if (w >> 10 == 0x2C) {                         /* RelocLgRepeat */
                if (pc >= ins.size()) return fail("truncated relocation");
                size_t blocks = ((w >> 6) & 0xF) + 1, times = (uint32_t)(w & 0x3F) << 16 | ins[pc++];
                if (pc - 2 < blocks) return fail("bad RelocLgRepeat");
                for (size_t i = 0; i < times; i++)
                    if (!run(ins, pc - 2 - blocks, pc - 2, t, st)) return false;
            } else if (w >> 10 == 0x2D) {                         /* RelocLgSetOrBySection */
                if (pc >= ins.size()) return fail("truncated relocation");
                int sub = (w >> 6) & 0xF;
                uint32_t idx = (uint32_t)(w & 0x3F) << 16 | ins[pc++];
                if (sub == 0) { if (!base_of(idx, v) || !add(v)) return false; }
                else if (sub == 1) { if (!base_of(idx, st.C)) return false; }
                else if (sub == 2) { if (!base_of(idx, st.D)) return false; }
                else return fail("bad RelocLgSetOrBySection subopcode");
            } else {
                return fail("unknown relocation opcode");
            }
        }
        return true;
    }

    bool load() {
        const uint8_t *b = blob.data();
        if (blob.size() < 40 || memcmp(b, "Joy!peffpwpc", 12) != 0) return fail("not a PowerPC PEF fragment");
        uint16_t nsec = be16(b + 32);
        if (40 + 28 * (size_t)nsec > blob.size()) return fail("truncated section table");
        for (int i = 0; i < nsec; i++) {
            const uint8_t *s = b + 40 + 28 * i;
            Section x{be32(s + 8), be32(s + 12), be32(s + 16), be32(s + 20), s[24]};
            if ((size_t)x.offset + x.packed > blob.size()) return fail("section outside the file");
            sec.push_back(x);
        }
        if (sec.size() < 2 || sec[0].total != PT_IMAGE_CODE_SIZE || sec[1].total != PT_IMAGE_DATA_SIZE)
            return fail("section sizes differ from the recompiled build");
        sec[0].base = PT_IMAGE_CODE_BASE;
        sec[1].base = PT_IMAGE_DATA_BASE;
        const Section *ld = nullptr;
        for (auto &s : sec) if (s.kind == 4) ld = &s;
        if (!ld) return fail("no loader section");
        /* place code and data */
        for (int i = 0; i < 2; i++) {
            Section &s = sec[i];
            memset(g_mem + s.base, 0, s.total);
            if (s.kind == 2) {
                if (!unpack_pidata(b + s.offset, s.packed, s.base, s.unpacked)) return false;
            } else {
                memcpy(g_mem + s.base, b + s.offset, s.unpacked < s.packed ? s.unpacked : s.packed);
            }
            s.placed = true;
        }
        /* loader section: imports, then relocations */
        const uint8_t *L = b + ld->offset;
        size_t Ln = ld->packed;
        if (Ln < 56) return fail("truncated loader section");
        uint32_t nlib = be32(L + 24), nsym = be32(L + 28), nrel = be32(L + 32), rel_off = be32(L + 36);
        if (nsym != PT_IMAGE_IMPORT_COUNT) return fail("import count differs from the recompiled build");
        size_t sym_base = 56 + (size_t)nlib * 24;
        if (sym_base + nsym * 4 + nrel * 12 > Ln) return fail("truncated loader section");
        for (uint32_t k = 0; k < nsym; k++) {
            uint32_t cls = (L[sym_base + 4 * k] & 0x0F);
            import_addr.push_back(cls == 1 ? PT_IMPORT_DATA_BASE + k * 256 : PT_IMPORT_TVEC_BASE + k * 8);
        }
        size_t rh = sym_base + nsym * 4;
        for (uint32_t i = 0; i < nrel; i++) {
            const uint8_t *r = L + rh + 12 * i;
            uint16_t s = be16(r);
            uint32_t count = be32(r + 4), first = be32(r + 8);
            if (s >= 2) return fail("relocations for an unexpected section");
            if ((size_t)rel_off + first + 2 * (size_t)count > Ln) return fail("truncated relocations");
            std::vector<uint16_t> ins(count);
            for (uint32_t j = 0; j < count; j++) ins[j] = be16(L + rel_off + first + 2 * j);
            RState st;
            st.C = sec[0].base;
            st.D = sec[1].base;
            if (!run(ins, 0, ins.size(), sec[s], st)) return false;
        }
        return true;
    }
};

}  // namespace

/* Loads the fragment into guest memory; returns an empty string or the reason it was refused. */
std::string load_game_fragment(const std::vector<uint8_t> &blob) {
    std::string sum = sha1_hex(blob.data(), blob.size());
    if (sum != PT_IMAGE_SHA1)
        return "this is not the " PT_IMAGE_APP_NAME " application this port was built from (SHA-1 " + sum +
               ", expected " PT_IMAGE_SHA1 ")";
    Loader l(blob);
    if (!l.load()) return l.err;
    return {};
}

}  // namespace pt
