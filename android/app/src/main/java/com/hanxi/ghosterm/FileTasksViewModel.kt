package com.hanxi.ghosterm

import android.app.Application
import androidx.compose.runtime.*
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.hanxi.ghosterm.data.*
import com.hanxi.ghosterm.domain.*
import kotlinx.coroutines.*
import java.io.File

/** Application-owned execution; pages never own the task job or its authentication. */
enum class FileTaskPhase { QUEUED, SCANNING, CONFIRMING, RUNNING, CANCELLING, FINISHED }
class FileTask(val request: FilePasteRequest, val clearClipboardAfterSuccess: Boolean = false) {
    var phase by mutableStateOf(FileTaskPhase.QUEUED); internal set
    var decisions by mutableStateOf<FilePasteDecisionState?>(null); internal set
    var createCapability by mutableStateOf(FileCreateCapability.UNKNOWN); internal set
    var namespaceReview by mutableStateOf<FilePasteCheck?>(null); internal set
    var namespaceAcknowledged by mutableStateOf(false); internal set
    var result by mutableStateOf<FileCopyProgress?>(null); internal set
    var error by mutableStateOf<String?>(null); internal set
    var cancelled by mutableStateOf(false); internal set
    internal var decisionOwner: FilePasteDecisions? = null
    internal var execution: FileCopyExecutor.Request? = null
    internal var job: Job? = null
    internal var recorded = false
    internal var recording = false
    var persistenceError by mutableStateOf<String?>(null); internal set
    internal lateinit var connections: (FilePasteRequest) -> FileTaskConnections
    val active get() = phase != FileTaskPhase.FINISHED
}

