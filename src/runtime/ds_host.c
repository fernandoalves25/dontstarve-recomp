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
#include <strings.h>
#include <pthread.h>
#include <signal.h>
#include <execinfo.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <time.h>
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
int g_ds_screen_w = 640, g_ds_screen_h = 480;

/* ============================================================ runtime coverage
 * Built with -DRECOMP_TRACE (COVERAGE=1), every lifted function calls
 * recomp_trace_enter on entry. We keep the set of guest VAs that actually ran
 * and dump it to build/executed_vas.txt (local, gitignored) on exit or crash.
 * "Lifted" is ~100%; "executed" is the honest measure of how far the port runs. */
#ifdef RECOMP_TRACE
uint32_t g_enter_trace[RECOMP_ENTER_SIZE];
uint32_t g_enter_idx;
#define COV_CAP (1u << 17)
static uint32_t g_cov[COV_CAP];
static uint32_t g_cov_n;
/* esp/ebp seen at each entry, parallel to g_enter_trace. DS_SENTINEL=1 aborts
 * at the first entry whose esp or ebp holds the RECOMP_RETADDR sentinel and
 * prints the recent entries -- pins a return-address leak to its function. */
static uint32_t g_enter_esp[RECOMP_ENTER_SIZE], g_enter_ebp[RECOMP_ENTER_SIZE];
static int g_sentinel_check = -1;
#define IS_SENTINEL(v) (((v) & 0xFFFF0000u) == (RECOMP_RETADDR & 0xFFFF0000u))
static void sentinel_report(uint32_t va) {
    fprintf(stderr, "\n[ds] *** sentinel in esp/ebp on entry to 0x%08X (esp=%08X ebp=%08X) ***\n",
            va, g_esp, g_ebp);
    uint32_t n = g_enter_idx < 48 ? g_enter_idx : 48;
    for (uint32_t i = g_enter_idx - n; i != g_enter_idx; i++) {
        uint32_t k = i & (RECOMP_ENTER_SIZE - 1);
        fprintf(stderr, "    #%-6u 0x%08X  esp=%08X ebp=%08X\n", i, g_enter_trace[k],
                g_enter_esp[k], g_enter_ebp[k]);
    }
    _exit(3);
}
/* DS_WATCH=va[,va...]: print the first stack arguments of each watched lifted
 * function on entry (as text when an argument points at a printable string).
 * Finding Lua's error functions and watching them is how a script error with
 * no message gets its message back. */
static uint32_t g_watch[16]; static int g_watch_n = -1;
static void watch_report(uint32_t va) {
    fprintf(stderr, "[watch] 0x%08X(", va);
    for (int i = 1; i <= 4; i++) {
        uint32_t a = MEM32(g_esp + 4u * (uint32_t)i);
        int str = a >= 0x10000u && a < 0x7F000000u;
        for (int k = 0; str && k < 4; k++) { uint8_t c = MEM8(a + (uint32_t)k); if (!c) { str = k > 0; break; } if (c < 0x20 || c > 0x7E) str = 0; }
        if (str) fprintf(stderr, "%s\"%.200s\"", i > 1 ? ", " : "", (const char *)ds_hostptr(a));
        else fprintf(stderr, "%s0x%08X", i > 1 ? ", " : "", a);
    }
    fprintf(stderr, ")\n");
}
void recomp_trace_enter(uint32_t va) {
    if (g_watch_n < 0) {
        g_watch_n = 0;
        for (const char *e = getenv("DS_WATCH"); e && *e && g_watch_n < 16; ) {
            char *end; g_watch[g_watch_n++] = (uint32_t)strtoul(e, &end, 16);
            e = (*end == ',') ? end + 1 : end;
            if (end == e && *e != ',') break;
        }
    }
    for (int i = 0; i < g_watch_n; i++) if (g_watch[i] == va) watch_report(va);
    uint32_t k = g_enter_idx & (RECOMP_ENTER_SIZE - 1);
    g_enter_esp[k] = g_esp; g_enter_ebp[k] = g_ebp;
    g_enter_trace[g_enter_idx++ & (RECOMP_ENTER_SIZE - 1)] = va;
    if (g_sentinel_check < 0) { const char *e = getenv("DS_SENTINEL"); g_sentinel_check = e && *e == '1'; }
    if (g_sentinel_check && (IS_SENTINEL(g_esp) || IS_SENTINEL(g_ebp))) sentinel_report(va);
    uint32_t h = (va * 2654435761u) & (COV_CAP - 1);
    while (g_cov[h]) { if (g_cov[h] == va) return; h = (h + 1) & (COV_CAP - 1); }
    if (g_cov_n < COV_CAP / 2) { g_cov[h] = va; g_cov_n++; }
}
static void cov_dump(void) {
    int fd = open("build/executed_vas.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    char b[16];
    for (uint32_t i = 0; i < COV_CAP; i++)
        if (g_cov[i]) { int n = snprintf(b, sizeof b, "%08X\n", g_cov[i]); if (write(fd, b, (size_t)n) < 0) break; }
    close(fd);
    fprintf(stderr, "[ds] coverage: %u distinct lifted functions executed -> build/executed_vas.txt\n", g_cov_n);
}
#else
static void cov_dump(void) {}
#endif

void ds_fatal(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "[ds] FATAL: "); vfprintf(stderr, fmt, ap); fprintf(stderr, "\n");
    va_end(ap); cov_dump(); abort();
}

