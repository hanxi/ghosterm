package com.hanxi.ghosterm

import android.app.Application
import androidx.activity.ComponentActivity
import androidx.compose.runtime.*
import androidx.compose.ui.test.*
import androidx.compose.ui.graphics.asAndroidBitmap
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
class FileTaskDockTest {
    @get:Rule val ui = createAndroidComposeRule<ComponentActivity>()
    private val context = InstrumentationRegistry.getInstrumentation().targetContext
    private lateinit var root: File
    private lateinit var home: File
    private lateinit var hosts: HostsViewModel
    private lateinit var files: FilesViewModel
    private lateinit var tasks: FileTasksViewModel
    private var shown by mutableStateOf(true)
    private val opened = AtomicInteger()
    private val source = FileLocation(FileEndpoint.APP, "source")
    private val target = FileLocation(FileEndpoint.APP, "target")
    private fun captureEmptyReview(name:String) {
        ui.waitForIdle()
        val output=File(context.cacheDir,"empty-task-review").apply { mkdirs() }
        val bitmap=ui.onRoot().captureToImage().asAndroidBitmap()
        File(output,"$name.png").outputStream().use { bitmap.compress(android.graphics.Bitmap.CompressFormat.PNG,100,it) }
    }
    @Before fun setup() {
        root = File(context.cacheDir, "dock-${UUID.randomUUID()}").apply { mkdirs() }
        home = File(root, "home").apply { mkdirs() }
        listOf("source", "target", "second").forEach { File(home, it).mkdir() }
        File(home, "source/a.txt").writeText("dock actual source ☃\n")
        val app = object : Application() { init { attachBaseContext(context) }; override fun getFilesDir() = root }
        val work = FileWorkRuntime(app)
        val runtime = RuntimeInfoViewModel()
        hosts = HostsViewModel(app); files = FilesViewModel(app, work = work); tasks = FileTasksViewModel(app, work)
        ui.setContent { if(shown) GhostermApp(hosts, runtime, files = files, fileTasks = tasks) }
        ui.waitUntil(10000) { hosts.snapshot != null && !hosts.busy && !tasks.journalLoading }
    }
    @After fun cleanup() {
        ui.runOnIdle { tasks.tasks.toList().forEach(tasks::cancel);files.leave() }
        ui.waitUntil(15000) { !tasks.busy && !tasks.contentReading }
        ui.runOnIdle { shown = false };root.deleteRecursively()
    }
    private fun snapshot() = runBlocking {
        FileClipboardSnapshot.capture(source, "source", LocalFileBrowser(home, context.contentResolver).browse(source).entries)
    }
    private fun connections(request: FilePasteRequest): FileTaskConnections {
        opened.incrementAndGet()
        return FileTaskConnections(request.source.endpoint, request.destination.endpoint,
            { _, _ -> error("local only") }, { _, _, _ -> error("no authentication") })
    }
    private fun prepare(destination: FileLocation = target, label: String = "target"): FileTask {
        val copied = snapshot()
        val task = ui.runOnIdle {
            tasks.start(copied, destination, label, ::connections)
            tasks.tasks.first().also { tasks.hide() }
        }
        ui.waitUntil(10000) { task.phase == FileTaskPhase.CONFIRMING || !task.active }
        assertEquals("real preflight reaches confirmation", FileTaskPhase.CONFIRMING, task.phase)
        return task
    }
    @Test fun emptyHomeHidesSummaryAndFileHistoryEntryReturnsToTheSameLocation() {
        ui.onNodeWithText("任务", substring = false).assertDoesNotExist()
        ui.onNode(hasText("设置") or hasContentDescription("设置")).assertIsDisplayed()
        ui.onNodeWithTag("file-task-dock").assertDoesNotExist()
        ui.onNodeWithTag("file-task-dock-status",useUnmergedTree=true).assertDoesNotExist()
        captureEmptyReview("empty-home")
        ui.runOnIdle { files.enter() };ui.waitUntil(10000) { !files.loading }
        ui.onNodeWithTag("file-entry:source").performClick();ui.waitUntil(10000) { !files.loading }
        val location = files.location
        ui.onNodeWithTag("file-task-dock-status",useUnmergedTree=true).assertDoesNotExist()
        captureEmptyReview("empty-file-browser")
        ui.onNodeWithTag("file-task-dock").assertIsDisplayed().performClick()
        ui.onNode(hasText("返回") or hasContentDescription("返回")).performClick()
        assertTrue(files.visible);assertEquals(location, files.location)
        ui.onNodeWithTag("file-entry:a.txt").assertIsDisplayed()
        assertTrue(tasks.tasks.isEmpty());assertEquals(0, opened.get())
        assertEquals("dock actual source ☃\n", File(home, "source/a.txt").readText())
    }
    @Test fun activeSummaryAndClipboardLeaveOneActualFileRow() {
        val task=prepare();val before=opened.get()
        File(home,"target/existing.txt").writeText("unchanged destination")
        ui.runOnIdle { files.enter() };ui.waitUntil { !files.loading }
        ui.onNodeWithTag("file-entry:source").performClick();ui.waitUntil { !files.loading }
        ui.onNodeWithTag("file-entry:a.txt").performTouchInput { longClick() };ui.onNodeWithTag("file-copy").performClick()
        ui.runOnIdle { files.back() };ui.waitUntil { !files.loading }
        ui.onNodeWithTag("file-entry:target").performClick();ui.waitUntil { !files.loading }
        ui.onNodeWithTag("file-task-dock-status",useUnmergedTree=true).assertTextEquals("确认粘贴")
        val row=ui.onNodeWithTag("file-entry:existing.txt").assertIsDisplayed().fetchSemanticsNode().boundsInRoot
        val list=ui.onNodeWithTag("file-list").fetchSemanticsNode().boundsInRoot
        assertTrue("actual task summary and clipboard must leave a file row",list.height>=row.height)
        assertEquals(FileTaskPhase.CONFIRMING,task.phase);assertEquals(before,opened.get())
        assertFalse(File(home,"target/a.txt").exists())
    }

