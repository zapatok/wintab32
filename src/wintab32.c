/*
 * wintab32.c - a WinTab 1.4 provider that reads a Linux tablet through evdev.
 *
 * Written for Wine on Wayland: XWayland never reports the tablet through
 * XInput-1, so Wine's own wintab32 sees zero devices.  This DLL bypasses X
 * completely and serves the WinTab API from /dev/input.
 *
 * Installed as a native override:
 *     cp wintab32.dll  $WINEPREFIX/drive_c/windows/system32/
 *     WINEDLLOVERRIDES="wintab32=n"
 *
 * Every entry point is logged to C:\wt.log (WT_LOG, WT_LOG_LEVEL=0..3), which
 * is also how the real call sequence of the host application is discovered.
 */
#define __USE_MINGW_ANSI_STDIO 1
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "wtapi.h"
#include "evdev.h"
#include "wtlog.h"
#include "wtmetrics.h"

#define SHIM_NAME    L"ToonBoom Linux WinTab 1.0"
#define SHIM_VERSION 0x0100          /* 1.0 */
#define SPEC_VERSION 0x0104          /* 1.4 */
#define PK_ALL       0x3FFF
#define MAX_CONTEXTS 8
#define DEFAULT_QUEUE 128

/* ------------------------------------------------------------------ */
/* context and packet storage                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    int      x, y, z;
    unsigned pressure, tangent;
    unsigned cursor, buttons;
    unsigned serial, time;
    int      azimuth, altitude, twist;
} RAWPKT;

typedef struct {
    HCTX   self;
    HWND   hwnd;
    int    used;
    int    enabled;
    int    overlap;
    LOGCONTEXTW lc;
    RAWPKT *queue;
    int    qsize;
    int    head;
    int    count;
    unsigned long last_post;   /* GetTickCount of the last WT_PACKET (coalescing) */
} CTX;

static CRITICAL_SECTION g_lock;
static INIT_ONCE        g_once = INIT_ONCE_STATIC_INIT;
static CTX             *g_ctx[MAX_CONTEXTS];
static unsigned         g_serial;

static int env_flag(const char *name, int def)
{
    const char *v = getenv(name);
    if (!v || !*v)
        return def;
    return atoi(v);
}

static int              g_coalesce = -1;   /* WT_COALESCE: 0 off (default) */
static int              g_coalesce_ms = 4; /* WT_COALESCE_MS */

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

static UINT pkt_size(WTPKT mask)
{
    UINT n = 0;
    if (mask & PK_CONTEXT)          n += sizeof(HCTX);
    if (mask & PK_STATUS)           n += 4;
    if (mask & PK_TIME)             n += 4;
    if (mask & PK_CHANGED)          n += 4;
    if (mask & PK_SERIAL_NUMBER)    n += 4;
    if (mask & PK_CURSOR)           n += 4;
    if (mask & PK_BUTTONS)          n += 4;
    if (mask & PK_X)                n += 4;
    if (mask & PK_Y)                n += 4;
    if (mask & PK_Z)                n += 4;
    if (mask & PK_NORMAL_PRESSURE)  n += 4;
    if (mask & PK_TANGENT_PRESSURE) n += 4;
    if (mask & PK_ORIENTATION)      n += 12;
    if (mask & PK_ROTATION)         n += 12;
    return n;
}

#define PUT32(p, v) do { unsigned _v = (unsigned)(v); memcpy((p), &_v, 4); (p) += 4; } while (0)
#define PUTS32(p, v) do { int _v = (int)(v); memcpy((p), &_v, 4); (p) += 4; } while (0)

static void build_packet(unsigned char *out, const CTX *ctx, const RAWPKT *r)
{
    const LOGCONTEXTW *lc = &ctx->lc;
    WTPKT mask = lc->lcPktData;
    unsigned char *p = out;

    if (mask & PK_CONTEXT) {
        HCTX h = ctx->self;
        memcpy(p, &h, sizeof(HCTX));
        p += sizeof(HCTX);
    }
    if (mask & PK_STATUS)           PUT32(p, 0);
    if (mask & PK_TIME)             PUT32(p, r->time);
    if (mask & PK_CHANGED)          PUT32(p, mask);
    if (mask & PK_SERIAL_NUMBER)    PUT32(p, r->serial);
    if (mask & PK_CURSOR)           PUT32(p, r->cursor);
    if (mask & PK_BUTTONS)          PUT32(p, r->buttons);
    if (mask & PK_X)                PUTS32(p, r->x);
    if (mask & PK_Y)                PUTS32(p, r->y);
    if (mask & PK_Z)                PUTS32(p, r->z);
    if (mask & PK_NORMAL_PRESSURE)  PUT32(p, r->pressure);
    if (mask & PK_TANGENT_PRESSURE) PUT32(p, r->tangent);
    if (mask & PK_ORIENTATION) {
        PUTS32(p, r->azimuth);
        PUTS32(p, r->altitude);
        PUTS32(p, r->twist);
    }
    if (mask & PK_ROTATION) {
        PUTS32(p, 0);
        PUTS32(p, 0);
        PUTS32(p, 0);
    }
}

static CTX *ctx_from_handle(HCTX h)
{
    CTX *ctx = (CTX *)h;
    int i;
    if (!ctx)
        return NULL;
    for (i = 0; i < MAX_CONTEXTS; i++)
        if (g_ctx[i] == ctx && ctx->used)
            return ctx;
    return NULL;
}

/* caller holds g_lock; returns 1 when the oldest packet had to be dropped */
static int push_locked(CTX *ctx, const RAWPKT *r)
{
    int dropped = 0;

    if (!ctx->used || !ctx->enabled || ctx->qsize <= 0)
        return 0;
    if (ctx->count == ctx->qsize) {        /* drop the oldest */
        ctx->head = (ctx->head + 1) % ctx->qsize;
        ctx->count--;
        dropped = 1;
    }
    ctx->queue[(ctx->head + ctx->count) % ctx->qsize] = *r;
    ctx->count++;
    return dropped;
}

/* ------------------------------------------------------------------ */
/* evdev sinks                                                         */
/* ------------------------------------------------------------------ */

