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
import com.hanxi.ghosterm.data.*
import com.hanxi.ghosterm.data.SafFilePublisher
import com.hanxi.ghosterm.data.SafFilePublisher.State
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
class SafPublicationTest {
    @get:Rule val ui=createAndroidComposeRule<MainActivity>()
    private val instrumentation=InstrumentationRegistry.getInstrumentation()
    private val context get()=instrumentation.targetContext
    private val resolver get()=context.contentResolver
    private val authority="com.hanxi.ghosterm.test.publication"
    private lateinit var root:File
    private lateinit var store:TransferStagingStore
    private lateinit var publisher:SafFilePublisher
    private lateinit var location:FileLocation
    private val requests=mutableListOf<SafFilePublisher.Request>()
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
        store=TransferStagingStore(File(root,"snapshots"),262144,524288,0);publisher=SafFilePublisher(resolver)
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

    @Test fun conflictPlanPublishesLocalBytesUnderNewSafName() = runBlocking<Unit> {
        val bytes="planned SAF bytes".toByteArray();File(root,"keep.txt").writeBytes(bytes)
        val local=LocalFileBrowser(root,resolver);val source=FileLocation(FileEndpoint.APP)
        val selected=local.list(source).single { it.name=="keep.txt" }
        val intent=FilePasteRequest.capture(FileClipboardSnapshot.capture(source,"App",listOf(selected)),location,"SAF")
        val access=FileDestinationAccess(root,resolver,local::browse)
        val checked=FilePastePreflight(access::inspect,access::relation).check(FileTreeScanner(local::browse).scan(intent))
        assertTrue(checked.failure.toString(),checked.checked)
        val decisions=FilePasteDecisions.begin(checked)
        decisions.choose(decisions.state().reference,"keep.txt",FilePasteChoice.KEEP_BOTH)
        val plan=decisions.confirm(decisions.state().reference)
        assertEquals("keep (2).txt",plan.roots.single().destinationName)
        assertEquals("opaque:keep",plan.check.conflicts.single().destination.id)
        assertEquals(0,state().getInt("creates"))
        local.snapshot(source,selected,store).use { frozen ->
            val operation=request(plan.roots.single().destinationName)
            val result=operation.execute(frozen);operation.awaitClosed()
            assertEquals(State.COMPLETE,result.state);assertEquals("keep (2).txt",result.actualName)
            assertArrayEquals(bytes,state().getByteArray("bytes"));assertEquals(bytes.size.toLong(),result.bytesWritten)
        }
        assertEquals(0L,store.reservedBytes);assertEquals(1,state().getInt("creates"));preserved()
        assertArrayEquals(bytes,File(root,"keep.txt").readBytes())
    }

    @Test fun targetPreflightUsesRealGrantFlagsAndConflictMetadataWithoutCreating() = runBlocking {
        File(root,"keep.txt").writeText("new")
        val local=LocalFileBrowser(root,resolver)
        val source=FileLocation(FileEndpoint.APP)
        val request=FilePasteRequest.capture(FileClipboardSnapshot.capture(source,"App",local.list(source).filter { it.name=="keep.txt" }),location,"SAF")
        val scanned=FileTreeScanner(local::browse).scan(request)
        val access=FileDestinationAccess(root,resolver,local::browse)
        val service=FilePastePreflight(access::inspect,access::relation)
        val checked=service.check(scanned)
        assertTrue(checked.failure.toString(),checked.checked)
        assertEquals(FileCreateCapability.ALLOWED,checked.create)
        assertEquals("opaque:keep",checked.conflicts.single().destination.id)
        assertEquals("keep.txt",checked.conflicts.single().source.name)
        val nestedRequest=FilePasteRequest.capture(FileClipboardSnapshot.capture(source,"App",request.items),location.copy(directoryId="pub:folder"),"Nested")
        val nestedCheck=service.check(FileTreeScanner(local::browse).scan(nestedRequest))
        assertTrue(nestedCheck.failure.toString(),nestedCheck.checked)
        assertEquals(0,state().getInt("creates"));assertEquals(0,state().getInt("writes"));assertEquals(0,state().getInt("reads"));assertEquals(0,state().getInt("deletes"))
        assertArrayEquals("original".toByteArray(),state().getByteArray("keep"))
        reset("no-create")
        assertEquals(FilePasteCheckFailureKind.READ_ONLY,service.check(scanned).failure?.kind)
        reset("list-fail")
        assertEquals(FilePasteCheckFailureKind.TARGET_READ,service.check(scanned).failure?.kind)
        reset("normal")
        resolver.releasePersistableUriPermission(Uri.parse(location.endpoint.treeUri),Intent.FLAG_GRANT_WRITE_URI_PERMISSION)
        assertEquals(FilePasteCheckFailureKind.READ_ONLY,service.check(scanned).failure?.kind)
        revoke()
        assertEquals(FilePasteCheckFailureKind.TARGET_READ,service.check(scanned).failure?.kind)
        assertEquals(0,state().getInt("creates"))
    }

