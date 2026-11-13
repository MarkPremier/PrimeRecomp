/* First run: copy the game off the player's Prime Target CD image.
 *
 * Accepts a BIN/CUE (one file or one file per track), or a data-only image
 * (.iso, .toast, .img, raw .bin). From the CD's HFS volume the "Prime Target"
 * and "Movies" folders are written to <exe>/game in the layout the runtime
 * reads (data fork, .rsrc resource fork, finder.tsv/folders.tsv with the Mac
 * names and Finder info). CD audio tracks become <exe>/cdaudio/trackNN.wav
 * with toc.tsv. The application is checked against the build the code was
 * recompiled from before anything is written.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "image_info.h"
#include "pt/hfs.h"
#include "pt/runtime.h"

namespace fs = std::filesystem;

namespace pt {
std::string sha1_hex(const uint8_t *data, size_t len);
}

namespace pt::setup {
namespace {

constexpr uint32_t kRaw = 2352, kLeadIn = 150;

/* ---------- progress window ---------- */
std::mutex g_mu;
std::wstring g_status = L"Reading the disc image...";
std::atomic<int> g_permille{0};
std::atomic<bool> g_done{false};

void status(const std::wstring &s, int permille) {
    std::lock_guard<std::mutex> l(g_mu);
    g_status = s;
    g_permille = permille;
}

LRESULT CALLBACK wndproc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_TIMER) {
        if (g_done) DestroyWindow(w);
        else InvalidateRect(w, nullptr, TRUE);
        return 0;
    }
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(w, &ps);
        RECT rc;
        GetClientRect(w, &rc);
        HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        SelectObject(dc, font);
        SetBkMode(dc, TRANSPARENT);
        std::wstring s;
        {
            std::lock_guard<std::mutex> l(g_mu);
            s = g_status;
        }
        RECT t{16, 16, rc.right - 16, 48};
        DrawTextW(dc, s.c_str(), -1, &t, DT_LEFT | DT_TOP | DT_END_ELLIPSIS | DT_SINGLELINE);
        RECT bar{16, 56, rc.right - 16, 76};
        FrameRect(dc, &bar, (HBRUSH)GetStockObject(BLACK_BRUSH));
        RECT fill = bar;
        InflateRect(&fill, -2, -2);
        fill.right = fill.left + (fill.right - fill.left) * g_permille / 1000;
        HBRUSH b = CreateSolidBrush(RGB(0, 90, 170));
        FillRect(dc, &fill, b);
        DeleteObject(b);
        EndPaint(w, &ps);
        return 0;
    }
    if (m == WM_CLOSE) return 0;   /* not cancellable mid-copy */
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(w, m, wp, lp);
}

/* ---------- disc images ---------- */
struct Track {
    int number = 0;
    bool audio = false;
    uint32_t sector = kRaw, data_off = 16;   /* bytes per sector, user data offset */
    size_t file = 0;                         /* index into Disc::files */
    uint64_t index1 = 0;                     /* INDEX 01, frames from the start of its file */
    uint64_t abs = 0, length = 0;            /* frames on the disc (no lead-in) */
};

struct Disc {
    std::vector<fs::path> files;
    std::vector<uint64_t> file_frames, file_start;
    std::vector<Track> tracks;
    uint64_t total = 0;
};

std::string read_text(const fs::path &p) {
    std::string s;
    if (FILE *f = _wfopen(p.c_str(), L"rb")) {
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
        fclose(f);
    }
    return s;
}

fs::path cue_file_path(const fs::path &cue, const std::string &name) {
    /* CUE sheets are usually ANSI or UTF-8; try UTF-8, then the ANSI code page */
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name.data(), (int)name.size(), nullptr, 0);
    UINT cp = n > 0 ? CP_UTF8 : CP_ACP;
    n = MultiByteToWideChar(cp, 0, name.data(), (int)name.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(cp, 0, name.data(), (int)name.size(), w.data(), n);
    fs::path p(w);
    return p.is_absolute() ? p : cue.parent_path() / p;
}

