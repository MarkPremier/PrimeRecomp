/* Dialog Manager, Control Manager, a minimal TextEdit and List Manager.
 *
 * Dialog items live in the guest DITL handle exactly as on a Mac:
 *   count-1 (2), then per item: handle(4) rect(8) type(1) len(1) data(len, padded even)
 * so GetDialogItem/SetDialogItem see real records. Standard items are drawn
 * natively in System 7 style; user items call back into the game.
 */
#include <windows.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "pt/host.h"
#include "pt/qd.h"
#include "pt/resources.h"
#include "pt/runtime.h"
#include "pt/ui.h"

namespace pt::dlg {

using namespace pt::qd;

/* ---------- ControlRecord ---------- */
namespace cr {
enum : uint32_t {
    nextControl = 0, contrlOwner = 4, contrlRect = 8, contrlVis = 16, contrlHilite = 17, contrlValue = 18,
    contrlMin = 20, contrlMax = 22, contrlDefProc = 24, contrlData = 28, contrlAction = 32, contrlRfCon = 36,
    contrlTitle = 40, size = 296,
};
}
enum { kWinControlList = 140, kDlgItems = 156, kDlgTextH = 160, kDlgEditField = 164, kDlgEditOpen = 166, kDlgDefItem = 168,
       kDialogSize = 170 };

static std::string g_param[4];

/* ---------- the port's own settings in Preferences > Graphics ----------
 * DITL 4001 (the Graphics pane, appended by the game's handle_switch) gains checkboxes:
 * "Smooth Motion" beside "Choose Monitor..." and "Widescreen" beside "Hardware Acceleration".
 * The game never sees them: clicks on them are handled here, and each choice is applied on OK
 * (item 1 of the Preferences dialog) and dropped on Cancel. */
struct PortCheck {
    const char *label;
    int16_t top, left, bottom, right;   /* in the pane, as the DITL item */
    bool (*get)();
    void (*set)(bool);
    uint32_t dlg = 0;
    int item = 0;
    int pending = -1;
};
static PortCheck g_checks[] = {
    {"Smooth Motion", 206, 172, 224, 312, interpolation_enabled, set_interpolation},
    {"Widescreen", 178, 205, 196, 310, widescreen_enabled, set_widescreen},
};

static PortCheck *check_labelled(const std::string &label) {
    for (auto &k : g_checks) if (label == k.label) return &k;
    return nullptr;
}

void patch_ditl(int16_t id, std::vector<uint8_t> &d) {
    static const char anchor[] = "Hardware Acceleration";
    if (id != 4001 || d.size() < 2 ||
        std::search(d.begin(), d.end(), anchor, anchor + sizeof anchor - 1) == d.end())
        return;
    int n = (int16_t)(d[0] << 8 | d[1]);
    auto be = [&](int v) { d.push_back((uint8_t)(v >> 8)); d.push_back((uint8_t)v); };
    for (auto &k : g_checks) {
        for (int i = 0; i < 4; i++) d.push_back(0);   /* item handle */
        be(k.top); be(k.left); be(k.bottom); be(k.right);
        d.push_back(5);                               /* enabled checkbox */
        size_t len = strlen(k.label);
        d.push_back((uint8_t)len);
        d.insert(d.end(), k.label, k.label + len);
        if (d.size() & 1) d.push_back(0);
        n++;
    }
    d[0] = (uint8_t)(n >> 8);                         /* count - 1 */
    d[1] = (uint8_t)n;
}

static int ctl_proc(uint32_t ch) { return (int)(ld32(ld32(ch) + cr::contrlDefProc) & 0xFFFF); }
static bool is_popup(uint32_t ch) { return (ctl_proc(ch) & ~15) == 1008; }
static int16_t v16(uint32_t a) { return (int16_t)ld16(a); }

/* ---------- drawing helpers (current port = owner window) ---------- */
static void set_fg(uint32_t p, RGB c) { st16(p + port::rgbFgColor, c.r); st16(p + port::rgbFgColor + 2, c.g); st16(p + port::rgbFgColor + 4, c.b); }
static RGB get_bg(uint32_t p) { return port_bk(p); }
static void frame(uint32_t p, const Rect &r, RGB c, int t = 1) {
    fill_rect_solid(p, {r.top, r.left, (int16_t)(r.top + t), r.right}, c);
    fill_rect_solid(p, {(int16_t)(r.bottom - t), r.left, r.bottom, r.right}, c);
    fill_rect_solid(p, {r.top, r.left, r.bottom, (int16_t)(r.left + t)}, c);
    fill_rect_solid(p, {r.top, (int16_t)(r.right - t), r.bottom, r.right}, c);
}
static void round_frame(uint32_t p, const Rect &r, RGB col, int rad, int t) {
    Surface s = surface_of_port(p);
    Rect clip = rect_sect(port_clip(p), s.bounds);
    uint32_t v = rgb_to_pixel(s, col);
    double rr = rad / 2.0;
    for (int y = r.top; y < r.bottom; y++)
        for (int x = r.left; x < r.right; x++) {
            double dx = 0, dy = 0;
            if (x < r.left + rr) dx = r.left + rr - x - 0.5; else if (x >= r.right - rr) dx = x + 0.5 - (r.right - rr);
            if (y < r.top + rr) dy = r.top + rr - y - 0.5; else if (y >= r.bottom - rr) dy = y + 0.5 - (r.bottom - rr);
            double d = std::sqrt(dx * dx + dy * dy);
            bool corner = dx > 0 && dy > 0;
            bool edge = corner ? (d <= rr && d > rr - t) : (x < r.left + t || x >= r.right - t || y < r.top + t || y >= r.bottom - t);
            if (edge && x >= clip.left && x < clip.right && y >= clip.top && y < clip.bottom)
                put_pixel(s, x - s.bounds.left, y - s.bounds.top, v);
        }
}
static void text_at(uint32_t p, int h, int v, const std::string &t) {
    st16(p + port::pnLoc, (uint16_t)v);
    st16(p + port::pnLoc + 2, (uint16_t)h);
    draw_text(p, (const uint8_t *)t.data(), (int)t.size());
}
static void sys_font(uint32_t p) { st16(p + port::txFont, 0); st16(p + port::txSize, 12); st8(p + port::txFace, 0); st16(p + port::txMode, 1); }

static std::string substitute(const std::string &s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '^' && i + 1 < s.size() && s[i + 1] >= '0' && s[i + 1] <= '3') { out += g_param[s[i + 1] - '0']; i++; }
        else out += s[i];
    }
    return out;
}

