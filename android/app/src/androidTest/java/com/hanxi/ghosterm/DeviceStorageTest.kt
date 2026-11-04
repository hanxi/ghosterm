package com.hanxi.ghosterm

import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.createAndroidComposeRule
import androidx.lifecycle.ViewModelProvider
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.hanxi.ghosterm.data.*
import com.hanxi.ghosterm.domain.*
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeout
import org.junit.*
import org.junit.Assert.*
import org.junit.runner.RunWith
import java.io.File
import java.io.FileInputStream
import java.util.UUID

/** Real shared storage, OS permission checks, production page and copy executor. */
@RunWith(AndroidJUnit4::class)
class DeviceStorageTest {
    @get:Rule val ui=createAndroidComposeRule<MainActivity>()
    private val instrumentation=InstrumentationRegistry.getInstrumentation()
    private val context get()=instrumentation.targetContext
    private val name="ghosterm-storage-${UUID.randomUUID()}"
    private val relative="Download/$name"
    private lateinit var privateRoot: File
    private fun model()=ViewModelProvider(ui.activity)[FilesViewModel::class.java]
    private fun shell(command:String):String = instrumentation.uiAutomation.executeShellCommand(command).use {
        FileInputStream(it.fileDescriptor).bufferedReader().readText()
    }
    private val expected get()=shell("cat /system/etc/hosts")
    @Before fun setup() {
        privateRoot=File(context.cacheDir,name).apply { mkdirs() }
        shell("mkdir -p /sdcard/$relative/empty")
        shell("cp /system/etc/hosts /sdcard/$relative/notes.txt")
        shell("cp /system/etc/hosts /sdcard/$relative/.hidden")
        ui.runOnIdle { model().enterLocal() }
        loaded()
    }
    @After fun cleanup() {
        ui.runOnIdle { model().requestClipboardClear();model().confirmClipboardClear();model().leave() }
        shell("rm -rf /sdcard/$relative")
        privateRoot.deleteRecursively()
    }
    private fun loaded() { ui.waitUntil(10000) { ui.runOnIdle { !model().loading } } }
    private fun source()=LocalFileBrowser(File(privateRoot,"home"),context.contentResolver)
    private fun location()=FileLocation(FileEndpoint.DEVICE,relative)
    private fun rejects(block:suspend ()->Unit) {
        var failure:Exception?=null
        try { runBlocking { block() } } catch(error:Exception) { failure=error }
        assertNotNull("must report an error",failure)
    }
    private fun grantedOrCheckDenial():Boolean {
        if(DeviceStorageAccess.granted(context)) return true
        rejects { source().list(location()) }
        return false
    }
    @Test fun permissionEntryFollowsActualGrant() {
        if(!DeviceStorageAccess.granted(context)) {
            ui.onNodeWithTag("file-device-storage").performClick()
            ui.waitUntil(10000) { instrumentation.uiAutomation.rootInActiveWindow?.packageName in setOf("com.android.settings","com.google.android.permissioncontroller","com.android.permissioncontroller") }
            fun deny(node:android.view.accessibility.AccessibilityNodeInfo?):Boolean {
                if(node==null) return false
                if(node.text?.toString() in setOf("DENY","Deny","拒绝","Don't allow","不允许")) {
                    var target:android.view.accessibility.AccessibilityNodeInfo?=node
                    while(target!=null) {
                        if(target.isClickable && target.performAction(android.view.accessibility.AccessibilityNodeInfo.ACTION_CLICK)) return true
                        target=target.parent
                    }
                }
                for(i in 0 until node.childCount) if(deny(node.getChild(i))) return true
                return false
            }
            val denied=deny(instrumentation.uiAutomation.rootInActiveWindow)
            if(!denied) repeat(3) {
                if(instrumentation.uiAutomation.rootInActiveWindow?.packageName!=context.packageName) {
                    shell("input keyevent 4");android.os.SystemClock.sleep(300)
                }
            }
            ui.waitUntil(10000) { ui.runOnIdle { !model().deviceAccessPending } }
            ui.onNodeWithText("设备存储访问未授权，仍可使用「选择设备文件夹」").performScrollTo().assertIsDisplayed()
            rejects { source().list(location()) }
            val entry=BrowserEntry("$relative/notes.txt","notes.txt",FileKind.FILE,21u,null)
            rejects { source().preview(location(),entry) }
            TransferStagingStore(File(privateRoot,"denied")).use { store -> rejects { source().snapshot(location(),entry,store) } }
        } else {
            ui.onNodeWithTag("file-device-storage").performClick();loaded()
            assertEquals(FileEndpoint.DEVICE,ui.runOnIdle { model().location.endpoint })
        }
    }
    @Test fun devicePageBrowsesDownloadsPreviewsAndCopiesWithoutGrantingTargetWrites() {
        if(!grantedOrCheckDenial()) return
        ui.onNodeWithTag("file-device-storage").performClick();loaded()
        assertEquals(FileEndpoint.DEVICE,ui.runOnIdle { model().location.endpoint })
        assertNull("Device directory read failed",ui.runOnIdle { model().error })
        ui.onNodeWithTag("file-list").performScrollToNode(hasTestTag("file-entry:Download"))
        ui.onNodeWithTag("file-entry:Download").performClick();loaded()
        ui.onNodeWithTag("file-list").performScrollToNode(hasTestTag("file-entry:$name"))
        ui.onNodeWithTag("file-entry:$name").performClick();loaded()
        assertEquals(relative,ui.runOnIdle { model().location.directoryId })
        ui.onNodeWithTag("file-list").performScrollToNode(hasTestTag("file-entry:notes.txt"))
        ui.onNodeWithTag("file-entry:notes.txt").performClick()
        ui.waitUntil(10000) { ui.runOnIdle { model().preview?.text!=null } }
        ui.onNodeWithTag("file-preview").assertTextEquals(expected)
        ui.runOnIdle { model().closePreview() }
        ui.onNodeWithTag("file-list").performScrollToNode(hasTestTag("file-entry:notes.txt"))
        ui.onNodeWithTag("file-entry:notes.txt").performTouchInput { longClick() }
        ui.onNodeWithTag("file-copy").performClick()
        assertEquals(FileEndpoint.DEVICE,ui.runOnIdle { model().clipboard!!.source.endpoint })
        ui.onNodeWithTag("file-paste").assertIsNotEnabled()
        ui.onNodeWithTag("file-tab-remote").performClick()
        ui.onNodeWithTag("file-tab-local").performClick();loaded()
        assertEquals(relative,ui.runOnIdle { model().location.directoryId })
        ui.onNodeWithTag("file-list").performScrollToNode(hasTestTag("file-entry:notes.txt"))
        ui.onNodeWithTag("file-entry:notes.txt").assertIsDisplayed()
    }
    @Test fun directReadsRequirePermissionAndRejectTraversalAndForgedEndpoints() {
        if(!grantedOrCheckDenial()) return
        val local=source()
        val listed=runCatching { runBlocking { local.list(location()) } }
        assertTrue("Shared storage must be browsable: ${listed.exceptionOrNull()}",listed.isSuccess)
        val entries=listed.getOrThrow()
        assertTrue(entries.any { it.name==".hidden" })
        val note=entries.single { it.name=="notes.txt" }
        assertEquals(expected,runBlocking { local.preview(location(),note) })
        rejects { local.list(FileLocation(FileEndpoint.DEVICE,"../")) }
        rejects { local.preview(location(),note.copy(id="../notes.txt")) }
        rejects { local.list(location().copy(endpoint=FileEndpoint.DEVICE.copy(rootId="/"))) }
    }
    @Test fun forgedDeviceIdsAreRejectedBeforeCopy()=runBlocking {
        val entry=BrowserEntry("../notes.txt","notes.txt",FileKind.FILE,1u,null)
        val request=FilePasteRequest.capture(FileClipboardSnapshot.capture(location(),"Downloads",listOf(entry)),
            FileLocation(FileEndpoint.APP),"home",FileConflictPolicy.ASK)
        val scan=FileTreeScanner { BrowserDirectory(it,listOf(entry)) }.scan(request)
        assertFalse("A device entry must stay inside its source directory",scan.complete)
        assertEquals(FileScanFailureKind.INVALID_ENTRY,scan.failure?.kind)
    }
    @Test fun deviceDirectoryCopiesIntoPrivateHomeThroughRealExecutor()=runBlocking {
        if(!grantedOrCheckDenial()) return@runBlocking
        val home=File(privateRoot,"home").apply { mkdirs() }
        val local=source()
        val source=FileLocation(FileEndpoint.DEVICE,"Download")
        val entry=local.list(source).single { it.name==name }
        val destination=FileLocation(FileEndpoint.APP)
        val request=FilePasteRequest.capture(FileClipboardSnapshot.capture(source,"Downloads",listOf(entry)),destination,"home",FileConflictPolicy.ASK)
        val scan=FileTreeScanner(local::browse).scan(request)
        assertTrue(scan.failure.toString(),scan.complete)
        val access=FileDestinationAccess(home,context.contentResolver,local::browse)
        val check=FilePastePreflight(access::inspect,access::relation).check(scan)
        assertTrue(check.failure.toString(),check.checked)
        val decisions=FilePasteDecisions.begin(check)
        val plan=decisions.confirm(decisions.state().reference)
        val executor=FileCopyExecutor(home,context.contentResolver,File(privateRoot,"tasks"),{
            FileTaskConnections(it.source.endpoint,it.destination.endpoint,{ _,_->error("unexpected remote") },{ _,_,_->error("unexpected authentication") })
        })
        val operation=executor.prepare(plan)
        try {
            val outcome=withTimeout(15000) { operation.execute() }
            assertEquals(outcome.error,FileCopyState.COMPLETE,outcome.state)
            assertEquals(expected,File(home,"$name/notes.txt").readText())
            assertEquals(expected,File(home,"$name/.hidden").readText())
            assertTrue(File(home,"$name/empty").isDirectory)
        } finally { operation.cancel();operation.awaitClosed() }
        rejects { access.inspect(location()) }
    }
}
