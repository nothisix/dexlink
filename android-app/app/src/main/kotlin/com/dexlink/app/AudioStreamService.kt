package com.dexlink.app

import android.annotation.SuppressLint
import android.app.Activity
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Intent
import android.content.pm.ServiceInfo
import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioPlaybackCaptureConfiguration
import android.media.AudioRecord
import android.media.projection.MediaProjection
import android.media.projection.MediaProjectionManager
import android.os.Build
import android.os.IBinder
import android.os.Process
import android.os.SystemClock
import com.dexlink.protocol.PacketProtocol
import java.io.BufferedOutputStream
import java.net.InetSocketAddress
import java.net.Socket
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicReference

class AudioStreamService : Service() {

    enum class LinkState {
        WAIT,
        CONNECTED,
        ERR
    }

    data class ServiceStatus(
        val active: Boolean = false,
        val linkState: LinkState = LinkState.WAIT,
        val audioPermGranted: Boolean = false
    )

    companion object {
        const val ACTION_START = "com.dexlink.app.START"
        const val ACTION_STOP = "com.dexlink.app.STOP"
        const val EXTRA_RESULT_CODE = "result_code"
        const val EXTRA_RESULT_DATA = "result_data"

        private const val CHANNEL_ID = "dexlink_audio"
        private const val NOTIF_ID = 27184
        private const val SAMPLE_RATE = 48000
        private const val CHUNK_BYTES = 960 // 5ms @ 48kHz 16-bit stereo (240 frames * 4 bytes)

        private val currentStatus = AtomicReference(ServiceStatus())
        @Volatile
        var statusListener: ((ServiceStatus) -> Unit)? = null

        fun getStatus(): ServiceStatus = currentStatus.get()

        private fun publishStatus(status: ServiceStatus) {
            currentStatus.set(status)
            statusListener?.invoke(status)
        }
    }

    private val running = AtomicBoolean(false)
    private var workerThread: Thread? = null
    private var mediaProjection: MediaProjection? = null

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_STOP -> {
                stopStreaming()
                stopSelf()
                return START_NOT_STICKY
            }

