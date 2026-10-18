/*
 * ds_user32.c - USER32 + GDI32: the window layer the exe needs before its first
 * frame. First pass is a HEADLESS fake window: one fixed 640x480 client rect
 * (the R36S screen) at (0,0) with no borders, a single primary monitor/display
 * mode, and an empty message queue. The guest WndProc is driven through
 * hle_call_guest for the messages a real CreateWindowEx/ShowWindow would send
 * (WM_NCCREATE, WM_CREATE, WM_SIZE, WM_MOVE, WM_SHOWWINDOW, WM_ACTIVATE...).
 * An SDL2 window (KMSDRM on the device) replaces the fake later; the guest sees
 * the same answers.
 *
 * Every export here is WINAPI = stdcall: RET(value, argc) pops ret + argc*4.
 * argc is the Win32 prototype's DWORD count -- note MonitorFromPoint takes its
 * POINT by value (3 dwords, not 2).
 */
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "ds_hle.h"

#define DS_SCREEN_BPP 32
#define DS_SCREEN_HZ 60

/* handle kinds (above ds_kernel32.c's 1..6) */
enum { HK_WINDOW = 0x20, HK_CURSOR, HK_ICON, HK_MONITOR, HK_WINSTA, HK_DC, HK_GDIOBJ };

/* window messages */
#define WM_CREATE        0x0001u
#define WM_MOVE          0x0003u
#define WM_SIZE          0x0005u
#define WM_ACTIVATE      0x0006u
#define WM_SETFOCUS      0x0007u
#define WM_SHOWWINDOW    0x0018u
#define WM_ACTIVATEAPP   0x001Cu
#define WM_WINDOWPOSCHANGED 0x0047u
#define WM_NCCREATE      0x0081u
#define WM_CLOSE         0x0010u

#define GWL_WNDPROC   (-4)
#define GWL_HINSTANCE (-6)
#define GWL_ID        (-12)
#define GWL_STYLE     (-16)
#define GWL_EXSTYLE   (-20)
#define GWL_USERDATA  (-21)
#define WS_VISIBLE    0x10000000u

#define MAKELONG16(lo, hi) (((uint32_t)(uint16_t)(lo)) | ((uint32_t)(uint16_t)(hi) << 16))

/* ===================== window classes + windows */
typedef struct { char name[64]; uint32_t atom, wndproc, style, cb_wnd_extra, hinst, cursor; } wclass;
#define MAX_CLASSES 16
static wclass g_class[MAX_CLASSES];
static int g_class_n;

typedef struct {
    uint32_t hwnd, wndproc, style, exstyle, userdata, hinst, id, parent;
    int visible, shown_once;
    uint8_t extra[64];          /* cbWndExtra bytes (GetWindowLong with idx >= 0) */
} wnd;
#define MAX_WND 16
static wnd g_wnd[MAX_WND];
static int g_wnd_n;
static uint32_t g_desktop, g_focus, g_capture;

static wclass *class_find(uint32_t name_or_atom) {
    for (int i = 0; i < g_class_n; i++) {
        if (name_or_atom <= 0xFFFF) { if (g_class[i].atom == name_or_atom) return &g_class[i]; }
        else if (!strcasecmp(g_class[i].name, (const char *)ds_hostptr(name_or_atom))) return &g_class[i];
    }
    return NULL;
}
static wnd *wnd_get(uint32_t hwnd) {
    for (int i = 0; i < g_wnd_n; i++) if (g_wnd[i].hwnd == hwnd) return &g_wnd[i];
    return NULL;
}
/* Send a message to a guest WndProc (stdcall LRESULT(HWND,UINT,WPARAM,LPARAM)). */
static uint32_t wnd_send(wnd *w, uint32_t msg, uint32_t wp, uint32_t lp) {
    if (!w || !w->wndproc) return 0;
    uint32_t a[4] = { w->hwnd, msg, wp, lp };
    if (ds_trace) fprintf(stderr, "[user32] -> WndProc %08X(hwnd %u, msg 0x%04X, %08X, %08X)\n",
                          w->wndproc, w->hwnd, msg, wp, lp);
    return hle_call_guest(w->wndproc, 4, a);
}
static uint32_t lparam_size(void) { return MAKELONG16(DS_SCREEN_W, DS_SCREEN_H); }

