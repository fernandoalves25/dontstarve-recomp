/*
 * host_linux.c - minimal Linux/ARM host for the recompiled Don't Starve guest.
 *
 * This is the M3b/M4 foundation: it defines the register/flag globals the
 * lifted code references, the dispatch lookups, the unimplemented/unresolved
 * handlers, and sets up guest memory (a flat region reached through g_mem_base)
 * with the PE's data sections loaded at their original VAs.
 *
 * It deliberately does NOT yet wire the Win32 import layer (win32hle), the CRT,
 * the TEB or graphics/audio. By default it initialises the guest image and
 * reports status (so it links and runs as a skeleton); pass --run-entry to
 * actually jump into the guest entry point (will fault until imports/TEB are
 * wired - that is the next milestone).
 *
 * Builds for the host (x86-64) now for fast iteration; the same file
 * cross-compiles for aarch64 later.
 */
#define RECOMP_STRICT_ICALL
#define RECOMP_LOCAL_REGS
#define RECOMP_X87_WINDOW
#define RECOMP_MEM_QUAL
#include "recomp_types.h"   /* pulls in recomp_simd.h (V128) */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/mman.h>

/* ============================================================
 * Register / flag / SIMD globals (the host side of recomp_types.h)
 * ============================================================ */
uint32_t g_eax, g_ecx, g_edx, g_esp, g_ebx, g_esi, g_edi, g_ebp;
uint16_t g_seg_cs, g_seg_ds, g_seg_es, g_seg_fs, g_seg_gs, g_seg_ss;
uint32_t g_fs_base, g_gs_base;
int      g_fp_top;
uint16_t g_fpu_cw = 0x037F;        /* x87 default control word */
uint64_t g_mm[8];
V128     g_xmm[8];
uint32_t g_mxcsr = 0x1F80;
ptrdiff_t g_mem_base;
uint32_t g_cur_func;

/* ICALL trace ring (diagnostics) */
uint32_t g_icall_trace[ICALL_TRACE_SIZE];
uint32_t g_icall_from[ICALL_TRACE_SIZE];
uint32_t g_icall_trace_idx;
uint32_t g_icall_count;

/* ============================================================
 * Dispatch (VA -> recompiled function)
 * ============================================================ */
recomp_func_t recomp_lookup(uint32_t va) {
    int lo = 0, hi = (int)recomp_dispatch_count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        uint32_t m = recomp_dispatch_table[mid].address;
        if (m == va) return recomp_dispatch_table[mid].func;
        if (m < va) lo = mid + 1; else hi = mid - 1;
    }
    return NULL;
}

recomp_func_t recomp_lookup_manual(uint32_t va) { (void)va; return NULL; }

/* Import bridge: IAT-slot VA -> host implementation. Empty until win32hle and
 * the DS-specific bridges (GLES2/EGL, FMOD, input, Steam/Twitch stubs) land. */
recomp_func_t recomp_lookup_import(uint32_t va) { (void)va; return NULL; }

/* ============================================================
 * Miss / unimplemented handlers
 * ============================================================ */
static uint64_t g_unresolved_count;

void recomp_unresolved(const char *kind, uint32_t va, uint32_t from) {
    if (g_unresolved_count < 40)
        fprintf(stderr, "[unresolved %s] VA 0x%08X from 0x%08X\n", kind, va, from);
    g_unresolved_count++;
}

void recomp_unimpl(uint32_t va, const char *what) {
    fprintf(stderr, "[RECOMP_UNIMPL] 0x%08X: %s\n", va, what ? what : "?");
    abort();
}

/* ============================================================
 * Guest memory + PE image layout (dontstarve_steam.exe, ImageBase 0x00400000)
 * Section RVAs/offsets from pe_analyze; VAs are ImageBase + RVA.
 * ============================================================ */
#define DS_IMAGE_BASE 0x00400000u

