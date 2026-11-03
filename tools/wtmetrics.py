#!/usr/bin/env python3
"""wtmetrics.py - turn a WT_METRICS CSV into latency numbers.

    ./tools/wtmetrics.py /tmp/wt.csv
    ./tools/wtmetrics.py /tmp/wt.csv --per-second

Columns written by the shim (src/wtmetrics.c):

    n          row number
    epoch_us   monotonic clock of the shim (microseconds since load)
    t_ev_us    kernel timestamp of the SYN_REPORT, same clock (0 = unknown)
    t_read_us  when the reader decoded the packet
    t_post_us  when WT_PACKET was posted (0 = coalesced into a later message)
    d_kernel_us  t_read_us - t_ev_us   (0 when the kernel timestamp is unknown)
    d_shim_us    t_post_us - t_read_us (0 when the packet was coalesced)
    queue      packets pending in the context after the push
    posted     1 = this packet carried a message
    dropped    1 = the queue was full and the oldest packet was lost
"""
from __future__ import annotations

import argparse
import csv
import statistics
import sys
from pathlib import Path

INT_COLS = ("n", "epoch_us", "t_ev_us", "t_read_us", "t_post_us",
            "d_kernel_us", "d_shim_us", "queue", "posted", "dropped")


def pct(values: list[int], q: float) -> float:
    """Nearest-rank percentile; q in 0..100."""
    if not values:
        return 0.0
    s = sorted(values)
    k = max(0, min(len(s) - 1, int(round((q / 100.0) * (len(s) - 1)))))
    return float(s[k])


def load(path: Path) -> list[dict[str, int]]:
    with path.open(newline="") as f:
        r = csv.DictReader(f)
        if r.fieldnames is None or "d_shim_us" not in r.fieldnames:
            sys.exit(f"{path}: not a WT_METRICS level-2 CSV (header: {r.fieldnames})")
        rows = []
        for raw in r:
            try:
                rows.append({c: int(raw[c]) for c in INT_COLS})
            except (KeyError, TypeError, ValueError):
                continue
    return rows


def ms(us: float) -> str:
    return f"{us / 1000.0:.3f} ms"


def visible(rows: list[dict[str, int]]) -> list[int]:
    """t_post of the message that actually carried each packet, minus t_read.

    A coalesced packet is only seen by Qt when a later packet posts a message,
    so d_shim_us (0 for those rows) understates their delay.  This walks the
    recorded order and charges each packet the post time of its carrier.
    """
    carrier = [0] * len(rows)
    nxt = 0
    for i in range(len(rows) - 1, -1, -1):
        if rows[i]["posted"]:
            nxt = rows[i]["t_post_us"]
        carrier[i] = nxt
    return [carrier[i] - rows[i]["t_read_us"] for i in range(len(rows)) if carrier[i]]


def report(rows: list[dict[str, int]], per_second: bool) -> None:
    if not rows:
        sys.exit("no rows")

    posted = [r for r in rows if r["posted"]]
    coalesced = [r for r in rows if not r["posted"]]
    d_shim = [r["d_shim_us"] for r in posted]
    d_kern = [r["d_kernel_us"] for r in rows if r["t_ev_us"] > 0]
    span_us = rows[-1]["t_read_us"] - rows[0]["t_read_us"]
    span_s = span_us / 1e6 if span_us > 0 else 0.0

    print(f"rows          {len(rows)}  ({len(posted)} posted, {len(coalesced)} coalesced)")
    print(f"window        {span_s:.2f} s")
    if span_s > 0:
        print(f"packet rate   {len(rows) / span_s:.1f} pkt/s")
        print(f"message rate  {len(posted) / span_s:.1f} msg/s")
    print(f"queue max     {max(r['queue'] for r in rows)}")
    print(f"dropped       {sum(r['dropped'] for r in rows)}")

    print("\nd_shim_us  (reader -> PostMessage; the shim's own cost)")
    if d_shim:
        print(f"  p50 {ms(pct(d_shim, 50)):>10}   p95 {ms(pct(d_shim, 95)):>10}"
              f"   p99 {ms(pct(d_shim, 99)):>10}   max {ms(max(d_shim)):>10}")
        print(f"  mean {ms(statistics.fmean(d_shim)):>8}   "
              f"first 3 messages: "
              + ", ".join(ms(v) for v in d_shim[:3]))
    else:
        print("  no posted packets")

    vis = visible(rows)
    if vis:
        print("\nd_visible_us (reader -> the message that carries it, coalesced included)")
        print(f"  p50 {ms(pct(vis, 50)):>10}   p95 {ms(pct(vis, 95)):>10}"
              f"   p99 {ms(pct(vis, 99)):>10}   max {ms(max(vis)):>10}")

    print("\nd_kernel_us (evdev timestamp -> reader; kernel + scheduling)")
    if d_kern:
        print(f"  p50 {ms(pct(d_kern, 50)):>10}   p95 {ms(pct(d_kern, 95)):>10}"
              f"   p99 {ms(pct(d_kern, 99)):>10}   max {ms(max(d_kern)):>10}")
    else:
        print("  not measured (device reports no timestamp, as in the FIFO bench)")

    if per_second and span_s >= 2:
        print("\nper second: pkt  msg  d_shim p95  queue max")
        buckets: dict[int, list[dict[str, int]]] = {}
        t0 = rows[0]["t_read_us"]
        for r in rows:
            buckets.setdefault((r["t_read_us"] - t0) // 1_000_000, []).append(r)
        for sec in sorted(buckets):
            b = buckets[sec]
            p = [r["d_shim_us"] for r in b if r["posted"]]
            print(f"  {sec:>3}s      {len(b):>4} {len(p):>4}  "
                  f"{ms(pct(p, 95)) if p else '-':>11}  {max(r['queue'] for r in b):>3}")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("csv", type=Path)
    ap.add_argument("--per-second", action="store_true",
                    help="break the window down second by second")
    a = ap.parse_args()
    report(load(a.csv), a.per_second)


if __name__ == "__main__":
    main()
