/* Resource Manager: resource forks are parsed into memory; resources become
 * guest handles on demand and are cached so repeated GetResource calls return
 * the same handle, as on a real Mac. */
#include "pt/resources.h"

#include <cstdio>
#include <map>

#include "pt/files.h"
#include "pt/runtime.h"
#include "pt/vfs.h"

namespace fs = std::filesystem;
namespace pt::dlg { void patch_ditl(int16_t id, std::vector<uint8_t> &ditl); }
using namespace pt;
using namespace pt::vfs;

namespace pt::res {

struct Res {
    uint32_t type;
    int16_t id;
    std::string name;
    uint8_t attr;
    std::vector<uint8_t> data;
    uint32_t handle = 0;
};
struct ResFile {
    int16_t ref;
    fs::path host;          /* data-fork path; the fork itself is host + ".rsrc" */
    std::vector<Res> res;
    bool dirty = false;
    bool writable = false;
};

static std::vector<ResFile> g_chain;   /* front = most recently opened */
static int16_t g_cur = 0;
static int16_t g_err = 0;
static bool g_load = true;

static void set_err(int16_t e) { g_err = e; st16(0x0A60, (uint16_t)e); }

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

static bool parse_fork(const std::vector<uint8_t> &f, std::vector<Res> &out) {
    if (f.size() < 16) return f.empty();
    uint32_t doff = be32(&f[0]), moff = be32(&f[4]);
    if (moff + 30 > f.size() || doff > f.size()) return false;
    const uint8_t *map = &f[moff];
    uint16_t tl = be16(map + 24), nl = be16(map + 26);
    const uint8_t *types = map + tl;
    int ntypes = (int16_t)be16(types) + 1;
    for (int t = 0; t < ntypes; t++) {
        const uint8_t *te = types + 2 + t * 8;
        uint32_t type = be32(te);
        int n = be16(te + 4) + 1;
        const uint8_t *refs = types + be16(te + 6);
        for (int i = 0; i < n; i++) {
            const uint8_t *r = refs + i * 12;
            Res res;
            res.type = type;
            res.id = (int16_t)be16(r);
            uint16_t no = be16(r + 2);
            res.attr = r[4];
            uint32_t dataoff = ((uint32_t)r[5] << 16) | (r[6] << 8) | r[7];
            if (no != 0xFFFF) {
                const uint8_t *nm = map + nl + no;
                res.name.assign((const char *)nm + 1, nm[0]);
            }
            const uint8_t *d = &f[doff + dataoff];
            uint32_t len = be32(d);
            res.data.assign(d + 4, d + 4 + len);
            out.push_back(std::move(res));
        }
    }
    return true;
}

static std::vector<uint8_t> build_fork(const std::vector<Res> &rs) {
    std::vector<uint8_t> data, names, out;
    std::map<uint32_t, std::vector<const Res *>> by_type;
    for (auto &r : rs) by_type[r.type].push_back(&r);
    auto put32 = [](std::vector<uint8_t> &v, uint32_t x) { for (int i = 3; i >= 0; i--) v.push_back((uint8_t)(x >> (8 * i))); };
    auto put16 = [](std::vector<uint8_t> &v, uint32_t x) { v.push_back((uint8_t)(x >> 8)); v.push_back((uint8_t)x); };
    std::vector<uint8_t> typelist, reflist;
    put16(typelist, (uint32_t)by_type.size() - 1);
    uint32_t reflist_base = 2 + 8 * (uint32_t)by_type.size();
    for (auto &[type, list] : by_type) {
        put32(typelist, type);
        put16(typelist, (uint32_t)list.size() - 1);
        put16(typelist, reflist_base + (uint32_t)reflist.size());
        for (auto *r : list) {
            put16(reflist, (uint16_t)r->id);
            if (r->name.empty()) put16(reflist, 0xFFFF);
            else {
                put16(reflist, (uint32_t)names.size());
                names.push_back((uint8_t)r->name.size());
                names.insert(names.end(), r->name.begin(), r->name.end());
            }
            uint32_t off = (uint32_t)data.size();
            reflist.push_back(r->attr);
            reflist.push_back((uint8_t)(off >> 16)); reflist.push_back((uint8_t)(off >> 8)); reflist.push_back((uint8_t)off);
            put32(reflist, 0);
            put32(data, (uint32_t)r->data.size());
            data.insert(data.end(), r->data.begin(), r->data.end());
        }
    }
    std::vector<uint8_t> map(28, 0);
    uint32_t tl_off = 28, nl_off = 28 + (uint32_t)(typelist.size() + reflist.size());
    map[24] = (uint8_t)(tl_off >> 8); map[25] = (uint8_t)tl_off;
    map[26] = (uint8_t)(nl_off >> 8); map[27] = (uint8_t)nl_off;
    map.insert(map.end(), typelist.begin(), typelist.end());
    map.insert(map.end(), reflist.begin(), reflist.end());
    map.insert(map.end(), names.begin(), names.end());
    uint32_t doff = 256, moff = doff + (uint32_t)data.size();
    out.resize(256, 0);
    std::vector<uint8_t> hdr;
    put32(hdr, doff); put32(hdr, moff); put32(hdr, (uint32_t)data.size()); put32(hdr, (uint32_t)map.size());
    memcpy(out.data(), hdr.data(), 16);
    memcpy(map.data(), hdr.data(), 16);
    out.insert(out.end(), data.begin(), data.end());
    out.insert(out.end(), map.begin(), map.end());
    return out;
}

static ResFile *file_by_ref(int16_t ref) {
    for (auto &f : g_chain)
        if (f.ref == ref) return &f;
    return nullptr;
}

static int16_t open_res_file(const fs::path &host, bool writable) {
    for (auto &f : g_chain)
        if (f.host == host) { g_cur = f.ref; return f.ref; }
    std::vector<uint8_t> bytes;
    fs::path rp = rsrc_path(host);
    if (FILE *fp = _wfopen(rp.c_str(), L"rb")) {
        fseek(fp, 0, SEEK_END);
        bytes.resize((size_t)ftell(fp));
        fseek(fp, 0, SEEK_SET);
        if (!bytes.empty()) fread(bytes.data(), 1, bytes.size(), fp);
        fclose(fp);
    } else {
        return -39; /* eofErr: no resource fork */
    }
    ResFile rf;
    rf.ref = files::alloc_refnum();
    rf.host = host;
    rf.writable = writable;
    if (!parse_fork(bytes, rf.res)) return -199; /* mapReadErr */
    for (auto &x : rf.res)   /* the port's own setting in the Preferences dialog (dialogs.cpp) */
        if (x.type == 'DITL') dlg::patch_ditl(x.id, x.data);
    logf(LOG_RES, "res: opened %s (%zu resources) as %d\n", host.u8string().c_str(), rf.res.size(), rf.ref);
    g_chain.insert(g_chain.begin(), std::move(rf));
    g_cur = g_chain.front().ref;
    return g_cur;
}

static void write_back(ResFile &f) {
    if (!f.dirty) return;
    auto bytes = build_fork(f.res);
    fs::path rp = rsrc_path(f.host);
    if (FILE *fp = _wfopen(rp.c_str(), L"wb")) {
        fwrite(bytes.data(), 1, bytes.size(), fp);
        fclose(fp);
    }
    f.dirty = false;
}

namespace hfs_glue {
std::vector<std::pair<std::string, std::vector<uint8_t>>> load(const std::wstring &image);
}

/* system/ (written by tools/extract_system.py): files.tsv lists "relative path <TAB> Mac name hex" */
static std::vector<std::pair<std::string, std::vector<uint8_t>>> load_system_dir(const fs::path &dir) {
    std::vector<std::pair<std::string, std::vector<uint8_t>>> out;
    FILE *list = _wfopen((dir / "files.tsv").c_str(), L"rb");
    if (!list) return out;
    char line[1024];
    while (fgets(line, sizeof line, list)) {
        std::string s(line);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        size_t tab = s.find('\t');
        if (tab == std::string::npos) continue;
        std::string name;
        for (size_t i = tab + 1; i + 1 < s.size(); i += 2) name += (char)std::stoi(s.substr(i, 2), nullptr, 16);
        std::vector<uint8_t> bytes;
        if (FILE *fp = _wfopen((dir / fs::u8path(s.substr(0, tab))).c_str(), L"rb")) {
            fseek(fp, 0, SEEK_END);
            bytes.resize((size_t)ftell(fp));
            fseek(fp, 0, SEEK_SET);
            if (!bytes.empty()) fread(bytes.data(), 1, bytes.size(), fp);
            fclose(fp);
        }
        if (!bytes.empty()) out.emplace_back(std::move(name), std::move(bytes));
    }
    fclose(list);
    return out;
}

static void load_system_files() {
    const char *dir_env = getenv("PT_SYSTEM_DIR");
    fs::path dir = dir_env && *dir_env ? fs::u8path(dir_env) : fs::u8path(data_root()).parent_path() / "system";
    auto forks = load_system_dir(dir);
    fs::path src = dir;
    if (forks.empty()) {
        /* else read a Mac OS disk image directly: PT_SYSTEM_HFV, any .hfv/.dsk beside the
         * program (SheepShaver/Basilisk II disks), or the development layout's macos7.hfv */
        std::vector<fs::path> images;
        if (const char *env = getenv("PT_SYSTEM_HFV"); env && *env) images.push_back(fs::u8path(env));
        fs::path home = fs::u8path(data_root()).parent_path();
        std::error_code ec;
        for (auto &e : fs::directory_iterator(home, ec)) {
            std::wstring x = e.path().extension().wstring();
            for (auto &ch : x) ch = (wchar_t)towlower(ch);
            if ((x == L".hfv" || x == L".dsk") && e.is_regular_file(ec)) images.push_back(e.path());
        }
        images.push_back(home.parent_path() / "PrimeTarget" / "Prime" / "macos7.hfv");
        for (auto &img : images) {
            forks = hfs_glue::load(img.wstring());
            if (!forks.empty()) { src = img; break; }
        }
    }
    if (forks.empty()) {
        logf(LOG_UNIMPL, "res: no Mac OS System file found; using built-in substitute fonts and cursor\n");
        return;
    }
    logf(LOG_RES, "res: system resources from %s\n", src.u8string().c_str());
    for (auto &[name, bytes] : forks) {
        ResFile rf;
        rf.ref = name == "System" ? 2 : files::alloc_refnum();
        rf.host = fs::path(L"<system>") / fs::u8path(name);
        if (!parse_fork(bytes, rf.res)) continue;
        logf(LOG_RES, "res: system file %s: %zu resources (ref %d)\n", name.c_str(), rf.res.size(), rf.ref);
        g_chain.push_back(std::move(rf));
    }
    st16(0x0A58, 2);   /* SysMap */
}

void init() {
    load_system_files();
    fs::path app;
    dir_host(app_vref(), app_dir(), app);
    Entry e;
    if (!lookup(app_vref(), app_dir(), app_name(), e)) fatal("application %s not found", mac_to_utf8(app_name()).c_str());
    int16_t ref = open_res_file(e.host, false);
    if (ref < 0) fatal("cannot open application resource fork (%d)", ref);
    st16(0x0900, (uint16_t)ref);   /* CurApRefNum */
    st16(0x0A5A, (uint16_t)ref);   /* CurMap */
    write_pstr(0x0910, app_name().substr(0, 31));   /* CurApName */
}

int16_t current_file() { return g_cur; }

/* search from the current file toward older ones */
static Res *find(uint32_t type, int16_t id, bool one_deep, ResFile **owner = nullptr) {
    bool started = false;
    for (auto &f : g_chain) {
        if (f.ref == g_cur) started = true;
        if (!started) continue;
        for (auto &r : f.res)
            if (r.type == type && r.id == id) {
                if (owner) *owner = &f;
                return &r;
            }
        if (one_deep) break;
    }
    return nullptr;
}

static uint32_t materialize(Res &r) {
    if (r.handle && ld32(r.handle)) return r.handle;
    if (!g_load) {
        r.handle = handle_from_block(0); /* empty handle, loaded later by LoadResource */
        handle_set_state(r.handle, 0x20);
        return r.handle;
    }
    r.handle = handle_new((uint32_t)r.data.size(), false);
    if (!r.handle) return 0;
    memcpy(g_mem + ld32(r.handle), r.data.data(), r.data.size());
    handle_set_state(r.handle, 0x20);
    return r.handle;
}

bool get_bytes(uint32_t type, int16_t id, std::vector<uint8_t> &out) {
    Res *r = find(type, id, false);
    if (!r) {
        /* fall back to every open file (e.g. app resources when another file is current) */
        for (auto &f : g_chain)
            for (auto &x : f.res)
                if (x.type == type && x.id == id) { out = x.data; return true; }
        return false;
    }
    out = r->data;
    return true;
}

std::vector<std::vector<uint8_t>> get_bytes_all(uint32_t type, int16_t id) {
    std::vector<std::vector<uint8_t>> out;
    for (auto &f : g_chain)
        for (auto &r : f.res)
            if (r.type == type && r.id == id) out.push_back(r.data);
    return out;
}

uint32_t get_handle(uint32_t type, int16_t id) {
    Res *r = find(type, id, false);
    return r ? materialize(*r) : 0;
}

uint32_t get_handle_by_name(uint32_t type, const std::string &name) {
    for (auto &f : g_chain)
        for (auto &r : f.res)
            if (r.type == type && mac_name_equal(r.name, name)) return materialize(r);
    return 0;
}

std::vector<int16_t> ids_of_type(uint32_t type) {
    std::vector<int16_t> ids;
    for (auto &f : g_chain)
        for (auto &r : f.res)
            if (r.type == type) ids.push_back(r.id);
    return ids;
}

static std::string fourcc(uint32_t t) {
    return {(char)(t >> 24), (char)(t >> 16), (char)(t >> 8), (char)t};
}

static void release(uint32_t h) {
    for (auto &f : g_chain)
        for (auto &r : f.res)
            if (r.handle == h) {
                handle_dispose(h);
                r.handle = 0;
                return;
            }
}

}  // namespace pt::res