/* ============================================================ the arena
 * A 2 GB reservation; guest VA v -> host (g_mem_base + v). The image sits at
 * 0x00400000; the bump allocator hands out everything else from 0x01000000 up. */
#define ARENA_SIZE   0x80000000ull     /* 2 GB, demand-paged (MAP_NORESERVE); covers the
                                        * exe (0x400000) and the CRT DLLs (~0x78480000). */
#define ARENA_HEAP0  0x01000000u       /* first free VA above the exe image region */
#define ARENA_HEAP_CEIL 0x70000000u    /* keep the bump heap below the CRT DLL images */
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
    if (next > ARENA_HEAP_CEIL) { ds_fatal("arena heap exhausted (want %u at 0x%08X)", size, va); }
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

/* Freed slots are reused (CloseHandle/FindClose): the game opens thousands of
 * files and sync objects while it loads. */
static uint32_t g_handle_free[MAX_HANDLES];
static int g_handle_free_n;
uint32_t ds_handle_new(int kind, void *obj) {
    uint32_t t;
    if (g_handle_free_n > 0) t = g_handle_free[--g_handle_free_n];
    else {
        if (g_handle_n >= MAX_HANDLES) {
            int cnt[64] = {0};
            for (int i = 1; i < g_handle_n; i++) cnt[g_handles[i].kind & 63]++;
            for (int k = 0; k < 64; k++) if (cnt[k]) fprintf(stderr, "[ds] handles of kind 0x%02X: %d\n", k, cnt[k]);
            ds_fatal("handle table full");
        }
        t = (uint32_t)g_handle_n++;
    }
    g_handles[t].kind = kind;
    g_handles[t].obj = obj;
    return t;
}
void ds_handle_free(uint32_t t) {
    if (t == 0 || t >= (uint32_t)g_handle_n || g_handles[t].kind == 0) return;
    g_handles[t].kind = 0; g_handles[t].obj = NULL;
    g_handle_free[g_handle_free_n++] = t;
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

/* Fair hand-over: how many threads wait for the machine, and how many times it
 * changed hands (a yielding thread retakes it only after someone else had it). */
static volatile int g_mach_waiters;
static volatile uint32_t g_mach_gen;
static void mach_lock(void) {
    if (pthread_mutex_trylock(&g_mach) != 0) {
        __atomic_add_fetch(&g_mach_waiters, 1, __ATOMIC_SEQ_CST);
        pthread_mutex_lock(&g_mach);
        __atomic_sub_fetch(&g_mach_waiters, 1, __ATOMIC_SEQ_CST);
    }
    __atomic_add_fetch(&g_mach_gen, 1, __ATOMIC_SEQ_CST);
}
void mach_enter(void) {
    if (g_ms.depth == 0) mach_lock(); else pthread_mutex_lock(&g_mach);
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
/* A shim about to block (Sleep, a wait, a contended lock) lets the other guest
 * threads run: save this thread's registers, drop every level of the machine
 * lock, and take it all back afterwards. The shim must read its arguments
 * BEFORE ds_block_begin (g_esp belongs to whoever runs meanwhile) and RET
 * only after ds_block_end. */
int ds_block_begin(void) {
    int d = g_ms.depth;
    regs_save(&g_ms.r);
    g_ms.depth = 0;
    for (int i = 0; i < d; i++) pthread_mutex_unlock(&g_mach);
    return d;
}
void ds_block_end(int d) {
    if (d > 0) { mach_lock(); for (int i = 1; i < d; i++) pthread_mutex_lock(&g_mach); }
    g_ms.depth = d;
    regs_load(&g_ms.r);
}

/* A preemption point (RECOMP_BACKEDGE every RECOMP_YIELD_EVERY loop back-edges,
 * and every swapped frame): if another guest thread waits for the machine,
 * hand it over and take it back once it has been taken. Without this the
 * render loop never blocks, and the WorldSim thread that generates a new
 * world never ran at all. */
void ds_yield(void) {
    if (__atomic_load_n(&g_mach_waiters, __ATOMIC_SEQ_CST) == 0) return;
    uint32_t gen = __atomic_load_n(&g_mach_gen, __ATOMIC_SEQ_CST);
    int d = ds_block_begin();
    for (int i = 0; i < 2000 && __atomic_load_n(&g_mach_gen, __ATOMIC_SEQ_CST) == gen; i++) {
        struct timespec t = { 0, 50000 }; nanosleep(&t, NULL);   /* until someone took it (<= 100 ms) */
    }
    ds_block_end(d);
}

/* ============================================================ setjmp/longjmp
 * Real non-local jumps for the guest (RECOMP_HOST_SETJMP in recomp_types.h):
 * a call to msvcr90's _setjmp/_setjmp3 runs the host setjmp in the lifted
 * caller's frame, and msvcr90's longjmp (a manual override) jumps back to it,
 * unwinding the host C frames in between -- Lua's pcall/error. The record is
 * keyed by the guest jmp_buf VA. If the jump crosses an hle_call_guest, the
 * machine lock is released down to the depth the setjmp saw. */
#define MAX_JMP 256
static struct { uint32_t va; recomp_jmp_t j; } g_jmp[MAX_JMP];
static int g_jmp_n;
recomp_jmp_t *recomp_jmp_slot(uint32_t va) {
    for (int i = 0; i < g_jmp_n; i++) if (g_jmp[i].va == va) return &g_jmp[i].j;
    if (g_jmp_n >= MAX_JMP) ds_fatal("too many live jmp_bufs");
    g_jmp[g_jmp_n].va = va;
    return &g_jmp[g_jmp_n++].j;
}
int recomp_is_setjmp(uint32_t va) { return va == 0x785926F0u /* _setjmp3 */ || va == 0x785926B0u /* _setjmp */; }
int recomp_jmp_depth(void) { return g_ms.depth; }
static void ds_longjmp(void) {                 /* msvcr90 longjmp(jmp_buf, int) */
    uint32_t va = MEM32(g_esp + 4), val = MEM32(g_esp + 8);
    recomp_jmp_t *j = NULL;
    for (int i = 0; i < g_jmp_n; i++) if (g_jmp[i].va == va) { j = &g_jmp[i].j; break; }
    if (!j) ds_fatal("longjmp to jmp_buf 0x%08X that no setjmp filled (from 0x%08X)", va, g_cur_func);
    j->val = val ? val : 1;
    while (g_ms.depth > j->depth) { g_ms.depth--; pthread_mutex_unlock(&g_mach); }
    longjmp(j->host, 1);
}

/* ============================================================ dispatch
 * The exe and each lifted DLL (msvcr90, msvcp90) have their own dispatch table,
 * each sorted by VA over a disjoint range; search them in turn. The DLL tables
 * are weak so the host still links before they are lifted in. */
extern __attribute__((weak)) const recomp_dispatch_entry_t msvcr90_recomp_dispatch_table[];
extern __attribute__((weak)) const uint32_t msvcr90_recomp_dispatch_count;
extern __attribute__((weak)) const recomp_dispatch_entry_t msvcp90_recomp_dispatch_table[];
extern __attribute__((weak)) const uint32_t msvcp90_recomp_dispatch_count;

static recomp_func_t table_lookup(const recomp_dispatch_entry_t *t, uint32_t n, uint32_t va) {
    uint32_t lo = 0, hi = n;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2, m = t[mid].address;
        if (m == va) return t[mid].func;
        if (m < va) lo = mid + 1; else hi = mid;
    }
    return NULL;
}
recomp_func_t recomp_lookup(uint32_t va) {
    recomp_func_t f = table_lookup(recomp_dispatch_table, recomp_dispatch_count, va);
    if (f) return f;
    if (msvcr90_recomp_dispatch_table && (f = table_lookup(msvcr90_recomp_dispatch_table, msvcr90_recomp_dispatch_count, va))) return f;
    if (msvcp90_recomp_dispatch_table && (f = table_lookup(msvcp90_recomp_dispatch_table, msvcp90_recomp_dispatch_count, va))) return f;
    return NULL;
}
/* Manual overrides: a guest VA whose lifted body is a trampoline (overrides.txt)
 * calling recomp_lookup_manual -> our native C. Used to HLE the CRT's EH helpers,
 * whose lifted form does raw return-address surgery incompatible with the recomp
 * call model (the "return address" is the RECOMP_RETADDR sentinel). */
#define MAX_MANUAL 64
static struct { uint32_t va; recomp_func_t fn; } g_manual[MAX_MANUAL];
static int g_manual_n;
void ds_register_manual(uint32_t va, recomp_func_t fn) {
    if (g_manual_n < MAX_MANUAL) { g_manual[g_manual_n].va = va; g_manual[g_manual_n].fn = fn; g_manual_n++; }
}
recomp_func_t recomp_lookup_manual(uint32_t va) {
    for (int i = 0; i < g_manual_n; i++) if (g_manual[i].va == va) return g_manual[i].fn;
    return NULL;
}

/* Native __EH_prolog (msvcr90 0x7857DF6C). Real code:
 *   push -1; push eax(scopetable); push fs:[0]; mov eax,[esp+0xC](=retaddr);
 *   mov fs:[0],esp; mov [esp+0xC],ebp; lea ebp,[esp+0xC]; push eax; ret
 * i.e. it repurposes the return-addr slot as the saved-ebp slot and returns via
 * push/ret. Here the recomp call wrapper handles the return, so we only build the
 * equivalent frame + SEH record, never treating the sentinel as a real address.
 * On entry g_esp points at the (sentinel) return-addr slot P, g_eax = scopetable. */
static void ds_eh_prolog(void) {
    static int n; if (n++ < 3) fprintf(stderr, "[ds] ds_eh_prolog #%d esp=%08X ebp=%08X eax=%08X caller=%08X\n", n, g_esp, g_ebp, g_eax, g_cur_func);
    uint32_t P = g_esp;
    uint32_t scopetable = g_eax;
    uint32_t old_fs0 = MEM32(g_fs_base + 0);
    MEM32(P)      = g_ebp;        /* retaddr slot -> saved ebp */
    MEM32(P - 4)  = 0xFFFFFFFFu;  /* try level = -1 */
    MEM32(P - 8)  = scopetable;   /* scopetable */
    MEM32(P - 12) = old_fs0;      /* EXCEPTION_REGISTRATION.Next */
    MEM32(g_fs_base + 0) = P - 12;/* fs:[0] = &record */
    g_ebp = P;
    g_esp = P - 12;
    g_eax = 0;
}

/* Native _SEH_prolog4 (msvcr90 0x7858CF00): push <size>; push <scopetable>;
 * call _SEH_prolog4. Builds the MSVC /GS EH4 frame the caller expects, in the
 * recomp convention (no return-address/esp surgery). On entry g_esp ->
 * [ret][scopetable][size]; ebp = &size-slot. */
static void ds_seh_prolog4(void) {
    uint32_t P = g_esp;
    uint32_t scopetable = MEM32(P + 4);
    uint32_t size       = MEM32(P + 8);
    uint32_t oldfs      = MEM32(g_fs_base + 0);
    uint32_t cookie     = MEM32(0x785BA2A8u);      /* __security_cookie (guest VA) */
    uint32_t ebp        = P + 8;
    MEM32(P + 8) = g_ebp;                           /* [ebp]     = saved ebp */
    MEM32(P + 4) = 0xFFFFFFFEu;                     /* [ebp-4]   = trylevel -2 */
    MEM32(P)     = scopetable ^ cookie;             /* [ebp-8]   = scopetable ^ cookie */
    MEM32(P - 4) = 0x7858CF5Eu;                     /* [ebp-0xC] = __except_handler4 */
    MEM32(P - 8) = oldfs;                           /* [ebp-0x10]= EXCEPTION_REGISTRATION.Next */
    MEM32(g_fs_base + 0) = P - 8;                   /* fs:[0] = &record */
    uint32_t sp = P - 8 - size;                     /* after sub esp, size */
    MEM32(sp - 4)  = g_ebx;
    MEM32(sp - 8)  = g_esi;
    MEM32(sp - 12) = g_edi;
    MEM32(sp - 16) = cookie ^ ebp;                  /* GS stack cookie */
    MEM32(P - 0x10) = sp - 16;                      /* [ebp-0x18] = esp snapshot */
    g_ebp = ebp;
    g_esp = sp - 16;
    g_eax = 0;
}

/* Native memcpy/memmove (msvcr90 0x7855AE20 / 0x7855B310). The VC9 versions are
 * hand-written asm that jumps into tables of tail-copy labels (`jmp [edx*4 +
 * TrailUp]`): those targets are not functions, so the lifted body hit an
 * unresolved ITAIL inside its first big copy. cdecl (dst, src, n) -> dst; VC9's
 * memcpy also tolerates overlap, so both are memmove. Only the return address
 * is popped. */
static void ds_memmove(void) {
    uint32_t d = MEM32(g_esp + 4), s = MEM32(g_esp + 8), n = MEM32(g_esp + 12);
    if (n) memmove(ds_hostptr(d), ds_hostptr(s), n);
    g_eax = d;
    g_esp += 4;
}

/* ============================================================ shim registry */
#define SHIM_BASE 0x7F000000u
#define SHIM_STEP 16u
#define MAX_SHIMS 4096
static struct { const char *name; recomp_func_t fn; uint32_t va; } g_shim[MAX_SHIMS];
static int g_shim_n;
static uint32_t g_last_import_va;
uint32_t ds_last_import_va(void) { return g_last_import_va; }   /* the shim VA being run */

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
    /* copy: a GetProcAddress name may sit in a guest stack buffer that is reused */
    g_shim[g_shim_n].name = strdup(name);
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
    cov_dump();
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
typedef struct { uint32_t image_base, entry, import_rva, export_rva, export_size, size_of_image; } ds_image;

/* IMAGE_EXPORT_DIRECTORY (not in pe_format.h) */
typedef struct {
    uint32_t Characteristics, TimeDateStamp;
    uint16_t MajorVersion, MinorVersion;
    uint32_t Name, Base, NumberOfFunctions, NumberOfNames;
    uint32_t AddressOfFunctions, AddressOfNames, AddressOfNameOrdinals;
} ds_export_dir;

/* Resolve an export name in a lifted DLL image to its function VA (0 if absent
 * or a forwarder, which we leave to the HLE/stub path). */
static uint32_t ds_dll_export(const ds_image *img, const char *name) {
    if (!img->export_rva) return 0;
    uint32_t base = img->image_base;
    ds_export_dir *ed = (ds_export_dir *)ds_hostptr(base + img->export_rva);
    uint32_t *names = (uint32_t *)ds_hostptr(base + ed->AddressOfNames);
    uint16_t *ords  = (uint16_t *)ds_hostptr(base + ed->AddressOfNameOrdinals);
    uint32_t *funcs = (uint32_t *)ds_hostptr(base + ed->AddressOfFunctions);
    for (uint32_t i = 0; i < ed->NumberOfNames; i++) {
        if (!strcmp((const char *)ds_hostptr(base + names[i]), name)) {
            uint32_t frva = funcs[ords[i]];
            if (frva >= img->export_rva && frva < img->export_rva + img->export_size) return 0; /* forwarder */
            return base + frva;
        }
    }
    return 0;
}

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
    img->export_rva = opt->DataDirectory[0].VirtualAddress;   /* PE_DIR_EXPORT */
    img->export_size = opt->DataDirectory[0].Size;
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

/* Walk a module's import directory (in the arena) and write each IAT slot's
 * resolved VA, via resolve(dll, name). Returns the number left unresolved. */
static int ds_pe_bind(const ds_image *img, uint32_t (*resolve)(const char *dll, const char *name)) {
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
                char nm[80]; snprintf(nm, sizeof nm, "%s#%u", dll, *ilt & 0xFFFFu);
                va = resolve(dll, nm);
            } else {
                const char *name = (const char *)ds_hostptr(base + (*ilt & 0x7FFFFFFFu) + 2);
                va = resolve(dll, name);
            }
            if (va) bound++; else unresolved++;
            MEM32(iat_va) = va;
        }
    }
    fprintf(stderr, "[ds] bound %d imports, %d unresolved\n", bound, unresolved);
    return unresolved;
}

