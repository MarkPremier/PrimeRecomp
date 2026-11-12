/* Virtual HFS: maps Mac volumes, directory IDs and Mac Roman names onto host folders. */
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace pt::vfs {

namespace fs = std::filesystem;

enum : int16_t {
    noErr = 0, dirFulErr = -33, dskFulErr = -34, nsvErr = -35, ioErr = -36, bdNamErr = -37,
    fnOpnErr = -38, eofErr = -39, posErr = -40, tmfoErr = -42, fnfErr = -43, wPrErr = -44,
    fLckdErr = -45, vLckdErr = -46, fBsyErr = -47, dupFNErr = -48, opWrErr = -49, paramErr = -50,
    rfNumErr = -51, permErr = -54, wrPermErr = -61, dirNFErr = -120,
};

struct Entry {
    std::string mac_name;   /* Mac Roman bytes */
    fs::path host;
    bool is_dir = false;
    uint32_t type = 0, creator = 0;
    uint16_t finder_flags = 0;
};

struct Spec {               /* resolved FSSpec */
    int16_t vref = 0;
    uint32_t parent = 0;    /* dirID of containing folder */
    std::string name;       /* Mac Roman */
};

void init();

/* volumes */
int16_t app_vref();
uint32_t app_dir();                         /* folder holding the application */
std::string app_name();                     /* Mac name of the running application */
bool volume_info(int16_t vref, std::string &name, fs::path &root);
int16_t volume_by_index(int index);         /* 1-based, 0 when out of range */
int16_t volume_by_name(const std::string &mac_name);

/* directories */
bool dir_host(int16_t vref, uint32_t dirID, fs::path &out);
uint32_t dir_id(int16_t vref, const fs::path &host);   /* assigns IDs on demand */
uint32_t dir_parent(int16_t vref, uint32_t dirID);
std::string dir_name(int16_t vref, uint32_t dirID);
std::vector<Entry> list(int16_t vref, uint32_t dirID);  /* Mac order: case-insensitive name */

int16_t make_wd(int16_t vref, uint32_t dirID);          /* working-directory refnum */

/* names */
bool lookup(int16_t vref, uint32_t dirID, const std::string &mac_name, Entry &out);
/* resolve a (vRefNum, dirID, partial or full pathname) triple; returns noErr or fnfErr
 * (fnfErr still fills `out` when the parent folder exists) */
int16_t resolve(int16_t vref, uint32_t dirID, const std::string &path, Spec &out);
fs::path host_path_for_new(const Spec &spec);          /* host path to create `spec` at */
void set_finder_info(const Spec &spec, uint32_t type, uint32_t creator, uint16_t flags);
fs::path rsrc_path(const fs::path &data_path);

std::string mac_to_utf8(const std::string &mac);
std::string utf8_to_mac(const std::string &utf8);
bool mac_name_equal(const std::string &a, const std::string &b);

/* FSSpec in guest memory: vRefNum(2) parID(4) name(Str63) */
Spec read_fsspec(uint32_t addr);
void write_fsspec(uint32_t addr, const Spec &s);

}  // namespace pt::vfs
