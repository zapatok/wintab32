/*
 * wtlog.c - append-only call log for the wintab32 shim.
 *
 * Default destination: C:\wt.log  (override with WT_LOG=<windows path>).
 * The file is truncated on the first write of each process unless
 * WT_LOG_APPEND=1.  WT_LOG_LEVEL=0..3 controls verbosity (default 2).
 */
#define __USE_MINGW_ANSI_STDIO 1
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wtlog.h"

static CRITICAL_SECTION g_lock;
static INIT_ONCE        g_once = INIT_ONCE_STATIC_INIT;
static HANDLE           g_file = INVALID_HANDLE_VALUE;
static int              g_level = WTLOG_CALL;
static char             g_path[MAX_PATH] = "C:\\wt.log";

static BOOL CALLBACK log_init(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    char buf[64];
    DWORD n;

    (void)once; (void)param; (void)ctx;
    InitializeCriticalSection(&g_lock);

    n = GetEnvironmentVariableA("WT_LOG", g_path, sizeof(g_path));
    if (n == 0 || n >= sizeof(g_path))
        lstrcpynA(g_path, "C:\\wt.log", sizeof(g_path));

    n = GetEnvironmentVariableA("WT_LOG_LEVEL", buf, sizeof(buf));
    if (n > 0 && n < sizeof(buf))
        g_level = atoi(buf);

    n = GetEnvironmentVariableA("WT_LOG_APPEND", buf, sizeof(buf));
    g_file = CreateFileA(g_path,
                         FILE_APPEND_DATA,
                         FILE_SHARE_READ | FILE_SHARE_WRITE,
                         NULL,
                         (n > 0 && buf[0] == '1') ? OPEN_ALWAYS : CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL,
                         NULL);
    if (g_file == INVALID_HANDLE_VALUE)
        return TRUE; /* logging silently disabled; the shim keeps working */

    {
        SYSTEMTIME st;
        char head[200];
        int len;
        GetLocalTime(&st);
        len = snprintf(head, sizeof(head),
                       "=== wintab32 shim start %04u-%02u-%02u %02u:%02u:%02u "
                       "pid=%lu level=%d ===\r\n",
                       st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                       st.wSecond, (unsigned long)GetCurrentProcessId(), g_level);
        if (len > 0)
            WriteFile(g_file, head, (DWORD)len, &n, NULL);
    }
    return TRUE;
}

void wtlog_open(void)
{
    InitOnceExecuteOnce(&g_once, log_init, NULL, NULL);
    if (!g_file)
        g_level = 0;
}

int wtlog_level(void)
{
    wtlog_open();
    return g_level;
}

const char *wtlog_path(void)
{
    return g_path;
}

void wtlog(int level, const char *fmt, ...)
{
    char line[4096];
    int  off;
    DWORD written = 0;
    va_list ap;
    DWORD tid = GetCurrentThreadId();
    DWORD tick;

    wtlog_open();
    if (level > g_level)
        return;

    tick = GetTickCount();
    off = snprintf(line, sizeof(line), "[%6lu.%03lu tid=%5lu] ",
                   (unsigned long)(tick / 1000u), (unsigned long)(tick % 1000u),
                   (unsigned long)tid);
    if (off < 0)
        off = 0;

    va_start(ap, fmt);
    off += vsnprintf(line + off, sizeof(line) - off - 2, fmt, ap);
    va_end(ap);
    if (off < 0)
        off = 0;
    if (off > (int)sizeof(line) - 3)
        off = (int)sizeof(line) - 3;
    line[off++] = '\r';
    line[off++] = '\n';
    line[off]   = 0;

    EnterCriticalSection(&g_lock);
    if (g_file != INVALID_HANDLE_VALUE)
        WriteFile(g_file, line, (DWORD)off, &written, NULL);
    LeaveCriticalSection(&g_lock);
}

void wtlog_close(void)
{
    wtlog_open();
    EnterCriticalSection(&g_lock);
    if (g_file != INVALID_HANDLE_VALUE) {
        CloseHandle(g_file);
        g_file = INVALID_HANDLE_VALUE;
    }
    LeaveCriticalSection(&g_lock);
}
