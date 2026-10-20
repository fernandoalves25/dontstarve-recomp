/*
 * r36s_hooks: small fixes inside Don't Starve on the R36S (x86-64, loaded with
 * BOX64_LD_PRELOAD next to the game). Each one is switched by an environment variable that
 * the launcher sets from port.cfg; unset leaves the game's own behaviour.
 *
 * FMOD tuning. The game's FMOD runs emulated and mixes ~85 ms ahead (4 blocks of 1024
 * samples at 48 kHz): any game frame slower than that leaves the sound card without audio
 * and the sound crackles. The game calls FMOD::EventSystem::init() itself; we take that call,
 * set the low-level System up first (allowed before init), then run the real init.
 *   R36S_FMOD_BLOCK     samples per mix block (FMOD default 1024)
 *   R36S_FMOD_BLOCKS    blocks mixed ahead (FMOD default 4)
 *   R36S_FMOD_RATE      mixing rate in Hz (default 48000; lower = less CPU)
 *   R36S_FMOD_VOICES    sounds mixed at once (default 64; lower = less CPU)
 *   R36S_FMOD_NOREVERB=1  ignore the game's reverb presets (EventSystem::setReverbProperties):
 *                       FMOD then never runs its software reverb, the most expensive part of
 *                       the mix. In a world the emulated mixer thread sat at 100% of a core
 *                       and the sound stuttered whatever the buffer size.
 *
 * Mouse pointer. The R36S has no mouse, but SDL draws its pointer over the game.
 *   R36S_HIDE_CURSOR=1  hide it when the window opens and keep it hidden
 *
 * Build (any x86-64 Linux gcc):
 *   gcc -O2 -shared -fPIC -fno-stack-protector -U_FORTIFY_SOURCE -o r36s_hooks.so r36s_hooks.c
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>

/* box64 wraps the old dlsym symbol version; newer toolchains default to GLIBC_2.34 */
__asm__(".symver dlsym,dlsym@GLIBC_2.2.5");

/* No atoi: with glibc 2.38 headers it becomes __isoc23_strtol, which box64 may not know */
static int env_int(const char *name)
{
    const char *value = getenv(name);
    int n = 0;
    while (value && *value >= '0' && *value <= '9')
        n = n * 10 + (*value++ - '0');
    return n;
}

/* ---- FMOD tuning ------------------------------------------------------------------- */

typedef int FMOD_RESULT;
typedef struct EventSystem EventSystem;
typedef struct System System;

/* FMOD Ex 4.44 C++ methods, called through their mangled names (this = first argument) */
FMOD_RESULT _ZN4FMOD11EventSystem15getSystemObjectEPPNS_6SystemE(EventSystem *, System **);
FMOD_RESULT _ZN4FMOD6System16setDSPBufferSizeEji(System *, unsigned int, int);
FMOD_RESULT _ZN4FMOD6System16getDSPBufferSizeEPjPi(System *, unsigned int *, int *);
FMOD_RESULT _ZN4FMOD6System17getSoftwareFormatEPiP17FMOD_SOUND_FORMATS1_S1_P18FMOD_DSP_RESAMPLERS1_(
    System *, int *, int *, int *, int *, int *, int *);
FMOD_RESULT _ZN4FMOD6System17setSoftwareFormatEi17FMOD_SOUND_FORMATii18FMOD_DSP_RESAMPLER(
    System *, int, int, int, int, int);
FMOD_RESULT _ZN4FMOD6System19setSoftwareChannelsEi(System *, int);

typedef FMOD_RESULT (*event_init_fn)(EventSystem *, int, unsigned int, void *, unsigned int);

