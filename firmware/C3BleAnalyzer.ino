#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_err.h>

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

// ============================================================
// C3 BLE Analyzer v7
// ESP32-C3 + Arduino-ESP32 3.x
//
// Monitor pasivo/autorizado:
// - BLE para control desde Android
// - escaneo Wi-Fi 2.4 GHz
// - selección por BSSID
// - monitor promiscuo pasivo
// - detección EAPOL M1/M2/M3/M4
// - captura PCAP robusta: Beacon/Probe Response + auth/assoc + EAPOL en RAM
// - visor de handshake enriquecido por BLE
// - exportación del PCAP por BLE en chunks Base64
//
// LAB_DEAUTH experimental y deliberadamente limitado:
// - solo unicast, nunca broadcast/multicast
// - exige BSSID seleccionado + monitor activo
// - el cliente debe haber sido observado asociado al BSSID seleccionado
// - requiere armado temporal explícito por BLE
// - una sola trama por armado + cooldown
//
// IMPORTANTE: esp_wifi_80211_tx() no documenta oficialmente DEAUTH entre
// los tipos de management soportados en ESP-IDF. Dependiendo de la versión
// Arduino-ESP32/IDF, el driver puede rechazar la trama con ESP_ERR_INVALID_ARG.
// En ese caso el firmware lo reporta a Android y no insiste.
//
// No implementa broadcast deauth, barridos, flooding ni cracking.
// ============================================================

// ---------------- BLE ----------------
static const char* DEVICE_NAME  = "C3-BLE-Analyzer";
static const char* SERVICE_UUID = "7f000001-5a23-4b8f-9c40-1f4b3f80a001";
static const char* COMMAND_UUID = "7f000002-5a23-4b8f-9c40-1f4b3f80a001";
static const char* EVENT_UUID   = "7f000003-5a23-4b8f-9c40-1f4b3f80a001";

// ---------------- Límites ----------------
constexpr int MAX_NETWORKS = 40;
constexpr int MAX_CLIENTS  = 8;
constexpr size_t MAX_COMMAND_LEN = 95;

// PCAP: contexto del AP + EAPOL. 32 frames de hasta 768 B => ~24 KB de RAM.
constexpr int MAX_PCAP_PACKETS = 32;
constexpr size_t MAX_PCAP_FRAME_LEN = 768;
// 96 bytes -> 128 chars Base64; el JSON completo queda cómodo dentro de MTU 247.
constexpr size_t PCAP_CHUNK_RAW = 96;

// ---------------- LAB: reconexión controlada ----------------
constexpr uint32_t LAB_ARM_WINDOW_MS = 30000;   // 30 s para ejecutar una prueba
constexpr uint32_t LAB_DEAUTH_COOLDOWN_MS = 10000; // mínimo 10 s entre intentos
constexpr uint32_t LAB_AUTH_WINDOW_MS = 120000; // autorización local para una prueba
constexpr uint32_t LAB_CONNECT_TIMEOUT_MS = 12000;

// ============================================================
// MODELOS
// ============================================================

struct NetworkInfo {
  String ssid;
  String bssid;
  uint8_t bssidBytes[6] = {};
  int32_t rssi = 0;
  uint8_t channel = 0;
  wifi_auth_mode_t auth = WIFI_AUTH_OPEN;
};

struct ClientHandshake {
  bool used = false;
  uint8_t mac[6] = {};
  uint8_t seenMask = 0;      // bit0 M1, bit1 M2, bit2 M3, bit3 M4
  uint32_t eapolCount = 0;
  uint16_t messageCount[4] = {};
  uint16_t retransmissions = 0;
  uint32_t lastSeen = 0;
  uint64_t lastReplay = 0;
};

struct ObservedClient {
  bool used = false;
  uint8_t mac[6] = {};
  uint32_t lastSeen = 0;
};

struct CapturedFrame {
  uint32_t tsUs = 0;         // timestamp relativo del driver Wi-Fi
  uint16_t len = 0;          // frame sin FCS
  uint8_t data[MAX_PCAP_FRAME_LEN] = {};
};

struct __attribute__((packed)) PcapGlobalHeader {
  uint32_t magicNumber;
  uint16_t versionMajor;
  uint16_t versionMinor;
  int32_t thisZone;
  uint32_t sigFigs;
  uint32_t snapLen;
  uint32_t network;
};

struct __attribute__((packed)) PcapPacketHeader {
  uint32_t tsSec;
  uint32_t tsUsec;
  uint32_t inclLen;
  uint32_t origLen;
};

// ============================================================
// ESTADO GLOBAL
// ============================================================

NetworkInfo networks[MAX_NETWORKS];
int networkCount = 0;

NetworkInfo selectedNetwork;
bool hasSelectedNetwork = false;

ClientHandshake clients[MAX_CLIENTS];
ObservedClient labObservedClients[MAX_CLIENTS];

BLEServer* bleServer = nullptr;
BLECharacteristic* eventCharacteristic = nullptr;
volatile bool bleConnected = false;
volatile bool bleAdvertisingRestartPending = false;
volatile uint32_t bleDisconnectedAt = 0;
uint32_t bleConnectionCount = 0;

volatile bool monitorRunning = false;
uint8_t monitorBssid[6] = {};
uint8_t monitorChannel = 0;
volatile uint32_t totalEapol = 0;
volatile int latestClient = -1;

CapturedFrame pcapFrames[MAX_PCAP_PACKETS];
volatile uint16_t pcapPacketCount = 0;
volatile uint16_t pcapEapolCount = 0;
// Para compatibilidad con la app, beaconCaptured significa "contexto AP util capturado".
// apContextSubtype indica que fue realmente: 8=Beacon, 5=Probe Response.
volatile bool beaconCaptured = false;
volatile uint8_t apContextSubtype = 0;
volatile uint8_t pcapMgmtContextCount = 0;
char monitorSsid[33] = {};
uint8_t monitorSsidLen = 0;
volatile bool pcapOverflow = false;
volatile bool pcapExportInProgress = false;

// LAB_DEAUTH se controla únicamente desde loop()/comandos BLE.
bool labDeauthArmed = false;
uint32_t labDeauthArmDeadline = 0;
uint32_t lastLabDeauthMs = 0;
bool labNetworkVerified = false;
uint8_t labVerifiedBssid[6] = {};
uint32_t labVerifyDeadline = 0;
volatile bool labRevokeOnBleDisconnect = false;

// El callback BLE solo copia el comando. Se procesa luego en loop().
char pendingCommand[MAX_COMMAND_LEN + 1] = {};
volatile bool commandPending = false;

portMUX_TYPE monitorMux = portMUX_INITIALIZER_UNLOCKED;
portMUX_TYPE commandMux = portMUX_INITIALIZER_UNLOCKED;

void clearLabAuthorization() {
  labNetworkVerified = false;
  labDeauthArmed = false;
  labVerifyDeadline = 0;
  memset(labVerifiedBssid, 0, sizeof(labVerifiedBssid));
}

