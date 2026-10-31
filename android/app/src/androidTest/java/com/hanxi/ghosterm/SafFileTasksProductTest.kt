package com.hanxi.ghosterm

import android.content.Intent
import android.net.Uri
import android.os.Bundle
import android.os.SystemClock
import android.view.accessibility.AccessibilityNodeInfo
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.createAndroidComposeRule
import androidx.lifecycle.ViewModelProvider
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.hanxi.ghosterm.data.FileCopyExecutor
import com.hanxi.ghosterm.data.FileTaskConnections
import com.hanxi.ghosterm.data.LocalFileBrowser
import com.hanxi.ghosterm.data.FileDestinationAccess
import com.hanxi.ghosterm.data.FilePastePreflight
import com.hanxi.ghosterm.data.FileTreeScanner
import com.hanxi.ghosterm.domain.*
import kotlinx.coroutines.*
import org.junit.*
import org.junit.Assert.*
import org.junit.runner.RunWith
import java.io.File
import java.util.UUID

/** Real DocumentsUI grants, separate provider process, observed bytes and cancellation leases. */
@RunWith(AndroidJUnit4::class)
class SafFileTasksProductTest {
    @get:Rule val ui=createAndroidComposeRule<MainActivity>()
    private val instrumentation=InstrumentationRegistry.getInstrumentation()
    private val context get()=instrumentation.targetContext
    private val resolver get()=context.contentResolver
    private val authority="com.hanxi.ghosterm.test.publication"
    private lateinit var root:File
    private lateinit var home:File
    private fun tasks()=ViewModelProvider(ui.activity)[FileTasksViewModel::class.java]
    private val latest get()=tasks().tasks.first()
    private val unique="saf-task-${UUID.randomUUID()}"
    private lateinit var location:FileLocation
    private fun model()=ViewModelProvider(ui.activity)[FilesViewModel::class.java]
    private fun <T> main(block:()->T):T=ui.runOnIdle(block)
    private fun state(action:String="state",mode:String="normal"):Bundle=checkNotNull(resolver.call(
        Uri.parse("content://com.hanxi.ghosterm.test.publicationcontrol"),action,null,Bundle().apply { putString("mode",mode) }))
    private fun reset(mode:String) { state("reset",mode) }
    private fun revoke() { resolver.persistedUriPermissions.filter { it.uri.authority==authority }.forEach {
        resolver.releasePersistableUriPermission(it.uri,(if(it.isReadPermission) Intent.FLAG_GRANT_READ_URI_PERMISSION else 0) or
            (if(it.isWritePermission) Intent.FLAG_GRANT_WRITE_URI_PERMISSION else 0))
    } }
    @Before fun setup() {
        home=File(context.filesDir,"home")
        root=File(home,unique).apply { mkdirs() };File(root,"source").mkdir();File(root,"target").mkdir()
        File(root,"source/new.txt").writeText("通过 SAF 页面复制 ☃")
        reset("normal");revoke()
        ui.onNodeWithText("本机文件").performClick();await { !model().loading }
        ui.onNodeWithTag("file-location").performClick();ui.onNodeWithText("选择设备文件夹").performClick()
        await { nodes().any { it.packageName?.toString()?.contains("documentsui")==true } }
        if(!nodes().any { it.text?.toString()=="keep.txt" }) {
            if(!click("Ghosterm Copy Destination")) {
                assertTrue(click("Show roots","Open navigation drawer","显示根目录","显示位置"))
                await { click("Ghosterm Copy Destination") }
            }
        }
        await { click("Use this folder","USE THIS FOLDER","使用此文件夹") }
        await { click("Allow","ALLOW","允许") || model().location.endpoint.treeUri!=null }
        await { !model().loading && model().location.endpoint.treeUri!=null && !model().choosingLocation }
        assertNull(main { model().error });location=main { model().location }
        assertEquals(authority,Uri.parse(location.endpoint.treeUri).authority)
        assertTrue(resolver.persistedUriPermissions.any { it.uri.toString()==location.endpoint.treeUri && it.isReadPermission && it.isWritePermission })
    }
    @After fun cleanup() {
        state("release")
        main { tasks().tasks.toList().forEach(tasks()::cancel) };await { !tasks().busy }
        main { model().leave() };revoke();root.deleteRecursively()
    }
    private fun nodes():List<AccessibilityNodeInfo> {
        val list=mutableListOf<AccessibilityNodeInfo>()
        fun visit(node:AccessibilityNodeInfo?) { if(node==null)return;list+=node;for(i in 0 until node.childCount)visit(node.getChild(i)) }
        visit(instrumentation.uiAutomation.rootInActiveWindow);return list
    }
    private fun click(vararg names:String):Boolean {
        for(found in nodes().filter { it.isVisibleToUser && (it.text?.toString() in names || it.contentDescription?.toString() in names) }) {
            var node:AccessibilityNodeInfo?=found
            while(node!=null) { if(node.isClickable && node.performAction(AccessibilityNodeInfo.ACTION_CLICK))return true;node=node.parent }
        };return false
    }
    private fun await(check:()->Boolean) {
        val end=SystemClock.elapsedRealtime()+10000
        while(SystemClock.elapsedRealtime()<end) { if(main(check))return;SystemClock.sleep(30) }
        fail("Timed out: "+nodes().take(30).joinToString { "${it.text}/${it.contentDescription}" })
    }
    private suspend fun until(check:()->Boolean) {
        assertTrue("expected provider/worker progress",withTimeoutOrNull(6000) { while(!check())delay(10);true } == true)
    }
    private fun connections(request:FilePasteRequest)=FileTaskConnections(request.source.endpoint,request.destination.endpoint,
        { _,_->error("SAF does not resolve SSH") },{ _,_,_->error("SAF does not authenticate") })
    private fun start(source:FileLocation,target:FileLocation) {
        val browser=LocalFileBrowser(home,resolver)
        val entries=runBlocking { browser.browse(source).entries }.filter { it.kind==FileKind.FILE }
        val snapshot=FileClipboardSnapshot.capture(source,source.directoryId,entries)
        main { tasks().start(snapshot,target,target.directoryId,::connections) { model().copyCompleted(it) } }
    }
    @Test fun realGrantedFolderCopyRetainsActualDocumentAndReadsBackToHome() {
        reset("rename")
        start(FileLocation(FileEndpoint.APP,"$unique/source"),location)
        await { latest.phase!=FileTaskPhase.SCANNING };assertEquals(FileTaskPhase.CONFIRMING,latest.phase)
        ui.onNodeWithTag("file-task-confirm").performScrollTo().performClick();await { !tasks().busy }
        val result=latest.result!!;assertEquals(FileCopyState.COMPLETE,result.state)
        val item=result.items.single();assertNotNull(item.documentUri)
        assertEquals("通过 SAF 页面复制 ☃",state().getByteArray("bytes")!!.toString(Charsets.UTF_8))
        assertNotEquals("new.txt",item.targetName)
        ui.onNodeWithTag("file-task-details").performScrollTo().performClick()
        ui.onNodeWithText("实际位置：${item.documentUri}").performScrollTo().assertIsDisplayed()
        ui.onNodeWithText("关闭结果").performClick()
        start(location,FileLocation(FileEndpoint.APP,"$unique/target"))
        await { latest.phase!=FileTaskPhase.SCANNING };assertEquals(FileTaskPhase.CONFIRMING,latest.phase)
        ui.onAllNodesWithTag("file-task-confirm").onFirst().performScrollTo().performClick();await { !tasks().busy }
        assertEquals(FileCopyState.COMPLETE,latest.result?.state)
        assertEquals("通过 SAF 页面复制 ☃",File(root,"target/${item.targetName}").readText())
        assertEquals("original",File(root,"target/keep.txt").readText())
    }
    @Test fun lostCreationReplyStaysUnknownAndShowsTheUnconfirmedResult() {
        reset("create-lost")
        start(FileLocation(FileEndpoint.APP,"$unique/source"),location)
        await { latest.phase!=FileTaskPhase.SCANNING };assertEquals(FileTaskPhase.CONFIRMING,latest.phase)
        ui.onNodeWithTag("file-task-confirm").performScrollTo().performClick();await { !tasks().busy }
        assertEquals(FileCopyState.UNKNOWN,latest.result?.state)
        ui.onNodeWithTag("file-task-status").assertTextEquals("复制结果未知，请核对实际位置")
        assertEquals(1,state().getInt("creates"));assertEquals(0,state().getInt("deletes"))
        ui.onNodeWithTag("file-task-details").performScrollTo().performClick()
        ui.onNodeWithText("new.txt · 结果未知").assertIsDisplayed()
    }
    @Test fun cancellingSourceListingWaitsForActualProviderReturn() {
        val browser=LocalFileBrowser(home,resolver)
        val snapshot=FileClipboardSnapshot.capture(location,"SAF",runBlocking { browser.browse(location).entries }.filter { it.kind==FileKind.FILE })
        reset("list-stall")
        try {
            main { tasks().start(snapshot,FileLocation(FileEndpoint.APP,"$unique/target"),"target",::connections,{}) }
            await { state().getInt("active")==1 }
            ui.onNodeWithTag("file-task-cancel").performScrollTo().performClick()
            SystemClock.sleep(200)
            assertTrue("source provider still owns its query",tasks().busy)
            assertEquals(FileTaskPhase.CANCELLING,latest.phase)
            assertEquals(1,state().getInt("active"))
            state("release");await { !tasks().busy }
            assertEquals(0,state().getInt("active"));assertTrue(latest.cancelled)
            assertFalse(File(root,"target/keep.txt").exists());assertEquals(0,state().getInt("writes"))
        } finally { state("release") }
    }
    @Test fun cancellingDestinationMetadataWaitsForActualProviderReturn() {
        reset("metadata-stall")
        try {
            start(FileLocation(FileEndpoint.APP,"$unique/source"),location)
            await { state().getInt("active")==1 }
            ui.onNodeWithTag("file-task-cancel").performScrollTo().performClick()
            SystemClock.sleep(200)
            assertTrue("target provider still owns its query",tasks().busy)
            assertEquals(FileTaskPhase.CANCELLING,latest.phase)
            state("release");await { !tasks().busy }
            assertEquals(0,state().getInt("active"));assertTrue(latest.cancelled)
            assertEquals(0,state().getInt("creates"));assertEquals(0,state().getInt("writes"))
        } finally { state("release") }
    }
}
