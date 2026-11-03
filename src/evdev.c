/*
 * evdev.c - read a Linux tablet from /dev/input and turn it into WinTab input.
 *
 * This code lives in a Windows PE built by mingw-w64 and runs under Wine.  It
 * never calls a Win32 file API for the device: the x86-64 kernel ABI is stable,
 * so the DLL issues openat/ioctl/read/poll itself.  That keeps the shim
 * independent from Wine's path translation and from msvcrt.
 *
 * Recognised devices: anything exposing ABS_X/ABS_Y, ideally ABS_PRESSURE and
 * BTN_TOOL_PEN (Wacom, XP-Pen, Huion, ...).  WT_DEV=/dev/input/eventN forces a
 * node; WT_DEV=none disables the reader.
 *
 * Environment:
 *   WT_DEV             device node (default: autodetect through sysfs)
 *   WT_X_RANGE         "min:max" override, wins over ioctl
 *   WT_Y_RANGE         "min:max" override
 *   WT_PRESSURE_RANGE  "min:max" override
 *   WT_PRESSURE_MAX    output range for pkNormalPressure (default 1023)
 *   WT_INVERT_X / WT_INVERT_Y   flip an axis (0/1)
 *   WT_SWAP_XY         swap axes (0/1)
 *   WT_RECHECK_MS      rescan interval while disconnected (default 1000)
 */
#define __USE_MINGW_ANSI_STDIO 1
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "evdev.h"
#include "wtlog.h"

/* ------------------------------------------------------------------ */
/* raw Linux syscalls                                                  */
/* ------------------------------------------------------------------ */

#if !defined(__x86_64__)
#error "the direct-syscall path is x86-64 only"
#endif

/*
 * Why no raw syscalls: Wine 11 on Linux installs a syscall dispatcher, so a
 * `syscall` instruction issued by a Windows PE is handled by ntdll and comes
 * back as STATUS_NOT_IMPLEMENTED (0xC0000002) instead of reaching the kernel.
 * Everything below goes through Win32 file APIs; Wine maps Z: to /.
 */
static void unix_to_win(const char *unixpath, char *out, size_t cap)
{
    size_t i;
    if (cap < 4) { out[0] = 0; return; }
    out[0] = 'Z'; out[1] = ':';
    for (i = 0; unixpath[i] && i + 3 < cap; i++)
        out[i + 2] = (unixpath[i] == '/') ? '\\' : unixpath[i];
    out[i + 2] = 0;
}

static intptr_t lx_open(const char *path)
{
    char win[300];
    HANDLE h;
    unix_to_win(path, win, sizeof(win));
    h = CreateFileA(win, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_EXISTING, 0, NULL);
    return (h == INVALID_HANDLE_VALUE) ? -1 : (intptr_t)h;
}

static void lx_close(intptr_t fd)
{
    if (fd != -1)
        CloseHandle((HANDLE)fd);
}

static long lx_read(intptr_t fd, void *buf, size_t len)
{
    DWORD got = 0;
    if (fd == -1)
        return -1;
    if (!ReadFile((HANDLE)fd, buf, (DWORD)len, &got, NULL))
        return -1;
    return (long)got;
}

static void lx_sleep_ms(unsigned ms)
{
    Sleep((DWORD)ms);
}

#define LX_EV_SYN  0x00
#define LX_EV_KEY  0x01
#define LX_EV_ABS  0x03
#define LX_EV_REL  0x02
#define LX_REL_X   0x00
#define LX_SYN_REPORT 0

#define LX_ABS_X          0x00
#define LX_ABS_Y          0x01
#define LX_ABS_PRESSURE   0x18

#define LX_BTN_TOOL_PEN    0x140
#define LX_BTN_TOOL_RUBBER 0x141
#define LX_BTN_TOUCH       0x14a
#define LX_BTN_STYLUS      0x14b
#define LX_BTN_STYLUS2     0x14c

