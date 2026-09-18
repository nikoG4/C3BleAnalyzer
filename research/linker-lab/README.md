# Laboratorio práctico de archivos estáticos y linking para ESP32-C3

Este laboratorio construye una biblioteca sintética `liblabguard.a` y cuatro
firmwares independientes para ESP32-C3. No lee ni modifica ninguna biblioteca
de Arduino, ESP-IDF o del driver Wi-Fi. Todo lo generado vive bajo este
directorio y se revierte eliminando `build/`.

## Requisitos detectados

- Arduino-ESP32 3.3.0, FQBN `esp32:esp32:esp32c3`.
- Arduino CLI 1.2.0.
- Toolchain `esp-rv32` 2411.
- GCC 14.2.0 y binutils 2.43.1.
- PowerShell en Windows.

## Ejecutar

Desde la raíz del repositorio:

```powershell
& .\research\linker-lab\tools\build.ps1
& .\research\linker-lab\tools\inspect.ps1
```

Para borrar únicamente los productos del laboratorio:

```powershell
& .\research\linker-lab\tools\clean.ps1
```

`build.ps1` valida las rutas antes de borrar `build/`. No escribe fuera de
`research/linker-lab/` salvo por las cachés normales que use Arduino CLI.

## Árbol relevante

```text
original/
  include/labguard.h
  src/labguard.c
  src/archive_marker.c
  LinkerLabOriginal/LinkerLabOriginal.ino
patched/
  src/labguard.c
  weak/labguard_weak.c
  weak/LinkerLabWeak/LinkerLabWeak.ino
  wrap/LinkerLabWrap/LinkerLabWrap.ino
  replaced/LinkerLabReplaced/LinkerLabReplaced.ino
tools/
  build.ps1
  inspect.ps1
  clean.ps1
build/
  archives/
  objects/
  extracted/
  libraries/
  inspection/
  firmware/
  logs/
```

`archive_marker.c` coloca una segunda función en otro miembro. Así se puede ver
que el linker selecciona miembros individuales de una biblioteca y que la
biblioteca sigue interviniendo incluso cuando la función principal se resuelve
mediante un símbolo fuerte o `--wrap`.

## Comandos binarios utilizados

Las rutas completas se calculan en `tools/build.ps1`. Estos son los comandos
efectivos, abreviando las rutas con variables para poder leerlos:

```powershell
# Compilar objetos RISC-V
& $Cc -march=rv32imc_zicsr_zifencei -mabi=ilp32 -Os -g3 `
  -ffunction-sections -fdata-sections -fno-jump-tables `
  -fno-tree-switch-conversion -std=gnu17 -I$IncludeDir `
  -c $Source -o $Object

# Crear e indexar el archivo
& $Ar rcs $OriginalArchive $OriginalObject $MarkerObject
& $Ranlib $OriginalArchive

# Listar, inspeccionar y desensamblar
& $Ar t $OriginalArchive
& $Nm -A -C --defined-only $OriginalArchive
& $ReadElf -Ws $OriginalObject
& $ObjDump -d -C $OriginalObject

# Extraer todos los miembros
Push-Location $ExtractedDir
& $Ar x $OriginalArchive
Pop-Location

# Sustituir solo labguard.o en una copia del archivo
Copy-Item $OriginalArchive $ReplacedArchive
& $Ar d $ReplacedArchive labguard.o
& $Ar r $ReplacedArchive $PatchedObject
& $Ranlib $ReplacedArchive

# Compilar cada firmware con su biblioteca precompilada local
& $ArduinoCli compile --fqbn esp32:esp32:esp32c3 `
  --build-path $BuildPath --output-dir $OutputPath `
  --libraries $LocalLibraryRoot --warnings all $SketchDirectory
```

La variante wrap añade mediante `library-wrap.properties`:

```text
-llabguard -Wl,--wrap=lab_frame_sanity_check
```

## 1. Biblioteca original

`ar t` produce:

```text
labguard.o
archive_marker.o
```

`nm -A -C --defined-only liblabguard.a` atribuye inequívocamente la función:

```text
liblabguard.a:labguard.o:00000000 T lab_frame_sanity_check
liblabguard.a:archive_marker.o:00000000 T labguard_archive_marker
```

`readelf -Ws labguard.o` muestra:

```text
599: 00000000 18 FUNC GLOBAL DEFAULT 11 lab_frame_sanity_check
```

Por tanto, el símbolo original es una función global fuerte en la sección 11,
`.text.lab_frame_sanity_check`.

El disassembly original es:

```text
00000000 <lab_frame_sanity_check>:
   0: 0c000793  li  a5,192
   4: 00f50463  beq a0,a5,c
   8: 4501      li  a0,0
   a: 8082      ret
   c: 10200513  li  a0,258
  10: 8082      ret
