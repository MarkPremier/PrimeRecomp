/* Sound Manager 3.x.
 *
 * Each channel has a command queue processed in order by the mixer thread.
 * Sounds play straight from guest memory (standard and extended headers;
 * 8-bit offset-binary or 16-bit big-endian, mono or stereo), resampled to the
 * host rate with linear interpolation. callBackCmd completions are delivered
 * on the game thread at Toolbox-call safe points, like completion routines.
 * The CD-audio soundtrack is mixed in from the ripped tracks.
 */
#include <windows.h>

#include <algorithm>
#include <deque>
#include <map>
#include <mutex>
#include <vector>

#include "pt/host.h"
#include "pt/runtime.h"

namespace pt::cd { void mix(int32_t *acc, int frames); }

namespace pt::snd {

struct Cmd { uint16_t cmd; uint16_t p1; uint32_t p2; };

struct Voice {
    bool active = false;
    uint32_t data = 0;          /* guest address of samples */
    uint32_t frames = 0;
    int channels = 1, bits = 8;
    double rate = 22050;        /* source rate */
    double pos = 0;
    uint32_t loop_start = 0, loop_end = 0;
};

struct Channel {
    uint32_t guest = 0;         /* SndChannel record */
    uint32_t callback = 0;      /* SndCallBackUPP */
    std::deque<Cmd> queue;
    Voice voice;
    int amp = 255;              /* ampCmd 0..255 */
    int vol_l = 0x100, vol_r = 0x100;
    double rate_mul = 1.0;
    bool paused = false;
    double wait_frames = 0;     /* waitCmd remaining (host frames) */
};

static std::map<uint32_t, Channel> g_chans;    /* keyed by guest SndChannelPtr */

/* QuickTime movie soundtracks */
struct MovieVoice { std::vector<int16_t> pcm; int channels; double rate; double pos; };
static std::map<uint32_t, MovieVoice> g_movie_voices;

void movie_audio_start(uint32_t id, std::vector<int16_t> pcm, int channels, double rate, double offset_s) {
    host::lock_audio();
    if (pcm.empty()) g_movie_voices.erase(id);
    else g_movie_voices[id] = {std::move(pcm), channels < 1 ? 1 : channels, rate, offset_s * rate};
    host::unlock_audio();
}
void movie_audio_stop(uint32_t id) {
    host::lock_audio();
    g_movie_voices.erase(id);
    host::unlock_audio();
}
static std::deque<std::pair<uint32_t, Cmd>> g_callbacks;
static int g_out_l = 0x100, g_out_r = 0x100;   /* default output volume */

static void start_header(Channel &ch, uint32_t h) {
    Voice v;
    uint32_t ptr = ld32(h);
    uint8_t enc = ld8(h + 20);
    v.rate = (double)ld32(h + 8) / 65536.0;
    if (v.rate <= 0) v.rate = 22254.5454;
    v.loop_start = ld32(h + 12);
    v.loop_end = ld32(h + 16);
    if (enc == 0x00) {          /* stdSH */
        v.frames = ld32(h + 4);
        v.data = ptr ? ptr : h + 22;
        v.channels = 1;
        v.bits = 8;
    } else if (enc == 0xFF) {   /* extSH */
        v.channels = (int)ld32(h + 4);
        v.frames = ld32(h + 22);
        v.bits = (int16_t)ld16(h + 48);
        v.data = ptr ? ptr : h + 64;
    } else {
        logf(LOG_UNIMPL, "snd: compressed sound header (encode %02X) not supported\n", enc);
        return;
    }
    if (v.channels < 1 || v.channels > 2) v.channels = 1;
    v.active = v.frames > 0;
    ch.voice = v;
}

/* run queued commands until one blocks (a sound playing or a wait) */
static void run_queue(uint32_t key, Channel &ch) {
    while (!ch.voice.active && ch.wait_frames <= 0 && !ch.paused && !ch.queue.empty()) {
        Cmd c = ch.queue.front();
        ch.queue.pop_front();
        switch (c.cmd & 0x7FFF) {
        case 0: break;                                    /* nullCmd */
        case 10: ch.wait_frames = c.p1 * host::kAudioRate / 2000.0; break;   /* waitCmd: half-ms */
        case 13: g_callbacks.push_back({key, c}); break;   /* callBackCmd */
        case 43: ch.amp = c.p1; break;                    /* ampCmd */
        case 46: ch.vol_l = c.p2 & 0xFFFF; ch.vol_r = c.p2 >> 16; break;   /* volumeCmd */
        case 80: case 81: start_header(ch, c.p2); break;  /* soundCmd / bufferCmd */
        case 82: ch.rate_mul = (double)c.p2 / 65536.0; break;   /* rateCmd */
        case 86: ch.rate_mul = (double)c.p2 / 65536.0; break;   /* rateMultiplierCmd */
        default: break;
        }
    }
}

static void immediate(uint32_t key, Channel &ch, const Cmd &c) {
    switch (c.cmd & 0x7FFF) {
    case 3: ch.voice.active = false; ch.wait_frames = 0; break;              /* quietCmd */
    case 4: ch.queue.clear(); break;                                         /* flushCmd */
    case 11: ch.paused = true; break;
    case 12: ch.paused = false; break;
    case 13: g_callbacks.push_back({key, c}); break;
    case 43: ch.amp = c.p1; break;
    case 45: if (c.p2) st16(c.p2, (uint16_t)ch.amp); break;                 /* getAmpCmd */
    case 46: ch.vol_l = c.p2 & 0xFFFF; ch.vol_r = c.p2 >> 16; break;
    case 47: if (c.p2) st32(c.p2, (uint32_t)ch.vol_r << 16 | (uint16_t)ch.vol_l); break;
    case 80: case 81: ch.queue.push_front(c); ch.voice.active = false; break;
    case 82: ch.rate_mul = (double)c.p2 / 65536.0; break;
    case 85: case 87: if (c.p2) st32(c.p2, (uint32_t)(ch.rate_mul * 65536.0)); break;
    case 86: ch.rate_mul = (double)c.p2 / 65536.0; break;
    default: ch.queue.push_front(c); break;
    }
    run_queue(key, ch);
}

static void mix(int16_t *out, int frames) {
    std::vector<int32_t> acc((size_t)frames * 2, 0);
    for (auto &[key, ch] : g_chans) {
        for (int i = 0; i < frames; i++) {
            if (ch.wait_frames > 0) { ch.wait_frames -= 1; if (ch.wait_frames <= 0) run_queue(key, ch); continue; }
            Voice &v = ch.voice;
            if (!v.active || ch.paused) { run_queue(key, ch); if (!v.active) continue; }
            uint32_t f = (uint32_t)v.pos;
            if (f >= v.frames) {
                if (v.loop_end > v.loop_start + 2 && v.loop_end <= v.frames && false) v.pos = v.loop_start;
                else { v.active = false; run_queue(key, ch); continue; }
                f = (uint32_t)v.pos;
            }
            double t = v.pos - f;
            int s[2];
            for (int c = 0; c < 2; c++) {
                int cc = v.channels == 2 ? c : 0;
                auto sample = [&](uint32_t fr) -> int {
                    if (fr >= v.frames) fr = v.frames - 1;
                    if (v.bits == 16) return (int16_t)ld16(v.data + (fr * v.channels + cc) * 2);
                    return ((int)ld8(v.data + fr * v.channels + cc) - 128) << 8;
                };
                s[c] = (int)(sample(f) * (1 - t) + sample(f + 1) * t);
            }
            double gain = ch.amp / 255.0;
            acc[2 * i] += (int32_t)(s[0] * gain * ch.vol_l / 256.0 * g_out_l / 256.0);
            acc[2 * i + 1] += (int32_t)(s[1] * gain * ch.vol_r / 256.0 * g_out_r / 256.0);
            v.pos += v.rate * ch.rate_mul / host::kAudioRate;
        }
    }
    for (auto it = g_movie_voices.begin(); it != g_movie_voices.end();) {
        MovieVoice &v = it->second;
        size_t nframes = v.pcm.size() / v.channels;
        double step = v.rate / host::kAudioRate;
        for (int i = 0; i < frames && (size_t)v.pos < nframes; i++) {
            size_t f = (size_t)v.pos;
            int l = v.pcm[f * v.channels], r = v.pcm[f * v.channels + (v.channels > 1 ? 1 : 0)];
            acc[2 * i] += l * g_out_l / 256;
            acc[2 * i + 1] += r * g_out_r / 256;
            v.pos += step;
        }
        if ((size_t)v.pos >= nframes) it = g_movie_voices.erase(it);
        else ++it;
    }
    cd::mix(acc.data(), frames);
    for (int i = 0; i < frames * 2; i++) out[i] = (int16_t)std::clamp(acc[i], -32768, 32767);
}

void init() { host::set_audio_fill(mix); }

/* deliver completions on the game thread */
void poll(PPCContext *c) {
    static bool busy = false;
    if (busy) return;
    std::deque<std::pair<uint32_t, Cmd>> todo;
    host::lock_audio();
    todo.swap(g_callbacks);
    host::unlock_audio();
    if (todo.empty()) return;
    busy = true;
    static uint32_t reuse = 0;
    if (!reuse) reuse = sys_alloc(8, 4);
    for (auto &[chan, cmd] : todo) {
        uint32_t cb;
        host::lock_audio();
        auto it = g_chans.find(chan);
        cb = it == g_chans.end() ? 0 : it->second.callback;
        host::unlock_audio();
        if (!cb) continue;
        st16(reuse, cmd.cmd); st16(reuse + 2, cmd.p1); st32(reuse + 4, cmd.p2);
        uint32_t args[2] = {chan, reuse};
        guest_call(c, cb, args, 2);
    }
    busy = false;
}

}  // namespace pt::snd

