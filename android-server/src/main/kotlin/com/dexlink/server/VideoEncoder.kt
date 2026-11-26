package com.dexlink.server

import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaCodecList
import android.media.MediaFormat
import android.os.Build
import android.os.Bundle
import android.os.Process
import android.os.SystemClock
import android.view.Surface
import com.dexlink.protocol.PacketProtocol
import java.io.OutputStream
import java.nio.ByteBuffer
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicReference

class VideoEncoder(
    private val width: Int,
    private val height: Int,
    private val fps: Int,
    private val bitrateBps: Int,
    private val codecType: Int,
    private val sendLock: Any,
    private val outputStream: OutputStream
) {
    private val mimeType = if (codecType == PacketProtocol.CODEC_HEVC) {
        MediaFormat.MIMETYPE_VIDEO_HEVC
    } else {
        MediaFormat.MIMETYPE_VIDEO_AVC
    }

    private var codec: MediaCodec? = null
    var inputSurface: Surface? = null
        private set

    private val running = AtomicBoolean(false)
    private var encoderThread: Thread? = null
    private val cachedConfigNal = AtomicReference<ByteArray?>(null)

    @Volatile
    var currentFps: Float = 0.0f
        private set

    fun prepare(): Surface {
        val encoder = createPreferredHardwareEncoder(mimeType)
        configureWithLowLatencyFallback(encoder)
        val surface = encoder.createInputSurface()
        encoder.start()
        codec = encoder
        inputSurface = surface
        return surface
    }

    fun startStreaming() {
        if (running.getAndSet(true)) return
        encoderThread = Thread({
            Process.setThreadPriority(Process.THREAD_PRIORITY_URGENT_DISPLAY)
            drainLoop()
        }, "dexlink-venc").apply {
            priority = Thread.MAX_PRIORITY
            start()
        }
    }

    private fun createPreferredHardwareEncoder(mime: String): MediaCodec {
        val codecList = MediaCodecList(MediaCodecList.REGULAR_CODECS)
        var swFallbackName: String? = null
        for (info in codecList.codecInfos) {
            if (!info.isEncoder) continue
            if (!info.supportedTypes.any { it.equals(mime, ignoreCase = true) }) continue
            val name = info.name
            val isHw = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                info.isHardwareAccelerated
            } else {
                !name.startsWith("OMX.google.", ignoreCase = true) &&
                    !name.startsWith("c2.android.", ignoreCase = true)
            }
            if (isHw) {
                return MediaCodec.createByCodecName(name)
            } else if (swFallbackName == null) {
                swFallbackName = name
            }
        }
        return if (swFallbackName != null) {
            MediaCodec.createByCodecName(swFallbackName)
        } else {
            MediaCodec.createEncoderByType(mime)
        }
    }

    private fun buildFormat(
        includeLatency: Boolean,
        includeOperatingRate: Boolean,
        includePriority: Boolean,
        includeCbr: Boolean
    ): MediaFormat {
        val format = MediaFormat.createVideoFormat(mimeType, width, height)
        format.setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface)
        format.setInteger(MediaFormat.KEY_BIT_RATE, bitrateBps)
        format.setInteger(MediaFormat.KEY_FRAME_RATE, fps)
        format.setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 1)
        format.setInteger(MediaFormat.KEY_MAX_B_FRAMES, 0)
        format.setLong(MediaFormat.KEY_REPEAT_PREVIOUS_FRAME_AFTER, 100_000L)
        if (includeCbr) {
            format.setInteger(MediaFormat.KEY_BITRATE_MODE, MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR)
        }
        if (includeLatency) {
            format.setInteger(MediaFormat.KEY_LATENCY, 0)
        }
        if (includePriority) {
            format.setInteger(MediaFormat.KEY_PRIORITY, 0)
        }
        if (includeOperatingRate) {
            format.setInteger(MediaFormat.KEY_OPERATING_RATE, fps)
        }
        return format
    }

    private fun configureWithLowLatencyFallback(encoder: MediaCodec) {
        val configs = listOf(
            buildFormat(includeLatency = true, includeOperatingRate = true, includePriority = true, includeCbr = true),
            buildFormat(includeLatency = false, includeOperatingRate = true, includePriority = true, includeCbr = true),
            buildFormat(includeLatency = false, includeOperatingRate = false, includePriority = true, includeCbr = true),
            buildFormat(includeLatency = false, includeOperatingRate = false, includePriority = false, includeCbr = false)
        )
        var lastException: Exception? = null
        for (fmt in configs) {
            try {
                encoder.configure(fmt, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
                return
            } catch (e: Exception) {
                lastException = e
                runCatching { encoder.reset() }
            }
        }
        throw IllegalStateException("MediaCodec configure failed for $mimeType ${width}x${height}", lastException)
    }

    private fun drainLoop() {
        val encoder = codec ?: return
        val info = MediaCodec.BufferInfo()
        val headerScratch = ByteArray(PacketProtocol.HEADER_SIZE)
        var scratchBuf = ByteArray(256 * 1024)
        var fpsFrames = 0
        var fpsWindowStart = SystemClock.uptimeMillis()

        val codecFlag = if (codecType == PacketProtocol.CODEC_HEVC) {
            PacketProtocol.VIDEO_FLAG_HEVC
        } else {
            PacketProtocol.VIDEO_FLAG_NONE
        }

        while (running.get()) {
            val idx = try {
                encoder.dequeueOutputBuffer(info, 10_000L)
            } catch (e: Exception) {
                if (running.get()) {
                    System.err.println("[VideoEncoder] dequeue error: ${e.message}")
                }
                break
            }

            if (idx == MediaCodec.INFO_TRY_AGAIN_LATER) {
                val now = SystemClock.uptimeMillis()
                if (now - fpsWindowStart >= 1000L) {
                    currentFps = fpsFrames * 1000.0f / (now - fpsWindowStart).coerceAtLeast(1L)
                    fpsFrames = 0
                    fpsWindowStart = now
                }
                continue
            }
            if (idx == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                continue
            }
            if (idx < 0) continue

            try {
                val outBuf: ByteBuffer? = encoder.getOutputBuffer(idx)
                if (outBuf != null && info.size > 0) {
                    outBuf.position(info.offset)
                    outBuf.limit(info.offset + info.size)

                    val isConfig = (info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG) != 0
                    val isKey = (info.flags and MediaCodec.BUFFER_FLAG_KEY_FRAME) != 0

                    if (isConfig) {
                        val cfg = ByteArray(info.size)
                        outBuf.get(cfg)
                        cachedConfigNal.set(cfg)
                        val pktFlags = codecFlag or PacketProtocol.VIDEO_FLAG_CONFIG
                        synchronized(sendLock) {
                            PacketProtocol.writePacket(
                                outputStream,
                                headerScratch,
                                PacketProtocol.TYPE_VIDEO_NAL,
                                pktFlags,
                                SystemClock.uptimeMillis(),
                                cfg
                            )
                        }
                    } else {
                        val cfg = if (isKey) cachedConfigNal.get() else null
                        val totalLen = (cfg?.size ?: 0) + info.size
                        if (scratchBuf.size < totalLen) {
                            scratchBuf = ByteArray(totalLen)
                        }
                        var pos = 0
                        if (cfg != null) {
                            System.arraycopy(cfg, 0, scratchBuf, 0, cfg.size)
                            pos += cfg.size
                        }
                        outBuf.get(scratchBuf, pos, info.size)

                        var pktFlags = codecFlag
                        if (isKey) pktFlags = pktFlags or PacketProtocol.VIDEO_FLAG_KEYFRAME

                        synchronized(sendLock) {
                            PacketProtocol.writePacket(
                                outputStream,
                                headerScratch,
                                PacketProtocol.TYPE_VIDEO_NAL,
                                pktFlags,
                                SystemClock.uptimeMillis(),
                                scratchBuf,
                                0,
                                totalLen
                            )
                        }

                        fpsFrames++
                        val now = SystemClock.uptimeMillis()
                        if (now - fpsWindowStart >= 1000L) {
                            currentFps = fpsFrames * 1000.0f / (now - fpsWindowStart).coerceAtLeast(1L)
                            fpsFrames = 0
                            fpsWindowStart = now
                        }
                    }
                }
            } catch (e: Exception) {
                if (running.get()) {
                    System.err.println("[VideoEncoder] send error: ${e.message}")
                }
                break
            } finally {
                runCatching { encoder.releaseOutputBuffer(idx, false) }
            }
        }
    }

    fun requestIdr() {
        val encoder = codec ?: return
        runCatching {
            val b = Bundle()
            b.putInt(MediaCodec.PARAMETER_KEY_REQUEST_SYNC_FRAME, 0)
            encoder.setParameters(b)
        }
    }

    fun updateBitrate(newBitrateBps: Int) {
        val encoder = codec ?: return
        runCatching {
            val b = Bundle()
            b.putInt(MediaCodec.PARAMETER_KEY_VIDEO_BITRATE, newBitrateBps)
            encoder.setParameters(b)
        }
    }

    fun stop() {
        running.set(false)
        runCatching { encoderThread?.join(500) }
        encoderThread = null
        runCatching { codec?.signalEndOfInputStream() }
        runCatching { codec?.stop() }
        runCatching { codec?.release() }
        codec = null
        runCatching { inputSurface?.release() }
        inputSurface = null
    }
}
