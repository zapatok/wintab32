/*
 * wtprobe.exe - drive the shim exactly like Qt 6.4's QWindowsWinTab32DLL does,
 * so the packet flow can be checked without Harmony.
 *
 *   wtprobe [seconds]
 *
 * Prints WTInfo answers, the context it opens, every proximity event and a
 * summary of the packets (count, first samples, pressure spread).
 */
#define __USE_MINGW_ANSI_STDIO 1
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../src/wtapi.h"

#define PACKETDATA  (PK_X | PK_Y | PK_BUTTONS | PK_NORMAL_PRESSURE | \
                     PK_TANGENT_PRESSURE | PK_ORIENTATION | PK_CURSOR | PK_Z | PK_TIME)

typedef UINT (WINAPI *pWTInfoW)(UINT, UINT, LPVOID);
typedef HCTX (WINAPI *pWTOpenW)(HWND, LOGCONTEXTW *, BOOL);
typedef BOOL (WINAPI *pWTClose)(HCTX);
typedef int  (WINAPI *pWTPacketsGet)(HCTX, int, LPVOID);
typedef BOOL (WINAPI *pWTEnable)(HCTX, BOOL);
typedef BOOL (WINAPI *pWTOverlap)(HCTX, BOOL);
typedef BOOL (WINAPI *pWTGetW)(HCTX, LOGCONTEXTW *);
typedef int  (WINAPI *pWTQueueSizeGet)(HCTX);
typedef BOOL (WINAPI *pWTQueueSizeSet)(HCTX, int);
typedef void (WINAPI *pWTShimInfo)(UINT, LPVOID, UINT);

#define WTI_INTERFACE   1
#define X_WTI_DEFSYSCTX   4
#define WTI_DEVICES   100
#define DVC_NAME        1
#define DVC_X          12
#define DVC_Y          13
#define DVC_NPRESSURE  15
#define IFC_WINTABID    1
#define IFC_SPECVERSION 2
#define IFC_NDEVICES    4
#define IFC_NCURSORS    5

static pWTInfoW         WTInfoW_;
static pWTOpenW         WTOpenW_;
static pWTClose         WTClose_;
static pWTPacketsGet    WTPacketsGet_;
static pWTEnable        WTEnable_;
static pWTOverlap       WTOverlap_;
static pWTGetW          WTGetW_;
static pWTQueueSizeGet  WTQueueSizeGet_;
static pWTQueueSizeSet  WTQueueSizeSet_;
static pWTShimInfo      WTShimInfo_;

static HCTX g_ctx;
static int  g_total, g_printed;
static unsigned g_pmin = 0xFFFFFFFFu, g_pmax;
static unsigned g_seen[64];
static int  g_nseen;

