#!/usr/bin/env bash
# Build the WinTab shim as a 64-bit Windows DLL with mingw-w64.
set -euo pipefail
cd "$(dirname "$0")"

CC=${CC:-x86_64-w64-mingw32-gcc}
CFLAGS=${CFLAGS:--O2 -g -Wall -Wextra -Wno-cast-function-type -municode}
mkdir -p dist

SRC="src/wintab32.c src/evdev.c src/wtlog.c src/wtmetrics.c"

$CC $CFLAGS -shared -o dist/wintab32.dll $SRC wintab32.def \
    -Wl,--enable-stdcall-fixup -Wl,--subsystem,windows \
    -luser32 -lkernel32 -ladvapi32

echo "built: dist/wintab32.dll ($(stat -c%s dist/wintab32.dll) bytes)"

# probe de prueba (imita a Qt: mismos exports, mismo contexto)
$CC ${PROBE_CFLAGS:--O2 -g -Wall} -o tools/wtprobe.exe tools/wtprobe.c \
    -luser32 -lkernel32 -Wl,--subsystem,console
echo "built: tools/wtprobe.exe ($(stat -c%s tools/wtprobe.exe) bytes)"
$CC -dumpmachine
file dist/wintab32.dll 2>/dev/null || true