using namespace pt;
using namespace pt::snd;

static Cmd read_cmd(uint32_t a) { return {ld16(a), ld16(a + 2), ld32(a + 4)}; }

HLE(SndNewChannel) {
    /* OSErr SndNewChannel(SndChannelPtr *chan, short synth, long init, SndCallBackUPP userRoutine) */
    uint32_t chp = ld32(ARG(0));
    if (!chp) { chp = guest_alloc(36 + 8 * 128, true); st32(ARG(0), chp); }
    st32(chp + 8, ARG(3));
    st16(chp + 30, 128);
    host::lock_audio();
    Channel ch;
    ch.guest = chp;
    ch.callback = ARG(3);
    g_chans[chp] = ch;
    host::unlock_audio();
    logf(LOG_SND, "SndNewChannel -> %08X (callback %08X)\n", chp, ARG(3));
    RET(0);
}

HLE(SndDisposeChannel) {
    host::lock_audio();
    g_chans.erase(ARG(0));
    host::unlock_audio();
    RET(0);
}

HLE(SndDoCommand) {
    host::lock_audio();
    auto it = g_chans.find(ARG(0));
    if (it == g_chans.end()) { host::unlock_audio(); RET(-205); return; }   /* badChannel */
    Cmd c0 = read_cmd(ARG(1));
    if (it->second.queue.size() >= 128) { host::unlock_audio(); RET(-203); return; }   /* queueFull */
    it->second.queue.push_back(c0);
    run_queue(ARG(0), it->second);
    host::unlock_audio();
    RET(0);
}

