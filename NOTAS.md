# wintab32 nativo para Harmony 25 · Wayland · Wine

Shim WinTab 1.4 en DLL PE x86-64 que lee la tableta desde `/dev/input` y se
instala como *native override* en el prefix. Sin X11, sin parchear Wine, sin
tocar `HarmonyPremium.exe`.

## Estado en una línea

**Funciona en el host.** Harmony 25 dibuja con presión variable bajo Wayland
(KDE, Wine 11.20 Staging), con un solo cursor y sin alternar entre lápiz y mouse.
Queda el retardo normal del compositor al reenviar el lápiz al puntero: el trazo
y el cursor se mueven juntos, sólo llegan un poco tarde.

Evidencia: tabla real con 962 paquetes y 64 presiones distintas (0–1023),
`WTGetW pktData=0x1fe4` —exactamente la máscara de Qt—, `WTQueueSizeGet = 128`,
un único `WTOpenW` por corrida, y el trazo con presión confirmado por el usuario
en pantalla. Detalle: `docs/harmony25-lapiz-presion.md`.

> **Hallazgo que cambió el diseño:** Qt 6.4.2 **no usa WinTab** salvo que se
> desactive WM_POINTER, y Wine **borra** las variables `QT_*` del entorno. Las
> dos cosas están resueltas y verificadas (secciones 2 y 7). Sin esto, ni el
> mejor shim del mundo se cargaría.
>
> **Segundo hallazgo:** con XWayland el lápiz llega **dos veces** (tablet y mouse
> sintetizado). Mientras las dos posiciones difieran, Harmony alterna lápiz y
> mouse dentro del mismo trazo. La solución está en 9.3–9.5.

---

## 1. Por qué no funcionaba (diagnóstico ya cerrado)

`wintab32` de Wine construye su tabla de dispositivos con `XListInputDevices`
(XInput 1). Bajo Wayland, XWayland sólo le contesta punteros virtuales:

```
wintab32:load_tablet_info XListInputDevices reports 12 devices
Device 0: Virtual core pointer
Device 4: xwayland-pointer:11
Device 7: xwayland-keyboard:11
```

Ninguno es la tableta → cero dispositivos → `WTOpen` no puede crear contexto →
Qt nunca recibe `QTabletEvent` → Harmony dibuja con trazo parejo. El hardware y
el kernel están bien (`ABS_PRESSURE` 0–2047, 838 muestras en 10 s).

Qt 6.4.2 **sólo** habla WinTab en Windows (`QWindowsWinTab32DLL`), no usa
WM_POINTER ni Windows Ink. Por eso la solución es darle un WinTab que no
dependa de X.

## 2. El segundo motivo: Qt prefiere WM_POINTER

`qwindowsintegration.cpp` (Qt 6.4.2, líneas 224-226):

```cpp
if (m_context.initPointer(m_options))
    QCoreApplication::setAttribute(Qt::AA_CompressHighFrequencyEvents);
else
    m_context.initTablet();
```

Y `initPointer()` sólo falla si se pidió la opción `DontUseWMPointer`
(`qwindowscontext.cpp:297-304`). O sea: **por defecto Qt nunca llama
`QWindowsTabletSupport::create()`**, nunca hace `LoadLibrary("wintab32")` y
toda la clase `QWinTabPointingDevice` queda muerta. La opción se llama
`nowmpointer` y se pasa al plugin de plataforma:

- `qt.conf` junto al .exe: `[Platforms] WindowsArguments = nowmpointer` ✅
- argumento: `HarmonyPremium.exe -platform windows:nowmpointer` ✅
- variable `QT_QPA_PLATFORM=windows:nowmpointer` ❌

La variable no sirve porque **Wine filtra las variables `QT_*`** al construir
el entorno del proceso Windows. Comprobado:

```
$ QT_QPA_PLATFORM=windows:nowmpointer WT_LOG='C:\wt.log' FOO=bar \
    wine cmd /c "echo A=[%QT_QPA_PLATFORM%] B=[%WT_LOG%] C=[%FOO%]"
A=[%QT_QPA_PLATFORM%]  B=[C:\wt.log]  C=[bar]
```

`QT_QPA_PLATFORM` llega literal (no existe en el proceso); `WT_LOG` y `FOO`
sí. Por eso `instalar.sh` escribe **qt.conf** y no confía en el entorno.

## 3. Qué hace este shim

- Exporta la API WinTab (46 símbolos) y resuelve todo desde `/dev/input/eventN`.
- Abre el nodo, hace `EVIOCGNAME`/`EVIOCGABS` para rangos reales y lanza un hilo
  lector con `poll(2)`; en cada `SYN_REPORT` emite un paquete.
