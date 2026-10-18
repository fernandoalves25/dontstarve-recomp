/*
 * ds_host.c - arena-model host spine for recompiled Don't Starve. See ds_hle.h.
 *
 * Reserves a contiguous guest arena, loads the PE image at its VAs inside it,
 * binds the IAT to shim VAs, and runs the lifted entry. All guest memory
 * (image, stacks, TIB, heap, handles-as-tokens) lives as uint32_t VAs resolved
 * through ADDR() = g_mem_base + va. This is the model aarch64 will ship.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <pthread.h>
#include <sys/mman.h>
#include "ds_hle.h"
#include "pe_format.h"

/* ============================================================ register file */
uint32_t  g_eax, g_ecx, g_edx, g_esp, g_ebx, g_esi, g_edi, g_ebp;
double    g_st[8];
int       g_fp_top;
uint16_t  g_fpu_cw = 0x027F;
uint16_t  g_seg_cs, g_seg_ds, g_seg_es, g_seg_fs, g_seg_gs, g_seg_ss;
uint64_t  g_mm[8];
V128      g_xmm[8];
uint32_t  g_mxcsr = 0x1F80;
uint32_t  g_fs_base, g_gs_base;
ptrdiff_t g_mem_base;
uint32_t  g_cur_func;
uint32_t  g_icall_trace[ICALL_TRACE_SIZE], g_icall_from[ICALL_TRACE_SIZE];
uint32_t  g_icall_trace_idx, g_icall_count;

int ds_trace;

void ds_fatal(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "[ds] FATAL: "); vfprintf(stderr, fmt, ap); fprintf(stderr, "\n");
    va_end(ap); abort();
}

/* ============================================================ the arena
 * A 2 GB reservation; guest VA v -> host (g_mem_base + v). The image sits at
 * 0x00400000; the bump allocator hands out everything else from 0x01000000 up. */
#define ARENA_SIZE   0x80000000ull     /* 2 GB, demand-paged (MAP_NORESERVE) */
#define ARENA_HEAP0  0x01000000u       /* first free VA above the image region */
static uint32_t g_brk = ARENA_HEAP0;

static int arena_init(void) {
    void *p = mmap(NULL, ARENA_SIZE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) { perror("mmap arena"); return 0; }
    g_mem_base = (ptrdiff_t)(uintptr_t)p;
    fprintf(stderr, "[ds] arena %llu MB at host %p (g_mem_base=0x%llx)\n",
            (unsigned long long)(ARENA_SIZE >> 20), p, (unsigned long long)g_mem_base);
    return 1;
}

uint32_t ds_arena_alloc(uint32_t size, uint32_t align) {
    if (align < 4) align = 4;
    g_brk = (g_brk + align - 1) & ~(align - 1);
    uint32_t va = g_brk;
    uint64_t next = (uint64_t)g_brk + size;
    if (next > ARENA_SIZE) { ds_fatal("arena exhausted (want %u at 0x%08X)", size, va); }
    g_brk = (uint32_t)next;
    return va;
}
void ds_arena_reset_to(uint32_t va) { g_brk = va; }

/* ============================================================ handle table
 * HANDLEs/HMODULEs/etc are opaque tokens, not host pointers (never truncated). */
typedef struct { int kind; void *obj; } handle_slot;
#define MAX_HANDLES 4096
static handle_slot g_handles[MAX_HANDLES];
static int g_handle_n = 1;   /* token 0 is reserved (NULL/invalid) */

uint32_t ds_handle_new(int kind, void *obj) {
    if (g_handle_n >= MAX_HANDLES) ds_fatal("handle table full");
    g_handles[g_handle_n].kind = kind;
    g_handles[g_handle_n].obj = obj;
    return (uint32_t)g_handle_n++;
}
void *ds_handle_get(uint32_t t, int kind) {
    if (t == 0 || t >= (uint32_t)g_handle_n) return NULL;
    if (kind && g_handles[t].kind != kind) return NULL;
    return g_handles[t].obj;
}
int ds_handle_kind(uint32_t t) { return (t && t < (uint32_t)g_handle_n) ? g_handles[t].kind : 0; }

/* ============================================================ machine lock
 * One recursive mutex serialises the global register file; each thread gets a
 * guest stack and a simulated TIB carved from the arena. */