static void sh_RegisterClassA(void) {        /* (const WNDCLASSA*) -> ATOM */
    uint32_t p = A32(0);
    if (g_class_n >= MAX_CLASSES) RET(0, 1);
    wclass *c = &g_class[g_class_n];
    memset(c, 0, sizeof *c);
    c->style = MEM32(p + 0); c->wndproc = MEM32(p + 4); c->cb_wnd_extra = MEM32(p + 12);
    c->hinst = MEM32(p + 16); c->cursor = MEM32(p + 24);
    uint32_t nm = MEM32(p + 36);
    if (nm > 0xFFFF) snprintf(c->name, sizeof c->name, "%s", (const char *)ds_hostptr(nm));
    c->atom = 0xC000u + (uint32_t)g_class_n;
    g_class_n++;
    fprintf(stderr, "[user32] RegisterClassA(\"%s\") wndproc 0x%08X -> atom 0x%04X\n", c->name, c->wndproc, c->atom);
    RET(c->atom, 1);
}

static void sh_CreateWindowExA(void) {
    /* (exStyle, lpClassName, lpWindowName, dwStyle, x, y, w, h, hParent, hMenu, hInstance, lpParam) */
    uint32_t exs = A32(0), cls = A32(1), title = A32(2), style = A32(3);
    uint32_t parent = A32(8), menu = A32(9), hinst = A32(10), param = A32(11);
    wclass *c = class_find(cls);
    fprintf(stderr, "[user32] CreateWindowExA(class \"%s\", \"%s\", style %08X, %dx%d) -> headless %dx%d\n",
            c ? c->name : "?", title ? (const char *)ds_hostptr(title) : "", style,
            (int)A32(6), (int)A32(7), DS_SCREEN_W, DS_SCREEN_H);
    if (!c || g_wnd_n >= MAX_WND) RET(0, 12);
    wnd *w = &g_wnd[g_wnd_n++];
    memset(w, 0, sizeof *w);
    w->hwnd = ds_handle_new(HK_WINDOW, 0);
    w->wndproc = c->wndproc; w->style = style & ~WS_VISIBLE; w->exstyle = exs;
    w->hinst = hinst; w->id = menu; w->parent = parent;
    uint32_t hwnd = w->hwnd;

    /* CREATESTRUCTA, read back by the guest's WM_NCCREATE/WM_CREATE handlers */
    uint32_t cs = ds_arena_alloc(48, 4);
    MEM32(cs + 0) = param;  MEM32(cs + 4) = hinst; MEM32(cs + 8) = menu; MEM32(cs + 12) = parent;
    MEM32(cs + 16) = DS_SCREEN_H; MEM32(cs + 20) = DS_SCREEN_W; MEM32(cs + 24) = 0; MEM32(cs + 28) = 0;
    MEM32(cs + 32) = style; MEM32(cs + 36) = title; MEM32(cs + 40) = cls; MEM32(cs + 44) = exs;

    if (!wnd_send(w, WM_NCCREATE, 0, cs)) {
        fprintf(stderr, "[user32] WM_NCCREATE returned FALSE: window creation aborted\n");
        RET(0, 12);
    }
    w = wnd_get(hwnd);
    if ((int32_t)wnd_send(w, WM_CREATE, 0, cs) == -1) {
        fprintf(stderr, "[user32] WM_CREATE returned -1: window creation aborted\n");
        RET(0, 12);
    }
    w = wnd_get(hwnd);
    wnd_send(w, WM_SIZE, 0 /*SIZE_RESTORED*/, lparam_size());
    wnd_send(w, WM_MOVE, 0, 0);
    if (style & WS_VISIBLE) {
        w->visible = 1; w->style |= WS_VISIBLE;
        wnd_send(w, WM_SHOWWINDOW, 1, 0);
    }
    RET(hwnd, 12);
}

static void sh_ShowWindow(void) {            /* (hwnd, nCmdShow) -> was visible */
    wnd *w = wnd_get(A32(0));
    if (!w) RET(0, 2);
    uint32_t hwnd = w->hwnd, cmd = A32(1);
    int was = w->visible, show = cmd != 0 /*SW_HIDE*/;
    w->visible = show;
    if (show) w->style |= WS_VISIBLE; else w->style &= ~WS_VISIBLE;
    if (show != was) wnd_send(w, WM_SHOWWINDOW, (uint32_t)show, 0);
    if (show && !(w = wnd_get(hwnd))->shown_once) {
        w->shown_once = 1;
        g_focus = hwnd;
        wnd_send(w, WM_ACTIVATEAPP, 1, 0x1001);
        wnd_send(wnd_get(hwnd), WM_ACTIVATE, 1 /*WA_ACTIVE*/, 0);
        wnd_send(wnd_get(hwnd), WM_SETFOCUS, 0, 0);
    }
    RET(was, 2);
}

