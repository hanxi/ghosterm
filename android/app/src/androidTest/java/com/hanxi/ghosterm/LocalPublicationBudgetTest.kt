package com.hanxi.ghosterm

import android.os.ParcelFileDescriptor
import android.system.Os
import android.system.OsConstants
import androidx.test.platform.app.InstrumentationRegistry
import com.hanxi.ghosterm.bridge.LocalPublicationFile
import com.hanxi.ghosterm.data.*
import com.hanxi.ghosterm.domain.*
import kotlinx.coroutines.runBlocking
import org.junit.*
import org.junit.Assert.*
import java.io.File
import java.io.IOException
import java.io.RandomAccessFile
import com.hanxi.ghosterm.bridge.FileDescriptorFlags
import java.util.UUID

class LocalPublicationBudgetTest {
    private val context=InstrumentationRegistry.getInstrumentation().targetContext
    private lateinit var root:File
    private lateinit var home:File
    private lateinit var stage:File
    private lateinit var snapshots:TransferStagingStore
    @Before fun setup() {
        root=File(context.cacheDir,"publication-budget-${UUID.randomUUID()}").apply { mkdirs() }
        home=File(root,"home").apply { mkdir() };stage=File(root,"stage").apply { mkdir() };Os.chmod(stage.path,448)
        snapshots=TransferStagingStore(File(root,"source-stage"),1024,2048,0)
    }
    @After fun cleanup() { snapshots.close();root.deleteRecursively() }
    private fun retained(bytes: Int):File=File(stage,UUID.randomUUID().toString()).apply { writeBytes(ByteArray(bytes){65});Os.chmod(path,384) }
    private fun rejected(block:()->Unit):LocalPublicationBudget.Rejected {
        try { block() } catch(error: LocalPublicationBudget.Rejected) { return error }
        fail("Expected an explicit capacity rejection");error("unreachable")
    }
    private suspend fun snapshot():TransferStagingStore.Snapshot {
        File(home,"source").writeText("copy")
        return LocalFileBrowser(home,context.contentResolver).snapshot(FileLocation(FileEndpoint.APP),BrowserEntry("source","source",FileKind.FILE,4u,null),snapshots)
    }
    @Test fun interruptedCandidateBytesAreCountedAndNeverDeleted()=runBlocking {
        val old=retained(8)
        snapshot().use { source ->
            var creates=0
            val result=LocalFilePublisher(home,stage,beforeCreate={ creates++ },budget=LocalPublicationBudget(11,2,0)).publish("","target",source)
            assertEquals(LocalFilePublisher.State.REJECTED,result.state);assertEquals(OsConstants.ENOSPC,result.error)
            assertTrue(result.rejectionReason.orEmpty().contains("容量不足"));assertNull(result.cleanupError)
            assertEquals(0,creates);assertFalse(File(home,"target").exists())
        }
        assertArrayEquals(ByteArray(8){65},old.readBytes());assertEquals(listOf(old.name),stage.list()!!.toList())
    }
    @Test fun emptyRetainedCandidatesConsumeCountBudget() {
        val old=retained(0)
        LocalPublicationFile.directory(stage,"").use { directory ->
            val error=rejected { LocalPublicationBudget(16,1,0).reserve(directory,UUID.randomUUID().toString(),0).close() }
            assertTrue(error.message.orEmpty().contains("数量"));assertTrue(old.isFile)
        }
    }
    @Test fun inflightReservationCountsFullExpectedSizeBeforeFileGrows() {
        val budget=LocalPublicationBudget(7,2,0)
        LocalPublicationFile.directory(stage,"").use { directory ->
            val name=UUID.randomUUID().toString();val first=budget.reserve(directory,name,4)
            try {
                LocalPublicationFile.candidate(directory.fd,name).use { assertEquals(0L,Os.fstat(it.fileDescriptor).st_size) }
                rejected { budget.reserve(directory,UUID.randomUUID().toString(),4).close() }
            } finally { File(stage,name).delete();first.close();first.close() }
            budget.reserve(directory,UUID.randomUUID().toString(),4).close()
        }
    }
    @Test fun inflightZeroByteReservationConsumesSlotAndReleaseIsIdempotent() {
        val budget=LocalPublicationBudget(8,1,0)
        LocalPublicationFile.directory(stage,"").use { directory ->
            val first=budget.reserve(directory,UUID.randomUUID().toString(),0)
            try { rejected { budget.reserve(directory,UUID.randomUUID().toString(),0).close() } }
            finally { first.close();first.close() }
            budget.reserve(directory,UUID.randomUUID().toString(),0).close()
        }
    }
    @Test fun activeNamedCandidateIsNotChargedAgainAsAnOldResidue() {
        val budget=LocalPublicationBudget(8,2,0)
        LocalPublicationFile.directory(stage,"").use { directory ->
            val name=UUID.randomUUID().toString();val first=budget.reserve(directory,name,4)
            try {
                LocalPublicationFile.candidate(directory.fd,name).use { Os.write(it.fileDescriptor,byteArrayOf(1,2,3,4),0,4) }
                val failure=try { budget.reserve(directory,UUID.randomUUID().toString(),4).close();null } catch(error:Exception) { error }
                assertNull("active candidate must not be charged twice",failure)
            } finally { File(stage,name).delete();first.close() }
        }
    }
    @Test fun freeSpaceReserveIsNotBorrowed()=runBlocking {
        snapshot().use { source ->
            var creates=0
            val result=LocalFilePublisher(home,stage,beforeCreate={ creates++ },budget=LocalPublicationBudget(8,2,Long.MAX_VALUE)).publish("","target",source)
            assertEquals(LocalFilePublisher.State.REJECTED,result.state);assertTrue(result.rejectionReason.orEmpty().contains("可用空间不足"))
            assertEquals(0,creates);assertTrue(stage.list()!!.isEmpty());assertFalse(File(home,"target").exists())
        }
    }
    @Test fun failureReleasesReservationBeforeNextPublication()=runBlocking {
        val budget=LocalPublicationBudget(4,1,0)
        snapshot().use { source ->
            var failed=false
            try { LocalFilePublisher(home,stage,beforePublish={ throw IOException("stop before publish") },budget=budget).publish("","failed",source) }
            catch(_:IOException) { failed=true }
            assertTrue(failed);assertTrue(stage.list()!!.isEmpty())
            assertEquals(LocalFilePublisher.State.PUBLISHED,LocalFilePublisher(home,stage,budget=budget).publish("","target",source).state)
            assertEquals("copy",File(home,"target").readText())
        }
    }
    @Test fun unsafeNamespaceEntriesAreRejectedWithoutDeletion() {
        val old=File(stage,"unrelated").apply { writeText("keep") }
        LocalPublicationFile.directory(stage,"").use { directory ->
            var failure:Exception?=null
            try { LocalPublicationBudget(32,2,0).reserve(directory,UUID.randomUUID().toString(),1).close() } catch(error:Exception) { failure=error }
            assertTrue(failure is IOException);assertEquals("keep",old.readText())
            old.delete();val linked=File(stage,UUID.randomUUID().toString());Os.symlink(File(home,"outside").path,linked.path)
            failure=null;try { LocalPublicationBudget(32,2,0).reserve(directory,UUID.randomUUID().toString(),1).close() } catch(error:Exception) { failure=error }
            assertTrue(failure is IOException);assertEquals(File(home,"outside").path,Os.readlink(linked.path))
        }
    }
    private fun privateDirectory(path: File)=path.apply { mkdirs();Os.chmod(this.path,448) }
    @Test fun globalScopeCountsOlderRequestDirectoriesWithoutDeletingTheirFiles()=runBlocking {
        val scope=privateDirectory(File(root,"tasks"))
        val older=privateDirectory(File(scope,UUID.randomUUID().toString()+"/destination"))
        val old=File(older,UUID.randomUUID().toString()).apply { writeBytes(ByteArray(8){65});Os.chmod(path,384) }
        val current=privateDirectory(File(scope,UUID.randomUUID().toString()+"/destination"))
        snapshot().use { source ->
            var creates=0
            val result=LocalFilePublisher(home,current,beforeCreate={ creates++ },budget=LocalPublicationBudget(11,2,0),budgetRoot=scope).publish("","target",source)
            assertEquals(LocalFilePublisher.State.REJECTED,result.state);assertEquals(0,creates);assertEquals(0L,result.bytesWritten)
            assertEquals(ByteArray(8){65}.toList(),old.readBytes().toList());assertTrue(current.list()!!.isEmpty())
        }
    }
    @Test fun globalScopeRejectsSymlinkedRequestInsteadOfFollowingIt() {
        val scope=privateDirectory(File(root,"tasks"))
        val owner=privateDirectory(File(root,"outside"));val destination=privateDirectory(File(owner,"destination"))
        Os.symlink(owner.path,File(scope,UUID.randomUUID().toString()).path)
        LocalPublicationFile.directory(scope,"").use { rootFd -> LocalPublicationFile.directory(destination,"").use { fd ->
            var failure:Exception?=null
            try { LocalPublicationBudget(8,2,0).reserve(fd,UUID.randomUUID().toString(),4,rootFd).close() } catch(error:Exception) { failure=error }
            assertNotNull(failure);assertTrue(destination.list()!!.isEmpty())
        } }
    }
    @Test fun requestDirectoryScanIsBoundedWithoutDeletingEntries() {
        val scope=privateDirectory(File(root,"tasks"));val dirs=(1..3).map { privateDirectory(File(scope,UUID.randomUUID().toString()+"/destination")) }
        LocalPublicationFile.directory(scope,"").use { rootFd -> LocalPublicationFile.directory(dirs.first(),"").use { fd ->
            val failure=rejected { LocalPublicationBudget(8,2,0,2).reserve(fd,UUID.randomUUID().toString(),4,rootFd).close() }
            assertTrue(failure.message.orEmpty().contains("目录"));assertEquals(3,scope.list()!!.size)
        } }
    }
    @Test fun childDirectoryUsesCloexecAndRejectsTraversal() {
        val name=UUID.randomUUID().toString();privateDirectory(File(stage,name))
        LocalPublicationFile.directory(stage,"").use { fd ->
            LocalPublicationFile.childDirectory(fd.fd,name).use { child ->
                assertEquals(OsConstants.FD_CLOEXEC,FileDescriptorFlags.descriptorFlags(child.fd) and OsConstants.FD_CLOEXEC)
                assertEquals(Os.stat(File(stage,name).path).st_ino,Os.fstat(child.fileDescriptor).st_ino)
            }
            val parent=try { Os.fstat(fd.fileDescriptor) } catch(_:Exception) { null }
            assertNotNull("opening a child must not close the borrowed parent",parent)
            assertEquals(Os.stat(stage.path).st_ino,parent!!.st_ino)
            var failed=false
            try { LocalPublicationFile.childDirectory(fd.fd,"..").close() } catch(_:IOException) { failed=true }
            assertTrue(failed)
        }
    }
    @Test fun scopeMembershipAndPrivatePermissionsAreRequired() {
        val scope=privateDirectory(File(root,"tasks"))
        LocalPublicationFile.directory(scope,"").use { rootFd -> LocalPublicationFile.directory(stage,"").use { fd ->
            var failure:Exception?=null
            try { LocalPublicationBudget(8,2,0).reserve(fd,UUID.randomUUID().toString(),4,rootFd).close() } catch(error:Exception) { failure=error }
            assertNotNull("unrelated destination must not use this root's budget",failure)
            Os.chmod(stage.path,511);failure=null
            try { LocalPublicationBudget(8,2,0).reserve(fd,UUID.randomUUID().toString(),4).close() } catch(error:Exception) { failure=error }
            assertNotNull("world writable staging is not private",failure)
        } }
    }
    @Test fun unknownRequestStructureIsNotSilentlyExcludedFromBudget() {
        val scope=privateDirectory(File(root,"tasks"));val owner=privateDirectory(File(scope,UUID.randomUUID().toString()))
        val destination=privateDirectory(File(owner,"destination"));val unknown=File(owner,"unrelated").apply { writeText("keep") }
        LocalPublicationFile.directory(scope,"").use { rootFd -> LocalPublicationFile.directory(destination,"").use { fd ->
            var failure:Exception?=null
            try { LocalPublicationBudget(8,2,0).reserve(fd,UUID.randomUUID().toString(),4,rootFd).close() } catch(error:Exception) { failure=error }
            assertNotNull(failure);assertEquals("keep",unknown.readText())
        } }
    }
    @Test fun enumerationUsesPinnedDirectoryInsteadOfReplacementPath() {
        LocalPublicationFile.directory(stage,"").use { directory ->
            val moved=File(root,"moved");assertTrue(stage.renameTo(moved));assertTrue(stage.mkdir())
            val replacement=File(stage,"unrelated").apply { writeText("leave this alone") }
            LocalPublicationBudget(8,1,0).reserve(directory,UUID.randomUUID().toString(),4).close()
            assertEquals("leave this alone",replacement.readText());assertTrue(moved.list()!!.isEmpty())
        }
    }
}