static void on_packet(const EV_PKT *p)
{
    RAWPKT r;
    int i;

    memset(&r, 0, sizeof(r));
    r.x = p->x;
    r.y = p->y;
    r.z = p->z;
    r.pressure = (unsigned)p->pressure;
    r.cursor = p->cursor;
    r.buttons = p->buttons;

    EnterCriticalSection(&g_lock);
    r.serial = ++g_serial;
    r.time = GetTickCount();
    for (i = 0; i < MAX_CONTEXTS; i++) {
        CTX *ctx = g_ctx[i];
        int dropped, posted = 0;
        int64_t t_post = 0;

        if (!ctx || !ctx->used || !ctx->enabled)
            continue;

        dropped = push_locked(ctx, &r);

        if (ctx->lc.lcOptions & CXO_MESSAGES) {
            /*
             * Coalescing: the queue keeps every sample, but the window is not
             * flooded with one message per sample.  Qt drains the whole queue
             * with WTPacketsGet(128) on each WT_PACKET, so several packets ride
             * on one message.  The first packet after an empty queue is posted
             * immediately, and so is the lift, so latency at the edges does not
             * change.  Off by default: measure first, then decide.
             */
            unsigned long now = GetTickCount();
            int skip = g_coalesce > 0 && ctx->count > 1 && r.buttons != 0 &&
                       (now - ctx->last_post) < (unsigned long)g_coalesce_ms;
            if (skip) {
                wtm_count_coalesced();
            } else {
                t_post = wtm_on() ? wtm_now_us() : 0;
                PostMessageW(ctx->hwnd, WT_PACKET, (WPARAM)r.serial, (LPARAM)ctx);
                ctx->last_post = now;
                posted = 1;
            }
        }

        if (wtm_on())
            wtm_record(p->t_ev_us, p->t_read_us, t_post, ctx->count,
                       posted, dropped);
    }
    LeaveCriticalSection(&g_lock);

    if (wtlog_level() >= WTLOG_PACKET)
        wtlog(WTLOG_PACKET, "evdev packet x=%d y=%d p=%u btn=%u cur=%u",
              r.x, r.y, r.pressure, r.buttons, r.cursor);
}

static void on_proximity(int enter, const EV_PKT *p)
{
    RAWPKT r;
    int i;

    memset(&r, 0, sizeof(r));
    r.x = p->x;
    r.y = p->y;
    r.pressure = (unsigned)p->pressure;
    r.cursor = p->cursor;
    r.buttons = p->buttons;

    EnterCriticalSection(&g_lock);
    r.serial = ++g_serial;
    r.time = GetTickCount();
    for (i = 0; i < MAX_CONTEXTS; i++) {
        CTX *ctx = g_ctx[i];
        if (!ctx || !ctx->used)
            continue;
        (void)push_locked(ctx, &r);
        if (ctx->lc.lcOptions & CXO_MESSAGES)
            PostMessageW(ctx->hwnd, WT_PROXIMITY, (WPARAM)r.serial,
                         (LPARAM)MAKELPARAM(enter ? 1 : 0, enter ? 1 : 0));
    }
    LeaveCriticalSection(&g_lock);

    wtlog(WTLOG_CALL, "evdev proximity %s cursor=%u (x=%d y=%d)",
          enter ? "enter" : "leave", r.cursor, r.x, r.y);
}

/* ------------------------------------------------------------------ */
/* initialisation                                                      */
/* ------------------------------------------------------------------ */

static BOOL CALLBACK shim_init(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    (void)once; (void)param; (void)ctx;
    InitializeCriticalSection(&g_lock);
    wtlog_open();
    wtm_init();
    g_coalesce    = env_flag("WT_COALESCE", 0) ? 1 : 0;
    g_coalesce_ms = env_flag("WT_COALESCE_MS", 4);
    if (g_coalesce_ms < 0)
        g_coalesce_ms = 0;
    if (g_coalesce)
        wtlog(WTLOG_INIT, "shim: packet coalescing on (%d ms window)", g_coalesce_ms);
    wtlog(WTLOG_INIT, "shim init: %ls", SHIM_NAME);
    evdev_set_callbacks(on_packet, on_proximity);
    evdev_start();
    return TRUE;
}

static void ensure_init(void)
{
    InitOnceExecuteOnce(&g_once, shim_init, NULL, NULL);
}

/* ------------------------------------------------------------------ */
/* WTInfo                                                              */
/* ------------------------------------------------------------------ */

static UINT put_bytes(LPVOID out, const void *src, UINT n, int wide)
{
    (void)wide;
    if (out && src)
        memcpy(out, src, n);
    return n;
}

static UINT put_wstr(LPVOID out, const WCHAR *s, int wide)
{
    int len = (int)wcslen(s);
    if (!out)
        return (UINT)((len + 1) * sizeof(WCHAR));

    if (wide) {
        memcpy(out, s, (size_t)(len + 1) * sizeof(WCHAR));
        return (UINT)((len + 1) * sizeof(WCHAR));
    } else {
        int n = WideCharToMultiByte(CP_ACP, 0, s, len + 1, (char *)out,
                                    len * 4 + 4, NULL, NULL);
        return (UINT)(n > 0 ? n : (len + 1));
    }
}

static void fill_context(LOGCONTEXTW *lc, int is_system)
{
    int xmin, xmax, ymin, ymax;
    int sw, sh;

    memset(lc, 0, sizeof(*lc));
    evdev_ranges(&xmin, &xmax, &ymin, &ymax);

    wcsncpy(lc->lcName, is_system ? L"ToonBoom Linux WinTab System"
                                  : L"ToonBoom Linux WinTab", LCNAMELEN - 1);
    lc->lcOptions = is_system ? (CXO_SYSTEM | CXO_MESSAGES | CXO_CSRMESSAGES)
                              : (CXO_MESSAGES | CXO_CSRMESSAGES);
    lc->lcStatus  = 0;
    lc->lcLocks   = CXL_INSIZE | CXL_INASPECT;
    lc->lcMsgBase = WT_DEFBASE;
    lc->lcDevice  = 0;
    lc->lcPktRate = 100;
    lc->lcPktData = PK_ALL;
    lc->lcPktMode = 0;
    lc->lcMoveMask = PK_X | PK_Y | PK_NORMAL_PRESSURE | PK_BUTTONS;
    lc->lcBtnDnMask = 0x7;
    lc->lcBtnUpMask = 0x0;

    lc->lcInOrgX = 0;
    lc->lcInOrgY = 0;
    lc->lcInExtX = xmax - xmin;
    lc->lcInExtY = ymax - ymin;

    lc->lcOutOrgX = 0;
    lc->lcOutOrgY = 0;
    lc->lcOutExtX = xmax - xmin;
    lc->lcOutExtY = -(ymax - ymin);

    lc->lcSensX = 65536;
    lc->lcSensY = 65536;
    lc->lcSensZ = 65536;

    sw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    sh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    lc->lcSysMode = is_system ? 1 : 0;
    lc->lcSysOrgX = GetSystemMetrics(SM_XVIRTUALSCREEN);
    lc->lcSysOrgY = GetSystemMetrics(SM_YVIRTUALSCREEN);
    lc->lcSysExtX = sw > 0 ? sw : 1920;
    lc->lcSysExtY = sh > 0 ? sh : 1080;
    lc->lcSysSensX = 65536;
    lc->lcSysSensY = 65536;
}