static uint32_t *wnd_long_slot(wnd *w, int32_t idx) {
    switch (idx) {
    case GWL_WNDPROC:   return &w->wndproc;
    case GWL_HINSTANCE: return &w->hinst;
    case GWL_ID:        return &w->id;
    case GWL_STYLE:     return &w->style;
    case GWL_EXSTYLE:   return &w->exstyle;
    case GWL_USERDATA:  return &w->userdata;
    default:
        if (idx >= 0 && idx + 4 <= (int32_t)sizeof w->extra) return (uint32_t *)(w->extra + idx);
        return NULL;
    }
}
static void sh_GetWindowLongA(void) {        /* (hwnd, nIndex) */
    wnd *w = wnd_get(A32(0)); uint32_t *s = w ? wnd_long_slot(w, (int32_t)A32(1)) : NULL;
    RET(s ? *s : 0, 2);
}
static void sh_SetWindowLongA(void) {        /* (hwnd, nIndex, dwNewLong) -> previous */
    wnd *w = wnd_get(A32(0)); uint32_t *s = w ? wnd_long_slot(w, (int32_t)A32(1)) : NULL;
    if (!s) RET(0, 3);
    uint32_t old = *s; *s = A32(2);
    if ((int32_t)A32(1) == GWL_STYLE) w->visible = (A32(2) & WS_VISIBLE) != 0;
    RET(old, 3);
}
static void sh_SendMessageA(void) {          /* (hwnd, msg, wParam, lParam) */
    uint32_t hwnd = A32(0), msg = A32(1), wp = A32(2), lp = A32(3);
    RET(wnd_send(wnd_get(hwnd), msg, wp, lp), 4);
}
static void sh_DefWindowProcA(void) {        /* (hwnd, msg, wParam, lParam) */
    uint32_t msg = A32(1);
    RET(msg == WM_NCCREATE ? 1 : 0, 4);
}

/* ===================== geometry: everything is the one 640x480 client rect at (0,0) */
static void put_rect(uint32_t p, int32_t l, int32_t t, int32_t r, int32_t b) {
    MEM32(p) = (uint32_t)l; MEM32(p + 4) = (uint32_t)t; MEM32(p + 8) = (uint32_t)r; MEM32(p + 12) = (uint32_t)b;
}
static void sh_GetClientRect(void) { if (A32(1)) put_rect(A32(1), 0, 0, DS_SCREEN_W, DS_SCREEN_H); RET(1, 2); }
static void sh_GetWindowRect(void) { if (A32(1)) put_rect(A32(1), 0, 0, DS_SCREEN_W, DS_SCREEN_H); RET(1, 2); }
static void sh_AdjustWindowRect(void) { RET(1, 3); }       /* (rect, style, bMenu): borderless, unchanged */
static void sh_MoveWindow(void) {                          /* (hwnd, x, y, w, h, repaint) */
    wnd *w = wnd_get(A32(0));
    if (w) wnd_send(w, WM_SIZE, 0, lparam_size());
    RET(1, 6);
}
static void sh_SetWindowPos(void) { RET(1, 7); }           /* (hwnd, after, x, y, cx, cy, flags) */
static void sh_MapWindowPoints(void) { RET(0, 4); }        /* (from, to, pts, n): all at (0,0) */
static void sh_InvalidateRect(void) { RET(1, 3); }
static void sh_IsWindowVisible(void) { wnd *w = wnd_get(A32(0)); RET(w ? w->visible : 0, 1); }
static void sh_IsIconic(void) { RET(0, 1); }
static void sh_GetForegroundWindow(void) { RET(g_focus ? g_focus : (g_wnd_n ? g_wnd[0].hwnd : 0), 0); }
static void sh_GetDesktopWindow(void) {
    if (!g_desktop) g_desktop = ds_handle_new(HK_WINDOW, 0);
    RET(g_desktop, 0);
}
static void sh_SetCapture(void)   { uint32_t old = g_capture; g_capture = A32(0); RET(old, 1); }
static void sh_ReleaseCapture(void) { g_capture = 0; RET(1, 0); }

