package com.hanxi.ghosterm.bridge

import android.os.Looper
import java.nio.CharBuffer
import java.nio.charset.CodingErrorAction

/** Core pins Session until successful source destruction. All operations use
 * original non-UI worker; drive Session separately. No finalizer or replay. */
class ExtensionFileSource private constructor(private val worker: Thread) : AutoCloseable {
    enum class Status { OK, WAIT, INVALID, DENIED, UNSUPPORTED, IO, LIMIT, CHANGED, CANCELLED, BUSY, UNKNOWN, CLOSED, NOT_FOUND }
    enum class Phase { HASHING, READY, SENDING, RECEIPT, CANCELLING, SUCCEEDED, FAILED, STOPPED, UNCERTAIN }
    class Admission internal constructor() {
        var status: Status = Status.INVALID
            internal set
        var source: ExtensionFileSource? = null
            internal set
    }
    data class Transfer(val state: ExtensionSession.TransferState, val confirmed: Boolean, val cancelRequested: Boolean,
        val accepted: Long, val resourceId: String, val code: String, val generation: Long)
    data class Info(val phase: Phase, val error: Status, val cleanupError: Status, val closed: Boolean, val hasTransfer: Boolean,
        val hashed: Long, val readBytes: Long, val accepted: Long, val descriptor: ExtensionResourceStore.Descriptor, val transfer: Transfer)
    private var handle = 0L
    private fun owner() {
        check(Thread.currentThread() === worker && Looper.myLooper() != Looper.getMainLooper() && handle != 0L) { "源文件已释放或不在创建 worker" }
    }
    fun step(): Phase { owner(); return Phase.entries[NativeExtensionSource.step(handle)] }
    fun send(): Status { owner(); return Status.entries[NativeExtensionSource.send(handle)] }
    fun cancel() { owner(); NativeExtensionSource.cancel(handle) }
    fun info(): Info {
        owner(); val packet = NativeExtensionSource.info(handle); val v = packet[0] as LongArray; val b = packet[1] as ByteArray
        check(v.size == 16 && b.size == 1318)
        return Info(Phase.entries[v[0].toInt()], Status.entries[v[1].toInt()], Status.entries[v[2].toInt()], v[3] != 0L, v[4] != 0L, v[5], v[6], v[7],
            ExtensionResourceStore.Descriptor(resourceText(b, 33, 65), resourceText(b, 98, 129), resourceText(b, 227, 1025), v[14], v[15] != 0L, resourceText(b, 0, 33), v[13]),
            Transfer(ExtensionSession.TransferState.entries[v[9].toInt()], v[10] != 0L, v[11] != 0L, v[8], resourceText(b, 1252, 33), resourceText(b, 1285, 33), v[12]))
    }
    fun closeStep(): Status {
        if (handle == 0L) return Status.CLOSED
        owner(); val status = Status.entries[NativeExtensionSource.close(handle)]
        if (status == Status.OK) handle = 0L
        return status
    }
    override fun close() { val status = closeStep(); check(status == Status.OK || status == Status.CLOSED) { "源文件尚未释放或发生关闭错误，保留 owner：$status" } }
    companion object {
        private fun encode(value: String, limit: Int): ByteArray {
            require('\u0000' !in value)
            val buffer = Charsets.UTF_8.newEncoder().onMalformedInput(CodingErrorAction.REPORT)
                .onUnmappableCharacter(CodingErrorAction.REPORT).encode(CharBuffer.wrap(value))
            require(buffer.remaining() <= limit) { "源文件参数过长" }
            return ByteArray(buffer.remaining()).also { buffer.get(it) }
        }
        internal fun open(session: Long, lease: ExtensionSession.Lease, rootFd: Int, path: String, mime: String, limit: Long): Admission {
            check(Looper.myLooper() != Looper.getMainLooper())
            val encodedPath = encode(path, 4096); val encodedMime = encode(mime, 128)
            val source = ExtensionFileSource(Thread.currentThread()); val result = Admission()
            // Allocate both Java owners before native construction. Filling the
            // already allocated objects after success cannot lose an owning pin.
            val values = NativeExtensionSource.open(session, lease.bytes, rootFd, encodedPath, encodedMime, limit)
            check(values.size == 2); result.status = Status.entries[values[0].toInt()]
            if (result.status == Status.OK) { check(values[1] > 0); source.handle = values[1]; result.source = source }
            return result
        }
    }
}
internal object NativeExtensionSource {
    init { System.loadLibrary("ghosterm_android") }
    external fun open(session: Long, lease: ByteArray, rootFd: Int, path: ByteArray, mime: ByteArray, limit: Long): LongArray
    external fun step(handle: Long): Int
    external fun send(handle: Long): Int
    external fun cancel(handle: Long)
    external fun info(handle: Long): Array<Any>
    external fun close(handle: Long): Int
}
