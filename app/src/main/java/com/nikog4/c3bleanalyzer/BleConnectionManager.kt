package com.nikog4.c3bleanalyzer

import android.Manifest
import android.app.Activity
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.bluetooth.BluetoothStatusCodes
import android.bluetooth.le.BluetoothLeScanner
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.Context
import android.content.pm.PackageManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.ParcelUuid
import java.nio.charset.StandardCharsets
import java.util.ArrayDeque
import java.util.UUID

/**
 * Central BLE robusto para C3 BLE Analyzer.
 *
 * Todo el estado interno vive en el main looper. Los callbacks Binder/BLE
 * solamente copian sus datos y los publican al main looper, evitando carreras
 * entre scan, GATT, cola de writes y reconexión.
 */
class BleConnectionManager(
    private val activity: Activity,
    private val deviceName: String,
    private val serviceUuid: UUID,
    private val commandUuid: UUID,
    private val eventUuid: UUID,
    private val listener: Listener
) {
    enum class State {
        DISCONNECTED,
        SCANNING,
        CONNECTING,
        DISCOVERING,
        SUBSCRIBING,
        READY,
        RECOVERING
    }

    interface Listener {
        fun onStateChanged(state: State, detail: String = "")
        fun onReady()
        fun onEvent(raw: String)
        fun onTransferInterrupted()
        fun onError(message: String)
    }

    private val main = Handler(Looper.getMainLooper())
    private val cccdUuid = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")
    private val backoffMs = longArrayOf(1000, 2000, 5000, 10000, 15000, 30000)

    private var scanner: BluetoothLeScanner? = null
    private var gatt: BluetoothGatt? = null
    private var commandCharacteristic: BluetoothGattCharacteristic? = null
    private var eventCharacteristic: BluetoothGattCharacteristic? = null

    private var state: State = State.DISCONNECTED
    private var userWantsConnection = false
    private var destroyed = false
    private var scanRunning = false
    private var servicesDiscoveryStarted = false
    private var reconnectAttempt = 0
    private var generation = 0

    private val commandQueue = ArrayDeque<String>()
    private var writeInFlight = false

    val isReady: Boolean get() = state == State.READY
    val isActive: Boolean get() = userWantsConnection || state != State.DISCONNECTED

    fun connect() {
        onMain {
            if (destroyed) return@onMain
            userWantsConnection = true
            reconnectAttempt = 0
            invalidateTimers()
            startScan(recovery = false)
        }
    }

    fun disconnect() {
        onMain {
            userWantsConnection = false
            invalidateTimers()
            stopScan()
            clearCommandQueue()
            closeCurrentGatt(disconnectFirst = true)
            setState(State.DISCONNECTED)
        }
    }

    fun shutdown() {
        onMain {
            destroyed = true
            userWantsConnection = false
            invalidateTimers()
            stopScan()
            clearCommandQueue()
            closeCurrentGatt(disconnectFirst = true)
        }
    }

    /**
     * En esta app los comandos salen del main thread. No guardamos acciones de
     * usuario mientras el enlace está caído: después de reconectar STATUS
     * restaura estado, pero START/STOP/SCAN no se ejecutan sorpresivamente.
     */
    fun send(command: String): Boolean {
        if (Looper.myLooper() != Looper.getMainLooper()) return false
        if (!isReady || command.isBlank()) return false
        commandQueue.addLast(command)
        pumpWrites()
        return true
    }

    private fun hasPermissions(): Boolean {
        return if (Build.VERSION.SDK_INT >= 31) {
            activity.checkSelfPermission(Manifest.permission.BLUETOOTH_SCAN) == PackageManager.PERMISSION_GRANTED &&
                activity.checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) == PackageManager.PERMISSION_GRANTED
        } else {
            activity.checkSelfPermission(Manifest.permission.ACCESS_FINE_LOCATION) == PackageManager.PERMISSION_GRANTED
        }
    }

    @Suppress("MissingPermission")
    private fun startScan(recovery: Boolean) {
        if (destroyed || !userWantsConnection) return
        if (!hasPermissions()) {
            userWantsConnection = false
            setState(State.DISCONNECTED)
            listener.onError("Faltan permisos Bluetooth")
            return
        }

        val manager = activity.getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager
        val adapter = manager.adapter
        if (adapter == null || !adapter.isEnabled) {
            scheduleReconnect("Bluetooth apagado")
            return
        }

        scanner = adapter.bluetoothLeScanner
        if (scanner == null) {
            scheduleReconnect("Escáner BLE no disponible")
            return
        }

        stopScan()
        scanRunning = true
        setState(
            if (recovery) State.RECOVERING else State.SCANNING,
            if (recovery) "buscando C3" else ""
        )

        // Los filtros en la lista se combinan con OR. El C3 anuncia ambos,
        // pero aceptar UUID o nombre evita depender de una MAC concreta.
        val filters = listOf(
            ScanFilter.Builder().setServiceUuid(ParcelUuid(serviceUuid)).build(),
            ScanFilter.Builder().setDeviceName(deviceName).build()
        )
        val settings = ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
            .build()

        try {
            scanner?.startScan(filters, settings, scanCallback)
        } catch (e: Exception) {
            scanRunning = false
            scheduleReconnect("falló scan: ${e.message ?: "desconocido"}")
            return
        }

        val token = generation
        main.postDelayed({
            if (token == generation && scanRunning && userWantsConnection) {
                stopScan()
                scheduleReconnect("C3 no encontrado")
            }
        }, 10_000)
    }

    @Suppress("MissingPermission")
    private fun stopScan() {
        if (!scanRunning) return
        scanRunning = false
        try { scanner?.stopScan(scanCallback) } catch (_: Exception) {}
    }

    private val scanCallback = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            main.post {
                if (!scanRunning || !userWantsConnection || destroyed) return@post
                stopScan()
                connectGatt(result.device)
            }
        }

        override fun onScanFailed(errorCode: Int) {
            main.post {
                scanRunning = false
                scheduleReconnect("scan error $errorCode")
            }
        }
    }

    @Suppress("MissingPermission")
    private fun connectGatt(device: BluetoothDevice) {
        if (!userWantsConnection || destroyed) return
        setState(State.CONNECTING)
        servicesDiscoveryStarted = false
        closeCurrentGatt(disconnectFirst = false)

        val newGatt = try {
            device.connectGatt(activity, false, gattCallback, BluetoothDevice.TRANSPORT_LE)
        } catch (_: Exception) {
            null
        }

        if (newGatt == null) {
            scheduleReconnect("connectGatt falló")
            return
        }
        gatt = newGatt

        val token = generation
        main.postDelayed({
            if (token == generation && state == State.CONNECTING && userWantsConnection) {
                recoverFromGatt("timeout conectando")
            }
        }, 12_000)
    }

    private val gattCallback = object : BluetoothGattCallback() {
        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
            main.post { handleConnectionStateChange(g, status, newState) }
        }

        override fun onMtuChanged(g: BluetoothGatt, mtu: Int, status: Int) {
            main.post {
                if (g === gatt) discoverServicesOnce(g)
            }
        }

        override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
            main.post { handleServicesDiscovered(g, status) }
        }

        override fun onDescriptorWrite(g: BluetoothGatt, descriptor: BluetoothGattDescriptor, status: Int) {
            main.post { handleDescriptorWrite(g, descriptor, status) }
        }

        override fun onCharacteristicWrite(g: BluetoothGatt, characteristic: BluetoothGattCharacteristic, status: Int) {
            main.post { handleCharacteristicWrite(g, characteristic, status) }
        }

        @Deprecated("Deprecated in API 33")
        override fun onCharacteristicChanged(g: BluetoothGatt, characteristic: BluetoothGattCharacteristic) {
            val value = characteristic.value?.copyOf() ?: byteArrayOf()
            main.post { deliverNotification(g, characteristic.uuid, value) }
        }

        override fun onCharacteristicChanged(g: BluetoothGatt, characteristic: BluetoothGattCharacteristic, value: ByteArray) {
            val copy = value.copyOf()
            main.post { deliverNotification(g, characteristic.uuid, copy) }
        }
    }

    @Suppress("MissingPermission")
    private fun handleConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
        if (g !== gatt) {
            try { g.close() } catch (_: Exception) {}
            return
        }

        if (status != BluetoothGatt.GATT_SUCCESS) {
            recoverFromGatt("GATT status $status")
            return
        }

        when (newState) {
            BluetoothProfile.STATE_CONNECTED -> {
                reconnectAttempt = 0
                servicesDiscoveryStarted = false
                try { g.requestConnectionPriority(BluetoothGatt.CONNECTION_PRIORITY_HIGH) } catch (_: Exception) {}
                val mtuStarted = try { g.requestMtu(247) } catch (_: Exception) { false }
                if (!mtuStarted) discoverServicesOnce(g)

                val token = generation
                main.postDelayed({
                    if (token == generation && g === gatt && !servicesDiscoveryStarted && userWantsConnection) {
                        discoverServicesOnce(g)
                    }
                }, 900)
            }

            BluetoothProfile.STATE_DISCONNECTED -> recoverFromGatt("enlace perdido")
        }
    }

    @Suppress("MissingPermission")
    private fun handleServicesDiscovered(g: BluetoothGatt, status: Int) {
        if (g !== gatt) return
        if (status != BluetoothGatt.GATT_SUCCESS) {
            recoverFromGatt("service discovery status $status")
            return
        }

        val service = g.getService(serviceUuid)
        val cmd = service?.getCharacteristic(commandUuid)
        val evt = service?.getCharacteristic(eventUuid)
        if (cmd == null || evt == null) {
            recoverFromGatt("servicio C3 incompleto")
            return
        }

        commandCharacteristic = cmd
        eventCharacteristic = evt
        setState(State.SUBSCRIBING)

        if (!g.setCharacteristicNotification(evt, true)) {
            recoverFromGatt("no se pudo habilitar notify local")
            return
        }

        val cccd = evt.getDescriptor(cccdUuid)
        if (cccd == null) {
            recoverFromGatt("CCCD no encontrado")
            return
        }

        val started = if (Build.VERSION.SDK_INT >= 33) {
            g.writeDescriptor(cccd, BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE) == BluetoothStatusCodes.SUCCESS
        } else {
            @Suppress("DEPRECATION")
            run {
                cccd.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
                g.writeDescriptor(cccd)
            }
        }

        if (!started) {
            recoverFromGatt("no se pudo escribir CCCD")
            return
        }

        val token = generation
        main.postDelayed({
            if (token == generation && state == State.SUBSCRIBING && userWantsConnection) {
                recoverFromGatt("timeout suscribiendo notify")
            }
        }, 6000)
    }

    private fun handleDescriptorWrite(g: BluetoothGatt, descriptor: BluetoothGattDescriptor, status: Int) {
        if (g !== gatt || descriptor.uuid != cccdUuid) return
        if (status != BluetoothGatt.GATT_SUCCESS) {
            recoverFromGatt("CCCD status $status")
            return
        }

        setState(State.READY)
        listener.onReady()
    }

    private fun handleCharacteristicWrite(g: BluetoothGatt, characteristic: BluetoothGattCharacteristic, status: Int) {
        if (g !== gatt || characteristic.uuid != commandUuid) return
        writeInFlight = false

        val current = commandQueue.firstOrNull()
        if (status != BluetoothGatt.GATT_SUCCESS) {
            if (commandQueue.isNotEmpty()) commandQueue.removeFirst()
            listener.onError("Falló comando BLE ($status): ${current ?: "?"}")
            recoverFromGatt("write status $status")
            return
        }

        if (commandQueue.isNotEmpty()) commandQueue.removeFirst()
        pumpWrites()
    }

    private fun deliverNotification(g: BluetoothGatt, uuid: UUID, value: ByteArray) {
        if (g !== gatt || uuid != eventUuid || value.isEmpty()) return
        listener.onEvent(String(value, StandardCharsets.UTF_8))
    }

    @Suppress("MissingPermission")
    private fun discoverServicesOnce(g: BluetoothGatt) {
        if (g !== gatt || servicesDiscoveryStarted || !userWantsConnection) return
        servicesDiscoveryStarted = true
        setState(State.DISCOVERING)
        if (!g.discoverServices()) {
            recoverFromGatt("discoverServices no inició")
            return
        }

        val token = generation
        main.postDelayed({
            if (token == generation && state == State.DISCOVERING && userWantsConnection) {
                recoverFromGatt("timeout descubriendo servicios")
            }
        }, 8000)
    }

    @Suppress("MissingPermission")
    private fun pumpWrites() {
        if (!isReady || writeInFlight || commandQueue.isEmpty()) return
        val g = gatt ?: return
        val c = commandCharacteristic ?: return
        val command = commandQueue.first()
        val bytes = command.toByteArray(StandardCharsets.UTF_8)
        writeInFlight = true

        val started = if (Build.VERSION.SDK_INT >= 33) {
            g.writeCharacteristic(c, bytes, BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT) == BluetoothStatusCodes.SUCCESS
        } else {
            @Suppress("DEPRECATION")
            run {
                c.writeType = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
                c.value = bytes
                g.writeCharacteristic(c)
            }
        }

        if (!started) {
            writeInFlight = false
            if (commandQueue.isNotEmpty()) commandQueue.removeFirst()
            listener.onError("No se pudo iniciar write BLE: $command")
            recoverFromGatt("write no inició")
        }
    }

    @Suppress("MissingPermission")
    private fun recoverFromGatt(reason: String) {
        if (destroyed) return
        clearCommandQueue()
        closeCurrentGatt(disconnectFirst = true)
        invalidateTimers()
        listener.onTransferInterrupted()

        if (!userWantsConnection) {
            setState(State.DISCONNECTED)
            return
        }
        scheduleReconnect(reason)
    }

    private fun scheduleReconnect(reason: String) {
        if (destroyed || !userWantsConnection) {
            setState(State.DISCONNECTED)
            return
        }

        stopScan()
        val delay = backoffMs[minOf(reconnectAttempt, backoffMs.lastIndex)]
        reconnectAttempt++
        val seconds = maxOf(1, delay / 1000)
        setState(State.RECOVERING, "$reason · ${seconds}s")

        val token = generation
        main.postDelayed({
            if (token == generation && userWantsConnection && !destroyed) {
                startScan(recovery = true)
            }
        }, delay)
    }

    @Suppress("MissingPermission")
    private fun closeCurrentGatt(disconnectFirst: Boolean) {
        val old = gatt
        gatt = null
        commandCharacteristic = null
        eventCharacteristic = null
        servicesDiscoveryStarted = false
        if (old != null) {
            if (disconnectFirst) {
                try { old.disconnect() } catch (_: Exception) {}
            }
            try { old.close() } catch (_: Exception) {}
        }
    }

    private fun clearCommandQueue() {
        commandQueue.clear()
        writeInFlight = false
    }

    private fun invalidateTimers() {
        generation++
        main.removeCallbacksAndMessages(null)
    }

    private fun setState(newState: State, detail: String = "") {
        state = newState
        listener.onStateChanged(newState, detail)
    }

    private inline fun onMain(crossinline block: () -> Unit) {
        if (Looper.myLooper() == Looper.getMainLooper()) {
            block()
        } else {
            main.post { block() }
        }
    }
}