/* ---------- controls ---------- */
void draw_control(uint32_t ch) {
    uint32_t c = ld32(ch);
    if (!ld8(c + cr::contrlVis)) return;
    uint32_t p = ld32(c + cr::contrlOwner);
    uint32_t saved = current_port();
    set_current_port(p);
    sys_font(p);
    Rect r = read_rect(c + cr::contrlRect);
    std::string title = read_pstr(c + cr::contrlTitle);
    int hil = ld8(c + cr::contrlHilite);
    bool inactive = hil == 255;
    RGB black{0, 0, 0}, gray{0x8888, 0x8888, 0x8888}, white{0xFFFF, 0xFFFF, 0xFFFF};
    RGB fg = inactive ? gray : black;
    int proc = ctl_proc(ch) & ~7 ? ctl_proc(ch) : ctl_proc(ch);
    int a, d, w, l;
    font_info(p, a, d, w, l);
    if (is_popup(ch)) {
        uint32_t priv = ld32(c + cr::contrlData);
        uint32_t mh = priv && ld32(priv) ? ld32(ld32(priv)) : 0;
        int title_w = priv && ld32(priv) ? v16(ld32(priv) + 6) : 0;
        fill_rect_solid(p, r, get_bg(p));
        set_fg(p, fg);
        if (!title.empty()) text_at(p, r.left, (r.top + r.bottom + a - d) / 2, title);
        Rect box{(int16_t)(r.top), (int16_t)(r.left + title_w + 2), (int16_t)(r.bottom - 1), (int16_t)(r.right - 1)};
        fill_rect_solid(p, box, white);
        frame(p, box, fg);
        fill_rect_solid(p, {(int16_t)(box.top + 2), box.right, (int16_t)(box.bottom + 1), (int16_t)(box.right + 1)}, fg);
        fill_rect_solid(p, {box.bottom, (int16_t)(box.left + 2), (int16_t)(box.bottom + 1), (int16_t)(box.right + 1)}, fg);
        int val = v16(c + cr::contrlValue);
        auto names = mh ? menu::item_names(mh) : std::vector<std::string>{};
        std::string cur = val >= 1 && val <= (int)names.size() ? names[val - 1] : std::string();
        text_at(p, box.left + 6, (box.top + box.bottom + a - d) / 2, cur);
        int tx = box.right - 16, ty = (box.top + box.bottom) / 2 - 2;
        for (int i = 0; i < 6; i++) fill_rect_solid(p, {(int16_t)(ty + i), (int16_t)(tx + i), (int16_t)(ty + i + 1), (int16_t)(tx + 11 - i)}, fg);
    } else if (proc == 0) {                 /* push button */
        fill_rect_solid(p, r, white);
        round_frame(p, r, fg, 16, 1);
        int tw = text_width(p, (const uint8_t *)title.data(), (int)title.size());
        if (hil && !inactive) {
            fill_rect_solid(p, {(int16_t)(r.top + 2), (int16_t)(r.left + 4), (int16_t)(r.bottom - 2), (int16_t)(r.right - 4)}, black);
            set_fg(p, white);
        } else set_fg(p, fg);
        text_at(p, (r.left + r.right - tw) / 2, (r.top + r.bottom + a - d) / 2, title);
    } else if (proc == 1 || proc == 2) {    /* check box / radio */
        fill_rect_solid(p, r, get_bg(p));
        int my = (r.top + r.bottom) / 2;
        Rect b{(int16_t)(my - 6), (int16_t)(r.left + 2), (int16_t)(my + 6), (int16_t)(r.left + 14)};
        fill_rect_solid(p, b, white);
        if (proc == 1) {
            frame(p, b, fg);
            if (v16(c + cr::contrlValue))
                for (int i = 1; i < 11; i++) {
                    fill_rect_solid(p, {(int16_t)(b.top + i), (int16_t)(b.left + i), (int16_t)(b.top + i + 1), (int16_t)(b.left + i + 1)}, fg);
                    fill_rect_solid(p, {(int16_t)(b.top + i), (int16_t)(b.right - 1 - i), (int16_t)(b.top + i + 1), (int16_t)(b.right - i)}, fg);
                }
        } else {
            round_frame(p, b, fg, 12, 1);
            if (v16(c + cr::contrlValue)) {
                Rect dot{(int16_t)(b.top + 3), (int16_t)(b.left + 3), (int16_t)(b.bottom - 3), (int16_t)(b.right - 3)};
                round_frame(p, dot, fg, 6, 3);
            }
        }
        set_fg(p, fg);
        text_at(p, r.left + 18, my + (a - d) / 2, title);
    } else {                                 /* scroll bar and others: a plain track */
        fill_rect_solid(p, r, {0xDDDD, 0xDDDD, 0xDDDD});
        frame(p, r, black);
    }
    set_fg(p, black);
    set_current_port(saved);
}

uint32_t new_control(uint32_t win, const Rect &r, const std::string &title, bool vis, int value, int min, int max, int proc,
                     uint32_t refcon) {
    uint32_t ch = handle_new(cr::size, true);
    uint32_t c = ld32(ch);
    st32(c + cr::contrlOwner, win);
    write_rect(c + cr::contrlRect, r);
    st8(c + cr::contrlVis, vis);
    st16(c + cr::contrlValue, (uint16_t)value);
    st16(c + cr::contrlMin, (uint16_t)min);
    st16(c + cr::contrlMax, (uint16_t)max);
    st32(c + cr::contrlDefProc, (uint32_t)proc);
    st32(c + cr::contrlRfCon, refcon);
    write_pstr(c + cr::contrlTitle, title);
    if ((proc & ~15) == 1008) {
        /* pop-up CDEF: min = menu ID, value = title width on input */
        uint32_t mh = res::get_handle('MENU', (int16_t)min);
        /* pop-up CDEF inputs: value = title style, min = MENU id, max = title width */
        uint32_t priv = handle_new(8, true);
        st32(ld32(priv), mh);
        st16(ld32(priv) + 4, (uint16_t)min);
        st16(ld32(priv) + 6, (uint16_t)max);
        st32(c + cr::contrlData, priv);
        int n = mh ? (int)menu::item_names(mh).size() : 0;
        st16(c + cr::contrlMin, 1);
        st16(c + cr::contrlMax, (uint16_t)n);
        st16(c + cr::contrlValue, 1);
        if (mh) menu::insert_hier(mh);   /* reachable through GetMenuHandle */
    }
    /* link into the window's control list */
    st32(c + cr::nextControl, ld32(win + kWinControlList));
    st32(win + kWinControlList, ch);
    return ch;
}

void dispose_control(uint32_t ch) {
    uint32_t c = ld32(ch);
    uint32_t win = ld32(c + cr::contrlOwner);
    uint32_t link = win + kWinControlList;
    while (ld32(link)) {
        if (ld32(link) == ch) { st32(link, ld32(c + cr::nextControl)); break; }
        link = ld32(ld32(link)) + cr::nextControl;
    }
    handle_dispose(ch);
}

