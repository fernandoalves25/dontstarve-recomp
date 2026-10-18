/*
 * ds_file.c - kernel32 file I/O over the host filesystem.
 *
 * The guest sees the standalone layout on a virtual drive (ds_kernel32.c):
 *   Z:\dontstarve\...  -> $DS_GAME_DIR  (default "gamedata": the game's install dir, bin\ and data\)
 *   Z:\home\...        -> $DS_HOME_DIR  (default "build/home": Documents, saves, logs)
 * Relative paths resolve against the guest cwd first. Lookup is case-insensitive
 * per component, as on Windows. Anything else does not exist.
 * Handles are tokens (ds_hle.h); std handles stay HK_FILE carrying their
 * nStdHandle id. All WINAPI (stdcall): RET pops the args.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <fnmatch.h>
#include "ds_hle.h"

#define INVALID_HANDLE  0xFFFFFFFFu
#define ERROR_FILE_NOT_FOUND 2u
#define ERROR_PATH_NOT_FOUND 3u
#define ERROR_ACCESS_DENIED  5u
#define ERROR_FILE_EXISTS    80u
#define ERROR_ALREADY_EXISTS 183u

typedef struct { int fd; char path[512]; } hostfile;

static int fio_trace = -1;
static int trace_on(void) { if (fio_trace < 0) { const char *e = getenv("DS_FILE_TRACE"); fio_trace = e && *e == '1'; } return fio_trace; }

/* ---- guest strings */
static void guest_w_to_a(uint32_t p, char *out, size_t cap) {   /* UTF-16 -> ASCII (paths) */
    size_t i = 0;
    for (; i < cap - 1 && MEM16(p + 2 * i); i++) { uint16_t c = MEM16(p + 2 * i); out[i] = c < 0x80 ? (char)c : '_'; }
    out[i] = 0;
}

/* Case-insensitive walk of `rel` (with '/') under `base`; writes the host path.
 * Returns 1 if every component exists, 0 if only the last one is missing (a
 * file that may be created), -1 if a directory along the way is missing. */
static int resolve_ci(const char *base, const char *rel, char *out, size_t cap) {
    snprintf(out, cap, "%s", base);
    const char *p = rel;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *e = strchr(p, '/'); size_t n = e ? (size_t)(e - p) : strlen(p);
        char comp[256]; if (n >= sizeof comp) n = sizeof comp - 1;
        memcpy(comp, p, n); comp[n] = 0;
        p += n;
        int last = p[strspn(p, "/")] == 0;
        if (!strcmp(comp, ".")) continue;
        if (!strcmp(comp, "..")) { char *s = strrchr(out, '/'); if (s && s != out) *s = 0; continue; }
        size_t ol = strlen(out);
        snprintf(out + ol, cap - ol, "/%s", comp);
        struct stat st;
        if (stat(out, &st) == 0) continue;
        out[ol] = 0;                                   /* exact miss: scan the directory */
        DIR *d = opendir(out[0] ? out : ".");
        int found = 0;
        if (d) {
            struct dirent *de;
            while ((de = readdir(d))) if (!strcasecmp(de->d_name, comp)) { snprintf(out + ol, cap - ol, "/%s", de->d_name); found = 1; break; }
            closedir(d);
        }
        if (!found) { snprintf(out + ol, cap - ol, "/%s%s", comp, p); return last ? 0 : -1; }   /* keep the rest for messages */
    }
    return 1;
}

/* Guest path -> host path. Returns as resolve_ci, or -2 if outside the virtual drive. */
static void ensure_home(void);
int ds_map_path(const char *gpath, char *out, size_t cap) {
    char full[600], *s;
    ensure_home();
    if (gpath[0] && gpath[1] == ':') snprintf(full, sizeof full, "%s", gpath);
    else if (gpath[0] == '\\' || gpath[0] == '/') snprintf(full, sizeof full, "Z:%s", gpath);
    else snprintf(full, sizeof full, "%s\\%s", ds_cwd(), gpath);
    for (s = full; *s; s++) if (*s == '\\') *s = '/';
    static const struct { const char *prefix, *env, *def; } roots[] = {
        { "Z:/dontstarve", "DS_GAME_DIR", "gamedata" },
        { "Z:/home", "DS_HOME_DIR", "build/home" },
    };
    for (size_t i = 0; i < sizeof roots / sizeof roots[0]; i++) {
        size_t n = strlen(roots[i].prefix);
        if (!strncasecmp(full, roots[i].prefix, n) && (full[n] == '/' || !full[n])) {
            const char *base = getenv(roots[i].env); if (!base || !*base) base = roots[i].def;
            return resolve_ci(base, full + n, out, cap);
        }
    }
    snprintf(out, cap, "%s", full);
    return -2;
}

