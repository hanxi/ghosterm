package com.hanxi.ghosterm.bridge

import java.nio.ByteBuffer
import java.nio.charset.CodingErrorAction

/** Read-only, source-preserving records. Recognized does not mean trusted. */
object KnownHostsBridge {
    init { System.loadLibrary("ghosterm_android") }
    fun records(bytes: ByteArray): List<KnownHostRecord> {
        require(bytes.size <= 1024 * 1024) { "主机验证记录不能超过 1 MiB" }
        Charsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT)
            .onUnmappableCharacter(CodingErrorAction.REPORT).decode(ByteBuffer.wrap(bytes))
        return recordBytes(bytes).map { it.toString(Charsets.UTF_8) }
            .also { check(it.size % 9 == 0) { "主机验证记录不完整" } }
            .chunked(9).map { KnownHostRecord(it[0].toInt(), it[1].toInt(), it[2].toInt(),
                it[3], it[4], it[5], it[6], it[7] == "1", it[8] == "1") }
    }
    private external fun recordBytes(bytes: ByteArray): Array<ByteArray>

    /** Must run off the UI thread. A failure can still mean the rename committed. */
    fun remove(path: String, expected: ByteArray, index: Int): KnownHostsUpdate {
        require(index >= 0 && '\u0000' !in path && expected.size <= 1024 * 1024)
        val fields = removeBytes(path.toByteArray(Charsets.UTF_8), expected, index)
            .map { it.toString(Charsets.UTF_8) }
        check(fields.size == 4) { "主机验证记录写入结果不完整" }
        return KnownHostsUpdate(fields[0].toInt(), fields[1] == "1", fields[2] == "1", fields[3])
    }
    private external fun removeBytes(path: ByteArray, expected: ByteArray, index: Int): Array<ByteArray>
}

data class KnownHostsUpdate(val status: Int, val committed: Boolean, val conflict: Boolean, val message: String) {
    val succeeded: Boolean get() = status == 0
}

data class KnownHostRecord(val line: Int, val byteStart: Int, val byteEnd: Int,
    val marker: String, val hosts: String, val keyType: String, val keyBase64: String,
    val recognized: Boolean, val hashed: Boolean)