static pthread_mutex_t g_mach = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;

typedef struct {
    uint32_t eax, ecx, edx, esp, ebx, esi, edi, ebp, fs, cur;
    double st[8]; int fp_top; uint16_t fpu_cw;
    uint64_t mm[8];
} regs_t;

static void regs_save(regs_t *r) {
    r->eax=g_eax; r->ecx=g_ecx; r->edx=g_edx; r->esp=g_esp;
    r->ebx=g_ebx; r->esi=g_esi; r->edi=g_edi; r->ebp=g_ebp;
    r->fs=g_fs_base; r->cur=g_cur_func;
    memcpy(r->st, g_st, sizeof g_st); r->fp_top=g_fp_top; r->fpu_cw=g_fpu_cw;
    memcpy(r->mm, g_mm, sizeof g_mm);
}
static void regs_load(const regs_t *r) {
    g_eax=r->eax; g_ecx=r->ecx; g_edx=r->edx; g_esp=r->esp;
    g_ebx=r->ebx; g_esi=r->esi; g_edi=r->edi; g_ebp=r->ebp;
    g_fs_base=r->fs; g_cur_func=r->cur;
    memcpy(g_st, r->st, sizeof g_st); g_fp_top=r->fp_top; g_fpu_cw=r->fpu_cw;
    memcpy(g_mm, r->mm, sizeof g_mm);
}

#define GUEST_STACK (4u << 20)

static uint32_t make_tib(uint32_t stack_lo, uint32_t stack_hi) {
    uint32_t tib = ds_arena_alloc(0x1000, 0x1000);
    memset(ds_hostptr(tib), 0, 0x1000);
    MEM32(tib + 0x00) = 0xFFFFFFFFu;   /* ExceptionList: end of chain */
    MEM32(tib + 0x04) = stack_hi;      /* StackBase */
    MEM32(tib + 0x08) = stack_lo;      /* StackLimit */
    MEM32(tib + 0x18) = tib;           /* Self (fs:[0x18]) */
    return tib;
}

typedef struct { regs_t r; int depth, inited; } mstate;
static __thread mstate g_ms;

void mach_enter(void) {
    pthread_mutex_lock(&g_mach);
    if (!g_ms.inited) {
        uint32_t lo = ds_arena_alloc(GUEST_STACK, 0x1000);
        g_ms.r.esp    = lo + GUEST_STACK - 64;
        g_ms.r.fs     = make_tib(lo, lo + GUEST_STACK);
        g_ms.r.fpu_cw = 0x027F;
        g_ms.inited   = 1;
    }
    if (g_ms.depth++ == 0) regs_load(&g_ms.r);
}
void mach_leave(void) {
    if (--g_ms.depth == 0) regs_save(&g_ms.r);
    pthread_mutex_unlock(&g_mach);
}

/* ============================================================ dispatch */
recomp_func_t recomp_lookup(uint32_t va) {
    uint32_t lo = 0, hi = recomp_dispatch_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2, m = recomp_dispatch_table[mid].address;
        if (m == va) return recomp_dispatch_table[mid].func;
        if (m < va) lo = mid + 1; else hi = mid;
    }
    return NULL;
}
recomp_func_t recomp_lookup_manual(uint32_t va) { (void)va; return NULL; }

/* ============================================================ shim registry */
#define SHIM_BASE 0x7F000000u
#define SHIM_STEP 16u
#define MAX_SHIMS 4096
static struct { const char *name; recomp_func_t fn; uint32_t va; } g_shim[MAX_SHIMS];
static int g_shim_n;
static uint32_t g_last_import_va;

