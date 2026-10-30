package com.hanxi.ghosterm

import android.content.res.AssetFileDescriptor
import android.os.ParcelFileDescriptor
import android.os.SystemClock
import android.system.Os
import android.system.OsConstants
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.hanxi.ghosterm.bridge.AnonymousStagingFile
import com.hanxi.ghosterm.data.LocalFileBrowser
import com.hanxi.ghosterm.data.TransferStagingStore
import com.hanxi.ghosterm.domain.*
import kotlinx.coroutines.*
import org.junit.After
import org.junit.Assert.*
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File
import java.io.FileInputStream
import java.util.UUID
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicReference
import kotlin.coroutines.CoroutineContext
import java.util.concurrent.LinkedBlockingQueue

@RunWith(AndroidJUnit4::class)
class TransferStagingTest {
    private val context=InstrumentationRegistry.getInstrumentation().targetContext
    private lateinit var root:File
    private lateinit var home:File
    private lateinit var staging:File
    private lateinit var store:TransferStagingStore
    private lateinit var local:LocalFileBrowser
    @Before fun setup() {
        root=File(context.cacheDir,"staging-${UUID.randomUUID()}").apply { mkdir() }
        home=File(root,"home").apply { mkdir() };staging=File(root,"private-staging")
        store=TransferStagingStore(staging,131072,262144,0)
        local=LocalFileBrowser(home,context.contentResolver)
    }
    @After fun cleanup() { store.close();root.deleteRecursively() }
    private fun location()=FileLocation(FileEndpoint.APP)
    private fun entry(file:File)=BrowserEntry(file.name,file.name,FileKind.FILE,file.length().toULong(),null)
    private fun open(file:File,offset:Long=0,length:Long=-1)=AssetFileDescriptor(ParcelFileDescriptor.open(file,ParcelFileDescriptor.MODE_READ_ONLY),offset,length)
    private suspend fun content(snapshot:TransferStagingStore.Snapshot):ByteArray=snapshot.withDescriptor { fd,_ ->
        ParcelFileDescriptor.fromFd(fd).use { copy -> FileInputStream(copy.fileDescriptor).readBytes() }
    }
    private suspend fun rejects(block:suspend ()->Unit) {
        var failure:Exception?=null
        try { block() } catch(e:CancellationException) { throw e } catch(e:Exception) { failure=e }
        assertNotNull("operation must explicitly fail",failure)
    }
    private fun openPaths()=File("/proc/self/fd").listFiles()!!.mapNotNull {
        try { Os.readlink(it.path).takeIf { path -> path.contains("/${root.name}/") } } catch(_:Exception) { null }
    }
    private suspend fun waitFor(test:()->Boolean) {
        withTimeout(10000) { while(!test()) delay(10) }
    }

    @Test fun localSnapshotIsAnonymousReadOnlyStableAndHasIndependentReaders() = runBlocking {
        val original=ByteArray(100003) { (it%251).toByte() };val file=File(home,"源.bin").apply { writeBytes(original) }
        val before=openPaths().size
        val snapshot=local.snapshot(location(),entry(file),store)
        try {
            assertEquals(100003L,snapshot.size);assertEquals(100003L,store.reservedBytes)
            assertEquals(emptyList<String>(),staging.list()!!.toList())
            assertEquals(448,Os.stat(staging.path).st_mode and 511)
            assertEquals(before+1,openPaths().size)
            file.writeText("changed after snapshot")
            assertArrayEquals(original,content(snapshot))
            assertArrayEquals(original,content(snapshot))
            snapshot.withDescriptor { fd,_ ->
                ParcelFileDescriptor.fromFd(fd).use { borrowed ->
                    assertEquals(OsConstants.O_RDONLY,Os.fcntlInt(borrowed.fileDescriptor,OsConstants.F_GETFL,0) and OsConstants.O_ACCMODE)
                    assertEquals(OsConstants.FD_CLOEXEC,Os.fcntlInt(borrowed.fileDescriptor,OsConstants.F_GETFD,0) and OsConstants.FD_CLOEXEC)
                    assertEquals(384,Os.fstat(borrowed.fileDescriptor).st_mode and 511)
                    rejects { Os.write(borrowed.fileDescriptor,byteArrayOf(1),0,1) }
                    Os.lseek(borrowed.fileDescriptor,19,OsConstants.SEEK_SET)
                    assertArrayEquals(original,content(snapshot))
                    assertEquals(19L,Os.lseek(borrowed.fileDescriptor,0,OsConstants.SEEK_CUR))
                }
            }
            assertEquals(setOf("源.bin"),local.list(location()).map { it.name }.toSet())
        } finally { snapshot.close() }
        assertEquals(0L,store.reservedBytes);assertEquals(before,openPaths().size)
        rejects { snapshot.withDescriptor { _,_ -> fail("disposed snapshot reused") } }
        assertEquals("changed after snapshot",file.readText())
    }

