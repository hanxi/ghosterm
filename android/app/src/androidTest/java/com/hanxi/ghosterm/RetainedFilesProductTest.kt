package com.hanxi.ghosterm

import android.app.Application
import android.system.Os
import androidx.activity.ComponentActivity
import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.*
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.createAndroidComposeRule
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.hanxi.ghosterm.bridge.LocalPublicationFile
import com.hanxi.ghosterm.data.*
import com.hanxi.ghosterm.domain.*
import kotlinx.coroutines.runBlocking
import org.junit.*
import org.junit.Assert.*
import org.junit.runner.RunWith
import java.io.File
import java.util.UUID

@RunWith(AndroidJUnit4::class)
class RetainedFilesProductTest {
    @get:Rule val ui=createAndroidComposeRule<ComponentActivity>()
    private val context=InstrumentationRegistry.getInstrumentation().targetContext
    private lateinit var root:File
    private lateinit var tasks:File
    private lateinit var destination:File
    private lateinit var candidate:File
    private lateinit var original:File
    private lateinit var journal:FileTaskJournal
    private lateinit var model:FileTasksViewModel
    private var page by mutableStateOf<FileTasksViewModel?>(null)
    @Before fun setup() {
        root=File(context.cacheDir,"retained-ui-${UUID.randomUUID()}").apply { mkdirs() }
        tasks=privateDir(File(root,"file-tasks"))
        destination=privateDir(File(privateDir(File(tasks,UUID.randomUUID().toString())),"destination"))
        candidate=File(destination,UUID.randomUUID().toString()).apply { writeText("preview bytes");Os.chmod(path,384) }
        val home=File(root,"home").apply { mkdir() }
        original=File(home,"original.txt").apply { writeText("keep original") }
        val application=object:Application() { init { attachBaseContext(context) };override fun getFilesDir()=root }
        journal=FileTaskJournal(File(root,"file-history/tasks.db"))
        runBlocking {
            val snapshot=FileClipboardSnapshot.capture(FileLocation(FileEndpoint.APP,""),"home",
                LocalFileBrowser(home,context.contentResolver).browse(FileLocation(FileEndpoint.APP,"")).entries)
            val request=FilePasteRequest.capture(snapshot,FileLocation(FileEndpoint.APP,""),"home")
            journal.load();journal.accept(request)
        }
        ui.setContent { MaterialTheme { page?.let { FileTasksScreen(it) } } }
        model=ui.runOnIdle { FileTasksViewModel(application,FileWorkRuntime(application)).also { page=it } }
        ui.waitUntil(10000) { !model.journalLoading }
    }
    @After fun cleanup() {
        ui.runOnIdle { model.hideRetained() }
        ui.waitUntil(10000) { descriptors()==0 }
        root.deleteRecursively()
    }
    private fun privateDir(file:File)=file.apply { mkdirs();Os.chmod(path,448) }
    private fun descriptors()=File("/proc/self/fd").listFiles()!!.count {
        runCatching { Os.readlink(it.path).let { path -> path.endsWith("/${root.name}/file-tasks") || path.contains("/${root.name}/file-tasks/") } }.getOrDefault(false)
    }
    private fun openList() {
        ui.onNodeWithTag("file-retained-open").performClick()
        ui.waitUntil(10000) { !model.retainedLoading }
    }
    private fun openPreview() {
        ui.onNodeWithTag("retained-item").performScrollTo().performClick()
        ui.waitUntil(10000) { !model.retainedLoading }
        ui.onNodeWithTag("retained-preview").assertTextEquals("preview bytes")
    }
    @Test fun tapPreviewsAndOnlySeparateConfirmationDeletesWithoutRewritingHistory() {
        val before=runBlocking { journal.load() }
        openList();openPreview()
        assertTrue(candidate.exists());assertTrue(descriptors()>=4)
        ui.onNodeWithTag("retained-clear-confirm").assertDoesNotExist()
        ui.onNodeWithTag("retained-clear-request").performClick()
        assertTrue(candidate.exists())
        ui.onNodeWithText("保留文件").performClick()
        ui.waitUntil(10000) { !model.retainedLoading }
        assertTrue(candidate.exists())
        ui.onNodeWithText("返回列表").performClick()
        ui.waitUntil(10000) { descriptors()==0 }
        openPreview();ui.onNodeWithTag("retained-clear-request").performClick()
        ui.onNodeWithTag("retained-clear-confirm").performClick()
        ui.waitUntil(10000) { !model.retainedLoading && model.retainedReview==null }
        assertFalse(candidate.exists());assertEquals("keep original",original.readText())
        assertEquals(before,runBlocking { journal.load() })
        ui.onNodeWithTag("retained-notice").assertTextEquals("所选暂存文件已清除")
        ui.onNodeWithTag("retained-empty").assertExists();assertEquals(0,descriptors())
    }
    @Test fun changedFileIsPreservedAndFailureRemainsVisibleAfterAutomaticRefresh() {
        val before=runBlocking { journal.load() }
        openList();openPreview();candidate.appendText(" modified")
        ui.onNodeWithTag("retained-clear-request").performClick()
        ui.onNodeWithTag("retained-clear-confirm").performClick()
        ui.waitUntil(10000) { !model.retainedLoading && model.retainedReview==null }
        ui.onNodeWithTag("retained-error").assertTextContains("暂存文件已变化",substring=true)
        assertEquals("preview bytes modified",candidate.readText())
        assertEquals(before,runBlocking { journal.load() });assertEquals(0,descriptors())
    }
    @Test fun unknownStructureIsVisibleAndNeverReportedAsEmpty() {
        assertTrue(candidate.delete());File(destination,"unknown-name").writeText("keep unknown")
        openList()
        ui.onNodeWithTag("retained-incomplete").assertExists()
        ui.onNodeWithTag("retained-empty").assertDoesNotExist()
        assertEquals("keep unknown",File(destination,"unknown-name").readText())
        assertEquals("keep original",original.readText());assertTrue(model.tasks.isEmpty())
    }
    @Test fun returningFromPreviewReleasesPinnedFilesWithoutDeleting() {
        openList();openPreview()
        ui.onNodeWithText("返回列表").performClick()
        ui.waitUntil(10000) { descriptors()==0 }
        ui.onNodeWithText("返回任务").performClick()
        assertFalse(model.retainedVisible);assertEquals("preview bytes",candidate.readText())
        assertEquals("keep original",original.readText())
    }
    @Test fun explicitCleanupReleasesActualBudgetSlot() {
        val budget=LocalPublicationBudget(1024,1,0)
        LocalPublicationFile.directory(tasks,"").use { scope -> LocalPublicationFile.directory(destination,"").use { dir ->
            var rejected=false
            try { budget.reserve(dir,UUID.randomUUID().toString(),1,scope).close() } catch(_:Exception) { rejected=true }
            assertTrue("retained file must consume a slot before cleanup",rejected)
            openList();openPreview();ui.onNodeWithTag("retained-clear-request").performClick()
            ui.onNodeWithTag("retained-clear-confirm").performClick()
            ui.waitUntil(10000) { !model.retainedLoading && !candidate.exists() }
            budget.reserve(dir,UUID.randomUUID().toString(),1,scope).close()
        } }
        assertEquals("keep original",original.readText())
    }
}