bool parse_cue(const fs::path &cue, Disc &d, std::string &err) {
    std::string text = read_text(cue);
    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find_first_of("\r\n", pos);
        std::string line = text.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
        pos = eol == std::string::npos ? text.size() : eol + 1;
        size_t a = line.find_first_not_of(" \t");
        if (a == std::string::npos) continue;
        line = line.substr(a);
        if (line.rfind("FILE ", 0) == 0) {
            std::string rest = line.substr(5), name;
            if (!rest.empty() && rest[0] == '"') name = rest.substr(1, rest.find('"', 1) - 1);
            else name = rest.substr(0, rest.rfind(' '));
            d.files.push_back(cue_file_path(cue, name));
        } else if (line.rfind("TRACK ", 0) == 0) {
            if (d.files.empty()) { err = "the CUE sheet has a TRACK before any FILE"; return false; }
            Track t;
            t.number = atoi(line.c_str() + 6);
            std::string mode = line.substr(line.rfind(' ') + 1);
            t.file = d.files.size() - 1;
            if (mode == "AUDIO") { t.audio = true; t.sector = kRaw; t.data_off = 0; }
            else if (mode == "MODE1/2048") { t.sector = 2048; t.data_off = 0; }
            else if (mode == "MODE1/2352") { t.sector = kRaw; t.data_off = 16; }
            else if (mode == "MODE2/2352") { t.sector = kRaw; t.data_off = 24; }
            else { err = "unsupported track type " + mode; return false; }
            d.tracks.push_back(t);
        } else if (line.rfind("INDEX 01 ", 0) == 0 && !d.tracks.empty()) {
            int m = 0, s = 0, f = 0;
            sscanf(line.c_str() + 9, "%d:%d:%d", &m, &s, &f);
            d.tracks.back().index1 = (uint64_t)(m * 60 + s) * 75 + f;
        }
    }
    if (d.tracks.empty()) { err = "the CUE sheet lists no tracks"; return false; }
    uint64_t start = 0;
    for (size_t i = 0; i < d.files.size(); i++) {
        std::error_code ec;
        uint64_t bytes = fs::file_size(d.files[i], ec);
        if (ec) { err = "cannot open " + d.files[i].u8string(); return false; }
        uint32_t sector = kRaw;
        for (auto &t : d.tracks) if (t.file == i) { sector = t.sector; break; }
        d.file_start.push_back(start);
        d.file_frames.push_back(bytes / sector);
        start += bytes / sector;
    }
    d.total = start;
    for (auto &t : d.tracks) t.abs = d.file_start[t.file] + t.index1;
    for (size_t i = 0; i < d.tracks.size(); i++)
        d.tracks[i].length = (i + 1 < d.tracks.size() ? d.tracks[i + 1].abs : d.total) - d.tracks[i].abs;
    return true;
}

/* a lone image: raw 2352-byte sectors if it starts with a sync pattern, else 2048 */
bool single_image(const fs::path &img, Disc &d, std::string &err) {
    static const uint8_t sync[12] = {0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0};
    std::error_code ec;
    uint64_t bytes = fs::file_size(img, ec);
    if (ec) { err = "cannot open " + img.u8string(); return false; }
    uint8_t head[16] = {};
    if (FILE *f = _wfopen(img.c_str(), L"rb")) { fread(head, 1, 16, f); fclose(f); }
    Track t;
    t.number = 1;
    if (bytes % kRaw == 0 && memcmp(head, sync, 12) == 0) {
        t.sector = kRaw;
        t.data_off = head[15] == 2 ? 24 : 16;
    } else {
        t.sector = 2048;
        t.data_off = 0;
    }
    d.files.push_back(img);
    d.file_start.push_back(0);
    d.file_frames.push_back(bytes / t.sector);
    d.total = bytes / t.sector;
    t.length = d.total;
    d.tracks.push_back(t);
    return true;
}

