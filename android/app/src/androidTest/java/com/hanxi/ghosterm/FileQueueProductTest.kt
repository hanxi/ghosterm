package com.hanxi.ghosterm

import android.app.Application
import androidx.activity.ComponentActivity
import androidx.compose.material3.MaterialTheme
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
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger

/** Real local publications and production task page; waiting requests must own no IO. */
@RunWith(AndroidJUnit4::class)
class FileQueueProductTest {
    @get:Rule val ui=createAndroidComposeRule<ComponentActivity>()
    private lateinit var root:File
    private lateinit var home:File
    private lateinit var page:FileTasksViewModel
    private lateinit var snapshot:FileClipboardSnapshot
    private val releases=mutableListOf<CountDownLatch>()
    @Before fun setup() {
        val context=InstrumentationRegistry.getInstrumentation().targetContext
        root=File(context.cacheDir,"queue-${UUID.randomUUID()}").apply { mkdirs() }
        home=File(root,"home").apply { mkdirs() }
        for(name in listOf("source","one","two","three")) File(home,name).mkdir()
        File(home,"source/a.txt").writeText("queued bytes ☃\n")
        val app=object:Application() { init { attachBaseContext(context) };override fun getFilesDir()=root }
        val work=FileWorkRuntime(app);page=FileTasksViewModel(app,work)
        val source=FileLocation(FileEndpoint.APP,"source")
        snapshot=FileClipboardSnapshot.capture(source,"source",runBlocking { LocalFileBrowser(home,context.contentResolver).browse(source).entries })
        ui.setContent { MaterialTheme { FileTasksScreen(page) } }
    }
    @After fun cleanup() {
        releases.forEach { it.countDown() }
        ui.runOnIdle { page.tasks.toList().forEach(page::cancel) }
        await("cancelled queue must release all requests") { !page.busy }
        root.deleteRecursively()
    }
    private fun connections(request:FilePasteRequest)=FileTaskConnections(request.source.endpoint,request.destination.endpoint,
        { _,_->error("local queue must not resolve SSH") },{ _,_,_->error("local queue must not authenticate") })
    private fun add(target:String,connect:(FilePasteRequest)->FileTaskConnections=::connections):FileTask = ui.runOnIdle {
        page.start(snapshot,FileLocation(FileEndpoint.APP,target),target,connect);page.tasks.first()
    }
    private fun await(description:String,condition:()->Boolean) {
        try { ui.waitUntil(15000,condition) }
        catch(timeout:ComposeTimeoutException) {
            fail("$description; observed phases: ${page.tasks.map { it.phase }}")
        }
    }
    private fun ready(task:FileTask) { await("next request must reach confirmation") { task.phase==FileTaskPhase.CONFIRMING || task.phase==FileTaskPhase.FINISHED }
        assertEquals("confirmation: ${task.error}",FileTaskPhase.CONFIRMING,task.phase) }
    private fun confirm(task:FileTask) { ui.runOnIdle { page.confirm(task,task.decisions!!.reference) } }
    private fun done(task:FileTask) { await("request must finish after actual cleanup") { task.phase==FileTaskPhase.FINISHED } }
    private fun copied(target:String) { assertEquals("queued bytes ☃\n",File(home,"$target/a.txt").readText()) }
    private fun node(task:FileTask,tag:String)=ui.onNode(hasTestTag(tag) and hasAnyAncestor(hasTestTag("file-task:${task.request.id}")))