static int track_control(uint32_t ch, int v, int h) {
    uint32_t c = ld32(ch);
    if (ld8(c + cr::contrlHilite) == 255) return 0;
    Rect r = read_rect(c + cr::contrlRect);
    if (is_popup(ch)) {
        uint32_t priv = ld32(c + cr::contrlData);
        uint32_t mh = ld32(ld32(priv));
        int title_w = v16(ld32(priv) + 6);
        uint32_t win = ld32(c + cr::contrlOwner);
        Surface s = surface_of_port(win);
        int gtop = r.top - s.bounds.top, gleft = r.left + title_w + 2 - s.bounds.left;
        int sel = menu::popup_select(mh, gtop, gleft, v16(c + cr::contrlValue));
        if (sel) { st16(c + cr::contrlValue, (uint16_t)sel); draw_control(ch); }
        return sel ? 1 : 0;
    }
    int proc = ctl_proc(ch);
    if (proc == 0) {
        /* highlight while the mouse is inside */
        bool inside = true;
        st8(c + cr::contrlHilite, 10);
        draw_control(ch);
        host::present(true);
        while (host::mouse_down()) {
            host::pump();
            int16_t gv, gh;
            host::mouse_global(gv, gh);
            Surface s = surface_of_port(ld32(c + cr::contrlOwner));
            int lv = gv + s.bounds.top, lh = gh + s.bounds.left;
            bool now = lv >= r.top && lv < r.bottom && lh >= r.left && lh < r.right;
            if (now != inside) { inside = now; st8(c + cr::contrlHilite, now ? 10 : 0); draw_control(ch); host::present(true); }
            Sleep(5);
        }
        st8(c + cr::contrlHilite, 0);
        draw_control(ch);
        return inside ? 10 : 0;
    }
    (void)v; (void)h;
    while (host::mouse_down()) { host::pump(); Sleep(5); }
    return proc == 1 ? 11 : proc == 2 ? 11 : 129;
}

/* ---------- dialog items ---------- */
struct ItemRef { uint32_t addr; uint32_t handle; Rect rect; uint8_t type; uint8_t len; uint32_t data; };

static std::vector<ItemRef> items(uint32_t dlg) {
    std::vector<ItemRef> out;
    uint32_t ih = ld32(dlg + kDlgItems);
    if (!ih || !ld32(ih)) return out;
    uint32_t p = ld32(ih);
    int n = v16(p) + 1;
    uint32_t a = p + 2;
    for (int i = 0; i < n; i++) {
        ItemRef it{a, ld32(a), read_rect(a + 4), ld8(a + 12), ld8(a + 13), a + 14};
        out.push_back(it);
        a += 14 + ((it.len + 1) & ~1u);
    }
    return out;
}

static bool hidden(const Rect &r) { return r.left >= 8192; }

static std::map<uint32_t, int> g_sel_start, g_sel_end;   /* per-dialog edit selection */

static std::string handle_text(uint32_t h) {
    if (!h || !ld32(h)) return {};
    return std::string((const char *)g_mem + ld32(h), handle_size(h));
}
static void set_handle_text(uint32_t h, const std::string &t) {
    handle_set_size(h, (uint32_t)t.size());
    if (!t.empty()) memcpy(g_mem + ld32(h), t.data(), t.size());
}

static void plot_icon(uint32_t p, const Rect &r, int16_t id) {
    std::vector<uint8_t> d;
    if (res::get_bytes('ICON', id, d) && d.size() >= 128) {
        for (int y = 0; y < 32; y++)
            for (int x = 0; x < 32; x++)
                if ((d[y * 4 + (x >> 3)] >> (7 - (x & 7))) & 1)
                    fill_rect_solid(p, {(int16_t)(r.top + y), (int16_t)(r.left + x), (int16_t)(r.top + y + 1), (int16_t)(r.left + x + 1)}, {0, 0, 0});
    }
}

static void draw_item(uint32_t dlg, int index, const ItemRef &it) {
    if (hidden(it.rect)) return;
    uint32_t p = dlg;
    int type = it.type & 0x7F;
    sys_font(p);
    set_fg(p, {0, 0, 0});
    if (type >= 4 && type <= 7) { if (it.handle) draw_control(it.handle); return; }
    switch (type) {
    case 0:    /* user item */
        if (it.handle) { uint32_t args[2] = {dlg, (uint32_t)index}; guest_call(current_ctx(), it.handle, args, 2); }
        break;
    case 8: {  /* static text */
        std::string t = substitute(handle_text(it.handle));
        fill_rect_solid(p, it.rect, port_bk(p));
        uint32_t tmp = guest_alloc((uint32_t)t.size() + 1, false);
        memcpy(g_mem + tmp, t.data(), t.size());
        te_text_box(p, tmp, (int)t.size(), it.rect, 0);
        guest_free(tmp);
        break;
    }
    case 16: { /* edit text */
        Rect f{(int16_t)(it.rect.top - 3), (int16_t)(it.rect.left - 3), (int16_t)(it.rect.bottom + 3), (int16_t)(it.rect.right + 3)};
        fill_rect_solid(p, f, {0xFFFF, 0xFFFF, 0xFFFF});
        frame(p, f, {0, 0, 0});
        std::string t = handle_text(it.handle);
        int a, d, w, l;
        font_info(p, a, d, w, l);
        text_at(p, it.rect.left, it.rect.top + a, t);
        if (v16(dlg + kDlgEditField) == index - 1) {
            int cx = it.rect.left + text_width(p, (const uint8_t *)t.data(), g_sel_start[dlg] < (int)t.size() ? g_sel_start[dlg] : (int)t.size());
            fill_rect_solid(p, {it.rect.top, (int16_t)cx, (int16_t)(it.rect.top + a + d), (int16_t)(cx + 1)}, {0, 0, 0});
        }
        break;
    }
    case 32: plot_icon(p, it.rect, (int16_t)(it.len >= 2 ? (ld8(it.data) << 8 | ld8(it.data + 1)) : 0)); break;
    case 64: if (it.handle) draw_picture(it.handle, it.rect); break;
    }
}

void draw_dialog(uint32_t dlg) {
    /* like the real DrawDialog: draws items only; the Window Manager erased exposed areas */
    uint32_t saved = current_port();
    set_current_port(dlg);
    auto list = items(dlg);
    for (size_t i = 0; i < list.size(); i++) draw_item(dlg, (int)i + 1, list[i]);
    set_current_port(saved);
}