/* ===================== display devices, modes, monitors */
static uint32_t g_monitor;
static uint32_t monitor(void) { if (!g_monitor) g_monitor = ds_handle_new(HK_MONITOR, 0); return g_monitor; }

static void put_str(uint32_t p, uint32_t cap, const char *s) {
    uint32_t n = (uint32_t)strlen(s); if (n >= cap) n = cap - 1;
    memcpy(ds_hostptr(p), s, n); MEM8(p + n) = 0;
}
static void sh_EnumDisplayDevicesA(void) {   /* (lpDevice, iDevNum, DISPLAY_DEVICEA*, dwFlags) */
    uint32_t dev = A32(0), idx = A32(1), p = A32(2);
    if (idx != 0 || !p) RET(0, 4);
    uint32_t cb = MEM32(p);
    if (cb < 168 || cb > 424) cb = 424;          /* DISPLAY_DEVICEA is 424 bytes */
    memset(ds_hostptr(p + 4), 0, cb - 4);
    if (!dev) {                              /* adapter 0 */
        put_str(p + 4, 32, "\\\\.\\DISPLAY1");
        put_str(p + 36, 128, "R36S Display");
        MEM32(p + 164) = 0x1 | 0x4;          /* ATTACHED_TO_DESKTOP | PRIMARY_DEVICE */
    } else {                                 /* monitor 0 on that adapter */
        put_str(p + 4, 32, "\\\\.\\DISPLAY1\\Monitor0");
        put_str(p + 36, 128, "Generic Monitor");
        MEM32(p + 164) = 0x1 | 0x2;          /* DISPLAY_DEVICE_ACTIVE | ATTACHED */
    }
    if (cb >= 168 + 128) put_str(p + 168, 128, "MONITOR\\R36S");
    RET(1, 4);
}
static void put_devmode(uint32_t p) {        /* DEVMODEA (156 bytes): only the fields games read */
    uint16_t size = MEM16(p + 36), extra = MEM16(p + 38);
    if (size < 124 || size > 220) size = 156;
    memset(ds_hostptr(p), 0, size);
    put_str(p, 32, "\\\\.\\DISPLAY1");
    MEM16(p + 32) = 0x0401;                  /* dmSpecVersion = DM_SPECVERSION */
    MEM16(p + 36) = size;
    MEM16(p + 38) = extra;
    MEM32(p + 40) = 0x00040000u | 0x00080000u | 0x00100000u | 0x00400000u; /* BITSPERPEL|PELSWIDTH|PELSHEIGHT|DISPLAYFREQUENCY */
    MEM32(p + 104) = DS_SCREEN_BPP;
    MEM32(p + 108) = DS_SCREEN_W;
    MEM32(p + 112) = DS_SCREEN_H;
    MEM32(p + 116) = 0;
    MEM32(p + 120) = DS_SCREEN_HZ;
}
static void sh_EnumDisplaySettingsA(void) {  /* (lpszDeviceName, iModeNum, DEVMODEA*) */
    uint32_t mode = A32(1), p = A32(2);
    /* ENUM_CURRENT_SETTINGS (-1), ENUM_REGISTRY_SETTINGS (-2), or mode 0: the one mode */
    if (!p || !(mode == 0 || mode == 0xFFFFFFFFu || mode == 0xFFFFFFFEu)) RET(0, 3);
    put_devmode(p);
    RET(1, 3);
}
static void sh_ChangeDisplaySettingsExA(void) { /* (name, DEVMODEA*, hwnd, flags, lParam) */
    uint32_t p = A32(1);
    if (p) fprintf(stderr, "[user32] ChangeDisplaySettingsExA(%ux%u) -> ok (fixed %dx%d)\n",
                   MEM32(p + 108), MEM32(p + 112), DS_SCREEN_W, DS_SCREEN_H);
    RET(0, 5);                               /* DISP_CHANGE_SUCCESSFUL */
}
static void sh_MonitorFromPoint(void)  { RET(monitor(), 3); }   /* (POINT by value = 2 dwords, flags) */
static void sh_MonitorFromWindow(void) { RET(monitor(), 2); }
static void sh_GetMonitorInfoA(void) {       /* (hMonitor, MONITORINFO[EXA]*) */
    uint32_t p = A32(1), cb = MEM32(p);
    put_rect(p + 4, 0, 0, DS_SCREEN_W, DS_SCREEN_H);    /* rcMonitor */
    put_rect(p + 20, 0, 0, DS_SCREEN_W, DS_SCREEN_H);   /* rcWork */
    MEM32(p + 36) = 1;                                  /* MONITORINFOF_PRIMARY */
    if (cb >= 72) put_str(p + 40, 32, "\\\\.\\DISPLAY1");
    RET(1, 2);
}
static void sh_SystemParametersInfoA(void) { /* (uiAction, uiParam, pvParam, fWinIni) */
    uint32_t act = A32(0), pv = A32(2);
    switch (act) {
    case 0x0030: if (pv) put_rect(pv, 0, 0, DS_SCREEN_W, DS_SCREEN_H); break;   /* SPI_GETWORKAREA */
    case 0x0032: if (pv) memset(ds_hostptr(pv + 4), 0, 20); break;            /* SPI_GETFILTERKEYS */
    case 0x0034: case 0x003A: if (pv) MEM32(pv + 4) = 0; break;               /* SPI_GET{TOGGLE,STICKY}KEYS */
    default: if (ds_trace) fprintf(stderr, "[user32] SystemParametersInfoA(0x%04X) ignored\n", act);
    }
    RET(1, 4);
}