using namespace pt::res;

HLE(GetResource) {
    Res *r = find(ARG(0), (int16_t)ARG(1), false);
    uint32_t h = r ? materialize(*r) : 0;
    set_err(h ? 0 : -192);
    logf(LOG_RES, "GetResource('%s', %d) -> %08X\n", fourcc(ARG(0)).c_str(), (int16_t)ARG(1), h);
    RET(h);
}
HLE(Get1Resource) {
    Res *r = find(ARG(0), (int16_t)ARG(1), true);
    uint32_t h = r ? materialize(*r) : 0;
    set_err(h ? 0 : -192);
    logf(LOG_RES, "Get1Resource('%s', %d) -> %08X\n", fourcc(ARG(0)).c_str(), (int16_t)ARG(1), h);
    RET(h);
}
HLE(ReleaseResource) { release(ARG(0)); set_err(0); }
HLE(ResError) { RET((int16_t)g_err); }
HLE(UseResFile) {
    if (file_by_ref((int16_t)ARG(0)) || (int16_t)ARG(0) == 0) {
        g_cur = (int16_t)ARG(0) == 0 ? 2 : (int16_t)ARG(0);
        set_err(0);
    } else {
        set_err(-193); /* resFNotFound */
    }
}
HLE(CurResFile) { RET((int16_t)g_cur); }
HLE(SetResLoad) { g_load = (ARG(0) & 0xFF) != 0; }
HLE(CloseResFile) {
    int16_t ref = (int16_t)ARG(0);
    for (auto it = g_chain.begin(); it != g_chain.end(); ++it)
        if (it->ref == ref) {
            write_back(*it);
            for (auto &r : it->res)
                if (r.handle) handle_dispose(r.handle);
            g_chain.erase(it);
            if (g_cur == ref) g_cur = g_chain.empty() ? 0 : g_chain.front().ref;
            set_err(0);
            return;
        }
    set_err(-193);
}

