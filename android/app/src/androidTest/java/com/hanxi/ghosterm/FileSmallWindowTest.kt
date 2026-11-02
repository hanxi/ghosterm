package com.hanxi.ghosterm

import android.app.Application
import android.os.SystemClock
import androidx.activity.ComponentActivity
import androidx.compose.runtime.*
import androidx.compose.ui.semantics.SemanticsActions
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.createAndroidComposeRule
import androidx.compose.ui.text.TextLayoutResult
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.hanxi.ghosterm.data.*
import com.hanxi.ghosterm.domain.*
import kotlinx.coroutines.runBlocking
import org.junit.*
import org.junit.Assert.*
import org.junit.rules.ExternalResource
import org.junit.rules.TestName
import org.junit.runner.RunWith
import java.io.File
import java.io.FileInputStream
import java.util.UUID

/** Native small landscape window and system font scale, with actual files/preflight. */
@RunWith(AndroidJUnit4::class)
class FileSmallWindowTest {
    private val instrumentation get() = InstrumentationRegistry.getInstrumentation()
    private fun shell(command: String) = instrumentation.uiAutomation.executeShellCommand(command).use {
        FileInputStream(it.fileDescriptor).bufferedReader().use { reader -> reader.readText() }
    }
    @get:Rule(order = -1) val testName = TestName()
    @get:Rule(order = 0) val geometry = object : ExternalResource() {
        private var size = "reset"
        private var density = "reset"
        private var rotationCommand = "set-user-rotation"
        private val system = linkedMapOf<String, String>()
        override fun before() {
            rotationCommand = if ("set-user-rotation" in shell("wm help")) "set-user-rotation" else "user-rotation"
            for (key in listOf("font_scale", "accelerometer_rotation", "user_rotation"))
                system[key] = shell("settings get system $key").trim()
            size = shell("wm size").lineSequence().firstOrNull { it.startsWith("Override size:") }?.substringAfter(":")?.trim() ?: "reset"
            density = shell("wm density").lineSequence().firstOrNull { it.startsWith("Override density:") }?.substringAfter(":")?.trim() ?: "reset"
            try {
                shell("settings put system font_scale 2.0")
                shell("wm $rotationCommand lock 0")
                val portrait = testName.methodName.startsWith("portrait")
                shell("wm density 160"); shell(if(portrait) "wm size 320x640" else "wm size 640x320")
                val deadline = SystemClock.elapsedRealtime() + 10_000
                while (SystemClock.elapsedRealtime() < deadline) {
                    val config = instrumentation.targetContext.resources.configuration
                    if ((if(portrait) config.screenWidthDp < config.screenHeightDp else config.screenWidthDp > config.screenHeightDp) && config.fontScale == 2f && config.densityDpi == 160) return
                    SystemClock.sleep(40)
                }
                fail("Fixture must reach the requested native orientation and system font scale 2 before opening the Activity")
            } catch (failure: Throwable) {
                restore(); throw failure
            }
        }
        override fun after() = restore()
        private fun restore() {
            shell("wm size $size"); shell("wm density $density")
            shell("wm $rotationCommand ${if (system["accelerometer_rotation"] == "1") "free" else "lock"} ${system["user_rotation"]?.takeUnless { it == "null" } ?: "0"}")
            for ((key, value) in system)
                shell(if (value == "null") "settings delete system $key" else "settings put system $key $value")
        }
    }
    @get:Rule(order = 1) val ui = createAndroidComposeRule<ComponentActivity>()
    private lateinit var root: File
    private lateinit var home: File
    private lateinit var files: FilesViewModel
    private lateinit var tasks: FileTasksViewModel
    private val content = "short window actual file\nsecond line\n"
    private val source = FileLocation(FileEndpoint.APP, "source")
    private val target = FileLocation(FileEndpoint.APP, "target")
    private var opens = 0
    private var shown by mutableStateOf(true)
    @Before fun prepare() {
        val context = instrumentation.targetContext
        root = File(context.cacheDir, "small-files-${UUID.randomUUID()}").apply { mkdirs() }
        home = File(root, "home").apply { mkdirs() }
        File(home, "source").mkdir(); File(home, "target").mkdir()
        File(home, "source/a.txt").writeText(content)
        val app = object : Application() { init { attachBaseContext(context) }; override fun getFilesDir() = root }
        val work = FileWorkRuntime(app)
        val hosts = HostsViewModel(app)
        files = FilesViewModel(app, work = work); tasks = FileTasksViewModel(app, work)
        val runtime = RuntimeInfoViewModel()
        ui.setContent { if(shown) GhostermApp(hosts, runtime, files = files, fileTasks = tasks) }
        ui.waitUntil(10_000) { hosts.snapshot != null && !hosts.busy && !tasks.journalLoading }
        ui.runOnIdle { files.enter() }; ui.waitUntil(10_000) { !files.loading }
    }
    @After fun cleanup() {
        if (::tasks.isInitialized) {
            ui.runOnIdle { tasks.tasks.toList().forEach(tasks::cancel); files.leave() }
            ui.waitUntil(15_000) { !tasks.busy && !tasks.contentReading }
        }
        ui.runOnIdle { shown = false }
        if (::root.isInitialized) root.deleteRecursively()
    }
    private fun fileRow(name: String) {
        ui.waitUntil(10_000) { !files.loading && files.visibleEntries.any { it.name == name } }
        val list = ui.onNodeWithTag("file-list").fetchSemanticsNode().boundsInRoot
        assertTrue("The list must retain a 48px viewport before scrolling: $list", list.height >= 48f)
        ui.onNodeWithTag("file-list").performScrollToNode(hasTestTag("file-entry:$name"))
        val bounds = ui.onNodeWithTag("file-entry:$name").assertIsDisplayed().fetchSemanticsNode().boundsInRoot
        assertTrue("A full 48px touch target must remain visible: $name bounds=$bounds", bounds.height >= 48f)
        val whole = ui.onNodeWithTag("file-entry:$name").getUnclippedBoundsInRoot()
        assertEquals("The whole file row must fit the visible list", (whole.bottom - whole.top).value, bounds.height, .5f)
    }
    @Test fun landscapeFontTwoKeepsFileRowsAndPreviewReadable() {
        fileRow("source")
        ui.onNodeWithTag("file-location").assertTextContains("本机", substring = true)
        ui.onNodeWithTag("file-entry:source").performClick(); ui.waitUntil(10_000) { !files.loading }
        fileRow("a.txt")
        val location = files.location
        ui.onNodeWithTag("file-entry:a.txt").performClick()
        ui.waitUntil(10_000) { files.preview?.text != null }
        val node = ui.onNodeWithTag("file-preview").performScrollTo().assertIsDisplayed().assertTextEquals(content)
        val layouts = mutableListOf<TextLayoutResult>()
        node.performSemanticsAction(SemanticsActions.GetTextLayoutResult) { it(layouts) }
        assertTrue(layouts.isNotEmpty())
        assertTrue("At least one whole preview line must be readable", node.fetchSemanticsNode().boundsInRoot.height >= layouts.first().getLineBottom(0) - layouts.first().getLineTop(0))
        ui.onNodeWithText("返回").performClick()
        ui.runOnIdle { assertEquals(location, files.location) }
        fileRow("a.txt")
        assertEquals(content, File(home, "source/a.txt").readText())
    }
    @Test fun compactDockPreservesConfirmationAndUnknownWithoutStartingWork() {
        val copied = runBlocking { FileClipboardSnapshot.capture(source, "source", LocalFileBrowser(home, instrumentation.targetContext.contentResolver).browse(source).entries) }
        val task = ui.runOnIdle {
            tasks.start(copied, target, "target") { request ->
                opens++
                FileTaskConnections(request.source.endpoint, request.destination.endpoint, { _, _ -> error("local only") }, { _, _, _ -> error("no authentication") })
            }
            tasks.tasks.first().also { tasks.hide() }
        }
        ui.waitUntil(10_000) { task.phase == FileTaskPhase.CONFIRMING || !task.active }
        assertEquals(FileTaskPhase.CONFIRMING, task.phase)
        fileRow("source")
        ui.onNodeWithTag("file-task-dock-status", useUnmergedTree = true).assertTextContains("确认粘贴", substring = true)
        val before = opens
        ui.onNodeWithTag("file-task-dock").assertIsDisplayed().performClick()
        ui.onNodeWithTag("file-tasks").assertIsDisplayed()
        assertEquals(before, opens); assertEquals(FileTaskPhase.CONFIRMING, task.phase)
        assertFalse(File(home, "target/a.txt").exists())
        ui.onNodeWithText("返回").performClick()
        ui.runOnIdle { tasks.cancel(task) }
        ui.waitUntil(15_000) { !task.active && !task.recording }
        ui.runOnIdle {
            assertFalse(tasks.visible); assertTrue(files.visible)
            val request = FilePasteRequest.capture(copied, target, "target")
            tasks.work.tasks.tasks.add(0, FileTask(request).apply {
                phase = FileTaskPhase.FINISHED
                result = FileCopyProgress(request.id, request.clipboardId, FileCopyState.UNKNOWN, emptyList())
            })
        }
        ui.onNodeWithTag("file-task-dock-status", useUnmergedTree = true).assertTextContains("复制结果未知", substring = true)
        ui.onNodeWithTag("file-task-dock").performClick()
        ui.onNodeWithTag("file-task-status").assertTextEquals("复制结果未知，请核对实际位置")
        assertEquals(before, opens); assertFalse(File(home, "target/a.txt").exists())
    }

