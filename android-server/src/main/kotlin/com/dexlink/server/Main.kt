package com.dexlink.server

import android.os.SystemClock
import android.view.KeyEvent
import com.dexlink.protocol.PacketProtocol
import java.io.BufferedInputStream
import java.io.BufferedOutputStream
import java.io.File
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.ServerSocket
import java.net.Socket
import java.nio.charset.StandardCharsets
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean

object Main {

    private data class Config(
        var host: String = "127.0.0.1",
        var port: Int = PacketProtocol.VIDEO_CONTROL_PORT,
        var listenMode: Boolean = false,
        var width: Int = 1920,
        var height: Int = 1080,
        var fps: Int = 60,
        var bitrateBps: Int = 20_000_000,
        var dpi: Int = 320,
        var mode: Int = PacketProtocol.MODE_DEX,
        var codec: Int = PacketProtocol.CODEC_H264,
        var launchPkg: String? = null
    )

    @JvmStatic
    fun main(args: Array<String>) {
        System.err.println("[DEXLINK] boot")

        val cfg = parseArgs(args)
        System.err.println("[DEXLINK] HiddenApi.prepare")
        HiddenApi.prepare()
        System.err.println("[DEXLINK] HiddenApi.createShellContext")
        val context = HiddenApi.createShellContext()
        System.err.println("[DEXLINK] connectTransport host=${cfg.host} port=${cfg.port} listen=${cfg.listenMode}")

        val socket = connectTransport(cfg)
        System.err.println("[DEXLINK] socket connected")
        socket.tcpNoDelay = true
        socket.sendBufferSize = 512 * 1024
        socket.receiveBufferSize = 128 * 1024

        val input = BufferedInputStream(socket.getInputStream(), 64 * 1024)
        val output = BufferedOutputStream(socket.getOutputStream(), 128 * 1024)
        val sendLock = Any()
        val running = AtomicBoolean(true)

        System.err.println("[DEXLINK] init controllers")
        val displayController = DisplayController(context)
        System.err.println("[DEXLINK] init InputInjector")
        val inputInjector = InputInjector()
        System.err.println("[DEXLINK] init TelemetryReader")
        val telemetryReader = TelemetryReader(context)

        var encoder: VideoEncoder? = null
        val pipelineLock = Any()

        fun startPipeline(initialPkg: String?) {
            synchronized(pipelineLock) {
                encoder?.stop()
                displayController.stopCapture()

                System.err.println("[DEXLINK] VideoEncoder.prepare ${cfg.width}x${cfg.height}@${cfg.fps}")
                val newEncoder = VideoEncoder(
                    width = cfg.width,
                    height = cfg.height,
                    fps = cfg.fps,
                    bitrateBps = cfg.bitrateBps,
                    codecType = cfg.codec,
                    sendLock = sendLock,
                    outputStream = output
                )
                val surface = newEncoder.prepare()
                System.err.println("[DEXLINK] DisplayController.startCapture mode=${cfg.mode}")
                displayController.startCapture(
                    mode = cfg.mode,
                    width = cfg.width,
                    height = cfg.height,
                    dpi = cfg.dpi,
                    fps = cfg.fps,
                    encoderSurface = surface,
                    launchPackage = initialPkg
                )
                System.err.println("[DEXLINK] VideoEncoder.startStreaming")
                newEncoder.startStreaming()
                encoder = newEncoder
                println("DEXLINK_STREAMING ${cfg.width}x${cfg.height}@${cfg.fps} mode=${cfg.mode} display=${displayController.activeDisplayId}")
            }
        }

        startPipeline(cfg.launchPkg)

        val telemetryScheduler = Executors.newSingleThreadScheduledExecutor { r ->
            Thread(r, "dexlink-telemetry").apply { isDaemon = true }
        }
        val telemetryHeader = ByteArray(PacketProtocol.HEADER_SIZE)
        val telemetryPayload = ByteArray(PacketProtocol.TELEMETRY_PAYLOAD_SIZE)
        telemetryScheduler.scheduleAtFixedRate({
            if (!running.get()) return@scheduleAtFixedRate
            runCatching {
                val snap = telemetryReader.read()
                var flags = 0
                if (snap.charging) flags = flags or PacketProtocol.TELEMETRY_CHARGING
                if (displayController.isScreenOff) flags = flags or PacketProtocol.TELEMETRY_SCREEN_OFF
                if (cfg.mode == PacketProtocol.MODE_DEX) flags = flags or PacketProtocol.TELEMETRY_DEX_MODE
                if (snap.tempC >= 43.0f) flags = flags or PacketProtocol.TELEMETRY_THERMAL_WARN

                val pkt = PacketProtocol.TelemetryPacket(
                    tempC = snap.tempC,
                    encFps = encoder?.currentFps ?: 0.0f,
                    batteryPct = snap.batteryPct,
                    statusFlags = flags,
                    reserved = displayController.activeDisplayId and 0xFFFF
                )
                PacketProtocol.encodeTelemetry(pkt, telemetryPayload)
                synchronized(sendLock) {
                    PacketProtocol.writePacket(
                        output,
                        telemetryHeader,
                        PacketProtocol.TYPE_TELEMETRY,
                        0,
                        SystemClock.uptimeMillis(),
                        telemetryPayload
                    )
                }
            }
        }, 200, 500, TimeUnit.MILLISECONDS)

        // Control & Input Receive Loop
        val hdrBuf = ByteArray(PacketProtocol.HEADER_SIZE)
        val pongHeader = ByteArray(PacketProtocol.HEADER_SIZE)
        try {
            while (running.get()) {
                PacketProtocol.readFully(input, hdrBuf, 0, PacketProtocol.HEADER_SIZE)
                val hdr = PacketProtocol.decodeHeader(hdrBuf)
                val payload = ByteArray(hdr.payloadSize)
                if (hdr.payloadSize > 0) {
                    PacketProtocol.readFully(input, payload, 0, hdr.payloadSize)
                }

                when (hdr.type) {
                    PacketProtocol.TYPE_TOUCH_EVENT -> {
                        if (payload.size >= PacketProtocol.TOUCH_PAYLOAD_SIZE) {
                            val ev = PacketProtocol.decodeTouch(payload)
                            inputInjector.injectTouch(
                                ev,
                                displayController.activeWidth,
                                displayController.activeHeight,
                                displayController.activeDisplayId
                            )
                        }
                    }

                    PacketProtocol.TYPE_KEY_EVENT -> {
                        if (payload.size >= PacketProtocol.KEY_PAYLOAD_SIZE) {
                            val ev = PacketProtocol.decodeKey(payload)
                            if (ev.keycode == KeyEvent.KEYCODE_HOME && cfg.mode == PacketProtocol.MODE_DEX) {
                                if (ev.action == PacketProtocol.KEY_DOWN) {
                                    displayController.launchDefaultDesktopOrHome(displayController.activeDisplayId)
                                }
                            } else {
                                inputInjector.injectKey(ev, displayController.activeDisplayId)
                            }
                        }
                    }

                    PacketProtocol.TYPE_CONTROL_CMD -> {
                        if (payload.size >= PacketProtocol.CONTROL_PAYLOAD_SIZE) {
                            val cmd = PacketProtocol.decodeControl(payload)
                            when (cmd.cmd) {
                                PacketProtocol.CMD_SET_RES -> {
                                    val newW = cmd.arg0.coerceIn(320, 3840)
                                    val newH = cmd.arg1.coerceIn(240, 2160)
                                    val newFps = if (cmd.argU16 in 24..144) cmd.argU16 else cfg.fps
                                    val newCodec = cmd.argU8
                                    if (newW != cfg.width || newH != cfg.height || newFps != cfg.fps || newCodec != cfg.codec) {
                                        cfg.width = newW
                                        cfg.height = newH
                                        cfg.fps = newFps
                                        cfg.codec = newCodec
                                        inputInjector.clearAllPointers(displayController.activeDisplayId)
                                        startPipeline(null)
                                    }
                                }

                                PacketProtocol.CMD_SET_BITRATE -> {
                                    val newBitrate = cmd.arg0.coerceIn(1_000_000, 80_000_000)
                                    cfg.bitrateBps = newBitrate
                                    encoder?.updateBitrate(newBitrate)
                                }

                                PacketProtocol.CMD_SCREEN_OFF -> {
                                    displayController.setScreenPowerOff(cmd.argU8 != 0)
                                }

                                PacketProtocol.CMD_REQUEST_IDR -> {
                                    encoder?.requestIdr()
                                }

                                PacketProtocol.CMD_SET_MODE -> {
                                    val newMode = cmd.argU8
                                    val newDpi = if (cmd.argU16 in 120..640) cmd.argU16 else cfg.dpi
                                    if (newMode != cfg.mode || newDpi != cfg.dpi) {
                                        cfg.mode = newMode
                                        cfg.dpi = newDpi
                                        inputInjector.clearAllPointers(displayController.activeDisplayId)
                                        startPipeline(null)
                                    }
                                }

                                PacketProtocol.CMD_PING -> {
                                    val pongPayload = PacketProtocol.encodeControl(
                                        PacketProtocol.ControlCommand(
                                            cmd = PacketProtocol.CMD_PONG,
                                            argU8 = 0,
                                            argU16 = 0,
                                            arg0 = cmd.arg0,
                                            arg1 = cmd.arg1
                                        )
                                    )
                                    synchronized(sendLock) {
                                        PacketProtocol.writePacket(
                                            output,
                                            pongHeader,
                                            PacketProtocol.TYPE_CONTROL_CMD,
                                            0,
                                            hdr.timestampMs,
                                            pongPayload
                                        )
                                    }
                                }

                                PacketProtocol.CMD_LAUNCH_APP -> {
                                    if (payload.size > PacketProtocol.CONTROL_PAYLOAD_SIZE) {
                                        val pkg = String(
                                            payload,
                                            PacketProtocol.CONTROL_PAYLOAD_SIZE,
                                            payload.size - PacketProtocol.CONTROL_PAYLOAD_SIZE,
                                            StandardCharsets.UTF_8
                                        ).trim()
                                        if (pkg.isNotEmpty()) {
                                            displayController.launchPackageOnDisplay(pkg)
                                        }
                                    }
                                }

                                PacketProtocol.CMD_NAV_ACTION -> {
                                    when {
                                        cmd.argU8 == 1 && cfg.mode == PacketProtocol.MODE_DEX -> {
                                            displayController.launchDefaultDesktopOrHome(displayController.activeDisplayId)
                                        }
                                        cmd.argU8 == 3 -> {
                                            displayController.closeForegroundAppOnDisplay(displayController.activeDisplayId)
                                        }
                                        else -> {
                                            val keycode = when (cmd.argU8) {
                                                0 -> KeyEvent.KEYCODE_BACK
                                                1 -> KeyEvent.KEYCODE_HOME
                                                2 -> KeyEvent.KEYCODE_APP_SWITCH
                                                else -> KeyEvent.KEYCODE_BACK
                                            }
                                            inputInjector.injectNavKey(keycode, displayController.activeDisplayId)
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        } catch (e: Exception) {
            System.err.println("[DEXLINK Server] Transport ended: ${e.message}")
        } finally {
            running.set(false)
            telemetryScheduler.shutdownNow()
            runCatching { displayController.setScreenPowerOff(false) }
            runCatching { encoder?.stop() }
            runCatching { displayController.stopCapture() }
            runCatching { socket.close() }
        }
    }

    private fun connectTransport(cfg: Config): Socket {
        if (cfg.listenMode) {
            ServerSocket(cfg.port, 4, InetAddress.getByName("127.0.0.1")).use { ss ->
                println("DEXLINK_LISTENING 127.0.0.1:${cfg.port}")
                return ss.accept()
            }
        }

        // Default adb reverse mode: connect to PC listener on 127.0.0.1:port with fast retry
        var lastErr: Exception? = null
        repeat(40) {
            try {
                val sock = Socket()
                sock.connect(InetSocketAddress(cfg.host, cfg.port), 1000)
                return sock
            } catch (e: Exception) {
                lastErr = e
                Thread.sleep(100)
            }
        }
        throw IllegalStateException("Failed to connect to ${cfg.host}:${cfg.port}", lastErr)
    }

    private fun parseArgs(args: Array<String>): Config {
        val cfg = Config()
        var i = 0
        while (i < args.size) {
            val key = args[i]
            val value = args.getOrNull(i + 1)
            when (key) {
                "--host" -> if (value != null) { cfg.host = value; i++ }
                "--port" -> if (value != null) { cfg.port = value.toIntOrNull() ?: cfg.port; i++ }
                "--listen" -> cfg.listenMode = true
                "--width" -> if (value != null) { cfg.width = value.toIntOrNull() ?: cfg.width; i++ }
                "--height" -> if (value != null) { cfg.height = value.toIntOrNull() ?: cfg.height; i++ }
                "--fps" -> if (value != null) { cfg.fps = value.toIntOrNull() ?: cfg.fps; i++ }
                "--bitrate" -> if (value != null) { cfg.bitrateBps = value.toIntOrNull() ?: cfg.bitrateBps; i++ }
                "--dpi" -> if (value != null) { cfg.dpi = value.toIntOrNull() ?: cfg.dpi; i++ }
                "--mode" -> if (value != null) {
                    cfg.mode = if (value.equals("mirror", ignoreCase = true)) PacketProtocol.MODE_MIRROR else PacketProtocol.MODE_DEX
                    i++
                }
                "--codec" -> if (value != null) {
                    cfg.codec = if (value.equals("hevc", ignoreCase = true)) PacketProtocol.CODEC_HEVC else PacketProtocol.CODEC_H264
                    i++
                }
                "--pkg" -> if (value != null) { cfg.launchPkg = value; i++ }
            }
            i++
        }
        return cfg
    }
}
