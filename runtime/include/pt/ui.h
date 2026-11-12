/* Cross-module entry points of the Window, Menu and Dialog Managers. */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "pt/host.h"
#include "pt/qd.h"

namespace pt::wm {
uint32_t new_window(uint32_t storage, const qd::Rect &bounds, const std::string &title, bool visible, int proc,
                    uint32_t behind, bool goaway, uint32_t refcon, uint32_t size);
void dispose_window(uint32_t w);
void show_window(uint32_t w, bool show);
void size_window(uint32_t w, int width, int height);
bool is_window(uint32_t w);
uint32_t front_window();
bool pending_window_event(uint16_t mask, host::MacEvent &e, bool remove);
uint32_t wmgr_port();
bool begin_update(uint32_t w);   /* false when there was nothing to update */
void end_update(uint32_t w);
}  // namespace pt::wm

namespace pt::menu {
int popup_select(uint32_t mh, int top, int left, int current);
std::vector<std::string> item_names(uint32_t mh);
void insert_hier(uint32_t mh);
}  // namespace pt::menu

namespace pt::qd {
void te_text_box(uint32_t port, uint32_t text, int len, const Rect &box, int just);
}

namespace pt {
PPCContext *current_ctx();   /* context of the guest code that made the current Toolbox call */
}
