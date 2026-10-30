package com.hanxi.ghosterm

import android.os.ParcelFileDescriptor
import android.system.Os
import android.system.OsConstants
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.hanxi.ghosterm.bridge.LocalPublicationFile
import com.hanxi.ghosterm.data.LocalFileBrowser
import com.hanxi.ghosterm.data.LocalFilePublisher
import com.hanxi.ghosterm.data.TransferStagingStore
import com.hanxi.ghosterm.domain.*
import kotlinx.coroutines.*
import org.junit.After
import org.junit.Assert.*
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File
import java.io.IOException
import java.util.UUID
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.TimeUnit
import kotlin.coroutines.CoroutineContext

@RunWith(AndroidJUnit4::class)
class LocalPublicationTest {
    private val context=InstrumentationRegistry.getInstrumentation().targetContext
    private lateinit var root:File
    private lateinit var home:File
    private lateinit var publications:File
    private lateinit var store:TransferStagingStore
    private lateinit var publisher:LocalFilePublisher
    private lateinit var local:LocalFileBrowser
    @Before fun setup() {
        root=File(context.cacheDir,"publication-${UUID.randomUUID()}").apply { mkdir() }
        home=File(root,"home").apply { mkdir() }
        store=TransferStagingStore(File(root,"staging"),131072,262144,0)
        publications=File(root,"publications").apply { mkdir() }
        publisher=LocalFilePublisher(home,publications);local=LocalFileBrowser(home,context.contentResolver)
    }
    @After fun cleanup() { store.close();root.deleteRecursively() }
    private fun fds()=File("/proc/self/fd").listFiles()!!.mapNotNull {
        try { Os.readlink(it.path).takeIf { path -> path.contains("/${root.name}/") }?.let { path->it.name.toInt() to path } }
        catch(_:Exception) { null }
    }
    private suspend fun snapshot(bytes:ByteArray):TransferStagingStore.Snapshot {
        val file=File(home,"source").apply { writeBytes(bytes) }
        return local.snapshot(FileLocation(FileEndpoint.APP),BrowserEntry(file.name,file.name,FileKind.FILE,bytes.size.toULong(),null),store)
    }
    private suspend fun rejects(block:suspend ()->Unit) {
        var failure:Exception?=null
        try { block() } catch(e:CancellationException) { throw e } catch(e:Exception) { failure=e }
        assertNotNull("must fail explicitly",failure)
    }
    private fun readonly(fd:Int):ParcelFileDescriptor {
        val raw=Os.open("/proc/self/fd/$fd",OsConstants.O_RDONLY or OsConstants.O_CLOEXEC,0)
        return try { ParcelFileDescriptor.dup(raw) } finally { Os.close(raw) }
    }

    @Test fun completeBytesEmptyFileAndNestedUnicodePublishWithoutLeaking() = runBlocking {
        val bytes=ByteArray(100003){(it%251).toByte()};File(home,"目录").mkdir()
        snapshot(bytes).use { frozen ->
            val held=fds().size;val progress=mutableListOf<Long>()
            val result=publisher.publish("目录","内容.bin",frozen) { copied,total ->
                assertEquals(bytes.size.toLong(),total);progress.add(copied)
                assertFalse("final path visible before publish",File(home,"目录/内容.bin").exists())
            }
            assertEquals(LocalFilePublisher.State.PUBLISHED,result.state);assertEquals(0,result.error);assertNull(result.cleanupError)
            assertArrayEquals(bytes,File(home,"目录/内容.bin").readBytes());assertArrayEquals(bytes,File(home,"source").readBytes())
            assertEquals(listOf(0L,32768L,65536L,98304L,100003L),progress)
            assertEquals(384,Os.stat(File(home,"目录/内容.bin").path).st_mode and 511)
            assertEquals(held,fds().size);assertEquals(bytes.size.toLong(),store.reservedBytes)
            assertEquals(setOf("内容.bin"),File(home,"目录").list()!!.toSet())
        }
        snapshot(byteArrayOf()).use { frozen ->
            assertEquals(LocalFilePublisher.State.PUBLISHED,publisher.publish("","empty",frozen).state)
            assertEquals(0L,File(home,"empty").length())
        }
        assertTrue(fds().isEmpty());assertTrue(publications.list()!!.isEmpty());assertEquals(0L,store.reservedBytes)
    }