HLE(SndDoImmediate) {
    host::lock_audio();
    auto it = g_chans.find(ARG(0));
    if (it == g_chans.end()) { host::unlock_audio(); RET(-205); return; }
    immediate(ARG(0), it->second, read_cmd(ARG(1)));
    host::unlock_audio();
    RET(0);
}

HLE(SndPlay) {
    /* OSErr SndPlay(SndChannelPtr chan, SndListHandle sndHandle, Boolean async) */
    uint32_t h = ARG(1);
    if (!h || !ld32(h)) { RET(-201); return; }
    uint32_t p = ld32(h);
    uint16_t format = ld16(p);
    uint32_t a = p + 2;
    if (format == 1) { uint16_t nmods = ld16(a); a += 2 + 6 * nmods; }
    else a += 2;   /* refCount */
    uint16_t ncmds = ld16(a);
    a += 2;
    uint32_t chan = ARG(0);
    bool temp = false;
    host::lock_audio();
    if (!chan) {
        chan = sys_alloc(36 + 8 * 128, 4);
        Channel ch;
        ch.guest = chan;
        g_chans[chan] = ch;
        temp = true;
    }
    Channel &ch = g_chans[chan];
    for (uint16_t i = 0; i < ncmds; i++) {
        Cmd c0 = read_cmd(a + 8 * i);
        if (c0.cmd & 0x8000) c0.p2 += p;   /* dataOffsetFlag */
        ch.queue.push_back(c0);
    }
    run_queue(chan, ch);
    host::unlock_audio();
    if (temp || !(ARG(2) & 0xFF)) {
        /* synchronous: wait for the channel to drain */
        for (;;) {
            host::lock_audio();
            bool busy = g_chans[chan].voice.active || !g_chans[chan].queue.empty();
            host::unlock_audio();
            if (!busy) break;
            host::pump();
            Sleep(2);
        }
        if (temp) { host::lock_audio(); g_chans.erase(chan); host::unlock_audio(); }
    }
    RET(0);
}

HLE(GetDefaultOutputVolume) { st32(ARG(0), (uint32_t)g_out_r << 16 | (uint16_t)g_out_l); RET(0); }
HLE(SetDefaultOutputVolume) {
    host::lock_audio();
    g_out_l = (int)(ARG(0) & 0xFFFF);
    g_out_r = (int)(ARG(0) >> 16);
    host::unlock_audio();
    RET(0);
}
