package com.hanxi.ghosterm.data

import android.content.ContentResolver
import android.net.Uri
import android.os.CancellationSignal
import android.os.SystemClock
import android.provider.OpenableColumns
import android.system.ErrnoException
import android.system.Os
import android.system.OsConstants
import android.system.StructPollfd
import com.hanxi.ghosterm.bridge.FileDescriptorFlags
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlinx.coroutines.withTimeout
import java.util.concurrent.Semaphore
import kotlin.coroutines.EmptyCoroutineContext
import kotlin.coroutines.resumeWithException

data class FontDocument(val name:String,val bytes:ByteArray)

/** One bounded import. URI permission is consumed now, never retained for terminal drawing. */
object FontDocumentReader {
    const val LIMIT=4*1024*1024
    private val workers=Semaphore(2)
    suspend fun read(resolver:ContentResolver,uri:Uri):FontDocument=withTimeout(30_000) { suspendCancellableCoroutine { waiting ->
        val signal=CancellationSignal()
        waiting.invokeOnCancellation { Dispatchers.IO.dispatch(EmptyCoroutineContext) { signal.cancel() } }
        Dispatchers.IO.dispatch(EmptyCoroutineContext) {
            var acquired=false
            try {
                if(!waiting.isActive) return@dispatch
                check(workers.tryAcquire()) { "先前的字体读取尚未释放，请稍后重试" };acquired=true
                require(uri.scheme=="content") { "请从系统文件选择器选择字体" }
                val deadline=SystemClock.elapsedRealtime()+30_000
                if(!waiting.isActive) return@dispatch
                val name=resolver.query(uri,arrayOf(OpenableColumns.DISPLAY_NAME),null,null,null,signal)?.use {
                    if(it.moveToFirst()) it.getString(0) else null
                } ?: "导入的字体"
                val bytes=checkNotNull(resolver.openAssetFileDescriptor(uri,"r",signal)) { "无法读取字体文件" }.use { asset ->
                    require(asset.declaredLength<=LIMIT) { "字体文件超过 4 MiB" }
                    asset.createInputStream().use { input ->
                        val fd=input.fd;val regular=Os.fstat(fd).st_mode and OsConstants.S_IFMT==OsConstants.S_IFREG
                        if(regular) {
                            val size=Os.fstat(fd).st_size
                            require(asset.startOffset>=0 && asset.startOffset<=size)
                            require(asset.declaredLength<0 || asset.declaredLength<=size-asset.startOffset) { "字体文件未读取完整" }
                            Os.lseek(fd,asset.startOffset,OsConstants.SEEK_SET)
                        } else require(asset.startOffset==0L) { "字体流不支持偏移" }
                        val flags=FileDescriptorFlags.status(fd);FileDescriptorFlags.status(fd,flags or OsConstants.O_NONBLOCK)
                        try {
                            val buffer=ByteArray(LIMIT+1);var used=0
                            val limit=if(regular && asset.declaredLength>=0) asset.declaredLength.toInt() else buffer.size
                            val poll=StructPollfd().apply { this.fd=fd;events=OsConstants.POLLIN.toShort() }
                            while(used<limit) {
                                if(!waiting.isActive) return@dispatch
                                check(SystemClock.elapsedRealtime()<deadline) { "读取字体超时，请重试" }
                                try {
                                    if(Os.poll(arrayOf(poll),100)==0) continue
                                    val n=Os.read(fd,buffer,used,limit-used)
                                    if(n==0) break
                                    used+=n
                                } catch(e:ErrnoException) { if(e.errno!=OsConstants.EAGAIN && e.errno!=OsConstants.EINTR) throw e }
                            }
                            require(used in 1..LIMIT) { "字体文件为空或超过 4 MiB" }
                            require(asset.declaredLength<0 || used.toLong()==asset.declaredLength) { "字体文件未读取完整" }
                            asset.parcelFileDescriptor.checkError()
                            buffer.copyOf(used)
                        } finally { FileDescriptorFlags.status(fd,flags) }
                    }
                }
                if(waiting.isActive) waiting.resume(FontDocument(name,bytes)) { _,_,_ -> }
            } catch(failure:Exception) { if(waiting.isActive) waiting.resumeWithException(failure) }
            finally { if(acquired) workers.release() }
        }
    } }
}
