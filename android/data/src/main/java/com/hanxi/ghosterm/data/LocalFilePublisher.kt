package com.hanxi.ghosterm.data

import android.os.ParcelFileDescriptor
import android.system.ErrnoException
import android.system.Os
import android.system.OsConstants
import com.hanxi.ghosterm.bridge.LocalPublicationFile
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.withContext
import java.io.File
import java.io.IOException
import java.util.UUID

/** Task-owned publication into application HOME. SAF has a separate provider contract.
 * Copies a frozen snapshot into an exclusive file in private staging, closes
 * its writable descriptor, then renames with NOREPLACE. No overwrite fallback.
 * Cancellation before rename leaves no destination; a cancelled result delivery can
 * lose the acknowledgement, so callers must inspect the target before retrying.
 * Blocking filesystem calls are not hard-preemptible. No fsync/durability promise.
 */
class LocalFilePublisher(private val home: File, private val staging: File,
    private val beforeCreate: suspend (String) -> Unit = {},
    private val beforePublish: suspend () -> Unit = {}) {
    enum class State { PUBLISHED, CONFLICT, REJECTED }
    data class Result(val state: State, val error: Int, val cleanupError: String? = null)

    /** checkpoint reports copied bytes, not completion, and may suspend for task pause. */
    suspend fun publish(directoryId: String, name: String, snapshot: TransferStagingStore.Snapshot,
        checkpoint: suspend (Long, Long) -> Unit = { _, _ -> }): Result {
        require(name.isNotEmpty() && name!="." && name!=".." && '/' !in name && '\u0000' !in name) { "无效的目标文件名" }
        return snapshot.withDescriptor { source, size -> withContext(Dispatchers.IO) {
            val cancellation=currentCoroutineContext()
            cancellation.ensureActive()
            var directory: ParcelFileDescriptor?=null
            var stage: ParcelFileDescriptor?=null
            val candidateName=UUID.randomUUID().toString()
            var writer: ParcelFileDescriptor?=null
            var reader: ParcelFileDescriptor?=null
            var result: Result?=null
            var cleanup: Exception?=null
            var primary: Throwable?=null
            try {
                directory=LocalPublicationFile.directory(home,directoryId)
                check(staging.canonicalFile!=home.canonicalFile && !staging.canonicalPath.startsWith(home.canonicalPath+File.separator)) { "目标暂存不能位于浏览目录内" }
                check(staging.isDirectory || staging.mkdirs()) { "无法创建目标暂存目录" }
                check(OsConstants.S_ISDIR(Os.lstat(staging.path).st_mode)) { "目标暂存目录不能是符号链接" }
                Os.chmod(staging.path,448)
                stage=LocalPublicationFile.directory(staging,"")
                beforeCreate(File(staging,candidateName).absolutePath)
                cancellation.ensureActive()
                writer=LocalPublicationFile.candidate(stage.fd,candidateName)
                val readonly=Os.open("/proc/self/fd/${writer.fd}",OsConstants.O_RDONLY or OsConstants.O_CLOEXEC,0)
                try { reader=ParcelFileDescriptor.dup(readonly) } finally { Os.close(readonly) }
                ParcelFileDescriptor.fromFd(source).use { input ->
                    val before=Os.fstat(input.fileDescriptor)
                    check(OsConstants.S_ISREG(before.st_mode) && before.st_size==size) { "暂存文件大小已改变" }
                    val buffer=ByteArray(32*1024)
                    try {
                        var copied=0L
                        checkpoint(0,size)
                        while(copied<size) {
                            cancellation.ensureActive()
                            val read=try { Os.pread(input.fileDescriptor,buffer,0,minOf(buffer.size.toLong(),size-copied).toInt(),copied) }
                            catch(e:ErrnoException) { if(e.errno==OsConstants.EINTR) continue;throw e }
                            check(read>0) { "暂存文件提前结束" }
                            var offset=0
                            while(offset<read) {
                                cancellation.ensureActive()
                                val written=try { Os.write(writer.fileDescriptor,buffer,offset,read-offset) }
                                catch(e:ErrnoException) { if(e.errno==OsConstants.EINTR) continue;throw e }
                                check(written>0) { "目标写入没有进展" };offset+=written
                            }
                            copied+=read
                            checkpoint(copied,size)
                        }
                        val after=Os.fstat(input.fileDescriptor)
                        check(before.st_size==after.st_size && before.st_mtim.tv_sec==after.st_mtim.tv_sec &&
                            before.st_mtim.tv_nsec==after.st_mtim.tv_nsec && before.st_ctim.tv_sec==after.st_ctim.tv_sec &&
                            before.st_ctim.tv_nsec==after.st_ctim.tv_nsec) { "复制时暂存内容发生变化" }
                    } finally { buffer.fill(0) }
                }
                check(Os.fstat(writer.fileDescriptor).st_size==size) { "目标长度不完整" }
                writer.close();writer=null
                cancellation.ensureActive()
                beforePublish()
                cancellation.ensureActive()
                val error=LocalPublicationFile.publish(directory.fd,stage.fd,reader.fd,candidateName,name,size)
                result=Result(when(error) { 0->State.PUBLISHED;OsConstants.EEXIST->State.CONFLICT;else->State.REJECTED },error)
                // Do not turn an acknowledged rename into failure on late cancellation.
            } catch(failure: Throwable) { primary=failure;throw failure } finally {
                val owned=reader?:writer
                if(stage!=null && owned!=null && result?.state!=State.PUBLISHED) try {
                    val error=LocalPublicationFile.remove(stage.fd,owned.fd,candidateName)
                    if(error!=0)cleanup=IOException("目标暂存清理失败（系统错误 $error）")
                } catch(e:Exception) { cleanup=e }
                for(fd in listOf(writer,reader,stage,directory)) try { fd?.close() } catch(e:Exception) { if(cleanup==null)cleanup=e }
                if(cleanup!=null) {
                    if(primary!=null) primary.addSuppressed(cleanup!!)
                    else if(result==null) throw cleanup!!
                }
            }
            checkNotNull(result).copy(cleanupError=cleanup?.message)
        } }
    }
}
