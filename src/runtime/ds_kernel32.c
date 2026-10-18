/*
 * ds_kernel32.c - first kernel32 (+ a little MSVCR90 CRT) shim batch for the
 * Don't Starve CRT startup. Arena-aware (ds_hle.h): args via A32/APTR, memory
 * via ADDR/MEM32, returns are guest VAs or handle tokens. Grown as --run-entry
 * names the next missing import.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "ds_hle.h"

enum { HK_HEAP = 1, HK_MODULE, HK_FILE, HK_MUTEX, HK_EVENT, HK_THREAD };

/* ---- a guest string helper: copy a C string into the arena, return its VA ---- */
static uint32_t ds_guest_strdup(const char *s) {
    uint32_t n = (uint32_t)strlen(s) + 1;
    uint32_t va = ds_arena_alloc(n, 1);
    memcpy(ds_hostptr(va), s, n);
    return va;
}

/* ===================== process / module ===================== */
static uint32_t g_cmdline_va;
static void sh_GetCommandLineA(void) {
    if (!g_cmdline_va) g_cmdline_va = ds_guest_strdup("dontstarve.exe");
    RET(g_cmdline_va, 0);
}
static void sh_GetModuleHandleA(void) {
    uint32_t name = A32(0);
    if (name == 0) RET(0x00400000u, 1);          /* the exe: its real ImageBase (a valid guest VA) */
    RET(ds_handle_new(HK_MODULE, 0), 1);          /* a named module: opaque token */
}
static void sh_GetModuleFileNameA(void) {
    /* (hModule, lpFilename, nSize) */
    char *buf = (char *)APTR(1); uint32_t size = A32(2);
    const char *path = "Z:\\dontstarve\\dontstarve.exe";
    uint32_t n = (uint32_t)strlen(path);
    if (n + 1 > size) n = size ? size - 1 : 0;
    memcpy(buf, path, n); if (size) buf[n] = 0;
    RET(n, 3);
}
static void sh_GetProcAddress(void) {
    /* (hModule, lpProcName) -> a shim VA for the name (stub if unimplemented) */
    uint32_t name = A32(1);
    if (name <= 0xFFFF) RET(0, 2);                /* by ordinal: unsupported for now */
    RET(ds_resolve_or_stub(ASTR(1)), 2);
}
static void sh_LoadLibraryA(void)  { RET(ds_handle_new(HK_MODULE, 0), 1); }
static void sh_FreeLibrary(void)   { RET(1, 1); }
static void sh_GetVersion(void)    { RET(0x1DB00106u, 0); }   /* Win7 6.1 build 7600 */
static void sh_GetVersionExA(void) {
    uint32_t p = A32(0);                          /* OSVERSIONINFOA* (guest VA) */
    /* dwOSVersionInfoSize already set by caller; fill the rest */
    MEM32(p + 4)  = 6;       /* dwMajorVersion */
    MEM32(p + 8)  = 1;       /* dwMinorVersion */
    MEM32(p + 12) = 7600;    /* dwBuildNumber  */
    MEM32(p + 16) = 2;       /* dwPlatformId = VER_PLATFORM_WIN32_NT */
    *(char *)ds_hostptr(p + 20) = 0;   /* szCSDVersion[0] = '\0' */
    RET(1, 1);
}
static void sh_GetStartupInfoA(void) {
    uint32_t p = A32(0);                           /* STARTUPINFOA* */
    memset(ds_hostptr(p), 0, 68);                  /* sizeof(STARTUPINFOA) == 68 */
    MEM32(p + 0) = 68;                             /* cb */
    RETV(1);
}
static void sh_GetCurrentProcess(void)   { RET(0xFFFFFFFFu, 0); }   /* pseudo-handle */
static void sh_GetCurrentProcessId(void) { RET(0x1000, 0); }
static void sh_GetCurrentThreadId(void)  { RET(0x1001, 0); }
static void sh_ExitProcess(void) { fprintf(stderr, "[ds] guest ExitProcess(%u)\n", A32(0)); exit((int)A32(0)); }