/* ===================== window station (the CRT checks it before MessageBox) */
static void sh_GetProcessWindowStation(void) { RET(ds_handle_new(HK_WINSTA, 0), 0); }
static void sh_GetUserObjectInformationW(void) { /* (h, nIndex, pvInfo, nLength, lpnLengthNeeded) */
    uint32_t idx = A32(1), pv = A32(2), len = A32(3), need = A32(4);
    if (idx == 1 /*UOI_FLAGS*/) {
        if (need) MEM32(need) = 12;
        if (!pv || len < 12) RET(0, 5);
        MEM32(pv) = 0; MEM32(pv + 4) = 0; MEM32(pv + 8) = 1;   /* USEROBJECTFLAGS.dwFlags = WSF_VISIBLE */
        RET(1, 5);
    }
    if (need) MEM32(need) = 0;
    RET(0, 5);
}

/* ===================== cursor, icons, keyboard */
static int32_t g_cur_x = -1, g_cur_y = -1;   /* screen centre until SetCursorPos */
static uint32_t g_cursor;
static void sh_LoadCursorA(void) { RET(ds_handle_new(HK_CURSOR, 0), 2); }
static void sh_LoadIconA(void)   { RET(ds_handle_new(HK_ICON, 0), 2); }
static void sh_SetCursor(void)   { uint32_t old = g_cursor; g_cursor = A32(0); RET(old, 1); }
static void sh_ClipCursor(void)  { RET(1, 1); }
static void sh_GetCursorPos(void) {
    uint32_t p = A32(0);
    if (g_cur_x < 0) { g_cur_x = DS_SCREEN_W / 2; g_cur_y = DS_SCREEN_H / 2; }
    if (p) { MEM32(p) = (uint32_t)g_cur_x; MEM32(p + 4) = (uint32_t)g_cur_y; }
    RET(1, 1);
}
static void sh_SetCursorPos(void) { g_cur_x = (int32_t)A32(0); g_cur_y = (int32_t)A32(1); RET(1, 2); }