/* user data of the first data track, 2048 bytes per sector */
bool read_data_track(const Disc &d, std::vector<uint8_t> &out, std::string &err) {
    const Track *t = nullptr;
    for (auto &x : d.tracks) if (!x.audio) { t = &x; break; }
    if (!t) { err = "the image has no data track"; return false; }
    FILE *f = _wfopen(d.files[t->file].c_str(), L"rb");
    if (!f) { err = "cannot open " + d.files[t->file].u8string(); return false; }
    uint64_t frames = std::min<uint64_t>(t->length, d.file_frames[t->file] - t->index1);
    out.resize((size_t)frames * 2048);
    _fseeki64(f, (int64_t)(t->index1 * t->sector), SEEK_SET);
    std::vector<uint8_t> buf((size_t)t->sector * 256);
    for (uint64_t done = 0; done < frames;) {
        uint64_t n = std::min<uint64_t>(256, frames - done);
        if (fread(buf.data(), t->sector, (size_t)n, f) != n) { fclose(f); err = "the image is truncated"; return false; }
        for (uint64_t i = 0; i < n; i++) memcpy(&out[(size_t)(done + i) * 2048], &buf[(size_t)i * t->sector + t->data_off], 2048);
        done += n;
        status(L"Reading the disc image...", (int)(200 * done / frames));
    }
    fclose(f);
    return true;
}

/* ---------- writing the game folder ---------- */
std::wstring mac_to_wide(const std::string &mac) {
    int n = MultiByteToWideChar(10000, 0, mac.data(), (int)mac.size(), nullptr, 0);   /* Mac Roman */
    std::wstring w(n, L'\0');
    MultiByteToWideChar(10000, 0, mac.data(), (int)mac.size(), w.data(), n);
    return w;
}