static UINT device_axis(UINT idx, LPVOID out, int wide, AXIS *axis)
{
    int xmin, xmax, ymin, ymax;
    evdev_ranges(&xmin, &xmax, &ymin, &ymax);
    memset(axis, 0, sizeof(*axis));
    axis->axUnits = TU_NONE;
    switch (idx) {
    case DVC_X:
        axis->axMin = xmin; axis->axMax = xmax; break;
    case DVC_Y:
        axis->axMin = ymin; axis->axMax = ymax; break;
    case DVC_Z:
        axis->axMin = 0; axis->axMax = 0; break;
    case DVC_NPRESSURE:
        axis->axMin = 0; axis->axMax = evdev_pressure_range(); break;
    case DVC_TPRESSURE:
        axis->axMin = 0; axis->axMax = evdev_pressure_range(); break;
    default:
        return 0;
    }
    return put_bytes(out, axis, sizeof(*axis), wide);
}

static UINT wtinfo_core(UINT cat, UINT idx, LPVOID out, int wide)
{
    int xmin, xmax, ymin, ymax;
    evdev_ranges(&xmin, &xmax, &ymin, &ymax);

    if (cat == 0) {
        UINT sz = 200;
        if (out) memset(out, 0, sz);
        return sz;
    }

    switch (cat) {
    case WTI_INTERFACE:
        switch (idx) {
        case IFC_WINTABID:
            return put_wstr(out, SHIM_NAME, wide);
        case IFC_SPECVERSION: {
            WORD v = SPEC_VERSION;
            return put_bytes(out, &v, sizeof(v), wide);
        }
        case IFC_IMPLVERSION: {
            WORD v = SHIM_VERSION;
            return put_bytes(out, &v, sizeof(v), wide);
        }
        case IFC_NDEVICES: {
            WORD v = 1;
            return put_bytes(out, &v, sizeof(v), wide);
        }
        case IFC_NCURSORS: {
            WORD v = 3;
            return put_bytes(out, &v, sizeof(v), wide);
        }
        case IFC_NCONTEXTS: {
            WORD v = 1;
            return put_bytes(out, &v, sizeof(v), wide);
        }
        case IFC_CTXOPTIONS: {
            UINT v = CXO_SYSTEM | CXO_MESSAGES | CXO_CSRMESSAGES;
            return put_bytes(out, &v, sizeof(v), wide);
        }
        case IFC_CTXSAVESIZE: {
            UINT v = (UINT)sizeof(LOGCONTEXTW);
            return put_bytes(out, &v, sizeof(v), wide);
        }
        case IFC_NEXTENSIONS: {
            WORD v = 0;
            return put_bytes(out, &v, sizeof(v), wide);
        }
        case IFC_NMANAGERS: {
            WORD v = 0;
            return put_bytes(out, &v, sizeof(v), wide);
        }
        default:
            return 0;
        }

    case WTI_STATUS:
        switch (idx) {
        case STA_CONTEXTS: { WORD v = 1; return put_bytes(out, &v, sizeof(v), wide); }
        case STA_SYSCTXS:  { WORD v = 1; return put_bytes(out, &v, sizeof(v), wide); }
        case STA_PKTRATE:  { UINT v = 100; return put_bytes(out, &v, sizeof(v), wide); }
        case STA_PKTDATA:  { WTPKT v = PK_ALL; return put_bytes(out, &v, sizeof(v), wide); }
        case STA_MANAGERS: { WORD v = 0; return put_bytes(out, &v, sizeof(v), wide); }
        case STA_SYSTEM:   { WORD v = 1; return put_bytes(out, &v, sizeof(v), wide); }
        case STA_BUTTONUSE: { DWORD v = 0x7; return put_bytes(out, &v, sizeof(v), wide); }
        case STA_SYSBTNUSE: { DWORD v = 0x7; return put_bytes(out, &v, sizeof(v), wide); }
        default: return 0;
        }

    case WTI_DEFCONTEXT:
    case WTI_DEFSYSCTX:
    case WTI_DDCTXS:
    case WTI_DSCTXS: {
        LOGCONTEXTW lc;
        int is_system = (cat == WTI_DEFSYSCTX || cat == WTI_DSCTXS);
        fill_context(&lc, is_system);

        if (idx == 0)
            return put_bytes(out, &lc, sizeof(lc), wide);

        switch (idx) {
        case CTX_NAME:      return put_wstr(out, lc.lcName, wide);
        case CTX_OPTIONS:   return put_bytes(out, &lc.lcOptions, 4, wide);
        case CTX_STATUS:    return put_bytes(out, &lc.lcStatus, 4, wide);
        case CTX_LOCKS:     return put_bytes(out, &lc.lcLocks, 4, wide);
        case CTX_MSGBASE:   return put_bytes(out, &lc.lcMsgBase, 4, wide);
        case CTX_DEVICE:    return put_bytes(out, &lc.lcDevice, 4, wide);
        case CTX_PKTRATE:   return put_bytes(out, &lc.lcPktRate, 4, wide);
        case CTX_PKTDATA:   return put_bytes(out, &lc.lcPktData, 4, wide);
        case CTX_PKTMODE:   return put_bytes(out, &lc.lcPktMode, 4, wide);
        case CTX_MOVEMASK:  return put_bytes(out, &lc.lcMoveMask, 4, wide);
        case CTX_BTNDNMASK: return put_bytes(out, &lc.lcBtnDnMask, 4, wide);
        case CTX_BTNUPMASK: return put_bytes(out, &lc.lcBtnUpMask, 4, wide);
        case CTX_INORGX:    return put_bytes(out, &lc.lcInOrgX, 4, wide);
        case CTX_INORGY:    return put_bytes(out, &lc.lcInOrgY, 4, wide);
        case CTX_INORGZ:    return put_bytes(out, &lc.lcInOrgZ, 4, wide);
        case CTX_INEXTX:    return put_bytes(out, &lc.lcInExtX, 4, wide);
        case CTX_INEXTY:    return put_bytes(out, &lc.lcInExtY, 4, wide);
        case CTX_INEXTZ:    return put_bytes(out, &lc.lcInExtZ, 4, wide);
        case CTX_OUTORGX:   return put_bytes(out, &lc.lcOutOrgX, 4, wide);
        case CTX_OUTORGY:   return put_bytes(out, &lc.lcOutOrgY, 4, wide);
        case CTX_OUTORGZ:   return put_bytes(out, &lc.lcOutOrgZ, 4, wide);
        case CTX_OUTEXTX:   return put_bytes(out, &lc.lcOutExtX, 4, wide);
        case CTX_OUTEXTY:   return put_bytes(out, &lc.lcOutExtY, 4, wide);
        case CTX_OUTEXTZ:   return put_bytes(out, &lc.lcOutExtZ, 4, wide);
        case CTX_SENSX:     return put_bytes(out, &lc.lcSensX, 4, wide);
        case CTX_SENSY:     return put_bytes(out, &lc.lcSensY, 4, wide);
        case CTX_SENSZ:     return put_bytes(out, &lc.lcSensZ, 4, wide);
        case CTX_SYSMODE:   return put_bytes(out, &lc.lcSysMode, 4, wide);
        case CTX_SYSORGX:   return put_bytes(out, &lc.lcSysOrgX, 4, wide);
        case CTX_SYSORGY:   return put_bytes(out, &lc.lcSysOrgY, 4, wide);
        case CTX_SYSEXTX:   return put_bytes(out, &lc.lcSysExtX, 4, wide);
        case CTX_SYSEXTY:   return put_bytes(out, &lc.lcSysExtY, 4, wide);
        case CTX_SYSSENSX:  return put_bytes(out, &lc.lcSysSensX, 4, wide);
        case CTX_SYSSENSY:  return put_bytes(out, &lc.lcSysSensY, 4, wide);
        default: return 0;
        }
    }

    case WTI_DEVICES:
    case WTI_DEVICES + 1:
    case WTI_DEVICES + 2:
        if (cat != WTI_DEVICES)
            return 0; /* single device */
        switch (idx) {
        case DVC_NAME: {
            WCHAR name[128];
            const char *n = evdev_name();
            if (!n || !n[0])
                n = "Linux evdev tablet";
            MultiByteToWideChar(CP_ACP, 0, n, -1, name, 128);
            return put_wstr(out, name, wide);
        }
        case DVC_HARDWARE: {
            DWORD v = HWC_INTEGRATED | HWC_HARDPROX | HWC_PHYSID_CURSORS;
            return put_bytes(out, &v, sizeof(v), wide);
        }
        case DVC_NCSRTYPES: { DWORD v = 1; return put_bytes(out, &v, sizeof(v), wide); }
        case DVC_FIRSTCSR:  { DWORD v = 1; return put_bytes(out, &v, sizeof(v), wide); }
        case DVC_PKTRATE:   { UINT  v = 100; return put_bytes(out, &v, sizeof(v), wide); }
        case DVC_PKTDATA:   { WTPKT v = PK_ALL; return put_bytes(out, &v, sizeof(v), wide); }
        case DVC_PKTMODE:   { WTPKT v = 0; return put_bytes(out, &v, sizeof(v), wide); }
        case DVC_CSRDATA:   { UINT  v = 0; return put_bytes(out, &v, sizeof(v), wide); }
        case DVC_X:
        case DVC_Y:
        case DVC_Z:
        case DVC_NPRESSURE:
        case DVC_TPRESSURE: {
            AXIS ax;
            return device_axis(idx, out, wide, &ax);
        }
        case DVC_ORIENTATION:
        case DVC_ROTATION: {
            AXIS ax[3];
            memset(ax, 0, sizeof(ax));   /* axResolution 0 -> no tilt support */
            return put_bytes(out, ax, sizeof(ax), wide);
        }
        case DVC_PNPID: {
            WCHAR name[128];
            const char *n = evdev_name();
            if (!n || !n[0])
                n = "Linux evdev tablet";
            MultiByteToWideChar(CP_ACP, 0, n, -1, name, 128);
            return put_wstr(out, name, wide);
        }
        default:
            return 0;
        }

    case WTI_CURSORS:
    case WTI_CURSORS + 1:
    case WTI_CURSORS + 2: {
        UINT n = cat - WTI_CURSORS;
        static const WCHAR *names[3] = { L"Cursor", L"Pen", L"Eraser" };
        switch (idx) {
        case CSR_NAME:      return put_wstr(out, names[n], wide);
        case CSR_ACTIVE:    { DWORD v = (n == 0) ? 0 : 1; return put_bytes(out, &v, 4, wide); }
        case CSR_PKTDATA:   { WTPKT v = PK_ALL; return put_bytes(out, &v, 4, wide); }
        case CSR_BUTTONS:   { DWORD v = 3; return put_bytes(out, &v, 4, wide); }
        case CSR_BUTTONBITS:{ DWORD v = 3; return put_bytes(out, &v, 4, wide); }
        case CSR_BUTTONMAP: {
            BYTE map[32];
            memset(map, 0, sizeof(map));
            map[0] = 1; map[1] = 2; map[2] = 4;
            return put_bytes(out, map, sizeof(map), wide);
        }
        case CSR_SYSBTNMAP: {
            /* tip -> left, barrel 1 -> right, barrel 2 -> middle */
            BYTE map[32];
            memset(map, 0, sizeof(map));
            map[0] = 1; map[1] = 4; map[2] = 2;
            return put_bytes(out, map, sizeof(map), wide);
        }
        case CSR_NPBUTTON:   { DWORD v = 0; return put_bytes(out, &v, 4, wide); }
        case CSR_NPBTNMARKS: { DWORD v[2] = { 0, 0 }; return put_bytes(out, v, 8, wide); }
        case CSR_NPRESPONSE: { DWORD v = 0; return put_bytes(out, &v, 4, wide); }
        case CSR_TPBUTTON:   { DWORD v = 0; return put_bytes(out, &v, 4, wide); }
        case CSR_PHYSID:     { DWORD v = 1; return put_bytes(out, &v, 4, wide); }
        case CSR_MODE:         { UINT  v = 0; return put_bytes(out, &v, 4, wide); }
        case CSR_MINPKTDATA:   { DWORD v = 0; return put_bytes(out, &v, 4, wide); }
        case CSR_MINBUTTONS:   { DWORD v = 1; return put_bytes(out, &v, 4, wide); }
        case CSR_CAPABILITIES: { DWORD v = 0; return put_bytes(out, &v, 4, wide); }
        case CSR_TYPE:         { DWORD v = 0x0802; return put_bytes(out, &v, 4, wide); }
        default: return 0;
        }
    }

    default:
        return 0;
    }
}

