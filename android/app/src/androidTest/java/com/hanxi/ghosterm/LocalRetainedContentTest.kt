package com.hanxi.ghosterm

import android.system.Os
import androidx.test.platform.app.InstrumentationRegistry
import com.hanxi.ghosterm.data.*
import com.hanxi.ghosterm.domain.*
import kotlinx.coroutines.*
import org.junit.*
import org.junit.Assert.*
import java.io.File
import java.util.UUID

class LocalRetainedContentTest {
    private val context=InstrumentationRegistry.getInstrumentation().targetContext
    private lateinit var root:File
    private lateinit var tasks:File
    private lateinit var owner:File
    private lateinit var destination:File
    private lateinit var file:File
    private lateinit var budget:LocalPublicationBudget
    private lateinit var service:LocalRetainedFiles
    private val abc=FileContentDigest(3,"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")
    private fun dir(f:File)=f.apply { mkdirs();Os.chmod(path,448) }
    private fun candidate(parent:File,name:String=UUID.randomUUID().toString())=File(parent,name).apply { writeText("abc");Os.chmod(path,384) }
    @Before fun setup() {
        root=File(context.cacheDir,"private-content-${UUID.randomUUID()}").apply { mkdirs() }
        tasks=dir(File(root,"file-tasks"));owner=dir(File(tasks,UUID.randomUUID().toString()));destination=dir(File(owner,"destination"))
        file=candidate(destination);budget=LocalPublicationBudget(freeSpaceReserve=0);service=LocalRetainedFiles(tasks,budget)
    }
    @After fun cleanup() { root.deleteRecursively() }
    private fun fds()=File("/proc/self/fd").listFiles()!!.count { runCatching { Os.readlink(it.path).contains("/${root.name}/") }.getOrDefault(false) }
    private fun record(path:String=file.absolutePath,digest:FileContentDigest?=abc):FileJournalRecord {
        val source=FileLocation(FileEndpoint.APP,"source");val target=FileLocation(FileEndpoint.APP,"target")
        val entry=BrowserEntry("source/a","a",FileKind.FILE,3u,null)
        val item=FileCopyItemResult(FileScanEntry(source,entry,"a"),FileCopyItemState.UNKNOWN,target,"a",candidatePath=path,sourceDigest=digest)
        return FileJournalRecord(FileJournalIntent(UUID.randomUUID().toString(),UUID.randomUUID().toString(),source,"source",listOf(entry),target,"target"),
            FileJournalPhase.RUNNING,0,items=listOf(FileJournalItem(item,FileCopyCheckpoint.PUBLISH)))
    }
    private suspend fun inspect(saved:FileJournalRecord=record(),read:suspend(String)->FileContentDigest?={ service.content(it) })=
        FileHistoryContentInspector({ error("Private candidate must not browse HOME") },{ _,_->error("Private candidate must not use a normal file reader") },read)
            .inspect(saved,saved.items.single(),true)
    @Test fun realPrivateContentDoesNotPrepareDeletionOrLeakDescriptors()=runBlocking<Unit> {
        val ref=service.load().items.single().ref
        val digest=runCatching { service.content(file.absolutePath) }
        assertTrue("private hash must succeed: ${digest.exceptionOrNull()}",digest.isSuccess)
        assertEquals(abc,digest.getOrThrow());assertEquals(0,fds())
        var denied=false
        try { service.clear(ref) } catch(_:IllegalStateException) { denied=true }
        assertTrue("Reading content must not authorize deletion",denied)
        assertEquals("abc",file.readText());assertEquals(0,fds())
    }
    @Test fun historyComparesPrivateCandidateWithoutInferringItsTaskOwnership()=runBlocking<Unit> {
        val saved=record();assertNotEquals(saved.intent.id,owner.name)
        assertEquals(FileHistoryContentState.MATCH,inspect(saved).state)
        file.writeText("ab");assertEquals(FileHistoryContentState.DIFFERENT,inspect(saved).state)
        assertTrue(file.delete());assertEquals(FileHistoryContentState.MISSING,inspect(saved).state)
        assertEquals(FileCopyItemState.UNKNOWN,saved.items.single().value.state);assertEquals(0,fds())
    }
    @Test fun noncanonicalForeignAndSourcePathsNeverReadAnything()=runBlocking<Unit> {
        for(path in listOf(File(root,"outside").absolutePath,
            File(tasks,"${owner.name}/source/${file.name}").absolutePath,
            File(tasks,"${owner.name}/destination/../${file.name}").absolutePath,
            File(tasks,"not-a-uuid/destination/${file.name}").absolutePath,
            file.absolutePath+"/",file.absolutePath.replace("/destination/","//destination/"))) {
            var reads=0
            val result=inspect(record(path)) { service.content(it,checkpoint={ reads++ }) }
            assertEquals(path,FileHistoryContentState.UNKNOWN,result.state);assertEquals(0,reads);assertEquals(0,fds())
        }
    }
    @Test fun missingRootAndNamedFileAreMissingButUnsupportedReaderIsUnknown()=runBlocking<Unit> {
        assertNull(service.content(File(destination,UUID.randomUUID().toString()).absolutePath))
        val moved=File(root,"moved");assertTrue(tasks.renameTo(moved))
        assertNull(service.content(file.absolutePath));assertEquals(0,fds())
        val saved=record()
        val unsupported=FileHistoryContentInspector({ error("no browse") },{ _,_->error("no read") }).inspect(saved,saved.items.single(),true)
        assertEquals(FileHistoryContentState.UNKNOWN,unsupported.state)
    }
    @Test fun modesLinksAndNonFilesAreRejectedBeforeHashing()=runBlocking<Unit> {
        var reads=0
        suspend fun rejected() {
            assertEquals(FileHistoryContentState.UNKNOWN,inspect { service.content(it,checkpoint={ reads++ }) }.state)
            assertEquals(0,reads);assertEquals(0,fds())
        }
        Os.chmod(file.path,420);rejected();Os.chmod(file.path,384)
        assertTrue(file.delete());Os.symlink(File(root,"outside").path,file.path);rejected();assertTrue(file.delete())
        dir(file);rejected();assertTrue(file.delete())
        file=candidate(destination)
        Os.chmod(owner.path,493);rejected();Os.chmod(owner.path,448)
        Os.chmod(tasks.path,493);rejected();Os.chmod(tasks.path,448)
    }
    @Test fun limitAndCancellationCloseAllHandlesWithoutChangingTheFile()=runBlocking<Unit> {
        var reads=0
        assertEquals(FileHistoryContentState.UNKNOWN,inspect { service.content(it,2,checkpoint={ reads++ }) }.state)
        assertEquals(0,reads);assertEquals(0,fds())
        var cancelled=false
        try { service.content(file.absolutePath,checkpoint={ throw CancellationException("cancel") }) }
        catch(_:CancellationException) { cancelled=true }
        assertTrue(cancelled);assertEquals(0,fds());assertEquals("abc",file.readText())
        budget.beginTask().close()
    }
    @Test fun activeCopyRejectsPrivateReadsAndNeverChangesItsCandidate()=runBlocking<Unit> {
        val active=budget.beginTask();var reads=0
        try {
            assertEquals(FileHistoryContentState.UNKNOWN,inspect { service.content(it,checkpoint={ reads++ }) }.state)
            assertEquals(0,reads);assertEquals("abc",file.readText());assertEquals(0,fds())
        } finally { active.close() }
        assertEquals(abc,service.content(file.absolutePath))
    }
    @Test fun observableContentChangeDuringReadIsUnknown()=runBlocking<Unit> {
        var changed=false;var observable=false
        val result=inspect { service.content(it,checkpoint={ if(!changed) {
            changed=true;file.writeText("abd")
            observable=file.setLastModified(file.lastModified()+2000)
        } }) }
        assertTrue(changed && observable);assertEquals(FileHistoryContentState.UNKNOWN,result.state)
        assertEquals("abd",file.readText());assertEquals(0,fds())
    }
    @Test fun replacedDirectoryNameCannotTurnTheOldPinnedFileIntoACurrentMatch()=runBlocking<Unit> {
        var moved=false
        val result=inspect { service.content(it,checkpoint={ if(!moved) {
            val other=File(owner,"moved");moved=destination.renameTo(other);check(moved)
            dir(destination);candidate(destination,file.name)
        } }) }
        assertTrue(moved);assertEquals(FileHistoryContentState.UNKNOWN,result.state)
        assertEquals("abc",file.readText());assertEquals("abc",File(owner,"moved/${file.name}").readText());assertEquals(0,fds())
    }
    @Test fun replacedRootCannotRedirectTheReadOrGrantCleanupAuthority()=runBlocking<Unit> {
        var moved=false
        val result=inspect { service.content(it,checkpoint={ if(!moved) {
            moved=tasks.renameTo(File(root,"moved-tasks"));check(moved)
            dir(tasks);dir(owner);dir(destination);candidate(destination,file.name)
        } }) }
        assertTrue(moved);assertEquals(FileHistoryContentState.UNKNOWN,result.state);assertEquals("abc",file.readText());assertEquals(0,fds())
    }
    @Test fun absentHistoricalDigestDoesNotOpenThePrivateReader()=runBlocking<Unit> {
        val result=inspect(record(digest=null)) { error("No reference must not read private data") }
        assertEquals(FileHistoryContentState.NO_REFERENCE,result.state);assertEquals(0,fds())
    }
}
