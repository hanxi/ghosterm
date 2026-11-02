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

@RunWith(AndroidJUnit4::class)
class FileCleanupProductTest {
    @get:Rule val ui=createAndroidComposeRule<ComponentActivity>()
    private val context=InstrumentationRegistry.getInstrumentation().targetContext
    private lateinit var root:File
    private lateinit var application:Application
    private lateinit var original:FileJournalRecord
    private lateinit var copyJournal:FileTaskJournal
    private lateinit var file:File
    private lateinit var model:FileTasksViewModel
    private var page by mutableStateOf<FileTasksViewModel?>(null)
    @Before fun setup() {
        root=File(context.cacheDir,"cleanup-ui-${UUID.randomUUID()}").apply { mkdirs() }
        val home=File(root,"home").apply { mkdir() };File(home,"source").mkdir();File(home,"target").mkdir()
        File(home,"source/a").writeText("source");file=File(home,"target/a").apply { writeText("uncertain target") }
        application=object:Application() { init { attachBaseContext(context) };override fun getFilesDir()=root }
        val source=FileLocation(FileEndpoint.APP,"source");val target=FileLocation(FileEndpoint.APP,"target")
        val selected=runBlocking { LocalFileBrowser(home,context.contentResolver).browse(source).entries.single() }
        val request=FilePasteRequest.capture(FileClipboardSnapshot.capture(source,"source",listOf(selected)),target,"target")
        val item=FileCopyItemResult(FileScanEntry(source,selected,"a"),FileCopyItemState.UNKNOWN,target,"a",createdId="target/a")
        copyJournal=FileTaskJournal(File(root,"file-history/tasks.db"))
        runBlocking {
            copyJournal.load();copyJournal.accept(request);copyJournal.phase(request.id,FileJournalPhase.CONFIRMING);copyJournal.begin(request.id,listOf(item))
            copyJournal.finish(request.id,FileCopyProgress(request.id,request.clipboardId,FileCopyState.UNKNOWN,listOf(item)),false,null)
            original=copyJournal.load().single()
        }
        model=ui.runOnIdle { FileTasksViewModel(application,FileWorkRuntime(application)).also { page=it } }
        ui.setContent { MaterialTheme { page?.let { FileTasksScreen(it) } } }
        ui.waitUntil(10000) { !model.journalLoading }
    }
    @After fun finish() {
        ui.runOnIdle { model.cancelCleanup();page=null }
        ui.waitUntil(15000) { !model.cleanupBusy };root.deleteRecursively()
    }
    private fun review() {
        ui.onNodeWithTag("file-history-details").performScrollTo().performClick()
        ui.onNodeWithTag("file-cleanup-review:a:false").performScrollTo().performClick()
        ui.waitUntil(10000) { model.cleanupReview!=null || model.cleanupError!=null }
        assertNull(model.cleanupError)
    }
    @Test fun keepAndFreshConfirmationDefaultUncheckedThenDeletionLeavesOriginalCopyRecord() {
        review();ui.onNodeWithTag("file-cleanup-confirm").assertIsNotEnabled()
        ui.onNodeWithTag("file-cleanup-consent").assertIsOff().performClick().assertIsOn()
        assertTrue(file.exists());assertTrue(runBlocking { FileCleanupJournal(File(root,"file-cleanup/operations.db")).load().isEmpty() })
        ui.onNodeWithTag("file-cleanup-keep").performClick();assertTrue(file.exists())
        review();ui.onNodeWithTag("file-cleanup-consent").assertIsOff();ui.onNodeWithTag("file-cleanup-confirm").assertIsNotEnabled()
        ui.onNodeWithTag("file-cleanup-consent").performClick();ui.onNodeWithTag("file-cleanup-confirm").performClick()
        ui.waitUntil(10000) { !model.cleanupBusy && model.cleanupRecords.size==1 }
        assertNull(model.cleanupError);assertEquals(FileCleanupState.REMOVED,model.cleanupRecords.single().state);assertFalse(file.exists())
        assertEquals(original,runBlocking { copyJournal.load().single() })
        ui.onNodeWithTag("file-cleanup-records-open").performClick()
        ui.waitUntil(10000) { !model.cleanupBusy }
        ui.onNodeWithText("已收到删除成功确认").assertIsDisplayed()
        val row=model.cleanupRecords.single()
        ui.onNodeWithTag("file-cleanup-clear:${row.id}").performScrollTo().performClick()
        ui.onNodeWithTag("file-cleanup-clear-confirm").performClick()
        ui.waitUntil(10000) { !model.cleanupBusy && model.cleanupRecords.isEmpty() }
        assertEquals(original,runBlocking { copyJournal.load().single() })
    }
    @Test fun staleMetadataAndRemovedHistoryCannotDeleteCurrentTarget() {
        review();file.writeText("changed content with another size")
        ui.onNodeWithTag("file-cleanup-consent").performClick();ui.onNodeWithTag("file-cleanup-confirm").performClick()
        ui.waitUntil(10000) { !model.cleanupBusy && model.cleanupRecords.size==1 }
        assertEquals(FileCleanupState.CHANGED,model.cleanupRecords.single().state);assertTrue(file.exists())
        review()
        ui.runOnIdle { model.work.tasks.history.clear();model.confirmCleanup() }
        assertNull(model.cleanupReview);assertTrue(model.cleanupError?.contains("历史记录")==true)
        assertEquals("changed content with another size",file.readText())
        ui.onNodeWithTag("file-cleanup-error").performScrollTo().assertIsDisplayed()
    }
}
