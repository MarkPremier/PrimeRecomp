/* SDL2 host layer.
 *
 * Presentation: the guest framebuffer (8/16/32 bpp, Mac pixel formats) is
 * converted through the screen's live colour table every present, so palette
 * animation (fades) shows up immediately. The Mac cursor is composited on top.
 *
 * Input: SDL keys map to Mac virtual key codes (Apple Extended Keyboard).
 * Left Alt is Command, Right Alt / Windows is Option, Ctrl is Control.
 * When the guest hides the cursor (gameplay), the mouse is captured in
 * relative mode and the game's own recentring (RawMouse/CrsrNew) is honoured.
 */
#define SDL_MAIN_HANDLED
#include <SDL.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <deque>
#include <mutex>

#include "pt/host.h"
#include "pt/qd.h"
#include "pt/runtime.h"

namespace pt::host {

const int kAudioRate = 44100;

static SDL_Window *g_win;
static SDL_Renderer *g_ren;
static SDL_Texture *g_tex;
static uint32_t g_rgba[640 * 480];
static uint64_t g_last_present = 0;
static std::deque<MacEvent> g_events;
static uint8_t g_keys[16];
static int g_mx = 320, g_my = 240;
static std::atomic<int> g_dx_acc{0}, g_dy_acc{0};   /* raw motion since take_mouse_delta (mouse look) */
static bool g_button = false;
static int g_cursor_level = 0;     /* < 0 hidden */
static bool g_obscured = false;
static uint8_t g_cursor[68];
static bool g_have_cursor = false;
static bool g_relative = false;
static AudioFill g_audio_fill = nullptr;
static SDL_AudioDeviceID g_audio = 0;
static std::mutex g_audio_mutex;

static int mac_keycode(SDL_Scancode s) {
    switch (s) {
    case SDL_SCANCODE_A: return 0x00; case SDL_SCANCODE_S: return 0x01; case SDL_SCANCODE_D: return 0x02;
    case SDL_SCANCODE_F: return 0x03; case SDL_SCANCODE_H: return 0x04; case SDL_SCANCODE_G: return 0x05;
    case SDL_SCANCODE_Z: return 0x06; case SDL_SCANCODE_X: return 0x07; case SDL_SCANCODE_C: return 0x08;
    case SDL_SCANCODE_V: return 0x09; case SDL_SCANCODE_B: return 0x0B; case SDL_SCANCODE_Q: return 0x0C;
    case SDL_SCANCODE_W: return 0x0D; case SDL_SCANCODE_E: return 0x0E; case SDL_SCANCODE_R: return 0x0F;
    case SDL_SCANCODE_Y: return 0x10; case SDL_SCANCODE_T: return 0x11; case SDL_SCANCODE_1: return 0x12;
    case SDL_SCANCODE_2: return 0x13; case SDL_SCANCODE_3: return 0x14; case SDL_SCANCODE_4: return 0x15;
    case SDL_SCANCODE_6: return 0x16; case SDL_SCANCODE_5: return 0x17; case SDL_SCANCODE_EQUALS: return 0x18;
    case SDL_SCANCODE_9: return 0x19; case SDL_SCANCODE_7: return 0x1A; case SDL_SCANCODE_MINUS: return 0x1B;
    case SDL_SCANCODE_8: return 0x1C; case SDL_SCANCODE_0: return 0x1D; case SDL_SCANCODE_RIGHTBRACKET: return 0x1E;
    case SDL_SCANCODE_O: return 0x1F; case SDL_SCANCODE_U: return 0x20; case SDL_SCANCODE_LEFTBRACKET: return 0x21;
    case SDL_SCANCODE_I: return 0x22; case SDL_SCANCODE_P: return 0x23; case SDL_SCANCODE_RETURN: return 0x24;
    case SDL_SCANCODE_L: return 0x25; case SDL_SCANCODE_J: return 0x26; case SDL_SCANCODE_APOSTROPHE: return 0x27;
    case SDL_SCANCODE_K: return 0x28; case SDL_SCANCODE_SEMICOLON: return 0x29; case SDL_SCANCODE_BACKSLASH: return 0x2A;
    case SDL_SCANCODE_COMMA: return 0x2B; case SDL_SCANCODE_SLASH: return 0x2C; case SDL_SCANCODE_N: return 0x2D;
    case SDL_SCANCODE_M: return 0x2E; case SDL_SCANCODE_PERIOD: return 0x2F; case SDL_SCANCODE_TAB: return 0x30;
    case SDL_SCANCODE_SPACE: return 0x31; case SDL_SCANCODE_GRAVE: return 0x32; case SDL_SCANCODE_BACKSPACE: return 0x33;
    case SDL_SCANCODE_ESCAPE: return 0x35; case SDL_SCANCODE_LALT: return 0x37; case SDL_SCANCODE_LSHIFT: return 0x38;
    case SDL_SCANCODE_CAPSLOCK: return 0x39; case SDL_SCANCODE_RALT: return 0x3A; case SDL_SCANCODE_LGUI: return 0x3A;
    case SDL_SCANCODE_LCTRL: return 0x3B; case SDL_SCANCODE_RSHIFT: return 0x3C; case SDL_SCANCODE_RCTRL: return 0x3E;
    case SDL_SCANCODE_KP_PERIOD: return 0x41; case SDL_SCANCODE_KP_MULTIPLY: return 0x43; case SDL_SCANCODE_KP_PLUS: return 0x45;
    case SDL_SCANCODE_NUMLOCKCLEAR: return 0x47; case SDL_SCANCODE_KP_DIVIDE: return 0x4B; case SDL_SCANCODE_KP_ENTER: return 0x4C;
    case SDL_SCANCODE_KP_MINUS: return 0x4E; case SDL_SCANCODE_KP_EQUALS: return 0x51; case SDL_SCANCODE_KP_0: return 0x52;
    case SDL_SCANCODE_KP_1: return 0x53; case SDL_SCANCODE_KP_2: return 0x54; case SDL_SCANCODE_KP_3: return 0x55;
    case SDL_SCANCODE_KP_4: return 0x56; case SDL_SCANCODE_KP_5: return 0x57; case SDL_SCANCODE_KP_6: return 0x58;
    case SDL_SCANCODE_KP_7: return 0x59; case SDL_SCANCODE_KP_8: return 0x5B; case SDL_SCANCODE_KP_9: return 0x5C;
    case SDL_SCANCODE_F5: return 0x60; case SDL_SCANCODE_F6: return 0x61; case SDL_SCANCODE_F7: return 0x62;
    case SDL_SCANCODE_F3: return 0x63; case SDL_SCANCODE_F8: return 0x64; case SDL_SCANCODE_F9: return 0x65;
    case SDL_SCANCODE_F11: return 0x67; case SDL_SCANCODE_F13: return 0x69; case SDL_SCANCODE_F14: return 0x6B;
    case SDL_SCANCODE_F10: return 0x6D; case SDL_SCANCODE_F12: return 0x6F; case SDL_SCANCODE_F15: return 0x71;
    case SDL_SCANCODE_INSERT: return 0x72; case SDL_SCANCODE_HOME: return 0x73; case SDL_SCANCODE_PAGEUP: return 0x74;
    case SDL_SCANCODE_DELETE: return 0x75; case SDL_SCANCODE_F4: return 0x76; case SDL_SCANCODE_END: return 0x77;
    case SDL_SCANCODE_F2: return 0x78; case SDL_SCANCODE_PAGEDOWN: return 0x79; case SDL_SCANCODE_F1: return 0x7A;
    case SDL_SCANCODE_LEFT: return 0x7B; case SDL_SCANCODE_RIGHT: return 0x7C; case SDL_SCANCODE_DOWN: return 0x7D;
    case SDL_SCANCODE_UP: return 0x7E;
    default: return -1;
    }
}

static uint8_t mac_char(SDL_Keycode k, int code, bool shift) {
    switch (code) {
    case 0x24: return 0x0D; case 0x4C: return 0x03; case 0x30: return 0x09; case 0x35: return 0x1B;
    case 0x33: return 0x08; case 0x75: return 0x7F; case 0x7B: return 0x1C; case 0x7C: return 0x1D;
    case 0x7E: return 0x1E; case 0x7D: return 0x1F; case 0x73: return 0x01; case 0x77: return 0x04;
    case 0x74: return 0x0B; case 0x79: return 0x0C; case 0x72: return 0x05;
    /* keypad: the Mac types its characters whatever Num Lock says */
    case 0x52: return '0'; case 0x53: return '1'; case 0x54: return '2'; case 0x55: return '3';
    case 0x56: return '4'; case 0x57: return '5'; case 0x58: return '6'; case 0x59: return '7';
    case 0x5B: return '8'; case 0x5C: return '9'; case 0x41: return '.'; case 0x45: return '+';
    case 0x4E: return '-'; case 0x43: return '*'; case 0x4B: return '/'; case 0x51: return '=';
    }
    if (k >= SDLK_a && k <= SDLK_z) return (uint8_t)(shift ? k - 32 : k);
    if (k >= 32 && k < 127) {
        if (!shift) return (uint8_t)k;
        static const char *from = "1234567890-=[]\\;',./`", *to = "!@#$%^&*()_+{}|:\"<>?~";
        const char *p = strchr(from, (char)k);
        return p ? (uint8_t)to[p - from] : (uint8_t)k;
    }
    if (code >= 0x60 && code <= 0x7A) return 0x10;   /* function keys */
    return 0;
}

uint16_t modifiers() {
    uint16_t m = g_button ? 0 : 0x80;   /* btnState is set when the button is UP */
    auto down = [](int k) { return (g_keys[k >> 3] >> (k & 7)) & 1; };
    if (down(0x37)) m |= 0x0100;
    if (down(0x38) || down(0x3C)) m |= 0x0200;
    if (down(0x39)) m |= 0x0400;
    if (down(0x3A) || down(0x3D)) m |= 0x0800;
    if (down(0x3B) || down(0x3E)) m |= 0x1000;
    return m;
}

void init() {
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS | SDL_INIT_TIMER) != 0)
        fatal("SDL_Init: %s", SDL_GetError());
    int scale = 2;
    if (const char *s = option("PT_SCALE")) scale = atoi(s) > 0 ? atoi(s) : 2;
    g_win = SDL_CreateWindow("Prime Target", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 640 * scale, 480 * scale,
                             SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!g_win) fatal("SDL_CreateWindow: %s", SDL_GetError());
    /* The game owned the whole screen on the Mac and draws its own pointer, so keep the
     * Windows pointer inside the window while it has focus (Alt+Tab leaves). mouse_grab = 0 turns it off. */
    if (const char *g = option("PT_MOUSE_GRAB"); !(g && *g == '0')) SDL_SetWindowMouseGrab(g_win, SDL_TRUE);
    g_ren = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_ACCELERATED);
    if (!g_ren) g_ren = SDL_CreateRenderer(g_win, -1, 0);
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
    SDL_RenderSetLogicalSize(g_ren, 640, 480);
    g_tex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, 640, 480);
    SDL_ShowCursor(SDL_DISABLE);

    SDL_AudioSpec want{}, have{};
    want.freq = kAudioRate;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    want.callback = [](void *, Uint8 *stream, int len) {
        std::lock_guard<std::mutex> lk(g_audio_mutex);
        if (g_audio_fill) g_audio_fill((int16_t *)stream, len / 4);
        else memset(stream, 0, len);
        /* PT_AUDIO_DUMP=file.raw: raw 44.1 kHz stereo s16 capture for checking the mixer */
        static FILE *dump = getenv("PT_AUDIO_DUMP") ? fopen(getenv("PT_AUDIO_DUMP"), "wb") : nullptr;
        if (dump) { fwrite(stream, 1, len, dump); fflush(dump); }
    };
    g_audio = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (g_audio) SDL_PauseAudioDevice(g_audio, 0);
    else logf(LOG_UNIMPL, "audio device unavailable: %s\n", SDL_GetError());
    st16(0x0172, 0x80);   /* MBState: up */
}

