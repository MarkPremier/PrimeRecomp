/* Virtual HFS over host folders.
 *
 * Volume -1 "Prime Target" is the extracted game volume (it doubles as the CD,
 * whose volume name the game also expects). Volume -2 "Macintosh HD" is a
 * writable per-user folder holding the System Folder (preferences).
 *
 * Every host folder written by tools/extract_game.py has finder.tsv (files)
 * and folders.tsv (subfolders) that record the original Mac Roman names and
 * Finder type/creator; folders/files without an entry use their host name.
 */
#include "pt/vfs.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <sstream>

#include "image_info.h"
#include "pt/runtime.h"

namespace pt::vfs {

/* ---------- Mac Roman <-> UTF-8 ---------- */
static const uint16_t kMacRomanHigh[128] = {
    0x00C4, 0x00C5, 0x00C7, 0x00C9, 0x00D1, 0x00D6, 0x00DC, 0x00E1, 0x00E0, 0x00E2, 0x00E4, 0x00E3, 0x00E5, 0x00E7, 0x00E9, 0x00E8,
    0x00EA, 0x00EB, 0x00ED, 0x00EC, 0x00EE, 0x00EF, 0x00F1, 0x00F3, 0x00F2, 0x00F4, 0x00F6, 0x00F5, 0x00FA, 0x00F9, 0x00FB, 0x00FC,
    0x2020, 0x00B0, 0x00A2, 0x00A3, 0x00A7, 0x2022, 0x00B6, 0x00DF, 0x00AE, 0x00A9, 0x2122, 0x00B4, 0x00A8, 0x2260, 0x00C6, 0x00D8,
    0x221E, 0x00B1, 0x2264, 0x2265, 0x00A5, 0x00B5, 0x2202, 0x2211, 0x220F, 0x03C0, 0x222B, 0x00AA, 0x00BA, 0x03A9, 0x00E6, 0x00F8,
    0x00BF, 0x00A1, 0x00AC, 0x221A, 0x0192, 0x2248, 0x2206, 0x00AB, 0x00BB, 0x2026, 0x00A0, 0x00C0, 0x00C3, 0x00D5, 0x0152, 0x0153,
    0x2013, 0x2014, 0x201C, 0x201D, 0x2018, 0x2019, 0x00F7, 0x25CA, 0x00FF, 0x0178, 0x2044, 0x20AC, 0x2039, 0x203A, 0xFB01, 0xFB02,
    0x2021, 0x00B7, 0x201A, 0x201E, 0x2030, 0x00C2, 0x00CA, 0x00C1, 0x00CB, 0x00C8, 0x00CD, 0x00CE, 0x00CF, 0x00CC, 0x00D3, 0x00D4,
    0xF8FF, 0x00D2, 0x00DA, 0x00DB, 0x00D9, 0x0131, 0x02C6, 0x02DC, 0x00AF, 0x02D8, 0x02D9, 0x02DA, 0x00B8, 0x02DD, 0x02DB, 0x02C7,
};

std::string mac_to_utf8(const std::string &mac) {
    std::string out;
    for (unsigned char ch : mac) {
        uint32_t u = ch < 0x80 ? ch : kMacRomanHigh[ch - 0x80];
        if (u < 0x80) out += (char)u;
        else if (u < 0x800) { out += (char)(0xC0 | (u >> 6)); out += (char)(0x80 | (u & 0x3F)); }
        else { out += (char)(0xE0 | (u >> 12)); out += (char)(0x80 | ((u >> 6) & 0x3F)); out += (char)(0x80 | (u & 0x3F)); }
    }
    return out;
}

std::string utf8_to_mac(const std::string &s) {
    std::string out;
    for (size_t i = 0; i < s.size();) {
        uint32_t u = (unsigned char)s[i];
        int n = u < 0x80 ? 1 : u < 0xE0 ? 2 : u < 0xF0 ? 3 : 4;
        if (n == 2) u = ((u & 0x1F) << 6) | (s[i + 1] & 0x3F);
        else if (n == 3) u = ((u & 0x0F) << 12) | ((s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3F);
        else if (n == 4) u = '?';
        i += n;
        if (u < 0x80) { out += (char)u; continue; }
        char m = '?';
        for (int k = 0; k < 128; k++)
            if (kMacRomanHigh[k] == u) { m = (char)(0x80 + k); break; }
        out += m;
    }
    return out;
}

static int mac_upper(int c) {
    if (c >= 'a' && c <= 'z') return c - 32;
    return c; /* good enough for the names this game uses */
}

bool mac_name_equal(const std::string &a, const std::string &b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++)
        if (mac_upper((unsigned char)a[i]) != mac_upper((unsigned char)b[i])) return false;
    return true;
}

static std::string from_hex(const std::string &h) {
    std::string out;
    for (size_t i = 0; i + 1 < h.size(); i += 2) out += (char)std::stoi(h.substr(i, 2), nullptr, 16);
    return out;
}
static std::string to_hex(const std::string &s) {
    static const char *d = "0123456789abcdef";
    std::string out;
    for (unsigned char ch : s) { out += d[ch >> 4]; out += d[ch & 15]; }
    return out;
}
static uint32_t four_cc(const std::string &s) {
    uint32_t v = 0;
    for (size_t i = 0; i < 4; i++) v = (v << 8) | (i < s.size() ? (unsigned char)s[i] : ' ');
    return v;
}

/* ---------- per-folder metadata ---------- */
struct FileMeta { std::string mac; uint32_t type, creator; uint16_t flags; };
struct FolderMeta {
    std::map<std::string, FileMeta> files;      /* host name -> meta */
    std::map<std::string, std::string> folders; /* host name -> mac name */
};
static std::map<fs::path, FolderMeta> g_meta;

static FolderMeta &meta_for(const fs::path &dir) {
    auto it = g_meta.find(dir);
    if (it != g_meta.end()) return it->second;
    FolderMeta m;
    std::ifstream f(dir / "finder.tsv");
    std::string line;
    while (std::getline(f, line)) {
        std::istringstream ss(line);
        std::string host, mac, type, creator, flags;
        std::getline(ss, host, '\t'); std::getline(ss, mac, '\t');
        std::getline(ss, type, '\t'); std::getline(ss, creator, '\t'); std::getline(ss, flags, '\t');
        if (host.empty()) continue;
        m.files[host] = FileMeta{from_hex(mac), four_cc(from_hex(type)), four_cc(from_hex(creator)),
                                 (uint16_t)(flags.empty() ? 0 : std::stoul(flags, nullptr, 16))};
    }
    std::ifstream g(dir / "folders.tsv");
    while (std::getline(g, line)) {
        auto tab = line.find('\t');
        if (tab != std::string::npos) m.folders[line.substr(0, tab)] = from_hex(line.substr(tab + 1));
    }
    return g_meta[dir] = m;
}

static void save_meta(const fs::path &dir) {
    FolderMeta &m = meta_for(dir);
    std::ofstream f(dir / "finder.tsv", std::ios::binary);
    char buf[16];
    for (auto &[host, fm] : m.files) {
        std::string t = {(char)(fm.type >> 24), (char)(fm.type >> 16), (char)(fm.type >> 8), (char)fm.type};
        std::string cr = {(char)(fm.creator >> 24), (char)(fm.creator >> 16), (char)(fm.creator >> 8), (char)fm.creator};
        snprintf(buf, sizeof buf, "%04x", fm.flags);
        f << host << '\t' << to_hex(fm.mac) << '\t' << to_hex(t) << '\t' << to_hex(cr) << '\t' << buf << '\n';
    }
}

static bool is_meta_file(const std::string &n) {
    return n == "finder.tsv" || n == "finder.json" || n == "folders.tsv" ||
           (n.size() > 5 && n.compare(n.size() - 5, 5, ".rsrc") == 0);
}

/* ---------- volumes and directory IDs ---------- */
struct Volume {
    int16_t vref;
    std::string name;  /* Mac Roman */
    fs::path root;
    std::map<uint32_t, fs::path> id_to_path;
    std::map<fs::path, uint32_t> path_to_id;
    uint32_t next_id = 16;
};
static std::vector<Volume> g_vols;
static int16_t g_app_vref;
static uint32_t g_app_dir;
static std::string g_app_name;

/* working-directory refnums (old Standard File / SFReply): stand for (volume, dirID) */
struct WD { int16_t vref; uint32_t dir; };
static std::vector<WD> g_wds;
static const int16_t kWDBase = -32000;

int16_t make_wd(int16_t vref, uint32_t dir) {
    for (size_t i = 0; i < g_wds.size(); i++)
        if (g_wds[i].vref == vref && g_wds[i].dir == dir) return (int16_t)(kWDBase - (int)i);
    g_wds.push_back({vref, dir});
    return (int16_t)(kWDBase - (int)(g_wds.size() - 1));
}

static void resolve_wd(int16_t &vref, uint32_t &dir) {
    if (vref > kWDBase) return;
    size_t i = (size_t)(kWDBase - vref);
    if (i >= g_wds.size()) return;
    if (dir == 0) dir = g_wds[i].dir;
    vref = g_wds[i].vref;
}

static Volume *vol(int16_t vref) {
    uint32_t unused = 1;
    resolve_wd(vref, unused);
    if (vref == 0) vref = g_app_vref;
    for (auto &v : g_vols)
        if (v.vref == vref) return &v;
    return nullptr;
}

static void add_volume(int16_t vref, const std::string &mac_name, const fs::path &root) {
    Volume v;
    v.vref = vref;
    v.name = mac_name;
    v.root = fs::absolute(root).lexically_normal();
    v.id_to_path[2] = v.root;
    v.path_to_id[v.root] = 2;
    g_vols.push_back(v);
}

void init() {
    fs::path game = fs::u8path(data_root());
    const char *ud = getenv("PT_USER_DIR");   /* the writable "Macintosh HD" (tests use their own) */
    fs::path user = (ud && *ud ? fs::u8path(ud) : game.parent_path() / "user") / "Macintosh HD";
    fs::create_directories(user / "System Folder" / "Preferences");
    add_volume(-1, "Prime Target", game);
    add_volume(-2, "Macintosh HD", user);
    g_app_vref = -1;
    g_app_dir = dir_id(-1, game / "Prime Target");
    const char *app = getenv("PT_APP_NAME");
    g_app_name = app && *app ? utf8_to_mac(app) : utf8_to_mac(PT_IMAGE_APP_NAME);
    logf(LOG_FILE, "vfs: game volume %s, user volume %s\n", game.u8string().c_str(), user.u8string().c_str());
}

int16_t app_vref() { return g_app_vref; }
uint32_t app_dir() { return g_app_dir; }
std::string app_name() { return g_app_name; }

bool volume_info(int16_t vref, std::string &name, fs::path &root) {
    Volume *v = vol(vref);
    if (!v) return false;
    name = v->name;
    root = v->root;
    return true;
}
int16_t volume_by_index(int index) { return index >= 1 && index <= (int)g_vols.size() ? g_vols[index - 1].vref : 0; }
int16_t volume_by_name(const std::string &n) {
    for (auto &v : g_vols)
        if (mac_name_equal(v.name, n)) return v.vref;
    return 0;
}

bool dir_host(int16_t vref, uint32_t dirID, fs::path &out) {
    resolve_wd(vref, dirID);
    Volume *v = vol(vref);
    if (!v) return false;
    if (dirID == 0) dirID = (vref == 0 || vref == g_app_vref) ? g_app_dir : 2;
    auto it = v->id_to_path.find(dirID);
    if (it == v->id_to_path.end()) return false;
    out = it->second;
    return true;
}

uint32_t dir_id(int16_t vref, const fs::path &host) {
    Volume *v = vol(vref);
    if (!v) return 0;
    fs::path p = fs::absolute(host).lexically_normal();
    auto it = v->path_to_id.find(p);
    if (it != v->path_to_id.end()) return it->second;
    uint32_t id = v->next_id++;
    v->path_to_id[p] = id;
    v->id_to_path[id] = p;
    return id;
}

uint32_t dir_parent(int16_t vref, uint32_t dirID) {
    Volume *v = vol(vref);
    fs::path p;
    if (!v || !dir_host(vref, dirID, p)) return 0;
    if (p == v->root) return 1;
    return dir_id(vref, p.parent_path());
}

static std::string mac_name_of_dir(const fs::path &p) {
    FolderMeta &m = meta_for(p.parent_path());
    std::string host = p.filename().u8string();
    auto it = m.folders.find(host);
    return it != m.folders.end() ? it->second : utf8_to_mac(host);
}

std::string dir_name(int16_t vref, uint32_t dirID) {
    Volume *v = vol(vref);
    fs::path p;
    if (!v || !dir_host(vref, dirID, p)) return {};
    if (p == v->root) return v->name;
    return mac_name_of_dir(p);
}

std::vector<Entry> list(int16_t vref, uint32_t dirID) {
    std::vector<Entry> out;
    fs::path dir;
    if (!dir_host(vref, dirID, dir)) return out;
    FolderMeta &m = meta_for(dir);
    std::error_code ec;
    for (auto &de : fs::directory_iterator(dir, ec)) {
        std::string host = de.path().filename().u8string();
        if (is_meta_file(host)) continue;
        Entry e;
        e.host = de.path();
        e.is_dir = de.is_directory();
        if (e.is_dir) {
            auto it = m.folders.find(host);
            e.mac_name = it != m.folders.end() ? it->second : utf8_to_mac(host);
        } else {
            auto it = m.files.find(host);
            if (it != m.files.end()) {
                e.mac_name = it->second.mac;
                e.type = it->second.type;
                e.creator = it->second.creator;
                e.finder_flags = it->second.flags;
            } else {
                e.mac_name = utf8_to_mac(host);
                e.type = '????';
                e.creator = '????';
            }
        }
        out.push_back(e);
    }
    std::sort(out.begin(), out.end(), [](const Entry &a, const Entry &b) {
        std::string x = a.mac_name, y = b.mac_name;
        for (auto &ch : x) ch = (char)mac_upper((unsigned char)ch);
        for (auto &ch : y) ch = (char)mac_upper((unsigned char)ch);
        return x < y;
    });
    return out;
}

bool lookup(int16_t vref, uint32_t dirID, const std::string &mac_name, Entry &out) {
    for (auto &e : list(vref, dirID))
        if (mac_name_equal(e.mac_name, mac_name)) {
            out = e;
            return true;
        }
    return false;
}

int16_t resolve(int16_t vref, uint32_t dirID, const std::string &path, Spec &out) {
    resolve_wd(vref, dirID);
    if (vref == 0) vref = g_app_vref;
    if (dirID == 0) dirID = (vref == g_app_vref) ? g_app_dir : 2;
    std::string p = path;
    /* full pathname "Volume:..." (contains a colon that is not leading) */
    size_t colon = p.find(':');
    if (colon != std::string::npos && colon > 0) {
        int16_t v = volume_by_name(p.substr(0, colon));
        if (!v) return nsvErr;
        vref = v;
        dirID = 2;
        p = p.substr(colon + 1);
        if (p.empty()) { /* the volume itself */
            out.vref = vref; out.parent = 1; out.name = vol(vref)->name;
            return noErr;
        }
    } else if (!p.empty() && p[0] == ':') {
        p = p.substr(1);
    }
    if (!vol(vref)) return nsvErr;
    fs::path tmp;
    if (!dir_host(vref, dirID, tmp)) return dirNFErr;
    /* walk components; "::" means parent */
    std::vector<std::string> parts;
    std::string cur;
    for (size_t i = 0; i <= p.size(); i++) {
        if (i == p.size() || p[i] == ':') { parts.push_back(cur); cur.clear(); }
        else cur += p[i];
    }
    if (!parts.empty() && parts.back().empty() && parts.size() > 1) parts.pop_back();
    for (size_t i = 0; i + 1 < parts.size(); i++) {
        if (parts[i].empty()) { dirID = dir_parent(vref, dirID); continue; }
        Entry e;
        if (!lookup(vref, dirID, parts[i], e) || !e.is_dir) return dirNFErr;
        dirID = dir_id(vref, e.host);
    }
    out.vref = vref;
    out.parent = dirID;
    out.name = parts.empty() ? std::string() : parts.back();
    if (out.name.empty()) { /* the directory itself */
        out.name = dir_name(vref, dirID);
        out.parent = dir_parent(vref, dirID);
        return noErr;
    }
    Entry e;
    return lookup(vref, dirID, out.name, e) ? noErr : fnfErr;
}

fs::path host_path_for_new(const Spec &spec) {
    fs::path dir;
    dir_host(spec.vref, spec.parent, dir);
    std::string host = mac_to_utf8(spec.name);
    for (auto &ch : host)
        if (strchr("<>:\"/\\|?*", ch) || (unsigned char)ch < 32) ch = '_';
    return dir / fs::u8path(host);
}

void set_finder_info(const Spec &spec, uint32_t type, uint32_t creator, uint16_t flags) {
    fs::path p = host_path_for_new(spec);
    Entry e;
    if (lookup(spec.vref, spec.parent, spec.name, e)) p = e.host;
    FolderMeta &m = meta_for(p.parent_path());
    m.files[p.filename().u8string()] = FileMeta{spec.name, type, creator, flags};
    save_meta(p.parent_path());
}

fs::path rsrc_path(const fs::path &data_path) {
    fs::path r = data_path;
    r += ".rsrc";
    return r;
}

Spec read_fsspec(uint32_t a) {
    Spec s;
    s.vref = (int16_t)ld16(a);
    s.parent = ld32(a + 2);
    s.name = read_pstr(a + 6);
    return s;
}
void write_fsspec(uint32_t a, const Spec &s) {
    st16(a, (uint16_t)s.vref);
    st32(a + 2, s.parent);
    std::string n = s.name.size() > 63 ? s.name.substr(0, 63) : s.name;
    write_pstr(a + 6, n);
}

}  // namespace pt::vfs
