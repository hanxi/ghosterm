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
import com.hanxi.ghosterm.data.FileTreeScanner
import com.hanxi.ghosterm.domain.*
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
class SafPreviewAssetsTest {
    @get:Rule val ui=createAndroidComposeRule<MainActivity>()
    private val instrumentation=InstrumentationRegistry.getInstrumentation()
    private val automation get()=instrumentation.uiAutomation
    private val resolver get()=instrumentation.targetContext.contentResolver
    private var grantedTree:Uri?=null
    private fun model()=ViewModelProvider(ui.activity)[FilesViewModel::class.java]
    private fun <T> main(block:()->T):T=ui.runOnIdle(block)
    @Before fun setup() { revoke();ui.onNodeWithText("本机文件").performClick();await { !model().loading } }
    @After fun cleanup() {
        releaseProducer()
        main { model().requestClipboardClear();model().confirmClipboardClear();model().leave() }
        revoke()
    }
    private fun revoke() {
        resolver.persistedUriPermissions.filter { it.uri.authority=="com.hanxi.ghosterm.test.previewassets" }.forEach {
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
            when(tapLegacyDocumentRow(instrumentation,found)) { true -> return true; false -> continue; null -> Unit }
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
        assertTrue("system roots accessible",systemClick("Show roots","Open navigation drawer","显示根目录","显示位置"))
        await { systemClick("Ghosterm Preview Assets") }
        await { systemClick("Use this folder","USE THIS FOLDER","使用此文件夹","ALLOW ACCESS TO \"GHOSTERM PREVIEW ASSETS\"") }
        await { systemClick("Allow","ALLOW","允许") || model().location.endpoint.treeUri!=null }
        await { !model().loading && model().location.endpoint.treeUri!=null && !model().choosingLocation }
        assertNull(main { model().error })
        grantedTree=Uri.parse(main { model().location.endpoint.treeUri!! })
        ui.onNodeWithTag("file-list").performScrollToNode(hasTestTag("file-entry:segment.txt"))
        ui.onNodeWithTag("file-entry:segment.txt").assertIsDisplayed()
    }
    private fun open(name:String) {
        ui.onNodeWithTag("file-list").performScrollToNode(hasTestTag("file-entry:$name"))
        ui.onNodeWithTag("file-entry:$name").performClick()
        await { model().preview?.let { it.text!=null || it.error!=null }==true }
    }
    private fun back() { ui.onNodeWithText("返回").performClick();await { model().preview==null } }
    @Test fun segmentUsesAbsoluteOffsetAndNeverReadsOutsideItsDeclaredBounds() {
        grant();open("segment.txt")
        ui.onNodeWithTag("file-preview").assertTextEquals("片段 UTF-8 🔑\n")
        back();open("positioned.txt")
        ui.onNodeWithTag("file-preview").assertTextEquals("片段 UTF-8 🔑\n")
        back();open("zero.txt")
        ui.onNodeWithTag("file-preview").assertTextEquals("")
    }
    @Test fun prematureEofCannotDisplayPartialUtf8AsACompleteDocument() {
        grant();open("short.txt")
        assertNotNull("declared fragment must be complete",main { model().preview!!.error })
        ui.onNodeWithTag("file-preview").assertDoesNotExist()
        back();open("segment.txt");ui.onNodeWithTag("file-preview").assertTextEquals("片段 UTF-8 🔑\n")
    }
    @Test fun reliablePipeFailureCannotBecomeSuccessfulText() {
        grant();open("error.txt")
        assertNotNull("producer error must remain visible",main { model().preview!!.error })
        ui.onNodeWithTag("file-preview").assertDoesNotExist()
        back();open("pipe.txt");ui.onNodeWithTag("file-preview").assertTextEquals("normal pipe\n")
    }
    private fun releaseProducer() { resolver.call(Uri.parse("content://com.hanxi.ghosterm.test.previewassets"),"preview:release",null,null) }
    private fun producerSent()=resolver.call(Uri.parse("content://com.hanxi.ghosterm.test.previewassets"),"preview:state",null,null)!!.getBoolean("sent")
    @Test fun declaredPipeWaitsForProducerCloseBeforeClaimingSuccess() {
        grant()
        ui.onNodeWithTag("file-list").performScrollToNode(hasTestTag("file-entry:delayed-error.txt"))
        ui.onNodeWithTag("file-entry:delayed-error.txt").performClick()
        try {
            await { producerSent() }
            val finished=runBlocking { withTimeoutOrNull(1500) {
                while(main { model().preview?.let { it.text!=null || it.error!=null }!=true })delay(10)
                true
            } }
            assertFalse("declared bytes do not replace producer EOF",finished==true)
        } finally { releaseProducer() }
        await { model().preview?.error!=null }
        ui.onNodeWithTag("file-preview").assertDoesNotExist()
        back();open("pipe.txt");ui.onNodeWithTag("file-preview").assertTextEquals("normal pipe\n")
    }
    @Test fun pipeOffsetAndSpecialDescriptorsAreExplicitlyRejected() {
        grant();open("pipe-offset.txt")
        assertNotNull("non-seekable asset cannot silently ignore its offset",main { model().preview!!.error })
        ui.onNodeWithTag("file-preview").assertDoesNotExist()
        back();open("special.txt")
        assertNotNull("special fd is not an empty document",main { model().preview!!.error })
        ui.onNodeWithTag("file-preview").assertDoesNotExist()
        back();open("zero.txt");ui.onNodeWithTag("file-preview").assertTextEquals("")
    }
    @Test fun logicalSizeMismatchDoesNotSilentlyChangeTheDocumentBeingPreviewed() {
        grant();open("size-mismatch.txt")
        assertNotNull("fresh metadata and declared segment size disagree",main { model().preview!!.error })
        ui.onNodeWithTag("file-preview").assertDoesNotExist()
        back();open("segment.txt");ui.onNodeWithTag("file-preview").assertTextEquals("片段 UTF-8 🔑\n")
    }
    @Test fun normalPipeAndZeroLengthStayReadable() {
        grant();open("pipe.txt");ui.onNodeWithTag("file-preview").assertTextEquals("normal pipe\n")
        back();open("zero.txt");ui.onNodeWithTag("file-preview").assertTextEquals("")
    }

    @Test fun invalidAssetRangeIsExplicitlyRejected() {
        grant();open("invalid-range.txt")
        assertNotNull("invalid declared range is not unknown length",main { model().preview!!.error })
        ui.onNodeWithTag("file-preview").assertDoesNotExist()
        back();open("pipe.txt");ui.onNodeWithTag("file-preview").assertTextEquals("normal pipe\n")
    }
    @Test fun unknownDescriptorLengthStillChecksTheCurrentLogicalSize() {
        grant();open("stale-size.txt")
        assertNotNull("fresh logical size must agree with actual bytes",main { model().preview!!.error })
        ui.onNodeWithTag("file-preview").assertDoesNotExist()
        back();open("segment.txt");ui.onNodeWithTag("file-preview").assertTextEquals("片段 UTF-8 🔑\n")
    }
    private fun activeReaders():Int {
        val tree=checkNotNull(grantedTree)
        val document=DocumentsContract.buildDocumentUriUsingTree(tree,"asset:root")
        return resolver.query(document,arrayOf(DocumentsContract.Document.COLUMN_SUMMARY),null,null,null)!!.use {
            check(it.moveToFirst());it.getString(0).removePrefix("active=").toInt()
        }
    }
    private fun assertReadersClosed() {
        val closed=runBlocking { withTimeoutOrNull(2000) { while(activeReaders()!=0)delay(10);true } }
        assertTrue("provider descriptor close must be acknowledged",closed==true)
    }
    @Test fun regularPreviewClosesProviderDescriptorsAfterSuccessAndReadFailure() {
        grant();open("segment.txt");ui.onNodeWithTag("file-preview").assertTextEquals("片段 UTF-8 🔑\n")
        assertReadersClosed()
        back();open("short.txt");assertNotNull(main { model().preview!!.error })
        assertReadersClosed()
    }

    @Test fun normalUnknownLengthPipeStaysReadable() {
        grant();open("pipe.txt");ui.onNodeWithTag("file-preview").assertTextEquals("normal pipe\n")
    }

    private fun rawSnapshotMustWaitForProducer(name:String) {
        grant()
        val context=instrumentation.targetContext
        val location=main { model().location }
        val source=LocalFileBrowser(File(context.filesDir,"home"),resolver)
        val entry=runBlocking { source.list(location) }.single { it.name==name }
        val directory=File(context.cacheDir,"saf-preview-raw-${UUID.randomUUID()}")
        val store=TransferStagingStore(directory,1024,4096)
        val scope=CoroutineScope(SupervisorJob()+Dispatchers.IO)
        val call=scope.async { source.snapshot(location,entry,store) }
        try {
            await { producerSent() }
            val completed=runBlocking { withTimeoutOrNull(1500) { call.await();true } }
            assertFalse("raw snapshot must wait for reliable producer completion",completed==true)
            releaseProducer()
            var failure:Exception?=null
            runBlocking { try { withTimeout(5000) { call.await() }.close() } catch(error:Exception) { failure=error } }
            assertNotNull("producer failure must not become an external/copy snapshot",failure)
        } finally {
            releaseProducer();call.cancel()
            runBlocking { call.join();store.closeAndAwait() }
            assertEquals(0L,store.reservedBytes)
            scope.cancel();directory.deleteRecursively()
        }
    }
    @Test fun rawFinitePipeSnapshotWaitsForProducerCompletion()=rawSnapshotMustWaitForProducer("delayed-error.txt")
    @Test fun rawEmptyPipeSnapshotDoesNotTreatZeroBytesAsProducerSuccess()=rawSnapshotMustWaitForProducer("empty-error.txt")
    @Test fun rawFinitePipeStillProducesTheExactSuccessfulBytes() {
        grant()
        val context=instrumentation.targetContext
        val location=main { model().location };val source=LocalFileBrowser(File(context.filesDir,"home"),resolver)
        val entry=runBlocking { source.list(location) }.single { it.name=="finite-pipe.txt" }
        val directory=File(context.cacheDir,"saf-preview-finite-${UUID.randomUUID()}");val store=TransferStagingStore(directory,1024,4096)
        try { runBlocking {
            source.snapshot(location,entry,store).use { snapshot ->
                assertEquals(4L,snapshot.size)
                snapshot.withDescriptor { fd,size ->
                    val borrowed=ParcelFileDescriptor.fromFd(fd)
                    ParcelFileDescriptor.AutoCloseInputStream(borrowed).use { assertArrayEquals("pipe".toByteArray(),it.readBytes()) }
                }
            }
        } } finally { runBlocking { store.closeAndAwait() };directory.deleteRecursively() }
    }

    private fun rawSnapshotMustReject(name:String) {
        grant()
        val context=instrumentation.targetContext
        val location=main { model().location };val source=LocalFileBrowser(File(context.filesDir,"home"),resolver)
        val entry=runBlocking { source.list(location) }.single { it.name==name }
        val directory=File(context.cacheDir,"saf-preview-reject-${UUID.randomUUID()}");val store=TransferStagingStore(directory,1024,4096)
        try {
            var failure:Exception?=null
            runBlocking { try { source.snapshot(location,entry,store).close() } catch(error:Exception) { failure=error } }
            assertNotNull("invalid/incomplete source must not publish a raw snapshot",failure)
        } finally {
            runBlocking { store.closeAndAwait() };assertEquals(0L,store.reservedBytes);directory.deleteRecursively()
        }
    }
    @Test fun rawFinitePipeRejectsBytesBeyondTheDeclaredLength()=rawSnapshotMustReject("finite-extra.txt")
    @Test fun rawSnapshotRejectsInvalidAssetRanges()=rawSnapshotMustReject("invalid-range.txt")

}
