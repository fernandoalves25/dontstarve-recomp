/*
 * ds_gles.c - the ANGLE bridge: the exe's libEGL / libGLESv2 imports (by
 * ordinal) onto the host's native EGL + GLES2.
 *
 * The game already speaks GLES2 (through ANGLE on Windows), so GL calls pass
 * straight through; only guest pointers need translating (ds_hostptr). The
 * EGL side is where the platform differs:
 *  - host display: EGL_PLATFORM_SURFACELESS_MESA (headless; the R36S build will
 *    put an SDL2/KMSDRM surface here),
 *  - the game's window surface is a DS_SCREEN_W x DS_SCREEN_H pbuffer,
 *  - EGLDisplay/EGLConfig/EGLSurface/EGLContext are opaque guest tokens.
 * DS_DUMP_FRAMES=N writes every Nth swapped frame to build/frames/*.ppm;
 * DS_EXIT_AFTER_FRAMES=N exits cleanly after N frames (coverage, CI).
 *
 * Ordinals come from the export tables of the game's own libEGL.dll and
 * libGLESv2.dll. GL_APIENTRY/EGLAPIENTRY are __stdcall: RET pops the args.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include "ds_hle.h"

/* guest pointer argument -> host pointer (NULL stays NULL) */
#define P(i)  (A32(i) ? ds_hostptr(A32(i)) : NULL)
/* float argument (pushed as its 32-bit pattern) */
static inline float AF(int i) { uint32_t u = A32(i); float f; memcpy(&f, &u, 4); return f; }

/* ===================== EGL */
#define TOK_DISPLAY 0x0E61D001u
#define TOK_CONFIG  0x0E6C0000u     /* + index */
#define TOK_SURFACE 0x0E650000u
#define TOK_CONTEXT 0x0E6E0000u
#define MAX_TOK 64

static EGLDisplay g_dpy = EGL_NO_DISPLAY;
static EGLConfig  g_cfg[MAX_TOK];   static int g_cfg_n;
static EGLSurface g_surf[MAX_TOK];  static int g_surf_n;
static EGLContext g_ctx[MAX_TOK];   static int g_ctx_n;
static uint32_t   g_egl_error = EGL_SUCCESS;

static uint32_t cfg_tok(EGLConfig c) {
    for (int i = 0; i < g_cfg_n; i++) if (g_cfg[i] == c) return TOK_CONFIG + (uint32_t)i;
    if (g_cfg_n >= MAX_TOK) return 0;
    g_cfg[g_cfg_n] = c; return TOK_CONFIG + (uint32_t)g_cfg_n++;
}
#define TOK_GET(arr, n, base, t) (((t) >= (base) && (t) < (base) + (uint32_t)(n)) ? arr[(t) - (base)] : NULL)

