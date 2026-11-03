#!/usr/bin/env bash
# medir_latencia.sh - corre la app (o el probe) con el medidor activo y resume.
#
#   ./tools/medir_latencia.sh              # abre Harmony; dibuja y cierra
#   ./tools/medir_latencia.sh 20           # idem, pero el probe dura 20 s
#   ./tools/medir_latencia.sh --probe 20   # sólo wtprobe, sin Harmony
#   ./tools/medir_latencia.sh --coalesce   # activa WT_COALESCE=1 para el A/B
#
# El CSV se escribe al cerrar la app (DLL_PROCESS_DETACH): mientras dibujas no
# hay fichero.  Ruta: $WT_METRICS_OUT o /tmp/wt-latencia.csv
set -uo pipefail
cd "$(dirname "$0")/.."

MODE=harmony
SECS=10
COALESCE=0
for a in "$@"; do
    case "$a" in
        --probe)    MODE=probe ;;
        --coalesce) COALESCE=1 ;;
        ''|*[!0-9]*) echo "argumento no reconocido: $a"; exit 2 ;;
        *)          SECS=$a ;;
    esac
done

CSV="${WT_METRICS_OUT:-/tmp/wt-latencia.csv}"
LOG="${WINEPREFIX:-$HOME/.local/share/wineprefixes/harmony25}/drive_c/wt.log"
rm -f "$CSV"

export WT_METRICS="$CSV"
export WT_METRICS_LEVEL="${WT_METRICS_LEVEL:-2}"
export WT_COALESCE="$COALESCE"
export WT_LOG_LEVEL="${WT_LOG_LEVEL:-2}"

echo "== medidor: $CSV (level $WT_METRICS_LEVEL, coalesce=$COALESCE)"

if [ "$MODE" = probe ]; then
    ./probar_shim.sh "$SECS"
else
    [ -x "$HOME/.local/bin/harmony25" ] || { echo "falta ~/.local/bin/harmony25"; exit 1; }
    echo "== abriendo Harmony. Dibuja al menos ${SECS} s con presión variable"
    echo "   y luego cierra la aplicación: el CSV se escribe al salir."
    "$HOME/.local/bin/harmony25"
    echo "== Harmony cerró"
fi

if [ ! -s "$CSV" ]; then
    cat <<EOF
!! no hay CSV en $CSV
   El medidor se escribe al descargar la DLL.  Causas típicas:
     - WT_METRICS no llegó al proceso: comprueba con WT_LOG_LEVEL=3 que el log
       diga "metrics: on, level 2, ...".
     - Harmony no cargó el shim: revisa que qt.conf tenga nowmpointer.
     - La app se mató (SIGKILL) en vez de cerrarse.
EOF
    exit 1
fi

echo
python3 tools/wtmetrics.py "$CSV" --per-second
echo
echo "== CSV: $CSV"
if [ -f "$LOG" ]; then
    echo "== paquetes en el log: $(grep -ac '^\[[^]]*\] packet ' "$LOG" 2>/dev/null || true) (sólo se escriben con WT_LOG_LEVEL=3)"
    grep -E "metrics: wrote|shim: packet coalescing|evdev: using" "$LOG" | tail -3
fi