    @Test fun targetPreflightSafRelationshipsUseOpaqueIdsAndRejectForeignTrees() = runBlocking {
        val local=LocalFileBrowser(root,resolver)
        val access=FileDestinationAccess(root,resolver,local::browse)
        val nested=location.copy(directoryId="pub:folder")
        assertEquals(FileLocationRelation.SAME,access.relation(location,location))
        assertEquals(FileLocationRelation.DESCENDANT,access.relation(location,nested))
        assertEquals(FileLocationRelation.SEPARATE,access.relation(nested,location))
        assertEquals(FileCreateCapability.ALLOWED,access.inspect(nested).create)
        val selected=local.list(location).filter { it.id=="pub:folder" }
        val request=FilePasteRequest.capture(FileClipboardSnapshot.capture(location,"SAF",selected),nested,"Nested")
        val scanned=FileTreeScanner(local::browse).scan(request)
        assertTrue(scanned.complete)
        val check=FilePastePreflight(access::inspect,access::relation).check(scanned)
        assertEquals(FilePasteCheckFailureKind.INSIDE_SOURCE,check.failure?.kind)
        for(invalid in listOf(location.copy(directoryId="outside"),location.copy(endpoint=location.endpoint.copy(rootId="pub:folder"),directoryId="pub:folder"),location.copy(directoryId="opaque:keep"))) {
            var failed=false;try { access.inspect(invalid) } catch(_:Exception) { failed=true };assertTrue(failed)
        }
        assertEquals(0,state().getInt("creates"));assertEquals(0,state().getInt("writes"))
    }
    private fun request(name:String="new.bin",destination:FileLocation=location)=publisher.prepare(destination,name).also { requests+=it }
    private fun fds()=File("/proc/self/fd").listFiles()!!.count { try { Os.readlink(it.path).contains("/${root.name}/") } catch(_:Exception) { false } }
    private fun preserved() { assertArrayEquals("original".toByteArray(),state().getByteArray("keep"));assertEquals(0,state().getInt("deletes")) }