std::string to_utf8(const std::wstring &w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

/* HFS names may contain characters Windows rejects (same rule as tools/extract_game.py) */
std::wstring host_name(const std::string &mac) {
    std::wstring w = mac_to_wide(mac);
    for (auto &c : w)
        if (c < 32 || wcschr(L"<>:\"/\\|?*", c)) c = L'_';
    while (!w.empty() && (w.back() == L' ' || w.back() == L'.')) w.pop_back();
    return w;
}

std::string hex(const std::string &bytes) {
    static const char *d = "0123456789abcdef";
    std::string s;
    for (unsigned char c : bytes) { s += d[c >> 4]; s += d[c & 15]; }
    return s;
}

std::string fourcc_hex(uint32_t v) {
    std::string b{(char)(v >> 24), (char)(v >> 16), (char)(v >> 8), (char)v};
    return hex(b);
}

bool write_file(const fs::path &p, const std::vector<uint8_t> &data, std::string &err) {
    FILE *f = _wfopen(p.c_str(), L"wb");
    if (!f) { err = "cannot write " + p.u8string(); return false; }
    bool ok = data.empty() || fwrite(data.data(), 1, data.size(), f) == data.size();
    ok = fclose(f) == 0 && ok;
    if (!ok) err = "cannot write " + p.u8string() + " (disk full?)";
    return ok;
}

struct Copier {
    hfs::Volume &v;
    std::string err;
    size_t files = 0, total_files = 0;

    size_t count(uint32_t dir) {
        size_t n = 0;
        for (auto &name : v.list(dir)) {
            std::vector<uint8_t> rec;
            if (!v.find(dir, name, rec)) continue;
            n += hfs::Volume::is_folder(rec) ? count(hfs::Volume::dir_id(rec)) : 1;
        }
        return n;
    }

    /* copy the children `names` of folder `dir` into host folder `out` */
    bool copy(uint32_t dir, const std::vector<std::string> &names, const fs::path &out) {
        std::error_code ec;
        fs::create_directories(out, ec);
        std::string finder, folders;
        for (auto &name : names) {
            std::vector<uint8_t> rec;
            if (!v.find(dir, name, rec)) continue;
            std::wstring h = host_name(name);
            if (hfs::Volume::is_folder(rec)) {
                folders += to_utf8(h) + "\t" + hex(name) + "\n";
                uint32_t sub = hfs::Volume::dir_id(rec);
                if (!copy(sub, v.list(sub), out / h)) return false;
                continue;
            }
            status(L"Copying " + mac_to_wide(name) + L"...", 200 + (int)(400 * files / std::max<size_t>(total_files, 1)));
            if (!write_file(out / h, v.data_fork(rec), err)) return false;
            auto rsrc = v.rsrc_fork(rec);
            if (!rsrc.empty() && !write_file(out / (h + L".rsrc"), rsrc, err)) return false;
            char flags[8];
            snprintf(flags, sizeof flags, "%04x", hfs::Volume::finder_flags(rec));
            finder += to_utf8(h) + "\t" + hex(name) + "\t" + fourcc_hex(hfs::Volume::file_type(rec)) + "\t" +
                      fourcc_hex(hfs::Volume::file_creator(rec)) + "\t" + flags + "\n";
            files++;
        }
        std::vector<uint8_t> a(finder.begin(), finder.end()), b(folders.begin(), folders.end());
        return write_file(out / "finder.tsv", a, err) && write_file(out / "folders.tsv", b, err);
    }
};

bool rip_audio(const Disc &d, const fs::path &out, std::string &err) {
    std::error_code ec;
    fs::create_directories(out, ec);
    std::string toc;
    uint64_t audio_total = 0, audio_done = 0;
    for (auto &t : d.tracks) if (t.audio) audio_total += t.length;
    std::vector<uint8_t> buf((size_t)kRaw * 256);
    for (auto &t : d.tracks) {
        char line[96];
        snprintf(line, sizeof line, "%d\t%s\t%llu\t%llu\n", t.number, t.audio ? "audio" : "data",
                 (unsigned long long)(t.abs + kLeadIn), (unsigned long long)t.length);
        toc += line;
        if (!t.audio) continue;
        wchar_t name[32];
        swprintf(name, 32, L"track%02d.wav", t.number);
        FILE *w = _wfopen((out / name).c_str(), L"wb");
        if (!w) { err = "cannot write " + (out / name).u8string(); return false; }
        uint32_t bytes = (uint32_t)(t.length * kRaw);
        uint8_t hdr[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 2, 0,
                           0x44, 0xAC, 0, 0, 0x10, 0xB1, 2, 0, 4, 0, 16, 0, 'd', 'a', 't', 'a'};
        uint32_t riff = 36 + bytes;
        memcpy(hdr + 4, &riff, 4);
        memcpy(hdr + 40, &bytes, 4);
        fwrite(hdr, 1, 44, w);
        /* the track may run on into the next file (its pregap) */
        for (uint64_t frame = t.abs, end = t.abs + t.length; frame < end;) {
            size_t fi = 0;
            while (fi + 1 < d.files.size() && d.file_start[fi + 1] <= frame) fi++;
            uint64_t in_file = frame - d.file_start[fi];
            uint64_t n = std::min<uint64_t>({256, end - frame, d.file_frames[fi] - in_file});
            FILE *f = _wfopen(d.files[fi].c_str(), L"rb");
            size_t got = 0;
            if (f) {
                _fseeki64(f, (int64_t)(in_file * kRaw), SEEK_SET);
                got = fread(buf.data(), kRaw, (size_t)n, f);
                fclose(f);
            }
            if (got != n || !n) { fclose(w); err = "the image is truncated"; return false; }
            if (fwrite(buf.data(), kRaw, (size_t)n, w) != n) { fclose(w); err = "cannot write audio (disk full?)"; return false; }
            frame += n;
            audio_done += n;
            status(L"Copying the soundtrack...", 600 + (int)(400 * audio_done / std::max<uint64_t>(audio_total, 1)));
        }
        if (fclose(w) != 0) { err = "cannot write audio (disk full?)"; return false; }
    }
    char line[64];
    snprintf(line, sizeof line, "leadout\t-\t%llu\t0\n", (unsigned long long)(d.total + kLeadIn));
    toc += line;
    std::vector<uint8_t> t(toc.begin(), toc.end());
    return write_file(out / "toc.tsv", t, err);
}

/* the whole import, on a worker thread; returns an error message or "" */
std::string import(const fs::path &image, const fs::path &root) {
    Disc d;
    std::string err;
    std::wstring ext = image.extension().wstring();
    for (auto &c : ext) c = (wchar_t)towlower(c);
    if (ext == L".cue" ? !parse_cue(image, d, err) : !single_image(image, d, err)) return err;
    std::vector<uint8_t> data;
    if (!read_data_track(d, data, err)) return err;
    hfs::Volume v;
    if (!v.open(std::move(data))) return "this image has no Mac (HFS) volume; it is not the Prime Target CD";
    std::vector<uint8_t> folder, app;
    if (!v.find(2, "Prime Target", folder) || !hfs::Volume::is_folder(folder))
        return "there is no \"Prime Target\" folder on this disc; it is not the Prime Target CD";
    uint32_t pt_dir = hfs::Volume::dir_id(folder);
    if (!v.find(pt_dir, PT_IMAGE_APP_NAME, app) || hfs::Volume::is_folder(app))
        return "the disc's Prime Target folder has no \"" PT_IMAGE_APP_NAME "\" application";
    auto code = v.data_fork(app);
    std::string sum = sha1_hex(code.data(), code.size());
    if (sum != PT_IMAGE_SHA1)
        return "this disc's copy of Prime Target is a different version from the one this port was built from "
               "(v1.2, the retail CD). SHA-1 " + sum + ", expected " PT_IMAGE_SHA1 ".";

    std::error_code ec;
    fs::path game_tmp = root / "game.partial", audio_tmp = root / "cdaudio.partial";
    fs::remove_all(game_tmp, ec);
    fs::remove_all(audio_tmp, ec);
    Copier c{v};
    std::vector<std::string> top;
    for (const char *n : {"Prime Target", "Movies"}) {
        std::vector<uint8_t> r;
        if (v.find(2, n, r)) top.push_back(n);
    }
    for (auto &n : top) {
        std::vector<uint8_t> r;
        v.find(2, n, r);
        c.total_files += c.count(hfs::Volume::dir_id(r));
    }
    if (!c.copy(2, top, game_tmp)) return c.err;
    bool audio = false;
    for (auto &t : d.tracks) audio |= t.audio;
    if (audio && !rip_audio(d, audio_tmp, err)) return err;
    fs::rename(game_tmp, root / "game", ec);
    if (ec) return "cannot create " + (root / "game").u8string() + ": " + ec.message();
    if (audio) {
        fs::remove_all(root / "cdaudio", ec);
        fs::rename(audio_tmp, root / "cdaudio", ec);
    }
    status(L"Done.", 1000);
    return {};
}

fs::path find_image(const fs::path &root) {
    /* a path passed on the command line (an image dropped on the exe) */
    int argc = 0;
    if (LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
        fs::path p = argc > 1 ? fs::path(argv[1]) : fs::path();
        LocalFree(argv);
        std::error_code ec;
        if (!p.empty() && fs::is_regular_file(p, ec)) return p;
    }
    /* an image beside the program: a CUE sheet first, then data-only images */
    std::error_code ec;
    for (const wchar_t *want : {L".cue", L".iso", L".toast", L".img", L".bin"})
        for (auto &e : fs::directory_iterator(root, ec)) {
            std::wstring x = e.path().extension().wstring();
            for (auto &ch : x) ch = (wchar_t)towlower(ch);
            if (e.is_regular_file(ec) && x == want) return e.path();
        }
    /* ask */
    wchar_t file[MAX_PATH * 4] = L"";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof ofn;
    ofn.lpstrFilter = L"Prime Target CD image (*.cue;*.iso;*.toast;*.img;*.bin)\0*.cue;*.iso;*.toast;*.img;*.bin\0"
                      L"All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = (DWORD)(sizeof file / sizeof file[0]);
    ofn.lpstrTitle = L"Prime Target: choose your Prime Target CD image";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    if (!GetOpenFileNameW(&ofn)) return {};
    return file;
}

}  // namespace