bool labAuthorizationValid() {
  if (!labNetworkVerified || !hasSelectedNetwork || !monitorRunning ||
      memcmp(labVerifiedBssid, selectedNetwork.bssidBytes, 6) != 0 ||
      static_cast<int32_t>(millis() - labVerifyDeadline) >= 0) {
    clearLabAuthorization();
    return false;
  }
  return true;
}

void wipeSecret(char* data, size_t length) {
  volatile char* p = data;
  while (length-- > 0) *p++ = 0;
}

// ============================================================
// UTILIDADES
// ============================================================

bool sameMac(const uint8_t* a, const uint8_t* b) {
  return memcmp(a, b, 6) == 0;
}

bool isBroadcastMac(const uint8_t* mac) {
  static const uint8_t ff[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
  return sameMac(mac, ff);
}

bool isMulticastMac(const uint8_t* mac) {
  return (mac[0] & 0x01U) != 0;
}

bool parseMac(const String& text, uint8_t out[6]) {
  unsigned int b[6];
  if (sscanf(
        text.c_str(),
        "%x:%x:%x:%x:%x:%x",
        &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]
      ) != 6) return false;

  for (int i = 0; i < 6; ++i) {
    if (b[i] > 0xFFU) return false;
    out[i] = static_cast<uint8_t>(b[i]);
  }
  return true;
}

String macToString(const uint8_t* mac) {
  char out[18];
  snprintf(
    out,
    sizeof(out),
    "%02X:%02X:%02X:%02X:%02X:%02X",
    mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]
  );
  return String(out);
}

String authToString(wifi_auth_mode_t auth) {
  switch (auth) {
    case WIFI_AUTH_OPEN:          return "OPEN";
    case WIFI_AUTH_WEP:           return "WEP";
    case WIFI_AUTH_WPA_PSK:       return "WPA";
    case WIFI_AUTH_WPA2_PSK:      return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:  return "WPA/WPA2";
    case WIFI_AUTH_WPA3_PSK:      return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/WPA3";
    default:                      return "OTHER";
  }
}

String jsonEscape(const String& input) {
  String out;
  out.reserve(input.length() + 12);

  for (size_t i = 0; i < input.length(); ++i) {
    const char c = input[i];
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '"':  out += "\\\""; break;
      case '\n': out += "\\n";  break;
      case '\r': out += "\\r";  break;
      case '\t': out += "\\t";  break;
      default:
        if (static_cast<uint8_t>(c) >= 0x20) out += c;
        break;
    }
  }
  return out;
}

uint16_t readBE16(const uint8_t* p) {
  return (static_cast<uint16_t>(p[0]) << 8) | p[1];
}

uint64_t readBE64(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
  return v;
}

String base64Encode(const uint8_t* data, size_t len) {
  static const char table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

  String out;
  out.reserve(((len + 2) / 3) * 4);

  for (size_t i = 0; i < len; i += 3) {
    const uint32_t a = data[i];
    const bool hasB = (i + 1) < len;
    const bool hasC = (i + 2) < len;
    const uint32_t b = hasB ? data[i + 1] : 0;
    const uint32_t c = hasC ? data[i + 2] : 0;
    const uint32_t triple = (a << 16) | (b << 8) | c;

    out += table[(triple >> 18) & 0x3F];
    out += table[(triple >> 12) & 0x3F];
    out += hasB ? table[(triple >> 6) & 0x3F] : '=';
    out += hasC ? table[triple & 0x3F] : '=';
  }

  return out;
}

// ============================================================
// BLE: EVENTOS HACIA ANDROID
// ============================================================

void notifyEvent(const String& json) {
  if (!bleConnected || eventCharacteristic == nullptr) return;

  eventCharacteristic->setValue(json);
  eventCharacteristic->notify();

  // Deja respirar al stack BLE, especialmente durante escaneo/exportación.
  delay(10);
}

void sendError(const String& message) {
  notifyEvent(
    String("{\"t\":\"error\",\"msg\":\"") +
    jsonEscape(message) +
    "\"}"
  );
}

void sendSelectedEvent() {
  if (!hasSelectedNetwork) return;

  String j;
  j.reserve(180);
  j += "{\"t\":\"selected\",\"s\":\"";
  j += jsonEscape(selectedNetwork.ssid);
  j += "\",\"b\":\"";
  j += selectedNetwork.bssid;
  j += "\",\"c\":";
  j += String(selectedNetwork.channel);
  j += ",\"r\":";
  j += String(selectedNetwork.rssi);
  j += ",\"a\":\"";
  j += authToString(selectedNetwork.auth);
  j += "\"}";

  notifyEvent(j);
}

void sendMonitorEvent(bool running) {
  String j;
  j.reserve(64);
  j += "{\"t\":\"monitor\",\"run\":";
  j += running ? "1" : "0";
  j += ",\"c\":";
  j += String(monitorChannel);
  j += "}";
  notifyEvent(j);
}

// ============================================================
// CLIENTES / HANDSHAKE / PCAP
// ============================================================

int findOrCreateClientUnsafe(const uint8_t* mac) {
  int freeSlot = -1;
  int oldestSlot = 0;
  uint32_t oldestTime = 0xFFFFFFFFUL;

  for (int i = 0; i < MAX_CLIENTS; ++i) {
    if (clients[i].used && sameMac(clients[i].mac, mac)) return i;
    if (!clients[i].used && freeSlot < 0) freeSlot = i;

    if (clients[i].used && clients[i].lastSeen < oldestTime) {
      oldestTime = clients[i].lastSeen;
      oldestSlot = i;
    }
  }

  const int idx = (freeSlot >= 0) ? freeSlot : oldestSlot;
  clients[idx] = ClientHandshake{};
  clients[idx].used = true;
  memcpy(clients[idx].mac, mac, 6);
  return idx;
}

int findOrCreateObservedClientUnsafe(const uint8_t* mac) {
  int freeSlot = -1;
  int oldestSlot = 0;
  uint32_t oldestTime = 0xFFFFFFFFUL;

  for (int i = 0; i < MAX_CLIENTS; ++i) {
    if (labObservedClients[i].used && sameMac(labObservedClients[i].mac, mac)) return i;
    if (!labObservedClients[i].used && freeSlot < 0) freeSlot = i;
    if (labObservedClients[i].used && labObservedClients[i].lastSeen < oldestTime) {
      oldestTime = labObservedClients[i].lastSeen;
      oldestSlot = i;
    }
  }

  const int idx = (freeSlot >= 0) ? freeSlot : oldestSlot;
  labObservedClients[idx] = ObservedClient{};
  labObservedClients[idx].used = true;
  memcpy(labObservedClients[idx].mac, mac, 6);
  return idx;
}

void resetMonitorStats() {
  portENTER_CRITICAL(&monitorMux);
  memset(clients, 0, sizeof(clients));
  memset(labObservedClients, 0, sizeof(labObservedClients));
  memset(pcapFrames, 0, sizeof(pcapFrames));
  totalEapol = 0;
  latestClient = -1;
  pcapPacketCount = 0;
  pcapEapolCount = 0;
  beaconCaptured = false;
  apContextSubtype = 0;
  pcapMgmtContextCount = 0;
  pcapOverflow = false;
  portEXIT_CRITICAL(&monitorMux);
}

