package com.hanxi.ghosterm.data

import android.content.ContentResolver
import android.net.Uri
import android.provider.DocumentsContract as Documents
import android.provider.DocumentsContract.Document
import android.system.ErrnoException
import android.system.Os
import android.system.OsConstants
import com.hanxi.ghosterm.bridge.LocalPublicationFile
import com.hanxi.ghosterm.bridge.RemoteRemovalResult
import com.hanxi.ghosterm.bridge.RemoteRemovalState
import com.hanxi.ghosterm.domain.*
import kotlinx.coroutines.*
import java.io.File
import java.util.UUID
import java.util.concurrent.atomic.AtomicBoolean

/** Explicit review is authority for one new operation, never ownership inferred from history. */
class FileResidualCleanup(private val home:File,private val resolver:ContentResolver,private val journal:FileCleanupJournal) {
    internal data class Stamp(val device:Long,val inode:Long,val size:Long,val mtime:Long,val mnano:Long,val ctime:Long,val cnano:Long)
    class Review internal constructor(internal val owner:UUID,val key:FileHistoryContentKey,val parent:FileLocation,
        val entry:BrowserEntry,val observedMillis:Long,internal val local:Stamp?,internal val localParent:Pair<Long,Long>?) {
        internal val once=AtomicBoolean()
        val attemptId=UUID.randomUUID().toString()
    }
    private val owner=UUID.randomUUID()
    companion object {
        fun eligible(record:FileJournalRecord,item:FileJournalItem,candidate:Boolean):Boolean {
            val parent=item.value.targetParent ?: return false
            return item in record.items && (candidate && item.value.candidatePath!=null && parent.endpoint.remote!=null ||
                !candidate && item.value.state in setOf(FileCopyItemState.CREATED,FileCopyItemState.UNKNOWN,FileCopyItemState.FAILED,FileCopyItemState.RUNNING))
        }
    }
    private fun browser(binding:RemoteFileBinding?):suspend(FileLocation)->BrowserDirectory = { parent ->
        if(parent.endpoint.remote==null) LocalFileBrowser(home,resolver).browse(parent)
        else {
            val remote=checkNotNull(binding);require(remote.endpoint==parent.endpoint)
            val found=remote.session.browse(parent.directoryId)
            BrowserDirectory(parent.copy(directoryId=found.path),found.entries.map { e ->
                BrowserEntry(e.path,e.name,when { e.symbolicLink->FileKind.LINK;e.directory->FileKind.DIRECTORY;e.regularFile->FileKind.FILE;else->FileKind.OTHER },
                    e.size,e.modifiedSeconds?.takeIf { it<=Long.MAX_VALUE/1000 }?.times(1000))
            })
        }
    }
    private fun stamp(fd:java.io.FileDescriptor):Stamp=Os.fstat(fd).let {
        Stamp(it.st_dev,it.st_ino,it.st_size,it.st_mtim.tv_sec,it.st_mtim.tv_nsec,it.st_ctim.tv_sec,it.st_ctim.tv_nsec)
    }
    private fun document(parent:FileLocation,entry:BrowserEntry):Uri {
        val tree=Uri.parse(checkNotNull(parent.endpoint.treeUri))
        check(tree.scheme=="content" && Documents.isTreeUri(tree) && Documents.getTreeDocumentId(tree)==parent.endpoint.rootId)
        check(resolver.persistedUriPermissions.any { it.uri==tree && it.isReadPermission && it.isWritePermission }) { "文件夹读写授权已失效" }
        val root=Documents.buildDocumentUriUsingTree(tree,parent.endpoint.rootId)
        val directory=Documents.buildDocumentUriUsingTree(tree,parent.directoryId)
        val target=Documents.buildDocumentUriUsingTree(tree,entry.id)
        check((directory==root || Documents.isChildDocument(resolver,root,directory)) && target!=root && target!=directory &&
            Documents.isChildDocument(resolver,directory,target)) { "文档不在已授权的目标目录内" }
        check(entry.kind==FileKind.FILE) { "提供方没有非递归删除空目录接口，请在系统文件管理器中整理该目录" }
        resolver.query(target,arrayOf(Document.COLUMN_DOCUMENT_ID,Document.COLUMN_FLAGS),null,null,null).use { cursor ->
            checkNotNull(cursor) { "无法核对清理目标" }
            SafDocumentQuery.requireComplete(cursor)
            check(cursor.moveToFirst() && cursor.getString(0)==entry.id &&
                cursor.getInt(1) and Document.FLAG_SUPPORTS_DELETE!=0 && !cursor.moveToNext()) { "提供方不支持删除这个文档" }
        }
        return target
    }
    suspend fun review(record:FileJournalRecord,item:FileJournalItem,candidate:Boolean,binding:RemoteFileBinding?=null):Review = withContext(Dispatchers.IO) {
        require(eligible(record,item,candidate)) { "该条目没有可清理的未完成位置" }
        val observation=FileHistoryInspector(browser(binding)).inspect(record.copy(items=listOf(item))).observations.single { it.candidate==candidate }
        check(observation.presence==FileHistoryPresence.PRESENT) { observation.error ?: "本次未找到该位置，没有发起删除" }
        val parent=checkNotNull(item.value.targetParent);val entry=checkNotNull(observation.entry)
        check(entry.kind in setOf(FileKind.FILE,FileKind.DIRECTORY)) { "不能通过清理入口删除符号链接或特殊文件" }
        check(entry.modifiedMillis!=null && (entry.kind==FileKind.DIRECTORY || entry.size?.let { it<=Long.MAX_VALUE.toULong() }==true)) { "缺少大小或修改时间，不能确认删除对象" }
        var local:Stamp?=null;var directory:Pair<Long,Long>?=null
        when {
            parent.endpoint.remote!=null -> {
                check(entry.id==parent.directoryId.trimEnd('/')+"/"+entry.name)
                check(checkNotNull(entry.modifiedMillis)/1000 in 0..0xffffffffL)
            }
            parent.endpoint.treeUri!=null -> document(parent,entry)
            else -> {
                require(parent.endpoint==FileEndpoint.APP)
                LocalPublicationFile.directory(home,parent.directoryId).use { fd ->
                    directory=Os.fstat(fd.fileDescriptor).let { it.st_dev to it.st_ino }
                    val selected=if(entry.kind==FileKind.DIRECTORY) LocalPublicationFile.childDirectory(fd.fd,entry.name)
                        else LocalPublicationFile.retainedCandidate(fd.fd,entry.name)
                    selected.use { local=stamp(it.fileDescriptor) }
                }
            }
        }
        currentCoroutineContext().ensureActive()
        Review(owner,FileHistoryContentKey(record.intent.id,item.value.source.relativePath,candidate),parent,entry,System.currentTimeMillis(),local,directory)
    }
    suspend fun remove(review:Review,binding:RemoteFileBinding?=null):FileCleanupRecord=withContext(Dispatchers.IO) {
        require(review.owner==owner && review.once.compareAndSet(false,true)) { "清理确认已失效，请重新检查当前位置" }
        var result=FileCleanupRecord(review.attemptId,review.key.requestId,review.key.relativePath,review.key.candidate,
            review.parent.endpoint.label,review.parent.directoryId,review.entry.id,review.entry.name,review.entry.kind==FileKind.DIRECTORY,
            review.observedMillis,System.currentTimeMillis())
        journal.begin(result) // Must commit before the first deletion side effect.
        fun observed(state:FileCleanupState,status:Int?=null,remote:UInt?=null,error:String?=null) {
            result=result.copy(state=state,status=status,remoteStatus=remote,error=error?.take(4096),updatedMillis=System.currentTimeMillis())
        }
        observed(FileCleanupState.NOT_ATTEMPTED)
        try {
            currentCoroutineContext().ensureActive()
            val fresh=browser(binding)(review.parent)
            check(fresh.location==review.parent) { "目标目录身份已改变" }
            val matches=fresh.entries.filter { it.id==review.entry.id }
            check(matches.size<=1) { "当前位置不唯一" }
            if(matches.isEmpty()) observed(FileCleanupState.ABSENT,0)
            else if(matches.single()!=review.entry) observed(FileCleanupState.CHANGED,3,error="文件信息已改变，请重新检查")
            else when {
                review.parent.endpoint.remote!=null -> {
                    val remote=checkNotNull(binding);require(remote.endpoint==review.parent.endpoint)
                    val request=remote.session.prepareRemoval(review.parent.directoryId,review.entry.name,result.directory,
                        if(result.directory)0 else checkNotNull(review.entry.size).toLong(),checkNotNull(review.entry.modifiedMillis)/1000)
                    fun consume(value:RemoteRemovalResult) {
                        observed(when(value.state) {
                            RemoteRemovalState.NOT_ATTEMPTED->FileCleanupState.NOT_ATTEMPTED;RemoteRemovalState.ABSENT->FileCleanupState.ABSENT
                            RemoteRemovalState.CHANGED->FileCleanupState.CHANGED;RemoteRemovalState.UNKNOWN->FileCleanupState.UNKNOWN
                            RemoteRemovalState.REJECTED->FileCleanupState.REJECTED;RemoteRemovalState.REMOVED->FileCleanupState.REMOVED
                        },value.status,value.remoteStatus,value.error)
                    }
                    try { consume(request.execute()) }
                    finally { withContext(NonCancellable) { request.awaitClosed();consume(request.outcome) } }
                }
                review.parent.endpoint.treeUri!=null -> {
                    val target=document(review.parent,review.entry)
                    currentCoroutineContext().ensureActive()
                    observed(FileCleanupState.UNKNOWN)
                    val acknowledged=Documents.deleteDocument(resolver,target)
                    if(acknowledged) observed(FileCleanupState.REMOVED,0)
                    else observed(FileCleanupState.REJECTED,3,error="提供方没有确认删除")
                }
                else -> {
                    LocalPublicationFile.directory(home,review.parent.directoryId).use { parent ->
                        val identity=Os.fstat(parent.fileDescriptor).let { it.st_dev to it.st_ino }
                        if(identity!=review.localParent) observed(FileCleanupState.CHANGED,3,error="目标目录已改变")
                        else {
                            val selected=if(result.directory)LocalPublicationFile.childDirectory(parent.fd,review.entry.name)
                                else LocalPublicationFile.retainedCandidate(parent.fd,review.entry.name)
                            selected.use { file ->
                                if(stamp(file.fileDescriptor)!=review.local) observed(FileCleanupState.CHANGED,3,error="当前文件身份或内容信息已改变")
                                else {
                                    currentCoroutineContext().ensureActive();observed(FileCleanupState.UNKNOWN)
                                    val path="/proc/self/fd/${parent.fd}/${review.entry.name}"
                                    // Both operations are non-recursive. The pinned parent never follows a later path replacement.
                                    if(result.directory) {
                                        val error=LocalPublicationFile.rmdir(parent.fd,file.fd,review.entry.name)
                                        if(error!=0)throw ErrnoException("rmdir",error)
                                    } else Os.remove(path)
                                    observed(FileCleanupState.REMOVED,0)
                                }
                            }
                        }
                    }
                }
            }
        } catch(cancel:CancellationException) {
            if(result.state!=FileCleanupState.REMOVED) observed(result.state,8,result.remoteStatus,"清理已取消；请核对当前位置")
            throw cancel
        } catch(error:Exception) {
            if(error is ErrnoException && error.errno==OsConstants.ENOENT) observed(FileCleanupState.ABSENT,0)
            else if(error is ErrnoException && result.state==FileCleanupState.UNKNOWN && error.errno in setOf(OsConstants.EACCES,OsConstants.EPERM,OsConstants.EROFS,OsConstants.ENOTEMPTY,OsConstants.EEXIST))
                observed(FileCleanupState.REJECTED,3,error=error.message)
            else observed(result.state,3,result.remoteStatus,error.message ?: "清理结果无法确认")
        } finally {
            // A late cancellation must not erase an acknowledgement or abandon its durable result.
            withContext(NonCancellable) { journal.finish(result) }
        }
        result
    }
}