/* US layout, scan code set 1: VK <-> scan code for the keys a game binds. */
static const struct { uint8_t vk, sc; } k_vsc[] = {
    {0x08,0x0E},{0x09,0x0F},{0x0D,0x1C},{0x10,0x2A},{0x11,0x1D},{0x12,0x38},{0x14,0x3A},{0x1B,0x01},
    {0x20,0x39},{0x25,0x4B},{0x26,0x48},{0x27,0x4D},{0x28,0x50},{0x2D,0x52},{0x2E,0x53},
    {0x24,0x47},{0x23,0x4F},{0x21,0x49},{0x22,0x51},
    {'1',0x02},{'2',0x03},{'3',0x04},{'4',0x05},{'5',0x06},{'6',0x07},{'7',0x08},{'8',0x09},{'9',0x0A},{'0',0x0B},
    {'Q',0x10},{'W',0x11},{'E',0x12},{'R',0x13},{'T',0x14},{'Y',0x15},{'U',0x16},{'I',0x17},{'O',0x18},{'P',0x19},
    {'A',0x1E},{'S',0x1F},{'D',0x20},{'F',0x21},{'G',0x22},{'H',0x23},{'J',0x24},{'K',0x25},{'L',0x26},
    {'Z',0x2C},{'X',0x2D},{'C',0x2E},{'V',0x2F},{'B',0x30},{'N',0x31},{'M',0x32},
    {0x70,0x3B},{0x71,0x3C},{0x72,0x3D},{0x73,0x3E},{0x74,0x3F},{0x75,0x40},{0x76,0x41},{0x77,0x42},
    {0x78,0x43},{0x79,0x44},{0x7A,0x57},{0x7B,0x58},
    {0xBA,0x27},{0xBB,0x0D},{0xBC,0x33},{0xBD,0x0C},{0xBE,0x34},{0xBF,0x35},{0xC0,0x29},
    {0xDB,0x1A},{0xDC,0x2B},{0xDD,0x1B},{0xDE,0x28},
};
static void sh_MapVirtualKeyA(void) {        /* (uCode, uMapType) */
    uint32_t code = A32(0), type = A32(1);
    uint32_t r = 0;
    for (size_t i = 0; i < sizeof k_vsc / sizeof k_vsc[0]; i++) {
        if (type == 0 /*VK_TO_VSC*/ && k_vsc[i].vk == code) { r = k_vsc[i].sc; break; }
        if (type == 1 /*VSC_TO_VK*/ && k_vsc[i].sc == code) { r = k_vsc[i].vk; break; }
    }
    if (type == 2 /*VK_TO_CHAR*/) {
        if ((code >= '0' && code <= '9') || (code >= 'A' && code <= 'Z') || code == ' ') r = code;
        else if (code == 0x0D) r = '\r'; else if (code == 0x08) r = '\b'; else if (code == 0x09) r = '\t';
    }
    RET(r, 2);
}

/* ===================== message queue
 * Empty, except for a scripted input: DS_INPUT="F click X Y; F key VK; F hold VK N; ..."
 * posts, once frame F has been swapped, a mouse move + left click at (X, Y)
 * or a key press of virtual key VK -- how a headless run gets past menus. */
