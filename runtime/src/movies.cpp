/* QuickTime Movie Toolbox (the 17 calls Prime Target uses).
 *
 * Movies are parsed from their 'moov' atom (the resource-fork 'moov'
 * resource, or a top-level atom in the data fork), video is Cinepak, audio is
 * uncompressed ('twos' 16-bit big-endian or 'raw ' 8-bit offset binary).
 * Playback follows the host clock: MoviesTask decodes every frame due by now
 * and draws the latest into the movie's GWorld/box with CopyBits.
 */
#include <windows.h>

#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "pt/cinepak.h"
#include "pt/qd.h"
#include "pt/runtime.h"
#include "pt/vfs.h"

namespace pt::snd {
void movie_audio_start(uint32_t id, std::vector<int16_t> pcm, int channels, double rate, double offset_s);
void movie_audio_stop(uint32_t id);
}

namespace pt::qt {

using namespace pt::qd;

struct Sample { uint32_t offset, size; double time, duration; };
struct Movie {
    std::wstring path;          /* data fork */
    double duration = 0;        /* seconds */
    int width = 0, height = 0;
    Rect natural{}, box{};
    std::vector<Sample> video;
    std::vector<int16_t> audio;
    int channels = 1;
    double rate = 22050;
    uint32_t port = 0, gdh = 0;
    cvid::Decoder dec;
    size_t next = 0;            /* next video sample to decode */
    bool playing = false, decoded_any = false;
    double start_host = 0, pos = 0;   /* pos: movie time when not playing */
    uint32_t pixels = 0;        /* guest 32-bit frame buffer */
};
static std::map<uint32_t, std::unique_ptr<Movie>> g_movies;   /* guest Movie handle -> state */

static double now_s() {
    static LARGE_INTEGER f, s;
    if (!f.QuadPart) { QueryPerformanceFrequency(&f); QueryPerformanceCounter(&s); }
    LARGE_INTEGER n;
    QueryPerformanceCounter(&n);
    return (double)(n.QuadPart - s.QuadPart) / (double)f.QuadPart;
}

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

struct Atom { uint32_t type; const uint8_t *data; size_t size; };
static std::vector<Atom> children(const uint8_t *d, size_t n) {
    std::vector<Atom> out;
    size_t p = 0;
    while (p + 8 <= n) {
        uint32_t sz = be32(d + p), type = be32(d + p + 4);
        if (sz < 8 || p + sz > n) break;
        out.push_back({type, d + p + 8, sz - 8});
        p += sz;
    }
    return out;
}
static const Atom *find(const std::vector<Atom> &v, uint32_t t) {
    for (auto &a : v) if (a.type == t) return &a;
    return nullptr;
}

struct Track {
    uint32_t kind = 0;          /* 'vide' / 'soun' */
    uint32_t format = 0;
    double timescale = 600;
    int width = 0, height = 0, channels = 1, bits = 8;
    double rate = 22050;
    std::vector<uint32_t> chunk_off, sizes;
    uint32_t const_size = 0, sample_count = 0;
    std::vector<std::pair<uint32_t, uint32_t>> stsc;   /* first chunk, samples per chunk */
    std::vector<std::pair<uint32_t, uint32_t>> stts;   /* count, duration */
};

static Track parse_track(const Atom &trak) {
    Track t;
    auto tk = children(trak.data, trak.size);
    const Atom *mdia = find(tk, 'mdia');
    if (!mdia) return t;
    auto md = children(mdia->data, mdia->size);
    if (const Atom *mdhd = find(md, 'mdhd')) t.timescale = be32(mdhd->data + 12);
    if (const Atom *hdlr = find(md, 'hdlr')) t.kind = be32(hdlr->data + 8);
    const Atom *minf = find(md, 'minf');
    if (!minf) return t;
    auto mi = children(minf->data, minf->size);
    const Atom *stbl = find(mi, 'stbl');
    if (!stbl) return t;
    auto st = children(stbl->data, stbl->size);
    if (const Atom *a = find(st, 'stsd')) {
        const uint8_t *e = a->data + 8;
        t.format = be32(e + 4);
        if (t.kind == 'vide') { t.width = be16(e + 32); t.height = be16(e + 34); }
        else { t.channels = be16(e + 24); t.bits = be16(e + 26); t.rate = be32(e + 32) / 65536.0; }
    }
    if (const Atom *a = find(st, 'stts')) {
        uint32_t n = be32(a->data + 4);
        for (uint32_t i = 0; i < n; i++) t.stts.push_back({be32(a->data + 8 + 8 * i), be32(a->data + 12 + 8 * i)});
    }
    if (const Atom *a = find(st, 'stsc')) {
        uint32_t n = be32(a->data + 4);
        for (uint32_t i = 0; i < n; i++) t.stsc.push_back({be32(a->data + 8 + 12 * i), be32(a->data + 12 + 12 * i)});
    }
    if (const Atom *a = find(st, 'stsz')) {
        t.const_size = be32(a->data + 4);
        t.sample_count = be32(a->data + 8);
        if (!t.const_size)
            for (uint32_t i = 0; i < t.sample_count; i++) t.sizes.push_back(be32(a->data + 12 + 4 * i));
    }
    if (const Atom *a = find(st, 'stco')) {
        uint32_t n = be32(a->data + 4);
        for (uint32_t i = 0; i < n; i++) t.chunk_off.push_back(be32(a->data + 8 + 4 * i));
    }
    return t;
}

static uint32_t samples_in_chunk(const Track &t, uint32_t chunk /* 1-based */) {
    uint32_t spc = 1;
    for (auto &e : t.stsc) if (e.first <= chunk) spc = e.second;
    return spc;
}

static bool load(Movie &m, const std::vector<uint8_t> &moov) {
    auto top = children(moov.data(), moov.size());
    const Atom *mv = find(top, 'moov');
    std::vector<Atom> kids = mv ? children(mv->data, mv->size) : top;
    if (const Atom *mvhd = find(kids, 'mvhd')) {
        double ts = be32(mvhd->data + 12);
        m.duration = ts ? be32(mvhd->data + 16) / ts : 0;
    }
    FILE *f = _wfopen(m.path.c_str(), L"rb");
    if (!f) return false;
    for (auto &k : kids) {
        if (k.type != 'trak') continue;
        Track t = parse_track(k);
        if (t.kind == 'vide' && t.format == 'cvid') {
            m.width = t.width;
            m.height = t.height;
            /* per-sample offsets, sizes and times */
            uint32_t s = 0;
            for (uint32_t c = 1; c <= t.chunk_off.size(); c++) {
                uint32_t off = t.chunk_off[c - 1], n = samples_in_chunk(t, c);
                for (uint32_t i = 0; i < n && s < t.sample_count; i++, s++) {
                    uint32_t sz = t.const_size ? t.const_size : t.sizes[s];
                    m.video.push_back({off, sz, 0, 0});
                    off += sz;
                }
            }
            double time = 0;
            size_t idx = 0;
            for (auto &e : t.stts)
                for (uint32_t i = 0; i < e.first && idx < m.video.size(); i++, idx++) {
                    m.video[idx].time = time / t.timescale;
                    m.video[idx].duration = e.second / t.timescale;
                    time += e.second;
                }
        } else if (t.kind == 'soun' && (t.format == 'twos' || t.format == 'raw ' || t.format == 'sowt')) {
            m.channels = t.channels;
            m.rate = t.rate;
            int bpf = t.channels * (t.bits / 8);
            for (uint32_t c = 1; c <= t.chunk_off.size(); c++) {
                uint32_t frames = samples_in_chunk(t, c);
                std::vector<uint8_t> raw((size_t)frames * bpf);
                _fseeki64(f, t.chunk_off[c - 1], SEEK_SET);
                size_t got = fread(raw.data(), 1, raw.size(), f);
                for (size_t i = 0; i + (t.bits / 8) <= got; i += t.bits / 8) {
                    int16_t v;
                    if (t.bits == 16) v = t.format == 'sowt' ? (int16_t)(raw[i] | raw[i + 1] << 8) : (int16_t)(raw[i] << 8 | raw[i + 1]);
                    else v = (int16_t)((int)raw[i] - 128) << 8;
                    m.audio.push_back(v);
                }
            }
        } else if (t.kind == 'vide') {
            logf(LOG_UNIMPL, "movie: unsupported video codec %08X\n", t.format);
        }
    }
    fclose(f);
    m.natural = {0, 0, (int16_t)m.height, (int16_t)m.width};
    m.box = m.natural;
    m.dec.reset(m.width, m.height);
    if (m.width && m.height) m.pixels = guest_alloc((uint32_t)m.width * m.height * 4, true);
    logf(LOG_GFX, "movie: %dx%d, %zu frames, %.1f s, audio %zu samples @ %.0f Hz\n", m.width, m.height, m.video.size(),
         m.duration, m.audio.size(), m.rate);
    return true;
}

static double movie_time(const Movie &m) { return m.playing ? m.pos + (now_s() - m.start_host) : m.pos; }

static void draw(Movie &m) {
    if (!m.pixels || !m.port || !m.decoded_any) return;
    auto &fr = m.dec.frame();
    for (size_t i = 0; i < fr.size(); i++) st32(m.pixels + 4 * (uint32_t)i, fr[i]);
    Surface s;
    s.base = m.pixels;
    s.row_bytes = (uint32_t)m.width * 4;
    s.depth = 32;
    s.bounds = m.natural;
    s.color_port = true;
    uint32_t saved = current_port();
    set_current_port(m.port);
    Surface d = surface_of_port(m.port);
    copy_bits(s, d, m.natural, m.box, 64 /* ditherCopy */, port_clip(m.port));
    set_current_port(saved);
}

static void task(Movie &m) {
    if (m.video.empty()) return;
    double t = movie_time(m);
    bool any = false;
    std::vector<uint8_t> buf;
    FILE *f = nullptr;
    while (m.next < m.video.size() && m.video[m.next].time <= t) {
        const Sample &s = m.video[m.next++];
        if (!f) f = _wfopen(m.path.c_str(), L"rb");
        if (!f) break;
        buf.resize(s.size);
        _fseeki64(f, s.offset, SEEK_SET);
        if (fread(buf.data(), 1, s.size, f) == s.size) { m.dec.decode(buf.data(), buf.size()); any = true; }
    }
    if (f) fclose(f);
    if (any) { m.decoded_any = true; draw(m); }
}

static Movie *get(uint32_t h) {
    auto it = g_movies.find(h);
    return it == g_movies.end() ? nullptr : it->second.get();
}

/* movie files opened with OpenMovieFile: resRefNum -> host path */
static std::map<int16_t, std::wstring> g_open;
static int16_t g_next_ref = 0x7000;

}  // namespace pt::qt

