# C3 BLE Analyzer

Android companion app + ESP32-C3 firmware for **authorized Wi-Fi laboratory analysis** over BLE.

The project uses Bluetooth Low Energy as the control/data link between an Android phone and an ESP32-C3. The C3 can scan nearby 2.4 GHz networks, monitor a manually selected BSSID, identify WPA/WPA2 EAPOL handshake messages and keep a small in-memory PCAP that can be exported back to Android.

> [!IMPORTANT]
> This repository is intended for your own equipment, test networks and environments where you have explicit authorization. It is designed around a manually selected network and includes deliberate limits on the experimental LAB functionality. It does not implement password cracking, broadcast flooding, automatic deauthentication loops or autonomous target selection.

## Project status

| Component | Current state |
| --- | --- |
| Android app | `0.5.0` / versionCode 5 |
| Android min SDK | 26 |
| Android target / compile SDK | 35 |
| Android language | Kotlin |
| Firmware | C3 BLE Analyzer v7 |
| MCU | ESP32-C3 |
| Framework | Arduino-ESP32 3.x |
| Control link | BLE GATT |
| Wi-Fi analysis | 2.4 GHz, selected BSSID/channel |
| EAPOL viewer | M1 / M2 / M3 / M4 + counters |
| Capture export | PCAP over BLE |
| LAB reconnection test | Experimental, guarded, unicast-only |

## Main features

### Android companion app

- Discovers `C3-BLE-Analyzer` by BLE service UUID or advertised device name.
- Handles Android Bluetooth permissions for both pre-Android 12 and Android 12+.
- Uses a dedicated BLE connection manager instead of placing all GATT logic in the activity.
- Requests a larger BLE MTU when possible.
- Re-discovers services and re-subscribes to notifications after a reconnect.
- Uses a write queue so commands are serialized instead of being fired concurrently.
- Automatically retries lost BLE connections with increasing backoff.
- Sends `STATUS` after reconnecting so the UI can rebuild its state without repeating previous user actions.
- Keeps the phone's normal Internet connection independent from the C3 Wi-Fi monitor.

### Wi-Fi discovery and monitor

- Scans nearby 2.4 GHz Wi-Fi networks from the ESP32-C3.
- Reports SSID, BSSID, channel, RSSI and security type to Android.
- Requires the user to select a BSSID before monitoring.
- Locks monitoring to the selected channel rather than continuously hopping channels.
- Uses promiscuous reception for passive observation of the selected network.

### WPA/WPA2 handshake viewer

The firmware identifies EAPOL traffic associated with the selected AP and tracks handshake state per observed client.

The Android UI shows:

- M1, M2, M3 and M4 presence.
- EAPOL frame count.
- Per-message counters.
- Retransmission count.
- Client MAC currently associated with the latest handshake state.
- Selected AP/BSSID, channel and security mode.
- Whether useful AP context was included in the capture.

A complete four-message sequence is shown as a completed handshake, while partial observations remain visible rather than being discarded.

### PCAP capture and export

The ESP32-C3 keeps a deliberately small capture buffer in RAM.

Current firmware limits:

- Up to **32 captured frames**.
- Up to **768 bytes per captured frame**.
- Roughly 24 KiB reserved for captured frame payloads.
- AP context can come from a Beacon or Probe Response.
- Authentication/association context and EAPOL frames can be retained.
- Buffer overflow is reported to Android instead of silently pretending the capture is complete.

PCAP data is transferred through BLE in Base64 chunks and reconstructed by the Android app. On Android 10+ the file is saved under:

```text
Downloads/C3BleAnalyzer/
```

with a timestamped `.pcap` filename derived from the selected SSID when possible.

### Robust BLE recovery

BLE state is modeled explicitly:

```text
DISCONNECTED
   ↓
SCANNING
   ↓
CONNECTING
   ↓
DISCOVERING
   ↓
SUBSCRIBING
   ↓
READY
   ↓
RECOVERING ──────┐
   ↑             │
   └─────────────┘
```

The reconnection logic intentionally does **not** queue arbitrary user actions while BLE is down. This prevents commands such as monitor start/stop or test actions from unexpectedly running after a connection is restored.

The C3 keeps its selected network, monitor state and capture independently from a temporary BLE interruption. Once Android reconnects, `STATUS` is used to synchronize the UI again.

## Experimental LAB reconnection mode

The firmware contains an experimental, deliberately constrained LAB-only reconnection test intended for controlled testing with the user's own AP and client device.

Safeguards currently implemented include:

- A network must already be manually selected.
- The passive monitor must already be running.
- Only a client recently observed communicating with the selected BSSID is eligible.
- Broadcast and multicast destinations are rejected.
- The action requires explicit temporary arming.
- One action consumes the current arm state.
- A cooldown is enforced after a successful driver submission.
- Changing network or monitor state disarms the LAB mode.
- There is no automatic repetition, target sweep or broadcast mode.