int ds_register(const ds_shim *shims) {
    int added = 0;
    for (; shims->name; shims++) {
        if (g_shim_n >= MAX_SHIMS) ds_fatal("too many shims");
        g_shim[g_shim_n].name = shims->name;
        g_shim[g_shim_n].fn   = shims->fn;
        g_shim[g_shim_n].va   = SHIM_BASE + SHIM_STEP * (uint32_t)g_shim_n;
        g_shim_n++; added++;
    }
    return added;
}
uint32_t ds_resolve(const char *name) {
    for (int i = 0; i < g_shim_n; i++) if (!strcmp(g_shim[i].name, name)) return g_shim[i].va;
    return 0;
}
const char *ds_hle_name(uint32_t va) {
    for (int i = 0; i < g_shim_n; i++) if (g_shim[i].va == va) return g_shim[i].name;
    return NULL;
}
static void ds_unimplemented_import(void) {
    const char *nm = ds_hle_name(g_last_import_va);
    ds_fatal("unimplemented import %s (0x%08X) from guest 0x%08X",
             nm ? nm : "?", g_last_import_va, g_cur_func);
}
uint32_t ds_resolve_or_stub(const char *name) {
    uint32_t va = ds_resolve(name);
    if (va) return va;
    if (g_shim_n >= MAX_SHIMS) ds_fatal("too many shims");
    g_shim[g_shim_n].name = name;   /* IAT name strings live in the arena for the run */
    g_shim[g_shim_n].fn   = ds_unimplemented_import;
    g_shim[g_shim_n].va   = SHIM_BASE + SHIM_STEP * (uint32_t)g_shim_n;
    return g_shim[g_shim_n++].va;
}
recomp_func_t recomp_lookup_import(uint32_t va) {
    if (va < SHIM_BASE || va >= SHIM_BASE + SHIM_STEP * (uint32_t)g_shim_n) return NULL;
    uint32_t i = (va - SHIM_BASE) / SHIM_STEP;
    if (i < (uint32_t)g_shim_n && g_shim[i].va == va) {
        g_last_import_va = va;
        if (ds_trace) fprintf(stderr, "[hle] %s\n", g_shim[i].name);
        return g_shim[i].fn;
    }
    return NULL;
}

/* ============================================================ miss handlers */
static uint64_t g_unresolved_count;
void recomp_unresolved(const char *kind, uint32_t va, uint32_t from) {
    if (g_unresolved_count < 60)
        fprintf(stderr, "[unresolved %s] VA 0x%08X from 0x%08X\n", kind, va, from);
    g_unresolved_count++;
}
void recomp_unimpl(uint32_t va, const char *what) {
    fprintf(stderr, "[RECOMP_UNIMPL] 0x%08X: %s\n", va, what ? what : "?");
    abort();
}

/* ============================================================ native->guest */
uint32_t hle_call_guest(uint32_t va, int nargs, const uint32_t *args) {
    recomp_func_t f = recomp_lookup(va);
    if (!f) f = recomp_lookup_import(va);
    if (!f) ds_fatal("no lifted function at 0x%08X", va);
    mach_enter();
    regs_t saved; regs_save(&saved);
    for (int i = nargs - 1; i >= 0; i--) PUSH32(g_esp, args[i]);
    PUSH32(g_esp, RECOMP_RETADDR);
    f();
    uint32_t eax = g_eax;
    regs_load(&saved);
    mach_leave();
    return eax;
}

/* ============================================================ PE load + bind */
typedef struct { uint32_t image_base, entry, import_rva, size_of_image; } ds_image;

static uint8_t *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = (uint8_t *)malloc((size_t)n);
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f); if (b) *len = (size_t)n; return b;
}

/* Load headers + sections into the arena at their VAs; fill *img. 0 on success. */
static int ds_pe_load(const char *path, ds_image *img) {
    size_t flen; uint8_t *file = slurp(path, &flen);
    if (!file) { fprintf(stderr, "[ds] cannot read %s\n", path); return 1; }
    pe_dos_header *dos = (pe_dos_header *)file;
    if (flen < sizeof *dos || dos->e_magic != 0x5A4D) { free(file); fprintf(stderr, "[ds] not MZ\n"); return 1; }
    pe_nt_headers32 *nt = (pe_nt_headers32 *)(file + dos->e_lfanew);
    if (nt->Signature != 0x00004550 || nt->OptionalHeader.Magic != 0x10B) {
        free(file); fprintf(stderr, "[ds] not PE32\n"); return 1;
    }
    pe_opt_header32 *opt = &nt->OptionalHeader;
    img->image_base = opt->ImageBase;
    img->entry = opt->ImageBase + opt->AddressOfEntryPoint;
    img->import_rva = opt->DataDirectory[PE_DIR_IMPORT].VirtualAddress;
    img->size_of_image = opt->SizeOfImage;

    /* headers */
    memcpy(ds_hostptr(opt->ImageBase), file, opt->SizeOfHeaders);
    /* sections */
    pe_section_header *sec = (pe_section_header *)((uint8_t *)&nt->OptionalHeader + nt->FileHeader.SizeOfOptionalHeader);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        uint32_t copy = sec[i].SizeOfRawData < sec[i].VirtualSize ? sec[i].SizeOfRawData : sec[i].VirtualSize;
        if (copy && (size_t)sec[i].PointerToRawData + copy <= flen)
            memcpy(ds_hostptr(opt->ImageBase + sec[i].VirtualAddress), file + sec[i].PointerToRawData, copy);
    }
    free(file);
    fprintf(stderr, "[ds] image mapped: base 0x%08X, entry 0x%08X, SizeOfImage 0x%X\n",
            img->image_base, img->entry, img->size_of_image);
    return 0;
}