using namespace pt;
using namespace pt::qt;

HLE(EnterMovies) { RET(0); }
HLE(ExitMovies) { (void)c; }

HLE(OpenMovieFile) {
    /* OSErr OpenMovieFile(const FSSpec *fileSpec, short *resRefNum, SInt8 permission) */
    vfs::Spec s = vfs::read_fsspec(ARG(0));
    vfs::Entry e;
    if (!vfs::lookup(s.vref, s.parent, s.name, e)) { RET(-43); return; }
    int16_t ref = g_next_ref++;
    g_open[ref] = e.host.wstring();
    st16(ARG(1), (uint16_t)ref);
    logf(LOG_GFX, "OpenMovieFile %s -> %d\n", e.host.u8string().c_str(), ref);
    RET(0);
}
HLE(CloseMovieFile) { g_open.erase((int16_t)ARG(0)); RET(0); }

HLE(NewMovieFromFile) {
    /* OSErr NewMovieFromFile(Movie *theMovie, short resRefNum, short *resId, StringPtr resName,
     *                        short newMovieFlags, Boolean *dataRefWasChanged) */
    auto it = g_open.find((int16_t)ARG(1));
    if (it == g_open.end()) { RET(-2009); return; }   /* invalidMovie */
    auto m = std::make_unique<Movie>();
    m->path = it->second;
    std::vector<uint8_t> moov;
    /* prefer the resource-fork 'moov' resource */
    {
        std::wstring rp = m->path + L".rsrc";
        if (FILE *f = _wfopen(rp.c_str(), L"rb")) {
            std::vector<uint8_t> r;
            fseek(f, 0, SEEK_END);
            r.resize((size_t)ftell(f));
            fseek(f, 0, SEEK_SET);
            if (!r.empty()) fread(r.data(), 1, r.size(), f);
            fclose(f);
            if (r.size() > 16) {
                uint32_t doff = be32(&r[0]), moff = be32(&r[4]);
                const uint8_t *map = &r[moff];
                uint16_t tl = be16(map + 24);
                int nt = (int16_t)be16(map + tl) + 1;
                for (int i = 0; i < nt && moov.empty(); i++) {
                    const uint8_t *te = map + tl + 2 + 8 * i;
                    if (be32(te) != 'moov') continue;
                    const uint8_t *ref = map + tl + be16(te + 6);
                    uint32_t off = (uint32_t)ref[5] << 16 | ref[6] << 8 | ref[7];
                    uint32_t len = be32(&r[doff + off]);
                    moov.assign(&r[doff + off + 4], &r[doff + off + 4 + len]);
                    std::vector<uint8_t> wrapped(8);
                    uint32_t sz = (uint32_t)moov.size() + 8;
                    wrapped = {(uint8_t)(sz >> 24), (uint8_t)(sz >> 16), (uint8_t)(sz >> 8), (uint8_t)sz, 'm', 'o', 'o', 'v'};
                    wrapped.insert(wrapped.end(), moov.begin(), moov.end());
                    moov = wrapped;
                }
            }
        }
    }
    if (moov.empty()) {
        /* self-contained movie: find the top-level 'moov' atom in the data fork */
        if (FILE *f = _wfopen(m->path.c_str(), L"rb")) {
            uint64_t pos = 0;
            uint8_t h[8];
            while (fread(h, 1, 8, f) == 8) {
                uint32_t sz = be32(h), type = be32(h + 4);
                if (sz < 8) break;
                if (type == 'moov') {
                    moov.resize(sz);
                    memcpy(moov.data(), h, 8);
                    fread(moov.data() + 8, 1, sz - 8, f);
                    break;
                }
                pos += sz;
                _fseeki64(f, (int64_t)pos, SEEK_SET);
            }
            fclose(f);
        }
    }
    if (moov.empty() || !load(*m, moov)) { RET(-2009); return; }
    uint32_t h = handle_new(16, true);
    g_movies[h] = std::move(m);
    st32(ARG(0), h);
    if (ARG(2)) st16(ARG(2), 128);
    if (ARG(5)) st8(ARG(5), 0);
    RET(0);
}