This feature is also hardware/driver dependent. `esp_wifi_80211_tx()` does not officially document deauthentication frames as a supported management-frame type on every Arduino-ESP32 / ESP-IDF combination. Some builds therefore return `ESP_ERR_INVALID_ARG` and the firmware reports that result without retrying automatically.

Protected Management Frames (802.11w / PMF) can independently cause a client to ignore an unprotected management frame even if the driver accepts transmission.

More detailed historical verification notes are in:

```text
firmware/LAB_SAFE_CHANGES_README.md
```

## Architecture

```text
┌─────────────────────────────┐
│         Android app         │
│                             │
│  MainActivity               │
│   ├─ Network list           │
│   ├─ Handshake viewer       │
│   ├─ PCAP receiver/export   │
│   └─ LAB status UI          │
│                             │
│  BleConnectionManager       │
│   ├─ Scan / connect         │
│   ├─ GATT discovery         │
│   ├─ Notifications          │
│   ├─ Command queue          │
│   └─ Reconnection/backoff   │
└──────────────┬──────────────┘
               │ BLE GATT
               │ commands / JSON events
┌──────────────▼──────────────┐
│          ESP32-C3           │
│                             │
│  BLE service                │
│   ├─ command characteristic │
│   └─ event characteristic   │
│                             │
│  Wi-Fi                      │
│   ├─ scan                   │
│   ├─ selected BSSID/channel │
│   ├─ promiscuous monitor    │
│   ├─ EAPOL classifier       │
│   └─ PCAP RAM buffer        │
└─────────────────────────────┘
```

## BLE service

The firmware advertises as:

```text
C3-BLE-Analyzer
```

GATT UUIDs:

| Purpose | UUID |
| --- | --- |
| Service | `7f000001-5a23-4b8f-9c40-1f4b3f80a001` |
| Commands | `7f000002-5a23-4b8f-9c40-1f4b3f80a001` |
| Events | `7f000003-5a23-4b8f-9c40-1f4b3f80a001` |

The command channel is text based. The normal application flow uses commands such as network scan, BSSID selection, monitor start/stop, status synchronization and PCAP retrieval. Events returned by the firmware are compact JSON objects such as network records, monitor state, handshake state and PCAP transfer chunks.

## Repository structure

```text
C3BleAnalyzer/
├─ app/
│  └─ src/main/
│     ├─ AndroidManifest.xml
│     └─ java/com/nikog4/c3bleanalyzer/
│        ├─ MainActivity.kt
│        └─ BleConnectionManager.kt
│
├─ firmware/
│  ├─ C3BleAnalyzer.ino
│  ├─ C3BleAnalyzer_v7_lab_unicast_deauth.ino
│  ├─ C3BleAnalyzer_v7_LAB_README.txt
│  └─ LAB_SAFE_CHANGES_README.md
│
├─ research/
│  └─ linker-lab/
│     ├─ original/
│     ├─ patched/
│     ├─ tools/
│     └─ README.md
│
├─ build-apk.cmd
├─ build.gradle.kts
├─ gradle.properties
└─ README.md
```

`C3BleAnalyzer.ino` is the canonical firmware file. The versioned LAB sketch is retained for traceability of the v7 work.

## Requirements

### Android

- Windows, Linux or macOS capable of running the Android build toolchain.
- JDK 17.
- Android SDK with API 35 installed.
- Gradle available on `PATH` for the current repository state.
- Android device with BLE support.
- Android 8.0 / API 26 or newer.

### ESP32-C3

- ESP32-C3 development board.
- Arduino IDE or Arduino CLI.
- Arduino-ESP32 3.x.
- USB data cable and the appropriate serial driver for the board.

The firmware notes include a build that was verified with Arduino-ESP32 3.3.0 / ESP-IDF 5.5 and FQBN:

```text
esp32:esp32:esp32c3
```

That is a documented verification environment, not a claim that every Arduino-ESP32 version behaves identically.

## Build the Android app

### Windows helper script

The repository includes:

```cmd
build-apk.cmd
```

The script derives the default Android SDK location from `%LOCALAPPDATA%` and generates a local `local.properties` file. No personal Windows username or absolute profile path needs to be committed to the repository.

Run from the repository root:

```cmd
build-apk.cmd
```

The expected debug APK is:

```text
app\build\outputs\apk\debug\app-debug.apk
```

### Manual SDK configuration

If your Android SDK is installed somewhere else, create an untracked `local.properties` in the repository root:

```properties
sdk.dir=C:/path/to/Android/Sdk
```

`local.properties` is already ignored by Git.

Then run:

```cmd
gradle :app:assembleDebug
```

## Build and flash the ESP32-C3 firmware

