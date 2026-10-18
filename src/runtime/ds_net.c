/*
 * ds_net.c - Winsock for a single-player handheld: an OFFLINE stack.
 * WSAStartup succeeds (the API exists), but no socket can be created and no
 * name resolves, so every online feature takes its "no network" path.
 * The exe imports WSOCK32 by ordinal, so each entry is registered under both
 * its name and "WSOCK32.dll#N". Winsock is WSAAPI = stdcall: RET pops the args.
 */
#include <stdio.h>
#include <string.h>
#include "ds_hle.h"

#define INVALID_SOCKET   0xFFFFFFFFu
#define SOCKET_ERROR     0xFFFFFFFFu
#define WSAENETDOWN      10050u
#define WSAHOST_NOT_FOUND 11001u

static uint32_t g_wsa_error;
static void fail(uint32_t err) { g_wsa_error = err; }

static void sh_WSAStartup(void) {             /* (wVersionRequested, LPWSADATA) */
    uint32_t ver = A32(0) & 0xFFFF, p = A32(1);
    if (p) {                                  /* WSADATA (x86): wVersion, wHighVersion, szDescription[257], szSystemStatus[129], ... */
        memset(ds_hostptr(p), 0, 400);
        MEM16(p) = (uint16_t)ver; MEM16(p + 2) = 0x0202;
        strcpy((char *)ds_hostptr(p + 4), "ds offline winsock");
        strcpy((char *)ds_hostptr(p + 4 + 257), "no network");
    }
    RET(0, 2);
}
static void sh_WSACleanup(void)      { RET(0, 0); }
static void sh_WSAGetLastError(void) { RET(g_wsa_error, 0); }
static void sh_WSASetLastError(void) { g_wsa_error = A32(0); RETV(1); }

static void sh_socket(void)      { fail(WSAENETDOWN); RET(INVALID_SOCKET, 3); }   /* (af, type, protocol) */
static void sh_accept(void)      { fail(WSAENETDOWN); RET(INVALID_SOCKET, 3); }
static void sh_closesocket(void) { RET(0, 1); }
static void sh_connect(void)     { fail(WSAENETDOWN); RET(SOCKET_ERROR, 3); }
static void sh_bind(void)        { fail(WSAENETDOWN); RET(SOCKET_ERROR, 3); }
static void sh_listen(void)      { fail(WSAENETDOWN); RET(SOCKET_ERROR, 2); }
static void sh_send(void)        { fail(WSAENETDOWN); RET(SOCKET_ERROR, 4); }
static void sh_recv(void)        { fail(WSAENETDOWN); RET(SOCKET_ERROR, 4); }
static void sh_sendto(void)      { fail(WSAENETDOWN); RET(SOCKET_ERROR, 6); }
static void sh_recvfrom(void)    { fail(WSAENETDOWN); RET(SOCKET_ERROR, 6); }
static void sh_select(void)      { fail(WSAENETDOWN); RET(SOCKET_ERROR, 5); }
static void sh_ioctlsocket(void) { fail(WSAENETDOWN); RET(SOCKET_ERROR, 3); }
static void sh_setsockopt(void)  { fail(WSAENETDOWN); RET(SOCKET_ERROR, 5); }
static void sh_getsockopt(void)  { fail(WSAENETDOWN); RET(SOCKET_ERROR, 5); }
static void sh_shutdown(void)    { fail(WSAENETDOWN); RET(SOCKET_ERROR, 2); }
static void sh_getpeername(void) { fail(WSAENETDOWN); RET(SOCKET_ERROR, 3); }
static void sh_getsockname(void) { fail(WSAENETDOWN); RET(SOCKET_ERROR, 3); }
static void sh_WSAFDIsSet(void)  { RET(0, 2); }                                      /* (s, fd_set*) */

static void sh_gethostname(void) {            /* (name, namelen) */
    const char *h = "r36s"; uint32_t n = (uint32_t)strlen(h);
    if (A32(1) <= n) { fail(10014u); RET(SOCKET_ERROR, 2); }    /* WSAEFAULT */
    memcpy(ds_hostptr(A32(0)), h, n + 1);
    RET(0, 2);
}
static void sh_gethostbyname(void) { fail(WSAHOST_NOT_FOUND); RET(0, 1); }
static void sh_gethostbyaddr(void) { fail(WSAHOST_NOT_FOUND); RET(0, 3); }           /* (addr, len, type) */
static void sh_getaddrinfo(void)   { if (A32(3)) MEM32(A32(3)) = 0; RET(WSAHOST_NOT_FOUND, 4); }  /* EAI_NONAME */
static void sh_freeaddrinfo(void)  { RETV(1); }

static void sh_htons(void) { uint32_t v = A32(0) & 0xFFFF; RET(((v & 0xFF) << 8) | (v >> 8), 1); }
static void sh_htonl(void) { RET(__builtin_bswap32(A32(0)), 1); }
static uint32_t g_ntoa_buf;
static void sh_inet_ntoa(void) {              /* (struct in_addr by value) -> static buffer */
    if (!g_ntoa_buf) g_ntoa_buf = ds_arena_alloc(16, 4);
    uint32_t a = A32(0);
    snprintf((char *)ds_hostptr(g_ntoa_buf), 16, "%u.%u.%u.%u", a & 0xFF, (a >> 8) & 0xFF, (a >> 16) & 0xFF, a >> 24);
    RET(g_ntoa_buf, 1);
}

#define BOTH(name, ord, fn) { name, fn }, { "WSOCK32.dll#" #ord, fn }
const ds_shim ds_net[] = {
    BOTH("accept", 1, sh_accept), BOTH("bind", 2, sh_bind), BOTH("closesocket", 3, sh_closesocket),
    BOTH("connect", 4, sh_connect), BOTH("getpeername", 5, sh_getpeername),
    BOTH("getsockname", 6, sh_getsockname), BOTH("getsockopt", 7, sh_getsockopt),
    BOTH("htonl", 8, sh_htonl), BOTH("htons", 9, sh_htons), BOTH("inet_ntoa", 11, sh_inet_ntoa),
    BOTH("ioctlsocket", 12, sh_ioctlsocket), BOTH("listen", 13, sh_listen),
    BOTH("ntohs", 15, sh_htons), BOTH("recv", 16, sh_recv), BOTH("recvfrom", 17, sh_recvfrom),
    BOTH("select", 18, sh_select), BOTH("send", 19, sh_send), BOTH("sendto", 20, sh_sendto),
    BOTH("setsockopt", 21, sh_setsockopt), BOTH("shutdown", 22, sh_shutdown),
    BOTH("socket", 23, sh_socket), BOTH("gethostbyaddr", 51, sh_gethostbyaddr),
    BOTH("gethostbyname", 52, sh_gethostbyname), BOTH("gethostname", 57, sh_gethostname),
    BOTH("WSAGetLastError", 111, sh_WSAGetLastError), BOTH("WSASetLastError", 112, sh_WSASetLastError),
    BOTH("WSAStartup", 115, sh_WSAStartup), BOTH("WSACleanup", 116, sh_WSACleanup),
    BOTH("__WSAFDIsSet", 151, sh_WSAFDIsSet),
    { "getaddrinfo", sh_getaddrinfo }, { "freeaddrinfo", sh_freeaddrinfo },
    { 0, 0 }
};
