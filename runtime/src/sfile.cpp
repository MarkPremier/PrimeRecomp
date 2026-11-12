/* Standard File Package (System 7 StandardGetFile/StandardPutFile and the
 * older SFPutFile), drawn natively in the style of the System 7 dialogs, plus
 * the remaining odds and ends: PlotIcon, trap-address queries, Slot Manager,
 * AppleTalk (absent), disk-initialisation. */
#include <windows.h>

#include <algorithm>
#include <string>
#include <vector>

#include "pt/host.h"
#include "pt/qd.h"
#include "pt/runtime.h"
#include "pt/ui.h"
#include "pt/vfs.h"

using namespace pt;
using namespace pt::qd;

namespace pt::sf {

struct Item { vfs::Entry e; };

static int16_t g_cur_vref = 0;
static uint32_t g_cur_dir = 0;

static void text(uint32_t p, int h, int v, const std::string &s, RGB c = {0, 0, 0}) {
    st16(p + port::rgbFgColor, c.r); st16(p + port::rgbFgColor + 2, c.g); st16(p + port::rgbFgColor + 4, c.b);
    st16(p + port::pnLoc, (uint16_t)v);
    st16(p + port::pnLoc + 2, (uint16_t)h);
    draw_text(p, (const uint8_t *)s.data(), (int)s.size());
}
static void box(uint32_t p, const Rect &r, RGB c) {
    fill_rect_solid(p, {r.top, r.left, (int16_t)(r.top + 1), r.right}, c);
    fill_rect_solid(p, {(int16_t)(r.bottom - 1), r.left, r.bottom, r.right}, c);
    fill_rect_solid(p, {r.top, r.left, r.bottom, (int16_t)(r.left + 1)}, c);
    fill_rect_solid(p, {r.top, (int16_t)(r.right - 1), r.bottom, r.right}, c);
}
static void button(uint32_t p, const Rect &r, const std::string &label, bool def, bool enabled, bool pressed) {
    RGB fg = enabled ? RGB{0, 0, 0} : RGB{0x8888, 0x8888, 0x8888};
    fill_rect_solid(p, r, pressed ? RGB{0, 0, 0} : RGB{0xFFFF, 0xFFFF, 0xFFFF});
    box(p, r, fg);
    if (def) box(p, {(int16_t)(r.top - 3), (int16_t)(r.left - 3), (int16_t)(r.bottom + 3), (int16_t)(r.right + 3)}, fg),
             box(p, {(int16_t)(r.top - 2), (int16_t)(r.left - 2), (int16_t)(r.bottom + 2), (int16_t)(r.right + 2)}, fg);
    int w = text_width(p, (const uint8_t *)label.data(), (int)label.size());
    text(p, (r.left + r.right - w) / 2, r.bottom - 5, label, pressed ? RGB{0xFFFF, 0xFFFF, 0xFFFF} : fg);
}

static std::vector<Item> list_dir(uint32_t filter, int ntypes, uint32_t types, bool put, PPCContext *c) {
    std::vector<Item> out;
    uint32_t pb = put ? 0 : sys_alloc(108, 4);
    for (auto &e : vfs::list(g_cur_vref, g_cur_dir)) {
        if (!e.is_dir && !put) {
            if (ntypes > 0) {
                bool ok = false;
                for (int i = 0; i < ntypes; i++) ok |= ld32(types + 4 * i) == e.type;
                if (!ok) continue;
            }
            if (filter) {
                /* FileFilterProc(CInfoPBPtr): TRUE hides the file */
                memset(g_mem + pb, 0, 108);
                uint32_t name = sys_alloc(64, 2);
                write_pstr(name, e.mac_name);
                st32(pb + 18, name);
                st16(pb + 22, (uint16_t)g_cur_vref);
                st32(pb + 32, e.type);
                st32(pb + 36, e.creator);
                st32(pb + 100, g_cur_dir);
                uint32_t arg = pb;
                if (guest_call(c, filter, &arg, 1) & 0xFF) continue;
            }
        }
        out.push_back({e});
    }
    return out;
}

/* returns true when the user confirmed; fills spec */
static bool run(PPCContext *c, bool put, const std::string &prompt, std::string name, uint32_t filter, int ntypes,
                uint32_t types, vfs::Spec &result, bool &replacing) {
    if (!g_cur_dir) { g_cur_vref = vfs::app_vref(); g_cur_dir = vfs::app_dir(); }
    const int W = 344, H = put ? 216 : 200;
    Rect wr{(int16_t)((480 - H) / 3 + 20), (int16_t)((640 - W) / 2), 0, 0};
    wr.bottom = (int16_t)(wr.top + H);
    wr.right = (int16_t)(wr.left + W);
    uint32_t w = wm::new_window(0, wr, "", true, 1, 0xFFFFFFFF, false, 0, 156);
    Rect listr{28, 12, (int16_t)(28 + 16 * 8 + 2), 232};
    Rect okr{(int16_t)(put ? 150 : 128), 248, (int16_t)(put ? 170 : 148), 330};
    Rect cancelr{(int16_t)(put ? 182 : 160), 248, (int16_t)(put ? 202 : 180), 330};
    Rect upr{28, 248, 48, 330};
    Rect namer{(int16_t)(H - 30), 14, (int16_t)(H - 12), 230};
    int sel = put ? -1 : 0, scroll = 0;
    std::vector<Item> items = list_dir(filter, ntypes, types, put, c);
    uint32_t last_click = 0;
    int last_sel = -2;
    bool done = false, ok = false;
    auto redraw = [&]() {
        uint32_t saved = current_port();
        set_current_port(w);
        st16(w + port::txFont, 0); st16(w + port::txSize, 12); st8(w + port::txFace, 0); st16(w + port::txMode, 1);
        fill_rect_solid(w, {0, 0, (int16_t)H, (int16_t)W}, {0xFFFF, 0xFFFF, 0xFFFF});
        std::string folder = vfs::dir_name(g_cur_vref, g_cur_dir);
        Rect fg{8, 14, 18, 26};   /* folder glyph beside the current folder's name */
        box(w, fg, {0, 0, 0});
        fill_rect_solid(w, {fg.top, fg.left, (int16_t)(fg.top + 3), (int16_t)(fg.left + 6)}, {0, 0, 0});
        text(w, 32, 18, folder);
        box(w, {(int16_t)(listr.top - 1), (int16_t)(listr.left - 1), (int16_t)(listr.bottom + 1), (int16_t)(listr.right + 1)}, {0, 0, 0});
        for (int i = 0; i < 8 && scroll + i < (int)items.size(); i++) {
            const Item &it = items[scroll + i];
            int y = listr.top + 1 + 16 * i;
            bool hl = scroll + i == sel;
            if (hl) fill_rect_solid(w, {(int16_t)y, (int16_t)(listr.left + 1), (int16_t)(y + 16), (int16_t)(listr.right - 1)}, {0, 0, 0});
            RGB fg = hl ? RGB{0xFFFF, 0xFFFF, 0xFFFF} : put && !it.e.is_dir ? RGB{0x8888, 0x8888, 0x8888} : RGB{0, 0, 0};
            /* little folder / document glyph */
            Rect g{(int16_t)(y + 3), (int16_t)(listr.left + 5), (int16_t)(y + 13), (int16_t)(listr.left + 17)};
            box(w, g, fg);
            if (it.e.is_dir) fill_rect_solid(w, {g.top, g.left, (int16_t)(g.top + 3), (int16_t)(g.left + 6)}, fg);
            text(w, listr.left + 22, y + 12, it.e.mac_name, fg);
        }
        if ((int)items.size() > 8) {   /* scroll indicator */
            int th = (listr.bottom - listr.top) * 8 / (int)items.size();
            int ty = listr.top + (listr.bottom - listr.top - th) * scroll / std::max(1, (int)items.size() - 8);
            fill_rect_solid(w, {(int16_t)ty, (int16_t)(listr.right - 4), (int16_t)(ty + th), (int16_t)listr.right}, {0x8888, 0x8888, 0x8888});
        }
        bool has_parent = vfs::dir_parent(g_cur_vref, g_cur_dir) > 1;
        button(w, upr, "Up", false, has_parent, false);
        bool can_ok = put ? !name.empty() : (sel >= 0 && sel < (int)items.size());
        button(w, okr, put ? "Save" : (sel >= 0 && sel < (int)items.size() && items[sel].e.is_dir ? "Open" : "Open"), true, can_ok, false);
        button(w, cancelr, "Cancel", false, true, false);
        if (put) {
            text(w, 14, namer.top - 6, prompt);
            fill_rect_solid(w, namer, {0xFFFF, 0xFFFF, 0xFFFF});
            box(w, {(int16_t)(namer.top - 2), (int16_t)(namer.left - 2), (int16_t)(namer.bottom + 2), (int16_t)(namer.right + 2)}, {0, 0, 0});
            text(w, namer.left + 2, namer.bottom - 4, name);
            int cx = namer.left + 2 + text_width(w, (const uint8_t *)name.data(), (int)name.size());
            fill_rect_solid(w, {(int16_t)(namer.top + 2), (int16_t)cx, (int16_t)(namer.bottom - 2), (int16_t)(cx + 1)}, {0, 0, 0});
        }
        set_current_port(saved);
        host::present(true);
    };
    auto enter_dir = [&](const vfs::Entry &e) {
        g_cur_dir = vfs::dir_id(g_cur_vref, e.host);
        items = list_dir(filter, ntypes, types, put, c);
        sel = put ? -1 : 0;
        scroll = 0;
    };
    auto confirm = [&]() {
        if (put) {
            if (name.empty()) return;
            vfs::Entry e;
            replacing = vfs::lookup(g_cur_vref, g_cur_dir, name, e);
            result = {g_cur_vref, g_cur_dir, name};
            ok = done = true;
        } else if (sel >= 0 && sel < (int)items.size()) {
            if (items[sel].e.is_dir) { enter_dir(items[sel].e); return; }
            result = {g_cur_vref, g_cur_dir, items[sel].e.mac_name};
            ok = done = true;
        }
    };
    redraw();
    host::flush_events(0xFFFF);
    while (!done) {
        host::pump();
        host::MacEvent e;
        if (!host::next_event(0xFFFF, e, true)) { Sleep(5); continue; }
        Surface s = surface_of_port(w);
        int lv = e.v + s.bounds.top, lh = e.h + s.bounds.left;
        auto in = [&](const Rect &r) { return lv >= r.top && lv < r.bottom && lh >= r.left && lh < r.right; };
        if (e.what == 1) {
            if (in(listr)) {
                int i = scroll + (lv - listr.top - 1) / 16;
                if (i < (int)items.size()) {
                    if (i == last_sel && e.when - last_click < 30) confirm();
                    else if (!put || items[i].e.is_dir) sel = i;
                    last_sel = i;
                    last_click = e.when;
                }
            } else if (in(okr)) confirm();
            else if (in(cancelr)) done = true;
            else if (in(upr)) {
                uint32_t par = vfs::dir_parent(g_cur_vref, g_cur_dir);
                if (par > 1) { g_cur_dir = par; items = list_dir(filter, ntypes, types, put, c); sel = put ? -1 : 0; scroll = 0; }
            }
        } else if (e.what == 3 || e.what == 5) {
            uint8_t ch = (uint8_t)e.message;
            bool cmd = (e.modifiers & 0x0100) != 0;
            if (ch == 0x0D || ch == 0x03) confirm();
            else if (ch == 0x1B || (cmd && ch == '.')) done = true;
            else if (ch == 0x1E && !put) sel = std::max(0, sel - 1);
            else if (ch == 0x1F && !put) sel = std::min((int)items.size() - 1, sel + 1);
            else if (put && ch == 0x08) { if (!name.empty()) name.pop_back(); }
            else if (put && ch >= 0x20 && ch != ':' && name.size() < 31) name += (char)ch;
            if (sel >= 0) { if (sel < scroll) scroll = sel; if (sel >= scroll + 8) scroll = sel - 7; }
        }
        if (!done) redraw();
    }
    wm::dispose_window(w);
    host::flush_events(0xFFFF);
    return ok;
}

}  // namespace pt::sf

