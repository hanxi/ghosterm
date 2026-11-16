package com.hanxi.ghosterm

import android.content.Context
import android.content.ContentResolver
import android.net.Uri
import android.os.CancellationSignal
import android.os.ParcelFileDescriptor
import android.os.SystemClock
import android.system.ErrnoException
import android.system.Os
import android.system.OsConstants
import android.system.StructPollfd
import com.hanxi.ghosterm.bridge.FileDescriptorFlags
import kotlinx.coroutines.*
import java.io.File
import java.security.MessageDigest

/** Immutable capture: only an unlinked read-only fd survives a successful read.
 * Each borrower owns its duplicate, so closing a page cannot revoke live IO. */
internal class PluginArchiveCapture private constructor(private val owner: ParcelFileDescriptor, val bytes: Long, val sha256: String) : AutoCloseable {
    class Rejected(message: String) : IllegalArgumentException(message)
    private var closed = false
    @Synchronized fun borrow(): ParcelFileDescriptor {
        check(!closed) { "ZIP 快照已释放，请重新选择。" }
        return ParcelFileDescriptor.dup(owner.fileDescriptor)
    }
    @Synchronized override fun close() { if (!closed) { closed = true; owner.close() } }

    companion object {
        @OptIn(InternalCoroutinesApi::class)
        suspend fun read(context: Context, resolver: ContentResolver, uri: Uri): PluginArchiveCapture {
            // Keep the produced owner outside withContext: prompt cancellation
            // while dispatching its result back must also close the capture.
            var result: PluginArchiveCapture? = null
            try {
                withTimeout(60000) { withContext(Dispatchers.IO) {
                    demand(uri.scheme == "content" && !uri.authority.isNullOrBlank(), "请选择文档提供方中的 ZIP 文件。")
                    demand(PluginSignatureDocuments.acquireArchiveSlot(), "先前的材料读取尚未释放，请稍后再试。")
                    try {
                        val job = currentCoroutineContext()
                        val cancellation = CancellationSignal()
                        val registration = job[Job]!!.invokeOnCompletion(onCancelling = true, invokeImmediately = true) { cancellation.cancel() }
                        try {
                            val started = SystemClock.elapsedRealtime()
                            job.ensureActive()
                            (resolver.openFileDescriptor(uri, "r", cancellation) ?: throw Rejected("无法打开所选 ZIP。")).use { input ->
                                job.ensureActive()
                                val fd = input.fileDescriptor; val before = Os.fstat(fd)
                                val regular = OsConstants.S_ISREG(before.st_mode)
                                demand(regular || OsConstants.S_ISFIFO(before.st_mode), "所选 ZIP 不是普通文件或数据流。")
                                val flags = FileDescriptorFlags.status(fd)
                                demand(flags and OsConstants.O_ACCMODE == OsConstants.O_RDONLY, "所选 ZIP 没有提供只读文件句柄。")
                                demand(FileDescriptorFlags.descriptorFlags(input.fd) and OsConstants.FD_CLOEXEC != 0, "所选 ZIP 没有提供可安全读取的文件句柄。")
                                val file = File.createTempFile("plugin-archive-capture-", ".zip", context.applicationContext.cacheDir)
                                try {
                                    val output = Os.open(file.path, OsConstants.O_WRONLY or OsConstants.O_NOFOLLOW or OsConstants.O_CLOEXEC, 0)
                                    var total = 0L
                                    val hash = MessageDigest.getInstance("SHA-256")
                                    try {
                                        FileDescriptorFlags.status(fd, flags or OsConstants.O_NONBLOCK)
                                        try {
                                            val buffer = ByteArray(16384)
                                            val poll = StructPollfd().apply { this.fd = fd; events = OsConstants.POLLIN.toShort() }
                                            while (true) {
                                                job.ensureActive()
                                                demand(SystemClock.elapsedRealtime() - started < 60000, "读取 ZIP 超时，请重新选择。")
                                                if (Os.poll(arrayOf(poll), 100) == 0) continue
                                                demand(poll.revents.toInt() and (OsConstants.POLLERR or OsConstants.POLLNVAL) == 0, "所选 ZIP 的数据流已失效。")
                                                val count = try { Os.read(fd, buffer, 0, buffer.size) }
                                                    catch (e: ErrnoException) { if (e.errno == OsConstants.EINTR || e.errno == OsConstants.EAGAIN) continue else throw e }
                                                if (count == 0) break
                                                demand(total <= PluginReleaseArchive.LIMIT - count, "ZIP 超过 72 MiB 上限。")
                                                var at = 0
                                                while (at < count) {
                                                    job.ensureActive()
                                                    val size = Os.write(output, buffer, at, count - at)
                                                    demand(size > 0, "无法写入 ZIP 私有快照。"); at += size
                                                }
                                                hash.update(buffer, 0, count); total += count
                                            }
                                            input.checkError()
                                            val after = Os.fstat(fd)
                                            demand(!regular || before.st_dev == after.st_dev && before.st_ino == after.st_ino && before.st_size == after.st_size &&
                                                before.st_mtim.tv_sec == after.st_mtim.tv_sec && before.st_mtim.tv_nsec == after.st_mtim.tv_nsec &&
                                                before.st_ctim.tv_sec == after.st_ctim.tv_sec && before.st_ctim.tv_nsec == after.st_ctim.tv_nsec,
                                                "所选 ZIP 在读取期间发生变化，请重新选择。")
                                            demand(total >= 22, "ZIP 内容不完整，请重新选择。")
                                            Os.fchmod(output, 256)
                                        } finally { FileDescriptorFlags.status(fd, flags) }
                                    } finally { Os.close(output) }
                                    job.ensureActive()
                                    val readOnly = Os.open(file.path, OsConstants.O_RDONLY or OsConstants.O_NOFOLLOW or OsConstants.O_CLOEXEC, 0)
                                    try {
                                        val owned = ParcelFileDescriptor.dup(readOnly)
                                        try {
                                            demand(file.delete(), "ZIP 私有快照清理失败。")
                                            result = PluginArchiveCapture(owned, total, hash.digest().joinToString("") { "%02x".format(it.toInt() and 255) })
                                        } catch (e: Throwable) { owned.close(); throw e }
                                    } finally { Os.close(readOnly) }
                                } finally { demand(!file.exists() || file.delete(), "ZIP 私有快照清理失败。") }
                            }
                        } catch (e: Exception) { job.ensureActive(); throw e }
                        finally { registration.dispose() }
                    } finally { PluginSignatureDocuments.releaseArchiveSlot() }
                } }
                currentCoroutineContext().ensureActive()
                return checkNotNull(result).also { result = null }
            } finally { result?.close() }
        }
        private fun demand(value: Boolean, message: String) { if (!value) throw Rejected(message) }
    }
}