void set_audio_fill(AudioFill fn) { std::lock_guard<std::mutex> lk(g_audio_mutex); g_audio_fill = fn; }
void lock_audio() { g_audio_mutex.lock(); }
void unlock_audio() { g_audio_mutex.unlock(); }

void post_event(const MacEvent &e) {
    if (g_events.size() < 64) g_events.push_back(e);
}

static void key_event(const SDL_KeyboardEvent &k, bool down) {
    int code = mac_keycode(k.keysym.scancode);
    if (code < 0) return;
    bool was = (g_keys[code >> 3] >> (code & 7)) & 1;
    if (down) g_keys[code >> 3] |= (uint8_t)(1 << (code & 7));
    else g_keys[code >> 3] &= (uint8_t)~(1 << (code & 7));
    if (code >= 0x37 && code <= 0x3E) return;   /* modifiers alone generate no events */
    bool shift = (modifiers() & 0x0200) != 0;
    uint8_t ch = mac_char(k.keysym.sym, code, shift);
    MacEvent e{};
    e.what = down ? (was || k.repeat ? 5 : 3) : 4;   /* autoKey, keyDown, keyUp */
    e.message = (uint32_t)code << 8 | ch;
    e.when = ticks();
    e.v = (int16_t)g_my;
    e.h = (int16_t)g_mx;
    e.modifiers = modifiers();
    post_event(e);
}