using namespace pt::sf;

static void write_sfreply_new(uint32_t r, bool good, bool replacing, const vfs::Spec &s) {
    memset(g_mem + r, 0, 88);
    st8(r, good);
    st8(r + 1, replacing);
    vfs::Entry e;
    if (good && vfs::lookup(s.vref, s.parent, s.name, e)) st32(r + 2, e.type);
    if (good) vfs::write_fsspec(r + 6, s);
}

HLE(StandardGetFile) {
    /* void StandardGetFile(FileFilterUPP, short numTypes, SFTypeList, StandardFileReply *) */
    vfs::Spec s;
    bool rep = false;
    bool ok = run(c, false, "", "", ARG(0), (int16_t)ARG(1), ARG(2), s, rep);
    write_sfreply_new(ARG(3), ok, false, s);
}

HLE(StandardPutFile) {
    /* void StandardPutFile(ConstStr255Param prompt, ConstStr255Param defaultName, StandardFileReply *) */
    vfs::Spec s;
    bool rep = false;
    bool ok = run(c, true, read_pstr(ARG(0)), read_pstr(ARG(1)), 0, 0, 0, s, rep);
    write_sfreply_new(ARG(2), ok, rep, s);
}

HLE(SFPutFile) {
    /* void SFPutFile(Point where, ConstStr255Param prompt, ConstStr255Param origName, DlgHookUPP, SFReply *reply)
     * SFReply: good(1) copy(1) fType(4) vRefNum(2) version(2) fName(Str63) */
    vfs::Spec s;
    bool rep = false;
    bool ok = run(c, true, read_pstr(ARG(1)), read_pstr(ARG(2)), 0, 0, 0, s, rep);
    uint32_t r = ARG(4);
    memset(g_mem + r, 0, 74);
    st8(r, ok);
    if (ok) {
        st16(r + 6, (uint16_t)vfs::make_wd(s.vref, s.parent));
        write_pstr(r + 10, s.name);
    }
}

