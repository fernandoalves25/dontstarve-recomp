/*
 * ds_sdl.c - the SDL2 backend: the real screen and buttons (DS_VIDEO=sdl).
 *
 * On the R36S (ArkOS / PortMaster) SDL2 drives the panel through KMSDRM and
 * gives a GLES2 context on the Mali GPU; on a desktop it opens a window. The
 * EGL bridge (ds_gles.c) asks this file for the window, the context and the
 * swap; USER32 (ds_user32.c) asks it to pump events into the message queue;
 * XInput (ds_input.c) reads the first SDL game controller as pad 0 -- Don't
 * Starve already plays with an XInput pad, so the device's buttons need no
 * remapping in the game. Without DS_SDL (no SDL2 at build time) every entry
 * point reports "not active" and the headless path is used.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ds_hle.h"

#ifdef DS_SDL
#include <SDL2/SDL.h>

static SDL_Window *g_win;
static SDL_GLContext g_ctx;
static SDL_GameController *g_pad;
static int g_active = -1;

int ds_sdl_active(void) {
    if (g_active < 0) { const char *v = getenv("DS_VIDEO"); g_active = v && !strcmp(v, "sdl"); }
    return g_active;
}

/* Create the window + GLES2 context (once). 0 on success. */
int ds_sdl_open(int w, int h) {
    if (g_win) return 0;
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "[sdl] SDL_Init: %s\n", SDL_GetError()); return -1;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN;
    if (!getenv("DS_WINDOWED")) flags |= SDL_WINDOW_FULLSCREEN;
    g_win = SDL_CreateWindow("Don't Starve", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w, h, flags);
    if (!g_win) { fprintf(stderr, "[sdl] SDL_CreateWindow: %s\n", SDL_GetError()); return -1; }
    g_ctx = SDL_GL_CreateContext(g_win);
    if (!g_ctx) { fprintf(stderr, "[sdl] SDL_GL_CreateContext: %s\n", SDL_GetError()); return -1; }
    SDL_GL_SetSwapInterval(1);
    SDL_ShowCursor(SDL_DISABLE);
    for (int i = 0; i < SDL_NumJoysticks() && !g_pad; i++)
        if (SDL_IsGameController(i)) g_pad = SDL_GameControllerOpen(i);
    fprintf(stderr, "[sdl] %s window %dx%d, GLES2 context, video driver %s, pad: %s\n",
            (flags & SDL_WINDOW_FULLSCREEN) ? "full-screen" : "windowed", w, h,
            SDL_GetCurrentVideoDriver(), g_pad ? SDL_GameControllerName(g_pad) : "none");
    return 0;
}
int  ds_sdl_make_current(int on) { return SDL_GL_MakeCurrent(g_win, on ? g_ctx : NULL) == 0; }
void ds_sdl_swap(void) { SDL_GL_SwapWindow(g_win); }

/* Map an SDL key to a Windows virtual key (the keys the game binds). */
static uint32_t vk_of(SDL_Keycode k) {
    if (k >= SDLK_a && k <= SDLK_z) return 'A' + (uint32_t)(k - SDLK_a);
    if (k >= SDLK_0 && k <= SDLK_9) return '0' + (uint32_t)(k - SDLK_0);
    if (k >= SDLK_F1 && k <= SDLK_F12) return 0x70 + (uint32_t)(k - SDLK_F1);
    switch (k) {
    case SDLK_RETURN: return 0x0D; case SDLK_ESCAPE: return 0x1B; case SDLK_SPACE: return 0x20;
    case SDLK_TAB: return 0x09; case SDLK_BACKSPACE: return 0x08;
    case SDLK_LEFT: return 0x25; case SDLK_UP: return 0x26; case SDLK_RIGHT: return 0x27; case SDLK_DOWN: return 0x28;
    case SDLK_LSHIFT: case SDLK_RSHIFT: return 0x10; case SDLK_LCTRL: case SDLK_RCTRL: return 0x11;
    case SDLK_LALT: case SDLK_RALT: return 0x12;
    }
    return 0;
}
/* Turn pending SDL events into Win32 messages (post(msg, wParam, lParam)). */
void ds_sdl_pump(void (*post)(uint32_t msg, uint32_t wp, uint32_t lp)) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_QUIT: post(0x0010u /*WM_CLOSE*/, 0, 0); break;
        case SDL_KEYDOWN: case SDL_KEYUP: {
            uint32_t vk = vk_of(e.key.keysym.sym);
            if (vk) post(e.type == SDL_KEYDOWN ? 0x0100u : 0x0101u, vk, e.type == SDL_KEYDOWN ? 1u : 0xC0000001u);
            break; }
        case SDL_MOUSEMOTION:
            post(0x0200u, 0, (uint32_t)(e.motion.x & 0xFFFF) | ((uint32_t)e.motion.y << 16)); break;
        case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP: {
            int down = e.type == SDL_MOUSEBUTTONDOWN;
            uint32_t lp = (uint32_t)(e.button.x & 0xFFFF) | ((uint32_t)e.button.y << 16);
            if (e.button.button == SDL_BUTTON_LEFT)  post(down ? 0x0201u : 0x0202u, down, lp);
            if (e.button.button == SDL_BUTTON_RIGHT) post(down ? 0x0204u : 0x0205u, down ? 2u : 0u, lp);
            break; }
        case SDL_CONTROLLERDEVICEADDED:
            if (!g_pad) { g_pad = SDL_GameControllerOpen(e.cdevice.which);
                          fprintf(stderr, "[sdl] pad connected: %s\n", g_pad ? SDL_GameControllerName(g_pad) : "?"); }
            break;
        case SDL_CONTROLLERDEVICEREMOVED:
            if (g_pad && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(g_pad)) == e.cdevice.which) {
                SDL_GameControllerClose(g_pad); g_pad = NULL;
            }
            break;
        }
    }
}