/* ===================== heap ===================== */
static void sh_GetProcessHeap(void) { RET(ds_handle_new(HK_HEAP, 0), 0); }
static void sh_HeapCreate(void)     { RET(ds_handle_new(HK_HEAP, 0), 3); }
static void sh_HeapDestroy(void)    { RET(1, 1); }
static void sh_HeapAlloc(void) {
    /* (hHeap, dwFlags, dwBytes) */
    uint32_t flags = A32(1), size = A32(2);
    uint32_t va = ds_arena_alloc(size ? size : 1, 8);
    if (flags & 0x8) memset(ds_hostptr(va), 0, size);  /* HEAP_ZERO_MEMORY */
    RET(va, 3);
}
static void sh_HeapFree(void)   { RET(1, 3); }
static void sh_HeapSize(void)   { RET(0, 3); }
static void sh_HeapReAlloc(void) {
    /* (hHeap, flags, mem, size) - bump allocator: new block + copy (old size
     * unknown; copy `size` which is >= old for a grow, the common case). */
    uint32_t oldva = A32(2), size = A32(3);
    uint32_t va = ds_arena_alloc(size ? size : 1, 8);
    if (oldva) memcpy(ds_hostptr(va), ds_hostptr(oldva), size);
    RET(va, 4);
}

/* ===================== virtual memory ===================== */
static void sh_VirtualAlloc(void) {
    /* (lpAddress, dwSize, flAllocationType, flProtect) */
    uint32_t addr = A32(0), size = A32(1);
    uint32_t va = addr ? addr : ds_arena_alloc(size, 0x1000);
    memset(ds_hostptr(va), 0, size);
    RET(va, 4);
}
static void sh_VirtualFree(void)      { RET(1, 3); }
static void sh_VirtualProtect(void)   { RET(1, 4); }

/* ===================== TLS (process-global for now) ===================== */
#define NTLS 1088
static uint32_t g_tls[NTLS]; static int g_tls_n;
static void sh_TlsAlloc(void)    { RET(g_tls_n < NTLS ? (uint32_t)g_tls_n++ : 0xFFFFFFFFu, 0); }
static void sh_TlsFree(void)     { RET(1, 1); }
static void sh_TlsSetValue(void) { uint32_t i = A32(0); if (i < NTLS) g_tls[i] = A32(1); RET(1, 2); }
static void sh_TlsGetValue(void) { uint32_t i = A32(0); RET(i < NTLS ? g_tls[i] : 0, 1); }

/* ===================== critical sections (no-op, single machine lock) ===================== */
static void sh_InitializeCriticalSection(void)        { RETV(1); }
static void sh_InitializeCriticalSectionAndSpinCount(void) { RET(1, 2); }
static void sh_DeleteCriticalSection(void)            { RETV(1); }
static void sh_EnterCriticalSection(void)             { RETV(1); }
static void sh_LeaveCriticalSection(void)             { RETV(1); }