static void init_items(uint32_t dlg) {
    auto list = items(dlg);
    for (auto &it : list) {
        int type = it.type & 0x7F;
        uint32_t h = 0;
        std::string data((const char *)g_mem + it.data, it.len);
        if (type == 8 || type == 16) {
            h = handle_new((uint32_t)data.size(), false);
            if (!data.empty()) memcpy(g_mem + ld32(h), data.data(), data.size());
        } else if (type >= 4 && type <= 6) {
            h = new_control(dlg, it.rect, data, true, 0, 0, 1, type - 4, 0);
        } else if (type == 7) {
            int16_t id = (int16_t)(ld8(it.data) << 8 | ld8(it.data + 1));
            std::vector<uint8_t> cn;
            if (res::get_bytes('CNTL', id, cn) && cn.size() >= 22) {
                auto b16 = [&](int o) { return (int16_t)(cn[o] << 8 | cn[o + 1]); };
                std::string title((const char *)&cn[23], cn[22]);
                h = new_control(dlg, it.rect, title, b16(10) != 0, b16(8), b16(14), b16(12), b16(16),
                                (uint32_t)(cn[18] << 24 | cn[19] << 16 | cn[20] << 8 | cn[21]));
            }
        } else if (type == 32) {
            h = res::get_handle('ICON', (int16_t)(ld8(it.data) << 8 | ld8(it.data + 1)));
        } else if (type == 64) {
            h = res::get_handle('PICT', (int16_t)(ld8(it.data) << 8 | ld8(it.data + 1)));
        }
        st32(it.addr, h);
    }
}

static uint32_t copy_ditl(int16_t id) {
    std::vector<uint8_t> d;
    if (!res::get_bytes('DITL', id, d)) return 0;
    uint32_t h = handle_new((uint32_t)d.size(), false);
    memcpy(g_mem + ld32(h), d.data(), d.size());
    return h;
}

static void apply_dctb(uint32_t dlg, int16_t id) {
    std::vector<uint8_t> d;
    if (!res::get_bytes('dctb', id, d) || d.size() < 16) return;
    int n = (int16_t)(d[6] << 8 | d[7]) + 1;
    for (int i = 0; i < n && 8 + 8 * i + 8 <= (int)d.size(); i++) {
        int part = d[8 + 8 * i] << 8 | d[9 + 8 * i];
        uint16_t r = (uint16_t)(d[10 + 8 * i] << 8 | d[11 + 8 * i]), g = (uint16_t)(d[12 + 8 * i] << 8 | d[13 + 8 * i]),
                 b = (uint16_t)(d[14 + 8 * i] << 8 | d[15 + 8 * i]);
        if (part == 0) { st16(dlg + port::rgbBkColor, r); st16(dlg + port::rgbBkColor + 2, g); st16(dlg + port::rgbBkColor + 4, b); }
        if (part == 2) { st16(dlg + port::rgbFgColor, r); st16(dlg + port::rgbFgColor + 2, g); st16(dlg + port::rgbFgColor + 4, b); }
    }
}

static Rect position(Rect b, uint16_t pos) {
    /* alertPositionMainScreen etc.: centre horizontally, a third down */
    int w = b.right - b.left, h = b.bottom - b.top;
    if (pos == 0xA80A || pos == 0x300A || pos == 0x700A || pos == 0xB00A || pos == 0xA00A || pos == 0x280A || pos == 0x680A) {
        int left = (640 - w) / 2, top = pos == 0x280A || pos == 0x680A ? (480 - h) / 2 : 20 + (460 - h) / 3;
        b = {(int16_t)top, (int16_t)left, (int16_t)(top + h), (int16_t)(left + w)};
    }
    return b;
}

uint32_t new_dialog(uint32_t storage, const Rect &bounds, const std::string &title, bool vis, int proc, uint32_t behind,
                    bool goaway, uint32_t refcon, uint32_t items_h, int16_t res_id) {
    uint32_t d = wm::new_window(storage, bounds, title, false, proc, behind, goaway, refcon, kDialogSize);
    st16(d + 108, 2);                    /* windowKind: dialogKind */
    st32(d + kDlgItems, items_h);
    st16(d + kDlgEditField, 0xFFFF);
    st16(d + kDlgDefItem, 1);
    if (res_id) apply_dctb(d, res_id);
    init_items(d);
    /* first edit field gets the focus */
    auto list = items(d);
    for (size_t i = 0; i < list.size(); i++)
        if ((list[i].type & 0x7F) == 16) { st16(d + kDlgEditField, (uint16_t)i); g_sel_start[d] = 0; g_sel_end[d] = (int)handle_text(list[i].handle).size(); break; }
    if (vis) {
        wm::show_window(d, true);
        draw_dialog(d);
    }
    return d;
}

void dispose_dialog(uint32_t d) {
    for (auto &it : items(d)) {
        int t = it.type & 0x7F;
        if ((t == 8 || t == 16) && it.handle) handle_dispose(it.handle);
    }
    while (ld32(d + kWinControlList)) dispose_control(ld32(d + kWinControlList));
    g_sel_start.erase(d);
    g_sel_end.erase(d);
    wm::dispose_window(d);
}

static int item_at(uint32_t d, int v, int h) {
    auto list = items(d);
    for (size_t i = 0; i < list.size(); i++) {
        Rect r = list[i].rect;
        if (!hidden(r) && v >= r.top && v < r.bottom && h >= r.left && h < r.right) return (int)i + 1;
    }
    return 0;
}

static void redraw_item(uint32_t d, int i) {
    auto list = items(d);
    if (i < 1 || i > (int)list.size()) return;
    uint32_t saved = current_port();
    set_current_port(d);
    draw_item(d, i, list[i - 1]);
    set_current_port(saved);
}

