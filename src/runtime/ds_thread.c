/*
 * ds_thread.c - kernel32 threads and waitable objects.
 *
 * Each guest thread is a host pthread running hle_call_guest(start, param);
 * mach_enter gives it its own guest stack and TIB. The machine lock still
 * serialises guest code: a thread runs until it blocks in one of the shims
 * below, which release the lock around the wait (ds_block_begin/end).
 * Waitable objects (threads, mutexes, semaphores) are host structs guarded by
 * one mutex + condition variable; any state change broadcasts.
 * All WINAPI (stdcall). Arguments are read before blocking, RET after.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <sched.h>
#include <pthread.h>
#include "ds_hle.h"

#define WAIT_OBJECT_0  0x000u
#define WAIT_TIMEOUT   0x102u
#define WAIT_FAILED    0xFFFFFFFFu
#define INFINITE       0xFFFFFFFFu
#define STILL_ACTIVE   259u

enum { OBJ_THREAD = 1, OBJ_MUTEX, OBJ_SEMAPHORE };
typedef struct {
    int type;
    uint32_t tid;                  /* thread: its id; mutex: owner id (0 = free) */
    int exited, suspended;         /* thread */
    uint32_t exit_code, start, param;
    int recursion;                 /* mutex */
    int32_t count, max;            /* semaphore */
} wobj;

static pthread_mutex_t g_sync = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_sync_cv = PTHREAD_COND_INITIALIZER;

static uint32_t g_next_tid = 0x1000;
static __thread uint32_t t_tid;
static __thread wobj *t_self;
uint32_t ds_thread_id(void) {          /* also GetCurrentThreadId */
    if (!t_tid) { pthread_mutex_lock(&g_sync); t_tid = g_next_tid += 4; pthread_mutex_unlock(&g_sync); }
    return t_tid;
}
static wobj *self_obj(void) {          /* the calling thread's object (main thread: made lazily) */
    if (!t_self) {
        t_self = (wobj *)calloc(1, sizeof *t_self);
        t_self->type = OBJ_THREAD; t_self->tid = ds_thread_id();
    }
    return t_self;
}
static wobj *obj(uint32_t h) {
    if (h == 0xFFFFFFFEu) return self_obj();               /* GetCurrentThread pseudo-handle */
    return (wobj *)ds_handle_get(h, HK_THREAD);            /* all waitables share this kind */
}

/* ---- waiting: g_sync held */
static int signaled_take(wobj *o, uint32_t me) {          /* 1 if o is signaled; consumes it */
    switch (o->type) {
    case OBJ_THREAD: return o->exited;
    case OBJ_MUTEX:
        if (o->tid == 0 || o->tid == me) { o->tid = me; o->recursion++; return 1; }
        return 0;
    case OBJ_SEMAPHORE:
        if (o->count > 0) { o->count--; return 1; }
        return 0;
    }
    return 0;
}
static int signaled_peek(wobj *o, uint32_t me) {
    switch (o->type) {
    case OBJ_THREAD: return o->exited;
    case OBJ_MUTEX: return o->tid == 0 || o->tid == me;
    case OBJ_SEMAPHORE: return o->count > 0;
    }
    return 0;
}
static void deadline(struct timespec *ts, uint32_t ms) {
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_sec += ms / 1000; ts->tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) { ts->tv_sec++; ts->tv_nsec -= 1000000000L; }
}
/* Wait for n objects (all or any). Returns a WAIT_* code. Caller holds the machine lock. */
static uint32_t wait_objs(wobj **o, uint32_t n, int all, uint32_t ms) {
    uint32_t me = ds_thread_id();
    struct timespec ts; if (ms != INFINITE) deadline(&ts, ms);
    pthread_mutex_lock(&g_sync);
    uint32_t r = WAIT_TIMEOUT;
    /* fast path without releasing the machine */
    for (;;) {
        if (all) {
            uint32_t k = 0; while (k < n && signaled_peek(o[k], me)) k++;
            if (k == n) { for (k = 0; k < n; k++) signaled_take(o[k], me); r = WAIT_OBJECT_0; break; }
        } else {
            uint32_t k = 0; while (k < n && !signaled_take(o[k], me)) k++;
            if (k < n) { r = WAIT_OBJECT_0 + k; break; }
        }
        if (ms == 0) break;
        int d = ds_block_begin();      /* let other guest threads run while we sleep */
        int e = ms == INFINITE ? pthread_cond_wait(&g_sync_cv, &g_sync)
                               : pthread_cond_timedwait(&g_sync_cv, &g_sync, &ts);
        pthread_mutex_unlock(&g_sync);
        ds_block_end(d);
        pthread_mutex_lock(&g_sync);
        if (e == ETIMEDOUT) {          /* one last look */
            if (!all) { uint32_t k = 0; while (k < n && !signaled_take(o[k], me)) k++; if (k < n) r = WAIT_OBJECT_0 + k; }
            break;
        }
    }
    pthread_mutex_unlock(&g_sync);
    return r;
}

