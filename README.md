# C3 BLE Analyzer v0.5 — Robust BLE + LAB unicast

Proyecto Android + firmware ESP32-C3 para laboratorio propio/autorizado.

## Qué trae esta versión

- App Android con BLE robusto: reconexión automática, re-descubrimiento de servicios, re-suscripción de notificaciones y `STATUS` al reconectar.
- Captura pasiva Beacon/Probe Response + EAPOL para exportar PCAP.
- Firmware v7 con modo LAB unicast limitado.
- App v0.5 con sección **Prueba de reconexión — LAB**.

## Modo LAB en la app

La app ahora muestra clientes vistos por el ESP32-C3 dentro del BSSID seleccionado y permite una prueba de reconexión controlada:

1. Seleccioná tu red de laboratorio.
2. Tocá **Iniciar monitor**.
3. Esperá a que aparezcan clientes o tocá **Actualizar clientes LAB**.
4. Elegí un cliente propio.
5. Tocá **Reconexión controlada**.
6. Confirmá **Armar y ejecutar**.

La app manda:

```text
LAB_ARM|LAB
LAB_DEAUTH|AA:BB:CC:DD:EE:FF
```

No hay botón de broadcast, no hay repetición automática y al reconectar BLE no se repite la acción.

## Salvaguardas del firmware

- Solo BSSID seleccionado manualmente.
- Solo monitor activo.
- Solo clientes vistos por el C3 comunicándose con ese BSSID.
- Cliente visto en los últimos 30 segundos.
- Solo unicast, nunca broadcast/multicast.
- Un intento por armado.
- Cooldown de 10 segundos.

## Compilar Android en Windows

Si Gradle no encuentra el SDK, creá `local.properties` en la raíz:

```cmd
echo sdk.dir=C:/Users/ll/AppData/Local/Android/Sdk>local.properties
```

Luego:

```cmd
build-apk.cmd
```

o:

```cmd
gradle :app:assembleDebug
```

APK esperado:

```text
app\build\outputs\apk\debug\app-debug.apk
```

## Firmware

Abrí en Arduino IDE:

```text
firmware/C3BleAnalyzer.ino
```

Seleccioná tu placa ESP32-C3 y compilá/flasheá.

## Nota técnica

En algunos core Arduino-ESP32/ESP-IDF el driver puede rechazar `esp_wifi_80211_tx()` con tramas Deauthentication porque no está documentado oficialmente como tipo soportado. En ese caso la app mostrará el error devuelto por el firmware.