/* ---------- small remaining calls ---------- */
HLE(PlotIcon) {
    /* void PlotIcon(const Rect *theRect, Handle theIcon): 32x32 1-bit ICON scaled into the rect */
    uint32_t h = ARG(1);
    if (!h || !ld32(h)) return;
    Rect r = read_rect(ARG(0));
    uint32_t p = current_port(), d = ld32(h);
    int w = r.right - r.left, hh = r.bottom - r.top;
    for (int y = 0; y < hh; y++)
        for (int x = 0; x < w; x++) {
            int sx = x * 32 / w, sy = y * 32 / hh;
            if ((ld8(d + sy * 4 + (sx >> 3)) >> (7 - (sx & 7))) & 1)
                fill_rect_solid(p, {(int16_t)(r.top + y), (int16_t)(r.left + x), (int16_t)(r.top + y + 1), (int16_t)(r.left + x + 1)}, port_fg(p));
        }
}

HLE(GetToolTrapAddress) {
    /* every trap is "implemented" except _Unimplemented itself */
    RET(0x40000000u | (ARG(0) & 0xFFFF));
}

HLE(NewGDevice) { RET(handle_new(qd::gd::size, true)); }
HLE(DIBadMount) { RET(0); }

/* Slot Manager: no NuBus video cards to describe */
HLE(SNextTypeSRsrc) { RET(-344); }   /* smNoMoresRsrcs */
HLE(SGetCString) { RET(-344); }

/* AppleTalk is reported absent (Gestalt 'atlk', .MPP); these only fail */
HLE(POpenSkt) { RET(-91); }
HLE(PCloseSkt) { RET(-91); }
HLE(PWriteDDP) { RET(-91); }
HLE(BuildDDPwds) { (void)c; }
HLE(GetNodeAddress) { RET(-97); }
HLE(PRegisterName) { RET(-97); }
HLE(PRemoveName) { RET(-97); }
HLE(PLookupName) { RET(-97); }
HLE(PKillNBP) { RET(-97); }
HLE(NBPSetEntity) { (void)c; }
HLE(NBPSetNTE) { (void)c; }
HLE(NBPExtract) { RET(-1024); }
HLE(PSetSelfSend) { RET(0); }