HLE(DisposeMovie) {
    Movie *m = get(ARG(0));
    if (!m) return;
    snd::movie_audio_stop(ARG(0));
    if (m->pixels) guest_free(m->pixels);
    g_movies.erase(ARG(0));
    handle_dispose(ARG(0));
}

HLE(SetMovieGWorld) {
    Movie *m = get(ARG(0));
    if (!m) return;
    m->port = ARG(1) ? ARG(1) : current_port();
    m->gdh = ARG(2);
}
HLE(GetMovieBox) { Movie *m = get(ARG(0)); if (m) write_rect(ARG(1), m->box); }
HLE(SetMovieBox) { Movie *m = get(ARG(0)); if (m) m->box = read_rect(ARG(1)); }
HLE(GetMovieColorTable) { st32(ARG(1), 0); RET(0); }
HLE(GoToBeginningOfMovie) {
    Movie *m = get(ARG(0));
    if (!m) return;
    m->pos = 0;
    m->next = 0;
    m->dec.reset(m->width, m->height);
    m->decoded_any = false;
    if (m->playing) {
        m->start_host = now_s();
        snd::movie_audio_start(ARG(0), m->audio, m->channels, m->rate, 0);
    }
}
HLE(PrerollMovie) { RET(0); }
HLE(StartMovie) {
    Movie *m = get(ARG(0));
    if (!m || m->playing) return;
    m->playing = true;
    m->start_host = now_s();
    snd::movie_audio_start(ARG(0), m->audio, m->channels, m->rate, m->pos);
}
HLE(StopMovie) {
    Movie *m = get(ARG(0));
    if (!m || !m->playing) return;
    m->pos = movie_time(*m);
    m->playing = false;
    snd::movie_audio_stop(ARG(0));
}
HLE(MoviesTask) {
    if (ARG(0)) { if (Movie *m = get(ARG(0))) task(*m); }
    else for (auto &[h, m] : g_movies) task(*m);
}
HLE(UpdateMovie) { Movie *m = get(ARG(0)); if (m) draw(*m); RET(0); }
HLE(IsMovieDone) {
    Movie *m = get(ARG(0));
    RET(!m || movie_time(*m) >= m->duration);
}
