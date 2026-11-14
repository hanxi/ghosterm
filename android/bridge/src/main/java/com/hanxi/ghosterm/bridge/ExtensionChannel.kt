package com.hanxi.ghosterm.bridge

import android.os.Looper
import java.nio.CharBuffer
import java.nio.charset.CodingErrorAction

class ExtensionChannelException(val status: Int, message: String) : IllegalStateException("$message · Core $status")

/** Native plugin transport. Construct and drive on one non-main worker. Bytes
 * belong to that worker's protocol decoder, never a UI/event queue. closeStep
 * retains this owner until actual release; no finalizer or automatic reconnect. */
class ExtensionChannel private constructor(private val worker: Thread, private val hops: List<Hop>) : AutoCloseable, AuthenticatedSSH {
    data class ProcessConfiguration(val executable: String, val arguments: List<String>, val directoryFd: Int,
        val environment: List<String> = emptyList(), val executionAuthorized: Boolean,
        val lifetimeMilliseconds: Int = 0, val stopGraceMilliseconds: Int = 0,
        val stdoutLimit: Long = 0, val stderrLimit: Long = 0)
    enum class ProcessReason { NONE, DEADLINE, STDOUT_LIMIT, STDERR_LIMIT }
    data class ProcessObservation(val error: Int, val cleanupError: Int, val groupSignalError: Int,
        val reason: ProcessReason, val childExited: Boolean, val reaped: Boolean, val groupFenced: Boolean,
        val stdoutEof: Boolean, val stderrEof: Boolean, val stopping: Boolean,
        val exitCode: Int?, val termSignal: Int?, val stdoutBytes: ULong, val stderrBytes: ULong,
        val diagnosticTruncated: Boolean, val diagnostic: ByteArray)
    enum class Stage(val nativeValue: Int) { NONE(0), AUTHENTICATION(1), FORWARD(2), EXEC(3), STREAM(4) }
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
    private var finalProcessObservation: Array<Any>? = null
    fun processObservation(): ProcessObservation {
        check(Thread.currentThread() === worker && Looper.myLooper() != Looper.getMainLooper())
        val snapshot = if (handle == 0L) checkNotNull(finalProcessObservation) { "插件通道已转交或释放" }
            else NativeExtensionChannel.processObservation(handle)
        return decodeProcessObservation(snapshot)
    }
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
    /** Presents the current route-authentication challenge through the same
     * credential UI contract as ordinary SSH sessions. The challenge token
     * and this channel identity are rechecked on every response. */
    override fun authentication(): SSHAuthState {
        owner()
        val current = info()
        val hop = current.hop
        val auth = current.authentication.getOrNull(hop)
            ?: return SSHAuthState(handle, 0, SSHAuthPhase.FAILED, current.error, SSHHostState.UNCHECKED,
                current.setupExpired, 0, "", "", this, hop)
        return SSHAuthState(handle, current.challenge,
            SSHAuthPhase.entries.getOrElse(auth.phase) { SSHAuthPhase.FAILED }, auth.error,
            SSHHostState.entries.getOrElse(auth.host) { SSHHostState.UNCHECKED }, auth.deadline,
            auth.methods, auth.keyType, auth.fingerprint, this, hop)
    }
    override fun decide(prompt: SSHAuthState, accept: Boolean) {
        checkPrompt(prompt, SSHAuthPhase.HOSTKEY)
        val status = decide(prompt.hop, prompt.request, if (accept) Decision.ACCEPT else Decision.REJECT)
        val accepted = status == 0 || (!accept && status == 5) // A Core DENIED result is the expected explicit rejection.
        check(accepted) { "SSH 主机确认已失效（Core $status）" }
    }
    override fun password(prompt: SSHAuthState, password: ByteArray) {
        try {
            checkPrompt(prompt, SSHAuthPhase.CREDENTIALS)
            val status = supply(prompt.hop, prompt.request, 1, password)
            check(status == 0) { "SSH 密码请求已失效（Core $status）" }
        } finally { password.fill(0) }
    }
    override fun privateKey(prompt: SSHAuthState, pem: ByteArray, passphrase: ByteArray) {
        try {
            checkPrompt(prompt, SSHAuthPhase.CREDENTIALS)
            val status = supply(prompt.hop, prompt.request, 2, pem, passphrase)
            check(status == 0) { "SSH 私钥请求已失效（Core $status）" }
        } finally { pem.fill(0); passphrase.fill(0) }
    }
    private fun checkPrompt(prompt: SSHAuthState, phase: SSHAuthPhase) {
        owner()
        check(prompt.owner == handle && prompt.scope === this && prompt.request > 0 && prompt.hop in hops.indices) {
            "SSH 认证请求不属于此插件通道"
        }
        val current = info()
        check(current.challenge == prompt.request && current.hop == prompt.hop &&
            current.authentication.getOrNull(prompt.hop)?.phase == phase.ordinal) {
            "SSH 认证请求已失效"
        }
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
        val observation = arrayOfNulls<Any>(1)
        return when (NativeExtensionChannel.close(handle, milliseconds, observation)) {
            0 -> false
            1 -> { handle = 0L
                @Suppress("UNCHECKED_CAST")
                val snapshot = observation[0] as Array<Any>?
                finalProcessObservation = snapshot; true }
            else -> throw ExtensionChannelException(3, "插件通道释放失败，仍保留 owner")
        }
    }
    internal fun adoptSession(role: Int, texts: Array<ByteArray>, required: Array<ByteArray>,
                              available: Array<ByteArray>, numbers: IntArray, methods: List<ExtensionSession.Method>? = null, store: Long = 0, scopeBytes: Long = 0): Long {
        owner()
        val result = if (store != 0L) NativeExtensionChannel.openSessionResources(handle, role, texts, required, available, numbers,
            (methods ?: emptyList()).map { it.name.toByteArray() }.toTypedArray(), (methods ?: emptyList()).map { it.capability.toByteArray() }.toTypedArray(),
            (methods ?: emptyList()).flatMap { listOf(it.receiver.ordinal, it.inputShape.ordinal, it.outputShape.ordinal) }.toIntArray(), store, scopeBytes)
            else if (methods == null) NativeExtensionChannel.openSession(handle, role, texts, required, available, numbers)
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
        internal fun decodeProcessObservation(fields: Array<Any>): ProcessObservation {
            check(fields.size == 2)
            val v = fields[0] as LongArray; val diagnostic = fields[1] as ByteArray
            check(v.size == 16 && diagnostic.size <= 4096)
            if (v[0] != 0L) throw ExtensionChannelException(v[0].toInt(), "无法观察本机进程通道")
            check(v[4] in 0L..3L)
            val exited = v[5] != 0L; val signalled = v[12] != 0L
            return ProcessObservation(v[1].toInt(), v[2].toInt(), v[3].toInt(), ProcessReason.entries[v[4].toInt()],
                exited, v[6] != 0L, v[7] != 0L, v[8] != 0L, v[9] != 0L, v[10] != 0L,
                if (exited && !signalled) v[11].toInt() else null, if (exited && signalled) v[12].toInt() else null,
                v[13].toULong(), v[14].toULong(), v[15] != 0L, diagnostic.copyOf())
        }
        /** Explicit trusted Native launch. No shell/PATH/PTY or ambient environment. */
        fun process(config: ProcessConfiguration): ExtensionChannel {
            val thread = worker()
            require(config.executable.startsWith("/") && config.arguments.size in 1..64 && config.environment.size <= 32)
            require(config.lifetimeMilliseconds in 0..3600000 && config.stopGraceMilliseconds in 0..5000 &&
                config.stdoutLimit in 0L..67108864L && config.stderrLimit in 0L..1048576L)
            fun encode(value: String): ByteArray {
                require('\u0000' !in value)
                val buffer = Charsets.UTF_8.newEncoder().onMalformedInput(CodingErrorAction.REPORT)
                    .onUnmappableCharacter(CodingErrorAction.REPORT).encode(CharBuffer.wrap(value))
                require(buffer.remaining() <= 4096)
                return ByteArray(buffer.remaining()).also { buffer.get(it) }
            }
            val channel = ExtensionChannel(thread, emptyList())
            return created(channel, NativeExtensionChannel.openProcess(encode(config.executable),
                config.arguments.map(::encode).toTypedArray(), config.directoryFd,
                config.environment.map(::encode).toTypedArray(), config.executionAuthorized,
                config.lifetimeMilliseconds, config.stopGraceMilliseconds, config.stdoutLimit, config.stderrLimit))
        }
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
    external fun openProcess(executable: ByteArray, arguments: Array<ByteArray>, directoryFd: Int,
        environment: Array<ByteArray>, authorized: Boolean, lifetime: Int, grace: Int, stdoutLimit: Long, stderrLimit: Long): LongArray
    external fun processObservation(handle: Long): Array<Any>
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
    external fun close(handle: Long, milliseconds: Int, observation: Array<Any?>? = null): Int
    external fun openSession(handle: Long, role: Int, texts: Array<ByteArray>, required: Array<ByteArray>,
        available: Array<ByteArray>, numbers: IntArray): LongArray
    external fun openSessionRpc(handle: Long, role: Int, texts: Array<ByteArray>, required: Array<ByteArray>,
        available: Array<ByteArray>, numbers: IntArray, names: Array<ByteArray>, capabilities: Array<ByteArray>, policies: IntArray): LongArray
    external fun openSessionResources(handle: Long, role: Int, texts: Array<ByteArray>, required: Array<ByteArray>,
        available: Array<ByteArray>, numbers: IntArray, names: Array<ByteArray>, capabilities: Array<ByteArray>, policies: IntArray,
        store: Long, scopeBytes: Long): LongArray

}
