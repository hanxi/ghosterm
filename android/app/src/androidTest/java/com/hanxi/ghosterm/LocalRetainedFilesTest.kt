package com.hanxi.ghosterm

import android.system.Os
import android.system.OsConstants
import androidx.test.platform.app.InstrumentationRegistry
import com.hanxi.ghosterm.bridge.*
import com.hanxi.ghosterm.data.*
import kotlinx.coroutines.runBlocking
import org.junit.*
import org.junit.Assert.*
import java.io.File
import java.util.UUID

class LocalRetainedFilesTest {
    private val context=InstrumentationRegistry.getInstrumentation().targetContext
    private lateinit var root:File
    private lateinit var tasks:File
    private lateinit var owner:File
    private lateinit var destination:File
    private lateinit var data:LocalRetainedFiles
    @Before fun setup() {
        root=File(context.cacheDir,"retained-${UUID.randomUUID()}").apply { mkdirs() }
        tasks=privateDir(File(root,"tasks"));owner=privateDir(File(tasks,UUID.randomUUID().toString()))
        destination=privateDir(File(owner,"destination"));data=LocalRetainedFiles(tasks)
    }
    @After fun cleanup() { root.deleteRecursively() }
    private fun privateDir(path:File)=path.apply { mkdirs();Os.chmod(this.path,448) }
    private fun file(value:ByteArray="temporary".toByteArray())=File(destination,UUID.randomUUID().toString()).apply { writeBytes(value);Os.chmod(path,384) }
    private fun fds()=File("/proc/self/fd").listFiles()!!.count { runCatching { Os.readlink(it.path).contains("/${root.name}/") }.getOrDefault(false) }
    private suspend fun rejects(block:suspend()->Unit) {
        var failure:Exception?=null
        try { block() } catch(error:Exception) { failure=error }
        assertNotNull("must reject stale or unsafe maintenance",failure)
    }
    @Test fun freshReviewAndConfirmationClearOnlySelectedFile()=runBlocking {
        val a=file("first".toByteArray());val b=file("keep".toByteArray());val external=File(root,"original").apply { writeText("original") }
        val loaded=data.load();assertTrue(loaded.complete);assertEquals(2,loaded.items.size)
        val selected=loaded.items.single { it.referencePath==a.path }
        val review=data.prepare(selected.ref);assertEquals("first",review.preview);assertFalse(review.truncated)
        assertTrue(a.exists());assertEquals(LocalRetainedFiles.Outcome.CLEARED,data.clear(selected.ref))
        assertFalse(a.exists());assertEquals("keep",b.readText());assertEquals("original",external.readText())
        rejects { data.clear(selected.ref) };assertEquals(0,fds())
    }
    @Test fun dismissReleasesAllDescriptorsAndPreservesContents()=runBlocking {
        val a=file();val selected=data.load().items.single();data.prepare(selected.ref)
        assertTrue(fds()>=4);data.dismiss(selected.ref);assertEquals(0,fds());assertEquals("temporary",a.readText())
        rejects { data.clear(selected.ref) }
    }
    @Test fun replacedCandidateIsPreservedAndOriginalIsNotUnlinked()=runBlocking {
        val a=file();val selected=data.load().items.single();data.prepare(selected.ref)
        val moved=File(destination,UUID.randomUUID().toString());assertTrue(a.renameTo(moved))
        a.writeText("replacement");Os.chmod(a.path,384)
        rejects { data.clear(selected.ref) }
        assertEquals("replacement",a.readText());assertEquals("temporary",moved.readText());assertEquals(0,fds())
    }
    @Test fun modifiedFileCannotUseOldConfirmation()=runBlocking {
        val a=file();val selected=data.load().items.single();data.prepare(selected.ref)
        a.appendText(" changed")
        rejects { data.clear(selected.ref) };assertEquals("temporary changed",a.readText());assertEquals(0,fds())
    }
    @Test fun changedFileBetweenListingAndReviewIsRejected()=runBlocking {
        val a=file();val selected=data.load().items.single();a.appendText(" changed")
        rejects { data.prepare(selected.ref) };assertTrue(a.isFile);assertEquals(0,fds())
    }
    @Test fun staleEpochAndForeignOwnerAreRejectedBeforeOpening()=runBlocking {
        file();val old=data.load().items.single();val fresh=data.load().items.single()
        rejects { data.prepare(old.ref) }
        rejects { LocalRetainedFiles(tasks).prepare(fresh.ref) }
        rejects { data.clear(fresh.ref) };assertEquals(0,fds())
    }
    @Test fun parentDirectoryReplacementCannotRedirectCleanup()=runBlocking {
        val a=file();val selected=data.load().items.single();data.prepare(selected.ref)
        val moved=File(owner,"moved");assertTrue(destination.renameTo(moved));privateDir(destination)
        val replacement=File(destination,a.name).apply { writeText("keep");Os.chmod(path,384) }
        rejects { data.clear(selected.ref) }
        assertEquals("keep",replacement.readText());assertEquals("temporary",File(moved,a.name).readText());assertEquals(0,fds())
    }
    @Test fun rootReplacementCannotRedirectCleanup()=runBlocking {
        val a=file();val selected=data.load().items.single();data.prepare(selected.ref)
        val moved=File(root,"moved");assertTrue(tasks.renameTo(moved));privateDir(tasks)
        rejects { data.clear(selected.ref) }
        assertTrue(File(moved,"${owner.name}/destination/${a.name}").isFile);assertEquals(0,fds())
    }
    @Test fun activeReservationBlocksMaintenanceAndIsNeverDeleted()=runBlocking {
        val a=file("data".toByteArray());val budget=LocalPublicationBudget(16,4,0);val service=LocalRetainedFiles(tasks,budget)
        val selected=service.load().items.single();service.prepare(selected.ref)
        LocalPublicationFile.directory(tasks,"").use { scope -> LocalPublicationFile.directory(destination,"").use { dir ->
            val active=budget.reserve(dir,UUID.randomUUID().toString(),4,scope)
            try { rejects { service.clear(selected.ref) };assertEquals("data",a.readText()) }
            finally { active.close() }
        } }
        val refreshed=service.load().items.single();service.prepare(refreshed.ref);service.dismiss(refreshed.ref);assertTrue(a.exists())
    }
    @Test fun missingCurrentPositionIsNotReportedAsDeletedByThisOperation()=runBlocking {
        val a=file();val selected=data.load().items.single();data.prepare(selected.ref);assertTrue(a.delete())
        assertEquals(LocalRetainedFiles.Outcome.ABSENT,data.clear(selected.ref));assertEquals(0,fds())
    }
    @Test fun boundedListingRetainsVerifiedRowsAndReportsIncomplete()=runBlocking {
        file();file();val service=LocalRetainedFiles(tasks,entryLimit=1)
        val snapshot=service.load();assertFalse(snapshot.complete);assertNotNull(snapshot.error);assertEquals(1,snapshot.items.size)
        val review=service.prepare(snapshot.items.single().ref);assertEquals("temporary",review.preview);service.dismiss(review.item.ref)
        assertEquals(2,destination.list()!!.size)
    }
    @Test fun binaryAndTruncatedTextReviewsRemainReadonly()=runBlocking {
        val binary=file(byteArrayOf(0xff.toByte(),0xfe.toByte()));val text=file(ByteArray(6000){65})
        val snapshot=data.load()
        val binaryReview=data.prepare(snapshot.items.single { it.referencePath==binary.path }.ref)
        assertNull(binaryReview.preview);data.dismiss(binaryReview.item.ref)
        val textReview=data.prepare(snapshot.items.single { it.referencePath==text.path }.ref)
        assertEquals("A".repeat(4096),textReview.preview);assertTrue(textReview.truncated);data.dismiss(textReview.item.ref)
        assertEquals(6000,text.readBytes().size);assertEquals(0,fds())
    }
    @Test fun missingRootIsEmptyButUnsafeRootIsExplicitFailure()=runBlocking {
        val absent=LocalRetainedFiles(File(root,"absent")).load();assertTrue(absent.complete);assertTrue(absent.items.isEmpty())
        val link=File(root,"link");Os.symlink(tasks.path,link.path)
        rejects { LocalRetainedFiles(link).load() }
    }
    @Test fun nativeRetainedReaderIsReadonlyCloexecAndNeverFollowsSymlinks() {
        val a=file();val link=File(destination,UUID.randomUUID().toString());Os.symlink(a.path,link.path)
        LocalPublicationFile.directory(destination,"").use { dir ->
            LocalPublicationFile.retainedCandidate(dir.fd,a.name).use { fd ->
                assertEquals(OsConstants.O_RDONLY,FileDescriptorFlags.status(fd.fileDescriptor) and OsConstants.O_ACCMODE)
                assertEquals(OsConstants.FD_CLOEXEC,FileDescriptorFlags.descriptorFlags(fd.fd) and OsConstants.FD_CLOEXEC)
            }
            var error:Exception?=null
            try { LocalPublicationFile.retainedCandidate(dir.fd,link.name).close() } catch(e:Exception) { error=e }
            assertNotNull(error)
            val directoryName=UUID.randomUUID().toString();privateDir(File(destination,directoryName))
            var directoryError:Exception?=null
            try { LocalPublicationFile.retainedCandidate(dir.fd,directoryName).close() } catch(e:Exception) { directoryError=e }
            assertNotNull("retained reader must reject directories",directoryError)
            assertEquals("borrowed parent must remain open",Os.stat(destination.path).st_ino,
                runCatching { Os.fstat(dir.fileDescriptor).st_ino }.getOrNull())
        }
    }
}