/* ------------------------------------------------------------------ */
/* exports                                                             */
/* ------------------------------------------------------------------ */

UINT WINAPI WTInfoW(UINT cat, UINT idx, LPVOID out)
{
    ensure_init();
    {
        UINT n = wtinfo_core(cat, idx, out, 1);
        wtlog(WTLOG_CALL, "WTInfoW(cat=%u idx=%u out=%p) -> %u", cat, idx, out, n);
        return n;
    }
}

UINT WINAPI WTInfoA(UINT cat, UINT idx, LPVOID out)
{
    ensure_init();
    {
        UINT n = wtinfo_core(cat, idx, out, 0);
        wtlog(WTLOG_CALL, "WTInfoA(cat=%u idx=%u out=%p) -> %u", cat, idx, out, n);
        return n;
    }
}

static void log_context(const char *fn, const LOGCONTEXTW *lc)
{
    if (!lc) {
        wtlog(WTLOG_CALL, "%s (no context)", fn);
        return;
    }
    wtlog(WTLOG_CALL,
          "%s name=\"%ls\" options=0x%04x pktData=0x%04x moveMask=0x%04x "
          "rate=%u dev=%u in=(%d,%d,%d)+(%d,%d,%d) out=(%d,%d,%d)+(%d,%d,%d)",
          fn, lc->lcName, lc->lcOptions, lc->lcPktData, lc->lcMoveMask,
          lc->lcPktRate, lc->lcDevice,
          lc->lcInOrgX, lc->lcInOrgY, lc->lcInOrgZ,
          lc->lcInExtX, lc->lcInExtY, lc->lcInExtZ,
          lc->lcOutOrgX, lc->lcOutOrgY, lc->lcOutOrgZ,
          lc->lcOutExtX, lc->lcOutExtY, lc->lcOutExtZ);
}

