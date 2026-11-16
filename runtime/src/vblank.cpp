/* The display's refresh timing, from the Windows compositor (DWM). With vsync, SDL's present
 * returns when the driver takes the frame, not at the refresh it shows on (frames queue), so
 * frame pacing (interp.cpp) counts refreshes from the compositor's last vblank instead. Kept
 * apart from host.cpp: <windows.h> defines RGB and friends. */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dwmapi.h>

#include <chrono>

#include "pt/host.h"

namespace pt::host {

bool vblank(std::chrono::steady_clock::time_point &last, double &period) {
    DWM_TIMING_INFO ti{};
    ti.cbSize = sizeof ti;
    if (FAILED(DwmGetCompositionTimingInfo(nullptr, &ti)) || ti.qpcRefreshPeriod == 0 || ti.qpcVBlank == 0) return false;
    LARGE_INTEGER freq, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    auto steady_now = std::chrono::steady_clock::now();
    double ago = (double)((int64_t)now.QuadPart - (int64_t)ti.qpcVBlank) / (double)freq.QuadPart;
    period = (double)ti.qpcRefreshPeriod / (double)freq.QuadPart;
    if (period < 1.0 / 500 || period > 1.0 / 20 || ago < -period || ago > 1.0) return false;
    last = steady_now - std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(ago));
    return true;
}

}  // namespace pt::host
