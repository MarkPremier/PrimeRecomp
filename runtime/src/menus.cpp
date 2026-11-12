/* Menu Manager.
 *
 * MenuInfo: menuID(0) menuWidth(2) menuHeight(4) menuProc(6) enableFlags(10)
 *           menuData(14): title Str, then items {Str name, icon, keyEquiv, mark, style}, 0-terminated.
 * Menus are drawn natively (System 7 look). Pull-down and pop-up tracking save
 * and restore the pixels underneath, as the real Menu Manager does.
 */
#include <windows.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "pt/host.h"
#include "pt/qd.h"
#include "pt/resources.h"
#include "pt/runtime.h"
#include "pt/ui.h"

namespace pt::menu {

using namespace pt::qd;

struct Item { std::string name; uint8_t icon, key, mark, style; };

std::vector<Item> items(uint32_t mh) {
    std::vector<Item> out;
    if (!mh || !ld32(mh)) return out;
    uint32_t p = ld32(mh), end = p + handle_size(mh);
    uint32_t a = p + 14 + 1 + g_mem[p + 14];
    while (a < end && g_mem[a]) {
        Item it;
        it.name = read_pstr(a);
        a += 1 + g_mem[a];
        it.icon = g_mem[a]; it.key = g_mem[a + 1]; it.mark = g_mem[a + 2]; it.style = g_mem[a + 3];
        a += 4;
        out.push_back(it);
    }
    return out;
}

static std::string title(uint32_t mh) { return read_pstr(ld32(mh) + 14); }

static void write_items(uint32_t mh, const std::vector<Item> &list) {
    std::string t = title(mh);
    uint32_t size = 14 + 1 + (uint32_t)t.size() + 1;
    for (auto &i : list) size += 1 + (uint32_t)i.name.size() + 4;
    handle_set_size(mh, size);
    uint32_t p = ld32(mh);
    write_pstr(p + 14, t);
    uint32_t a = p + 14 + 1 + (uint32_t)t.size();
    for (auto &i : list) {
        write_pstr(a, i.name);
        a += 1 + (uint32_t)i.name.size();
        st8(a, i.icon); st8(a + 1, i.key); st8(a + 2, i.mark); st8(a + 3, i.style);
        a += 4;
    }
    st8(a, 0);
}

bool enabled(uint32_t mh, int item) {
    uint32_t f = ld32(ld32(mh) + 10);
    if (!(f & 1)) return false;
    return item > 31 || ((f >> item) & 1);
}

static std::vector<uint32_t> g_bar;        /* menu bar, left to right */
static std::vector<uint32_t> g_hier;       /* hierarchical / pop-up menus */

uint32_t by_id(int16_t id) {
    for (uint32_t m : g_bar) if ((int16_t)ld16(ld32(m)) == id) return m;
    for (uint32_t m : g_hier) if ((int16_t)ld16(ld32(m)) == id) return m;
    return 0;
}

static void calc_size(uint32_t mh, uint32_t port) {
    auto list = items(mh);
    int w = 40;
    for (auto &i : list) w = std::max(w, text_width(port, (const uint8_t *)i.name.data(), (int)i.name.size()) + 40);
    st16(ld32(mh) + 2, (uint16_t)w);
    st16(ld32(mh) + 4, (uint16_t)(list.size() * 16));
}

/* ---------- drawing ---------- */
static uint32_t draw_port() { return wm::wmgr_port(); }

struct Saved { Rect r; std::vector<uint8_t> px; };
static Saved save_bits(const Rect &r0) {
    Screen sc = screen();
    Rect r = rect_sect(r0, {0, 0, (int16_t)sc.height, (int16_t)sc.width});
    Saved s{r, {}};
    int bpp = sc.depth / 8;
    for (int y = r.top; y < r.bottom; y++)
        s.px.insert(s.px.end(), g_mem + sc.base + y * sc.row_bytes + r.left * bpp, g_mem + sc.base + y * sc.row_bytes + r.right * bpp);
    return s;
}
static void restore_bits(const Saved &s) {
    Screen sc = screen();
    int bpp = sc.depth / 8, w = (s.r.right - s.r.left) * bpp;
    for (int y = s.r.top; y < s.r.bottom; y++)
        memcpy(g_mem + sc.base + y * sc.row_bytes + s.r.left * bpp, s.px.data() + (size_t)(y - s.r.top) * w, w);
}

static void draw_list(uint32_t mh, const Rect &r, int hilite) {
    uint32_t p = draw_port();
    uint32_t saved = current_port();
    set_current_port(p);
    st16(p + port::txFont, 0); st16(p + port::txSize, 12); st8(p + port::txFace, 0); st16(p + port::txMode, 1);
    fill_rect_solid(p, r, {0xFFFF, 0xFFFF, 0xFFFF});
    RGB black{0, 0, 0}, gray{0x8888, 0x8888, 0x8888}, white{0xFFFF, 0xFFFF, 0xFFFF};
    fill_rect_solid(p, {r.top, r.left, (int16_t)(r.top + 1), r.right}, black);
    fill_rect_solid(p, {(int16_t)(r.bottom - 1), r.left, r.bottom, r.right}, black);
    fill_rect_solid(p, {r.top, r.left, r.bottom, (int16_t)(r.left + 1)}, black);
    fill_rect_solid(p, {r.top, (int16_t)(r.right - 1), r.bottom, r.right}, black);
    fill_rect_solid(p, {(int16_t)(r.top + 2), r.right, (int16_t)(r.bottom + 1), (int16_t)(r.right + 1)}, black);
    fill_rect_solid(p, {r.bottom, (int16_t)(r.left + 2), (int16_t)(r.bottom + 1), (int16_t)(r.right + 1)}, black);
    auto list = items(mh);
    for (size_t i = 0; i < list.size(); i++) {
        int y = r.top + 1 + (int)i * 16;
        bool sep = list[i].name == "-";
        bool on = enabled(mh, (int)i + 1);
        Rect ir{(int16_t)y, (int16_t)(r.left + 1), (int16_t)(y + 16), (int16_t)(r.right - 1)};
        bool hl = (int)i + 1 == hilite && on && !sep;
        if (hl) fill_rect_solid(p, ir, black);
        if (sep) { fill_rect_solid(p, {(int16_t)(y + 7), ir.left, (int16_t)(y + 8), ir.right}, gray); continue; }
        RGB fg = hl ? white : on ? black : gray;
        st16(p + port::rgbFgColor, fg.r); st16(p + port::rgbFgColor + 2, fg.g); st16(p + port::rgbFgColor + 4, fg.b);
        if (list[i].mark) {
            uint8_t m = list[i].mark;
            st16(p + port::pnLoc, (uint16_t)(y + 12)); st16(p + port::pnLoc + 2, (uint16_t)(r.left + 4));
            draw_text(p, &m, 1);
        }
        st16(p + port::pnLoc, (uint16_t)(y + 12)); st16(p + port::pnLoc + 2, (uint16_t)(r.left + 16));
        draw_text(p, (const uint8_t *)list[i].name.data(), (int)list[i].name.size());
        if (list[i].key && list[i].key < 0x1B) {} else if (list[i].key) {
            uint8_t cmd[2] = {0x11, list[i].key};   /* Command glyph in Chicago */
            st16(p + port::pnLoc + 2, (uint16_t)(r.right - 28));
            draw_text(p, cmd, 2);
        }
    }
    st16(p + port::rgbFgColor, 0); st16(p + port::rgbFgColor + 2, 0); st16(p + port::rgbFgColor + 4, 0);
    set_current_port(saved);
}

/* track a menu list at r; returns item (1-based) or 0 */
static int track_list(uint32_t mh, const Rect &r, int initial, bool release_selects) {
    int n = (int)items(mh).size();
    int cur = initial;
    draw_list(mh, r, cur);
    host::present(true);
    bool seen_down = host::mouse_down();
    for (;;) {
        host::pump();
        int16_t v, h;
        host::mouse_global(v, h);
        int sel = 0;
        if (h >= r.left && h < r.right && v > r.top && v < r.bottom - 1) sel = (v - r.top - 1) / 16 + 1;
        if (sel > n) sel = 0;
        if (sel != cur) { cur = sel; draw_list(mh, r, cur); host::present(true); }
        bool down = host::mouse_down();
        if (down) seen_down = true;
        if (!down && (seen_down || release_selects)) break;
        Sleep(5);
    }
    host::flush_events(0xFFFF & ~(1 << 3 | 1 << 4));
    if (cur && !enabled(mh, cur)) cur = 0;
    if (cur && items(mh)[cur - 1].name == "-") cur = 0;
    return cur;
}

int popup_select(uint32_t mh, int top, int left, int current) {
    calc_size(mh, draw_port());
    int w = (int16_t)ld16(ld32(mh) + 2), hgt = (int16_t)ld16(ld32(mh) + 4) + 2;
    int y = top - (current > 0 ? (current - 1) * 16 : 0) - 1;
    y = std::max(20, std::min(y, 480 - hgt - 2));
    Rect r{(int16_t)y, (int16_t)left, (int16_t)(y + hgt), (int16_t)(left + w)};
    Saved s = save_bits({r.top, r.left, (int16_t)(r.bottom + 1), (int16_t)(r.right + 1)});
    int sel = track_list(mh, r, current, true);
    restore_bits(s);
    host::present(true);
    return sel;
}

std::vector<std::string> item_names(uint32_t mh) {
    std::vector<std::string> out;
    for (auto &i : items(mh)) out.push_back(i.name);
    return out;
}

void insert_hier(uint32_t mh) {
    if (std::find(g_hier.begin(), g_hier.end(), mh) == g_hier.end()) g_hier.push_back(mh);
}

void set_mark(uint32_t mh, int item, uint8_t mark) {
    auto list = items(mh);
    if (item < 1 || item > (int)list.size()) return;
    list[item - 1].mark = mark;
    write_items(mh, list);
}

static void parse_append(uint32_t mh, const std::string &data) {
    auto list = items(mh);
    std::string cur;
    auto flush = [&]() {
        Item it{"", 0, 0, 0, 0};
        bool disabled = false;
        std::string name;
        for (size_t i = 0; i < cur.size(); i++) {
            char ch = cur[i];
            if (ch == '^' && i + 1 < cur.size()) it.icon = (uint8_t)(cur[++i] - '0');
            else if (ch == '!' && i + 1 < cur.size()) it.mark = (uint8_t)cur[++i];
            else if (ch == '<' && i + 1 < cur.size()) { char s = cur[++i]; it.style |= s == 'B' ? 1 : s == 'I' ? 2 : s == 'U' ? 4 : s == 'O' ? 8 : s == 'S' ? 16 : 0; }
            else if (ch == '/' && i + 1 < cur.size()) it.key = (uint8_t)cur[++i];
            else if (ch == '(') disabled = true;
            else name += ch;
        }
        it.name = name;
        list.push_back(it);
        int idx = (int)list.size();
        if (idx <= 31) {
            uint32_t f = ld32(ld32(mh) + 10);
            f = disabled || name == "-" ? f & ~(1u << idx) : f | (1u << idx);
            st32(ld32(mh) + 10, f);
        }
        cur.clear();
    };
    for (char ch : data) {
        if (ch == ';' || ch == '\r') flush();
        else cur += ch;
    }
    if (!cur.empty()) flush();
    write_items(mh, list);
}

}  // namespace pt::menu


