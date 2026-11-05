package com.hanxi.ghosterm.bridge

import android.os.Looper
import java.nio.CharBuffer
import java.nio.charset.CodingErrorAction

/** Trusted native package snapshot; same worker as the consumed Session.
 * Stop alone is cross-thread. No finalizer, auto-install or implicit backend. */
class ExtensionRuntime private constructor(private val worker: Thread,
    @Suppress("unused") private val store: ExtensionResourceStore?) : AutoCloseable {
    enum class Phase { CREATED, STARTING, ACTIVE, FAILED, STOPPED, CLOSED }
    enum class Failure { NONE, SCRIPT, MEMORY, CPU, CANCELLED, JOBS, PROTOCOL, IO, EXPIRED }
    data class Module(val name: String, val source: ByteArray, val sha256: String)
    data class Config(val modules: List<Module>, val entry: String, val role: ExtensionSession.Role,
        val memoryBytes: Long = 0, val stackBytes: Long = 0, val cpuMilliseconds: Int = 0,
        val jobsPerStep: Int = 0, val activationMilliseconds: Int = 0)
    data class Info(val phase: Phase, val error: Failure, val cleanupError: Int, val sessionEnd: ExtensionSession.End,
        val sessionError: Int, val disposeError: Boolean, val engineReleased: Boolean,
        val steps: ULong, val jobs: ULong, val moduleBytes: ULong)
    @Volatile private var handle = 0L
    private fun owner() {
        check(Thread.currentThread() === worker && Looper.myLooper() != Looper.getMainLooper() && handle != 0L) {
            "运行时已释放或不在创建它的非 UI worker"
        }
    }
    fun step(): Phase { owner(); return Phase.entries[NativeExtensionRuntime.step(handle)] }
    fun info(): Info {
        owner(); val v = NativeExtensionRuntime.info(handle); check(v.size == 10)
        return Info(Phase.entries[v[0].toInt()], Failure.entries[v[1].toInt()], v[2].toInt(),
            ExtensionSession.End.entries[v[3].toInt()], v[4].toInt(), v[5] != 0L, v[6] != 0L,
            v[7].toULong(), v[8].toULong(), v[9].toULong())
    }
    fun stop() { val id = handle; if (id != 0L) NativeExtensionRuntime.stop(id) }
    fun closeStep(): Boolean {
        if (handle == 0L) return true
        owner(); return when (NativeExtensionRuntime.close(handle)) {
            0 -> false
            1 -> { handle = 0L; true }
            else -> throw ExtensionChannelException(3, "运行时关闭错误，保留 owner")
        }
    }
    override fun close() { if (handle == 0L) return; owner(); repeat(32) { if (closeStep()) return }
        throw ExtensionChannelException(3, "运行时仍在释放，请由原 worker 重试") }
    companion object {
        fun supportStatus(): Int = NativeExtensionRuntime.support()
        private fun encode(value: String, limit: Int): ByteArray {
            require('\u0000' !in value)
            val buffer = Charsets.UTF_8.newEncoder().onMalformedInput(CodingErrorAction.REPORT)
                .onUnmappableCharacter(CodingErrorAction.REPORT).encode(CharBuffer.wrap(value))
            require(buffer.remaining() <= limit)
            return ByteArray(buffer.remaining()).also { buffer.get(it) }
        }
        fun adopt(session: ExtensionSession, config: Config): ExtensionRuntime {
            val worker = session.runtimeWorker()
            require(config.modules.isNotEmpty() && config.modules.size <= 64)
            require(config.memoryBytes >= 0 && config.stackBytes >= 0 && config.cpuMilliseconds >= 0 && config.jobsPerStep >= 0 && config.activationMilliseconds >= 0)
            val modules = config.modules.toList(); var total = 0
            val names = modules.map { encode(it.name, 255) }.toTypedArray()
            val sources = modules.map {
                require(it.source.isNotEmpty() && it.source.size <= 131072 && total <= 1048576 - it.source.size)
                total += it.source.size; it.source.copyOf()
            }.toTypedArray()
            val digests = modules.map { require(it.sha256.matches(Regex("[a-f0-9]{64}"))); encode(it.sha256, 64) }.toTypedArray()
            val entry = encode(config.entry, 255)
            val limits = longArrayOf(config.role.ordinal.toLong(), config.memoryBytes, config.stackBytes,
                config.cpuMilliseconds.toLong(), config.jobsPerStep.toLong(), config.activationMilliseconds.toLong())
            // Allocate Java owner and all arguments before Core takes Session.
            val result = ExtensionRuntime(worker, session.runtimeStore())
            val values = session.adoptRuntime { NativeExtensionRuntime.open(it, names, sources, digests, entry, limits) }
            check(values.size == 2)
            if (values[0] != 0L) throw ExtensionChannelException(values[0].toInt(), "运行时未接管会话")
            check(values[1] > 0); result.handle = values[1]; return result
        }
    }
}
internal object NativeExtensionRuntime {
    init { System.loadLibrary("ghosterm_android") }
    external fun support(): Int
    external fun open(session: Long, names: Array<ByteArray>, sources: Array<ByteArray>, digests: Array<ByteArray>, entry: ByteArray, limits: LongArray): LongArray
    external fun step(handle: Long): Int
    external fun info(handle: Long): LongArray
    external fun stop(handle: Long)
    external fun close(handle: Long): Int
}
