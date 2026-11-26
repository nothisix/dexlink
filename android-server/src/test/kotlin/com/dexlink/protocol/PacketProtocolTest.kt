package com.dexlink.protocol

import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFails

class PacketProtocolTest {

    @Test
    fun headerRoundTripAndPayloadDecoding() {
        val hdrScratch = ByteArray(PacketProtocol.HEADER_SIZE)
        val out = ByteArrayOutputStream()
        val cmdBytes = PacketProtocol.encodeControl(
            PacketProtocol.ControlCommand(
                cmd = PacketProtocol.CMD_SET_RES,
                argU8 = PacketProtocol.CODEC_H264,
                argU16 = 120,
                arg0 = 1920,
                arg1 = 1080
            )
        )
        PacketProtocol.writePacket(
            out,
            hdrScratch,
            PacketProtocol.TYPE_CONTROL_CMD,
            0x03,
            0x12345678L,
            cmdBytes
        )

        val bytes = out.toByteArray()
        assertEquals(PacketProtocol.HEADER_SIZE + PacketProtocol.CONTROL_PAYLOAD_SIZE, bytes.size)

        val inp = ByteArrayInputStream(bytes)
        val readHdr = ByteArray(PacketProtocol.HEADER_SIZE)
        PacketProtocol.readFully(inp, readHdr, 0, readHdr.size)
        val hdr = PacketProtocol.decodeHeader(readHdr)
        assertEquals(PacketProtocol.TYPE_CONTROL_CMD, hdr.type)
        assertEquals(0x03, hdr.flags)
        assertEquals(12, hdr.payloadSize)
        assertEquals(0x12345678L, hdr.timestampMs)

        val readPayload = ByteArray(hdr.payloadSize)
        PacketProtocol.readFully(inp, readPayload, 0, readPayload.size)
        val decodedCmd = PacketProtocol.decodeControl(readPayload)
        assertEquals(PacketProtocol.CMD_SET_RES, decodedCmd.cmd)
        assertEquals(120, decodedCmd.argU16)
        assertEquals(1920, decodedCmd.arg0)
        assertEquals(1080, decodedCmd.arg1)
    }

    @Test
    fun rejectsCorruptHeaderType() {
        val bad = ByteArray(PacketProtocol.HEADER_SIZE)
        bad[0] = 0x7F.toByte()
        assertFails { PacketProtocol.decodeHeader(bad) }
    }
}