/* ======================================================================== */
using namespace pt;
using namespace pt::qd;
using namespace pt::menu;

HLE(InitMenus) { (void)c; }

HLE(GetMenu) {
    uint32_t h = res::get_handle('MENU', (int16_t)ARG(0));
    if (h) st32(ld32(h) + 10, ld32(ld32(h) + 10));   /* already in MenuInfo format */
    RET(h);
}
HLE(NewMenu) {
    std::string t = read_pstr(ARG(1));
    uint32_t h = handle_new(14 + 1 + (uint32_t)t.size() + 1, true);
    uint32_t p = ld32(h);
    st16(p, (uint16_t)ARG(0));
    st32(p + 10, 0xFFFFFFFF);
    write_pstr(p + 14, t);
    RET(h);
}
HLE(AppendMenu) { parse_append(ARG(0), read_pstr(ARG(1))); }
HLE(AppendResMenu) {
    /* fonts or desk accessories: list resource names of that type */
    uint32_t mh = ARG(0), type = ARG(1);
    std::string data;
    if (type == 'FONT' || type == 'FOND') data = "Chicago;Geneva;Monaco;New York;Helvetica;Times;Courier";
    if (!data.empty()) parse_append(mh, data);
}
HLE(InsertMenu) {
    uint32_t mh = ARG(0);
    int16_t before = (int16_t)ARG(1);
    auto &list = before == -1 ? g_hier : g_bar;
    if (std::find(list.begin(), list.end(), mh) != list.end()) return;
    if (before > 0) {
        for (auto it = list.begin(); it != list.end(); ++it)
            if ((int16_t)ld16(ld32(*it)) == before) { list.insert(it, mh); return; }
    }
    list.push_back(mh);
}
HLE(GetMenuHandle) { RET(by_id((int16_t)ARG(0))); }
HLE(DeleteMenuItem) {
    auto list = items(ARG(0));
    int i = (int16_t)ARG(1);
    if (i < 1 || i > (int)list.size()) return;
    list.erase(list.begin() + (i - 1));
    uint32_t f = ld32(ld32(ARG(0)) + 10);
    uint32_t low = f & ((1u << i) - 1), high = (f >> (i + 1)) << i;
    st32(ld32(ARG(0)) + 10, low | high);
    write_items(ARG(0), list);
}
HLE(CountMItems) { RET((uint32_t)items(ARG(0)).size()); }
HLE(GetMenuItemText) {
    auto list = items(ARG(0));
    int i = (int16_t)ARG(1);
    write_pstr(ARG(2), i >= 1 && i <= (int)list.size() ? list[i - 1].name : std::string());
}
HLE(SetMenuItemText) {
    auto list = items(ARG(0));
    int i = (int16_t)ARG(1);
    if (i < 1 || i > (int)list.size()) return;
    list[i - 1].name = read_pstr(ARG(2));
    write_items(ARG(0), list);
}
HLE(EnableItem) {
    uint32_t a = ld32(ARG(0)) + 10;
    int i = (int16_t)ARG(1);
    if (i <= 31) st32(a, ld32(a) | (1u << i));
}
HLE(DisableItem) {
    uint32_t a = ld32(ARG(0)) + 10;
    int i = (int16_t)ARG(1);
    if (i <= 31) st32(a, ld32(a) & ~(1u << i));
}
HLE(CheckItem) { set_mark(ARG(0), (int16_t)ARG(1), (ARG(2) & 0xFF) ? 0x12 : 0); }