typedef struct {
    int64_t  sec, usec;
    uint16_t type, code;
    int32_t  value;
} lx_input_event;



/* ------------------------------------------------------------------ */
/* state                                                               */
/* ------------------------------------------------------------------ */

#define NAME_MAX_LEN 256

static EV_ON_PACKET    g_on_packet;
static EV_ON_PROXIMITY g_on_prox;

static volatile LONG g_stop;
static HANDLE        g_thread;
static intptr_t      g_fd = -1;
static char          g_path[128] = "";
static char          g_name[NAME_MAX_LEN] = "";
static int           g_present;

static int g_xmin, g_xmax = 15200, g_ymin, g_ymax = 9500;
static int g_pmin, g_pmax = 2047;
static int g_pout_max = 1023;
static int g_invert_x, g_invert_y, g_swap_xy;
static int g_pos_cursor = 1;   /* WT_POS_CURSOR: take the position from the pointer */

static int g_in_prox;
static unsigned g_cursor = 1;
static int g_tip, g_b1, g_b2;
static int g_raw_x, g_raw_y, g_raw_p;
static int g_dirty;

static int env_int(const char *name, int def)
{
    char buf[64];
    DWORD n = GetEnvironmentVariableA(name, buf, sizeof(buf));
    if (n == 0 || n >= sizeof(buf))
        return def;
    return atoi(buf);
}

static int parse_range(const char *name, int *lo, int *hi)
{
    char buf[64];
    DWORD n = GetEnvironmentVariableA(name, buf, sizeof(buf));
    char *colon;
    if (n == 0 || n >= sizeof(buf))
        return 0;
    colon = strchr(buf, ':');
    if (!colon)
        return 0;
    *colon = 0;
    *lo = atoi(buf);
    *hi = atoi(colon + 1);
    return (*hi > *lo);
}

/* ------------------------------------------------------------------ */
/* device discovery                                                    */
/* ------------------------------------------------------------------ */

static int read_text(const char *path, char *out, int cap)
{
    intptr_t fd = lx_open(path);
    long n;
    if (fd == -1)
        return -1;
    n = lx_read(fd, out, (size_t)(cap - 1));
    lx_close(fd);
    if (n <= 0)
        return (int)(n < 0 ? -n : 0);
    out[n] = 0;
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r'))
        out[--n] = 0;
    return (int)n;
}

/* Opens `path` and fills g_name/g_path from sysfs.  -1 on failure. */
static intptr_t open_and_query(const char *path)
{
    const char *base;
    char sys[200], namebuf[NAME_MAX_LEN];
    intptr_t h;

    memset(namebuf, 0, sizeof(namebuf));
    h = lx_open(path);
    if (h == -1)
        return -1;

    base = strrchr(path, '/');
    if (base && !strncmp(path, "/dev/input/", 11)) {
        snprintf(sys, sizeof(sys), "/sys/class/input/%s/device/name", base + 1);
        if (read_text(sys, namebuf, sizeof(namebuf)) <= 0)
            namebuf[0] = 0;
    }
    lstrcpynA(g_name, namebuf[0] ? namebuf : "(unnamed)", sizeof(g_name));
    lstrcpynA(g_path, path, sizeof(g_path));
    return h;
}

static int name_score(const char *name)
{
    char low[NAME_MAX_LEN];
    int i, score = 0;

    for (i = 0; i < NAME_MAX_LEN - 1 && name[i]; i++)
        low[i] = (char)((name[i] >= 'A' && name[i] <= 'Z') ? name[i] + 32 : name[i]);
    low[i] = 0;

    if (strstr(low, "wacom"))                        score += 100;
    if (strstr(low, "pen") || strstr(low, "stylus"))  score += 50;
    if (strstr(low, "tablet"))                       score += 20;
    if (strstr(low, "pad"))                          score -= 200;
    if (strstr(low, "touch"))                        score -= 200;
    if (strstr(low, "finger"))                       score -= 200;
    if (strstr(low, "keyboard"))                     score -= 200;
    if (strstr(low, "mouse"))                        score -= 200;
    if (strstr(low, "consumer"))                     score -= 200;
    return score;
}

