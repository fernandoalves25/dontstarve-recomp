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
#include <stdint.h>
#include <pthread.h>
#include "ds_hle.h"

#define DS_GAME_ROOT "Z:\\dontstarve"
#define DS_EXE_PATH  DS_GAME_ROOT "\\bin\\dontstarve_steam.exe"
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
    const char *path = DS_EXE_PATH;
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

/* ===================== TLS / FLS (real per-thread storage) =====================
 * Index allocation is process-global; the VALUES are per host-thread (__thread),
 * so each guest thread (= one host pthread) gets its own slots, as Win32 TLS
 * guarantees. The CRT's _tiddata is lifted guest code; it only needs these. */
#define NSLOT 1088
static int g_tls_next, g_fls_next;
static __thread uint32_t t_tls[NSLOT];
static __thread uint32_t t_fls[NSLOT];
static void sh_TlsAlloc(void)    { RET(g_tls_next < NSLOT ? (uint32_t)g_tls_next++ : 0xFFFFFFFFu, 0); }
static void sh_TlsFree(void)     { RET(1, 1); }
static void sh_TlsSetValue(void) { uint32_t i = A32(0); if (i < NSLOT) t_tls[i] = A32(1); RET(1, 2); }
static void sh_TlsGetValue(void) { uint32_t i = A32(0); RET(i < NSLOT ? t_tls[i] : 0, 1); }

/* ===================== critical sections =====================
 * The CRT initialises a CS struct in guest memory and then reads its fields, so
 * Initialize must leave a valid struct (no-op => garbage deref). Real mutual
 * exclusion is backed by a host recursive pthread_mutex, keyed by the guest CS
 * VA (so it ports to aarch64 and is correct once WorldSim/HTTP become threads).
 * RTL_CRITICAL_SECTION (x86): +0 DebugInfo, +4 LockCount, +8 RecursionCount,
 * +12 OwningThread, +16 LockSemaphore, +20 SpinCount. */
#define MAX_CS 4096
static struct { uint32_t va; pthread_mutex_t m; } g_cs[MAX_CS];
static int g_cs_n;
static pthread_mutex_t g_cs_tab_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t *cs_mutex(uint32_t va) {
    pthread_mutex_lock(&g_cs_tab_lock);
    for (int i = 0; i < g_cs_n; i++)
        if (g_cs[i].va == va) { pthread_mutex_t *m = &g_cs[i].m; pthread_mutex_unlock(&g_cs_tab_lock); return m; }
    pthread_mutex_t *m = NULL;
    if (g_cs_n < MAX_CS) {
        int i = g_cs_n++;
        g_cs[i].va = va;
        pthread_mutexattr_t a; pthread_mutexattr_init(&a);
        pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
        pthread_mutex_init(&g_cs[i].m, &a);
        m = &g_cs[i].m;
    }
    pthread_mutex_unlock(&g_cs_tab_lock);
    return m;
}
static void cs_init_struct(uint32_t va) {
    MEM32(va + 0) = 0;            /* DebugInfo = NULL */
    MEM32(va + 4) = 0xFFFFFFFFu;  /* LockCount = -1 (unlocked) */
    MEM32(va + 8) = 0;            /* RecursionCount */
    MEM32(va + 12) = 0;           /* OwningThread */
    MEM32(va + 16) = 0;           /* LockSemaphore */
    MEM32(va + 20) = 0;           /* SpinCount */
}
static void sh_InitializeCriticalSection(void)        { uint32_t v = A32(0); cs_init_struct(v); cs_mutex(v); RETV(1); }
static void sh_InitializeCriticalSectionAndSpinCount(void) { uint32_t v = A32(0); cs_init_struct(v); cs_mutex(v); RET(1, 2); }
static void sh_DeleteCriticalSection(void)            { RETV(1); }  /* keep the host mutex alive */
static void sh_EnterCriticalSection(void)             { pthread_mutex_t *m = cs_mutex(A32(0)); if (m) pthread_mutex_lock(m); RETV(1); }
static void sh_LeaveCriticalSection(void)             { pthread_mutex_t *m = cs_mutex(A32(0)); if (m) pthread_mutex_unlock(m); RETV(1); }
static void sh_TryEnterCriticalSection(void)          { pthread_mutex_t *m = cs_mutex(A32(0)); RET(m && pthread_mutex_trylock(m) == 0 ? 1 : 0, 1); }

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
static void sh_encode_pointer(void) { RET(A32(0), 0); }    /* identity; MSVCR90 _encode_pointer (cdecl) */
static void sh_decode_pointer(void) { RET(A32(0), 0); }
/* kernel32 Encode/DecodePointer are WINAPI: they pop their argument. Sharing the
 * cdecl shim left esp 4 low in the CRT's _decode_pointer, whose pop esi/pop ebp
 * then read the wrong slots -- the 0xDEAD0000 "this" in _LocaleUpdate. */