/* Module-aware resolver: MSVCR90/MSVCP90 imports -> the lifted DLL's exports;
 * everything else (kernel32/user32/gdi32/ole32/...) -> an HLE shim (or a
 * self-naming stub in permissive mode). */
static int g_permissive = 1;
static const ds_image *g_msvcr_img, *g_msvcp_img;
static uint32_t resolve_import(const char *dll, const char *name) {
    if (g_msvcr_img && strncasecmp(dll, "msvcr90", 7) == 0) { uint32_t va = ds_dll_export(g_msvcr_img, name); if (va) return va; }
    if (g_msvcp_img && strncasecmp(dll, "msvcp90", 7) == 0) { uint32_t va = ds_dll_export(g_msvcp_img, name); if (va) return va; }
    return g_permissive ? ds_resolve_or_stub(name) : ds_resolve(name);
}

/* ============================================================ crash handler
 * Portable: prints the last lifted function entered (g_cur_func, set by
 * RECOMP_ENTER), the fault address as a guest VA, and the recent indirect-call
 * trace. The single most useful thing during bring-up. */
static void crash_handler(int sig, siginfo_t *si, void *uc) {
    (void)uc;
    uintptr_t fa = (uintptr_t)si->si_addr;
    fprintf(stderr, "\n[ds] *** signal %d at fault addr %p ***\n", sig, si->si_addr);
    fprintf(stderr, "[ds] g_cur_func = 0x%08X  (last lifted function entered)\n", g_cur_func);
    if (fa >= (uintptr_t)g_mem_base && fa < (uintptr_t)g_mem_base + ARENA_SIZE)
        fprintf(stderr, "[ds] fault is guest VA 0x%08X\n", (uint32_t)(fa - (uintptr_t)g_mem_base));
    else
        fprintf(stderr, "[ds] fault is a HOST address (not in the guest arena)\n");
    fprintf(stderr, "[ds] arena base host %p\n", (void *)(uintptr_t)g_mem_base);
    /* Host C backtrace = the guest call chain (recomp uses real C calls). Names
     * resolve with -rdynamic; this is our gdb-free backtrace. */
    void *bt[48]; int nbt = backtrace(bt, 48);
    fprintf(stderr, "[ds] host/guest backtrace (%d frames):\n", nbt); fflush(stderr);
    backtrace_symbols_fd(bt, nbt, 2);
    fprintf(stderr, "[ds] recent indirect calls (newest last):\n");
    for (int i = 0; i < 8; i++) {
        uint32_t idx = (g_icall_trace_idx - 8 + (uint32_t)i) & (ICALL_TRACE_SIZE - 1);
        uint32_t t = g_icall_trace[idx];
        if (t) {
            const char *nm = ds_hle_name(t);
            fprintf(stderr, "    0x%08X %-24s from 0x%08X\n", t, nm ? nm : "", g_icall_from[idx]);
        }
    }
    cov_dump();
    _exit(139);
}
static void install_crash_handler(void) {
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = crash_handler; sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL); sigaction(SIGBUS, &sa, NULL); sigaction(SIGFPE, &sa, NULL);
}

