package com.dexlink.protocol

import java.io.EOFException
import java.io.IOException
import java.io.InputStream
import java.io.OutputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder

object PacketProtocol {
    const val VIDEO_CONTROL_PORT: Int = 27183
    const val AUDIO_TELEMETRY_PORT: Int = 27184
    const val HEADER_SIZE: Int = 12
    const val MAX_PAYLOAD_SIZE: Int = 16 * 1024 * 1024

    const val TYPE_VIDEO_NAL: Int = 0x01
    const val TYPE_AUDIO_PCM: Int = 0x02
    const val TYPE_TOUCH_EVENT: Int = 0x03
    const val TYPE_KEY_EVENT: Int = 0x04
    const val TYPE_CONTROL_CMD: Int = 0x05
    const val TYPE_TELEMETRY: Int = 0x06

    const val VIDEO_FLAG_NONE: Int = 0x00
    const val VIDEO_FLAG_CONFIG: Int = 0x01
    const val VIDEO_FLAG_KEYFRAME: Int = 0x02
    const val VIDEO_FLAG_HEVC: Int = 0x04

    const val AUDIO_FLAG_PCM_48K_STEREO_S16: Int = 0x01

    const val TELEMETRY_CHARGING: Int = 0x01
    const val TELEMETRY_AUDIO_ACTIVE: Int = 0x02
    const val TELEMETRY_SCREEN_OFF: Int = 0x04
    const val TELEMETRY_DEX_MODE: Int = 0x08
    const val TELEMETRY_THERMAL_WARN: Int = 0x10

    const val TOUCH_DOWN: Int = 0
    const val TOUCH_UP: Int = 1
    const val TOUCH_MOVE: Int = 2
    const val TOUCH_CANCEL: Int = 3
    const val TOUCH_HOVER_MOVE: Int = 4

    const val KEY_DOWN: Int = 0
    const val KEY_UP: Int = 1

    const val CMD_SET_RES: Int = 0x01
    const val CMD_SET_BITRATE: Int = 0x02
    const val CMD_SCREEN_OFF: Int = 0x03
    const val CMD_REQUEST_IDR: Int = 0x04
    const val CMD_SET_MODE: Int = 0x05
    const val CMD_PING: Int = 0x06
    const val CMD_PONG: Int = 0x07
    const val CMD_LAUNCH_APP: Int = 0x08
    const val CMD_NAV_ACTION: Int = 0x09

    const val MODE_MIRROR: Int = 0
    const val MODE_DEX: Int = 1

    const val CODEC_H264: Int = 0
    const val CODEC_HEVC: Int = 1

    const val TOUCH_PAYLOAD_SIZE: Int = 20
    const val KEY_PAYLOAD_SIZE: Int = 12
    const val CONTROL_PAYLOAD_SIZE: Int = 12
    const val TELEMETRY_PAYLOAD_SIZE: Int = 12

    data class PacketHeader(
        val type: Int,
        val flags: Int,
        val reserved: Int,
        val payloadSize: Int,
        val timestampMs: Long
    )

    data class TouchEvent(
        val pointerId: Int,
        val action: Int,
        val normX: Float,
        val normY: Float,
        val pressure: Float
    )

    data class KeyEventPacket(
        val keycode: Int,
        val action: Int,
        val metaState: Int
    )

    data class ControlCommand(
        val cmd: Int,
        val argU8: Int,
        val argU16: Int,
        val arg0: Int,
        val arg1: Int
    )

    data class TelemetryPacket(
        val tempC: Float,
        val encFps: Float,
        val batteryPct: Int,
        val statusFlags: Int,
        val reserved: Int = 0
    )

    fun encodeHeader(
        out: ByteArray,
        type: Int,
        flags: Int,
        reserved: Int,
        payloadSize: Int,
        timestampMs: Long
    ) {
        out[0] = (type and 0xFF).toByte()
        out[1] = (flags and 0xFF).toByte()
        out[2] = (reserved and 0xFF).toByte()
        out[3] = ((reserved ushr 8) and 0xFF).toByte()
        out[4] = (payloadSize and 0xFF).toByte()
        out[5] = ((payloadSize ushr 8) and 0xFF).toByte()
        out[6] = ((payloadSize ushr 16) and 0xFF).toByte()
        out[7] = ((payloadSize ushr 24) and 0xFF).toByte()
        val ts = timestampMs and 0xFFFFFFFFL
        out[8] = (ts and 0xFFL).toByte()
        out[9] = ((ts ushr 8) and 0xFFL).toByte()
        out[10] = ((ts ushr 16) and 0xFFL).toByte()
        out[11] = ((ts ushr 24) and 0xFFL).toByte()
    }

