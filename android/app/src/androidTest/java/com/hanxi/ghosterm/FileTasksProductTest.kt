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
import kotlinx.coroutines.withTimeout
import kotlinx.coroutines.delay
import org.junit.*
import org.junit.Assert.*
import org.junit.runner.RunWith
import java.io.File
import java.util.UUID
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

@RunWith(AndroidJUnit4::class)
class FileTasksProductTest {
    @get:Rule val ui=createAndroidComposeRule<ComponentActivity>()
    private lateinit var root: File
    private lateinit var home: File
    private lateinit var files: FilesViewModel
    private lateinit var tasks: FileTasksViewModel
    private lateinit var local: LocalFileBrowser
    private val latest get()=tasks.tasks.first()
    @Before fun setup() {
        val context=InstrumentationRegistry.getInstrumentation().targetContext
        root=File(context.cacheDir,"task-ui-${UUID.randomUUID()}").apply { mkdirs() }
        home=File(root,"home").apply { mkdirs() }
        File(home,"source").mkdir();File(home,"target").mkdir();File(home,"other").mkdir()
        File(home,"source/a.txt").writeText("真实复制 ☃\n");File(home,"source/b.txt").writeText("second")
        val app=object:Application() { init { attachBaseContext(context) };override fun getFilesDir()=root }
        val work=FileWorkRuntime(app)
        files=FilesViewModel(app,work=work);tasks=FileTasksViewModel(app,work);local=LocalFileBrowser(home,context.contentResolver)
        ui.setContent { MaterialTheme {
            if(tasks.visible) FileTasksScreen(tasks)
            else FilesScreen(files,{},onPaste={
                tasks.start(files.clipboard!!,files.location,files.location.directoryId,::connections)
            },showTasks=tasks::show)
        } }
        ui.runOnIdle { files.enter() };loaded()
    }
    private fun connections(request: FilePasteRequest)=FileTaskConnections(request.source.endpoint,request.destination.endpoint,
        { _,_->error("local task must not resolve SSH") },{ _,_,_->error("local task must not authenticate") })
    @After fun cleanup() {
        ui.runOnIdle { tasks.tasks.toList().forEach(tasks::cancel);files.leave() }
        ui.waitUntil(15000) { !tasks.busy }
        root.deleteRecursively()
    }
    private fun loaded() { ui.waitUntil(10000) { !files.loading } }
    private fun enter(name: String) { ui.onNodeWithTag("file-entry:$name").performClick();loaded() }
    private fun copy(name: String) {
        ui.onNodeWithTag("file-entry:$name").performTouchInput { longClick() }
        ui.onNodeWithTag("file-copy").performClick()
    }
    private fun prepare(name: String="a.txt") {
        enter("source");copy(name)
        ui.runOnIdle { files.back() };loaded();enter("target")
        ui.onNodeWithTag("file-paste").performClick()
        assertEquals("paste button creates a task",1,tasks.tasks.size)
        ui.waitUntil(10000) { latest.phase!=FileTaskPhase.SCANNING }
        assertEquals("preflight must reach confirmation: ${latest.error}",FileTaskPhase.CONFIRMING,latest.phase)
    }
    private fun confirm() {
        ui.onNodeWithTag("file-task-confirm").performScrollTo().performClick()
        assertNotEquals("confirm button starts execution",FileTaskPhase.CONFIRMING,latest.phase);finished()
    }
    private fun finished() { ui.waitUntil(15000) { !tasks.busy } }
    @Test fun pasteFreezesDestinationAndKeepsDefaultClipboardAfterRealCopy() {
        prepare();assertFalse(File(home,"target/a.txt").exists())
        val clipboard=files.clipboard!!.id
        // Change the browser without changing the confirmation's fixed destination.
        ui.runOnIdle { files.back() };loaded()
        ui.runOnIdle { files.tap(files.entries.single { it.name=="other" }) };loaded()
        ui.onNodeWithTag("file-task-destination").assertTextContains("target",substring=true)
        confirm()
        assertEquals(FileCopyState.COMPLETE,latest.result!!.state)
        assertTrue("copy must use the frozen target",File(home,"target/a.txt").isFile)
        assertEquals("真实复制 ☃\n",File(home,"target/a.txt").readText());assertFalse(File(home,"other/a.txt").exists())
        assertNotNull("default policy keeps the clipboard",files.clipboard)
        assertEquals(clipboard,files.clipboard!!.id);assertEquals("真实复制 ☃\n",File(home,"source/a.txt").readText())
        ui.onNodeWithTag("file-task-status").assertTextEquals("全部复制完成")
        ui.onNodeWithTag("file-task-details").performScrollTo().performClick()
        ui.onNodeWithText("实际位置：target/a.txt").performScrollTo().assertIsDisplayed()
    }
    @Test fun explicitKeepBothNeverOverwritesAndStaleConfirmationCannotRun() {
        File(home,"target/a.txt").writeText("original")
        prepare();val stale=latest.decisions!!.reference
        ui.onNodeWithTag("file-task-keep").performScrollTo().performClick()
        ui.runOnIdle { tasks.confirm(latest,stale) }
        assertEquals(FileTaskPhase.CONFIRMING,latest.phase);assertNotNull(latest.error)
        assertFalse(File(home,"target/a (2).txt").exists())
        confirm();assertEquals("original",File(home,"target/a.txt").readText())
        assertEquals("真实复制 ☃\n",File(home,"target/a (2).txt").readText())
        val result=latest.result
        ui.runOnIdle { tasks.confirm(latest,stale) }
        assertSame(result,latest.result);assertFalse(File(home,"target/a (3).txt").exists())
    }
    @Test fun allSkippedAndCancelledConfirmationKeepClipboardAndNeverWrite() {
        File(home,"target/a.txt").writeText("original")
        prepare();ui.onNodeWithTag("file-task-skip").performScrollTo().performClick();confirm()
        assertEquals(FileCopyState.ALL_SKIPPED,latest.result!!.state)
        ui.onNodeWithTag("file-task-status").assertTextEquals("全部跳过，未复制任何项目")
        assertNotNull(files.clipboard);assertEquals("original",File(home,"target/a.txt").readText())
        ui.runOnIdle { tasks.hide() };loaded()
        ui.onNodeWithTag("file-paste").performClick();ui.waitUntil { latest.phase==FileTaskPhase.CONFIRMING }
        assertEquals(1,latest.decisions!!.pending.size) // Previous choice was request-local.
        ui.onAllNodesWithTag("file-task-cancel").onFirst().performScrollTo().performClick()
        assertFalse(tasks.busy);assertTrue(latest.cancelled);assertNull(latest.result);assertNotNull(files.clipboard)
        assertEquals("original",File(home,"target/a.txt").readText())
    }
    @Test fun changedSourceFailsAndFinishedTaskCannotClearNewClipboard() {
        prepare();val old=files.clipboard!!
        File(home,"source/a.txt").writeText("changed after scan")
        confirm();assertEquals(FileCopyState.FAILED,latest.result!!.state)
        assertFalse(File(home,"target/a.txt").exists())
        ui.runOnIdle { files.copyCompleted(latest.result!!,true) };loaded()
        assertNotNull("failed copy cannot clear its source list",files.clipboard);assertEquals(old.id,files.clipboard!!.id)
        ui.runOnIdle { tasks.hide();files.back() };loaded();enter("source");copy("b.txt")
        ui.onNodeWithText("替换清单").performClick();val replacement=files.clipboard!!.id
        val success=FileCopyProgress(latest.request.id,old.id,FileCopyState.COMPLETE,emptyList())
        ui.runOnIdle { files.copyCompleted(success,true) };loaded()
        assertNotNull("old completion cannot clear a newer clipboard",files.clipboard);assertEquals(replacement,files.clipboard!!.id)
        val used=FileCopyProgress("current",replacement,FileCopyState.COMPLETE,emptyList())
        ui.runOnIdle { files.copyCompleted(used,false) };loaded();assertNotNull(files.clipboard)
        ui.runOnIdle { files.copyCompleted(used,true) };loaded();assertNull(files.clipboard)
    }
    @Test fun leavingBrowserDoesNotCancelConfirmedTreeAndResultsRetainEmptyDirectories() {
        File(home,"source/tree/empty").mkdirs();File(home,"source/tree/.hidden").writeText("hidden")
        prepare("tree")
        val task=latest;val ref=task.decisions!!.reference
        ui.runOnIdle { tasks.confirm(task,ref);tasks.hide();files.leave() }
        finished();assertEquals(FileCopyState.COMPLETE,task.result!!.state)
        assertTrue(File(home,"target/tree/empty").isDirectory)
        assertEquals("hidden",File(home,"target/tree/.hidden").readText());assertFalse(tasks.visible)
        assertNotNull(files.clipboard)
    }
    @Test fun singleSlotRemainsOccupiedUntilCancelledAcquisitionReturns() {
        val source=FileLocation(FileEndpoint.APP,"source")
        val entries=runBlocking { local.browse(source).entries }.filter { it.name=="a.txt" }
        val snapshot=FileClipboardSnapshot.capture(source,"source",entries)
        val entered=CountDownLatch(1);val release=CountDownLatch(1)
        try {
            ui.runOnIdle { tasks.start(snapshot,FileLocation(FileEndpoint.APP,"target"),"target",{
                entered.countDown();check(release.await(10,TimeUnit.SECONDS));connections(it)
            }) }
            assertTrue(entered.await(5,TimeUnit.SECONDS));val first=latest
            ui.runOnIdle { tasks.cancel(first);tasks.start(snapshot,FileLocation(FileEndpoint.APP,"other"),"other",::connections) }
            val queued=latest;assertNotSame(first,queued);assertEquals(2,tasks.tasks.size)
            assertEquals(FileTaskPhase.QUEUED,queued.phase)
            assertTrue(tasks.busy);assertEquals(FileTaskPhase.CANCELLING,first.phase);assertNull(tasks.notice)
            ui.runOnIdle { tasks.remove(first) };assertEquals(2,tasks.tasks.size)
            release.countDown();ui.waitUntil(15000) { queued.phase==FileTaskPhase.CONFIRMING }
            assertTrue(first.cancelled);assertEquals(FileTaskPhase.FINISHED,first.phase)
            assertFalse(File(home,"target/a.txt").exists());assertFalse(File(home,"other/a.txt").exists())
            ui.runOnIdle { tasks.cancel(queued);tasks.remove(first);tasks.remove(queued) };assertTrue(tasks.tasks.isEmpty())
        } finally { release.countDown() }
    }
    @Test fun completionDoesNotClearSelectionOrPreviewInAnotherDirectory() {
        File(home,"other/stay.txt").writeText("still reading")
        prepare();confirm();val result=latest.result!!
        ui.runOnIdle { tasks.hide();files.back() };loaded();enter("other")
        ui.onNodeWithTag("file-entry:stay.txt").performTouchInput { longClick() }
        val selected=files.selected
        ui.runOnIdle { files.copyCompleted(result) };loaded()
        assertTrue(files.selecting);assertEquals(selected,files.selected)
        ui.runOnIdle { files.clearSelection() }
        ui.onNodeWithTag("file-entry:stay.txt").performClick()
        ui.waitUntil { files.preview?.text!=null }
        val preview=files.preview
        ui.runOnIdle { files.copyCompleted(result) };loaded()
        assertSame(preview,files.preview);assertEquals("still reading",files.preview?.text)
    }
    @Test fun connectionTimeoutIsFailureRatherThanUserCancellation() {
        val endpoint=FileEndpoint("timeout","timeout","/",remote=RemoteFileIdentity("timeout","127.0.0.1","user",2222))
        val snapshot=FileClipboardSnapshot.capture(FileLocation(endpoint),"/",listOf(BrowserEntry("/file","file",FileKind.FILE,1uL,null)))
        ui.runOnIdle {
            tasks.start(snapshot,FileLocation(FileEndpoint.APP,"target"),"target",{ request ->
                FileTaskConnections(request.source.endpoint,request.destination.endpoint,
                    { _,_->withTimeout(20) { delay(1000);error("DNS must time out") } },{ _,_,_->error("No connection may be created") })
            })
        }
        finished();assertFalse("deadline is not a user cancellation",latest.cancelled)
        assertTrue(latest.error?.contains("超时")==true);assertNull(latest.decisions);assertNull(latest.result)
        ui.onNodeWithTag("file-task-status").assertTextEquals("复制未完成")
        ui.onNodeWithTag("file-task-error").assertTextEquals("任务连接超时，请重新确认后重试")
        assertFalse(File(home,"target/file").exists())
    }
    @Test fun sameFolderFailureIsVisibleAndDoesNotBecomeConfirmation() {
        enter("source");copy("a.txt");ui.onNodeWithTag("file-paste").performClick();finished()
        assertTrue(latest.error!!.contains("同一文件夹"));assertNull(latest.decisions)
        ui.onNodeWithTag("file-task-error").assertIsDisplayed();ui.onNodeWithTag("file-task-confirm").assertDoesNotExist()
        assertNotNull(files.clipboard)
    }
}