bool captureFrameUnsafe(
  const uint8_t* frame,
  int frameLenIncludingFcs,
  uint32_t timestampUs,
  bool isEapol
) {
  if (pcapExportInProgress) return false;

  // sig_len incluye el FCS de 4 bytes. PCAP DLT_IEEE802_11
  // se guarda sin FCS para que Wireshark lo decodifique limpio.
  if (frameLenIncludingFcs <= 4) return false;

  const int frameLen = frameLenIncludingFcs - 4;
  if (frameLen <= 0 || frameLen > static_cast<int>(MAX_PCAP_FRAME_LEN)) {
    pcapOverflow = true;
    return false;
  }

  if (pcapPacketCount >= MAX_PCAP_PACKETS) {
    pcapOverflow = true;
    return false;
  }

  CapturedFrame& dst = pcapFrames[pcapPacketCount];
  dst.tsUs = timestampUs;
  dst.len = static_cast<uint16_t>(frameLen);
  memcpy(dst.data, frame, frameLen);
  ++pcapPacketCount;

  if (isEapol) ++pcapEapolCount;
  return true;
}

// ============================================================
// PARSEO DE CONTEXTO 802.11 MANAGEMENT
// ============================================================

bool managementFrameMatchesSelectedAp(const uint8_t* frame, int frameLenNoFcs) {
  if (frame == nullptr || frameLenNoFcs < 24) return false;

  const uint8_t* addr1 = frame + 4;
  const uint8_t* addr2 = frame + 10;
  const uint8_t* addr3 = frame + 16;

  return sameMac(addr1, monitorBssid) ||
         sameMac(addr2, monitorBssid) ||
         sameMac(addr3, monitorBssid);
}

bool managementFrameHasUsableSsid(
  const uint8_t* frame,
  int frameLenNoFcs,
  uint8_t subtype
) {
  // Beacon y Probe Response tienen 24 bytes de MAC header +
  // 12 bytes de fixed parameters antes de los Information Elements.
  if (subtype != 8 && subtype != 5) return false;
  if (frame == nullptr || frameLenNoFcs < 36) return false;

  int pos = 36;
  while (pos + 2 <= frameLenNoFcs) {
    const uint8_t id = frame[pos];
    const uint8_t ieLen = frame[pos + 1];
    pos += 2;

    if (pos + ieLen > frameLenNoFcs) return false;

    if (id == 0) { // SSID IE
      if (ieLen == 0 || ieLen > 32) return false;

      // Si sabemos el SSID seleccionado, exigimos que coincida. Esto evita
      // guardar una trama de contexto que hcx no pueda asociar correctamente.
      if (monitorSsidLen > 0) {
        if (ieLen != monitorSsidLen) return false;
        return memcmp(frame + pos, monitorSsid, monitorSsidLen) == 0;
      }

      // Para una red marcada como oculta aceptamos cualquier SSID no vacio
      // que aparezca en un Probe Response pasivo.
      return true;
    }

    pos += ieLen;
  }

  return false;
}

bool isUsefulAssociationManagementSubtype(uint8_t subtype) {
  // Association Request/Response, Reassociation Request/Response y Authentication.
  return subtype == 0 || subtype == 1 || subtype == 2 || subtype == 3 || subtype == 11;
}

// ============================================================
// WIFI PROMISCUOUS CALLBACK
// ============================================================

void wifiSnifferCallback(void* buf, wifi_promiscuous_pkt_type_t type) {
  if (!monitorRunning || buf == nullptr) return;
  if (type != WIFI_PKT_DATA && type != WIFI_PKT_MGMT) return;

  auto* packet = static_cast<wifi_promiscuous_pkt_t*>(buf);
  const uint8_t* frame = packet->payload;
  const int len = packet->rx_ctrl.sig_len;

  if (frame == nullptr || len < 28) return;
  if (packet->rx_ctrl.rx_state != 0) return;

  const uint16_t fc = frame[0] | (static_cast<uint16_t>(frame[1]) << 8);
  const uint8_t frameType = (fc >> 2) & 0x03;
  const uint8_t subtype = (fc >> 4) & 0x0F;

  // ----------------------------------------------------------
  // Management frames del AP seleccionado.
  //
  // hcxpcapngtool necesita especialmente un Beacon o Probe Response con
  // ESSID para derivar la PMK. Guardamos exactamente UNO como contexto AP.
  // También conservamos unas pocas tramas Authentication/Association si
  // aparecen durante una reconexion normal; no son obligatorias para el
  // handshake, pero hacen el dump mas completo para diagnostico.
  // ----------------------------------------------------------
  if (type == WIFI_PKT_MGMT && frameType == 0) {
    const int frameLenNoFcs = len - 4;
    if (frameLenNoFcs < 24) return;
    if (!managementFrameMatchesSelectedAp(frame, frameLenNoFcs)) return;

    if (subtype == 8 || subtype == 5) { // Beacon / Probe Response
      if (!managementFrameHasUsableSsid(frame, frameLenNoFcs, subtype)) return;

      portENTER_CRITICAL(&monitorMux);
      if (!beaconCaptured) {
        if (captureFrameUnsafe(frame, len, packet->rx_ctrl.timestamp, false)) {
          beaconCaptured = true;
          apContextSubtype = subtype;
        }
      }
      portEXIT_CRITICAL(&monitorMux);
      return;
    }

    if (isUsefulAssociationManagementSubtype(subtype)) {
      portENTER_CRITICAL(&monitorMux);
      // Limita el ruido y deja amplio espacio para EAPOL y el Beacon.
      if (pcapMgmtContextCount < 8 && pcapPacketCount < (MAX_PCAP_PACKETS - 8)) {
        if (captureFrameUnsafe(frame, len, packet->rx_ctrl.timestamp, false)) {
          ++pcapMgmtContextCount;
        }
      }
      portEXIT_CRITICAL(&monitorMux);
      return;
    }

    return;
  }

  // Desde aquí solo Data frames.
  if (type != WIFI_PKT_DATA || frameType != 2) return;

  const bool toDS   = (fc & 0x0100) != 0;
  const bool fromDS = (fc & 0x0200) != 0;
  if (toDS == fromDS) return;

  const uint8_t* addr1 = frame + 4;
  const uint8_t* addr2 = frame + 10;

  const uint8_t* clientMac = nullptr;
  bool apToClient = false;
  bool clientToAp = false;

  if (toDS && !fromDS) {
    if (!sameMac(addr1, monitorBssid)) return;
    clientMac = addr2;
    clientToAp = true;
  } else if (!toDS && fromDS) {
    if (!sameMac(addr2, monitorBssid)) return;
    clientMac = addr1;
    apToClient = true;
  }

  if (clientMac == nullptr) return;

  // Las direcciones broadcast/multicast no representan una estación cliente.
  // Filtrarlas antes de tocar cualquiera de las tablas evita que
  // FF:FF:FF:FF:FF:FF (u otra dirección de grupo) aparezca en la UI.
  if (isBroadcastMac(clientMac) || isMulticastMac(clientMac)) return;

  // Registra clientes vistos intercambiando Data con el AP seleccionado,
  // aunque todavía no haya aparecido un EAPOL. Esto permite que el modo LAB
  // solo pueda apuntar a una estación realmente observada en esa BSSID.
  const uint32_t observedNow = millis();
  portENTER_CRITICAL(&monitorMux);
  const int observedIdx = findOrCreateObservedClientUnsafe(clientMac);
  labObservedClients[observedIdx].lastSeen = observedNow;
  portEXIT_CRITICAL(&monitorMux);

  int headerLength = 24;
  if ((subtype & 0x08) != 0) {
    headerLength += 2;
    if ((fc & 0x8000) != 0) headerLength += 4;
  }

  if (len < headerLength + 8 + 17 + 4) return;

  const uint8_t* llc = frame + headerLength;
  if (
    llc[0] != 0xAA || llc[1] != 0xAA || llc[2] != 0x03 ||
    llc[6] != 0x88 || llc[7] != 0x8E
  ) return;

  const uint8_t* eapol = llc + 8;
  const int available = (len - 4) - headerLength - 8;
  if (available < 17) return;

  portENTER_CRITICAL(&monitorMux);
  ++totalEapol;
  captureFrameUnsafe(frame, len, packet->rx_ctrl.timestamp, true);
  portEXIT_CRITICAL(&monitorMux);

  if (eapol[1] != 3) return;

  const uint16_t bodyLength = readBE16(eapol + 2);
  if (bodyLength < 13) return;
  if (static_cast<int>(bodyLength) + 4 > available) return;

  const uint16_t keyInfo = readBE16(eapol + 5);
  const uint64_t replayCounter = readBE64(eapol + 9);

  const bool pairwise = (keyInfo & 0x0008) != 0;
  const bool install  = (keyInfo & 0x0040) != 0;
  const bool ack      = (keyInfo & 0x0080) != 0;
  const bool mic      = (keyInfo & 0x0100) != 0;
  const bool secure   = (keyInfo & 0x0200) != 0;

  if (!pairwise) return;

  uint8_t message = 0;
  if (apToClient && ack && !mic) {
    message = 1;
  } else if (clientToAp && mic && !ack && !secure) {
    message = 2;
  } else if (apToClient && mic && ack) {
    // M3 es el EAPOL-Key pairwise que vuelve del AP con ACK+MIC.
    // Algunos AP/retransmisiones no presentan INSTALL exactamente como
    // espera un clasificador demasiado estricto, por eso no lo exigimos.
    message = 3;
  } else if (clientToAp && mic && secure && !ack) {
    message = 4;
  }

  if (message == 0) return;

  const uint32_t now = millis();

  portENTER_CRITICAL(&monitorMux);
  const int idx = findOrCreateClientUnsafe(clientMac);
  ClientHandshake& session = clients[idx];

  const uint8_t bit = static_cast<uint8_t>(1U << (message - 1));

  // No reseteamos el progreso al ver un M1 posterior. En una captura real
  // puede aparecer un M1 retransmitido después de M3/M4 y un reset aquí
  // haría desaparecer mensajes que sí fueron capturados. El estado se
  // reinicia al comenzar un nuevo monitor, no dentro del callback.
  if ((session.seenMask & bit) != 0) ++session.retransmissions;

  session.seenMask |= bit;
  ++session.messageCount[message - 1];
  session.lastReplay = replayCounter;
  ++session.eapolCount;
  session.lastSeen = now;
  latestClient = idx;
  portEXIT_CRITICAL(&monitorMux);
}

