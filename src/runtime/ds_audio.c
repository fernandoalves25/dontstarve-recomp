/*
 * ds_audio.c - sound for the FMOD shim: the game's own FSB5 banks, decoded
 * and mixed on the host (DS_AUDIO=sdl; silent otherwise).
 *
 *  - Banks: every data/sound/*.fsb is indexed (FSB5, Vorbis; see
 *    tools/audio/fsb5.py for the layout) by wave name.
 *  - Events -> waves: by name, until the FEV event tables are parsed. An event
 *    "group/leaf" plays a wave named leaf, leaf_N, or group_leaf(_N), picked
 *    at random each start -- the way the banks are named.
 *  - Vorbis: FSB5 drops the three Vorbis headers and keeps the CRC32 of the
 *    setup header. They are rebuilt with libvorbis's own encoder (the CRC
 *    picks rate x channels x quality) and cached in $DS_HOME_DIR.
 *  - Mixer: SDL audio, 44.1 kHz stereo, 32 voices; volume, 3D distance
 *    attenuation and pan from the listener, loops for *_LP waves.
 * Everything here runs under the machine lock except the SDL callback, which
 * only reads voices under g_mix (a host mutex).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <math.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <pthread.h>
#include <zlib.h>
#include <vorbis/codec.h>
#include <vorbis/vorbisenc.h>
#include "ds_hle.h"
#ifdef DS_SDL
#include <SDL2/SDL.h>
#endif

/* ===================== FSB5 index */
typedef struct {
    char name[64];
    uint16_t bank, channels;
    uint32_t rate, samples, crc, offset, size;     /* offset/size: Vorbis packets in the bank file */
    int16_t *pcm; uint32_t frames;                 /* decoded on first use (interleaved) */
} wave;
typedef struct { char path[512]; uint8_t *data; size_t len; } bank;
static wave *g_waves; static int g_nwaves;
static bank g_banks[96]; static int g_nbanks;
static int g_indexed;

static const uint32_t k_rates[16] = { 0, 8000, 11000, 11025, 16000, 22050, 24000, 32000, 44100, 48000 };

