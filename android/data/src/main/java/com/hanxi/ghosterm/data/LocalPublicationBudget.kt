package com.hanxi.ghosterm.data

import android.os.ParcelFileDescriptor
import android.os.Process
import android.os.StatFs
import android.system.Os
import android.system.OsConstants
import java.io.Closeable
import java.io.IOException
import java.nio.file.Files
import java.nio.file.Paths
import java.util.UUID

/** Destination candidates only, including interrupted candidates from earlier processes.
 * TransferStagingStore accounts for anonymous sources separately. A reservation spans
 * create/write/publish/cleanup. This grants no ownership or permission to delete a file.
 * Production scope covers every UUID request directory under the application task root,
 * including older requests whose journal records were cleared.
 */
class LocalPublicationBudget(private val byteLimit: Long = 512L*1024*1024,
    private val candidateLimit: Int = 32, private val freeSpaceReserve: Long = 32L*1024*1024,
    private val directoryLimit: Int = 4096) {
    companion object { val SHARED = LocalPublicationBudget() }
    init { require(byteLimit>0 && candidateLimit>0 && freeSpaceReserve>=0 && directoryLimit>0) }
    class Rejected(val systemError: Int, message: String): IOException(message)
    private data class Directory(val device: Long, val inode: Long)
    private data class Candidate(val directory: Directory, val name: String)
    private fun canonical(value: String)=try { UUID.fromString(value).toString()==value } catch(_:IllegalArgumentException) { false }
    private fun identity(fd: ParcelFileDescriptor): Directory {
        val stat=Os.fstat(fd.fileDescriptor)
        check(OsConstants.S_ISDIR(stat.st_mode) && stat.st_uid==Process.myUid() && (stat.st_mode and 511)==448) { "目标暂存目录归属或权限无效" }
        return Directory(stat.st_dev,stat.st_ino)
    }
    private fun child(directory: ParcelFileDescriptor, name: String) =
        com.hanxi.ghosterm.bridge.LocalPublicationFile.childDirectory(directory.fd,name)
    private val lock=Any()
    private val active=mutableMapOf<Directory,MutableMap<Candidate,Long>>()
    private val tasks=mutableSetOf<UUID>()
    /** Protects anonymous-source work as well as target publication. */
    fun beginTask(): Closeable = synchronized(lock) {
        val token=UUID.randomUUID();check(tasks.add(token))
        var closed=false
        Closeable { synchronized(lock) { if(!closed) { check(tasks.remove(token));closed=true } } }
    }
    /** Explicit maintenance never races a cooperating publication reservation. */
    fun <T> whileIdle(root: ParcelFileDescriptor, block: () -> T): T = synchronized(lock) {
        identity(root)
        check(active.values.all { it.isEmpty() }) { "有文件正在复制，请结束后再查看或清理暂存" }
        check(tasks.isEmpty()) { "复制任务仍在读取或释放暂存，请结束后再整理" }
        block()
    }
    fun reserve(directory: ParcelFileDescriptor, name: String, bytes: Long,
        taskRoot: ParcelFileDescriptor? = null): Closeable = synchronized(lock) {
        require(bytes>=0 && canonical(name))
        val destination=identity(directory)
        val key=if(taskRoot==null) destination else identity(taskRoot)
        val token=Candidate(destination,name)
        val current=active[key].orEmpty()
        check(token !in current) { "目标暂存预约重复" }
        var charged=0L;var count=0
        fun charge(size: Long) {
            if(size<0 || size>byteLimit-charged) throw Rejected(OsConstants.ENOSPC,"目标暂存容量不足，请先核对保留的暂存文件")
            if(count>=candidateLimit) throw Rejected(OsConstants.ENOSPC,"目标暂存文件数量已达上限，请先核对保留的暂存文件")
            charged+=size;count++
        }
        current.values.forEach(::charge)
        // Pin every opened directory; do not resolve a replacement pathname.
        var found=false
        fun scanCandidates(fd: ParcelFileDescriptor) {
            val id=identity(fd)
            if(id==destination)found=true
            Files.newDirectoryStream(Paths.get("/proc/self/fd/${fd.fd}")).use { entries ->
                for(entry in entries) {
                    val candidate=entry.fileName.toString()
                    val file=Os.lstat(entry.toString())
                    if(!canonical(candidate) || !OsConstants.S_ISREG(file.st_mode) || file.st_uid!=Process.myUid() ||
                        file.st_nlink!=1L || (file.st_mode and 511)!=384 || file.st_size<0)
                        throw IOException("目标暂存目录包含无法核对的条目，未删除任何文件")
                    if(Candidate(id,candidate) !in current) charge(file.st_size)
                }
            }
        }
        if(taskRoot==null) scanCandidates(directory)
        else {
            var tasks=0
            Files.newDirectoryStream(Paths.get("/proc/self/fd/${taskRoot.fd}")).use { entries ->
                for(entry in entries) {
                    if(++tasks>directoryLimit) throw Rejected(OsConstants.ENOSPC,"保留的任务暂存目录超过核对上限")
                    val task=entry.fileName.toString()
                    check(canonical(task)) { "任务暂存根目录包含未知条目" }
                    child(taskRoot,task).use { owner ->
                        identity(owner)
                        Files.newDirectoryStream(Paths.get("/proc/self/fd/${owner.fd}")).use { children ->
                            for(item in children) {
                                when(val component=item.fileName.toString()) {
                                    "destination" -> child(owner,component).use(::scanCandidates)
                                    "source" -> child(owner,component).use { source ->
                                        identity(source)
                                        Files.newDirectoryStream(Paths.get("/proc/self/fd/${source.fd}")).use { visible ->
                                            check(!visible.iterator().hasNext()) { "匿名来源暂存目录出现未知文件" }
                                        }
                                    }
                                    else -> throw IOException("任务暂存目录包含未知结构，未删除任何文件")
                                }
                            }
                        }
                    }
                }
            }
        }
        check(found) { "当前目标暂存目录不属于任务预算范围" }
        charge(bytes)
        // Existing allocations already reduce availableBytes. Reserving full sizes
        // for other writers is conservative, and never borrows the free-space reserve.
        val available=StatFs("/proc/self/fd/${directory.fd}").availableBytes
        var future=bytes
        for(size in current.values) {
            if(size>Long.MAX_VALUE-future) throw Rejected(OsConstants.ENOSPC,"目标暂存空间预约过大")
            future+=size
        }
        if(available<freeSpaceReserve || future>available-freeSpaceReserve)
            throw Rejected(OsConstants.ENOSPC,"设备可用空间不足，无法创建目标暂存文件")
        active.getOrPut(key) { linkedMapOf() }[token]=bytes
        var closed=false
        Closeable {
            synchronized(lock) {
                if(!closed) {
                    closed=true
                    val owned=checkNotNull(active[key])
                    check(owned.remove(token)==bytes) { "目标暂存预约账目不一致" }
                    if(owned.isEmpty())active.remove(key)
                }
            }
        }
    }
}
