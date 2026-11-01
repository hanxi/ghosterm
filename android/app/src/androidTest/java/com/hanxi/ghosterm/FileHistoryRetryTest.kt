package com.hanxi.ghosterm

import android.database.sqlite.SQLiteDatabase
import androidx.test.platform.app.InstrumentationRegistry
import com.hanxi.ghosterm.data.FileTaskJournal
import com.hanxi.ghosterm.domain.*
import kotlinx.coroutines.runBlocking
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test
import java.io.File
import java.util.UUID

class FileHistoryRetryTest {
    private val source=FileLocation(FileEndpoint.APP,"source")
    private val target=FileLocation(FileEndpoint.APP,"target")
    private fun entry(name: String, kind: FileKind=FileKind.FILE)=BrowserEntry("source/$name",name,kind,1uL,1)
    private fun request(items: List<BrowserEntry> = listOf(entry("a")))=FilePasteRequest.capture(FileClipboardSnapshot.capture(source,"source",items),target,"target")
    private fun record(request: FilePasteRequest, items: List<FileJournalItem> = emptyList(), phase: FileJournalPhase=FileJournalPhase.RUNNING)=
        FileJournalRecord(FileJournalIntent(request.id,request.clipboardId,source,"source",request.items,target,"target"),phase,0,items=items)
    private fun saved(value: BrowserEntry=entry("a"),state: FileCopyItemState=FileCopyItemState.FAILED,
        checkpoint: FileCopyCheckpoint=FileCopyCheckpoint.RESULT,parent: FileLocation=source,path: String=value.name,created: String?=null,candidate: String?=null)=
        FileJournalItem(FileCopyItemResult(FileScanEntry(parent,value,path),state,target,value.name,createdId=created,candidatePath=candidate),checkpoint)
    @Test fun queuedIntentRequiresExplicitNewRequestAndFreshPolicy() {
        val old=request()
        val retry=FileCopyRetry.plan(record(old,phase=FileJournalPhase.QUEUED))
        val group=retry.groups.single()
        val fresh=FilePasteRequest.capture(FileClipboardSnapshot.capture(group.source,group.sourceLabel,group.entries),group.destination,group.destinationLabel,retryOf=old.id)
        assertNotEquals(old.id,fresh.id);assertNotEquals(old.clipboardId,fresh.clipboardId)
        assertEquals(old.id,fresh.retryOf);assertEquals(FileConflictPolicy.ASK,fresh.conflictPolicy)
    }
    @Test fun emptyRunningOrUnknownRecordCannotReplaySelection() {
        for(phase in listOf(FileJournalPhase.RUNNING,FileJournalPhase.CANCELLING)) {
            val retry=FileCopyRetry.plan(record(request(),phase=phase))
            assertTrue(retry.groups.isEmpty());assertEquals(1,retry.blocked)
        }
        val retry=FileCopyRetry.plan(record(request(),phase=FileJournalPhase.FINISHED).copy(outcome=FileCopyState.UNKNOWN))
        assertTrue(retry.groups.isEmpty())
    }
    @Test fun acknowledgedFailuresRetryWithoutCompletedOrSkippedItems() {
        val old=request(listOf(entry("a"),entry("done"),entry("skip")))
        val retry=FileCopyRetry.plan(record(old,listOf(saved(),saved(entry("done"),FileCopyItemState.COMPLETE),saved(entry("skip"),FileCopyItemState.SKIPPED))))
        assertEquals(listOf("a"),retry.groups.single().entries.map { it.name });assertEquals(2,retry.retained);assertEquals(1,retry.retryItems)
    }
    @Test fun sideEffectCheckpointsNeverBecomeRetryableFromStaleItemState() {
        for(point in FileCopyCheckpoint.entries.filter { it!=FileCopyCheckpoint.RESULT }) {
            val retry=FileCopyRetry.plan(record(request(),listOf(saved(checkpoint=point))))
            assertTrue("$point has no acknowledged failed result",retry.groups.isEmpty());assertEquals(1,retry.blocked)
        }
    }
    @Test fun untouchedPlanRetriesButPlanWithCandidateRemainsUnknown() {
        val clean=saved(state=FileCopyItemState.NOT_ATTEMPTED,checkpoint=FileCopyCheckpoint.PREPARING)
        assertEquals(1,FileCopyRetry.plan(record(request(),listOf(clean))).retryItems)
        val dirty=clean.copy(value=clean.value.copy(candidatePath="target/.candidate"))
        val retry=FileCopyRetry.plan(record(request(),listOf(dirty)))
        assertTrue(retry.groups.isEmpty());assertEquals(1,retry.blocked)
    }
    @Test fun initialSkippedPlanIsRetainedWithoutReplay() {
        val retry=FileCopyRetry.plan(record(request(),listOf(saved(state=FileCopyItemState.SKIPPED,checkpoint=FileCopyCheckpoint.PREPARING))))
        assertTrue(retry.groups.isEmpty());assertEquals(1,retry.retained);assertEquals(0,retry.blocked);assertEquals(0,retry.retryItems)
    }
    @Test fun acknowledgedDirectoryRoutesFailedChildToRecordedDestination() {
        val tree=entry("tree",FileKind.DIRECTORY)
        val child=BrowserEntry("source/tree/child","child",FileKind.FILE,1u,1)
        val items=listOf(saved(tree,FileCopyItemState.COMPLETE,created="target/tree"),saved(child,parent=source.copy(directoryId="source/tree"),path="tree/child"))
        val retry=FileCopyRetry.plan(record(request(listOf(tree)),items))
        assertEquals("target/tree",retry.groups.single().destination.directoryId)
        assertEquals("source/tree",retry.groups.single().source.directoryId)
        assertEquals(listOf("child"),retry.groups.single().entries.map { it.name });assertEquals(1,retry.retained)
        val unconfirmed=items.first().copy(checkpoint=FileCopyCheckpoint.DIRECTORY_CREATED)
        assertTrue(FileCopyRetry.plan(record(request(listOf(tree)),listOf(unconfirmed,items.last()))).groups.isEmpty())
    }
    @Test fun foreignSourceOrChangedRootIdentityIsRejected() {
        assertThrows(IllegalArgumentException::class.java) { FileCopyRetry.plan(record(request(),listOf(saved(entry("foreign"))))) }
        assertThrows(IllegalArgumentException::class.java) { FileCopyRetry.plan(record(request(),listOf(saved(entry().copy(id="elsewhere"))))) }
    }
    private fun entry()=entry("a")
    @Test fun descendantCannotSwitchSourceDirectoryBehindRecordedParent() {
        val tree=entry("tree",FileKind.DIRECTORY)
        val child=BrowserEntry("source/tree/child","child",FileKind.FILE,1u,1)
        val items=listOf(saved(tree,FileCopyItemState.COMPLETE,created="target/tree"),
            saved(child,parent=source.copy(directoryId="source/another"),path="tree/child"))
        assertThrows(IllegalArgumentException::class.java) { FileCopyRetry.plan(record(request(listOf(tree)),items)) }
    }
    @Test fun mismatchedTargetEndpointIsRejected() {
        val item=saved();val foreign=item.copy(value=item.value.copy(targetParent=FileLocation(FileEndpoint("other","other",""))))
        assertThrows(IllegalArgumentException::class.java) { FileCopyRetry.plan(record(request(),listOf(foreign))) }
    }
    @Test fun duplicateHistoryPathsAreRejected() {
        assertThrows(IllegalArgumentException::class.java) { FileCopyRetry.plan(record(request(),listOf(saved(),saved()))) }
    }
    @Test fun invalidOriginIdIsRejectedBeforeAdmission() {
        assertThrows(IllegalArgumentException::class.java) { FilePasteRequest.capture(FileClipboardSnapshot.capture(source,"source",listOf(entry())),target,"target",retryOf="invalid") }
    }
    private suspend fun withJournal(block: suspend(File,FileTaskJournal)->Unit) {
        val root=File(InstrumentationRegistry.getInstrumentation().targetContext.cacheDir,"history-retry-${UUID.randomUUID()}").apply { mkdirs() }
        try { val path=File(root,"tasks.db");block(path,FileTaskJournal(path)) } finally { root.deleteRecursively() }
    }
    private fun payload(path: File,id: String): ByteArray = SQLiteDatabase.openDatabase(path.path,null,SQLiteDatabase.OPEN_READONLY).use { db ->
        db.rawQuery("SELECT payload FROM tasks WHERE id=?",arrayOf(id)).use { check(it.moveToFirst());it.getBlob(0) }
    }
    private fun changeHeader(path: File,id: String, change: (JSONObject)->Unit) {
        val old=payload(path,id);val json=JSONObject(old.toString(Charsets.UTF_8));change(json);val fresh=json.toString().toByteArray()
        SQLiteDatabase.openDatabase(path.path,null,SQLiteDatabase.OPEN_READWRITE).use { db ->
            db.execSQL("UPDATE tasks SET payload=? WHERE id=?",arrayOf(fresh,id))
            db.execSQL("UPDATE budget SET bytes=bytes+? WHERE id=1",arrayOf(fresh.size-old.size))
        }
    }
    @Test fun releasedVersionOneAndNewLineageRoundTripWithoutRewritingOldRecord()=runBlocking {
        withJournal { path,journal ->
            val old=request();journal.load();journal.accept(old);val bytes=payload(path,old.id)
            assertEquals("1",JSONObject(bytes.toString(Charsets.UTF_8)).getString("version"))
            val fresh=FilePasteRequest.capture(FileClipboardSnapshot.capture(source,"source",old.items),target,"target",retryOf=old.id)
            journal.accept(fresh)
            assertEquals("2",JSONObject(payload(path,fresh.id).toString(Charsets.UTF_8)).getString("version"))
            assertEquals(old.id,FileTaskJournal(path).load().single { it.intent.id==fresh.id }.intent.retryOf)
            assertArrayEquals(bytes,payload(path,old.id))
            journal.finish(fresh.id,null,false,null);journal.remove(old.id)
            assertEquals(old.id,FileTaskJournal(path).load().single().intent.retryOf)
        }
    }
    @Test fun selfReferencingLineageFailsWithValidAccounting()=runBlocking {
        withJournal { path,journal ->
            val old=request();val fresh=FilePasteRequest.capture(FileClipboardSnapshot.capture(source,"source",old.items),target,"target",retryOf=old.id)
            journal.load();journal.accept(fresh);changeHeader(path,fresh.id) { it.put("retryOf",fresh.id) }
            val error=try { journal.load();null } catch(error: Exception) { error }
            assertNotNull(error);assertTrue(error!!.message.orEmpty().contains("任务不能引用自身"))
        }
    }
    @Test fun lineageHeaderCannotOmitRequiredOriginOrAcceptUnknownVersion()=runBlocking {
        withJournal { path,journal ->
            val fresh=FilePasteRequest.capture(FileClipboardSnapshot.capture(source,"source",listOf(entry())),target,"target",retryOf=UUID.randomUUID().toString())
            journal.load();journal.accept(fresh);changeHeader(path,fresh.id) { it.remove("retryOf") }
            var rejected=false
            try { journal.load() } catch(_: Exception) { rejected=true };assertTrue(rejected)
            changeHeader(path,fresh.id) { it.put("version","99") }
            rejected=false;try { journal.load() } catch(_: Exception) { rejected=true };assertTrue(rejected)
        }
    }
}
