package com.hanxi.ghosterm.bridge

import android.os.Looper

class ExtensionChannelException(val status: Int, message: String) : IllegalStateException("$message · Core $status")

/** Native plugin transport. Construct and drive on one non-main worker. Bytes
 * belong to that worker's protocol decoder, never a UI/event queue. closeStep
 * retains this owner until actual release; no finalizer or automatic reconnect. */
class ExtensionChannel private constructor(private val worker: Thread, private val hops: List<Hop>) : AutoCloseable {
    enum class Decision(val nativeValue: Int) { ACCEPT(0), REJECT(1), UNKNOWN(2) }
    data class Hop(val host: String, val port: Int, val user: String, val knownHosts: String? = null,
                   val viaHost: String? = null, val timeoutMilliseconds: Int = 60000)
    data class IO(val state: Int, val count: Int, val error: Int) {
        val data: Boolean get() = state == 0
        val wait: Boolean get() = state == 1
        val eof: Boolean get() = state == 2
    }
    data class Authentication(val phase: Int, val error: Int, val host: Int, val deadline: Boolean,
                              val methods: Int, val decision: Int, val keyType: String, val fingerprint: String)
    data class Info(val phase: Int, val stage: Int, val failureStage: Int, val error: Int, val cleanupError: Int, val hop: Int,
                    val setupExpired: Boolean, val challenge: Long, val authentication: List<Authentication>,
                    val accepted: Long, val sent: Long, val received: Long, val stderrBytes: Long,
                    val buffered: Int, val remoteEof: Boolean, val exitStatus: Int?,
                    val diagnosticTruncated: Boolean, val diagnostic: ByteArray)
    @Volatile private var handle = 0L
    private fun owner() {
        check(Thread.currentThread() === worker && Looper.myLooper() != Looper.getMainLooper()) {
            "插件通道必须在创建它的非 UI worker 中调用"
        }
        check(handle != 0L) { "插件通道已释放" }
    }
    fun step(milliseconds: Int = 0): Int { owner(); require(milliseconds >= 0); return NativeExtensionChannel.step(handle, milliseconds) }
    fun info(): Info {
        owner(); val fields = NativeExtensionChannel.info(handle)
        val values = fields[0] as LongArray
        @Suppress("UNCHECKED_CAST") val texts = fields[1] as Array<ByteArray>
        val count = values[5].toInt()
        check(count in 0..8 && values.size == 18 + count * 6 && texts.size == count * 2)
        val auth = (0 until count).map { h ->
            val at = 18 + h * 6
            Authentication(values[at].toInt(), values[at + 1].toInt(), values[at + 2].toInt(), values[at + 3] != 0L,
                values[at + 4].toInt(), values[at + 5].toInt(), texts[h * 2].toString(Charsets.UTF_8), texts[h * 2 + 1].toString(Charsets.UTF_8))
        }
        return Info(values[0].toInt(), values[1].toInt(), values[2].toInt(), values[3].toInt(), values[17].toInt(), values[4].toInt(),
            values[6] != 0L, values[7], auth, values[8], values[9], values[10], values[11], values[12].toInt(),
            values[13] != 0L, if (values[14] != 0L) values[15].toInt() else null, values[16] != 0L, fields[2] as ByteArray)
    }
    fun write(bytes: ByteArray, offset: Int = 0, count: Int = bytes.size - offset): IO {
        owner(); require(offset >= 0 && count > 0 && offset <= bytes.size - count)
        val result = NativeExtensionChannel.write(handle, bytes, offset, count)
        return IO(result[0], result[1], result[2])
    }
    fun read(capacity: Int = 16384, milliseconds: Int = 0): Pair<IO, ByteArray> {
        owner(); require(capacity in 1..16384 && milliseconds >= 0)
        val result = NativeExtensionChannel.read(handle, capacity, milliseconds)
        val values = result[0] as IntArray; val io = IO(values[0], values[1], values[2])
        check(io.count in 0..capacity)
        return io to (result[1] as ByteArray).copyOf(io.count)
    }
    fun decide(hop: Int, challenge: Long, decision: Decision): Int {
        owner(); return NativeExtensionChannel.decide(handle, hop, challenge, decision.nativeValue)
    }
    /** Caller supplies unlocked material only after the native challenge. Byte
     * arrays are erased on return (String secrets are deliberately not used). */
    fun supply(hop: Int, challenge: Long, kind: Int, first: ByteArray, second: ByteArray = byteArrayOf()): Int {
        try {
            owner()
            require(hop in hops.indices && kind in 1..2)
            val entry = hops[hop]
            return NativeExtensionChannel.supply(handle, hop, challenge, entry.host.toByteArray(), entry.port,
                entry.user.toByteArray(), (entry.knownHosts ?: "").toByteArray(), kind, first, second)
        } finally { first.fill(0); second.fill(0) }
    }
    /** Cross-thread stop is generation checked and does not free the owner. */
    fun stop() { val id = handle; if (id != 0L) NativeExtensionChannel.stop(id) }
    fun closeStep(milliseconds: Int = 0): Boolean {
        owner(); require(milliseconds >= 0)
        return when (NativeExtensionChannel.close(handle, milliseconds)) {
            0 -> false
            1 -> { handle = 0L; true }
            else -> throw ExtensionChannelException(3, "插件通道释放失败，仍保留 owner")
        }
    }
    internal fun adoptSession(role: Int, texts: Array<ByteArray>, required: Array<ByteArray>,
                              available: Array<ByteArray>, numbers: IntArray, methods: List<ExtensionSession.Method>? = null): Long {
        owner()
        val result = if (methods == null) NativeExtensionChannel.openSession(handle, role, texts, required, available, numbers)
            else NativeExtensionChannel.openSessionRpc(handle, role, texts, required, available, numbers,
                methods.map { it.name.toByteArray() }.toTypedArray(), methods.map { it.capability.toByteArray() }.toTypedArray(),
                methods.flatMap { listOf(it.receiver.ordinal, it.inputShape.ordinal, it.outputShape.ordinal) }.toIntArray())
        check(result.size == 2)
        if (result[0] != 0L) throw ExtensionChannelException(result[0].toInt(), "无法接管插件会话")
        check(result[1] > 0L); handle = 0L; return result[1]
    }
    override fun close() {
        if (handle == 0L) return
        owner()
        repeat(32) { if (closeStep()) return }
        throw ExtensionChannelException(3, "插件通道仍在释放，请由 worker 重试")
    }
    companion object {
        private fun worker(): Thread {
            check(Looper.myLooper() != Looper.getMainLooper()) { "插件通道不能在 UI 线程创建" }
            return Thread.currentThread()
        }
        private fun created(channel: ExtensionChannel, values: LongArray): ExtensionChannel {
            check(values.size == 2)
            if (values[0] != 0L) throw ExtensionChannelException(values[0].toInt(), "无法创建插件通道")
            check(values[1] > 0L); channel.handle = values[1]; return channel
        }
        /** Takes exclusive pipe/socket descriptors only on success. */
        fun adoptDescriptors(readFd: Int, writeFd: Int): ExtensionChannel =
            created(ExtensionChannel(worker(), emptyList()), NativeExtensionChannel.openFd(readFd, writeFd))
        /** First hop uses this explicitly resolved address; inner addresses are
         * explicit viaHost strings interpreted by the preceding SSH server. */
        fun ssh(hops: List<Hop>, family: Int, address: ByteArray, scope: Int = 0, bootstrap: String,
                setupMilliseconds: Int = 30000, admissionMilliseconds: Int = 5000,
                stderrLimit: Int = 262144): ExtensionChannel {
            val thread = worker(); val route = hops.toList()
            require(route.size in 1..8 && address.size == 16 && family in listOf(4, 6) && route[0].viaHost == null &&
                route.drop(1).all { !it.viaHost.isNullOrEmpty() } && !bootstrap.contains('\u0000'))
            val channel = ExtensionChannel(thread, route)
            return created(channel, NativeExtensionChannel.openRoute(route.map { it.host.toByteArray() }.toTypedArray(),
                route.map { it.port }.toIntArray(), route.map { it.user.toByteArray() }.toTypedArray(),
                route.map { (it.knownHosts ?: "").toByteArray() }.toTypedArray(), route.map { (it.viaHost ?: "").toByteArray() }.toTypedArray(),
                address, family, scope, route.map { it.timeoutMilliseconds }.toIntArray(), bootstrap.toByteArray(),
                setupMilliseconds, admissionMilliseconds, stderrLimit))
        }
    }
}
internal object NativeExtensionChannel {
    init { System.loadLibrary("ghosterm_android") }
    external fun openFd(readFd: Int, writeFd: Int): LongArray
    external fun openRoute(hosts: Array<ByteArray>, ports: IntArray, users: Array<ByteArray>, known: Array<ByteArray>,
        via: Array<ByteArray>, address: ByteArray, family: Int, scope: Int, timeouts: IntArray,
        command: ByteArray, setup: Int, admission: Int, stderrLimit: Int): LongArray
    external fun step(handle: Long, milliseconds: Int): Int
    external fun info(handle: Long): Array<Any>
    external fun read(handle: Long, capacity: Int, milliseconds: Int): Array<Any>
    external fun write(handle: Long, bytes: ByteArray, offset: Int, count: Int): IntArray
    external fun decide(handle: Long, hop: Int, challenge: Long, decision: Int): Int
    external fun supply(handle: Long, hop: Int, challenge: Long, host: ByteArray, port: Int, user: ByteArray,
        known: ByteArray, kind: Int, first: ByteArray, second: ByteArray): Int
    external fun stop(handle: Long)
    external fun close(handle: Long, milliseconds: Int): Int
    external fun openSession(handle: Long, role: Int, texts: Array<ByteArray>, required: Array<ByteArray>,
        available: Array<ByteArray>, numbers: IntArray): LongArray
    external fun openSessionRpc(handle: Long, role: Int, texts: Array<ByteArray>, required: Array<ByteArray>,
        available: Array<ByteArray>, numbers: IntArray, names: Array<ByteArray>, capabilities: Array<ByteArray>, policies: IntArray): LongArray
}
