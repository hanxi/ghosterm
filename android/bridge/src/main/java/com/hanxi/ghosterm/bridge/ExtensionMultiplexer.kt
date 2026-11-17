package com.hanxi.ghosterm.bridge

import android.os.Looper

/** Physical owner for explicitly registered logical channels. No finalizer,
 * automatic connection, package approval or protocol bytes on the UI thread. */
class ExtensionMultiplexer private constructor(private val worker: Thread) : AutoCloseable {
    data class Info(val error: Int, val cleanupError: Int, val instances: Int,
        val received: ULong, val sent: ULong, val rejected: ULong, val stopping: Boolean, val released: Boolean)
    data class ScopeInfo(val connection: String, val instance: String, val epoch: ULong,
        val error: Int, val closeError: Int, val queuedFrames: Int,
        val received: ULong, val sent: ULong, val rejected: ULong,
        val closing: Boolean, val closeSent: Boolean, val released: Boolean)
    @Volatile private var handle = 0L
    private var finalProcessObservation: Array<Any>? = null
    private fun owner() {
        check(Thread.currentThread() === worker && Looper.myLooper() != Looper.getMainLooper()) {
            "多实例连接必须由创建它的非 UI worker 调用"
        }
        check(handle != 0L) { "多实例物理 owner 已释放" }
    }
    fun openChannel(instance: String, epoch: ULong): ExtensionChannel {
        owner(); requireID(instance); require(epoch != 0uL)
        return ExtensionChannel.fromMultiplexer(this,
            NativeExtensionMultiplexer.openChannel(handle, instance.toByteArray(Charsets.UTF_8), epoch.toLong()))
    }
    fun step(milliseconds: Int = 0): Int {
        owner(); require(milliseconds >= 0); return NativeExtensionMultiplexer.step(handle, milliseconds)
    }
    fun info(): Info {
        owner(); val v = NativeExtensionMultiplexer.info(handle)
        check(v.size == 8 && v[2] in 0L..8L)
        return Info(v[0].toInt(), v[1].toInt(), v[2].toInt(), v[3].toULong(), v[4].toULong(), v[5].toULong(), v[6] != 0L, v[7] != 0L)
    }
    fun scopeInfo(instance: String, epoch: ULong): ScopeInfo {
        owner(); requireID(instance); require(epoch != 0uL)
        val fields = NativeExtensionMultiplexer.scopeInfo(handle, instance.toByteArray(Charsets.UTF_8), epoch.toLong())
        check(fields.size == 3)
        val v = fields[0] as LongArray
        check(v.size == 11)
        if (v[0] != 0L) throw ExtensionChannelException(v[0].toInt(), "所选实例状态不可读")
        check(v[3] in 0L..64L && v[4].toULong() == epoch)
        val connection = (fields[1] as ByteArray).toString(Charsets.UTF_8)
        val identity = (fields[2] as ByteArray).toString(Charsets.UTF_8)
        check(identity == instance); requireID(connection)
        return ScopeInfo(connection, identity, epoch, v[1].toInt(), v[2].toInt(), v[3].toInt(),
            v[5].toULong(), v[6].toULong(), v[7].toULong(), v[8] != 0L, v[9] != 0L, v[10] != 0L)
    }
    fun transportInfo(): ExtensionChannel.Info {
        owner(); return ExtensionChannel.decodeInfo(NativeExtensionMultiplexer.transportInfo(handle))
    }
    fun processObservation(): ExtensionChannel.ProcessObservation {
        check(Thread.currentThread() === worker && Looper.myLooper() != Looper.getMainLooper())
        val fields = if (handle == 0L) checkNotNull(finalProcessObservation) { "物理进程观察不可用" }
            else NativeExtensionMultiplexer.processObservation(handle)
        return ExtensionChannel.decodeProcessObservation(fields)
    }
    fun stop() { val id = handle; if (id != 0L) NativeExtensionMultiplexer.stop(id) }
    fun closeStep(milliseconds: Int = 0): Boolean {
        owner(); require(milliseconds >= 0)
        val observation = arrayOfNulls<Any>(1)
        return when (NativeExtensionMultiplexer.close(handle, milliseconds, observation)) {
            0 -> false
            1 -> {
                @Suppress("UNCHECKED_CAST")
                val snapshot = observation[0] as Array<Any>?
                finalProcessObservation = snapshot; handle = 0; true
            }
            else -> throw ExtensionChannelException(3, "物理多实例连接仍待释放")
        }
    }
    override fun close() {
        if (handle == 0L) return
        repeat(32) { if (closeStep()) return }
        throw ExtensionChannelException(3, "物理连接仍持有子 owner，请在 worker 释放后重试")
    }
    companion object {
        fun adopt(channel: ExtensionChannel, connection: String, frameBytes: Int = 4096,
            receiveFrames: Int = 64, instances: Int = 8): ExtensionMultiplexer {
            check(Looper.myLooper() != Looper.getMainLooper()); requireID(connection)
            require(frameBytes in 512..65536 && receiveFrames in 1..64 && instances in 1..8)
            val result = ExtensionMultiplexer(Thread.currentThread())
            result.handle = channel.adoptMultiplexer(connection.toByteArray(Charsets.UTF_8), frameBytes, receiveFrames, instances)
            return result
        }
        private fun requireID(value: String) {
            require(value.length == 32 && value.all { it in '0'..'9' || it in 'a'..'f' } && value.any { it != '0' })
        }
    }
}

internal object NativeExtensionMultiplexer {
    init { System.loadLibrary("ghosterm_android") }
    external fun open(channel: Long, connection: ByteArray, frame: Int, receive: Int, instances: Int): LongArray
    external fun openChannel(handle: Long, instance: ByteArray, epoch: Long): LongArray
    external fun step(handle: Long, milliseconds: Int): Int
    external fun info(handle: Long): LongArray
    external fun scopeInfo(handle: Long, instance: ByteArray, epoch: Long): Array<Any>
    external fun transportInfo(handle: Long): Array<Any>
    external fun processObservation(handle: Long): Array<Any>
    external fun stop(handle: Long)
    external fun close(handle: Long, milliseconds: Int, observation: Array<Any?>): Int
}