// ============================================================
// MONITOR
// ============================================================

void stopMonitor() {
  clearLabAuthorization();
  const bool wasRunning = monitorRunning;
  monitorRunning = false;

  if (wasRunning) {
    esp_wifi_set_promiscuous(false);
    Serial.println("[MONITOR] detenido");
  }

  sendMonitorEvent(false);
}

void startMonitor() {
  clearLabAuthorization();
  if (!hasSelectedNetwork) {
    sendError("Selecciona una red primero");
    return;
  }

  if (monitorRunning) {
    monitorRunning = false;
    esp_wifi_set_promiscuous(false);
    delay(20);
  }

  WiFi.scanDelete();
  WiFi.mode(WIFI_STA);
  delay(30);

  monitorChannel = selectedNetwork.channel;
  memcpy(monitorBssid, selectedNetwork.bssidBytes, 6);

  memset(monitorSsid, 0, sizeof(monitorSsid));
  monitorSsidLen = 0;
  if (selectedNetwork.ssid != "[Hidden]") {
    monitorSsidLen = static_cast<uint8_t>(selectedNetwork.ssid.length() > 32 ? 32 : selectedNetwork.ssid.length());
    memcpy(monitorSsid, selectedNetwork.ssid.c_str(), monitorSsidLen);
    monitorSsid[monitorSsidLen] = '\0';
  }

  resetMonitorStats();

  esp_err_t err = esp_wifi_set_channel(monitorChannel, WIFI_SECOND_CHAN_NONE);
  if (err != ESP_OK) {
    sendError(String("No se pudo fijar el canal: ") + String(static_cast<int>(err)));
    return;
  }

  wifi_promiscuous_filter_t filter;
  memset(&filter, 0, sizeof(filter));
  filter.filter_mask = WIFI_PROMIS_FILTER_MASK_DATA | WIFI_PROMIS_FILTER_MASK_MGMT;

  err = esp_wifi_set_promiscuous_filter(&filter);
  if (err != ESP_OK) {
    sendError(String("No se pudo configurar filtro: ") + String(static_cast<int>(err)));
    return;
  }

  err = esp_wifi_set_promiscuous_rx_cb(wifiSnifferCallback);
  if (err != ESP_OK) {
    sendError(String("No se pudo registrar callback: ") + String(static_cast<int>(err)));
    return;
  }

  monitorRunning = true;

  err = esp_wifi_set_promiscuous(true);
  if (err != ESP_OK) {
    monitorRunning = false;
    sendError(String("No se pudo iniciar promiscuo: ") + String(static_cast<int>(err)));
    return;
  }

  // Espera brevemente un Beacon/Probe Response valido. Los AP normalmente
  // emiten Beacon cada ~100 ms, asi que 1.8 s da margen de sobra sin
  // interrumpir BLE ni el callback promiscuo.
  const uint32_t contextDeadline = millis() + 1800;
  while (!beaconCaptured && static_cast<int32_t>(millis() - contextDeadline) < 0) {
    delay(10);
  }

  Serial.printf(
    "[MONITOR] %s | %s | CH %u | contexto=%s\n",
    selectedNetwork.ssid.c_str(),
    selectedNetwork.bssid.c_str(),
    monitorChannel,
    beaconCaptured ? (apContextSubtype == 8 ? "Beacon" : "ProbeResponse") : "PENDIENTE"
  );

  if (!beaconCaptured) {
    Serial.println("[PCAP] Aun no hay Beacon/Probe Response con SSID; se seguira escuchando.");
  }

  sendMonitorEvent(true);
}

