package com.nikog4.c3bleanalyzer

import android.Manifest
import android.app.Activity
import android.app.AlertDialog
import android.bluetooth.*
import android.bluetooth.le.*
import android.content.ContentValues
import android.content.pm.PackageManager
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.os.Build
import android.os.Bundle
import android.os.Environment
import android.os.Handler
import android.os.Looper
import android.provider.MediaStore
import android.util.Base64
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.widget.*
import org.json.JSONObject
import java.io.ByteArrayOutputStream
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.UUID

class MainActivity : Activity() {

    companion object {
        private val SERVICE_UUID = UUID.fromString("7f000001-5a23-4b8f-9c40-1f4b3f80a001")
        private val COMMAND_UUID = UUID.fromString("7f000002-5a23-4b8f-9c40-1f4b3f80a001")
        private val EVENT_UUID = UUID.fromString("7f000003-5a23-4b8f-9c40-1f4b3f80a001")
        private const val DEVICE_NAME = "C3-BLE-Analyzer"
        private const val REQ_PERMS = 100
    }

    data class Network(
        val ssid: String,
        val bssid: String,
        val channel: Int,
        val rssi: Int,
        val security: String
    )

    data class LabClient(
        val mac: String,
        val ageMs: Long
    )

    private val handler = Handler(Looper.getMainLooper())

    private lateinit var root: LinearLayout
    private lateinit var deviceStatus: TextView
    private lateinit var connectButton: Button
    private lateinit var scanWifiButton: Button
    private lateinit var selectedPanel: LinearLayout
    private lateinit var selectedTitle: TextView
    private lateinit var selectedDetails: TextView
    private lateinit var monitorPanel: LinearLayout
    private lateinit var monitorStatus: TextView
    private lateinit var monitorChannel: TextView
    private lateinit var monitorEapol: TextView
    private lateinit var monitorClient: TextView
    private lateinit var m1: TextView
    private lateinit var m2: TextView
    private lateinit var m3: TextView
    private lateinit var m4: TextView
    private lateinit var handshakeSummary: TextView
    private lateinit var handshakeDetails: TextView
    private lateinit var pcapStatus: TextView
    private lateinit var exportPcapButton: Button
    private lateinit var networksContainer: LinearLayout
    private lateinit var progress: ProgressBar
    private lateinit var labPanel: LinearLayout
    private lateinit var labStatus: TextView
    private lateinit var labRefreshButton: Button
    private lateinit var labClientsContainer: LinearLayout

    private lateinit var ble: BleConnectionManager
    private var connected = false
    private var monitorIsRunning = false
    private var selected: Network? = null
    private val networks = linkedMapOf<String, Network>()
    private val labClients = linkedMapOf<String, LabClient>()
    private var labArmed = false
    private var labTtlMs = 0L
    private var labCooldownMs = 0L
    private var labClientsLoading = false
    private var labActionInProgress = false
    private var pendingLabDeauthMac: String? = null
    private var lastLabResult = ""