#define WM_KEYDOWN     0x0100u
#define WM_KEYUP       0x0101u
#define WM_CHAR        0x0102u
#define WM_MOUSEMOVE   0x0200u
#define WM_LBUTTONDOWN 0x0201u
#define WM_LBUTTONUP   0x0202u
typedef struct { uint32_t frame, msg, wp, lp; } qmsg;
/* scripted input (DS_INPUT), in frame order */
#define MAX_SCRIPT 256
static qmsg g_script[MAX_SCRIPT]; static int g_script_n, g_script_next, g_q_loaded;
/* the queue PeekMessage reads: a ring of posted messages */
#define QRING 1024
static qmsg g_ring[QRING]; static uint32_t g_ring_head, g_ring_tail;
static void post(uint32_t msg, uint32_t wp, uint32_t lp) {
    if (g_ring_tail - g_ring_head >= QRING) return;          /* full: drop */
    qmsg *q = &g_ring[g_ring_tail++ % QRING]; q->frame = 0; q->msg = msg; q->wp = wp; q->lp = lp;
}
static void q_push(uint32_t frame, uint32_t msg, uint32_t wp, uint32_t lp) {
    if (g_script_n < MAX_SCRIPT) { qmsg *q = &g_script[g_script_n++]; q->frame = frame; q->msg = msg; q->wp = wp; q->lp = lp; }
}
static void q_load(void) {
    g_q_loaded = 1;
    const char *e = getenv("DS_INPUT");
    while (e && *e) {
        unsigned f, a, b; char what[16]; int used = 0;
        if (sscanf(e, " %u %15s %n", &f, what, &used) < 2) break;
        e += used;
        if (!strcmp(what, "click") && sscanf(e, "%u %u%n", &a, &b, &used) == 2) {
            uint32_t lp = MAKELONG16(a, b);
            /* spaced out: the UI picks the hovered widget on its own (sim) tick */
            q_push(f, WM_MOUSEMOVE, 0, lp); q_push(f + 10, WM_MOUSEMOVE, 0, lp);
            q_push(f + 20, WM_LBUTTONDOWN, 1, lp); q_push(f + 26, WM_LBUTTONUP, 0, lp);
            e += used;
        } else if (!strcmp(what, "hold") && sscanf(e, "%u %u%n", &a, &b, &used) == 2) {   /* F hold VK N */
            uint32_t sc = 0;
            for (size_t i = 0; i < sizeof k_vsc / sizeof k_vsc[0]; i++) if (k_vsc[i].vk == a) sc = k_vsc[i].sc;
            q_push(f, WM_KEYDOWN, a, 1u | (sc << 16));
            for (uint32_t r = 30; r < b; r += 30) q_push(f + r, WM_KEYDOWN, a, 0x40000001u | (sc << 16));  /* auto-repeat */
            q_push(f + b, WM_KEYUP, a, 0xC0000001u | (sc << 16));
            e += used;
        } else if (!strcmp(what, "key") && sscanf(e, "%u%n", &a, &used) == 1) {
            uint32_t sc = 0;
            for (size_t i = 0; i < sizeof k_vsc / sizeof k_vsc[0]; i++) if (k_vsc[i].vk == a) sc = k_vsc[i].sc;
            q_push(f, WM_KEYDOWN, a, 1u | (sc << 16)); q_push(f + 8, WM_KEYUP, a, 0xC0000001u | (sc << 16));
            e += used;
        }
        while (*e && *e != ';') e++;
        if (*e == ';') e++;
    }
    if (g_script_n) fprintf(stderr, "[user32] DS_INPUT: %d scripted messages\n", g_script_n);
}
static void sh_PeekMessageA(void) {          /* (MSG*, hwnd, min, max, wRemoveMsg) */
    if (!g_q_loaded) q_load();
    uint32_t m = A32(0), remove = A32(4) & 1;
    if (ds_sdl_active()) ds_sdl_pump(post);    /* the real screen and buttons */
    while (g_script_next < g_script_n && ds_frame_count() >= g_script[g_script_next].frame) {
        qmsg *q = &g_script[g_script_next++];
        fprintf(stderr, "[user32] input msg 0x%04X (%08X, %08X) at frame %u\n", q->msg, q->wp, q->lp, ds_frame_count());
        post(q->msg, q->wp, q->lp);
    }
    if (g_ring_head != g_ring_tail && g_wnd_n) {
        qmsg *q = &g_ring[g_ring_head % QRING];
        if (q->msg >= WM_MOUSEMOVE && q->msg <= 0x020Au) { g_cur_x = (int32_t)(q->lp & 0xFFFF); g_cur_y = (int32_t)(q->lp >> 16); }
        if (m) {                             /* MSG: hwnd, message, wParam, lParam, time, pt.x, pt.y */
            MEM32(m) = g_wnd[0].hwnd; MEM32(m + 4) = q->msg; MEM32(m + 8) = q->wp; MEM32(m + 12) = q->lp;
            MEM32(m + 16) = 0; MEM32(m + 20) = (uint32_t)g_cur_x; MEM32(m + 24) = (uint32_t)g_cur_y;
        }
        if (remove) g_ring_head++;
        RET(1, 5);
    }
    RET(0, 5);
}
static void sh_TranslateMessage(void){ RET(0, 1); }
static void sh_DispatchMessageA(void) {              /* (const MSG*): hwnd +0, message +4, wParam +8, lParam +12 */
    uint32_t m = A32(0);
    RET(m ? wnd_send(wnd_get(MEM32(m)), MEM32(m + 4), MEM32(m + 8), MEM32(m + 12)) : 0, 1);
}

static void sh_MessageBoxA(void) {           /* (hwnd, text, caption, type) */
    uint32_t type = A32(3) & 0xF;
    fprintf(stderr, "[user32] MessageBoxA [%s]: %s\n", A32(2) ? ASTR(2) : "", A32(1) ? ASTR(1) : "");
    static const uint32_t answer[] = { 1 /*OK*/, 1 /*OK*/, 3 /*ABORT*/, 6 /*YES*/, 6 /*YES*/, 2 /*CANCEL*/, 2 /*CANCEL*/ };
    RET(type < 7 ? answer[type] : 1, 4);
}

/* ===================== clipboard: never available */
static void sh_OpenClipboard(void)     { RET(0, 1); }
static void sh_CloseClipboard(void)    { RET(1, 0); }
static void sh_GetClipboardData(void)  { RET(0, 1); }

static void sh_SetProcessDPIAware(void){ RET(1, 0); }   /* via GetProcAddress */
static void sh_GetDC(void)             { RET(ds_handle_new(HK_DC, 0), 1); }
static void sh_ReleaseDC(void)         { RET(1, 2); }