// ============================================================
// PCAP STREAM -> BLE
// ============================================================

class PcapBleStreamer {
public:
  void write(const uint8_t* data, size_t len) {
    while (len > 0) {
      const size_t room = PCAP_CHUNK_RAW - used_;
      const size_t take = (len < room) ? len : room;
      memcpy(buffer_ + used_, data, take);
      used_ += take;
      data += take;
      len -= take;

      if (used_ == PCAP_CHUNK_RAW) flush();
    }
  }

  void finish() {
    if (used_ > 0) flush();
  }

  uint32_t chunks() const { return chunkIndex_; }

private:
  uint8_t buffer_[PCAP_CHUNK_RAW] = {};
  size_t used_ = 0;
  uint32_t chunkIndex_ = 0;

  void flush() {
    if (used_ == 0) return;

    const String encoded = base64Encode(buffer_, used_);

    String j;
    j.reserve(encoded.length() + 48);
    j += "{\"t\":\"pcap_chunk\",\"i\":";
    j += String(chunkIndex_);
    j += ",\"d\":\"";
    j += encoded;
    j += "\"}";

    notifyEvent(j);
    ++chunkIndex_;
    used_ = 0;
  }
};

void exportPcap() {
  uint16_t countSnapshot = 0;
  uint16_t eapolSnapshot = 0;
  bool beaconSnapshot = false;
  uint8_t contextSubtypeSnapshot = 0;
  bool overflowSnapshot = false;

  portENTER_CRITICAL(&monitorMux);
  pcapExportInProgress = true;
  countSnapshot = pcapPacketCount;
  eapolSnapshot = pcapEapolCount;
  beaconSnapshot = beaconCaptured;
  contextSubtypeSnapshot = apContextSubtype;
  overflowSnapshot = pcapOverflow;
  portEXIT_CRITICAL(&monitorMux);

  if (eapolSnapshot == 0) {
    pcapExportInProgress = false;
    sendError("Todavia no hay EAPOL para exportar");
    return;
  }

  if (!beaconSnapshot) {
    pcapExportInProgress = false;
    sendError("Falta Beacon/Probe Response con ESSID; espera unos segundos y vuelve a exportar");
    return;
  }

  uint32_t totalSize = sizeof(PcapGlobalHeader);
  for (uint16_t i = 0; i < countSnapshot; ++i) {
    totalSize += sizeof(PcapPacketHeader) + pcapFrames[i].len;
  }

  String begin;
  begin.reserve(100);
  begin += "{\"t\":\"pcap_begin\",\"size\":";
  begin += String(totalSize);
  begin += ",\"packets\":";
  begin += String(countSnapshot);
  begin += ",\"eapol\":";
  begin += String(eapolSnapshot);
  begin += ",\"beacon\":";
  begin += beaconSnapshot ? "1" : "0";
  begin += ",\"ctx\":\"";
  begin += contextSubtypeSnapshot == 8 ? "beacon" : (contextSubtypeSnapshot == 5 ? "probe" : "none");
  begin += "\"";
  begin += ",\"overflow\":";
  begin += overflowSnapshot ? "1" : "0";
  begin += "}";
  notifyEvent(begin);

  PcapBleStreamer stream;

  PcapGlobalHeader gh;
  gh.magicNumber  = 0xA1B2C3D4;
  gh.versionMajor = 2;
  gh.versionMinor = 4;
  gh.thisZone     = 0;
  gh.sigFigs      = 0;
  gh.snapLen      = MAX_PCAP_FRAME_LEN;
  gh.network      = 105; // LINKTYPE_IEEE802_11

  stream.write(reinterpret_cast<const uint8_t*>(&gh), sizeof(gh));

  for (uint16_t i = 0; i < countSnapshot; ++i) {
    const CapturedFrame& f = pcapFrames[i];

    PcapPacketHeader ph;
    ph.tsSec   = f.tsUs / 1000000UL;
    ph.tsUsec  = f.tsUs % 1000000UL;
    ph.inclLen = f.len;
    ph.origLen = f.len;

    stream.write(reinterpret_cast<const uint8_t*>(&ph), sizeof(ph));
    stream.write(f.data, f.len);
  }

  stream.finish();

  String end;
  end.reserve(80);
  end += "{\"t\":\"pcap_end\",\"size\":";
  end += String(totalSize);
  end += ",\"chunks\":";
  end += String(stream.chunks());
  end += "}";
  notifyEvent(end);

  pcapExportInProgress = false;

  Serial.printf(
    "[PCAP] exportado: %u paquetes, %lu bytes, %lu chunks%s\n",
    countSnapshot,
    static_cast<unsigned long>(totalSize),
    static_cast<unsigned long>(stream.chunks()),
    overflowSnapshot ? " (overflow previo)" : ""
  );
}

// ============================================================
// ESCANEO WIFI
// ============================================================

void sortNetworksByRssi() {
  for (int i = 0; i < networkCount - 1; ++i) {
    for (int j = i + 1; j < networkCount; ++j) {
      if (networks[j].rssi > networks[i].rssi) {
        NetworkInfo tmp = networks[i];
        networks[i] = networks[j];
        networks[j] = tmp;
      }
    }
  }
}

void scanWifi() {
  clearLabAuthorization();
  if (monitorRunning) {
    monitorRunning = false;
    esp_wifi_set_promiscuous(false);
    delay(20);
    sendMonitorEvent(false);
  }

  notifyEvent("{\"t\":\"scan_start\"}");

  WiFi.scanDelete();
  WiFi.mode(WIFI_STA);
  delay(30);

  Serial.println("[SCAN] buscando redes...");

  const int found = WiFi.scanNetworks(false, true);

  if (found < 0) {
    networkCount = 0;
    sendError("Fallo el escaneo Wi-Fi");
    notifyEvent("{\"t\":\"scan_end\",\"n\":0}");
    return;
  }

  networkCount = (found > MAX_NETWORKS) ? MAX_NETWORKS : found;

  for (int i = 0; i < networkCount; ++i) {
    NetworkInfo& n = networks[i];

    n.ssid = WiFi.SSID(i);
    if (n.ssid.length() == 0) n.ssid = "[Hidden]";

    n.bssid = WiFi.BSSIDstr(i);

    const uint8_t* bssidPtr = WiFi.BSSID(i);
    if (bssidPtr != nullptr) memcpy(n.bssidBytes, bssidPtr, 6);
    else memset(n.bssidBytes, 0, 6);

    n.rssi = WiFi.RSSI(i);
    n.channel = WiFi.channel(i);
    n.auth = WiFi.encryptionType(i);
  }

  sortNetworksByRssi();

  for (int i = 0; i < networkCount; ++i) {
    const NetworkInfo& n = networks[i];

    String j;
    j.reserve(180);
    j += "{\"t\":\"net\",\"s\":\"";
    j += jsonEscape(n.ssid);
    j += "\",\"b\":\"";
    j += n.bssid;
    j += "\",\"c\":";
    j += String(n.channel);
    j += ",\"r\":";
    j += String(n.rssi);
    j += ",\"a\":\"";
    j += authToString(n.auth);
    j += "\"}";

    notifyEvent(j);
  }

  WiFi.scanDelete();

  notifyEvent(
    String("{\"t\":\"scan_end\",\"n\":") +
    String(networkCount) +
    "}"
  );

  Serial.printf("[SCAN] %d redes enviadas\n", networkCount);
}

