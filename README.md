# mount-watch (cpp-mount-watch)

Monitor de sistemas de archivos y montajes de Linux en **C++23 sin dependencias externas**.

Lee `/proc/mounts` del kernel y combina cada punto de montaje con `statvfs(2)` para
reportar uso real por filesystem: tamaño total, usado, disponible, porcentaje y
marcador de solo-lectura. Filtra los filesystems pseudo/sintéticos (`proc`, `sysfs`,
`devpts`, `cgroup`, `overlay`, `squashfs`...) para mostrar solo los de datos reales
(ext4, xfs, btrfs, vfat, tmpfs...), con `--all` para verlos todos.

## Build

```bash
g++ -std=c++23 -O2 -Wall -Wextra src/main.cpp -o mount-watch
```

## Uso

```bash
./mount-watch                 # snapshot único con tabla de todos los filesystems
./mount-watch --watch 5       # refresco cada 5 s con deltas de espacio disponible
./mount-watch --json          # snapshot en JSON (para scripting con jq/python)
./mount-watch --demo          # datos sintéticos realistas (contenedores/VMs)
./mount-watch --all           # incluir filesystems pseudo/sintéticos
./mount-watch --no-color      # salida sin códigos ANSI
./mount-watch --selftest      # 30 tests de auto-verificación
```

### Modo watch

`--watch N` redibuja la tabla cada N segundos y muestra el **delta de espacio
disponible** por filesystem entre frames (`(+N)` / `(-N)` en bytes). Combínalo con
`--timeout T` para salida automática (ideal para cron):

```bash
./mount-watch --watch 10 --timeout 3600
```

### Salida JSON

```bash
./mount-watch --json | python3 -m json.tool
```

```json
{
  "ts": 1787000000,
  "filesystems": 6,
  "total_bytes": 5632925171712,
  "used_bytes": 4209478492160,
  "avail_bytes": 1423446679552,
  "mounts": [
    {"device":"/dev/nvme0n1p2","mountpoint":"/","fstype":"ext4","ro":false,"ok":true,"total":511101108224,"used":217216971776,"avail":293884136448,"use_pct":42.5}
  ]
}
```

## Cómo funciona

1. **Parseo de `/proc/mounts`**: 6 campos por línea (`device mountpoint fstype options dump pass`).
   Los puntos de montaje pueden contener escapes octales (`\040` = espacio, `\134` = backslash)
   que se decodifican para obtener la ruta real.
2. **Filtro**: se descartan por defecto los filesystems pseudo/sintéticos
   (`proc`, `sysfs`, `devpts`, `devtmpfs`, `cgroup`, `cgroup2`, `overlay`, `squashfs`, ...)
   que no almacenan datos de usuario. `--all` los muestra.
3. **`statvfs(2)`** sobre cada punto de montaje para obtener bloques totales/libres/reservados
   con `f_frsize` real: `total = f_blocks × f_frsize`, `used = (f_blocks − f_bfree) × f_frsize`,
   `avail = f_bavail × f_frsize` (espacio realmente usable para el usuario, sin reserva root).
4. **Detección de solo-lectura**: el flag `ro` en las opciones del mount.
5. **Modo watch**: map mountpoint → `avail` del frame anterior para calcular deltas.

## Features

- Table de todos los filesystems con tamaño, usado, disponible, % de uso y marcador RO
- Filtro de filesystems pseudo/sintéticos con `--all` para verlos todos
- Modo watch (`--watch N`) con deltas de espacio disponible entre frames y `--timeout T`
- Salida JSON (`--json`) con snapshot completo, validada con `python3 -m json.tool`
- Decodificación de escapes octales de `/proc/mounts` (`\040`, `\134`...)
- Marcador visual de color por uso (verde < 75 %, amarillo 75–90 %, rojo ≥ 90 %)
- `--demo` con datos sintéticos realistas para contenedores/VMs
- `--no-color` con paleta ANSI mutable que se vacía por completo
- 30 tests de auto-verificación (`--selftest`)

## Limitaciones

- `statvfs` reporta el espacio del filesystem, no cuotas por usuario ni uso de proyectos
  concretos dentro de un mismo mount.
- Los montajes `tmpfs` se incluyen (ocupan RAM) — son los únicos "sintéticos" que se
  muestran por defecto, marcados con su tamaño real.
- En contenedores, los archivos bind-mount (`/etc/hosts`, `/etc/resolv.conf`...) aparecen
  como montajes con el device del filesystem que los respalda.

Licencia MIT.