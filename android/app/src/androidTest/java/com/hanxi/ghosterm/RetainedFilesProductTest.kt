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
    @Test fun registeredPlanFindsItsRetainedCandidateWithoutACompletedItemCheckpoint() {
        openList()
        ui.runOnIdle { assertEquals("未关联记录的暂存",model.retainedLabel(model.retainedSnapshot!!.items.single())) }
        ui.runOnIdle { model.hideRetained() }
        val before=runBlocking { journal.load().single() }
        val plan=destination.parentFile!!.name
        assertNotEquals(before.intent.id,plan)
        runBlocking { journal.phase(before.intent.id,FileJournalPhase.CONFIRMING);journal.begin(before.intent.id,emptyList(),plan) }
        ui.runOnIdle { model.reloadHistory() };ui.waitUntil(10000) { !model.journalLoading }
        openList()
        val expected="任务记录：home → home"
        ui.runOnIdle {
            val item=model.retainedSnapshot!!.items.single()
            assertEquals(plan,item.executionPlanId);assertEquals(expected,model.retainedLabel(item))
        }
        ui.onNodeWithText(expected).assertExists()
        assertEquals("preview bytes",candidate.readText());assertTrue(model.history.single().items.isEmpty())
        ui.runOnIdle { model.hideRetained();model.removeHistory(model.history.single()) }
        ui.waitUntil(10000) { !model.journalLoading };openList()
        ui.runOnIdle { assertEquals("未关联记录的暂存",model.retainedLabel(model.retainedSnapshot!!.items.single())) }
        assertEquals("preview bytes",candidate.readText());assertEquals("keep original",original.readText())
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
        ui.onNode(hasText("返回任务") or hasContentDescription("返回任务")).performClick()
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
    private fun emptyRequest():File {
        val owner=privateDir(File(tasks,UUID.randomUUID().toString()))
        privateDir(File(owner,"source"));privateDir(File(owner,"destination"));return owner
    }
    @Test fun emptyDirectoryCleanupNeedsConfirmationAndPreservesHistoryAndFiles() {
        val owner=emptyRequest();val before=runBlocking { journal.load() };openList()
        ui.onNodeWithTag("retained-empty-directories").performScrollTo().performClick()
        ui.waitUntil(10000) { !model.retainedLoading }
        assertTrue(owner.isDirectory);ui.onNodeWithTag("retained-empty-cancel").performClick()
        ui.waitUntil(10000) { !model.retainedLoading && model.emptyRetainedReview==null && descriptors()==0 }
        assertTrue(owner.isDirectory)
        ui.onNodeWithTag("retained-empty-directories").performScrollTo().performClick()
        ui.waitUntil(10000) { !model.retainedLoading }
        ui.onNodeWithTag("retained-empty-confirm").performClick()
        ui.waitUntil(10000) { !model.retainedLoading && model.emptyRetainedReview==null }
        assertFalse(owner.exists());assertEquals("preview bytes",candidate.readText());assertEquals("keep original",original.readText())
        assertEquals(before,runBlocking { journal.load() });assertEquals(0,descriptors())
        ui.onNodeWithTag("retained-notice").assertTextEquals("已整理 1 个空目录 · 0 个位置已不存在")
    }
    @Test fun changedEmptyDirectoryShowsFailureAfterRefreshAndKeepsNewFile() {
        val owner=emptyRequest();val before=runBlocking { journal.load() };openList()
        ui.onNodeWithTag("retained-empty-directories").performScrollTo().performClick()
        ui.waitUntil(10000) { !model.retainedLoading }
        val newFile=File(owner,"destination/new.txt").apply { writeText("new bytes") }
        ui.onNodeWithTag("retained-empty-confirm").performClick()
        ui.waitUntil(10000) { !model.retainedLoading && model.emptyRetainedReview==null }
        ui.onNodeWithTag("retained-error").assertTextContains("1 项整理未确认",substring=true)
        assertEquals("new bytes",newFile.readText());assertEquals("preview bytes",candidate.readText())
        assertEquals(before,runBlocking { journal.load() });assertEquals(0,descriptors())
    }
    @Test fun clearingPageModelReleasesEmptyReviewWithoutDeletingDirectories() {
        val owner=emptyRequest();openList()
        ui.onNodeWithTag("retained-empty-directories").performScrollTo().performClick()
        ui.waitUntil(10000) { !model.retainedLoading }
        assertEquals(1,descriptors())
        ui.runOnIdle {
            val store=androidx.lifecycle.ViewModelStore();store.put("tasks",model);page=null;store.clear()
        }
        val released=runCatching { ui.waitUntil(5000) { descriptors()==0 } }
        assertTrue("cleared page must release the prepared root",released.isSuccess)
        assertTrue(owner.isDirectory);assertEquals("preview bytes",candidate.readText())
    }

}