static int16_t g_hilite = 0;
static void draw_bar() {
    int mbar = (int16_t)ld16(0x0BAA);
    if (mbar <= 0) return;
    uint32_t p = wm::wmgr_port();
    uint32_t saved = current_port();
    set_current_port(p);
    fill_rect_solid(p, {0, 0, (int16_t)(mbar - 1), 640}, {0xFFFF, 0xFFFF, 0xFFFF});
    fill_rect_solid(p, {(int16_t)(mbar - 1), 0, (int16_t)mbar, 640}, {0, 0, 0});
    st16(p + port::txFont, 0); st16(p + port::txSize, 12); st8(p + port::txFace, 0); st16(p + port::txMode, 1);
    int x = 10;
    for (uint32_t m : g_bar) {
        std::string t = read_pstr(ld32(m) + 14);
        int w = text_width(p, (const uint8_t *)t.data(), (int)t.size());
        bool hl = (int16_t)ld16(ld32(m)) == g_hilite;
        if (hl) fill_rect_solid(p, {0, (int16_t)(x - 6), (int16_t)(mbar - 1), (int16_t)(x + w + 6)}, {0, 0, 0});
        RGB fg = hl ? RGB{0xFFFF, 0xFFFF, 0xFFFF} : RGB{0, 0, 0};
        st16(p + port::rgbFgColor, fg.r); st16(p + port::rgbFgColor + 2, fg.g); st16(p + port::rgbFgColor + 4, fg.b);
        st16(p + port::pnLoc, 14); st16(p + port::pnLoc + 2, (uint16_t)x);
        draw_text(p, (const uint8_t *)t.data(), (int)t.size());
        x += w + 14;
    }
    st16(p + port::rgbFgColor, 0); st16(p + port::rgbFgColor + 2, 0); st16(p + port::rgbFgColor + 4, 0);
    set_current_port(saved);
}
HLE(DrawMenuBar) { draw_bar(); }
HLE(HiliteMenu) { g_hilite = (int16_t)ARG(0); draw_bar(); }