    fun decodeHeader(buf: ByteArray, offset: Int = 0): PacketHeader {
        if (buf.size - offset < HEADER_SIZE) {
            throw IllegalArgumentException("Header buffer underflow")
        }
        val type = buf[offset].toInt() and 0xFF
        val flags = buf[offset + 1].toInt() and 0xFF
        val reserved = (buf[offset + 2].toInt() and 0xFF) or
            ((buf[offset + 3].toInt() and 0xFF) shl 8)
        val payloadSize = (buf[offset + 4].toInt() and 0xFF) or
            ((buf[offset + 5].toInt() and 0xFF) shl 8) or
            ((buf[offset + 6].toInt() and 0xFF) shl 16) or
            ((buf[offset + 7].toInt() and 0xFF) shl 24)
        val ts = (buf[offset + 8].toLong() and 0xFFL) or
            ((buf[offset + 9].toLong() and 0xFFL) shl 8) or
            ((buf[offset + 10].toLong() and 0xFFL) shl 16) or
            ((buf[offset + 11].toLong() and 0xFFL) shl 24)
        if (type < TYPE_VIDEO_NAL || type > TYPE_TELEMETRY) {
            throw IOException("Invalid packet type: $type")
        }
        if (payloadSize < 0 || payloadSize > MAX_PAYLOAD_SIZE) {
            throw IOException("Invalid payload size: $payloadSize")
        }
        return PacketHeader(type, flags, reserved, payloadSize, ts)
    }

    fun decodeTouch(payload: ByteArray): TouchEvent {
        val bb = ByteBuffer.wrap(payload).order(ByteOrder.LITTLE_ENDIAN)
        val pointerId = bb.int
        val action = bb.get().toInt() and 0xFF
        bb.position(8)
        val normX = bb.float
        val normY = bb.float
        val pressure = bb.float
        return TouchEvent(pointerId, action, normX, normY, pressure)
    }

    fun decodeKey(payload: ByteArray): KeyEventPacket {
        val bb = ByteBuffer.wrap(payload).order(ByteOrder.LITTLE_ENDIAN)
        val keycode = bb.int
        val action = bb.get().toInt() and 0xFF
        bb.position(8)
        val metaState = bb.int
        return KeyEventPacket(keycode, action, metaState)
    }

    fun encodeControl(cmd: ControlCommand): ByteArray {
        val bb = ByteBuffer.allocate(CONTROL_PAYLOAD_SIZE).order(ByteOrder.LITTLE_ENDIAN)
        bb.put((cmd.cmd and 0xFF).toByte())
        bb.put((cmd.argU8 and 0xFF).toByte())
        bb.putShort((cmd.argU16 and 0xFFFF).toShort())
        bb.putInt(cmd.arg0)
        bb.putInt(cmd.arg1)
        return bb.array()
    }

    fun decodeControl(payload: ByteArray): ControlCommand {
        val bb = ByteBuffer.wrap(payload).order(ByteOrder.LITTLE_ENDIAN)
        val cmd = bb.get().toInt() and 0xFF
        val argU8 = bb.get().toInt() and 0xFF
        val argU16 = bb.short.toInt() and 0xFFFF
        val arg0 = bb.int
        val arg1 = bb.int
        return ControlCommand(cmd, argU8, argU16, arg0, arg1)
    }

    fun encodeTelemetry(t: TelemetryPacket, dst: ByteArray) {
        val bb = ByteBuffer.wrap(dst, 0, TELEMETRY_PAYLOAD_SIZE).order(ByteOrder.LITTLE_ENDIAN)
        bb.putFloat(t.tempC)
        bb.putFloat(t.encFps)
        bb.put((t.batteryPct and 0xFF).toByte())
        bb.put((t.statusFlags and 0xFF).toByte())
        bb.putShort((t.reserved and 0xFFFF).toShort())
    }

    @Throws(IOException::class)
    fun readFully(input: InputStream, dst: ByteArray, offset: Int, length: Int) {
        var readTotal = 0
        while (readTotal < length) {
            val n = input.read(dst, offset + readTotal, length - readTotal)
            if (n < 0) {
                throw EOFException("Unexpected EOF while reading $length bytes")
            }
            readTotal += n
        }
    }

    @Throws(IOException::class)
    fun writePacket(
        output: OutputStream,
        headerScratch: ByteArray,
        type: Int,
        flags: Int,
        timestampMs: Long,
        payload: ByteArray,
        payloadOffset: Int = 0,
        payloadLength: Int = payload.size,
        reserved: Int = 0
    ) {
        encodeHeader(headerScratch, type, flags, reserved, payloadLength, timestampMs)
        output.write(headerScratch, 0, HEADER_SIZE)
        if (payloadLength > 0) {
            output.write(payload, payloadOffset, payloadLength)
        }
        output.flush()
    }
}
