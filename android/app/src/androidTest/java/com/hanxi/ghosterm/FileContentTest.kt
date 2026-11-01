package com.hanxi.ghosterm

import android.database.sqlite.SQLiteDatabase
import android.os.ParcelFileDescriptor
import androidx.test.platform.app.InstrumentationRegistry
import com.hanxi.ghosterm.data.*
import com.hanxi.ghosterm.domain.*
import kotlinx.coroutines.*
import org.junit.*
import org.junit.Assert.*
import org.json.JSONObject
import java.io.File
import java.io.FileInputStream
import java.io.IOException
import java.util.UUID

class FileContentTest {
    private val context=InstrumentationRegistry.getInstrumentation().targetContext
    private lateinit var root:File
    private lateinit var home:File
    private lateinit var browser:LocalFileBrowser
    private val source=FileLocation(FileEndpoint.APP,"source")
    private val target=FileLocation(FileEndpoint.APP,"target")
    private val abc=FileContentDigest(3,"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")
    @Before fun setup() {
        root=File(context.cacheDir,"content-${UUID.randomUUID()}").apply { mkdirs() }
        home=File(root,"home").apply { mkdir() };File(home,"source").mkdir();File(home,"target").mkdir()
        File(home,"source/a").writeText("abc");File(home,"target/a").writeText("abc")
        browser=LocalFileBrowser(home,context.contentResolver)
    }
    @After fun cleanup() { root.deleteRecursively() }
    private fun connections(r:FilePasteRequest)=FileTaskConnections(r.source.endpoint,r.destination.endpoint,
        { _,_->error("local only") },{ _,_,_->error("no authentication") })
    private suspend fun plan():FileCopyPlan {
        File(home,"target/a").delete()
        val request=FilePasteRequest.capture(FileClipboardSnapshot.capture(source,"source",browser.browse(source).entries),target,"target")
        val scan=FileTreeScanner(browser::browse).scan(request)
        val access=FileDestinationAccess(home,context.contentResolver,browser::browse)
        val decisions=FilePasteDecisions.begin(FilePastePreflight(access::inspect,access::relation).check(scan))
        return decisions.confirm(decisions.state().reference)
    }
    private suspend fun saved(digest:FileContentDigest?=abc,candidate:String?=null):FileJournalRecord {
        val entry=browser.browse(source).entries.single()
        val item=FileJournalItem(FileCopyItemResult(FileScanEntry(source,entry,"a"),FileCopyItemState.UNKNOWN,
            target,"a",createdId="target/a",candidatePath=candidate,sourceDigest=digest),FileCopyCheckpoint.PUBLISH)
        return FileJournalRecord(FileJournalIntent(UUID.randomUUID().toString(),UUID.randomUUID().toString(),source,"source",listOf(entry),target,"target"),
            FileJournalPhase.RUNNING,0,items=listOf(item))
    }
    private suspend fun inspect(record:FileJournalRecord,candidate:Boolean=false):FileHistoryContentResult {
        val store=TransferStagingStore(File(root,"inspect"),freeSpaceReserve=0)
        try { return FileHistoryContentInspector(browser::browse) { location,entry ->
            browser.snapshot(location,entry,store).use { it.contentDigest() }
        }.inspect(record,record.items.single(),candidate) }
        finally { store.closeAndAwait() }
    }
    @Test fun realSnapshotDigestKeepsPublisherOffsetAndCapacityLease()=runBlocking<Unit> {
        val store=TransferStagingStore(File(root,"snapshots"),freeSpaceReserve=0)
        val snap=browser.snapshot(source,browser.browse(source).entries.single(),store)
        try {
            assertEquals(abc,snap.contentDigest());assertEquals(abc,snap.contentDigest())
            snap.withDescriptor { fd,_ -> ParcelFileDescriptor.fromFd(fd).use { copy -> assertEquals("abc",FileInputStream(copy.fileDescriptor).readBytes().toString(Charsets.UTF_8)) } }
            assertTrue(store.reservedBytes>=3)
        } finally { snap.close();store.closeAndAwait() }
        assertEquals(0L,store.reservedBytes)
        File(home,"source/a").writeBytes(byteArrayOf())
        val empty=TransferStagingStore(File(root,"empty"),freeSpaceReserve=0)
        try { browser.snapshot(source,browser.browse(source).entries.single(),empty).use {
            assertEquals(FileContentDigest(0,"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"),it.contentDigest())
        } } finally { empty.closeAndAwait() }
    }
    @Test fun multiChunkBinaryDigestIncludesEveryByte()=runBlocking<Unit> {
        val bytes=ByteArray(150123) { (it*31+7).toByte() }
        File(home,"source/a").writeBytes(bytes)
        val expected=java.security.MessageDigest.getInstance("SHA-256").digest(bytes).joinToString("") { "%02x".format(it.toInt() and 255) }
        val store=TransferStagingStore(File(root,"binary"),freeSpaceReserve=0)
        try { browser.snapshot(source,browser.browse(source).entries.single(),store).use { snapshot ->
            val actual=runCatching { snapshot.contentDigest() }
            assertTrue("multi-chunk hash must succeed: ${actual.exceptionOrNull()}",actual.isSuccess)
            assertEquals(FileContentDigest(bytes.size.toLong(),expected),actual.getOrThrow())
        } } finally { store.closeAndAwait() }
        assertEquals(0L,store.reservedBytes)
    }
    @Test fun digestCheckpointCancellationReleasesReaderWithoutDestroyingSnapshot()=runBlocking<Unit> {
        val store=TransferStagingStore(File(root,"snapshots"),freeSpaceReserve=0)
        try { browser.snapshot(source,browser.browse(source).entries.single(),store).use { snapshot ->
            var rejected=false
            try { snapshot.contentDigest { throw CancellationException("cancel") } } catch(_:CancellationException) { rejected=true }
            assertTrue(rejected);assertEquals(abc,snapshot.contentDigest())
        } } finally { store.closeAndAwait() }
        assertEquals(0L,store.reservedBytes)
    }
    @Test fun digestIsDurableBeforeCreationAndRetainedAcrossNewJournal()=runBlocking<Unit> {
        val plan=plan();val path=File(root,"history/tasks.db");val journal=FileTaskJournal(path)
        journal.load();journal.accept(plan.request);journal.phase(plan.request.id,FileJournalPhase.CONFIRMING)
        var checked=false;var emptyBeforeCreate=false
        val digestBeforeCreate=java.util.concurrent.atomic.AtomicReference<FileContentDigest?>()
        val run=FileCopyExecutor(home,context.contentResolver,File(root,"staging"),::connections,record={ value,point ->
            journal.checkpoint(plan.request.id,value,point)
            if(point==FileCopyCheckpoint.LOCAL_CREATE) {
                digestBeforeCreate.set(FileTaskJournal(path).load().single().items.single().value.sourceDigest)
                emptyBeforeCreate=File(home,"target").list()!!.isEmpty();checked=true
            }
        }).prepare(plan)
        journal.begin(plan.request.id,run.outcome.items)
        assertNull(journal.load().single().items.single().value.sourceDigest)
        val outcome=run.execute();run.awaitClosed()
        val finished=runCatching { journal.finish(plan.request.id,outcome,false,null) }
        assertTrue("Acknowledged result must be durably recorded: ${finished.exceptionOrNull()}",finished.isSuccess)
        assertTrue(checked && emptyBeforeCreate);assertEquals(abc,digestBeforeCreate.get())
        assertEquals(FileCopyState.COMPLETE,outcome.state)
        assertEquals(abc,FileTaskJournal(path).load().single().items.single().value.sourceDigest)
        assertEquals("abc",File(home,"target/a").readText())
        SQLiteDatabase.openDatabase(path.path,null,SQLiteDatabase.OPEN_READWRITE).use { db ->
            val payload=db.rawQuery("SELECT payload FROM items",null).use { it.moveToFirst();JSONObject(it.getBlob(0).toString(Charsets.UTF_8)) }
            assertEquals("3",payload.getString("version"))
            payload.getJSONObject("digest").put("sha256","invalid")
            val values=android.content.ContentValues().apply { put("payload",payload.toString().toByteArray()) };db.update("items",values,null,null)
        }
        var failure:Exception?=null
        try { FileTaskJournal(path).load() } catch(e:Exception) { failure=e }
        assertNotNull(failure);assertTrue(failure!!.message.orEmpty().contains("摘要"))
    }
    @Test fun digestRecordFailurePreventsEveryDestinationSideEffect()=runBlocking<Unit> {
        val run=FileCopyExecutor(home,context.contentResolver,File(root,"staging"),::connections,record={ value,_ ->
            if(value.sourceDigest!=null) throw IOException("digest record refused")
        }).prepare(plan())
        val result=run.execute();run.awaitClosed()
        assertNotNull(run.recordingError);assertNotEquals(FileCopyState.COMPLETE,result.state)
        assertTrue(File(home,"target").list()!!.isEmpty());assertEquals("abc",File(home,"source/a").readText())
    }
    @Test fun historicalMatchMismatchAndMissingNeverPromoteUnknownResult()=runBlocking<Unit> {
        val record=saved()
        assertEquals(FileHistoryContentState.MATCH,inspect(record).state)
        File(home,"target/a").writeText("abd")
        assertEquals(FileHistoryContentState.DIFFERENT,inspect(record).state)
        File(home,"target/a").delete()
        assertEquals(FileHistoryContentState.MISSING,inspect(record).state)
        assertEquals(FileCopyItemState.UNKNOWN,record.items.single().value.state)
    }
    @Test fun legacyWithoutReferenceDoesNotReadOrGuess()=runBlocking<Unit> {
        val record=saved(null)
        val result=FileHistoryContentInspector({ error("no browse") },{ _,_->error("no read") }).inspect(record,record.items.single())
        assertEquals(FileHistoryContentState.NO_REFERENCE,result.state);assertNull(result.actual)
    }
    @Test fun changedMetadataAndReadFailureCannotReportMatchingBytes()=runBlocking<Unit> {
        val record=saved();val entry=browser.browse(target).entries.single();var listings=0
        val inspector=FileHistoryContentInspector({ BrowserDirectory(it,listOf(if(listings++==0)entry else entry.copy(modifiedMillis=123))) },{ _,_->abc })
        assertEquals(FileHistoryContentState.UNKNOWN,inspector.inspect(record,record.items.single()).state)
        val failure=FileHistoryContentInspector(browser::browse) { _,_->throw IOException("read denied") }.inspect(record,record.items.single())
        assertEquals(FileHistoryContentState.UNKNOWN,failure.state);assertEquals("read denied",failure.error)
    }
    @Test fun symbolicLinksAndReplacementsAreNotFollowedByName()=runBlocking<Unit> {
        val record=saved();var reads=0
        val original=browser.browse(target).entries.single()
        val inspector=FileHistoryContentInspector({ BrowserDirectory(it,listOf(original.copy(id="target/other"))) },{ _,_->reads++;abc })
        assertEquals(FileHistoryContentState.MISSING,inspector.inspect(record,record.items.single()).state)
        val link=FileHistoryContentInspector({ BrowserDirectory(it,listOf(original.copy(kind=FileKind.LINK))) },{ _,_->reads++;abc })
        assertEquals(FileHistoryContentState.UNKNOWN,link.inspect(record,record.items.single()).state);assertEquals(0,reads)
    }
    @Test fun retainedCandidateContentIsIndependentOfFinalTargetAndIsNeverDeleted()=runBlocking<Unit> {
        File(home,"target/.candidate").writeText("ab")
        val record=saved(candidate="target/.candidate")
        assertEquals(FileHistoryContentState.MATCH,inspect(record).state)
        assertEquals(FileHistoryContentState.DIFFERENT,inspect(record,true).state)
        assertEquals("ab",File(home,"target/.candidate").readText());assertEquals(FileCopyItemState.UNKNOWN,record.items.single().value.state)
    }
}
