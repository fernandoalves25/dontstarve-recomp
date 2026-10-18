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
    uint32_t used;                                 /* LRU stamp for the PCM budget */
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

/* ===================== FEV: event -> sound definitions -> waves
 * The LGCY chunk of each .fev (see tools/audio/fev.py for the layout):
 * banks, a category tree, the event group tree, and the sound definition
 * table. An event is a path ("dontstarve/bee/bee_takeoff"); its sound
 * definitions list weighted waves (FEV bank index + wave index in that FSB). */
typedef struct { uint16_t bank, idx, weight; } fev_ent;          /* bank: our g_banks index */
/* a sound definition: weighted waves, played at vol; spawn_max > 0: while the
 * event plays, a new wave starts every spawn_min..spawn_max ms (nspawn at once) */
typedef struct { int n, cfg; fev_ent *e; float vol; uint32_t spawn_min, spawn_max, nspawn; } fev_def;
/* a layer plays one sound definition while its parameter (if any) is inside
 * [lo, hi] (normalized 0..1 over the parameter's range); env: it has envelopes */
typedef struct { int def, param, env, loop; float lo, hi; } fev_layer;
#define MAXLAY 8
#define MAXPAR 4
typedef struct {
    char *path, *cat; int fev, nlay, npar; fev_layer lay[MAXLAY];
    const char *pname[MAXPAR]; float pmin[MAXPAR], pmax[MAXPAR];
} fev_event;
static fev_def *g_defs[8]; static int g_ndefs[8];               /* per .fev */
static fev_event *g_events; static int g_nevents;
typedef struct { const uint8_t *b; size_t n, p; int bad; } rd;
static uint32_t r32(rd *r) { if (r->p + 4 > r->n) { r->bad = 1; return 0; } uint32_t v; memcpy(&v, r->b + r->p, 4); r->p += 4; return v; }
static const char *rstr(rd *r, uint32_t *len) {
    uint32_t n = r32(r); if (r->bad || r->p + n > r->n) { r->bad = 1; return ""; }
    const char *s = (const char *)r->b + r->p; r->p += n; if (len) *len = n; return s;
}
static int bank_by_name(const char *nm) {                       /* FEV bank name -> our FSB */
    for (int i = 0; i < g_nbanks; i++) {
        const char *f = strrchr(g_banks[i].path, '/'); f = f ? f + 1 : g_banks[i].path;
        size_t l = strlen(nm);
        if (!strncasecmp(f, nm, l) && !strcasecmp(f + l, ".fsb")) return i;
    }
    return -1;
}
static void read_cats(rd *r, char *prefix, int depth, char ***paths, int *np) {
    uint32_t l; const char *nm = rstr(r, &l); r32(r); r32(r); r32(r); r32(r); uint32_t n = r32(r);
    char path[256];
    if (depth == 0) snprintf(path, sizeof path, "%s", nm); else if (depth == 1) snprintf(path, sizeof path, "%s", nm);
    else snprintf(path, sizeof path, "%s/%s", prefix, nm);
    *paths = (char **)realloc(*paths, sizeof(char *) * (size_t)(*np + 1)); (*paths)[(*np)++] = strdup(path);
    for (uint32_t i = 0; i < n && !r->bad && i < 256; i++) read_cats(r, depth == 0 ? (char *)"" : path, depth + 1, paths, np);
}
static void load_fev(const uint8_t *d, size_t n) {
    if (n < 12 || memcmp(d, "RIFF", 4) || g_nevents < 0) return;
    const uint8_t *lg = NULL, *sr = NULL; size_t lgn = 0, srn = 0;
    for (size_t off = 12; off + 8 <= n; ) {                     /* RIFF 'FEV ', LIST 'PROJ' */
        uint32_t sz; memcpy(&sz, d + off + 4, 4);
        if (!memcmp(d + off, "LIST", 4)) { off += 12; continue; }
        if (!memcmp(d + off, "LGCY", 4)) { lg = d + off + 8; lgn = sz; }
        if (!memcmp(d + off, "STRR", 4)) { sr = d + off + 8; srn = sz; }
        off += 8 + sz + (sz & 1);
    }
    if (!lg || !sr || srn < 4) return;
    uint32_t ns; memcpy(&ns, sr, 4);
    const char **S = (const char **)calloc(ns, sizeof *S);
    for (uint32_t i = 0; i < ns; i++) { uint32_t o; memcpy(&o, sr + 4 + 4 * i, 4); S[i] = (const char *)sr + 4 + 4 * ns + o; }
    int fi = 0; while (fi < 8 && g_defs[fi]) fi++;
    if (fi >= 8) { free(S); return; }
    rd r = { lg, lgn, 0, 0 };
    r32(&r); r32(&r); uint32_t pl; const char *proj = rstr(&r, &pl);
    uint32_t nb = r32(&r); r32(&r);
    int bankmap[128];
    for (uint32_t i = 0; i < nb && !r.bad; i++) {
        r32(&r); r32(&r); r32(&r); r32(&r); r32(&r);
        const char *bn = rstr(&r, NULL);
        if (i < 128) bankmap[i] = bank_by_name(bn);
    }
    char **cats = NULL; int ncats = 0; read_cats(&r, (char *)"", 0, &cats, &ncats);
    size_t groups_at = r.p;
    /* the sound definition table: a u32 count, then records whose names are
     * "/..." strings. Try every such spot and keep the largest table that
     * reads through cleanly (its first name is not always the first "/" string). */
    size_t defs_at = 0; fev_def *defs = NULL; uint32_t ndefs = 0;
    for (size_t q = groups_at + 4; q + 8 <= lgn; q++) {
        uint32_t v; memcpy(&v, lg + q, 4); if (v >= ns || S[v][0] != '/') continue;
        rd t = { lg, lgn, q - 4, 0 }; uint32_t cnt = r32(&t);
        if (!cnt || cnt > 100000 || cnt <= ndefs) continue;
        fev_def *dd = (fev_def *)calloc(cnt, sizeof *dd);
        for (uint32_t k = 0; k < cnt && !t.bad; k++) {
            uint32_t nm = r32(&t), cf = r32(&t), ne = r32(&t);
            dd[k].cfg = (int)cf; dd[k].vol = 1.0f;
            if (nm >= ns || S[nm][0] != '/' || ne > 10000) { t.bad = 1; break; }
            dd[k].e = (fev_ent *)calloc(ne ? ne : 1, sizeof(fev_ent));
            for (uint32_t e = 0; e < ne && !t.bad; e++) {
                uint32_t ty = r32(&t), w = r32(&t);
                if (ty == 0) {
                    rstr(&t, NULL); uint32_t bk = r32(&t), ix = r32(&t); r32(&t);
                    if (bk < 128 && bk < nb && bankmap[bk] >= 0) { fev_ent *x = &dd[k].e[dd[k].n++]; x->bank = (uint16_t)bankmap[bk]; x->idx = (uint16_t)ix; x->weight = (uint16_t)w; }
                } else if (ty != 2 && ty != 3) t.bad = 1;
            }
        }
        if (t.bad) { for (uint32_t k = 0; k < cnt; k++) free(dd[k].e); free(dd); continue; }
        if (defs) { for (uint32_t k = 0; k < ndefs; k++) free(defs[k].e); free(defs); }
        defs = dd; ndefs = cnt; defs_at = q - 4;
    }
    if (!defs) { fprintf(stderr, "[audio] %s.fev: no sound definition table\n", proj); free(S); return; }
    g_defs[fi] = defs; g_ndefs[fi] = (int)ndefs;
    /* group/event tree; an event ends with its category path string */
    r.p = groups_at;
    struct { uint32_t left_sub, left_ev; char path[256]; } st[32]; int sp = 0;
    uint32_t ng = r32(&r);
    int nev0 = g_nevents;
    for (uint32_t g = 0; g < ng && !r.bad; g++) {
        /* iterative DFS over one top-level group */
        sp = 0;
        uint32_t nm = r32(&r), np = r32(&r), nsub = r32(&r), nev = r32(&r);
        if (np || nm >= ns) { r.bad = 1; break; }
        snprintf(st[0].path, sizeof st[0].path, "%s/%s", proj, S[nm]); st[0].left_sub = nsub; st[0].left_ev = nev; sp = 1;
        while (sp > 0 && !r.bad) {
            if (st[sp - 1].left_sub) {
                st[sp - 1].left_sub--;
                nm = r32(&r); np = r32(&r); nsub = r32(&r); nev = r32(&r);
                if (np || nm >= ns || sp >= 32) { r.bad = 1; break; }
                snprintf(st[sp].path, sizeof st[sp].path, "%s/%s", st[sp - 1].path, S[nm]);
                st[sp].left_sub = nsub; st[sp].left_ev = nev; sp++;
                continue;
            }
            if (st[sp - 1].left_ev) {
                st[sp - 1].left_ev--;
                size_t es = r.p; uint32_t ty = r32(&r), en = r32(&r);
                if (en >= ns) { r.bad = 1; break; }
                size_t end = 0, cl = 0; int ci = -1;        /* the closing category string: one forward scan */
                for (size_t q = es + 8; q + 8 <= defs_at && !end; q++) {
                    uint32_t v; memcpy(&v, lg + q, 4);
                    if (v < 2 || v > 128) continue;
                    for (int c = 0; c < ncats; c++)
                        if (strlen(cats[c]) + 1 == v && q + 4 + v <= defs_at && !memcmp(lg + q + 4, cats[c], v)) { end = q; cl = 4 + v; ci = c; break; }
                }
                if (!end) { r.bad = 1; break; }
                fev_event *e;
                g_events = (fev_event *)realloc(g_events, sizeof *g_events * (size_t)(g_nevents + 1));
                e = &g_events[g_nevents++]; memset(e, 0, sizeof *e); e->fev = fi;
                char path[320]; snprintf(path, sizeof path, "%s/%s", st[sp - 1].path, S[en]); e->path = strdup(path);
                e->cat = strdup(cats[ci]);
                if (ty == 0x10) {
                    uint16_t di; memcpy(&di, lg + es + 172, 2);
                    uint32_t mode; memcpy(&mode, lg + es + 186, 4);     /* 0: loop, 1: one-shot */
                    if (di < ndefs) { fev_layer *y = &e->lay[e->nlay++]; y->def = di; y->param = -1; y->lo = 0; y->hi = 1; y->loop = mode == 0; }
                }
                /* a complex event's layers: u16 flags (2), i16 priority (-1),
                 * i16 parameter, u16 nsounds, u16 nenvelopes, then the first
                 * sound instance's u16 sound definition */
                else {
                    for (size_t q = es + 8; q + 28 <= end && e->nlay < MAXLAY; q++) {
                        const uint8_t *L = lg + q;
                        if (L[0] != 2 || L[1] || L[2] != 0xFF || L[3] != 0xFF || L[6] < 1 || L[6] > 8 || L[7] || L[8] > 0x20 || L[9]) continue;
                        uint16_t di; int16_t pi; float st, ln;
                        memcpy(&di, L + 10, 2); memcpy(&pi, L + 4, 2); memcpy(&st, L + 12, 4); memcpy(&ln, L + 16, 4);
                        if (di >= ndefs) continue;
                        fev_layer *y = &e->lay[e->nlay++];
                        uint32_t mode; memcpy(&mode, L + 24, 4);
                        y->def = di; y->param = pi; y->env = L[8]; y->lo = st; y->hi = st + ln; y->loop = mode == 0;
                        if (!(y->lo >= 0.0f && y->hi <= 1.01f && y->hi > y->lo)) { y->lo = 0; y->hi = 1; }
                    }
                    /* the parameters: u32 n, n x 32 bytes { u32 name, u32 0, f32 min,
                     * f32 max, ... }, then (usually 8) bytes before the category string */
                    for (size_t tail = 0; tail <= 64 && !e->npar; tail += 4)
                        for (uint32_t np = 1; np <= MAXPAR && !e->npar; np++) {
                            if (end < es + tail + 32 * np + 4) break;
                            size_t at = end - tail - 32 * np - 4; uint32_t c; memcpy(&c, lg + at, 4);
                            if (c != np) continue;
                            int ok = 1;
                            for (uint32_t k = 0; k < np && ok; k++) {
                                uint32_t nm, z; float mn, mx; const uint8_t *R = lg + at + 4 + 32 * k;
                                memcpy(&nm, R, 4); memcpy(&z, R + 4, 4); memcpy(&mn, R + 8, 4); memcpy(&mx, R + 12, 4);
                                if (nm >= ns || z || !(mx > mn) || mx > 1e6f) ok = 0;
                                else { e->pname[k] = S[nm]; e->pmin[k] = mn; e->pmax[k] = mx; }
                            }
                            if (ok) e->npar = (int)np;
                        }
                }
                r.p = end + cl;
                continue;
            }
            sp--;
        }
    }
    /* the sound definition configs follow the event tree: u32 n, n x 74 bytes
     * { u32 play mode, u32 spawn min ms, u32 spawn max ms, u32 max spawned, f32 volume, ... } */
    if (!r.bad) {
        uint32_t nc = r32(&r);
        if (!r.bad && r.p + 74ull * nc <= defs_at)
            for (uint32_t k = 0; k < ndefs; k++) {
                fev_def *df = &defs[k]; if (df->cfg < 0 || (uint32_t)df->cfg >= nc) continue;
                const uint8_t *C = lg + r.p + 74u * (uint32_t)df->cfg;
                memcpy(&df->spawn_min, C + 4, 4); memcpy(&df->spawn_max, C + 8, 4); memcpy(&df->nspawn, C + 12, 4); memcpy(&df->vol, C + 16, 4);
                if (!(df->vol >= 0.0f && df->vol <= 4.0f)) df->vol = 1.0f;
                if (df->spawn_max > 600000 || df->spawn_min > df->spawn_max) df->spawn_min = df->spawn_max = 0;
                if (df->nspawn < 1) df->nspawn = 1; if (df->nspawn > 4) df->nspawn = 4;
            }
    }
    int nodef = 0; for (int i = nev0; i < g_nevents; i++) nodef += !g_events[i].nlay;
    fprintf(stderr, "[audio] %s.fev: %d events (%d without a sound), %u sound definitions%s\n", proj, g_nevents - nev0, nodef, ndefs, r.bad ? " (tree cut short)" : "");
    for (int c = 0; c < ncats; c++) free(cats[c]); free(cats); free(S);
}
static void load_fevs(void) {
    char host[512]; ds_map_path("Z:\\dontstarve\\data\\sound", host, sizeof host);
    DIR *dir = opendir(host); if (!dir) return;
    struct dirent *e;
    while ((e = readdir(dir))) {
        size_t l = strlen(e->d_name);
        if (l > 4 && !strcasecmp(e->d_name + l - 4, ".fev")) {
            char p[1024]; snprintf(p, sizeof p, "%s/%s", host, e->d_name);
            int fd = open(p, O_RDONLY); if (fd < 0) continue;
            struct stat sb; if (fstat(fd, &sb)) { close(fd); continue; }
            uint8_t *d = (uint8_t *)mmap(NULL, (size_t)sb.st_size, PROT_READ, MAP_PRIVATE, fd, 0); close(fd);
            if (d != MAP_FAILED) load_fev(d, (size_t)sb.st_size);   /* strings stay mapped (S[] points in) */
        }
    }
    closedir(dir);
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
static int wave_of(uint16_t bank, uint16_t idx) {               /* our wave index for (bank, index) */
    for (int i = 0; i < g_nwaves; i++) if (g_waves[i].bank == bank) return i + idx < g_nwaves && g_waves[i + idx].bank == bank ? i + idx : -1;
    return -1;
}
typedef struct { int n; int idx[24]; int param, env, loop; float lo, hi, vol; uint32_t spawn_min, spawn_max, nspawn; } evlayer;
typedef struct {
    int nlay, loop, npar; evlayer lay[MAXLAY]; const char *cat;
    const char *pname[MAXPAR]; float pmin[MAXPAR], pmax[MAXPAR];
} evmap;
static void map_event(const char *path, evmap *m) {
    memset(m, 0, sizeof *m);
    if (!g_indexed) { build_index(); load_fevs(); }
    for (int i = 0; i < g_nevents; i++) {
        if (strcasecmp(g_events[i].path, path)) continue;
        fev_event *e = &g_events[i];
        for (int k = 0; k < e->nlay; k++) {                    /* every layer, each with its waves */
            fev_def *d = &g_defs[e->fev][e->lay[k].def];
            evlayer *y = &m->lay[m->nlay];
            for (int j = 0; j < d->n && y->n < 24; j++) { int w = wave_of(d->e[j].bank, d->e[j].idx); if (w >= 0) y->idx[y->n++] = w; }
            if (!y->n) continue;
            y->param = e->lay[k].param < e->npar ? e->lay[k].param : -1;
            y->env = e->lay[k].env; y->lo = e->lay[k].lo; y->hi = e->lay[k].hi; y->loop = e->lay[k].loop;
            y->vol = d->vol; y->spawn_min = d->spawn_min; y->spawn_max = d->spawn_max; y->nspawn = d->spawn_max ? d->nspawn : 1;
            m->nlay++;
        }
        m->npar = e->npar; m->cat = e->cat;
        for (int k = 0; k < e->npar; k++) { m->pname[k] = e->pname[k]; m->pmin[k] = e->pmin[k]; m->pmax[k] = e->pmax[k]; }
        break;
    }
    char p[256]; snprintf(p, sizeof p, "%s", path);
    char *leaf = strrchr(p, '/'); if (!leaf) leaf = p; else *leaf++ = 0;
    if (m->nlay == 0) {                                         /* not in a .fev: by name */
        char *grp = strrchr(p, '/'); grp = grp ? grp + 1 : p;
        char stems[3][160];
        snprintf(stems[0], sizeof stems[0], "%s", leaf);
        snprintf(stems[1], sizeof stems[1], "%s_%s", grp, leaf);
        snprintf(stems[2], sizeof stems[2], "%s%s", grp, leaf);
        evlayer *y = &m->lay[0]; y->param = -1; y->lo = 0; y->hi = 1; y->vol = 1; y->nspawn = 1;
        for (int s = 0; s < 3 && y->n == 0; s++)
            for (int i = 0; i < g_nwaves && y->n < 24; i++)
                if (name_match(g_waves[i].name, stems[s])) y->idx[y->n++] = i;
        m->nlay = y->n ? 1 : 0;
        y->loop = (strstr(leaf, "LP") != NULL || strcasestr(leaf, "amb") != NULL || strcasestr(path, "/music/") != NULL)
                  && !strcasestr(leaf, "stinger");
    }
    for (int k = 0; k < m->nlay; k++) m->loop |= m->lay[k].loop || m->lay[k].spawn_max;
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
    int layer; const evlayer *lay;               /* looping layers pick a new wave each time round */
    uint32_t wait;                               /* spawning layers: silent frames before the next wave */
    const char *cat;                             /* the event's category ("set_sfx/voice") */
    const wave *w; double pos, step;
    float vol, gain_l, gain_r;
    float x, y, z; int has3d;
} voice;
static voice g_voice[NVOICE];
static pthread_mutex_t g_mix = PTHREAD_MUTEX_INITIALIZER;
static float g_lx, g_ly, g_lz;                   /* listener */
static float g_master = 1.0f;
static int g_audio_on = -1;

/* Decoded PCM is kept under a budget (DS_AUDIO_MB, default 192, 96 on aarch64): the least
 * recently started waves no voice is playing are dropped first. */
static size_t g_pcm_bytes, g_pcm_cap; static uint32_t g_clock;
static int decode_lru(wave *w) {
    w->used = ++g_clock;
    if (w->pcm) return 1;
    if (!decode(w)) return 0;
#if defined(__aarch64__)
    const int def_mb = 96;                          /* a 1 GB handheld */
#else
    const int def_mb = 192;
#endif
    if (!g_pcm_cap) { const char *e = getenv("DS_AUDIO_MB"); g_pcm_cap = (size_t)(e && atoi(e) > 0 ? atoi(e) : def_mb) << 20; }
    g_pcm_bytes += (size_t)w->frames * w->channels * 2;
    pthread_mutex_lock(&g_mix);                     /* the mixer may switch a looping voice's wave */
    while (g_pcm_bytes > g_pcm_cap) {
        int old = -1;
        for (int i = 0; i < g_nwaves; i++) {
            wave *c = &g_waves[i]; if (!c->pcm || c == w) continue;
            int busy = 0; for (int k = 0; k < NVOICE && !busy; k++) busy = g_voice[k].active && g_voice[k].w == c;
            if (!busy && (old < 0 || c->used < g_waves[old].used)) old = i;
        }
        if (old < 0) break;
        wave *c = &g_waves[old];
        g_pcm_bytes -= (size_t)c->frames * c->channels * 2; free(c->pcm); c->pcm = NULL; c->frames = 0;
    }
    pthread_mutex_unlock(&g_mix);
    return 1;
}

/* Category volumes, set by the game's mixer ("master", "set_music/soundtrack",
 * "set_sfx/voice", ...): an event is scaled by its category and every parent. */
#define MAXCAT 64
static struct { char name[64]; float vol; } g_cat[MAXCAT]; static int g_ncat;
int ds_audio_category(const char *name) {
    for (int i = 0; i < g_ncat; i++) if (!strcasecmp(g_cat[i].name, name)) return i;
    if (g_ncat >= MAXCAT) return -1;
    snprintf(g_cat[g_ncat].name, sizeof g_cat[0].name, "%s", name); g_cat[g_ncat].vol = 1.0f;
    return g_ncat++;
}
static float cat_vol(const char *path) {
    float g = 1.0f;
    for (int i = 0; i < g_ncat; i++) {
        size_t l = strlen(g_cat[i].name);
        if (!strcasecmp(g_cat[i].name, "master") || (path && !strncasecmp(path, g_cat[i].name, l) && (!path[l] || path[l] == '/')))
            g *= g_cat[i].vol;
    }
    return g;
}
static void spatialize(voice *v) {
    float l = 1.0f, r = 1.0f, att = 1.0f;
    if (v->has3d) {
        float dx = v->x - g_lx, dz = v->z - g_lz, d = sqrtf(dx * dx + dz * dz);
        att = d < 4.0f ? 1.0f : d > 40.0f ? 0.0f : 1.0f - (d - 4.0f) / 36.0f;
        float pan = d > 0.01f ? dx / (d + 6.0f) : 0.0f;            /* -1 .. 1 */
        l = pan > 0 ? 1.0f - pan : 1.0f; r = pan < 0 ? 1.0f + pan : 1.0f;
    }
    float c = cat_vol(v->cat);
    v->gain_l = v->vol * att * l * c * g_master; v->gain_r = v->vol * att * r * c * g_master;
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
            if (v->wait) {                                     /* between two spawned waves */
                if (--v->wait) continue;
                if (v->lay && v->lay->n > 1) {
                    const wave *n = &g_waves[v->lay->idx[rand() % v->lay->n]];
                    if (n->pcm) { v->w = w = n; v->step = (double)w->rate / OUT_RATE; }
                }
                v->pos = 0; p = 0;
            }
            if (p >= w->frames) {
                const evlayer *y = v->lay;
                if (y && y->spawn_max && v->ev) {                 /* spawn the next one later */
                    uint32_t ms = y->spawn_min + (y->spawn_max > y->spawn_min ? (uint32_t)rand() % (y->spawn_max - y->spawn_min) : 0);
                    v->wait = ms * (OUT_RATE / 1000) + 1; v->pos = 0; continue;
                }
                if (!v->loop) { v->active = 0; break; }
                v->pos -= w->frames;
                if (y && y->n > 1) {                            /* another of the layer's waves, if decoded */
                    const wave *n = &g_waves[y->idx[rand() % y->n]];
                    if (n->pcm) { v->w = w = n; v->pos = 0; v->step = (double)w->rate / OUT_RATE; }
                }
                p = (uint32_t)v->pos;
                if (p >= w->frames) { v->active = 0; break; }
            }
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
    if (g_audio_on && getenv("DS_AUDIO_CHECK")) {             /* decode every wave once */
        if (!g_indexed) { build_index(); load_fevs(); }
        int bad = 0;
        for (int i = 0; i < g_nwaves; i++) {
            wave *w = &g_waves[i];
            if (!decode(w)) { if (bad++ < 8) fprintf(stderr, "[audio] check: %s does not decode\n", w->name); continue; }
            free(w->pcm); w->pcm = NULL; w->frames = 0;
        }
        fprintf(stderr, "[audio] check: %d of %d waves decode\n", g_nwaves - bad, g_nwaves);
    }
    return g_audio_on;
}

/* ===================== the FMOD shim's view: events */
typedef struct {
    int used, started; evmap m; float vol; float x, y, z; int has3d;
    float val[MAXPAR];                             /* parameter values */
} event;
static event g_ev[DS_AUDIO_MAXEV];

/* A new event for `path`; returns its id (stored in the guest object), 0 if none. */
uint32_t ds_audio_event_new(const char *path) {
    if (!audio_on()) return 0;
    for (uint32_t i = 1; i < DS_AUDIO_MAXEV; i++) if (!g_ev[i].used) {
        event *e = &g_ev[i]; memset(e, 0, sizeof *e);
        e->used = 1; e->vol = 1.0f;
        map_event(path, &e->m);
        for (int k = 0; k < e->m.npar; k++) e->val[k] = e->m.pmin[k];
        if (getenv("DS_AUDIO_TRACE")) {
            int n = 0; for (int k = 0; k < e->m.nlay; k++) n += e->m.lay[k].n;
            fprintf(stderr, "[audio] event %s -> %d layer(s), %d wave(s)%s\n", path, e->m.nlay, n, e->m.loop ? " (loop)" : "");
        }
        return i;
    }
    return 0;
}
static event *ev(uint32_t id) { return id && id < DS_AUDIO_MAXEV && g_ev[id].used ? &g_ev[id] : NULL; }

/* How loud layer k is at the current parameter values: 0 = not playing. A
 * layer covers [lo, hi] of its parameter; a layer covering the whole range
 * with envelopes is faded by the parameter instead (music "intensity": 0 is
 * silent) -- the envelope points themselves are not decoded yet. */
static float layer_gain(const event *e, int k) {
    const evlayer *y = &e->m.lay[k];
    if (y->param < 0) return 1.0f;
    float mn = e->m.pmin[y->param], mx = e->m.pmax[y->param];
    float t = (e->val[y->param] - mn) / (mx - mn);
    if (y->env && y->lo <= 0.01f && y->hi >= 0.98f) { float g = 2.0f * t; return g < 0 ? 0 : g > 1 ? 1 : g; }
    return t >= y->lo - 0.001f && t <= y->hi + 0.001f ? 1.0f : 0.0f;
}
/* the voices of event id's layer k (k < 0: any layer); g_mix held */
static int layer_voices(uint32_t id, int k, int stop) {
    int n = 0;
    for (int i = 0; i < NVOICE; i++) {
        voice *v = &g_voice[i];
        if (!v->active || v->ev != id || (k >= 0 && v->layer != k)) continue;
        n++; if (stop) v->active = 0;
    }
    return n;
}
/* g_mix held */
static void start_layer(event *e, uint32_t id, int k, wave *w, uint32_t wait) {
    int n = -1;
    for (int i = 0; i < NVOICE; i++) if (!g_voice[i].active) { n = i; break; }
    if (n < 0) n = rand() % NVOICE;                             /* steal */
    const evlayer *y = &e->m.lay[k];
    voice *v = &g_voice[n]; memset(v, 0, sizeof *v);
    v->active = 1; v->loop = y->loop; v->ev = id; v->layer = k; v->lay = y; v->w = w; v->wait = wait; v->cat = e->m.cat;
    v->step = (double)w->rate / OUT_RATE; v->vol = e->vol * y->vol * layer_gain(e, k);
    v->x = e->x; v->y = e->y; v->z = e->z; v->has3d = e->has3d;
    spatialize(v);
}
/* Start the layers that should sound now and are not playing (decoding outside the lock). */
static void sync_layers(event *e, uint32_t id) {
    for (int k = 0; k < e->m.nlay; k++) {
        const evlayer *y = &e->m.lay[k];
        pthread_mutex_lock(&g_mix); int on = layer_voices(id, k, 0) > 0; pthread_mutex_unlock(&g_mix);
        float g = layer_gain(e, k);
        if (on || (g <= 0.0f && !(y->env && y->param >= 0))) {
            if (on && g <= 0.0f && !y->env) {                   /* left its range: stop */
                pthread_mutex_lock(&g_mix); layer_voices(id, k, 1); pthread_mutex_unlock(&g_mix);
            }
            continue;
        }
        wave *w = &g_waves[y->idx[rand() % y->n]];
        if (!decode_lru(w)) continue;
        if (y->loop || y->spawn_max) for (int j = 0; j < y->n; j++) decode_lru(&g_waves[y->idx[j]]);   /* variety */
        pthread_mutex_lock(&g_mix);
        for (uint32_t j = 0; j < y->nspawn; j++)                /* extra spawners start a little later */
            start_layer(e, id, k, w, j ? (y->spawn_min + (uint32_t)rand() % (y->spawn_max - y->spawn_min + 1)) * (OUT_RATE / 1000) + 1 : 0);
        pthread_mutex_unlock(&g_mix);
    }
}
void ds_audio_event_start(uint32_t id) {
    event *e = ev(id); if (!e || !e->m.nlay) return;
    ds_audio_event_stop(id);                                     /* a restart starts over */
    e->started = 1;
    sync_layers(e, id);
}
void ds_audio_event_stop(uint32_t id) {
    event *e = ev(id); if (!e) return;
    pthread_mutex_lock(&g_mix);
    layer_voices(id, -1, 1);
    e->started = 0;
    pthread_mutex_unlock(&g_mix);
}
void ds_audio_event_release(uint32_t id) {        /* the game lets go: loops stop, one-shots finish */
    event *e = ev(id); if (!e) return;
    pthread_mutex_lock(&g_mix);
    for (int i = 0; i < NVOICE; i++) {
        voice *v = &g_voice[i]; if (!v->active || v->ev != id) continue;
        if (v->loop || v->wait || (v->lay && v->lay->spawn_max)) v->active = 0;   /* loops and spawners end */
        else { v->ev = 0; v->lay = NULL; }                                        /* a one-shot plays out */
    }
    e->used = 0;
    pthread_mutex_unlock(&g_mix);
}
/* g_mix held */
static void ev_update(event *e, uint32_t id) {
    for (int i = 0; i < NVOICE; i++) {
        voice *v = &g_voice[i]; if (!v->active || v->ev != id || v->layer >= e->m.nlay) continue;
        v->vol = e->vol * e->m.lay[v->layer].vol * layer_gain(e, v->layer); v->x = e->x; v->y = e->y; v->z = e->z; v->has3d = e->has3d;
        spatialize(v);
    }
}
void ds_audio_event_volume(uint32_t id, float vol) {
    event *e = ev(id); if (!e) return;
    pthread_mutex_lock(&g_mix); e->vol = vol; ev_update(e, id); pthread_mutex_unlock(&g_mix);
}
void ds_audio_event_pos(uint32_t id, float x, float y, float z) {
    event *e = ev(id); if (!e) return;
    pthread_mutex_lock(&g_mix); e->x = x; e->y = y; e->z = z; e->has3d = 1; ev_update(e, id); pthread_mutex_unlock(&g_mix);
}
int ds_audio_event_param_index(uint32_t id, const char *name) {
    event *e = ev(id); if (!e || !name) return -1;
    for (int k = 0; k < e->m.npar; k++) if (!strcasecmp(e->m.pname[k], name)) return k;
    return -1;
}
void ds_audio_event_param(uint32_t id, int idx, float value) {
    event *e = ev(id); if (!e || idx < 0 || idx >= e->m.npar) return;
    if (value < e->m.pmin[idx]) value = e->m.pmin[idx];
    if (value > e->m.pmax[idx]) value = e->m.pmax[idx];
    if (e->val[idx] == value) return;
    e->val[idx] = value;
    pthread_mutex_lock(&g_mix); ev_update(e, id); pthread_mutex_unlock(&g_mix);
    if (e->started) sync_layers(e, id);                         /* a layer may come in or drop out */
}
int ds_audio_event_playing(uint32_t id) {
    event *e = ev(id); if (!e) return 0;
    pthread_mutex_lock(&g_mix);
    int p = layer_voices(id, -1, 0) > 0;
    pthread_mutex_unlock(&g_mix);
    return p;
}
void ds_audio_listener(float x, float y, float z) {
    pthread_mutex_lock(&g_mix);
    g_lx = x; g_ly = y; g_lz = z;
    for (int i = 0; i < DS_AUDIO_MAXEV; i++) if (g_ev[i].used) ev_update(&g_ev[i], (uint32_t)i);
    pthread_mutex_unlock(&g_mix);
}
void ds_audio_category_volume(int idx, float vol) {
    if (idx < 0 || idx >= g_ncat) return;
    pthread_mutex_lock(&g_mix);
    g_cat[idx].vol = vol < 0 ? 0 : vol;
    for (int i = 0; i < NVOICE; i++) if (g_voice[i].active) spatialize(&g_voice[i]);
    pthread_mutex_unlock(&g_mix);
}
float ds_audio_category_get(int idx) { return idx >= 0 && idx < g_ncat ? g_cat[idx].vol : 1.0f; }
size_t ds_audio_pcm_bytes(void) { return g_pcm_bytes; }
