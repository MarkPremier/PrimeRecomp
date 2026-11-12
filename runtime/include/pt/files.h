#pragma once

#include <cstdint>
#include <filesystem>

namespace pt::files {

/* open a fork; returns refNum > 0, or a negative OSErr */
int16_t open_fork(const std::filesystem::path &host, bool rsrc, int perm);
int16_t close(int16_t refNum);
int16_t read(int16_t refNum, uint32_t buffer, uint32_t &count);
int16_t write(int16_t refNum, uint32_t buffer, uint32_t &count);
int16_t set_pos(int16_t refNum, int16_t mode, int32_t offset);
int16_t get_pos(int16_t refNum, uint32_t &pos);
int16_t get_eof(int16_t refNum, uint32_t &eof);
int16_t set_eof(int16_t refNum, uint32_t eof);
bool is_open(int16_t refNum);
std::filesystem::path host_of(int16_t refNum);
int16_t alloc_refnum();   /* for the Resource Manager's in-memory files */

}  // namespace pt::files