static void set_relative(bool on) {
    if (on == g_relative) return;
    g_relative = on;
    SDL_SetRelativeMouseMode(on ? SDL_TRUE : SDL_FALSE);
    logf(LOG_EVT, "relative mouse %s\n", on ? "on" : "off");
}

/* SDL's renderer already rewrites mouse event coordinates into the 640x480 logical
 * space (SDL_RenderSetLogicalSize), so they are guest coordinates as they come */
static void window_to_guest(int wx, int wy, int &gx, int &gy) {
    gx = wx;
    gy = wy;
}

/* PT_INPUT="t_ms action args; ...": scripted input for automated testing.
 * actions: move X Y | click X Y | down | up | key NAME (return, esc, space, up, down, left, right, tab, or a letter) */
struct Scripted { uint64_t t; std::string op; int a, b; std::string key; };
static std::vector<Scripted> g_script;
static size_t g_script_pos = 0;
static void load_script() {
    const char *env = getenv("PT_INPUT");
    if (!env) return;
    std::string s(env);
    size_t p = 0;
    while (p < s.size()) {
        size_t e = s.find(';', p);
        std::string item = s.substr(p, e == std::string::npos ? std::string::npos : e - p);
        Scripted x{0, "", 0, 0, ""};
        char op[32] = {0}, key[32] = {0};
        unsigned long long t = 0;
        int n = sscanf(item.c_str(), " %llu %31s %d %d", &t, op, &x.a, &x.b);
        if (n >= 2) {
            x.t = t; x.op = op;
            if (x.op == "key" || x.op == "hold" || x.op == "release") { sscanf(item.c_str(), " %*llu %*s %31s", key); x.key = key; }
            g_script.push_back(x);
        }
        if (e == std::string::npos) break;
        p = e + 1;
    }
}
static int key_code_named(const std::string &k) {
    static const struct { const char *n; int c; uint8_t ch; } t[] = {
        {"return", 0x24, 0x0D}, {"esc", 0x35, 0x1B}, {"space", 0x31, ' '}, {"tab", 0x30, 9},
        {"up", 0x7E, 0x1E}, {"down", 0x7D, 0x1F}, {"left", 0x7B, 0x1C}, {"right", 0x7C, 0x1D}};
    for (auto &x : t) if (k == x.n) return x.c << 8 | x.ch;
    if (k.size() == 3 && k[0] == 'k' && k[1] == 'p' && k[2] >= '0' && k[2] <= '9') {
        static const int kp[10] = {0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5B, 0x5C};
        return kp[k[2] - '0'] << 8 | k[2];
    }
    if (k == "shift") return 0x38 << 8;
    if (k == "ctrl") return 0x3B << 8;
    if (k == "option") return 0x3A << 8;
    if (k == "cmd") return 0x37 << 8;
    static const char *letters = "asdfhgzxcv?bqweryt";
    if (k.size() == 1) { const char *p = strchr(letters, k[0]); if (p) return (int)(p - letters) << 8 | k[0]; }
    return -1;
}
static void run_script() {
    static bool loaded = false;
    if (!loaded) { loaded = true; load_script(); }
    uint64_t now = SDL_GetTicks64();
    while (g_script_pos < g_script.size() && g_script[g_script_pos].t <= now) {
        Scripted &x = g_script[g_script_pos++];
        if (x.op == "move" || x.op == "click") {
            g_dx_acc += x.a - g_mx;
            g_dy_acc += x.b - g_my;
            g_mx = x.a;
            g_my = x.b;
        }
        if (x.op == "nudge") {   /* relative motion, as a mouse in relative mode reports it */
            g_dx_acc += x.a;
            g_dy_acc += x.b;
        }
        if (x.op == "click" || x.op == "down") { g_button = true; post_event({1, 0, ticks(), (int16_t)g_my, (int16_t)g_mx, modifiers()}); }
        if (x.op == "click" || x.op == "up") {
            if (x.op == "click") { g_button = false; post_event({2, 0, ticks(), (int16_t)g_my, (int16_t)g_mx, modifiers()}); }
            else { g_button = false; post_event({2, 0, ticks(), (int16_t)g_my, (int16_t)g_mx, modifiers()}); }
        }
        if (x.op == "hold" || x.op == "release") {
            int kc = key_code_named(x.key);
            if (kc >= 0) {
                int code = kc >> 8;
                if (x.op == "hold") g_keys[code >> 3] |= (uint8_t)(1 << (code & 7));
                else g_keys[code >> 3] &= (uint8_t)~(1 << (code & 7));
            }
        }
        if (x.op == "key") {
            int kc = key_code_named(x.key);
            if (kc >= 0) {
                post_event({3, (uint32_t)kc, ticks(), (int16_t)g_my, (int16_t)g_mx, modifiers()});
                post_event({4, (uint32_t)kc, ticks(), (int16_t)g_my, (int16_t)g_mx, modifiers()});
            }
        }
    }
}

