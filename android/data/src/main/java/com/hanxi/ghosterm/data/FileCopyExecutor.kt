package com.hanxi.ghosterm.data

import android.content.ContentResolver
import android.provider.DocumentsContract
import android.net.Uri
import com.hanxi.ghosterm.bridge.*
import com.hanxi.ghosterm.domain.*
import com.hanxi.ghosterm.data.FileTaskConnections.Role
import kotlinx.coroutines.*
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.asStateFlow
import java.io.File
import java.util.UUID
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicReference
import kotlin.coroutines.resume

/** Executes one frozen plan. Connections, source snapshots and partial results belong
 * to the request, never to the browser. No overwrite, directory merge, automatic
 * reconnect, deletion of uncertain targets, or process-death recovery is implied. */
class FileCopyExecutor(private val home: File, private val resolver: ContentResolver,
    private val stagingRoot: File,
    private val connections: (FilePasteRequest) -> FileTaskConnections,
    private val fileLimit: Long = 256L*1024*1024,
    private val renameAttempts: Int = 10000,
    private val record: suspend (FileCopyItemResult, FileCopyCheckpoint) -> Unit = { _,_ -> }) {
    init { require(fileLimit>0 && renameAttempts in 1..10000) }
    fun prepare(plan: FileCopyPlan) = Request(plan)

    inner class Request internal constructor(val plan: FileCopyPlan) {
        private val lifecycle=Any()
        private val started=AtomicBoolean()
        private val cancelled=AtomicBoolean()
        private val finalized=AtomicBoolean()
        private val cancelOperation=AtomicReference<(() -> Unit)?>(null)
        private val closed=CompletableDeferred<Unit>()
        @Volatile private var worker: Job?=null
        private val results=LinkedHashMap<String,FileCopyItemResult>().apply {
            val skips=plan.skipped.associateBy { it.source.relativePath }
            plan.check.scan.entries.forEach { node ->
                put(node.relativePath,FileCopyItemResult(node,
                    if(node.relativePath in skips) FileCopyItemState.SKIPPED else FileCopyItemState.NOT_ATTEMPTED,
                    error=skips[node.relativePath]?.reason?.let { if(it==FilePasteDecisionKind.UNSUPPORTED) "已跳过不支持的条目" else "已跳过同名项目" }))
            }
        }
        private var failure: String?=null
        @Volatile private var cancellationFailure: String?=null
        private var verified=false
        private var activity: FileCopyActivity?=null
        private var activityOwner: Any?=null
        private val mutable=MutableStateFlow(progress(FileCopyState.NOT_STARTED))
        val updates=mutable.asStateFlow()
        val outcome get()=updates.value
        val finished get()=closed.isCompleted
        private fun progress(state: FileCopyState)=FileCopyProgress(plan.request.id,plan.request.clipboardId,state,results.values,failure,activity)
        private fun observe(source: FileScanEntry, stage: FileCopyStage, total: ULong?): (ULong, ULong?) -> Unit {
            val token=Any()
            val began=android.os.SystemClock.elapsedRealtime()
            var previous=0uL
            var emittedAt=began
            synchronized(lifecycle) {
                activityOwner=token
                activity=FileCopyActivity(source.relativePath,stage,0uL,total,began,began)
                mutable.value=progress(FileCopyState.RUNNING)
            }
            return { bytes,knownTotal -> synchronized(lifecycle) {
                if(activityOwner===token && !finalized.get()) {
                    check(bytes>=previous) { "文件进度倒退" };previous=bytes
                    val now=android.os.SystemClock.elapsedRealtime()
                    val next=if(stage==FileCopyStage.WRITING && knownTotal!=null && bytes>=knownTotal) FileCopyStage.VERIFYING else stage
                    val previousActivity=activity
                    activity=FileCopyActivity(source.relativePath,next,bytes,knownTotal,began,now)
                    // Four UI observations per second, plus metadata/phase changes
                    // and the actual final byte count. Never write a journal here.
                    if(now-emittedAt>=250 || previousActivity?.totalBytes!=knownTotal || previousActivity?.stage!=next || knownTotal==bytes) {
                        mutable.value=progress(FileCopyState.RUNNING);emittedAt=now
                    }
                }
            } }
        }
        @Volatile var recordingError: String?=null; private set
        private suspend fun report(value: FileCopyItemResult, point: FileCopyCheckpoint = FileCopyCheckpoint.RESULT) {
            // Memory keeps the actual observation even if its durable write fails.
            synchronized(lifecycle) { results[value.source.relativePath]=value;mutable.value=progress(FileCopyState.RUNNING) }
            recordingError?.let { error(it) }
            try { withContext(NonCancellable) { record(value,point) } }
            catch(error: Exception) {
                recordingError="任务记录保存失败：${error.message ?: "未知原因"}；请核对实际位置后再操作"
                throw error
            }
        }
        private fun active() { if(cancelled.get()) throw CancellationException("复制已取消") }
        fun cancel() {
            synchronized(lifecycle) {
                if(finalized.get() || !cancelled.compareAndSet(false,true)) return
                if(!started.get()) { finish();return }
            }
            // Acquiring connections is cancellable. Side effects run in a retained
            // owner and receive their own explicit cancellation signal instead.
            worker?.cancel()
            cancelOperation.get()?.let { action -> CoroutineScope(Dispatchers.IO).launch {
                try { action() } catch(error: Exception) { cancellationFailure=error.message ?: "取消请求失败" }
            } }
        }
        suspend fun awaitClosed() { closed.await() }
        private suspend fun <T> operation(cancel: () -> Unit, block: suspend () -> T): T {
            active();cancelOperation.set(cancel)
            try { active();return block() } finally { cancelOperation.compareAndSet(cancel,null) }
        }
        suspend fun execute(): FileCopyProgress {
            synchronized(lifecycle) {
                check(!finalized.get() && started.compareAndSet(false,true)) { "复制任务只能执行一次，取消后不能启动" }
            }
            return suspendCancellableCoroutine { waiting ->
                val owner=CoroutineScope(Dispatchers.IO).launch(start=CoroutineStart.LAZY) {
                    // Flush the latest observation even if IO stalls immediately
                    // after a short write inside the 250 ms coalescing window.
                    val ticker=CoroutineScope(Dispatchers.IO).launch {
                        while(isActive) {
                            delay(250)
                            synchronized(lifecycle) {
                                if(!finalized.get() && activity!=mutable.value.activity) mutable.value=progress(FileCopyState.RUNNING)
                            }
                        }
                    }
                    var store: TransferStagingStore?=null
                    try {
                        active()
                        if(!plan.hasWork) return@launch
                        val privateRoot=File(stagingRoot,plan.id)
                        check(privateRoot.canonicalFile!=home.canonicalFile && !privateRoot.canonicalPath.startsWith(home.canonicalPath+File.separator)) { "任务暂存不能位于浏览目录内" }
                        val snapshots=TransferStagingStore(File(privateRoot,"source"),fileLimit,fileLimit)
                        store=snapshots
                        connections(plan.request).use { scope ->
                            // The request retains late results even if execute's caller goes away.
                            withContext(NonCancellable) { copy(scope,snapshots,File(privateRoot,"destination")) }
                        }
                    } catch(cancel: CancellationException) {
                        if(!cancelled.get()) failure=cancel.message ?: "复制中断"
                    } catch(error: Exception) { failure=error.message ?: "复制失败" }
                    finally {
                        withContext(NonCancellable) {
                            try { store?.closeAndAwait() } catch(error: Exception) { failure=failure ?: error.message ?: "暂存释放失败" }
                            finally { ticker.cancelAndJoin() }
                        }
                    }
                }
                worker=owner
                owner.invokeOnCompletion { error ->
                    if(error!=null && error !is CancellationException) failure=failure ?: error.message ?: "复制失败"
                    finish()
                    if(waiting.isActive) waiting.resume(outcome)
                }
                waiting.invokeOnCancellation { cancel() }
                if(cancelled.get()) owner.cancel() else owner.start()
            }
        }
        private fun finish(): Unit = synchronized(lifecycle) {
            if(!finalized.compareAndSet(false,true)) return
            failure=failure ?: cancellationFailure
            val values=results.values
            val state=when {
                values.any { it.state==FileCopyItemState.UNKNOWN } -> FileCopyState.UNKNOWN
                failure!=null -> FileCopyState.FAILED
                verified && values.all { it.state==FileCopyItemState.COMPLETE } -> FileCopyState.COMPLETE
                cancelled.get() -> FileCopyState.CANCELLED
                values.all { it.state==FileCopyItemState.SKIPPED } -> FileCopyState.ALL_SKIPPED
                verified && values.all { it.state==FileCopyItemState.COMPLETE || it.state==FileCopyItemState.SKIPPED } -> FileCopyState.WITH_SKIPS
                else -> FileCopyState.FAILED
            }
            mutable.value=progress(state);closed.complete(Unit);Unit
        }
        private suspend fun copy(scope: FileTaskConnections.Scope, store: TransferStagingStore, localStage: File) {
            val local=LocalFileBrowser(home,resolver)
            suspend fun browse(role: Role, location: FileLocation): BrowserDirectory {
                active()
                val files=scope.remote(role,location.endpoint)?.session ?: return local.browse(location)
                return operation(files::close) {
                    val found=files.browse(location.directoryId)
                    BrowserDirectory(location.copy(directoryId=found.path),found.entries.map { entry ->
                        BrowserEntry(entry.path,entry.name,when {
                            entry.symbolicLink->FileKind.LINK;entry.directory->FileKind.DIRECTORY
                            entry.regularFile->FileKind.FILE;else->FileKind.OTHER
                        },entry.size,entry.modifiedSeconds?.takeIf { it<=Long.MAX_VALUE/1000 }?.times(1000))
                    })
                }
            }
            val access=FileDestinationAccess(home,resolver) { browse(Role.DESTINATION,it) }
            val scanner=FileTreeScanner { browse(Role.SOURCE,it) }
            suspend fun rescan(): FileTreeScan {
                active();val scan=scanner.scan(plan.request)
                check(scan.complete && scan.entries.associateBy { it.relativePath }==plan.check.scan.entries.associateBy { it.relativePath }) { scan.failure?.message ?: "来源已改变，请重新复制" }
                return scan
            }
            val scan=rescan()
            val check=FilePastePreflight(access::inspect,access::relation).check(scan)
            check(check.checked) { check.failure?.message ?: "无法重新确认目标" }
            active()
            val directoryCreator=FileDirectoryCreator(home,resolver) { endpoint -> checkNotNull(scope.remote(Role.DESTINATION,endpoint)) }
            val created=HashMap<String,FileLocation>()
            val roots=plan.roots.associateBy { it.source.relativePath }
            val names=plan.roots.associate { it.source.relativePath to it.destinationName }.toMutableMap()
            val reserved=plan.roots.mapTo(HashSet()) { it.destinationName }
            val skipped=HashSet<String>()
            for(item in plan.entries) {
                active()
                synchronized(lifecycle) { activityOwner=null;activity=null }
                val source=item.source;val root=source.relativePath.substringBefore('/')
                if(root in skipped) { report(FileCopyItemResult(source,FileCopyItemState.SKIPPED,error="已跳过同名目录"));continue }
                val top='/' !in source.relativePath
                val parent=if(top) plan.request.destination else checkNotNull(created[source.relativePath.substringBeforeLast('/')]) { "目标父目录未创建" }
                var name=if(top) names.getValue(root) else source.entry.name
                var result=FileCopyItemResult(source,FileCopyItemState.RUNNING,parent,name)
                report(result)
                var writing=false
                try {
                    val current=browse(Role.SOURCE,source.parent)
                    check(current.location==source.parent && current.entries.singleOrNull { it.id==source.entry.id }==source.entry) { "来源条目已改变，请重新复制" }
                    val target=access.inspect(parent)
                    check(target.directory.location==parent && target.create!=FileCreateCapability.DENIED) { "目标目录已改变或只读" }
                    val occupied=target.directory.entries.mapTo(HashSet()) { it.name }
                    if(name in occupied) {
                        val policy=if(top) roots.getValue(root).onConflict else FileConflictPolicy.ASK
                        when(policy) {
                            FileConflictPolicy.SKIP -> {
                                skipped.add(root);report(result.copy(state=FileCopyItemState.SKIPPED,error="已按本次策略跳过同名项目"));continue
                            }
                            FileConflictPolicy.KEEP_BOTH -> {
                                occupied.addAll(reserved)
                                name=FileCopyNames.next(source.entry.name,source.entry.kind,occupied,renameAttempts)
                                names[root]=name;reserved.add(name);result=result.copy(targetName=name);report(result)
                            }
                            FileConflictPolicy.ASK -> { report(result.copy(state=FileCopyItemState.CONFLICT,error="目标出现同名项目，请重新确认"));return }
                        }
                    }
                    active()
                    if(source.entry.kind==FileKind.DIRECTORY) {
                        report(result,FileCopyCheckpoint.DIRECTORY_CREATE)
                        active()
                        val request=directoryCreator.prepare(parent,name,observed={ out ->
                            result=result.copy(state=if(out.state==FileDirectoryCreator.State.CREATED) FileCopyItemState.CREATED else FileCopyItemState.UNKNOWN,
                                createdId=out.location?.directoryId,documentUri=out.documentUri,targetName=out.actualName ?: name)
                            report(result,FileCopyCheckpoint.DIRECTORY_CREATED)
                        })
                        operation(request::cancel) {
                            try { request.execute() } catch(_: CancellationException) { /* retained outcome below */ }
                            finally { withContext(NonCancellable) { request.awaitClosed() } }
                        }
                        val out=request.outcome
                        val state=when(out.state) {
                            FileDirectoryCreator.State.NOT_ATTEMPTED->FileCopyItemState.NOT_ATTEMPTED
                            FileDirectoryCreator.State.CONFLICT->FileCopyItemState.CONFLICT
                            FileDirectoryCreator.State.REJECTED->FileCopyItemState.FAILED
                            FileDirectoryCreator.State.UNKNOWN->FileCopyItemState.UNKNOWN
                            FileDirectoryCreator.State.CREATED->FileCopyItemState.CREATED
                            FileDirectoryCreator.State.COMPLETE->FileCopyItemState.COMPLETE
                        }
                        result=result.copy(state=state,createdId=out.location?.directoryId,documentUri=out.documentUri,
                            targetName=out.actualName ?: name,error=out.error,coreStatus=out.coreStatus,remoteStatus=out.remoteStatus)
                        report(result)
                        if(state!=FileCopyItemState.COMPLETE) return
                        created[source.relativePath]=checkNotNull(out.location)
                        continue
                    }
                    check(source.entry.kind==FileKind.FILE) { "执行清单包含不支持的条目" }
                    val from=scope.remote(Role.SOURCE,source.parent.endpoint)?.session
                    val reading=observe(source,FileCopyStage.READING,source.entry.size)
                    val snapshot=operation({ store.close();from?.close() }) {
                        if(from==null) local.snapshot(source.parent,source.entry,store) { bytes,total -> reading(bytes.toULong(),total?.toULong()) }
                        else store.receive(from,source.entry.id,source.entry.size?.let { size ->
                            check(size<=Long.MAX_VALUE.toULong());size.toLong()
                        },progress={ reading(it.bytes,it.totalBytes) })
                    }
                    snapshot.use {
                        reading(snapshot.size.toULong(),snapshot.size.toULong())
                        active()
                        val after=browse(Role.SOURCE,source.parent)
                        check(after.location==source.parent && after.entries.singleOrNull { it.id==source.entry.id }==source.entry) { "读取期间来源已改变" }
                        active();writing=true
                        val destination=scope.remote(Role.DESTINATION,parent.endpoint)?.session
                        val writingProgress=observe(source,FileCopyStage.WRITING,snapshot.size.toULong())
                        result=when {
                            destination!=null -> {
                                val candidate=parent.directoryId.trimEnd('/')+"/.ghosterm-copy-"+UUID.randomUUID()
                                result=result.copy(candidatePath=candidate);report(result,FileCopyCheckpoint.REMOTE_CREATE)
                                operation(destination::close) {
                                    snapshot.withDescriptor { fd,size ->
                                        val staged=destination.createExclusive(candidate,fd,size,progress={ writingProgress(it.bytes,it.totalBytes) })
                                        if(staged.state!=RemoteCreationState.COMPLETE) result.copy(
                                            state=when(staged.state) {
                                                RemoteCreationState.NOT_ATTEMPTED->FileCopyItemState.NOT_ATTEMPTED
                                                RemoteCreationState.REJECTED->FileCopyItemState.FAILED
                                                RemoteCreationState.CREATED->FileCopyItemState.CREATED
                                                else->FileCopyItemState.UNKNOWN
                                            },bytes=staged.bytesAcknowledged,coreStatus=staged.status,error="远端暂存未完成（${staged.status}）")
                                        else {
                                            active()
                                            val targetId=parent.directoryId.trimEnd('/')+"/"+name
                                            report(result.copy(state=FileCopyItemState.CREATED,bytes=staged.bytesAcknowledged),FileCopyCheckpoint.PUBLISH)
                                            active()
                                            val published=destination.publish(candidate,targetId,expectedSize=size)
                                            result.copy(state=when(published.state) {
                                                RemotePublicationState.PUBLISHED->FileCopyItemState.COMPLETE
                                                RemotePublicationState.CONFLICT->FileCopyItemState.CONFLICT
                                                RemotePublicationState.UNKNOWN->FileCopyItemState.UNKNOWN
                                                else->FileCopyItemState.FAILED
                                            },createdId=targetId.takeIf { published.state==RemotePublicationState.PUBLISHED },
                                                candidatePath=candidate.takeUnless { published.state==RemotePublicationState.PUBLISHED },
                                                bytes=size.toULong(),coreStatus=published.status,remoteStatus=published.remoteStatus,error=if(published.state==RemotePublicationState.PUBLISHED) null else "远端发布未完成（${published.status}）")
                                        }
                                    }
                                }
                            }
                            parent.endpoint==FileEndpoint.APP -> {
                                val out=LocalFilePublisher(home,localStage,
                                    beforeCreate={ path ->
                                        result=result.copy(candidatePath=path);report(result,FileCopyCheckpoint.LOCAL_CREATE);active()
                                    },beforePublish={ report(result,FileCopyCheckpoint.PUBLISH);active() }
                                ).publish(parent.directoryId,name,snapshot) { bytes,total -> writingProgress(bytes.toULong(),total.toULong());active() }
                                result.copy(state=when(out.state) {
                                    LocalFilePublisher.State.PUBLISHED->FileCopyItemState.COMPLETE
                                    LocalFilePublisher.State.CONFLICT->FileCopyItemState.CONFLICT
                                    LocalFilePublisher.State.REJECTED->FileCopyItemState.FAILED
                                },createdId=if(out.state==LocalFilePublisher.State.PUBLISHED) parent.directoryId.trimEnd('/').let { if(it.isEmpty()) name else "$it/$name" } else null,
                                    candidatePath=if(out.state==LocalFilePublisher.State.PUBLISHED || out.cleanupError==null) null else result.candidatePath,
                                    bytes=snapshot.size.toULong(),error=out.cleanupError ?: if(out.error!=0) "本机发布失败（${out.error}）" else null)
                            }
                            else -> {
                                report(result,FileCopyCheckpoint.DOCUMENT_CREATE)
                                active()
                                val request=SafFilePublisher(resolver) { out ->
                                    result=result.copy(state=if(out.state==SafFilePublisher.State.CREATED) FileCopyItemState.CREATED else FileCopyItemState.UNKNOWN,
                                        documentUri=out.documentUri,targetName=out.actualName ?: name)
                                    report(result,FileCopyCheckpoint.DOCUMENT_CREATED)
                                }.prepare(parent,name)
                                operation(request::cancel) {
                                    try { request.execute(snapshot) { bytes,total -> writingProgress(bytes.toULong(),total.toULong()) } } catch(_: CancellationException) { /* retained outcome below */ }
                                    finally { withContext(NonCancellable) { request.awaitClosed() } }
                                }
                                val out=request.outcome
                                result.copy(state=when(out.state) {
                                    SafFilePublisher.State.NOT_ATTEMPTED->FileCopyItemState.NOT_ATTEMPTED
                                    SafFilePublisher.State.CONFLICT->FileCopyItemState.CONFLICT
                                    SafFilePublisher.State.UNKNOWN->FileCopyItemState.UNKNOWN
                                    SafFilePublisher.State.CREATED->FileCopyItemState.CREATED
                                    SafFilePublisher.State.COMPLETE->FileCopyItemState.COMPLETE
                                },targetName=out.actualName ?: name,documentUri=out.documentUri,
                                    createdId=out.documentUri?.let { runCatching { DocumentsContract.getDocumentId(Uri.parse(it)) }.getOrNull() },
                                    bytes=out.bytesWritten.toULong(),error=out.error)
                            }
                        }
                        report(result)
                        if(result.state!=FileCopyItemState.COMPLETE || result.error!=null) {
                            failure=result.error.takeIf { result.state==FileCopyItemState.COMPLETE };return
                        }
                    }
                } catch(error: Exception) {
                    if(recordingError!=null) { failure=recordingError;return }
                    report(result.copy(state=if(writing) FileCopyItemState.UNKNOWN else if(cancelled.get()) FileCopyItemState.NOT_ATTEMPTED else FileCopyItemState.FAILED,
                        error=error.message ?: "复制失败"))
                    return
                }
            }
            // A tree changed while copying must not become a complete batch merely
            // because every formerly enumerated file was successfully published.
            active();rescan();verified=true
        }
    }
}
