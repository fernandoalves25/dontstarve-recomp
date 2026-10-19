/*
 * ds_sample.c -- a sampling profiler built into the host (DS_SAMPLE=1).
 *
 * Measuring on the device is the only way to know where a frame's time goes
 * there (game code, the GPU driver, the kernel, the audio mixer), so the host
 * can profile itself and write the result to stderr (the port's log.txt):
 * setitimer(ITIMER_PROF) sends SIGPROF every DS_SAMPLE_MS of process CPU time
 * (any thread), the handler stores the interrupted PC and the thread id, and
 * a reporter thread prints, every DS_SAMPLE_SEC seconds:
 *   - the CPU clock and governor, and the GPU's (devfreq),
 *   - CPU time per thread (from /proc: user + system),
 *   - samples per library and the top functions. A lifted function's name is
 *     its guest address (sub_005B6200); the binary keeps its dynamic symbols
 *     (-rdynamic) even stripped. Runtime statics show as the nearest exported
 *     symbol; the raw offset resolves them against the unstripped build.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <pthread.h>
#include <unistd.h>
#include <dirent.h>
#include <dlfcn.h>
#include <link.h>
#include <time.h>
#include <ucontext.h>
#include <sys/time.h>
#include <sys/syscall.h>

#define SMAX (1u << 18)
static uintptr_t s_pc[SMAX];
static int s_tid[SMAX];
static volatile uint32_t s_n;
static int s_self_tid;

static void on_prof(int sig, siginfo_t *si, void *uc_)
{
    (void)sig; (void)si;
    ucontext_t *uc = (ucontext_t *)uc_;
    uintptr_t pc;
#if defined(__aarch64__)
    pc = (uintptr_t)uc->uc_mcontext.pc;
#elif defined(__x86_64__)
    pc = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
#else
    pc = 0;
#endif
    uint32_t i = __atomic_fetch_add(&s_n, 1, __ATOMIC_RELAXED);
    if (i < SMAX) { s_pc[i] = pc; s_tid[i] = (int)syscall(SYS_gettid); }
}

static void read_line(const char *path, char *out, size_t n)
{
    out[0] = 0;
    FILE *f = fopen(path, "r");
    if (!f) return;
    if (fgets(out, (int)n, f)) out[strcspn(out, "\n")] = 0;
    fclose(f);
}

static void clocks(void)
{
    char gov[64], cur[64], max[64];
    read_line("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor", gov, sizeof gov);
    read_line("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", cur, sizeof cur);
    read_line("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq", max, sizeof max);
    fprintf(stderr, "[sample] cpu: governor %s, %s / %s kHz", gov[0] ? gov : "?", cur[0] ? cur : "?", max[0] ? max : "?");
    DIR *d = opendir("/sys/class/devfreq");
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[512], g[64], c[64];
        snprintf(p, sizeof p, "/sys/class/devfreq/%s/governor", e->d_name); read_line(p, g, sizeof g);
        snprintf(p, sizeof p, "/sys/class/devfreq/%s/cur_freq", e->d_name); read_line(p, c, sizeof c);
        fprintf(stderr, "; %s: %s %s Hz", e->d_name, g, c);
    }
    if (d) closedir(d);
    char tz[64];
    read_line("/sys/class/thermal/thermal_zone0/temp", tz, sizeof tz);
    if (tz[0]) fprintf(stderr, "; temp %.1f C", atoi(tz) / 1000.0);
    fputc('\n', stderr);
}

/* CPU seconds per thread since the last report, from /proc/self/task/<tid>/stat */
#define TMAX 64
static int t_tid[TMAX]; static long t_ticks[TMAX], t_sys[TMAX]; static int t_n;