/* returns the item hit, or 0 when the event was consumed without a hit */
static int handle_event(uint32_t d, const host::MacEvent &e, PPCContext *c) {
    (void)c;
    auto list = items(d);
    if (e.what == 6) {   /* update */
        if (e.message == d) {
            if (!wm::begin_update(d)) return 0;   /* already handled (e.g. by the filter) */
            draw_dialog(d);
            wm::end_update(d);
        } else {
            wm::begin_update(e.message);   /* nobody handled it: drop it rather than spin */
            wm::end_update(e.message);
        }
        return 0;
    }
    if (e.what == 1) {
        Surface s = surface_of_port(d);
        int lv = e.v + s.bounds.top, lh = e.h + s.bounds.left;
        int i = item_at(d, lv, lh);
        if (!i) { MessageBeep(MB_OK); return 0; }
        const ItemRef &it = list[i - 1];
        int type = it.type & 0x7F;
        if (type >= 4 && type <= 7) {
            if (!it.handle || !track_control(it.handle, lv, lh)) return 0;
        } else if (type == 16) {
            st16(d + kDlgEditField, (uint16_t)(i - 1));
            g_sel_start[d] = g_sel_end[d] = (int)handle_text(it.handle).size();
            for (size_t k = 0; k < list.size(); k++) if ((list[k].type & 0x7F) == 16) redraw_item(d, (int)k + 1);
        }
        return (it.type & 0x80) ? 0 : i;
    }
    if (e.what == 3 || e.what == 5) {
        uint8_t ch = (uint8_t)e.message;
        int def = v16(d + kDlgDefItem);
        if ((ch == 0x0D || ch == 0x03) && def >= 1 && def <= (int)list.size()) {
            const ItemRef &it = list[def - 1];
            if (it.handle && (it.type & 0x7F) >= 4 && (it.type & 0x7F) <= 7 && ld8(ld32(it.handle) + cr::contrlHilite) != 255) {
                st8(ld32(it.handle) + cr::contrlHilite, 10); draw_control(it.handle); host::present(true); Sleep(130);
                st8(ld32(it.handle) + cr::contrlHilite, 0); draw_control(it.handle);
                return def;
            }
        }
        int ef = v16(d + kDlgEditField);
        if (ef >= 0 && ef < (int)list.size()) {
            uint32_t th = list[ef].handle;
            std::string t = handle_text(th);
            int s0 = std::min(g_sel_start[d], (int)t.size()), s1 = std::min(g_sel_end[d], (int)t.size());
            if (ch == 0x09) {   /* tab: next edit field */
                for (size_t k = 1; k <= list.size(); k++) {
                    int n = (ef + (int)k) % (int)list.size();
                    if ((list[n].type & 0x7F) == 16) {
                        st16(d + kDlgEditField, (uint16_t)n);
                        g_sel_start[d] = 0; g_sel_end[d] = (int)handle_text(list[n].handle).size();
                        redraw_item(d, ef + 1); redraw_item(d, n + 1);
                        break;
                    }
                }
                return 0;
            }
            if (ch == 0x08) { if (s1 > s0) t.erase(s0, s1 - s0); else if (s0 > 0) { t.erase(s0 - 1, 1); s0--; } }
            else if (ch == 0x1C) { s0 = std::max(0, s0 - 1); }
            else if (ch == 0x1D) { s0 = std::min((int)t.size(), s0 + 1); }
            else if (ch >= 0x20 && ch != 0x7F) { t.erase(s0, s1 - s0); t.insert(t.begin() + s0, (char)ch); s0++; }
            else return 0;
            set_handle_text(th, t);
            g_sel_start[d] = g_sel_end[d] = s0;
            redraw_item(d, ef + 1);
            return (list[ef].type & 0x80) ? 0 : ef + 1;
        }
    }
    return 0;
}


int modal_dialog(PPCContext *c, uint32_t d, uint32_t filter) {
    uint32_t evbuf = sys_alloc(16, 4);
    uint32_t hitbuf = sys_alloc(2, 2);
    for (;;) {
        host::pump();
        host::MacEvent e{};
        bool got = host::next_event(0xFFFF, e, true) || wm::pending_window_event(0xFFFF, e, true);
        if (!got) {
            e.what = 0; e.when = ticks(); host::mouse_global(e.v, e.h); e.modifiers = host::modifiers();
        }
        if (filter) {
            st16(evbuf, e.what); st32(evbuf + 2, e.message); st32(evbuf + 6, e.when);
            st16(evbuf + 10, (uint16_t)e.v); st16(evbuf + 12, (uint16_t)e.h); st16(evbuf + 14, e.modifiers);
            st16(hitbuf, 0);
            uint32_t args[3] = {d, evbuf, hitbuf};
            uint32_t r = guest_call(c, filter, args, 3);
            if (r & 0xFF) return v16(hitbuf);
            /* the filter may have rewritten the event */
            e.what = ld16(evbuf); e.message = ld32(evbuf + 2);
            e.v = (int16_t)ld16(evbuf + 10); e.h = (int16_t)ld16(evbuf + 12); e.modifiers = ld16(evbuf + 14);
        }
        if (!got && e.what == 0) { host::present(false); Sleep(2); continue; }
        int hit = handle_event(d, e, c);
        PortCheck *mine = nullptr;   /* the port's checkboxes: ours, not the game's */
        for (auto &k : g_checks) if (hit && d == k.dlg && hit == k.item) mine = &k;
        if (mine) {
            uint32_t h = items(d)[hit - 1].handle;
            if (h) {
                int v = !v16(ld32(h) + cr::contrlValue);
                st16(ld32(h) + cr::contrlValue, (uint16_t)v);
                draw_control(h);
                mine->pending = v;
            }
            continue;
        }
        if (hit) return hit;
    }
}

}  // namespace pt::dlg

/* ======================================================================== */
using namespace pt;
using namespace pt::qd;
using namespace pt::dlg;

HLE(InitDialogs) { (void)c; }

HLE(GetNewDialog) {
    /* DialogPtr GetNewDialog(short dialogID, void *storage, WindowPtr behind) */
    std::vector<uint8_t> d;
    int16_t id = (int16_t)ARG(0);
    if (!res::get_bytes('DLOG', id, d) || d.size() < 21) { RET(0); return; }
    auto b16 = [&](int o) { return (int16_t)(d[o] << 8 | d[o + 1]); };
    Rect b{b16(0), b16(2), b16(4), b16(6)};
    int proc = b16(8);
    bool vis = d[10] != 0, goaway = d[12] != 0;
    uint32_t refcon = (uint32_t)(d[14] << 24 | d[15] << 16 | d[16] << 8 | d[17]);
    int16_t items_id = b16(18);
    std::string title((const char *)&d[21], d[20]);
    size_t pos_off = (21 + d[20] + 1) & ~1u;
    if (pos_off + 2 <= d.size()) b = position(b, (uint16_t)b16((int)pos_off));
    uint32_t ih = copy_ditl(items_id);
    RET(new_dialog(ARG(1), b, title, vis, proc, ARG(2), goaway, refcon, ih, id));
}

HLE(DisposeDialog) {
    for (auto &k : g_checks)
        if (ARG(0) == k.dlg) { k.dlg = 0; k.item = 0; k.pending = -1; }
    dispose_dialog(ARG(0));
}
HLE(DrawDialog) { draw_dialog(ARG(0)); }

HLE(ModalDialog) {
    uint32_t d = wm::front_window();
    int hit = modal_dialog(c, d, ARG(0));
    for (auto &k : g_checks)
        if (d == k.dlg && (hit == 1 || hit == 2)) {   /* Preferences: OK applies, Cancel drops */
            if (hit == 1 && k.pending >= 0 && (k.pending != 0) != k.get()) k.set(k.pending != 0);
            k.pending = -1;
        }
    st16(ARG(1), (uint16_t)hit);
}

