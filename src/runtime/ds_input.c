/*
 * ds_input.c - XInput 1.3 + DirectInput 8. First pass: headless, so no pad
 * is connected (ERROR_DEVICE_NOT_CONNECTED) and DirectInput lists no devices.
 * An SDL2 GameController backend (the R36S buttons) replaces this later.
 * XINPUT1_3 is imported by ordinal: 2 GetState, 3 SetState, 4 GetCapabilities,
 * 5 Enable, 8 GetKeystroke. All WINAPI (stdcall).
 */
#include <stdio.h>
#include <string.h>
#include "ds_hle.h"

#define ERROR_DEVICE_NOT_CONNECTED 1167u

static void sh_XInputEnable(void)          { RETV(1); }                               /* (BOOL) */
static void sh_XInputGetState(void)        { RET(ERROR_DEVICE_NOT_CONNECTED, 2); }    /* (idx, XINPUT_STATE*) */
static void sh_XInputSetState(void)        { RET(ERROR_DEVICE_NOT_CONNECTED, 2); }    /* (idx, XINPUT_VIBRATION*) */
static void sh_XInputGetCapabilities(void) { RET(ERROR_DEVICE_NOT_CONNECTED, 3); }    /* (idx, flags, XINPUT_CAPABILITIES*) */
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
