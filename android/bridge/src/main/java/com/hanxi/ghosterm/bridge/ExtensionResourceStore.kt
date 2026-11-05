package com.hanxi.ghosterm.bridge

import android.os.Looper

/** Trusted native private-directory owner. Explicit close on original worker;
 * WAIT retains all handles and accounting. No finalizer or implicit release. */
class ExtensionResourceStore(directoryFd: Int, totalBytes: Long = 1048576, resourceBytes: Long = 262144, slots: Int = 64) : AutoCloseable {
    enum class Status { OK, WAIT, INVALID, DENIED, CLOSED, PROTOCOL, LIMIT, IO, UNSUPPORTED, NOT_FOUND }
    data class Descriptor(val sha256: String, val mime: String, val displayName: String, val size: Long,
        val sizeKnown: Boolean = true, val resourceId: String = "", val generation: Long = 0)
    data class Usage(val reservedBytes: Long, val resources: Long, val scopes: Long, val sessions: Long)
    data class Info(val descriptor: Descriptor, val received: Long, val reserved: Long, val references: Long, val published: Boolean)
    class Snapshot internal constructor(private val store: ExtensionResourceStore, private val lease: ExtensionSession.Lease,
        private val scope: Long, private var handle: Long, private val token: ByteArray, val descriptor: Descriptor) : AutoCloseable {
        fun info(): Pair<Status, Info?> {
            val owner = store.nativeHandle(); if (handle == 0L) return Status.CLOSED to null
            val packet = NativeExtensionResourceStore.info(owner, lease.bytes, scope, handle, token)
            val v = packet[0] as LongArray; val b = packet[1] as ByteArray; check(v.size == 8 && b.size == 1252)
            val status = Status.entries[v[0].toInt()]
            return status to if (status == Status.OK) Info(resourceDescriptor(b, 0, v[5], v[6], v[7] != 0L), v[1], v[2], v[3], v[4] != 0L) else null
        }
        fun read(offset: Long, capacity: Int = 16384): Pair<Status, ByteArray> {
            val owner = store.nativeHandle(); if (handle == 0L) return Status.CLOSED to byteArrayOf()
            require(offset >= 0 && capacity in 1..16384)
            val packet = NativeExtensionResourceStore.read(owner, lease.bytes, scope, handle, token, offset, capacity)
            val v = packet[0] as LongArray; val b = packet[1] as ByteArray; check(v.size == 2 && b.size == capacity && v[1] in 0..capacity.toLong())
            return Status.entries[v[0].toInt()] to b.copyOf(v[1].toInt())
        }
        fun retain(): Pair<Status, Snapshot?> {
            val owner = store.nativeHandle(); if (handle == 0L) return Status.CLOSED to null
            // Allocate the owning object and success pair before adding a pin.
            val copy = Snapshot(store, lease, scope, handle, token.copyOf(), descriptor)
            val success = Status.OK to copy
            val v = NativeExtensionResourceStore.reference(owner, lease.bytes, scope, handle, token, true)
            check(v.size == 2); val status = Status.entries[v[0].toInt()]
            return if (status == Status.OK) success else status to null
        }
        fun closeStep(): Status {
            if (handle == 0L) return Status.CLOSED
            val v = NativeExtensionResourceStore.reference(store.nativeHandle(), lease.bytes, scope, handle, token, false)
            check(v.size == 2); handle = v[1]; if (handle == 0L) token.fill(0)
            return Status.entries[v[0].toInt()]
        }
        override fun close() { val status = closeStep(); check(status == Status.OK || status == Status.NOT_FOUND || status == Status.CLOSED) { "资源快照清理失败，保留引用供 worker 重试：$status" } }
    }
    private val worker = Thread.currentThread()
    private var handle: Long
    init {
        check(Looper.myLooper() != Looper.getMainLooper()) { "资源 Store 不能在 UI 线程创建" }
        val v = NativeExtensionResourceStore.open(directoryFd, totalBytes, resourceBytes, slots)
        check(v.size == 2 && v[0] == 0L && v[1] > 0) { "无法打开资源 Store" }; handle = v[1]
    }
    internal fun nativeHandle(): Long {
        check(Thread.currentThread() === worker && Looper.myLooper() != Looper.getMainLooper() && handle != 0L) { "资源 Store 已释放或不在创建 worker" }
        return handle
    }
    fun usage(): Usage { val v = NativeExtensionResourceStore.usage(nativeHandle()); check(v.size == 4); return Usage(v[0], v[1], v[2], v[3]) }
    fun closeStep(): Status {
        if (handle == 0L) return Status.CLOSED
        val status = Status.entries[NativeExtensionResourceStore.close(nativeHandle())]
        if (status == Status.OK || status == Status.IO) handle = 0L
        return status
    }
    override fun close() { val status = closeStep(); check(status == Status.OK || status == Status.CLOSED) { "资源 Store 尚未释放或发生关闭错误：$status" } }
    internal fun snapshot(lease: ExtensionSession.Lease, v: LongArray, b: ByteArray): Snapshot =
        Snapshot(this, lease, v[1], v[2], b.copyOfRange(0, 33), resourceDescriptor(b, 33, v[3], v[4], v[5] != 0L))
    internal fun discard(lease: ExtensionSession.Lease, packet: Array<Any>) {
        val v = packet[0] as LongArray; val b = packet[1] as ByteArray
        NativeExtensionResourceStore.discard(nativeHandle(), lease.bytes, v, b)
    }
}
internal fun resourceText(b: ByteArray, at: Int, size: Int): String = b.copyOfRange(at, at + size).takeWhile { it != 0.toByte() }.toByteArray().toString(Charsets.UTF_8)
private fun resourceDescriptor(b: ByteArray, at: Int, generation: Long, size: Long, known: Boolean) = ExtensionResourceStore.Descriptor(
    resourceText(b, at + 33, 65), resourceText(b, at + 98, 129), resourceText(b, at + 227, 1025), size, known, resourceText(b, at, 33), generation)
internal object NativeExtensionResourceStore {
    init { System.loadLibrary("ghosterm_android") }
    external fun open(fd: Int, total: Long, resource: Long, slots: Int): LongArray
    external fun usage(handle: Long): LongArray
    external fun close(handle: Long): Int
    external fun info(handle: Long, lease: ByteArray, owner: Long, ref: Long, token: ByteArray): Array<Any>
    external fun read(handle: Long, lease: ByteArray, owner: Long, ref: Long, token: ByteArray, offset: Long, capacity: Int): Array<Any>
    external fun reference(handle: Long, lease: ByteArray, owner: Long, ref: Long, token: ByteArray, retain: Boolean): LongArray
    external fun discard(handle: Long, lease: ByteArray, metadata: LongArray, bytes: ByteArray): Int
}