HLE(ParamText) {
    for (int i = 0; i < 4; i++) g_param[i] = ARG(i) ? read_pstr(ARG(i)) : std::string();
}

HLE(Alert) {
    /* short Alert(short alertID, ModalFilterUPP filterProc) */
    std::vector<uint8_t> a;
    int16_t id = (int16_t)ARG(0);
    if (!res::get_bytes('ALRT', id, a) || a.size() < 12) { RET(1); return; }
    auto b16 = [&](int o) { return (int16_t)(a[o] << 8 | a[o + 1]); };
    Rect b{b16(0), b16(2), b16(4), b16(6)};
    if (a.size() >= 14) b = position(b, (uint16_t)b16(12));
    uint32_t ih = copy_ditl(b16(8));
    logf(LOG_UNIMPL | LOG_GFX, "Alert %d: %s | %s\n", id, g_param[0].c_str(), g_param[1].c_str());
    uint32_t d = new_dialog(0, b, "", true, 1, 0xFFFFFFFF, false, 0, ih, id);
    /* default button outline */
    auto list = items(d);
    if (!list.empty() && list[0].handle) {
        uint32_t saved = current_port();
        set_current_port(d);
        Rect r = list[0].rect;
        round_frame(d, {(int16_t)(r.top - 4), (int16_t)(r.left - 4), (int16_t)(r.bottom + 4), (int16_t)(r.right + 4)}, {0, 0, 0}, 22, 3);
        set_current_port(saved);
    }
    int hit = modal_dialog(c, d, ARG(1));
    dispose_dialog(d);
    RET(hit);
}

HLE(GetDialogItem) {
    /* void GetDialogItem(DialogPtr, short item, short *type, Handle *item, Rect *box) */
    auto list = items(ARG(0));
    int i = (int16_t)ARG(1);
    if (i < 1 || i > (int)list.size()) return;
    if (ARG(2)) st16(ARG(2), list[i - 1].type);
    if (ARG(3)) st32(ARG(3), list[i - 1].handle);
    if (ARG(4)) write_rect(ARG(4), list[i - 1].rect);
}
HLE(SetDialogItem) {
    auto list = items(ARG(0));
    int i = (int16_t)ARG(1);
    if (i < 1 || i > (int)list.size()) return;
    st8(list[i - 1].addr + 12, (uint8_t)ARG(2));
    st32(list[i - 1].addr, ARG(3));
    write_rect(list[i - 1].addr + 4, read_rect(ARG(4)));
}
HLE(GetDialogItemText) { write_pstr(ARG(1), handle_text(ARG(0)).substr(0, 255)); }
HLE(SetDialogItemText) {
    set_handle_text(ARG(0), read_pstr(ARG(1)));
    /* redraw the item wherever it lives */
    uint32_t w = wm::front_window();
    if (w) {
        auto list = items(w);
        for (size_t i = 0; i < list.size(); i++)
            if (list[i].handle == ARG(0)) { redraw_item(w, (int)i + 1); break; }
    }
}
HLE(SelectDialogItemText) {
    uint32_t d = ARG(0);
    st16(d + kDlgEditField, (uint16_t)((int16_t)ARG(1) - 1));
    g_sel_start[d] = (int16_t)ARG(2);
    g_sel_end[d] = (int16_t)ARG(3);
    redraw_item(d, (int16_t)ARG(1));
}
HLE(FindDialogItem) {
    int16_t v = (int16_t)(ARG(1) >> 16), h = (int16_t)ARG(1);
    RET(item_at(ARG(0), v, h) - 1);
}
static void hide_show(uint32_t d, int i, bool hide) {
    auto list = items(d);
    if (i < 1 || i > (int)list.size()) return;
    Rect r = list[i - 1].rect;
    if (hide == hidden(r)) return;
    uint32_t saved = current_port();
    set_current_port(d);
    if (hide) {
        fill_rect_solid(d, {(int16_t)(r.top - 4), (int16_t)(r.left - 4), (int16_t)(r.bottom + 4), (int16_t)(r.right + 4)}, port_bk(d));
        r.left += 16384; r.right += 16384;
    } else { r.left -= 16384; r.right -= 16384; }
    write_rect(list[i - 1].addr + 4, r);
    uint32_t h = list[i - 1].handle;
    int t = list[i - 1].type & 0x7F;
    if (t >= 4 && t <= 7 && h) { st8(ld32(h) + cr::contrlVis, !hide); write_rect(ld32(h) + cr::contrlRect, r); }
    if (!hide) draw_item(d, i, items(d)[i - 1]);
    set_current_port(saved);
}
HLE(HideDialogItem) { hide_show(ARG(0), (int16_t)ARG(1), true); }
HLE(ShowDialogItem) { hide_show(ARG(0), (int16_t)ARG(1), false); }
HLE(CountDITL) { RET(items(ARG(0)).size()); }

HLE(AppendDITL) {
    /* void AppendDITL(DialogPtr, Handle theHandle, DITLMethod method) */
    uint32_t d = ARG(0), src = ARG(1);
    int method = (int16_t)ARG(2);
    uint32_t ih = ld32(d + kDlgItems);
    auto before = items(d);
    Rect pr = read_rect(d + port::portRect);
    int dh = 0, dv = 0;
    if (method == 1) dh = pr.right;
    else if (method == 2) dv = pr.bottom;
    else if (method < 0 && -method <= (int)before.size()) { Rect r = before[-method - 1].rect; dh = r.left; dv = r.top; }
    uint32_t sp = ld32(src);
    int n = v16(sp) + 1;
    uint32_t old_size = handle_size(ih), add_size = handle_size(src) - 2;
    handle_set_size(ih, old_size + add_size);
    memcpy(g_mem + ld32(ih) + old_size, g_mem + ld32(src) + 2, add_size);
    st16(ld32(ih), (uint16_t)(before.size() + n - 1));
    auto after = items(d);
    Rect grow = pr;
    for (size_t i = before.size(); i < after.size(); i++) {
        Rect r = after[i].rect;
        r = {(int16_t)(r.top + dv), (int16_t)(r.left + dh), (int16_t)(r.bottom + dv), (int16_t)(r.right + dh)};
        write_rect(after[i].addr + 4, r);
        grow = rect_union(grow, {r.top, r.left, (int16_t)(r.bottom + 6), (int16_t)(r.right + 6)});
        /* materialise the new item's handle */
        int type = after[i].type & 0x7F;
        std::string data((const char *)g_mem + after[i].data, after[i].len);
        uint32_t h = 0;
        if (type == 8 || type == 16) { h = handle_new((uint32_t)data.size(), false); if (!data.empty()) memcpy(g_mem + ld32(h), data.data(), data.size()); }
        else if (PortCheck *k = type == 5 ? check_labelled(data) : nullptr) {   /* the port's (patch_ditl) */
            if (k->dlg != d) k->pending = -1;
            int v = k->pending >= 0 ? k->pending : k->get();
            h = new_control(d, r, data, true, v, 0, 1, 1, 0);
            k->dlg = d;
            k->item = (int)i + 1;
        }
        else if (type >= 4 && type <= 6) h = new_control(d, r, data, true, 0, 0, 1, type - 4, 0);
        else if (type == 7) {
            int16_t id = (int16_t)(ld8(after[i].data) << 8 | ld8(after[i].data + 1));
            std::vector<uint8_t> cn;
            if (res::get_bytes('CNTL', id, cn) && cn.size() >= 22) {
                auto b16 = [&](int o) { return (int16_t)(cn[o] << 8 | cn[o + 1]); };
                h = new_control(d, r, std::string((const char *)&cn[23], cn[22]), b16(10) != 0, b16(8), b16(14), b16(12), b16(16), 0);
            }
        } else if (type == 32) h = res::get_handle('ICON', (int16_t)(ld8(after[i].data) << 8 | ld8(after[i].data + 1)));
        else if (type == 64) h = res::get_handle('PICT', (int16_t)(ld8(after[i].data) << 8 | ld8(after[i].data + 1)));
        st32(after[i].addr, h);
    }
    if (grow.right > pr.right || grow.bottom > pr.bottom) {
        wm::size_window(d, std::max<int>(pr.right, grow.right) - pr.left, std::max<int>(pr.bottom, grow.bottom) - pr.top);
    }
    draw_dialog(d);
}

