# wintab32 for Wine — WinTab 1.4 over evdev

A replacement `wintab32.dll` for Wine on Linux that serves the WinTab 1.4 API
straight from `/dev/input`, so pen pressure and tilt reach Windows applications
that expect a digitizer.

## Why this exists

Wine's own `wintab32` builds its device list with `XListInputDevices` (XInput 1).
Under Wayland, XWayland answers with virtual pointers only — the tablet never
appears — so Wine reports zero devices and applications see no pen:

```
wintab32:load_tablet_info XListInputDevices reports 12 devices
Device 0: Virtual core pointer
Device 4: xwayland-pointer:11
Device 5: xwayland-relative-pointer:11
Device 6: xwayland-pointer-gestures:11
Device 7: xwayland-keyboard:11
```

No tablet in that list. This DLL skips X entirely and talks to evdev.

## What it does

- Implements the WinTab 1.4 surface: `WTInfo`, `WTOpen/Close/Enable/Disable`,
  `WTPacketsGet`, `WTPacket`, `WTQueueSize*`, `WTGet/Set`, cursor and context
  management, `WTDataGet`/`WTDataPeek` for the packet queue.
- Reads `ABS_PRESSURE`, `ABS_TILT_X/Y`, `ABS_X/Y`, `BTN_TOOL_PEN`, `BTN_TOUCH`
  and `BTN_STYLUS` from the evdev node it detects as a tablet.
- Advertises one or more devices depending on what evdev reports, with a
  `WTI_DEFCONTEXT` whose `lcInExt` matches the device range (Qt scales with
  `lcInExt` and ignores `lcOutExt`, same as Wine).
- Logs every entry point to `C:\wt.log`. That log is how the real call
  sequence of the host application is discovered — some applications load
  WinTab dynamically and never mention it in their imports.

## Status

Works. Verified on a Wacom Intuos PT S 2 under Wine 11.20 staging, Wayland/KDE:
962 packets with 64 distinct pressure levels, `WTGetW pktData=0x1fe4`, one
`WTOpenW` per run.

Two non-obvious things make it work, and skipping either leaves the DLL dead:

1. **Qt must be told to skip WM_POINTER.** Qt 6.4.2 only calls
   `initTablet()` when `initPointer()` fails, and `initPointer()` only fails
   with the `DontUseWMPointer` option. Put next to the executable:

   ```ini
   [Platforms]
   WindowsArguments = nowmpointer
   ```

   `qt.conf` works; `QT_QPA_PLATFORM=windows:nowmpointer` does **not**, because
   Wine strips `QT_*` variables before building the Windows process environment.

2. **The tablet must reach the shim once, not twice.** Under XWayland the pen
   arrives as both tablet and synthesized mouse; while the two positions differ,
   the host alternates between them inside a single stroke.

## Build

Requires `mingw-w64` (x86_64) and `make`-free simplicity:

```bash
./build.sh            # produces dist/wintab32.dll
```

## Install

```bash
./instalar.sh --todo  # DLL + override + qt.conf + the udev rule (asks sudo once)
```

Without `--todo` the udev rule is left out, and the pen may be unreadable
(`/dev/input` is root-only by default). Then, at any moment:

```bash
./verificar.sh        # read-only: says which of the five pieces is missing
```

Manual equivalent:

```bash
cp dist/wintab32.dll "$WINEPREFIX/drive_c/windows/system32/"
WINEPREFIX="$WINEPREFIX" wine reg add \
  'HKEY_CURRENT_USER\Software\Wine\DllOverrides' /v wintab32 /d native /f
printf '[Platforms]\nWindowsArguments = nowmpointer\n' \
  > "$WINEPREFIX/drive_c/Program Files/ToonBoom/Harmony25/win64/bin/qt.conf"
```

Uninstall with `./desinstalar.sh`, which removes the DLL and the override.

## Diagnose

```bash
./verificar.sh                   # five checks, exact fix per failure
./probar_shim.sh                 # runs the probe app under the prefix
WT_LOG_LEVEL=3 wine yourapp.exe  # verbose log to C:\wt.log
```

`tools/wtprobe.exe` enumerates devices, opens a context and dumps packets, so
you can separate "the shim is broken" from "the app never loaded it".

Reading the log:

| line | meaning |
|---|---|
| `DllMain Initialization` | the DLL was loaded |
| `load_tablet_info … reports N devices` | evdev scan finished |
| `WTPacketsGet n=…` | the app is pulling packets |
| nothing at all | nobody loaded the DLL — the shim is not the problem |

## Latency

There is an optional meter, off by default, that records every packet and dumps a
CSV when the process unloads the DLL:

```bash
WT_METRICS=/tmp/wt.csv ./probar_shim.sh 10
python3 tools/wtmetrics.py /tmp/wt.csv --per-second
./tools/medir_latencia.sh              # opens the app, draw, close, get numbers
```

Measured on a FIFO bench (Wine 8, Xvfb): 8 µs per packet of shim cost, but 207 µs
with `WT_POS_CURSOR` on — `GetCursorPos()` is an X round-trip per packet.
`WT_POS_CACHE=1` queries the pointer once per read burst instead: same
coordinates, back to 8 µs. Message coalescing (`WT_COALESCE=1`) cuts messages 7x
but adds a wait tail, and the queue never went above 43 of 128 with zero drops,
so it stays off. Numbers, commands and caveats: [docs/LATENCIA.md](docs/LATENCIA.md).

## Layout

```
src/       wintab32.c, evdev.c, wtapi.h, evdev.h, wtlog.c, wtmetrics.c
ref/       Wine's own wintab32 + Qt's tablet support, for reference
tools/     wtprobe.c (probe), mkfake.py (synthetic device),
           wtmetrics.py (CSV -> percentiles), medir_latencia.sh
docs/      LATENCIA.md (what the shim costs, measured)
dist/      built DLL
build.sh   mingw-w64 build
```

## Permissions

Reading `/dev/input/eventN` requires root or membership of the `input` group:

```bash
sudo usermod -aG input "$USER"      # then log out and back in
```

Or a udev rule with `TAG+="uaccess"`.

## License

MIT. See [LICENSE](LICENSE).

WinTab is a specification by LCS/Telegraphics; `ref/` contains upstream files
kept only as reference and carry their own licenses.
