# Compilación LAB autorizada para ESP32-C3

Esta variante incorpora el método de enlazado observado en Radio Ink para el
core **Arduino-ESP32 3.3.0 / ESP-IDF 5.5** que ya se verificó en este proyecto.
No cambia la biblioteca global de Arduino. El build normal en Arduino IDE
mantiene el filtro original y no habilita la transmisión LAB.

## Compilar

En Windows, desde la raíz del repositorio:

```powershell
& .\firmware\build-lab.ps1
```

El script exige el SHA-256 conocido de `libnet80211.a`; se detiene si hay otro
core o biblioteca. El resultado queda en `.build\lab-firmware\C3BleAnalyzer.ino.bin`.
No flashea la placa automáticamente.

## Una prueba en tu propia red

1. Flasheá el binario LAB en tu ESP32-C3. Conectá por BLE la app y por USB
   un monitor serie a 115200 baudios.
2. En la app, escaneá y seleccioná el BSSID de tu AP WPA personal. Iniciá
   el monitor. No uses un SSID oculto ni un AP abierto para esta variante.
3. En el monitor serie enviá una sola línea: `LAB_VERIFY|tu_clave_wifi`.
   El C3 se asocia al BSSID exacto, comprueba `WL_CONNECTED` y el BSSID,
   desconecta sin guardar credenciales en NVS y reinicia el monitor.
   El firmware no imprime la clave. Cuidá el historial o eco del programa
   de terminal que uses.
4. Tenés 120 segundos para que reaparezca un cliente propio en la lista LAB
   y pulsar «Reconexión controlada» en la app. El armado dura 30 segundos.
   Una verificación permite **un solo intento**, aunque falle. Para otra
   prueba hay que verificar de nuevo la clave por USB.
5. Si cambiás de red, parás o reiniciás el monitor, o se desconecta BLE,
   se revoca la autorización.

Solo se envía una trama unicast al cliente elegido y visto recientemente
en el BSSID seleccionado. No hay broadcast, barrido, repetición ni modo
automático. Conocer la clave prueba acceso, no propiedad: usá equipos propios
o una autorización explícita.

## Qué comprobar

`ESP_OK` significa que el driver aceptó el paquete; no demuestra transmisión
por aire ni reconexión del cliente. Comprobá con otra captura o el estado del
cliente. Si se negoció PMF/802.11w, una deauth sin protección puede ignorarse.
Esta rama no se ha probado físicamente con una placa.

El método de Radio Ink sustituye una comprobación global del SDK durante el
enlazado. Por eso este binario experimental exige además el filtro del firmware,
la verificación local y un único intento. No distribuyas el binario como
versión general sin revisión y pruebas de hardware.
