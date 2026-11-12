/* Host platform layer (SDL2): window, presentation, input, audio device. */
#pragma once

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
uint16_t modifiers();

/* cursor */
void set_cursor(const uint8_t data[68]);
void hide_cursor();
void show_cursor();
void obscure_cursor();

/* audio: the mixer callback fills interleaved stereo int16 at 44100 Hz */
using AudioFill = void (*)(int16_t *out, int frames);
void set_audio_fill(AudioFill fn);
void lock_audio();
void unlock_audio();
extern const int kAudioRate;

}  // namespace pt::host
