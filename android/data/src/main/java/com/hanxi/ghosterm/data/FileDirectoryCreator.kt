package com.hanxi.ghosterm.data

import android.content.ContentResolver
import android.net.Uri
import android.os.CancellationSignal
import android.provider.DocumentsContract as Documents
import android.provider.DocumentsContract.Document
import android.system.OsConstants
import com.hanxi.ghosterm.bridge.LocalPublicationFile
import com.hanxi.ghosterm.bridge.RemoteDirectoryCreationState
import com.hanxi.ghosterm.bridge.RemoteFileSession
import com.hanxi.ghosterm.domain.*
import kotlinx.coroutines.*
import java.io.File
import java.nio.CharBuffer
import java.util.concurrent.atomic.AtomicBoolean
import kotlin.coroutines.EmptyCoroutineContext
import kotlin.coroutines.resume
import kotlin.coroutines.resumeWithException

/** Creates exactly one new directory. No merge, recursive mkdirs, removal or retry.
 * A result is process-local evidence, not a durable task journal or a directory lease.
 * Remote requests capture a caller-owned binding at preparation, never resolve an
 * alias or switch a connection during execution. Local/provider IO is not bounded
 * by remoteTimeoutMillis, which applies only to the controlled SFTP operation. */
class FileDirectoryCreator(private val home: File, private val resolver: ContentResolver,
    private val remote: ((FileEndpoint) -> RemoteFileBinding)? = null) {
    enum class State { NOT_ATTEMPTED, CONFLICT, REJECTED, UNKNOWN, CREATED, COMPLETE }
    data class Outcome(val state: State = State.NOT_ATTEMPTED, val location: FileLocation? = null,
        val documentUri: String? = null, val actualName: String? = null, val error: String? = null,
        val coreStatus: Int? = null, val remoteStatus: UInt? = null)

    fun prepare(parent: FileLocation, name: String, remoteTimeoutMillis: Int = 5000,
        observed: suspend (Outcome) -> Unit = {}): Request {
        validName(name)
        if(parent.endpoint.remote!=null) {
            val binding=checkNotNull(remote) { "此端点尚不支持受控目录创建" }.invoke(parent.endpoint)
            val request=binding.prepareDirectory(parent,name,remoteTimeoutMillis)
            return Request(parent,name,request,observed)
        }
        require(parent.endpoint==FileEndpoint.APP || (parent.endpoint.treeUri!=null && parent.endpoint.remote==null)) {
            "此端点尚不支持受控目录创建"
        }
        return Request(parent,name,observed=observed)
    }

    /** Cancellation may precede a provider's reply. Always retain this request and
     * awaitClosed before releasing the task slot; a late side effect remains visible. */
    inner class Request internal constructor(val parent: FileLocation, val requestedName: String,
        private val remoteRequest: RemoteFileSession.DirectoryRequest? = null,
        private val observed: suspend (Outcome) -> Unit = {}) {
        @Volatile var outcome = Outcome(); private set
        private val started=AtomicBoolean()
        private val cancelled=AtomicBoolean()
        private val signal=CancellationSignal()
        private val closed=CompletableDeferred<Unit>()
        @Volatile private var worker: Job?=null
        val finished get()=closed.isCompleted
        fun cancel() {
            cancelled.set(true)
            remoteRequest?.cancel()
            if(!started.get()) closed.complete(Unit)
            worker?.cancel()
            Dispatchers.IO.dispatch(EmptyCoroutineContext) { signal.cancel() }
        }
        suspend fun awaitClosed() { closed.await() }
        suspend fun execute(): Outcome {
            check(started.compareAndSet(false,true)) { "目录请求不能重复执行" }
            return suspendCancellableCoroutine { waiting ->
                val owner=CoroutineScope(Dispatchers.IO).launch(start=CoroutineStart.LAZY) {
                    try {
                        active()
                        if(remoteRequest!=null) remoteDirectory() else if(parent.endpoint==FileEndpoint.APP) local() else saf()
                    } catch(failure: Exception) {
                        failed(failure)
                        if(failure is CancellationException) throw failure
                    }
                }
                worker=owner
                owner.invokeOnCompletion { failure ->
                    if(failure!=null) failed(failure)
                    closed.complete(Unit)
                    if(waiting.isActive) {
                        if(failure is CancellationException) waiting.resumeWithException(failure)
                        else waiting.resume(outcome)
                    }
                }
                waiting.invokeOnCancellation { cancel() }
                if(cancelled.get()) owner.cancel() else owner.start()
            }
        }
        private fun failed(failure: Throwable) {
            if(outcome.state!=State.COMPLETE) outcome=outcome.copy(error=
                if(failure is CancellationException) "创建已取消；请核对目标目录" else failure.message ?: "目录创建失败")
        }
        private suspend fun active() { currentCoroutineContext().ensureActive();signal.throwIfCanceled() }
        private suspend fun remoteDirectory() {
            val request=checkNotNull(remoteRequest)
            try { request.execute() }
            finally {
                // Cancellation of this worker must not discard the native reply
                // or release a task slot while the native owner is still cleaning up.
                withContext(NonCancellable) { request.awaitClosed() }
                val result=request.outcome
                val state=when(result.state) {
                    RemoteDirectoryCreationState.NOT_ATTEMPTED -> State.NOT_ATTEMPTED
                    RemoteDirectoryCreationState.CONFLICT -> State.CONFLICT
                    RemoteDirectoryCreationState.UNKNOWN -> State.UNKNOWN
                    RemoteDirectoryCreationState.REJECTED -> State.REJECTED
                    RemoteDirectoryCreationState.CREATED -> State.CREATED
                    RemoteDirectoryCreationState.COMPLETE -> State.COMPLETE
                }
                val created=state==State.CREATED || state==State.COMPLETE
                outcome=Outcome(state,if(created) parent.copy(directoryId=request.path) else null,
                    actualName=if(created) requestedName else null,error=result.error,
                    coreStatus=result.status,remoteStatus=result.remoteStatus)
            }
        }
        private suspend fun local() {
            LocalPublicationFile.directory(home,parent.directoryId).use { pinned ->
                active()
                outcome=Outcome(State.UNKNOWN)
                val error=LocalPublicationFile.mkdir(pinned.fd,requestedName)
                if(error!=0) {
                    outcome=Outcome(if(error==OsConstants.EEXIST) State.CONFLICT else State.REJECTED,
                        error="目录创建失败（系统错误 $error）")
                    return
                }
                val id=if(parent.directoryId.isEmpty()) requestedName else parent.directoryId+"/"+requestedName
                outcome=Outcome(State.CREATED,parent.copy(directoryId=id),actualName=requestedName)
                withContext(NonCancellable) { observed(outcome) }
                active()
                // Verify through the same pinned parent; do not follow a replacement symlink.
                LocalPublicationFile.directory(File("/proc/self/fd/${pinned.fd}/."),requestedName).use { child ->
                    check(File("/proc/self/fd/${child.fd}").list()?.isEmpty()==true) { "新目录不是空目录" }
                }
                active()
                outcome=outcome.copy(state=State.COMPLETE)
            }
        }
        private suspend fun saf() {
            val endpoint=parent.endpoint
            val tree=Uri.parse(checkNotNull(endpoint.treeUri))
            check(tree.scheme=="content" && !tree.authority.isNullOrEmpty() && Documents.isTreeUri(tree)) { "无效的文档树" }
            check(Documents.getTreeDocumentId(tree)==endpoint.rootId) { "文档树身份已改变" }
            check(resolver.persistedUriPermissions.any { it.uri==tree && it.isReadPermission && it.isWritePermission }) { "目录读写授权已失效" }
            val root=Documents.buildDocumentUriUsingTree(tree,endpoint.rootId)
            val uri=Documents.buildDocumentUriUsingTree(tree,parent.directoryId)
            check(root==uri || Documents.isChildDocument(resolver,root,uri)) { "目标不在授权文档树内" }
            val parentInfo=info(uri)
            check(parentInfo.directory && parentInfo.flags and Document.FLAG_DIR_SUPPORTS_CREATE!=0) { "目标目录不支持创建" }
            val existing=children(tree,parent.directoryId)
            if(requestedName in existing.values) { outcome=Outcome(State.CONFLICT);return }
            active()
            outcome=Outcome(State.UNKNOWN)
            val created=Documents.createDocument(resolver,uri,Document.MIME_TYPE_DIR,requestedName)
                ?: error("创建结果未知；请核对目标目录")
            outcome=outcome.copy(documentUri=created.toString())
                withContext(NonCancellable) { observed(outcome) }
            check(created.scheme=="content" && created.authority==tree.authority && Documents.isTreeUri(created) &&
                Documents.getTreeDocumentId(created)==endpoint.rootId) { "提供方返回了其他文档树" }
            val id=Documents.getDocumentId(created)
            check(id !in existing && id!=endpoint.rootId && id!=parent.directoryId) { "提供方未返回新目录" }
            outcome=outcome.copy(state=State.CREATED,location=parent.copy(directoryId=id))
                withContext(NonCancellable) { observed(outcome) }
            active()
            check(Documents.isChildDocument(resolver,uri,created)) { "新目录不在目标目录内" }
            val actual=info(created)
            check(actual.directory && actual.flags and Document.FLAG_VIRTUAL_DOCUMENT==0) { "新条目不是普通目录" }
            validName(actual.name)
            outcome=outcome.copy(actualName=actual.name)
                withContext(NonCancellable) { observed(outcome) }
            check(children(tree,id).isEmpty()) { "新目录不是空目录" }
            check(children(tree,parent.directoryId)[id]==actual.name) { "新目录不在指定的直接父目录下" }
            active()
            outcome=outcome.copy(state=State.COMPLETE)
        }
        private fun info(uri: Uri): Info {
            signal.throwIfCanceled()
            val columns=arrayOf(Document.COLUMN_DOCUMENT_ID,Document.COLUMN_DISPLAY_NAME,Document.COLUMN_MIME_TYPE,Document.COLUMN_FLAGS)
            return checkNotNull(resolver.query(uri,columns,null,null,null,signal)) { "无法读取目录信息" }.use { cursor ->
                check(cursor.moveToFirst() && cursor.getString(0)==Documents.getDocumentId(uri)) { "目录身份不一致" }
                check(!cursor.isNull(1) && !cursor.isNull(2) && !cursor.isNull(3)) { "目录信息不完整" }
                val result=Info(cursor.getString(1),cursor.getString(2)==Document.MIME_TYPE_DIR,cursor.getInt(3))
                check(!cursor.moveToNext()) { "目录身份不唯一" }
                result
            }
        }
        private fun children(tree: Uri, id: String): Map<String,String> {
            signal.throwIfCanceled()
            val uri=Documents.buildChildDocumentsUriUsingTree(tree,id)
            return checkNotNull(resolver.query(uri,arrayOf(Document.COLUMN_DOCUMENT_ID,Document.COLUMN_DISPLAY_NAME),null,null,null,signal)) { "无法检查目录内容" }.use { cursor ->
                val result=linkedMapOf<String,String>();val names=hashSetOf<String>();var bytes=0L
                while(cursor.moveToNext()) {
                    signal.throwIfCanceled()
                    check(result.size<4096) { "目录条目超过检查上限" }
                    val child=checkNotNull(cursor.getString(0));val name=checkNotNull(cursor.getString(1))
                    check(child.isNotEmpty()) { "文档身份无效" };validName(name)
                    bytes+=child.toByteArray(Charsets.UTF_8).size.toLong()+name.toByteArray(Charsets.UTF_8).size
                    check(bytes<=4*1024*1024) { "目录元数据超限" }
                    check(result.put(child,name)==null && names.add(name)) { "目录返回重复身份或名称" }
                }
                result
            }
        }
    }
    private data class Info(val name: String,val directory: Boolean,val flags: Int)
    private fun validName(name: String) {
        require(name.isNotEmpty() && name!="." && name!=".." && '/' !in name && '\u0000' !in name) { "无效的目录名称" }
        require(Charsets.UTF_8.newEncoder().encode(CharBuffer.wrap(name)).remaining()<=255) { "目录名称超过 255 UTF-8 字节" }
    }
}