static void index_bank(const char *path) {
    /* mapped, not read: 150 MB of banks on a 1 GB handheld are paged in as played */
    int fd = open(path, O_RDONLY); if (fd < 0) return;
    struct stat st; if (fstat(fd, &st) || st.st_size < 64) { close(fd); return; }
    long n = (long)st.st_size;
    uint8_t *d = (uint8_t *)mmap(NULL, (size_t)n, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (d == MAP_FAILED) return;
    if (memcmp(d, "FSB5", 4)) { munmap(d, (size_t)n); return; }
    uint32_t ver, ns, shs, nts, ds, mode;
    memcpy(&ver, d + 4, 4); memcpy(&ns, d + 8, 4); memcpy(&shs, d + 12, 4);
    memcpy(&nts, d + 16, 4); memcpy(&ds, d + 20, 4); memcpy(&mode, d + 24, 4);
    if (mode != 15 || g_nbanks >= 96) { munmap(d, (size_t)n); return; }     /* Vorbis banks only */
    int b = g_nbanks++;
    snprintf(g_banks[b].path, sizeof g_banks[b].path, "%s", path);
    g_banks[b].data = d; g_banks[b].len = (size_t)n;
    uint32_t hdr = ver == 1 ? 60 : 64, pos = hdr, base = (uint32_t)g_nwaves;
    g_waves = (wave *)realloc(g_waves, sizeof(wave) * (size_t)(g_nwaves + (int)ns));
    for (uint32_t i = 0; i < ns; i++) {
        uint64_t raw; memcpy(&raw, d + pos, 8); pos += 8;
        wave *w = &g_waves[g_nwaves++]; memset(w, 0, sizeof *w);
        int more = raw & 1;
        w->bank = (uint16_t)b; w->rate = k_rates[(raw >> 1) & 0xF] ? k_rates[(raw >> 1) & 0xF] : 44100;
        w->channels = (uint16_t)(((raw >> 5) & 1) + 1);
        w->offset = (uint32_t)(((raw >> 6) & 0x0FFFFFFF) * 16); w->samples = (uint32_t)((raw >> 34) & 0x3FFFFFFF);
        while (more) {
            uint32_t c; memcpy(&c, d + pos, 4); pos += 4;
            more = c & 1; uint32_t size = (c >> 1) & 0xFFFFFF, typ = (c >> 25) & 0x7F;
            if (typ == 1) w->channels = d[pos];
            else if (typ == 2) memcpy(&w->rate, d + pos, 4);
            else if (typ == 11) memcpy(&w->crc, d + pos, 4);
            pos += size;
        }
    }
    uint32_t names = hdr + shs, data = names + nts;
    for (uint32_t i = 0; i < ns; i++) {
        wave *w = &g_waves[base + i];
        if (nts) { uint32_t o; memcpy(&o, d + names + 4 * i, 4); snprintf(w->name, sizeof w->name, "%s", (const char *)d + names + o); }
        uint32_t end = i + 1 < ns ? g_waves[base + i + 1].offset : ds;
        w->size = end - w->offset; w->offset += data;
    }
}
static void build_index(void) {
    g_indexed = 1;
    char host[512];
    ds_map_path("Z:\\dontstarve\\data\\sound", host, sizeof host);
    DIR *dir = opendir(host); if (!dir) { fprintf(stderr, "[audio] no sound folder at %s\n", host); return; }
    struct dirent *e;
    while ((e = readdir(dir))) {
        size_t l = strlen(e->d_name);
        if (l > 4 && !strcasecmp(e->d_name + l - 4, ".fsb")) {
            char p[1024]; snprintf(p, sizeof p, "%s/%s", host, e->d_name); index_bank(p);
        }
    }
    closedir(dir);
    fprintf(stderr, "[audio] indexed %d waves in %d banks\n", g_nwaves, g_nbanks);
}

/* ===================== event name -> waves */
static int name_match(const char *wave_name, const char *stem) {   /* stem or stem_N, any case */
    size_t n = strlen(stem);
    if (strncasecmp(wave_name, stem, n)) return 0;
    const char *r = wave_name + n;
    if (!*r) return 1;
    if (*r != '_' || !r[1]) return 0;
    for (r++; *r; r++) if (!isdigit((unsigned char)*r)) return 0;
    return 1;
}
typedef struct { int n; int idx[24]; int loop; } evmap;
static void map_event(const char *path, evmap *m) {
    m->n = 0; m->loop = 0;
    if (!g_indexed) build_index();
    char p[256]; snprintf(p, sizeof p, "%s", path);
    char *leaf = strrchr(p, '/'); if (!leaf) leaf = p; else *leaf++ = 0;
    char *grp = strrchr(p, '/'); grp = grp ? grp + 1 : p;
    char stems[3][160];
    snprintf(stems[0], sizeof stems[0], "%s", leaf);
    snprintf(stems[1], sizeof stems[1], "%s_%s", grp, leaf);
    snprintf(stems[2], sizeof stems[2], "%s%s", grp, leaf);
    for (int s = 0; s < 3 && m->n == 0; s++)
        for (int i = 0; i < g_nwaves && m->n < 24; i++)
            if (name_match(g_waves[i].name, stems[s])) m->idx[m->n++] = i;
    m->loop = strstr(leaf, "LP") != NULL || strcasestr(leaf, "amb") != NULL || strcasestr(path, "music") != NULL;
}

/* ===================== Vorbis headers (rebuilt with libvorbis's encoder) */
typedef struct { uint32_t crc; int channels; uint8_t *ident, *setup; long nident, nsetup; } vsetup;
static vsetup g_vs[32]; static int g_nvs;
static void vs_cache_path(char *out, size_t cap) {
    const char *h = getenv("DS_HOME_DIR"); snprintf(out, cap, "%s/vorbis_setups.bin", h && *h ? h : "build/home");
}
static void vs_add(uint32_t crc, int ch, const uint8_t *id, long nid, const uint8_t *st, long nst) {
    if (g_nvs >= 32) return;
    vsetup *v = &g_vs[g_nvs++];
    v->crc = crc; v->channels = ch; v->nident = nid; v->nsetup = nst;
    v->ident = (uint8_t *)malloc((size_t)nid); memcpy(v->ident, id, (size_t)nid);
    v->setup = (uint8_t *)malloc((size_t)nst); memcpy(v->setup, st, (size_t)nst);
}
static void vs_load_cache(void) {
    char p[600]; vs_cache_path(p, sizeof p);
    FILE *f = fopen(p, "rb"); if (!f) return;
    uint32_t crc; int32_t ch, nid, nst;
    while (fread(&crc, 4, 1, f) == 1 && fread(&ch, 4, 1, f) == 1 && fread(&nid, 4, 1, f) == 1 && fread(&nst, 4, 1, f) == 1) {
        uint8_t *id = malloc((size_t)nid), *st = malloc((size_t)nst);
        if (fread(id, 1, (size_t)nid, f) != (size_t)nid || fread(st, 1, (size_t)nst, f) != (size_t)nst) { free(id); free(st); break; }
        vs_add(crc, ch, id, nid, st, nst); free(id); free(st);
    }
    fclose(f);
}
static void vs_save(const vsetup *v) {
    char p[600]; vs_cache_path(p, sizeof p);
    FILE *f = fopen(p, "ab"); if (!f) return;
    int32_t ch = v->channels, nid = (int32_t)v->nident, nst = (int32_t)v->nsetup;
    fwrite(&v->crc, 4, 1, f); fwrite(&ch, 4, 1, f); fwrite(&nid, 4, 1, f); fwrite(&nst, 4, 1, f);
    fwrite(v->ident, 1, (size_t)nid, f); fwrite(v->setup, 1, (size_t)nst, f);
    fclose(f);
}
static const vsetup *vs_find(uint32_t crc, int channels) {
    static int loaded, *misses_n; static uint32_t misses[64]; static int nmiss;
    (void)misses_n;
    if (!loaded) { loaded = 1; vs_load_cache(); }
    for (int i = 0; i < g_nvs; i++) if (g_vs[i].crc == crc) return &g_vs[i];
    for (int i = 0; i < nmiss; i++) if (misses[i] == crc) return NULL;
    static const int rates[] = { 44100, 48000, 32000, 22050, 24000, 16000, 11025, 11000, 8000 };
    for (size_t r = 0; r < sizeof rates / sizeof rates[0]; r++)
        for (int q = -10; q <= 100; q++) {
            vorbis_info vi; vorbis_info_init(&vi);
            if (vorbis_encode_init_vbr(&vi, channels, rates[r], (float)q / 100.0f)) { vorbis_info_clear(&vi); continue; }
            vorbis_comment vc; vorbis_comment_init(&vc);
            vorbis_dsp_state vd; vorbis_analysis_init(&vd, &vi);
            ogg_packet id, cm, st;
            vorbis_analysis_headerout(&vd, &vc, &id, &cm, &st);
            uint32_t c = (uint32_t)crc32(0, st.packet, (uInt)st.bytes);
            if (c == crc) vs_add(crc, channels, id.packet, id.bytes, st.packet, st.bytes);
            vorbis_dsp_clear(&vd); vorbis_comment_clear(&vc); vorbis_info_clear(&vi);
            if (c == crc) { vs_save(&g_vs[g_nvs - 1]); return &g_vs[g_nvs - 1]; }
        }
    if (nmiss < 64) misses[nmiss++] = crc;
    fprintf(stderr, "[audio] no Vorbis setup header for CRC %08X (%d ch): those waves stay silent\n", crc, channels);
    return NULL;
}

/* Decode a wave to interleaved int16 (cached). 0 on failure. */
static int decode(wave *w) {
    if (w->pcm) return 1;
    const vsetup *v = vs_find(w->crc, w->channels);
    if (!v || v->channels != w->channels) return 0;
    uint8_t ident[64]; long nid = v->nident < 64 ? v->nident : 64;
    memcpy(ident, v->ident, (size_t)nid);
    memcpy(ident + 12, &w->rate, 4);                     /* the encoder ran at a rate of the same family */
    vorbis_info vi; vorbis_comment vc; vorbis_info_init(&vi); vorbis_comment_init(&vc);
    ogg_packet op; memset(&op, 0, sizeof op);
    op.packet = ident; op.bytes = nid; op.b_o_s = 1;
    if (vorbis_synthesis_headerin(&vi, &vc, &op)) goto fail;
    oggpack_buffer ob; oggpack_writeinit(&ob);          /* an empty comment header */
    oggpack_write(&ob, 3, 8); for (const char *s = "vorbis"; *s; s++) oggpack_write(&ob, (unsigned long)*s, 8);
    oggpack_write(&ob, 0, 32); oggpack_write(&ob, 0, 32); oggpack_write(&ob, 1, 1);
    op.packet = oggpack_get_buffer(&ob); op.bytes = oggpack_bytes(&ob); op.b_o_s = 0;
    int bad = vorbis_synthesis_headerin(&vi, &vc, &op);
    oggpack_writeclear(&ob);
    if (bad) goto fail;
    op.packet = v->setup; op.bytes = v->nsetup;
    if (vorbis_synthesis_headerin(&vi, &vc, &op)) goto fail;
    vorbis_dsp_state vd; vorbis_block vb;
    if (vorbis_synthesis_init(&vd, &vi)) goto fail;
    vorbis_block_init(&vd, &vb);
    size_t cap = (size_t)(w->samples ? w->samples : 4096) + 4096, n = 0;
    int16_t *pcm = (int16_t *)malloc(cap * (size_t)w->channels * 2);
    const uint8_t *d = g_banks[w->bank].data + w->offset, *end = d + w->size;
    long pno = 3;
    while (d + 2 <= end) {
        uint16_t len; memcpy(&len, d, 2); d += 2;
        if (!len || d + len > end) break;
        op.packet = (unsigned char *)d; op.bytes = len; op.packetno = pno++; op.granulepos = -1;
        d += len;
        if (vorbis_synthesis(&vb, &op) == 0) vorbis_synthesis_blockin(&vd, &vb);
        float **out; int got;
        while ((got = vorbis_synthesis_pcmout(&vd, &out)) > 0) {
            if (n + (size_t)got > cap) { cap = (n + (size_t)got) * 2; pcm = (int16_t *)realloc(pcm, cap * (size_t)w->channels * 2); }
            for (int i = 0; i < got; i++)
                for (int c = 0; c < w->channels; c++) {
                    float s = out[c][i] * 32767.0f;
                    pcm[(n + (size_t)i) * w->channels + (size_t)c] = (int16_t)(s > 32767.0f ? 32767 : s < -32768.0f ? -32768 : s);
                }
            n += (size_t)got;
            vorbis_synthesis_read(&vd, got);
        }
    }
    vorbis_block_clear(&vb); vorbis_dsp_clear(&vd);
    vorbis_comment_clear(&vc); vorbis_info_clear(&vi);
    if (w->samples && n > w->samples) n = w->samples;
    w->pcm = pcm; w->frames = (uint32_t)n;
    return n > 0;
fail:
    vorbis_comment_clear(&vc); vorbis_info_clear(&vi);
    return 0;
}

/* ===================== voices + mixer */
#define NVOICE 32
#define OUT_RATE 44100
typedef struct {
    int active, loop; uint32_t ev;               /* ev: event handle that owns it */
    const wave *w; double pos, step;
    float vol, gain_l, gain_r;
    float x, y, z; int has3d;
} voice;
static voice g_voice[NVOICE];
static pthread_mutex_t g_mix = PTHREAD_MUTEX_INITIALIZER;
static float g_lx, g_ly, g_lz;                   /* listener */
static float g_master = 1.0f;
static int g_audio_on = -1;

static void spatialize(voice *v) {
    float l = 1.0f, r = 1.0f, att = 1.0f;
    if (v->has3d) {
        float dx = v->x - g_lx, dz = v->z - g_lz, d = sqrtf(dx * dx + dz * dz);
        att = d < 4.0f ? 1.0f : d > 40.0f ? 0.0f : 1.0f - (d - 4.0f) / 36.0f;
        float pan = d > 0.01f ? dx / (d + 6.0f) : 0.0f;            /* -1 .. 1 */
        l = pan > 0 ? 1.0f - pan : 1.0f; r = pan < 0 ? 1.0f + pan : 1.0f;
    }
    v->gain_l = v->vol * att * l * g_master; v->gain_r = v->vol * att * r * g_master;
}
#ifdef DS_SDL
static void mix_cb(void *ud, Uint8 *stream, int len) {
    (void)ud;
    int16_t *out = (int16_t *)stream; int frames = len / 4;
    static float acc[8192];
    if (frames > 4096) frames = 4096;
    memset(acc, 0, sizeof(float) * (size_t)frames * 2);
    pthread_mutex_lock(&g_mix);
    for (int k = 0; k < NVOICE; k++) {
        voice *v = &g_voice[k]; if (!v->active) continue;
        const wave *w = v->w;
        for (int i = 0; i < frames; i++) {
            uint32_t p = (uint32_t)v->pos;
            if (p >= w->frames) { if (v->loop) { v->pos -= w->frames; p = (uint32_t)v->pos; } else { v->active = 0; break; } }
            float sl, sr;
            if (w->channels == 2) { sl = w->pcm[2 * p]; sr = w->pcm[2 * p + 1]; } else sl = sr = w->pcm[p];
            acc[2 * i] += sl * v->gain_l; acc[2 * i + 1] += sr * v->gain_r;
            v->pos += v->step;
        }
    }
    pthread_mutex_unlock(&g_mix);
    for (int i = 0; i < frames * 2; i++) { float s = acc[i]; out[i] = (int16_t)(s > 32767 ? 32767 : s < -32768 ? -32768 : s); }
}
#endif
static int audio_on(void) {
    if (g_audio_on >= 0) return g_audio_on;
    g_audio_on = 0;
#ifdef DS_SDL
    const char *a = getenv("DS_AUDIO");
    if (!a || strcmp(a, "sdl")) return 0;
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) { fprintf(stderr, "[audio] SDL audio: %s\n", SDL_GetError()); return 0; }
    SDL_AudioSpec want, have; memset(&want, 0, sizeof want);
    want.freq = OUT_RATE; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 1024; want.callback = mix_cb;
    SDL_AudioDeviceID dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!dev) { fprintf(stderr, "[audio] open: %s\n", SDL_GetError()); return 0; }
    SDL_PauseAudioDevice(dev, 0);
    g_audio_on = 1;
    fprintf(stderr, "[audio] SDL audio %d Hz stereo (%s)\n", have.freq, SDL_GetCurrentAudioDriver());