- Convierte `ABS_X/Y`, `ABS_PRESSURE` (0–2047), `BTN_TOUCH`, `BTN_STYLUS`,
  `BTN_STYLUS2`, `BTN_TOOL_PEN/RUBBER` a `pkX/pkY/pkNormalPressure/pkButtons/
  pkCursor`, con escala de presión configurable.
- Mantiene una cola por contexto y avisa con `PostMessage` (`WT_PACKET`,
  `WT_PROXIMITY`), igual que el `wintab32` de Wine.
- Reconecta solo si la tableta se desconecta; si no hay tableta, `WTInfo`
  responde con rangos por defecto y la app abre igual (el mouse no se toca).
- Registra cada export invocado en `C:\wt.log` (`WT_LOG`, `WT_LOG_LEVEL`).

Los syscalls son directos (`openat`, `read`, `poll`, `ioctl`) vía `syscall`
x86-64, no `msvcrt`/`kernel32`: así no dependemos de la traducción de rutas de
Wine ni de que exista `/dev/input` visible como `Z:\`.

## 4. Fase 0 — qué llama realmente la app

Los nombres se resolvieron **estáticamente** en el binario que usa Harmony:
`platforms/qwindows.dll` contiene la clase `QWinTabPointingDevice` y resuelve
por `GetProcAddress` exactamente nueve símbolos (no hay import estático de
`wintab32.dll` en el exe ni en las DLL del árbol `bin`):

`WTInfoW`, `WTOpenW`, `WTClose`, `WTEnable`, `WTOverlap`, `WTPacketsGet`,
`WTGetW`, `WTQueueSizeGet`, `WTQueueSizeSet`.

Secuencia (Qt 6.4.2 `qwindowstabletsupport.cpp` + captura en banco):

| # | Llamada | Cuándo | Argumentos | Qué espera | Este shim |
|---|---------|--------|------------|------------|-----------|
| 1 | `WTInfoW(WTI_INTERFACE, IFC_WINTABID, NULL/ptr)` | init | — | tamaño y nombre | 52 B `"ToonBoom Linux WinTab 1.0"` |
| 2 | `WTInfoW(WTI_DEFSYSCTX, 0, &lc)` | init | `lc` 212 B | contexto sistema | 212 B, `lcInExt` = rango real |
| 3 | `WTOpenW(hwndDummy, &lc, TRUE)` | init | `CXO_MESSAGES|CXO_CSRMESSAGES`, `lcPktData=0x1FE4`, `lcPktMode=0`, `lcOutExtY=-lcInExtY` | HCTX | contexto válido + 1 paquete de proximidad si el lápiz está cerca |
| 4 | `WTQueueSizeGet(ctx)` | init | — | 128 | 128 |
| 5 | `WTQueueSizeSet(ctx, 128)` | si ≠128 | 128 | TRUE | TRUE (realloc) |
| 6 | `WTGetW(ctx, &lc)` | al cambiar cursor | — | `lc.lcDevice` | `lcDevice=0`, resto del contexto |
| 7 | `WTInfoW(WTI_DEVICES+dev, DVC_NPRESSURE)` | idem | — | `axMin/axMax` | 0 / 1023 |
| 8 | `WTInfoW(WTI_DEVICES+dev, DVC_TPRESSURE)` | idem | — | igual | 0 / 1023 |
| 9 | `WTInfoW(WTI_DEFCONTEXT, 0, &lc)` | idem | — | `lcInExtX/Y` | rango real del device |
| 10 | `WTInfoW(WTI_CURSORS+cur, CSR_TYPE/PHYSID/SYSBTNMAP)` | cambio de cursor | `cur` ∈ 0..2 | tipo 0x0802 = stylus | 0x0802, physid 1, mapa 1/4/2 |
| 11 | `WTInfoW(WTI_DEVICES, DVC_ORIENTATION, AXIS[3])` | constructor | — | `axResolution≠0` = hay tilt | 0 ⇒ sin tilt (no lo tenemos) |
| 12 | `WTEnable(ctx, TRUE)` + `WTOverlap(ctx, TRUE)` | al activar ventana | — | TRUE | TRUE |
| 13 | `WTPacketsGet(ctx, 128, buf)` | por cada `WT_PACKET` | buffer de 128×44 B | nº paquetes | paquetes en orden pktdef |
| 14 | `WTPacketsGet(ctx, 1, &p)` | al entrar en proximidad | — | 1 paquete | 1 (lo empuja `WTOpen`/enter) |
| 15 | `WTClose(ctx)` | al cerrar | — | TRUE | TRUE |

### Confirmado con el binario real de Harmony 25

Ejecutando `HarmonyPremium.exe` (el parcheado, sin modificarlo) con el shim
instalado y `qt.conf` con `nowmpointer`, `C:\wt.log` registró esto — es la
secuencia real, no una deducción:

```text
=== wintab32 shim start 2026-11-02 21:14:04 pid=32 level=2 ===
[  3817.531 tid=  36] shim init: ToonBoom Linux WinTab 1.0
[  3817.531 tid=  36] evdev: using /dev/input/event20 name="(unnamed)"
                       x=0..15200 y=0..9500 pressure=0..2047 -> 0..1023