void selectByBssid(const String& bssid) {
  for (int i = 0; i < networkCount; ++i) {
    if (networks[i].bssid.equalsIgnoreCase(bssid)) {
      selectedNetwork = networks[i];
      hasSelectedNetwork = true;
      clearLabAuthorization();
      // Una selección nueva no debe conservar un PCAP de la red anterior.
      resetMonitorStats();

      Serial.printf(
        "[SELECT] %s | %s | CH %u\n",
        selectedNetwork.ssid.c_str(),
        selectedNetwork.bssid.c_str(),
        selectedNetwork.channel
      );

      sendSelectedEvent();
      return;
    }
  }

  sendError("BSSID no encontrado; vuelve a escanear");
}

// ============================================================
// LAB: DEAUTH UNICAST CONTROLADO
// ============================================================

bool labArmStillValid() {
  if (!labDeauthArmed) return false;
  if (static_cast<int32_t>(millis() - labDeauthArmDeadline) >= 0) {
    labDeauthArmed = false;
    return false;
  }
  return true;
}

bool observedClientSnapshot(const uint8_t* mac, ObservedClient& snapshot) {
  bool found = false;
  portENTER_CRITICAL(&monitorMux);
  for (int i = 0; i < MAX_CLIENTS; ++i) {
    if (labObservedClients[i].used && sameMac(labObservedClients[i].mac, mac)) {
      snapshot = labObservedClients[i];
      found = true;
      break;
    }
  }
  portEXIT_CRITICAL(&monitorMux);
  return found;
}

void sendLabStatus() {
  const bool verified = labAuthorizationValid();
  const bool armed = verified && labArmStillValid();
  uint32_t ttl = 0;
  if (armed) ttl = labDeauthArmDeadline - millis();

  uint32_t cooldown = 0;
  if (lastLabDeauthMs != 0) {
    const uint32_t elapsed = millis() - lastLabDeauthMs;
    if (elapsed < LAB_DEAUTH_COOLDOWN_MS) cooldown = LAB_DEAUTH_COOLDOWN_MS - elapsed;
  }

  String j;
  j.reserve(120);
  j += "{\"t\":\"lab\",\"armed\":";
  j += armed ? "1" : "0";
  j += ",\"ttl_ms\":";
  j += String(ttl);
  j += ",\"cooldown_ms\":";
  j += String(cooldown);
  j += ",\"verified\":";
  j += verified ? "1" : "0";
  j += ",\"verify_ttl_ms\":";
  j += String(verified ? labVerifyDeadline - millis() : 0);
  j += ",\"mode\":\"unicast-only\"}";
  notifyEvent(j);
}

void sendObservedLabClients() {
  ObservedClient snap[MAX_CLIENTS];
  int n = 0;

  portENTER_CRITICAL(&monitorMux);
  for (int i = 0; i < MAX_CLIENTS; ++i) {
    if (labObservedClients[i].used) snap[n++] = labObservedClients[i];
  }
  portEXIT_CRITICAL(&monitorMux);

  notifyEvent(String("{\"t\":\"lab_clients_begin\",\"n\":") + String(n) + "}");
  for (int i = 0; i < n; ++i) {
    String j;
    j.reserve(100);
    j += "{\"t\":\"lab_client\",\"m\":\"";
    j += macToString(snap[i].mac);
    j += "\",\"age_ms\":";
    j += String(millis() - snap[i].lastSeen);
    j += "}";
    notifyEvent(j);
  }
  notifyEvent("{\"t\":\"lab_clients_end\"}");
}

// La contraseña solo entra por USB Serial; nunca por BLE, PCAP, NVS ni logs.
void verifyLabPassword(char* password) {
#if !defined(LAB_ENABLE_RAW_TX)
  sendError("LAB activo no está compilado; usa la compilación experimental");
  return;
#endif
  if (!bleConnected) {
    Serial.println("[LAB] conecta primero la app por BLE");
    return;
  }
  if (!hasSelectedNetwork || selectedNetwork.ssid == "[Hidden]" ||
      selectedNetwork.auth == WIFI_AUTH_OPEN || password[0] == '\0') {
    sendError("Selecciona un AP WPA con SSID visible e introduce su clave por USB");
    return;
  }

  clearLabAuthorization();
  stopMonitor();
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.begin(selectedNetwork.ssid.c_str(), password,
             selectedNetwork.channel, selectedNetwork.bssidBytes, true);

  const uint32_t deadline = millis() + LAB_CONNECT_TIMEOUT_MS;
  while (WiFi.status() != WL_CONNECTED &&
         static_cast<int32_t>(millis() - deadline) < 0) {
    delay(50);
  }
  const uint8_t* connectedBssid = WiFi.BSSID();
  const bool verified = WiFi.status() == WL_CONNECTED &&
      connectedBssid != nullptr &&
      memcmp(connectedBssid, selectedNetwork.bssidBytes, 6) == 0;

  // No guardar la PSK en NVS ni dejar la conexión o su configuración activa.
  WiFi.disconnect(false, true);
  delay(50);
  if (!verified) {
    sendError("No se verificó la conexión al BSSID seleccionado");
    return;
  }

  startMonitor();
  if (!monitorRunning) {
    sendError("Conexión verificada, pero no se pudo reiniciar el monitor");
    return;
  }
  memcpy(labVerifiedBssid, selectedNetwork.bssidBytes, 6);
  labVerifyDeadline = millis() + LAB_AUTH_WINDOW_MS;
  labNetworkVerified = true;
  Serial.printf("[LAB] acceso WPA verificado para %s; ventana 120 s\n",
                selectedNetwork.bssid.c_str());
  sendLabStatus();
}

void processSerialLabCommand() {
  static char line[80] = {};
  static size_t used = 0;
  static bool discarding = false;
  while (Serial.available() > 0) {
    const char c = static_cast<char>(Serial.read());
    if (c == '\r' || c == '\n') {
      if (!discarding && used > 0) {
        line[used] = '\0';
        if (strncmp(line, "LAB_VERIFY|", 11) == 0) {
          verifyLabPassword(line + 11);
        } else {
          Serial.println("[LAB] comando USB desconocido");
        }
      }
      wipeSecret(line, sizeof(line));
      used = 0;
      discarding = false;
      continue;
    }
    if (discarding) continue;
    if (used + 1 >= sizeof(line)) {
      wipeSecret(line, sizeof(line));
      used = 0;
      discarding = true;
      Serial.println("[LAB] comando USB demasiado largo");
      continue;
    }
    line[used++] = c;
  }
}