            ACTION_START -> {
                val resultCode = intent.getIntExtra(EXTRA_RESULT_CODE, Activity.RESULT_CANCELED)
                @Suppress("DEPRECATION")
                val data: Intent? = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                    intent.getParcelableExtra(EXTRA_RESULT_DATA, Intent::class.java)
                } else {
                    intent.getParcelableExtra(EXTRA_RESULT_DATA)
                }
                if (resultCode != Activity.RESULT_OK || data == null) {
                    publishStatus(ServiceStatus(active = false, linkState = LinkState.ERR, audioPermGranted = false))
                    stopSelf()
                    return START_NOT_STICKY
                }
                startForegroundProjection()
                startStreaming(resultCode, data)
                return START_NOT_STICKY
            }
        }
        return START_NOT_STICKY
    }

    private fun startForegroundProjection() {
        val nm = getSystemService(NotificationManager::class.java)
        val ch = NotificationChannel(CHANNEL_ID, "DEXLINK", NotificationManager.IMPORTANCE_LOW)
        nm.createNotificationChannel(ch)

        val notif = Notification.Builder(this, CHANNEL_ID)
            .setContentTitle("DEXLINK")
            .setContentText("USB AUDIO ACTIVE")
            .setSmallIcon(android.R.drawable.stat_sys_data_bluetooth)
            .setOngoing(true)
            .build()

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            startForeground(NOTIF_ID, notif, ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION)
        } else {
            startForeground(NOTIF_ID, notif)
        }
    }

    @SuppressLint("MissingPermission")
    private fun startStreaming(resultCode: Int, data: Intent) {
        stopStreaming()

        val mpm = getSystemService(MediaProjectionManager::class.java)
        val projection = mpm.getMediaProjection(resultCode, data)
        if (projection == null) {
            publishStatus(ServiceStatus(active = false, linkState = LinkState.ERR, audioPermGranted = false))
            stopSelf()
            return
        }

        mediaProjection = projection
        projection.registerCallback(object : MediaProjection.Callback() {
            override fun onStop() {
                stopStreaming()
                stopSelf()
            }
        }, null)

        running.set(true)
        publishStatus(ServiceStatus(active = true, linkState = LinkState.WAIT, audioPermGranted = true))

        workerThread = Thread({
            Process.setThreadPriority(Process.THREAD_PRIORITY_URGENT_AUDIO)
            runCaptureLoop(projection)
        }, "dexlink-audio-tx").apply {
            priority = Thread.MAX_PRIORITY
            start()
        }
    }

    @SuppressLint("MissingPermission")
    private fun runCaptureLoop(projection: MediaProjection) {
        val captureConfig = AudioPlaybackCaptureConfiguration.Builder(projection)
            .addMatchingUsage(AudioAttributes.USAGE_MEDIA)
            .addMatchingUsage(AudioAttributes.USAGE_GAME)
            .addMatchingUsage(AudioAttributes.USAGE_UNKNOWN)
            .build()

        val audioFormat = AudioFormat.Builder()
            .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
            .setSampleRate(SAMPLE_RATE)
            .setChannelMask(AudioFormat.CHANNEL_IN_STEREO)
            .build()

        val minBuf = AudioRecord.getMinBufferSize(
            SAMPLE_RATE,
            AudioFormat.CHANNEL_IN_STEREO,
            AudioFormat.ENCODING_PCM_16BIT
        ).coerceAtLeast(CHUNK_BYTES * 4)

        val audioRecord = try {
            AudioRecord.Builder()
                .setAudioPlaybackCaptureConfig(captureConfig)
                .setAudioFormat(audioFormat)
                .setBufferSizeInBytes(minBuf)
                .build()
        } catch (_: Exception) {
            publishStatus(ServiceStatus(active = false, linkState = LinkState.ERR, audioPermGranted = false))
            return
        }

        if (audioRecord.state != AudioRecord.STATE_INITIALIZED) {
            audioRecord.release()
            publishStatus(ServiceStatus(active = false, linkState = LinkState.ERR, audioPermGranted = false))
            return
        }

        val pcmBuf = ByteArray(CHUNK_BYTES)
        val headerScratch = ByteArray(PacketProtocol.HEADER_SIZE)
        val telemetryPayload = ByteArray(PacketProtocol.TELEMETRY_PAYLOAD_SIZE)
        var socket: Socket? = null
        var output: BufferedOutputStream? = null
        var lastTelemetryMs = 0L

        audioRecord.startRecording()

        try {
            while (running.get()) {
                val readBytes = audioRecord.read(pcmBuf, 0, CHUNK_BYTES, AudioRecord.READ_BLOCKING)
                if (readBytes <= 0) continue

                if (socket == null || socket.isClosed || !socket.isConnected) {
                    val connected = tryConnectAudioSocket()
                    if (connected != null) {
                        socket = connected
                        output = BufferedOutputStream(connected.getOutputStream(), 16 * 1024)
                        publishStatus(ServiceStatus(active = true, linkState = LinkState.CONNECTED, audioPermGranted = true))
                    } else {
                        publishStatus(ServiceStatus(active = true, linkState = LinkState.WAIT, audioPermGranted = true))
                        Thread.sleep(250)
                        continue
                    }
                }

                val out = output ?: continue
                val nowMs = SystemClock.uptimeMillis()
                try {
                    PacketProtocol.writePacket(
                        out,
                        headerScratch,
                        PacketProtocol.TYPE_AUDIO_PCM,
                        PacketProtocol.AUDIO_FLAG_PCM_48K_STEREO_S16,
                        nowMs,
                        pcmBuf,
                        0,
                        readBytes
                    )

                    if (nowMs - lastTelemetryMs >= 500L) {
                        lastTelemetryMs = nowMs
                        val thermal = TelemetryHelper.read(this)
                        var flags = PacketProtocol.TELEMETRY_AUDIO_ACTIVE
                        if (thermal.charging) flags = flags or PacketProtocol.TELEMETRY_CHARGING
                        if (thermal.thermalWarn) flags = flags or PacketProtocol.TELEMETRY_THERMAL_WARN
                        PacketProtocol.encodeTelemetry(
                            PacketProtocol.TelemetryPacket(
                                tempC = thermal.tempC,
                                encFps = 0.0f,
                                batteryPct = thermal.batteryPct,
                                statusFlags = flags
                            ),
                            telemetryPayload
                        )
                        PacketProtocol.writePacket(
                            out,
                            headerScratch,
                            PacketProtocol.TYPE_TELEMETRY,
                            0,
                            nowMs,
                            telemetryPayload
                        )
                    }
                } catch (_: Exception) {
                    runCatching { socket.close() }
                    socket = null
                    output = null
                    publishStatus(ServiceStatus(active = true, linkState = LinkState.WAIT, audioPermGranted = true))
                }
            }
        } finally {
            runCatching { audioRecord.stop() }
            runCatching { audioRecord.release() }
            runCatching { socket?.close() }
        }
    }

    private fun tryConnectAudioSocket(): Socket? {
        return runCatching {
            val s = Socket()
            s.tcpNoDelay = true
            s.sendBufferSize = 64 * 1024
            s.connect(InetSocketAddress("127.0.0.1", PacketProtocol.AUDIO_TELEMETRY_PORT), 350)
            s
        }.getOrNull()
    }

    private fun stopStreaming() {
        running.set(false)
        runCatching { workerThread?.join(500) }
        workerThread = null
        runCatching { mediaProjection?.stop() }
        mediaProjection = null
        publishStatus(ServiceStatus(active = false, linkState = LinkState.WAIT, audioPermGranted = false))
    }

    override fun onDestroy() {
        stopStreaming()
        super.onDestroy()
    }
}