/* ===================== time ===================== */
static void sh_GetTickCount(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    RET((uint32_t)(t.tv_sec * 1000ull + t.tv_nsec / 1000000ull), 0);
}
static void sh_GetSystemTimeAsFileTime(void) {
    uint32_t p = A32(0);
    struct timespec t; clock_gettime(CLOCK_REALTIME, &t);
    uint64_t ft = (uint64_t)(t.tv_sec + 11644473600ull) * 10000000ull + t.tv_nsec / 100;
    MEM32(p) = (uint32_t)ft; MEM32(p + 4) = (uint32_t)(ft >> 32);
    RETV(1);
}
static void sh_QueryPerformanceCounter(void) {
    uint32_t p = A32(0);
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    uint64_t c = (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec;
    MEM32(p) = (uint32_t)c; MEM32(p + 4) = (uint32_t)(c >> 32);
    RET(1, 1);
}
static void sh_QueryPerformanceFrequency(void) {
    uint32_t p = A32(0);
    MEM32(p) = 1000000000u; MEM32(p + 4) = 0;   /* ns ticks */
    RET(1, 1);
}

/* ===================== error / misc ===================== */
static uint32_t g_last_error;
static void sh_GetLastError(void)              { RET(g_last_error, 0); }
static void sh_SetLastError(void)              { g_last_error = A32(0); RETV(1); }
static void sh_SetUnhandledExceptionFilter(void) { RET(0, 1); }
static void sh_IsDebuggerPresent(void)         { RET(0, 0); }
static void sh_DisableThreadLibraryCalls(void) { RET(1, 1); }
static void sh_InterlockedIncrement(void) { uint32_t p = A32(0); uint32_t v = MEM32(p) + 1; MEM32(p) = v; RET(v, 1); }
static void sh_InterlockedDecrement(void) { uint32_t p = A32(0); uint32_t v = MEM32(p) - 1; MEM32(p) = v; RET(v, 1); }
static void sh_InterlockedExchange(void)  { uint32_t p = A32(0), old = MEM32(p); MEM32(p) = A32(1); RET(old, 2); }
static void sh_InterlockedExchangeAdd(void){ uint32_t p = A32(0), old = MEM32(p); MEM32(p) = old + A32(1); RET(old, 2); }
static void sh_InterlockedCompareExchange(void) { /* (dest, exchange, comparand) */
    uint32_t p = A32(0), old = MEM32(p);
    if (old == A32(2)) MEM32(p) = A32(1);
    RET(old, 3);
}

/* ===================== MSVCR90 CRT startup (all __cdecl: argc=0 on RET,
 * the lifted caller cleans its own args) ===================== */
static uint32_t g_fmode_va, g_commode_va;
static uint32_t guest_int(uint32_t *slot, uint32_t val) {
    if (!*slot) { *slot = ds_arena_alloc(4, 4); MEM32(*slot) = val; }
    return *slot;
}
static void sh_set_app_type(void)   { RET(0, 0); }         /* __set_app_type(int) */
static void sh_p__fmode(void)       { RET(guest_int(&g_fmode_va, 0), 0); }    /* __p__fmode() */
static void sh_p__commode(void)     { RET(guest_int(&g_commode_va, 0), 0); }  /* __p__commode() */
static void sh_encode_pointer(void) { RET(A32(0), 0); }    /* identity */
static void sh_decode_pointer(void) { RET(A32(0), 0); }
static void sh_controlfp(void)      { RET(0x0009001Fu, 0); }          /* _controlfp(new,mask) */
static void sh_controlfp_s(void)    { if (A32(0)) MEM32(A32(0)) = 0x0008001Fu; RET(0, 0); } /* (cur,new,mask) */
static void sh_setusermatherr(void) { RET(0, 0); }
static void sh_configthreadlocale(void){ RET(0, 0); }
static void sh_initterm(void) {        /* _initterm(PVFV* start, PVFV* end): run void ctors */
    uint32_t p = A32(0), end = A32(1);
    for (; p < end; p += 4) { uint32_t fn = MEM32(p); if (fn) hle_call_guest(fn, 0, 0); }
    RET(0, 0);
}
static void sh_initterm_e(void) {      /* _initterm_e: run int ctors, stop on nonzero */
    uint32_t p = A32(0), end = A32(1), rc = 0;
    for (; p < end && rc == 0; p += 4) { uint32_t fn = MEM32(p); if (fn) rc = hle_call_guest(fn, 0, 0); }
    RET(rc, 0);
}
static uint32_t g_argv_va;
static void sh_getmainargs(void) {     /* __getmainargs(&argc,&argv,&env,wild,&startinfo) */
    if (!g_cmdline_va) g_cmdline_va = ds_guest_strdup("dontstarve.exe");
    if (!g_argv_va) { g_argv_va = ds_arena_alloc(8, 4); MEM32(g_argv_va) = g_cmdline_va; MEM32(g_argv_va + 4) = 0; }
    if (A32(0)) MEM32(A32(0)) = 1;             /* *argc = 1 */
    if (A32(1)) MEM32(A32(1)) = g_argv_va;     /* *argv */
    if (A32(2)) MEM32(A32(2)) = 0;             /* *env = NULL */
    RET(0, 0);
}
static void sh_amsg_exit(void)  { fprintf(stderr, "[ds] CRT _amsg_exit(%u)\n", A32(0)); exit(255); }
static void sh_cexit(void)      { RET(0, 0); }
static void sh_exit(void)       { fprintf(stderr, "[ds] guest exit(%u)\n", A32(0)); exit((int)A32(0)); }
static void sh_XcptFilter(void) { RET(0, 0); }             /* EXCEPTION_CONTINUE_SEARCH */
static void sh_lock(void)       { RET(0, 0); }
static void sh_unlock(void)     { RET(0, 0); }
static void sh_onexit(void)     { RET(A32(0), 0); }
static void sh_dllonexit(void)  { RET(A32(0), 0); }
/* cdecl libc passthroughs the CRT may import directly */
static void sh_malloc(void)     { uint32_t n = A32(0); RET(ds_arena_alloc(n ? n : 1, 8), 0); }
static void sh_free(void)       { RET(0, 0); }
static void sh_calloc(void)     { uint32_t n = A32(0) * A32(1); uint32_t va = ds_arena_alloc(n ? n : 1, 8); memset(ds_hostptr(va), 0, n); RET(va, 0); }
/* cdecl mem/str (operate on arena memory; pointer results come back as guest VAs) */
static void sh_memset(void)  { uint32_t d = A32(0); memset(ds_hostptr(d), (int)A32(1), A32(2)); RET(d, 0); }
static void sh_memcpy(void)  { uint32_t d = A32(0); memcpy(ds_hostptr(d), ds_hostptr(A32(1)), A32(2)); RET(d, 0); }
static void sh_memmove(void) { uint32_t d = A32(0); memmove(ds_hostptr(d), ds_hostptr(A32(1)), A32(2)); RET(d, 0); }
static void sh_memcmp(void)  { RET((uint32_t)memcmp(ds_hostptr(A32(0)), ds_hostptr(A32(1)), A32(2)), 0); }
static void sh_strlen(void)  { RET((uint32_t)strlen(ASTR(0)), 0); }
static void sh_strcmp(void)  { RET((uint32_t)strcmp(ASTR(0), ASTR(1)), 0); }
static void sh_strncmp(void) { RET((uint32_t)strncmp(ASTR(0), ASTR(1), A32(2)), 0); }
static void sh_strcpy(void)  { uint32_t d = A32(0); strcpy((char *)ds_hostptr(d), ASTR(1)); RET(d, 0); }
static void sh_strncpy(void) { uint32_t d = A32(0); strncpy((char *)ds_hostptr(d), ASTR(1), A32(2)); RET(d, 0); }
static void sh_strcat(void)  { uint32_t d = A32(0); strcat((char *)ds_hostptr(d), ASTR(1)); RET(d, 0); }
static void sh_strchr(void)  { char *s = ASTR(0); char *r = strchr(s, (int)A32(1)); RET(r ? A32(0) + (uint32_t)(r - s) : 0, 0); }
static void sh_strrchr(void) { char *s = ASTR(0); char *r = strrchr(s, (int)A32(1)); RET(r ? A32(0) + (uint32_t)(r - s) : 0, 0); }
static void sh_strstr(void)  { char *s = ASTR(0); char *r = strstr(s, ASTR(1)); RET(r ? A32(0) + (uint32_t)(r - s) : 0, 0); }
static void sh_getenv(void)  { RET(0, 0); }   /* no environment: NULL */
static void sh_putenv(void)  { RET(0, 0); }

const ds_shim ds_kernel32[] = {
    { "GetCommandLineA", sh_GetCommandLineA },
    { "GetModuleHandleA", sh_GetModuleHandleA },
    { "GetModuleFileNameA", sh_GetModuleFileNameA },
    { "GetProcAddress", sh_GetProcAddress },
    { "LoadLibraryA", sh_LoadLibraryA },
    { "FreeLibrary", sh_FreeLibrary },
    { "GetVersion", sh_GetVersion },
    { "GetVersionExA", sh_GetVersionExA },
    { "GetStartupInfoA", sh_GetStartupInfoA },
    { "GetCurrentProcess", sh_GetCurrentProcess },
    { "GetCurrentProcessId", sh_GetCurrentProcessId },
    { "GetCurrentThreadId", sh_GetCurrentThreadId },
    { "ExitProcess", sh_ExitProcess },
    { "GetProcessHeap", sh_GetProcessHeap },
    { "HeapCreate", sh_HeapCreate },
    { "HeapDestroy", sh_HeapDestroy },
    { "HeapAlloc", sh_HeapAlloc },
    { "HeapFree", sh_HeapFree },
    { "HeapSize", sh_HeapSize },
    { "HeapReAlloc", sh_HeapReAlloc },
    { "VirtualAlloc", sh_VirtualAlloc },
    { "VirtualFree", sh_VirtualFree },
    { "VirtualProtect", sh_VirtualProtect },
    { "TlsAlloc", sh_TlsAlloc },
    { "TlsFree", sh_TlsFree },
    { "TlsSetValue", sh_TlsSetValue },
    { "TlsGetValue", sh_TlsGetValue },
    { "InitializeCriticalSection", sh_InitializeCriticalSection },
    { "InitializeCriticalSectionAndSpinCount", sh_InitializeCriticalSectionAndSpinCount },
    { "DeleteCriticalSection", sh_DeleteCriticalSection },
    { "EnterCriticalSection", sh_EnterCriticalSection },
    { "LeaveCriticalSection", sh_LeaveCriticalSection },
    { "GetTickCount", sh_GetTickCount },
    { "GetSystemTimeAsFileTime", sh_GetSystemTimeAsFileTime },
    { "QueryPerformanceCounter", sh_QueryPerformanceCounter },
    { "QueryPerformanceFrequency", sh_QueryPerformanceFrequency },
    { "GetLastError", sh_GetLastError },
    { "SetLastError", sh_SetLastError },
    { "SetUnhandledExceptionFilter", sh_SetUnhandledExceptionFilter },
    { "IsDebuggerPresent", sh_IsDebuggerPresent },
    { "DisableThreadLibraryCalls", sh_DisableThreadLibraryCalls },
    { "InterlockedIncrement", sh_InterlockedIncrement },
    { "InterlockedDecrement", sh_InterlockedDecrement },
    { "InterlockedExchange", sh_InterlockedExchange },
    { "InterlockedExchangeAdd", sh_InterlockedExchangeAdd },
    { "InterlockedCompareExchange", sh_InterlockedCompareExchange },
    { "EncodePointer", sh_encode_pointer },
    { "DecodePointer", sh_decode_pointer },
    /* MSVCR90 CRT startup (__cdecl) */
    { "__set_app_type", sh_set_app_type },
    { "__p__fmode", sh_p__fmode },
    { "__p__commode", sh_p__commode },
    { "_encode_pointer", sh_encode_pointer },
    { "_decode_pointer", sh_decode_pointer },
    { "_controlfp", sh_controlfp },
    { "_controlfp_s", sh_controlfp_s },
    { "__setusermatherr", sh_setusermatherr },
    { "_configthreadlocale", sh_configthreadlocale },
    { "_initterm", sh_initterm },
    { "_initterm_e", sh_initterm_e },
    { "__getmainargs", sh_getmainargs },
    { "_amsg_exit", sh_amsg_exit },
    { "_cexit", sh_cexit },
    { "exit", sh_exit },
    { "_exit", sh_exit },
    { "_XcptFilter", sh_XcptFilter },
    { "_lock", sh_lock },
    { "_unlock", sh_unlock },
    { "_onexit", sh_onexit },
    { "__dllonexit", sh_dllonexit },
    { "malloc", sh_malloc },
    { "free", sh_free },
    { "calloc", sh_calloc },
    { "memset", sh_memset }, { "memcpy", sh_memcpy }, { "memmove", sh_memmove }, { "memcmp", sh_memcmp },
    { "strlen", sh_strlen }, { "strcmp", sh_strcmp }, { "strncmp", sh_strncmp },
    { "strcpy", sh_strcpy }, { "strncpy", sh_strncpy }, { "strcat", sh_strcat },
    { "strchr", sh_strchr }, { "strrchr", sh_strrchr }, { "strstr", sh_strstr },
    { "getenv", sh_getenv }, { "_putenv", sh_putenv },
    { 0, 0 }
};

void ds_host_init(void) {
    ds_register(ds_kernel32);
}