HLE(FSpOpenResFile) {
    /* short FSpOpenResFile(const FSSpec *spec, SignedByte permission) */
    Spec s = read_fsspec(ARG(0));
    Entry e;
    if (!lookup(s.vref, s.parent, s.name, e)) { set_err(-43); RET((uint16_t)-1); return; }
    int perm = (int8_t)ARG(1);
    int16_t ref = open_res_file(e.host, perm != 1);
    if (ref < 0) { set_err(ref); RET((uint16_t)-1); return; }
    set_err(0);
    RET((uint16_t)ref);
}

HLE(FSpCreateResFile) {
    /* void FSpCreateResFile(const FSSpec *spec, OSType creator, OSType fileType, ScriptCode scriptTag) */
    Spec s = read_fsspec(ARG(0));
    Entry e;
    fs::path host;
    if (lookup(s.vref, s.parent, s.name, e)) host = e.host;
    else {
        host = host_path_for_new(s);
        if (FILE *f = _wfopen(host.c_str(), L"wb")) fclose(f);
        set_finder_info(s, ARG(2), ARG(1), 0);
    }
    fs::path rp = rsrc_path(host);
    std::error_code ec;
    if (!fs::exists(rp, ec) || fs::file_size(rp, ec) == 0) {
        auto bytes = build_fork({});
        if (FILE *f = _wfopen(rp.c_str(), L"wb")) { fwrite(bytes.data(), 1, bytes.size(), f); fclose(f); }
    }
    set_err(0);
}

