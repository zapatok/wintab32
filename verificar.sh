#!/usr/bin/env bash
# Autodiagnóstico del shim wintab32. Sólo lee: no instala, no modifica nada.
#
#   ./verificar.sh [RUTA_DEL_PREFIX]
#
# Revisa las cuatro cosas que han fallado de verdad en este proyecto:
#   1. la DLL instalada es la nuestra y es la que dice dist/,
#   2. el override nativo está en el registro del prefix,
#   3. qt.conf tiene nowmpointer junto al .exe (si no, Qt ni carga wintab32),
#   4. el nodo del lápiz se deja abrir por este usuario.
# Sale con 0 si todo está bien, 1 si algo falla.
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
PREFIX="${1:-${WINEPREFIX:-$HOME/.local/share/wineprefixes/harmony25}}"
SYS32="$PREFIX/drive_c/windows/system32"
DLL="$SYS32/wintab32.dll"
DIST="$HERE/dist/wintab32.dll"
ME="$(id -un)"
FAILS=0

ok()   { printf '  ok     %s\n' "$*"; }
bad()  { printf '  FALLA  %s\n' "$*"; FAILS=$((FAILS + 1)); }
warn() { printf '  aviso  %s\n' "$*"; }

echo "== autodiagnóstico: $PREFIX"

# ---------------------------------------------------------------- 1. la DLL
echo "1) DLL instalada"
if [ ! -f "$DLL" ]; then
    bad "no existe $DLL (corre ./instalar.sh)"
elif ! grep -qa 'WTShimInfo' "$DLL" 2>/dev/null; then
    bad "$DLL es el builtin de Wine, no el shim (corre ./instalar.sh)"
else
    size=$(stat -c%s "$DLL")
    ok "es el shim ($size bytes)"
    if [ -f "$DIST" ]; then
        if cmp -s "$DLL" "$DIST"; then
            ok "coincide con dist/ ($size bytes)"
        else
            warn "dist/ es distinta ($(stat -c%s "$DIST") bytes): reinicia Harmony para cargar la nueva"
        fi
    fi
fi

# ----------------------------------------------------------- 2. el override
echo "2) override nativo en el registro"
REG="$PREFIX/user.reg"
if [ ! -f "$REG" ]; then
    bad "no existe $REG (¿prefix equivocado?)"
else
    val=$(grep -E '^[[:space:]]*"wintab32"[[:space:]]*=' "$REG" 2>/dev/null | tail -1 \
          | sed -E 's/.*=[[:space:]]*"([^"]*)".*/\1/')
    case "$val" in
        native|n) ok "wintab32=$val" ;;
        "")       bad "sin override: Wine cargaría su builtin (corre ./instalar.sh)" ;;
        *)        bad "wintab32=$val (debería ser native)" ;;
    esac
fi

# --------------------------------------------------------------- 3. qt.conf
echo "3) qt.conf junto al .exe"
find_dir() {
    local p="$PREFIX" c
    for c in \
        "$p/drive_c/Program Files/ToonBoom/Harmony25/win64/bin" \
        "$p/drive_c/Program Files/ToonBoom/Harmony25/win32/bin" \
        "$p/drive_c/Program Files (x86)/ToonBoom/Harmony25/win32/bin"; do
        [ -f "$c/HarmonyPremium.exe" ] && { echo "$c"; return 0; }
    done
    if [ -f "$HOME/.local/bin/harmony25" ]; then
        c=$(sed -n 's/^[[:space:]]*BIN="\(.*\)".*/\1/p' "$HOME/.local/bin/harmony25" | head -1)
        c="${c//\$WINEPREFIX/$p}"; c="${c//\$HOME/$HOME}"
        [ -n "$c" ] && [ -f "$c/HarmonyPremium.exe" ] && { echo "$c"; return 0; }
    fi
    find "$p" -maxdepth 9 -name HarmonyPremium.exe -printf '%h\n' 2>/dev/null | head -1
}
HDIR="$(find_dir)"
if [ -z "$HDIR" ]; then
    warn "no encontré HarmonyPremium.exe en el prefix (si está en otro sitio, pásalo: ./instalar.sh PREFIX DIR)"
else
    QTC="$HDIR/qt.conf"
    if [ ! -f "$QTC" ]; then
        bad "falta $QTC (Qt 6.4.2 no carga wintab32 sin nowmpointer)"
    elif awk 'BEGIN{s=0} /^\[Platforms\]/{s=1;next} /^\[/{s=0} s && /nowmpointer/{found=1} END{exit !found}' "$QTC"; then
        ok "$QTC tiene windows:nowmpointer"
    else
        bad "$QTC existe pero sin nowmpointer en [Platforms] (corre ./instalar.sh)"
    fi
fi

# --------------------------------------------------------- 4. el nodo real
echo "4) nodo del lápiz en /dev/input"
NODE=""; NNAME=""
for n in /sys/class/input/event*/device/name; do
    name=$(cat "$n" 2>/dev/null) || continue
    case "$name" in
        *[Ww]acom*|*[Ss]tylus*|*[Pp]en*) NODE="/dev/input/$(basename "$(dirname "$(dirname "$n")")")"; NNAME="$name"; break ;;
    esac
done
if [ -z "$NODE" ]; then
    warn "no hay tableta conectada (el shim sigue con el mouse; conecta el lápiz y reintenta)"
elif [ ! -e "$NODE" ]; then
    bad "$NODE no existe (aquí no hay /dev/input: ¿contenedor?)"
elif [ -r "$NODE" ]; then
    ok "$NODE legible (\"$NNAME\")"
else
    bad "$NODE existe pero $ME no puede leerlo"
    echo "         permanente:  sudo $HERE/instalar.sh --udev"
    echo "         por sesión:  sudo usermod -aG input $ME   (cerrar sesión)"
    echo "         ahora mismo: sudo setfacl -m \"u:$ME:r\" $NODE"
fi

# --------------------------------------------------------------- 5. el log
echo "5) último arranque de Harmony"
LOG="$PREFIX/drive_c/wt.log"
if [ -f "$LOG" ]; then
    if grep -qa 'evdev: using ' "$LOG"; then
        ok "$(grep -a 'evdev: using ' "$LOG" | tail -1 | sed 's/^\[[^]]*\] *//')"
    else
        warn "wt.log sin 'evdev: using': la app no llegó a abrir el contexto"
    fi
    grep -qa 'evdev: no tablet device found' "$LOG" && \
        warn "wt.log dice que no encontró tableta (revisa el punto 4)"
else
    warn "no hay $LOG todavía (abre Harmony una vez)"
fi

echo
if [ "$FAILS" -eq 0 ]; then
    echo "== todo en orden."
else
    echo "== $FAILS problema(s). Arriba está el comando exacto de cada uno."
fi
exit $((FAILS > 0))