    @Test fun journalUriIsPersistedBeforeContentWrite() = runBlocking<Unit> {
        val args=InstrumentationRegistry.getArguments()
        val crash=args.getString("journalCrash")=="true"
        val token=args.getString("token")
        if(crash) require(token!=null && token.matches(Regex("[a-z0-9-]{1,60}")))
        val taskRoot=if(crash) File(context.filesDir,"journal-crash-$token") else File(root,"journal-task")
        check(!taskRoot.exists());val home=File(taskRoot,"home")
        File(home,"source").mkdirs();File(home,"target").mkdir()
        File(home,"source/a.txt").writeText("process death bytes ☃\n")
        val source=FileLocation(FileEndpoint.APP,"source");val browser=LocalFileBrowser(home,resolver)
        val intent=FilePasteRequest.capture(FileClipboardSnapshot.capture(source,"source",browser.browse(source).entries),location,"SAF destination")
        val scan=FileTreeScanner(browser::browse).scan(intent)
        val access=FileDestinationAccess(home,resolver,browser::browse)
        val decisions=FilePasteDecisions.begin(FilePastePreflight(access::inspect,access::relation).check(scan))
        val plan=decisions.confirm(decisions.state().reference)
        val journal=FileTaskJournal(File(taskRoot,"file-history/tasks.db"))
        journal.load();journal.accept(intent);journal.phase(intent.id,FileJournalPhase.CONFIRMING)
        var observedUri:String?=null
        val operation=FileCopyExecutor(home,resolver,File(taskRoot,"staging"),{ r ->
            FileTaskConnections(r.source.endpoint,r.destination.endpoint,{ _,_->error("no remote") },{ _,_,_->error("no auth") })
        },record={ value,point ->
            journal.checkpoint(intent.id,value,point)
            if(point==FileCopyCheckpoint.DOCUMENT_CREATED) observedUri=value.documentUri
            val boundary=if(args.getString("point")=="saf-create-intent") FileCopyCheckpoint.DOCUMENT_CREATE else FileCopyCheckpoint.DOCUMENT_CREATED
            if(crash && point==boundary) {
                val pending=File(taskRoot,"ready.pending")
                java.io.FileOutputStream(pending).use {
                    it.write("${android.os.Process.myPid()}\n${intent.id}\n${args.getString("point")}\n".toByteArray());it.fd.sync()
                }
                check(pending.renameTo(File(taskRoot,"ready")))
                CompletableDeferred<Unit>().await()
            }
            if(!crash && point==FileCopyCheckpoint.DOCUMENT_CREATED) throw java.io.IOException("stop before SAF content write")
        }).prepare(plan)
        journal.begin(intent.id,operation.outcome.items)
        try {
            val result=withTimeout(15000) { operation.execute() };operation.awaitClosed()
            assertNotNull("URI checkpoint must be observed",observedUri)
            assertNotNull("checkpoint failure must stop copying",operation.recordingError)
            assertNotEquals(FileCopyState.COMPLETE,result.state)
            val saved=FileTaskJournal(File(taskRoot,"file-history/tasks.db")).load().single()
            assertTrue(saved.interrupted);assertEquals(observedUri,saved.items.single().value.documentUri)
            assertEquals(FileCopyCheckpoint.DOCUMENT_CREATED,saved.items.single().checkpoint)
            assertEquals("URI must be recorded before provider identity validation",FileCopyItemState.UNKNOWN,saved.items.single().value.state)
            assertEquals(state().getString("latest"),DocumentsContract.getDocumentId(Uri.parse(observedUri)))
            assertEquals(1,state().getInt("creates"));assertEquals(0,state().getInt("writes"))
            assertArrayEquals(byteArrayOf(),state().getByteArray("bytes"));preserved()
        } finally { operation.cancel();withContext(NonCancellable) { operation.awaitClosed() } }
    }

