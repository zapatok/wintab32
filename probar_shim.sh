#!/usr/bin/env bash
# Prueba de humo del shim contra la tableta real, fuera de Harmony.
#   ./probar_shim.sh [segundos]
# Abre un contexto WinTab igual que Qt, imprime ejes/presión y resume los
# paquetes. Requiere sesión gráfica (Wayland con XWayland sirve: escribe en
# /dev/input, no en X).
set -uo pipefail
cd "$(dirname "$0")"

PREFIX="${WINEPREFIX:-$HOME/.local/share/wineprefixes/harmony25}"
WINE_BIN="$(command -v wine || command -v wine64 || true)"
SECS="${1:-10}"
LOG="$PREFIX/drive_c/wt.log"

[ -n "$WINE_BIN" ] || { echo "no encontré wine en PATH"; exit 1; }
[ -f tools/wtprobe.exe ] || { echo "falta tools/wtprobe.exe"; exit 1; }

rm -f "$LOG"
echo "== $($WINE_BIN --version 2>/dev/null) · prefix $PREFIX"

WINEPREFIX="$PREFIX" WINEDLLOVERRIDES="wintab32=n" WINEDEBUG=-all \
WT_LOG='C:\wt.log' WT_LOG_LEVEL=3 \
    "$WINE_BIN" tools/wtprobe.exe "$SECS"
RC=$?

echo "== exit=$RC"
if [ -f "$LOG" ]; then
    echo "== $LOG (arranque y llamadas)"
    sed -n '1,14p' "$LOG"
    echo "== diagnóstico de /dev/input"
    grep -E "evdev: (candidate|using|scanned|no /dev/input|forced)" "$LOG" | head -8
    echo "== cola de llamadas WinTab"
    grep -E "WTOpenW name|WTOpenW\(|WTQueueSize|WTEnable" "$LOG" | head -6
    echo "== paquetes registrados: $(grep -c 'evdev packet' "$LOG" || echo 0)"
    echo "== presión vista: $(grep -o 'evdev packet.*p=[0-9]*' "$LOG" | sed 's/.*p=//' | sort -n | uniq | tr '\n' ' ')"
else
    echo "!! no se escribió $LOG"
fi

if [ -f "$LOG" ] && ! grep -q "evdev: using" "$LOG"; then
    cat <<EOF
== 0 paquetes y el shim no abrió ninguna tableta.
   Causa más probable: permisos de /dev/input.
     id -nG | tr ' ' '\n' | grep -x input     # ¿aparece "input"?
   Permanente:  sudo usermod -aG input $USER   y reiniciar sesión.
   Para probar YA sin cerrar sesión:
     sudo setfacl -m "u:$USER:r" /dev/input/event20
EOF
fi
exit $RC
