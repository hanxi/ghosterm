package com.hanxi.ghosterm.data

import android.os.ParcelFileDescriptor
import android.os.Process
import android.system.ErrnoException
import android.system.Os
import android.system.OsConstants
import android.system.StructStat
import com.hanxi.ghosterm.bridge.LocalPublicationFile
import kotlinx.coroutines.*
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import java.io.File
import java.io.IOException
import java.nio.ByteBuffer
import java.nio.CharBuffer
import java.nio.charset.CodingErrorAction
import java.nio.file.Files
import java.nio.file.Paths
import java.util.Collections
import java.util.UUID

/** Fresh, explicit file maintenance in the app's private destination namespace.
 * A journal path is display context only. No automatic recovery or deletion.
 * Same-UID hostile writers racing private directories remain outside this contract,
 * as for LocalPublicationFile.remove; each selected object is pinned and rechecked.
 */
class LocalRetainedFiles(private val root: File, private val budget: LocalPublicationBudget = LocalPublicationBudget.SHARED,
    private val entryLimit: Int = 4096, private val taskLimit: Int = 4096) {
    class Ref internal constructor(internal val owner: UUID, internal val epoch: Long, internal val id: UUID)
    data class Item(val ref: Ref, val referencePath: String, val size: Long, val modifiedMillis: Long)
    data class Snapshot(val items: List<Item>, val complete: Boolean, val error: String?)
    data class Review(val item: Item, val preview: String?, val truncated: Boolean)
    enum class Outcome { CLEARED, ABSENT }
    private data class Identity(val device: Long, val inode: Long)
    private data class Fingerprint(val identity: Identity, val size: Long, val modifiedSeconds: Long,
        val modifiedNanos: Long, val changedSeconds: Long, val changedNanos: Long)
    private data class Record(val item: Item, val task: String, val candidate: String,
        val root: Identity, val owner: Identity, val directory: Identity, val file: Fingerprint)
    private class Held(val record: Record, val root: ParcelFileDescriptor, val owner: ParcelFileDescriptor,
        val directory: ParcelFileDescriptor, val file: ParcelFileDescriptor) {
        fun close() {
            var error: Exception?=null
            for(fd in listOf(file,directory,owner,root)) try { fd.close() } catch(e:Exception) { if(error==null)error=e else error.addSuppressed(e) }
            error?.let { throw it }
        }
    }
    private val ownerId=UUID.randomUUID()
    private var epoch=0L
    private val records=mutableMapOf<UUID,Record>()
    private val mutex=Mutex()
    private var held: Held?=null
    init { require(entryLimit>0 && taskLimit>0) }
    private fun canonical(value: String)=try { UUID.fromString(value).toString()==value } catch(_:IllegalArgumentException) { false }
    private fun directory(fd: ParcelFileDescriptor):Identity {
        val st=Os.fstat(fd.fileDescriptor)
        check(OsConstants.S_ISDIR(st.st_mode) && st.st_uid==Process.myUid() && (st.st_mode and 511)==448) { "暂存目录归属或权限已改变" }
        return Identity(st.st_dev,st.st_ino)
    }
    private fun fingerprint(st: StructStat): Fingerprint {
        check(OsConstants.S_ISREG(st.st_mode) && st.st_uid==Process.myUid() && st.st_nlink==1L &&
            (st.st_mode and 511)==384 && st.st_size>=0) { "此暂存条目的类型、归属或链接已改变" }
        return Fingerprint(Identity(st.st_dev,st.st_ino),st.st_size,st.st_mtim.tv_sec,st.st_mtim.tv_nsec,st.st_ctim.tv_sec,st.st_ctim.tv_nsec)
    }
    private fun present(): Boolean = try {
        val st=Os.lstat(root.path);check(OsConstants.S_ISDIR(st.st_mode)) { "暂存根目录不是普通目录" };true
    } catch(error:ErrnoException) { if(error.errno==OsConstants.ENOENT)false else throw error }
    private fun closeHeld() { val old=held;held=null;old?.close() }
    private fun record(ref: Ref): Record {
        require(ref.owner==ownerId && ref.epoch==epoch) { "暂存列表已失效，请刷新" }
        return checkNotNull(records[ref.id]?.takeIf { it.item.ref===ref }) { "暂存选择不属于当前列表" }
    }
    suspend fun load(): Snapshot = withContext(Dispatchers.IO) { mutex.withLock {
        closeHeld();epoch++;records.clear()
        val rows=ArrayList<Item>();var failureReason: String?=null
        if(!present())return@withLock Snapshot(emptyList(),true,null)
        LocalPublicationFile.directory(root,"").use { rootFd -> budget.whileIdle(rootFd) {
            val rootIdentity=directory(rootFd)
            var tasks=0;var entries=0
            try {
                Files.newDirectoryStream(Paths.get("/proc/self/fd/${rootFd.fd}")).use { children ->
                    for(entry in children) {
                        ensureActive()
                        check(++tasks<=taskLimit) { "本次读取的任务目录已达上限，列表不完整" }
                        val task=entry.fileName.toString()
                        check(canonical(task)) { "暂存区包含未知任务条目，列表不完整" }
                        LocalPublicationFile.childDirectory(rootFd.fd,task).use { owner ->
                            val ownerIdentity=directory(owner)
                            Files.newDirectoryStream(Paths.get("/proc/self/fd/${owner.fd}")).use { parts ->
                                for(part in parts) {
                                    when(val name=part.fileName.toString()) {
                                        "source" -> LocalPublicationFile.childDirectory(owner.fd,name).use { source ->
                                            directory(source)
                                            Files.newDirectoryStream(Paths.get("/proc/self/fd/${source.fd}")).use { visible ->
                                                check(!visible.iterator().hasNext()) { "来源暂存包含未知条目，列表不完整" }
                                            }
                                        }
                                        "destination" -> LocalPublicationFile.childDirectory(owner.fd,name).use { destination ->
                                            val destinationIdentity=directory(destination)
                                            Files.newDirectoryStream(Paths.get("/proc/self/fd/${destination.fd}")).use { candidates ->
                                                for(candidate in candidates) {
                                                    ensureActive();check(++entries<=entryLimit) { "本次读取的暂存文件已达上限，列表不完整" }
                                                    val filename=candidate.fileName.toString()
                                                    check(canonical(filename)) { "暂存区包含未知文件名，列表不完整" }
                                                    val stamp=fingerprint(Os.lstat(candidate.toString()))
                                                    val ref=Ref(ownerId,epoch,UUID.randomUUID())
                                                    val millis=stamp.modifiedSeconds.takeIf { it>=0 && it<=Long.MAX_VALUE/1000 }?.times(1000) ?: 0
                                                    val item=Item(ref,File(root,"$task/destination/$filename").absolutePath,stamp.size,millis)
                                                    records[ref.id]=Record(item,task,filename,rootIdentity,ownerIdentity,destinationIdentity,stamp);rows+=item
                                                }
                                            }
                                        }
                                        else -> error("暂存区包含未知结构，列表不完整")
                                    }
                                }
                            }
                        }
                    }
                }
            } catch(cancel:CancellationException) { throw cancel }
            catch(failure:Exception) { failureReason=failure.message ?: "未能完整读取暂存" }
        } }
        ensureActive()
        Snapshot(Collections.unmodifiableList(rows.sortedByDescending { it.modifiedMillis }),failureReason==null,failureReason)
    } }
    suspend fun prepare(ref: Ref): Review = withContext(Dispatchers.IO) { mutex.withLock {
        closeHeld();val selected=record(ref)
        val opened=ArrayList<ParcelFileDescriptor>()
        var primary: Throwable?=null
        try {
            fun keep(fd:ParcelFileDescriptor)=fd.also { opened+=it }
            val rootFd=keep(LocalPublicationFile.directory(root,""))
            budget.whileIdle(rootFd) {
                check(directory(rootFd)==selected.root) { "暂存根目录已改变，请刷新" }
                val owner=keep(LocalPublicationFile.childDirectory(rootFd.fd,selected.task))
                check(directory(owner)==selected.owner) { "任务暂存位置已改变，请刷新" }
                val destination=keep(LocalPublicationFile.childDirectory(owner.fd,"destination"))
                check(directory(destination)==selected.directory) { "目标暂存位置已改变，请刷新" }
                val file=keep(LocalPublicationFile.retainedCandidate(destination.fd,selected.candidate))
                check(fingerprint(Os.fstat(file.fileDescriptor))==selected.file) { "暂存文件已改变，请刷新" }
                val size=minOf(selected.item.size,4096).toInt();val bytes=ByteArray(size)
                var used=0
                while(used<size) {
                    ensureActive()
                    val count=try { Os.pread(file.fileDescriptor,bytes,used,size-used,used.toLong()) } catch(e:ErrnoException) {
                        if(e.errno==OsConstants.EINTR)continue;throw e
                    }
                    check(count>0) { "暂存文件提前结束，请刷新" };used+=count
                }
                val preview=try {
                    if(bytes.any { it==0.toByte() })null else {
                        val decoder=Charsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT).onUnmappableCharacter(CodingErrorAction.REPORT)
                        val output=CharBuffer.allocate(size+1)
                        val result=decoder.decode(ByteBuffer.wrap(bytes),output,selected.item.size<=4096)
                        result.throwExceptionIfError();output.flip();output.toString()
                    }
                } catch(_:java.nio.charset.CharacterCodingException) { null } finally { bytes.fill(0) }
                check(fingerprint(Os.fstat(file.fileDescriptor))==selected.file) { "读取时暂存文件发生变化，请刷新" }
                ensureActive()
                held=Held(selected,rootFd,owner,destination,file);opened.clear()
                Review(selected.item,preview,selected.item.size>4096)
            }
        } catch(error:Throwable) { primary=error;throw error }
        finally {
            var cleanup:Exception?=null
            for(fd in opened.asReversed()) try { fd.close() } catch(error:Exception) { if(cleanup==null)cleanup=error else cleanup.addSuppressed(error) }
            cleanup?.let { if(primary!=null)primary.addSuppressed(it) else throw it }
        }
    } }
    private fun java.nio.charset.CoderResult.throwExceptionIfError() { if(isError)throwException() }
    suspend fun dismiss(ref: Ref) = withContext(NonCancellable+Dispatchers.IO) { mutex.withLock {
        if(held?.record?.item?.ref===ref)closeHeld()
    } }
    suspend fun clear(ref: Ref): Outcome = withContext(NonCancellable+Dispatchers.IO) { mutex.withLock {
        val selected=record(ref)
        val current=checkNotNull(held?.takeIf { it.record===selected }) { "请先重新打开并核对暂存文件" }
        try {
            budget.whileIdle(current.root) {
                LocalPublicationFile.directory(root,"").use { freshRoot ->
                    check(directory(freshRoot)==selected.root) { "暂存根目录已改变，未执行清理" }
                    LocalPublicationFile.childDirectory(freshRoot.fd,selected.task).use { freshOwner ->
                        check(directory(freshOwner)==selected.owner) { "任务暂存目录已改变，未执行清理" }
                        LocalPublicationFile.childDirectory(freshOwner.fd,"destination").use { freshDirectory ->
                            check(directory(freshDirectory)==selected.directory) { "目标暂存目录已改变，未执行清理" }
                            val named=try { Os.lstat("/proc/self/fd/${freshDirectory.fd}/${selected.candidate}") }
                            catch(error:ErrnoException) { if(error.errno==OsConstants.ENOENT)null else throw error }
                            if(named==null) Outcome.ABSENT else {
                                check(fingerprint(named)==selected.file && fingerprint(Os.fstat(current.file.fileDescriptor))==selected.file) { "暂存文件已变化，未执行清理" }
                                val code=LocalPublicationFile.remove(freshDirectory.fd,current.file.fd,selected.candidate)
                                check(code==0) { "暂存清理未完成（系统错误 $code）" }
                                check(Os.fstat(current.file.fileDescriptor).st_nlink==0L) { "无法确认所选暂存已清除，请刷新" }
                                Outcome.CLEARED
                            }
                        }
                    }
                }
            }
        } finally { closeHeld();records.remove(ref.id) }
    } }
}
