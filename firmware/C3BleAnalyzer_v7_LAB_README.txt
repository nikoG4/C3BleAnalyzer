C3 BLE Analyzer v7 - LAB unicast deauth

Firmware:
  C3BleAnalyzer_v7_lab_unicast_deauth.ino

Objetivo:
  Añade una prueba controlada de reconexión por DEAUTH unicast al firmware v6.

Salvaguardas implementadas:
  - Nunca usa broadcast ni multicast.
  - Requiere red seleccionada y monitor activo.
  - El cliente objetivo debe haberse observado intercambiando Data con el BSSID seleccionado.
  - Solo acepta clientes vistos en los últimos 30 s.
  - Requiere armado explícito durante 30 s.
  - El armado se consume con un único intento.
  - Cooldown de 10 s.
  - Se desarma al cambiar red, iniciar o parar el monitor.
  - No hace channel hopping, barridos, flooding ni deauth automático.

Comandos BLE nuevos:
  LAB_STATUS
  LAB_CLIENTS
  LAB_ARM|LAB
  LAB_DEAUTH|AA:BB:CC:DD:EE:FF
  LAB_DISARM

Flujo manual de prueba:
  1. SELECT|<BSSID>
  2. START
  3. LAB_CLIENTS
  4. LAB_ARM|LAB
  5. LAB_DEAUTH|<MAC de un cliente propio visto>

Eventos BLE nuevos:
  {"t":"lab",...}
  {"t":"lab_client",...}
  {"t":"lab_deauth",...}

Importante sobre ESP32-C3/ESP-IDF:
  esp_wifi_80211_tx() no documenta oficialmente DEAUTH entre sus management frames
  soportados. En algunas versiones del driver el intento puede devolver
  ESP_ERR_INVALID_ARG. El firmware informa ese error y no reintenta automáticamente.

Si PMF/802.11w está activo y negociado, el cliente puede ignorar una trama DEAUTH
no protegida aunque el driver haya aceptado la transmisión.
