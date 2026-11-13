/* Port options: primetarget.ini beside the program, overridden by environment
 * variables. option("PT_MOUSE_SPEED") reads PT_MOUSE_SPEED, else the ini's
 * mouse_speed. A commented template is written on first run.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

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
    "; Window\n"
    "; scale = 2              ; window size: 640x480 times this\n"
    "; mouse_grab = 1         ; 0: let the pointer leave the window\n";

static std::map<std::string, std::string> load() {
    std::map<std::string, std::string> kv;
    wchar_t buf[MAX_PATH * 4];
    GetModuleFileNameW(nullptr, buf, MAX_PATH * 4);
    std::filesystem::path ini = std::filesystem::path(buf).parent_path() / "primetarget.ini";
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

/* the value of option `env_name` (e.g. "PT_MOUSE_SPEED"), or nullptr */
const char *option(const char *env_name) {
    if (const char *e = getenv(env_name); e && *e) return e;
    static std::once_flag once;
    static std::map<std::string, std::string> kv;
    std::call_once(once, [] { kv = load(); });
    std::string key(env_name);
    if (key.rfind("PT_", 0) == 0) key = key.substr(3);
    for (auto &ch : key) ch = (char)tolower((unsigned char)ch);
    auto it = kv.find(key);
    return it == kv.end() || it->second.empty() ? nullptr : it->second.c_str();
}

}  // namespace pt