HLE(ShortenDITL) {
    uint32_t d = ARG(0);
    int n = (int16_t)ARG(1);
    auto list = items(d);
    int keep = std::max(0, (int)list.size() - n);
    uint32_t saved = current_port();
    set_current_port(d);
    for (int i = keep; i < (int)list.size(); i++) {
        /* removed items leave the screen (the game's preference panes rely on it) */
        Rect r = list[i].rect;
        if (!hidden(r))
            fill_rect_solid(d, {(int16_t)(r.top - 4), (int16_t)(r.left - 4), (int16_t)(r.bottom + 4), (int16_t)(r.right + 4)},
                            port_bk(d));
        int t = list[i].type & 0x7F;
        if (t >= 4 && t <= 7 && list[i].handle) dispose_control(list[i].handle);
        else if ((t == 8 || t == 16) && list[i].handle) handle_dispose(list[i].handle);
    }
    for (auto &k : g_checks)
        if (d == k.dlg && k.item > keep) k.item = 0;   /* its pane went; the choice stays */
    if (keep < (int)list.size()) {
        uint32_t ih = ld32(d + kDlgItems);
        uint32_t cut = list[keep].addr - ld32(ih);
        handle_set_size(ih, cut);
        st16(ld32(ih), (uint16_t)(keep - 1));
    }
    set_current_port(saved);
    draw_dialog(d);
}

/* ---------- Control Manager entry points ---------- */
HLE(NewControl) {
    /* ControlHandle NewControl(WindowPtr, const Rect *, ConstStr255Param title, Boolean visible,
     *                          short value, short min, short max, short procID, long refCon) */
    uint32_t ch = new_control(ARG(0), read_rect(ARG(1)), read_pstr(ARG(2)), ARG(3) & 0xFF, (int16_t)ARG(4), (int16_t)ARG(5),
                              (int16_t)ARG(6), (int16_t)ARG(7), ARG(8));
    if (ARG(3) & 0xFF) draw_control(ch);
    RET(ch);
}
HLE(DisposeControl) { dispose_control(ARG(0)); }
HLE(KillControls) { while (ld32(ARG(0) + kWinControlList)) dispose_control(ld32(ARG(0) + kWinControlList)); }
HLE(GetControlValue) { RET((int32_t)v16(ld32(ARG(0)) + cr::contrlValue)); }
HLE(SetControlValue) {
    uint32_t c0 = ld32(ARG(0));
    int v = (int16_t)ARG(1), mn = v16(c0 + cr::contrlMin), mx = v16(c0 + cr::contrlMax);
    if (v < mn) v = mn;
    if (v > mx) v = mx;
    st16(c0 + cr::contrlValue, (uint16_t)v);
    draw_control(ARG(0));
}
HLE(GetControlMaximum) { RET((int32_t)v16(ld32(ARG(0)) + cr::contrlMax)); }
HLE(SetControlMaximum) { st16(ld32(ARG(0)) + cr::contrlMax, (uint16_t)ARG(1)); draw_control(ARG(0)); }
HLE(GetControlTitle) { write_pstr(ARG(1), read_pstr(ld32(ARG(0)) + cr::contrlTitle)); }
HLE(SetControlTitle) { write_pstr(ld32(ARG(0)) + cr::contrlTitle, read_pstr(ARG(1))); draw_control(ARG(0)); }
HLE(HiliteControl) { st8(ld32(ARG(0)) + cr::contrlHilite, (uint8_t)ARG(1)); draw_control(ARG(0)); }
HLE(ShowControl) { st8(ld32(ARG(0)) + cr::contrlVis, 1); draw_control(ARG(0)); }
HLE(HideControl) {
    uint32_t c0 = ld32(ARG(0));
    st8(c0 + cr::contrlVis, 0);
    uint32_t w = ld32(c0 + cr::contrlOwner), saved = current_port();
    set_current_port(w);
    fill_rect_solid(w, read_rect(c0 + cr::contrlRect), port_bk(w));
    set_current_port(saved);
}
HLE(MoveControl) {
    uint32_t c0 = ld32(ARG(0));
    Rect r = read_rect(c0 + cr::contrlRect);
    int16_t h = (int16_t)ARG(1), v = (int16_t)ARG(2);
    write_rect(c0 + cr::contrlRect, {v, h, (int16_t)(v + r.bottom - r.top), (int16_t)(h + r.right - r.left)});
    draw_control(ARG(0));
}
HLE(SizeControl) {
    uint32_t c0 = ld32(ARG(0));
    Rect r = read_rect(c0 + cr::contrlRect);
    write_rect(c0 + cr::contrlRect, {r.top, r.left, (int16_t)(r.top + (int16_t)ARG(2)), (int16_t)(r.left + (int16_t)ARG(1))});
    draw_control(ARG(0));
}
HLE(DrawControls) {
    for (uint32_t ch = ld32(ARG(0) + kWinControlList); ch; ch = ld32(ld32(ch) + cr::nextControl)) draw_control(ch);
}
HLE(FindControl) {
    /* short FindControl(Point pt, WindowPtr, ControlHandle *) - pt in local coordinates */
    int16_t v = (int16_t)(ARG(0) >> 16), h = (int16_t)ARG(0);
    st32(ARG(2), 0);
    for (uint32_t ch = ld32(ARG(1) + kWinControlList); ch; ch = ld32(ld32(ch) + cr::nextControl)) {
        uint32_t c0 = ld32(ch);
        Rect r = read_rect(c0 + cr::contrlRect);
        if (!ld8(c0 + cr::contrlVis) || ld8(c0 + cr::contrlHilite) == 255) continue;
        if (v >= r.top && v < r.bottom && h >= r.left && h < r.right) {
            st32(ARG(2), ch);
            int proc = ctl_proc(ch);
            RET(is_popup(ch) ? 1 : proc == 0 ? 10 : proc <= 2 ? 11 : 129);
            return;
        }
    }
    RET(0);
}
HLE(TrackControl) { RET(track_control(ARG(0), (int16_t)(ARG(1) >> 16), (int16_t)ARG(1))); }

