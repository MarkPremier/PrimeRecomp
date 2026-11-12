/* Device Manager (.AppleCD) and the Sound Input Manager.
 *
 * On a Power Mac, CD-audio music plays through the CD drive and is routed to
 * the speakers via the sound-input "Internal CD" source with playthrough.
 * Prime Target reads the disc's table of contents through the .AppleCD driver
 * (csCode 100, ReadTOC) and drives playback with the audio csCodes.
 *
 * The TOC comes from cdaudio/toc.tsv (tools/rip_cd_audio.py). Playback state
 * is tracked here; the audio itself is produced by the mixer (sound module).
 */
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "pt/runtime.h"

using namespace pt;

static const int16_t kAppleCDRefNum = -10;

namespace pt::cd {

struct Track { int number; bool audio; uint32_t start; uint32_t length; };
static std::vector<Track> g_tracks;
static uint32_t g_leadout = 0;

static bool g_playing = false, g_paused = false;
static uint32_t g_play_start = 0, g_play_end = 0;   /* absolute frames */

static void load_toc() {
    if (!g_tracks.empty()) return;
    std::string path = data_root() + "\\..\\cdaudio\\toc.tsv";
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        std::istringstream ss(line);
        std::string n, type, start, len;
        ss >> n >> type >> start >> len;
        if (n == "leadout") g_leadout = (uint32_t)std::stoul(start);
        else if (!n.empty()) g_tracks.push_back({std::stoi(n), type == "audio", (uint32_t)std::stoul(start), (uint32_t)std::stoul(len)});
    }
    if (g_tracks.empty()) logf(LOG_UNIMPL, "cd: no TOC at %s (run tools/rip_cd_audio.py); music disabled\n", path.c_str());
}

static uint8_t bcd(unsigned v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }
static unsigned unbcd(uint8_t b) { return (b >> 4) * 10 + (b & 15); }
static void msf(uint32_t frames, uint8_t out[3]) {
    out[0] = bcd(frames / (75 * 60));
    out[1] = bcd((frames / 75) % 60);
    out[2] = bcd(frames % 75);
}

static const Track *track(int n) {
    for (auto &t : g_tracks)
        if (t.number == n) return &t;
    return nullptr;
}

/* csParam position: positioning type in a word, then a 32-bit address */
static uint32_t decode_position(uint16_t type, uint32_t addr) {
    switch (type) {
    case 0: return addr;                                                   /* logical block */
    case 1: return unbcd((uint8_t)(addr >> 16)) * 4500 + unbcd((uint8_t)(addr >> 8)) * 75 + unbcd((uint8_t)addr); /* BCD MSF */
    case 2: { const Track *t = track((int)unbcd((uint8_t)addr)); return t ? t->start : 0; } /* BCD track */
    default: return 0;
    }
}

static int16_t control(uint32_t pb) {
    load_toc();
    int16_t cs = (int16_t)ld16(pb + 26);
    uint32_t p = pb + 28;   /* csParam */
    switch (cs) {
    case 100: { /* ReadTOC */
        uint16_t type = ld16(p);
        if (g_tracks.empty()) return -17; /* controlErr: no disc */
        int first = g_tracks.front().number, last = g_tracks.back().number;
        if (type == 1) {
            st8(p, bcd(first));
            st8(p + 1, bcd(last));
        } else if (type == 2) {
            uint8_t m[3];
            msf(g_leadout, m);
            st8(p, m[0]); st8(p + 1, m[1]); st8(p + 2, m[2]);
        } else if (type == 3) {
            uint32_t buf = ld32(p + 2);
            uint16_t size = ld16(p + 6);
            int start = unbcd(ld8(p + 8));
            uint32_t o = 0;
            for (auto &t : g_tracks) {
                if (t.number < start || o + 4 > size) continue;
                uint8_t m[3];
                msf(t.start, m);
                st8(buf + o, t.audio ? 0x00 : 0x04);
                st8(buf + o + 1, m[0]); st8(buf + o + 2, m[1]); st8(buf + o + 3, m[2]);
                o += 4;
            }
        } else if (type == 4) {
            /* raw Q-channel TOC: a pad byte, then 5-byte entries
             * {control, point, pmin, psec, pframe}: A0 first, A1 last, A2 lead-out, then tracks */
            uint32_t buf = ld32(p + 2);
            uint32_t o = buf + 1;
            auto entry = [&](uint8_t ctl, uint8_t point, uint8_t a, uint8_t b, uint8_t c) {
                st8(o, ctl); st8(o + 1, point); st8(o + 2, a); st8(o + 3, b); st8(o + 4, c); o += 5;
            };
            st8(buf, 0);
            entry(0x04, 0xA0, bcd(first), 0, 0);
            entry(0x00, 0xA1, bcd(last), 0, 0);
            uint8_t m[3];
            msf(g_leadout, m);
            entry(0x00, 0xA2, m[0], m[1], m[2]);
            for (auto &t : g_tracks) {
                msf(t.start, m);
                entry(t.audio ? 0x00 : 0x04, bcd(t.number), m[0], m[1], m[2]);
            }
        } else {
            logf(LOG_UNIMPL, "cd: ReadTOC type %u\n", type);
            return -17;
        }
        return 0;
    }
    case 103: /* AudioTrackSearch: position, then play mode */
        g_play_start = decode_position(ld16(p), ld32(p + 2));
        g_playing = ld16(p + 6) != 0;
        g_paused = false;
        logf(LOG_SND, "cd: track search to frame %u (play=%d)\n", g_play_start, g_playing);
        return 0;
    case 104: { /* AudioPlay: position type, address, stop flag, play mode */
        uint32_t pos = decode_position(ld16(p), ld32(p + 2));
        if (ld16(p + 6)) g_play_end = pos;        /* this call sets the stop address */
        else { g_play_start = pos; g_playing = true; g_paused = false; }
        logf(LOG_SND, "cd: play frame %u..%u\n", g_play_start, g_play_end);
        return 0;
    }
    case 105: g_paused = ld32(p) != 0; return 0;               /* AudioPause */
    case 106: g_play_end = decode_position(ld16(p), ld32(p + 2)); /* AudioStop */
              if (!ld16(p) && !ld32(p + 2)) g_playing = false;
              return 0;
    case 107: { /* AudioStatus */
        st8(p, g_playing ? (g_paused ? 1 : 0) : 3);  /* 0 playing, 1 paused, 3 completed */
        st8(p + 1, 9);                               /* play mode: stereo */
        st8(p + 2, 0);
        uint8_t m[3];
        msf(g_play_start, m);
        st8(p + 3, m[0]); st8(p + 4, m[1]); st8(p + 5, m[2]);
        return 0;
    }
    case 109: return 0;   /* AudioControl (volume) */
    case 112: return 0;   /* ReadAudioVolume / misc */
    default:
        logf(LOG_UNIMPL, "cd: csCode %d\n", cs);
        return 0;
    }
}

}  // namespace pt::cd