/*
 * Discovery: read /sys/class/input/eventN/device/name through Wine's file
 * layer, score the names and try to open the best ones.  No ioctls: Wine's
 * Win32 layer has no path to them, and the shim does not need them (ranges are
 * the Intuos defaults or WT_*_RANGE overrides).
 */
static intptr_t find_device(void)
{
    char forced[128];
    DWORD n;
    int  i, j, opened = 0, verbose;
    struct { int score, idx; } cand[64];
    int nc = 0;
    static unsigned calls;

    verbose = (++calls <= 2) || (calls % 60 == 0);

    n = GetEnvironmentVariableA("WT_DEV", forced, sizeof(forced));
    if (n > 0 && n < sizeof(forced)) {
        if (!lstrcmpiA(forced, "none")) {
            wtlog(WTLOG_INIT, "evdev: disabled by WT_DEV=none");
            return -1;
        }
        wtlog(WTLOG_INIT, "evdev: forced device %s", forced);
        return open_and_query(forced);
    }

    for (i = 0; i < 64; i++) {
        char sys[160], name[NAME_MAX_LEN];
        int  len, score;

        snprintf(sys, sizeof(sys), "/sys/class/input/event%d/device/name", i);
        len = read_text(sys, name, sizeof(name));
        if (len <= 0)
            continue;
        opened++;
        score = name_score(name);
        if (verbose)
            wtlog(WTLOG_INIT, "evdev: /dev/input/event%d is \"%s\" (score %d)", i, name, score);
        if (score > 0 && nc < 64) {
            cand[nc].score = score;
            cand[nc].idx   = i;
            nc++;
        }
    }

    /* best score first */
    for (i = 0; i < nc; i++)
        for (j = i + 1; j < nc; j++)
            if (cand[j].score > cand[i].score) {
                int ts = cand[i].score, ti = cand[i].idx;
                cand[i].score = cand[j].score; cand[i].idx = cand[j].idx;
                cand[j].score = ts;            cand[j].idx = ti;
            }

    for (i = 0; i < nc; i++) {
        char path[64];
        intptr_t h;
        snprintf(path, sizeof(path), "/dev/input/event%d", cand[i].idx);
        h = open_and_query(path);
        if (h != -1) {
            wtlog(WTLOG_INIT, "evdev: using %s name=\"%s\" (score %d)",
                  path, g_name, cand[i].score);
            return h;
        }
        if (verbose)
            wtlog(WTLOG_INIT, "evdev: %s looks right but open failed (err %lu)",
                  path, (unsigned long)GetLastError());
    }

    if (verbose) {
        if (nc > 0)
            wtlog(WTLOG_INIT, "evdev: %d candidate(s) found, none could be opened; "
                  "grant read access (udev rule or `usermod -aG input <user>`) or force "
                  "WT_DEV=/dev/input/eventN", nc);
        else if (opened == 0)
            wtlog(WTLOG_INIT, "evdev: no /sys/class/input/eventN/device/name readable "
                  "(Wine file layer); force a node with WT_DEV=/dev/input/eventN");
        else
            wtlog(WTLOG_INIT, "evdev: %d nodes seen, none scored as a pen; "
                  "force one with WT_DEV=/dev/input/eventN", opened);
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* packet emission                                                     */
/* ------------------------------------------------------------------ */

static int virtual_rect(int *vx, int *vy, int *vw, int *vh)
{
    *vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    *vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    *vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    *vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    return (*vw > 0 && *vh > 0);
}

/*
 * QT_POS_CURSOR mode: report the pointer position instead of the pen's.
 *
 * Rationale (see NOTAS 9.4): under XWayland the stylus does not drive the core
 * pointer, and XWayland also feeds it as a *relative* pointer, so the absolute
 * device position and the visible cursor drift apart.  Qt then flips between
 * PenMode and MouseMode, and Harmony alternates between pencil and mouse.
 * If the packet carries the inverse map of the pointer, both agree by
 * construction: Qt stays in PenMode, the brush sits exactly on the cursor, and
 * the pressure - which is still the pen's - is the only thing the shim adds.
 * The pen's absolute position is not lost: it still drives the pointer through
 * the compositor; we merely stop claiming a second, incompatible one.
 */
static int cursor_to_dev(int *px, int *py)
{
    int vx, vy, vw, vh;
    POINT pt;

    if (!GetCursorPos(&pt))
        return 0;
    if (!virtual_rect(&vx, &vy, &vw, &vh))
        return 0;
    if (g_xmax <= g_xmin || g_ymax <= g_ymin)
        return 0;

    *px = g_xmin + (int)(((int64_t)(pt.x - vx) * (g_xmax - g_xmin)) / vw);
    *py = g_ymin + (int)(((int64_t)(pt.y - vy) * (g_ymax - g_ymin)) / vh);
    if (*px < g_xmin) *px = g_xmin;
    if (*px > g_xmax) *px = g_xmax;
    if (*py < g_ymin) *py = g_ymin;
    if (*py > g_ymax) *py = g_ymax;
    return 1;
}

static void make_pkt(EV_PKT *pkt)
{
    int x, y;

    if (g_pos_cursor && cursor_to_dev(&x, &y)) {
        /* already in device coordinates, no swap/invert on top */
    } else {
        x = g_raw_x;
        y = g_raw_y;
        if (g_swap_xy) {
            int t = x; x = y; y = t;
        }
        if (g_invert_x)
            x = g_xmin + g_xmax - x;
        if (g_invert_y)
            y = g_ymin + g_ymax - y;
    }

    pkt->x = x;
    pkt->y = y;
    pkt->z = 0;
    pkt->buttons = (unsigned)((g_tip ? 1u : 0u) | (g_b1 ? 2u : 0u) | (g_b2 ? 4u : 0u));
    pkt->cursor  = g_cursor;

    if (g_pmax > g_pmin && g_pout_max > 0) {
        int p = g_raw_p - g_pmin;
        int span = g_pmax - g_pmin;
        if (p < 0) p = 0;
        if (p > span) p = span;
        pkt->pressure = (int)(((int64_t)p * g_pout_max) / span);
    } else {
        pkt->pressure = 0;
    }
}

/* ------------------------------------------------------------------ */
/* mouse filter: one device, not two                                   */
/* ------------------------------------------------------------------ */

static HINSTANCE g_inst;
static HHOOK     g_hook;
static int       g_block = -1;
static unsigned long g_last_evt;   /* watchdog: never block on a silent device */

void evdev_set_instance(void *hinst) { g_inst = (HINSTANCE)hinst; }

static int is_mouse_msg(UINT m)
{
    switch (m) {
    case WM_MOUSEMOVE:      case WM_LBUTTONDOWN:   case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:  case WM_RBUTTONDOWN:   case WM_RBUTTONUP:
    case WM_RBUTTONDBLCLK:  case WM_MBUTTONDOWN:   case WM_MBUTTONUP:
    case WM_MBUTTONDBLCLK:  case WM_XBUTTONDOWN:   case WM_XBUTTONUP:
    case WM_XBUTTONDBLCLK:  case WM_MOUSEWHEEL:    case WM_MOUSEHWHEEL:
    case WM_MOUSELEAVE:
        return 1;
    }
    return 0;
}

static LRESULT CALLBACK mouse_filter(int code, WPARAM wp, LPARAM lp)
{
    if (code == HC_ACTION && g_block && g_in_prox
        && (GetTickCount() - g_last_evt) < 2000) {
        const MSG *m = (const MSG *)lp;
        if (m && is_mouse_msg(m->message))
            return 1;
    }
    return CallNextHookEx(g_hook, code, wp, lp);
}

int evdev_mouse_filter(int on)
{
    if (g_block < 0)
        g_block = env_int("WT_MOUSE_BLOCK", 1);
    if (on && !g_hook && g_block) {
        g_hook = SetWindowsHookExA(WH_GETMESSAGE, mouse_filter, g_inst,
                                   GetCurrentThreadId());
        wtlog(WTLOG_INIT, "evdev: mouse filter %s (WT_MOUSE_BLOCK)",
              g_hook ? "installed" : "failed");
    } else if (!on && g_hook) {
        UnhookWindowsHookEx(g_hook);
        g_hook = NULL;
    }
    return g_hook != NULL;
}

/* ------------------------------------------------------------------ */
/* pointer sync: the pen drives the system pointer                     */
/* ------------------------------------------------------------------ */

/*
 * A real tablet under X11 moves the core pointer; that is why Qt compares the
 * pen position with the mouse position and stays in PenMode.  Under XWayland
 * the stylus is invisible to X, so the pointer never follows.  We warp it
 * ourselves with SetCursorPos (Wine -> XWarpPointer): one cursor, and Qt's
 * mouse/pen distance stays inside its 20 px gate.
 */
static int           g_sync = -1;
static int           g_sync_ms = 12;
static unsigned long g_last_sync;

static void sync_pointer(void)
{
    unsigned long now;
    int vx, vy, vw, vh, sx, sy;

    if (g_sync < 0) {
        g_sync    = env_int("WT_MOUSE_SYNC", 0);
        g_sync_ms = env_int("WT_MOUSE_SYNC_MS", 12);
        if (g_sync_ms < 0)
            g_sync_ms = 0;
    }
    if (!g_sync || !g_in_prox)
        return;

    now = GetTickCount();
    if (g_sync_ms && (now - g_last_sync) < (unsigned long)g_sync_ms)
        return;

    if (!virtual_rect(&vx, &vy, &vw, &vh) || g_xmax <= g_xmin || g_ymax <= g_ymin)
        return;

    sx = vx + (int)(((int64_t)(g_raw_x - g_xmin) * vw) / (g_xmax - g_xmin));
    sy = vy + (int)(((int64_t)(g_raw_y - g_ymin) * vh) / (g_ymax - g_ymin));
    SetCursorPos(sx, sy);
    g_last_sync = now;
    {
        static int logged;
        if (!logged) {
            POINT got;
            if (GetCursorPos(&got))
                wtlog(WTLOG_INIT, "evdev: sync wants %d,%d and the pointer landed at %ld,%ld "
                      "(virtual %d,%d %dx%d)", sx, sy, got.x, got.y,
                      vx, vy, vw, vh);
            logged = 1;
        }
    }
}

static void emit_snapshot(void)
{
    EV_PKT pkt;
    make_pkt(&pkt);
    if (g_on_packet)
        g_on_packet(&pkt);
}

/* ------------------------------------------------------------------ */
/* event decoding                                                      */
/* ------------------------------------------------------------------ */

static void handle_event(const lx_input_event *ev)
{
    g_last_evt = GetTickCount();
    switch (ev->type) {
    case LX_EV_SYN:
        if (ev->code == LX_SYN_REPORT) {
            if (g_dirty && g_in_prox) {
                emit_snapshot();
                sync_pointer();
            }
            g_dirty = 0;
        }
        break;

    case LX_EV_ABS:
        switch (ev->code) {
        case LX_ABS_X:       g_raw_x = ev->value; g_dirty = 1; break;
        case LX_ABS_Y:       g_raw_y = ev->value; g_dirty = 1; break;
        case LX_ABS_PRESSURE:g_raw_p = ev->value; g_dirty = 1; break;
        default: break;
        }
        break;

    case LX_EV_KEY:
        switch (ev->code) {
        case LX_BTN_TOOL_PEN:
        case LX_BTN_TOOL_RUBBER:
            if (ev->value) {
                int was = g_in_prox;
                g_cursor = (ev->code == LX_BTN_TOOL_RUBBER) ? 2u : 1u;
                g_in_prox = 1;
                g_dirty = 1;
                if (!was && g_on_prox) {
                    EV_PKT snap;
                    make_pkt(&snap);
                    g_on_prox(1, &snap);
                    sync_pointer();
                }
            } else if (g_in_prox) {
                EV_PKT snap;
                g_in_prox = 0;
                g_tip = g_b1 = g_b2 = 0;
                g_dirty = 1;
                make_pkt(&snap);
                snap.buttons = 0;
                if (g_on_prox)
                    g_on_prox(0, &snap);
            }
            break;

        case LX_BTN_TOUCH:
            g_tip = ev->value ? 1 : 0;
            g_dirty = 1;
            if (g_tip && !g_in_prox) {
                EV_PKT snap;
                g_in_prox = 1;
                make_pkt(&snap);
                snap.buttons = 1;
                if (g_on_prox)
                    g_on_prox(1, &snap);
            }
            break;

        case LX_BTN_STYLUS:  g_b1 = ev->value ? 1 : 0; g_dirty = 1; break;
        case LX_BTN_STYLUS2: g_b2 = ev->value ? 1 : 0; g_dirty = 1; break;
        default: break;
        }
        break;

    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* reader thread                                                       */
/* ------------------------------------------------------------------ */

static DWORD WINAPI reader_thread(LPVOID param)
{
    unsigned char buf[24 * 64];
    size_t  have = 0;
    unsigned recheck = (unsigned)env_int("WT_RECHECK_MS", 1000);
    int logged_absent = 0;
    (void)param;

    if (recheck < 100)
        recheck = 100;

    for (;;) {
        if (InterlockedCompareExchange(&g_stop, 0, 0))
            break;

        if (g_fd == -1) {
            intptr_t fd = find_device();
            if (fd == -1) {
                if (!logged_absent) {
                    wtlog(WTLOG_INIT,
                          "evdev: no tablet device found (checked /dev/input/event0..63). "
                          "Grant read access first: udev rule or `usermod -aG input <user>`.");
                    logged_absent = 1;
                }
                g_present = 0;
                lx_sleep_ms(recheck);
                continue;
            }
            g_fd = fd;
            logged_absent = 0;
            g_present = 1;
            have = 0;
            g_in_prox = 0;
            g_tip = g_b1 = g_b2 = 0;

            wtlog(WTLOG_INIT,
                  "evdev: using %s name=\"%s\" x=%d..%d y=%d..%d pressure=%d..%d -> 0..%d",
                  g_path, g_name, g_xmin, g_xmax, g_ymin, g_ymax, g_pmin, g_pmax,
                  g_pout_max);

            wtlog(WTLOG_PACKET, "evdev: reading %s (blocking ReadFile)", g_path);
        }

        {
            long n = lx_read(g_fd, buf + have, sizeof(buf) - have);
            size_t off = 0;

            if (n <= 0) {
                wtlog(WTLOG_INIT, "evdev: read on %s ended (%ld), reopening", g_path, n);
                lx_close(g_fd);
                g_fd = -1;
                g_present = 0;
                g_in_prox = 0;
                g_tip = g_b1 = g_b2 = 0;
                lx_sleep_ms(recheck);
                continue;
            }
            have += (size_t)n;
            while (have - off >= sizeof(lx_input_event)) {
                lx_input_event ev;
                memcpy(&ev, buf + off, sizeof(ev));
                off += sizeof(ev);
                handle_event(&ev);
            }
            if (off) {
                memmove(buf, buf + off, have - off);
                have -= off;
            }
        }
    }

    if (g_fd >= 0) {
        lx_close(g_fd);
        g_fd = -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* public API                                                          */
/* ------------------------------------------------------------------ */

void evdev_set_callbacks(EV_ON_PACKET on_packet, EV_ON_PROXIMITY on_prox)
{
    g_on_packet = on_packet;
    g_on_prox   = on_prox;
}

int evdev_start(void)
{
    if (g_thread)
        return 1;

    g_pout_max = env_int("WT_PRESSURE_MAX", 1023);
    if (g_pout_max <= 0)
        g_pout_max = 1023;
    g_invert_x = env_int("WT_INVERT_X", 0);
    g_invert_y = env_int("WT_INVERT_Y", 0);
    g_swap_xy  = env_int("WT_SWAP_XY", 0);
    g_pos_cursor = env_int("WT_POS_CURSOR", 1);
    wtlog(WTLOG_INIT, "evdev: position from %s (WT_POS_CURSOR), pointer sync %s (WT_MOUSE_SYNC, %d ms)",
          g_pos_cursor ? "the system pointer" : "the tablet's absolute axes",
          env_int("WT_MOUSE_SYNC", 0) ? "on" : "off", env_int("WT_MOUSE_SYNC_MS", 12));

    /* Ranges: the Intuos defaults, overridable by environment.  Parsed before
     * discovery so WTInfo answers with them even if no node is open yet. */
    if (parse_range("WT_X_RANGE", &g_xmin, &g_xmax))
        wtlog(WTLOG_INIT, "evdev: x range forced to %d..%d", g_xmin, g_xmax);
    if (parse_range("WT_Y_RANGE", &g_ymin, &g_ymax))
        wtlog(WTLOG_INIT, "evdev: y range forced to %d..%d", g_ymin, g_ymax);
    if (parse_range("WT_PRESSURE_RANGE", &g_pmin, &g_pmax))
        wtlog(WTLOG_INIT, "evdev: pressure range forced to %d..%d", g_pmin, g_pmax);

    /* Synchronous discovery: WTInfo must answer with the real ranges before
     * the application opens its context. */
    g_stop = 0;
    {
        intptr_t fd = find_device();
        if (fd != -1) {
            g_fd = fd;
            g_present = 1;
            wtlog(WTLOG_INIT,
                  "evdev: using %s name=\"%s\" x=%d..%d y=%d..%d pressure=%d..%d -> 0..%d",
                  g_path, g_name, g_xmin, g_xmax, g_ymin, g_ymax, g_pmin, g_pmax,
                  g_pout_max);
        } else {
            wtlog(WTLOG_INIT,
                  "evdev: no tablet device found at startup; will retry every %d ms",
                  env_int("WT_RECHECK_MS", 1000));
        }
    }

    g_thread = CreateThread(NULL, 0, reader_thread, NULL, 0, NULL);
    if (!g_thread) {
        wtlog(WTLOG_INIT, "evdev: CreateThread failed (%lu)", GetLastError());
        return 0;
    }
    SetThreadPriority(g_thread, THREAD_PRIORITY_ABOVE_NORMAL);
    return 1;
}

void evdev_stop(void)
{
    if (!g_thread)
        return;
    InterlockedExchange(&g_stop, 1);
    WaitForSingleObject(g_thread, 3000);
    CloseHandle(g_thread);
    g_thread = NULL;
    g_present = 0;
}

int evdev_has_device(void) { return g_present; }
const char *evdev_path(void) { return g_path; }
const char *evdev_name(void) { return g_name; }
int evdev_pressure_range(void) { return g_pout_max; }

int evdev_in_proximity(void)
{
    return g_in_prox;
}

int evdev_snapshot(EV_PKT *out)
{
    if (!g_present || !g_in_prox || !out)
        return 0;
    make_pkt(out);
    return 1;
}

void evdev_ranges(int *xmin, int *xmax, int *ymin, int *ymax)
{
    if (g_present) {
        *xmin = g_xmin; *xmax = g_xmax; *ymin = g_ymin; *ymax = g_ymax;
    } else {
        *xmin = 0; *xmax = 15200; *ymin = 0; *ymax = 9500;
    }
}
