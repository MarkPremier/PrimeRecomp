/* Port options: primetarget.ini beside the program, overridden by environment
 * variables. option("PT_MOUSE_SPEED") reads PT_MOUSE_SPEED, else the ini's
 * mouse_speed. A commented template is written on first run.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>

namespace pt {

static const char kTemplate[] =
    "; Prime Target port options. Remove the ; at the start of a line to change one.\n"
    "; (Environment variables with the same name in capitals and a PT_ prefix, such as\n"
    "; PT_MOUSE_SPEED, override this file.)\n"
    "\n"
    "; Mouse look (Preferences > Controls > Mouse and Keyboard, \"Mouse controls view\")\n"
    "; mouse_speed = 1        ; turning and looking speed: 0.5 slower, 2 faster\n"
    "; mouse_invert = 0       ; 1: pushing the mouse forward looks down\n"
    "; mouse_lock_y = 0       ; 1: the mouse only turns left and right; looking up and down\n"
    ";                        ;    is left to the keys\n"
    "; original_mouse = 0     ; 1: the original game's mouse (key presses, flips up/down)\n"
    "\n"
    "; Frame rate. The game world runs 30 times a second; the port draws frames in between\n"
    "; with the camera, platforms and moving objects blended between ticks, as Aleph One does.\n"
    "; interpolate = 1        ; 0: the original 30 frames per second\n"
    "; max_fps = 0            ; cap on frames per second (0: the display's refresh rate)\n"
    "; vsync = 1              ; 0: don't wait for the display (may tear)\n"
    "\n"
    "; Window\n"
    "; widescreen = auto      ; the 3D view's shape (also Preferences > Graphics > Widescreen):\n"
    ";                        ;    auto (the monitor's), 0 (the original 4:3),\n"
    ";                        ;    or a shape such as 16:9 (up to about 2:1); menus stay 4:3\n"
    "; scale = 2              ; window size: 480 lines times this\n"
    "; mouse_grab = 1         ; 0: let the pointer leave the window\n"
    "; fullscreen = 0         ; 1: start filling the monitor (Alt+Enter switches; Window Size\n"
    ";                        ;    \"Full Screen\" in Preferences > Graphics also fills it)\n";

static std::filesystem::path ini_path() {
    wchar_t buf[MAX_PATH * 4];
    GetModuleFileNameW(nullptr, buf, MAX_PATH * 4);
    return std::filesystem::path(buf).parent_path() / "primetarget.ini";
}

static std::map<std::string, std::string> load() {
    std::map<std::string, std::string> kv;
    std::filesystem::path ini = ini_path();
    FILE *f = _wfopen(ini.c_str(), L"rb");
    if (!f) {
        if (FILE *w = _wfopen(ini.c_str(), L"wb")) {
            fputs(kTemplate, w);
            fclose(w);
        }
        return kv;
    }
    char line[512];
    while (fgets(line, sizeof line, f)) {
        std::string s(line);
        size_t semi = s.find(';');
        if (semi != std::string::npos) s.resize(semi);
        size_t eq = s.find('=');
        if (eq == std::string::npos) continue;
        auto trim = [](std::string t) {
            while (!t.empty() && isspace((unsigned char)t.back())) t.pop_back();
            size_t a = 0;
            while (a < t.size() && isspace((unsigned char)t[a])) a++;
            return t.substr(a);
        };
        std::string k = trim(s.substr(0, eq)), v = trim(s.substr(eq + 1));
        for (auto &ch : k) ch = (char)tolower((unsigned char)ch);
        if (!k.empty()) kv[k] = v;
    }
    fclose(f);
    return kv;
}

static std::map<std::string, std::string> &values() {
    static std::once_flag once;
    static std::map<std::string, std::string> kv;
    std::call_once(once, [] { kv = load(); });
    return kv;
}

static std::string key_of(const char *env_name) {
    std::string key(env_name);
    if (key.rfind("PT_", 0) == 0) key = key.substr(3);
    for (auto &ch : key) ch = (char)tolower((unsigned char)ch);
    return key;
}

/* the value of option `env_name` (e.g. "PT_MOUSE_SPEED"), or nullptr */
const char *option(const char *env_name) {
    if (const char *e = getenv(env_name); e && *e) return e;
    auto &kv = values();
    auto it = kv.find(key_of(env_name));
    return it == kv.end() || it->second.empty() ? nullptr : it->second.c_str();
}

/* Save an option (from an in-game setting) to primetarget.ini: replaces its line, or the
 * commented-out template line for it, or adds one; everything else is kept. */
void set_option(const char *env_name, const char *value) {
    std::string key = key_of(env_name);
    values()[key] = value;
    std::filesystem::path ini = ini_path();
    std::string text;
    if (FILE *f = _wfopen(ini.c_str(), L"rb")) {
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
        fclose(f);
    } else {
        text = kTemplate;
    }
    std::string out, line, active = key + " = " + value;
    bool done = false;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        line = text.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
        pos = eol == std::string::npos ? text.size() : eol + 1;
        std::string probe = line;
        size_t a = probe.find_first_not_of(" \t;");
        probe = a == std::string::npos ? "" : probe.substr(a);
        bool is_key = !done && probe.rfind(key, 0) == 0 &&
                      probe.find_first_not_of(" \t", key.size()) != std::string::npos &&
                      probe[probe.find_first_not_of(" \t", key.size())] == '=';
        if (is_key) {
            size_t comment = line.find(';', line.find('='));   /* keep the explanation */
            out += active + (comment != std::string::npos ? std::string(std::max<int>(1, 25 - (int)active.size()), ' ') +
                                                                 line.substr(comment) : "") + "\n";
            done = true;
        } else {
            out += line + "\n";
        }
    }
    if (!done) out += active + "\n";
    if (FILE *f = _wfopen(ini.c_str(), L"wb")) {
        fwrite(out.data(), 1, out.size(), f);
        fclose(f);
    }
}

}  // namespace pt
