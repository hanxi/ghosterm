package com.hanxi.ghosterm

import android.system.Os
import android.system.OsConstants
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.hanxi.ghosterm.bridge.LocalPublicationFile
import com.hanxi.ghosterm.data.*
import com.hanxi.ghosterm.data.FileDirectoryCreator.State
import com.hanxi.ghosterm.domain.*
import kotlinx.coroutines.*
import org.junit.*
import org.junit.Assert.*
import org.junit.runner.RunWith
import java.io.File
import java.util.UUID

@RunWith(AndroidJUnit4::class)
class LocalDirectoryTest {
    private val context=InstrumentationRegistry.getInstrumentation().targetContext
    private lateinit var root:File
    private lateinit var home:File
    private lateinit var creator:FileDirectoryCreator
    private val location=FileLocation(FileEndpoint.APP)
    @Before fun setup() {
        root=File(context.cacheDir,"directory-${UUID.randomUUID()}").apply { mkdir() }
        home=File(root,"home").apply { mkdir() };creator=FileDirectoryCreator(home,context.contentResolver)
    }
    @After fun cleanup() { root.deleteRecursively() }
    private suspend fun rejected(block:suspend ()->Unit) {
        var failure:Exception?=null
        try { block() } catch(e:Exception) { failure=e }
        assertNotNull("expected explicit rejection",failure)
    }
    private fun fds()=File("/proc/self/fd").listFiles()!!.mapNotNull {
        try { Os.readlink(it.path).takeIf { path -> path.contains("/${root.name}/") } } catch(_:Exception) { null }
    }
    @Test fun createsNestedEmptyDirectoriesAndCopiesRealBytes() = runBlocking {
        val result=creator.prepare(location,"目录 😀").execute()
        assertEquals(State.COMPLETE,result.state);assertEquals("目录 😀",result.location!!.directoryId)
        assertEquals("目录 😀",result.actualName);assertNull(result.documentUri)
        val child=creator.prepare(result.location!!,"empty").execute()
        assertEquals(State.COMPLETE,child.state);assertTrue(File(home,"目录 😀/empty").list()!!.isEmpty())
        assertEquals(448,Os.stat(File(home,"目录 😀").path).st_mode and 511)
        val source=File(home,"source").apply { writeText("directory copy bytes") }
        val browser=LocalFileBrowser(home,context.contentResolver)
        TransferStagingStore(File(root,"snapshots"),131072,262144,0).use { store ->
            browser.snapshot(location,browser.list(location).single { it.name==source.name },store).use { snapshot ->
                val outcome=LocalFilePublisher(home,File(root,"publications")).publish(result.location!!.directoryId,"content",snapshot)
                assertEquals(LocalFilePublisher.State.PUBLISHED,outcome.state)
                assertEquals(source.readText(),File(home,"目录 😀/content").readText())
            }
            assertEquals(0L,store.reservedBytes)
        }
        assertTrue(fds().isEmpty())
    }
    @Test fun existingObjectsRemainConflictsIncludingDanglingLinks() = runBlocking {
        File(home,"keep").writeText("original");File(home,"folder").mkdir();Os.symlink("absent",File(home,"link").path)
        for(name in listOf("keep","folder","link")) {
            val result=creator.prepare(location,name).execute()
            assertEquals(name,State.CONFLICT,result.state);assertNull(result.location)
        }
        assertEquals("original",File(home,"keep").readText());assertTrue(File(home,"folder").isDirectory)
        assertEquals("absent",Os.readlink(File(home,"link").path));assertTrue(fds().isEmpty())
    }
    @Test fun nativeComponentValidationAndPinnedParentDoNotEscape() = runBlocking {
        val selected=File(home,"selected").apply { mkdir() };val outside=File(root,"outside").apply { mkdir() }
        LocalPublicationFile.directory(home,"selected").use { pinned ->
            assertTrue(selected.renameTo(File(home,"moved")));Os.symlink(outside.path,selected.path)
            assertEquals(0,LocalPublicationFile.mkdir(pinned.fd,"inside"))
            assertTrue(File(home,"moved/inside").isDirectory);assertTrue(outside.list()!!.isEmpty())
            for(name in listOf("",".","..","../escape","a/b","/absolute"))
                assertEquals(name,OsConstants.EINVAL,LocalPublicationFile.mkdir(pinned.fd,name))
            assertEquals(OsConstants.EEXIST,LocalPublicationFile.mkdir(pinned.fd,"inside"))
            assertEquals(OsConstants.EBADF,LocalPublicationFile.mkdir(-1,"valid"))
        }
        for(parent in listOf("selected","../outside","missing","/absolute","moved/..","moved//inside")) {
            val result=creator.prepare(location.copy(directoryId=parent),"new").execute()
            assertEquals(parent,State.NOT_ATTEMPTED,result.state);assertNotNull(result.error)
        }
        assertTrue(outside.list()!!.isEmpty());assertTrue(fds().isEmpty())
    }
    @Test fun invalidNamesAndRemoteEndpointsAreRejectedBeforeIo() = runBlocking {
        for(name in listOf("",".","..","a/b","a\u0000b","\ud800","x\udc00","界".repeat(86)))
            rejected { creator.prepare(location,name) }
        val remote=FileEndpoint("remote:test","server","/",remote=RemoteFileIdentity("server","example","user",22))
        rejected { creator.prepare(FileLocation(remote),"new") }
        assertEquals(State.COMPLETE,creator.prepare(location,"界".repeat(85)).execute().state)
        assertEquals(1,home.list()!!.size);assertTrue(fds().isEmpty())
    }
    @Test fun cancellationBeforeStartAndSealedRequestDoNotRepeatIo() = runBlocking {
        val pending=creator.prepare(location,"cancelled");pending.cancel();pending.awaitClosed()
        val job=launch { pending.execute();fail("cancelled operation delivered") };job.join()
        assertTrue(job.isCancelled);assertEquals(State.NOT_ATTEMPTED,pending.outcome.state);assertFalse(File(home,"cancelled").exists())
        rejected { pending.execute() }
        val request=creator.prepare(location,"once");assertEquals(State.COMPLETE,request.execute().state)
        var repeated:Exception?=null
        try { request.execute() } catch(failure:Exception) { repeated=failure }
        assertTrue("completed request must reject repeated execution before cancellation",repeated is IllegalStateException)
        request.cancel();request.awaitClosed();assertEquals(State.COMPLETE,request.outcome.state)
        rejected { request.execute() };assertEquals(setOf("once"),home.list()!!.toSet());assertTrue(fds().isEmpty())
    }
    @Test fun concurrentCreatorsHaveOneWinnerAndOneConflict() = runBlocking {
        val start=CompletableDeferred<Unit>()
        val jobs=(1..2).map { async(Dispatchers.Default) { start.await();creator.prepare(location,"race").execute() } }
        start.complete(Unit);val results=jobs.awaitAll()
        assertEquals(1,results.count { it.state==State.COMPLETE });assertEquals(1,results.count { it.state==State.CONFLICT })
        assertEquals(setOf("race"),home.list()!!.toSet());assertTrue(File(home,"race").isDirectory);assertTrue(fds().isEmpty())
    }
    @Test fun kernelDenialIsRejectedWithoutInventingADirectory() = runBlocking {
        val parent=File(home,"readonly").apply { mkdir() };Os.chmod(parent.path,320)
        try {
            val result=creator.prepare(location.copy(directoryId="readonly"),"denied").execute()
            assertEquals(State.REJECTED,result.state);assertNotNull(result.error);assertNull(result.location)
            assertTrue(parent.list()!!.isEmpty());assertTrue(fds().isEmpty())
        } finally { Os.chmod(parent.path,448) }
    }
}
