package com.hanxi.ghosterm

import android.app.Application
import android.content.ActivityNotFoundException
import androidx.activity.ComponentActivity
import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.*
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.ui.Modifier
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.createAndroidComposeRule
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.hanxi.ghosterm.data.*
import com.hanxi.ghosterm.domain.*
import kotlinx.coroutines.*
import org.junit.After
import org.junit.Assert.*
import org.junit.Before
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File
import java.util.UUID

@RunWith(AndroidJUnit4::class)
class ExternalPreviewProductTest {
    @get:Rule val ui=createAndroidComposeRule<ComponentActivity>()
    private lateinit var root: File
    private lateinit var model: FilesViewModel
    private lateinit var app: Application
    private lateinit var work: FileWorkRuntime
    private lateinit var cache: ExternalPreviewCache
    private var showScreen by mutableStateOf(false)
    private lateinit var reader: DelayedSource
    private val context get()=InstrumentationRegistry.getInstrumentation().targetContext
    private class DelayedSource(val local: LocalFileBrowser): FileBrowserSource by local, PreviewFileSource {
        var gate: CompletableDeferred<Unit>?=null
        val began=CompletableDeferred<Unit>();val ended=CompletableDeferred<Unit>()
        override suspend fun snapshot(location: FileLocation, entry: BrowserEntry, store: TransferStagingStore,
            progress: (Long,Long?)->Unit): TransferStagingStore.Snapshot {
            began.complete(Unit)
            try {
                // Model a provider returning late despite cancellation; actual staging remains bounded.
                return withContext(NonCancellable) { gate?.await();local.snapshot(location,entry,store,progress) }
            } finally { ended.complete(Unit) }
        }
    }
    @Before fun setup() {
        root=File(context.cacheDir,"external-product-${UUID.randomUUID()}").apply { mkdirs() }
        val home=File(root,"home").apply { mkdir() }
        File(home,"first.bin").writeBytes(byteArrayOf(0,1,2));File(home,"second.txt").writeText("second")
        app=object:Application() { init { attachBaseContext(context) };override fun getFilesDir()=root }
        work=FileWorkRuntime(app)
        cache=ExternalPreviewCache(context,File(root,"previews"),"com.hanxi.ghosterm.previews")
        reader=DelayedSource(LocalFileBrowser(home,context.contentResolver))
        InstrumentationRegistry.getInstrumentation().runOnMainSync { model=FilesViewModel(app,reader,work,cache);model.enter() }
        // A real Compose root keeps idling deterministic even for model-only cases.
        // Those cases do not mount FilesScreen, so they cannot accidentally auto-launch.
        ui.setContent { MaterialTheme { if(showScreen) FilesScreen(model,{}) else Box(Modifier.fillMaxSize()) } }
        waitFor { !model.loading && !work.tasks.journalLoading }
    }
    @After fun cleanup() {
        ui.runOnIdle { model.leave();reader.gate?.complete(Unit) }
        if(reader.began.isCompleted) runBlocking { withTimeout(5000) { reader.ended.await() } }
        root.deleteRecursively()
    }
    private fun waitFor(condition: ()->Boolean) { ui.waitUntil(10_000) { ui.runOnIdle(condition) } }
    private fun preview(name: String) {
        ui.runOnIdle { model.tap(model.entries.single { it.name==name }) }
        waitFor { model.preview?.let { it.text!=null || it.error!=null }==true }
    }
    @Test fun binaryPreviewOffersExplicitExternalOpenAndNeverStartsOnTap() {
        ui.runOnIdle { showScreen=true }
        preview("first.bin")
        assertNotNull(ui.runOnIdle { model.preview!!.error })
        ui.onNodeWithTag("file-open-external").assertIsEnabled()
        assertFalse(reader.began.isCompleted)
        reader.gate=CompletableDeferred()
        ui.onNodeWithTag("file-open-external").performClick()
        assertTrue("external-open button must start the raw reader",runBlocking {
            withTimeoutOrNull(2000) { reader.began.await();true }==true
        })
        ui.onNodeWithTag("file-open-external").assertIsNotEnabled()
        ui.runOnIdle { model.leave();reader.gate!!.complete(Unit) }
    }
    @Test fun explicitExternalRequestIsConsumedExactlyOnce() {
        preview("first.bin")
        ui.runOnIdle { model.openExternal() }
        waitFor { model.externalReady!=null }
        val copy=ui.runOnIdle { model.externalReady!! }
        var launched=0
        ui.runOnIdle { model.launchExternal(copy) { launched++;assertEquals(3L,it.bytes) };model.launchExternal(copy) { launched++ } }
        assertEquals(1,launched);assertNull(ui.runOnIdle { model.externalReady })
    }
    @Test fun leavingDuringLateDownloadCannotAutoOpenAndCleansTheCopy() {
        preview("first.bin");reader.gate=CompletableDeferred()
        ui.runOnIdle { model.openExternal() };waitFor { reader.began.isCompleted }
        ui.runOnIdle { model.leave();reader.gate!!.complete(Unit) }
        waitFor { reader.ended.isCompleted }
        waitFor { File(root,"previews").listFiles()?.none { it.name!="staging" } != false }
        assertNull(ui.runOnIdle { model.externalReady });assertFalse(ui.runOnIdle { model.externalPreparing })
    }
    @Test fun switchingFilesRejectsLateCopyAndKeepsNewPreview() {
        preview("first.bin");reader.gate=CompletableDeferred()
        ui.runOnIdle { model.openExternal() };waitFor { reader.began.isCompleted }
        preview("second.txt")
        ui.runOnIdle { reader.gate!!.complete(Unit) };waitFor { reader.ended.isCompleted }
        assertEquals("second",ui.runOnIdle { model.preview!!.text })
        assertNull(ui.runOnIdle { model.externalReady })
        ui.runOnIdle { model.openExternal() };waitFor { model.externalReady!=null }
        assertEquals("second.txt",ui.runOnIdle { model.externalReady!!.name })
    }
    @Test fun missingHandlerShowsErrorAndReleasesOnlyThePreparedCopy() {
        preview("first.bin");ui.runOnIdle { model.openExternal() };waitFor { model.externalReady!=null }
        val copy=ui.runOnIdle { model.externalReady!! }
        ui.runOnIdle { model.launchExternal(copy) { throw ActivityNotFoundException("no handler") } }
        waitFor { File(root,"previews").listFiles()?.none { it.name!="staging" }==true }
        assertTrue(ui.runOnIdle { model.externalError!!.contains("没有可打开") })
        assertArrayEquals(byteArrayOf(0,1,2),File(root,"home/first.bin").readBytes())
        assertNull(ui.runOnIdle { model.externalReady })
    }
    @Test fun readyCopyCannotLaunchAfterPreviewCloses() {
        preview("first.bin");ui.runOnIdle { model.openExternal() };waitFor { model.externalReady!=null }
        val ready=ui.runOnIdle { model.externalReady!! };var launches=0
        ui.runOnIdle { model.closePreview();model.launchExternal(ready) { launches++ } }
        assertEquals(0,launches);assertNull(ui.runOnIdle { model.externalReady })
    }