void pump() {
    /* The game recentres the mouse by writing RawMouse/MTemp and setting CrsrNew. Apply that
     * before this pump's motion, so movement since the game last looked adds to the new centre
     * rather than being thrown away (mouse control felt jerky). */
    if (g_mem[0x08CE]) {
        g_my = (int16_t)ld16(0x082C);
        g_mx = (int16_t)ld16(0x082E);
        g_mem[0x08CE] = 0;
    }
    run_script();
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
        case SDL_QUIT: {
            /* behave like Command-Q */
            MacEvent e{3, 0x0C << 8 | 'q', ticks(), (int16_t)g_my, (int16_t)g_mx, (uint16_t)(modifiers() | 0x0100)};
            post_event(e);
            static int quits = 0;
            if (++quits >= 2) { SDL_Quit(); std::_Exit(0); }
            break;
        }
        case SDL_KEYDOWN:
            if (ev.key.keysym.sym == SDLK_RETURN && (ev.key.keysym.mod & KMOD_ALT)) {
                bool fs = SDL_GetWindowFlags(g_win) & SDL_WINDOW_FULLSCREEN_DESKTOP;
                SDL_SetWindowFullscreen(g_win, fs ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
                break;
            }
            key_event(ev.key, true);
            break;
        case SDL_KEYUP: key_event(ev.key, false); break;
        case SDL_MOUSEMOTION:
            g_dx_acc += ev.motion.xrel;
            g_dy_acc += ev.motion.yrel;
            if (g_relative) { g_mx += ev.motion.xrel; g_my += ev.motion.yrel; }
            else window_to_guest(ev.motion.x, ev.motion.y, g_mx, g_my);
            g_obscured = false;
            break;
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP: {
            if (ev.button.button != SDL_BUTTON_LEFT) break;
            if (!g_relative) window_to_guest(ev.button.x, ev.button.y, g_mx, g_my);
            g_button = ev.type == SDL_MOUSEBUTTONDOWN;
            MacEvent e{(uint16_t)(g_button ? 1 : 2), 0, ticks(), (int16_t)g_my, (int16_t)g_mx, modifiers()};
            post_event(e);
            break;
        }
        case SDL_WINDOWEVENT:
            if (ev.window.event == SDL_WINDOWEVENT_FOCUS_LOST) { memset(g_keys, 0, sizeof g_keys); set_relative(false); }
            break;
        }
    }
    if (!g_relative) {
        g_mx = g_mx < 0 ? 0 : g_mx > 639 ? 639 : g_mx;
        g_my = g_my < 0 ? 0 : g_my > 479 ? 479 : g_my;
    }
    bool want_rel = g_cursor_level < 0 && (SDL_GetWindowFlags(g_win) & SDL_WINDOW_INPUT_FOCUS);
    set_relative(want_rel);
    /* low-memory mirrors */
    st32(0x016A, ticks());                             /* Ticks */
    memcpy(g_mem + 0x0174, g_keys, 16);                /* KeyMap */
    st16(0x0172, g_button ? 0x00 : 0x80);              /* MBState */
    st16(0x0830, (uint16_t)g_my); st16(0x0832, (uint16_t)g_mx);   /* Mouse */
    if (!g_mem[0x08CE]) { st16(0x082C, (uint16_t)g_my); st16(0x082E, (uint16_t)g_mx); st16(0x0828, (uint16_t)g_my); st16(0x082A, (uint16_t)g_mx); }
    present(false);
}