    @Test fun actualCopyChangesDockFromConfirmationToFinishedThenImmutableHistory() {
        val task = prepare()
        ui.onNodeWithTag("file-task-dock-status", useUnmergedTree = true).assertTextEquals("确认粘贴")
        assertFalse(File(home, "target/a.txt").exists())
        val before = opened.get()
        ui.onNodeWithTag("file-task-dock").performClick()
        assertEquals("opening the dock does not create a connection", before, opened.get())
        assertEquals("opening the dock does not confirm a write", FileTaskPhase.CONFIRMING, task.phase)
        assertFalse(File(home, "target/a.txt").exists())
        ui.onNodeWithTag("file-task-confirm").performScrollTo().performClick()
        ui.waitUntil(15000) { !task.active && !task.recording }
        assertEquals(FileCopyState.COMPLETE, task.result?.state)
        ui.onNode(hasText("返回") or hasContentDescription("返回")).performClick()
        ui.onNodeWithTag("file-task-dock-status", useUnmergedTree = true).assertTextEquals("全部复制完成")
        assertEquals(File(home, "source/a.txt").readText(), File(home, "target/a.txt").readText())
        ui.runOnIdle { tasks.archive(task) };ui.waitUntil(10000) { task !in tasks.tasks && !tasks.contentReading }
        ui.onNodeWithTag("file-task-dock-status", useUnmergedTree = true).assertDoesNotExist()
        captureEmptyReview("history-only-home")
        val count = opened.get()
        ui.onNodeWithTag("home-task-history").assertIsDisplayed().performClick()
        ui.onNodeWithTag("file-tasks").assertIsDisplayed()
        assertEquals("viewing archived work never restarts IO", count, opened.get())
        assertEquals(File(home, "source/a.txt").readText(), File(home, "target/a.txt").readText())
    }
    @Test fun newerQueuedRequestDoesNotHideTheTaskWaitingForConfirmation() {
        val first = prepare()
        val copied = snapshot()
        val second = ui.runOnIdle {
            tasks.start(copied, FileLocation(FileEndpoint.APP, "second"), "second", ::connections)
            tasks.tasks.first().also { tasks.hide() }
        }
        ui.waitUntil(10000) { second.recorded && !second.recording }
        assertEquals(FileTaskPhase.QUEUED, second.phase)
        ui.onNodeWithTag("file-task-dock-status", useUnmergedTree = true).assertTextEquals("确认粘贴")
        ui.onNodeWithText("应用文件 / source → 应用文件 / target", useUnmergedTree = true).assertExists()
        ui.onNodeWithText("另有 1 个待处理任务", useUnmergedTree = true).assertIsDisplayed()
        assertEquals(FileTaskPhase.CONFIRMING, first.phase)
        assertFalse(File(home, "target/a.txt").exists());assertFalse(File(home, "second/a.txt").exists())
    }
    @Test fun unknownCurrentResultIsNeverPresentedAsSuccessfulCopy() {
        val request = FilePasteRequest.capture(snapshot(), target, "target")
        ui.runOnIdle { tasks.work.tasks.tasks.add(FileTask(request).apply {
            phase = FileTaskPhase.FINISHED
            result = FileCopyProgress(request.id, request.clipboardId, FileCopyState.UNKNOWN, emptyList())
        }) }
        ui.onNodeWithTag("file-task-dock-status", useUnmergedTree = true).assertTextEquals("复制结果未知，请核对实际位置")
        ui.onNodeWithTag("file-task-dock").performClick()
        ui.onNodeWithTag("file-task-status").assertTextEquals("复制结果未知，请核对实际位置")
        assertEquals(0, opened.get());assertFalse(File(home, "target/a.txt").exists())
    }
    @Test fun copiedSourceAndTaskDockLeaveAVisibleFileRowAndPasteOnNarrowScreens() {
        ui.runOnIdle { files.enter() };ui.waitUntil(10000) { !files.loading }
        ui.onNodeWithTag("file-entry:source").performClick();ui.waitUntil(10000) { !files.loading }
        ui.onNodeWithTag("file-entry:a.txt").performTouchInput { longClick() }
        ui.onNodeWithTag("file-copy").performClick()
        ui.runOnIdle { files.back() };ui.waitUntil(10000) { !files.loading }
        File(home, "target/existing.txt").writeText("unchanged destination")
        ui.onNodeWithTag("file-entry:target").performClick();ui.waitUntil(10000) { !files.loading }
        ui.onNodeWithTag("file-clipboard").assertIsDisplayed()
        ui.onNodeWithTag("file-paste").assertIsDisplayed().assertIsEnabled()
        ui.onNodeWithTag("file-task-dock").assertIsDisplayed()
        ui.onNodeWithTag("file-entry:existing.txt").assertIsDisplayed()
        val list = ui.onNodeWithTag("file-list").fetchSemanticsNode().boundsInRoot
        val row = ui.onNodeWithTag("file-entry:existing.txt").fetchSemanticsNode().boundsInRoot
        assertTrue("clipboard and task dock leave room for an actual file row", list.height >= row.height)
        assertTrue(tasks.tasks.isEmpty());assertEquals(0, opened.get())
        assertEquals("unchanged destination", File(home, "target/existing.txt").readText())
    }
}
