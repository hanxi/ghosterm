package com.hanxi.ghosterm.bridge

import android.os.Looper
import java.nio.CharBuffer
import java.nio.charset.CodingErrorAction

/** Fixed bytes on one worker; stop alone is cross-thread. No installation or consent. */
class ExtensionPackage private constructor(private val worker: Thread) : AutoCloseable {
    enum class Phase { CAPTURING, CAPTURED, CHECKED, FAILED, CLOSED }
    enum class Failure { NONE, INVALID, UNKNOWN, UNSUPPORTED, DENIED, LIMIT, MEMORY, CANCELLED, IO, INTERNAL, CHANGED }
    data class Info(val phase: Phase, val error: Failure, val code: String, val members: ULong, val bytes: ULong)
    @Volatile private var handle = 0L
    internal fun pointer(): Long {
        check(Thread.currentThread() === worker && Looper.myLooper() != Looper.getMainLooper() && handle != 0L) {
            "插件包已释放或不在原非 UI worker"
        }
        return handle
    }
    fun step(): Phase = Phase.entries[NativeExtensionPackage.step(pointer())]
    fun check(hostJSON: String?, registryJSON: String? = null, memoryBytes: Long = 0, stackBytes: Long = 0, cpuMilliseconds: Int = 0): Phase {
        val id = pointer(); require(memoryBytes >= 0 && stackBytes >= 0 && cpuMilliseconds >= 0)
        return Phase.entries[NativeExtensionPackage.check(id, hostJSON?.let { encode(it, 65536) }, registryJSON?.let { encode(it, 1048576) }, memoryBytes, stackBytes, cpuMilliseconds)]
    }
    fun info(): Info {
        val id = pointer(); val v = NativeExtensionPackage.info(id); check(v.size == 4)
        return Info(Phase.entries[v[0].toInt()], Failure.entries[v[1].toInt()], NativeExtensionPackage.code(id).toString(Charsets.UTF_8), v[2].toULong(), v[3].toULong())
    }
    data class Metadata(val id: String, val version: String, val publisher: String, val license: String,
        val entry: String, val runtime: String, val variant: String, val sdkVersion: String,
        val packageDigest: String, val manifestDigest: String, val requires: List<String>,
        val optionalMissing: List<String>, val protocols: List<Int>, val executor: Boolean,
        val associationRequired: Boolean, val authorizationGranted: Boolean, val publisherAuthenticated: Boolean)
    fun metadata(): Metadata {
        val id = pointer(); val text = NativeExtensionPackage.metadata(id).map { it.toString(Charsets.UTF_8) }
        val n = NativeExtensionPackage.metadataNumbers(id); check(text.size == 42 && n.size == 15)
        check(n[0] in 0..16 && n[1] in 0..16 && n[2] in 0..8)
        return Metadata(text[0], text[1], text[2], text[3], text[4], text[5], text[6], text[7], text[8], text[9],
            text.subList(10, 10 + n[0].toInt()), text.subList(26, 26 + n[1].toInt()),
            (0 until n[2].toInt()).map { n[7 + it].toInt() }, n[3] != 0L, n[4] != 0L, n[5] != 0L, n[6] != 0L)
    }
    fun stop() { val id = handle; if (id != 0L) NativeExtensionPackage.stop(id) }
    fun closeStep(): Boolean {
        if (handle == 0L) return true
        val status = NativeExtensionPackage.close(pointer()); if (status == -1) return false
        handle = 0L
        if (status != 0) throw ExtensionChannelException(status, "插件包关闭错误，引用已释放")
        return true
    }
    override fun close() { repeat(32) { if (closeStep()) return }; throw ExtensionChannelException(3, "插件包仍被取消调用占用，请由原 worker 重试") }
    companion object {
        private fun encode(value: String, limit: Int): ByteArray {
            require('\u0000' !in value)
            val bytes = Charsets.UTF_8.newEncoder().onMalformedInput(CodingErrorAction.REPORT).onUnmappableCharacter(CodingErrorAction.REPORT).encode(CharBuffer.wrap(value))
            require(bytes.remaining() <= limit); return ByteArray(bytes.remaining()).also { bytes.get(it) }
        }
        fun capture(directoryFd: Int, members: Int = 1024, entries: Int = 4096, memberBytes: Long = 16777216, totalBytes: Long = 67108864): ExtensionPackage {
            check(Looper.myLooper() != Looper.getMainLooper()); require(members > 0 && entries > 0 && memberBytes > 0 && totalBytes > 0)
            val result = ExtensionPackage(Thread.currentThread())
            val v = NativeExtensionPackage.open(directoryFd, members, entries, memberBytes, totalBytes); check(v.size == 2)
            if (v[0] != 0L) throw ExtensionChannelException(v[0].toInt(), "无法采集插件包")
            check(v[1] > 0); result.handle = v[1]; return result
        }
    }
}
internal object NativeExtensionPackage {
    init { System.loadLibrary("ghosterm_android") }
    external fun open(fd: Int, members: Int, entries: Int, memberBytes: Long, totalBytes: Long): LongArray
    external fun step(handle: Long): Int
    external fun check(handle: Long, host: ByteArray?, registry: ByteArray?, memory: Long, stack: Long, cpu: Int): Int
    external fun info(handle: Long): LongArray
    external fun metadata(handle: Long): Array<ByteArray>
    external fun metadataNumbers(handle: Long): LongArray
    external fun code(handle: Long): ByteArray
    external fun stop(handle: Long)
    external fun close(handle: Long): Int
}