static void sh_WaitForSingleObject(void) {        /* (h, ms) */
    wobj *o = obj(A32(0)); uint32_t ms = A32(1);
    if (!o) RET(WAIT_FAILED, 2);
    uint32_t r = wait_objs(&o, 1, 0, ms);
    RET(r, 2);
}
static void sh_WaitForMultipleObjects(void) {     /* (n, const HANDLE*, bWaitAll, ms) */
    uint32_t n = A32(0), hs = A32(1), all = A32(2), ms = A32(3);
    wobj *o[64];
    if (n == 0 || n > 64) RET(WAIT_FAILED, 4);
    for (uint32_t i = 0; i < n; i++) if (!(o[i] = obj(MEM32(hs + 4 * i)))) RET(WAIT_FAILED, 4);
    uint32_t r = wait_objs(o, n, all != 0, ms);
    RET(r, 4);
}
static void host_sleep(uint32_t ms) {
    int d = ds_block_begin();
    if (ms == 0) sched_yield();
    else { struct timespec t = { ms / 1000, (long)(ms % 1000) * 1000000L }; nanosleep(&t, NULL); }
    ds_block_end(d);
}
static void sh_Sleep(void)   { uint32_t ms = A32(0); host_sleep(ms); RETV(1); }
static void sh_SleepEx(void) { uint32_t ms = A32(0); host_sleep(ms); RET(0, 2); }   /* (ms, bAlertable) */

/* ---- mutexes / semaphores */
static uint32_t new_obj(int type) {
    wobj *o = (wobj *)calloc(1, sizeof *o); o->type = type;
    return ds_handle_new(HK_THREAD, o);
}
static void sh_CreateMutexA(void) {               /* (sec, bInitialOwner, name) */
    uint32_t h = new_obj(OBJ_MUTEX);
    if (A32(1)) { wobj *o = obj(h); o->tid = ds_thread_id(); o->recursion = 1; }
    ds_set_last_error(0);
    RET(h, 3);
}
static void sh_ReleaseMutex(void) {
    wobj *o = obj(A32(0)); uint32_t ok = 0;
    pthread_mutex_lock(&g_sync);
    if (o && o->type == OBJ_MUTEX && o->tid == ds_thread_id()) {
        if (--o->recursion == 0) o->tid = 0;
        ok = 1; pthread_cond_broadcast(&g_sync_cv);
    }
    pthread_mutex_unlock(&g_sync);
    if (!ok) ds_set_last_error(288);              /* ERROR_NOT_OWNER */
    RET(ok, 1);
}
static void sh_CreateSemaphoreA(void) {           /* (sec, lInitialCount, lMaximumCount, name) */
    uint32_t h = new_obj(OBJ_SEMAPHORE);
    wobj *o = obj(h); o->count = (int32_t)A32(1); o->max = (int32_t)A32(2);
    ds_set_last_error(0);
    RET(h, 4);
}
static void sh_ReleaseSemaphore(void) {           /* (h, lReleaseCount, lpPreviousCount) */
    wobj *o = obj(A32(0)); int32_t add = (int32_t)A32(1); uint32_t prev = A32(2), ok = 0;
    pthread_mutex_lock(&g_sync);
    if (o && o->type == OBJ_SEMAPHORE && add > 0 && o->count + add <= o->max) {
        if (prev) MEM32(prev) = (uint32_t)o->count;
        o->count += add; ok = 1; pthread_cond_broadcast(&g_sync_cv);
    }
    pthread_mutex_unlock(&g_sync);
    if (!ok) ds_set_last_error(298);              /* ERROR_TOO_MANY_POSTS */
    RET(ok, 3);
}