HCTX WINAPI WTOpenW(HWND hwnd, LPLOGCONTEXTW lc, BOOL enable)
{
    CTX *ctx;
    int slot = -1, i;

    ensure_init();

    if (!lc || !hwnd) {
        wtlog(WTLOG_CALL, "WTOpenW rejected: hwnd=%p lc=%p", hwnd, lc);
        return NULL;
    }

    ctx = (CTX *)calloc(1, sizeof(*ctx));
    if (!ctx)
        return NULL;
    ctx->queue = (RAWPKT *)calloc(DEFAULT_QUEUE, sizeof(RAWPKT));
    if (!ctx->queue) {
        free(ctx);
        return NULL;
    }
    ctx->self = ctx;
    ctx->hwnd = hwnd;
    ctx->lc = *lc;
    ctx->lc.lcDevice = 0;
    ctx->lc.lcMsgBase = WT_DEFBASE;
    if (!ctx->lc.lcPktData)
        ctx->lc.lcPktData = PK_ALL;
    if (!ctx->lc.lcMoveMask)
        ctx->lc.lcMoveMask = PK_X | PK_Y | PK_NORMAL_PRESSURE | PK_BUTTONS;
    ctx->lc.lcStatus = 0;
    ctx->qsize = DEFAULT_QUEUE;
    ctx->head = ctx->count = 0;
    ctx->enabled = enable ? 1 : 0;
    ctx->overlap = 0;
    ctx->used = 1;

    EnterCriticalSection(&g_lock);
    for (i = 0; i < MAX_CONTEXTS; i++)
        if (!g_ctx[i]) { slot = i; break; }
    if (slot >= 0)
        g_ctx[slot] = ctx;
    if (slot >= 0) {
        EV_PKT snap;
        if (ctx->enabled && evdev_snapshot(&snap)) {
            RAWPKT r;
            memset(&r, 0, sizeof(r));
            r.x = snap.x; r.y = snap.y; r.pressure = (unsigned)snap.pressure;
            r.cursor = snap.cursor; r.buttons = 0;
            r.serial = ++g_serial;
            r.time = GetTickCount();
            push_locked(ctx, &r);
            PostMessageW(ctx->hwnd, WT_PROXIMITY, (WPARAM)r.serial,
                         (LPARAM)MAKELPARAM(1, 1));
            wtlog(WTLOG_CALL, "WTOpenW: primed context with live proximity");
        }
    }
    LeaveCriticalSection(&g_lock);

    if (slot < 0) {
        free(ctx->queue);
        free(ctx);
        wtlog(WTLOG_CALL, "WTOpenW failed: no free context slot");
        return NULL;
    }

    log_context("WTOpenW", &ctx->lc);
    wtlog(WTLOG_CALL, "WTOpenW(hwnd=%p) -> ctx=%p enabled=%d", hwnd, ctx,
          ctx->enabled);
    /* The pointer duplicates the pen while it is in range; drop it so the app
     * has a single input device (see NOTAS 9.5). */
    evdev_mouse_filter(1);
    return ctx;
}

HCTX WINAPI WTOpenA(HWND hwnd, LPLOGCONTEXTA lc, BOOL enable)
{
    LOGCONTEXTW w;
    if (!lc)
        return NULL;
    memset(&w, 0, sizeof(w));
    MultiByteToWideChar(CP_ACP, 0, lc->lcName, -1, w.lcName, LCNAMELEN);
    w.lcOptions = lc->lcOptions; w.lcStatus = lc->lcStatus; w.lcLocks = lc->lcLocks;
    w.lcMsgBase = lc->lcMsgBase; w.lcDevice = lc->lcDevice; w.lcPktRate = lc->lcPktRate;
    w.lcPktData = lc->lcPktData; w.lcPktMode = lc->lcPktMode; w.lcMoveMask = lc->lcMoveMask;
    w.lcBtnDnMask = lc->lcBtnDnMask; w.lcBtnUpMask = lc->lcBtnUpMask;
    w.lcInOrgX = lc->lcInOrgX; w.lcInOrgY = lc->lcInOrgY; w.lcInOrgZ = lc->lcInOrgZ;
    w.lcInExtX = lc->lcInExtX; w.lcInExtY = lc->lcInExtY; w.lcInExtZ = lc->lcInExtZ;
    w.lcOutOrgX = lc->lcOutOrgX; w.lcOutOrgY = lc->lcOutOrgY; w.lcOutOrgZ = lc->lcOutOrgZ;
    w.lcOutExtX = lc->lcOutExtX; w.lcOutExtY = lc->lcOutExtY; w.lcOutExtZ = lc->lcOutExtZ;
    w.lcSensX = lc->lcSensX; w.lcSensY = lc->lcSensY; w.lcSensZ = lc->lcSensZ;
    w.lcSysMode = lc->lcSysMode; w.lcSysOrgX = lc->lcSysOrgX; w.lcSysOrgY = lc->lcSysOrgY;
    w.lcSysExtX = lc->lcSysExtX; w.lcSysExtY = lc->lcSysExtY;
    w.lcSysSensX = lc->lcSysSensX; w.lcSysSensY = lc->lcSysSensY;
    return WTOpenW(hwnd, &w, enable);
}

