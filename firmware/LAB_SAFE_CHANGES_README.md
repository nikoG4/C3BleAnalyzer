# Diagnóstico LAB y compilación ESP32-C3

Fecha de verificación: 2026-09-18.

## Entorno verificado

- Arduino CLI: 1.2.0 (`9c495211`).
- Paquete/core: `esp32:esp32` 3.3.0 (Arduino-ESP32 3.3.0).
- ESP-IDF embebido: 5.5, rama `release/v5.5`, commit `b66b5448e0`
  (`IDF_VER=v5.5-1-gb66b5448e0`).
- Bibliotecas precompiladas: `idf-release_v5.5-b66b5448-v1`.
- Toolchain RISC-V: paquete `esp-rv32` 2411; GCC 14.2.0
  (`crosstool-NG esp-14.2.0_20241119`).
- GNU objcopy: 2.43.1.
- FQBN usado: `esp32:esp32:esp32c3` (ESP32C3 Dev Module).

La biblioteca enlazada por el mapa de compilación fue la copia instalada por
Arduino-ESP32 bajo el perfil local del usuario. En Windows una ruta equivalente
es:

```text
%LOCALAPPDATA%\Arduino15\packages\esp32\tools\esp32-arduino-libs\idf-release_v5.5-b66b5448-v1\esp32c3\lib\libnet80211.a
```

SHA-256 del original, que no fue modificado:

```text
2929D7B8634D7570D4E2466B1B518CF2D63652E6EB1D7A6862F5415040227A8A
```

## Cambios del firmware

- Se descartan direcciones broadcast y multicast antes de registrarlas como
  clientes observados. `FF:FF:FF:FF:FF:FF` no puede entrar en esa tabla.
- El cooldown solo comienza cuando `esp_wifi_80211_tx()` devuelve `ESP_OK`.
  Un intento fallido sigue consumiendo el armado manual, conservando la regla
  de una única acción por armado.
- El resultado se registra con `esp_err_to_name(err)`, valor decimal y valor
  hexadecimal. El evento BLE también incorpora `err_name` y `err_hex` sin
  eliminar los campos existentes.
- No se cambió la app Android ni se eliminó BLE, captura EAPOL o PCAP.
- `C3BleAnalyzer.ino` es el archivo canónico y es idéntico byte a byte a
  `C3BleAnalyzer_v7_lab_unicast_deauth.ino` (SHA-256 de ambos en esa
  verificación:
  `8FB9C7B5711939E13FE9FE33A8A87FDDDE02C2ECE7D5EC7D348CAEDC684DE555`).

No se modificó, sustituyó ni parcheó ninguna biblioteca global. Tampoco se
incluye un mecanismo para omitir la validación de tramas del driver Wi-Fi.

## Compilación verificada

Arduino CLI exige que el archivo principal y su carpeta tengan el mismo nombre.
Para verificar el archivo conservando la estructura actual del repositorio se
copió temporalmente el archivo canónico a una carpeta homónima dentro del
directorio temporal del sistema y se ejecutó un comando equivalente a:

```powershell
arduino-cli compile --fqbn esp32:esp32:esp32c3 `
  --output-dir .build\canonical --warnings all `
  "$env:TEMP\c3bleanalyzer-canonical-build\C3BleAnalyzer"
```

Resultado de esa verificación:

- Compilación: correcta.
- Flash: 1.212.848 / 1.310.720 bytes (92%).
- Variables globales: 69.272 / 327.680 bytes (21%).
- Bin de aplicación: `.build\canonical\C3BleAnalyzer.ino.bin`.
- Bin combinado: `.build\canonical\C3BleAnalyzer.ino.merged.bin`.
- SHA-256 del bin de aplicación:
  `B02F578DE5B99F1FCBC2A858C74D1B16D97628B436E3B10A6860A1B6FC458C8F`.

El comportamiento físico no fue ensayado en esa verificación ni se flasheó el
dispositivo. Por tanto, el único resultado observado disponible en ese punto
seguía siendo el rechazo previo `ESP_ERR_INVALID_ARG` (258 / `0x102`); no había
evidencia de `ESP_OK`.

La compilación producía advertencias preexistentes sobre `memset` en structs,
operaciones sobre `volatile`, una variable `install` sin uso y la deprecación
de `BLE2902`; no producía errores.

## Privacidad de la documentación

Las rutas de este documento son deliberadamente portables. No deben añadirse
rutas absolutas que contengan nombres de usuario, carpetas personales, SSID,
BSSID reales, capturas privadas ni otros datos del entorno de laboratorio.