[  3817.532 tid=  36] WTInfoW(cat=4 idx=0 out=000000000011b0b0) -> 212
[  3817.532 tid=  36] WTOpenW name="ToonBoom Linux WinTab System" options=0x000d
                       pktData=0x1fe4 moveMask=0x1fe4 rate=100 dev=0
                       in=(0,0,0)+(15200,9500,0) out=(0,0,0)+(15200,-9500,0)
[  3817.532 tid=  36] WTOpenW(hwnd=0000000000010058) -> ctx=00000000086b06f0 enabled=1
[  3817.532 tid=  36] WTQueueSizeGet(00000000086b06f0) -> 128
[  3817.532 tid=  36] WTInfoW(cat=100 idx=17 out=000000000011b190) -> 48
[  3818.528 tid=  36] WTEnable(00000000086b06f0, 1) -> 1
[  3818.528 tid=  36] WTOverlap(00000000086b06f0, 1) -> 1
```

Lectura:

- `cat=4` = `WTI_DEFSYSCTX`, `cat=100 idx=17` = `WTI_DEVICES + DVC_ORIENTATION`
  (48 B = 3 × `AXIS`): el constructor de Qt pidiendo tilt, tal cual.
- `pktData = 0x1FE4` es **exactamente** la máscara `PACKETDATA` de Qt
  (`PK_X|PK_Y|PK_BUTTONS|PK_NORMAL_PRESSURE|PK_TANGENT_PRESSURE|PK_ORIENTATION|
  PK_CURSOR|PK_Z|PK_TIME`). Confirma que Harmony usa el Qt estándar y que el
  layout de 44 B de la sección 5 es el correcto.
- `options=0x000d` = `CXO_SYSTEM|CXO_MESSAGES|CXO_CSRMESSAGES`.
- No aparecen todavía `WTGetW` ni `CSR_TYPE/PHYSID/SYSBTNMAP`: Qt los pide
  cuando el lápiz **entra en proximidad**. En el contenedor no hay lápiz, así
  que ese tramo queda para el log del host (sección 10).

Si en el host el log no muestra `WTOpenW`, la app está desactivando WinTab por
su cuenta (existe el símbolo `?setWinTabEnabled@BUIAPP_Application@@QEAAX_N@Z`
en `ToonBoomBaseUiApplication.dll`): hay que activarlo en las preferencias de
Harmony.

## 5. ABI: los detalles que deciden si hay presión

**Paquete de 44 bytes.** El orden lo fija `pktdef.h` (contexto, estado, tiempo,
cambiado, serial, cursor, botones, x, y, z, presión, presión tangencial,
orientación, rotación) y la máscara de Qt es
`PK_X|PK_Y|PK_BUTTONS|PK_NORMAL_PRESSURE|PK_TANGENT_PRESSURE|PK_ORIENTATION|PK_CURSOR|PK_Z|PK_TIME`:

| offset | campo |
|---|---|
| 0 | `pkTime` |
| 4 | `pkCursor` |
| 8 | `pkButtons` |
| 12 | `pkX` |
| 16 | `pkY` |
| 20 | `pkZ` |
| 24 | `pkNormalPressure` |
| 28 | `pkTangentPressure` |
| 32..43 | `pkOrientation` (3×int) |

Comprobado compilando `pktdef.h` con mingw: `sizeof(PACKET)=44`, `pkX@12`,
`pkNormalPressure@24`. Si el orden se equivoca, Qt lee basura y el trazo sale
parejo aunque la presión llegue bien.

**Presión.** Qt calcula
`p = pkNormalPressure / (maxPressure - minPressure)` **sólo si `pkButtons != 0`**
y el cursor es pluma o goma. De ahí las dos condiciones:
`pkButtons` con el bit 0 puesto mientras la punta toca, y
`DVC_NPRESSURE.axMax = 1023`. El shim normaliza 0–2047 → 0–1023
(`WT_PRESSURE_MAX` para cambiarlo).

**Coordenadas.** Qt **ignora** `lcOut*`: usa `lcInExt` del **DEFCONTEXT** y
mapea el rango del device al escritorio virtual. Por eso el shim devuelve
coordenadas crudas del device (0..15200 / 0..9500 en el Intuos PT S 2) y declara
esos mismos rangos. Wine hace lo mismo (`x11_wintab.c` asigna `axis_data`
directo).

**Proximidad.** `WT_PROXIMITY = 0x7FF5` (no 0x7FF1), `wParam`=serial,
`lParam`= `MAKELPARAM(enter,enter)`; Qt mira `LOWORD(lParam) != 0` y en ese
mensaje llama `WTPacketsGet(ctx,1,&p)`, así que **tiene que haber un paquete en
cola** o el cursor no se registra.

**Modo.** Si al bajar la pluma la posición calculada está a más de 20 px del
mouse, Qt pasa a `MouseMode`: usa la posición del mouse y **mantiene la
presión**. Es aceptable, pero si el trazo aparece desplazado, ese es el motivo.

## 6. Compilar

```bash
sudo apt install mingw-w64        # Debian/Ubuntu dentro del contenedor
./build.sh                        # -> dist/wintab32.dll
```

`build.sh` no necesita Wine: es `x86_64-w64-mingw32-gcc -shared` con
`wintab32.def`.

## 7. Instalar / desinstalar

```bash
./instalar.sh                     # o ./instalar.sh /ruta/al/prefix
./desinstalar.sh
```

`instalar.sh` hace cuatro cosas:

1. respalda el builtin en `system32/wintab32.dll.wine-builtin` (una vez) y
   copia el shim a `system32/wintab32.dll`;
2. escribe `HKCU\Software\Wine\DllOverrides\wintab32 = native` en el prefix
   (persistente y **sin tocar** `~/.local/share/applications/harmony25.desktop`);
3. crea (o amplía) **`qt.conf` junto a `HarmonyPremium.exe`** con
   `[Platforms] WindowsArguments = nowmpointer`. Sin esto Qt no carga wintab32
   (sección 2). En esta instalación el exe está **dentro del prefix**:
   `$PREFIX/drive_c/Program Files/ToonBoom/Harmony25/win64/bin` — ese es el
   primer sitio que mira; si falla, prueba el `BIN=` del wrapper
   `~/.local/bin/harmony25`, la línea `Exec=` del `.desktop` y una búsqueda
   acotada. Se le puede pasar el directorio como 2º argumento. Deja copia en
   `qt.conf.antes-shim` si ya existía;
4. avisa si falta el grupo `input` o si el nodo no es legible.

Equivalente manual: `WINEDLLOVERRIDES="wintab32=n"` al lanzar (o `winecfg` →
Libraries → `wintab32` → *Native*) **más** `qt.conf`, o `-platform
windows:nowmpointer` en la línea de comandos.

## 8. Permisos de `/dev/input`

El nodo es `root:input 0660`; sin acceso el shim no lee nada (lo dice en el log:
`evdev: no tablet device found`). Dos opciones:

```bash
# A) grupo (recomendado, sobrevive reinicios)
sudo usermod -aG input daniel       # y volver a iniciar sesión
#    (mientras no cierres sesión, el grupo viejo sigue activo: usa la opción C)
# C) prueba inmediata sin relogin (se pierde al reiniciar)
sudo setfacl -m "u:daniel:r" /dev/input/event20