    @Test fun capacityIncludesLiveReadersAndFailureReleasesReservations() = runBlocking {
        store.close();store=TransferStagingStore(staging,8,8,0)
        val file=File(home,"bytes").apply { writeText("12345678") }
        val a=local.snapshot(location(),entry(file),store)
        a.withDescriptor { _,_ ->
            a.close();assertEquals(8L,store.reservedBytes)
            rejects { local.snapshot(location(),entry(file),store) }
        }
        assertEquals(0L,store.reservedBytes)
        local.snapshot(location(),entry(file),store).close()
        assertEquals(0L,store.reservedBytes)
        rejects { store.snapshot(8) { error("provider open failed") } }
        assertEquals(0L,store.reservedBytes)
        val empty=File(home,"empty").apply { writeBytes(byteArrayOf()) }
        local.snapshot(location(),entry(empty),store).use { assertEquals(0L,it.size);assertArrayEquals(byteArrayOf(),content(it)) }
    }

    @Test fun exactLimitUnknownLengthAndAssetSliceDoNotTruncateSilently() = runBlocking {
        store.close();store=TransferStagingStore(staging,8,16,0)
        val file=File(home,"bytes").apply { writeText("12345678") }
        store.snapshot { open(file) }.use { assertEquals(8L,it.size);assertEquals("12345678",content(it).toString(Charsets.UTF_8)) }
        file.appendText("9")
        rejects { store.snapshot { open(file) } };assertEquals(0L,store.reservedBytes)
        store.snapshot { open(file,2,3) }.use { assertEquals("345",content(it).toString(Charsets.UTF_8)) }
        rejects { store.snapshot { open(file,8,3) } }
        rejects { store.snapshot(5) { open(file,2,3) } }
        rejects { store.snapshot(8) { open(file) } }
        assertEquals(0L,store.reservedBytes);assertTrue(openPaths().isEmpty())
    }

    @Test fun cancelledPipeClosesOwnerAndDoesNotPublishSnapshot() = runBlocking {
        val pipes=ParcelFileDescriptor.createReliablePipe();val handed=CountDownLatch(1)
        val failure=AtomicReference<Throwable?>()
        val writer=Thread {
            try { handed.await(5,TimeUnit.SECONDS);repeat(500) { Os.write(pipes[1].fileDescriptor,byteArrayOf(65),0,1);Thread.sleep(10) } }
            catch(e:Throwable) { failure.set(e) } finally { pipes[1].close() }
        }.apply { start() }
        val job=launch {
            store.snapshot { handed.countDown();AssetFileDescriptor(pipes[0],0,-1) }.use { fail("cancelled stream published") }
        }
        try {
            waitFor { handed.count==0L && openPaths().any { it.contains("(deleted)") } }
            val start=SystemClock.uptimeMillis();repeat(20) { delay(10) };val calibrated=SystemClock.uptimeMillis()-start
            val cancelledAt=SystemClock.uptimeMillis();job.cancelAndJoin()
            waitFor { store.reservedBytes==0L }
            assertTrue("cancel must release before the natural stream end",SystemClock.uptimeMillis()-cancelledAt < maxOf(2000L,calibrated*3/2+500))
            withContext(Dispatchers.IO) { writer.join(2000) }
            assertFalse(writer.isAlive);assertNotNull("EPIPE proves reader was closed",failure.get())
            assertFalse(pipes[0].fileDescriptor.valid());assertTrue(openPaths().isEmpty())
        } finally { job.cancelAndJoin();pipes.forEach { it.close() };writer.interrupt() }
    }