FMOD_RESULT _ZN4FMOD11EventSystem4initEijPvj(EventSystem *events, int maxchannels,
                                              unsigned int flags, void *extra,
                                              unsigned int eventflags)
{
    event_init_fn real = (event_init_fn)dlsym(RTLD_NEXT, "_ZN4FMOD11EventSystem4initEijPvj");
    int want_block = env_int("R36S_FMOD_BLOCK"), want_blocks = env_int("R36S_FMOD_BLOCKS");
    int want_rate = env_int("R36S_FMOD_RATE"), want_voices = env_int("R36S_FMOD_VOICES");
    /* 1 = no interpolation (cheapest), 2 = linear, 3 = cubic, 4 = spline; 0 = the game's */
    int want_resampler = env_int("R36S_FMOD_RESAMPLER");
    System *sys = NULL;

    if (real && (want_block || want_blocks || want_rate || want_voices || want_resampler) &&
        _ZN4FMOD11EventSystem15getSystemObjectEPPNS_6SystemE(events, &sys) == 0 && sys) {
        unsigned int block = 0;
        int blocks = 0, rate = 0, format = 0, outch = 0, inch = 0, resampler = 0, bits = 0;

        _ZN4FMOD6System16getDSPBufferSizeEPjPi(sys, &block, &blocks);
        if (want_block > 0 || want_blocks > 0)
            _ZN4FMOD6System16setDSPBufferSizeEji(sys, want_block > 0 ? want_block : block,
                                                 want_blocks > 0 ? want_blocks : blocks);
        if ((want_rate > 0 || (want_resampler >= 1 && want_resampler <= 4)) &&
            _ZN4FMOD6System17getSoftwareFormatEPiP17FMOD_SOUND_FORMATS1_S1_P18FMOD_DSP_RESAMPLERS1_(
                sys, &rate, &format, &outch, &inch, &resampler, &bits) == 0)
            _ZN4FMOD6System17setSoftwareFormatEi17FMOD_SOUND_FORMATii18FMOD_DSP_RESAMPLER(
                sys, want_rate > 0 ? want_rate : rate, format, outch, inch,
                want_resampler >= 1 && want_resampler <= 4 ? want_resampler - 1 : resampler);
        if (want_voices > 0)
            _ZN4FMOD6System19setSoftwareChannelsEi(sys, want_voices);

        _ZN4FMOD6System16getDSPBufferSizeEPjPi(sys, &block, &blocks);
        _ZN4FMOD6System17getSoftwareFormatEPiP17FMOD_SOUND_FORMATS1_S1_P18FMOD_DSP_RESAMPLERS1_(
            sys, &rate, &format, &outch, &inch, &resampler, &bits);
        fprintf(stderr, "[R36S] FMOD: %u x %d blocks at %d Hz (%.0f ms ahead), voices %d, resampler %d\n",
                block, blocks, rate, rate ? 1000.0 * block * blocks / rate : 0.0,
                want_voices > 0 ? want_voices : 64, resampler);
    }
    return real ? real(events, maxchannels, flags, extra, eventflags) : -1;
}

/* ---- No reverb ------------------------------------------------------------------------ */
typedef struct FMOD_REVERB_PROPERTIES FMOD_REVERB_PROPERTIES;
typedef FMOD_RESULT (*set_reverb_fn)(EventSystem *, const FMOD_REVERB_PROPERTIES *);

FMOD_RESULT _ZN4FMOD11EventSystem19setReverbPropertiesEPK22FMOD_REVERB_PROPERTIES(
    EventSystem *events, const FMOD_REVERB_PROPERTIES *props)
{
    static set_reverb_fn real;
    static int off = -1;
    if (off < 0) {
        off = env_int("R36S_FMOD_NOREVERB") == 1;
        if (off) fprintf(stderr, "[R36S] FMOD: reverb off\n");
    }
    if (off) return 0;                                   /* FMOD_OK, nothing applied */
    if (!real)
        real = (set_reverb_fn)dlsym(RTLD_NEXT, "_ZN4FMOD11EventSystem19setReverbPropertiesEPK22FMOD_REVERB_PROPERTIES");
    return real ? real(events, props) : 0;
}

/* ---- Mouse pointer ----------------------------------------------------------------- */

typedef struct SDL_Window SDL_Window;

/* The game may call SDL_ShowCursor every frame, and a lookup through box64 is slow: resolve
 * the real function and read the setting once */
static int (*real_show_cursor)(int);
static int hide_cursor = -1;

static void cursor_setup(void)
{
    if (hide_cursor < 0) {
        real_show_cursor = (int (*)(int))dlsym(RTLD_NEXT, "SDL_ShowCursor");
        hide_cursor = env_int("R36S_HIDE_CURSOR") == 1;
    }
}

int SDL_ShowCursor(int toggle)
{
    cursor_setup();
    if (!real_show_cursor)
        return 0;
    if (!hide_cursor)
        return real_show_cursor(toggle);
    if (toggle != 0) /* the game asks to show it (or queries): keep it hidden */
        real_show_cursor(0);
    return 0;
}

SDL_Window *SDL_CreateWindow(const char *title, int x, int y, int w, int h, unsigned int flags)
{
    SDL_Window *(*real)(const char *, int, int, int, int, unsigned int) =
        (SDL_Window * (*)(const char *, int, int, int, int, unsigned int))
            dlsym(RTLD_NEXT, "SDL_CreateWindow");
    SDL_Window *window = real ? real(title, x, y, w, h, flags) : NULL;

    cursor_setup();
    if (window && hide_cursor && real_show_cursor) {
        real_show_cursor(0);
        fprintf(stderr, "[R36S] mouse pointer hidden\n");
    }
    return window;
}