# B) regla udev con ACL para el usuario de la sesión
sudo tee /etc/udev/rules.d/99-wacom-uaccess.rules >/dev/null <<'EOF'
SUBSYSTEM=="input", KERNEL=="event*", ATTRS{name}=="Wacom Intuos PT S 2 Pen*", TAG+="uaccess"
EOF
sudo udevadm control --reload-rules && sudo udevadm trigger
```

Verificar: `test -r /dev/input/event20 && echo ok`.

## 9. Verificación en banco (hecha, en el contenedor)

No hay `/dev/input` en el contenedor, así que se usó un FIFO con el formato
exacto de `struct input_event` (`tools/mkfake.py`) y Wine 8:

```bash
./run_wine_test.sh
```

Resultado:

```
WTInfo id: 52 bytes -> ToonBoom Linux WinTab 1.0
spec=0x0104 devices=1 cursors=3
DVC_X min=0 max=15200   DVC_Y min=0 max=9500   DVC_NPRESSURE min=0 max=1023
DEFSYSCTX: options=0xd pktData=0x3fff inExt=(15200,9500,0) outExt=(15200,-9500,0)
WTOpenW -> 0x...800      queue: 128 -> 128      WTEnable=1 WTOverlap=1
WTGetW: dev=0 pktData=0x1fe4
  proximity enter (packets=1)
  packet x=389  y=4847  btn=0x1 pressure=51
  packet x=779  y=4944  btn=0x1 pressure=104
  ...