bool next_event(uint16_t mask, MacEvent &out, bool remove) {
    for (auto it = g_events.begin(); it != g_events.end(); ++it) {
        if (mask & (1u << it->what)) {
            out = *it;
            if (remove) g_events.erase(it);
            return true;
        }
    }
    return false;
}
void flush_events(uint16_t mask) {
    for (auto it = g_events.begin(); it != g_events.end();)
        if (mask & (1u << it->what)) it = g_events.erase(it);
        else ++it;
}
void key_map(uint8_t out[16]) { memcpy(out, g_keys, 16); }
void mouse_global(int16_t &v, int16_t &h) { v = (int16_t)g_my; h = (int16_t)g_mx; }
bool mouse_down() { return g_button; }
void take_mouse_delta(int &dx, int &dy) { dx = g_dx_acc.exchange(0); dy = g_dy_acc.exchange(0); }
void peek_mouse_delta(int &dx, int &dy) { dx = g_dx_acc.load(); dy = g_dy_acc.load(); }
int refresh_rate() {
    SDL_DisplayMode m;
    int d = g_win ? SDL_GetWindowDisplayIndex(g_win) : 0;
    return SDL_GetCurrentDisplayMode(d < 0 ? 0 : d, &m) == 0 && m.refresh_rate > 0 ? m.refresh_rate : 60;
}

