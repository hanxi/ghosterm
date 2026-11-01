package com.hanxi.ghosterm

import android.net.Uri
import android.os.Handler
import android.os.HandlerThread
import android.os.ProxyFileDescriptorCallback
import android.os.storage.StorageManager
import android.content.Context
import java.util.concurrent.atomic.AtomicBoolean
import android.os.ParcelFileDescriptor
import android.system.ErrnoException
import android.system.Os
import android.system.OsConstants
import android.webkit.MimeTypeMap
import com.hanxi.ghosterm.data.TransferStagingStore
import kotlinx.coroutines.*
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlin.coroutines.resumeWithException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.withContext
import java.io.File
import java.io.FileNotFoundException
import java.io.FileOutputStream
import java.util.Locale
import java.util.UUID

/** Private, immutable, read-only external previews. Published copies survive page exits
 * for delayed receivers (24h); open descriptors pin copies until their close callback.
 * No browser roots, transfer candidates or arbitrary paths are exposed by the provider.
 * Each copy <=16 MiB, published/reserved copies <=64 MiB/16 slots, anonymous staging
 * independently <=64 MiB/4 slots (<=128 MiB combined). All IO is off the UI thread.
 */
class ExternalPreviewCache(private val context: Context, private val root: File, private val authority: String,
    private val now: () -> Long = System::currentTimeMillis,
    private val fileLimit: Long = FILE_LIMIT, private val totalLimit: Long = TOTAL_LIMIT,
    private val retentionMillis: Long = RETENTION_MILLIS,
    private val deleteCopy: (File) -> Boolean = File::delete) {
    companion object {
        const val FILE_LIMIT = 16L * 1024 * 1024
        const val TOTAL_LIMIT = 64L * 1024 * 1024
        const val RETENTION_MILLIS = 24L * 60 * 60 * 1000
        private val proxyHandler by lazy { Handler(HandlerThread("ghosterm-preview-read").apply { start() }.looper) }
        private val TOKEN = Regex("[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}")
    }
    init { require(fileLimit>0 && totalLimit>=fileLimit && retentionMillis>0) }
    private val scope=CoroutineScope(SupervisorJob()+Dispatchers.IO)
    private val cleanupFailure=MutableStateFlow<String?>(null)
    val cleanupError=cleanupFailure.asStateFlow()
    fun discardLater(ready: Ready) { scope.launch {
        try { discard(ready) }
        catch(error: Exception) { cleanupFailure.value="预览副本清理失败：${error.message ?: "未知原因"}" }
    } }
    fun sweepLater() { scope.launch {
        try { sweep();cleanupFailure.value=null }
        catch(error: Exception) { cleanupFailure.value="预览缓存清理失败：${error.message ?: "未知原因"}" }
    } }
    private val lock=Any()
    private val writing=mutableSetOf<String>()
    private val readers=mutableMapOf<String,Int>()
    private val readerBytes=mutableMapOf<String,Long>()
    private val staging=TransferStagingStore(File(root,"staging"),fileLimit,totalLimit,snapshotLimit=4)
    private fun initialize() {
        check(root.isDirectory || root.mkdirs()) { "无法创建预览缓存" }
        check(OsConstants.S_ISDIR(Os.lstat(root.path).st_mode)) { "预览缓存目录不是普通目录" }
        Os.chmod(root.path,448)
    }
    private fun copies(): List<File> {
        initialize()
        return checkNotNull(root.listFiles()) { "无法读取预览缓存" }.filter { it.name!="staging" }.onEach {
            val token=it.name.removeSuffix(".part")
            check(TOKEN.matches(token) && OsConstants.S_ISREG(Os.lstat(it.path).st_mode)) { "预览缓存含无法确认的条目" }
        }
    }
    /** Only cache-owned UUID files are swept; a failed unlink is visible and blocks admission. */
    fun sweep() = synchronized(lock) {
        val time=now()
        copies().forEach { file ->
            val token=file.name.removeSuffix(".part")
            if(token !in writing && readers[token]==null &&
                (file.name.endsWith(".part") || time-file.lastModified()>=retentionMillis)) {
                check(deleteCopy(file)) { "无法清理过期预览缓存，请检查设备存储空间" }
            }
        }
    }
    data class Ready(val uri: Uri, val name: String, val mime: String, val bytes: Long)
    suspend fun prepare(name: String, expected: ULong?,
        load: suspend (TransferStagingStore) -> TransferStagingStore.Snapshot): Ready = suspendCancellableCoroutine { waiting ->
        val worker=scope.launch(start=CoroutineStart.LAZY) {
            try {
                val ready=prepareCopy(name,expected,load)
                waiting.resume(ready,onCancellation={ _, copy, _ -> discardLater(copy) })
            } catch(error: Exception) { if(waiting.isActive) waiting.resumeWithException(error) }
        }
        waiting.invokeOnCancellation { worker.cancel() }
        worker.start()
    }
    private suspend fun prepareCopy(name: String, expected: ULong?,
        load: suspend (TransferStagingStore) -> TransferStagingStore.Snapshot): Ready {
        check(expected==null || expected<=fileLimit.toULong()) { "外部打开最多支持 16 MiB，请先复制到设备文件夹" }
        val display=name.takeIf { it.isNotBlank() && it.length<=255 && it.none { c -> c=='/' || c=='\\' || c.code<32 || c.code==127 } } ?: "文件"
        val token=UUID.randomUUID().toString()
        synchronized(lock) {
            sweep()
            val files=copies()
            val named=files.map { it.name }.toSet()
            val missingPins=readerBytes.filterKeys { it !in named }
            val used=files.filter { it.name !in writing && it.name.removeSuffix(".part") !in writing }.sumOf { it.length() }+missingPins.values.sum()
            check(files.count { !it.name.endsWith(".part") }+writing.size+missingPins.size<16 &&
                used+writing.size*fileLimit<=totalLimit-fileLimit) { "预览缓存已满，请等待过期副本释放" }
            writing.add(token)
        }
        val part=File(root,"$token.part");val published=File(root,token)
        var complete=false
        try {
            currentCoroutineContext().ensureActive()
            load(staging).use { snapshot ->
                check(snapshot.size<=fileLimit) { "文件超过预览限额" }
                check(part.createNewFile()) { "无法创建预览副本" }
                Os.chmod(part.path,384)
                FileOutputStream(part).use { output ->
                    snapshot.withDescriptor { fd,size ->
                        val borrowed=ParcelFileDescriptor.fromFd(fd)
                        try {
                            val buffer=ByteArray(32*1024);var count=0L
                            try {
                                while(true) {
                                    currentCoroutineContext().ensureActive()
                                    val n=try { Os.read(borrowed.fileDescriptor,buffer,0,buffer.size) }
                                    catch(error: ErrnoException) { if(error.errno==OsConstants.EINTR) continue;throw error }
                                    if(n==0) break
                                    check(n.toLong()<=fileLimit-count) { "文件超过预览限额" }
                                    output.write(buffer,0,n);count+=n
                                }
                                check(count==size) { "预览副本不完整" }
                            } finally { buffer.fill(0) }
                        } finally { borrowed.close() }
                    }
                }
            }
            currentCoroutineContext().ensureActive()
            val bytes=part.length()
            synchronized(lock) {
                Os.chmod(part.path,256) // 0400; consumers get a separate O_RDONLY descriptor.
                Os.rename(part.path,published.path)
                check(published.setLastModified(now())) { "无法记录预览副本期限" }
                complete=true
            }
            val extension=display.substringAfterLast('.',"").lowercase(Locale.ROOT)
            return Ready(Uri.Builder().scheme("content").authority(authority).appendPath(token).appendPath(display).build(),
                display,MimeTypeMap.getSingleton().getMimeTypeFromExtension(extension) ?: "application/octet-stream",bytes)
        } finally {
            synchronized(lock) {
                writing.remove(token)
                if(!complete) {
                    try {
                        check(!part.exists() || part.delete()) { "无法清理未完成的预览副本" }
                        check(!published.exists() || published.delete()) { "无法清理未完成的预览副本" }
                    } catch(error: Exception) {
                        cleanupFailure.value="预览副本清理失败：${error.message ?: "未知原因"}"
                        throw error
                    }
                }
            }
        }
    }
    private fun token(uri: Uri): String {
        if(uri.scheme!="content" || uri.authority!=authority || uri.query!=null || uri.fragment!=null ||
            uri.pathSegments.size!=2 || !TOKEN.matches(uri.pathSegments[0]) || uri.pathSegments[1].isEmpty())
            throw FileNotFoundException("无效的预览地址")
        return uri.pathSegments[0]
    }
    private fun file(uri: Uri): File {
        val file=File(root,token(uri))
        if(!file.exists() || !OsConstants.S_ISREG(Os.lstat(file.path).st_mode) ||
            now()-file.lastModified()>=retentionMillis) throw FileNotFoundException("预览副本已过期，请重新打开文件")
        return file
    }
    fun info(uri: Uri): Pair<String,Long> = synchronized(lock) { val copy=file(uri);uri.pathSegments.last() to copy.length() }
    /** Seekable proxy: the raw cache inode never leaves this process. A consumer
     * cannot keep unlinked disk bytes alive after this owner process dies. */
    fun open(uri: Uri, mode: String): ParcelFileDescriptor {
        if(mode!="r") throw FileNotFoundException("预览副本只允许读取")
        val (owned,length,id)=synchronized(lock) {
            val file=file(uri)
            val fd=Os.open(file.path,OsConstants.O_RDONLY or OsConstants.O_CLOEXEC or OsConstants.O_NOFOLLOW,0)
            val owned=try { ParcelFileDescriptor.dup(fd) } finally { Os.close(fd) }
            try {
                val stat=Os.fstat(owned.fileDescriptor)
                check(OsConstants.S_ISREG(stat.st_mode) && stat.st_size in 0..fileLimit) { "预览副本已改变" }
                readers[file.name]=(readers[file.name] ?: 0)+1;readerBytes[file.name]=stat.st_size
                Triple(owned,stat.st_size,file.name)
            } catch(error: Exception) { owned.close();throw error }
        }
        val released=AtomicBoolean(false)
        fun release() {
            if(!released.compareAndSet(false,true)) return
            try { owned.close() }
            finally { synchronized(lock) {
                val count=checkNotNull(readers[id])-1
                if(count==0) { readers.remove(id);readerBytes.remove(id) } else readers[id]=count
            } }
        }
        try {
            val storage=checkNotNull(context.getSystemService(StorageManager::class.java)) { "设备不支持只读外部预览" }
            return storage.openProxyFileDescriptor(ParcelFileDescriptor.MODE_READ_ONLY,object: ProxyFileDescriptorCallback() {
                override fun onGetSize()=length
                override fun onRead(offset: Long, size: Int, data: ByteArray): Int {
                    if(offset<0 || size<0 || size>data.size) throw ErrnoException("preview read",OsConstants.EINVAL)
                    if(offset>=length) return 0
                    val needed=minOf(size.toLong(),length-offset).toInt();var count=0
                    while(count<needed) {
                        val read=try { Os.pread(owned.fileDescriptor,data,count,needed-count,offset+count) }
                        catch(error: ErrnoException) { if(error.errno==OsConstants.EINTR) continue;throw error }
                        catch(error: java.io.InterruptedIOException) {
                            if(error.bytesTransferred<0 || error.bytesTransferred>needed-count) throw ErrnoException("preview read",OsConstants.EIO)
                            count+=error.bytesTransferred;continue
                        }
                        if(read==0) throw ErrnoException("preview changed",OsConstants.EIO)
                        count+=read
                    }
                    return count
                }
                override fun onWrite(offset: Long,size: Int,data: ByteArray): Int = throw ErrnoException("read-only preview",OsConstants.EACCES)
                override fun onRelease()=release()
            },proxyHandler)
        } catch(error: Exception) { release();throw error }
    }
    suspend fun discard(ready: Ready) = withContext(Dispatchers.IO) {
        synchronized(lock) {
            val copy=File(root,token(ready.uri))
            check(readers[copy.name]==null) { "预览副本正在使用" }
            check(!copy.exists() || copy.delete()) { "无法删除预览副本" }
        }
    }
}
