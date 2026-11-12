/* File Manager, Folder Manager, Alias Manager (trivial), Process Manager info.
 * Parameter-block offsets follow Inside Macintosh: Files (1992). */
#include "pt/files.h"

#include <io.h>

#include <chrono>
#include <cstdio>
#include <map>
#include <set>

#include "pt/runtime.h"
#include "pt/vfs.h"

namespace fs = std::filesystem;
using namespace pt;
using namespace pt::vfs;

static const int16_t kAppleCDRefNum = -10;  /* must match cdaudio.cpp */

namespace pt::files {

struct OpenFile {
    FILE *fp = nullptr;
    fs::path host;
    bool rsrc = false;
    bool writable = false;
    uint32_t pos = 0;
};
static std::map<int16_t, OpenFile> g_open;
static int16_t g_next_ref = 3;

static std::set<int16_t> g_reserved;   /* refNums held by in-memory resource files */
int16_t alloc_refnum() {
    while (g_open.count(g_next_ref) || g_reserved.count(g_next_ref)) g_next_ref++;
    g_reserved.insert(g_next_ref);
    return g_next_ref++;
}

int16_t open_fork(const fs::path &host, bool rsrc, int perm) {
    fs::path p = rsrc ? rsrc_path(host) : host;
    std::error_code ec;
    if (!fs::exists(host, ec)) return fnfErr;
    bool want_write = perm == 2 || perm == 3 || perm == 0;
    FILE *fp = nullptr;
    if (want_write) {
        if (!fs::exists(p, ec)) { FILE *t = _wfopen(p.c_str(), L"wb"); if (t) fclose(t); }
        fp = _wfopen(p.c_str(), L"r+b");
    }
    bool writable = fp != nullptr;
    if (!fp) {
        if (perm == 2 || perm == 3) return wrPermErr;
        fp = _wfopen(p.c_str(), L"rb");
    }
    if (!fp) {
        if (rsrc) { /* missing resource fork: an empty, read-only fork */
            fp = _wfopen(L"NUL", L"rb");
        }
        if (!fp) return fnfErr;
    }
    int16_t ref = alloc_refnum();
    g_open[ref] = OpenFile{fp, host, rsrc, writable, 0};
    logf(LOG_FILE, "open %s%s perm %d -> ref %d\n", host.u8string().c_str(), rsrc ? " [rsrc]" : "", perm, ref);
    return ref;
}

static OpenFile *get(int16_t ref) {
    auto it = g_open.find(ref);
    return it == g_open.end() ? nullptr : &it->second;
}

bool is_open(int16_t ref) { return get(ref) != nullptr; }
fs::path host_of(int16_t ref) { OpenFile *f = get(ref); return f ? f->host : fs::path(); }

int16_t close(int16_t ref) {
    OpenFile *f = get(ref);
    if (!f) return rfNumErr;
    fclose(f->fp);
    g_open.erase(ref);
    return noErr;
}

int16_t get_eof(int16_t ref, uint32_t &eof) {
    OpenFile *f = get(ref);
    if (!f) return rfNumErr;
    _fseeki64(f->fp, 0, SEEK_END);
    eof = (uint32_t)_ftelli64(f->fp);
    return noErr;
}

int16_t read(int16_t ref, uint32_t buffer, uint32_t &count) {
    OpenFile *f = get(ref);
    if (!f) { count = 0; return rfNumErr; }
    _fseeki64(f->fp, f->pos, SEEK_SET);
    size_t n = fread(g_mem + buffer, 1, count, f->fp);
    f->pos += (uint32_t)n;
    int16_t err = n < count ? eofErr : noErr;
    logf(LOG_FILE, "read ref %d: %u of %u bytes at %u\n", ref, (unsigned)n, count, f->pos - (uint32_t)n);
    count = (uint32_t)n;
    return err;
}

int16_t write(int16_t ref, uint32_t buffer, uint32_t &count) {
    OpenFile *f = get(ref);
    if (!f) { count = 0; return rfNumErr; }
    if (!f->writable) { count = 0; return wrPermErr; }
    _fseeki64(f->fp, f->pos, SEEK_SET);
    size_t n = fwrite(g_mem + buffer, 1, count, f->fp);
    fflush(f->fp);
    f->pos += (uint32_t)n;
    count = (uint32_t)n;
    return n < count ? dskFulErr : noErr;
}

int16_t set_pos(int16_t ref, int16_t mode, int32_t offset) {
    OpenFile *f = get(ref);
    if (!f) return rfNumErr;
    uint32_t eof;
    get_eof(ref, eof);
    int64_t np;
    switch (mode & 3) {
    case 0: return noErr;                          /* fsAtMark */
    case 1: np = offset; break;                    /* fsFromStart */
    case 2: np = (int64_t)eof + offset; break;     /* fsFromLEOF */
    default: np = (int64_t)f->pos + offset; break; /* fsFromMark */
    }
    if (np < 0) return posErr;
    if (np > eof) { f->pos = eof; return eofErr; }
    f->pos = (uint32_t)np;
    return noErr;
}

int16_t get_pos(int16_t ref, uint32_t &pos) {
    OpenFile *f = get(ref);
    if (!f) return rfNumErr;
    pos = f->pos;
    return noErr;
}

int16_t set_eof(int16_t ref, uint32_t eof) {
    OpenFile *f = get(ref);
    if (!f) return rfNumErr;
    if (!f->writable) return wrPermErr;
    fflush(f->fp);
    if (_chsize_s(_fileno(f->fp), eof)) return dskFulErr;
    if (f->pos > eof) f->pos = eof;
    return noErr;
}

}  // namespace pt::files

