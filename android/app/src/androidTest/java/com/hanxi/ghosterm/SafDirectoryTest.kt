package com.hanxi.ghosterm

import android.content.Intent
import android.content.res.AssetFileDescriptor
import android.net.Uri
import android.os.Bundle
import android.os.ParcelFileDescriptor
import android.os.SystemClock
import android.provider.DocumentsContract
import android.system.Os
import android.system.OsConstants
import android.view.accessibility.AccessibilityNodeInfo
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.createAndroidComposeRule
import androidx.lifecycle.ViewModelProvider
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.hanxi.ghosterm.data.SafFilePublisher
import com.hanxi.ghosterm.data.FileDirectoryCreator.State
import com.hanxi.ghosterm.data.FileDirectoryCreator
import com.hanxi.ghosterm.data.TransferStagingStore
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
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.TimeUnit
import kotlin.coroutines.CoroutineContext

/** Real DocumentsUI grants, separate provider process, observed bytes and cancellation leases. */
@RunWith(AndroidJUnit4::class)
class SafDirectoryTest {
    @get:Rule val ui=createAndroidComposeRule<MainActivity>()
    private val instrumentation=InstrumentationRegistry.getInstrumentation()
    private val context get()=instrumentation.targetContext
    private val resolver get()=context.contentResolver
    private val authority="com.hanxi.ghosterm.test.publication"
    private lateinit var root:File
    private lateinit var store:TransferStagingStore
    private lateinit var creator:FileDirectoryCreator
    private lateinit var location:FileLocation
    private val requests=mutableListOf<FileDirectoryCreator.Request>()
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
        store=TransferStagingStore(File(root,"snapshots"),262144,524288,0);creator=FileDirectoryCreator(root,resolver)
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
        runBlocking { for(request in requests) { request.cancel();withTimeout(15000) { request.awaitClosed() } } }
        main { model().leave() };revoke();store.close();root.deleteRecursively()
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
    private suspend fun snapshot(bytes:ByteArray):TransferStagingStore.Snapshot {
        val file=File(root,"source").apply { writeBytes(bytes) }
        return store.snapshot(bytes.size.toLong()) { AssetFileDescriptor(ParcelFileDescriptor.open(file,ParcelFileDescriptor.MODE_READ_ONLY),0,bytes.size.toLong()) }
    }