BOOL WINAPI WTClose(HCTX h)
{
    CTX *ctx;
    int i;
    ensure_init();
    EnterCriticalSection(&g_lock);
    ctx = ctx_from_handle(h);
    if (ctx) {
        for (i = 0; i < MAX_CONTEXTS; i++)
            if (g_ctx[i] == ctx)
                g_ctx[i] = NULL;
        ctx->used = 0;
    }
    LeaveCriticalSection(&g_lock);
    if (ctx) {
        free(ctx->queue);
        free(ctx);
    }
    wtlog(WTLOG_CALL, "WTClose(%p) -> %d", h, ctx ? 1 : 0);
    return ctx ? TRUE : FALSE;
}

int WINAPI WTPacketsGet(HCTX h, int max, LPVOID buf)
{
    CTX *ctx;
    int n = 0;

    ensure_init();
    if (max <= 0 || !buf)
        return 0;

    EnterCriticalSection(&g_lock);
    ctx = ctx_from_handle(h);
    if (ctx) {
        UINT sz = pkt_size(ctx->lc.lcPktData);
        unsigned char *out = (unsigned char *)buf;
        while (n < max && ctx->count > 0 && sz > 0) {
            RAWPKT r = ctx->queue[ctx->head];
            ctx->head = (ctx->head + 1) % ctx->qsize;
            ctx->count--;
            build_packet(out, ctx, &r);
            out += sz;
            n++;
        }
    }
    LeaveCriticalSection(&g_lock);

    if (wtlog_level() >= WTLOG_PACKET)
        wtlog(WTLOG_PACKET, "WTPacketsGet(%p, %d) -> %d", h, max, n);
    return n;
}

int WINAPI WTPacketsPeek(HCTX h, int max, LPVOID buf)
{
    CTX *ctx;
    int n = 0;

    ensure_init();
    EnterCriticalSection(&g_lock);
    ctx = ctx_from_handle(h);
    if (ctx && buf) {
        UINT sz = pkt_size(ctx->lc.lcPktData);
        int i;
        unsigned char *out = (unsigned char *)buf;
        for (i = 0; i < max && i < ctx->count && sz > 0; i++) {
            RAWPKT r = ctx->queue[(ctx->head + i) % ctx->qsize];
            build_packet(out, ctx, &r);
            out += sz;
            n++;
        }
    }
    LeaveCriticalSection(&g_lock);
    wtlog(WTLOG_CALL, "WTPacketsPeek(%p, %d) -> %d", h, max, n);
    return n;
}

BOOL WINAPI WTPacket(HCTX h, UINT serial, LPVOID buf)
{
    CTX *ctx;
    BOOL found = FALSE;

    ensure_init();
    EnterCriticalSection(&g_lock);
    ctx = ctx_from_handle(h);
    if (ctx && buf) {
        int i;
        for (i = 0; i < ctx->count; i++) {
            RAWPKT r = ctx->queue[(ctx->head + i) % ctx->qsize];
            if (r.serial == serial) {
                build_packet((unsigned char *)buf, ctx, &r);
                found = TRUE;
                break;
            }
        }
    }
    LeaveCriticalSection(&g_lock);
    wtlog(WTLOG_CALL, "WTPacket(%p, serial=%u) -> %d", h, serial, found);
    return found;
}

BOOL WINAPI WTEnable(HCTX h, BOOL enable)
{
    CTX *ctx;
    ensure_init();
    EnterCriticalSection(&g_lock);
    ctx = ctx_from_handle(h);
    if (ctx) {
        ctx->enabled = enable ? 1 : 0;
        if (!ctx->enabled)
            ctx->count = 0;
    }
    LeaveCriticalSection(&g_lock);
    wtlog(WTLOG_CALL, "WTEnable(%p, %d) -> %d", h, enable, ctx ? 1 : 0);
    return ctx ? TRUE : FALSE;
}

BOOL WINAPI WTOverlap(HCTX h, BOOL state)
{
    CTX *ctx;
    ensure_init();
    EnterCriticalSection(&g_lock);
    ctx = ctx_from_handle(h);
    if (ctx)
        ctx->overlap = state ? 1 : 0;
    LeaveCriticalSection(&g_lock);
    wtlog(WTLOG_CALL, "WTOverlap(%p, %d) -> %d", h, state, ctx ? 1 : 0);
    return ctx ? TRUE : FALSE;
}

BOOL WINAPI WTGetW(HCTX h, LPLOGCONTEXTW lc)
{
    CTX *ctx;
    ensure_init();
    if (!lc)
        return FALSE;
    EnterCriticalSection(&g_lock);
    ctx = ctx_from_handle(h);
    if (ctx) {
        *lc = ctx->lc;
        lc->lcStatus = ctx->enabled ? 0 : CXS_DISABLED;
    }
    LeaveCriticalSection(&g_lock);
    wtlog(WTLOG_CALL, "WTGetW(%p) -> %d dev=%u pktData=0x%04x", h, ctx ? 1 : 0,
          lc->lcDevice, lc->lcPktData);
    return ctx ? TRUE : FALSE;
}

BOOL WINAPI WTGetA(HCTX h, LPLOGCONTEXTA lc)
{
    LOGCONTEXTW w;
    int n;
    ensure_init();
    if (!lc || !WTGetW(h, &w))
        return FALSE;
    memset(lc, 0, sizeof(*lc));
    n = WideCharToMultiByte(CP_ACP, 0, w.lcName, -1, lc->lcName, LCNAMELEN, NULL, NULL);
    if (n <= 0)
        lc->lcName[0] = 0;
    lc->lcOptions = w.lcOptions; lc->lcStatus = w.lcStatus; lc->lcLocks = w.lcLocks;
    lc->lcMsgBase = w.lcMsgBase; lc->lcDevice = w.lcDevice; lc->lcPktRate = w.lcPktRate;
    lc->lcPktData = w.lcPktData; lc->lcPktMode = w.lcPktMode; lc->lcMoveMask = w.lcMoveMask;
    lc->lcBtnDnMask = w.lcBtnDnMask; lc->lcBtnUpMask = w.lcBtnUpMask;
    lc->lcInOrgX = w.lcInOrgX; lc->lcInOrgY = w.lcInOrgY; lc->lcInOrgZ = w.lcInOrgZ;
    lc->lcInExtX = w.lcInExtX; lc->lcInExtY = w.lcInExtY; lc->lcInExtZ = w.lcInExtZ;
    lc->lcOutOrgX = w.lcOutOrgX; lc->lcOutOrgY = w.lcOutOrgY; lc->lcOutOrgZ = w.lcOutOrgZ;
    lc->lcOutExtX = w.lcOutExtX; lc->lcOutExtY = w.lcOutExtY; lc->lcOutExtZ = w.lcOutExtZ;
    lc->lcSensX = w.lcSensX; lc->lcSensY = w.lcSensY; lc->lcSensZ = w.lcSensZ;
    lc->lcSysMode = w.lcSysMode; lc->lcSysOrgX = w.lcSysOrgX; lc->lcSysOrgY = w.lcSysOrgY;
    lc->lcSysExtX = w.lcSysExtX; lc->lcSysExtY = w.lcSysExtY;
    lc->lcSysSensX = w.lcSysSensX; lc->lcSysSensY = w.lcSysSensY;
    return TRUE;
}

