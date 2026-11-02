package com.hanxi.ghosterm.data

import android.content.ContentResolver
import android.database.ContentObserver
import android.database.Cursor
import android.net.Uri
import android.os.CancellationSignal
import android.os.SystemClock
import android.provider.DocumentsContract
import java.util.concurrent.Semaphore
import java.util.concurrent.TimeUnit

/** Directory cursors may be partial even when their rows look valid. Never use
 * those rows for source scans, conflict decisions, or a successful empty list. */
internal object SafDocumentQuery {
    private const val LOADING_TIMEOUT_MS = 30_000L

    @Suppress("DEPRECATION") // Bundle.get keeps malformed provider values distinguishable from absence.
    private fun loading(cursor: Cursor): Boolean {
        val extras = cursor.extras
        val error = extras.get(DocumentsContract.EXTRA_ERROR)
        check(error == null || error is String) { "文件提供方返回无效错误状态" }
        check(error == null) { "文件提供方读取失败：${(error as String).ifBlank { "未知错误" }.take(512)}" }
        val pending = extras.get(DocumentsContract.EXTRA_LOADING)
        check(pending == null || pending is Boolean) { "文件提供方返回无效加载状态" }
        return pending == true
    }

    /** Explicit cleanup rechecks must not proceed with incomplete metadata. */
    fun requireComplete(cursor: Cursor) {
        check(!loading(cursor)) { "文件提供方仍在加载，请稍后重新核对" }
    }

    /** Called only from IO owners. Cursors and their observers never escape while
     * loading; cancellation also interrupts the wait without blocking the UI. */
    fun read(resolver: ContentResolver, uri: Uri, columns: Array<String>, signal: CancellationSignal): Cursor {
        val deadline = SystemClock.elapsedRealtime() + LOADING_TIMEOUT_MS
        while (true) {
            signal.throwIfCanceled()
            val cursor = checkNotNull(resolver.query(uri, columns, null, null, null, signal)) { "文件提供方查询失败" }
            var returned = false
            try {
                signal.throwIfCanceled()
                if (!loading(cursor)) {
                    returned = true
                    return cursor
                }
                val changed = Semaphore(0)
                val observer = object : ContentObserver(null) {
                    override fun onChange(selfChange: Boolean) { changed.release() }
                }
                cursor.registerContentObserver(observer)
                try {
                    // Also requery once a second: a notification may have raced
                    // registration, and a broken provider must still time out.
                    val retryAt = minOf(deadline, SystemClock.elapsedRealtime() + 1_000L)
                    while (true) {
                        signal.throwIfCanceled()
                        val now = SystemClock.elapsedRealtime()
                        check(now < deadline) { "文件提供方加载超时，请重试" }
                        if (now >= retryAt || changed.tryAcquire(minOf(100L, retryAt - now), TimeUnit.MILLISECONDS)) break
                    }
                } finally { cursor.unregisterContentObserver(observer) }
            } finally { if (!returned) cursor.close() }
        }
    }
}