static void threads(double wall)
{
    DIR *d = opendir("/proc/self/task");
    if (!d) return;
    long hz = sysconf(_SC_CLK_TCK);
    struct dirent *e;
    fprintf(stderr, "[sample] threads (CPU %% of %.0f s wall, user+system):", wall);
    while ((e = readdir(d))) {
        int tid = atoi(e->d_name);
        if (tid <= 0) continue;
        char p[128], buf[512], comm[64];
        snprintf(p, sizeof p, "/proc/self/task/%d/stat", tid); read_line(p, buf, sizeof buf);
        snprintf(p, sizeof p, "/proc/self/task/%d/comm", tid); read_line(p, comm, sizeof comm);
        char *rp = strrchr(buf, ')');
        if (!rp) continue;
        unsigned long ut = 0, st = 0;
        /* after ")": state ppid pgrp session tty tpgid flags minflt cminflt majflt cmajflt utime stime */
        if (sscanf(rp + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", &ut, &st) != 2) continue;
        long ticks = (long)(ut + st), prev = 0, prev_sys = 0;
        int k;
        for (k = 0; k < t_n && t_tid[k] != tid; k++) {}
        if (k < t_n) { prev = t_ticks[k]; prev_sys = t_sys[k]; }
        else if (t_n < TMAX) { k = t_n++; t_tid[k] = tid; }
        if (k < TMAX) { t_ticks[k] = ticks; t_sys[k] = (long)st; }
        double pct = wall > 0 ? 100.0 * (ticks - prev) / hz / wall : 0;
        double sys = wall > 0 ? 100.0 * ((long)st - prev_sys) / hz / wall : 0;
        if (pct >= 0.5) fprintf(stderr, " %s[%d] %.0f%% (kernel %.0f%%)", comm, tid, pct, sys);
    }
    closedir(d);
    fputc('\n', stderr);
}

typedef struct { uintptr_t lo, hi; const char *lib, *sym; uintptr_t base; uint32_t n; } fn_t;
static int cmp_pc(const void *a, const void *b)
{ uintptr_t x = *(const uintptr_t *)a, y = *(const uintptr_t *)b; return x < y ? -1 : x > y; }
static int cmp_n(const void *a, const void *b)
{ uint32_t x = ((const fn_t *)a)->n, y = ((const fn_t *)b)->n; return x > y ? -1 : x < y; }

static void report(uint32_t n, int top)
{
    static uintptr_t pcs[SMAX];
    uint32_t main_n = 0;
    for (uint32_t i = 0; i < n; i++) if (s_tid[i] != s_self_tid) pcs[main_n++] = s_pc[i];
    qsort(pcs, main_n, sizeof *pcs, cmp_pc);
    static fn_t fn[16384]; int nf = 0;
    for (uint32_t i = 0; i < main_n; i++) {
        uintptr_t pc = pcs[i];
        if (nf && pc >= fn[nf - 1].lo && pc < fn[nf - 1].hi) { fn[nf - 1].n++; continue; }
        if (nf == (int)(sizeof fn / sizeof *fn)) break;
        Dl_info di; const ElfW(Sym) *sym = NULL;
        fn_t f = { pc, pc + 1, "?", NULL, 0, 1 };
        if (dladdr1((void *)pc, &di, (void **)&sym, RTLD_DL_SYMENT)) {
            const char *b = di.dli_fname ? strrchr(di.dli_fname, '/') : NULL;
            f.lib = b ? b + 1 : (di.dli_fname && di.dli_fname[0] ? di.dli_fname : "main");
            f.base = (uintptr_t)di.dli_fbase;
            if (di.dli_sname && sym && pc < (uintptr_t)di.dli_saddr + sym->st_size) {
                f.sym = di.dli_sname; f.lo = (uintptr_t)di.dli_saddr; f.hi = f.lo + sym->st_size;
            } else f.lo = f.hi = 0;               /* unnamed: one entry per 256-byte bucket */
        }
        if (!f.sym) {
            f.lo = pc & ~(uintptr_t)255; f.hi = f.lo + 256;
            if (nf && fn[nf - 1].lo == f.lo && !fn[nf - 1].sym) { fn[nf - 1].n++; continue; }
        }
        fn[nf++] = f;
    }
    /* per library */
    struct { const char *lib; uint32_t n; } libs[32]; int nl = 0;
    for (int i = 0; i < nf; i++) {
        int k;
        for (k = 0; k < nl && strcmp(libs[k].lib, fn[i].lib); k++) {}
        if (k == nl) { if (nl == 32) continue; libs[nl].lib = fn[i].lib; libs[nl].n = 0; nl++; }
        libs[k].n += fn[i].n;
    }
    fprintf(stderr, "[sample] %u samples by library:", main_n);
    for (int k = 0; k < nl; k++)
        if (libs[k].n * 200 >= main_n) fprintf(stderr, " %s %.1f%%", libs[k].lib, 100.0 * libs[k].n / main_n);
    fputc('\n', stderr);
    qsort(fn, nf, sizeof *fn, cmp_n);
    for (int i = 0; i < nf && i < top; i++)
        fprintf(stderr, "[sample]  %5.1f%%  %s %s +0x%lx\n", 100.0 * fn[i].n / main_n, fn[i].lib,
                fn[i].sym ? fn[i].sym : "-", (unsigned long)(fn[i].lo - fn[i].base));
}

static void *reporter(void *arg)
{
    (void)arg;
    s_self_tid = (int)syscall(SYS_gettid);
    const char *e = getenv("DS_SAMPLE_SEC"); int sec = e ? atoi(e) : 30; if (sec < 2) sec = 2;
    e = getenv("DS_SAMPLE_TOP"); int top = e ? atoi(e) : 30;
    threads(0);
    for (unsigned round = 1;; round++) {
        struct timespec ts = { sec, 0 };
        while (nanosleep(&ts, &ts) != 0) {}
        uint32_t n = __atomic_load_n(&s_n, __ATOMIC_RELAXED);
        if (n > SMAX) n = SMAX;
        fprintf(stderr, "[sample] ---- report %u (last %d s) ----\n", round, sec);
        clocks();
        threads(sec);
        report(n, top);
        __atomic_store_n(&s_n, 0, __ATOMIC_RELAXED);
    }
    return NULL;
}

void ds_sample_init(void)
{
    const char *e = getenv("DS_SAMPLE");
    if (!e || !*e || !strcmp(e, "0")) return;
    e = getenv("DS_SAMPLE_MS"); int ms = e ? atoi(e) : 2; if (ms < 1) ms = 1;
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_prof; sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGPROF, &sa, NULL);
    pthread_t t; pthread_attr_t at; pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    sigset_t all, old; sigfillset(&all); pthread_sigmask(SIG_BLOCK, &all, &old);  /* the reporter takes no signals */
    pthread_create(&t, &at, reporter, NULL);
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    pthread_setname_np(t, "ds-sample");
    struct itimerval it = { { 0, ms * 1000 }, { 0, ms * 1000 } };
    setitimer(ITIMER_PROF, &it, NULL);
    fprintf(stderr, "[sample] profiling every %d ms of CPU time, reports in log\n", ms);
}