static void sh_EncodePointer(void)  { RET(A32(0), 1); }
static void sh_DecodePointer(void)  { RET(A32(0), 1); }
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

/* ===================== CRT-init batch: environment / codepage / locale /
 * std handles / system info (stdcall kernel32) ===================== */
static uint32_t g_env_block;
static uint32_t env_block(void) { /* an empty environment: "\0\0" */
    if (!g_env_block) { g_env_block = ds_arena_alloc(4, 2); MEM32(g_env_block) = 0; }
    return g_env_block;
}
static void sh_GetEnvironmentVariableA(void) { g_last_error = 203; RET(0, 3); } /* ERROR_ENVVAR_NOT_FOUND */
static void sh_GetEnvironmentVariableW(void) { g_last_error = 203; RET(0, 3); }
static void sh_GetEnvironmentStrings(void)   { RET(env_block(), 0); }
static void sh_GetEnvironmentStringsA(void)  { RET(env_block(), 0); }
static void sh_GetEnvironmentStringsW(void)  { RET(env_block(), 0); }
static void sh_FreeEnvironmentStringsA(void) { RET(1, 1); }
static void sh_FreeEnvironmentStringsW(void) { RET(1, 1); }
static void sh_SetEnvironmentVariableA(void) { RET(1, 2); }

/* std handles carry their nStdHandle id in the token's obj */
static void sh_GetStdHandle(void) { RET(ds_handle_new(HK_FILE, (void *)(intptr_t)(int32_t)A32(0)), 1); }
static void sh_GetFileType(void)  { RET(2, 1); }              /* FILE_TYPE_CHAR */
static void sh_SetHandleCount(void){ RET(A32(0), 1); }
static void sh_WriteFile(void) {   /* (h, buf, n, *written, ovl) -> host stdout/stderr */
    uint32_t h = A32(0), n = A32(2);
    int id = (int)(intptr_t)ds_handle_get(h, HK_FILE);
    FILE *out = (id == -12) ? stderr : stdout;
    size_t w = fwrite(ds_hostptr(A32(1)), 1, n, out);
    if (A32(3)) MEM32(A32(3)) = (uint32_t)w;
    RET(1, 5);
}
static void sh_CloseHandle(void)  { RET(1, 1); }