    @Test fun fifoWaitsForEachConfirmationAndKeepsIndependentDestinations() {
        val first=add("one");ready(first)
        val second=add("two");val third=add("three")
        await("waiting intents durably admitted") { second.recorded && third.recorded && !second.recording && !third.recording }
        ui.runOnIdle {
            assertEquals(FileTaskPhase.QUEUED,second.phase);assertEquals(FileTaskPhase.QUEUED,third.phase)
            assertEquals(1,page.queuedAhead(second));assertEquals(2,page.queuedAhead(third))
            assertNotEquals(first.request.id,second.request.id);assertEquals(snapshot.id,second.request.clipboardId)
        }
        assertFalse(File(home,"two/a.txt").exists());assertFalse(File(home,"three/a.txt").exists())
        confirm(first);done(first);ready(second);copied("one")
        ui.runOnIdle { assertEquals(FileTaskPhase.QUEUED,third.phase);assertEquals(1,page.queuedAhead(third)) }
        assertFalse(File(home,"two/a.txt").exists())
        confirm(second);done(second);ready(third);copied("two")
        assertFalse(File(home,"three/a.txt").exists())
        confirm(third);done(third);copied("three");assertFalse(page.busy)
        assertEquals("queued bytes ☃\n",File(home,"source/a.txt").readText())
    }
    @Test fun cancelledWaitingRequestNeverAcquiresAndNextLiveRequestCanRun() {
        val first=add("one");ready(first)
        val calls=AtomicInteger()
        val second=add("two") { calls.incrementAndGet();connections(it) }
        val third=add("three")
        ui.runOnIdle { page.cancel(second) };done(second)
        assertEquals(FileTaskPhase.FINISHED,second.phase);assertTrue(second.cancelled)
        assertEquals(0,calls.get());assertFalse(File(home,"two/a.txt").exists())
        confirm(first);done(first);ready(third)
        assertEquals(0,calls.get());confirm(third);done(third);copied("three")
        assertFalse(File(home,"two/a.txt").exists())
    }
    @Test fun failedQueuedTargetIsRecheckedAndDoesNotStrandFollowingRequest() {
        val first=add("one");ready(first)
        val second=add("two");val third=add("three")
        await("waiting intents durably admitted") { second.recorded && third.recorded && !second.recording && !third.recording }
        assertTrue(File(home,"two").delete())
        ui.runOnIdle { page.cancel(first) }
        done(second);assertNotNull(second.error);assertNull(second.decisions);assertNull(second.result)
        ready(third);assertFalse(File(home,"two").exists());confirm(third);done(third);copied("three")
        assertFalse(File(home,"one/a.txt").exists())
    }
    @Test fun queuedConflictsAndLateActionsNeverReusePreviousRequestsDecision() {
        File(home,"one/a.txt").writeText("keep one");File(home,"two/a.txt").writeText("keep two")
        val first=add("one");ready(first);val second=add("two")
        val old=first.decisions!!.reference
        ui.runOnIdle { page.choose(first,old,"a.txt",FilePasteChoice.SKIP,true) }
        confirm(first);done(first);ready(second)
        assertEquals(FileCopyState.ALL_SKIPPED,first.result!!.state)
        assertEquals(1,second.decisions!!.pending.size)
        ui.runOnIdle { page.confirm(second,old);page.cancel(first);page.remove(first) }
        assertEquals(FileTaskPhase.CONFIRMING,second.phase);assertNotNull(second.error)
        assertEquals("keep two",File(home,"two/a.txt").readText())
        ui.runOnIdle { page.choose(second,second.decisions!!.reference,"a.txt",FilePasteChoice.KEEP_BOTH,false) }
        confirm(second);done(second)
        assertEquals("queued bytes ☃\n",File(home,"two/a (2).txt").readText())
        assertEquals("keep one",File(home,"one/a.txt").readText());assertEquals("keep two",File(home,"two/a.txt").readText())
    }
    @Test fun pendingLimitRejectsExtraRequestAndCancelledWaitingSlotCanBeReused() {
        val first=add("one");ready(first)
        repeat(31) { add("two") }
        val ids=page.tasks.map { it.request.id }
        add("three")
        assertEquals(32,page.tasks.size);assertEquals(ids,page.tasks.map { it.request.id });assertNotNull(page.notice)
        val cancelled=page.tasks.first()
        ui.runOnIdle { page.cancel(cancelled) };done(cancelled)
        val admitted=add("three")
        assertNotEquals(cancelled.request.id,admitted.request.id);assertEquals(33,page.tasks.size)
        assertEquals(FileTaskPhase.QUEUED,admitted.phase);assertNull(page.notice)
        assertFalse(File(home,"one/a.txt").exists());assertFalse(File(home,"three/a.txt").exists())
    }
    @Test fun waitingPageShowsPositionAndCanCancelWithoutStartingCopy() {
        val first=add("one");ready(first);val second=add("two")
        node(second,"file-task-status").performScrollTo().assertTextEquals("排队等待")
        node(second,"file-task-queue-position").assertTextEquals("前方还有 1 个任务")
        node(second,"file-task-confirm").assertDoesNotExist()
        node(second,"file-task-cancel").performScrollTo().performClick();done(second)
        assertEquals(FileTaskPhase.FINISHED,second.phase);assertTrue(second.cancelled)
        node(second,"file-task-error").assertTextEquals("排队任务已取消，尚未读取或写入")
        assertEquals(FileTaskPhase.CONFIRMING,first.phase);assertFalse(File(home,"two/a.txt").exists())
        assertEquals("queued bytes ☃\n",File(home,"source/a.txt").readText())
    }
    @Test fun cancellingExecutionDoesNotAdvanceQueueBeforeActualConnectionRelease() {
        val entered=CountDownLatch(1);val release=CountDownLatch(1);releases+=release
        val calls=AtomicInteger()
        val first=add("one") { request ->
            if(calls.incrementAndGet()==2) { entered.countDown();check(release.await(20,TimeUnit.SECONDS)) }
            connections(request)
        }
        ready(first);confirm(first);assertTrue(entered.await(5,TimeUnit.SECONDS))
        val second=add("two")
        ui.runOnIdle {
            page.cancel(first);assertEquals(FileTaskPhase.CANCELLING,first.phase)
            assertEquals(FileTaskPhase.QUEUED,second.phase);page.remove(first);assertTrue(first in page.tasks)
        }
        assertFalse(File(home,"two/a.txt").exists())
        release.countDown();done(first);ready(second)
        assertEquals(FileCopyState.CANCELLED,first.result!!.state);assertFalse(File(home,"one/a.txt").exists())
        confirm(second);done(second);copied("two")
    }
}
