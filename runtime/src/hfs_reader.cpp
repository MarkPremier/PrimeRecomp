/* The System file and font suitcases from the player's own Mac OS disk image
 * (System Folder:System and System Folder:Fonts), read with pt/hfs.h. */
#include <string>
#include <vector>

#include "pt/hfs.h"

namespace pt::hfs {

/* Loads System + font suitcases; returns resource forks keyed by name. */
std::vector<std::pair<std::string, std::vector<uint8_t>>> load_system(const std::wstring &image) {
    std::vector<std::pair<std::string, std::vector<uint8_t>>> out;
    Volume v;
    if (!v.open(image)) return out;
    std::vector<uint8_t> sysfolder, rec;
    if (!v.find(2, "System Folder", sysfolder) || !Volume::is_folder(sysfolder)) return out;
    uint32_t sf = Volume::dir_id(sysfolder);
    if (v.find(sf, "System", rec)) out.emplace_back("System", v.rsrc_fork(rec));
    std::vector<uint8_t> fonts;
    if (v.find(sf, "Fonts", fonts) && Volume::is_folder(fonts)) {
        uint32_t fd = Volume::dir_id(fonts);
        for (auto &name : v.list(fd)) {
            std::vector<uint8_t> frec;
            if (v.find(fd, name, frec) && !Volume::is_folder(frec)) {
                auto fork = v.rsrc_fork(frec);
                if (!fork.empty()) out.emplace_back(name, std::move(fork));
            }
        }
    }
    return out;
}

}  // namespace pt::hfs

namespace pt::res::hfs_glue {
std::vector<std::pair<std::string, std::vector<uint8_t>>> load(const std::wstring &image) {
    return pt::hfs::load_system(image);
}
}  // namespace pt::res::hfs_glue