/* Walk the import directory (in the arena) and write each IAT slot's shim VA.
 * Returns the number left unresolved. */
static int ds_pe_bind(const ds_image *img, uint32_t (*resolve)(const char *)) {
    if (!img->import_rva) return 0;
    uint32_t base = img->image_base;
    int bound = 0, unresolved = 0;
    pe_import_descriptor *d = (pe_import_descriptor *)ds_hostptr(base + img->import_rva);
    for (; d->Name; d++) {
        const char *dll = (const char *)ds_hostptr(base + d->Name);
        uint32_t ilt_rva = d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk;
        uint32_t *ilt = (uint32_t *)ds_hostptr(base + ilt_rva);
        uint32_t iat_va = base + d->FirstThunk;
        for (; *ilt; ilt++, iat_va += 4) {
            uint32_t va = 0;
            if (*ilt & PE_ORDINAL_FLAG) {
                char nm[64];
                snprintf(nm, sizeof nm, "%s#%u", dll, *ilt & 0xFFFFu);
                va = resolve(nm);
            } else {
                const char *name = (const char *)ds_hostptr(base + (*ilt & 0x7FFFFFFFu) + 2);
                va = resolve(name);
                if (!va) fprintf(stderr, "[ds] unresolved import %s!%s\n", dll, name);
            }
            if (va) bound++; else unresolved++;
            MEM32(iat_va) = va;
        }
    }
    fprintf(stderr, "[ds] bound %d imports, %d unresolved\n", bound, unresolved);
    return unresolved;
}

/* ============================================================ main */
int main(int argc, char **argv) {
    const char *exe = NULL; int run_entry = 0, permissive = 1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--run-entry")) run_entry = 1;
        else if (!strcmp(argv[i], "--strict")) permissive = 0;
        else if (!strcmp(argv[i], "--trace")) ds_trace = 1;
        else if (argv[i][0] != '-') exe = argv[i];
    }
    if (!exe) exe = "work/dontstarve_steam.exe";
    if (getenv("DS_HLE_TRACE")) ds_trace = 1;

    fprintf(stderr, "=== Don't Starve recompiled - arena host (64-bit) ===\n");
    if (!arena_init()) return 1;

    ds_image img;
    if (ds_pe_load(exe, &img) != 0) return 1;

    extern void ds_host_init(void);   /* registers shim modules (ds_shims.c) */
    ds_host_init();
    fprintf(stderr, "[ds] registered %d shims\n", g_shim_n);

    int unresolved = ds_pe_bind(&img, permissive ? ds_resolve_or_stub : ds_resolve);
    fprintf(stderr, "[ds] dispatch: %u fns; entry 0x%08X %s\n", recomp_dispatch_count,
            img.entry, recomp_lookup(img.entry) ? "in dispatch" : "MISSING");

    if (!run_entry) {
        fprintf(stderr, "[ds] boot OK (%d unresolved). Pass --run-entry to start the guest.\n", unresolved);
        return 0;
    }
    fprintf(stderr, "[ds] running guest entry...\n");
    uint32_t args[1];
    uint32_t rc = hle_call_guest(img.entry, 0, args);
    fprintf(stderr, "[ds] guest entry returned 0x%08X\n", rc);
    return (int)rc;
}