class FileTaskStore(application: Application, private val scope: CoroutineScope,
    private val onCompleted: (FileTask, FileCopyProgress) -> Unit = { _, _ -> }) {
    companion object { internal const val MAX_REQUESTS = 32 }
    val tasks = mutableStateListOf<FileTask>()
    var notice by mutableStateOf<String?>(null); private set
    val busy get() = tasks.any { it.active || it.recording }
    val history = mutableStateListOf<FileJournalRecord>()
    var backgroundHold by mutableStateOf<String?>(null); private set
    internal fun holdForBackground(reason: String) { backgroundHold=reason }
    internal fun allowBackgroundScheduling() { backgroundHold=null;advance() }
    internal fun stopForBackground(reason: String) {
        holdForBackground(reason)
        tasks.filter { it.active }.toList().forEach(::cancel)
    }
    var journalLoading by mutableStateOf(true); private set
    var journalError by mutableStateOf<String?>(null); private set
    private var contentReadOwner by mutableStateOf<Any?>(null)
    val contentReading get() = contentReadOwner!=null
    internal fun beginContentRead(): Any? {
        if(busy || journalLoading || contentReadOwner!=null) return null
        return Any().also { contentReadOwner=it }
    }
    internal fun endContentRead(reference: Any): Boolean {
        if(contentReadOwner!==reference) return false
        contentReadOwner=null;advance();return true
    }
    private val journal = FileTaskJournal(File(application.filesDir,"file-history/tasks.db"))
    private val initialized = scope.async {
        try { history.addAll(journal.load()) }
        catch(error: Exception) { journalError=error.message ?: "无法读取任务记录" }
        finally { journalLoading=false }
    }
    fun reloadHistory() {
        if(busy || journalLoading) return
        journalLoading=true
        scope.launch {
            try {
                val found=journal.load()
                history.clear();history.addAll(found.filter { record -> tasks.none { it.request.id==record.intent.id } })
                journalError=null
            } catch(error: Exception) { journalError=error.message ?: "无法读取任务记录" }
            finally { journalLoading=false }
        }
    }
    private fun recordFailure(task: FileTask, error: Exception) {
        task.persistenceError="任务记录保存失败：${error.message ?: "未知原因"}。已停止推进，请核对实际位置。"
        journalError=task.persistenceError
    }
    private suspend fun finish(task: FileTask) {
        withContext(NonCancellable) {
            if(task.recorded) try { journal.finish(task.request.id,task.result,task.cancelled,task.error ?: task.persistenceError) }
            catch(error: Exception) { recordFailure(task,error) }
        }
        task.recording=false;task.phase=FileTaskPhase.FINISHED;task.job=null
    }
    private val home = File(application.filesDir,"home")
    private val staging = File(application.filesDir,"file-tasks")
    private val resolver = application.contentResolver
    private val completedState = kotlinx.coroutines.flow.MutableStateFlow<FileCopyProgress?>(null)
    val completed: kotlinx.coroutines.flow.StateFlow<FileCopyProgress?> = completedState
    fun start(snapshot: FileClipboardSnapshot, destination: FileLocation, label: String,
        connections: (FilePasteRequest) -> FileTaskConnections, preferences: AppPreferences = AppPreferences(), retryOf: String? = null) {
        if(journalError!=null) { notice="任务记录不可用，请先在文件任务中核对记录错误";return }
        if(tasks.count { it.active } >= MAX_REQUESTS) {
            notice="待处理请求已达 $MAX_REQUESTS 个，请先完成或取消已有任务";return
        }
        val request=FilePasteRequest.capture(snapshot,destination,label,preferences.conflict,retryOf)
        val task=FileTask(request, preferences.clipboardAfterPaste == ClipboardAfterPaste.CLEAR).also { it.connections=connections }
        notice=null;tasks.add(0,task);task.recording=true
        task.job=scope.launch(start=CoroutineStart.LAZY) {
            try {
                initialized.await()
                check(journalError==null) { journalError ?: "任务记录不可用" }
                journal.accept(request);task.recorded=true
            } catch(error: Exception) { recordFailure(task,error);task.error="任务未开始，未读取或写入文件" }
            finally {
                task.recording=false;task.job=null
                if(task.cancelled || !task.recorded) finish(task)
                advance()
            }
        }.also { it.start() }
    }
    fun queuedAhead(task: FileTask): Int? {
        if(task !in tasks || task.phase!=FileTaskPhase.QUEUED) return null
        return tasks.asReversed().filter { it.active }.indexOf(task).takeIf { it>=0 }
    }
    private fun advance() {
        // Scanning, confirmation, execution and actual cancellation cleanup share one slot.
        if(journalLoading || journalError!=null || backgroundHold!=null) return
        if(contentReadOwner!=null) return
        if(tasks.any { it.active && it.phase!=FileTaskPhase.QUEUED }) return
        val task=tasks.lastOrNull { it.phase==FileTaskPhase.QUEUED } ?: return
        if(!task.recorded || task.recording) return
        task.phase=FileTaskPhase.SCANNING
        scan(task)
    }
    private fun scan(task: FileTask, namespaceConsent: FileNamespaceConsent? = null) {
        val request=task.request
        val connections=task.connections
        task.job=scope.launch(start=CoroutineStart.LAZY) {
            try {
                try { journal.phase(request.id,FileJournalPhase.SCANNING) }
                catch(error: Exception) { if(error !is CancellationException) recordFailure(task,error);throw error }
                val check=withContext(Dispatchers.IO) {
                    connections(request).use { scope ->
                        val local=LocalFileBrowser(home,resolver)
                        suspend fun browse(role: FileTaskConnections.Role, location: FileLocation): BrowserDirectory {
                            val remote=scope.remote(role,location.endpoint)?.session ?: return local.browse(location)
                            val found=remote.browse(location.directoryId)
                            return BrowserDirectory(location.copy(directoryId=found.path),found.entries.map { entry ->
                                BrowserEntry(entry.path,entry.name,when {
                                    entry.symbolicLink->FileKind.LINK;entry.directory->FileKind.DIRECTORY
                                    entry.regularFile->FileKind.FILE;else->FileKind.OTHER
                                },entry.size,entry.modifiedSeconds?.takeIf { it<=Long.MAX_VALUE/1000 }?.times(1000))
                            })
                        }
                        val scan=FileTreeScanner { browse(FileTaskConnections.Role.SOURCE,it) }.scan(request)
                        check(scan.complete) { scan.failure?.message ?: "来源扫描不完整" }
                        val access=FileDestinationAccess(home,resolver) { browse(FileTaskConnections.Role.DESTINATION,it) }
                        FilePastePreflight(access::inspect,access::relation).check(scan,namespaceConsent=namespaceConsent)
                    }
                }
                ensureActive()
                if(check.failure?.kind==FilePasteCheckFailureKind.RELATION_UNKNOWN &&
                    request.source.endpoint.remote!=null && request.destination.endpoint.remote!=null) {
                    try { journal.phase(request.id,FileJournalPhase.CONFIRMING) }
                    catch(error: Exception) { if(error !is CancellationException) recordFailure(task,error);throw error }
                    ensureActive()
                    task.namespaceReview=check;task.phase=FileTaskPhase.CONFIRMING
                    return@launch
                }
                check(check.checked) { check.failure?.message ?: "无法确认复制目标" }
                try { journal.phase(request.id,FileJournalPhase.CONFIRMING) }
                catch(error: Exception) { if(error !is CancellationException) recordFailure(task,error);throw error }
                ensureActive()
                task.createCapability=check.create
                task.namespaceAcknowledged=check.namespaceConsent!=null
                task.decisionOwner=FilePasteDecisions.begin(check)
                task.decisions=task.decisionOwner!!.state();task.phase=FileTaskPhase.CONFIRMING
            } catch(timeout: TimeoutCancellationException) {
                task.error="任务连接超时，请重新确认后重试"
            } catch(cancel: CancellationException) {
                task.cancelled=true;task.error="复制准备已取消"
            } catch(failure: Exception) { task.error=failure.message ?: "无法准备复制" }
            finally {
                task.job=null
                if(task.phase!=FileTaskPhase.CONFIRMING) finish(task)
                advance()
            }
        }.also { it.start() }
    }
    fun confirmNamespace(task: FileTask, review: FilePasteCheck) {
        if(task !in tasks || task.phase!=FileTaskPhase.CONFIRMING || task.namespaceReview!==review ||
            task.cancelled || task.job!=null || journalError!=null) return
        val consent=FileNamespaceConsent.confirm(review)
        task.namespaceReview=null;task.error=null;task.phase=FileTaskPhase.SCANNING
        scan(task,consent)
    }
    fun choose(task: FileTask, ref: FilePasteDecisionRef, path: String, choice: FilePasteChoice, all: Boolean) {
        if(task !in tasks || task.phase!=FileTaskPhase.CONFIRMING) return
        try { task.decisions=checkNotNull(task.decisionOwner).choose(ref,path,choice,all);task.error=null }
        catch(failure: Exception) { task.error=failure.message ?: "确认已失效" }
    }
    fun confirm(task: FileTask, ref: FilePasteDecisionRef) {
        if(task !in tasks || task.phase!=FileTaskPhase.CONFIRMING || journalError!=null) return
        try {
            val plan=checkNotNull(task.decisionOwner).confirm(ref)
            val execution=FileCopyExecutor(home,resolver,staging,task.connections,record={ value,point ->
                journal.checkpoint(task.request.id,value,point)
            }).prepare(plan)
            task.execution=execution;task.decisions=null;task.error=null;task.phase=FileTaskPhase.RUNNING
            task.job=scope.launch(start=CoroutineStart.LAZY) {
                val observer=launch { execution.updates.collect { task.result=it } }
                var began=false
                try {
                    try { journal.begin(task.request.id,execution.outcome.items,plan.id);began=true }
                    catch(error: Exception) { if(error !is CancellationException) recordFailure(task,error);throw error }
                    ensureActive();execution.execute()
                }
                catch(cancel: CancellationException) { execution.cancel() }
                catch(failure: Exception) { execution.cancel();task.error=failure.message ?: "复制未完成" }
                finally {
                    withContext(NonCancellable) { execution.awaitClosed() }
                    observer.cancel();task.result=execution.outcome.takeIf { began }
                    execution.recordingError?.let { task.persistenceError=it;journalError=it }
                    finish(task)
                    task.result?.let { onCompleted(task, it) }
                    completedState.value=task.result
                    advance()
                }
            }.also { it.start() }
        } catch(failure: Exception) { task.error=failure.message ?: "无法开始复制" }
    }
    fun retryPlan(task: FileTask): FileCopyRetry? {
        if(task !in tasks || task.active || task.recording || task.persistenceError!=null) return null
        return task.result?.let { FileCopyRetry.plan(task.request,it) }
    }
    fun retry(task: FileTask, preferences: AppPreferences) {
        val plan=retryPlan(task) ?: return
        if(plan.groups.isEmpty()) { notice="没有可重试项目，请先核对结果不明的项目";return }
        if(journalError!=null || journalLoading) { notice="任务记录不可用，请先核对记录错误";return }
        if(tasks.count { it.active }+plan.groups.size>MAX_REQUESTS) {
            notice="重试需 ${plan.groups.size} 个目录请求，等待队列腾出空间后再重试";return
        }
        // Admission runs on the same owner as start(): check capacity for the
        // entire batch first. Every group receives a new UUID and fresh policy.
        for(group in plan.groups) start(FileClipboardSnapshot.capture(group.source,group.sourceLabel,group.entries),
            group.destination,group.destinationLabel,task.connections,preferences,task.request.id)
    }
    fun historyRetryPlan(record: FileJournalRecord): FileCopyRetry? {
        if(record !in history || journalLoading || journalError!=null) return null
        return FileCopyRetry.plan(record)
    }
    fun retryHistory(record: FileJournalRecord, connections: (FilePasteRequest) -> FileTaskConnections, preferences: AppPreferences) {
        val plan=historyRetryPlan(record) ?: run { notice="历史记录状态已改变，请重新检查后再发起";return }
        if(plan.groups.isEmpty()) { notice="没有可重新发起的项目，请先核对结果不明的项目";return }
        if(tasks.count { it.active }+plan.groups.size>MAX_REQUESTS) {
            notice="需 ${plan.groups.size} 个目录请求，等待队列腾出空间后再重试";return
        }
        for(group in plan.groups) start(FileClipboardSnapshot.capture(group.source,group.sourceLabel,group.entries),
            group.destination,group.destinationLabel,connections,preferences,record.intent.id)
    }
    fun cancelRequest(id: String) { tasks.firstOrNull { it.request.id==id }?.let(::cancel) }
    fun pause(task: FileTask) {
        if(task in tasks && task.phase==FileTaskPhase.RUNNING && !task.cancelled) task.execution?.pause()
    }
    fun continueTask(task: FileTask) {
        if(task in tasks && task.phase==FileTaskPhase.RUNNING && !task.cancelled) task.execution?.resume()
    }
    fun cancel(task: FileTask) {
        if(task !in tasks || !task.active || task.phase==FileTaskPhase.CANCELLING) return
        task.cancelled=true
        if(task.phase==FileTaskPhase.QUEUED || task.phase==FileTaskPhase.CONFIRMING) {
            val queued=task.phase==FileTaskPhase.QUEUED
            task.decisions?.let { task.decisionOwner?.cancel(it.reference) }
            task.decisions=null;task.namespaceReview=null;task.phase=FileTaskPhase.CANCELLING
            task.error=if(queued) "排队任务已取消，尚未读取或写入" else "复制已取消，尚未写入目标"
            // An in-flight admission must finish (or fail) before its cancellation is recorded.
            if(!task.recording) task.job=scope.launch(start=CoroutineStart.LAZY) {
                finish(task);advance()
            }.also { it.start() }
        } else {
            task.phase=FileTaskPhase.CANCELLING
            task.execution?.cancel() ?: task.job?.cancel()
        }
    }
    fun remove(task: FileTask) {
        if(task.active || task.recording) return
        task.recording=true
        scope.launch {
            try { if(task.recorded) journal.remove(task.request.id);tasks.remove(task) }
            catch(error: Exception) { recordFailure(task,error) }
            finally { task.recording=false }
        }
    }
    fun removeHistory(record: FileJournalRecord) {
        if(record !in history || journalLoading || busy || contentReadOwner!=null) return
        journalLoading=true
        scope.launch {
            try { journal.remove(record.intent.id);history.remove(record) }
            catch(error: Exception) { journalError=error.message ?: "无法清除任务记录" }
            finally { journalLoading=false }
        }
    }
}