    private var pcapPacketCount = 0
    private var pcapEapolCount = 0
    private var pcapExpectedSize = 0
    private var pcapExpectedPackets = 0
    private var pcapExpectedEapol = 0
    private var pcapExpectedBeacon = false
    private var pcapExporting = false
    private var pcapOverflow = false
    private var beaconCaptured = false
    private var lastHandshakeMask = 0
    private var lastEapolCount = 0
    private var lastClient = "--"
    private var lastRetransmissions = 0
    private var lastMessageCounts = intArrayOf(0, 0, 0, 0)
    private var pcapBytes = ByteArrayOutputStream()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.statusBarColor = Color.rgb(11, 16, 32)
        window.navigationBarColor = Color.rgb(11, 16, 32)
        buildUi()
        setupBleManager()
        requestNeededPermissions()
    }

    override fun onDestroy() {
        if (::ble.isInitialized) ble.shutdown()
        super.onDestroy()
    }

    private fun requestNeededPermissions() {
        val perms = mutableListOf<String>()
        if (Build.VERSION.SDK_INT >= 31) {
            if (checkSelfPermission(Manifest.permission.BLUETOOTH_SCAN) != PackageManager.PERMISSION_GRANTED)
                perms += Manifest.permission.BLUETOOTH_SCAN
            if (checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) != PackageManager.PERMISSION_GRANTED)
                perms += Manifest.permission.BLUETOOTH_CONNECT
        } else {
            if (checkSelfPermission(Manifest.permission.ACCESS_FINE_LOCATION) != PackageManager.PERMISSION_GRANTED)
                perms += Manifest.permission.ACCESS_FINE_LOCATION
        }
        if (perms.isNotEmpty()) requestPermissions(perms.toTypedArray(), REQ_PERMS)
    }

    private fun buildUi() {
        val scroll = ScrollView(this).apply { setBackgroundColor(Color.rgb(11, 16, 32)) }
        root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(18), dp(24), dp(18), dp(48))
        }
        scroll.addView(root, ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT))

        root.addView(text("C3 BLE Analyzer", 28, true, Color.WHITE))
        root.addView(text("Control BLE robusto · monitor Wi‑Fi pasivo · PCAP · LAB unicast", 14, false, Color.rgb(151, 163, 184)).apply {
            setPadding(0, dp(4), 0, dp(18))
        })

        val connection = panel()
        deviceStatus = text("● Desconectado", 17, true, Color.rgb(230, 120, 120))
        connection.addView(deviceStatus)
        connection.addView(text("El teléfono mantiene Internet mientras el C3 dedica Wi‑Fi al canal seleccionado.", 13, false, Color.rgb(155, 166, 186)).apply {
            setPadding(0, dp(8), 0, 0)
        })
        connectButton = button("Buscar y conectar C3") { toggleConnection() }
        connection.addView(connectButton)
        root.addView(connection)

        scanWifiButton = button("Escanear redes Wi‑Fi") { sendCommand("SCAN") }.apply { isEnabled = false }
        root.addView(scanWifiButton)

        progress = ProgressBar(this).apply { visibility = View.GONE }
        root.addView(progress, LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, dp(42)).apply {
            gravity = Gravity.CENTER_HORIZONTAL
        })

        selectedPanel = panel().apply { visibility = View.GONE }
        selectedPanel.addView(label("RED SELECCIONADA"))
        selectedTitle = text("", 22, true, Color.WHITE)
        selectedPanel.addView(selectedTitle)
        selectedDetails = text("", 14, false, Color.rgb(183, 194, 212)).apply { typeface = Typeface.MONOSPACE }
        selectedPanel.addView(selectedDetails)
        selectedPanel.addView(button("Iniciar monitor") {
            resetPcapUi()
            sendCommand("START")
        })
        root.addView(selectedPanel)

        monitorPanel = panel().apply { visibility = View.GONE }
        monitorStatus = text("● Detenido", 18, true, Color.rgb(165, 176, 195))
        monitorPanel.addView(monitorStatus)

        val row = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            weightSum = 2f
            setPadding(0, dp(14), 0, 0)
        }
        monitorChannel = metric("Canal", "-")
        monitorEapol = metric("EAPOL", "0")
        row.addView(monitorChannel, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        row.addView(monitorEapol, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        monitorPanel.addView(row)

        monitorClient = text("Cliente: --", 15, true, Color.rgb(225, 231, 242)).apply {
            setPadding(0, dp(16), 0, dp(12))
            typeface = Typeface.MONOSPACE
        }
        monitorPanel.addView(monitorClient)

        monitorPanel.addView(label("WPA/WPA2 4-WAY HANDSHAKE"))

        val hs = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            weightSum = 4f
            setPadding(0, dp(10), 0, 0)
        }
        m1 = handshakeBox("M1")
        m2 = handshakeBox("M2")
        m3 = handshakeBox("M3")
        m4 = handshakeBox("M4")
        listOf(m1, m2, m3, m4).forEach {
            hs.addView(it, LinearLayout.LayoutParams(0, dp(58), 1f).apply {
                setMargins(dp(3), 0, dp(3), 0)
            })
        }
        monitorPanel.addView(hs)

        handshakeSummary = text("Esperando handshake", 18, true, Color.rgb(238, 191, 100)).apply {
            setPadding(0, dp(16), 0, dp(7))
        }
        monitorPanel.addView(handshakeSummary)

        handshakeDetails = text("Sin datos todavía.", 13, false, Color.rgb(183, 194, 212)).apply {
            typeface = Typeface.MONOSPACE
            setPadding(0, 0, 0, dp(4))
        }
        monitorPanel.addView(handshakeDetails)

        pcapStatus = text("PCAP: esperando Beacon + EAPOL", 14, false, Color.rgb(165, 176, 195)).apply {
            setPadding(0, dp(16), 0, 0)
        }
        monitorPanel.addView(pcapStatus)

        exportPcapButton = button("Exportar PCAP") { requestPcapExport() }.apply { isEnabled = false }
        monitorPanel.addView(exportPcapButton)

        labPanel = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            visibility = View.GONE
            setPadding(dp(14), dp(14), dp(14), dp(14))
            background = rounded(Color.rgb(16, 24, 39), Color.rgb(70, 85, 115), 16f)
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT
            ).apply { topMargin = dp(16) }
        }
        labPanel.addView(label("PRUEBA DE RECONEXIÓN — LAB"))
        labStatus = text("LAB inactivo. Iniciá el monitor para detectar clientes propios.", 13, false, Color.rgb(183, 194, 212)).apply {
            setPadding(0, dp(8), 0, 0)
        }
        labPanel.addView(labStatus)
        labRefreshButton = button("Actualizar clientes LAB") { requestLabClients() }.apply { isEnabled = false }
        labPanel.addView(labRefreshButton)
        labClientsContainer = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        labPanel.addView(labClientsContainer)
        monitorPanel.addView(labPanel)

        monitorPanel.addView(button("Detener", danger = true) { sendCommand("STOP") })
        root.addView(monitorPanel)

        root.addView(label("REDES CERCANAS").apply { setPadding(0, dp(22), 0, dp(9)) })
        networksContainer = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        root.addView(networksContainer)

        setContentView(scroll)
    }

    private fun setupBleManager() {
        ble = BleConnectionManager(
            activity = this,
            deviceName = DEVICE_NAME,
            serviceUuid = SERVICE_UUID,
            commandUuid = COMMAND_UUID,
            eventUuid = EVENT_UUID,
            listener = object : BleConnectionManager.Listener {
                override fun onStateChanged(state: BleConnectionManager.State, detail: String) {
                    when (state) {
                        BleConnectionManager.State.DISCONNECTED -> {
                            connected = false
                            deviceStatus.text = "● Desconectado"
                            deviceStatus.setTextColor(Color.rgb(230, 120, 120))
                            connectButton.isEnabled = true
                            connectButton.text = "Buscar y conectar C3"
                            scanWifiButton.isEnabled = false
                            exportPcapButton.isEnabled = false
                        }

                        BleConnectionManager.State.SCANNING -> {
                            connected = false
                            deviceStatus.text = "● Buscando $DEVICE_NAME"
                            deviceStatus.setTextColor(Color.rgb(238, 191, 100))
                            connectButton.isEnabled = true
                            connectButton.text = "Cancelar"
                            scanWifiButton.isEnabled = false
                        }

                        BleConnectionManager.State.CONNECTING -> {
                            connected = false
                            deviceStatus.text = "● Conectando…"
                            deviceStatus.setTextColor(Color.rgb(238, 191, 100))
                            connectButton.isEnabled = true
                            connectButton.text = "Cancelar"
                            scanWifiButton.isEnabled = false
                        }

                        BleConnectionManager.State.DISCOVERING -> {
                            connected = false
                            deviceStatus.text = "● Preparando servicios BLE…"
                            deviceStatus.setTextColor(Color.rgb(238, 191, 100))
                            connectButton.text = "Cancelar"
                            scanWifiButton.isEnabled = false
                        }

                        BleConnectionManager.State.SUBSCRIBING -> {
                            connected = false
                            deviceStatus.text = "● Activando notificaciones…"
                            deviceStatus.setTextColor(Color.rgb(238, 191, 100))
                            connectButton.text = "Cancelar"
                            scanWifiButton.isEnabled = false
                        }

                        BleConnectionManager.State.READY -> {
                            connected = true
                            deviceStatus.text = "● Conectado a $DEVICE_NAME"
                            deviceStatus.setTextColor(Color.rgb(76, 224, 148))
                            connectButton.isEnabled = true
                            connectButton.text = "Desconectar"
                            scanWifiButton.isEnabled = true
                            exportPcapButton.isEnabled = pcapEapolCount > 0 && !pcapExporting
                        }

                        BleConnectionManager.State.RECOVERING -> {
                            connected = false
                            deviceStatus.text = if (detail.isBlank()) {
                                "● Reconectando…"
                            } else {
                                "● Reconectando · $detail"
                            }
                            deviceStatus.setTextColor(Color.rgb(238, 191, 100))
                            connectButton.isEnabled = true
                            connectButton.text = "Cancelar reconexión"
                            scanWifiButton.isEnabled = false
                            exportPcapButton.isEnabled = false
                        }
                    }
                    updateLabUi()
                }

                override fun onReady() {
                    // El C3 conserva monitor, selección y PCAP aunque BLE se corte.
                    // STATUS reconstruye la pantalla al volver a conectar.
                    sendCommand("STATUS")
                }

                override fun onEvent(raw: String) {
                    handleEvent(raw)
                }

                override fun onTransferInterrupted() {
                    pendingLabDeauthMac = null
                    labActionInProgress = false
                    if (pcapExporting) {
                        pcapExporting = false
                        pcapStatus.text = "Transferencia PCAP interrumpida · reconectando BLE…"
                    }
                    updateLabUi()
                }

                override fun onError(message: String) {
                    toast(message)
                }
            }
        )
    }

    private fun toggleConnection() {
        if (!::ble.isInitialized) return
        if (ble.isActive) {
            ble.disconnect()
        } else {
            ble.connect()
        }
    }

    private fun handleEvent(raw: String) {
        try {
            val j = JSONObject(raw)

            runOnUiThread {
                when (j.optString("t")) {
                    "hello" -> Unit

                    "scan_start" -> {
                        networks.clear()
                        networksContainer.removeAllViews()
                        progress.visibility = View.VISIBLE
                    }

                    "net" -> {
                        val n = Network(
                            j.optString("s", "[Hidden]"),
                            j.optString("b"),
                            j.optInt("c"),
                            j.optInt("r"),
                            j.optString("a", "?")
                        )
                        networks[n.bssid] = n
                        renderNetworks()
                    }

                    "scan_end" -> progress.visibility = View.GONE

                    "selected" -> {
                        selected = Network(
                            j.optString("s"),
                            j.optString("b"),
                            j.optInt("c"),
                            j.optInt("r"),
                            j.optString("a")
                        )
                        resetPcapUi()
                        resetLabUi()
                        showSelected()
                    }

                    "monitor" -> {
                        val running = j.optInt("run") == 1
                        monitorIsRunning = running
                        monitorPanel.visibility = View.VISIBLE
                        monitorStatus.text = if (running) "● Escuchando" else "● Detenido"
                        monitorStatus.setTextColor(
                            if (running) Color.rgb(76, 224, 148)
                            else Color.rgb(165, 176, 195)
                        )
                        monitorChannel.text = "CANAL\n${j.optInt("c", 0)}"
                        labPanel.visibility = if (running) View.VISIBLE else View.GONE
                        if (running && connected) {
                            sendCommand("LAB_STATUS")
                            sendCommand("LAB_CLIENTS")
                        } else {
                            labClients.clear()
                        }
                        updateLabUi()
                    }

                    "hs" -> {
                        monitorPanel.visibility = View.VISIBLE
                        lastHandshakeMask = j.optInt("mask")
                        lastEapolCount = j.optInt("e", 0)
                        pcapEapolCount = j.optInt("pe", lastEapolCount)
                        pcapPacketCount = j.optInt("p", pcapEapolCount)
                        beaconCaptured = j.optInt("bcn", 0) == 1
                        lastRetransmissions = j.optInt("rtx", 0)
                        lastMessageCounts = intArrayOf(
                            j.optInt("m1c", 0),
                            j.optInt("m2c", 0),
                            j.optInt("m3c", 0),
                            j.optInt("m4c", 0)
                        )
                        lastClient = j.optString("m", "--")
                        pcapOverflow = j.optInt("ov", 0) == 1

                        monitorEapol.text = "EAPOL\n$lastEapolCount"
                        monitorClient.text = "Cliente: $lastClient"

                        setHs(m1, "M1", lastHandshakeMask and 1 != 0)
                        setHs(m2, "M2", lastHandshakeMask and 2 != 0)
                        setHs(m3, "M3", lastHandshakeMask and 4 != 0)
                        setHs(m4, "M4", lastHandshakeMask and 8 != 0)

                        updateHandshakeViewer()

                        val beaconText = if (beaconCaptured) "Beacon ✓" else "Beacon …"
                        val suffix = if (pcapOverflow) " · buffer lleno" else ""
                        pcapStatus.text = "PCAP: $beaconText · $pcapEapolCount EAPOL · $pcapPacketCount frame(s)$suffix"
                        exportPcapButton.isEnabled = connected && pcapEapolCount > 0 && !pcapExporting
                    }

                    "lab" -> {
                        labArmed = j.optInt("armed", 0) == 1
                        labTtlMs = j.optLong("ttl_ms", 0L)
                        labCooldownMs = j.optLong("cooldown_ms", 0L)
                        updateLabUi()

                        val mac = pendingLabDeauthMac
                        if (mac != null && labActionInProgress && labArmed && labCooldownMs <= 0L) {
                            pendingLabDeauthMac = null
                            lastLabResult = "LAB armado. Enviando unicast a $mac…"
                            updateLabUi()
                            sendCommand("LAB_DEAUTH|$mac")
                        }

                        if (labCooldownMs > 0L) {
                            handler.postDelayed({
                                if (connected && monitorIsRunning) sendCommand("LAB_STATUS")
                            }, minOf(labCooldownMs + 250L, 10_500L))
                        }
                    }

                    "lab_clients_begin" -> {
                        labClients.clear()
                        labClientsLoading = true
                        updateLabUi()
                    }

                    "lab_client" -> {
                        val mac = j.optString("m")
                        if (mac.isNotBlank()) {
                            labClients[mac] = LabClient(mac, j.optLong("age_ms", 0L))
                        }
                    }

                    "lab_clients_end" -> {
                        labClientsLoading = false
                        updateLabUi()
                    }

                    "lab_deauth" -> {
                        val mac = j.optString("m", "--")
                        val ok = j.optInt("ok", 0) == 1
                        val err = j.optInt("err", 0)
                        labActionInProgress = false
                        pendingLabDeauthMac = null
                        labArmed = false
                        lastLabResult = if (ok) {
                            "Unicast LAB enviado a $mac. Observando reconexión y EAPOL…"
                        } else {
                            "El driver rechazó LAB_DEAUTH para $mac. err=$err"
                        }
                        toast(if (ok) "LAB enviado; esperando reconexión" else "LAB rechazado por driver: $err")
                        updateLabUi()
                        handler.postDelayed({
                            if (connected && monitorIsRunning) {
                                sendCommand("LAB_STATUS")
                                sendCommand("LAB_CLIENTS")
                            }
                        }, 800L)
                    }

                    "pcap_begin" -> {
                        pcapExporting = true
                        pcapExpectedSize = j.optInt("size", 0)
                        pcapExpectedPackets = j.optInt("packets", 0)
                        pcapExpectedEapol = j.optInt("eapol", 0)
                        pcapExpectedBeacon = j.optInt("beacon", 0) == 1
                        pcapOverflow = j.optInt("overflow", 0) == 1
                        pcapBytes = ByteArrayOutputStream(maxOf(1024, pcapExpectedSize))
                        exportPcapButton.isEnabled = false
                        pcapStatus.text = "Recibiendo PCAP · 0 / $pcapExpectedSize bytes"
                    }

                    "pcap_chunk" -> {
                        val encoded = j.optString("d")
                        val chunk = Base64.decode(encoded, Base64.DEFAULT)
                        pcapBytes.write(chunk)
                        pcapStatus.text = "Recibiendo PCAP · ${pcapBytes.size()} / $pcapExpectedSize bytes"
                    }

                    "pcap_end" -> {
                        val actualSize = pcapBytes.size()
                        pcapExporting = false

                        if (actualSize != pcapExpectedSize) {
                            pcapStatus.text = "PCAP incompleto: $actualSize / $pcapExpectedSize bytes"
                            exportPcapButton.isEnabled = pcapEapolCount > 0
                            toast("Transferencia PCAP incompleta; vuelve a exportar")
                        } else {
                            val bytes = pcapBytes.toByteArray()
                            val saved = savePcap(bytes)
                            pcapStatus.text = if (saved) {
                                "PCAP guardado · ${if (pcapExpectedBeacon) "Beacon + " else ""}$pcapExpectedEapol EAPOL · $pcapExpectedPackets frame(s)${if (pcapOverflow) " · captura truncada" else ""}"
                            } else {
                                "No se pudo guardar el PCAP"
                            }
                            exportPcapButton.isEnabled = pcapEapolCount > 0
                        }
                    }

                    "error" -> {
                        if (pcapExporting) {
                            pcapExporting = false
                            exportPcapButton.isEnabled = pcapEapolCount > 0
                        }
                        val msg = j.optString("msg", "Error en ESP32")
                        if (msg.contains("LAB", ignoreCase = true) || labActionInProgress) {
                            pendingLabDeauthMac = null
                            labActionInProgress = false
                            lastLabResult = msg
                            updateLabUi()
                            if (connected && monitorIsRunning) sendCommand("LAB_STATUS")
                        }
                        toast(msg)
                    }
                }
            }
        } catch (_: Exception) {
            runOnUiThread { toast("Evento BLE inválido o truncado") }
        }
    }

    private fun renderNetworks() {
        networksContainer.removeAllViews()
        networks.values.sortedByDescending { it.rssi }.forEach { n ->
            val card = panel(compact = true)
            card.addView(text(if (n.ssid.isBlank()) "[Red oculta]" else n.ssid, 19, true, Color.WHITE))
            card.addView(text("${n.security} · CH ${n.channel} · ${n.rssi} dBm", 13, false, Color.rgb(158, 169, 188)))
            card.addView(text(n.bssid, 13, false, Color.rgb(181, 191, 209)).apply {
                typeface = Typeface.MONOSPACE
                setPadding(0, dp(9), 0, 0)
            })
            card.addView(button("Seleccionar") { sendCommand("SELECT|${n.bssid}") })
            networksContainer.addView(card)
        }
    }

    private fun showSelected() {
        val n = selected ?: return
        selectedPanel.visibility = View.VISIBLE
        selectedTitle.text = n.ssid.ifBlank { "[Red oculta]" }
        selectedDetails.text = "Canal ${n.channel}   ·   ${n.rssi} dBm   ·   ${n.security}\n${n.bssid}"
        updateHandshakeViewer()
    }

    private fun updateHandshakeViewer() {
        if (!::handshakeSummary.isInitialized || !::handshakeDetails.isInitialized) return

        val n = selected
        val complete = lastHandshakeMask == 0x0F
        val started = lastHandshakeMask != 0

        handshakeSummary.text = when {
            complete -> "Handshake completo ✓"
            started -> "Handshake en progreso"
            else -> "Esperando handshake"
        }
        handshakeSummary.setTextColor(
            when {
                complete -> Color.rgb(97, 226, 160)
                started -> Color.rgb(238, 191, 100)
                else -> Color.rgb(165, 176, 195)
            }
        )

        val counts = "M1 x${lastMessageCounts[0]} · M2 x${lastMessageCounts[1]} · M3 x${lastMessageCounts[2]} · M4 x${lastMessageCounts[3]}"
        val beacon = if (beaconCaptured) "Sí" else "No todavía"
        val apName = n?.ssid?.ifBlank { "[Red oculta]" } ?: "--"
        val bssid = n?.bssid ?: "--"
        val channel = n?.channel?.toString() ?: "--"
        val security = n?.security ?: "--"

        handshakeDetails.text = buildString {
            append("AP: ").append(apName).append('\n')
            append("BSSID: ").append(bssid).append('\n')
            append("Cliente: ").append(lastClient).append('\n')
            append("Canal/seguridad: ").append(channel).append(" / ").append(security).append('\n')
            append("EAPOL observados: ").append(lastEapolCount).append('\n')
            append("Mensajes: ").append(counts).append('\n')
            append("Retransmisiones: ").append(lastRetransmissions).append('\n')
            append("Beacon incluido: ").append(beacon).append('\n')
            append("PCAP: ").append(pcapPacketCount).append(" frame(s), ").append(pcapEapolCount).append(" EAPOL")
        }
    }

    private fun requestLabClients() {
        if (!connected) {
            toast("BLE no está conectado")
            return
        }
        if (!monitorIsRunning) {
            toast("Iniciá el monitor antes de buscar clientes LAB")
            return
        }
        labClientsLoading = true
        updateLabUi()
        sendCommand("LAB_CLIENTS")
        sendCommand("LAB_STATUS")
    }

    private fun confirmLabReconnect(client: LabClient) {
        if (!connected || !monitorIsRunning) {
            toast("LAB requiere BLE conectado y monitor activo")
            return
        }
        if (labCooldownMs > 0L) {
            toast("LAB en cooldown: espera ${formatSeconds(labCooldownMs)}")
            return
        }
        if (labActionInProgress) return

        val networkName = selected?.ssid?.ifBlank { "[Red oculta]" } ?: "red seleccionada"
        AlertDialog.Builder(this)
            .setTitle("Reconexión controlada LAB")
            .setMessage(
                "Se enviará una única trama unicast al cliente ${client.mac} dentro de $networkName.\n\n" +
                    "Usalo solo con tu AP y tu dispositivo de prueba. No hay broadcast ni repetición automática."
            )
            .setNegativeButton("Cancelar", null)
            .setPositiveButton("Armar y ejecutar") { dialog, _ ->
                dialog.dismiss()
                pendingLabDeauthMac = client.mac
                labActionInProgress = true
                lastLabResult = "Armando LAB para ${client.mac}…"
                updateLabUi()
                sendCommand("LAB_ARM|LAB")
            }
            .show()
    }

    private fun updateLabUi() {
        if (!::labPanel.isInitialized) return

        labPanel.visibility = if (monitorIsRunning) View.VISIBLE else View.GONE
        labRefreshButton.isEnabled = connected && monitorIsRunning && !labActionInProgress

        val status = buildString {
            append("Modo: unicast-only")
            append("\nEstado: ")
            when {
                !connected -> append("BLE desconectado")
                !monitorIsRunning -> append("monitor detenido")
                labActionInProgress -> append("ejecutando prueba")
                labArmed -> append("armado por ").append(formatSeconds(labTtlMs))
                labCooldownMs > 0L -> append("cooldown ").append(formatSeconds(labCooldownMs))
                else -> append("listo")
            }
            if (lastLabResult.isNotBlank()) {
                append("\nÚltimo evento: ").append(lastLabResult)
            }
            append("\nClientes detectados: ").append(labClients.size)
        }
        labStatus.text = status
        renderLabClients()
    }

    private fun renderLabClients() {
        if (!::labClientsContainer.isInitialized) return
        labClientsContainer.removeAllViews()

        if (!monitorIsRunning) {
            labClientsContainer.addView(text("Iniciá el monitor para detectar clientes propios del BSSID seleccionado.", 13, false, Color.rgb(151, 163, 184)).apply {
                setPadding(0, dp(10), 0, 0)
            })
            return
        }

        if (labClientsLoading && labClients.isEmpty()) {
            labClientsContainer.addView(text("Buscando clientes vistos por el C3…", 13, false, Color.rgb(151, 163, 184)).apply {
                setPadding(0, dp(10), 0, 0)
            })
            return
        }

        if (labClients.isEmpty()) {
            labClientsContainer.addView(text(
                "Sin clientes detectados todavía. Dejá tu celular/PC navegar unos segundos en la red seleccionada y tocá Actualizar.",
                13,
                false,
                Color.rgb(151, 163, 184)
            ).apply { setPadding(0, dp(10), 0, 0) })
            return
        }

        labClients.values.sortedBy { it.ageMs }.forEach { client ->
            val card = LinearLayout(this).apply {
                orientation = LinearLayout.VERTICAL
                setPadding(dp(12), dp(12), dp(12), dp(12))
                background = rounded(Color.rgb(22, 31, 49), Color.rgb(55, 70, 100), 14f)
                layoutParams = LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT,
                    ViewGroup.LayoutParams.WRAP_CONTENT
                ).apply { topMargin = dp(10) }
            }
            card.addView(text(client.mac, 15, true, Color.WHITE).apply { typeface = Typeface.MONOSPACE })
            card.addView(text("Visto hace ${formatSeconds(client.ageMs)}", 13, false, Color.rgb(158, 169, 188)).apply {
                setPadding(0, dp(5), 0, 0)
            })
            val btn = button("Reconexión controlada") { confirmLabReconnect(client) }.apply {
                isEnabled = connected && monitorIsRunning && labCooldownMs <= 0L && !labActionInProgress
            }
            card.addView(btn)
            labClientsContainer.addView(card)
        }
    }

    private fun resetLabUi() {
        labClients.clear()
        labClientsLoading = false
        labArmed = false
        labTtlMs = 0L
        labCooldownMs = 0L
        labActionInProgress = false
        pendingLabDeauthMac = null
        lastLabResult = ""
        updateLabUi()
    }

    private fun formatSeconds(ms: Long): String {
        val seconds = maxOf(0L, (ms + 999L) / 1000L)
        return "${seconds}s"
    }

    private fun requestPcapExport() {
        if (pcapEapolCount <= 0) {
            toast("Todavía no hay paquetes EAPOL")
            return
        }
        if (pcapExporting) return

        pcapExporting = true
        exportPcapButton.isEnabled = false
        pcapStatus.text = "Solicitando PCAP…"
        sendCommand("GET_PCAP")
    }

    private fun resetPcapUi() {
        pcapPacketCount = 0
        pcapEapolCount = 0
        pcapExpectedSize = 0
        pcapExpectedPackets = 0
        pcapExpectedEapol = 0
        pcapExpectedBeacon = false
        pcapExporting = false
        pcapOverflow = false
        beaconCaptured = false
        lastHandshakeMask = 0
        lastEapolCount = 0
        lastClient = "--"
        lastRetransmissions = 0
        lastMessageCounts = intArrayOf(0, 0, 0, 0)
        pcapBytes.reset()

        if (::pcapStatus.isInitialized) pcapStatus.text = "PCAP: esperando Beacon + EAPOL"
        if (::exportPcapButton.isInitialized) exportPcapButton.isEnabled = false

        if (::monitorEapol.isInitialized) monitorEapol.text = "EAPOL\n0"
        if (::monitorClient.isInitialized) monitorClient.text = "Cliente: --"
        if (::handshakeSummary.isInitialized) {
            handshakeSummary.text = "Esperando handshake"
            handshakeSummary.setTextColor(Color.rgb(238, 191, 100))
        }
        if (::handshakeDetails.isInitialized) handshakeDetails.text = "Sin datos todavía."
        if (::m1.isInitialized) setHs(m1, "M1", false)
        if (::m2.isInitialized) setHs(m2, "M2", false)
        if (::m3.isInitialized) setHs(m3, "M3", false)
        if (::m4.isInitialized) setHs(m4, "M4", false)
    }

    private fun savePcap(bytes: ByteArray): Boolean {
        return try {
            val baseName = selected?.ssid
                ?.takeIf { it.isNotBlank() && it != "[Hidden]" }
                ?.replace(Regex("[^A-Za-z0-9._-]+"), "_")
                ?.trim('_')
                ?.takeIf { it.isNotBlank() }
                ?: "capture"

            val stamp = SimpleDateFormat("yyyyMMdd_HHmmss", Locale.US).format(Date())
            val fileName = "${baseName}_$stamp.pcap"

            if (Build.VERSION.SDK_INT >= 29) {
                val values = ContentValues().apply {
                    put(MediaStore.Downloads.DISPLAY_NAME, fileName)
                    put(MediaStore.Downloads.MIME_TYPE, "application/vnd.tcpdump.pcap")
                    put(MediaStore.Downloads.RELATIVE_PATH, Environment.DIRECTORY_DOWNLOADS + "/C3BleAnalyzer")
                    put(MediaStore.Downloads.IS_PENDING, 1)
                }

                val uri = contentResolver.insert(MediaStore.Downloads.EXTERNAL_CONTENT_URI, values)
                    ?: return false

                contentResolver.openOutputStream(uri, "w")?.use { it.write(bytes) }
                    ?: return false

                values.clear()
                values.put(MediaStore.Downloads.IS_PENDING, 0)
                contentResolver.update(uri, values, null, null)

                toast("Guardado en Descargas/C3BleAnalyzer/$fileName")
            } else {
                val dir = File(getExternalFilesDir(Environment.DIRECTORY_DOWNLOADS), "C3BleAnalyzer")
                if (!dir.exists()) dir.mkdirs()
                val file = File(dir, fileName)
                file.writeBytes(bytes)
                toast("Guardado: ${file.absolutePath}")
            }

            true
        } catch (e: Exception) {
            toast("Error guardando PCAP: ${e.message ?: "desconocido"}")
            false
        }
    }

    private fun sendCommand(command: String) {
        if (!::ble.isInitialized || !ble.send(command)) {
            if (pcapExporting) pcapExporting = false
            toast("BLE no está listo para enviar: $command")
        }
    }

    private fun panel(compact: Boolean = false): LinearLayout = LinearLayout(this).apply {
        orientation = LinearLayout.VERTICAL
        setPadding(
            dp(if (compact) 16 else 18),
            dp(if (compact) 15 else 18),
            dp(if (compact) 16 else 18),
            dp(if (compact) 15 else 18)
        )
        background = rounded(Color.rgb(20, 28, 45), Color.rgb(48, 62, 88), 18f)
        rootParams(this, if (compact) 9 else 12)
    }

    private fun rootParams(v: View, marginBottom: Int) {
        v.layoutParams = LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.WRAP_CONTENT
        ).apply { bottomMargin = dp(marginBottom) }
    }

    private fun button(title: String, danger: Boolean = false, action: () -> Unit): Button = Button(this).apply {
        text = title
        isAllCaps = false
        textSize = 15f
        setTextColor(Color.WHITE)
        background = rounded(
            if (danger) Color.rgb(105, 53, 64) else Color.rgb(79, 103, 238),
            Color.TRANSPARENT,
            13f
        )
        setPadding(dp(12), dp(4), dp(12), dp(4))
        setOnClickListener { action() }
        layoutParams = LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(54)).apply {
            topMargin = dp(12)
        }
    }

    private fun label(s: String) = text(s, 12, false, Color.rgb(133, 148, 173)).apply {
        letterSpacing = 0.12f
    }

    private fun text(s: String, sp: Int, bold: Boolean, color: Int) = TextView(this).apply {
        text = s
        textSize = sp.toFloat()
        setTextColor(color)
        if (bold) setTypeface(typeface, Typeface.BOLD)
    }

    private fun metric(label: String, value: String) = text("${label.uppercase()}\n$value", 17, true, Color.WHITE)

    private fun handshakeBox(name: String) = text("$name ○", 18, true, Color.rgb(139, 151, 172)).apply {
        gravity = Gravity.CENTER
        background = rounded(Color.rgb(34, 43, 62), Color.rgb(58, 72, 98), 12f)
    }

    private fun setHs(v: TextView, name: String, ok: Boolean) {
        v.text = if (ok) "$name ✓" else "$name ○"
        v.setTextColor(if (ok) Color.rgb(97, 226, 160) else Color.rgb(139, 151, 172))
        v.background = rounded(
            if (ok) Color.rgb(20, 61, 55) else Color.rgb(34, 43, 62),
            if (ok) Color.rgb(50, 143, 111) else Color.rgb(58, 72, 98),
            12f
        )
    }

    private fun rounded(fill: Int, stroke: Int, radius: Float) = GradientDrawable().apply {
        setColor(fill)
        cornerRadius = dp(radius.toInt()).toFloat()
        if (stroke != Color.TRANSPARENT) setStroke(dp(1), stroke)
    }

    private fun dp(v: Int) = (v * resources.displayMetrics.density).toInt()
    private fun toast(s: String) = Toast.makeText(this, s, Toast.LENGTH_SHORT).show()
}
