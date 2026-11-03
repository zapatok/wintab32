/*
 * evdev.h - Linux input layer for the WinTab shim.
 *
 * Runs inside a Windows PE (mingw-w64).  Devices are opened through the Win32
 * file API (CreateFileA/ReadFile on Z:\dev\input\eventN): raw syscalls are
 * trapped by Wine's dispatcher (STATUS_NOT_IMPLEMENTED) and ioctls are not
 * available to a PE, so the sysfs name is the only metadata we depend on.
 */
#ifndef EVDEV_H
#define EVDEV_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int      x, y, z;      /* raw absolute device coordinates */
    int      pressure;     /* already scaled to the WinTab range */
    unsigned buttons;      /* bit0 tip, bit1 stylus, bit2 stylus2 */
    unsigned cursor;       /* 1 = pen, 2 = eraser */
    int64_t  t_ev_us;      /* kernel SYN timestamp, metric clock (0 = unknown) */
    int64_t  t_read_us;    /* when the reader decoded it (0 = metrics off) */
} EV_PKT;

typedef void (*EV_ON_PACKET)(const EV_PKT *pkt);
typedef void (*EV_ON_PROXIMITY)(int enter, const EV_PKT *pkt);

/* Register the sink before calling evdev_start(). */
void evdev_set_callbacks(EV_ON_PACKET on_packet, EV_ON_PROXIMITY on_prox);

/* Idempotent.  Returns 1 when the reader thread is running. */
int  evdev_start(void);
void evdev_stop(void);

/* Introspection for WTInfo / logging. */
int  evdev_has_device(void);
const char *evdev_path(void);
const char *evdev_name(void);
void evdev_ranges(int *xmin, int *xmax, int *ymin, int *ymax);
int  evdev_pressure_range(void);

/* Current device state, for priming a freshly opened WinTab context. */
int  evdev_in_proximity(void);
int  evdev_snapshot(EV_PKT *out);

/* While the pen is in range, the pointer's mouse messages duplicate what the
 * tablet already reports; the filter drops them so the app sees one device. */
void evdev_set_instance(void *hinst);
int  evdev_mouse_filter(int on);

#ifdef __cplusplus
}
#endif

#endif /* EVDEV_H */
