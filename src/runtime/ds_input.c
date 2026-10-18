/*
 * ds_input.c - XInput 1.3 + DirectInput 8. Pad 0 is the SDL game controller
 * (the R36S buttons, ds_sdl.c) and/or the scripted DS_PAD; DirectInput lists
 * no devices.
 * XINPUT1_3 is imported by ordinal: 2 GetState, 3 SetState, 4 GetCapabilities,
 * 5 Enable, 8 GetKeystroke. All WINAPI (stdcall).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ds_hle.h"

#define ERROR_DEVICE_NOT_CONNECTED 1167u

/* A scripted pad for headless tests (DS_PAD), connected as pad 0 when set:
 *   "F buttons HEX N"   hold XINPUT_GAMEPAD_* bits HEX for N frames from frame F
 *   "F stick LX LY N"   left stick at (LX, LY), -32768..32767, Y up
 * e.g. DS_PAD="900 buttons 1000 10; 3000 stick 0 32000 120" (A, then walk up). */
#define MAX_PAD 128
static struct { uint32_t f, n; uint16_t buttons; int16_t lx, ly; int stick; } g_pad[MAX_PAD];
static int g_npad = -1;
static void pad_load(void) {
    g_npad = 0;
    const char *e = getenv("DS_PAD");
    while (e && *e && g_npad < MAX_PAD) {
        unsigned f, n, b; int x, y, used = 0; char what[16];
        if (sscanf(e, " %u %15s %n", &f, what, &used) < 2) break;
        e += used;
        if (!strcmp(what, "buttons") && sscanf(e, "%x %u", &b, &n) == 2) {
            g_pad[g_npad].f = f; g_pad[g_npad].n = n; g_pad[g_npad].buttons = (uint16_t)b; g_npad++;
        } else if (!strcmp(what, "stick") && sscanf(e, "%d %d %u", &x, &y, &n) == 3) {
            g_pad[g_npad].f = f; g_pad[g_npad].n = n; g_pad[g_npad].lx = (int16_t)x; g_pad[g_npad].ly = (int16_t)y;
            g_pad[g_npad].stick = 1; g_npad++;
        }
        while (*e && *e != ';') e++;
        if (*e == ';') e++;
    }
    if (getenv("DS_PAD")) fprintf(stderr, "[input] DS_PAD: %d scripted pad events (pad 0 connected)\n", g_npad);
}
/* pad 0: the SDL game controller, plus the scripted pad. 1 if connected. */
static int pad_state(uint16_t *b, uint8_t *lt, uint8_t *rt, int16_t th[4]) {
    if (g_npad < 0) pad_load();
    int on = ds_sdl_xinput(b, lt, rt, th);
    if (!on) { *b = 0; *lt = *rt = 0; memset(th, 0, 4 * sizeof th[0]); }
    if (!getenv("DS_PAD")) return on;
    uint32_t fr = ds_frame_count();
    for (int i = 0; i < g_npad; i++) {
        if (fr < g_pad[i].f || fr >= g_pad[i].f + g_pad[i].n) continue;
        if (g_pad[i].stick) { th[0] = g_pad[i].lx; th[1] = g_pad[i].ly; } else *b |= g_pad[i].buttons;
    }
    return 1;
}
static void sh_XInputEnable(void)          { RETV(1); }                               /* (BOOL) */
static void sh_XInputGetState(void) {      /* (idx, XINPUT_STATE*): pad 0 is the SDL game controller */
    uint32_t idx = A32(0), p = A32(1);
    uint16_t b; uint8_t lt, rt; int16_t th[4];
    if (idx != 0 || !pad_state(&b, &lt, &rt, th)) RET(ERROR_DEVICE_NOT_CONNECTED, 2);
    static uint32_t packet; static uint16_t lastb; static int16_t lastth[4];
    if (b != lastb || memcmp(th, lastth, sizeof lastth)) { packet++; lastb = b; memcpy(lastth, th, sizeof lastth); }
    /* XINPUT_STATE: dwPacketNumber, XINPUT_GAMEPAD { wButtons, bLeftTrigger, bRightTrigger, sThumbLX/LY/RX/RY } */
    MEM32(p) = packet; MEM16(p + 4) = b; MEM8(p + 6) = lt; MEM8(p + 7) = rt;
    for (int i = 0; i < 4; i++) MEM16(p + 8 + 2u * (uint32_t)i) = (uint16_t)th[i];
    RET(0, 2);
}
static void sh_XInputSetState(void)        { RET(ERROR_DEVICE_NOT_CONNECTED, 2); }    /* (idx, XINPUT_VIBRATION*) */
static void sh_XInputGetCapabilities(void) {   /* (idx, flags, XINPUT_CAPABILITIES*) */
    uint16_t b; uint8_t lt, rt; int16_t th[4];
    uint32_t idx = A32(0), p = A32(2);
    if (idx != 0 || !pad_state(&b, &lt, &rt, th)) RET(ERROR_DEVICE_NOT_CONNECTED, 3);
    memset(ds_hostptr(p), 0, 20);                /* Type, SubType, Flags, Gamepad caps, Vibration */
    MEM8(p) = 1; MEM8(p + 1) = 1;                /* XINPUT_DEVTYPE_GAMEPAD, XINPUT_DEVSUBTYPE_GAMEPAD */
    MEM16(p + 4) = 0xF3FF; MEM8(p + 6) = 0xFF; MEM8(p + 7) = 0xFF;
    for (int i = 0; i < 4; i++) MEM16(p + 8 + 2u * (uint32_t)i) = 0xFFC0;
    RET(0, 3);
}
static void sh_XInputGetKeystroke(void)    { RET(ERROR_DEVICE_NOT_CONNECTED, 3); }    /* (idx, reserved, XINPUT_KEYSTROKE*) */

