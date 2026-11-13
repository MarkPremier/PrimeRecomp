/* Read-only HFS volume reader (Inside Macintosh: Files, chapter 2).
 *
 * Used to read the System file from the player's own Mac OS disk image, and
 * to copy the game off its CD on first run. Works on an in-memory image; a
 * CD's Apple partition map is followed to its HFS partition.
 */
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace pt::hfs {

struct Extent { uint16_t start, count; };

class Volume {
public:
    bool open(const std::wstring &path) {
        FILE *f = _wfopen(path.c_str(), L"rb");
        if (!f) return false;
        _fseeki64(f, 0, SEEK_END);
        std::vector<uint8_t> img((size_t)_ftelli64(f));
        _fseeki64(f, 0, SEEK_SET);
        size_t n = fread(img.data(), 1, img.size(), f);
        fclose(f);
        if (n != img.size()) return false;
        return open(std::move(img));
    }

    /* an HFS volume, or a disk with an Apple partition map holding one */
    bool open(std::vector<uint8_t> &&img) {
        img_ = std::move(img);
        base_ = 0;
        if (img_.size() > 4 && img_[0] == 'E' && img_[1] == 'R') {
            size_t blk = be16(&img_[2]);
            if (!blk) return false;
            for (size_t i = 1; (i + 1) * blk <= img_.size() && img_[i * blk] == 'P' && img_[i * blk + 1] == 'M'; i++) {
                const uint8_t *pm = &img_[i * blk];
                if (strncmp((const char *)pm + 48, "Apple_HFS", 32) == 0) {
                    base_ = (size_t)be32(pm + 8) * 512;
                    break;
                }
            }
        }
        if (base_ + 1024 + 162 > img_.size()) return false;
        const uint8_t *mdb = &img_[base_ + 1024];
        if (be16(mdb) != 0x4244) return false;   /* 'BD' */
        name_.assign((const char *)mdb + 37, mdb[36] < 28 ? mdb[36] : 27);
        blk_size_ = be32(mdb + 20);
        alloc_start_ = be16(mdb + 28);
        for (int i = 0; i < 3; i++) {
            xt_ext_[i] = {be16(mdb + 134 + 4 * i), be16(mdb + 136 + 4 * i)};
            ct_ext_[i] = {be16(mdb + 150 + 4 * i), be16(mdb + 152 + 4 * i)};
        }
        overflow_ = read_extents(xt_ext_, be32(mdb + 130), 0, 3, 0);
        catalog_ = read_extents(ct_ext_, be32(mdb + 146), 0, 4, 0);
        return !catalog_.empty();
    }

    const std::string &name() const { return name_; }

    /* catalog record (folder: type 1, file: type 2) for `name` in folder `parent` (2 = root) */
    bool find(uint32_t parent, const std::string &name, std::vector<uint8_t> &rec) {
        bool found = false;
        walk_leaves(catalog_, [&](const uint8_t *key, const uint8_t *data) {
            if (found || be32(key + 2) != parent) return;
            std::string nm((const char *)key + 7, key[6]);
            if ((data[0] == 1 || data[0] == 2) && eq(nm, name)) {
                rec.assign(data, data + 102);
                found = true;
            }
        });
        return found;
    }

    /* names of the files and folders in folder `parent`, in catalog order */
    std::vector<std::string> list(uint32_t parent) {
        std::vector<std::string> names;
        walk_leaves(catalog_, [&](const uint8_t *key, const uint8_t *data) {
            if (be32(key + 2) == parent && (data[0] == 1 || data[0] == 2))
                names.emplace_back((const char *)key + 7, key[6]);
        });
        return names;
    }

    static bool is_folder(const std::vector<uint8_t> &rec) { return rec[0] == 1; }
    static uint32_t dir_id(const std::vector<uint8_t> &rec) { return be32(&rec[6]); }
    static uint32_t file_type(const std::vector<uint8_t> &rec) { return be32(&rec[4]); }
    static uint32_t file_creator(const std::vector<uint8_t> &rec) { return be32(&rec[8]); }
    static uint16_t finder_flags(const std::vector<uint8_t> &rec) { return be16(&rec[12]); }

    std::vector<uint8_t> data_fork(const std::vector<uint8_t> &rec) { return fork(rec, 74, 26, 0); }
    std::vector<uint8_t> rsrc_fork(const std::vector<uint8_t> &rec) { return fork(rec, 86, 36, 0xFF); }

private:
    std::vector<uint8_t> img_, catalog_, overflow_;
    size_t base_ = 0;
    std::string name_;
    uint32_t blk_size_ = 0;
    uint16_t alloc_start_ = 0;
    Extent xt_ext_[3]{}, ct_ext_[3]{};

    static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
    static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
    static bool eq(const std::string &a, const std::string &b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); i++)
            if (toupper((unsigned char)a[i]) != toupper((unsigned char)b[i])) return false;
        return true;
    }

    std::vector<uint8_t> fork(const std::vector<uint8_t> &rec, int ext_off, int len_off, uint8_t type) {
        if (rec[0] != 2) return {};
        Extent ext[3];
        for (int i = 0; i < 3; i++) ext[i] = {be16(&rec[ext_off + 4 * i]), be16(&rec[ext_off + 2 + 4 * i])};
        return read_extents(ext, be32(&rec[len_off]), be32(&rec[20]), type, 1);
    }

    /* read `length` bytes of a fork; fork_type 0 data / 0xFF rsrc, use_overflow for files */
    std::vector<uint8_t> read_extents(const Extent *first, uint32_t length, uint32_t file_id, uint8_t fork_type,
                                      int use_overflow) {
        std::vector<uint8_t> out;
        std::vector<Extent> exts(first, first + 3);
        uint32_t blocks_done = 0;
        for (size_t i = 0; out.size() < length; i++) {
            if (i == exts.size() && (!use_overflow || !more_extents(file_id, fork_type, (uint16_t)blocks_done, exts)))
                break;
            const Extent e = exts[i];
            if (!e.count) {
                if (!use_overflow || !more_extents(file_id, fork_type, (uint16_t)blocks_done, exts)) break;
                continue;
            }
            size_t off = base_ + (size_t)alloc_start_ * 512 + (size_t)e.start * blk_size_;
            size_t n = (size_t)e.count * blk_size_;
            if (off + n > img_.size()) break;
            out.insert(out.end(), img_.begin() + off, img_.begin() + off + n);
            blocks_done += e.count;
        }
        if (out.size() > length) out.resize(length);
        return out;
    }

    bool more_extents(uint32_t file_id, uint8_t fork, uint16_t start_block, std::vector<Extent> &exts) {
        bool found = false;
        walk_leaves(overflow_, [&](const uint8_t *key, const uint8_t *data) {
            if (!found && key[1] == fork && be32(key + 2) == file_id && be16(key + 6) == start_block) {
                for (int i = 0; i < 3; i++) exts.push_back({be16(data + 4 * i), be16(data + 2 + 4 * i)});
                found = true;
            }
        });
        return found;
    }

    template <class F> void walk_leaves(const std::vector<uint8_t> &tree, F fn) {
        if (tree.size() < 512) return;
        const uint8_t *hdr = &tree[14];
        uint32_t node = be32(hdr + 10);   /* bthFNode: first leaf */
        uint16_t nsize = be16(hdr + 18);
        int guard = 0;
        while (node && nsize && (size_t)(node + 1) * nsize <= tree.size() && guard++ < 1 << 20) {
            const uint8_t *n = &tree[(size_t)node * nsize];
            uint16_t nrecs = be16(n + 10);
            for (uint16_t r = 0; r < nrecs; r++) {
                uint16_t off = be16(n + nsize - 2 * (r + 1));
                if (off >= nsize) break;
                const uint8_t *key = n + off;
                uint32_t data_off = (1 + key[0] + 1) & ~1u;
                fn(key, key + data_off);
            }
            node = be32(n);   /* ndFLink */
        }
    }
};

}  // namespace pt::hfs
