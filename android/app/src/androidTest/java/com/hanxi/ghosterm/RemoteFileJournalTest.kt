package com.hanxi.ghosterm

import android.content.Context
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.hanxi.ghosterm.bridge.*
import com.hanxi.ghosterm.data.*
import com.hanxi.ghosterm.domain.*
import kotlinx.coroutines.*
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File
import java.io.IOException
import java.util.UUID

/** Real SSH, separate checking connection, no credentials or live handles in the journal. */
internal class RemoteJournalScenario(private val context:Context, val root:File) {
    companion object {
        val points=listOf("remote-create","remote-publish","remote-published","remote-directory")
        const val CONTENT="process death bytes ☃\n"
    }
    private val fixture by lazy { sshFixture() }
    val journal=FileTaskJournal(File(root,"file-history/tasks.db"))
    private val home=File(root,"home")
    private fun connection():ResolvedSSHConnection {
        val known=File(root,"known").apply {
            writeText("[journal.ghosterm.test]:${fixture.getInt("port")} "+fixture.getString("public")+"\n")
        }
        return ResolvedSSHConnection("journal.ghosterm.test",fixture.getInt("port"),fixture.getString("user"),known.path,byteArrayOf(127,0,0,1))
    }
    private suspend fun authenticate(session:RemoteFileSession) {
        withTimeout(10000) {
            var auth=session.authentication()
            while(auth.phase!=SSHAuthPhase.CREDENTIALS) {
                assertNotEquals("SSH fixture must reach credentials",SSHAuthPhase.FAILED,auth.phase)
                delay(10);auth=session.authentication()
            }
            session.privateKey(auth,fixture.getString("filePem").toByteArray())
            while(!session.state().ready) { assertFalse(session.state().finished);delay(10) }
        }
    }
    private suspend fun <T> connected(block:suspend(RemoteFileSession)->T):T {
        val session=RemoteFileSession(connection())
        try { authenticate(session);return block(session) }
        finally { withContext(NonCancellable) { session.closeAndAwait() } }
    }
    suspend fun perform(point:String,boundary:suspend(String)->Unit):FileCopyProgress {
        require(point in points)
        check(!root.exists());check(File(home,"source").mkdirs());check(File(home,"target").mkdir())
        File(home,"source/a.txt").writeText(CONTENT)
        if(point=="remote-directory") {
            File(home,"source/folder").mkdir();File(home,"source/folder/child").writeText(CONTENT)
        }
        val local=LocalFileBrowser(home,context.contentResolver)
        val source=FileLocation(FileEndpoint.APP,"source")
        val plan=connected { browser ->
            val created=browser.prepareDirectory(fixture.getString("remoteScratch"),"journal-${UUID.randomUUID()}")
            assertEquals(RemoteDirectoryCreationState.COMPLETE,created.execute().state);created.awaitClosed()
            val identity=RemoteFileIdentity("journal","journal.ghosterm.test",fixture.getString("user"),fixture.getInt("port"))
            val endpoint=FileEndpoint("journal-${UUID.randomUUID()}","journal",created.path,remote=identity)
            val destination=FileLocation(endpoint,created.path)
            suspend fun browse(location:FileLocation):BrowserDirectory {
                if(location.endpoint.remote==null) return local.browse(location)
                val found=browser.browse(location.directoryId)
                assertTrue(found.entries.isEmpty())
                return BrowserDirectory(location.copy(directoryId=found.path),emptyList())
            }
            val selected=local.browse(source).entries.filter { it.name==if(point=="remote-directory") "folder" else "a.txt" }
            assertEquals(1,selected.size)
            val request=FilePasteRequest.capture(FileClipboardSnapshot.capture(source,"source",selected),destination,"remote target")
            val scan=FileTreeScanner(::browse).scan(request);assertTrue(scan.complete)
            val access=FileDestinationAccess(home,context.contentResolver,::browse)
            val preflight=FilePastePreflight(access::inspect,access::relation).check(scan);assertTrue(preflight.checked)
            val decisions=FilePasteDecisions.begin(preflight)
            decisions.confirm(decisions.state().reference)
        }
        journal.load();journal.accept(plan.request);journal.phase(plan.request.id,FileJournalPhase.CONFIRMING)
        val run=FileCopyExecutor(home,context.contentResolver,File(root,"staging"),{ request ->
            FileTaskConnections(request.source.endpoint,request.destination.endpoint,{ _,_->connection() },{ _,_,session->authenticate(session) })
        },record={ value,checkpoint ->
            if((point=="remote-published" || point=="remote-directory") && value.state==FileCopyItemState.COMPLETE)
                boundary(plan.request.id) // Intentionally stop before the final observation commits.
            journal.checkpoint(plan.request.id,value,checkpoint)
            if((point=="remote-create" && checkpoint==FileCopyCheckpoint.REMOTE_CREATE) ||
                (point=="remote-publish" && checkpoint==FileCopyCheckpoint.PUBLISH)) boundary(plan.request.id)
        }).prepare(plan)
        journal.begin(plan.request.id,run.outcome.items)
        try { return run.execute() }
        finally { withContext(NonCancellable) { run.cancel();run.awaitClosed() } }
    }
    suspend fun verify(point:String,record:FileJournalRecord) {
        assertTrue("remote interrupted record must remain read-only",record.interrupted)
        assertEquals(FileJournalPhase.RUNNING,record.phase)
        val item=record.items.first { it.value.source.relativePath==if(point=="remote-directory") "folder" else "a.txt" }
        val destination=record.intent.destination
        assertEquals("journal.ghosterm.test",destination.endpoint.remote!!.host)
        connected { checking ->
            val entries=checking.browse(destination.directoryId).entries
            when(point) {
                "remote-create"->{
                    assertEquals("creation intent must be durable before SSH write",FileCopyCheckpoint.REMOTE_CREATE,item.checkpoint)
                    assertNotNull(item.value.candidatePath)
                    assertTrue("failed create checkpoint must leave remote directory empty",entries.isEmpty())
                }
                "remote-publish"->{
                    assertEquals("publication intent must be durable before SSH rename",FileCopyCheckpoint.PUBLISH,item.checkpoint)
                    assertEquals(FileCopyItemState.CREATED,item.value.state)
                    assertEquals(listOf(item.value.candidatePath),entries.map { it.path })
                    assertArrayEquals(CONTENT.toByteArray(),checking.preview(checkNotNull(item.value.candidatePath)))
                }
                "remote-published"->{
                    assertEquals(FileCopyCheckpoint.PUBLISH,item.checkpoint)
                    assertNotEquals("old record must not invent completion",FileCopyItemState.COMPLETE,item.value.state)
                    assertEquals(listOf("a.txt"),entries.map { it.name })
                    assertArrayEquals(CONTENT.toByteArray(),checking.preview(destination.directoryId+"/a.txt"))
                }
                "remote-directory"->{
                    assertEquals("directory intent must survive lost result",FileCopyCheckpoint.DIRECTORY_CREATE,item.checkpoint)
                    assertNotEquals(FileCopyItemState.COMPLETE,item.value.state)
                    assertEquals(listOf("folder"),entries.map { it.name });assertTrue(entries.single().directory)
                    assertTrue("failed directory record must prevent child writes",checking.browse(destination.directoryId+"/folder").entries.isEmpty())
                    assertEquals(FileCopyItemState.NOT_ATTEMPTED,record.items.single { it.value.source.relativePath=="folder/child" }.value.state)
                }
                else->error("unknown remote checkpoint")
            }
        }
        assertEquals(CONTENT,File(home,"source/a.txt").readText())
    }
}