/* ======================= helpers ======================= */
static bool spec_host(const Spec &s, fs::path &out, bool &is_dir) {
    Entry e;
    if (!lookup(s.vref, s.parent, s.name, e)) return false;
    out = e.host;
    is_dir = e.is_dir;
    return true;
}

static void put_result(uint32_t pb, int16_t err) { st16(pb + 16, (uint16_t)err); }

static uint32_t mac_date(const fs::path &p) {
    std::error_code ec;
    auto t = fs::last_write_time(p, ec);
    if (ec) return 0;
    auto secs = std::chrono::duration_cast<std::chrono::seconds>(t.time_since_epoch()).count();
    /* file_time_type epoch on MSVC is 1601-01-01 */
    int64_t unix_secs = secs - 11644473600ll;
    return (uint32_t)(unix_secs + 2082844800ll);
}

/* ======================= FSSpec calls ======================= */
HLE(FSMakeFSSpec) {
    /* OSErr FSMakeFSSpec(short vRefNum, long dirID, ConstStr255Param fileName, FSSpec *spec) */
    Spec s;
    std::string name = ARG(2) ? read_pstr(ARG(2)) : std::string();
    int16_t err = resolve((int16_t)ARG(0), ARG(1), name, s);
    if (err == noErr || err == fnfErr) write_fsspec(ARG(3), s);
    logf(LOG_FILE, "FSMakeFSSpec(%d, %u, \"%s\") -> %d (%d:%u:%s)\n", (int16_t)ARG(0), ARG(1),
         mac_to_utf8(name).c_str(), err, s.vref, s.parent, mac_to_utf8(s.name).c_str());
    RET((int32_t)err);
}

HLE(FSpOpenDF) {
    Spec s = read_fsspec(ARG(0));
    fs::path host;
    bool dir;
    if (!spec_host(s, host, dir) || dir) {
        logf(LOG_FILE, "FSpOpenDF %s: not found\n", mac_to_utf8(s.name).c_str());
        RET(fnfErr);
        return;
    }
    int16_t r = files::open_fork(host, false, (int8_t)ARG(1));
    if (r < 0) { RET(r); return; }
    st16(ARG(2), r);
    RET(0);
}

HLE(FSpCreate) {
    /* OSErr FSpCreate(const FSSpec *spec, OSType creator, OSType fileType, ScriptCode scriptTag) */
    Spec s = read_fsspec(ARG(0));
    fs::path host;
    bool dir;
    if (spec_host(s, host, dir)) { RET(dupFNErr); return; }
    fs::path p = host_path_for_new(s);
    FILE *f = _wfopen(p.c_str(), L"wb");
    if (!f) { RET(ioErr); return; }
    fclose(f);
    set_finder_info(s, ARG(2), ARG(1), 0);
    logf(LOG_FILE, "FSpCreate %s\n", p.u8string().c_str());
    RET(0);
}

