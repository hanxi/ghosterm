package com.hanxi.ghosterm.data

import android.content.res.AssetFileDescriptor
import android.os.CancellationSignal
import android.os.ParcelFileDescriptor
import android.os.StatFs
import android.system.ErrnoException
import android.system.Os
import android.system.OsConstants
import android.system.StructPollfd
import kotlinx.coroutines.launch
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.suspendCancellableCoroutine
import java.io.Closeable
import java.io.File
import com.hanxi.ghosterm.bridge.AnonymousStagingFile
import kotlin.coroutines.EmptyCoroutineContext
import kotlin.coroutines.resumeWithException

/** One process-scoped store for transfer snapshots, outside the browsable HOME.
 * Files are anonymous from creation (O_TMPFILE): only owned descriptors
 * keep bytes alive, including across page changes. Process death releases them.
 * This is transient staging, not a durable job journal or destination commit.
 * All limits count reservations, including writers and outstanding reader leases.
 * Local filesystem/provider calls are not hard-preemptible; pipe reads poll cancellation.
 */
class TransferStagingStore(private val directory: File,
    val fileLimit: Long = 256L * 1024 * 1024,
    private val totalLimit: Long = 512L * 1024 * 1024,
    private val freeSpaceReserve: Long = 32L * 1024 * 1024,
    private val snapshotLimit: Int = 32) : Closeable {
    init { require(fileLimit > 0 && totalLimit >= fileLimit && freeSpaceReserve >= 0 && snapshotLimit > 0) }
    private val lock = Any()
    private var closed = false
    private var reserved = 0L
    private var slots = 0
    private val snapshots = mutableSetOf<Snapshot>()
    private val receivers = mutableSetOf<kotlinx.coroutines.Job>()
    private val sourceReads = mutableSetOf<CancellationSignal>()
    private val released = CompletableDeferred<Unit>()
    private fun completeClose() {
        if(closed && slots==0 && receivers.isEmpty() && sourceReads.isEmpty()) released.complete(Unit)
    }
    val reservedBytes: Long get() = synchronized(lock) { reserved }

    private fun reserve(amount: Long) = synchronized(lock) {
        check(!closed) { "文件暂存已关闭" }
        check(amount <= totalLimit - reserved) { "文件暂存容量不足，请等待其他复制任务释放空间" }
        check(slots < snapshotLimit) { "暂存文件数量已达上限，请等待其他任务完成" }
        reserved += amount;slots++
    }
    private fun release(amount: Long) = synchronized(lock) { reserved -= amount;slots--;check(reserved >= 0 && slots >= 0);completeClose() }
    private fun active() = synchronized(lock) { !closed }

    /** open must return a NEW, exclusively owned source descriptor; never a borrowed
     * descriptor from a browser/terminal. The store closes it on every outcome.
     * expectedSize is the frozen source size if known; null still enforces fileLimit.
     * Asset offsets/declared lengths are honored; reliable-pipe errors are propagated.
     * A returned snapshot is a complete byte copy, not a promise of remote publication.
     */
    suspend fun snapshot(expectedSize: Long? = null,
        progress: (Long, Long?) -> Unit = { _, _ -> },
        open: (CancellationSignal) -> AssetFileDescriptor): Snapshot = suspendCancellableCoroutine { waiting ->
        val signal = CancellationSignal()
        synchronized(lock) {
            if(closed) { waiting.resumeWithException(IllegalStateException("文件暂存已关闭"));return@suspendCancellableCoroutine }
            sourceReads.add(signal)
        }
        waiting.invokeOnCancellation { Dispatchers.IO.dispatch(EmptyCoroutineContext) { signal.cancel() } }
        Dispatchers.IO.dispatch(EmptyCoroutineContext) {
            try {
                val result = copy(expectedSize, signal, { waiting.isActive && active() }, progress, open)
                waiting.resume(result, onCancellation = { _, value, _ -> value.close() })
            } catch (failure: Exception) { if (waiting.isActive) waiting.resumeWithException(failure) }
            finally { synchronized(lock) { sourceReads.remove(signal);completeClose() } }
        }
    }

    private fun copy(expected: Long?, signal: CancellationSignal, active: () -> Boolean,
        progress: (Long, Long?) -> Unit,
        open: (CancellationSignal) -> AssetFileDescriptor): Snapshot {
        require(expected == null || expected in 0..fileLimit) { "文件超过暂存上限或大小无效" }
        val reservation = expected ?: fileLimit
        check(active()) { "文件暂存已取消" }
        reserve(reservation)
        var writer: ParcelFileDescriptor? = null
        var reader: ParcelFileDescriptor? = null
        var transferred = false
        try {
            check(directory.isDirectory || directory.mkdirs()) { "无法创建文件暂存目录" }
            check(OsConstants.S_ISDIR(Os.lstat(directory.path).st_mode)) { "文件暂存目录不能是符号链接" }
            Os.chmod(directory.path,448) // 0700; never use the user-visible HOME.
            val available = StatFs(directory.path).availableBytes
            check(available >= freeSpaceReserve && reservedBytes <= available - freeSpaceReserve) { "设备可用空间不足，无法暂存文件" }
            writer = AnonymousStagingFile.create(directory)
            val destination = checkNotNull(writer).fileDescriptor
            var size = 0L
            check(active()) { "文件暂存已取消" }
            open(signal).use { asset ->
                val length = asset.declaredLength
                check(length < 0 || length <= fileLimit) { "文件超过暂存上限" }
                check(expected == null || length < 0 || length == expected) { "源文件大小已改变，请重新复制" }
                run {
                    val source = asset.parcelFileDescriptor.fileDescriptor
                    val before = Os.fstat(source)
                    check(OsConstants.S_ISREG(before.st_mode) || OsConstants.S_ISFIFO(before.st_mode)) { "不支持暂存此文件类型" }
                    if(OsConstants.S_ISREG(before.st_mode)) Os.lseek(source,asset.startOffset,OsConstants.SEEK_SET)
                    else check(asset.startOffset==0L) { "管道文档不能带文件偏移" }
                    val flags = Os.fcntlInt(source,OsConstants.F_GETFL,0)
                    Os.fcntlInt(source,OsConstants.F_SETFL,flags or OsConstants.O_NONBLOCK)
                    val poll = StructPollfd().apply { this.fd=source;events=OsConstants.POLLIN.toShort() }
                    val buffer = ByteArray(32 * 1024)
                    try {
                        val total=if(length>=0) length else expected
                        progress(0,total)
                        while (length < 0 || size < length) {
                            check(active()) { "文件暂存已取消" }
                            // Probe one byte beyond the budget even when metadata omitted size.
                            val remaining = minOf(fileLimit, reservation) - size
                            val count = minOf(buffer.size.toLong(), remaining + 1, if(length < 0) Long.MAX_VALUE else length-size).toInt()
                            val read = try {
                                if(Os.poll(arrayOf(poll),100)==0) continue
                                Os.read(source,buffer,0,count)
                            } catch (failure: ErrnoException) {
                                if(failure.errno==OsConstants.EINTR || failure.errno==OsConstants.EAGAIN) continue
                                throw failure
                            }
                            if(read==0) break
                            check(read.toLong() <= remaining) { "文件超过暂存上限或源文件大小已改变" }
                            var offset=0
                            while(offset<read) {
                                check(active()) { "文件暂存已取消" }
                                val written=try { Os.write(destination,buffer,offset,read-offset) }
                                catch(failure:ErrnoException) { if(failure.errno==OsConstants.EINTR) continue;throw failure }
                                check(written>0) { "文件暂存写入没有进展" };offset+=written
                            }
                            size += read
                            progress(size,total)
                        }
                        asset.parcelFileDescriptor.checkError()
                        check(length < 0 || size == length) { "源文件提前结束" }
                        check(expected == null || size == expected) { "源文件大小已改变，请重新复制" }
                        val after=Os.fstat(source)
                        if(OsConstants.S_ISREG(before.st_mode)) check(before.st_size==after.st_size &&
                            before.st_mtim.tv_sec==after.st_mtim.tv_sec && before.st_mtim.tv_nsec==after.st_mtim.tv_nsec &&
                            before.st_ctim.tv_sec==after.st_ctim.tv_sec && before.st_ctim.tv_nsec==after.st_ctim.tv_nsec) { "读取时源文件发生变化" }
                    } finally { buffer.fill(0);Os.fcntlInt(source,OsConstants.F_SETFL,flags) }
                }
            }
            // Reopen the anonymous inode read-only; a dup of O_RDWR would allow mutation.
            val readonly=Os.open("/proc/self/fd/${checkNotNull(writer).fd}",OsConstants.O_RDONLY or OsConstants.O_CLOEXEC,0)
            try {
                check(Os.fstat(readonly).st_size==size) { "文件暂存长度不一致" }
                reader=ParcelFileDescriptor.dup(readonly)
            } finally { Os.close(readonly) }
            writer.close();writer=null
            synchronized(lock) {
                check(active()) { "文件暂存已取消" }
                val result=Snapshot(checkNotNull(reader),size,reservation)
                snapshots.add(result);transferred=true;reader=null
                return result
            }
        } finally {
            try { writer?.close() } finally {
                try { reader?.close() } finally { if(!transferred) release(reservation) }
            }
        }
    }

    /** Complete remote bytes into private staging. This owns a separate file
     * connection operation, never a final target. Caller retains the connection. */
    suspend fun receive(files: com.hanxi.ghosterm.bridge.RemoteFileSession, path: String,
        expectedSize: Long? = null, timeoutMillis: Int = 5000,
        progress: ((com.hanxi.ghosterm.bridge.RemoteTransferProgress) -> Unit)? = null): Snapshot = suspendCancellableCoroutine { waiting ->
        val worker = kotlinx.coroutines.CoroutineScope(Dispatchers.IO).launch(start = kotlinx.coroutines.CoroutineStart.LAZY) {
            try {
                val result = receiveCopy(files,path,expectedSize,timeoutMillis,progress)
                waiting.resume(result,onCancellation = { _, value, _ -> value.close() })
            } catch(failure: Exception) { if(waiting.isActive) waiting.resumeWithException(failure) }
        }
        worker.invokeOnCompletion { failure ->
            synchronized(lock) { receivers.remove(worker);completeClose() }
            if(failure != null && waiting.isActive) waiting.resumeWithException(failure)
        }
        synchronized(lock) {
            if(closed) worker.cancel()
            else { receivers.add(worker);worker.start() }
        }
        waiting.invokeOnCancellation { worker.cancel() }
    }

    private suspend fun receiveCopy(files: com.hanxi.ghosterm.bridge.RemoteFileSession,
        path: String, expected: Long?, timeoutMillis: Int,
        progress: ((com.hanxi.ghosterm.bridge.RemoteTransferProgress) -> Unit)?): Snapshot {
        require(expected == null || expected in 0..fileLimit) { "文件超过暂存上限或大小无效" }
        val reservation = expected ?: fileLimit
        reserve(reservation)
        var writer: ParcelFileDescriptor? = null
        var reader: ParcelFileDescriptor? = null
        var transferred = false
        try {
            check(directory.isDirectory || directory.mkdirs()) { "无法创建文件暂存目录" }
            check(OsConstants.S_ISDIR(Os.lstat(directory.path).st_mode)) { "文件暂存目录不能是符号链接" }
            Os.chmod(directory.path,448)
            val available = StatFs(directory.path).availableBytes
            check(available >= freeSpaceReserve && reservedBytes <= available-freeSpaceReserve) { "设备可用空间不足，无法暂存文件" }
            writer=AnonymousStagingFile.create(directory)
            kotlinx.coroutines.currentCoroutineContext().ensureActive()
            // Zero reservation still probes remote EOF, but never writes a byte.
            val size=files.receive(path,writer.fd,reservation,timeoutMillis,progress)
            check(expected == null || size == expected) { "源文件大小已改变，请重新复制" }
            check(size <= reservation) { "远端文件超过暂存预算" }
            val readonly=Os.open("/proc/self/fd/${writer.fd}",OsConstants.O_RDONLY or OsConstants.O_CLOEXEC,0)
            try {
                check(Os.fstat(readonly).st_size == size) { "远端暂存长度不一致" }
                reader=ParcelFileDescriptor.dup(readonly)
            } finally { Os.close(readonly) }
            writer.close();writer=null
            kotlinx.coroutines.currentCoroutineContext().ensureActive()
            synchronized(lock) {
                check(!closed) { "文件暂存已关闭" }
                val result=Snapshot(checkNotNull(reader),size,reservation)
                snapshots.add(result);transferred=true;reader=null
                return result
            }
        } finally {
            // RemoteFileSession awaits native fd release even when cancelled.
            try { writer?.close() } finally {
                try { reader?.close() } finally { if(!transferred) release(reservation) }
            }
        }
    }

    /** Snapshot close prevents new readers. Existing readers keep their fd AND
     * capacity reservation until their block returns, even if store/page is closed. */
    inner class Snapshot internal constructor(private val descriptor: ParcelFileDescriptor,
        val size: Long, private val reservation: Long) : Closeable {
        private var disposed=false
        private var readers=0
        private fun finished() { if(disposed && readers==0 && snapshots.remove(this)) release(reservation) }
        suspend fun <T> withDescriptor(block: suspend (Int,Long) -> T): T {
            val loan=synchronized(lock) {
                check(!disposed && !closed) { "文件暂存已释放" }
                val fd=Os.open("/proc/self/fd/${descriptor.fd}",OsConstants.O_RDONLY or OsConstants.O_CLOEXEC,0)
                val copy=try { ParcelFileDescriptor.dup(fd) } finally { Os.close(fd) }
                readers++;copy
            }
            try { return block(loan.fd,size) }
            finally { try { loan.close() } finally { synchronized(lock) { readers--;finished() } } }
        }
        override fun close() = synchronized(lock) {
            if(!disposed) { disposed=true;try { descriptor.close() } finally { finished() } }
        }
    }
    override fun close() {
        val pending=synchronized(lock) {
            closed=true
            snapshots.toList().forEach { it.close() }
            completeClose()
            receivers.toList() to sourceReads.toList()
        }
        pending.first.forEach { it.cancel() }
        pending.second.forEach { signal -> Dispatchers.IO.dispatch(EmptyCoroutineContext) { signal.cancel() } }
    }
    /** Includes blocked source providers, anonymous writers and outstanding leases.
     * A cancelled waiter does not release capacity or terminate the actual workers. */
    suspend fun closeAndAwait() { close();released.await() }
}
