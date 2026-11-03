# Latencia del shim

Qué añade el shim entre el kernel y la cola de mensajes de Qt, medido —no estimado—, y
qué se decidió con los números.

El recorrido completo es:

```
tableta → HID → evdev (/dev/input/event20) → hilo lector del shim → cola del contexto
        → PostMessage(WT_PACKET) → hilo de Qt → QTabletEvent → motor de Harmony
```

Aquí se miden dos tramos:

- `d_shim_us` = `t_post − t_read`: lo que tarda el shim desde que saca el paquete de
  `ReadFile` hasta que lo publica con `PostMessage`.
- `d_visible_us` = `t_post` del mensaje que **realmente** lo lleva − `t_read`. Si el
  paquete se agrupó con otros, ese mensaje es posterior; sin esto, los paquetes
  agrupados parecerían instantáneos.

Lo que pasa después de Qt (motor de Harmony) no se mide: haría falta instrumentar la app.

Build medida: `dist/wintab32.dll` 369 226 B (enumeración de `/sys/class/input` incl.)
para el host, y 365 572 B para las tablas del banco; las dos llevan `wtmetrics.c`.

## Cómo se activa

Todo apagado por defecto; sin `WT_METRICS` la ruta caliente no paga ni un
`QueryPerformanceCounter` (sólo el `if` de `wtm_on()`, que lee una variable ya cargada).

| Variable | Efecto |
|---|---|
| `WT_METRICS=/ruta.csv` | Enciende el medidor y fija el CSV (`/tmp/x` se traduce a `Z:\tmp\x`). |
| `WT_METRICS_LEVEL=1` | Sólo resumen: `packets,posts,coalesced,level,elapsed_us`. |
| `WT_METRICS_LEVEL=2` | Una fila por paquete (por defecto cuando hay `WT_METRICS`). |
| `WT_COALESCE=1` | Agrupa: un `WT_PACKET` por ráfaga en vez de uno por paquete. **Apagado.** |
| `WT_COALESCE_MS=4` | Ventana de agrupado en ms. |
| `WT_POS_CACHE=1` | Consulta la posición del puntero una vez por ráfaga. **Recomendado.** |

El CSV se escribe en `DLL_PROCESS_DETACH`, o sea **al cerrar la app**, no mientras dibujas.

Columnas: `n, epoch_us, t_ev_us, t_read_us, t_post_us, d_kernel_us, d_shim_us, queue,
posted, dropped`. `posted=0` = agrupado en un mensaje posterior (su `d_shim_us` es 0, y
por eso el analizador calcula `d_visible_us`). `t_ev_us=0` = el nodo no reporta
timestamp, como el FIFO del banco.

## Comandos exactos

Banco (contenedor: FIFO con eventos sintéticos, Wine 8, Xvfb; sin tableta real):

```bash
./build.sh
WT_METRICS=/tmp/wt.csv SECONDS_RUN=8 ./run_wine_test.sh
WT_POS_CACHE=1 WT_METRICS=/tmp/wt-cache.csv SECONDS_RUN=8 ./run_wine_test.sh
WT_COALESCE=1 WT_METRICS=/tmp/wt-coal.csv SECONDS_RUN=8 ./run_wine_test.sh
python3 tools/wtmetrics.py /tmp/wt.csv --per-second
```

Tableta real, sin abrir Harmony:

```bash
WT_METRICS=/tmp/wt.csv ./probar_shim.sh 10
python3 tools/wtmetrics.py /tmp/wt.csv
```

Tableta real, dibujando en Harmony (el caso que importa):

```bash
./tools/medir_latencia.sh                # abre Harmony, dibuja ≥10 s, cierra
./tools/medir_latencia.sh --coalesce     # la otra mitad del A/B
```

## Resultados medidos

### Coste por paquete (banco, 8 s, ráfagas de ~40 eventos por `ReadFile`)

| configuración | por paquete p50 | p95 | `d_shim` p50 | p95 | msg/s |
|---|---|---|---|---|---|
| por defecto (`WT_POS_CURSOR=1`) | **173 µs** | 240 µs | 1.352 ms | 2.720 ms | 173.0 |
| `WT_POS_CACHE=1` | **8 µs** | 13 µs | 0.274 ms | 0.729 ms | 173.3 |
| `WT_POS_CURSOR=0` (sin posición del puntero) | 8 µs | 78 µs | 0.075 ms | 0.517 ms | 173.3 |

