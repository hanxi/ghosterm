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
class SafCopyExecutorTest {
    @get:Rule val ui=createAndroidComposeRule<MainActivity>()
    private val instrumentation=InstrumentationRegistry.getInstrumentation()
    private val context get()=instrumentation.targetContext
    private val resolver get()=context.contentResolver
    private val authority="com.hanxi.ghosterm.test.publication"
    private lateinit var root:File
    private lateinit var home:File
    private val copies=mutableListOf<FileCopyExecutor.Request>()
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
        root=File(context.cacheDir,"saf-publication-${UUID.randomUUID()}").apply { mkdir() }
        home=File(root,"home").apply { mkdir() };File(home,"source").mkdir();File(home,"target").mkdir()
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
        runBlocking { for(copy in copies) { copy.cancel();withTimeout(15000) { copy.awaitClosed() } } }
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
    private suspend fun until(message:String="expected provider/worker progress",check:()->Boolean) {
        assertTrue(message,withTimeoutOrNull(6000) { while(!check())delay(10);true } == true)
    }
    private val origin get()=FileLocation(FileEndpoint.APP,"source")
    private suspend fun plan(source:FileLocation=origin,destination:FileLocation=location,names:List<String>?=null):FileCopyPlan {
        val browser=LocalFileBrowser(home,resolver)
        val items=browser.list(source).filter { names==null || it.name in names }
        val paste=FilePasteRequest.capture(FileClipboardSnapshot.capture(source,"source",items),destination,"target")
        val scan=FileTreeScanner(browser::browse).scan(paste);assertTrue(scan.failure.toString(),scan.complete)
        val access=FileDestinationAccess(home,resolver,browser::browse)
        val checked=FilePastePreflight(access::inspect,access::relation).check(scan);assertTrue(checked.failure.toString(),checked.checked)
        val decisions=FilePasteDecisions.begin(checked)
        return decisions.confirm(decisions.state().reference)
    }
    private fun copy(plan:FileCopyPlan)=FileCopyExecutor(home,resolver,File(root,"tasks"),{
        FileTaskConnections(it.source.endpoint,it.destination.endpoint,{ _,_->error("unexpected remote") },{ _,_,_->error("unexpected auth") })
    },262144).prepare(plan).also { copies+=it }
    @Test fun nestedSafTargetsUseActualNamesAndOpaqueParentIds()=runBlocking<Unit> {
        reset("rename")
        File(home,"source/folder/empty").mkdirs();File(home,"source/folder/hello").writeText("文档复制 ☃")
        val request=copy(plan());val out=request.execute();request.awaitClosed()
        assertEquals(FileCopyState.COMPLETE,out.state);assertTrue(out.mayClearClipboard)
        val folder=out.items.single { it.source.relativePath=="folder" }
        assertEquals("provider-folder",folder.targetName);assertTrue(folder.createdId!!.startsWith("opaque:new:"))
        val file=out.items.single { it.source.entry.name=="hello" }
        assertEquals(folder.createdId,file.targetParent!!.directoryId);assertEquals("provider-hello",file.targetName)
        assertEquals("文档复制 ☃",resolver.openInputStream(Uri.parse(file.documentUri))!!.bufferedReader().use { it.readText() })
        val empty=out.items.single { it.source.entry.name=="empty" }
        assertTrue(LocalFileBrowser(home,resolver).list(FileLocation(location.endpoint,empty.createdId!!)).isEmpty())
        assertArrayEquals("original".toByteArray(),state().getByteArray("keep"));assertEquals(0,state().getInt("deletes"))
    }
    @Test fun safSourcePublishesIntoPrivateHomeWithoutWritingProvider()=runBlocking<Unit> {
        val request=copy(plan(location,FileLocation(FileEndpoint.APP,"target"),listOf("keep.txt")))
        val out=request.execute();request.awaitClosed()
        assertEquals(FileCopyState.COMPLETE,out.state);assertEquals("original",File(home,"target/keep.txt").readText())
        assertEquals(8uL,out.committedBytes);assertEquals(0,state().getInt("writes"));assertEquals(0,state().getInt("creates"))
    }
    @Test fun sourceReadProgressStaysZeroUntilProviderReturnsBytes()=runBlocking<Unit> {
        reset("source-read-stall")
        val request=copy(plan(location,FileLocation(FileEndpoint.APP,"target"),listOf("keep.txt")))
        val waiting=async { request.execute() }
        try {
            until { state().getInt("reads")>0 }
            val before=request.outcome
            assertEquals("source phase while provider is blocked",FileCopyStage.READING,before.activity?.stage)
            assertEquals("blocked provider has returned no bytes",0uL,before.activity?.bytes)
            assertEquals("reading is not destination commit",0uL,before.committedBytes)
        } finally { state("release") }
        val out=withTimeout(10000) { waiting.await() };request.awaitClosed()
        assertEquals(FileCopyState.COMPLETE,out.state);assertEquals("original",File(home,"target/keep.txt").readText())
    }
    @Test fun partialWriteProgressIsVisibleWhileProviderIsStalled()=runBlocking<Unit> {
        reset("pipe-stall")
        val file=File(home,"source/file").apply { writeBytes(ByteArray(100003) { (it%251).toByte() }) }
        val request=copy(plan());val waiting=async { request.execute() }
        try {
            until("partial write bytes become visible while stalled") { request.outcome.activity?.let { it.stage==FileCopyStage.WRITING && it.bytes>0uL }==true }
            val progress=request.outcome
            assertTrue("partial writes are real and smaller than total",progress.activity!!.bytes<file.length().toULong())
            assertEquals("partial writes are not committed",0uL,progress.committedBytes)
            assertFalse("blocked write is still running",request.finished)
            request.cancel()
        } finally { state("release") }
        withTimeout(10000) { waiting.await();request.awaitClosed() }
        assertEquals(FileCopyState.CANCELLED,request.outcome.state)
    }
    @Test fun partialSourceProgressSurvivesTheCoalescingWindow()=runBlocking<Unit> {
        reset("source-pipe-stall")
        val request=copy(plan(location,FileLocation(FileEndpoint.APP,"target"),listOf("keep.txt")))
        val waiting=async { request.execute() }
        try {
            until("source bytes become visible during stall") { request.outcome.activity?.bytes==4uL }
            val progress=request.outcome
            assertEquals("partial source phase",FileCopyStage.READING,progress.activity?.stage)
            assertEquals("partial source total",8uL,progress.activity?.totalBytes)
            assertEquals("partial source bytes are not committed",0uL,progress.committedBytes)
        } finally { state("release") }
        val out=withTimeout(10000) { waiting.await() };request.awaitClosed()
        assertEquals(FileCopyState.COMPLETE,out.state);assertEquals("original",File(home,"target/keep.txt").readText())
    }
    @Test fun cancellationWaitsForLateCreatedDirectoryAndKeepsItsIdentity()=runBlocking<Unit> {
        reset("late-create");File(home,"source/folder/child").mkdirs()
        val request=copy(plan());val waiting=async { request.execute() }
        until { state().getInt("creates")==1 && state().getString("latest")!=null }
        request.cancel();delay(100);assertFalse(request.finished);assertFalse(waiting.isCompleted)
        state("release");val out=withTimeout(10000) { waiting.await();request.awaitClosed();request.outcome }
        assertEquals(FileCopyState.CANCELLED,out.state)
        val created=out.items.single { it.source.relativePath=="folder" }
        assertEquals(FileCopyItemState.CREATED,created.state);assertEquals(state().getString("latest"),created.createdId)
        assertNotNull(created.documentUri);assertEquals(1,state().getInt("creates"));assertEquals(0,state().getInt("deletes"))
        assertEquals(FileCopyItemState.NOT_ATTEMPTED,out.items.single { it.source.entry.name=="child" }.state)
    }
    @Test fun lostCreationReplyIsUnknownAndNeverRetriedOrDeleted()=runBlocking<Unit> {
        reset("create-lost");File(home,"source/a").writeText("first");File(home,"source/b").writeText("later")
        val request=copy(plan());val out=request.execute();request.awaitClosed()
        assertEquals(FileCopyState.UNKNOWN,out.state);assertEquals(FileCopyItemState.UNKNOWN,out.items.first().state)
        assertEquals(FileCopyItemState.NOT_ATTEMPTED,out.items.last().state)
        assertEquals(1,state().getInt("creates"));assertEquals(0,state().getInt("deletes"));assertEquals(0,state().getInt("writes"))
        assertNotNull(state().getString("latest"));assertFalse(out.mayClearClipboard)
    }
    @Test fun cancelledCallerCannotDiscardReadbackSideEffectsOrReleaseEarly()=runBlocking<Unit> {
        reset("read-stall");File(home,"source/file").writeText("written before cancel")
        val request=copy(plan());val waiting=launch { request.execute() }
        until { state().getInt("reads")>0 }
        assertEquals("SAF write awaits verification",FileCopyStage.VERIFYING,request.outcome.activity?.stage)
        assertEquals("SAF observed written bytes",21uL,request.outcome.activity?.bytes)
        assertEquals("SAF readback has not committed bytes",0uL,request.outcome.committedBytes)
        waiting.cancelAndJoin();delay(100)
        assertFalse("provider still holds the readback request",request.finished)
        state("release");withTimeout(10000) { request.awaitClosed() }
        val out=request.outcome;assertEquals(FileCopyState.CANCELLED,out.state)
        assertEquals(FileCopyItemState.CREATED,out.items.single().state);assertNotNull(out.items.single().documentUri)
        assertEquals(0uL,out.committedBytes)
        assertEquals(1,state().getInt("creates"));assertEquals(0,state().getInt("deletes"));assertFalse(out.mayClearClipboard)
    }
    @Test fun changedSourceAfterPublicationKeepsResultButCannotClearClipboard()=runBlocking<Unit> {
        reset("read-stall");val source=File(home,"source/file").apply { writeText("snapshot") }
        val request=copy(plan());val waiting=async { request.execute() }
        until { state().getInt("reads")>0 };source.writeText("changed after snapshot");state("release")
        val out=withTimeout(10000) { waiting.await() };request.awaitClosed()
        assertEquals(FileCopyState.FAILED,out.state);assertNotNull(out.error);assertFalse(out.mayClearClipboard)
        assertEquals(FileCopyItemState.COMPLETE,out.items.single().state);assertEquals(8uL,out.committedBytes)
        assertArrayEquals("snapshot".toByteArray(),state().getByteArray("bytes"))
    }
    @Test fun changedFollowingSourceIsRejectedBeforeItsDestinationIsCreated()=runBlocking<Unit> {
        reset("read-stall");File(home,"source/a").writeText("first")
        val second=File(home,"source/b").apply { writeText("later") }
        val request=copy(plan());val waiting=async { request.execute() }
        until { state().getInt("reads")>0 }
        second.writeText("other");assertTrue(second.setLastModified(second.lastModified()+2000));state("release")
        val out=withTimeout(10000) { waiting.await() };request.awaitClosed()
        assertEquals(FileCopyState.FAILED,out.state);assertEquals(1,state().getInt("creates"))
        assertEquals(FileCopyItemState.COMPLETE,out.items.first().state)
        assertEquals(FileCopyItemState.FAILED,out.items.last().state);assertFalse(out.mayClearClipboard)
        assertArrayEquals("first".toByteArray(),state().getByteArray("bytes"))
    }
    @Test fun revokedTargetBeforeExecutionDoesNotCreateAnything()=runBlocking<Unit> {
        File(home,"source/file").writeText("new");val request=copy(plan());revoke()
        val out=request.execute();request.awaitClosed();assertEquals(FileCopyState.FAILED,out.state)
        assertEquals(0,state().getInt("creates"));assertEquals(0,state().getInt("writes"));assertFalse(out.mayClearClipboard)
    }
}