/* XINPUT_GAMEPAD from the SDL pad: wButtons, bLeftTrigger, bRightTrigger,
 * sThumbLX/LY/RX/RY (Y up, as XInput). 1 if a pad is connected. */
int ds_sdl_xinput(uint16_t *buttons, uint8_t *lt, uint8_t *rt, int16_t thumbs[4]) {
    if (!g_pad) return 0;
    static const struct { SDL_GameControllerButton b; uint16_t x; } map[] = {
        { SDL_CONTROLLER_BUTTON_DPAD_UP, 0x0001 }, { SDL_CONTROLLER_BUTTON_DPAD_DOWN, 0x0002 },
        { SDL_CONTROLLER_BUTTON_DPAD_LEFT, 0x0004 }, { SDL_CONTROLLER_BUTTON_DPAD_RIGHT, 0x0008 },
        { SDL_CONTROLLER_BUTTON_START, 0x0010 }, { SDL_CONTROLLER_BUTTON_BACK, 0x0020 },
        { SDL_CONTROLLER_BUTTON_LEFTSTICK, 0x0040 }, { SDL_CONTROLLER_BUTTON_RIGHTSTICK, 0x0080 },
        { SDL_CONTROLLER_BUTTON_LEFTSHOULDER, 0x0100 }, { SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, 0x0200 },
        { SDL_CONTROLLER_BUTTON_A, 0x1000 }, { SDL_CONTROLLER_BUTTON_B, 0x2000 },
        { SDL_CONTROLLER_BUTTON_X, 0x4000 }, { SDL_CONTROLLER_BUTTON_Y, 0x8000 },
    };
    uint16_t b = 0;
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++) if (SDL_GameControllerGetButton(g_pad, map[i].b)) b |= map[i].x;
    *buttons = b;
    *lt = (uint8_t)(SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) >> 7);
    *rt = (uint8_t)(SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) >> 7);
    int16_t ly = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_LEFTY);
    int16_t ry = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_RIGHTY);
    thumbs[0] = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_LEFTX);
    thumbs[1] = (int16_t)(ly == -32768 ? 32767 : -ly);
    thumbs[2] = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_RIGHTX);
    thumbs[3] = (int16_t)(ry == -32768 ? 32767 : -ry);
    return 1;
}

#else  /* built without SDL2: headless only */
int  ds_sdl_active(void) { return 0; }
int  ds_sdl_open(int w, int h) { (void)w; (void)h; return -1; }
int  ds_sdl_make_current(int on) { (void)on; return 0; }
void ds_sdl_swap(void) {}
void ds_sdl_pump(void (*post)(uint32_t, uint32_t, uint32_t)) { (void)post; }
int  ds_sdl_xinput(uint16_t *b, uint8_t *lt, uint8_t *rt, int16_t t[4]) { (void)b; (void)lt; (void)rt; (void)t; return 0; }
#endif