/* ---- threads */
static void thread_exit(wobj *o, uint32_t code) {
    pthread_mutex_lock(&g_sync);
    o->exited = 1; o->exit_code = code;
    pthread_cond_broadcast(&g_sync_cv);
    pthread_mutex_unlock(&g_sync);
}
static void *thread_main(void *arg) {
    wobj *o = (wobj *)arg;
    t_self = o; t_tid = o->tid;
    pthread_mutex_lock(&g_sync);
    while (o->suspended) pthread_cond_wait(&g_sync_cv, &g_sync);
    pthread_mutex_unlock(&g_sync);
    uint32_t a[1] = { o->param };
    uint32_t rc = hle_call_guest(o->start, 1, a);   /* LPTHREAD_START_ROUTINE is stdcall(LPVOID) */
    thread_exit(o, rc);
    return NULL;
}
static void sh_CreateThread(void) {   /* (sec, stack, lpStartAddress, lpParameter, dwCreationFlags, lpThreadId) */
    wobj *o = (wobj *)calloc(1, sizeof *o);
    o->type = OBJ_THREAD; o->start = A32(2); o->param = A32(3);
    o->suspended = (A32(4) & 0x4) != 0;            /* CREATE_SUSPENDED */
    pthread_mutex_lock(&g_sync); o->tid = g_next_tid += 4; pthread_mutex_unlock(&g_sync);
    uint32_t h = ds_handle_new(HK_THREAD, o);
    if (A32(5)) MEM32(A32(5)) = o->tid;
    fprintf(stderr, "[thread] CreateThread(start 0x%08X, param 0x%08X%s) -> tid 0x%X\n",
            o->start, o->param, o->suspended ? ", suspended" : "", o->tid);
    pthread_t pt; pthread_attr_t at; pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&at, 16u << 20);     /* lifted C recursion is deep: a roomy host stack */
    if (pthread_create(&pt, &at, thread_main, o)) RET(0, 6);
    RET(h, 6);
}
static void sh_ResumeThread(void) {
    wobj *o = obj(A32(0)); uint32_t prev = 0;
    pthread_mutex_lock(&g_sync);
    if (o && o->type == OBJ_THREAD) { prev = (uint32_t)o->suspended; o->suspended = 0; pthread_cond_broadcast(&g_sync_cv); }
    pthread_mutex_unlock(&g_sync);
    RET(prev, 1);
}
static void sh_ExitThread(void) {
    uint32_t code = A32(0);
    wobj *o = self_obj();
    thread_exit(o, code);
    ds_block_begin();                              /* hand the machine back for good */
    pthread_exit(NULL);
}
static void sh_GetExitCodeThread(void) {           /* (h, lpExitCode) */
    wobj *o = obj(A32(0));
    if (!o || o->type != OBJ_THREAD) RET(0, 2);
    if (A32(1)) MEM32(A32(1)) = o->exited ? o->exit_code : STILL_ACTIVE;
    RET(1, 2);
}
static void sh_GetCurrentThreadId(void) { RET(ds_thread_id(), 0); }
static void sh_SetThreadPriority(void)  { RET(1, 2); }
static void sh_DuplicateHandle(void) {  /* (srcProc, src, dstProc, *dst, access, inherit, options) */
    uint32_t src = A32(1), dst = A32(3);
    uint32_t h = src;
    if (src == 0xFFFFFFFEu) h = ds_handle_new(HK_THREAD, self_obj());   /* real handle for this thread */
    if (dst) MEM32(dst) = h;
    RET(1, 7);
}

const ds_shim ds_thread[] = {
    { "CreateThread", sh_CreateThread }, { "ResumeThread", sh_ResumeThread },
    { "ExitThread", sh_ExitThread }, { "GetExitCodeThread", sh_GetExitCodeThread },
    { "GetCurrentThreadId", sh_GetCurrentThreadId }, { "SetThreadPriority", sh_SetThreadPriority },
    { "DuplicateHandle", sh_DuplicateHandle },
    { "WaitForSingleObject", sh_WaitForSingleObject }, { "WaitForMultipleObjects", sh_WaitForMultipleObjects },
    { "Sleep", sh_Sleep }, { "SleepEx", sh_SleepEx },
    { "CreateMutexA", sh_CreateMutexA }, { "ReleaseMutex", sh_ReleaseMutex },
    { "CreateSemaphoreA", sh_CreateSemaphoreA }, { "ReleaseSemaphore", sh_ReleaseSemaphore },
    { 0, 0 }
};