/* ---------- TextEdit (only what the game uses outside dialogs) ---------- */

HLE(TEInit) { (void)c; }
HLE(TENew) {
    uint32_t h = handle_new(104, true);
    uint32_t t = ld32(h);
    write_rect(t, read_rect(ARG(0)));
    write_rect(t + 8, read_rect(ARG(1)));
    st32(t + 62, handle_new(0, true));
    st32(t + 82, current_port());   /* inPort */
    RET(h);
}
HLE(TEDispose) { handle_dispose(ld32(ld32(ARG(0)) + 62)); handle_dispose(ARG(0)); }
HLE(TEUpdate) {
    uint32_t t = ld32(ARG(1));
    uint32_t th = ld32(t + 62);
    te_text_box(current_port(), ld32(th), (int)handle_size(th), read_rect(t + 8), 0);
}
static void te_set_text(uint32_t te, const std::string &s) {
    uint32_t t = ld32(te);
    uint32_t th = ld32(t + 62);
    handle_set_size(th, (uint32_t)s.size());
    if (!s.empty()) memcpy(g_mem + ld32(th), s.data(), s.size());
    st16(ld32(te) + 60, (uint16_t)s.size());
}
static std::string te_text(uint32_t te) { uint32_t th = ld32(ld32(te) + 62); return std::string((const char *)g_mem + ld32(th), handle_size(th)); }
HLE(TEKey) {
    std::string s = te_text(ARG(1));
    uint32_t t = ld32(ARG(1));
    int s0 = v16(t + 32), s1 = v16(t + 34);
    char ch = (char)ARG(0);
    if (ch == 8) { if (s1 > s0) s.erase(s0, s1 - s0); else if (s0 > 0) s.erase(--s0, 1); }
    else { s.erase(s0, s1 - s0); s.insert(s.begin() + s0, ch); s0++; }
    te_set_text(ARG(1), s);
    st16(ld32(ARG(1)) + 32, (uint16_t)s0); st16(ld32(ARG(1)) + 34, (uint16_t)s0);
}
HLE(TEInsert) {
    std::string s = te_text(ARG(2));
    uint32_t t = ld32(ARG(2));
    int s0 = std::min<int>(v16(t + 32), (int)s.size());
    s.insert(s0, std::string((const char *)g_mem + ARG(0), ARG(1)));
    te_set_text(ARG(2), s);
}
HLE(TEDelete) {
    std::string s = te_text(ARG(0));
    uint32_t t = ld32(ARG(0));
    int s0 = v16(t + 32), s1 = v16(t + 34);
    if (s1 > s0 && s1 <= (int)s.size()) s.erase(s0, s1 - s0);
    te_set_text(ARG(0), s);
    st16(ld32(ARG(0)) + 34, (uint16_t)s0);
}
HLE(TESetSelect) { st16(ld32(ARG(2)) + 32, (uint16_t)ARG(0)); st16(ld32(ARG(2)) + 34, (uint16_t)ARG(1)); }
HLE(TEActivate) { st16(ld32(ARG(0)) + 36, 1); }
HLE(TEDeactivate) { st16(ld32(ARG(0)) + 36, 0); }
HLE(TEIdle) { (void)c; }
HLE(TEClick) { (void)c; }
HLE(TECalText) { (void)c; }
HLE(TEScroll) { (void)c; }
HLE(TEAutoView) { (void)c; }
HLE(TECut) { (void)c; }
HLE(TECopy) { (void)c; }
HLE(TEPaste) { (void)c; }
static uint32_t g_te_scrap = 0;
HLE(TEScrapHandle) { if (!g_te_scrap) g_te_scrap = handle_new(0, true); RET(g_te_scrap); }

/* ---------- List Manager: a minimal single-column list ---------- */
HLE(LNew) {
    uint32_t h = handle_new(64, true);
    write_rect(ld32(h), read_rect(ARG(0)));
    RET(h);
}
HLE(LDispose) { handle_dispose(ARG(0)); }
HLE(LAddRow) { RET(ARG(1)); }
HLE(LDelRow) { (void)c; }
HLE(LSetCell) { (void)c; }
HLE(LUpdate) { (void)c; }
HLE(LClick) { RET(0); }
HLE(LGetSelect) { RET(0); }
HLE(LSetSelect) { (void)c; }
HLE(LAutoScroll) { (void)c; }
HLE(LSetDrawingMode) { (void)c; }

/* ---------- Scrap Manager ---------- */
static std::vector<uint8_t> g_scrap;
static uint32_t g_scrap_type = 0;
HLE(ZeroScrap) { g_scrap.clear(); g_scrap_type = 0; RET(0); }
HLE(PutScrap) { g_scrap.assign(g_mem + ARG(2), g_mem + ARG(2) + ARG(0)); g_scrap_type = ARG(1); RET(0); }
HLE(GetScrap) {
    /* long GetScrap(Handle hDest, ResType theType, long *offset) */
    if (ARG(1) != g_scrap_type || g_scrap.empty()) { RET(-102); return; }
    if (ARG(0)) { handle_set_size(ARG(0), (uint32_t)g_scrap.size()); memcpy(g_mem + ld32(ARG(0)), g_scrap.data(), g_scrap.size()); }
    if (ARG(2)) st32(ARG(2), 0);
    RET(g_scrap.size());
}

/* ---------- Printing Manager: no printer ---------- */
HLE(PrOpen) { (void)c; }
HLE(PrClose) { (void)c; }
HLE(PrError) { RET(-128); }   /* iPrAbort */
HLE(PrintDefault) { (void)c; }
HLE(PrStlDialog) { RET(0); }
HLE(PrJobDialog) { RET(0); }
HLE(PrOpenDoc) { RET(0); }
HLE(PrCloseDoc) { (void)c; }
HLE(PrOpenPage) { (void)c; }
HLE(PrClosePage) { (void)c; }
HLE(PrPicFile) { (void)c; }
