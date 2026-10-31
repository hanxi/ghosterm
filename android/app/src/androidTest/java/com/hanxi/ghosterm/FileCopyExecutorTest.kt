package com.hanxi.ghosterm

import android.content.res.AssetFileDescriptor
import android.os.ParcelFileDescriptor
import android.system.Os
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.hanxi.ghosterm.bridge.*
import com.hanxi.ghosterm.data.*
import com.hanxi.ghosterm.domain.*
import kotlinx.coroutines.*
import org.junit.*
import org.junit.Assert.*
import org.junit.runner.RunWith
import java.io.File
import java.util.UUID
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

@RunWith(AndroidJUnit4::class)
class FileCopyExecutorTest {
    private val context=InstrumentationRegistry.getInstrumentation().targetContext
    private val fixture by lazy { sshFixture() }
    private lateinit var root:File
    private lateinit var home:File
    private lateinit var local:LocalFileBrowser
    private val origin=FileLocation(FileEndpoint.APP,"source")
    private val target=FileLocation(FileEndpoint.APP,"target")
    private val requests=mutableListOf<FileCopyExecutor.Request>()
    @Before fun setup() {
        root=File(context.cacheDir,"copy-executor-${UUID.randomUUID()}").apply { mkdir() }
        home=File(root,"home").apply { mkdir() };File(home,"source").mkdir();File(home,"target").mkdir()
        local=LocalFileBrowser(home,context.contentResolver)
    }
    @After fun cleanup() { runBlocking {
        for(request in requests) { request.cancel();withTimeout(15000) { request.awaitClosed() } }
        root.deleteRecursively()
    } }
    private suspend fun plan(source:FileLocation=origin,destination:FileLocation=target,
        policy:FileConflictPolicy=FileConflictPolicy.ASK,select:List<String>?=null,
        browse:suspend(FileLocation)->BrowserDirectory=local::browse):FileCopyPlan {
        val listed=browse(source).entries
        val entries=select?.map { name -> listed.single { it.name==name } } ?: listed
        val request=FilePasteRequest.capture(FileClipboardSnapshot.capture(source,"source",entries),destination,"target",policy)
        val scan=FileTreeScanner(browse).scan(request);assertTrue(scan.failure.toString(),scan.complete)
        val access=FileDestinationAccess(home,context.contentResolver,browse)
        val check=FilePastePreflight(access::inspect,access::relation).check(scan);assertTrue(check.failure.toString(),check.checked)
        val decisions=FilePasteDecisions.begin(check)
        for(question in decisions.state().pending) {
            assertEquals(FilePasteDecisionKind.UNSUPPORTED,question.kind)
            decisions.choose(decisions.state().reference,question.source.relativePath,FilePasteChoice.SKIP)
        }
        return decisions.confirm(decisions.state().reference)
    }
    private fun executor(prepare:()->Unit={},limit:Long=262144,attempts:Int=10000)=FileCopyExecutor(home,context.contentResolver,File(root,"tasks"),{
        prepare();FileTaskConnections(it.source.endpoint,it.destination.endpoint,{ _,_->error("unexpected remote") },{ _,_,_->error("unexpected auth") })
    },limit,attempts)
    private fun request(plan:FileCopyPlan,executor:FileCopyExecutor=executor())=executor.prepare(plan).also { requests+=it }
    private suspend fun run(plan:FileCopyPlan,executor:FileCopyExecutor=executor()):FileCopyProgress {
        val request=request(plan,executor);val outcome=withTimeout(15000) { request.execute() };request.awaitClosed()
        assertTrue(request.finished);assertSame(outcome,request.outcome);return outcome
    }
    @Test fun phaseAverageUsesElapsedTimeWithoutInventingUnknownTotals() {
        val activity=FileCopyActivity("file",FileCopyStage.READING,1000uL,null,100,200)
        assertNull("speed needs a time window",activity.averageBytesPerSecond(1099))
        assertEquals("phase speed uses real elapsed time",500.0,activity.averageBytesPerSecond(2100)!!,0.001)
        assertNull("unknown total stays unknown",activity.totalBytes)
        assertNull("no bytes means no speed estimate",activity.copy(bytes=0uL).averageBytesPerSecond(2100))
        assertNull("verification is not byte transfer",activity.copy(stage=FileCopyStage.VERIFYING).averageBytesPerSecond(2100))
        assertNull("unknown size has no time estimate",activity.estimatedSeconds(2100))
        val known=activity.copy(totalBytes=2000uL,observedMillis=2000)
        assertEquals("remaining time is for this phase",2L,known.estimatedSeconds(2100))
        assertNull("reached byte total is not zero seconds to completion",known.copy(totalBytes=1000uL).estimatedSeconds(2100))
        assertNull("stalled IO has no remaining-time estimate",known.estimatedSeconds(4101))
        assertNull("verification duration is unknown",known.copy(stage=FileCopyStage.VERIFYING).estimatedSeconds(2100))
    }
    @Test fun localWriteBytesStayUncommittedUntilPublication()=runBlocking<Unit> {
        val source=File(home,"source/file").apply { writeBytes(ByteArray(100003) { (it%251).toByte() }) }
        lateinit var copy:FileCopyExecutor.Request
        var before:FileCopyProgress?=null
        val executor=FileCopyExecutor(home,context.contentResolver,File(root,"tasks"),{
            FileTaskConnections(it.source.endpoint,it.destination.endpoint,{ _,_->error("unexpected remote") },{ _,_,_->error("unexpected auth") })
        },record={ _,point -> if(point==FileCopyCheckpoint.PUBLISH) {
            before=copy.outcome
        } })
        copy=request(plan(),executor)
        val out=copy.execute();copy.awaitClosed()
        assertNotNull("local publication checkpoint executed",before)
        val progress=checkNotNull(before)
        assertEquals("local final bytes await publication",FileCopyStage.VERIFYING,progress.activity?.stage)
        assertEquals("local observed write byte count",source.length().toULong(),progress.activity?.bytes)
        assertEquals("local write has not committed",0uL,progress.committedBytes)
        assertEquals(FileCopyState.RUNNING,progress.state)
        assertEquals(FileCopyState.COMPLETE,out.state)
        assertEquals(source.length().toULong(),out.committedBytes)
        assertArrayEquals(source.readBytes(),File(home,"target/file").readBytes())
    }
    @Test fun localTreeCopiesRealBytesEmptyHiddenAndUnicodeAndKeepsSource()=runBlocking<Unit> {
        File(home,"source/目录 ☃/empty").mkdirs();File(home,"source/目录 ☃/.hidden").writeText("内容\n".repeat(50))
        File(home,"source/zero").writeBytes(byteArrayOf())
        val plan=plan();val outcome=run(plan)
        assertEquals(FileCopyState.COMPLETE,outcome.state);assertTrue(outcome.mayClearClipboard)
        assertEquals(plan.request.id,outcome.requestId);assertEquals(plan.request.clipboardId,outcome.clipboardId)
        assertEquals(plan.entries.size,outcome.completed)
        assertTrue(File(home,"target/目录 ☃/empty").isDirectory)
        assertTrue(File(home,"target/目录 ☃/.hidden").isFile)
        assertArrayEquals(File(home,"source/目录 ☃/.hidden").readBytes(),File(home,"target/目录 ☃/.hidden").readBytes())
        assertEquals(0L,File(home,"target/zero").length());assertTrue(File(home,"source/zero").exists())
        assertEquals(File(home,"source/目录 ☃/.hidden").length().toULong(),outcome.committedBytes)
        assertThrows(UnsupportedOperationException::class.java) { (outcome.items as MutableList).clear() }
    }
    @Test fun frozenPlanAndSourceChangesStopBeforeDestinationWrites()=runBlocking<Unit> {
        File(home,"source/file").writeText("before")
        val original=plan()
        val outcome=run(original,executor({ File(home,"source/file").writeText("changed length") }))
        assertEquals(FileCopyState.FAILED,outcome.state);assertFalse(outcome.mayClearClipboard)
        assertTrue(File(home,"target").list()!!.isEmpty());assertEquals(FileCopyItemState.NOT_ATTEMPTED,outcome.items.single().state)
    }
    @Test fun destinationSymlinkAndReadFailuresDoNotBecomeEmptyTargets()=runBlocking<Unit> {
        File(home,"source/file").writeText("new");val plan=plan()
        val elsewhere=File(root,"elsewhere").apply { mkdir() };File(elsewhere,"file").writeText("keep")
        val out=run(plan,executor({ File(home,"target").delete();Os.symlink(elsewhere.path,File(home,"target").path) }))
        assertEquals(FileCopyState.FAILED,out.state);assertEquals("keep",File(elsewhere,"file").readText())
        File(home,"target").delete()
    }
    @Test fun lateConflictsAskSkipAndKeepBothNeverOverwrite()=runBlocking<Unit> {
        File(home,"source/a.txt").writeText("new");File(home,"source/a (2).txt").writeText("incoming")
        val keep=plan(policy=FileConflictPolicy.KEEP_BOTH,select=listOf("a.txt","a (2).txt"))
        val kept=run(keep,executor({ File(home,"target/a.txt").writeText("old") }))
        assertEquals(FileCopyState.COMPLETE,kept.state)
        assertTrue(File(home,"target/a (3).txt").isFile)
        assertEquals("old",File(home,"target/a.txt").readText());assertEquals("new",File(home,"target/a (3).txt").readText())
        assertEquals("incoming",File(home,"target/a (2).txt").readText())
        File(home,"target").deleteRecursively();File(home,"target").mkdir()
        val ask=plan();val stopped=run(ask,executor({ File(home,"target/a (2).txt").writeText("late") }))
        assertEquals(FileCopyState.FAILED,stopped.state);assertTrue(stopped.items.any { it.state==FileCopyItemState.CONFLICT })
        assertFalse(stopped.mayClearClipboard);assertEquals("late",File(home,"target/a (2).txt").readText())
    }
    @Test fun directorySkipIncludesAllChildrenAndMixedUnsupportedIsVisible()=runBlocking<Unit> {
        File(home,"source/folder/empty").mkdirs();File(home,"source/folder/text").writeText("source")
        val frozen=plan(policy=FileConflictPolicy.SKIP)
        val skipped=run(frozen,executor({ File(home,"target/folder").mkdir();File(home,"target/folder/existing").writeText("old") }))
        assertEquals(FileCopyState.ALL_SKIPPED,skipped.state);assertTrue(skipped.items.all { it.state==FileCopyItemState.SKIPPED })
        assertEquals(listOf("existing"),File(home,"target/folder").list()!!.toList());assertFalse(skipped.mayClearClipboard)
        File(home,"source/file").writeText("new");Os.symlink("file",File(home,"source/link").path)
        val mixed=run(plan(select=listOf("file","link")))
        assertEquals(FileCopyState.WITH_SKIPS,mixed.state);assertEquals(1,mixed.completed);assertFalse(mixed.mayClearClipboard)
        assertEquals(FileCopyItemState.SKIPPED,mixed.items.single { it.source.entry.kind==FileKind.LINK }.state)
        assertFalse(File(home,"target/link").exists())
    }
    @Test fun allSkippedDoesNotAcquireConnectionsOrCreateStaging()=runBlocking<Unit> {
        File(home,"source/file").writeText("new");File(home,"target/file").writeText("old")
        val acquisitions=java.util.concurrent.atomic.AtomicInteger()
        val out=run(plan(policy=FileConflictPolicy.SKIP),executor({ acquisitions.incrementAndGet() }))
        assertEquals("all skipped must perform no IO",0,acquisitions.get())
        assertEquals(FileCopyState.ALL_SKIPPED,out.state);assertFalse(out.mayClearClipboard)
        assertFalse(File(root,"tasks").exists());assertEquals("old",File(home,"target/file").readText())
    }
    @Test fun cancellationBeforeStartAndRepeatedExecutionCannotWriteAgain()=runBlocking<Unit> {
        File(home,"source/file").writeText("new");val plan=plan();val cancelled=request(plan)
        cancelled.cancel();cancelled.awaitClosed();assertEquals(FileCopyState.CANCELLED,cancelled.outcome.state)
        assertTrue(runCatching { cancelled.execute() }.isFailure);assertTrue(File(home,"target").list()!!.isEmpty())
        val completed=request(plan);assertEquals(FileCopyState.COMPLETE,completed.execute().state)
        assertTrue(runCatching { completed.execute() }.isFailure);completed.cancel();assertEquals(FileCopyState.COMPLETE,completed.outcome.state)
    }
    @Test fun renameAndSnapshotBudgetsStopWithoutOverwriting()=runBlocking<Unit> {
        File(home,"source/a").writeText("123456789");val plan=plan(policy=FileConflictPolicy.KEEP_BOTH)
        val out=run(plan,executor({ File(home,"target/a").writeText("old");File(home,"target/a (2)").writeText("older") },attempts=1))
        assertEquals(FileCopyState.FAILED,out.state);assertEquals("old",File(home,"target/a").readText());assertFalse(out.mayClearClipboard)
        File(home,"target").deleteRecursively();File(home,"target").mkdir()
        val oversized=run(plan(),executor(limit=4));assertEquals(FileCopyState.FAILED,oversized.state)
        assertTrue(File(home,"target").list()!!.isEmpty());assertEquals(0uL,oversized.committedBytes)
    }
    @Test fun partialTreeRetainsCreatedDirectoriesAndStopsFollowingWrites()=runBlocking<Unit> {
        File(home,"source/folder/empty").mkdirs();File(home,"source/folder/large").writeText("123456789")
        File(home,"source/z-last").writeText("ok")
        val out=run(plan(),executor(limit=4))
        assertEquals(FileCopyState.FAILED,out.state);assertTrue(File(home,"target/folder").isDirectory)
        assertTrue(out.items.any { it.source.entry.kind==FileKind.DIRECTORY && it.state==FileCopyItemState.COMPLETE })
        assertFalse(File(home,"target/z-last").exists());assertFalse(out.mayClearClipboard)
        assertEquals(FileCopyItemState.NOT_ATTEMPTED,out.items.single { it.source.entry.name=="z-last" }.state)
    }
    @Test fun sourceStoreCloseWaitsForBlockedProviderAndItsLateDescriptor()=runBlocking<Unit> {
        val store=TransferStagingStore(File(root,"staging"),1024,1024,0)
        val entered=CountDownLatch(1);val release=CountDownLatch(1);val source=File(root,"read").apply { writeText("x") }
        val reading=async(Dispatchers.IO) { runCatching { store.snapshot(1) {
            entered.countDown();assertTrue(release.await(5,TimeUnit.SECONDS))
            AssetFileDescriptor(ParcelFileDescriptor.open(source,ParcelFileDescriptor.MODE_READ_ONLY),0,1)
        } } }
        assertTrue(entered.await(5,TimeUnit.SECONDS))
        val close=async { store.closeAndAwait() };delay(80)
        assertFalse("provider still owns an outstanding source call",close.isCompleted)
        release.countDown();withTimeout(5000) { close.await();reading.await() }
        assertEquals(0L,store.reservedBytes);assertTrue(reading.await().isFailure)
        store.closeAndAwait()
    }
    @Test fun sourceStoreCloseKeepsOutstandingReaderReservationUntilItReturns()=runBlocking<Unit> {
        val store=TransferStagingStore(File(root,"staging"),1024,1024,0)
        val source=File(root,"read").apply { writeText("x") }
        val snapshot=store.snapshot(1) { AssetFileDescriptor(ParcelFileDescriptor.open(source,ParcelFileDescriptor.MODE_READ_ONLY),0,1) }
        val entered=CompletableDeferred<Unit>();val release=CompletableDeferred<Unit>()
        val reader=launch { snapshot.withDescriptor { _,_->entered.complete(Unit);release.await() } };entered.await()
        val close=async { store.closeAndAwait() };delay(80);assertFalse(close.isCompleted);assertEquals(1L,store.reservedBytes)
        release.complete(Unit);withTimeout(5000) { reader.join();close.await() };assertEquals(0L,store.reservedBytes)
    }
    @Test fun storeCloseSignalsBlockedSourceAndCancelledWaiterDoesNotReopenIt()=runBlocking<Unit> {
        val store=TransferStagingStore(File(root,"staging"),1024,1024,0)
        val entered=CountDownLatch(1);val cancelled=CountDownLatch(1);val release=CountDownLatch(1)
        val source=File(root,"read").apply { writeText("x") }
        val reading=async(Dispatchers.IO) { runCatching { store.snapshot(1) { signal ->
            signal.setOnCancelListener { cancelled.countDown() };entered.countDown()
            assertTrue(release.await(5,TimeUnit.SECONDS))
            AssetFileDescriptor(ParcelFileDescriptor.open(source,ParcelFileDescriptor.MODE_READ_ONLY),0,1)
        } } }
        try {
            assertTrue(entered.await(5,TimeUnit.SECONDS))
            val close=launch(Dispatchers.IO) { store.closeAndAwait() }
            assertTrue("source receives CancellationSignal",cancelled.await(2,TimeUnit.SECONDS))
            close.cancelAndJoin();assertEquals(1L,store.reservedBytes)
        } finally { release.countDown() }
        withTimeout(5000) { reading.await();store.closeAndAwait() };assertEquals(0L,store.reservedBytes)
        assertTrue(runCatching { store.snapshot(1) { error("closed store must not open source") } }.isFailure)
    }
    @Test fun plannedRenameUsesFrozenNameAndRetainsActualResult()=runBlocking<Unit> {
        File(home,"source/a.txt").writeText("new");File(home,"target/a.txt").writeText("keep")
        val frozen=plan(policy=FileConflictPolicy.KEEP_BOTH)
        val planned=frozen.roots.single().destinationName;assertEquals("a (2).txt",planned)
        val out=run(frozen,executor({ assertTrue(File(home,"target/a.txt").renameTo(File(home,"target/original-kept"))) }))
        assertEquals(FileCopyState.COMPLETE,out.state);assertTrue(File(home,"target/$planned").isFile)
        assertEquals(planned,out.items.single().targetName);assertEquals("target/$planned",out.items.single().createdId)
        assertEquals("keep",File(home,"target/original-kept").readText());assertEquals("new",File(home,"target/$planned").readText())
    }
    @Test fun cancelledAuthenticationCannotStartWritesAndRetainsBrowser()=runBlocking<Unit> {
        val browser=RemoteFileSession(connection())
        try {
            authenticate(browser)
            val identity=RemoteFileIdentity("copy","copy.ghosterm.test",fixture.getString("user"),fixture.getInt("port"))
            val endpoint=FileEndpoint("cancel-remote","copy",fixture.getString("remoteScratch"),remote=identity)
            val directory=browser.prepareDirectory(endpoint.rootId,"cancel-${UUID.randomUUID()}")
            assertEquals(RemoteDirectoryCreationState.COMPLETE,directory.execute().state);directory.awaitClosed()
            val remote=FileLocation(endpoint,directory.path)
            suspend fun browse(location:FileLocation):BrowserDirectory {
                if(location.endpoint.remote==null)return local.browse(location)
                val found=browser.browse(location.directoryId)
                return BrowserDirectory(location.copy(directoryId=found.path),emptyList())
            }
            File(home,"source/file").writeText("must not publish")
            val entered=CompletableDeferred<RemoteFileSession>();val release=CompletableDeferred<Unit>()
            val executor=FileCopyExecutor(home,context.contentResolver,File(root,"tasks"),{
                FileTaskConnections(it.source.endpoint,it.destination.endpoint,{ _,_->connection() },{ _,_,session ->
                    authenticate(session);entered.complete(session)
                    withContext(NonCancellable) { release.await() }
                })
            })
            val request=request(plan(destination=remote,browse=::browse),executor)
            val waiting=launch { request.execute() }
            val acquired=withTimeout(10000) { entered.await() }
            try {
                waiting.cancelAndJoin();delay(80);assertFalse(request.finished)
            } finally { release.complete(Unit) }
            withTimeout(10000) { request.awaitClosed() }
            assertEquals(FileCopyState.CANCELLED,request.outcome.state)
            assertTrue(request.outcome.items.all { it.state==FileCopyItemState.NOT_ATTEMPTED })
            assertTrue(acquired.state().finished);assertTrue(browser.state().ready)
            assertTrue(browser.browse(remote.directoryId).entries.isEmpty())
        } finally { browser.closeAndAwait() }
    }
    private fun connection():ResolvedSSHConnection {
        val known=File(root,"known").apply { writeText("[copy.ghosterm.test]:${fixture.getInt("port")} "+fixture.getString("public")+"\n") }
        return ResolvedSSHConnection("copy.ghosterm.test",fixture.getInt("port"),fixture.getString("user"),known.path,byteArrayOf(127,0,0,1))
    }
    private suspend fun authenticate(session:RemoteFileSession) {
        withTimeout(10000) {
            var auth=session.authentication()
            while(auth.phase!=SSHAuthPhase.CREDENTIALS) { assertNotEquals(SSHAuthPhase.FAILED,auth.phase);delay(10);auth=session.authentication() }
            session.privateKey(auth,fixture.getString("filePem").toByteArray())
            while(!session.state().ready) { assertFalse(session.state().finished);delay(10) }
        }
    }
    @Test fun remoteRefusalPreservesCodesAndCandidateWithoutTouchingTarget()=runBlocking<Unit> {
        val browser=RemoteFileSession(connection())
        try {
            authenticate(browser)
            val identity=RemoteFileIdentity("copy","copy.ghosterm.test",fixture.getString("user"),fixture.getInt("port"))
            val endpoint=FileEndpoint("readonly-remote","copy",fixture.getString("remoteReadOnly"),remote=identity)
            val remote=FileLocation(endpoint,endpoint.rootId)
            suspend fun browse(location:FileLocation):BrowserDirectory {
                if(location.endpoint.remote==null)return local.browse(location)
                val found=browser.browse(location.directoryId);assertTrue(found.entries.isEmpty())
                return BrowserDirectory(location.copy(directoryId=found.path),emptyList())
            }
            val executor=FileCopyExecutor(home,context.contentResolver,File(root,"tasks"),{
                FileTaskConnections(it.source.endpoint,it.destination.endpoint,{ _,_->connection() },{ _,_,session->authenticate(session) })
            })
            File(home,"source/folder").mkdir();File(home,"source/file").writeText("unpublished")
            val directory=run(plan(destination=remote,select=listOf("folder"),browse=::browse),executor)
            assertEquals(FileCopyState.FAILED,directory.state)
            val d=directory.items.single();assertEquals(FileCopyItemState.FAILED,d.state)
            assertNotNull(d.coreStatus);assertNotEquals(0,d.coreStatus)
            assertNotNull(d.remoteStatus);assertNotEquals(0u,d.remoteStatus);assertNull(d.createdId)
            val file=run(plan(destination=remote,select=listOf("file"),browse=::browse),executor)
            assertEquals(FileCopyState.FAILED,file.state);val item=file.items.single()
            assertEquals(FileCopyItemState.FAILED,item.state);assertNotNull(item.coreStatus);assertNotEquals(0,item.coreStatus)
            assertNotNull(item.candidatePath);assertTrue(item.candidatePath!!.startsWith(remote.directoryId+"/.ghosterm-copy-"))
            assertNull(item.createdId);assertFalse(file.mayClearClipboard);assertEquals(0uL,file.committedBytes)
            assertTrue(browser.state().ready);assertTrue(browser.browse(remote.directoryId).entries.isEmpty())
        } finally { browser.closeAndAwait() }
    }
    @Test fun realSshCopiesBothDirectionsAndKeepsBrowserIndependent()=runBlocking<Unit> {
        val browser=RemoteFileSession(connection())
        try {
            authenticate(browser)
            val identity=RemoteFileIdentity("copy","copy.ghosterm.test",fixture.getString("user"),fixture.getInt("port"))
            val endpoint=FileEndpoint("task-remote","copy",fixture.getString("remoteScratch"),remote=identity)
            suspend fun mkdir(name:String):String {
                val request=browser.prepareDirectory(endpoint.rootId,name)
                assertEquals(RemoteDirectoryCreationState.COMPLETE,request.execute().state);request.awaitClosed();return request.path
            }
            val remote=FileLocation(endpoint,mkdir("batch-${UUID.randomUUID()}"))
            suspend fun browse(location:FileLocation):BrowserDirectory {
                if(location.endpoint.remote==null)return local.browse(location)
                val dir=browser.browse(location.directoryId)
                return BrowserDirectory(location.copy(directoryId=dir.path),dir.entries.map {
                    BrowserEntry(it.path,it.name,if(it.directory)FileKind.DIRECTORY else FileKind.FILE,it.size,it.modifiedSeconds?.times(1000))
                })
            }
            val executor=FileCopyExecutor(home,context.contentResolver,File(root,"tasks"),{
                FileTaskConnections(it.source.endpoint,it.destination.endpoint,{ _,_->connection() },{ _,_,s->authenticate(s) })
            },262144)
            File(home,"source/dir/empty").mkdirs();File(home,"source/dir/hello").writeText("真实 SSH ☃\n")
            val uploaded=run(plan(destination=remote,browse=::browse),executor)
            assertEquals(FileCopyState.COMPLETE,uploaded.state)
            assertTrue(uploaded.items.all { it.coreStatus==0 && it.remoteStatus==0u })
            assertEquals("真实 SSH ☃\n",runCatching { browser.previewText(remote.directoryId+"/dir/hello") }.getOrNull())
            val downloaded=run(plan(source=remote,browse=::browse),executor)
            assertEquals(FileCopyState.COMPLETE,downloaded.state);assertEquals("真实 SSH ☃\n",File(home,"target/dir/hello").readText())
            assertTrue(File(home,"target/dir/empty").isDirectory);assertTrue(browser.state().ready)
            val other=FileLocation(endpoint.copy(id="other-role"),mkdir("other-${UUID.randomUUID()}"))
            val copied=run(plan(source=remote,destination=other,browse=::browse),executor)
            assertEquals(FileCopyState.COMPLETE,copied.state)
            assertEquals("真实 SSH ☃\n",runCatching { browser.previewText(other.directoryId+"/dir/hello") }.getOrNull())
            assertTrue(browser.browse(other.directoryId+"/dir/empty").entries.isEmpty())
        } finally { browser.closeAndAwait() }
    }
}