static void mkdir_p(const char *dir) {
    char tmp[512]; snprintf(tmp, sizeof tmp, "%s", dir);
    for (char *p = tmp + 1; *p; p++) if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
    mkdir(tmp, 0755);
}
int ds_mkdirs(const char *gpath) {             /* SHCreateDirectoryEx: every missing level */
    char host[512];
    int r = ds_map_path(gpath, host, sizeof host);
    if (r == -2) return 0;
    mkdir_p(host);
    struct stat st;
    return stat(host, &st) == 0 && S_ISDIR(st.st_mode);
}
static void ensure_home(void) {   /* Documents and the game's folder in it exist, as after a first run on a PC */
    static int done; if (done) return; done = 1;
    const char *h = getenv("DS_HOME_DIR");
    char d[512]; snprintf(d, sizeof d, "%s/Documents/Klei/DoNotStarve", h && *h ? h : "build/home");
    mkdir_p(d);
}

/* ===================== CreateFile */
static uint32_t create_file(const char *gpath, uint32_t access, uint32_t disp) {
    char host[512];
    int r = ds_map_path(gpath, host, sizeof host);
    int wr = (access & 0x40000000u) != 0, rd = (access & 0x80000000u) != 0 || !wr;
    int fl = (rd && wr) ? O_RDWR : wr ? O_WRONLY : O_RDONLY;
    switch (disp) {
    case 1: fl |= O_CREAT | O_EXCL; break;    /* CREATE_NEW */
    case 2: fl |= O_CREAT | O_TRUNC; break;   /* CREATE_ALWAYS */
    case 3: break;                            /* OPEN_EXISTING */
    case 4: fl |= O_CREAT; break;             /* OPEN_ALWAYS */
    case 5: fl |= O_TRUNC; break;             /* TRUNCATE_EXISTING */
    }
    int fd = -1;
    struct stat st;
    if (r >= 0 && !(stat(host, &st) == 0 && S_ISDIR(st.st_mode))) fd = open(host, fl, 0644);
    if (trace_on())                           /* the DLC/patch probes miss by design: trace only */
        fprintf(stderr, "[file] CreateFile(%s, %s%s, disp %u) -> %s%s\n", gpath, rd ? "r" : "", wr ? "w" : "",
                disp, host, fd < 0 ? "  NOT FOUND" : "");
    if (fd < 0) {
        uint32_t err = r == -1 || r == -2 ? ERROR_PATH_NOT_FOUND : errno == EEXIST ? ERROR_FILE_EXISTS
                     : errno == EACCES || errno == EISDIR ? ERROR_ACCESS_DENIED : ERROR_FILE_NOT_FOUND;
        if (r == 1 && stat(host, &st) == 0 && S_ISDIR(st.st_mode)) err = ERROR_ACCESS_DENIED;
        ds_set_last_error(err);
        return INVALID_HANDLE;
    }
    hostfile *hf = (hostfile *)calloc(1, sizeof *hf);
    hf->fd = fd; snprintf(hf->path, sizeof hf->path, "%s", host);
    /* OPEN_ALWAYS / CREATE_ALWAYS report whether the file already existed */
    ds_set_last_error((disp == 2 || disp == 4) && r == 1 ? ERROR_ALREADY_EXISTS : 0);
    return ds_handle_new(HK_HOSTFILE, hf);
}
/* (lpFileName, dwDesiredAccess, dwShareMode, lpSecurityAttributes, dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile) */
static void sh_CreateFileA(void) { RET(create_file(ASTR(0), A32(1), A32(4)), 7); }
static void sh_CreateFileW(void) { char p[520]; guest_w_to_a(A32(0), p, sizeof p); RET(create_file(p, A32(1), A32(4)), 7); }

static hostfile *hfile(uint32_t h) { return (hostfile *)ds_handle_get(h, HK_HOSTFILE); }

