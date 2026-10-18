/*
 * ds_hle.h - arena-model host for the recompiled Don't Starve guest.
 *
 * This is the 64-bit-host spine that will also ship on aarch64 (the SAME memory
 * model): the guest lives in a contiguous arena reserved on the host, guest
 * pointers stay 32-bit, and every access resolves as g_mem_base + (uint32_t)va
 * (exactly recomp_types.h's ADDR()). Unlike pcrecomp/runtime/win32hle (which is
 * a 32-bit-host harness that treats a guest VA as a host address), everything
 * here goes through ADDR(), so x86-64 debug and aarch64 ship use one model.
 *
 * Shim ABI (mirrors win32hle's, but arena-aware): an import is a recomp_func_t
 * run inside the lifted model. On entry g_esp -> [ret][arg0][arg1]... in GUEST
 * memory; a shim reads args with A32/APTR/ASTR (through ADDR), does the work,
 * and returns with RET/RETV, which pops ret + argc*4 (stdcall, callee-cleans).
 * Values handed back to the guest are GUEST VAs (uint32_t) or handle tokens,
 * never truncated host pointers.
 */
#ifndef DS_HLE_H
#define DS_HLE_H
#include <stdint.h>
#include "recomp_types.h"   /* g_eax, g_esp, MEM32, ADDR, recomp_func_t, dispatch */

/* ---- stdcall argument access (arena-aware) ---- */
#define A32(i)   ((uint32_t)MEM32(g_esp + 4u + 4u * (uint32_t)(i)))
#define APTR(i)  ((void *)(uintptr_t)ADDR(A32(i)))   /* host pointer to the guest buffer */
#define ASTR(i)  ((char *)(uintptr_t)ADDR(A32(i)))
/* A guest VA argument kept as a VA (for pointer arithmetic in guest space). */
#define AVA(i)   (A32(i))

/* ---- return from a stdcall shim: set eax, pop ret + argc args ---- */
#define RET(eax, argc)   do { g_eax = (uint32_t)(eax); g_esp += 4u + 4u * (uint32_t)(argc); return; } while (0)
#define RETV(argc)       RET(0u, argc)

/* host pointer <-> guest VA within the arena (for shims that need raw access) */
static inline void    *ds_hostptr(uint32_t va) { return (void *)(uintptr_t)ADDR(va); }

/* ---- arena allocator: hands out guest VAs backed by the reserved arena ---- */
uint32_t ds_arena_alloc(uint32_t size, uint32_t align);   /* 0 on exhaustion */
void     ds_arena_reset_to(uint32_t va);                  /* rare: rewind bump pointer */

/* ---- handle table: opaque uint32_t tokens (NOT host pointers) ---- */
uint32_t ds_handle_new(int kind, void *host_obj);         /* token >= 0x1 */
void    *ds_handle_get(uint32_t token, int kind);         /* NULL if wrong kind/unknown */
int      ds_handle_kind(uint32_t token);

/* ---- shim registry ---- */
typedef struct { const char *name; recomp_func_t fn; } ds_shim;
int       ds_register(const ds_shim *shims);              /* NULL-terminated; returns count */
uint32_t  ds_resolve(const char *name);                   /* synthetic VA or 0 */
uint32_t  ds_resolve_or_stub(const char *name);           /* unknown -> self-naming abort stub */
const char *ds_hle_name(uint32_t va);                     /* name for a synthetic VA or NULL */

/* ---- machine lock + native->guest ---- */
void      mach_enter(void);
void      mach_leave(void);
uint32_t  hle_call_guest(uint32_t va, int nargs, const uint32_t *args);

/* ---- host services ---- */
void      ds_fatal(const char *fmt, ...);
extern int ds_trace;                                      /* DS_HLE_TRACE: one line per shim call */

/* each module's table (defined in its .c, registered by the host) */
extern const ds_shim ds_kernel32[];
extern const ds_shim ds_win32[];

#endif /* DS_HLE_H */
