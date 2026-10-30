package com.hanxi.ghosterm.data

import android.content.ContentResolver
import android.net.Uri
import android.os.CancellationSignal
import android.provider.OpenableColumns
import android.system.ErrnoException
import android.system.Os
import android.system.OsConstants
import android.system.StructPollfd
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlin.coroutines.EmptyCoroutineContext
import kotlin.coroutines.resumeWithException

/** Caller owns and erases bytes. No URI grants, document copies or plaintext survive the import. */
class IdentityDocument(val name: String, val bytes: ByteArray)

object IdentityDocumentReader {
    /** SAF providers may return pipes. Poll on an IO worker so cancellation does not wait for EOF. */
    suspend fun read(resolver: ContentResolver, uri: Uri): IdentityDocument = suspendCancellableCoroutine { waiting ->
        val signal = CancellationSignal()
        waiting.invokeOnCancellation {
            // A provider's cancellation callback may involve Binder; never wait for it on the UI thread.
            Dispatchers.IO.dispatch(EmptyCoroutineContext) { signal.cancel() }
        }
        Dispatchers.IO.dispatch(EmptyCoroutineContext) {
            val buffer = ByteArray(1024 * 1024 + 1)
            try {
                if (!waiting.isActive) return@dispatch
                val name = resolver.query(uri,arrayOf(OpenableColumns.DISPLAY_NAME),null,null,null,signal)?.use {
                    if (it.moveToFirst()) it.getString(0) else null
                } ?: "导入的 SSH 密钥"
                if (!waiting.isActive) return@dispatch
                val count = checkNotNull(resolver.openAssetFileDescriptor(uri,"r",signal)) { "无法读取所选密钥" }.use { asset ->
                    asset.createInputStream().use { input ->
                        val fd = input.fd
                        val flags = Os.fcntlInt(fd,OsConstants.F_GETFL,0)
                        Os.fcntlInt(fd,OsConstants.F_SETFL,flags or OsConstants.O_NONBLOCK)
                        try {
                            val poll = StructPollfd().apply { this.fd = fd; events = OsConstants.POLLIN.toShort() }
                            val limit = if (asset.declaredLength >= 0) minOf(buffer.size.toLong(),asset.declaredLength).toInt() else buffer.size
                            var used = 0
                            while (used < limit) {
                                if (!waiting.isActive) return@dispatch
                                try {
                                    if (Os.poll(arrayOf(poll),100) == 0) continue
                                    val n = Os.read(fd,buffer,used,limit-used)
                                    if (n == 0) break
                                    used += n
                                } catch (failure: ErrnoException) {
                                    if (failure.errno != OsConstants.EAGAIN && failure.errno != OsConstants.EINTR) throw failure
                                }
                            }
                            check(used in 1..1024*1024) { "密钥必须为 1 字节至 1 MiB" }
                            used
                        } finally { Os.fcntlInt(fd,OsConstants.F_SETFL,flags) }
                    }
                }
                if (!waiting.isActive) return@dispatch
                val document = IdentityDocument(name,buffer.copyOf(count))
                waiting.resume(document) { _, unused, _ -> unused.bytes.fill(0) }
            } catch (failure: Exception) {
                if (waiting.isActive) waiting.resumeWithException(failure)
            } finally { buffer.fill(0) }
        }
    }
}
