/* Read-only HFS reader for the user's own Mac OS disk image.
 *
 * Used only to load the System file's resource fork and the font suitcases in
 * System Folder:Fonts at runtime (cursors, patterns, beeps, bitmap fonts),
 * so no Apple system software is copied into the project.
 * Format: Inside Macintosh: Files, chapter 2 ("Data Organization on Volumes").
 */
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "pt/runtime.h"

namespace pt::hfs {

struct Extent { uint16_t start, count; };

class Volume {
public:
    bool open(const std::wstring &path) {
        FILE *f = _wfopen(path.c_str(), L"rb");
        if (!f) return false;
        _fseeki64(f, 0, SEEK_END);
        img_.resize((size_t)_ftelli64(f));
        _fseeki64(f, 0, SEEK_SET);
        fread(img_.data(), 1, img_.size(), f);
        fclose(f);
        const uint8_t *mdb = &img_[1024];
        if (be16(mdb) != 0x4244) return false;   /* 'BD' */
        blk_size_ = be32(mdb + 20);
        alloc_start_ = be16(mdb + 28);
        for (int i = 0; i < 3; i++) {
            xt_ext_[i] = {be16(mdb + 134 + 4 * i), be16(mdb + 136 + 4 * i)};
            ct_ext_[i] = {be16(mdb + 150 + 4 * i), be16(mdb + 152 + 4 * i)};
        }
        catalog_ = read_extents(ct_ext_, be32(mdb + 146), 0, 4, 0);
        overflow_ = read_extents(xt_ext_, be32(mdb + 130), 0, 3, 0);
        return !catalog_.empty();
    }

    /* file record for name inside parent dirID; returns false if absent */
    bool find(uint32_t parent, const std::string &name, std::vector<uint8_t> &rec) {
        bool found = false;
        walk_leaves(catalog_, [&](const uint8_t *key, const uint8_t *data) {
            uint32_t par = be32(key + 2);
            std::string nm((const char *)key + 7, key[6]);
            if (par == parent && eq(nm, name)) { rec.assign(data, data + 102); found = true; }
        });
        return found;
    }

    std::vector<std::string> list(uint32_t parent) {
        std::vector<std::string> names;
        walk_leaves(catalog_, [&](const uint8_t *key, const uint8_t *data) {
            if (be32(key + 2) == parent && (data[0] == 1 || data[0] == 2))
                names.emplace_back((const char *)key + 7, key[6]);
        });
        return names;
    }

    static uint32_t dir_id(const std::vector<uint8_t> &rec) { return be32(&rec[6]); }

    std::vector<uint8_t> rsrc_fork(const std::vector<uint8_t> &rec) {
        if (rec[0] != 2) return {};
        Extent ext[3];
        for (int i = 0; i < 3; i++) ext[i] = {be16(&rec[86 + 4 * i]), be16(&rec[88 + 4 * i])};
        return read_extents(ext, be32(&rec[36]), be32(&rec[20]), 0xFF, 1);
    }

private:
    std::vector<uint8_t> img_, catalog_, overflow_;
    uint32_t blk_size_ = 0;
    uint16_t alloc_start_ = 0;
    Extent xt_ext_[3], ct_ext_[3];

    static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
    static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
    static bool eq(const std::string &a, const std::string &b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); i++)
            if (toupper((unsigned char)a[i]) != toupper((unsigned char)b[i])) return false;
        return true;
    }

    /* read `length` bytes of a fork; fork_type 0 data / 0xFF rsrc, use_overflow for files */
    std::vector<uint8_t> read_extents(const Extent *first, uint32_t length, uint32_t file_id, uint8_t fork_type,
                                      int use_overflow) {
        std::vector<uint8_t> out;
        std::vector<Extent> exts(first, first + 3);
        uint32_t blocks_done = 0;
        for (size_t i = 0; out.size() < length; i++) {
            if (i == exts.size()) {
                if (!use_overflow || !more_extents(file_id, fork_type, (uint16_t)blocks_done, exts)) break;
            }
            const Extent &e = exts[i];
            if (!e.count) {
                if (!use_overflow || !more_extents(file_id, fork_type, (uint16_t)blocks_done, exts)) break;
                continue;
            }
            size_t off = (size_t)alloc_start_ * 512 + (size_t)e.start * blk_size_;
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
            if (key[1] == fork && be32(key + 2) == file_id && be16(key + 6) == start_block) {
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
        while (node && (size_t)(node + 1) * nsize <= tree.size()) {
            const uint8_t *n = &tree[(size_t)node * nsize];
            uint16_t nrecs = be16(n + 10);
            for (uint16_t r = 0; r < nrecs; r++) {
                uint16_t off = be16(n + nsize - 2 * (r + 1));
                const uint8_t *key = n + off;
                uint32_t data_off = (1 + key[0] + 1) & ~1u;
                fn(key, key + data_off);
            }
            node = be32(n);   /* ndFLink */
        }
    }
};

/* Loads System + font suitcases; returns resource forks keyed by name. */
std::vector<std::pair<std::string, std::vector<uint8_t>>> load_system(const std::wstring &image) {
    std::vector<std::pair<std::string, std::vector<uint8_t>>> out;
    Volume v;
    if (!v.open(image)) return out;
    std::vector<uint8_t> sysfolder, rec;
    if (!v.find(2, "System Folder", sysfolder) || sysfolder[0] != 1) return out;
    uint32_t sf = Volume::dir_id(sysfolder);
    if (v.find(sf, "System", rec)) out.emplace_back("System", v.rsrc_fork(rec));
    std::vector<uint8_t> fonts;
    if (v.find(sf, "Fonts", fonts) && fonts[0] == 1) {
        uint32_t fd = Volume::dir_id(fonts);
        for (auto &name : v.list(fd)) {
            std::vector<uint8_t> frec;
            if (v.find(fd, name, frec) && frec[0] == 2) {
                auto fork = v.rsrc_fork(frec);
                if (!fork.empty()) out.emplace_back(name, std::move(fork));
            }
        }
    }
    return out;
}

}  // namespace pt::hfs

namespace pt::res::hfs_glue {
std::vector<std::pair<std::string, std::vector<uint8_t>>> load(const std::wstring &image) {
    return pt::hfs::load_system(image);
}
}  // namespace pt::res::hfs_glue