/* ===================== GDI32: DCs and bitmaps exist, drawing is a no-op */
static void sh_CreateCompatibleDC(void)     { RET(ds_handle_new(HK_DC, 0), 1); }
static void sh_CreateCompatibleBitmap(void) { RET(ds_handle_new(HK_GDIOBJ, 0), 3); }
static void sh_GetStockObject(void)         { RET(ds_handle_new(HK_GDIOBJ, 0), 1); }
static void sh_SelectObject(void)           { RET(ds_handle_new(HK_GDIOBJ, 0), 2); }  /* "previous" object */
static void sh_DeleteObject(void)           { RET(1, 1); }
static void sh_SetStretchBltMode(void)      { RET(1, 2); }   /* previous mode: BLACKONWHITE */
static void sh_BitBlt(void)                 { RET(1, 9); }
static void sh_StretchBlt(void)             { RET(1, 11); }
static void sh_GetDIBits(void)              { RET(0, 7); }   /* (hdc, hbm, start, lines, bits, bmi, usage): nothing copied */

const ds_shim ds_user32[] = {
    { "RegisterClassA", sh_RegisterClassA }, { "CreateWindowExA", sh_CreateWindowExA },
    { "ShowWindow", sh_ShowWindow },
    { "GetWindowLongA", sh_GetWindowLongA }, { "SetWindowLongA", sh_SetWindowLongA },
    { "SendMessageA", sh_SendMessageA }, { "DefWindowProcA", sh_DefWindowProcA },
    { "GetClientRect", sh_GetClientRect }, { "GetWindowRect", sh_GetWindowRect },
    { "AdjustWindowRect", sh_AdjustWindowRect }, { "MoveWindow", sh_MoveWindow },
    { "SetWindowPos", sh_SetWindowPos }, { "MapWindowPoints", sh_MapWindowPoints },
    { "InvalidateRect", sh_InvalidateRect }, { "IsWindowVisible", sh_IsWindowVisible },
    { "IsIconic", sh_IsIconic }, { "GetForegroundWindow", sh_GetForegroundWindow },
    { "GetDesktopWindow", sh_GetDesktopWindow },
    { "SetCapture", sh_SetCapture }, { "ReleaseCapture", sh_ReleaseCapture },
    { "EnumDisplayDevicesA", sh_EnumDisplayDevicesA }, { "EnumDisplaySettingsA", sh_EnumDisplaySettingsA },
    { "ChangeDisplaySettingsExA", sh_ChangeDisplaySettingsExA },
    { "MonitorFromPoint", sh_MonitorFromPoint }, { "MonitorFromWindow", sh_MonitorFromWindow },
    { "GetMonitorInfoA", sh_GetMonitorInfoA }, { "SystemParametersInfoA", sh_SystemParametersInfoA },
    { "GetProcessWindowStation", sh_GetProcessWindowStation },
    { "GetUserObjectInformationW", sh_GetUserObjectInformationW },
    { "LoadCursorA", sh_LoadCursorA }, { "LoadIconA", sh_LoadIconA }, { "SetCursor", sh_SetCursor },
    { "ClipCursor", sh_ClipCursor }, { "GetCursorPos", sh_GetCursorPos }, { "SetCursorPos", sh_SetCursorPos },
    { "MapVirtualKeyA", sh_MapVirtualKeyA },
    { "PeekMessageA", sh_PeekMessageA }, { "TranslateMessage", sh_TranslateMessage },
    { "DispatchMessageA", sh_DispatchMessageA }, { "MessageBoxA", sh_MessageBoxA },
    { "OpenClipboard", sh_OpenClipboard }, { "CloseClipboard", sh_CloseClipboard },
    { "GetClipboardData", sh_GetClipboardData },
    { "SetProcessDPIAware", sh_SetProcessDPIAware }, { "GetDC", sh_GetDC }, { "ReleaseDC", sh_ReleaseDC },
    /* GDI32 */
    { "CreateCompatibleDC", sh_CreateCompatibleDC }, { "CreateCompatibleBitmap", sh_CreateCompatibleBitmap },
    { "GetStockObject", sh_GetStockObject }, { "SelectObject", sh_SelectObject },
    { "DeleteObject", sh_DeleteObject }, { "SetStretchBltMode", sh_SetStretchBltMode },
    { "BitBlt", sh_BitBlt }, { "StretchBlt", sh_StretchBlt }, { "GetDIBits", sh_GetDIBits },
    { 0, 0 }
};