/* Makes <root>/game from the player's CD image; returns false if they cancelled. */
bool import_game(const fs::path &root) {
    fs::path image = find_image(root);
    if (image.empty()) return false;
    logf(LOG_UNIMPL, "setup: importing %s\n", image.u8string().c_str());

    WNDCLASSW wc{};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_WAIT);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"PrimeTargetSetup";
    RegisterClassW(&wc);
    RECT r{0, 0, 460, 96};
    AdjustWindowRect(&r, WS_CAPTION | WS_SYSMENU, FALSE);
    int w = r.right - r.left, h = r.bottom - r.top;
    HWND win = CreateWindowW(wc.lpszClassName, L"Prime Target: first-time setup", WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                             (GetSystemMetrics(SM_CXSCREEN) - w) / 2, (GetSystemMetrics(SM_CYSCREEN) - h) / 2, w, h,
                             nullptr, nullptr, wc.hInstance, nullptr);
    SetTimer(win, 1, 100, nullptr);
    std::string err;
    std::thread worker([&] {
        err = import(image, root);
        g_done = true;
    });
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    worker.join();
    if (!err.empty()) fatal("Could not set up Prime Target from %s:\n\n%s", image.u8string().c_str(), err.c_str());
    return true;
}

}  // namespace pt::setup