    @Test fun existingFileDirectoryAndDanglingLinkAreNeverOverwritten() = runBlocking {
        val keep=File(home,"keep").apply { writeText("original") };File(home,"folder").mkdir()
        Os.symlink("missing-target",File(home,"link").path)
        snapshot("replacement".toByteArray()).use { frozen ->
            for(name in listOf("keep","folder","link")) {
                val result=publisher.publish("",name,frozen)
                assertEquals(LocalFilePublisher.State.CONFLICT,result.state);assertEquals(OsConstants.EEXIST,result.error)
            }
        }
        assertEquals("original",keep.readText());assertTrue(File(home,"folder").isDirectory)
        assertEquals("missing-target",Os.readlink(File(home,"link").path))
        assertEquals(setOf("keep","folder","link","source"),home.list()!!.toSet());assertTrue(fds().isEmpty())
    }

    @Test fun simultaneousPublishHasExactlyOneWinner() = runBlocking {
        snapshot("first".toByteArray()).use { first -> snapshot("second".toByteArray()).use { second ->
            val ready=CompletableDeferred<Unit>();val release=CompletableDeferred<Unit>();var count=0
            val barrier:suspend (Long,Long)->Unit={ copied,total -> if(copied==total) {
                synchronized(this@LocalPublicationTest) { count++;if(count==2)ready.complete(Unit) };release.await()
            } }
            val a=async { publisher.publish("","race",first,barrier) }
            val b=async { publisher.publish("","race",second,barrier) }
            withTimeout(5000) { ready.await() };assertFalse(File(home,"race").exists());release.complete(Unit)
            val results=listOf(a.await(),b.await())
            assertEquals(1,results.count { it.state==LocalFilePublisher.State.PUBLISHED })
            assertEquals(1,results.count { it.state==LocalFilePublisher.State.CONFLICT })
            assertEquals(if(results[0].state==LocalFilePublisher.State.PUBLISHED) "first" else "second",File(home,"race").readText())
        } }
        assertTrue(fds().isEmpty())
    }

    @Test fun pathsSymlinkParentsAndInvalidNamesCannotEscapeHome() = runBlocking {
        val outside=File(root,"outside").apply { mkdir() };File(home,"nested").mkdir()
        Os.symlink(outside.path,File(home,"redirect").path)
        Os.symlink(outside.path,File(home,"nested/redirect").path)
        val linkRoot=File(root,"home-link");Os.symlink(home.path,linkRoot.path)
        snapshot("safe".toByteArray()).use { frozen ->
            for(path in listOf("..","../outside","/outside","nested/..","nested//x","nested/","redirect","nested/redirect",".","nested/./x"))
                rejects { publisher.publish(path,"bad",frozen) }
            for(name in listOf("",".","..","../bad","a/b","bad\u0000suffix","\uD800")) rejects { publisher.publish("",name,frozen) }
            rejects { LocalFilePublisher(linkRoot,publications).publish("","bad",frozen) }
            assertEquals(LocalFilePublisher.State.PUBLISHED,publisher.publish("nested","合法",frozen).state)
        }
        assertTrue(outside.list()!!.isEmpty());assertFalse(File(root,"bad").exists());assertTrue(fds().isEmpty())
    }