    @Test fun copiedFilesStayVisibleThroughSelectionDetailsAndConfirmedPaste() {
        ui.onNodeWithTag("file-entry:source").performClick(); ui.waitUntil(10_000) { !files.loading }
        fileRow("a.txt")
        ui.onNodeWithTag("file-entry:a.txt").performTouchInput { longClick() }
        fileRow("a.txt")
        ui.onNodeWithTag("file-copy").assertIsDisplayed().performClick()
        ui.waitUntil(10_000) { files.clipboard != null && !files.selecting }
        val snapshot = checkNotNull(files.clipboard)
        ui.onNodeWithTag("file-clipboard-names", useUnmergedTree = true).assertIsDisplayed().assertTextContains("a.txt", substring = true)
        ui.onNodeWithTag("file-clipboard-details").performClick()
        ui.onNodeWithTag("file-clipboard-items").performScrollToNode(hasTestTag("file-clipboard-item:${snapshot.items.single().id}"))
        val item = ui.onNodeWithTag("file-clipboard-item:${snapshot.items.single().id}").assertIsDisplayed()
        assertTrue("Copied item remains readable in the actual sheet", item.fetchSemanticsNode().boundsInRoot.height >= 48f)
        ui.onNodeWithText("返回文件").performClick()
        ui.runOnIdle { files.back() }; ui.waitUntil(10_000) { !files.loading }
        File(home, "target/existing.txt").writeText("preserve destination")
        fileRow("target"); ui.onNodeWithTag("file-entry:target").performClick()
        ui.waitUntil(10_000) { !files.loading }
        val location = files.location
        fileRow("existing.txt")
        ui.onNodeWithTag("file-paste").assertIsDisplayed().assertIsEnabled().performClick()
        ui.runOnIdle { assertTrue("Paste must present its task without writing", tasks.visible) }
        ui.waitUntil(10_000) { tasks.tasks.isNotEmpty() && tasks.tasks.first().phase == FileTaskPhase.CONFIRMING }
        assertFalse("Showing paste confirmation must not write", File(home, "target/a.txt").exists())
        ui.onNodeWithTag("file-task-confirm").performScrollTo().performClick()
        ui.waitUntil(15_000) { !tasks.tasks.first().active && !tasks.tasks.first().recording }
        assertEquals(FileCopyState.COMPLETE, tasks.tasks.first().result?.state)
        assertEquals(content, File(home, "target/a.txt").readText())
        assertEquals(content, File(home, "source/a.txt").readText())
        assertEquals("preserve destination", File(home, "target/existing.txt").readText())
        ui.onNodeWithText("返回").performClick()
        ui.runOnIdle { assertEquals(location, files.location); assertEquals(snapshot.id, files.clipboard?.id) }
        fileRow("a.txt"); fileRow("existing.txt")
        ui.onNodeWithText("清空").performClick()
        ui.onNodeWithText("清空复制清单？").assertIsDisplayed()
        ui.runOnIdle { assertEquals(snapshot.id, files.clipboard?.id) }
        ui.onNodeWithText("取消").performClick()
        ui.runOnIdle { assertEquals(snapshot.id, files.clipboard?.id) }
    }
    @Test fun portraitFontTwoKeepsCopiedFilesAndConfirmedPaste() {
        copiedFilesStayVisibleThroughSelectionDetailsAndConfirmedPaste()
    }

}