HLE(OpenDriver) {
    /* OSErr OpenDriver(ConstStr255Param name, short *drvrRefNum) */
    std::string name = read_pstr(ARG(0));
    if (name == ".AppleCD") {
        st16(ARG(1), (uint16_t)kAppleCDRefNum);
        RET(0);
        return;
    }
    logf(LOG_UNIMPL, "OpenDriver(%s): no such driver\n", name.c_str());
    RET(-35);
}

static int16_t driver_control(uint32_t pb) {
    int16_t ref = (int16_t)ld16(pb + 24);
    int16_t err = ref == kAppleCDRefNum ? cd::control(pb) : -28; /* notOpenErr */
    logf(LOG_SND, "driver control ref %d csCode %d -> %d\n", ref, (int16_t)ld16(pb + 26), err);
    st16(pb + 16, (uint16_t)err);
    return err;
}

HLE(PBControlSync) { RET(driver_control(ARG(0))); }
HLE(PBControlAsync) { RET(driver_control(ARG(0))); }

/* ======================= Sound Input Manager ======================= */
static int16_t g_input_source = 2;     /* 1 = Microphone, 2 = Internal CD */
static int16_t g_playthrough = 7;

HLE(SPBOpenDevice) {
    /* OSErr SPBOpenDevice(ConstStr255Param deviceName, short permission, long *inRefNum) */
    st32(ARG(2), 1);
    RET(0);
}
HLE(SPBCloseDevice) { RET(0); }

HLE(SPBGetDeviceInfo) {
    /* OSErr SPBGetDeviceInfo(long inRefNum, OSType infoType, void *infoData) */
    switch (ARG(1)) {
    case 'snam': { /* siInputSourceNames: handle to an STR#-format list */
        const char *names[] = {"Microphone", "Internal CD"};
        uint32_t h = handle_new(64, true);
        uint32_t p = ld32(h);
        st16(p, 2);
        uint32_t o = p + 2;
        for (auto *n : names) { write_pstr(o, n); o += 1 + (uint32_t)strlen(n); }
        handle_set_size(h, o - p);
        st32(ARG(2), h);
        RET(0);
        return;
    }
    case 'sour': st16(ARG(2), (uint16_t)g_input_source); RET(0); return;
    case 'plth': st16(ARG(2), (uint16_t)g_playthrough); RET(0); return;
    default:
        logf(LOG_UNIMPL, "SPBGetDeviceInfo %08X\n", ARG(1));
        RET(-231); /* siUnknownInfoType */
    }
}

HLE(SPBSetDeviceInfo) {
    switch (ARG(1)) {
    case 'sour': g_input_source = (int16_t)ld16(ARG(2)); RET(0); return;
    case 'plth': g_playthrough = (int16_t)ld16(ARG(2)); RET(0); return;
    default:
        logf(LOG_UNIMPL, "SPBSetDeviceInfo %08X\n", ARG(1));
        RET(-231);
    }
}