HLE(FSpDelete) {
    Spec s = read_fsspec(ARG(0));
    fs::path host;
    bool dir;
    if (!spec_host(s, host, dir)) { RET(fnfErr); return; }
    std::error_code ec;
    fs::remove(host, ec);
    fs::remove(rsrc_path(host), ec);
    RET(0);
}

HLE(FSpGetFInfo) {
    Spec s = read_fsspec(ARG(0));
    Entry e;
    if (!lookup(s.vref, s.parent, s.name, e) || e.is_dir) { RET(fnfErr); return; }
    uint32_t fi = ARG(1);
    st32(fi + 0, e.type);
    st32(fi + 4, e.creator);
    st16(fi + 8, e.finder_flags);
    st32(fi + 10, 0);
    st16(fi + 14, 0);
    RET(0);
}

HLE(ResolveAliasFile) {
    /* OSErr ResolveAliasFile(FSSpec *theSpec, Boolean resolveAliasChains, Boolean *targetIsFolder, Boolean *wasAliased) */
    Spec s = read_fsspec(ARG(0));
    Entry e;
    bool found = lookup(s.vref, s.parent, s.name, e);
    if (ARG(2)) st8(ARG(2), found && e.is_dir);
    if (ARG(3)) st8(ARG(3), 0);
    RET(found ? 0 : fnfErr);
}

/* ======================= classic refNum calls ======================= */
HLE(FSRead) {
    uint32_t count = ld32(ARG(1));
    int16_t err = files::read((int16_t)ARG(0), ARG(2), count);
    st32(ARG(1), count);
    RET(err);
}
HLE(FSWrite) {
    uint32_t count = ld32(ARG(1));
    int16_t err = files::write((int16_t)ARG(0), ARG(2), count);
    st32(ARG(1), count);
    RET(err);
}
HLE(SetFPos) { RET(files::set_pos((int16_t)ARG(0), (int16_t)ARG(1), (int32_t)ARG(2))); }
HLE(FSClose) { RET(files::close((int16_t)ARG(0))); }

/* ======================= parameter-block calls ======================= */
HLE(PBReadSync) {
    uint32_t pb = ARG(0);
    int16_t ref = (int16_t)ld16(pb + 24);
    int16_t mode = (int16_t)ld16(pb + 44);
    int16_t err = files::set_pos(ref, mode, (int32_t)ld32(pb + 46));
    uint32_t count = ld32(pb + 36);
    if (err == noErr) err = files::read(ref, ld32(pb + 32), count);
    else count = 0;
    st32(pb + 40, count);
    uint32_t pos = 0;
    files::get_pos(ref, pos);
    st32(pb + 46, pos);
    put_result(pb, err);
    RET(err);
}
HLE(PBWriteSync) {
    uint32_t pb = ARG(0);
    int16_t ref = (int16_t)ld16(pb + 24);
    int16_t err = files::set_pos(ref, (int16_t)ld16(pb + 44), (int32_t)ld32(pb + 46));
    uint32_t count = ld32(pb + 36);
    if (err == noErr || err == eofErr) err = files::write(ref, ld32(pb + 32), count);
    st32(pb + 40, count);
    uint32_t pos = 0;
    files::get_pos(ref, pos);
    st32(pb + 46, pos);
    put_result(pb, err);
    RET(err);
}
HLE(PBCloseSync) {
    int16_t err = files::close((int16_t)ld16(ARG(0) + 24));
    put_result(ARG(0), err);
    RET(err);
}
HLE(PBGetEOFSync) {
    uint32_t eof = 0;
    int16_t err = files::get_eof((int16_t)ld16(ARG(0) + 24), eof);
    st32(ARG(0) + 28, eof); /* ioMisc */
    put_result(ARG(0), err);
    RET(err);
}
HLE(PBSetEOFSync) {
    int16_t err = files::set_eof((int16_t)ld16(ARG(0) + 24), ld32(ARG(0) + 28));
    put_result(ARG(0), err);
    RET(err);
}
HLE(PBGetFPosSync) {
    uint32_t pos = 0;
    int16_t err = files::get_pos((int16_t)ld16(ARG(0) + 24), pos);
    st32(ARG(0) + 46, pos);
    st16(ARG(0) + 44, 0);
    put_result(ARG(0), err);
    RET(err);
}
HLE(PBSetFPosSync) {
    int16_t err = files::set_pos((int16_t)ld16(ARG(0) + 24), (int16_t)ld16(ARG(0) + 44), (int32_t)ld32(ARG(0) + 46));
    uint32_t pos = 0;
    files::get_pos((int16_t)ld16(ARG(0) + 24), pos);
    st32(ARG(0) + 46, pos);
    put_result(ARG(0), err);
    RET(err);
}
HLE(PBFlushFileSync) { put_result(ARG(0), 0); RET(0); }
HLE(PBFlushVolSync) { put_result(ARG(0), 0); RET(0); }