static void sh_ReadFile(void) {               /* (h, buf, n, *read, ovl) */
    hostfile *f = hfile(A32(0));
    uint32_t n = A32(2), got = 0;
    if (f) { ssize_t r = read(f->fd, ds_hostptr(A32(1)), n); got = r > 0 ? (uint32_t)r : 0; }
    if (A32(3)) MEM32(A32(3)) = got;
    if (!f) { ds_set_last_error(6); RET(0, 5); }   /* ERROR_INVALID_HANDLE */
    RET(1, 5);
}
static void sh_WriteFile(void) {              /* (h, buf, n, *written, ovl): files, or host stdout/stderr */
    uint32_t h = A32(0), n = A32(2), w = 0;
    hostfile *f = hfile(h);
    if (f) { ssize_t r = write(f->fd, ds_hostptr(A32(1)), n); w = r > 0 ? (uint32_t)r : 0; }
    else {
        int id = (int)(intptr_t)ds_handle_get(h, HK_FILE);
        w = (uint32_t)fwrite(ds_hostptr(A32(1)), 1, n, id == -12 ? stderr : stdout);
    }
    if (A32(3)) MEM32(A32(3)) = w;
    RET(1, 5);
}
static void sh_CloseHandle(void) {
    hostfile *f = hfile(A32(0));
    if (f && f->fd >= 0) { close(f->fd); f->fd = -1; }
    RET(1, 1);
}
static void sh_GetFileType(void) {            /* FILE_TYPE_DISK for files, CHAR for std handles */
    RET(hfile(A32(0)) ? 1 : 2, 1);
}
static void sh_FlushFileBuffers(void) { RET(1, 1); }
static void sh_SetEndOfFile(void) {
    hostfile *f = hfile(A32(0));
    if (f) { off_t pos = lseek(f->fd, 0, SEEK_CUR); if (ftruncate(f->fd, pos) == 0) RET(1, 1); }
    RET(0, 1);
}
static void sh_SetFilePointer(void) {         /* (h, lDistanceToMove, *lpDistanceToMoveHigh, dwMoveMethod) */
    hostfile *f = hfile(A32(0));
    if (!f) { ds_set_last_error(6); RET(0xFFFFFFFFu, 4); }
    int64_t d = (int32_t)A32(1);
    if (A32(2)) d = (int64_t)(((uint64_t)MEM32(A32(2)) << 32) | A32(1));
    off_t r = lseek(f->fd, (off_t)d, (int)A32(3));   /* FILE_BEGIN/CURRENT/END == SEEK_SET/CUR/END */
    if (r < 0) { ds_set_last_error(131); RET(0xFFFFFFFFu, 4); }   /* ERROR_NEGATIVE_SEEK */
    if (A32(2)) MEM32(A32(2)) = (uint32_t)((uint64_t)r >> 32);
    ds_set_last_error(0);
    RET((uint32_t)r, 4);
}
static void sh_SetFilePointerEx(void) {       /* (h, LARGE_INTEGER by value = 2 dwords, *newpos, method) */
    hostfile *f = hfile(A32(0));
    if (!f) RET(0, 5);
    int64_t d = (int64_t)(((uint64_t)A32(2) << 32) | A32(1));
    off_t r = lseek(f->fd, (off_t)d, (int)A32(4));
    if (r < 0) RET(0, 5);
    if (A32(3)) { MEM32(A32(3)) = (uint32_t)r; MEM32(A32(3) + 4) = (uint32_t)((uint64_t)r >> 32); }
    RET(1, 5);
}
static void sh_GetFileSize(void) {            /* (h, *lpFileSizeHigh) */
    hostfile *f = hfile(A32(0)); struct stat st;
    if (!f || fstat(f->fd, &st)) RET(0xFFFFFFFFu, 2);
    if (A32(1)) MEM32(A32(1)) = (uint32_t)((uint64_t)st.st_size >> 32);
    RET((uint32_t)st.st_size, 2);
}
static void sh_GetFileSizeEx(void) {          /* (h, PLARGE_INTEGER) */
    hostfile *f = hfile(A32(0)); struct stat st;
    if (!f || fstat(f->fd, &st)) RET(0, 2);
    MEM32(A32(1)) = (uint32_t)st.st_size; MEM32(A32(1) + 4) = (uint32_t)((uint64_t)st.st_size >> 32);
    RET(1, 2);
}