static void sh_GetACP(void)   { RET(1252, 0); }
static void sh_GetOEMCP(void) { RET(437, 0); }
static void sh_IsValidCodePage(void) { RET(1, 1); }
static void sh_GetCPInfo(void) {   /* (cp, lpCPInfo) minimal single-byte */
    uint32_t p = A32(1);
    MEM32(p + 0) = 1;              /* MaxCharSize */
    MEM8(p + 4) = '?';             /* DefaultChar[0] */
    memset(ds_hostptr(p + 5), 0, 0x12 - 5);
    RET(1, 2);
}
static void sh_GetStringTypeW(void) { RET(1, 4); }   /* best-effort: say "handled" */
static void sh_GetStringTypeA(void) { RET(1, 5); }
static void sh_MultiByteToWideChar(void) { /* (cp,flags,src,srclen,dst,dstlen) ASCII widen */
    const char *src = (const char *)APTR(2); int srclen = (int)A32(3);
    uint32_t dst = A32(4); int dstlen = (int)A32(5);
    int n = (srclen < 0) ? (int)strlen(src) + 1 : srclen;
    if (dstlen == 0) RET(n, 6);
    int m = n < dstlen ? n : dstlen;
    for (int i = 0; i < m; i++) MEM16(dst + (uint32_t)i * 2) = (uint8_t)src[i];
    RET(m, 6);
}
static void sh_WideCharToMultiByte(void) { /* (cp,flags,src,srclen,dst,dstlen,def,used) ASCII narrow */
    uint32_t src = A32(2); int srclen = (int)A32(3);
    uint32_t dst = A32(4); int dstlen = (int)A32(5);
    int n = 0; if (srclen < 0) { while (MEM16(src + (uint32_t)n * 2)) n++; n++; } else n = srclen;
    if (dstlen == 0) RET(n, 8);
    int m = n < dstlen ? n : dstlen; char *d = (char *)ds_hostptr(dst);
    for (int i = 0; i < m; i++) d[i] = (char)MEM16(src + (uint32_t)i * 2);
    RET(m, 8);
}
static void sh_LCMapStringW(void) { RET(0, 6); }
static void sh_LCMapStringA(void) { RET(0, 6); }
static void sh_GetLocaleInfoA(void) { RET(0, 4); }
static void sh_GetLocaleInfoW(void) { RET(0, 4); }
static void sh_GetThreadLocale(void) { RET(0x0409, 0); }     /* en-US */
static void sh_SetThreadLocale(void) { RET(1, 1); }
static void sh_GetUserDefaultLCID(void) { RET(0x0409, 0); }
static void sh_IsValidLocale(void) { RET(1, 2); }
static void sh_EnumSystemLocalesA(void) { RET(1, 2); }

static void sh_GetModuleHandleW(void)   { RET(A32(0) ? ds_handle_new(HK_MODULE, 0) : 0x00400000u, 1); }
static void sh_GetModuleFileNameW(void) { if (A32(2)) MEM16(A32(1)) = 0; RET(0, 3); }
static void sh_GetStartupInfoW(void)    { uint32_t p = A32(0); memset(ds_hostptr(p), 0, 68); MEM32(p) = 68; RETV(1); }
static void sh_GetVersionExW(void)      { uint32_t p = A32(0); MEM32(p+4)=6; MEM32(p+8)=1; MEM32(p+12)=7600; MEM32(p+16)=2; RET(1,1); }
static void sh_GetCurrentThread(void)   { RET(0xFFFFFFFEu, 0); }
static void sh_TerminateProcess(void)   { fprintf(stderr, "[ds] TerminateProcess\n"); exit((int)A32(1)); }
static void sh_UnhandledExceptionFilter(void){ RET(0, 1); }  /* EXCEPTION_CONTINUE_SEARCH */
static void sh_HeapSetInformation(void) { RET(1, 4); }
static void sh_IsProcessorFeaturePresent(void) { /* PF_* : claim nothing exotic */
    uint32_t f = A32(0);
    /* 0=FP_ERRATA,1=FP_EMULATED? report MMX/SSE/SSE2 present (10,6,? ) conservatively 0 */
    (void)f; RET(0, 1);
}
static void sh_GetSystemInfo(void) {   /* fill SYSTEM_INFO (minimal) */
    uint32_t p = A32(0);
    memset(ds_hostptr(p), 0, 36);
    MEM32(p + 4)  = 0x1000;        /* dwPageSize */
    MEM32(p + 20) = 4;             /* dwNumberOfProcessors */
    MEM32(p + 28) = 0x1000;        /* dwAllocationGranularity (approx) */
    RETV(1);
}
static void sh_FlsAlloc(void)    { RET(g_fls_next < NSLOT ? (uint32_t)g_fls_next++ : 0xFFFFFFFFu, 1); }
static void sh_FlsSetValue(void) { uint32_t i = A32(0); if (i < NSLOT) t_fls[i] = A32(1); RET(1, 2); }
static void sh_FlsGetValue(void) { uint32_t i = A32(0); RET(i < NSLOT ? t_fls[i] : 0, 1); }
static void sh_FlsFree(void)     { RET(1, 1); }
static void sh_Sleep(void)       { RETV(1); }
static void sh_RtlUnwind(void)   { RETV(4); }   /* SEH unwind: no-op for now */
static uint32_t g_cmdlineW;
static void sh_GetCommandLineW(void) {
    if (!g_cmdlineW) {
        const char *s = "dontstarve.exe";
        uint32_t n = (uint32_t)strlen(s);
        g_cmdlineW = ds_arena_alloc((n + 1) * 2, 2);
        for (uint32_t i = 0; i <= n; i++) MEM16(g_cmdlineW + i * 2) = (uint8_t)s[i];
    }
    RET(g_cmdlineW, 0);
}
static void sh_GetModuleHandleExW(void) { if (A32(2)) MEM32(A32(2)) = 0x00400000u; RET(1, 3); }
static void sh_GetProcessAffinityMask(void) { if (A32(1)) MEM32(A32(1)) = 0xF; if (A32(2)) MEM32(A32(2)) = 0xF; RET(1, 3); }