HLE(AddResource) {
    /* void AddResource(Handle theData, ResType theType, short theID, ConstStr255Param name) */
    ResFile *f = file_by_ref(g_cur);
    if (!f) { set_err(-193); return; }
    Res r;
    r.type = ARG(1);
    r.id = (int16_t)ARG(2);
    r.name = ARG(3) ? read_pstr(ARG(3)) : std::string();
    r.attr = 0;
    uint32_t h = ARG(0);
    uint32_t sz = handle_size(h);
    r.data.assign(g_mem + ld32(h), g_mem + ld32(h) + sz);
    r.handle = h;
    handle_set_state(h, handle_get_state(h) | 0x20);
    for (auto it = f->res.begin(); it != f->res.end(); ++it)
        if (it->type == r.type && it->id == r.id) { f->res.erase(it); break; }
    f->res.push_back(std::move(r));
    f->dirty = true;
    set_err(0);
}

HLE(GetIndString) {
    /* void GetIndString(Str255 theString, short strListID, short index) */
    std::vector<uint8_t> d;
    std::string out;
    int16_t id = (int16_t)ARG(1), index = (int16_t)ARG(2);
    if (get_bytes('STR#', id, d) && d.size() >= 2) {
        int n = (d[0] << 8) | d[1];
        size_t p = 2;
        for (int i = 1; i <= n && p < d.size(); i++) {
            uint8_t len = d[p];
            if (i == index) { out.assign((const char *)&d[p + 1], len); break; }
            p += 1 + len;
        }
    }
    write_pstr(ARG(0), out);
    logf(LOG_RES, "GetIndString(%d, %d) = \"%s\"\n", id, index, mac_to_utf8(out).c_str());
}

HLE(GetString) { RET(get_handle('STR ', (int16_t)ARG(0))); }
HLE(GetPicture) { RET(get_handle('PICT', (int16_t)ARG(0))); }
HLE(GetIcon) { RET(get_handle('ICON', (int16_t)ARG(0))); }

HLE(GetCTable) {
    /* returns a fresh copy (not a resource) of the 'clut' */
    std::vector<uint8_t> d;
    if (!get_bytes('clut', (int16_t)ARG(0), d)) { RET(0); return; }
    uint32_t h = handle_new((uint32_t)d.size(), false);
    memcpy(g_mem + ld32(h), d.data(), d.size());
    RET(h);
}