### Arduino IDE

Open:

```text
firmware/C3BleAnalyzer.ino
```

Select an ESP32-C3 board profile matching your hardware, choose the serial port and compile/upload normally.

### Arduino CLI

A generic compile command is:

```powershell
arduino-cli compile --fqbn esp32:esp32:esp32c3 firmware/C3BleAnalyzer
```

Depending on Arduino CLI's sketch-folder naming requirements, you may prefer Arduino IDE or place the sketch temporarily in a folder named `C3BleAnalyzer` for command-line verification. The repository's LAB verification notes describe that approach without relying on user-specific paths.

## Basic passive workflow

1. Flash the firmware and start the Android application.
2. Connect to the advertised `C3-BLE-Analyzer` device.
3. Ask the C3 to scan nearby networks.
4. Select the BSSID belonging to your authorized test network.
5. Start the monitor.
6. Generate normal traffic with a client on that network.
7. Observe EAPOL M1-M4 state and capture counters in Android.
8. Export the PCAP when relevant EAPOL data has been captured.

The project does not recover Wi-Fi passwords. Exported captures can be inspected with standard packet-analysis tools for legitimate troubleshooting, protocol study and lab validation.

## Capture behavior and limitations

A few limitations are intentional or inherent to the hardware:

- ESP32-C3 is a 2.4 GHz device; this project does not monitor 5 GHz or 6 GHz networks.
- Monitoring is bound to the selected channel.
- A small fixed RAM buffer means a busy environment can overflow the PCAP buffer.
- A four-way handshake is only visible when the relevant EAPOL exchange actually occurs while the C3 is listening on the correct channel.
- Missing M1/M2/M3/M4 messages can be caused by timing, RF conditions, channel mismatch, capture limits or the client's connection behavior.
- BLE and Wi-Fi share resources on the ESP32-C3, so the project favors a compact event protocol and bounded capture sizes.
- LAB transmission support varies across Arduino-ESP32 / ESP-IDF driver versions.
- PMF can prevent the experimental reconnection mechanism from affecting a client.

## Linker research lab

`research/linker-lab/` is a separate reproducible experiment for understanding static archives and symbol resolution on the ESP32-C3 toolchain.

It demonstrates, using a synthetic local library:

- normal static archive linking;
- weak vs. strong symbols;
- GNU linker `--wrap` behavior;
- extracting and replacing one object inside a local `.a` archive;
- inspection through `ar`, `nm`, `readelf`, `objdump` and linker maps.

It deliberately does **not** modify the globally installed Arduino-ESP32 / ESP-IDF libraries. Generated outputs remain under `research/linker-lab/build/` and are ignored by Git.

See:

```text
research/linker-lab/README.md
```

## Privacy and repository hygiene

The repository should not contain machine-specific development information.

Already ignored:

- `local.properties`
- `.gradle/`
- `.idea/`
- `.vscode/`
- Android build outputs
- APK/AAB files
- firmware `.elf`, `.map` and `.bin` outputs
- linker-lab generated builds

Documentation and scripts should prefer environment variables such as `%LOCALAPPDATA%`, `%TEMP%` or relative repository paths instead of absolute paths containing a developer's username.

Do not commit:

- personal Windows/Linux/macOS home paths;
- real Wi-Fi credentials;
- private SSIDs/BSSIDs captured from production environments;
- exported PCAPs from networks you do not intend to publish;
- serial logs containing passwords or other private test data.

## Troubleshooting

### Android cannot find the C3

- Confirm Bluetooth is enabled.
- Grant the requested Bluetooth permissions.
- Confirm the firmware is running and advertising as `C3-BLE-Analyzer`.
- Reopen the app or disconnect/reconnect the board if the BLE stack is in an unusual state.

### Gradle cannot find the Android SDK

Run `build-apk.cmd` on a standard Windows Android SDK installation, or create your own untracked `local.properties` with the correct `sdk.dir`.

### No EAPOL messages appear

- Confirm the selected BSSID and channel are correct.
- Confirm the monitor is running.
- Generate a legitimate reconnect/authentication event with your own client and AP.
- Remember that ordinary encrypted data traffic does not imply a new four-way handshake will occur.

### PCAP export is disabled

The Android app enables PCAP export after the firmware reports captured EAPOL data. If a BLE transfer is interrupted, reconnect and request the export again.

### LAB reports `ESP_ERR_INVALID_ARG`

This can be a driver limitation rather than a BLE failure. The current firmware reports the error and does not automatically retry the action.

## License

No explicit open-source license is currently included in the repository. Until a license is added, normal copyright rules apply to the source code.

## Disclaimer

Use this project only on systems and networks you own or are explicitly authorized to test. Wireless regulations and organizational policies vary by jurisdiction and environment; the operator is responsible for complying with them.
