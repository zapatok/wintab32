#!/usr/bin/env bash
# Instala el shim wintab32 nativo para Harmony 25.
#
#   ./instalar.sh [RUTA_DEL_PREFIX] [DIR_DE_HARMONY]
#   sudo ./instalar.sh --udev     # sólo instala la regla udev de /dev/input
#
# Hace tres cosas:
#   1. copia dist/wintab32.dll a system32 del prefix (respalda el builtin),
#   2. deja el override nativo en el registro del prefix,
#   3. escribe qt.conf junto a HarmonyPremium.exe con
#      [Platforms] WindowsArguments = nowmpointer
#      Sin eso Qt 6.4.2 nunca carga wintab32 (prefiere WM_POINTER).
#
# No toca el .exe, ni sus DLL, ni el lanzador del menú.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
RULE_DIR="${WT_UDEV_DIR:-/etc/udev/rules.d}"
RULE="$RULE_DIR/70-wacom-wintab.rules"

if [ "${1:-}" = "--udev" ]; then
    [ "$(id -u)" = 0 ] || { echo "ERROR: la regla udev necesita root:  sudo $0 --udev"; exit 1; }
    rm -f "$RULE_DIR/99-wacom-wintab.rules"   # versión anterior, orden equivocado
    cat > "$RULE" <<'RULE_EOF'
# wintab32 shim (Harmony 25 bajo Wine): permite al usuario de la sesión
# leer el nodo del lápiz.  "uaccess" lo gestiona systemd-logind, así que
# sobrevive a reconexiones de la tableta y a reinicios.
SUBSYSTEM=="input", KERNEL=="event*", ATTRS{name}=="Wacom*", TAG+="uaccess", MODE="0664"
RULE_EOF
    if command -v udevadm >/dev/null 2>&1; then
        udevadm control --reload-rules
        udevadm trigger --subsystem-match=input
        sleep 1
    else
        echo "!! sin udevadm: la regla se aplicará al reconectar la tableta o al reiniciar"
    fi
    echo "== regla instalada: $RULE"
    echo "   comprueba con:  getfacl -p /dev/input/event20"
    exit 0
fi

# --todo: instala todo, incluida la regla udev (pide sudo una vez) y se acabó.
CON_UDEV=0
[ "${1:-}" = "--todo" ] && { CON_UDEV=1; shift; }

PREFIX="${1:-${WINEPREFIX:-$HOME/.local/share/wineprefixes/harmony25}}"
SYS32="$PREFIX/drive_c/windows/system32"
DST="$SYS32/wintab32.dll"
BACKUP="$SYS32/wintab32.dll.wine-builtin"
SRC="$HERE/dist/wintab32.dll"

echo "== prefix: $PREFIX"
[ -f "$SRC" ] || { echo "ERROR: falta $SRC; corre ./build.sh primero"; exit 1; }
[ -d "$SYS32" ] || { echo "ERROR: no existe $SYS32 (¿prefix equivocado?)"; exit 1; }

# ---------------------------------------------------------------- 1. la DLL
# Our DLL exports WTShimInfo, and PE export names are stored as ASCII; the
# WTInfo id string is UTF-16LE, where a plain grep does not find it.
is_ours() {
    grep -qa 'WTShimInfo' "$1" 2>/dev/null && return 0
    command -v grep >/dev/null && grep -qaP 'T\x00o\x00o\x00n\x00B\x00' "$1" 2>/dev/null
}

if [ -f "$DST" ] && ! is_ours "$DST"; then
    [ -f "$BACKUP" ] || { cp -p "$DST" "$BACKUP"; echo "== respaldo del builtin: $BACKUP"; }
fi
install -m 0644 "$SRC" "$DST"
echo "== DLL: $DST ($(stat -c%s "$DST") bytes)"

# ------------------------------------------------------- 2. override nativo
WINE_BIN="$(command -v wine || command -v wine64 || true)"
if [ -n "$WINE_BIN" ]; then
    if WINEPREFIX="$PREFIX" "$WINE_BIN" reg add 'HKCU\Software\Wine\DllOverrides' \
            /v wintab32 /d native /f >/dev/null 2>&1; then
        echo "== registro: HKCU\\Software\\Wine\\DllOverrides\\wintab32 = native"
    else
        echo "!! no se pudo escribir el registro; usa WINEDLLOVERRIDES=\"wintab32=n\""
    fi
else
    echo "!! no encontré wine en PATH; agrega WINEDLLOVERRIDES=\"wintab32=n\" al lanzador"
fi