typedef struct { const char *name; uint32_t va; long file_off; uint32_t rsize; } ds_section_t;
static const ds_section_t DS_SECTIONS[] = {
    { ".text",  0x00401000u, 0x00000400, 0x0027CC00u },
    { ".rdata", 0x0067E000u, 0x0027D000, 0x00074800u },
    { ".data",  0x006F3000u, 0x002F1800, 0x0000D000u },  /* VSize 0x10C48: zero tail */
    { ".tls",   0x00704000u, 0x002FE800, 0x00000200u },
    { ".rsrc",  0x00705000u, 0x002FEA00, 0x00015200u },
};

#define GUEST_SPACE_SIZE  (256u * 1024u * 1024u)   /* 256 MB window (covers image + stack + TIB) */
#define GUEST_TIB_VA      0x00100000u
#define GUEST_STACK_TOP   0x00300000u              /* below .text (0x401000); 1 MB stack */
#define GUEST_STACK_SIZE  0x00100000u
#define DS_ENTRY_VA       0x00662ABDu

static int setup_guest_memory(const char *exe_path) {
    void *region = mmap(NULL, GUEST_SPACE_SIZE, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (region == MAP_FAILED) { perror("mmap guest space"); return 0; }
    g_mem_base = (ptrdiff_t)(uintptr_t)region;
    fprintf(stderr, "[host] guest space %u MB at host %p (g_mem_base=0x%llx)\n",
            GUEST_SPACE_SIZE >> 20, region, (unsigned long long)g_mem_base);

    FILE *f = fopen(exe_path, "rb");
    if (!f) { fprintf(stderr, "[host] cannot open %s\n", exe_path); return 0; }
    for (size_t i = 0; i < sizeof(DS_SECTIONS)/sizeof(DS_SECTIONS[0]); i++) {
        const ds_section_t *s = &DS_SECTIONS[i];
        if (fseek(f, s->file_off, SEEK_SET) != 0) { fprintf(stderr, "[host] seek %s failed\n", s->name); fclose(f); return 0; }
        size_t got = fread((void *)ADDR(s->va), 1, s->rsize, f);
        fprintf(stderr, "[host] %-6s VA 0x%08X <- %zu/%u bytes\n", s->name, s->va, got, s->rsize);
        if (got != s->rsize) { fprintf(stderr, "[host] short read on %s\n", s->name); }
    }
    fclose(f);

    /* Simulated TIB and stack */
    memset((void *)ADDR(GUEST_TIB_VA), 0, 0x1000);
    g_fs_base = GUEST_TIB_VA;
    g_esp = GUEST_STACK_TOP - 16;
    g_ebp = 0;
    (void)GUEST_STACK_SIZE;
    return 1;
}

int main(int argc, char **argv) {
    const char *exe = NULL;
    int run_entry = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--run-entry")) run_entry = 1;
        else if (argv[i][0] != '-') exe = argv[i];
    }
    if (!exe) exe = "work/dontstarve_steam.exe";

    fprintf(stderr, "=== Don't Starve recompiled - Linux host skeleton ===\n");
    fprintf(stderr, "[host] dispatch table: %u functions\n", recomp_dispatch_count);
    fprintf(stderr, "[host] entry VA 0x%08X -> %s\n", DS_ENTRY_VA,
            recomp_lookup(DS_ENTRY_VA) ? "found in dispatch" : "NOT FOUND");

    if (!setup_guest_memory(exe)) { fprintf(stderr, "[host] guest memory setup failed\n"); return 1; }
    fprintf(stderr, "[host] guest image initialised. ESP=0x%08X FS_BASE=0x%08X\n", g_esp, g_fs_base);

    if (run_entry) {
        recomp_func_t entry = recomp_lookup(DS_ENTRY_VA);
        if (!entry) { fprintf(stderr, "[host] entry not in dispatch; abort\n"); return 1; }
        fprintf(stderr, "[host] jumping into guest entry (imports/TEB NOT wired - expect a fault)\n");
        entry();
        fprintf(stderr, "[host] guest entry returned\n");
    } else {
        fprintf(stderr, "[host] skeleton OK (pass --run-entry to jump into the guest)\n");
    }
    return 0;
}