HLE(MenuSelect) {
    /* long MenuSelect(Point startPt): pull down the title under the mouse, track, return (menu << 16) | item */
    int mbar = (int16_t)ld16(0x0BAA);
    if (mbar <= 0 || g_bar.empty()) { RET(0); return; }
    uint32_t p = wm::wmgr_port();
    int16_t h = (int16_t)ARG(0);
    int x = 10;
    for (uint32_t m : g_bar) {
        std::string t = read_pstr(ld32(m) + 14);
        int w = text_width(p, (const uint8_t *)t.data(), (int)t.size());
        if (h >= x - 6 && h < x + w + 8) {
            g_hilite = (int16_t)ld16(ld32(m));
            draw_bar();
            calc_size(m, p);
            int mw = (int16_t)ld16(ld32(m) + 2), mh = (int16_t)ld16(ld32(m) + 4) + 2;
            Rect r{(int16_t)mbar, (int16_t)(x - 6), (int16_t)(mbar + mh), (int16_t)(x - 6 + mw)};
            auto saved = save_bits({r.top, r.left, (int16_t)(r.bottom + 1), (int16_t)(r.right + 1)});
            int item = track_list(m, r, 0, false);
            restore_bits(saved);
            RET(item ? ((uint32_t)(uint16_t)ld16(ld32(m)) << 16) | (uint32_t)item : 0);
            if (!item) { g_hilite = 0; draw_bar(); }
            return;
        }
        x += w + 14;
    }
    RET(0);
}

HLE(MenuKey) {
    uint8_t ch = (uint8_t)toupper((int)(ARG(0) & 0xFF));
    for (uint32_t m : g_bar) {
        auto list = items(m);
        for (size_t i = 0; i < list.size(); i++)
            if (toupper(list[i].key) == ch && enabled(m, (int)i + 1)) {
                g_hilite = (int16_t)ld16(ld32(m));
                draw_bar();
                RET(((uint32_t)(uint16_t)g_hilite << 16) | (uint32_t)(i + 1));
                return;
            }
    }
    RET(0);
}