/** Activity-local navigation over the one application task owner. */
class FileTasksViewModel @JvmOverloads constructor(application: Application,
    internal val work: FileWorkRuntime = (application as? GhostermApplication)?.fileWork ?: FileWorkRuntime(application)
) : AndroidViewModel(application) {
    private val retainedOwner=lazy { LocalRetainedFiles(File(application.filesDir,"file-tasks")) }
    var retainedVisible by mutableStateOf(false); private set
    var retainedLoading by mutableStateOf(false); private set
    var retainedSnapshot by mutableStateOf<LocalRetainedFiles.Snapshot?>(null); private set
    var retainedReview by mutableStateOf<LocalRetainedFiles.Review?>(null); private set
    var emptyRetainedReview by mutableStateOf<LocalRetainedFiles.EmptyReview?>(null); private set
    var retainedError by mutableStateOf<String?>(null); private set
    var retainedNotice by mutableStateOf<String?>(null); private set
    private var retainedRef: LocalRetainedFiles.Ref?=null
    private var retainedJob: Job?=null
    private var retainedEpoch=0L
    val retainedReleaseError get() = work.retainedReleaseError
    fun showRetained() {
        if(busy || journalLoading || inspectingId!=null) return
        retainedVisible=true;refreshRetained()
    }
    fun refreshRetained() = loadRetained(clearError=true)
    private fun loadRetained(clearError:Boolean) {
        if(!retainedVisible || busy || retainedLoading || retainedReview!=null || emptyRetainedReview!=null) return
        val ticket=++retainedEpoch;retainedLoading=true;if(clearError)retainedError=null;retainedSnapshot=null
        retainedJob=viewModelScope.launch {
            try {
                val result=retainedOwner.value.load();ensureActive()
                if(ticket==retainedEpoch)retainedSnapshot=result
            } catch(cancel:CancellationException) { throw cancel }
            catch(error:Exception) { if(ticket==retainedEpoch)retainedError=error.message ?: "无法读取保留暂存" }
            finally { if(ticket==retainedEpoch) { retainedLoading=false;retainedJob=null } }
        }
    }
    fun retainedLabel(item: LocalRetainedFiles.Item):String {
        val matches=history.flatMap { it.items }.map { it.value }.plus(tasks.flatMap { it.result?.items.orEmpty() })
            .filter { it.targetParent?.endpoint==FileEndpoint.APP && it.candidatePath==item.referencePath }.map { it.source.relativePath }.distinct()
        if(matches.isNotEmpty()) return if(matches.size==1) "记录引用：${matches.single()}" else "多条记录引用的暂存位置"
        val plans=history.filter { it.executionPlanId==item.executionPlanId }
        return when(plans.size) {
            0->"未关联记录的暂存"
            1->"任务记录：${plans.single().intent.sourceLabel} → ${plans.single().intent.destinationLabel}"
            else->"多条记录引用的暂存位置"
        }
    }
    fun openRetained(item: LocalRetainedFiles.Item) {
        if(!retainedVisible || busy || retainedLoading || emptyRetainedReview!=null) return
        val ticket=++retainedEpoch;retainedRef=item.ref;retainedLoading=true;retainedError=null;retainedNotice=null
        retainedJob=viewModelScope.launch {
            var delivered=false
            try {
                val review=retainedOwner.value.prepare(item.ref);ensureActive()
                if(ticket==retainedEpoch) { retainedReview=review;delivered=true }
            } catch(cancel:CancellationException) { throw cancel }
            catch(error:Exception) { if(ticket==retainedEpoch)retainedError=error.message ?: "无法核对暂存文件" }
            finally {
                if(!delivered)work.releaseRetained(retainedOwner.value,item.ref)
                if(ticket==retainedEpoch) { retainedLoading=false;retainedJob=null;if(!delivered)retainedRef=null }
            }
        }
    }
    fun dismissRetained() {
        retainedEpoch++;retainedJob?.cancel();retainedJob=null
        retainedRef?.let { work.releaseRetained(retainedOwner.value,it) }
        emptyRetainedReview?.let { work.releaseEmptyRetained(retainedOwner.value,it) }
        emptyRetainedReview=null;retainedRef=null;retainedReview=null;retainedLoading=false
    }
    fun hideRetained() { dismissRetained();retainedVisible=false;retainedSnapshot=null }
    fun clearRetained() {
        val review=retainedReview ?: return
        if(!retainedVisible || busy || retainedLoading) return
        val ticket=++retainedEpoch;retainedLoading=true;retainedError=null
        retainedJob=viewModelScope.launch {
            try {
                val result=retainedOwner.value.clear(review.item.ref)
                if(ticket==retainedEpoch)retainedNotice=if(result==LocalRetainedFiles.Outcome.CLEARED) "所选暂存文件已清除" else "该暂存位置已不存在"
            } catch(error:Exception) { if(ticket==retainedEpoch)retainedError=error.message ?: "无法确认清理结果，请刷新" }
            finally {
                if(ticket==retainedEpoch) {
                    retainedRef=null;retainedReview=null;retainedLoading=false;retainedJob=null
                    loadRetained(clearError=false)
                }
            }
        }
    }
    fun requestEmptyRetained() {
        if(!retainedVisible || busy || retainedLoading || retainedReview!=null || emptyRetainedReview!=null ||
            (retainedSnapshot?.emptyRequestCount ?: 0)==0) return
        val ticket=++retainedEpoch;retainedLoading=true;retainedError=null;retainedNotice=null
        val prepared=java.util.concurrent.atomic.AtomicReference<LocalRetainedFiles.EmptyReview?>()
        retainedJob=viewModelScope.launch {
            var delivered=false
            try {
                val review=retainedOwner.value.prepareEmpty { prepared.set(it) }
                ensureActive()
                if(ticket==retainedEpoch) { emptyRetainedReview=review;delivered=true }
            } catch(cancel:CancellationException) { throw cancel }
            catch(error:Exception) { if(ticket==retainedEpoch)retainedError=error.message ?: "无法核对空暂存目录" }
            finally {
                if(!delivered)prepared.get()?.let { work.releaseEmptyRetained(retainedOwner.value,it) }
                if(ticket==retainedEpoch) { retainedLoading=false;retainedJob=null }
            }
        }
    }
    fun clearEmptyRetained() {
        val review=emptyRetainedReview ?: return
        if(!retainedVisible || busy || retainedLoading) return
        val ticket=++retainedEpoch;retainedLoading=true;retainedError=null
        retainedJob=viewModelScope.launch {
            try {
                val result=retainedOwner.value.clearEmpty(review)
                if(ticket==retainedEpoch) {
                    retainedNotice="已整理 ${result.removed} 个空目录 · ${result.missing} 个位置已不存在"
                    if(result.issues.isNotEmpty())retainedError="${result.issues.size} 项整理未确认，部分空子目录可能已移除；请刷新核对。"+
                        result.issues.take(3).joinToString("\n",prefix="\n")
                }
            } catch(error:Exception) { if(ticket==retainedEpoch)retainedError=error.message ?: "无法确认空目录整理结果，请刷新" }
            finally {
                if(ticket==retainedEpoch) {
                    emptyRetainedReview=null;retainedLoading=false;retainedJob=null
                    loadRetained(clearError=false)
                }
            }
        }
    }
    val presenterId = java.util.UUID.randomUUID()
    val tasks get() = work.tasks.tasks
    val busy get() = work.tasks.busy
    val notice get() = work.tasks.notice
    val history get() = work.tasks.history
    val journalLoading get() = work.tasks.journalLoading
    val journalError get() = work.tasks.journalError
    val background get() = work.background
    fun retryBackground() = work.background.requestFromUser(presenterId)
    fun reloadHistory() { cancelInspection();inspections.clear();contentInspections.clear();identityInspections.clear();inspectionError=null;historyRetryError=null;work.tasks.reloadHistory() }
    fun removeHistory(record: FileJournalRecord) { cancelInspection();inspections.remove(record.intent.id);contentInspections.keys.filter { it.requestId==record.intent.id }.forEach(contentInspections::remove);identityInspections.keys.filter { it.requestId==record.intent.id }.forEach(identityInspections::remove);work.tasks.removeHistory(record) }
    val inspections = mutableStateMapOf<String,FileHistoryInspection>()
    var inspectingId by mutableStateOf<String?>(null); private set
    var inspectionError by mutableStateOf<String?>(null); private set
    private var inspectionJob: Job? = null
    fun cancelInspection() { inspectionJob?.cancel() }
    fun inspectHistory(record: FileJournalRecord) {
        if(busy || journalLoading || inspectingId!=null || record !in history) return
        inspectingId=record.intent.id;inspectionError=null;inspections.remove(record.intent.id)
        inspectionJob=viewModelScope.launch {
            try {
                val app=getApplication<Application>()
                val result=withContext(Dispatchers.IO) {
                    work.authentication.fileHistoryConnections(record.intent.id,record.intent.destination.endpoint,::cancelInspection).use { scope ->
                        val local=LocalFileBrowser(File(app.filesDir,"home"),app.contentResolver)
                        FileHistoryInspector { location ->
                            val remote=scope.remote(FileTaskConnections.Role.DESTINATION,location.endpoint)?.session
                            if(remote==null) local.browse(location) else {
                                val found=remote.browse(location.directoryId)
                                BrowserDirectory(location.copy(directoryId=found.path),found.entries.map { entry ->
                                    BrowserEntry(entry.path,entry.name,when {
                                        entry.symbolicLink->FileKind.LINK;entry.directory->FileKind.DIRECTORY
                                        entry.regularFile->FileKind.FILE;else->FileKind.OTHER
                                    },entry.size,entry.modifiedSeconds?.takeIf { it<=Long.MAX_VALUE/1000 }?.times(1000))
                                })
                            }
                        }.inspect(record)
                    }
                }
                ensureActive()
                if(record in history) inspections[record.intent.id]=result
            } catch(timeout: TimeoutCancellationException) { inspectionError="核对连接超时，请重新发起核对" }
            catch(cancel: CancellationException) { throw cancel }
            catch(error: Exception) { inspectionError=error.message ?: "无法核对历史位置" }
            finally { if(inspectingId==record.intent.id) { inspectingId=null;inspectionJob=null } }
        }
    }
    val contentReading get() = work.tasks.contentReading
    val identityInspections=mutableStateMapOf<FileHistoryContentKey,LocalFileIdentityResult>()
    var inspectingIdentity by mutableStateOf<FileHistoryContentKey?>(null); private set
    fun inspectIdentity(record:FileJournalRecord,item:FileJournalItem,candidate:Boolean) {
        if(!viewModelScope.isActive || busy || journalLoading || inspectingId!=null || record !in history || item !in record.items) return
        val key=FileHistoryContentKey(record.intent.id,item.value.source.relativePath,candidate)
        if(item.value.localIdentity==null) {
            identityInspections[key]=LocalFileIdentityResult(key,LocalFileIdentityState.NO_REFERENCE,error="这条记录没有本机文件创建身份参考")
            return
        }
        val identityReference=work.tasks.beginContentRead()
        if(identityReference==null) {
            identityInspections[key]=LocalFileIdentityResult(key,LocalFileIdentityState.UNKNOWN,error="另一个页面正在核验文件，或复制尚未结束，请稍后重试")
            return
        }
        inspectingId=record.intent.id;inspectingIdentity=key;inspectionError=null;identityInspections.remove(key)
        inspectionJob=viewModelScope.launch(start=CoroutineStart.UNDISPATCHED) {
            try {
                val app=getApplication<Application>()
                val observed=withTimeout(15_000) {
                    LocalFileIdentityReader(File(app.filesDir,"home"),File(app.filesDir,"file-tasks")).inspect(record,item,candidate)
                }
                ensureActive()
                if(record in history)identityInspections[key]=observed
            } catch(timeout:TimeoutCancellationException) {
                ensureActive()
                identityInspections[key]=LocalFileIdentityResult(key,LocalFileIdentityState.UNKNOWN,error="文件身份核对超时，请重新核对")
            } catch(cancel:CancellationException) { throw cancel }
            catch(error:Exception) {
                ensureActive()
                identityInspections[key]=LocalFileIdentityResult(key,LocalFileIdentityState.UNKNOWN,error=error.message ?: "文件身份无法确认")
            } finally {
                work.tasks.endContentRead(identityReference)
                if(inspectingIdentity==key) { inspectingIdentity=null;inspectingId=null;inspectionJob=null }
            }
        }
    }
    val contentInspections=mutableStateMapOf<FileHistoryContentKey,FileHistoryContentResult>()
    var inspectingContent by mutableStateOf<FileHistoryContentKey?>(null); private set
    fun inspectContent(record: FileJournalRecord, item: FileJournalItem, candidate: Boolean) {
        if(!viewModelScope.isActive || busy || journalLoading || inspectingId!=null || record !in history || item !in record.items) return
        val key=FileHistoryContentKey(record.intent.id,item.value.source.relativePath,candidate)
        if(item.value.sourceDigest==null) {
            contentInspections[key]=FileHistoryContentResult(key,FileHistoryContentState.NO_REFERENCE,null,error="这条历史记录没有来源内容摘要，无法比较")
            return
        }
        val readReference=work.tasks.beginContentRead()
        if(readReference==null) {
            contentInspections[key]=FileHistoryContentResult(key,FileHistoryContentState.UNKNOWN,item.value.sourceDigest,error="另一个页面正在核验内容，或复制尚未结束，请稍后重试")
            return
        }
        inspectingId=record.intent.id;inspectingContent=key;inspectionError=null;contentInspections.remove(key)
        inspectionJob=viewModelScope.launch(start=CoroutineStart.UNDISPATCHED) {
            try {
                val app=getApplication<Application>()
                val observed=withTimeout(60_000) { withContext(Dispatchers.IO) {
                    work.authentication.fileHistoryConnections(record.intent.id,record.intent.destination.endpoint,::cancelInspection).use { scope ->
                        val local=LocalFileBrowser(File(app.filesDir,"home"),app.contentResolver)
                        val store=TransferStagingStore(File(app.filesDir,"file-content-inspection"))
                        suspend fun browse(location: FileLocation): BrowserDirectory {
                            val remote=scope.remote(FileTaskConnections.Role.DESTINATION,location.endpoint)?.session ?: return local.browse(location)
                            val found=remote.browse(location.directoryId)
                            return BrowserDirectory(location.copy(directoryId=found.path),found.entries.map { entry ->
                                BrowserEntry(entry.path,entry.name,when {
                                    entry.symbolicLink->FileKind.LINK;entry.directory->FileKind.DIRECTORY
                                    entry.regularFile->FileKind.FILE;else->FileKind.OTHER
                                },entry.size,entry.modifiedSeconds?.takeIf { it<=Long.MAX_VALUE/1000 }?.times(1000))
                            })
                        }
                        try {
                            FileHistoryContentInspector(::browse,read={ location,entry ->
                                val remote=scope.remote(FileTaskConnections.Role.DESTINATION,location.endpoint)?.session
                                val snapshot=if(remote==null) local.snapshot(location,entry,store) else store.receive(remote,entry.id,
                                    entry.size?.let { check(it<=Long.MAX_VALUE.toULong());it.toLong() })
                                snapshot.use { it.contentDigest() }
                            },retained={ path -> LocalRetainedFiles(File(app.filesDir,"file-tasks")).content(path) }).inspect(record,item,candidate)
                        } finally { withContext(NonCancellable) { store.closeAndAwait() } }
                    }
                } }
                ensureActive()
                if(record in history) contentInspections[key]=observed
            } catch(timeout:TimeoutCancellationException) {
                ensureActive()
                contentInspections[key]=FileHistoryContentResult(key,FileHistoryContentState.UNKNOWN,item.value.sourceDigest,error="内容核验超时，请重新发起")
            } catch(cancel:CancellationException) { throw cancel }
            catch(error:Exception) {
                ensureActive()
                contentInspections[key]=FileHistoryContentResult(key,FileHistoryContentState.UNKNOWN,item.value.sourceDigest,error=error.message ?: "内容核验失败")
            } finally {
                // IO owners and the snapshot store have already finished their NonCancellable cleanup.
                work.tasks.endContentRead(readReference)
                if(inspectingContent==key) { inspectingContent=null;inspectingId=null;inspectionJob=null }
            }
        }
    }
    val authentication get() = work.authentication
    val presentsAuthentication get() = work.presenter == presenterId
    var visible by mutableStateOf(false); private set
    fun show() { visible=true }
    fun hide() { hideRetained();cancelInspection();retryTask=null;retryHistory=null;originId=null;visible=false }
    fun start(snapshot: FileClipboardSnapshot, destination: FileLocation, label: String,
        connections: (FilePasteRequest) -> FileTaskConnections = work::connections) {
        visible=true
        work.tasks.start(snapshot,destination,label,connections,work.preferences)
    }
    fun startUserTask(snapshot: FileClipboardSnapshot, destination: FileLocation, label: String) {
        visible=true
        val preferences = work.preferences
        work.background.submitFromUser(presenterId) { work.tasks.start(snapshot,destination,label,work::connections,preferences) }
    }
    var retryTask by mutableStateOf<FileTask?>(null); private set
    fun retryPlan(task: FileTask) = work.tasks.retryPlan(task)
    fun requestRetry(task: FileTask) {
        if(work.tasks.retryPlan(task)?.groups?.isNotEmpty()==true) retryTask=task
    }
    fun dismissRetry() { retryTask=null }
    fun confirmRetry() {
        val task=retryTask ?: return
        retryTask=null
        val preferences=work.preferences
        work.background.submitFromUser(presenterId) { work.tasks.retry(task,preferences) }
    }
    var retryHistory by mutableStateOf<FileJournalRecord?>(null); private set
    var historyRetryError by mutableStateOf<String?>(null); private set
    fun historyRetryPlan(record: FileJournalRecord) = work.tasks.historyRetryPlan(record)
    fun requestHistoryRetry(record: FileJournalRecord) {
        historyRetryError=null
        try { if(historyRetryPlan(record)!=null) retryHistory=record }
        catch(error: Exception) { historyRetryError=error.message ?: "历史记录无法用于重新发起" }
    }
    fun dismissHistoryRetry() { retryHistory=null }
    fun confirmHistoryRetry() {
        val record=retryHistory ?: return
        retryHistory=null
        val preferences=work.preferences
        work.background.submitFromUser(presenterId) { work.tasks.retryHistory(record,work::connections,preferences) }
    }
    var originId by mutableStateOf<String?>(null); private set
    fun showOrigin(id: String) { originId=id }
    fun dismissOrigin() { originId=null }
    fun choose(task: FileTask, ref: FilePasteDecisionRef, path: String, choice: FilePasteChoice, all: Boolean) =
        work.tasks.choose(task,ref,path,choice,all)
    fun confirm(task: FileTask, ref: FilePasteDecisionRef) = work.tasks.confirm(task,ref)
    fun confirmNamespace(task: FileTask, review: FilePasteCheck) = work.tasks.confirmNamespace(task,review)
    fun cancelRequest(id: String) = work.tasks.cancelRequest(id)
    fun pause(task: FileTask) = work.tasks.pause(task)
    fun continueTask(task: FileTask) = work.tasks.continueTask(task)
    fun cancel(task: FileTask) = work.tasks.cancel(task)
    fun remove(task: FileTask) = work.tasks.remove(task)
    fun queuedAhead(task: FileTask) = work.tasks.queuedAhead(task)
    fun resumed() = work.resume(presenterId)
    fun paused() = work.pause(presenterId)
    fun focused() = work.focus(presenterId)
    override fun onCleared() {
        retainedRef?.let { if(retainedOwner.isInitialized())work.releaseRetained(retainedOwner.value,it) }
        emptyRetainedReview?.let { if(retainedOwner.isInitialized())work.releaseEmptyRetained(retainedOwner.value,it) }
        work.release(presenterId);super.onCleared() }
}