    @Test fun cancellationAndCheckpointFailureDiscardUnpublishedCandidate() = runBlocking {
        snapshot(ByteArray(100003){65}).use { frozen ->
            val held=fds().size;val started=CompletableDeferred<Unit>()
            val job=launch { publisher.publish("","cancelled",frozen) { copied,_ -> if(copied>0) { started.complete(Unit);awaitCancellation() } } }
            withTimeout(5000) { started.await() }
            assertEquals(100003L,store.reservedBytes);assertTrue(fds().any { it.second.contains("/publications/") })
            assertFalse(File(home,"cancelled").exists());job.cancelAndJoin()
            assertFalse(File(home,"cancelled").exists());assertEquals(held,fds().size)
            rejects { publisher.publish("","failed",frozen) { copied,_ -> if(copied>0)throw IOException("checkpoint failure") } }
            assertFalse(File(home,"failed").exists());assertEquals(held,fds().size)
            val boundary=launch { publisher.publish("","boundary",frozen) { copied,total ->
                if(copied==total)currentCoroutineContext().cancel()
            } }
            boundary.join();assertFalse(File(home,"boundary").exists());assertEquals(held,fds().size)
            assertEquals(LocalFilePublisher.State.PUBLISHED,publisher.publish("","retry",frozen).state)
        }
        assertTrue(fds().isEmpty());assertTrue(publications.list()!!.isEmpty());assertEquals(0L,store.reservedBytes)
    }

    @Test fun cancelledAcknowledgementDoesNotDeletePublishedFile() = runBlocking {
        val queue=LinkedBlockingQueue<Runnable>()
        val dispatcher=object:CoroutineDispatcher() { override fun dispatch(context:CoroutineContext,block:Runnable) { queue.add(block) } }
        snapshot("published".toByteArray()).use { frozen ->
            val held=fds().size
            val job=launch(dispatcher,start=CoroutineStart.UNDISPATCHED) {
                publisher.publish("","late-cancel",frozen);fail("cancelled acknowledgement delivered")
            }
            val delivery=withContext(Dispatchers.IO) { queue.poll(5,TimeUnit.SECONDS) }
            try {
                assertNotNull(delivery);assertEquals("published",File(home,"late-cancel").readText())
            } finally { job.cancel();delivery?.run();job.join() }
            assertEquals("published",File(home,"late-cancel").readText());assertEquals(held,fds().size)
            assertEquals(LocalFilePublisher.State.CONFLICT,publisher.publish("","late-cancel",frozen).state)
        }
        assertTrue(fds().isEmpty())
    }

    @Test fun changedSnapshotAndIncompleteCandidateCannotPublish() = runBlocking {
        for(source in listOf(true,false)) snapshot(ByteArray(100003){65}).use { frozen ->
            var changed=false
            rejects { publisher.publish("","changed",frozen) { copied,total ->
                if(!changed && (if(source)copied==0L else copied==total)) {
                    val path=if(source)"/staging/" else "/publications/"
                    val fd=fds().first { it.second.contains(path) }.first
                    val writable=Os.open("/proc/self/fd/$fd",OsConstants.O_RDWR or OsConstants.O_CLOEXEC,0)
                    try { Os.ftruncate(writable,total+1) } finally { Os.close(writable) };changed=true
                }
            } }
            assertTrue("must change the actual owned inode",changed)
            assertFalse(File(home,"changed").exists());assertTrue(publications.list()!!.isEmpty())
        }
        assertTrue(fds().isEmpty())
    }

    @Test fun renameRejectionAndUnsafeStagingRemainExplicit() = runBlocking {
        snapshot("ready".toByteArray()).use { frozen ->
            val result=publisher.publish("","a".repeat(300),frozen)
            assertEquals(LocalFilePublisher.State.REJECTED,result.state)
            assertEquals(OsConstants.ENAMETOOLONG,result.error);assertNull(result.cleanupError)
            assertTrue(publications.list()!!.isEmpty())
            rejects { LocalFilePublisher(home,File(home,"staging")).publish("","unsafe",frozen) }
            val linked=File(root,"stage-link");Os.symlink(publications.path,linked.path)
            rejects { LocalFilePublisher(home,linked).publish("","unsafe",frozen) }
            assertFalse(File(home,"unsafe").exists());assertFalse(File(home,"staging").exists())
        }
        assertEquals(setOf("source"),home.list()!!.toSet());assertTrue(fds().isEmpty())
    }