    @Test fun completeBytesAndEmptyNestedDocumentCloseAndVerify() = runBlocking {
        val bytes=ByteArray(100003){(it%251).toByte()};val progress=mutableListOf<Long>()
        snapshot(bytes).use { frozen ->
            val held=fds();val operation=request("内容.bin",location.copy(directoryId="pub:folder"))
            val result=operation.execute(frozen) { copied,total -> assertEquals(bytes.size.toLong(),total);progress+=copied }
            assertEquals(State.COMPLETE,result.state);assertNull(result.error);assertEquals("内容.bin",result.actualName)
            assertEquals(bytes.size.toLong(),result.bytesWritten);assertArrayEquals(bytes,state().getByteArray("bytes"))
            assertEquals(listOf(0L,32768L,65536L,98304L,100003L),progress)
            assertEquals(held,fds());assertTrue(operation.finished);assertEquals(1,state().getInt("closed"));assertEquals(1,state().getInt("reads"))
        }
        snapshot(byteArrayOf()).use { assertEquals(State.COMPLETE,request("empty").execute(it).state);assertEquals(0,state().getByteArray("bytes")!!.size) }
        assertEquals(0L,store.reservedBytes);assertEquals(0,fds());preserved()
    }
    @Test fun observedConflictNeverCreatesOrWrites() = runBlocking {
        snapshot("new".toByteArray()).use { assertEquals(State.CONFLICT,request("keep.txt").execute(it).state) }
        assertEquals(0,state().getInt("creates"));assertEquals(0,state().getInt("writes"));preserved()
    }
    @Test fun invalidNamesTreeIdentityAndReadOnlyGrantStopBeforeCreate() = runBlocking {
        for(name in listOf("",".","..","a/b","bad\u0000","\uD800")) {
            var failed=false;try { publisher.prepare(location,name) } catch(_:Exception) { failed=true };assertTrue(name,failed)
        }
        snapshot("new".toByteArray()).use { frozen ->
            for(destination in listOf(location.copy(directoryId="outside"),location.copy(endpoint=location.endpoint.copy(rootId="different")))) {
                val result=request(destination=destination).execute(frozen);assertEquals(State.NOT_ATTEMPTED,result.state);assertNotNull(result.error)
                if(destination.endpoint.rootId=="different")assertEquals("目标文档树身份已改变",result.error)
            }
            resolver.releasePersistableUriPermission(Uri.parse(location.endpoint.treeUri),Intent.FLAG_GRANT_WRITE_URI_PERMISSION)
            assertEquals(State.NOT_ATTEMPTED,request().execute(frozen).state)
            revoke();assertEquals(State.NOT_ATTEMPTED,request().execute(frozen).state)
        }
        assertEquals(0,state().getInt("creates"));preserved()
    }
    @Test fun unsupportedDirectoryAndFailedOrDuplicateListingDoNotCreate() = runBlocking {
        snapshot("new".toByteArray()).use { frozen -> for(mode in listOf("no-create","list-fail","duplicate","too-many")) {
            reset(mode);val result=request().execute(frozen)
            assertEquals(mode,State.NOT_ATTEMPTED,result.state);assertNotNull(result.error);assertEquals(0,state().getInt("creates"))
        } };preserved()
    }
    @Test fun returnedExistingOrParentIdentityIsNeverOpenedOrDeleted() = runBlocking {
        snapshot("new".toByteArray()).use { frozen -> for(mode in listOf("return-existing","return-parent")) {
            reset(mode);val result=request().execute(frozen)
            assertEquals(mode,State.UNKNOWN,result.state);assertNotNull(result.documentUri);assertNotNull(result.error)
            assertEquals(1,state().getInt("creates"));assertEquals(0,state().getInt("writes"));preserved()
        } }
    }
    @Test fun failedCreateReplyRemainsUnknownEvenIfDocumentWasCreated() = runBlocking {
        snapshot("new".toByteArray()).use { frozen -> for(mode in listOf("create-denied","create-lost")) {
            reset(mode);val result=request().execute(frozen)
            assertEquals(mode,State.UNKNOWN,result.state);assertNull(result.documentUri);assertNotNull(result.error)
            assertEquals(mode=="create-lost",state().getString("latest")!=null);assertEquals(0,state().getInt("writes"));preserved()
        } }
    }
    @Test fun providerChosenNameAndPipeDestinationAreVerified() = runBlocking {
        val bytes=ByteArray(100003){(it%97).toByte()}
        snapshot(bytes).use { frozen -> for(mode in listOf("rename","pipe-write")) {
            reset(mode);val result=request().execute(frozen)
            assertEquals(result.error,State.COMPLETE,result.state)
            assertEquals(if(mode=="rename") "provider-new.bin" else "new.bin",result.actualName)
            assertArrayEquals(bytes,state().getByteArray("bytes"));assertEquals(1,state().getInt("closed"));preserved()
        } }
    }
    @Test fun writeDenialRetainsCreatedUriAndDoesNotDelete() = runBlocking {
        snapshot("new".toByteArray()).use { frozen -> for(mode in listOf("no-write","write-denied","new-nonempty","virtual","wrong-metadata")) {
            reset(mode);val result=request().execute(frozen)
            assertEquals(mode,State.CREATED,result.state);assertNotNull(result.documentUri);assertNotNull(result.error)
            assertEquals(0L,result.bytesWritten);assertEquals(0,state().getInt("reads"));if(mode!="write-denied")assertEquals(0,state().getInt("writes"));preserved()
        } }
    }
    @Test fun corruptedShortExtraUnreadableAndReliableErrorNeverComplete() = runBlocking {
        snapshot("verified bytes".toByteArray()).use { frozen -> for(mode in listOf("corrupt","short","extra","read-denied","pipe-error")) {
            reset(mode);val result=request().execute(frozen)
            assertEquals(mode,State.CREATED,result.state);assertNotNull(result.documentUri);assertNotNull(result.error)
            assertEquals(frozen.size,result.bytesWritten);assertEquals(1,state().getInt("closed"));preserved()
        } }
        assertEquals(0,fds())
    }
    @Test fun cancellationBeforeCreateAndCheckpointFailureRetainTruth() = runBlocking {
        snapshot("new".toByteArray()).use { frozen ->
            val cancelled=request();cancelled.cancel()
            assertTrue("unstarted cancellation has no worker to await",withTimeoutOrNull(1000) { cancelled.awaitClosed();true } == true)
            try { cancelled.execute(frozen);fail("must cancel") } catch(_:CancellationException) {}
            cancelled.awaitClosed();assertEquals(State.NOT_ATTEMPTED,cancelled.outcome.state);assertEquals(0,state().getInt("creates"))
            val failed=request();val result=failed.execute(frozen) { _,_->error("checkpoint failed") }
            assertEquals(State.CREATED,result.state);assertNotNull(result.error);assertEquals(0L,result.bytesWritten);preserved()
        }
        assertEquals(0,fds())
    }
    @Test fun cancelledDelayedCreateKeepsLeaseUntilReplyAndRetainsUri() = runBlocking {
        reset("late-create");val frozen=snapshot("new".toByteArray());val operation=request()
        val caller=async { operation.execute(frozen) }
        try {
            until { state().getString("latest")!=null };caller.cancelAndJoin();frozen.close()
            assertEquals(State.UNKNOWN,operation.outcome.state);assertFalse(operation.finished)
            assertEquals(3L,store.reservedBytes);assertTrue(fds()>0)
        } finally { state("release");caller.cancelAndJoin();withTimeout(6000) { operation.awaitClosed() };frozen.close() }
        assertEquals(State.CREATED,operation.outcome.state);assertNotNull(operation.outcome.documentUri)
        assertEquals(0,state().getInt("writes"));assertEquals(0L,store.reservedBytes);assertEquals(0,fds());preserved()
    }
    @Test fun backpressuredWriterCancellationClosesLeaseAndKeepsPartialDocument() = runBlocking {
        reset("pipe-stall");val frozen=snapshot(ByteArray(200003){65});val operation=request()
        val caller=async { operation.execute(frozen) }
        try {
            until { operation.outcome.bytesWritten>0 };delay(150);caller.cancelAndJoin();frozen.close()
            assertTrue("writer must close after cancellation",withTimeoutOrNull(2000) { operation.awaitClosed();true } == true)
            assertEquals(State.CREATED,operation.outcome.state);assertNotNull(operation.outcome.error)
            assertTrue(operation.outcome.bytesWritten in 1 until 200003);assertEquals(0L,store.reservedBytes);assertEquals(0,fds())
        } finally { state("release");caller.cancelAndJoin();withTimeout(6000) { operation.awaitClosed() };frozen.close() }
        until { state().getInt("active")==0 };assertEquals(1,state().getInt("writeErrors"));preserved()
    }
    @Test fun cancellationDuringReadbackWaitKeepsLeaseAndCreatedOutcome() = runBlocking {
        reset("read-stall");val frozen=snapshot("new".toByteArray());val operation=request()
        val caller=async { operation.execute(frozen) }
        try {
            until { state().getInt("reads")==1 };caller.cancelAndJoin();frozen.close()
            assertEquals(State.CREATED,operation.outcome.state);assertFalse(operation.finished);assertEquals(3L,store.reservedBytes)
        } finally { state("release");caller.cancelAndJoin();withTimeout(6000) { operation.awaitClosed() };frozen.close() }
        assertEquals(State.CREATED,operation.outcome.state);assertNotNull(operation.outcome.error)
        assertEquals(0L,store.reservedBytes);assertEquals(0,fds());preserved()
    }
    @Test fun sourceMutationStopsBeforeReadbackAndRetainsCreatedDocument() = runBlocking {
        snapshot("source bytes".toByteArray()).use { frozen ->
            val operation=request()
            val result=operation.execute(frozen) { copied,total -> if(copied==total) {
                frozen.withDescriptor { source,_ ->
                    val writable=Os.open("/proc/self/fd/$source",OsConstants.O_RDWR or OsConstants.O_CLOEXEC,0)
                    try { Os.pwrite(writable,byteArrayOf(99),0,1,0) } finally { Os.close(writable) }
                }
            } }
            assertEquals(State.CREATED,result.state);assertNotNull(result.error);assertNotNull(result.documentUri)
            assertEquals(0,state().getInt("reads"));preserved()
        }
        assertEquals(0,fds())
    }
    @Test fun cancellationStopsSuspendedProgressBeforeReleasingReservation() = runBlocking {
        val frozen=snapshot("new".toByteArray());val operation=request()
        val entered=CompletableDeferred<Unit>();val release=CompletableDeferred<Unit>()
        val caller=async { operation.execute(frozen) { _,_->entered.complete(Unit);release.await() } }
        try {
            withTimeout(6000) { entered.await() };caller.cancelAndJoin();frozen.close()
            val stopped=withTimeoutOrNull(2000) { operation.awaitClosed();true } ?: false
            assertTrue("worker must cancel suspended progress",stopped)
            assertEquals(State.CREATED,operation.outcome.state);assertEquals(0L,store.reservedBytes);assertEquals(0,fds())
        } finally { release.complete(Unit);caller.cancelAndJoin();withTimeout(6000) { operation.awaitClosed() };frozen.close() }
        preserved()
    }
    @Test fun cancelledResultDeliveryKeepsCompleteAndRequestIsOneShot() = runBlocking {
        snapshot("new".toByteArray()).use { frozen ->
            val once=request("once.bin")
            assertEquals(State.COMPLETE,once.execute(frozen).state)
            var duplicate:IllegalStateException?=null
            try { once.execute(frozen) } catch(failure:IllegalStateException) { duplicate=failure }
            assertEquals("粘贴请求不能重复执行",duplicate?.message)
            val operation=request();val queue=LinkedBlockingQueue<Runnable>()
            val dispatcher=object:CoroutineDispatcher() { override fun dispatch(context:CoroutineContext,block:Runnable) { queue.add(block) } }
            val caller=async(dispatcher) { operation.execute(frozen) }
            try {
                checkNotNull(queue.poll(3,TimeUnit.SECONDS)).run()
                withTimeout(6000) { operation.awaitClosed() }
                assertEquals(State.COMPLETE,operation.outcome.state)
                caller.cancel();checkNotNull(queue.poll(3,TimeUnit.SECONDS)).run();caller.join()
                assertEquals(State.COMPLETE,operation.outcome.state);assertNull(operation.outcome.error)
                var repeated:IllegalStateException?=null
                try { operation.execute(frozen) } catch(failure:IllegalStateException) { repeated=failure }
                assertEquals("粘贴请求不能重复执行",repeated?.message)
                assertEquals(2,state().getInt("creates"));preserved()
            } finally { caller.cancel();while(queue.isNotEmpty())queue.poll()?.run();caller.join() }
        }
    }
}