/* ============================================================ main */
extern void ds_host_init(void);   /* registers HLE shim modules (ds_kernel32.c) */

static void call_dllmain(const ds_image *img, const char *tag) {
    uint32_t a[3] = { img->image_base, 1 /*DLL_PROCESS_ATTACH*/, 0 };
    fprintf(stderr, "[ds] %s DllMain(PROCESS_ATTACH) @ 0x%08X\n", tag, img->entry);
    uint32_t rc = hle_call_guest(img->entry, 3, a);
    fprintf(stderr, "[ds] %s DllMain returned %u\n", tag, rc);
}

int main(int argc, char **argv) {
    const char *exe = NULL; int run_entry = 0, permissive = 1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--run-entry")) run_entry = 1;
        else if (!strcmp(argv[i], "--strict")) permissive = 0;
        else if (!strcmp(argv[i], "--trace")) ds_trace = 1;
        else if (argv[i][0] != '-') exe = argv[i];
    }
    if (!exe) exe = "work/dontstarve.exe";
    if (getenv("DS_HLE_TRACE")) ds_trace = 1;
    if (getenv("DS_SCREEN")) sscanf(getenv("DS_SCREEN"), "%dx%d", &g_ds_screen_w, &g_ds_screen_h);
    g_permissive = permissive;

    fprintf(stderr, "=== Don't Starve recompiled - arena host (64-bit) ===\n");
    install_crash_handler();
    atexit(cov_dump);
    if (!arena_init()) return 1;

    /* Map the exe and the lifted CRT DLLs into the arena at their ImageBases. */
    ds_image img_exe, img_r, img_p;
    if (ds_pe_load(exe, &img_exe) != 0) return 1;
    /* the VC9 CRT DLLs (the images the lifted code was built from): DS_CRT_DIR, default work/ */
    const char *crt = getenv("DS_CRT_DIR"); if (!crt || !*crt) crt = "work";
    char pr[512], pp[512];
    snprintf(pr, sizeof pr, "%s/msvcr90.dll", crt); snprintf(pp, sizeof pp, "%s/msvcp90.dll", crt);
    int have_r = (ds_pe_load(pr, &img_r) == 0); if (have_r) g_msvcr_img = &img_r;
    int have_p = (ds_pe_load(pp, &img_p) == 0); if (have_p) g_msvcp_img = &img_p;

    ds_host_init();
    ds_register_manual(0x7857DF6Cu, ds_eh_prolog);     /* msvcr90 __EH_prolog */
    ds_register_manual(0x7858CF00u, ds_seh_prolog4);   /* msvcr90 _SEH_prolog4 */
    ds_register_manual(0x7855AE20u, ds_memmove);       /* msvcr90 memcpy */
    ds_register_manual(0x7855B310u, ds_memmove);       /* msvcr90 memmove */
    ds_register_manual(0x785947D8u, ds_longjmp);       /* msvcr90 longjmp */
    recomp_yield_hook = ds_yield;                      /* RECOMP_BACKEDGE preemption point */
    fprintf(stderr, "[ds] registered %d HLE shims; lifted DLLs: msvcr90=%d msvcp90=%d\n",
            g_shim_n, have_r, have_p);

    /* Bind every module's IAT (DLLs depend on kernel32 HLE + each other). */
    int u = ds_pe_bind(&img_exe, resolve_import);
    if (have_r) ds_pe_bind(&img_r, resolve_import);
    if (have_p) ds_pe_bind(&img_p, resolve_import);
    fprintf(stderr, "[ds] dispatch entry 0x%08X %s\n", img_exe.entry,
            recomp_lookup(img_exe.entry) ? "in dispatch" : "MISSING");

    if (!run_entry) {
        fprintf(stderr, "[ds] boot OK (exe %d unresolved). Pass --run-entry to start.\n", u);
        return 0;
    }

    /* OS loader order: initialise the CRT DLLs (msvcr90 first; msvcp90 needs it)
     * before the exe's own CRT startup runs. */
    if (have_r) call_dllmain(&img_r, "msvcr90");
    if (have_p) call_dllmain(&img_p, "msvcp90");

    fprintf(stderr, "[ds] running exe entry...\n");
    uint32_t args[1];
    uint32_t rc = hle_call_guest(img_exe.entry, 0, args);
    fprintf(stderr, "[ds] exe entry returned 0x%08X\n", rc);
    return (int)rc;
}
