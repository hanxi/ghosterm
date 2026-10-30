package com.hanxi.ghosterm

import android.accessibilityservice.AccessibilityService
import android.content.Intent
import android.os.SystemClock
import android.provider.DocumentsContract
import android.net.Uri
import android.view.accessibility.AccessibilityNodeInfo
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.createAndroidComposeRule
import androidx.lifecycle.ViewModelProvider
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.hanxi.ghosterm.data.LocalFileBrowser
import kotlinx.coroutines.*
import com.hanxi.ghosterm.data.TransferStagingStore
import android.os.ParcelFileDescriptor
import java.io.FileInputStream
import java.util.UUID
import org.junit.After
import org.junit.Assert.*
import org.junit.Before
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File

/** Real system tree picker, persistent URI grant, test-APK provider and Activity recreation. */
@RunWith(AndroidJUnit4::class)
class FilesSafTest {
    @get:Rule val ui=createAndroidComposeRule<MainActivity>()
    private val instrumentation=InstrumentationRegistry.getInstrumentation()
    private val automation get()=instrumentation.uiAutomation
    private val resolver get()=instrumentation.targetContext.contentResolver
    private fun model()=ViewModelProvider(ui.activity)[FilesViewModel::class.java]
    private fun <T> main(block:()->T):T=ui.runOnIdle(block)
    @Before fun setup() { revoke();ui.onNodeWithText("本机文件").performClick();await { !model().loading } }
    @After fun cleanup() { main { model().leave() };revoke() }
    private fun revoke() {
        resolver.persistedUriPermissions.filter { it.uri.authority=="com.hanxi.ghosterm.test.browser" }.forEach {
            resolver.releasePersistableUriPermission(it.uri,(if(it.isReadPermission) Intent.FLAG_GRANT_READ_URI_PERMISSION else 0) or
                (if(it.isWritePermission) Intent.FLAG_GRANT_WRITE_URI_PERMISSION else 0))
        }
    }
    private fun nodes():List<AccessibilityNodeInfo> {
        val list=mutableListOf<AccessibilityNodeInfo>()
        fun visit(node:AccessibilityNodeInfo?) { if(node==null)return;list+=node;for(i in 0 until node.childCount)visit(node.getChild(i)) }
        visit(automation.rootInActiveWindow);return list
    }
    private fun await(state:()->Boolean) {
        val end=SystemClock.elapsedRealtime()+10000
        while(SystemClock.elapsedRealtime()<end) { if(main(state))return;SystemClock.sleep(30) }
        fail("Timed out: "+nodes().take(40).joinToString { "${it.text?.take(80)}/${it.contentDescription?.take(80)}" })
    }
    private fun systemClick(vararg names:String):Boolean {
        for(found in nodes().filter { it.isVisibleToUser && (it.text?.toString() in names || it.contentDescription?.toString() in names) }) {
            var node:AccessibilityNodeInfo?=found
            while(node!=null) { if(node.isClickable && node.performAction(AccessibilityNodeInfo.ACTION_CLICK))return true;node=node.parent }
        };return false
    }
    private fun picker() {
        ui.onNodeWithTag("file-location").performClick();ui.onNodeWithText("选择设备文件夹").performClick()
        await { nodes().any { it.packageName?.toString()?.contains("documentsui")==true } }
    }
    private fun grant() {
        picker()
        if(!nodes().any { it.text?.toString()=="notes.txt" }) {
            if(!systemClick("Ghosterm Browser Files")) {
                assertTrue("system roots accessible",systemClick("Show roots","Open navigation drawer","显示根目录","显示位置"))
                await { systemClick("Ghosterm Browser Files") }
            }
        }
        await { systemClick("Use this folder","USE THIS FOLDER","使用此文件夹") }
        await { systemClick("Allow","ALLOW","允许") || model().location.endpoint.treeUri!=null }
        await { !model().loading && model().location.endpoint.treeUri!=null && !model().choosingLocation }
        assertNull(main { model().error })
        ui.onNodeWithTag("file-entry:notes.txt").assertIsDisplayed()
    }
    @Test fun realTreeGrantReadsOpaqueIdsSurvivesRotationAndDetectsRevocation() {
        grant();val owner=main { model() };val endpoint=main { owner.location.endpoint }
        assertEquals("tree:root",endpoint.rootId)
        assertTrue(resolver.persistedUriPermissions.any { it.uri.toString()==endpoint.treeUri && it.isReadPermission })
        val fresh=LocalFileBrowser(File(instrumentation.targetContext.filesDir,"home"),resolver)
        assertTrue(runBlocking { fresh.endpoints() }.any { it.id==endpoint.id })
        ui.onNodeWithTag("file-entry:Folder").performClick();await { !model().loading }
        assertEquals("opaque:folder",main { model().location.directoryId })
        ui.onNodeWithTag("file-entry:你好.txt").performClick();await { model().preview?.text!=null }
        ui.onNodeWithTag("file-preview").assertTextEquals("tree child 中文\n")
        ui.activityRule.scenario.recreate()
        ui.onNodeWithTag("file-preview").assertTextEquals("tree child 中文\n")
        assertSame(owner,main { model() })
        ui.onNodeWithText("返回").performClick();ui.onNodeWithText("返回").performClick();await { !model().loading }
        revoke();main { model().reload() };await { !model().loading }
        ui.onNodeWithTag("file-list-error").assertTextContains("授权已失效",substring=true)
        ui.onNodeWithTag("file-list-empty").assertDoesNotExist()
        grant()
    }
    @Test fun pickerCancelNeverCreatesGrantOrChangesLocation() {
        val before=main { model().location }
        picker();assertTrue(automation.performGlobalAction(AccessibilityService.GLOBAL_ACTION_BACK))
        await { !model().pickerPending }
        assertEquals(before,main { model().location });assertTrue(resolver.persistedUriPermissions.none { it.uri.authority=="com.hanxi.ghosterm.test.browser" })
        ui.onNodeWithText("文件位置").assertIsDisplayed()
    }
    private fun awaitStreams(tree:String,expected:Int):Int {
        val uri=DocumentsContract.buildDocumentUriUsingTree(Uri.parse(tree),"opaque:slow")
        val end=SystemClock.elapsedRealtime()+2000
        while(SystemClock.elapsedRealtime()<end) {
            val active=resolver.query(uri,arrayOf(DocumentsContract.Document.COLUMN_SUMMARY),null,null,null)!!.use {
                check(it.moveToFirst());it.getString(0)
            }
            if(active.substringBefore(';')=="active=$expected") return active.substringAfter(";natural=").toInt()
            SystemClock.sleep(30)
        }
        throw AssertionError("provider pipe must reach active=$expected before its four-second natural completion")
    }
    @Test fun providerErrorsEmptyUnknownSizeAndCancelledStreamRemainDistinct() {
        grant()
        ui.onNodeWithTag("file-entry:Empty").performClick();await { !model().loading }
        ui.onNodeWithTag("file-list-empty").assertIsDisplayed();ui.onNodeWithText("返回").performClick();await { !model().loading }
        ui.onNodeWithTag("file-entry:Denied").performClick();await { !model().loading }
        ui.onNodeWithTag("file-list-error").assertExists();ui.onNodeWithTag("file-list-empty").assertDoesNotExist()
        ui.onNodeWithText("返回").performClick();await { !model().loading }
        ui.onNodeWithTag("file-entry:unknown-large.txt").performClick();await { model().preview?.error!=null }
        ui.onNodeWithTag("file-preview-error").assertTextContains("256 KiB",substring=true)
        ui.onNodeWithText("返回").performClick()
        ui.onNodeWithTag("file-entry:slow.txt").performClick();await { model().preview!=null }
        assertNull(main { model().preview?.text })
        val tree=main { model().location.endpoint.treeUri!! }
        val naturalEnds=awaitStreams(tree,1)
        ui.onNodeWithText("返回").performClick()
        assertEquals("stream closed by cancellation, not natural completion",naturalEnds,awaitStreams(tree,0))
        ui.onNodeWithTag("file-entry:notes.txt").performClick()
        await { model().preview?.text!=null };ui.onNodeWithTag("file-preview").assertTextEquals("provider notes 🔑\n")
        ui.onNodeWithTag("file-preview").assertTextEquals("provider notes 🔑\n")
        assertEquals("opaque:note",main { model().preview!!.entry.id })
    }
    @Test fun grantedDocumentsStageCompleteBytesAndRevocationPreventsNewSnapshots() {
        grant()
        val location=main { model().location }
        val directory=File(instrumentation.targetContext.cacheDir,"saf-stage-${UUID.randomUUID()}")
        val source=LocalFileBrowser(File(instrumentation.targetContext.filesDir,"home"),resolver)
        TransferStagingStore(directory,1024*1024,2*1024*1024,0).use { store ->
            try { runBlocking {
                val entries=source.list(location)
                val note=entries.single { it.id=="opaque:note" }
                source.snapshot(location,note,store).use { snapshot ->
                    snapshot.withDescriptor { fd,size ->
                        val bytes=ParcelFileDescriptor.fromFd(fd).use { FileInputStream(it.fileDescriptor).readBytes() }
                        assertEquals("provider notes 🔑\n",bytes.toString(Charsets.UTF_8));assertEquals(bytes.size.toLong(),size)
                    }
                }
                source.snapshot(location,entries.single { it.id=="opaque:large" },store).use { snapshot ->
                    assertEquals(256*1024+1L,snapshot.size)
                    snapshot.withDescriptor { fd,_ ->
                        val bytes=ParcelFileDescriptor.fromFd(fd).use { FileInputStream(it.fileDescriptor).readBytes() }
                        assertTrue(bytes.all { it==97.toByte() });assertEquals(256*1024+1,bytes.size)
                    }
                }
                val job=launch { source.snapshot(location,entries.single { it.id=="opaque:slow" },store).use { fail("cancelled SAF source published") } }
                yield()
                val natural=withContext(Dispatchers.IO) { awaitStreams(location.endpoint.treeUri!!,1) }
                job.cancelAndJoin()
                assertEquals(natural,withContext(Dispatchers.IO) { awaitStreams(location.endpoint.treeUri!!,0) })
                withTimeout(5000) { while(store.reservedBytes!=0L) delay(10) }
                revoke()
                var failed=false
                try { source.snapshot(location,note,store).close() } catch(_:Exception) { failed=true }
                assertTrue("revoked provider must not be read",failed)
                assertEquals(0L,store.reservedBytes);assertTrue(directory.list()!!.isEmpty())
            } } finally { directory.deleteRecursively() }
        }
    }

}
