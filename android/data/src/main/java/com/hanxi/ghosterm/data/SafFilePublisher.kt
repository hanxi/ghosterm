package com.hanxi.ghosterm.data

import android.content.ContentResolver
import android.database.Cursor
import android.net.Uri
import android.os.CancellationSignal
import android.os.ParcelFileDescriptor
import android.provider.DocumentsContract as Documents
import android.provider.DocumentsContract.Document
import android.system.ErrnoException
import android.system.Os
import android.system.OsConstants
import android.system.StructPollfd
import com.hanxi.ghosterm.domain.FileLocation
import kotlinx.coroutines.*
import java.io.FileDescriptor
import java.nio.CharBuffer
import java.security.MessageDigest
import java.util.concurrent.atomic.AtomicBoolean
import kotlin.coroutines.EmptyCoroutineContext
import kotlin.coroutines.resume
import kotlin.coroutines.resumeWithException

/** Creates a NEW SAF document, never opens an existing target for writing.
 * SAF has no universal atomic/non-overwrite rename or durable commit contract.
 * The created document can be visible while incomplete. Retain its URI on failure;
 * do not automatically delete or retry a provider operation with unknown effects.
 */
class SafFilePublisher(private val resolver: ContentResolver,
    private val observed: suspend (Outcome) -> Unit = {}) {
    enum class State { NOT_ATTEMPTED, CONFLICT, UNKNOWN, CREATED, COMPLETE }
    data class Outcome(val state: State = State.NOT_ATTEMPTED, val documentUri: String? = null,
        val actualName: String? = null, val bytesWritten: Long = 0, val error: String? = null)

    fun prepare(location: FileLocation, name: String): Request {
        validName(name)
        require(location.endpoint.remote == null && location.endpoint.treeUri != null) { "目标不是授权文档目录" }
        return Request(location, name)
    }

    /** One-shot operation. Cancellation can return before a provider call finishes.
     * outcome remains observable even if delivery to execute's caller is cancelled.
     * awaitClosed waits for the actual worker/descriptor lease, not just the caller.
     * Neither this object nor its result is a persistent recovery journal.
     */
    inner class Request internal constructor(val location: FileLocation, val requestedName: String) {
        @Volatile var outcome = Outcome(); private set
        private val started = AtomicBoolean()
        private val cancelled = AtomicBoolean()
        private val signal = CancellationSignal()
        private val closed = CompletableDeferred<Unit>()
        @Volatile private var worker: Job? = null
        val finished: Boolean get() = closed.isCompleted

        fun cancel() {
            cancelled.set(true)
            if (!started.get()) closed.complete(Unit)
            worker?.cancel()
            // A remote cancellation callback can itself block. Never invoke it on UI.
            Dispatchers.IO.dispatch(EmptyCoroutineContext) { signal.cancel() }
        }
        suspend fun awaitClosed() { closed.await() }

        suspend fun execute(snapshot: TransferStagingStore.Snapshot,
            checkpoint: suspend (Long, Long) -> Unit = { _, _ -> }): Outcome {
            check(started.compareAndSet(false, true)) { "粘贴请求不能重复执行" }
            return suspendCancellableCoroutine { waiting ->
                val owner = CoroutineScope(Dispatchers.IO).launch(start = CoroutineStart.LAZY) {
                    try {
                        snapshot.withDescriptor { fd, size ->
                            ParcelFileDescriptor.fromFd(fd).use { input -> copy(input.fileDescriptor, size, checkpoint) }
                        }
                    } catch (failure: Exception) {
                        failed(failure)
                        if (failure is CancellationException) throw failure
                    }
                }
                worker = owner
                owner.invokeOnCompletion { failure ->
                    if (failure != null) failed(failure)
                    closed.complete(Unit)
                    if (waiting.isActive) {
                        if (failure is CancellationException) waiting.resumeWithException(failure)
                        else waiting.resume(outcome)
                    }
                }
                waiting.invokeOnCancellation { cancel() }
                if (cancelled.get()) owner.cancel() else owner.start()
            }
        }

        private fun failed(failure: Throwable) {
            if (outcome.state != State.COMPLETE) outcome = outcome.copy(error =
                if (failure is CancellationException) "复制已取消；请核对已创建文档" else failure.message ?: "文档复制失败")
        }
        private suspend fun active() { currentCoroutineContext().ensureActive();signal.throwIfCanceled() }

        private suspend fun copy(source: FileDescriptor, size: Long, checkpoint: suspend (Long, Long) -> Unit) {
            active()
            val endpoint = location.endpoint
            val tree = Uri.parse(checkNotNull(endpoint.treeUri))
            check(tree.scheme == "content" && Documents.isTreeUri(tree)) { "无效的文档树" }
            check(Documents.getTreeDocumentId(tree) == endpoint.rootId) { "目标文档树身份已改变" }
            check(resolver.persistedUriPermissions.any { it.uri == tree && it.isReadPermission && it.isWritePermission }) { "目标目录读写授权已失效" }
            val root = Documents.buildDocumentUriUsingTree(tree, endpoint.rootId)
            val parent = Documents.buildDocumentUriUsingTree(tree, location.directoryId)
            check(parent == root || Documents.isChildDocument(resolver, root, parent)) { "目标目录不在授权文档树内" }
            val directory = info(parent)
            check(directory.mime == Document.MIME_TYPE_DIR && directory.flags and Document.FLAG_DIR_SUPPORTS_CREATE != 0) { "目标目录不支持创建文档" }
            val existing = children(tree, location.directoryId)
            if (existing.values.any { it == requestedName }) { outcome = Outcome(State.CONFLICT);return }
            val before = Os.fstat(source)
            check(OsConstants.S_ISREG(before.st_mode) && before.st_size == size) { "暂存文件大小已改变" }
            active()
            // createDocument has no CancellationSignal and may commit before losing its reply.
            outcome = Outcome(State.UNKNOWN)
            val created = Documents.createDocument(resolver, parent, "application/octet-stream", requestedName)
                ?: error("创建结果未知；请检查目标目录后再操作")
            outcome = outcome.copy(documentUri = created.toString())
            withContext(NonCancellable) { observed(outcome) }
            check(created.scheme == "content" && created.authority == tree.authority && Documents.isTreeUri(created) &&
                Documents.getTreeDocumentId(created) == endpoint.rootId) { "提供方返回了其他文档树" }
            val id = Documents.getDocumentId(created)
            check(id !in existing && id != endpoint.rootId && id != location.directoryId) { "提供方没有返回新文档，已停止写入" }
            // Preserve the known side effect before checking cancellation of a delayed reply.
            outcome = outcome.copy(state = State.CREATED)
            withContext(NonCancellable) { observed(outcome) }
            active()
            check(Documents.isChildDocument(resolver, parent, created)) { "新文档不在目标目录内" }
            val target = info(created)
            file(target)
            check(target.flags and Document.FLAG_SUPPORTS_WRITE != 0) { "新文档不支持写入" }
            check(target.size == null || target.size == 0L) { "新文档已有内容，已停止写入" }
            outcome = outcome.copy(actualName = target.name)
            withContext(NonCancellable) { observed(outcome) }
            val expectedHash = MessageDigest.getInstance("SHA-256")
            active()
            val writer = resolver.openFileDescriptor(created, "w", signal) ?: error("无法打开新文档")
            var primary: Throwable? = null
            try {
                val fd = writer.fileDescriptor
                val stat = Os.fstat(fd)
                check(OsConstants.S_ISREG(stat.st_mode) || OsConstants.S_ISFIFO(stat.st_mode)) { "不支持此文档写入类型" }
                if (OsConstants.S_ISREG(stat.st_mode)) {
                    check(stat.st_size == 0L) { "新文档写入句柄已有内容" }
                    Os.lseek(fd, 0, OsConstants.SEEK_SET)
                }
                nonblocking(fd) {
                    val buffer = ByteArray(32 * 1024)
                    try {
                        var copied = 0L
                        checkpoint(0, size)
                        while (copied < size) {
                            active()
                            val count = try { Os.pread(source, buffer, 0, minOf(buffer.size.toLong(), size - copied).toInt(), copied) }
                            catch (failure: ErrnoException) { if (failure.errno == OsConstants.EINTR) continue;throw failure }
                            check(count > 0) { "暂存文件提前结束" }
                            var offset = 0
                            while (offset < count) {
                                active()
                                val written = try {
                                    if (!ready(fd, OsConstants.POLLOUT)) continue
                                    Os.write(fd, buffer, offset, count - offset)
                                } catch (failure: ErrnoException) { if (retryable(failure)) continue;throw failure }
                                check(written > 0) { "文档写入没有进展" }
                                offset += written
                                outcome = outcome.copy(bytesWritten = copied + offset)
                            }
                            expectedHash.update(buffer, 0, count)
                            copied += count
                            checkpoint(copied, size)
                        }
                    } finally { buffer.fill(0) }
                }
                val after = Os.fstat(source)
                check(before.st_size == after.st_size && before.st_mtim == after.st_mtim && before.st_ctim == after.st_ctim) { "复制时暂存内容发生变化" }
                active()
                writer.checkError()
            } catch (failure: Throwable) { primary = failure;throw failure }
            finally {
                try {
                    if (primary != null) writer.closeWithError("Ghosterm copy did not complete") else writer.close()
                } catch (cleanup: Exception) { if (primary != null) primary.addSuppressed(cleanup) else throw cleanup }
            }
            // Close alone does not prove an asynchronous provider committed the bytes.
            active()
            verify(created, target, size, expectedHash.digest())
            active()
            outcome = outcome.copy(state = State.COMPLETE)
        }

        private suspend fun verify(uri: Uri, created: Info, size: Long, expected: ByteArray) {
            val before = info(uri);file(before)
            check(before.name == created.name && (before.size == null || before.size == size)) { "写入后的文档信息不一致" }
            val actual = MessageDigest.getInstance("SHA-256")
            active()
            val asset = resolver.openAssetFileDescriptor(uri, "r", signal) ?: error("无法核验新文档内容")
            asset.use {
                val length = asset.declaredLength
                check(length < 0 || length == size) { "写入后的文档长度不一致" }
                val fd = asset.parcelFileDescriptor.fileDescriptor
                val stat = Os.fstat(fd)
                check(OsConstants.S_ISREG(stat.st_mode) || OsConstants.S_ISFIFO(stat.st_mode)) { "不支持核验此文档类型" }
                if (OsConstants.S_ISREG(stat.st_mode)) Os.lseek(fd, asset.startOffset, OsConstants.SEEK_SET)
                else check(asset.startOffset == 0L) { "管道文档不能带文件偏移" }
                var read = 0L
                nonblocking(fd) {
                    val buffer = ByteArray(32 * 1024)
                    try {
                        while (length < 0 || read < length) {
                            active()
                            val count = minOf(buffer.size.toLong(), size - read + 1,
                                if (length < 0) Long.MAX_VALUE else length - read).toInt()
                            val n = try {
                                if (!ready(fd, OsConstants.POLLIN)) continue
                                Os.read(fd, buffer, 0, count)
                            } catch (failure: ErrnoException) { if (retryable(failure)) continue;throw failure }
                            if (n == 0) break
                            check(n <= size - read) { "写入后的文档超过预期长度" }
                            actual.update(buffer, 0, n);read += n
                        }
                        asset.parcelFileDescriptor.checkError()
                        check(read == size && actual.digest().contentEquals(expected)) { "写入后的文档内容核验失败" }
                        val after = Os.fstat(fd)
                        if (OsConstants.S_ISREG(stat.st_mode)) check(stat.st_size == after.st_size &&
                            stat.st_mtim == after.st_mtim && stat.st_ctim == after.st_ctim) { "核验期间文档内容发生变化" }
                    } finally { buffer.fill(0) }
                }
            }
            val after = info(uri);file(after)
            check(after.name == before.name && (after.size == null || after.size == size) &&
                (before.modified == null || after.modified == before.modified)) { "核验期间文档信息发生变化" }
        }

        private fun info(uri: Uri): Info {
            signal.throwIfCanceled()
            return (resolver.query(uri, columns, null, null, null, signal) ?: error("无法读取文档信息")).use { cursor ->
                check(cursor.moveToFirst()) { "文档信息不存在" }
                val id = cursor.required(Document.COLUMN_DOCUMENT_ID)
                check(id == Documents.getDocumentId(uri)) { "文档身份不一致" }
                val result = Info(cursor.required(Document.COLUMN_DISPLAY_NAME), cursor.required(Document.COLUMN_MIME_TYPE),
                    cursor.getInt(cursor.getColumnIndexOrThrow(Document.COLUMN_FLAGS)), cursor.number(Document.COLUMN_SIZE), cursor.number(Document.COLUMN_LAST_MODIFIED))
                check(!cursor.moveToNext()) { "文档身份不唯一" }
                result
            }
        }
        private fun children(tree: Uri, parent: String): Map<String, String> {
            val uri = Documents.buildChildDocumentsUriUsingTree(tree, parent)
            return (resolver.query(uri, arrayOf(Document.COLUMN_DOCUMENT_ID, Document.COLUMN_DISPLAY_NAME), null, null, null, signal)
                ?: error("无法检查目标目录冲突")).use { cursor ->
                val result = mutableMapOf<String, String>()
                while (cursor.moveToNext()) {
                    signal.throwIfCanceled()
                    check(result.size < 4096) { "目标目录条目超过检查上限" }
                    val id = cursor.required(Document.COLUMN_DOCUMENT_ID)
                    check(result.put(id, cursor.required(Document.COLUMN_DISPLAY_NAME)) == null) { "目标目录返回重复文档身份" }
                }
                result
            }
        }
    }

    private data class Info(val name: String, val mime: String, val flags: Int, val size: Long?, val modified: Long?)
    private fun file(info: Info) { check(info.mime != Document.MIME_TYPE_DIR && info.flags and Document.FLAG_VIRTUAL_DOCUMENT == 0) { "目标不是普通文档" } }
    private suspend fun <T> nonblocking(fd: FileDescriptor, block: suspend () -> T): T {
        val flags = Os.fcntlInt(fd, OsConstants.F_GETFL, 0)
        Os.fcntlInt(fd, OsConstants.F_SETFL, flags or OsConstants.O_NONBLOCK)
        try { return block() } finally { Os.fcntlInt(fd, OsConstants.F_SETFL, flags) }
    }
    private fun ready(fd: FileDescriptor, events: Int): Boolean =
        Os.poll(arrayOf(StructPollfd().apply { this.fd = fd;this.events = events.toShort() }), 100) != 0
    private fun retryable(error: ErrnoException) = error.errno == OsConstants.EINTR || error.errno == OsConstants.EAGAIN
    private fun Cursor.required(column: String): String = getString(getColumnIndexOrThrow(column))?.takeIf { it.isNotEmpty() }
        ?: error("提供方缺少文档信息")
    private fun Cursor.number(column: String): Long? {
        val index = getColumnIndex(column)
        return if (index < 0 || isNull(index)) null else getLong(index).takeIf { it >= 0 }
    }
    private fun validName(name: String) {
        require(name.isNotEmpty() && name != "." && name != ".." && '/' !in name && '\u0000' !in name) { "无效的目标文件名" }
        Charsets.UTF_8.newEncoder().encode(CharBuffer.wrap(name))
    }
    private val columns = arrayOf(Document.COLUMN_DOCUMENT_ID, Document.COLUMN_DISPLAY_NAME, Document.COLUMN_MIME_TYPE,
        Document.COLUMN_FLAGS, Document.COLUMN_SIZE, Document.COLUMN_LAST_MODIFIED)
}