static Spec pb_spec(uint32_t pb, uint32_t dirIDoff, int16_t &err) {
    /* ioNamePtr + ioVRefNum + ioDirID */
    Spec s;
    std::string name = ld32(pb + 18) ? read_pstr(ld32(pb + 18)) : std::string();
    err = resolve((int16_t)ld16(pb + 22), ld32(pb + dirIDoff), name, s);
    return s;
}

HLE(PBHOpenDFSync) {
    uint32_t pb = ARG(0);
    int16_t err;
    Spec s = pb_spec(pb, 48, err);
    fs::path host;
    bool dir;
    if (err == noErr && (!spec_host(s, host, dir) || dir)) err = fnfErr;
    if (err == noErr) {
        int16_t r = files::open_fork(host, false, (int8_t)ld8(pb + 27));
        if (r < 0) err = r;
        else st16(pb + 24, r);
    }
    put_result(pb, err);
    RET(err);
}

HLE(PBHCreateSync) {
    uint32_t pb = ARG(0);
    int16_t err;
    Spec s = pb_spec(pb, 48, err);
    if (err == noErr) err = dupFNErr;
    else if (err == fnfErr) {
        fs::path p = host_path_for_new(s);
        FILE *f = _wfopen(p.c_str(), L"wb");
        if (f) { fclose(f); set_finder_info(s, '????', '????', 0); err = noErr; }
        else err = ioErr;
    }
    put_result(pb, err);
    RET(err);
}

HLE(PBDeleteSync) {
    uint32_t pb = ARG(0);
    int16_t err;
    Spec s = pb_spec(pb, 48, err);
    fs::path host;
    bool dir;
    if (err == noErr && spec_host(s, host, dir)) {
        std::error_code ec;
        fs::remove(host, ec);
        fs::remove(rsrc_path(host), ec);
    }
    put_result(pb, err);
    RET(err);
}

HLE(PBHSetFInfoSync) {
    uint32_t pb = ARG(0);
    int16_t err;
    Spec s = pb_spec(pb, 48, err);
    if (err == noErr) set_finder_info(s, ld32(pb + 32), ld32(pb + 36), ld16(pb + 40));
    put_result(pb, err);
    RET(err);
}