void armLabDeauth(const String& token) {
#if !defined(LAB_ENABLE_RAW_TX)
  sendError("LAB activo no está compilado");
  return;
#endif
  if (!labAuthorizationValid()) {
    sendError("Verifica primero el acceso al AP por USB (LAB_VERIFY|clave)");
    sendLabStatus();
    return;
  }
  if (!hasSelectedNetwork || !monitorRunning) {
    sendError("LAB requiere red seleccionada y monitor activo");
    return;
  }

  if (token != "LAB") {
    sendError("Armado LAB rechazado");
    return;
  }

  labDeauthArmed = true;
  labDeauthArmDeadline = millis() + LAB_ARM_WINDOW_MS;
  Serial.println("[LAB] deauth unicast armado por 30 s");
  sendLabStatus();
}

void disarmLabDeauth() {
  labDeauthArmed = false;
  labDeauthArmDeadline = 0;
  Serial.println("[LAB] desarmado");
  sendLabStatus();
}

void labDeauthUnicast(const String& macText) {
#if !defined(LAB_ENABLE_RAW_TX)
  sendError("LAB activo no está compilado");
  return;
#endif
  if (!labAuthorizationValid()) {
    sendError("Autorización WPA ausente o vencida");
    return;
  }
  if (!hasSelectedNetwork || !monitorRunning) {
    sendError("LAB requiere red seleccionada y monitor activo");
    return;
  }

  if (!labArmStillValid()) {
    sendError("LAB no esta armado o el armado expiro");
    sendLabStatus();
    return;
  }

  // Consumimos el armado aunque la transmisión falle: cada intento requiere
  // una confirmación nueva desde Android.
  clearLabAuthorization();

  const uint32_t now = millis();
  if (lastLabDeauthMs != 0 && (now - lastLabDeauthMs) < LAB_DEAUTH_COOLDOWN_MS) {
    sendError("LAB en cooldown; espera antes de otra prueba");
    sendLabStatus();
    return;
  }

  uint8_t client[6] = {};
  if (!parseMac(macText, client)) {
    sendError("MAC de cliente invalida");
    return;
  }

  // Nunca permitimos broadcast/multicast ni apuntar al propio AP.
  if (isBroadcastMac(client) || isMulticastMac(client) || sameMac(client, monitorBssid)) {
    sendError("LAB solo permite un cliente unicast valido");
    return;
  }

  ObservedClient observed;
  if (!observedClientSnapshot(client, observed)) {
    sendError("Cliente no observado en el BSSID seleccionado");
    return;
  }

  // Exige actividad reciente para reducir el riesgo de usar una MAC obsoleta.
  if ((now - observed.lastSeen) > 30000UL) {
    sendError("Cliente demasiado antiguo; espera a verlo activo y reintenta");
    return;
  }

  // IEEE 802.11 Deauthentication management frame, unicast únicamente.
  // Address1 = cliente, Address2 = AP seleccionado, Address3 = BSSID.
  uint8_t frame[26] = {
    0xC0, 0x00,             // Frame Control: Management / Deauthentication
    0x00, 0x00,             // Duration
    0,0,0,0,0,0,           // DA
    0,0,0,0,0,0,           // SA
    0,0,0,0,0,0,           // BSSID
    0x00, 0x00,             // Sequence Control (driver puede sustituirlo)
    0x03, 0x00              // Reason 3: station leaving ESS
  };

  memcpy(frame + 4,  client,       6);
  memcpy(frame + 10, monitorBssid, 6);
  memcpy(frame + 16, monitorBssid, 6);

  // El canal ya está fijado por startMonitor(). No hacemos channel hopping.
  // NOTA: el API oficial no lista DEAUTH entre los tipos soportados; por eso
  // tratamos un rechazo del driver como un resultado esperado/diagnosticable.
  const esp_err_t err = esp_wifi_80211_tx(WIFI_IF_STA, frame, sizeof(frame), true);
  if (err == ESP_OK) {
    // El cooldown representa una transmisión aceptada por el driver, no un
    // intento fallido. El armado ya fue consumido en ambos casos.
    lastLabDeauthMs = now;
  }

  String j;
  j.reserve(150);
  j += "{\"t\":\"lab_deauth\",\"m\":\"";
  j += macToString(client);
  j += "\",\"b\":\"";
  j += selectedNetwork.bssid;
  j += "\",\"ok\":";
  j += (err == ESP_OK) ? "1" : "0";
  j += ",\"err\":";
  j += String(static_cast<int>(err));
  j += ",\"err_hex\":\"0x";
  j += String(static_cast<uint32_t>(err), HEX);
  j += "\",\"err_name\":\"";
  j += esp_err_to_name(err);
  j += "\"";
  j += "}";
  notifyEvent(j);

  Serial.printf(
    "[LAB] deauth unicast -> %s | BSSID %s | err=%d (0x%X, %s)\n",
    macToString(client).c_str(),
    selectedNetwork.bssid.c_str(),
    static_cast<int>(err),
    static_cast<unsigned int>(err),
    esp_err_to_name(err)
  );

  if (err != ESP_OK) {
    sendError(String("Driver rechazo DEAUTH raw: ") + esp_err_to_name(err));
  }

  sendLabStatus();
}

// ============================================================
// BLE CALLBACKS
// ============================================================

class ServerCallbacks : public BLEServerCallbacks {
public:
  void onConnect(BLEServer* server) override {
    (void)server;
    bleConnected = true;
    bleAdvertisingRestartPending = false;
    ++bleConnectionCount;
    Serial.printf("[BLE] conectado (#%lu)\n", static_cast<unsigned long>(bleConnectionCount));
  }

  void onDisconnect(BLEServer* server) override {
    (void)server;
    // No tocamos el monitor Wi-Fi ni el PCAP. Solo se perdió el canal de
    // control. El advertising se reinicia desde loop() fuera del callback BLE.
    bleConnected = false;
    labRevokeOnBleDisconnect = true;
    bleDisconnectedAt = millis();
    bleAdvertisingRestartPending = true;
    Serial.println("[BLE] desconectado; monitor Wi-Fi continúa activo");
  }
};

class CommandCallbacks : public BLECharacteristicCallbacks {
public:
  void onWrite(BLECharacteristic* characteristic) override {
    // Arduino-ESP32 3.x: getValue() devuelve Arduino String.
    const String raw = characteristic->getValue();

    size_t n = raw.length();
    if (n > MAX_COMMAND_LEN) n = MAX_COMMAND_LEN;

    portENTER_CRITICAL(&commandMux);

    if (n > 0) memcpy(pendingCommand, raw.c_str(), n);
    pendingCommand[n] = '\0';
    commandPending = true;

    portEXIT_CRITICAL(&commandMux);
  }
};

// ============================================================
// COMANDOS BLE
// ============================================================

void sendHandshakeSnapshot();

