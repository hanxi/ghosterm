package com.hanxi.ghosterm

import android.system.Os
import android.system.OsConstants
import androidx.test.platform.app.InstrumentationRegistry
import com.hanxi.ghosterm.bridge.LocalPublicationFile
import com.hanxi.ghosterm.data.*
import com.hanxi.ghosterm.domain.*
import kotlinx.coroutines.*
import org.junit.*
import org.junit.Assert.*
import java.io.File
import java.util.UUID

class LocalEmptyRequestsTest {
    private val context=InstrumentationRegistry.getInstrumentation().targetContext
    private lateinit var root:File
    private lateinit var tasks:File
    private lateinit var data:LocalRetainedFiles
    private val reviews=mutableListOf<Pair<LocalRetainedFiles,LocalRetainedFiles.EmptyReview>>()
    @Before fun setup() {
        root=File(context.cacheDir,"empty-requests-${UUID.randomUUID()}").apply { mkdirs() }
        tasks=privateDir(File(root,"tasks"));data=LocalRetainedFiles(tasks)
    }
    @After fun cleanup() { runBlocking { reviews.forEach { (service,review)->service.dismissEmpty(review) } };root.deleteRecursively() }
    private fun privateDir(file:File)=file.apply { mkdirs();Os.chmod(path,448) }
    private fun task(vararg names:String= arrayOf("source","destination"))=privateDir(File(tasks,UUID.randomUUID().toString())).also { owner -> names.forEach { privateDir(File(owner,it)) } }
    private suspend fun review(service:LocalRetainedFiles=data)=service.prepareEmpty().also { reviews+=service to it }
    private suspend fun rejects(block:suspend()->Unit) {
        var error:Exception?=null
        try { block() } catch(e:Exception) { error=e }
        assertNotNull("stale, active or unreviewed cleanup must be rejected",error)
    }
    private fun descriptors()=File("/proc/self/fd").listFiles()!!.count { runCatching { Os.readlink(it.path).contains("/${root.name}/") }.getOrDefault(false) }
    @Test fun confirmedBatchRemovesOnlyEmptyRequestsAndNeverFiles()=runBlocking {
        val a=task();val b=task(*emptyArray());val retained=task()
        val file=File(retained,"destination/${UUID.randomUUID()}").apply { writeText("preserve");Os.chmod(path,384) }
        val original=File(root,"original").apply { writeText("original") }
        val snapshot=data.load();assertEquals(2,snapshot.emptyRequestCount);assertEquals(1,snapshot.items.size)
        val selected=review();assertTrue(a.exists());assertTrue(b.exists());assertEquals(1,descriptors())
        val result=data.clearEmpty(selected)
        assertEquals(2,result.removed);assertEquals(0,result.missing);assertTrue(result.issues.toString(),result.issues.isEmpty())
        assertFalse(a.exists());assertFalse(b.exists());assertEquals("preserve",file.readText());assertEquals("original",original.readText())
        assertEquals(0,descriptors());rejects { data.clearEmpty(selected) }
    }
    @Test fun cancellationPreservesDirectoriesAndReleasesReviewRoot()=runBlocking {
        val owner=task();data.load();val selected=review();assertEquals(1,descriptors())
        data.dismissEmpty(selected);assertEquals(0,descriptors());assertTrue(owner.isDirectory)
        rejects { data.clearEmpty(selected) }
    }
    @Test fun fileAddedAfterReviewIsNeverRemovedAndErrorIsExplicit()=runBlocking {
        val owner=task();data.load();val selected=review()
        val file=File(owner,"destination/new.txt").apply { writeText("keep new") }
        val result=data.clearEmpty(selected)
        assertEquals(0,result.removed);assertEquals(1,result.issues.size);assertEquals("keep new",file.readText())
        assertTrue(File(owner,"source").isDirectory);assertEquals(0,descriptors())
    }
    @Test fun replacedOwnerIsNotAcceptedByOldReview()=runBlocking {
        val owner=task();data.load();val selected=review();val moved=File(root,"moved")
        assertTrue(owner.renameTo(moved));privateDir(owner);privateDir(File(owner,"source"));privateDir(File(owner,"destination"))
        val result=data.clearEmpty(selected)
        assertEquals(0,result.removed);assertEquals(1,result.issues.size);assertTrue(owner.isDirectory);assertTrue(moved.isDirectory)
    }
    @Test fun replacedChildIsNotAcceptedByOldReview()=runBlocking {
        val owner=task();data.load();val selected=review();val child=File(owner,"destination")
        val moved=File(root,"moved-child");assertTrue(child.renameTo(moved));privateDir(child)
        val result=data.clearEmpty(selected)
        assertEquals(0,result.removed);assertEquals(1,result.issues.size);assertTrue(child.isDirectory);assertTrue(moved.isDirectory)
    }
    @Test fun missingRequestIsNotCountedAsRemovedByThisBatch()=runBlocking {
        val owner=task(*emptyArray());data.load();val selected=review();assertTrue(owner.delete())
        val result=data.clearEmpty(selected)
        assertEquals(0,result.removed);assertEquals(1,result.missing);assertTrue(result.issues.isEmpty());assertEquals(0,descriptors())
    }
    @Test fun replacedRootCannotRedirectBatch()=runBlocking {
        val owner=task();data.load();val selected=review();val moved=File(root,"old-tasks")
        assertTrue(tasks.renameTo(moved));privateDir(tasks)
        rejects { data.clearEmpty(selected) }
        assertTrue(File(moved,owner.name).isDirectory);assertEquals(0,descriptors())
    }
    @Test fun refreshedAndForeignReviewsCannotAuthorizeCleanup()=runBlocking {
        val owner=task();data.load();val old=review();assertEquals(1,data.load().emptyRequestCount);rejects { data.clearEmpty(old) }
        val current=review();val other=LocalRetainedFiles(tasks);other.load();val foreign=review(other)
        rejects { other.clearEmpty(current) };assertTrue(owner.isDirectory)
        data.dismissEmpty(current);other.dismissEmpty(foreign);assertEquals(0,descriptors())
    }
    @Test fun partialScanCanMaintainOnlyItsVerifiedEmptyRequests()=runBlocking {
        task();task();val limited=LocalRetainedFiles(tasks,taskLimit=1)
        val snapshot=limited.load();assertFalse(snapshot.complete);assertEquals(1,snapshot.emptyRequestCount)
        val result=limited.clearEmpty(review(limited));assertEquals(1,result.removed);assertEquals(1,tasks.list()!!.size)
        val fresh=limited.load();assertTrue(fresh.complete);assertEquals(1,fresh.emptyRequestCount)
    }
    @Test fun unknownStructureDoesNotBecomeAnEmptyRequest()=runBlocking {
        val owner=task("unexpected");val snapshot=data.load()
        assertFalse(snapshot.complete);assertEquals(0,snapshot.emptyRequestCount)
        rejects { review() };assertTrue(File(owner,"unexpected").isDirectory)
    }
    @Test fun sourceTaskLeaseBlocksMaintenanceBeforeTargetReservation()=runBlocking {
        val owner=task();val budget=LocalPublicationBudget();val service=LocalRetainedFiles(tasks,budget)
        service.load();val selected=review(service);val lease=budget.beginTask()
        try { rejects { service.clearEmpty(selected) };rejects { service.load() };assertTrue(owner.isDirectory) }
        finally { lease.close();lease.close() }
        assertEquals(1,service.load().emptyRequestCount);assertEquals(0,descriptors())
    }
    @Test fun cleanupRestoresDirectoryBudgetWithoutIgnoringScanBound()=runBlocking {
        task();val last=task();val budget=LocalPublicationBudget(directoryLimit=1)
        LocalPublicationFile.directory(tasks,"").use { scope -> LocalPublicationFile.childDirectory(scope.fd,last.name).use { owner ->
            LocalPublicationFile.childDirectory(owner.fd,"destination").use { destination ->
                var blocked=false
                try { budget.reserve(destination,UUID.randomUUID().toString(),1,scope).close() } catch(_:LocalPublicationBudget.Rejected) { blocked=true }
                assertTrue("scan bound must reject the old accumulated requests",blocked)
            }
        } }
        data.load();assertEquals(2,data.clearEmpty(review()).removed)
        val fresh=task()
        LocalPublicationFile.directory(tasks,"").use { scope -> LocalPublicationFile.childDirectory(scope.fd,fresh.name).use { owner ->
            LocalPublicationFile.childDirectory(owner.fd,"destination").use { destination ->
                budget.reserve(destination,UUID.randomUUID().toString(),1,scope).close()
            }
        } }
        assertEquals(1,tasks.list()!!.size)
    }
    @Test fun changedRequestDoesNotAbortOtherVerifiedEmptyRequests()=runBlocking {
        val changed=task();val empty=task();data.load();val selected=review()
        val file=File(changed,"destination/new.txt").apply { writeText("keep new") }
        val outcome=runCatching { data.clearEmpty(selected) }
        assertTrue("a changed row must not abort other verified rows",outcome.isSuccess)
        val result=outcome.getOrThrow()
        assertEquals(1,result.removed);assertEquals(1,result.issues.size);assertFalse(empty.exists());assertEquals("keep new",file.readText())
    }
    @Test fun allChildrenAreCheckedBeforeRemovingAnyDirectory()=runBlocking {
        val owner=task();data.load();val selected=review();val names=owner.list()!!.toList()
        File(owner,"${names.last()}/new.txt").writeText("keep")
        val result=data.clearEmpty(selected);assertEquals(1,result.issues.size)
        assertTrue("preflight must preserve every child if one has changed",names.all { File(owner,it).isDirectory })
    }
    @Test fun nativeRmdirRejectsNonemptyFilesLinksMismatchAndTraversal() {
        val a=task(*emptyArray());val b=task(*emptyArray());val file=File(a,"keep").apply { writeText("keep") }
        val link=File(tasks,"link");Os.symlink(a.path,link.path)
        LocalPublicationFile.directory(tasks,"").use { parent -> LocalPublicationFile.childDirectory(parent.fd,a.name).use { child ->
            assertEquals(OsConstants.ENOTEMPTY,LocalPublicationFile.rmdir(parent.fd,child.fd,a.name))
            assertEquals(OsConstants.EINVAL,LocalPublicationFile.rmdir(parent.fd,child.fd,b.name))
            assertEquals(OsConstants.EINVAL,LocalPublicationFile.rmdir(parent.fd,child.fd,"link"))
            assertEquals(OsConstants.EINVAL,LocalPublicationFile.rmdir(parent.fd,child.fd,".."))
            assertEquals(OsConstants.ENOENT,LocalPublicationFile.rmdir(parent.fd,child.fd,"absent"))
            assertEquals("keep",file.readText());assertTrue(b.isDirectory);assertEquals(Os.stat(tasks.path).st_ino,Os.fstat(parent.fileDescriptor).st_ino)
        } }
    }
    @Test fun nativeRmdirRemovesOnlyPinnedEmptyDirectoryAndKeepsBorrowedFds() {
        val a=task(*emptyArray())
        LocalPublicationFile.directory(tasks,"").use { parent -> LocalPublicationFile.childDirectory(parent.fd,a.name).use { child ->
            assertEquals(0,LocalPublicationFile.rmdir(parent.fd,child.fd,a.name));assertFalse(a.exists())
            assertEquals(0L,Os.fstat(child.fileDescriptor).st_nlink)
            assertEquals(Os.stat(tasks.path).st_ino,runCatching { Os.fstat(parent.fileDescriptor).st_ino }.getOrNull())
        } }
    }
    @Test fun actualExecutorRegistersBeforeSourceWorkAndReleasesAfterSuccessOrFailure()=runBlocking {
        val home=File(root,"home").apply { mkdir() };File(home,"source").mkdir();File(home,"target").mkdir()
        File(home,"source/a.txt").writeText("copy bytes")
        val browser=LocalFileBrowser(home,context.contentResolver);val origin=FileLocation(FileEndpoint.APP,"source")
        val target=FileLocation(FileEndpoint.APP,"target")
        val request=FilePasteRequest.capture(FileClipboardSnapshot.capture(origin,"source",browser.browse(origin).entries),target,"target")
        val access=FileDestinationAccess(home,context.contentResolver,browser::browse)
        val checked=FilePastePreflight(access::inspect,access::relation).check(FileTreeScanner(browser::browse).scan(request))
        val decisions=FilePasteDecisions.begin(checked);val plan=decisions.confirm(decisions.state().reference)
        for(fail in listOf(false,true)) {
            var protected=false
            val executor=FileCopyExecutor(home,context.contentResolver,tasks,{ intent ->
                protected=runCatching { runBlocking { data.load() } }.isFailure
                if(fail)error("controlled connection failure")
                FileTaskConnections(intent.source.endpoint,intent.destination.endpoint,{ _,_->error("local only") },{ _,_,_->error("no auth") })
            })
            val running=executor.prepare(plan);val result=withTimeout(15000) { running.execute() };running.awaitClosed()
            assertTrue("executor must protect the source phase",protected)
            assertTrue("executor must release its maintenance lease",runCatching { data.load() }.isSuccess)
            assertEquals(if(fail)FileCopyState.FAILED else FileCopyState.COMPLETE,result.state)
        }
        assertEquals("copy bytes",File(home,"target/a.txt").readText())
    }
    @Test fun cancellationAtReturnBoundaryReleasesRootViaHandoff()=runBlocking {
        val owner=task();data.load()
        val known=java.util.concurrent.atomic.AtomicReference<LocalRetainedFiles.EmptyReview?>()
        val delivered=java.util.concurrent.atomic.AtomicBoolean(false);val lifetime=Job()
        val worker=async(lifetime+Dispatchers.Default) {
            try {
                data.prepareEmpty { known.set(it);lifetime.cancel() }
                delivered.set(true)
            } finally { known.get()?.let { data.dismissEmpty(it) } }
        }
        worker.join()
        assertTrue("controlled boundary cancellation must reach the caller",worker.isCancelled)
        assertNotNull("the token must be handed off before the cancellable return",known.get())
        assertFalse("the return value must be dropped on cancellation",delivered.get())
        assertEquals(0,descriptors());assertTrue(owner.isDirectory)
    }
    @Test fun lateOldReviewReleaseCannotCloseNewReview()=runBlocking {
        val owner=task();data.load();val old=review();val current=review()
        data.dismissEmpty(old);assertEquals(1,descriptors());assertTrue(owner.isDirectory)
        assertEquals(1,data.clearEmpty(current).removed);assertFalse(owner.exists());assertEquals(0,descriptors())
    }

}