# ------------------------------------------------------------- 3. qt.conf
# Directorio real de HarmonyPremium.exe. En esta instalación vive DENTRO del
# prefix: drive_c/Program Files/ToonBoom/Harmony25/win64/bin
find_harmony_dir() {
    local pfx="$1" cand line
    for cand in \
        "$pfx/drive_c/Program Files/ToonBoom/Harmony25/win64/bin" \
        "$pfx/drive_c/Program Files (x86)/ToonBoom/Harmony25/win64/bin" \
        "$pfx/drive_c/Program Files/ToonBoom Animation/Toon Boom Harmony 25 Premium/win64/bin"
    do
        [ -f "$cand/HarmonyPremium.exe" ] && { printf '%s\n' "$cand"; return 0; }
    done
    # wrapper del lanzador (~/.local/bin/harmony25): BIN="$WINEPREFIX/drive_c/..."
    if [ -f "$HOME/.local/bin/harmony25" ]; then
        line="$(sed -n 's/^BIN="\{0,1\}\([^"]*\)"\{0,1\}$/\1/p' "$HOME/.local/bin/harmony25" | head -1)"
        line="${line//\$WINEPREFIX/$pfx}"; line="${line//\$HOME/$HOME}"
        [ -f "$line/HarmonyPremium.exe" ] && { printf '%s\n' "$line"; return 0; }
    fi
    # línea Exec= del .desktop
    if [ -f "$HOME/.local/share/applications/harmony25.desktop" ]; then
        line="$(grep -m1 '^Exec=' "$HOME/.local/share/applications/harmony25.desktop" | cut -d= -f2- || true)"
        cand="$(printf '%s\n' "$line" | tr ' ' '\n' | grep -i 'HarmonyPremium.exe$' | head -1 || true)"
        case "$cand" in
            [Cc]:\\*|[Cc]:/*) cand="$(dirname "$pfx/drive_c/${cand//\\//}")" ;;
            /*) cand="$(dirname "$cand")" ;;
            *) cand="" ;;
        esac
        [ -n "$cand" ] && [ -f "$cand/HarmonyPremium.exe" ] && { printf '%s\n' "$cand"; return 0; }
    fi
    # último recurso: búsqueda acotada
    cand="$(find "$HOME" "$pfx" -maxdepth 9 -name HarmonyPremium.exe -printf '%h\n' 2>/dev/null | head -1)"
    [ -n "$cand" ] && printf '%s\n' "$cand"
}

HARMONY_DIR="${2:-${HARMONY_DIR:-}}"
[ -n "$HARMONY_DIR" ] && [ ! -f "$HARMONY_DIR/HarmonyPremium.exe" ] && HARMONY_DIR=""
[ -z "$HARMONY_DIR" ] && HARMONY_DIR="$(find_harmony_dir "$PREFIX" || true)"

QTCONF=""
if [ -n "$HARMONY_DIR" ] && [ -d "$HARMONY_DIR" ]; then
    QTCONF="$HARMONY_DIR/qt.conf"
    if [ ! -f "$QTCONF" ]; then
        printf '[Platforms]\nWindowsArguments = nowmpointer\n' > "$QTCONF"
        cp -p "$QTCONF" "$QTCONF.wintab-shim"
        echo "== qt.conf creado: $QTCONF"
    elif grep -qi '^WindowsArguments' "$QTCONF"; then
        if grep -qi 'nowmpointer' "$QTCONF"; then
            echo "== qt.conf ya tiene nowmpointer: $QTCONF"
        else
            echo "!! $QTCONF ya define WindowsArguments sin nowmpointer; agrégalo a mano:"
            echo "     [Platforms]"
            echo "     WindowsArguments = nowmpointer"
        fi
    else
        [ -f "$QTCONF.antes-shim" ] || cp -p "$QTCONF" "$QTCONF.antes-shim"
        printf '\n[Platforms]\nWindowsArguments = nowmpointer\n' >> "$QTCONF"
        echo "== qt.conf ampliado con [Platforms]: $QTCONF (copia en .antes-shim)"
    fi
else
    echo "!! no encontré HarmonyPremium.exe (ni en el prefix ni con el lanzador)."
    echo "   Pásalo a mano:  ./instalar.sh '$PREFIX' \"/ruta/al/win64/bin\""
    echo "   el directorio es el que contiene HarmonyPremium.exe; ahí debe quedar qt.conf con:"
    echo "     [Platforms]"
    echo "     WindowsArguments = nowmpointer"
fi

# --------------------------------------------------------- 4. /dev/input
echo
NODE=""
for n in /sys/class/input/event*/device/name; do
    case "$(cat "$n" 2>/dev/null)" in
        *[Ww]acom*Pen*) NODE="/dev/input/$(basename "$(dirname "$(dirname "$n")")")"; break;;
    esac
done
if [ -z "$NODE" ]; then
    echo "== no detecté nodo de lápiz Wacom ahora; el shim autodetecta al arrancar"
elif [ -r "$NODE" ]; then
    echo "== tableta: $NODE legible"
elif [ ! -e "$NODE" ]; then
    echo "!! $NODE no existe aquí (¿contenedor?); en el host debería existir."
elif [ -f "$RULE" ]; then
    echo "!! $NODE existe pero no es legible todavía."
    echo "   La regla udev está puesta; el ACL lo aplica logind al conectar."
    echo "   Reconecta la tableta o reinicia la sesión, y comprueba:  getfacl -p $NODE"
else
    echo "!! $NODE existe pero no es legible."
    echo "   Lo directo (una regla udev, sobrevive a todo):   sudo $0 --udev"
    echo "   Alternativa por grupo (cerrar y abrir sesión):   sudo usermod -aG input ${USER:-$(id -un)}"
    echo "   Sólo para probar ya (se pierde al reconectar):   sudo setfacl -m \"u:${USER:-$(id -un)}:r\" $NODE"
fi

cat <<'EOF'

== instalado. Ahora el autodiagnóstico:
EOF
[ -x "$HERE/verificar.sh" ] && { echo; "$HERE/verificar.sh" "$PREFIX" || true; }
cat <<'EOF'

== siguientes pasos
   1) ./probar_shim.sh 10     # prueba el shim con el lápiz real
   2) abrir Harmony y dibujar
   3) revisar el log:  $PREFIX/drive_c/wt.log   (WT_LOG_LEVEL=3 = cada paquete)
   Volver atrás:       ./desinstalar.sh
EOF
