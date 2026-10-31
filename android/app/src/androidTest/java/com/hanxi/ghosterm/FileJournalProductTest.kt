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