    @Test fun reliablePipeErrorIsNotACompleteSnapshotAndStoreCloseKeepsBorrowedContent() = runBlocking {
        val pipes=ParcelFileDescriptor.createReliablePipe()
        Os.write(pipes[1].fileDescriptor,"bad".toByteArray(),0,3);pipes[1].closeWithError("source read failed")
        rejects { store.snapshot { AssetFileDescriptor(pipes[0],0,-1) } }
        assertEquals(0L,store.reservedBytes);assertFalse(pipes[0].fileDescriptor.valid())
        val file=File(home,"bytes").apply { writeText("held") }
        val snapshot=local.snapshot(location(),entry(file),store)
        snapshot.withDescriptor { fd,_ ->
            store.close();assertEquals(4L,store.reservedBytes)
            ParcelFileDescriptor.fromFd(fd).use { assertEquals("held",FileInputStream(it.fileDescriptor).readBytes().toString(Charsets.UTF_8)) }
            rejects { snapshot.withDescriptor { _,_ -> fail("closed store reused") } }
        }
        assertEquals(0L,store.reservedBytes);assertTrue(openPaths().isEmpty())
        rejects { store.snapshot { open(file) } }
    }

    @Test fun sourceLinksStaleSizeWrongParentAndNoSpaceFailBeforePublication() = runBlocking {
        val file=File(home,"bytes").apply { writeText("1234") };val selected=entry(file)
        file.appendText("5");rejects { local.snapshot(location(),selected,store) }
        rejects { local.snapshot(location(),selected.copy(size=8uL),store) }
        Os.symlink(file.path,File(home,"link").path)
        rejects { local.snapshot(location(),selected.copy(id="link"),store) }
        rejects { local.snapshot(location(),selected.copy(id="../home/bytes"),store) }
        rejects { local.snapshot(location(),selected.copy(kind=FileKind.LINK),store) }
        Os.mkfifo(File(home,"pipe").path,384)
        rejects { local.snapshot(location(),selected.copy(id="pipe"),store) }
        val before=file.readBytes();assertEquals(0L,store.reservedBytes);assertTrue(openPaths().isEmpty())
        TransferStagingStore(File(root,"no-space"),8,8,Long.MAX_VALUE).use { full ->
            rejects { full.snapshot(5) { open(file) } };assertEquals(0L,full.reservedBytes)
        }
        val outside=File(root,"outside").apply { mkdir() };File(outside,"keep").writeText("keep")
        val link=File(root,"store-link");Os.symlink(outside.path,link.path)
        TransferStagingStore(link,8,8,0).use { unsafe -> rejects { unsafe.snapshot(5) { open(file) } } }
        assertEquals(listOf("keep"),outside.list()!!.toList());assertArrayEquals(before,file.readBytes())
    }
    @Test fun cancellationAfterCopyBeforeDeliveryClosesTheUnclaimedSnapshot() = runBlocking {
        val queue=LinkedBlockingQueue<Runnable>()
        val dispatcher=object:CoroutineDispatcher() { override fun dispatch(context:CoroutineContext,block:Runnable) { queue.add(block) } }
        val file=File(home,"ready").apply { writeText("ready") }
        val release=CountDownLatch(1)
        val job=launch(dispatcher,start=CoroutineStart.UNDISPATCHED) {
            store.snapshot(5) {
                check(release.await(5,TimeUnit.SECONDS)) { "caller did not release the source" }
                open(file)
            }.use { fail("cancelled delivery published") }
        }
        // The caller must suspend before IO completes. Otherwise a fast copy can
        // return inline from suspendCancellableCoroutine, before cancellation exists.
        release.countDown()
        val delivery=withContext(Dispatchers.IO) { queue.poll(5,TimeUnit.SECONDS) }
        try {
            assertNotNull("copy must finish and queue result before cancellation",delivery)
            assertEquals(5L,store.reservedBytes)
        } finally { job.cancel();delivery?.run();job.join() }
        assertEquals(0L,store.reservedBytes);assertTrue(openPaths().isEmpty())
    }