/* CInfoPBRec: file and directory variants share the header */
static void fill_catinfo(uint32_t pb, int16_t vref, uint32_t parent, const Entry &e) {
    if (ld32(pb + 18)) write_pstr(ld32(pb + 18), e.mac_name);
    st16(pb + 22, (uint16_t)vref);
    st16(pb + 24, 0);      /* ioFRefNum */
    st8(pb + 30, e.is_dir ? 0x10 : 0x00);
    st8(pb + 31, 0);
    std::error_code ec;
    if (e.is_dir) {
        memset(g_mem + pb + 32, 0, 16);      /* ioDrUsrWds */
        st32(pb + 48, dir_id(vref, e.host)); /* ioDrDirID */
        uint16_t n = 0;
        for (auto &de : fs::directory_iterator(e.host, ec)) { (void)de; n++; }
        st16(pb + 52, n);                    /* ioDrNmFls (approximate) */
        st32(pb + 72, mac_date(e.host));
        st32(pb + 76, mac_date(e.host));
        st32(pb + 80, 0);
        memset(g_mem + pb + 84, 0, 16);
        st32(pb + 100, parent);              /* ioDrParID */
    } else {
        st32(pb + 32, e.type);
        st32(pb + 36, e.creator);
        st16(pb + 40, e.finder_flags);
        st32(pb + 42, 0);
        st16(pb + 46, 0);
        /* ioDirID on output for files is the file number; keep the parent's caller value */
        uint32_t dlen = (uint32_t)fs::file_size(e.host, ec);
        uint32_t rlen = fs::exists(rsrc_path(e.host), ec) ? (uint32_t)fs::file_size(rsrc_path(e.host), ec) : 0;
        st16(pb + 52, 0);
        st32(pb + 54, dlen);
        st32(pb + 58, (dlen + 511) & ~511u);
        st16(pb + 62, 0);
        st32(pb + 64, rlen);
        st32(pb + 68, (rlen + 511) & ~511u);
        st32(pb + 72, mac_date(e.host));
        st32(pb + 76, mac_date(e.host));
        st32(pb + 80, 0);
        memset(g_mem + pb + 84, 0, 16);
        st32(pb + 100, parent);              /* ioFlParID */
        st32(pb + 104, 0);
    }
}

HLE(PBGetCatInfoSync) {
    uint32_t pb = ARG(0);
    int16_t vref = (int16_t)ld16(pb + 22);
    int16_t index = (int16_t)ld16(pb + 28);
    uint32_t dirID = ld32(pb + 48);
    int16_t err = noErr;
    if (vref == 0) vref = app_vref();
    if (index > 0) {
        /* enumerate the index'th item of directory dirID */
        if (dirID == 0) dirID = (vref == app_vref()) ? app_dir() : 2;
        auto items = list(vref, dirID);
        if (index > (int)items.size()) err = fnfErr;
        else fill_catinfo(pb, vref, dirID, items[index - 1]);
    } else if (index < 0) {
        /* information about the directory dirID itself */
        if (dirID == 0) dirID = (vref == app_vref()) ? app_dir() : 2;
        fs::path host;
        if (!dir_host(vref, dirID, host)) err = dirNFErr;
        else {
            Entry e;
            e.is_dir = true;
            e.host = host;
            e.mac_name = dir_name(vref, dirID);
            fill_catinfo(pb, vref, dir_parent(vref, dirID), e);
        }
    } else {
        Spec s;
        std::string name = ld32(pb + 18) ? read_pstr(ld32(pb + 18)) : std::string();
        err = resolve(vref, dirID, name, s);
        if (err == noErr) {
            Entry e;
            if (lookup(s.vref, s.parent, s.name, e)) fill_catinfo(pb, s.vref, s.parent, e);
            else { /* the resolved spec is a directory reached by path */
                fs::path host;
                uint32_t id = 0;
                for (auto &it : list(s.vref, s.parent))
                    if (mac_name_equal(it.mac_name, s.name)) id = dir_id(s.vref, it.host);
                if (!id) err = fnfErr;
                else if (dir_host(s.vref, id, host)) {
                    e.is_dir = true; e.host = host; e.mac_name = s.name;
                    fill_catinfo(pb, s.vref, s.parent, e);
                }
            }
        }
    }
    logf(LOG_FILE, "PBGetCatInfo(vref %d, dir %u, index %d) -> %d\n", vref, dirID, index, err);
    put_result(pb, err);
    RET(err);
}

