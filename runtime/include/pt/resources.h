#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace pt::res {

void init();   /* opens the application's resource fork */

/* host-side access for other managers (dialogs, menus, QuickDraw) */
bool get_bytes(uint32_t type, int16_t id, std::vector<uint8_t> &out);   /* search chain */
uint32_t get_handle(uint32_t type, int16_t id);                          /* guest handle or 0 */
uint32_t get_handle_by_name(uint32_t type, const std::string &mac_name);
std::vector<int16_t> ids_of_type(uint32_t type);
int16_t current_file();

}  // namespace pt::res
