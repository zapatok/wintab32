#!/usr/bin/env python3
"""Feed a fake tablet event stream into a FIFO, in the exact evdev wire format.

Used to exercise the shim under Wine in a container that has no /dev/input.

    mkfake.py FIFO [seconds]

Writes one full stylus stroke: proximity in, pressure ramping 0 -> max -> 0,
a barrel-button press, proximity out.  Frame rate follows a 133 Hz pen.
"""
import os, struct, sys, time

EV_SYN, EV_KEY, EV_ABS = 0x00, 0x01, 0x03
SYN_REPORT = 0
ABS_X, ABS_Y, ABS_PRESSURE = 0x00, 0x01, 0x18
BTN_TOOL_PEN, BTN_TOOL_RUBBER = 0x140, 0x141
BTN_TOUCH, BTN_STYLUS = 0x14A, 0x14B

EVENT = struct.Struct("qqHHi")          # struct input_event on x86-64


def ev(t, c, v):
    return EVENT.pack(0, 0, t, c, v)


def stroke(pmax=2047, frames=40, xmax=15200, ymax=9500):
    out = []
    out.append(ev(EV_KEY, BTN_TOOL_PEN, 1))
    out.append(ev(EV_SYN, SYN_REPORT, 0))
    for i in range(frames):
        f = i / (frames - 1.0)
        # triangle: 0 at the ends, max in the middle
        p = int(pmax * (1.0 - abs(2 * f - 1.0)))
        out.append(ev(EV_ABS, ABS_X, int(xmax * f)))
        out.append(ev(EV_ABS, ABS_Y, int(ymax * (0.5 + 0.4 * f))))
        out.append(ev(EV_ABS, ABS_PRESSURE, p))
        out.append(ev(EV_KEY, BTN_TOUCH, 1 if p > 30 else 0))
        out.append(ev(EV_SYN, SYN_REPORT, 0))
    out.append(ev(EV_KEY, BTN_STYLUS, 1))
    out.append(ev(EV_SYN, SYN_REPORT, 0))
    out.append(ev(EV_KEY, BTN_STYLUS, 0))
    out.append(ev(EV_KEY, BTN_TOUCH, 0))
    out.append(ev(EV_KEY, BTN_TOOL_PEN, 0))
    out.append(ev(EV_SYN, SYN_REPORT, 0))
    return b"".join(out)


def main():
    path = sys.argv[1]
    seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 8.0
    data = stroke()
    fd = os.open(path, os.O_WRONLY)      # blocks until the reader connects
    end = time.time() + seconds
    try:
        while time.time() < end:
            fd_w = os.write(fd, data)
            assert fd_w == len(data)
            time.sleep(0.25)             # repeat the stroke ~4x/s
    finally:
        os.close(fd)


if __name__ == "__main__":
    main()