void set_cursor(const uint8_t data[68]) { memcpy(g_cursor, data, 68); g_have_cursor = true; }
void hide_cursor() { g_cursor_level--; }
void show_cursor() { if (g_cursor_level < 0) g_cursor_level++; }
void init_cursor() { g_cursor_level = 0; g_obscured = false; }
void obscure_cursor() { g_obscured = true; }

void present(bool force) {
    uint64_t now = SDL_GetTicks64();
    if (!force && now - g_last_present < 15) return;
    g_last_present = now;
    qd::Screen sc = qd::screen();
    const uint8_t *fb = g_mem + sc.base;
    if (sc.depth == 8) {
        auto pal = qd::screen_palette();
        uint32_t lut[256];
        for (int i = 0; i < 256; i++) {
            qd::RGB c = i < (int)pal.size() ? pal[i] : qd::RGB{0, 0, 0};
            lut[i] = 0xFF000000u | (uint32_t)(c.r >> 8) << 16 | (uint32_t)(c.g >> 8) << 8 | (c.b >> 8);
        }
        for (int y = 0; y < 480; y++)
            for (int x = 0; x < 640; x++) g_rgba[y * 640 + x] = lut[fb[y * sc.row_bytes + x]];
    } else if (sc.depth == 16) {
        for (int y = 0; y < 480; y++)
            for (int x = 0; x < 640; x++) {
                uint32_t v = (uint32_t)fb[y * sc.row_bytes + 2 * x] << 8 | fb[y * sc.row_bytes + 2 * x + 1];
                uint32_t r = (v >> 10) & 31, g = (v >> 5) & 31, b = v & 31;
                g_rgba[y * 640 + x] = 0xFF000000u | (r << 19 | (r >> 2) << 16) | (g << 11 | (g >> 2) << 8) | (b << 3 | b >> 2);
            }
    } else if (sc.depth == 32) {
        for (int y = 0; y < 480; y++)
            for (int x = 0; x < 640; x++) {
                const uint8_t *p = fb + y * sc.row_bytes + 4 * x;
                g_rgba[y * 640 + x] = 0xFF000000u | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
            }
    } else {
        for (int y = 0; y < 480; y++)
            for (int x = 0; x < 640; x++) {
                int bit = (fb[y * sc.row_bytes + (x * sc.depth >> 3)] >> (8 - sc.depth - (x * sc.depth & 7))) & ((1 << sc.depth) - 1);
                g_rgba[y * 640 + x] = bit ? 0xFF000000u : 0xFFFFFFFFu;
            }
    }
    /* composite the Mac cursor */
    if (g_have_cursor && g_cursor_level >= 0 && !g_obscured) {
        int hy = (int16_t)(g_cursor[64] << 8 | g_cursor[65]), hx = (int16_t)(g_cursor[66] << 8 | g_cursor[67]);
        for (int y = 0; y < 16; y++) {
            uint16_t d = (uint16_t)(g_cursor[2 * y] << 8 | g_cursor[2 * y + 1]);
            uint16_t m = (uint16_t)(g_cursor[32 + 2 * y] << 8 | g_cursor[33 + 2 * y]);
            for (int x = 0; x < 16; x++) {
                int px = g_mx - hx + x, py = g_my - hy + y;
                if (px < 0 || px >= 640 || py < 0 || py >= 480) continue;
                bool db = (d >> (15 - x)) & 1, mb = (m >> (15 - x)) & 1;
                uint32_t &o = g_rgba[py * 640 + px];
                if (mb) o = db ? 0xFF000000u : 0xFFFFFFFFu;
                else if (db) o ^= 0x00FFFFFFu;
            }
        }
    }
    /* debugging aids: periodic screenshots and a run-time limit */
    static const char *shot_dir = getenv("PT_SHOT_DIR");
    static const char *run_secs = getenv("PT_RUN_SECONDS");
    static uint64_t last_shot = 0;
    static int shot_n = 0;
    if (shot_dir && now - last_shot >= 2000) {
        last_shot = now;
        SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormatFrom(g_rgba, 640, 480, 32, 640 * 4, SDL_PIXELFORMAT_ARGB8888);
        char path[512];
        snprintf(path, sizeof path, "%s/shot_%03d.bmp", shot_dir, shot_n++);
        SDL_SaveBMP(surf, path);
        SDL_FreeSurface(surf);
        if (sc.depth == 8) {
            auto pal = qd::screen_palette();
            unsigned hist[256] = {0};
            for (int y = 0; y < 480; y++)
                for (int x = 0; x < 640; x++) hist[fb[y * sc.row_bytes + x]]++;
            int top = 0;
            for (int i = 1; i < 256; i++) if (hist[i] > hist[top]) top = i;
            unsigned lit = 0;
            for (size_t i = 0; i < pal.size(); i++) if (pal[i].r | pal[i].g | pal[i].b) lit++;
            fprintf(stderr, "[shot %d] most common index %d (%u px) rgb %04X %04X %04X; %u non-black palette entries\n",
                    shot_n - 1, top, hist[top], pal.size() > (size_t)top ? pal[top].r : 0,
                    pal.size() > (size_t)top ? pal[top].g : 0, pal.size() > (size_t)top ? pal[top].b : 0, lit);
        }
    }
    if (run_secs && now > (uint64_t)atoi(run_secs) * 1000) { SDL_Quit(); std::_Exit(0); }
    SDL_UpdateTexture(g_tex, nullptr, g_rgba, 640 * 4);
    SDL_RenderClear(g_ren);
    SDL_RenderCopy(g_ren, g_tex, nullptr, nullptr);
    SDL_RenderPresent(g_ren);
}

}  // namespace pt::host