void processPendingCommand() {
  if (!commandPending) return;

  char local[MAX_COMMAND_LEN + 1];

  portENTER_CRITICAL(&commandMux);
  memcpy(local, pendingCommand, sizeof(local));
  local[MAX_COMMAND_LEN] = '\0';
  commandPending = false;
  portEXIT_CRITICAL(&commandMux);

  String cmd(local);
  cmd.trim();

  if (cmd.length() == 0) return;

  Serial.print("[CMD] ");
  Serial.println(cmd);

  if (cmd == "SCAN") {
    scanWifi();
  }
  else if (cmd == "START") {
    startMonitor();
  }
  else if (cmd == "STOP") {
    stopMonitor();
  }
  else if (cmd == "GET_PCAP") {
    exportPcap();
  }
  else if (cmd == "STATUS") {
    // Resincronización completa después de cualquier reconexión BLE.
    // El monitor y el PCAP pueden haber seguido avanzando sin el teléfono.
    notifyEvent("{\"t\":\"hello\",\"v\":7}");
    if (hasSelectedNetwork) sendSelectedEvent();
    sendMonitorEvent(monitorRunning);
    sendHandshakeSnapshot();
    sendLabStatus();
  }
  else if (cmd == "LAB_STATUS") {
    sendLabStatus();
  }
  else if (cmd == "LAB_CLIENTS") {
    sendObservedLabClients();
  }
  else if (cmd == "LAB_DISARM") {
    disarmLabDeauth();
  }
  else if (cmd.startsWith("LAB_ARM|")) {
    String token = cmd.substring(8);
    token.trim();
    armLabDeauth(token);
  }
  else if (cmd.startsWith("LAB_DEAUTH|")) {
    String client = cmd.substring(11);
    client.trim();
    labDeauthUnicast(client);
  }
  else if (cmd.startsWith("SELECT|")) {
    String bssid = cmd.substring(7);
    bssid.trim();
    selectByBssid(bssid);
  }
  else {
    sendError("Comando desconocido");
  }
}

// ============================================================
// ESTADO PERIÓDICO DEL MONITOR
// ============================================================

void sendHandshakeSnapshot() {
  uint32_t totalSnapshot = 0;
  uint16_t pcapCountSnapshot = 0;
  uint16_t pcapEapolSnapshot = 0;
  bool beaconSnapshot = false;
  uint8_t contextSubtypeSnapshot = 0;
  uint8_t mgmtContextSnapshot = 0;
  bool pcapOverflowSnapshot = false;
  int clientIndexSnapshot = -1;
  ClientHandshake clientSnapshot;

  portENTER_CRITICAL(&monitorMux);
  totalSnapshot = totalEapol;
  pcapCountSnapshot = pcapPacketCount;
  pcapEapolSnapshot = pcapEapolCount;
  beaconSnapshot = beaconCaptured;
  contextSubtypeSnapshot = apContextSubtype;
  mgmtContextSnapshot = pcapMgmtContextCount;
  pcapOverflowSnapshot = pcapOverflow;
  clientIndexSnapshot = latestClient;

  if (clientIndexSnapshot >= 0 && clientIndexSnapshot < MAX_CLIENTS) {
    clientSnapshot = clients[clientIndexSnapshot];
  }
  portEXIT_CRITICAL(&monitorMux);

  String j;
  j.reserve(260);
  j += "{\"t\":\"hs\",\"m\":\"";

  if (clientSnapshot.used) j += macToString(clientSnapshot.mac);
  else j += "--";

  j += "\",\"mask\":";
  j += String(clientSnapshot.used ? clientSnapshot.seenMask : 0);
  j += ",\"e\":";
  j += String(totalSnapshot);
  j += ",\"pe\":";
  j += String(pcapEapolSnapshot);
  j += ",\"p\":";
  j += String(pcapCountSnapshot);
  j += ",\"bcn\":";
  j += beaconSnapshot ? "1" : "0";
  j += ",\"ctx\":";
  j += String(contextSubtypeSnapshot);
  j += ",\"mg\":";
  j += String(mgmtContextSnapshot);
  j += ",\"rtx\":";
  j += String(clientSnapshot.used ? clientSnapshot.retransmissions : 0);
  j += ",\"m1c\":";
  j += String(clientSnapshot.used ? clientSnapshot.messageCount[0] : 0);
  j += ",\"m2c\":";
  j += String(clientSnapshot.used ? clientSnapshot.messageCount[1] : 0);
  j += ",\"m3c\":";
  j += String(clientSnapshot.used ? clientSnapshot.messageCount[2] : 0);
  j += ",\"m4c\":";
  j += String(clientSnapshot.used ? clientSnapshot.messageCount[3] : 0);
  j += ",\"ov\":";
  j += pcapOverflowSnapshot ? "1" : "0";
  j += "}";

  notifyEvent(j);
}

// ============================================================
// SETUP / LOOP
// ============================================================

void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println();
  Serial.println("================================");
  Serial.println(" C3 BLE Analyzer v7 + robust BLE + LAB unicast");
  Serial.println("================================");

  // Inicializa radio Wi-Fi, pero sin conectarse a ninguna red.
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false, false);
  delay(50);

  BLEDevice::init(DEVICE_NAME);
  BLEDevice::setMTU(247);

  bleServer = BLEDevice::createServer();
  bleServer->setCallbacks(new ServerCallbacks());

  BLEService* service = bleServer->createService(SERVICE_UUID);

  BLECharacteristic* commandCharacteristic = service->createCharacteristic(
    COMMAND_UUID,
    BLECharacteristic::PROPERTY_WRITE |
    BLECharacteristic::PROPERTY_WRITE_NR
  );
  commandCharacteristic->setCallbacks(new CommandCallbacks());

  eventCharacteristic = service->createCharacteristic(
    EVENT_UUID,
    BLECharacteristic::PROPERTY_READ |
    BLECharacteristic::PROPERTY_NOTIFY
  );

  eventCharacteristic->addDescriptor(new BLE2902());
  eventCharacteristic->setValue("{\"t\":\"hello\",\"v\":7}");

  service->start();

  BLEAdvertising* advertising = bleServer->getAdvertising();
  advertising->addServiceUUID(SERVICE_UUID);
  advertising->setScanResponse(true);
  advertising->start();

  Serial.print("[BLE] advertising como: ");
  Serial.println(DEVICE_NAME);
  Serial.println("[READY] esperando Android...");
}

void loop() {
  // Reinicia advertising fuera del callback del stack BLE. Esto evita hacer
  // trabajo adicional dentro de onDisconnect() y hace la recuperación más
  // consistente en Arduino-ESP32 3.x.
  if (
    bleAdvertisingRestartPending &&
    !bleConnected &&
    (millis() - bleDisconnectedAt >= 250)
  ) {
    BLEAdvertising* advertising = bleServer != nullptr ? bleServer->getAdvertising() : nullptr;
    if (advertising != nullptr) {
      advertising->start();
      Serial.println("[BLE] advertising reanudado");
    }
    bleAdvertisingRestartPending = false;
  }

  if (labRevokeOnBleDisconnect) {
    labRevokeOnBleDisconnect = false;
    clearLabAuthorization();
  }

  processPendingCommand();
  processSerialLabCommand();

  if (monitorRunning && bleConnected && !pcapExportInProgress) {
    static uint32_t lastStatusMs = 0;

    if (millis() - lastStatusMs >= 400) {
      lastStatusMs = millis();
      sendHandshakeSnapshot();
    }
  }

  delay(5);
}