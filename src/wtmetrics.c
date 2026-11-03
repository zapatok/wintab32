/*
 * wtmetrics.c - latency meter for the WinTab shim.
 *
 * What is measured: the time the shim spends between decoding a packet and
 * handing it to the application (WT_PACKET), plus the time the packet spent in
 * the kernel queue before we read it.  Nothing else: the app side (Qt, the
 * stroke engine) is outside this instrument.
 *
 * Design notes
 *  - Single producer (the evdev reader thread) writes the ring, the consumer is
 *    wtm_close() after the reader has stopped, so no lock and no allocation on
 *    the hot path.
 *  - The ring is a fixed array: at 200 Hz it covers ~40 s, enough for a stroke.
 *    wtm_close() writes whatever is in it, oldest first.
 *  - The kernel timestamp in struct input_event is CLOCK_REALTIME; the shim
 *    keeps CLOCK_MONOTONIC (QPC).  The offset between both clocks is refreshed
 *    once per read() and applied per packet, so d_kernel_us carries the
 *    sampling error of that refresh (tens of microseconds), while d_shim_us is
 *    exact: both ends are the same clock.
 */
#define __USE_MINGW_ANSI_STDIO 1
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wtmetrics.h"
#include "wtlog.h"

#define WTM_DEFAULT_RING 8192
#define WTM_MAX_RING     65536

typedef struct {
    int64_t t_ev;      /* kernel SYN_REPORT timestamp, same clock as t_read */
    int64_t t_read;    /* reader decoded it */
    int64_t t_post;    /* WT_PACKET posted (0 when coalesced away) */
    int     queue;     /* packets pending in the context after the push */
    int     posted;
    int     dropped;   /* the queue was full on this push */
} WTM_REC;

static WTM_REC  g_ring[WTM_DEFAULT_RING];
static int      g_ring_cap = WTM_DEFAULT_RING;
static volatile LONG g_head;

static int      g_inited;
static int      g_on;
static int      g_level = 2;
static char     g_path[MAX_PATH];

static LARGE_INTEGER g_freq;
static LARGE_INTEGER g_t0;

static LONG     g_packets;     /* rows recorded */
static LONG     g_posts;       /* WT_PACKET messages sent */
static LONG     g_coalesced;   /* packets carried by an earlier message */

/* ------------------------------------------------------------------ */

int64_t wtm_now_us(void)
{
    LARGE_INTEGER c;
    int64_t d;

    if (!g_freq.QuadPart)
        QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&c);
    d = (int64_t)(c.QuadPart - g_t0.QuadPart);
    if (!g_freq.QuadPart)
        return 0;
    /* Divide first: d * 1e6 overflows after ~10 days of uptime. */
    return (d / g_freq.QuadPart) * 1000000 +
           ((d % g_freq.QuadPart) * 1000000) / g_freq.QuadPart;
}

int wtm_on(void)    { return g_on; }
int wtm_level(void) { return g_level; }

void wtm_init(void)
{
    const char *p;
    const char *lvl;

    if (g_inited)
        return;
    g_inited = 1;

    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_t0);

    p = getenv("WT_METRICS");
    if (!p || !*p) {
        g_on = 0;
        return;
    }
    lvl = getenv("WT_METRICS_LEVEL");
    if (lvl && *lvl) {
        int v = atoi(lvl);
        g_level = (v <= 1) ? 1 : 2;
    }
    if (g_level >= 2) {
        const char *ring = getenv("WT_METRICS_RING");
        if (ring && *ring) {
            int v = atoi(ring);
            if (v > 0 && v <= WTM_MAX_RING) {
                g_ring_cap = v;
                if (g_ring_cap > WTM_DEFAULT_RING) {
                    wtlog(WTLOG_INIT,
                          "metrics: ring window capped at %d records", WTM_DEFAULT_RING);
                    g_ring_cap = WTM_DEFAULT_RING;
                }
            }
        }
    }

    if (strlen(p) >= sizeof(g_path) - 3) {
        wtlog(WTLOG_INIT, "metrics: WT_METRICS path too long, disabled");
        return;
    }
    /* "/tmp/x.csv" inside a PE means Z:\tmp\x.csv. */
    if (p[0] == '/' || (p[0] && p[1] == ':'))
        snprintf(g_path, sizeof(g_path), "%s%s", (p[0] == '/') ? "Z:" : "", p);
    else
        snprintf(g_path, sizeof(g_path), "%s", p);

    g_on = 1;
    wtlog(WTLOG_INIT, "metrics: on, level %d, %s", g_level, g_path);
}

void wtm_record(int64_t t_ev_us, int64_t t_read_us, int64_t t_post_us,
                int queue_len, int posted, int dropped)
{
    WTM_REC *r;
    LONG slot;

    if (!g_on)
        return;

    g_packets++;
    if (posted)
        g_posts++;

    if (g_level < 2)
        return;

    slot = g_head;
    r = &g_ring[slot % g_ring_cap];
    r->t_ev    = t_ev_us;
    r->t_read  = t_read_us;
    r->t_post  = t_post_us;
    r->queue   = queue_len;
    r->posted  = posted;
    r->dropped = dropped ? 1 : 0;
    g_head = slot + 1;                 /* single writer: plain store is enough */
}

void wtm_count_coalesced(void)
{
    if (g_on)
        g_coalesced++;
}

/* ------------------------------------------------------------------ */

static void csv_header(FILE *f)
{
    fprintf(f, "n,epoch_us,t_ev_us,t_read_us,t_post_us,"
               "d_kernel_us,d_shim_us,queue,posted,dropped\n");
}

static void csv_summary(FILE *f)
{
    fprintf(f, "packets,posts,coalesced,level,elapsed_us\n");
    fprintf(f, "%ld,%ld,%ld,%d,%lld\n",
            (long)g_packets, (long)g_posts, (long)g_coalesced, g_level,
            (long long)wtm_now_us());
}

void wtm_close(void)
{
    FILE *f;
    LONG head, first, i;

    if (!g_on) {
        g_inited = 0;
        return;
    }
    g_on = 0;

    f = fopen(g_path, "wb");
    if (!f) {
        wtlog(WTLOG_INIT, "metrics: cannot write %s", g_path);
        g_inited = 0;
        return;
    }

    if (g_level < 2) {
        csv_summary(f);
        fclose(f);
        wtlog(WTLOG_INIT, "metrics: %ld packets, %ld posts, %ld coalesced",
              (long)g_packets, (long)g_posts, (long)g_coalesced);
        g_inited = 0;
        return;
    }

    head  = g_head;
    first = (head > g_ring_cap) ? (head - g_ring_cap) : 0;

    csv_header(f);
    for (i = first; i < head; i++) {
        const WTM_REC *r = &g_ring[i % g_ring_cap];
        int64_t d_kernel = (r->t_ev && r->t_read) ? (r->t_read - r->t_ev) : 0;
        int64_t d_shim   = r->t_post ? (r->t_post - r->t_read) : 0;
        fprintf(f, "%ld,%lld,%lld,%lld,%lld,%lld,%lld,%d,%d,%d\n",
                (long)(i - first + 1),
                (long long)r->t_read,
                (long long)r->t_ev,
                (long long)r->t_read,
                (long long)r->t_post,
                (long long)d_kernel,
                (long long)d_shim,
                r->queue, r->posted, r->dropped);
    }
    fclose(f);

    wtlog(WTLOG_INIT, "metrics: wrote %ld rows (level %d), %ld posts, %ld coalesced: %s",
          (long)(head - first), g_level, (long)g_posts, (long)g_coalesced, g_path);

    g_inited = 0;
}