total packets=1376 distinct pressures=20 min=0 max=996
```

Es decir: exports correctos, contexto, cola, mensajes y **presión escalada**. Lo
que el banco **no** prueba: el nodo real, los rangos del Intuos y el consumo por
parte de Harmony.

Nota sobre el log de Harmony en el contenedor (sección 4): allí
`/dev/input/event20` existe como nodo heredado del host pero `EVIOCGNAME`
falla, por eso aparece `name="(unnamed)"` y los rangos 15200×9500 son los de
respaldo. En el host se leen nombre y rangos reales del ioctl; si no, se
fuerzan con `WT_X_RANGE`/`WT_Y_RANGE`/`WT_PRESSURE_RANGE`.

### 9.1 Por qué no hay syscalls crudos ni ioctls (v3)

La v1 leía `/dev/input` con la instrucción `syscall` (openat/read/ioctl/poll).
En el contenedor funcionaba; en el host, cada apertura devolvía
**-1073741822 = 0xC0000002 `STATUS_NOT_IMPLEMENTED`**. Causa: Wine 11 instala un
*syscall dispatcher* en ntdll y trata la instrucción `syscall` de un PE como una
llamada al sistema de Windows, que no conoce `openat`; nunca llega al kernel de
Linux. Nada que ver con permisos: los permisos eran el segundo problema.

La v3 usa sólo la capa Win32, que Wine sí traduce a syscalls reales:

| operación | antes | ahora |
|---|---|---|
| abrir nodo | `syscall(openat)` | `CreateFileA("Z:\\dev\\input\\eventN")` |
| leer eventos | `syscall(read)` | `ReadFile` (bloqueante) |
| esperar | `poll()` | `ReadFile` bloqueante |
| cerrar | `syscall(close)` | `CloseHandle` |
| nombre del nodo | `EVIOCGNAME` | `/sys/class/input/eventN/device/name` |
| rangos de ejes | `EVIOCGABS` | defaults del Intuos + `WT_*_RANGE` |

Comprobado con un `.exe` de prueba bajo Wine 8: `CreateFileA("Z:\\dev\\urandom")`
+ `ReadFile` devuelve bytes, y `Z:\\sys\\class\\input\\event20\\device\\name`
devuelve `Wacom Intuos PT S 2 Pen`. Es decir, la capa de archivos de Wine sí
llega a `/dev` y `/sys`.

Contrapartida: sin `EVIOCGBIT`/`EVIOCGABS` la detección es por nombre (sysfs) y
los rangos son fijos salvo que se pasen por entorno. Para esta tableta es
suficiente y el nombre del nodo del lápiz siempre está disponible.

### 9.2 Detección del nodo (v3: por nombre en sysfs)

1. Lee `/sys/class/input/event0..63/device/name` con `CreateFileA`+`ReadFile`.
2. Puntúa: `wacom`/`intuos` +100, `pen`/`stylus`/`tablet` +50, y resta por
   `pad`/`touch`/`keyboard`/`mouse`/`consumer`/`control` (-200). El nodo del
   lápiz de la Intuos PT S 2 queda con **score 150**; el `… Pad` y el `… Finger`
   quedan en negativo y no compiten.
3. Abre los candidatos de mayor a menor; el primero que abre gana.
4. Si ninguno abre, el log dice cuántos candidatos había y por qué falló:

```
evdev: /dev/input/event20 is "Wacom Intuos PT S 2 Pen" (score 150)
evdev: /dev/input/event20 looks right but open failed (err 5)
   -> err 5 = ERROR_ACCESS_DENIED: grupo input / ACL.
   -> err 3 = ERROR_PATH_NOT_FOUND: el nodo no existe (contenedor, tableta desconectada).
evdev: 1 candidate(s) found, none could be opened; grant read access (udev rule or
       `usermod -aG input <user>`) or force WT_DEV=/dev/input/eventN
