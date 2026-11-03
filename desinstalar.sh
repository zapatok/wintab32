#!/usr/bin/env bash
# Quita el shim, el override y el qt.conf que puso instalar.sh.
set -euo pipefail

RULE=/etc/udev/rules.d/70-wacom-wintab.rules
if [ "${1:-}" = "--udev" ]; then
    [ "$(id -u)" = 0 ] || { echo "ERROR: hace falta root:  sudo $0 --udev"; exit 1; }
    rm -f "$RULE" /etc/udev/rules.d/99-wacom-wintab.rules
    udevadm control --reload-rules
    udevadm trigger --subsystem-match=input --action=change
    echo "== regla udev eliminada"
    exit 0
fi

PREFIX="${1:-${WINEPREFIX:-$HOME/.local/share/wineprefixes/harmony25}}"
HARMONY_DIR="${2:-${HARMONY_DIR:-}}"
SYS32="$PREFIX/drive_c/windows/system32"
DST="$SYS32/wintab32.dll"
BACKUP="$SYS32/wintab32.dll.wine-builtin"

echo "== prefix: $PREFIX"

WINE_BIN="$(command -v wine || command -v wine64 || true)"
if [ -n "$WINE_BIN" ]; then
    WINEPREFIX="$PREFIX" "$WINE_BIN" reg delete 'HKCU\Software\Wine\DllOverrides' \
        /v wintab32 /f >/dev/null 2>&1 \
        && echo "== override borrado" \
        || echo "== el override no estaba"
fi

if [ -f "$BACKUP" ]; then
    mv -f "$BACKUP" "$DST"
    echo "== builtin restaurado ($(stat -c%s "$DST") bytes)"
else
    rm -f "$DST"
    echo "== sin respaldo: quité el shim (Wine regenera el builtin con 'wineboot -u')"
fi

if [ -z "$HARMONY_DIR" ]; then
    for cand in \
        "$PREFIX/drive_c/Program Files/ToonBoom/Harmony25/win64/bin" \
        "$PREFIX/drive_c/Program Files (x86)/ToonBoom/Harmony25/win64/bin"
    do
        [ -f "$cand/HarmonyPremium.exe" ] && { HARMONY_DIR="$cand"; break; }
    done
fi
[ -z "$HARMONY_DIR" ] && HARMONY_DIR="$(find "$HOME" "$PREFIX" -maxdepth 9 -name HarmonyPremium.exe -printf '%h\n' 2>/dev/null | head -1)"

if [ -n "$HARMONY_DIR" ] && [ -f "$HARMONY_DIR/qt.conf" ]; then
    QTCONF="$HARMONY_DIR/qt.conf"
    if [ -f "$QTCONF.wintab-shim" ] && cmp -s "$QTCONF" "$QTCONF.wintab-shim"; then
        rm -f "$QTCONF" "$QTCONF.wintab-shim"
        echo "== qt.conf eliminado (era solo nuestro)"
    elif [ -f "$QTCONF.antes-shim" ]; then
        mv -f "$QTCONF.antes-shim" "$QTCONF"
        echo "== qt.conf restaurado a su versión previa"
    else
        echo "!! $QTCONF quedó con el bloque [Platforms]; quítalo a mano si molesta"
    fi
fi
echo "== hecho"