/* ===================== attributes / directories */
static uint32_t file_attrs(const char *gpath) {
    char host[512]; struct stat st;
    if (ds_map_path(gpath, host, sizeof host) != 1 || stat(host, &st)) {
        ds_set_last_error(ERROR_FILE_NOT_FOUND);
        if (trace_on()) fprintf(stderr, "[file] GetFileAttributes(%s) -> missing\n", gpath);
        return 0xFFFFFFFFu;                    /* INVALID_FILE_ATTRIBUTES */
    }
    return S_ISDIR(st.st_mode) ? 0x10u : 0x80u;   /* DIRECTORY : NORMAL */
}
static void sh_GetFileAttributesA(void) { RET(file_attrs(ASTR(0)), 1); }
static void sh_GetFileAttributesW(void) { char p[520]; guest_w_to_a(A32(0), p, sizeof p); RET(file_attrs(p), 1); }
static uint32_t create_dir(const char *gpath) {
    char host[512];
    int r = ds_map_path(gpath, host, sizeof host);
    if (r < 0) { ds_set_last_error(ERROR_PATH_NOT_FOUND); return 0; }
    if (r == 1) { ds_set_last_error(ERROR_ALREADY_EXISTS); return 0; }
    if (mkdir(host, 0755)) { ds_set_last_error(ERROR_PATH_NOT_FOUND); return 0; }
    return 1;
}
static void sh_CreateDirectoryA(void) { RET(create_dir(ASTR(0)), 2); }   /* (path, lpSecurityAttributes) */
static void sh_CreateDirectoryW(void) { char p[520]; guest_w_to_a(A32(0), p, sizeof p); RET(create_dir(p), 2); }
static void sh_DeleteFileA(void) {
    char host[512];
    if (ds_map_path(ASTR(0), host, sizeof host) == 1 && unlink(host) == 0) RET(1, 1);
    ds_set_last_error(ERROR_FILE_NOT_FOUND); RET(0, 1);
}

static void sh_DeleteFileW(void) {
    char p[520], host[512]; guest_w_to_a(A32(0), p, sizeof p);
    if (ds_map_path(p, host, sizeof host) == 1 && unlink(host) == 0) RET(1, 1);
    ds_set_last_error(ERROR_FILE_NOT_FOUND); RET(0, 1);
}
static void sh_RemoveDirectoryW(void) {
    char p[520], host[512]; guest_w_to_a(A32(0), p, sizeof p);
    if (ds_map_path(p, host, sizeof host) == 1 && rmdir(host) == 0) RET(1, 1);
    ds_set_last_error(ERROR_PATH_NOT_FOUND); RET(0, 1);
}
static void sh_MoveFileExA(void) {            /* (from, to, flags) */
    char a[512], b[512];
    if (ds_map_path(ASTR(0), a, sizeof a) != 1 || ds_map_path(ASTR(1), b, sizeof b) < 0) { ds_set_last_error(ERROR_FILE_NOT_FOUND); RET(0, 3); }
    if (!(A32(2) & 1) && access(b, F_OK) == 0) { ds_set_last_error(ERROR_ALREADY_EXISTS); RET(0, 3); }  /* no MOVEFILE_REPLACE_EXISTING */
    RET(rename(a, b) == 0, 3);
}
static void sh_GetFullPathNameW(void) {       /* (name, nBufferLength in WCHARs, buf, *filepart) */
    char nm[520], full[600]; guest_w_to_a(A32(0), nm, sizeof nm);
    if (nm[0] && nm[1] == ':') snprintf(full, sizeof full, "%s", nm);
    else snprintf(full, sizeof full, "%s\\%s", ds_cwd(), nm);
    uint32_t n = (uint32_t)strlen(full), cap = A32(1), buf = A32(2);
    if (!buf || cap <= n) RET(n + 1, 4);
    for (uint32_t i = 0; i <= n; i++) MEM16(buf + 2 * i) = (uint8_t)full[i];
    if (A32(3)) { char *sl = strrchr(full, '\\'); MEM32(A32(3)) = buf + 2 * (uint32_t)(sl ? sl - full + 1 : 0); }
    RET(n, 4);
}

