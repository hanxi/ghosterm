package com.hanxi.ghosterm

import android.app.Application
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
class FileTaskArchiveTest {
    @get:Rule val ui=createAndroidComposeRule<ComponentActivity>()
    private val context=InstrumentationRegistry.getInstrumentation().targetContext
    private lateinit var root:File
    private lateinit var home:File
    private lateinit var application:Application
    private lateinit var model:FileTasksViewModel
    private lateinit var journal:FileTaskJournal
    private var page by mutableStateOf<FileTasksViewModel?>(null)
    private val source=FileLocation(FileEndpoint.APP,"source")
    private val target=FileLocation(FileEndpoint.APP,"target")
    private val connections=AtomicInteger()
    @Before fun setup() {
        root=File(context.cacheDir,"archive-${UUID.randomUUID()}").apply { mkdirs() }
        home=File(root,"home").apply { mkdir() };File(home,"source").mkdir();File(home,"target").mkdir()
        File(home,"source/a.txt").writeText("archive source bytes")
        application=object:Application() { init { attachBaseContext(context) };override fun getFilesDir()=root }
        journal=FileTaskJournal(File(root,"file-history/tasks.db"))
        model=ui.runOnIdle { FileTasksViewModel(application,FileWorkRuntime(application)).also { page=it } }
        ui.setContent { MaterialTheme { page?.let { FileTasksScreen(it) } } }
        ui.waitUntil(10000) { !model.journalLoading }
    }
    @After fun finish() {
        ui.runOnIdle { model.cancelCleanup();model.tasks.toList().forEach(model::cancel) }
        ui.waitUntil(15000) { !model.busy && !model.cleanupBusy };ui.runOnIdle { page=null };root.deleteRecursively()
    }
    private fun snapshot()=runBlocking {
        FileClipboardSnapshot.capture(source,"source",LocalFileBrowser(home,context.contentResolver).browse(source).entries)
    }
    private fun local(request:FilePasteRequest):FileTaskConnections {
        connections.incrementAndGet()
        return FileTaskConnections(request.source.endpoint,request.destination.endpoint,{ _,_->error("local only") },{ _,_,_->error("no authentication") })
    }
    private fun done(task:FileTask) { ui.waitUntil(15000) { !task.active && !task.recording && !model.contentReading } }
    private fun seed(finished:Boolean=true):FileTask {
        val request=FilePasteRequest.capture(snapshot(),target,"target")
        val selected=request.items.single()
        val item=FileCopyItemResult(FileScanEntry(source,selected,selected.name),FileCopyItemState.UNKNOWN,target,
            selected.name,createdId="target/${selected.name}")
        val result=FileCopyProgress(request.id,request.clipboardId,FileCopyState.UNKNOWN,listOf(item))
        runBlocking {
            journal.accept(request);journal.phase(request.id,FileJournalPhase.CONFIRMING);journal.begin(request.id,listOf(item))
            if(finished) journal.finish(request.id,result,false,null)
        }
        File(home,"target/a.txt").writeText("partial target")
        return ui.runOnIdle { FileTask(request).apply {
            phase=FileTaskPhase.FINISHED;recorded=true;this.result=if(finished)result else null
            connections=::local;model.work.tasks.tasks.add(this)
        } }
    }
    @Test fun realCompletedTreeMovesWithoutCopyReplayOrRecordChangesAndStaleClearCannotDeleteIt() {
        File(home,"source/tree").mkdir();File(home,"source/tree/b.txt").writeText("nested B")
        File(home,"source/tree/c.txt").writeText("nested C")
        ui.runOnIdle { model.start(snapshot(),target,"target",::local) }
        val task=model.tasks.single();ui.waitUntil(15000) { task.phase==FileTaskPhase.CONFIRMING }
        ui.runOnIdle { model.confirm(task,checkNotNull(task.decisions).reference) };done(task)
        assertEquals(task.error,FileCopyState.COMPLETE,task.result?.state)
        val before=runBlocking { journal.load().single() };val count=connections.get()
        ui.onNodeWithTag("file-task-archive").performScrollTo().assertIsEnabled().performClick()
        ui.waitUntil(10000) { !task.recording }
        assertFalse("archive must release the global reader",model.contentReading)
        assertFalse("finished task must leave the live list",task in model.tasks)
        assertNull(model.journalError);assertEquals(listOf(before),model.history.toList());assertEquals(listOf(before),runBlocking { journal.load() })
        assertEquals("archive must not reopen any connection",count,connections.get())
        assertEquals("archive source bytes",File(home,"target/a.txt").readText());assertEquals("nested B",File(home,"target/tree/b.txt").readText())
        assertEquals("nested C",File(home,"target/tree/c.txt").readText())
        ui.runOnIdle { model.remove(task);assertFalse("stale clear must not start a record write",task.recording) }
        assertEquals("stale live-card clear cannot erase archived history",listOf(before),runBlocking { journal.load() })
        ui.onNodeWithTag("file-history-details").performScrollTo().assertExists()
    }
    @Test fun uncertainFinishedTaskCanBeReviewedAndCleanedInTheSameRuntime() {
        val task=seed();val before=runBlocking { journal.load().single() };val runtime=model.work
        ui.onNodeWithTag("file-task-archive").performScrollTo().performClick()
        ui.waitUntil(10000) { !task.recording }
        assertFalse("archive must release the global reader",model.contentReading)
        assertFalse("finished task must leave the live list",task in model.tasks)
        assertSame(runtime,model.work);assertEquals(before,model.history.single())
        ui.onNodeWithTag("file-history-details").performScrollTo().performClick()
        ui.onNodeWithTag("file-cleanup-review:a.txt:false").performScrollTo().performClick()
        ui.waitUntil(10000) { model.cleanupReview!=null || model.cleanupError!=null };assertNull(model.cleanupError)
        ui.onNodeWithTag("file-cleanup-confirm").assertIsNotEnabled();assertTrue(File(home,"target/a.txt").exists())
        ui.onNodeWithTag("file-cleanup-consent").performClick();ui.onNodeWithTag("file-cleanup-confirm").performClick()
        ui.waitUntil(10000) { !model.cleanupBusy && model.cleanupRecords.size==1 }
        assertNull(model.cleanupError);assertEquals(FileCleanupState.REMOVED,model.cleanupRecords.single().state)
        assertFalse(File(home,"target/a.txt").exists());assertEquals("archive source bytes",File(home,"source/a.txt").readText())
        assertEquals(listOf(before),runBlocking { journal.load() });assertEquals(0,connections.get())
    }
    @Test fun activeUnsavedForeignAndLeasedTasksCannotArchiveOrClearRecords() {
        val task=seed();val before=runBlocking { journal.load().single() }
        ui.runOnIdle {
            task.phase=FileTaskPhase.CANCELLING;assertFalse(model.canArchive(task));model.archive(task);assertTrue(task in model.tasks)
            task.phase=FileTaskPhase.FINISHED;task.recorded=false;assertFalse(model.canArchive(task));model.archive(task)
            task.recorded=true;task.persistenceError="unsaved";assertFalse(model.canArchive(task));task.persistenceError=null
            val foreign=FileTask(task.request).apply { phase=FileTaskPhase.FINISHED;recorded=true;result=task.result }
            assertFalse(model.canArchive(foreign));model.archive(foreign)
            val lease=checkNotNull(model.work.tasks.beginContentRead())
            try { assertFalse(model.canArchive(task));model.archive(task);model.remove(task);assertFalse("record clear is blocked by maintenance lease",task.recording);assertTrue(task in model.tasks) }
            finally { model.work.tasks.endContentRead(lease) }
        }
        assertTrue(model.history.isEmpty());assertEquals(listOf(before),runBlocking { journal.load() })
        ui.runOnIdle { model.start(snapshot(),target,"another copy",::local) }
        val running=model.tasks.first { it!==task };ui.waitUntil(10000) { running.phase==FileTaskPhase.CONFIRMING }
        ui.runOnIdle { assertFalse(model.canArchive(task));model.archive(task);model.cancel(running) };done(running)
        assertTrue(task in model.tasks);assertTrue(model.history.isEmpty())
    }
    @Test fun durableRunningRecordCannotBePresentedAsFinishedHistory() {
        val task=seed(finished=false);val before=runBlocking { journal.load().single() }
        ui.runOnIdle { model.archive(task) };done(task)
        assertTrue("unfinalized durable task must stay in live results",task in model.tasks)
        assertTrue(model.history.isEmpty());assertTrue(model.journalError?.contains("尚未结束")==true)
        assertEquals(listOf(before),runBlocking { journal.load() });assertEquals("partial target",File(home,"target/a.txt").readText())
    }
    @Test fun changedHeaderOrItemSnapshotRetainsCurrentResultsAndShowsReadError() {
        val task=seed();val saved=checkNotNull(task.result);val before=runBlocking { journal.load().single() }
        for(header in listOf(true,false)) {
            ui.runOnIdle {
                task.result=FileCopyProgress(saved.requestId,saved.clipboardId,if(header)FileCopyState.COMPLETE else saved.state,
                    if(header)saved.items else saved.items.map { it.copy(bytes=it.bytes+1uL) })
                model.archive(task)
            }
            done(task)
            assertTrue("mismatched persisted snapshot cannot replace visible result",task in model.tasks);assertTrue(model.history.isEmpty())
            assertTrue(model.journalError?.contains(if(header)"不一致" else "完整保存")==true)
            ui.onNodeWithText(checkNotNull(model.journalError),substring=false).assertExists()
            assertEquals(listOf(before),runBlocking { journal.load() })
            ui.runOnIdle { task.result=saved;model.reloadHistory() };ui.waitUntil(10000) { !model.journalLoading }
        }
    }
}