static void sh_eglGetDisplay(void) {             /* (EGLNativeDisplayType: the window's HDC) */
    if (ds_sdl_active()) RET(TOK_DISPLAY, 1);      /* SDL owns the display, window and context */
    if (g_dpy == EGL_NO_DISPLAY) {
        PFNEGLGETPLATFORMDISPLAYEXTPROC gpd =
            (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
        if (gpd) g_dpy = gpd(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
        if (g_dpy == EGL_NO_DISPLAY) g_dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    }
    RET(g_dpy == EGL_NO_DISPLAY ? 0 : TOK_DISPLAY, 1);
}
static void sh_eglInitialize(void) {             /* (dpy, *major, *minor) */
    if (ds_sdl_active()) { if (A32(1)) MEM32(A32(1)) = 1; if (A32(2)) MEM32(A32(2)) = 4; RET(1, 3); }
    EGLint ma = 0, mi = 0;
    EGLBoolean ok = eglInitialize(g_dpy, &ma, &mi);
    if (A32(1)) MEM32(A32(1)) = 1;               /* report EGL 1.4, as ANGLE did */
    if (A32(2)) MEM32(A32(2)) = 4;
    fprintf(stderr, "[egl] eglInitialize -> host EGL %d.%d (%s), vendor %s\n", ma, mi, ok ? "ok" : "FAILED",
            ok ? eglQueryString(g_dpy, EGL_VENDOR) : "-");
    eglBindAPI(EGL_OPENGL_ES_API);
    RET(ok, 3);
}
/* Copy a guest EGL attrib list (EGL_NONE-terminated), forcing what this host
 * can give: pbuffer surfaces (the window is headless) rendering GLES2. */
static int attribs_for_config(uint32_t va, EGLint *out, int cap) {
    int n = 0, have_surf = 0, have_rt = 0;
    for (uint32_t p = va; p && n < cap - 5; p += 8) {
        EGLint k = (EGLint)MEM32(p);
        if (k == EGL_NONE) break;
        EGLint v = (EGLint)MEM32(p + 4);
        if (k == EGL_SURFACE_TYPE) { v = EGL_PBUFFER_BIT; have_surf = 1; }
        if (k == EGL_RENDERABLE_TYPE) { v |= EGL_OPENGL_ES2_BIT; have_rt = 1; }
        if (k == EGL_NATIVE_RENDERABLE) continue;
        out[n++] = k; out[n++] = v;
    }
    if (!have_surf) { out[n++] = EGL_SURFACE_TYPE; out[n++] = EGL_PBUFFER_BIT; }
    if (!have_rt)   { out[n++] = EGL_RENDERABLE_TYPE; out[n++] = EGL_OPENGL_ES2_BIT; }
    out[n] = EGL_NONE;
    return n;
}
static void sh_eglChooseConfig(void) {           /* (dpy, attribs, configs, size, *num) */
    if (ds_sdl_active()) {                         /* one config: the SDL window's */
        if (A32(2) && A32(3)) MEM32(A32(2)) = TOK_CONFIG;
        if (A32(4)) MEM32(A32(4)) = 1;
        RET(1, 5);
    }
    EGLint at[128]; attribs_for_config(A32(1), at, 128);
    uint32_t out = A32(2); EGLint size = (EGLint)A32(3), num = 0;
    EGLConfig cs[MAX_TOK];
    if (size > MAX_TOK) size = MAX_TOK;
    EGLBoolean ok = eglChooseConfig(g_dpy, at, out ? cs : NULL, out ? size : 0, &num);
    if (out) for (EGLint i = 0; i < num; i++) MEM32(out + 4u * (uint32_t)i) = cfg_tok(cs[i]);
    if (A32(4)) MEM32(A32(4)) = (uint32_t)num;
    fprintf(stderr, "[egl] eglChooseConfig -> %d config(s)\n", num);
    RET(ok, 5);
}
static void sh_eglGetConfigs(void) {             /* (dpy, configs, size, *num): pbuffer + GLES2 ones only */
    if (ds_sdl_active()) {
        if (A32(1) && A32(2)) MEM32(A32(1)) = TOK_CONFIG;
        if (A32(3)) MEM32(A32(3)) = 1;
        RET(1, 4);
    }
    EGLint at[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE };
    uint32_t out = A32(1); EGLint size = (EGLint)A32(2), num = 0;
    EGLConfig cs[MAX_TOK];
    if (size > MAX_TOK) size = MAX_TOK;
    EGLBoolean ok = eglChooseConfig(g_dpy, at, out ? cs : NULL, out ? size : 0, &num);
    if (out) for (EGLint i = 0; i < num; i++) MEM32(out + 4u * (uint32_t)i) = cfg_tok(cs[i]);
    if (A32(3)) MEM32(A32(3)) = (uint32_t)num;
    RET(ok, 4);
}
static void sh_eglCreateWindowSurface(void) {    /* (dpy, config, HWND, attribs) -> a pbuffer */
    if (ds_sdl_active()) RET(ds_sdl_open(DS_SCREEN_W, DS_SCREEN_H) == 0 ? TOK_SURFACE : 0, 4);
    EGLConfig c = TOK_GET(g_cfg, g_cfg_n, TOK_CONFIG, A32(1));
    EGLint pa[] = { EGL_WIDTH, DS_SCREEN_W, EGL_HEIGHT, DS_SCREEN_H, EGL_NONE };
    EGLSurface s = c ? eglCreatePbufferSurface(g_dpy, c, pa) : EGL_NO_SURFACE;
    fprintf(stderr, "[egl] eglCreateWindowSurface(hwnd %u) -> %dx%d pbuffer %s\n", A32(2), DS_SCREEN_W, DS_SCREEN_H,
            s != EGL_NO_SURFACE ? "ok" : "FAILED");
    if (s == EGL_NO_SURFACE || g_surf_n >= MAX_TOK) { g_egl_error = eglGetError(); RET(0, 4); }
    g_surf[g_surf_n] = s;
    RET(TOK_SURFACE + (uint32_t)g_surf_n++, 4);
}
static void sh_eglCreateContext(void) {          /* (dpy, config, share, attribs) */
    if (ds_sdl_active()) RET(TOK_CONTEXT, 4);     /* SDL created it with the window */
    EGLConfig c = TOK_GET(g_cfg, g_cfg_n, TOK_CONFIG, A32(1));
    EGLContext share = A32(2) ? TOK_GET(g_ctx, g_ctx_n, TOK_CONTEXT, A32(2)) : EGL_NO_CONTEXT;
    EGLint at[32]; int n = 0;
    for (uint32_t p = A32(3); p && n < 30; p += 8) {
        if ((EGLint)MEM32(p) == EGL_NONE) break;
        at[n++] = (EGLint)MEM32(p); at[n++] = (EGLint)MEM32(p + 4);
    }
    at[n] = EGL_NONE;
    EGLContext x = c ? eglCreateContext(g_dpy, c, share, at) : EGL_NO_CONTEXT;
    fprintf(stderr, "[egl] eglCreateContext -> %s\n", x != EGL_NO_CONTEXT ? "ok" : "FAILED");
    if (x == EGL_NO_CONTEXT || g_ctx_n >= MAX_TOK) { g_egl_error = eglGetError(); RET(0, 4); }
    g_ctx[g_ctx_n] = x;
    RET(TOK_CONTEXT + (uint32_t)g_ctx_n++, 4);
}
static void sh_eglMakeCurrent(void) {            /* (dpy, draw, read, ctx) */
    if (ds_sdl_active()) RET(ds_sdl_make_current(A32(3) != 0), 4);
    EGLSurface d = A32(1) ? TOK_GET(g_surf, g_surf_n, TOK_SURFACE, A32(1)) : EGL_NO_SURFACE;
    EGLSurface r = A32(2) ? TOK_GET(g_surf, g_surf_n, TOK_SURFACE, A32(2)) : EGL_NO_SURFACE;
    EGLContext x = A32(3) ? TOK_GET(g_ctx, g_ctx_n, TOK_CONTEXT, A32(3)) : EGL_NO_CONTEXT;
    EGLBoolean ok = eglMakeCurrent(g_dpy, d, r, x);
    static int once;
    if (ok && x != EGL_NO_CONTEXT && !once++)
        fprintf(stderr, "[egl] context current: %s | %s | GLSL %s\n", (const char *)glGetString(GL_RENDERER),
                (const char *)glGetString(GL_VERSION), (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION));
    RET(ok, 4);
}

static uint32_t g_frame;
uint32_t ds_frame_count(void) { return g_frame; }
static void dump_frame(uint32_t n) {
    static uint8_t *px;
    if (!px) px = (uint8_t *)malloc((size_t)DS_SCREEN_W * DS_SCREEN_H * 4);
    glReadPixels(0, 0, DS_SCREEN_W, DS_SCREEN_H, GL_RGBA, GL_UNSIGNED_BYTE, px);
    mkdir("build/frames", 0755);
    char path[64]; snprintf(path, sizeof path, "build/frames/frame_%05u.ppm", n);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", DS_SCREEN_W, DS_SCREEN_H);
    for (int y = DS_SCREEN_H - 1; y >= 0; y--)   /* GL rows are bottom-up */
        for (int x = 0; x < DS_SCREEN_W; x++) fwrite(px + 4 * (y * DS_SCREEN_W + x), 1, 3, f);
    fclose(f);
}
static void sh_eglSwapBuffers(void) {            /* (dpy, surface) */
    static int every = -1;
    static struct timespec t0;
    if (every < 0) { const char *e = getenv("DS_DUMP_FRAMES"); every = e ? atoi(e) : 0; clock_gettime(CLOCK_MONOTONIC, &t0); }
    EGLSurface s = TOK_GET(g_surf, g_surf_n, TOK_SURFACE, A32(1));
    g_frame++;
    if (every > 0 && g_frame % (uint32_t)every == 0) dump_frame(g_frame);
    EGLBoolean ok;
    if (ds_sdl_active()) { ds_sdl_swap(); ok = EGL_TRUE; }
    else ok = s ? eglSwapBuffers(g_dpy, s) : EGL_FALSE;
    ds_yield();                                  /* a frame is a natural preemption point */
    static int exit_after = -1;
    if (exit_after < 0) { const char *e = getenv("DS_EXIT_AFTER_FRAMES"); exit_after = e ? atoi(e) : 0; }
    if (exit_after > 0 && g_frame >= (uint32_t)exit_after) {
        fprintf(stderr, "[egl] DS_EXIT_AFTER_FRAMES=%d reached: exiting\n", exit_after);
        exit(0);                                 /* atexit writes the coverage */
    }
    if (g_frame == 1 || g_frame % 120 == 0) {
        struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
        double sec = (double)(t.tv_sec - t0.tv_sec) + (double)(t.tv_nsec - t0.tv_nsec) / 1e9;
        fprintf(stderr, "[egl] frame %u (%.1f fps avg)\n", g_frame, sec > 0 ? g_frame / sec : 0.0);
    }
    RET(ok, 2);
}
static void sh_eglGetProcAddress(void) {         /* (name) -> a shim VA, or NULL if we have none */
    uint32_t va = ds_resolve(ASTR(0));
    fprintf(stderr, "[egl] eglGetProcAddress(%s) -> %s\n", ASTR(0), va ? "shim" : "NULL");
    RET(va, 1);
}
static void sh_eglGetError(void) { uint32_t e = g_egl_error; g_egl_error = EGL_SUCCESS; RET(e != EGL_SUCCESS ? e : (uint32_t)eglGetError(), 0); }

/* ===================== GLES2: straight passthrough, guest pointers translated */
#define U(i) ((GLuint)A32(i))
#define I(i) ((GLint)A32(i))
#define E(i) ((GLenum)A32(i))

static void sh_glActiveTexture(void)        { glActiveTexture(E(0)); RETV(1); }
static void sh_glAttachShader(void)         { glAttachShader(U(0), U(1)); RETV(2); }
static void sh_glBindAttribLocation(void)   { glBindAttribLocation(U(0), U(1), (const GLchar *)P(2)); RETV(3); }
static void sh_glBindBuffer(void)           { glBindBuffer(E(0), U(1)); RETV(2); }
static void sh_glBindFramebuffer(void)      { glBindFramebuffer(E(0), U(1)); RETV(2); }
static void sh_glBindRenderbuffer(void)     { glBindRenderbuffer(E(0), U(1)); RETV(2); }
static void sh_glBindTexture(void)          { glBindTexture(E(0), U(1)); RETV(2); }
static void sh_glBlendEquation(void)        { glBlendEquation(E(0)); RETV(1); }
static void sh_glBlendEquationSeparate(void){ glBlendEquationSeparate(E(0), E(1)); RETV(2); }
static void sh_glBlendFunc(void)            { glBlendFunc(E(0), E(1)); RETV(2); }
static void sh_glBlendFuncSeparate(void)    { glBlendFuncSeparate(E(0), E(1), E(2), E(3)); RETV(4); }
static void sh_glBufferData(void)           { glBufferData(E(0), (GLsizeiptr)A32(1), P(2), E(3)); RETV(4); }
static void sh_glCheckFramebufferStatus(void){ RET(glCheckFramebufferStatus(E(0)), 1); }
static void sh_glClear(void)                { glClear((GLbitfield)A32(0)); RETV(1); }
static void sh_glClearColor(void)           { glClearColor(AF(0), AF(1), AF(2), AF(3)); RETV(4); }
static void sh_glClearStencil(void)         { glClearStencil(I(0)); RETV(1); }
static void sh_glColorMask(void)            { glColorMask((GLboolean)A32(0), (GLboolean)A32(1), (GLboolean)A32(2), (GLboolean)A32(3)); RETV(4); }
static void sh_glCompileShader(void)        { glCompileShader(U(0)); RETV(1); }
/* ---- DXT (S3TC) without the GPU's help
 * Don't Starve's textures are DXT5 (a few DXT1). ANGLE took them as is; a GLES2
 * driver without S3TC (the R36S's Mali-G31) cannot, so they are decoded here
 * and uploaded uncompressed: RGBA4444 by default (half the memory of RGBA8),
 * DS_SOFT_DXT=rgba8 for full precision, DS_SOFT_DXT=1 to force the path. */
#define FMT_DXT1_RGB  0x83F0
#define FMT_DXT1_RGBA 0x83F1
#define FMT_DXT3      0x83F2
#define FMT_DXT5      0x83F3
static int g_soft_dxt = -1, g_dxt_rgba8;
static int soft_dxt(void) {
    if (g_soft_dxt < 0) {
        const char *v = getenv("DS_SOFT_DXT"), *x = (const char *)glGetString(GL_EXTENSIONS);
        g_dxt_rgba8 = v && !strcmp(v, "rgba8");
        g_soft_dxt = (v && *v && strcmp(v, "0")) || !x || !strstr(x, "texture_compression_s3tc");
        if (g_soft_dxt) fprintf(stderr, "[egl] no S3TC on this GPU: DXT textures decoded on the CPU (%s)\n", g_dxt_rgba8 ? "RGBA8" : "RGBA4444");
    }
    return g_soft_dxt;
}
static void rgb565(uint16_t c, uint8_t *o) {
    o[0] = (uint8_t)(((c >> 11) & 31) * 255 / 31); o[1] = (uint8_t)(((c >> 5) & 63) * 255 / 63); o[2] = (uint8_t)((c & 31) * 255 / 31);
}
/* one 4x4 block -> 16 RGBA8 pixels */
static void dxt_block(GLenum f, const uint8_t *b, uint8_t px[16][4]) {
    const uint8_t *cb = f == FMT_DXT3 || f == FMT_DXT5 ? b + 8 : b;
    uint16_t c0 = (uint16_t)(cb[0] | cb[1] << 8), c1 = (uint16_t)(cb[2] | cb[3] << 8);
    uint8_t pal[4][4]; rgb565(c0, pal[0]); rgb565(c1, pal[1]);
    pal[0][3] = pal[1][3] = pal[2][3] = 255; pal[3][3] = 255;
    if (c0 > c1 || f == FMT_DXT3 || f == FMT_DXT5)
        for (int k = 0; k < 3; k++) { pal[2][k] = (uint8_t)((2 * pal[0][k] + pal[1][k]) / 3); pal[3][k] = (uint8_t)((pal[0][k] + 2 * pal[1][k]) / 3); }
    else {
        for (int k = 0; k < 3; k++) { pal[2][k] = (uint8_t)((pal[0][k] + pal[1][k]) / 2); pal[3][k] = 0; }
        if (f == FMT_DXT1_RGBA) pal[3][3] = 0;
    }
    uint32_t idx = (uint32_t)cb[4] | (uint32_t)cb[5] << 8 | (uint32_t)cb[6] << 16 | (uint32_t)cb[7] << 24;
    for (int i = 0; i < 16; i++) memcpy(px[i], pal[(idx >> (2 * i)) & 3], 4);
    if (f == FMT_DXT3)
        for (int i = 0; i < 16; i++) px[i][3] = (uint8_t)(((b[i / 2] >> (4 * (i & 1))) & 15) * 17);
    else if (f == FMT_DXT5) {
        uint8_t a[8]; a[0] = b[0]; a[1] = b[1];
        if (a[0] > a[1]) for (int k = 1; k < 7; k++) a[k + 1] = (uint8_t)(((7 - k) * a[0] + k * a[1]) / 7);
        else { for (int k = 1; k < 5; k++) a[k + 1] = (uint8_t)(((5 - k) * a[0] + k * a[1]) / 5); a[6] = 0; a[7] = 255; }
        uint64_t bits = 0; for (int k = 0; k < 6; k++) bits |= (uint64_t)b[2 + k] << (8 * k);
        for (int i = 0; i < 16; i++) px[i][3] = a[(bits >> (3 * i)) & 7];
    }
}
static int dxt_upload(GLenum target, GLint level, GLenum f, GLsizei w, GLsizei h, const uint8_t *d) {
    if (f != FMT_DXT1_RGB && f != FMT_DXT1_RGBA && f != FMT_DXT3 && f != FMT_DXT5) return 0;
    size_t bs = f == FMT_DXT3 || f == FMT_DXT5 ? 16 : 8;
    int bw = (w + 3) / 4, bh = (h + 3) / 4;
    void *out = malloc((size_t)w * (size_t)h * (g_dxt_rgba8 ? 4 : 2) + 16);
    if (!out) return 0;
    for (int by = 0; by < bh; by++)
        for (int bx = 0; bx < bw; bx++) {
            uint8_t px[16][4];
            if (d) dxt_block(f, d + ((size_t)by * (size_t)bw + (size_t)bx) * bs, px); else memset(px, 0, sizeof px);
            for (int i = 0; i < 16; i++) {
                int x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
                if (x >= w || y >= h) continue;
                size_t o = (size_t)y * (size_t)w + (size_t)x;
                if (g_dxt_rgba8) memcpy((uint8_t *)out + 4 * o, px[i], 4);
                else ((uint16_t *)out)[o] = (uint16_t)((px[i][0] >> 4) << 12 | (px[i][1] >> 4) << 8 | (px[i][2] >> 4) << 4 | (px[i][3] >> 4));
            }
        }
    GLint align; glGetIntegerv(GL_UNPACK_ALIGNMENT, &align);
    glPixelStorei(GL_UNPACK_ALIGNMENT, g_dxt_rgba8 ? 4 : 2);
    glTexImage2D(target, level, GL_RGBA, w, h, 0, GL_RGBA, g_dxt_rgba8 ? GL_UNSIGNED_BYTE : GL_UNSIGNED_SHORT_4_4_4_4, out);
    glPixelStorei(GL_UNPACK_ALIGNMENT, align);
    free(out);
    return 1;
}

static void sh_glCompressedTexImage2D(void) { /* (target, level, ifmt, w, h, border, imageSize, data) */
    if (soft_dxt() && dxt_upload(E(0), I(1), E(2), I(3), I(4), (const uint8_t *)P(7))) RETV(8);
    glCompressedTexImage2D(E(0), I(1), E(2), I(3), I(4), I(5), I(6), P(7)); RETV(8);
}
static void sh_glCreateProgram(void)        { RET(glCreateProgram(), 0); }
static void sh_glCreateShader(void)         { RET(glCreateShader(E(0)), 1); }
static void sh_glCullFace(void)             { glCullFace(E(0)); RETV(1); }
static void sh_glDeleteBuffers(void)        { glDeleteBuffers(I(0), (const GLuint *)P(1)); RETV(2); }
static void sh_glDeleteFramebuffers(void)   { glDeleteFramebuffers(I(0), (const GLuint *)P(1)); RETV(2); }
static void sh_glDeleteTextures(void)       { glDeleteTextures(I(0), (const GLuint *)P(1)); RETV(2); }
static void sh_glDeleteProgram(void)        { glDeleteProgram(U(0)); RETV(1); }
static void sh_glDeleteRenderbuffers(void)  { glDeleteRenderbuffers(I(0), (const GLuint *)P(1)); RETV(2); }
static void sh_glDeleteShader(void)         { glDeleteShader(U(0)); RETV(1); }
static void sh_glDepthFunc(void)            { glDepthFunc(E(0)); RETV(1); }
static void sh_glDepthMask(void)            { glDepthMask((GLboolean)A32(0)); RETV(1); }
static void sh_glDisable(void)              { glDisable(E(0)); RETV(1); }
static void sh_glDisableVertexAttribArray(void){ glDisableVertexAttribArray(U(0)); RETV(1); }
static void sh_glDrawArrays(void)           { glDrawArrays(E(0), I(1), I(2)); RETV(3); }
static void sh_glEnable(void)               { glEnable(E(0)); RETV(1); }
static void sh_glEnableVertexAttribArray(void){ glEnableVertexAttribArray(U(0)); RETV(1); }
static void sh_glFinish(void)               { glFinish(); RETV(0); }
static void sh_glFramebufferRenderbuffer(void){ glFramebufferRenderbuffer(E(0), E(1), E(2), U(3)); RETV(4); }
static void sh_glFramebufferTexture2D(void) { glFramebufferTexture2D(E(0), E(1), E(2), U(3), I(4)); RETV(5); }
static void sh_glFrontFace(void)            { glFrontFace(E(0)); RETV(1); }
static void sh_glGenBuffers(void)           { glGenBuffers(I(0), (GLuint *)P(1)); RETV(2); }
static void sh_glGenFramebuffers(void)      { glGenFramebuffers(I(0), (GLuint *)P(1)); RETV(2); }
static void sh_glGenRenderbuffers(void)     { glGenRenderbuffers(I(0), (GLuint *)P(1)); RETV(2); }
static void sh_glGenTextures(void)          { glGenTextures(I(0), (GLuint *)P(1)); RETV(2); }
static void sh_glGetActiveAttrib(void) {    /* (prog, index, bufSize, *length, *size, *type, name) */
    glGetActiveAttrib(U(0), U(1), I(2), (GLsizei *)P(3), (GLint *)P(4), (GLenum *)P(5), (GLchar *)P(6)); RETV(7);
}
static void sh_glGetAttribLocation(void)    { RET((uint32_t)glGetAttribLocation(U(0), (const GLchar *)P(1)), 2); }
static void sh_glGetError(void)             { RET(glGetError(), 0); }
static void sh_glGetIntegerv(void)          { glGetIntegerv(E(0), (GLint *)P(1)); RETV(2); }
static void sh_glGetProgramiv(void)         { glGetProgramiv(U(0), E(1), (GLint *)P(2)); RETV(3); }
static void sh_glGetProgramInfoLog(void)    { glGetProgramInfoLog(U(0), I(1), (GLsizei *)P(2), (GLchar *)P(3)); RETV(4); }
static void sh_glGetShaderiv(void)          { glGetShaderiv(U(0), E(1), (GLint *)P(2)); RETV(3); }
static void sh_glGetShaderInfoLog(void)     { glGetShaderInfoLog(U(0), I(1), (GLsizei *)P(2), (GLchar *)P(3)); RETV(4); }
static void sh_glGetString(void) {          /* host string -> a guest copy, cached per name */
    static struct { GLenum e; uint32_t va; } cache[16]; static int n;
    GLenum e = E(0);
    for (int i = 0; i < n; i++) if (cache[i].e == e) RET(cache[i].va, 1);
    const char *s = (const char *)glGetString(e);
    if (!s) RET(0, 1);
    static const char dxt[] = " GL_EXT_texture_compression_dxt1 GL_ANGLE_texture_compression_dxt3"
                              " GL_ANGLE_texture_compression_dxt5 GL_EXT_texture_compression_s3tc";
    int add = e == GL_EXTENSIONS && soft_dxt();                 /* decoded here, so still on offer */
    uint32_t len = (uint32_t)strlen(s) + 1, va = ds_arena_alloc(len + (add ? (uint32_t)sizeof dxt : 0), 4);
    memcpy(ds_hostptr(va), s, len);
    if (add) memcpy((char *)ds_hostptr(va) + len - 1, dxt, sizeof dxt);
    if (n < 16) { cache[n].e = e; cache[n].va = va; n++; }
    RET(va, 1);
}
static void sh_glGetUniformLocation(void)   { RET((uint32_t)glGetUniformLocation(U(0), (const GLchar *)P(1)), 2); }
static void sh_glLinkProgram(void)          { glLinkProgram(U(0)); RETV(1); }
static void sh_glPixelStorei(void)          { glPixelStorei(E(0), I(1)); RETV(2); }
static void sh_glPolygonOffset(void)        { glPolygonOffset(AF(0), AF(1)); RETV(2); }
static void sh_glRenderbufferStorage(void)  { glRenderbufferStorage(E(0), E(1), I(2), I(3)); RETV(4); }
static void sh_glScissor(void)              { glScissor(I(0), I(1), I(2), I(3)); RETV(4); }
/* ANGLE accepts `#extension` anywhere; GLSL ES (Mesa, and the Mali driver)
 * wants every one before any other code. The game's shaders are assembled
 * from includes, so their #extension lines land mid-file: hoist them to the
 * top (after #version), leaving a blank line in place. */
static char *hoist_extensions(const char *src) {
    size_t n = strlen(src);
    char *ext = (char *)malloc(n + 1), *body = (char *)malloc(n + 1), *ver = (char *)malloc(n + 1);
    size_t ne = 0, nb = 0, nv = 0;
    for (const char *p = src; *p; ) {
        const char *e = strchr(p, '\n'); size_t len = e ? (size_t)(e - p + 1) : strlen(p);
        const char *q = p; while (q < p + len && (*q == ' ' || *q == '\t')) q++;
        if (!strncmp(q, "#extension", 10)) { memcpy(ext + ne, p, len); ne += len; if (!e) ext[ne++] = '\n'; body[nb++] = '\n'; }
        else if (!strncmp(q, "#version", 8) && nb == 0 && nv == 0) { memcpy(ver + nv, p, len); nv += len; }
        else { memcpy(body + nb, p, len); nb += len; }
        p += len;
    }
    char *out = (char *)malloc(nv + ne + nb + 1);
    memcpy(out, ver, nv); memcpy(out + nv, ext, ne); memcpy(out + nv + ne, body, nb); out[nv + ne + nb] = 0;
    free(ext); free(body); free(ver);
    return out;
}
static void sh_glShaderSource(void) {       /* (shader, count, const char *const *strings, const GLint *lengths) */
    GLsizei n = I(1);
    size_t total = 0;
    for (GLsizei i = 0; i < n; i++) {
        uint32_t sva = MEM32(A32(2) + 4u * (uint32_t)i);
        GLint len = A32(3) ? (GLint)MEM32(A32(3) + 4u * (uint32_t)i) : -1;
        total += sva ? (len < 0 ? strlen((const char *)ds_hostptr(sva)) : (size_t)len) : 0;
    }
    char *joined = (char *)malloc(total + 1); size_t at = 0;
    for (GLsizei i = 0; i < n; i++) {
        uint32_t sva = MEM32(A32(2) + 4u * (uint32_t)i);
        GLint len = A32(3) ? (GLint)MEM32(A32(3) + 4u * (uint32_t)i) : -1;
        if (!sva) continue;
        size_t l = len < 0 ? strlen((const char *)ds_hostptr(sva)) : (size_t)len;
        memcpy(joined + at, ds_hostptr(sva), l); at += l;
    }
    joined[at] = 0;
    char *src = hoist_extensions(joined);
    const GLchar *one = src;
    glShaderSource(U(0), 1, &one, NULL);
    free(src); free(joined);
    RETV(4);
}
static void sh_glStencilFunc(void)          { glStencilFunc(E(0), I(1), U(2)); RETV(3); }
static void sh_glStencilMask(void)          { glStencilMask(U(0)); RETV(1); }
static void sh_glStencilOp(void)            { glStencilOp(E(0), E(1), E(2)); RETV(3); }
static void sh_glTexImage2D(void) {         /* (target, level, ifmt, w, h, border, fmt, type, pixels) */
    glTexImage2D(E(0), I(1), I(2), I(3), I(4), I(5), E(6), E(7), P(8)); RETV(9);
}
static void sh_glTexParameteri(void)        { glTexParameteri(E(0), E(1), I(2)); RETV(3); }
static void sh_glUniform1f(void)            { glUniform1f(I(0), AF(1)); RETV(2); }
static void sh_glUniform1fv(void)           { glUniform1fv(I(0), I(1), (const GLfloat *)P(2)); RETV(3); }
static void sh_glUniform1i(void)            { glUniform1i(I(0), I(1)); RETV(2); }
static void sh_glUniform2fv(void)           { glUniform2fv(I(0), I(1), (const GLfloat *)P(2)); RETV(3); }
static void sh_glUniform3fv(void)           { glUniform3fv(I(0), I(1), (const GLfloat *)P(2)); RETV(3); }
static void sh_glUniform4fv(void)           { glUniform4fv(I(0), I(1), (const GLfloat *)P(2)); RETV(3); }
static void sh_glUniformMatrix2fv(void)     { glUniformMatrix2fv(I(0), I(1), (GLboolean)A32(2), (const GLfloat *)P(3)); RETV(4); }
static void sh_glUniformMatrix3fv(void)     { glUniformMatrix3fv(I(0), I(1), (GLboolean)A32(2), (const GLfloat *)P(3)); RETV(4); }
static void sh_glUniformMatrix4fv(void)     { glUniformMatrix4fv(I(0), I(1), (GLboolean)A32(2), (const GLfloat *)P(3)); RETV(4); }
static void sh_glUseProgram(void)           { glUseProgram(U(0)); RETV(1); }
static void sh_glVertexAttribPointer(void) {/* (index, size, type, normalized, stride, ptr) */
    /* with an array buffer bound, ptr is an offset into it; without, a client (guest) pointer */
    GLint vbo = 0; glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &vbo);
    const void *ptr = vbo ? (const void *)(uintptr_t)A32(5) : P(5);
    glVertexAttribPointer(U(0), I(1), E(2), (GLboolean)A32(3), I(4), ptr);
    RETV(6);
}
static void sh_glViewport(void) {
    static int logged;
    if (logged < 4 && (I(2) != DS_SCREEN_W || I(3) != DS_SCREEN_H)) { logged++; fprintf(stderr, "[gl] glViewport(%d, %d, %d, %d)\n", I(0), I(1), I(2), I(3)); }
    glViewport(I(0), I(1), I(2), I(3)); RETV(4);
}

/* name + "dll#ordinal" (ordinals from the game's own ANGLE DLLs' export tables) */
#define EGL(ord, name)  { #name, sh_##name }, { "libEGL.dll#" #ord, sh_##name }
#define GL(ord, name)   { #name, sh_##name }, { "libGLESv2.dll#" #ord, sh_##name }
const ds_shim ds_gles[] = {
    EGL(1, eglGetError), EGL(2, eglGetDisplay), EGL(3, eglInitialize), EGL(6, eglGetConfigs),
    EGL(7, eglChooseConfig), EGL(9, eglCreateWindowSurface), EGL(23, eglCreateContext),
    EGL(25, eglMakeCurrent), EGL(32, eglSwapBuffers), EGL(34, eglGetProcAddress),
    GL(1, glActiveTexture), GL(2, glAttachShader), GL(3, glBindAttribLocation), GL(4, glBindBuffer),
    GL(5, glBindFramebuffer), GL(6, glBindRenderbuffer), GL(7, glBindTexture), GL(9, glBlendEquation),
    GL(10, glBlendEquationSeparate), GL(11, glBlendFunc), GL(12, glBlendFuncSeparate),
    GL(13, glBufferData), GL(15, glCheckFramebufferStatus), GL(16, glClear), GL(17, glClearColor),
    GL(19, glClearStencil), GL(20, glColorMask), GL(21, glCompileShader),
    GL(22, glCompressedTexImage2D), GL(26, glCreateProgram), GL(27, glCreateShader),
    GL(28, glCullFace), GL(29, glDeleteBuffers), GL(30, glDeleteFramebuffers),
    GL(31, glDeleteTextures), GL(32, glDeleteProgram), GL(33, glDeleteRenderbuffers),
    GL(34, glDeleteShader), GL(36, glDepthFunc), GL(37, glDepthMask), GL(39, glDisable),
    GL(40, glDisableVertexAttribArray), GL(41, glDrawArrays), GL(43, glEnable),
    GL(44, glEnableVertexAttribArray), GL(45, glFinish), GL(47, glFramebufferRenderbuffer),
    GL(48, glFramebufferTexture2D), GL(49, glFrontFace), GL(50, glGenBuffers),
    GL(52, glGenFramebuffers), GL(53, glGenRenderbuffers), GL(54, glGenTextures),
    GL(55, glGetActiveAttrib), GL(58, glGetAttribLocation), GL(61, glGetError),
    GL(64, glGetIntegerv), GL(65, glGetProgramiv), GL(66, glGetProgramInfoLog),
    GL(68, glGetShaderiv), GL(69, glGetShaderInfoLog), GL(72, glGetString),
    GL(77, glGetUniformLocation), GL(90, glLinkProgram), GL(91, glPixelStorei),
    GL(92, glPolygonOffset), GL(95, glRenderbufferStorage), GL(97, glScissor),
    GL(99, glShaderSource), GL(100, glStencilFunc), GL(102, glStencilMask), GL(104, glStencilOp),
    GL(106, glTexImage2D), GL(109, glTexParameteri), GL(112, glUniform1f), GL(113, glUniform1fv),
    GL(114, glUniform1i), GL(117, glUniform2fv), GL(121, glUniform3fv), GL(125, glUniform4fv),
    GL(128, glUniformMatrix2fv), GL(129, glUniformMatrix3fv), GL(130, glUniformMatrix4fv),
    GL(131, glUseProgram), GL(141, glVertexAttribPointer), GL(142, glViewport),
    { 0, 0 }
};