HLE(PBHGetVInfoSync) {
    uint32_t pb = ARG(0);
    int16_t vref = (int16_t)ld16(pb + 22);
    int16_t index = (int16_t)ld16(pb + 28);
    if (index > 0) {
        vref = volume_by_index(index);
        if (!vref) { put_result(pb, nsvErr); RET(nsvErr); return; }
    } else if (index < 0 && ld32(pb + 18)) {
        vref = volume_by_name(read_pstr(ld32(pb + 18)));
    }
    std::string name;
    fs::path root;
    if (!vref) vref = app_vref();
    if (!volume_info(vref, name, root)) { put_result(pb, nsvErr); RET(nsvErr); return; }
    if (ld32(pb + 18)) write_pstr(ld32(pb + 18), name);
    st16(pb + 22, (uint16_t)vref);
    st32(pb + 30, 0);
    st32(pb + 34, 0);
    st16(pb + 38, vref == -1 ? 0x8080 : 0);  /* game volume: locked (it is the CD) */
    st16(pb + 40, 0);
    st16(pb + 46, 0xFFFF);                   /* ioVNmAlBlks */
    st32(pb + 48, 32768);                    /* ioVAlBlkSiz */
    st32(pb + 52, 32768);
    st16(pb + 62, 0x8000);                   /* ioVFrBlk */
    st16(pb + 64, 0x4244);                   /* 'BD' HFS */
    st16(pb + 66, 1);
    st16(pb + 68, (uint16_t)(vref == -1 ? kAppleCDRefNum : -1)); /* game volume sits on the CD driver */
    st16(pb + 70, 0);
    put_result(pb, 0);
    RET(0);
}

HLE(PBGetFCBInfoSync) {
    uint32_t pb = ARG(0);
    int16_t ref = (int16_t)ld16(pb + 24);
    if (!files::is_open(ref)) { put_result(pb, rfNumErr); RET(rfNumErr); return; }
    fs::path host = files::host_of(ref);
    if (ld32(pb + 18)) write_pstr(ld32(pb + 18), utf8_to_mac(host.filename().u8string()));
    uint32_t eof = 0, pos = 0;
    files::get_eof(ref, eof);
    files::get_pos(ref, pos);
    st16(pb + 36, 0);
    st32(pb + 40, eof);
    st32(pb + 44, eof);
    st32(pb + 48, pos);
    st16(pb + 52, (uint16_t)app_vref());
    st32(pb + 58, dir_id(app_vref(), host.parent_path()));
    put_result(pb, 0);
    RET(0);
}

/* ======================= Folder Manager ======================= */
HLE(FindFolder) {
    /* OSErr FindFolder(short vRefNum, OSType folderType, Boolean createFolder, short *foundVRefNum, long *foundDirID) */
    uint32_t type = ARG(1);
    fs::path root;
    std::string vname;
    volume_info(-2, vname, root);
    fs::path p;
    switch (type) {
    case 'pref': p = root / "System Folder" / "Preferences"; break;
    case 'macs': p = root / "System Folder"; break;
    case 'temp': p = root / "Temporary Items"; break;
    case 'trsh': p = root / "Trash"; break;
    case 'desk': p = root / "Desktop Folder"; break;
    case 'extn': p = root / "System Folder" / "Extensions"; break;
    case 'ctrl': p = root / "System Folder" / "Control Panels"; break;
    default:
        logf(LOG_UNIMPL, "FindFolder type %08X unsupported\n", type);
        RET(-43);
        return;
    }
    std::error_code ec;
    fs::create_directories(p, ec);
    st16(ARG(3), (uint16_t)-2);
    st32(ARG(4), dir_id(-2, p));
    RET(0);
}

/* ======================= Process Manager ======================= */
HLE(GetCurrentProcess) {
    st32(ARG(0), 0);
    st32(ARG(0) + 4, 2); /* kCurrentProcess-ish PSN */
    RET(0);
}

HLE(GetProcessInformation) {
    uint32_t info = ARG(1);
    if (ld32(info + 4)) write_pstr(ld32(info + 4), app_name());
    st32(info + 8, 0);
    st32(info + 12, 2);
    st32(info + 16, 'APPL');
    st32(info + 20, 'PTar');
    st32(info + 24, 0x5800);
    st32(info + 28, PT_HEAP_BASE);
    st32(info + 32, PT_HEAP_END - PT_HEAP_BASE);
    st32(info + 36, guest_heap_free_bytes());
    st32(info + 40, 0);
    st32(info + 44, 1);
    st32(info + 48, 0);
    st32(info + 52, ticks());
    if (ld32(info + 56)) {
        Spec s;
        s.vref = app_vref();
        s.parent = app_dir();
        s.name = app_name();
        write_fsspec(ld32(info + 56), s);
    }
    RET(0);
}