    @Test fun nativeCandidateRequiresCompleteReadonlyOwnedFile() = runBlocking {
        LocalPublicationFile.directory(home,"").use { directory -> LocalPublicationFile.directory(publications,"").use { stage ->
            assertEquals(OsConstants.FD_CLOEXEC,Os.fcntlInt(directory.fileDescriptor,OsConstants.F_GETFD,0) and OsConstants.FD_CLOEXEC)
            val candidate=LocalPublicationFile.candidate(stage.fd,"candidate")
            try {
                assertEquals(1L,Os.fstat(candidate.fileDescriptor).st_nlink)
                assertEquals(384,Os.fstat(candidate.fileDescriptor).st_mode and 511)
                assertEquals(OsConstants.FD_CLOEXEC,Os.fcntlInt(candidate.fileDescriptor,OsConstants.F_GETFD,0) and OsConstants.FD_CLOEXEC)
                assertTrue(home.list()!!.isEmpty());Os.write(candidate.fileDescriptor,byteArrayOf(1,2,3),0,3)
                rejects { LocalPublicationFile.candidate(stage.fd,"candidate").close() }
                assertArrayEquals(byteArrayOf(1,2,3),File(publications,"candidate").readBytes())
                assertEquals(OsConstants.EINVAL,LocalPublicationFile.publish(directory.fd,stage.fd,candidate.fd,"candidate","bad",3))
                readonly(candidate.fd).use { ready ->
                    candidate.close()
                    assertEquals(OsConstants.EINVAL,LocalPublicationFile.publish(directory.fd,stage.fd,ready.fd,"candidate","bad",4))
                    assertEquals(OsConstants.EINVAL,LocalPublicationFile.publish(directory.fd,stage.fd,ready.fd,"candidate","bad",-1))
                    assertEquals(OsConstants.EINVAL,LocalPublicationFile.publish(directory.fd,stage.fd,ready.fd,"candidate","../bad",3))
                    assertEquals(0,LocalPublicationFile.publish(directory.fd,stage.fd,ready.fd,"candidate","ready",3))
                    assertEquals(OsConstants.ENOENT,LocalPublicationFile.publish(directory.fd,stage.fd,ready.fd,"candidate","alias",3))
                    assertEquals(0,LocalPublicationFile.remove(stage.fd,ready.fd,"candidate"))
                    assertTrue(directory.fileDescriptor.valid());assertTrue(ready.fileDescriptor.valid())
                }
            } finally { candidate.close() }
        } }
        assertArrayEquals(byteArrayOf(1,2,3),File(home,"ready").readBytes());assertTrue(fds().isEmpty());assertTrue(publications.list()!!.isEmpty())
    }

    @Test fun replacedStagingPathIsNeitherPublishedNorRemoved() = runBlocking {
        LocalPublicationFile.directory(home,"").use { directory -> LocalPublicationFile.directory(publications,"").use { stage ->
            LocalPublicationFile.candidate(stage.fd,"candidate").use { original ->
                Os.write(original.fileDescriptor,byteArrayOf(1),0,1)
                readonly(original.fd).use { ready ->
                    File(publications,"candidate").renameTo(File(publications,"moved"))
                    val replacement=File(publications,"candidate").apply { writeText("keep") }
                    assertEquals(OsConstants.EINVAL,LocalPublicationFile.publish(directory.fd,stage.fd,ready.fd,"candidate","target",1))
                    assertEquals(OsConstants.EINVAL,LocalPublicationFile.remove(stage.fd,ready.fd,"candidate"))
                    assertEquals("keep",replacement.readText());assertFalse(File(home,"target").exists())
                    replacement.delete();Os.symlink(File(publications,"moved").path,replacement.path)
                    assertEquals(OsConstants.EINVAL,LocalPublicationFile.remove(stage.fd,ready.fd,"candidate"))
                    assertTrue(OsConstants.S_ISLNK(Os.lstat(replacement.path).st_mode))
                }
            }
        } }
        assertTrue(fds().isEmpty())
    }
}
