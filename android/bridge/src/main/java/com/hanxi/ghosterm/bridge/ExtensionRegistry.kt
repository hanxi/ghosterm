package com.hanxi.ghosterm.bridge

import android.os.Looper

/** Worker-owned catalog. Source acceptance is not execution permission. */
class ExtensionRegistry : AutoCloseable {
    enum class Status { OK, INVALID, UNKNOWN, DENIED, LIMIT, MEMORY, NOT_FOUND, CONFLICT, STALE, IO, UNSUPPORTED, CANCELLED }
    enum class Source { UNKNOWN, DENIED, ALLOWED }
    class RegistryException(val status: Status) : IllegalStateException("插件注册表：$status")
    data class Selection(val status: Status, val packageBytes: ExtensionPackage? = null, val command: ExtensionPackage.Command? = null)
    private val worker = Thread.currentThread()
    private var handle = 0L
    init {
        check(Looper.myLooper() != Looper.getMainLooper())
        val v = NativeExtensionRegistry.open(); check(v.size == 2)
        val status = Status.entries[v[0].toInt()]
        if (status != Status.OK) throw RegistryException(status)
        check(v[1] > 0); handle = v[1]
    }
    private fun pointer(): Long {
        check(Thread.currentThread() === worker && Looper.myLooper() != Looper.getMainLooper() && handle != 0L) { "注册表已释放或不在原非 UI worker" }
        return handle
    }
    fun put(packageBytes: ExtensionPackage, source: Source, expectedDigest: String? = null): Status =
        Status.entries[NativeExtensionRegistry.put(pointer(), packageBytes.pointer(), source.ordinal, expectedDigest?.let { ExtensionPackage.encode(it, 64) })]
    fun remove(id: String, executor: Boolean, expectedDigest: String): Status =
        Status.entries[NativeExtensionRegistry.remove(pointer(), ExtensionPackage.encode(id, 128), executor, ExtensionPackage.encode(expectedDigest, 64))]
    fun resolve(command: String, executor: Boolean, selectedId: String? = null, selectedDigest: String? = null): Selection {
        val v = NativeExtensionRegistry.resolve(pointer(), ExtensionPackage.encode(command, 519), executor,
            selectedId?.let { ExtensionPackage.encode(it, 128) }, selectedDigest?.let { ExtensionPackage.encode(it, 64) })
        check(v.size == 2); val status = Status.entries[v[0].toInt()]
        if (status != Status.OK) { check(v[1] == 0L); return Selection(status) }
        val selected = ExtensionPackage.adopt(v[1])
        try { return Selection(status, selected, selected.commands().single { it.path == command }) }
        catch (error: Throwable) { selected.close(); throw error }
    }
    /** IO still consumes the registry; callers must not retry the old handle. */
    fun closeStatus(): Status {
        if (handle == 0L) return Status.OK
        val status = Status.entries[NativeExtensionRegistry.close(pointer())]; handle = 0L; return status
    }
    override fun close() { val status = closeStatus(); if (status != Status.OK) throw RegistryException(status) }
}
internal object NativeExtensionRegistry {
    init { System.loadLibrary("ghosterm_android") }
    external fun open(): LongArray
    external fun put(handle: Long, packageHandle: Long, source: Int, expected: ByteArray?): Int
    external fun remove(handle: Long, plugin: ByteArray, executor: Boolean, expected: ByteArray): Int
    external fun resolve(handle: Long, command: ByteArray, executor: Boolean, plugin: ByteArray?, digest: ByteArray?): LongArray
    external fun close(handle: Long): Int
}