```

`WT_DEV=/dev/input/eventN` salta toda la detección; `WT_DEV=none` la desactiva.

### 9.3 El puntero sigue al lápiz (`WT_MOUSE_SYNC`)

En X11 la tableta mueve el *core pointer*: por eso Qt compara la posición del
lápiz con la del mouse y, si están cerca, se queda en `PenMode`. Bajo XWayland
el lápiz no existe para X, así que el puntero se quedaba quieto y aparecían **dos
cursores**: el del pincel (con presión, donde está el lápiz) y el del mouse
(aparte). Al tocar el canvas con el mouse, la app volvía al trazo plano.

Por eso el shim mueve el puntero él mismo: en cada `SYN_REPORT`, y sólo si hay
lápiz en proximidad, calcula la posición de pantalla con el mismo mapeo que usa
Qt (extensión del dispositivo sobre el escritorio virtual, sin invertir Y) y
llama a `SetCursorPos`, que Wine traduce a `XWarpPointer`.

```
evdev: pointer sync on (WT_MOUSE_SYNC, 12 ms)
evdev: sync wants 0,0 and the pointer landed at 0,0 (virtual 0,0 1280x1024)
```

La segunda línea se escribe una vez: sirve para detectar escalado DPI (si
`wants` y `landed` difieren mucho, Wine está escalando las coordenadas y hay que
ajustar). Efectos:

- Un solo cursor mientras el lápiz se mueve; el mouse sigue mandando cuando es
  el mouse el que se mueve (último dispositivo que habla, gana: igual que una
  tableta real).
- Con el lápiz fuera de proximidad no se toca nada: mouse normal.
- `WT_MOUSE_SYNC=0` lo desactiva; `WT_MOUSE_SYNC_MS` (12 por defecto) limita la
  frecuencia de warps.

**v4 — esto no alcanzó.** El warp arregla el cursor pero *genera*
`WM_MOUSEMOVE`, y XWayland a la vez mueve el puntero por su cuenta. Resultado en
Harmony: el trazo alterna entre lápiz (con presión) y mouse (plano) dentro del
mismo trazo. La solución definitiva son las dos secciones siguientes; el warp
queda apagado por defecto (`WT_MOUSE_SYNC=0`) y sólo se usa si el puntero deriva.

### 9.4 La posición del paquete es la del puntero (`WT_POS_CURSOR`)

Bajo XWayland el lápiz no mueve el *core pointer*: la posición absoluta del
dispositivo y la del cursor visible son dos verdades distintas. Qt compara ambas
al apoyar el lápiz (`qwindowstabletsupport.cpp:630`, umbral `m_absoluteRange = 20`)
y, si no coinciden, entra en `MouseMode` y **sustituye la posición del lápiz por
la del mouse** (`:638`). De ahí los saltos.

En vez de pelear con dos posiciones, el paquete reporta la del puntero: se lee
`GetCursorPos()`, se invierte el mismo mapeo que aplica Qt (escritorio virtual ->
extensión del dispositivo) y eso va en `pkX`/`pkY`. Consecuencias:

- Qt calcula `globalPos == mouseLocation`, o sea `PenMode` siempre, sin saltos.
- El pincel queda exactamente bajo el cursor que el usuario ve.
- La presión sigue siendo la del lápiz: es lo único que el shim aporta.
- `WT_POS_CURSOR=0` vuelve a la posición absoluta del dispositivo (para comparar).

El cursor lo mueve el compositor al reenviar el lápiz; si esa reentrega se
retrasa, el trazo se retrasa igual que el cursor, nunca se separan.

### 9.5 Un solo dispositivo: filtro de mensajes de mouse (`WT_MOUSE_BLOCK`)

Aunque las posiciones coincidan, Harmony recibe **dos flujos**: `QTabletEvent`
(con presión, del shim) y los eventos de mouse que XWayland sintetiza desde el
puntero. Su motor de trazo cambia de fuente según el último evento, y eso es el
"a veces usa la presión, a veces no".

El shim instala un hook `WH_GETMESSAGE` en el hilo de GUI al abrir el contexto
(`WTOpenW`) y descarta `WM_MOUSEMOVE`, botones, `WM_MOUSEWHEEL` y `WM_MOUSELEAVE`
mientras el lápiz está en proximidad. Queda un solo dispositivo, el lápiz, con la
posición del cursor (9.4). Es el mismo comportamiento de una tableta real en
Windows: con el lápiz cerca, el mouse no manda.

```
evdev: mouse filter installed (WT_MOUSE_BLOCK)
```

- `WT_MOUSE_BLOCK=0` lo desactiva (si algún diálogo necesitara el mouse con el
  lápiz apoyado).
- El hook es por hilo y del propio proceso; se retira en `DLL_PROCESS_DETACH`.

### 9.6 Permisos: el ACL se cae, la regla udev no

`setfacl` sirve para probar en el acto, pero **el ACL vive en el inodo**: si la
tableta se reconecta, si udev recrea el nodo o si se reinicia, desaparece y el
shim vuelve a `open failed (err 5)` / `present=0` / cero paquetes. El síntoma es
idéntico al de "no hay presión": Harmony abre y dibuja plano.

La forma estable es una regla udev con `uaccess`, que systemd-logind traduce a un
ACL del usuario de la sesión (y lo re-aplica solo):

```bash
cd /var/home/daniel/dsh-sandbox/workspace/tb/wintab
sudo ./instalar.sh --udev          # /etc/udev/rules.d/70-wacom-wintab.rules
getfacl -p /dev/input/event20 | grep "$USER"
```

```
SUBSYSTEM=="input", KERNEL=="event*", ATTRS{name}=="Wacom*", TAG+="uaccess", MODE="0664"
```

El número importa: el builtin `uaccess` de systemd-logind se encola en
`73-seat-late.rules`, así que una regla `99-*` llega tarde y el ACL no se aplica.
De ahí `70-wacom-wintab.rules`; `MODE="0664"` queda como red de seguridad por si
el seat no está disponible (el nodo de un lápiz no es un teclado).

Alternativa: `sudo usermod -aG input daniel` + cerrar y abrir sesión (el grupo no
entra en el proceso actual). Para revertir la regla: `sudo ./desinstalar.sh --udev`.

## 10. Verificación en el host (el paso que falta)

```bash
cd /var/home/daniel/dsh-sandbox/workspace/tb/wintab
./instalar.sh                     # DLL + override + qt.conf
./probar_shim.sh 10               # shim contra el lápiz, sin Harmony
```

Con la pluma cerca de la tableta, el log del host debe mostrar además las
llamadas que en el contenedor no pudieron ocurrir:

```bash
grep -E "WTGetW|cat=200|WTOpenW|evdev: using" "$WINEPREFIX/drive_c/wt.log"
```

`probar_shim.sh` corre `wtprobe.exe` (imita a Qt: mismos 9 exports, mismo
contexto, misma cola) contra la tableta real e imprime
`evdev: using /dev/input/event20 name="Wacom Intuos PT S 2 Pen" x=0..15200 ...`
más el rango de presiones visto.

Después, con Harmony abierto y un trazo:

```bash
grep -E "evdev: using|WTOpenW|WTInfoW\(cat=4" "$WINEPREFIX/drive_c/wt.log"
grep -c "evdev packet" "$WINEPREFIX/drive_c/wt.log"
```

Criterio de éxito: el log muestra `WTOpenW` con `pktData=0x1fe4`, y los
`evdev packet` tienen `p=` variando mientras se dibuja; en pantalla el trazo
engorda y afina.

### 10.1 Las dos corridas que deciden (v4)

Con la DLL 338182 B (22:55), un solo trazo por corrida:

```bash
# A: comportamiento por defecto (posición del puntero + filtro de mouse)
WT_LOG_LEVEL=3 ~/.local/bin/harmony25
```

- Si dibuja con presión estable y un solo cursor: listo, es la configuración.
- Si **no dibuja nada** con el lápiz: el motor de trazo de Harmony usa el mouse,
  no `QTabletEvent`. Probar `WT_MOUSE_BLOCK=0` y anotar.
- Si dibuja pero **el trazo va desplazado del lápiz**: el compositor no reenvía
  el lápiz al puntero. Probar `WT_MOUSE_SYNC=1` (el shim mueve el puntero) y ver
  si desaparece el desplazamiento.

**Resultado en el host (2026-11-02):**

- A con los valores por defecto: presión estable y un solo cursor; queda el
  retardo del compositor. Es la configuración que se queda.
- `WT_POS_CURSOR=0 WT_MOUSE_SYNC=1`: **trazos erráticos**, la misma pelea de la
  v3. Descartado: el warp y el puntero de XWayland se suman y, entre warp y warp,
  paquete y cursor se separan (Qt entra en `MouseMode`). El filtro de mouse no lo
  arregla porque el problema es la posición, no el flujo de eventos.

```bash
# B: control, posición absoluta del dispositivo (como v3)
WT_POS_CURSOR=0 WT_LOG_LEVEL=3 ~/.local/bin/harmony25
```

Comparar A contra B responde si el problema era la posición o el doble flujo de
eventos. En el log, `evdev: position from …` dice cuál está activo y
`mouse filter installed` confirma el hook.

## 11. Variables de entorno

| variable | efecto |
|---|---|
| `WT_DEV` | `/dev/input/eventN` fuerza el nodo; `none` desactiva el lector |
| `WT_LOG` | ruta del log (por defecto `C:\wt.log`) |
| `WT_LOG_LEVEL` | 0 off, 1 init, 2 llamadas, 3 paquetes |
| `WT_PRESSURE_MAX` | tope de `pkNormalPressure` (1023) |
| `WT_PRESSURE_RANGE` | `min:max` de entrada si el ioctl no sirve |
| `WT_X_RANGE` / `WT_Y_RANGE` | `min:max` de ejes |
| `WT_INVERT_X` / `WT_INVERT_Y` / `WT_SWAP_XY` | orientación |
| `WT_POS_CURSOR` | 1 | `pkX`/`pkY` salen de `GetCursorPos()` invertido (9.4) |
| `WT_MOUSE_BLOCK` | 1 | descarta los mensajes de mouse con el lápiz en proximidad (9.5) |
| `WT_MOUSE_SYNC` | 0 | mueve el puntero a la posición absoluta del lápiz (v3; sólo si el cursor deriva) |
| `WT_MOUSE_SYNC_MS` | 12 | throttle de los warps |
| `WT_RECHECK_MS` | reintento de autodetección (1000) |

## 12. Limitaciones

- **Sin tilt ni rotación**: `DVC_ORIENTATION.axResolution = 0`; el Intuos PT S 2
  no reporta inclinación por evdev de todos modos.
- **Un dispositivo**: se elige el mejor puntuado (nombre con `wacom`/`pen`,
  penaliza `pad`/`touch`/`mouse`); no hay multi-tableta simultánea.
- **Sin API de manager** (`WTMgr*`): stubs que devuelven FALSE. Qt no las usa.
- **`pkSerialNumber`/`pkTime`** propios (contador y `GetTickCount`), no los del
  kernel; alcanza para el orden y los timestamps de Qt.
- **No mapea `lcOut*`**: igual que Wine; Qt tampoco lo usa.
- El shim no filtra por ventana: entrega paquetes a todo contexto habilitado.

## 13. Si algo falla

| síntoma | qué mirar |
|---|---|
| volvió a 0 paquetes tras funcionar | el ACL se cayó (reconexión/reboot): `sudo ./instalar.sh --udev`; el shim reintenta cada 1 s, no hace falta reiniciar Harmony |
| `evdev: … open failed (err 5)` | `ERROR_ACCESS_DENIED`: falta grupo `input` o ACL sobre el nodo |
| `evdev: … open failed (err 3)` | `ERROR_PATH_NOT_FOUND`: no existe `/dev/input/eventN` (contenedor, tableta desconectada) |
| nada de `evdev:` en el log y `build:` viejo | la DLL del prefix no es la nueva: `./instalar.sh` otra vez y comparar `build:` que imprime `probar_shim.sh` |
| Harmony abre pero trazo parejo | `wt.log` sin `WTOpenW` → WinTab desactivado en la app, o el override no tomó (`winecfg` → Libraries) |
| `evdev: scanned … none exposes ABS_PRESSURE` | ningún nodo es lápiz: `WT_DEV=/dev/input/eventN` a mano |
| `evdev: scanned 0 nodes (N denied)` | permisos: `id -nG`, grupo `input` o ACL |
| Cursor salta o se va a la esquina | rangos mal declarados: `WT_X_RANGE`/`WT_Y_RANGE` según `EVIOCGABS` |
| Trazo desplazado | Qt en `MouseMode` (>20 px del mouse): mover el lápiz sobre el canvas al bajar |
| Presión siempre 0 | `pkButtons` sin bit 0 (¿`BTN_TOUCH` no llega?) o `DVC_NPRESSURE.axMax=0` |
| Harmony no arranca | el override apunta a una DLL ausente: `./desinstalar.sh` y revisar |
| `instalar.sh` no encuentra el exe | pásale el directorio: `./instalar.sh "$WINEPREFIX" "$HOME/.local/share/wineprefixes/harmony25/drive_c/Program Files/ToonBoom/Harmony25/win64/bin"` |

## 14. Rollback

```bash
./desinstalar.sh                  # borra el override y restaura el builtin
```

No se modificó `HarmonyPremium.exe`, ni sus DLL, ni el Wine del sistema, ni el
lanzador del menú.