    @Test fun nativeAllocationHasNoNameAndRejectsSymlinkDirectory() = runBlocking {
        staging.mkdir()
        AnonymousStagingFile.create(staging).use { fd ->
            assertEquals(0L,Os.fstat(fd.fileDescriptor).st_nlink)
            assertEquals(384,Os.fstat(fd.fileDescriptor).st_mode and 511)
            assertEquals(OsConstants.FD_CLOEXEC,Os.fcntlInt(fd.fileDescriptor,OsConstants.F_GETFD,0) and OsConstants.FD_CLOEXEC)
            Os.write(fd.fileDescriptor,byteArrayOf(1,2,3),0,3)
            assertEquals(3L,Os.fstat(fd.fileDescriptor).st_size)
            assertTrue(staging.list()!!.isEmpty())
        }
        val link=File(root,"native-link");Os.symlink(staging.path,link.path)
        rejects { AnonymousStagingFile.create(link).close() }
        assertTrue(staging.list()!!.isEmpty());assertTrue(openPaths().isEmpty())
    }

    @Test fun emptySnapshotsStillConsumeSlotsUntilLastReaderReleases() = runBlocking {
        store.close();store=TransferStagingStore(staging,8,8,0,2)
        val file=File(home,"empty").apply { writeBytes(byteArrayOf()) }
        val first=local.snapshot(location(),entry(file),store)
        val second=local.snapshot(location(),entry(file),store)
        try {
            assertEquals(0L,store.reservedBytes)
            rejects { local.snapshot(location(),entry(file),store) }
            first.withDescriptor { _,_ ->
                first.close();rejects { local.snapshot(location(),entry(file),store) }
            }
            local.snapshot(location(),entry(file),store).close()
        } finally { first.close();second.close() }
        assertTrue(openPaths().isEmpty())
    }

    @Test fun sourceMetadataChangeDuringReadRejectsTheSnapshot() = runBlocking {
        val file=File(home,"changing").apply { writeBytes(ByteArray(65536){65}) }
        var changed=false
        val source=object:ParcelFileDescriptor(ParcelFileDescriptor.open(file,ParcelFileDescriptor.MODE_READ_ONLY)) {
            override fun checkError() {
                super.checkError()
                // This existing provider-error checkpoint runs after reading and before
                // the final fstat. Mutate deterministically, without a racing thread.
                assertEquals(file.length(),Os.lseek(fileDescriptor,0,OsConstants.SEEK_CUR))
                val before=Os.fstat(fileDescriptor)
                assertTrue(file.setLastModified(file.lastModified()-2000))
                assertNotEquals(before.st_mtim.tv_sec,Os.fstat(fileDescriptor).st_mtim.tv_sec)
                changed=true
            }
        }
        try {
            rejects { store.snapshot(file.length()) { AssetFileDescriptor(source,0,-1) }.close() }
            assertTrue("metadata mutation must happen on the open source",changed)
            assertEquals(0L,store.reservedBytes);assertTrue(openPaths().isEmpty())
        } finally { source.close() }
    }

}