/* ===================== WinMain-era batch: sync objects, memory, paths.
 * The guest sees the Steam layout under a virtual drive: the exe in
 * Z:\dontstarve\bin, the data in Z:\dontstarve\data. File I/O maps onto the
 * host later; these only answer queries about the layout. */
static char g_cwd[260] = DS_GAME_ROOT "\\bin";

static uint32_t copy_out_a(uint32_t dst, uint32_t cap, const char *s) {   /* Win32 buffer rules */
    uint32_t n = (uint32_t)strlen(s);
    if (!dst || cap <= n) return n + 1;          /* too small: required size incl. NUL */
    memcpy(ds_hostptr(dst), s, n + 1);
    return n;
}
/* Mutexes/semaphores: one machine lock serialises guest threads, so the
 * objects only need to exist; waits succeed at once. */
static void sh_CreateMutexA(void)      { g_last_error = 0; RET(ds_handle_new(HK_MUTEX, 0), 3); }
static void sh_ReleaseMutex(void)      { RET(1, 1); }
static void sh_CreateSemaphoreA(void)  { g_last_error = 0; RET(ds_handle_new(HK_EVENT, 0), 4); }
static void sh_ReleaseSemaphore(void)  { if (A32(2)) MEM32(A32(2)) = 0; RET(1, 3); }
static void sh_WaitForSingleObject(void)    { RET(0, 2); }   /* WAIT_OBJECT_0 */
static void sh_WaitForMultipleObjects(void) { RET(0, 4); }
static void sh_SleepEx(void)           { RET(0, 2); }
static void sh_SetThreadPriority(void) { RET(1, 2); }
static void sh_OutputDebugStringA(void){ fprintf(stderr, "[guest] %s", ASTR(0)); RETV(1); }
static void sh_lstrlenA(void)          { RET(A32(0) ? (uint32_t)strlen(ASTR(0)) : 0, 1); }
static void sh_GlobalLock(void)        { RET(A32(0), 1); }   /* GMEM_FIXED: handle == pointer */
static void sh_GlobalUnlock(void)      { RET(1, 1); }
static void sh_FlushConsoleInputBuffer(void) { RET(1, 1); }
static void sh_LoadLibraryW(void)      { RET(ds_handle_new(HK_MODULE, 0), 1); }
static void sh_FormatMessageA(void)    { RET(0, 7); }
static void sh_GlobalMemoryStatus(void) {          /* MEMORYSTATUS (32 bytes) */
    uint32_t p = A32(0);
    memset(ds_hostptr(p), 0, 32);
    MEM32(p) = 32; MEM32(p + 8) = 1024u << 20; MEM32(p + 12) = 768u << 20;   /* phys total/avail */
    MEM32(p + 16) = 2048u << 20; MEM32(p + 20) = 1536u << 20;               /* page file */
    MEM32(p + 24) = 0x7FFE0000u; MEM32(p + 28) = 0x60000000u;               /* virtual */
    RETV(1);
}
static void sh_GlobalMemoryStatusEx(void) {        /* MEMORYSTATUSEX (64 bytes, 64-bit fields) */
    uint32_t p = A32(0);
    memset(ds_hostptr(p), 0, 64);
    MEM32(p) = 64;
    MEM32(p + 8)  = 1024u << 20;  /* ullTotalPhys */
    MEM32(p + 16) = 768u << 20;   /* ullAvailPhys */
    MEM32(p + 24) = 2048u << 20;  /* ullTotalPageFile */
    MEM32(p + 32) = 1536u << 20;  /* ullAvailPageFile */
    MEM32(p + 40) = 0x7FFE0000u;  /* ullTotalVirtual */
    MEM32(p + 48) = 0x60000000u;  /* ullAvailVirtual */
    RET(1, 1);
}
static void sh_GetCurrentDirectoryA(void) { RET(copy_out_a(A32(1), A32(0), g_cwd), 2); }
static void sh_SetCurrentDirectoryA(void) {
    const char *d = ASTR(0);
    size_t n = strlen(g_cwd);
    if (d[0] && d[1] == ':') snprintf(g_cwd, sizeof g_cwd, "%s", d);
    else snprintf(g_cwd + n, sizeof g_cwd - n, "\\%s", d);
    fprintf(stderr, "[ds] SetCurrentDirectoryA(%s) -> %s\n", d, g_cwd);
    RET(1, 1);
}
static void sh_GetFullPathNameA(void) {            /* (name, cap, buf, *filepart) */
    const char *nm = ASTR(0); char full[520];
    if (nm[0] && nm[1] == ':') snprintf(full, sizeof full, "%s", nm);
    else snprintf(full, sizeof full, "%s\\%s", g_cwd, nm);
    uint32_t r = copy_out_a(A32(2), A32(1), full);
    if (A32(3) && r < A32(1)) {
        char *sl = strrchr(full, '\\');
        MEM32(A32(3)) = A32(2) + (uint32_t)(sl ? sl - full + 1 : 0);
    }
    RET(r, 4);
}
static void sh_GetLongPathNameA(void) { RET(copy_out_a(A32(1), A32(2), ASTR(0)), 3); }
static void sh_ExpandEnvironmentStringsA(void) {   /* returns chars incl. NUL */
    uint32_t r = copy_out_a(A32(1), A32(2), ASTR(0));
    RET(r < A32(2) ? r + 1 : r, 3);
}
static void sh_RaiseException(void) {
    ds_fatal("guest RaiseException(code 0x%08X, flags %u, nargs %u) from 0x%08X",
             A32(0), A32(1), A32(2), g_cur_func);
}

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
    { "TryEnterCriticalSection", sh_TryEnterCriticalSection },
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
    { "EncodePointer", sh_EncodePointer },
    { "DecodePointer", sh_DecodePointer },
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
    /* CRT-init kernel32 batch */
    { "GetEnvironmentVariableA", sh_GetEnvironmentVariableA },
    { "GetEnvironmentVariableW", sh_GetEnvironmentVariableW },
    { "GetEnvironmentStrings", sh_GetEnvironmentStrings },
    { "GetEnvironmentStringsA", sh_GetEnvironmentStringsA },
    { "GetEnvironmentStringsW", sh_GetEnvironmentStringsW },
    { "FreeEnvironmentStringsA", sh_FreeEnvironmentStringsA },
    { "FreeEnvironmentStringsW", sh_FreeEnvironmentStringsW },
    { "SetEnvironmentVariableA", sh_SetEnvironmentVariableA },
    { "GetStdHandle", sh_GetStdHandle }, { "GetFileType", sh_GetFileType },
    { "SetHandleCount", sh_SetHandleCount }, { "WriteFile", sh_WriteFile },
    { "CloseHandle", sh_CloseHandle },
    { "GetACP", sh_GetACP }, { "GetOEMCP", sh_GetOEMCP },
    { "IsValidCodePage", sh_IsValidCodePage }, { "GetCPInfo", sh_GetCPInfo },
    { "GetStringTypeW", sh_GetStringTypeW }, { "GetStringTypeA", sh_GetStringTypeA },
    { "MultiByteToWideChar", sh_MultiByteToWideChar }, { "WideCharToMultiByte", sh_WideCharToMultiByte },
    { "LCMapStringW", sh_LCMapStringW }, { "LCMapStringA", sh_LCMapStringA },
    { "GetLocaleInfoA", sh_GetLocaleInfoA }, { "GetLocaleInfoW", sh_GetLocaleInfoW },
    { "GetThreadLocale", sh_GetThreadLocale }, { "SetThreadLocale", sh_SetThreadLocale },
    { "GetUserDefaultLCID", sh_GetUserDefaultLCID }, { "IsValidLocale", sh_IsValidLocale },
    { "EnumSystemLocalesA", sh_EnumSystemLocalesA },
    { "GetModuleHandleW", sh_GetModuleHandleW }, { "GetModuleFileNameW", sh_GetModuleFileNameW },
    { "GetStartupInfoW", sh_GetStartupInfoW }, { "GetVersionExW", sh_GetVersionExW },
    { "GetCurrentThread", sh_GetCurrentThread }, { "TerminateProcess", sh_TerminateProcess },
    { "UnhandledExceptionFilter", sh_UnhandledExceptionFilter },
    { "HeapSetInformation", sh_HeapSetInformation },
    { "IsProcessorFeaturePresent", sh_IsProcessorFeaturePresent },
    { "GetSystemInfo", sh_GetSystemInfo },
    { "FlsAlloc", sh_FlsAlloc }, { "FlsSetValue", sh_FlsSetValue },
    { "FlsGetValue", sh_FlsGetValue }, { "FlsFree", sh_FlsFree },
    { "Sleep", sh_Sleep }, { "RtlUnwind", sh_RtlUnwind },
    { "GetCommandLineW", sh_GetCommandLineW }, { "GetModuleHandleExW", sh_GetModuleHandleExW },
    { "GetProcessAffinityMask", sh_GetProcessAffinityMask },
    /* WinMain-era batch */
    { "CreateMutexA", sh_CreateMutexA }, { "ReleaseMutex", sh_ReleaseMutex },
    { "CreateSemaphoreA", sh_CreateSemaphoreA }, { "ReleaseSemaphore", sh_ReleaseSemaphore },
    { "WaitForSingleObject", sh_WaitForSingleObject }, { "WaitForMultipleObjects", sh_WaitForMultipleObjects },
    { "SleepEx", sh_SleepEx }, { "SetThreadPriority", sh_SetThreadPriority },
    { "OutputDebugStringA", sh_OutputDebugStringA }, { "lstrlenA", sh_lstrlenA },
    { "GlobalLock", sh_GlobalLock }, { "GlobalUnlock", sh_GlobalUnlock },
    { "FlushConsoleInputBuffer", sh_FlushConsoleInputBuffer }, { "LoadLibraryW", sh_LoadLibraryW },
    { "FormatMessageA", sh_FormatMessageA },
    { "GlobalMemoryStatus", sh_GlobalMemoryStatus }, { "GlobalMemoryStatusEx", sh_GlobalMemoryStatusEx },
    { "GetCurrentDirectoryA", sh_GetCurrentDirectoryA }, { "SetCurrentDirectoryA", sh_SetCurrentDirectoryA },
    { "GetFullPathNameA", sh_GetFullPathNameA }, { "GetLongPathNameA", sh_GetLongPathNameA },
    { "ExpandEnvironmentStringsA", sh_ExpandEnvironmentStringsA }, { "RaiseException", sh_RaiseException },
    { 0, 0 }
};

void ds_host_init(void) {
    ds_register(ds_kernel32);
    ds_register(ds_win32);
}