/* ===================== FindFirstFile / FindNextFile over a host directory */
typedef struct { DIR *d; char dir[512]; char pat[256]; int wide; } finder;
/* WIN32_FIND_DATA{A,W}: attrs +0, 3 FILETIMEs +4..+27, size hi +28 / lo +32, cFileName +44 */
static int find_next(finder *f, uint32_t out) {
    struct dirent *de;
    while (f->d && (de = readdir(f->d))) {
        if (fnmatch(f->pat, de->d_name, FNM_CASEFOLD)) continue;
        char hp[800]; struct stat st;
        snprintf(hp, sizeof hp, "%s/%s", f->dir, de->d_name);
        if (stat(hp, &st)) continue;
        memset(ds_hostptr(out), 0, f->wide ? 592 : 320);
        MEM32(out) = S_ISDIR(st.st_mode) ? 0x10u : 0x80u;
        uint64_t ft = ((uint64_t)st.st_mtime + 11644473600ull) * 10000000ull;
        for (int k = 0; k < 3; k++) { MEM32(out + 4 + 8 * k) = (uint32_t)ft; MEM32(out + 8 + 8 * k) = (uint32_t)(ft >> 32); }
        MEM32(out + 28) = (uint32_t)((uint64_t)st.st_size >> 32);
        MEM32(out + 32) = (uint32_t)st.st_size;
        size_t n = strlen(de->d_name); if (n > 259) n = 259;
        for (size_t i = 0; i <= n; i++) {
            char c = i < n ? de->d_name[i] : 0;
            if (f->wide) MEM16(out + 44 + 2 * (uint32_t)i) = (uint8_t)c; else MEM8(out + 44 + (uint32_t)i) = (uint8_t)c;
        }
        return 1;
    }
    return 0;
}
static uint32_t find_first(const char *gpat, uint32_t out, int wide) {
    char g[520]; snprintf(g, sizeof g, "%s", gpat);
    char *sl = strrchr(g, '\\'); char *fs = strrchr(g, '/'); if (fs > sl) sl = fs;
    const char *pat = sl ? sl + 1 : g;
    char dirg[520]; if (sl) { *sl = 0; snprintf(dirg, sizeof dirg, "%s", g); } else snprintf(dirg, sizeof dirg, ".");
    finder *f = (finder *)calloc(1, sizeof *f);
    f->wide = wide;
    snprintf(f->pat, sizeof f->pat, "%s", !strcmp(pat, "*.*") ? "*" : pat);
    if (ds_map_path(dirg, f->dir, sizeof f->dir) == 1) f->d = opendir(f->dir);
    if (trace_on()) fprintf(stderr, "[file] FindFirstFile(%s) -> %s/%s%s\n", gpat, f->dir, f->pat, f->d ? "" : "  NO DIR");
    if (!find_next(f, out)) {
        if (f->d) closedir(f->d);
        free(f);
        ds_set_last_error(ERROR_FILE_NOT_FOUND);
        if (trace_on()) fprintf(stderr, "[file]   -> no match\n");
        return INVALID_HANDLE;
    }
    uint32_t h = ds_handle_new(HK_FIND, f);
    if (trace_on()) fprintf(stderr, "[file]   -> handle %u (%s), first \"%s\"\n", h, wide ? "W" : "A",
                            wide ? "(wide)" : (const char *)ds_hostptr(out + 44));
    return h;
}
static void sh_FindFirstFileA(void) { RET(find_first(ASTR(0), A32(1), 0), 2); }
static void sh_FindFirstFileW(void) { char p[520]; guest_w_to_a(A32(0), p, sizeof p); RET(find_first(p, A32(1), 1), 2); }
static void sh_FindNextFileW(void)  { finder *f = ds_handle_get(A32(0), HK_FIND); if (f && find_next(f, A32(1))) RET(1, 2); ds_set_last_error(18); RET(0, 2); }
static void sh_FindNextFileA(void)  { finder *f = ds_handle_get(A32(0), HK_FIND); if (f && find_next(f, A32(1))) RET(1, 2); ds_set_last_error(18); RET(0, 2); }
static void sh_FindClose(void) {
    finder *f = ds_handle_get(A32(0), HK_FIND);
    if (f && f->d) { closedir(f->d); f->d = NULL; }
    RET(f != NULL, 1);
}

const ds_shim ds_file[] = {
    { "CreateFileA", sh_CreateFileA }, { "CreateFileW", sh_CreateFileW },
    { "ReadFile", sh_ReadFile }, { "WriteFile", sh_WriteFile }, { "CloseHandle", sh_CloseHandle },
    { "GetFileType", sh_GetFileType }, { "FlushFileBuffers", sh_FlushFileBuffers },
    { "SetEndOfFile", sh_SetEndOfFile },
    { "SetFilePointer", sh_SetFilePointer }, { "SetFilePointerEx", sh_SetFilePointerEx },
    { "GetFileSize", sh_GetFileSize }, { "GetFileSizeEx", sh_GetFileSizeEx },
    { "GetFileAttributesA", sh_GetFileAttributesA }, { "GetFileAttributesW", sh_GetFileAttributesW },
    { "CreateDirectoryA", sh_CreateDirectoryA }, { "CreateDirectoryW", sh_CreateDirectoryW },
    { "DeleteFileA", sh_DeleteFileA }, { "DeleteFileW", sh_DeleteFileW },
    { "RemoveDirectoryW", sh_RemoveDirectoryW }, { "MoveFileExA", sh_MoveFileExA },
    { "GetFullPathNameW", sh_GetFullPathNameW },
    { "FindFirstFileA", sh_FindFirstFileA }, { "FindFirstFileW", sh_FindFirstFileW }, { "FindNextFileW", sh_FindNextFileW },
    { "FindNextFileA", sh_FindNextFileA }, { "FindClose", sh_FindClose },
    { 0, 0 }
};