@RunWith(AndroidJUnit4::class)
class RemoteFileJournalTest {
    private fun checkPoint(point:String)=runBlocking<Unit> {
        val context=InstrumentationRegistry.getInstrumentation().targetContext
        val root=File(context.cacheDir,"remote-journal-${UUID.randomUUID()}")
        val scenario=RemoteJournalScenario(context,root)
        try {
            var hits=0
            val result=withTimeout(20000) { scenario.perform(point) { hits++;throw IOException("deliberate record boundary failure") } }
            assertEquals("required durable checkpoint must execute exactly once",1,hits)
            assertEquals(FileCopyState.FAILED,result.state)
            assertFalse(result.mayClearClipboard)
            if(point=="remote-published" || point=="remote-directory")
                assertEquals("known side effect must survive record failure",FileCopyItemState.COMPLETE,result.items.first().state)
            val record=scenario.journal.load().single()
            scenario.verify(point,record)
            scenario.journal.remove(record.intent.id);assertTrue(scenario.journal.load().isEmpty())
            scenario.verify(point,record) // Clearing metadata must not remove any remote side effect.
        } finally { root.deleteRecursively() }
    }
    @Test fun creationIntentPrecedesRemoteWrite()=checkPoint("remote-create")
    @Test fun publicationIntentPreservesCompleteCandidate()=checkPoint("remote-publish")
    @Test fun publishedFileSurvivesMissingFinalRecord()=checkPoint("remote-published")
    @Test fun directoryCreationRecordFailureStopsChildWrites()=checkPoint("remote-directory")
}