    @Test fun actualBackgroundStopCancelsPreparationAndRequiresAnotherExplicitOpen() {
        ui.runOnIdle { showScreen=true }
        preview("first.bin");reader.gate=CompletableDeferred()
        ui.onNodeWithTag("file-open-external").performClick();waitFor { reader.began.isCompleted }
        try {
            ui.activityRule.scenario.moveToState(androidx.lifecycle.Lifecycle.State.CREATED)
            InstrumentationRegistry.getInstrumentation().runOnMainSync {
                assertFalse("stopped presenter must cancel external preparation",model.externalPreparing)
                assertNull(model.externalReady)
            }
        } finally {
            reader.gate!!.complete(Unit)
            runBlocking { withTimeout(5000) { reader.ended.await() } }
            ui.activityRule.scenario.moveToState(androidx.lifecycle.Lifecycle.State.RESUMED)
        }
        assertNull(ui.runOnIdle { model.externalReady })
        ui.onNodeWithTag("file-open-external").assertIsEnabled()
    }

    @Test fun explicitCacheClearRequiresConfirmationAndKeepsOriginalFiles() {
        preview("first.bin");ui.runOnIdle { model.openExternal() };waitFor { model.externalReady!=null }
        val copy=ui.runOnIdle { model.externalReady!! }
        ui.runOnIdle { model.launchExternal(copy) {};model.confirmExternalClear() }
        waitFor { !model.externalClearing };assertEquals("unconfirmed cleanup must retain the copy",3L,runCatching { cache.info(copy.uri).second }.getOrNull())
        ui.runOnIdle { showScreen=true }
        ui.onNode(hasText("更多") or hasContentDescription("更多")).performClick();ui.onNodeWithTag("file-preview-cache-clear").performClick()
        ui.onNodeWithText("清理预览副本？").assertIsDisplayed();assertEquals(3L,cache.info(copy.uri).second)
        ui.onNodeWithText("取消").performClick();assertEquals(3L,cache.info(copy.uri).second)
        ui.onNode(hasText("更多") or hasContentDescription("更多")).performClick();ui.onNodeWithTag("file-preview-cache-clear").performClick()
        ui.onNodeWithTag("file-preview-cache-clear-confirm").performClick()
        waitFor { !model.externalClearing && model.externalClearResult!=null }
        assertEquals(1,ui.runOnIdle { model.externalClearResult!!.removed })
        ui.onNodeWithTag("file-preview-cache-clear-result").assertTextContains("已清理 1 个副本",substring=true)
        assertTrue(runCatching { cache.info(copy.uri) }.isFailure)
        assertArrayEquals(byteArrayOf(0,1,2),File(root,"home/first.bin").readBytes())
    }
    @Test fun cacheClearPreservesPreparedCopyUntilPresenterActuallyHandsItOff() {
        preview("first.bin");ui.runOnIdle { model.openExternal() };waitFor { model.externalReady!=null }
        val copy=ui.runOnIdle { model.externalReady!! }
        ui.runOnIdle { model.requestExternalClear();model.confirmExternalClear() }
        waitFor { !model.externalClearing && model.externalClearResult!=null }
        assertEquals(0,ui.runOnIdle { model.externalClearResult!!.removed })
        assertEquals(1,ui.runOnIdle { model.externalClearResult!!.retained });assertEquals(3L,cache.info(copy.uri).second)
        ui.runOnIdle { model.launchExternal(copy) {};model.requestExternalClear();model.confirmExternalClear() }
        waitFor { !model.externalClearing && model.externalClearResult!=null }
        assertEquals(1,ui.runOnIdle { model.externalClearResult!!.removed });assertTrue(runCatching { cache.info(copy.uri) }.isFailure)
    }

    @Test fun cacheClearOwnershipFailureIsVisibleAndDoesNotDeleteTheCopy() {
        preview("first.bin");ui.runOnIdle { model.openExternal() };waitFor { model.externalReady!=null }
        val copy=ui.runOnIdle { model.externalReady!! }
        val unowned=File(root,"previews/unowned.txt").apply { writeText("keep") }
        ui.runOnIdle { model.launchExternal(copy) {};showScreen=true;model.requestExternalClear();model.confirmExternalClear() }
        waitFor { !model.externalClearing }
        assertNotNull(ui.runOnIdle { model.externalClearError });assertNull(ui.runOnIdle { model.externalClearResult })
        ui.onNodeWithTag("file-preview-cache-clear-error").assertIsDisplayed()
        assertEquals(3L,cache.info(copy.uri).second);assertEquals("keep",unowned.readText())
    }

}