El coste por paquete sale de `t_post[k] − t_post[k−1]` dentro de la misma ráfaga, así
que no arrastra el resto de la ráfaga. La diferencia entre la primera y la tercera fila
es una sola llamada: `GetCursorPos()`, un round-trip a X por paquete.

**Conclusión:** la posición del puntero cuesta ~20× más que todo lo demás junto, y se
consulta 40 veces por ráfaga para obtener el mismo valor. `WT_POS_CACHE=1` la consulta
una vez por ráfaga: mismas coordenadas (el puntero no se mueve solo entre dos `ReadFile`
seguidos) y el coste vuelve a 8 µs. Con el modo por defecto, la última muestra de una
ráfaga de 40 llega ~7 ms tarde; con caché, ~0.3 ms.

### ¿Añade el shim menos de 1 ms?

Con la configuración recomendada (`WT_POS_CACHE=1`), **sí**: `d_visible` p95
**0.729 ms** y p99 **0.792 ms** por ráfaga, o sea ~8 µs por paquete.

Con el default actual (sin caché), **no**: p95 **2.720 ms** y p99 **3.531 ms**, y todo el
exceso es `GetCursorPos()`. Por eso la recomendación de abajo es activar `WT_POS_CACHE`
en el lanzador mientras no haya medida del host.

### Agrupado de mensajes (`WT_COALESCE`)

| configuración | filas | posted | msg/s | `d_visible` p50 | p95 | p99 | max |
|---|---|---|---|---|---|---|---|
| sin agrupar | 1386 | 1386 | 173.0 | 1.352 ms | 2.720 ms | 3.531 ms | 15.629 ms |
| `WT_COALESCE=1` | 1386 | 193 | 24.1 | 2.520 ms | 5.277 ms | **243.527 ms** | 244.465 ms |
| `WT_COALESCE=1 WT_POS_CACHE=1` | 1386 | 104 | 13.0 | 0.519 ms | 1.271 ms | **249.767 ms** | 250.240 ms |

Los paquetes entregados no bajan (1386 en las tres), pero el agrupado **añade una cola
de espera**: el último tramo de una ráfaga sólo se publica cuando llega el evento
siguiente. En el banco eso son hasta 244 ms (el generador hace pausas entre pasadas); en
un trazo real es el hueco entre dos movimientos.

**Conclusión: el agrupado se queda apagado.** El motivo del encargo era la presión sobre
la cola de mensajes, y esa presión no existe: la cola nunca pasó de 43 de 128 paquetes y
`dropped` fue 0 en todas las corridas, también con el lápiz a 200 Hz sintéticos. Cambiar
1.5 ms de latencia por ráfaga por 3–240 ms de cola no compensa. Si algún día Harmony se
ve msg-bound (se notaría en `queue` creciendo y `dropped > 0`), el flag ya está y este
párrafo es la medida de lo que cuesta.

### Probe contra el banco, 5 s, DLL instalada en el prefix

881 filas, 175.8 pkt/s, cola máxima 43, 0 descartados, `d_shim` p50 0.976 ms / p95 1.992
/ p99 8.029 / max 9.171 ms. Por segundo:

```
  0s   180 pkt   180 msg   8.029 ms   34
  1s   155       155       1.968 ms   40
  2s   168       168       1.817 ms   40
  3s   168       168       1.903 ms   35
  4s   168       168       2.026 ms   43
  5s    42        42       1.823 ms   41
```

El pico del primer segundo es el arranque (primer `GetCursorPos` + creación del contexto).

### Con el medidor apagado

Misma build, sin `WT_METRICS`: 1403 paquetes, 20 niveles de presión 0..996, ningún CSV.
El comportamiento es el de antes; el medidor sólo cuesta un `if`.

## Medido en el host (Bazzite, wine-11.20 staging, Harmony abierto)

Dos corridas dibujando de verdad, mismo dibujo aproximado, 24 s cada una:

| corrida | filas | pkt/s | msg/s | cola máx | dropped | `d_shim` p50 | p95 | p99 | `d_kernel` p50 | p95 | p99 |
|---|---|---|---|---|---|---|---|---|---|---|---|
| `WT_POS_CACHE=1` (el lanzador) | 3025 | 127.3 | 127.3 | 67 | 0 | **4 µs** | **9 µs** | 96 µs | 41 µs | **60 µs** | 130 µs |
| `WT_POS_CACHE=0` | 3243 | 131.1 | 131.1 | 52 | 0 | **5 µs** | **10 µs** | 99 µs | 42 µs | **60 µs** | 133 µs |

Lo primero que salta a la vista: **la caché no cambia nada en el host**. La diferencia
que medimos en el banco (173 → 8 µs) era de Xvfb: allí `GetCursorPos()` era un
round-trip por socket a un servidor X remoto dentro del contenedor. Contra XWayland, la
consulta ya está servida desde estado local y cuesta lo mismo que todo lo demás.
`WT_POS_CACHE` se queda en el lanzador porque no estorba, pero **no es lo que arregló el
trazo**: lo que lo arregló fue leer la posición del puntero (`WT_POS_CURSOR`) y filtrar
el mouse sintetizado (`WT_MOUSE_BLOCK`).

Reparto real por paquete, con la tableta a 133 Hz (7.5 ms entre paquetes):

```
kernel (timestamp del evento) --60 µs (p95)--> ReadFile --9 µs (p95)--> PostMessage
```

Es decir, **~70 µs en total en el p95** desde que el kernel marca el evento hasta que el
mensaje está en la cola de Qt: menos del **1 %** del intervalo entre paquetes. El shim no
es el cuello de botella, y no queda nada que optimizar ahí.

Detalles que confirman que no hay presión:

- `pkt/s` = `msg/s` en las dos corridas: no hay agrupado, y el ritmo (127–133) es
  exactamente el de reporte del Intuos PT S 2, así que no se pierde ninguna muestra.
- `dropped` = 0 y la cola pasa el 99 % del tiempo entre 1 y 8 de 128.
- Los picos de cola (67 y 52) y los saltos de `d_shim` (0.09–0.3 ms) caen en los dos
  últimos segundos, cuando la app está guardando y cerrando; ahí el hilo de Qt no drena.
- Los máximos sueltos (12.6 ms de `d_shim`, 102 ms de `d_kernel`) son de arranque y
  cierre, no del trazo: en la ventana de dibujo la distribución es plana.

### Hasta dónde se puede llegar

Desde la DLL ya está todo: el tramo medible suma 70 µs. Lo que queda por debajo del ojo
es el viaje `PostMessage → QTabletEvent → motor de Harmony → frame del compositor`, que
no se mide desde el shim y que en Wayland cuesta al menos un frame (16.7 ms a 60 Hz). Ese
es el "delay normal del lápiz" que se nota al dibujar y **no se puede bajar sin parchear
Qt o el compositor**, que está fuera del alcance acordado.

Para seguir por ese lado haría falta instrumentar la app: o un Qt propio con
`QElapsedTimer` en `QWindowsTabletSupport::translateTabletPacketEvent`, o medir el
intervalo entre `WT_PACKET` y el repintado con una captura de alta velocidad. Ninguna de
las dos es una DLL.

## Cómo reproducirlo

```bash
cd ~/dsh-sandbox/workspace/tb/wintab
./tools/medir_latencia.sh          # dibuja 20-30 s y cierra Harmony
WT_POS_CACHE=0 ./tools/medir_latencia.sh
```

## Límites de esta medición

- El banco es un FIFO con eventos sintéticos: sin timestamps de kernel (`d_kernel_us` no
  se puede medir ahí) y sin latencia real de HID.
- Xvfb + `llvmpipe` exageran las colas del planificador; los p99 del banco no son los del
  host. Los valores absolutos de `GetCursorPos` también son de Xvfb.
- No se mide el tramo Qt → motor de Harmony.
- Las ráfagas del banco son de ~40 eventos; con la tableta real el tamaño depende del
  kernel y del ritmo del lápiz, y el coste por paquete se reparte distinto.
- Las cifras varían entre corridas (±20 % en este contenedor con Xvfb); lo que se
  sostiene es el orden de magnitud y la proporción entre configuraciones.
