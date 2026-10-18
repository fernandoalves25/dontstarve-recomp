/*
 * ds_win32.c - small Win32/platform surfaces the exe touches before its window
 * exists: WINMM timers, steam_api, COM init, the event log, psapi, shell32
 * folder queries. Shim ABI in ds_hle.h (WINAPI = stdcall: RET pops the args;
 * steam_api is S_CALLTYPE = __cdecl: argc 0).
 */
#include <stdio.h>
#include <string.h>
#include "ds_hle.h"

/* ===================== WINMM */
static void sh_timeBeginPeriod(void) { RET(0, 1); }   /* TIMERR_NOERROR */
static void sh_timeEndPeriod(void)   { RET(0, 1); }

/* ===================== steam_api (__cdecl)
 * "Not running under Steam": the game's offline path. A DRM-free run of the
 * user's own copy -- no licence is ever forged. */
static void sh_SteamAPI_RestartAppIfNecessary(void) { RET(0, 0); }   /* false: keep running */
static void sh_SteamAPI_Init(void)          { fprintf(stderr, "[ds] SteamAPI_Init -> false (offline)\n"); RET(0, 0); }
static void sh_SteamAPI_Shutdown(void)      { RET(0, 0); }
static void sh_SteamAPI_RunCallbacks(void)  { RET(0, 0); }
static void sh_SteamAPI_GetHSteamUser(void) { RET(0, 0); }
static void sh_SteamAPI_GetHSteamPipe(void) { RET(0, 0); }
static void sh_SteamAPI_RegisterCallResult(void)   { RET(0, 0); }
static void sh_SteamAPI_UnregisterCallResult(void) { RET(0, 0); }
static void sh_SteamAPI_WriteMiniDump(void)        { RET(0, 0); }
static void sh_SteamAPI_SetMiniDumpComment(void)   { RET(0, 0); }
static void sh_SteamInternal_CreateInterface(void) { RET(0, 0); }    /* NULL interface */
static void sh_SteamInternal_ContextInit(void)     { RET(0, 0); }

/* ===================== ole32 / oleaut32 */
#define S_OK            0u
#define E_NOINTERFACE   0x80004002u
#define REGDB_E_CLASSNOTREG 0x80040154u
static void sh_CoInitialize(void)          { RET(S_OK, 1); }
static void sh_CoInitializeEx(void)        { RET(S_OK, 2); }
static void sh_CoUninitialize(void)        { RETV(0); }
static void sh_CoInitializeSecurity(void)  { RET(S_OK, 9); }
static void sh_CoSetProxyBlanket(void)     { RET(S_OK, 8); }
static void sh_CoCreateInstance(void) {    /* (clsid, outer, ctx, iid, *ppv) */
    if (A32(4)) MEM32(A32(4)) = 0;
    RET(REGDB_E_CLASSNOTREG, 5);
}
static void sh_SysAllocString(void)        { RET(0, 1); }
static void sh_SysFreeString(void)         { RETV(1); }
static void sh_VariantClear(void)          { RET(S_OK, 1); }

/* ===================== advapi32 event log */
static void sh_RegisterEventSourceA(void)  { RET(ds_handle_new(1, 0), 2); }
static void sh_DeregisterEventSource(void) { RET(1, 1); }
static void sh_ReportEventA(void)          { RET(1, 9); }

/* ===================== psapi */
static void sh_GetProcessMemoryInfo(void) {   /* (proc, PROCESS_MEMORY_COUNTERS*, cb) */
    uint32_t p = A32(1), cb = A32(2);
    if (p && cb) { memset(ds_hostptr(p), 0, cb); MEM32(p) = cb; }
    RET(1, 3);
}