    private fun request(name:String="new folder",parent:FileLocation=location)=creator.prepare(parent,name).also { requests+=it }
    private fun unchanged() {
        assertArrayEquals("original".toByteArray(),state().getByteArray("keep"))
        assertEquals(0,state().getInt("writes"));assertEquals(0,state().getInt("reads"));assertEquals(0,state().getInt("deletes"))
    }
    @Test fun createsOpaqueNestedAndEmptyDirectoriesWithActualNames() = runBlocking {
        reset("rename")
        val first=request("目录 😀");val result=first.execute();first.awaitClosed()
        assertEquals(State.COMPLETE,result.state);assertEquals("provider-目录 😀",result.actualName)
        assertTrue(result.location!!.directoryId.startsWith("opaque:new:"))
        assertEquals(result.location!!.directoryId,DocumentsContract.getDocumentId(Uri.parse(result.documentUri)))
        val second=request("empty",result.location!!).execute()
        assertEquals(State.COMPLETE,second.state);assertEquals("provider-empty",second.actualName)
        val browser=LocalFileBrowser(root,resolver)
        assertTrue(browser.list(second.location!!).isEmpty())
        assertEquals(second.location!!.directoryId,browser.list(result.location!!).single().id)
        assertEquals(2,state().getInt("creates"));unchanged()
    }
    @Test fun existingNamesAndInvalidTargetsNeverCreate() = runBlocking {
        for(name in listOf("keep.txt","Nested"))assertEquals(State.CONFLICT,request(name).execute().state)
        for(parent in listOf(location.copy(directoryId="outside"),location.copy(directoryId="opaque:keep"),
            location.copy(endpoint=location.endpoint.copy(rootId="wrong")))) {
            val result=request(parent=parent).execute();assertEquals(State.NOT_ATTEMPTED,result.state);assertNotNull(result.error)
        }
        assertEquals(0,state().getInt("creates"));unchanged()
    }
    @Test fun deniedRevokedAndUnreadableTargetsNeverCreate() = runBlocking {
        for(mode in listOf("no-create","list-fail","duplicate","too-many","directory-metadata")) {
            reset(mode);val result=request().execute()
            assertEquals(mode,State.NOT_ATTEMPTED,result.state);assertNotNull(result.error);assertEquals(0,state().getInt("creates"))
        }
        reset("normal")
        resolver.releasePersistableUriPermission(Uri.parse(location.endpoint.treeUri),Intent.FLAG_GRANT_WRITE_URI_PERMISSION)
        assertEquals(State.NOT_ATTEMPTED,request().execute().state);assertEquals(0,state().getInt("creates"))
        revoke();assertEquals(State.NOT_ATTEMPTED,request().execute().state);unchanged()
    }
    @Test fun uncertainCreationNeverRetriesOrClaimsSuccess() = runBlocking {
        for(mode in listOf("return-existing","return-parent","create-denied","create-lost")) {
            reset(mode);val result=request().execute()
            assertEquals(mode,State.UNKNOWN,result.state);assertNotNull(result.error)
            assertNull(result.location);assertEquals(1,state().getInt("creates"));unchanged()
        }
    }
    @Test fun invalidNewDirectoryRetainsIdentityWithoutDeleting() = runBlocking {
        for(mode in listOf("directory-file","directory-outside","directory-nonempty","wrong-metadata","directory-indirect")) {
            reset(mode);val result=request().execute()
            assertEquals(mode,State.CREATED,result.state);assertNotNull(result.documentUri);assertNotNull(result.location)
            assertNotNull(result.error);assertEquals(1,state().getInt("creates"));unchanged()
            if(mode in listOf("directory-file","directory-outside","wrong-metadata"))
                assertNull("must not adopt a name from unverified directory metadata",result.actualName)
            else assertEquals("new folder",result.actualName)
        }
    }
    @Test fun cancellationBeforeStartAndRepeatedExecutionDoNotCreate() = runBlocking {
        val pending=request();pending.cancel();pending.awaitClosed()
        val job=launch { pending.execute();fail("cancelled request completed") };job.join()
        assertTrue(job.isCancelled);assertEquals(State.NOT_ATTEMPTED,pending.outcome.state)
        var rejected=false;try { pending.execute() } catch(_:IllegalStateException) { rejected=true }
        assertTrue(rejected);assertEquals(0,state().getInt("creates"));unchanged()
    }
    @Test fun cancellationWaitsForLateProviderAndRetainsCreatedDirectory() = runBlocking {
        reset("late-create");val request=request()
        val job=launch { request.execute();fail("cancelled directory delivered") }
        until { state().getString("latest")!=null }
        job.cancel();job.join()
        assertFalse(request.finished);assertEquals(State.UNKNOWN,request.outcome.state)
        state("release");withTimeout(5000) { request.awaitClosed() }
        assertEquals(State.CREATED,request.outcome.state);assertNotNull(request.outcome.location)
        assertNotNull(request.outcome.documentUri);assertNotNull(request.outcome.error)
        assertEquals(1,state().getInt("creates"));unchanged()
    }
    @Test fun creationResultSurvivesCancellationAfterCompletion() = runBlocking {
        val request=request();assertEquals(State.COMPLETE,request.execute().state)
        request.cancel();request.awaitClosed();assertEquals(State.COMPLETE,request.outcome.state)
        var rejected=false;try { request.execute() } catch(_:IllegalStateException) { rejected=true }
        assertTrue(rejected);assertEquals(1,state().getInt("creates"));unchanged()
    }
    @Test fun newlyCreatedDirectoryAcceptsRealFilePublicationByOpaqueId() = runBlocking {
        val made=request("parent").execute();assertEquals(State.COMPLETE,made.state)
        val bytes="file inside SAF directory".toByteArray()
        snapshot(bytes).use { frozen ->
            val write=SafFilePublisher(resolver).prepare(made.location!!,"content.txt")
            val result=write.execute(frozen);write.awaitClosed()
            assertEquals(SafFilePublisher.State.COMPLETE,result.state)
            assertArrayEquals(bytes,state().getByteArray("bytes"))
            assertEquals("content.txt",LocalFileBrowser(root,resolver).list(made.location!!).single().name)
        }
        assertEquals(0L,store.reservedBytes);assertEquals(2,state().getInt("creates"))
        assertEquals(0,state().getInt("deletes"));assertArrayEquals("original".toByteArray(),state().getByteArray("keep"))
    }
}
