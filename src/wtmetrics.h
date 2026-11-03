/*
 * wtmetrics.h - latency meter for the WinTab shim.
 *
 * Off by default.  When WT_METRICS is unset every entry point here returns
 * immediately and the reader thread never calls a clock, so the hot path costs
 * one predictable branch.
 *
 *   WT_METRICS=/tmp/wt.csv     CSV destination (default: off)
 *   WT_METRICS_LEVEL=2         1 = counters only, 2 = one row per packet
 *   WT_METRICS_RING=8192       records kept in memory (max 65536)
 */
#ifndef WTMETRICS_H
#define WTMETRICS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Monotonic microseconds since the shim was loaded.  Only call when wtm_on(). */
int64_t wtm_now_us(void);

/* Reads the environment once; safe to call from several threads. */
void wtm_init(void);
int  wtm_on(void);
int  wtm_level(void);

/* One row per packet handed to the application.
 * t_ev_us   kernel timestamp of the SYN_REPORT that produced it (same clock as
 *           the shim; 0 when unknown), t_read_us when the reader decoded it,
 *           t_post_us when the WT_PACKET message went out (0 when coalesced). */
void wtm_record(int64_t t_ev_us, int64_t t_read_us, int64_t t_post_us,
                int queue_len, int posted, int dropped);

void wtm_count_coalesced(void);

/* Writes the CSV and closes it.  Called once, from DLL_PROCESS_DETACH. */
void wtm_close(void);

#ifdef __cplusplus
}
#endif

#endif /* WTMETRICS_H */
