/* Host platform layer (SDL2): window, presentation, input, audio device. */
#pragma once

#include <chrono>
#include <cstdint>
#include <deque>

namespace pt::host {

void init();
void pump();                 /* poll input, keep low-memory globals current, present if due */
void present(bool force = false);

/* input state for the Event Manager */
struct MacEvent {
    uint16_t what;
    uint32_t message;
    uint32_t when;
    int16_t v, h;
    uint16_t modifiers;
};
bool next_event(uint16_t mask, MacEvent &out, bool remove);
void flush_events(uint16_t mask);
void post_event(const MacEvent &e);
void key_map(uint8_t out[16]);
void mouse_global(int16_t &v, int16_t &h);
bool mouse_down();
void take_mouse_delta(int &dx, int &dy);   /* mouse motion (guest pixels) since the last call */
void peek_mouse_delta(int &dx, int &dy);   /* the same, without taking it */
int refresh_rate();                        /* the window's display, in Hz */
/* the last present: just before SDL_RenderPresent and when it returned (with vsync, at a refresh) */
void last_present(std::chrono::steady_clock::time_point &begin, std::chrono::steady_clock::time_point &end);
bool vsync_on();
/* the compositor's last vblank and the refresh period (vblank.cpp); false if not available */
bool vblank(std::chrono::steady_clock::time_point &last, double &period);
/* widescreen: the presented frame has side_width() extra columns each side of the Mac screen;
 * side_row(y) holds them (left side_width() pixels, then the right ones, 0xAARRGGBB), shown
 * while the function given to set_sides_live returns true */
int side_width();
void reconfigure_width();   /* re-read the widescreen option and resize to it */
uint32_t *side_row(int y);
void set_sides_live(bool (*fn)());
/* high resolution: hires_frame(w, h, depth) is a w x h buffer of the full-size 3D view's guest
 * pixels (8, 16 or 32 bits, rows packed), converted through the display's colours as it is
 * presented (so fades show) in place of the Mac screen while set_hires_live's function says so */
uint8_t *hires_frame(int w, int h, int depth);
void set_hires_live(bool (*fn)());
bool is_fullscreen();                      /* the window covers its monitor */
void set_fullscreen(bool on);
uint16_t modifiers();

/* cursor */
void set_cursor(const uint8_t data[68]);
void hide_cursor();
void show_cursor();
void init_cursor();   /* InitCursor: cursor level back to 0 (visible) */
void obscure_cursor();

/* audio: the mixer callback fills interleaved stereo int16 at 44100 Hz */
using AudioFill = void (*)(int16_t *out, int frames);
void set_audio_fill(AudioFill fn);
bool audio_running();   /* the mixer is being called (a device is open and playing) */
void lock_audio();
void unlock_audio();
extern const int kAudioRate;

}  // namespace pt::host