```

El ELF final conserva `4201a1d0 T lab_frame_sanity_check`. El `.map` atribuye
esa dirección a `liblabguard.a(labguard.o)`. En una placa, el sketch está
preparado para imprimir:

```text
ORIGINAL:
type=0xC0
result=258 / 0x102
```

## 2. Weak frente a strong

La variante de archivo declara la función con `__attribute__((weak))`.
`readelf` confirma:

```text
599: 00000000 18 FUNC WEAK DEFAULT 11 lab_frame_sanity_check
```

El sketch aporta una implementación fuerte que devuelve cero. En el ELF final:

```text
42000020 T lab_frame_sanity_check
```

El `.map` atribuye el símbolo al objeto del sketch, no a `labguard.o`. El
miembro débil no se extrae porque la referencia ya quedó satisfecha por el
símbolo fuerte. `archive_marker.o` sí se extrae de la misma biblioteca.

La implementación fuerte usa `__attribute__((noipa, used))`. Esto impide que
LTO propague el cero y que `--gc-sections` elimine después el símbolo; así la
resolución queda visible en `nm`, `readelf` y el mapa.

## 3. Linker wrap

La opción `--wrap=lab_frame_sanity_check` transforma referencias no definidas a
la función en referencias a `__wrap_lab_frame_sanity_check`. El ELF contiene:

```text
42000020 T __wrap_lab_frame_sanity_check
```

El miembro original `labguard.o` no se extrae porque ya no hay una referencia
pendiente a su símbolo; `archive_marker.o` demuestra que la biblioteca local sí
fue enlazada. Este mecanismo no cambia el archivo `.a`.

## 4. Extracción y sustitución de un objeto

El archivo original se copia a `build/archives/replaced/liblabguard.a`.
`ar d` elimina solamente `labguard.o`; `ar r` inserta el objeto compilado desde
`patched/src/labguard.c`. `archive_marker.o` se conserva.

El disassembly del objeto nuevo es:

```text
00000000 <lab_frame_sanity_check>:
   0: 4501  li  a0,0
   2: 8082  ret
```

El ELF final conserva un símbolo global de archivo:

```text
4201a1d8 T lab_frame_sanity_check
```

El `.map` lo atribuye a la copia local
`liblabguard.a(labguard.o)`, cuya sección mide cuatro bytes en vez de los 18
bytes originales.

Los tres sketches modificados están preparados para mostrar `result=0` por
Serial. No se ha inferido este resultado desde hardware: se demuestra a nivel
de código, objetos y enlace, pero la prueba física queda pendiente.

## Resultados del build verificado

| Variante | Flash | Símbolo final relevante | Resultado programado |
|---|---:|---|---:|
| original | 285654 B | `T lab_frame_sanity_check` desde archivo | 258 / `0x102` |
| weak/strong | 285664 B | `T lab_frame_sanity_check` desde sketch | 0 |
| wrap | 285664 B | `T __wrap_lab_frame_sanity_check` | 0 |
| object replacement | 285654 B | `T lab_frame_sanity_check` desde archivo reemplazado | 0 |

Todos usan 11.840 bytes de variables globales. Cada salida contiene `.elf`,
`.map`, `.bin`, `.merged.bin`, bootloader y particiones bajo
`build/firmware/<variante>/`. Las salidas completas de `nm`, `readelf`,
`objdump` y los extractos de mapa están en `build/inspection/`.

## Estado de prueba

- BUILD ORIGINAL: sí.
- BUILD WEAK/STRONG: sí.
- BUILD WRAP: sí.
- BUILD OBJECT REPLACEMENT: sí.
- HARDWARE TEST: pendiente; no se flasheó ninguna placa.

El comportamiento Serial indicado es el comportamiento compilado, no una
captura física. Para una prueba real hay que elegir puerto y confirmar
explícitamente antes de ejecutar `arduino-cli upload`.

## Volver al estado anterior

Ejecuta `tools/clean.ps1` o elimina manualmente `research/linker-lab/build/`.
Los fuentes del laboratorio pueden conservarse para repetirlo. Ningún archivo
del core Arduino, ESP-IDF, toolchain o firmware principal fue modificado.