#endif
    return g_audio_on;
}

/* ===================== the FMOD shim's view: events */
#define MAXEV 4096
typedef struct { int used; evmap m; float vol; float x, y, z; int has3d; int voice; } event;
static event g_ev[MAXEV];

/* A new event for `path`; returns its id (stored in the guest object), 0 if none. */
uint32_t ds_audio_event_new(const char *path) {
    if (!audio_on()) return 0;
    for (uint32_t i = 1; i < MAXEV; i++) if (!g_ev[i].used) {
        event *e = &g_ev[i]; memset(e, 0, sizeof *e);
        e->used = 1; e->vol = 1.0f; e->voice = -1;
        map_event(path, &e->m);
        if (getenv("DS_AUDIO_TRACE")) fprintf(stderr, "[audio] event %s -> %d wave(s)%s\n", path, e->m.n, e->m.loop ? " (loop)" : "");
        return i;
    }
    return 0;
}
static event *ev(uint32_t id) { return id && id < MAXEV && g_ev[id].used ? &g_ev[id] : NULL; }
void ds_audio_event_start(uint32_t id) {
    event *e = ev(id); if (!e || !e->m.n) return;
    wave *w = &g_waves[e->m.idx[rand() % e->m.n]];
    if (!decode(w)) return;
    pthread_mutex_lock(&g_mix);
    int k = -1;
    for (int i = 0; i < NVOICE; i++) if (!g_voice[i].active) { k = i; break; }
    if (k < 0) k = rand() % NVOICE;                             /* steal */
    voice *v = &g_voice[k]; memset(v, 0, sizeof *v);
    v->active = 1; v->loop = e->m.loop; v->ev = id; v->w = w;
    v->step = (double)w->rate / OUT_RATE; v->vol = e->vol;
    v->x = e->x; v->y = e->y; v->z = e->z; v->has3d = e->has3d;
    spatialize(v);
    e->voice = k;
    pthread_mutex_unlock(&g_mix);
}
void ds_audio_event_stop(uint32_t id) {
    event *e = ev(id); if (!e) return;
    pthread_mutex_lock(&g_mix);
    if (e->voice >= 0 && g_voice[e->voice].ev == id) g_voice[e->voice].active = 0;
    e->voice = -1;
    pthread_mutex_unlock(&g_mix);
}
void ds_audio_event_release(uint32_t id) {        /* the game lets go: loops stop, one-shots finish */
    event *e = ev(id); if (!e) return;
    pthread_mutex_lock(&g_mix);
    if (e->voice >= 0 && g_voice[e->voice].ev == id) {
        if (g_voice[e->voice].loop) g_voice[e->voice].active = 0; else g_voice[e->voice].ev = 0;
    }
    e->used = 0;
    pthread_mutex_unlock(&g_mix);
}
static void ev_update(event *e, uint32_t id) {
    if (e->voice < 0) return;
    voice *v = &g_voice[e->voice]; if (v->ev != id || !v->active) return;
    v->vol = e->vol; v->x = e->x; v->y = e->y; v->z = e->z; v->has3d = e->has3d;
    spatialize(v);
}
void ds_audio_event_volume(uint32_t id, float vol) {
    event *e = ev(id); if (!e) return;
    pthread_mutex_lock(&g_mix); e->vol = vol; ev_update(e, id); pthread_mutex_unlock(&g_mix);
}
void ds_audio_event_pos(uint32_t id, float x, float y, float z) {
    event *e = ev(id); if (!e) return;
    pthread_mutex_lock(&g_mix); e->x = x; e->y = y; e->z = z; e->has3d = 1; ev_update(e, id); pthread_mutex_unlock(&g_mix);
}
int ds_audio_event_playing(uint32_t id) {
    event *e = ev(id); if (!e || e->voice < 0) return 0;
    pthread_mutex_lock(&g_mix);
    int p = g_voice[e->voice].active && g_voice[e->voice].ev == id;
    pthread_mutex_unlock(&g_mix);
    return p;
}
void ds_audio_listener(float x, float y, float z) {
    pthread_mutex_lock(&g_mix);
    g_lx = x; g_ly = y; g_lz = z;
    for (int i = 0; i < MAXEV; i++) if (g_ev[i].used) ev_update(&g_ev[i], (uint32_t)i);
    pthread_mutex_unlock(&g_mix);
}