/* ===================== shell32: the user's folders live under Z:\home */
#define DS_HOME "Z:\\home"
static void sh_SHGetFolderPathW(void) {      /* (hwnd, csidl, token, flags, pszPath[MAX_PATH]) */
    const char *s = DS_HOME "\\Documents";
    uint32_t d = A32(4);
    for (uint32_t i = 0; ; i++) { MEM16(d + 2 * i) = (uint8_t)s[i]; if (!s[i]) break; }
    RET(S_OK, 5);
}
static void sh_SHCreateDirectoryExA(void)  { fprintf(stderr, "[ds] SHCreateDirectoryExA(%s)\n", ASTR(1)); RET(0, 3); }
static void sh_ShellExecuteA(void)         { RET(2, 6); }      /* <= 32: failure (file not found) */
static void sh_SHBrowseForFolderA(void)    { RET(0, 1); }      /* cancelled */
static void sh_SHGetPathFromIDListA(void)  { RET(0, 2); }

/* ===================== console: report "not a console" so the CRT's _write
 * takes its WriteFile path (ds_kernel32 forwards std handles to the host). */
static void sh_GetConsoleMode(void)      { RET(0, 2); }
static void sh_GetConsoleCP(void)        { RET(437, 0); }
static void sh_GetConsoleOutputCP(void)  { RET(437, 0); }

const ds_shim ds_win32[] = {
    { "GetConsoleMode", sh_GetConsoleMode }, { "GetConsoleCP", sh_GetConsoleCP },
    { "GetConsoleOutputCP", sh_GetConsoleOutputCP },
    { "timeBeginPeriod", sh_timeBeginPeriod }, { "timeEndPeriod", sh_timeEndPeriod },
    { "SteamAPI_RestartAppIfNecessary", sh_SteamAPI_RestartAppIfNecessary },
    { "SteamAPI_Init", sh_SteamAPI_Init }, { "SteamAPI_Shutdown", sh_SteamAPI_Shutdown },
    { "SteamAPI_RunCallbacks", sh_SteamAPI_RunCallbacks },
    { "SteamAPI_GetHSteamUser", sh_SteamAPI_GetHSteamUser },
    { "SteamAPI_GetHSteamPipe", sh_SteamAPI_GetHSteamPipe },
    { "SteamAPI_RegisterCallResult", sh_SteamAPI_RegisterCallResult },
    { "SteamAPI_UnregisterCallResult", sh_SteamAPI_UnregisterCallResult },
    { "SteamAPI_WriteMiniDump", sh_SteamAPI_WriteMiniDump },
    { "SteamAPI_SetMiniDumpComment", sh_SteamAPI_SetMiniDumpComment },
    { "SteamInternal_CreateInterface", sh_SteamInternal_CreateInterface },
    { "SteamInternal_ContextInit", sh_SteamInternal_ContextInit },
    { "CoInitialize", sh_CoInitialize }, { "CoInitializeEx", sh_CoInitializeEx },
    { "CoUninitialize", sh_CoUninitialize }, { "CoInitializeSecurity", sh_CoInitializeSecurity },
    { "CoSetProxyBlanket", sh_CoSetProxyBlanket }, { "CoCreateInstance", sh_CoCreateInstance },
    { "SysAllocString", sh_SysAllocString }, { "SysFreeString", sh_SysFreeString },
    { "VariantClear", sh_VariantClear },
    /* the exe imports OLEAUT32 by ordinal */
    { "OLEAUT32.dll#2", sh_SysAllocString }, { "OLEAUT32.dll#6", sh_SysFreeString },
    { "OLEAUT32.dll#9", sh_VariantClear },
    { "RegisterEventSourceA", sh_RegisterEventSourceA },
    { "DeregisterEventSource", sh_DeregisterEventSource }, { "ReportEventA", sh_ReportEventA },
    { "GetProcessMemoryInfo", sh_GetProcessMemoryInfo },
    { "SHGetFolderPathW", sh_SHGetFolderPathW }, { "SHCreateDirectoryExA", sh_SHCreateDirectoryExA },
    { "ShellExecuteA", sh_ShellExecuteA }, { "SHBrowseForFolderA", sh_SHBrowseForFolderA },
    { "SHGetPathFromIDListA", sh_SHGetPathFromIDListA },
    { 0, 0 }
};