/* ===================== DirectInput 8: an interface with no devices.
 * The game does not check DirectInput8Create's result before calling
 * EnumDevices on the interface, so it must exist. Its vtable lives in guest
 * memory and points at the shim VAs below (COM = stdcall, `this` first).
 * IDirectInput8A: 0 QueryInterface 1 AddRef 2 Release 3 CreateDevice
 * 4 EnumDevices 5 GetDeviceStatus 6 RunControlPanel 7 Initialize 8 FindDevice
 * 9 EnumDevicesBySemantics 10 ConfigureDevices */
#define DI_OK                0u
#define E_NOINTERFACE        0x80004002u
#define DIERR_DEVICENOTREG   0x80040154u
#define DI_NOTATTACHED       1u
static void sh_DI8_QueryInterface(void)   { if (A32(2)) MEM32(A32(2)) = 0; RET(E_NOINTERFACE, 3); }
static void sh_DI8_AddRef(void)           { RET(1, 1); }
static void sh_DI8_Release(void)          { RET(0, 1); }
static void sh_DI8_CreateDevice(void)     { if (A32(2)) MEM32(A32(2)) = 0; RET(DIERR_DEVICENOTREG, 4); }
static void sh_DI8_EnumDevices(void)      { RET(DI_OK, 5); }            /* (this, type, cb, ref, flags): none */
static void sh_DI8_GetDeviceStatus(void)  { RET(DI_NOTATTACHED, 2); }
static void sh_DI8_RunControlPanel(void)  { RET(DI_OK, 3); }
static void sh_DI8_Initialize(void)       { RET(DI_OK, 3); }
static void sh_DI8_FindDevice(void)       { RET(DIERR_DEVICENOTREG, 4); }
static void sh_DI8_EnumDevicesBySemantics(void) { RET(DI_OK, 6); }
static void sh_DI8_ConfigureDevices(void) { RET(DI_OK, 5); }
static const char *const k_di8_methods[] = {
    "IDirectInput8A::QueryInterface", "IDirectInput8A::AddRef", "IDirectInput8A::Release",
    "IDirectInput8A::CreateDevice", "IDirectInput8A::EnumDevices", "IDirectInput8A::GetDeviceStatus",
    "IDirectInput8A::RunControlPanel", "IDirectInput8A::Initialize", "IDirectInput8A::FindDevice",
    "IDirectInput8A::EnumDevicesBySemantics", "IDirectInput8A::ConfigureDevices",
};
static uint32_t g_di8;                            /* the one interface (guest VA) */
/* DirectInput8Create(hinst, dwVersion, riidltf, ppvOut, punkOuter) */
static void sh_DirectInput8Create(void) {
    if (!g_di8) {
        uint32_t n = sizeof k_di8_methods / sizeof k_di8_methods[0];
        uint32_t vt = ds_arena_alloc(4 * n, 4);
        for (uint32_t i = 0; i < n; i++) MEM32(vt + 4 * i) = ds_resolve(k_di8_methods[i]);
        g_di8 = ds_arena_alloc(16, 4);
        MEM32(g_di8) = vt;
    }
    if (A32(3)) MEM32(A32(3)) = g_di8;
    fprintf(stderr, "[input] DirectInput8Create -> interface with no devices (headless)\n");
    RET(DI_OK, 5);
}

#define XI(ord, name) { #name, sh_##name }, { "XINPUT1_3.dll#" #ord, sh_##name }
const ds_shim ds_input[] = {
    XI(2, XInputGetState), XI(3, XInputSetState), XI(4, XInputGetCapabilities),
    XI(5, XInputEnable), XI(8, XInputGetKeystroke),
    { "DirectInput8Create", sh_DirectInput8Create },
    { "IDirectInput8A::QueryInterface", sh_DI8_QueryInterface }, { "IDirectInput8A::AddRef", sh_DI8_AddRef },
    { "IDirectInput8A::Release", sh_DI8_Release }, { "IDirectInput8A::CreateDevice", sh_DI8_CreateDevice },
    { "IDirectInput8A::EnumDevices", sh_DI8_EnumDevices }, { "IDirectInput8A::GetDeviceStatus", sh_DI8_GetDeviceStatus },
    { "IDirectInput8A::RunControlPanel", sh_DI8_RunControlPanel }, { "IDirectInput8A::Initialize", sh_DI8_Initialize },
    { "IDirectInput8A::FindDevice", sh_DI8_FindDevice },
    { "IDirectInput8A::EnumDevicesBySemantics", sh_DI8_EnumDevicesBySemantics },
    { "IDirectInput8A::ConfigureDevices", sh_DI8_ConfigureDevices },
    { 0, 0 }
};