BOOL WINAPI WTSetW(HCTX h, LPLOGCONTEXTW lc)
{
    CTX *ctx;
    ensure_init();
    if (!lc)
        return FALSE;
    EnterCriticalSection(&g_lock);
    ctx = ctx_from_handle(h);
    if (ctx) {
        HWND hwnd = ctx->hwnd;
        ctx->lc = *lc;
        ctx->lc.lcDevice = 0;
        ctx->lc.lcMsgBase = WT_DEFBASE;
        ctx->hwnd = hwnd;
    }
    LeaveCriticalSection(&g_lock);
    wtlog(WTLOG_CALL, "WTSetW(%p, pktData=0x%04x) -> %d", h, lc->lcPktData,
          ctx ? 1 : 0);
    return ctx ? TRUE : FALSE;
}

BOOL WINAPI WTSetA(HCTX h, LPLOGCONTEXTA lc)
{
    LOGCONTEXTW w;
    if (!lc)
        return FALSE;
    memset(&w, 0, sizeof(w));
    MultiByteToWideChar(CP_ACP, 0, lc->lcName, -1, w.lcName, LCNAMELEN);
    w.lcOptions = lc->lcOptions; w.lcStatus = lc->lcStatus; w.lcLocks = lc->lcLocks;
    w.lcMsgBase = lc->lcMsgBase; w.lcDevice = lc->lcDevice; w.lcPktRate = lc->lcPktRate;
    w.lcPktData = lc->lcPktData; w.lcPktMode = lc->lcPktMode; w.lcMoveMask = lc->lcMoveMask;
    w.lcBtnDnMask = lc->lcBtnDnMask; w.lcBtnUpMask = lc->lcBtnUpMask;
    w.lcInOrgX = lc->lcInOrgX; w.lcInOrgY = lc->lcInOrgY; w.lcInOrgZ = lc->lcInOrgZ;
    w.lcInExtX = lc->lcInExtX; w.lcInExtY = lc->lcInExtY; w.lcInExtZ = lc->lcInExtZ;
    w.lcOutOrgX = lc->lcOutOrgX; w.lcOutOrgY = lc->lcOutOrgY; w.lcOutOrgZ = lc->lcOutOrgZ;
    w.lcOutExtX = lc->lcOutExtX; w.lcOutExtY = lc->lcOutExtY; w.lcOutExtZ = lc->lcOutExtZ;
    w.lcSensX = lc->lcSensX; w.lcSensY = lc->lcSensY; w.lcSensZ = lc->lcSensZ;
    w.lcSysMode = lc->lcSysMode; w.lcSysOrgX = lc->lcSysOrgX; w.lcSysOrgY = lc->lcSysOrgY;
    w.lcSysExtX = lc->lcSysExtX; w.lcSysExtY = lc->lcSysExtY;
    w.lcSysSensX = lc->lcSysSensX; w.lcSysSensY = lc->lcSysSensY;
    return WTSetW(h, &w);
}

BOOL WINAPI WTExtGet(HCTX h, UINT ext, LPVOID buf)
{
    (void)h; (void)ext; (void)buf;
    ensure_init();
    wtlog(WTLOG_CALL, "WTExtGet(%p, %u) -> 0", h, ext);
    return FALSE;
}

BOOL WINAPI WTExtSet(HCTX h, UINT ext, LPVOID buf)
{
    (void)h; (void)ext; (void)buf;
    ensure_init();
    wtlog(WTLOG_CALL, "WTExtSet(%p, %u) -> 0", h, ext);
    return FALSE;
}

BOOL WINAPI WTSave(HCTX h, LPVOID buf)
{
    CTX *ctx;
    ensure_init();
    EnterCriticalSection(&g_lock);
    ctx = ctx_from_handle(h);
    if (ctx && buf)
        memcpy(buf, &ctx->lc, sizeof(ctx->lc));
    LeaveCriticalSection(&g_lock);
    wtlog(WTLOG_CALL, "WTSave(%p) -> %d", h, ctx ? 1 : 0);
    return ctx ? TRUE : FALSE;
}

HCTX WINAPI WTRestore(HWND hwnd, LPVOID buf, int len, BOOL enable)
{
    LOGCONTEXTW *lc = (LOGCONTEXTW *)buf;
    ensure_init();
    wtlog(WTLOG_CALL, "WTRestore(hwnd=%p len=%d) -> %s", hwnd, len,
          (lc && len >= (int)sizeof(*lc)) ? "open" : "null");
    if (!lc || len < (int)sizeof(*lc))
        return NULL;
    return WTOpenW(hwnd, lc, enable);
}

int WINAPI WTQueueSizeGet(HCTX h)
{
    CTX *ctx;
    int n = 0;
    ensure_init();
    EnterCriticalSection(&g_lock);
    ctx = ctx_from_handle(h);
    if (ctx)
        n = ctx->qsize;
    LeaveCriticalSection(&g_lock);
    wtlog(WTLOG_CALL, "WTQueueSizeGet(%p) -> %d", h, n);
    return n;
}

BOOL WINAPI WTQueueSizeSet(HCTX h, int size)
{
    CTX *ctx;
    BOOL ok = FALSE;

    ensure_init();
    if (size <= 0)
        size = DEFAULT_QUEUE;
    if (size > 8192)
        size = 8192;

    EnterCriticalSection(&g_lock);
    ctx = ctx_from_handle(h);
    if (ctx) {
        RAWPKT *q = (RAWPKT *)calloc((size_t)size, sizeof(RAWPKT));
        if (q) {
            int i, n = ctx->count < size ? ctx->count : size;
            for (i = 0; i < n; i++)
                q[i] = ctx->queue[(ctx->head + ctx->count - n + i) % ctx->qsize];
            free(ctx->queue);
            ctx->queue = q;
            ctx->qsize = size;
            ctx->head = 0;
            ctx->count = n;
            ok = TRUE;
        }
    }
    LeaveCriticalSection(&g_lock);
    wtlog(WTLOG_CALL, "WTQueueSizeSet(%p, %d) -> %d", h, size, ok);
    return ok;
}