static void note_pressure(unsigned p)
{
    int i;
    if (p < g_pmin) g_pmin = p;
    if (p > g_pmax) g_pmax = p;
    for (i = 0; i < g_nseen; i++)
        if (g_seen[i] == p)
            return;
    if (g_nseen < (int)(sizeof(g_seen) / sizeof(g_seen[0])))
        g_seen[g_nseen++] = p;
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == 0x7FF0) {          /* WT_PACKET: wParam=serial, lParam=context */
        unsigned char buf[44 * 128];
        int n = WTPacketsGet_((HCTX)lp, 128, buf);
        int i;
        for (i = 0; i < n; i++) {
            const unsigned char *p = buf + (size_t)i * 44;
            int32_t x, y;
            uint32_t btn, press;
            /* pktdef order with Qt's mask: TIME 0, CURSOR 4, BUTTONS 8,
             * X 12, Y 16, Z 20, NPRESSURE 24, TPRESSURE 28, ORIENT 32..43 */
            memcpy(&x, p + 12, 4);
            memcpy(&y, p + 16, 4);
            memcpy(&btn, p + 8, 4);
            memcpy(&press, p + 24, 4);
            g_total++;
            note_pressure(press);
            if (g_printed < 12) {
                printf("  packet x=%-6d y=%-6d btn=0x%x pressure=%u\n",
                       x, y, btn, press);
                g_printed++;
            }
        }
        if (n <= 0)
            printf("  WT_PACKET with empty queue\n");
        return 0;
    }
    if (msg == 0x7FF5) {          /* WT_PROXIMITY: wParam=serial, lParam=enter */
        unsigned char buf[44];
        int n = WTPacketsGet_(g_ctx, 1, buf);
        printf("  proximity %s (packets=%d)\n",
               LOWORD(lp) ? "enter" : "leave", n);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int main(int argc, char **argv)
{
    HMODULE dll;
    WNDCLASSEXW wc;
    HWND hwnd;
    LOGCONTEXTW lc;
    HCTX ctx;
    int seconds = (argc > 1) ? atoi(argv[1]) : 5;
    DWORD deadline;

    setvbuf(stdout, NULL, _IONBF, 0);
    printf("wtprobe: loading wintab32.dll\n");
    dll = LoadLibraryA("wintab32.dll");
    if (!dll) {
        printf("FAIL: LoadLibrary(wintab32) error %lu\n", GetLastError());
        return 2;
    }
    WTInfoW_        = (pWTInfoW)GetProcAddress(dll, "WTInfoW");
    WTOpenW_        = (pWTOpenW)GetProcAddress(dll, "WTOpenW");
    WTClose_        = (pWTClose)GetProcAddress(dll, "WTClose");
    WTPacketsGet_   = (pWTPacketsGet)GetProcAddress(dll, "WTPacketsGet");
    WTEnable_       = (pWTEnable)GetProcAddress(dll, "WTEnable");
    WTOverlap_      = (pWTOverlap)GetProcAddress(dll, "WTOverlap");
    WTGetW_         = (pWTGetW)GetProcAddress(dll, "WTGetW");
    WTQueueSizeGet_ = (pWTQueueSizeGet)GetProcAddress(dll, "WTQueueSizeGet");
    WTQueueSizeSet_ = (pWTQueueSizeSet)GetProcAddress(dll, "WTQueueSizeSet");
    WTShimInfo_     = (pWTShimInfo)GetProcAddress(dll, "WTShimInfo");

    if (!WTInfoW_ || !WTOpenW_ || !WTClose_ || !WTPacketsGet_ || !WTEnable_ ||
        !WTOverlap_ || !WTGetW_ || !WTQueueSizeGet_ || !WTQueueSizeSet_) {
        printf("FAIL: missing exports (Qt needs all 9)\n");
        return 2;
    }

    {
        WCHAR id[128] = {0};
        WORD spec = 0, ndev = 0, ncur = 0;
        UINT n;
        char dev[256] = {0}, path[256] = {0}, present[8] = {0};

        n = WTInfoW_(WTI_INTERFACE, IFC_WINTABID, id);
        printf("WTInfo id: %u bytes -> %ls\n", n, id);
        WTInfoW_(WTI_INTERFACE, IFC_SPECVERSION, &spec);
        WTInfoW_(WTI_INTERFACE, IFC_NDEVICES, &ndev);
        WTInfoW_(WTI_INTERFACE, IFC_NCURSORS, &ncur);
        printf("spec=0x%04x devices=%u cursors=%u\n", spec, ndev, ncur);

        {
            WCHAR dname[256] = {0};
            AXIS ax;
            WTInfoW_(WTI_DEVICES, DVC_NAME, dname);
            printf("device name: %ls\n", dname);
            memset(&ax, 0, sizeof(ax));
            WTInfoW_(WTI_DEVICES, DVC_X, &ax);
            printf("DVC_X        min=%ld max=%ld units=%u\n", ax.axMin, ax.axMax, ax.axUnits);
            memset(&ax, 0, sizeof(ax));
            WTInfoW_(WTI_DEVICES, DVC_Y, &ax);
            printf("DVC_Y        min=%ld max=%ld units=%u\n", ax.axMin, ax.axMax, ax.axUnits);
            memset(&ax, 0, sizeof(ax));
            WTInfoW_(WTI_DEVICES, DVC_NPRESSURE, &ax);
            printf("DVC_NPRESSURE min=%ld max=%ld\n", ax.axMin, ax.axMax);
        }

        if (WTShimInfo_) {
            WTShimInfo_(1, path, sizeof(path));
            WTShimInfo_(2, dev, sizeof(dev));
            WTShimInfo_(3, present, sizeof(present));
    {
        HMODULE hm = GetModuleHandleA("wintab32.dll");
        char mp[MAX_PATH], bi[64];
        mp[0] = 0; bi[0] = 0;
        if (hm) GetModuleFileNameA(hm, mp, sizeof(mp));
        if (WTShimInfo_) WTShimInfo_(4, bi, sizeof(bi));
        printf("dll: %s\nbuild: %s\n", mp[0] ? mp : "(?)", bi[0] ? bi : "(?)");
    }
            printf("shim: dev=%s present=%s node=%s\n", dev, present, path);
        }
    }

    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"WTProbeWnd";
    RegisterClassExW(&wc);
    hwnd = CreateWindowExW(0, L"WTProbeWnd", L"probe", 0, 0, 0, 1, 1,
                           HWND_MESSAGE, NULL, wc.hInstance, NULL);
    if (!hwnd) {
        printf("FAIL: CreateWindow error %lu\n", GetLastError());
        return 2;
    }

    memset(&lc, 0, sizeof(lc));
    WTInfoW_(WTI_DEFSYSCTX, 0, &lc);
    printf("DEFSYSCTX: name=\"%ls\" options=0x%x pktData=0x%x inExt=(%ld,%ld,%ld) outExt=(%ld,%ld,%ld)\n",
           lc.lcName, lc.lcOptions, (unsigned)lc.lcPktData,
           lc.lcInExtX, lc.lcInExtY, lc.lcInExtZ,
           lc.lcOutExtX, lc.lcOutExtY, lc.lcOutExtZ);

    lc.lcOptions |= 0x0004 | 0x0008;            /* CXO_MESSAGES | CXO_CSRMESSAGES */
    lc.lcPktData = PACKETDATA;
    lc.lcPktMode = 0;
    lc.lcMoveMask = PACKETDATA;
    lc.lcBtnDnMask = lc.lcBtnUpMask = 0;
    lc.lcOutOrgX = 0;
    lc.lcOutOrgY = 0;
    lc.lcOutExtX = lc.lcInExtX;
    lc.lcOutExtY = -lc.lcInExtY;

    ctx = WTOpenW_(hwnd, &lc, TRUE);
    g_ctx = ctx;
    printf("WTOpenW -> %p\n", ctx);
    if (!ctx) {
        printf("FAIL: WTOpenW returned NULL\n");
        return 2;
    }
    printf("queue: %d -> ", WTQueueSizeGet_(ctx));
    printf("%d\n", WTQueueSizeSet_(ctx, 128) ? 128 : -1);
    printf("WTEnable=%d WTOverlap=%d\n", WTEnable_(ctx, TRUE), WTOverlap_(ctx, TRUE));

    {
        LOGCONTEXTW back;
        memset(&back, 0, sizeof(back));
        if (WTGetW_(ctx, &back))
            printf("WTGetW: dev=%u pktData=0x%x\n", back.lcDevice, (unsigned)back.lcPktData);
    }

    printf("reading for %d s ...\n", seconds);
    deadline = GetTickCount() + (DWORD)seconds * 1000;
    while ((int)(deadline - GetTickCount()) > 0) {
        MSG msg;
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(5);
    }

    printf("total packets=%d distinct pressures=%d min=%u max=%u\n",
           g_total, g_nseen,
           g_total ? g_pmin : 0, g_total ? g_pmax : 0);
    printf("pressure values:");
    {
        int i;
        for (i = 0; i < g_nseen; i++)
            printf(" %u", g_seen[i]);
        printf("\n");
    }

    {
        POINT cur;
        if (GetCursorPos(&cur))
            printf("cursor after stream: %ld,%ld (virtual desktop %dx%d)\n",
                   cur.x, cur.y,
                   GetSystemMetrics(SM_CXVIRTUALSCREEN),
                   GetSystemMetrics(SM_CYVIRTUALSCREEN));
    }
    printf("WTClose=%d\n", WTClose_(ctx));
    return g_total > 0 ? 0 : 1;
}
