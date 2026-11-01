package com.hanxi.ghosterm

import android.app.Application
import android.database.sqlite.SQLiteDatabase
import androidx.activity.ComponentActivity
import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.*
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.createAndroidComposeRule
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.hanxi.ghosterm.data.*
import com.hanxi.ghosterm.domain.*
import kotlinx.coroutines.runBlocking
import org.junit.*
import org.junit.Assert.*
import org.junit.runner.RunWith
import java.io.File
import java.util.UUID
import java.util.concurrent.atomic.AtomicInteger

@RunWith(AndroidJUnit4::class)
class FileJournalProductTest {
    @get:Rule val ui=createAndroidComposeRule<ComponentActivity>()
    private val context=InstrumentationRegistry.getInstrumentation().targetContext
    private lateinit var root:File
    private lateinit var home:File
    private lateinit var application:Application
    private lateinit var snapshot:FileClipboardSnapshot
    private lateinit var path:File
    private lateinit var journal:FileTaskJournal
    private var page by mutableStateOf<FileTasksViewModel?>(null)
    private val calls=AtomicInteger()
    private val target=FileLocation(FileEndpoint.APP,"target")
    @Before fun setup() {
        root=File(context.cacheDir,"journal-ui-${UUID.randomUUID()}").apply { mkdirs() }
        home=File(root,"home").apply { mkdir() };File(home,"source").mkdir();File(home,"target").mkdir()
        File(home,"source/a.txt").writeText("persistent UI bytes")
        application=object:Application() { init { attachBaseContext(context) };override fun getFilesDir()=root }
        path=File(root,"file-history/tasks.db");journal=FileTaskJournal(path)
        snapshot=runBlocking { FileClipboardSnapshot.capture(FileLocation(FileEndpoint.APP,"source"),"source",
            LocalFileBrowser(home,context.contentResolver).browse(FileLocation(FileEndpoint.APP,"source")).entries) }
        ui.setContent { MaterialTheme { page?.let { FileTasksScreen(it) } } }
    }
    @After fun cleanup() {
        ui.runOnIdle { page?.tasks?.toList()?.forEach { page!!.cancel(it) } }
        ui.waitUntil(15000) { page?.busy!=true };root.deleteRecursively()
    }
    private fun open():FileTasksViewModel {
        val model=ui.runOnIdle { FileTasksViewModel(application,FileWorkRuntime(application)).also { page=it } }
        ui.waitUntil(10000) { !model.journalLoading };return model
    }
    private fun connections(request:FilePasteRequest):FileTaskConnections {
        calls.incrementAndGet()
        return FileTaskConnections(request.source.endpoint,request.destination.endpoint,{ _,_->error("local only") },{ _,_,_->error("no auth") })
    }
    private fun waitDone(task:FileTask) { ui.waitUntil(15000) { task.phase==FileTaskPhase.FINISHED } }
    @Test fun sharedContentLeaseQueuesCopiesAndRejectsAnotherReaderOrLateRelease() {
        val old=FilePasteRequest.capture(snapshot,target,"old target")
        runBlocking { journal.load();journal.accept(old) }
        val model=open();val store=model.work.tasks
        val oldRecord=model.history.single()
        val references=mutableListOf<Any>()
        try {
            val first=ui.runOnIdle { checkNotNull(store.beginContentRead()).also(references::add) }
            val second=ui.runOnIdle { store.beginContentRead()?.also(references::add) }
            assertNull("only one reader may reserve the app's temporary budget",second)
            ui.runOnIdle { model.removeHistory(oldRecord) }
            ui.waitUntil(10000) { !model.journalLoading }
            assertTrue(model.history.contains(oldRecord))
            assertTrue(runBlocking { journal.load().any { it.intent.id==old.id } })
            ui.runOnIdle { model.start(snapshot,target,"target",::connections) }
            val task=model.tasks.single()
            ui.waitUntil(10000) { !task.recording }
            ui.runOnIdle {
                assertEquals(FileTaskPhase.QUEUED,task.phase);assertEquals(0,calls.get())
                assertTrue(File(home,"target").list()!!.isEmpty())
                assertFalse(store.endContentRead(Any()));assertTrue(store.contentReading)
                assertTrue(store.endContentRead(first))
            }
            ui.waitUntil(10000) { task.phase==FileTaskPhase.CONFIRMING }
            assertEquals(1,calls.get());assertTrue(File(home,"target").list()!!.isEmpty())
            ui.runOnIdle { model.cancel(task) };waitDone(task)
            ui.runOnIdle {
                val next=checkNotNull(store.beginContentRead()).also(references::add)
                assertFalse(store.endContentRead(first));assertTrue(store.contentReading)
                assertTrue(store.endContentRead(next));assertFalse(store.contentReading)
            }
        } finally { ui.runOnIdle { references.asReversed().forEach { store.endContentRead(it) } } }
    }
    @Test fun privateCandidateContentUsesReadonlyNamespaceAndDoesNotChangeUnknownHistory() {
        val request=FilePasteRequest.capture(snapshot,target,"target")
        val plan=File(root,"file-tasks/${UUID.randomUUID()}")
        val destination=File(plan,"destination")
        destination.mkdirs();android.system.Os.chmod(File(root,"file-tasks").path,448)
        android.system.Os.chmod(plan.path,448);android.system.Os.chmod(destination.path,448)
        val candidate=File(destination,UUID.randomUUID().toString())
        val bytes=File(home,"source/a.txt").readBytes();candidate.writeBytes(bytes);android.system.Os.chmod(candidate.path,384)
        val digest=FileContentDigest(bytes.size.toLong(),java.security.MessageDigest.getInstance("SHA-256").digest(bytes).joinToString("") { "%02x".format(it.toInt() and 255) })
        val item=FileCopyItemResult(FileScanEntry(snapshot.source,snapshot.items.single(),"a.txt"),FileCopyItemState.UNKNOWN,
            target,"a.txt",candidatePath=candidate.absolutePath,sourceDigest=digest)
        runBlocking { journal.load();journal.accept(request);journal.phase(request.id,FileJournalPhase.CONFIRMING);journal.begin(request.id,listOf(item)) }
        val before=runBlocking { journal.load() };val model=open()
        ui.onNodeWithTag("file-history-details").performScrollTo().performClick()
        ui.onNodeWithTag("file-history-content:a.txt:true").performScrollTo().performClick()
        ui.waitUntil(10000) { model.inspectingId==null && model.contentInspections.isNotEmpty() }
        ui.onNodeWithTag("file-history-content-result").assertTextEquals("内容与复制时的来源一致")
        assertArrayEquals(bytes,candidate.readBytes());assertEquals(before,runBlocking { journal.load() })
        ui.runOnIdle { assertFalse(model.contentReading) }
        assertTrue(candidate.delete())
        ui.onNodeWithTag("file-history-content:a.txt:true").performScrollTo().performClick()
        ui.waitUntil(10000) { model.inspectingId==null && model.contentInspections.isNotEmpty() }
        ui.onNodeWithTag("file-history-content-result").assertTextEquals("本次读取未找到该文件")
        assertEquals(before,runBlocking { journal.load() });assertEquals(FileCopyItemState.UNKNOWN,model.history.single().items.single().value.state)
    }
    @Test fun explicitContentVerificationShowsMatchAndDifferenceWithoutChangingOldResult() {
        val request=FilePasteRequest.capture(snapshot,target,"target")
        val bytes=File(home,"source/a.txt").readBytes()
        val digest=FileContentDigest(bytes.size.toLong(),java.security.MessageDigest.getInstance("SHA-256").digest(bytes).joinToString("") { "%02x".format(it.toInt() and 255) })
        val item=FileCopyItemResult(FileScanEntry(snapshot.source,snapshot.items.single(),"a.txt"),FileCopyItemState.UNKNOWN,
            target,"a.txt",createdId="target/a.txt",sourceDigest=digest)
        runBlocking { journal.load();journal.accept(request);journal.phase(request.id,FileJournalPhase.CONFIRMING);journal.begin(request.id,listOf(item)) }
        File(home,"target/a.txt").writeBytes(bytes)
        val before=runBlocking { journal.load() };val model=open()
        ui.onNodeWithTag("file-history-details").performScrollTo().performClick()
        ui.onNodeWithTag("file-history-content:a.txt:false").performScrollTo().performClick()
        ui.runOnIdle { assertTrue("核验按钮必须启动本条记录的检查",model.inspectingId==request.id || model.contentInspections.keys.any { it.requestId==request.id }) }
        ui.waitUntil(10000) { model.inspectingId==null && model.contentInspections.isNotEmpty() }
        ui.onNodeWithTag("file-history-content-result").assertTextEquals("内容与复制时的来源一致")
        assertEquals(FileHistoryContentState.MATCH,model.contentInspections.values.single().state)
        ui.runOnIdle { assertFalse("completed IO must release the shared reader slot",model.contentReading) }
        File(home,"target/a.txt").writeText("different bytes")
        ui.onNodeWithTag("file-history-content:a.txt:false").performScrollTo().performClick()
        ui.waitUntil(10000) { model.inspectingId==null && model.contentInspections.values.singleOrNull()?.state==FileHistoryContentState.DIFFERENT }
        ui.onNodeWithTag("file-history-content-result").assertTextEquals("内容与复制时的来源不一致")
        assertEquals(before,runBlocking { journal.load() });assertEquals(FileCopyItemState.UNKNOWN,model.history.single().items.single().value.state)
        assertEquals("different bytes",File(home,"target/a.txt").readText());assertEquals(0,calls.get())
    }
    @Test fun reopenedQueuedIntentIsReadOnlyAndClearingHistoryNeverStartsIt() {
        val request=FilePasteRequest.capture(snapshot,target,"target")
        runBlocking { journal.load();journal.accept(request) }
        val model=open()
        assertTrue(model.tasks.isEmpty());assertEquals(1,model.history.size);assertEquals(request.id,model.history.single().intent.id)
        ui.onNodeWithTag("file-history-status").assertTextEquals("已中断 · 结果待核对")
        ui.onNodeWithTag("file-task-confirm").assertDoesNotExist()
        ui.onNodeWithTag("file-task-cancel").assertDoesNotExist()
        assertTrue(File(home,"target").list()!!.isEmpty());assertEquals(0,calls.get())
        ui.onNodeWithTag("file-history-remove").performClick();ui.onNodeWithText("清除记录",useUnmergedTree=true).performClick()
        ui.waitUntil(10000) { model.history.isEmpty() && !model.journalLoading }
        assertTrue(runBlocking { journal.load().isEmpty() });assertTrue(File(home,"source/a.txt").isFile)
    }
    @Test fun explicitHistoryInspectionPreservesOriginalUnknownAndJournal() {
        val request=FilePasteRequest.capture(snapshot,target,"target")
        val item=FileCopyItemResult(FileScanEntry(snapshot.source,snapshot.items.single(),"a.txt"),
            FileCopyItemState.UNKNOWN,target,"a.txt",candidatePath="target/.candidate")
        runBlocking {
            journal.load();journal.accept(request)
            journal.phase(request.id,FileJournalPhase.CONFIRMING);journal.begin(request.id,listOf(item))
        }
        File(home,"target/a.txt").writeText("unrelated current bytes")
        File(home,"target/.candidate").writeText("partial")
        val before=runBlocking { journal.load() }
        val model=open()
        assertTrue(model.inspections.isEmpty());assertEquals(0,calls.get())
        ui.onNodeWithTag("file-history-inspect").performScrollTo().performClick()
        ui.waitUntil(10000) { model.inspectingId==null && model.inspections.isNotEmpty() }
        assertEquals(listOf(FileHistoryPresence.PRESENT,FileHistoryPresence.PRESENT),model.inspections.values.single().observations.map { it.presence })
        ui.onNodeWithTag("file-history-inspection-summary").assertTextEquals("存在 2 · 未找到 0 · 无法核对 0")
        assertEquals(before,runBlocking { journal.load() })
        assertEquals("unrelated current bytes",File(home,"target/a.txt").readText())
        assertEquals("partial",File(home,"target/.candidate").readText())
    }
    @Test fun explicitHistoricalRetryCreatesLinkedRequestAndPreservesOldRecord() {
        val old=FilePasteRequest.capture(snapshot,target,"target")
        runBlocking { journal.load();journal.accept(old) }
        val before=runBlocking { journal.load().single() }
        val model=open()
        ui.onNodeWithTag("file-history-retry").performScrollTo().performClick()
        assertTrue(model.tasks.isEmpty());assertEquals(0,calls.get())
        ui.onNodeWithTag("file-history-retry-confirm").performClick()
        ui.waitUntil(10000) { model.tasks.singleOrNull()?.phase in setOf(FileTaskPhase.CONFIRMING,FileTaskPhase.FINISHED) }
        val task=model.tasks.single()
        assertEquals(task.error,FileTaskPhase.CONFIRMING,task.phase)
        assertNotEquals(old.id,task.request.id);assertEquals(old.id,task.request.retryOf)
        assertEquals(FileConflictPolicy.ASK,task.request.conflictPolicy)
        assertEquals(before,runBlocking { journal.load().single { it.intent.id==old.id } })
        ui.onNodeWithTag("file-task-origin").performScrollTo().performClick()
        ui.onNodeWithText("原记录已保留，不受重新发起的结果影响。").assertExists()
        ui.onNodeWithText("返回任务").performClick()
        ui.onNodeWithTag("file-task-confirm").performScrollTo().performClick();waitDone(task)
        assertEquals(FileCopyState.COMPLETE,task.result!!.state)
        assertEquals("persistent UI bytes",File(home,"target/a.txt").readText())
        val reopened=open()
        assertEquals(old.id,reopened.history.single { it.intent.id==task.request.id }.intent.retryOf)
        assertEquals(before,reopened.history.single { it.intent.id==old.id })
        runBlocking { journal.remove(old.id) };ui.runOnIdle { reopened.reloadHistory() }
        ui.waitUntil(10000) { !reopened.journalLoading }
        ui.onNodeWithTag("file-history-origin").performScrollTo().performClick()
        ui.onNodeWithText("原任务记录已清除；这条任务保留了原任务的关联。").assertExists()
        ui.onNodeWithText("返回任务").performClick()
        assertEquals("persistent UI bytes",File(home,"target/a.txt").readText())
    }
    @Test fun unconfirmedHistoricalSideEffectCannotBeReplayed() {
        val old=FilePasteRequest.capture(snapshot,target,"target")
        val item=FileCopyItemResult(FileScanEntry(snapshot.source,snapshot.items.single(),"a.txt"),FileCopyItemState.NOT_ATTEMPTED,target,"a.txt")
        runBlocking {
            journal.load();journal.accept(old);journal.phase(old.id,FileJournalPhase.CONFIRMING);journal.begin(old.id,listOf(item))
            journal.checkpoint(old.id,item,FileCopyCheckpoint.DOCUMENT_CREATE)
        }
        val before=runBlocking { journal.load() };val model=open()
        ui.onNodeWithTag("file-history-retry").performScrollTo().performClick()
        ui.onNodeWithTag("file-history-retry-confirm").assertIsNotEnabled()
        ui.onNodeWithText("保留记录").performClick()
        assertTrue(model.tasks.isEmpty());assertEquals(before,runBlocking { journal.load() })
        assertTrue(File(home,"target").list()!!.isEmpty());assertEquals(0,calls.get())
    }
    @Test fun historicalRetryRechecksChangedSourceBeforeAnyCopy() {
        val old=FilePasteRequest.capture(snapshot,target,"target")
        runBlocking { journal.load();journal.accept(old) }
        val before=runBlocking { journal.load().single() }
        File(home,"source/a.txt").writeText("changed source with a different size")
        val model=open()
        ui.onNodeWithTag("file-history-retry").performScrollTo().performClick()
        ui.onNodeWithTag("file-history-retry-confirm").performClick()
        ui.waitUntil(10000) { model.tasks.singleOrNull()?.phase==FileTaskPhase.FINISHED }
        assertNotNull(model.tasks.single().error);assertTrue(File(home,"target").list()!!.isEmpty())
        assertEquals(before,runBlocking { journal.load().single { it.intent.id==old.id } })
        assertEquals(old.id,model.tasks.single().request.retryOf)
    }
    @Test fun historicalRetryQueuesBehindExistingConfirmationAndKeepsAssociation() {
        val old=FilePasteRequest.capture(snapshot,target,"old target")
        runBlocking { journal.load();journal.accept(old) }
        File(home,"other").mkdir();val model=open()
        val first=ui.runOnIdle { model.start(snapshot,FileLocation(FileEndpoint.APP,"other"),"other",::connections);model.tasks.single() }
        ui.waitUntil(10000) { first.phase==FileTaskPhase.CONFIRMING }
        val history=model.history.single()
        ui.runOnIdle { model.requestHistoryRetry(history);model.confirmHistoryRetry() }
        ui.waitUntil(10000) { model.tasks.size==2 && runBlocking { journal.load().size==3 } }
        val queued=model.tasks.first { it!==first }
        assertEquals(FileTaskPhase.QUEUED,queued.phase);assertEquals(old.id,queued.request.retryOf)
        assertTrue(File(home,"target").list()!!.isEmpty());assertEquals(1,calls.get())
        ui.runOnIdle { model.cancel(first) };waitDone(first)
        ui.waitUntil(10000) { queued.phase==FileTaskPhase.CONFIRMING || queued.phase==FileTaskPhase.FINISHED }
        assertEquals(queued.error,FileTaskPhase.CONFIRMING,queued.phase)
        assertEquals(old.id,runBlocking { journal.load().single { it.intent.id==queued.request.id }.intent.retryOf })
    }
    @Test fun destinationQuotaCountsResidueFromAnotherTaskAndReportsZeroWrittenBytes() {
        val previous=File(root,"file-tasks/${UUID.randomUUID()}/destination").apply { mkdirs() }
        android.system.Os.chmod(previous.parentFile!!.path,448);android.system.Os.chmod(previous.path,448)
        val leftover=File(previous,UUID.randomUUID().toString())
        java.io.RandomAccessFile(leftover,"rw").use { it.setLength(512L*1024*1024) }
        android.system.Os.chmod(leftover.path,384)
        val model=open()
        val task=ui.runOnIdle { model.start(snapshot,target,"target",::connections);model.tasks.single() }
        ui.waitUntil(10000) { task.phase==FileTaskPhase.CONFIRMING }
        ui.onNodeWithTag("file-task-confirm").performScrollTo().performClick();waitDone(task)
        val result=task.result!!.items.single()
        assertEquals(FileCopyItemState.FAILED,result.state);assertEquals(0uL,result.bytes)
        assertTrue(result.error.orEmpty().contains("暂存容量不足"));assertNull(result.candidatePath)
        assertTrue(File(home,"target").list()!!.isEmpty());assertEquals(512L*1024*1024,leftover.length())
        assertEquals(result,runBlocking { journal.load().single().items.single().value })
    }
    @Test fun admissionFailureNeverScansAndDoesNotErasePreviousHistory() {
        val previous=FilePasteRequest.capture(snapshot,target,"target")
        runBlocking { journal.load();journal.accept(previous) }
        SQLiteDatabase.openDatabase(path.path,null,SQLiteDatabase.OPEN_READWRITE).use {
            it.execSQL("CREATE TRIGGER reject_admission BEFORE INSERT ON tasks BEGIN SELECT RAISE(ABORT,'admission refused'); END")
        }
        val model=open()
        val task=ui.runOnIdle { model.start(snapshot,target,"target",::connections);model.tasks.single() }
        waitDone(task)
        assertEquals("no connection factory before durable intent",0,calls.get())
        assertNotNull(task.persistenceError);assertNotNull(model.journalError)
        assertEquals(listOf(previous.id),runBlocking { journal.load().map { it.intent.id } })
        assertTrue(File(home,"target").list()!!.isEmpty())
        ui.onNodeWithTag("file-journal-error").assertExists()
    }
    @Test fun finishedProductPersistsResultsAndNewRuntimeDoesNotReplayCopy() {
        val model=open()
        val task=ui.runOnIdle { model.start(snapshot,target,"target",::connections);model.tasks.single() }
        ui.waitUntil(10000) { task.phase==FileTaskPhase.CONFIRMING || task.phase==FileTaskPhase.FINISHED }
        assertEquals(task.error,FileTaskPhase.CONFIRMING,task.phase)
        assertEquals(FileJournalPhase.CONFIRMING,runBlocking { journal.load().single().phase })
        ui.onNodeWithTag("file-task-confirm").performScrollTo().performClick();waitDone(task)
        assertEquals(FileCopyState.COMPLETE,task.result!!.state)
        assertEquals("persistent UI bytes",File(home,"target/a.txt").readText())
        val callsBefore=calls.get();val reopened=open()
        assertTrue(reopened.tasks.isEmpty());assertFalse(reopened.history.single().interrupted)
        assertEquals(FileCopyState.COMPLETE,reopened.history.single().outcome)
        assertEquals(task.result!!.items,reopened.history.single().items.map { it.value })
        assertEquals(callsBefore,calls.get());assertEquals(listOf("a.txt"),File(home,"target").list()!!.toList())
        ui.onNodeWithTag("file-history-status").assertTextEquals("历史任务 · 全部复制完成")
    }
    @Test fun completionRecordFailureStopsAlreadyQueuedNextRequest() {
        File(home,"other").mkdir();val model=open()
        val first=ui.runOnIdle { model.start(snapshot,target,"target",::connections);model.tasks.first() }
        ui.waitUntil(10000) { first.phase==FileTaskPhase.CONFIRMING }
        val second=ui.runOnIdle { model.start(snapshot,FileLocation(FileEndpoint.APP,"other"),"other",::connections);model.tasks.first() }
        ui.waitUntil(10000) { runBlocking { journal.load().size==2 } }
        SQLiteDatabase.openDatabase(path.path,null,SQLiteDatabase.OPEN_READWRITE).use {
            it.execSQL("CREATE TRIGGER reject_complete BEFORE UPDATE ON items WHEN instr(CAST(NEW.payload AS TEXT), '\"state\":\"COMPLETE\"')>0 BEGIN SELECT RAISE(ABORT,'final record refused'); END")
        }
        ui.runOnIdle { model.confirm(first,first.decisions!!.reference) };waitDone(first)
        ui.runOnIdle {
            assertNotNull(first.persistenceError);assertNotNull(model.journalError)
            assertEquals("journal failure must stop queue advance",FileTaskPhase.QUEUED,second.phase)
        }
        assertEquals(2,calls.get());assertTrue(File(home,"other").list()!!.isEmpty())
        assertEquals(FileCopyItemState.COMPLETE,first.result!!.items.single().state)
        assertEquals("persistent UI bytes",File(home,"target/a.txt").readText())
    }
    @Test fun failedHistoryRemovalKeepsVisibleRecordAndOriginalDatabase() {
        val request=FilePasteRequest.capture(snapshot,target,"target")
        runBlocking { journal.load();journal.accept(request) }
        SQLiteDatabase.openDatabase(path.path,null,SQLiteDatabase.OPEN_READWRITE).use {
            it.execSQL("CREATE TRIGGER reject_remove BEFORE DELETE ON tasks BEGIN SELECT RAISE(ABORT,'remove refused'); END")
        }
        val model=open();ui.onNodeWithTag("file-history-remove").performClick()
        ui.onNodeWithText("清除记录",useUnmergedTree=true).performClick()
        ui.waitUntil(10000) { !model.journalLoading && model.journalError!=null }
        assertEquals(1,model.history.size);assertEquals(request.id,runBlocking { journal.load().single().intent.id })
        ui.onNodeWithTag("file-history-status").assertTextEquals("已中断 · 结果待核对")
    }
    @Test fun corruptHistoryIsVisibleAndBlocksNewRequestsWithoutReset() {
        path.parentFile!!.mkdirs();path.writeBytes(ByteArray(512) { 0x55 })
        val original=path.readBytes();val model=open()
        assertNotNull(model.journalError);assertTrue(model.history.isEmpty())
        ui.runOnIdle { model.start(snapshot,target,"target",::connections) }
        assertTrue(model.tasks.isEmpty());assertEquals(0,calls.get());assertArrayEquals(original,path.readBytes())
        ui.onNodeWithTag("file-journal-error").assertExists()
        ui.onNodeWithText("还没有复制任务。选择文件并复制后，到目标文件夹粘贴。").assertDoesNotExist()
        ui.onNodeWithText("重新读取记录").performClick();ui.waitUntil(10000) { !model.journalLoading }
        assertNotNull(model.journalError);assertArrayEquals(original,path.readBytes())
    }
}