BOOL WINAPI WTQueuePacketsEx(HCTX h, UINT *oldest, UINT *count)
{
    CTX *ctx;
    ensure_init();
    EnterCriticalSection(&g_lock);
    ctx = ctx_from_handle(h);
    if (ctx) {
        if (oldest && ctx->count)
            *oldest = ctx->queue[ctx->head].serial;
        if (count)
            *count = (UINT)ctx->count;
    }
    LeaveCriticalSection(&g_lock);
    return ctx ? TRUE : FALSE;
}

/* packet queue inspection for tests */
void WINAPI WTShimInfo(UINT which, LPVOID out, UINT len)
{
    char *dst = (char *)out;
    if (!out || !len)
        return;
    switch (which) {
    case 1:
        lstrcpynA(dst, evdev_path()[0] ? evdev_path() : "", (int)len);
        break;
    case 2:
        lstrcpynA(dst, evdev_name()[0] ? evdev_name() : "", (int)len);
        break;
    case 3:
        lstrcpynA(dst, evdev_has_device() ? "1" : "0", (int)len);
        break;
    case 4:
        lstrcpynA(dst, "built " __DATE__ " " __TIME__, (int)len);
        break;
    default:
        dst[0] = 0;
        break;
    }
}

/* ---- manager / configuration stubs ---------------------------------- */

BOOL WINAPI WTConfig(HWND hwnd, LPVOID lc)
{
    (void)hwnd; (void)lc;
    ensure_init();
    wtlog(WTLOG_CALL, "WTConfig -> 0");
    return FALSE;
}

HANDLE WINAPI WTMgrOpen(HWND hwnd, UINT msgbase)
{
    (void)hwnd; (void)msgbase;
    ensure_init();
    wtlog(WTLOG_CALL, "WTMgrOpen -> 0 (no manager in this shim)");
    return NULL;
}

BOOL WINAPI WTMgrClose(HANDLE mgr)
{
    (void)mgr;
    ensure_init();
    wtlog(WTLOG_CALL, "WTMgrClose -> 0");
    return FALSE;
}

int WINAPI WTDataGet(HCTX h, UINT table, UINT row, UINT n, LPVOID buf, LPVOID size)
{
    (void)h; (void)table; (void)row; (void)n; (void)buf; (void)size;
    ensure_init();
    return 0;
}

int WINAPI WTDataPeek(HCTX h, UINT table, UINT row, UINT n, LPVOID buf, LPVOID size)
{
    (void)h; (void)table; (void)row; (void)n; (void)buf; (void)size;
    ensure_init();
    return 0;
}

BOOL WINAPI WTMgrContextEnum(HANDLE m, LPHCTX list, UINT n)
{ (void)m; (void)list; (void)n; return FALSE; }
HCTX WINAPI WTMgrContextSelect(HANDLE m, HCTX h) { (void)m; return h; }
HCTX WINAPI WTMgrContextOwner(HANDLE m, HCTX h) { (void)m; return h; }
HCTX WINAPI WTMgrDefContext(HANDLE m, BOOL sys) { (void)m; (void)sys; return NULL; }
HCTX WINAPI WTMgrDefContextEx(HANDLE m, UINT dev, BOOL sys)
{ (void)m; (void)dev; (void)sys; return NULL; }
BOOL WINAPI WTMgrDeviceConfig(HANDLE m, UINT dev, UINT idx, LPVOID buf)
{ (void)m; (void)dev; (void)idx; (void)buf; return FALSE; }
BOOL WINAPI WTMgrExt(HANDLE m, UINT ext, LPVOID buf) { (void)m; (void)ext; (void)buf; return FALSE; }
BOOL WINAPI WTMgrCsrEnable(HANDLE m, UINT cur, BOOL en)
{ (void)m; (void)cur; (void)en; return FALSE; }
BOOL WINAPI WTMgrCsrButtonMap(HANDLE m, UINT cur, LPVOID a, LPVOID b)
{ (void)m; (void)cur; (void)a; (void)b; return FALSE; }
BOOL WINAPI WTMgrCsrPressureBtnMarks(HANDLE m, UINT cur, DWORD a, DWORD b)
{ (void)m; (void)cur; (void)a; (void)b; return FALSE; }
BOOL WINAPI WTMgrCsrPressureResponse(HANDLE m, UINT cur, LPVOID a, LPVOID b)
{ (void)m; (void)cur; (void)a; (void)b; return FALSE; }
BOOL WINAPI WTMgrCsrExt(HANDLE m, UINT cur, UINT ext, LPVOID buf)
{ (void)m; (void)cur; (void)ext; (void)buf; return FALSE; }
BOOL WINAPI WTMgrCsrPressureBtnMarksEx(HANDLE m, UINT cur, LPVOID a, LPVOID b)
{ (void)m; (void)cur; (void)a; (void)b; return FALSE; }
HANDLE WINAPI WTMgrConfigReplaceExA(HWND h, HANDLE m, LPCSTR a, LPCSTR b)
{ (void)h; (void)m; (void)a; (void)b; return NULL; }
HANDLE WINAPI WTMgrConfigReplaceExW(HWND h, HANDLE m, LPCWSTR a, LPCWSTR b)
{ (void)h; (void)m; (void)a; (void)b; return NULL; }
HANDLE WINAPI WTMgrPacketHookExA(HWND h, HANDLE m, LPCSTR a, LPCSTR b)
{ (void)h; (void)m; (void)a; (void)b; return NULL; }
HANDLE WINAPI WTMgrPacketHookExW(HWND h, HANDLE m, LPCWSTR a, LPCWSTR b)
{ (void)h; (void)m; (void)a; (void)b; return NULL; }
BOOL WINAPI WTMgrPacketUnhook(HANDLE hook) { (void)hook; return FALSE; }
HANDLE WINAPI WTMgrPacketHookNext(HANDLE hook, DWORD flags, LPVOID a, LPVOID b)
{ (void)hook; (void)flags; (void)a; (void)b; return NULL; }

/* ------------------------------------------------------------------ */

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        evdev_set_instance(inst);
    } else if (reason == DLL_PROCESS_DETACH) {
        evdev_mouse_filter(0);
        evdev_stop();
        wtm_close();
        wtlog_close();
    }
    return TRUE;
}
