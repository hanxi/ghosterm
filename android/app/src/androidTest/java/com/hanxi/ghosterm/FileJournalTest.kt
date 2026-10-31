package com.hanxi.ghosterm

import android.database.sqlite.SQLiteDatabase
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.hanxi.ghosterm.data.*
import com.hanxi.ghosterm.domain.*
import kotlinx.coroutines.*
import org.junit.*
import org.junit.Assert.*
import org.junit.runner.RunWith
import java.io.File
import java.util.UUID

@RunWith(AndroidJUnit4::class)
class FileJournalTest {
    private val context=InstrumentationRegistry.getInstrumentation().targetContext
    private lateinit var root: File
    private lateinit var home: File
    private lateinit var path: File
    private lateinit var journal: FileTaskJournal
    private val source=FileLocation(FileEndpoint.APP,"source")
    private val target=FileLocation(FileEndpoint.APP,"target")
    @Before fun setup() {
        root=File(context.cacheDir,"journal-${UUID.randomUUID()}").apply { mkdirs() }
        home=File(root,"home").apply { mkdir() };File(home,"source").mkdir();File(home,"target").mkdir()
        File(home,"source/a.txt").writeText("journal bytes 中文\n")
        path=File(root,"history/tasks.db");journal=FileTaskJournal(path)
    }
    @After fun cleanup() { root.deleteRecursively() }
    private suspend fun plan(): FileCopyPlan {
        val browser=LocalFileBrowser(home,context.contentResolver)
        val request=FilePasteRequest.capture(FileClipboardSnapshot.capture(source,"source",browser.browse(source).entries),target,"target")
        val scan=FileTreeScanner(browser::browse).scan(request)
        val access=FileDestinationAccess(home,context.contentResolver,browser::browse)
        val decisions=FilePasteDecisions.begin(FilePastePreflight(access::inspect,access::relation).check(scan))
        return decisions.confirm(decisions.state().reference)
    }
    private fun connections(request: FilePasteRequest)=FileTaskConnections(request.source.endpoint,request.destination.endpoint,
        { _,_->error("local only") },{ _,_,_->error("no authentication") })
    private suspend fun prepared(plan: FileCopyPlan,record:suspend(FileCopyItemResult,FileCopyCheckpoint)->Unit={ value,point -> journal.checkpoint(plan.request.id,value,point) }):FileCopyExecutor.Request {
        journal.load();journal.accept(plan.request);journal.phase(plan.request.id,FileJournalPhase.CONFIRMING)
        val run=FileCopyExecutor(home,context.contentResolver,File(root,"staging"),::connections,record=record).prepare(plan)
        journal.begin(plan.request.id,run.outcome.items)
        return run
    }
    private suspend fun rejected(action: suspend ()->Unit) {
        var failure:Exception?=null
        try { action() } catch(error:Exception) { failure=error }
        assertNotNull("operation must fail explicitly",failure)
    }
    private fun sql(statement:String) { SQLiteDatabase.openDatabase(path.path,null,SQLiteDatabase.OPEN_READWRITE).use { it.execSQL(statement) } }

    @Test fun intentAndTerminalResultsSurviveNewStoreAndClearingDoesNotDeleteFiles()=runBlocking<Unit> {
        val plan=plan();val run=prepared(plan)
        val before=FileTaskJournal(path).load().single()
        assertTrue(before.interrupted);assertEquals(plan.request.id,before.intent.id)
        assertEquals(plan.request.items,before.intent.selected)
        assertEquals(FileCopyItemState.NOT_ATTEMPTED,before.items.single().value.state)
        val result=withTimeout(15000) { run.execute() };run.awaitClosed()
        assertEquals(FileCopyState.COMPLETE,result.state)
        journal.finish(plan.request.id,result,false,null)
        val recovered=FileTaskJournal(path).load().single()
        assertFalse(recovered.interrupted);assertEquals(FileCopyState.COMPLETE,recovered.outcome)
        assertEquals(result.items,recovered.items.map { it.value })
        journal.remove(plan.request.id);assertTrue(journal.load().isEmpty())
        assertTrue("clearing history must retain the published file",File(home,"target/a.txt").isFile)
        assertEquals("journal bytes 中文\n",File(home,"target/a.txt").readText())
        assertEquals("journal bytes 中文\n",File(home,"source/a.txt").readText())
    }
    @Test fun localIntentPrecedesCandidateAndPublicationAndRecordsActualResult()=runBlocking<Unit> {
        val plan=plan();val seen=mutableListOf<FileCopyCheckpoint>()
        var createIdentity=false;var absentAtCreate=false;var candidateAtPublish=false;var absentAtPublish=false
        val saved=mutableListOf<Pair<FileCopyItemResult,FileCopyItemResult>>()
        val run=prepared(plan) { value,point ->
            journal.checkpoint(plan.request.id,value,point);seen+=point
            when(point) {
                FileCopyCheckpoint.LOCAL_CREATE->{
                    createIdentity=value.candidatePath!=null
                    absentAtCreate=value.candidatePath?.let { !File(it).exists() } ?: false
                }
                FileCopyCheckpoint.PUBLISH->{
                    candidateAtPublish=value.candidatePath?.let { File(it).isFile } ?: false
                    absentAtPublish=!File(home,"target/a.txt").exists()
                }
                else->Unit
            }
            saved+=value to FileTaskJournal(path).load().single().items.single().value
        }
        val result=withTimeout(15000) { run.execute() };run.awaitClosed()
        assertEquals(FileCopyState.COMPLETE,result.state)
        assertTrue("candidate identity must precede creation",createIdentity && absentAtCreate)
        assertTrue("publication intent must precede rename",candidateAtPublish && absentAtPublish)
        saved.forEach { (actual,recorded) -> assertEquals(actual,recorded) }
        assertTrue("local create checkpoint must be reached",FileCopyCheckpoint.LOCAL_CREATE in seen)
        assertTrue("publication checkpoint must be reached",FileCopyCheckpoint.PUBLISH in seen)
        assertEquals(FileCopyItemState.COMPLETE,journal.load().single().items.single().value.state)
        assertNull(journal.load().single().items.single().value.candidatePath)
    }
    @Test fun failedPublicationIntentStopsRenameAndKeepsPreviousRecord()=runBlocking<Unit> {
        val plan=plan();val run=prepared(plan)
        sql("CREATE TRIGGER reject_publish BEFORE UPDATE ON items WHEN instr(CAST(NEW.payload AS TEXT), '\"checkpoint\":\"PUBLISH\"')>0 BEGIN SELECT RAISE(ABORT,'journal write refused'); END")
        val result=withTimeout(15000) { run.execute() };run.awaitClosed()
        assertNotNull("record failure must be visible",run.recordingError)
        assertFalse("failed journal commit must prevent publication",File(home,"target/a.txt").exists())
        assertNotEquals(FileCopyState.COMPLETE,result.state)
        val saved=FileTaskJournal(path).load().single()
        assertTrue(saved.interrupted);assertEquals(FileCopyCheckpoint.LOCAL_CREATE,saved.items.single().checkpoint)
        assertEquals("journal bytes 中文\n",File(home,"source/a.txt").readText())
    }
    @Test fun failedCompletionRecordPreservesActualPublishedResultWithoutRetrying()=runBlocking<Unit> {
        val plan=plan();val run=prepared(plan)
        sql("CREATE TRIGGER reject_complete BEFORE UPDATE ON items WHEN instr(CAST(NEW.payload AS TEXT), '\"state\":\"COMPLETE\"')>0 BEGIN SELECT RAISE(ABORT,'final record refused'); END")
        val result=withTimeout(15000) { run.execute() };run.awaitClosed()
        assertNotNull(run.recordingError)
        assertEquals("actual success cannot be rewritten as an unattempted/unknown file",FileCopyItemState.COMPLETE,result.items.single().state)
        assertEquals("journal bytes 中文\n",File(home,"target/a.txt").readText())
        assertEquals(FileCopyCheckpoint.PUBLISH,FileTaskJournal(path).load().single().items.single().checkpoint)
        rejected { journal.finish(plan.request.id,result,false,run.recordingError) }
        assertTrue(journal.load().single().interrupted)
        sql("DROP TRIGGER reject_complete")
        journal.finish(plan.request.id,result,false,run.recordingError)
        assertEquals(FileCopyItemState.COMPLETE,journal.load().single().items.single().value.state)
        assertNotNull(journal.load().single().error)
        assertEquals(listOf("a.txt"),File(home,"target").list()!!.toList())
    }
    @Test fun failedPlanTransactionKeepsOriginalIntentAndNoPartialItems()=runBlocking<Unit> {
        File(home,"source/b.txt").writeText("second")
        val plan=plan();journal.load();journal.accept(plan.request);journal.phase(plan.request.id,FileJournalPhase.CONFIRMING)
        sql("CREATE TRIGGER reject_second BEFORE INSERT ON items WHEN NEW.path='b.txt' BEGIN SELECT RAISE(ABORT,'plan refused'); END")
        val values=plan.check.scan.entries.map { FileCopyItemResult(it,FileCopyItemState.NOT_ATTEMPTED) }
        rejected { journal.begin(plan.request.id,values) }
        val restored=runCatching { FileTaskJournal(path).load() }
        assertTrue("rollback must leave a readable original record: ${restored.exceptionOrNull()}",restored.isSuccess)
        val saved=restored.getOrThrow().single()
        assertEquals(FileJournalPhase.CONFIRMING,saved.phase);assertTrue("partial plan must roll back",saved.items.isEmpty())
        assertTrue(File(home,"target").list()!!.isEmpty())
        sql("DROP TRIGGER reject_second");journal.begin(plan.request.id,values)
        assertEquals(2,journal.load().single().items.size)
    }
    @Test fun corruptOrUnknownDatabaseIsPreservedAndNeverBecomesEmptySuccess()=runBlocking<Unit> {
        val plan=plan();journal.load();journal.accept(plan.request)
        sql("PRAGMA user_version=73")
        val versionBytes=path.readBytes()
        rejected { FileTaskJournal(path).load() };assertTrue("unknown version must be preserved",path.exists());assertArrayEquals(versionBytes,path.readBytes())
        path.writeBytes(ByteArray(512) { 0x7f })
        val corrupt=path.readBytes()
        rejected { FileTaskJournal(path).load() }
        assertTrue("corrupt database must not be deleted",path.exists());assertArrayEquals(corrupt,path.readBytes())
    }
    @Test fun metadataCorruptionAndOversizePlanFailWithoutLosingIntent()=runBlocking<Unit> {
        val unicode="中文-☃-😀.txt"
        assertTrue(File(home,"source/a.txt").renameTo(File(home,"source/$unicode")))
        val plan=plan();journal.load();journal.accept(plan.request);journal.phase(plan.request.id,FileJournalPhase.CONFIRMING)
        val source=plan.check.scan.entries.single()
        assertEquals(unicode,journal.load().single().intent.selected.single().name)
        for(malformed in listOf("name-\uD800","name-\uDC00")) {
            val invalid=FilePasteRequest.capture(FileClipboardSnapshot.capture(plan.request.source,malformed,plan.request.items),
                plan.request.destination,plan.request.destinationPathLabel)
            rejected { journal.accept(invalid) }
            assertEquals("invalid Unicode must not admit a changed intent",listOf(plan.request.id),journal.load().map { it.intent.id })
            rejected { journal.begin(plan.request.id,listOf(FileCopyItemResult(
                source.copy(entry=source.entry.copy(name=malformed)),FileCopyItemState.NOT_ATTEMPTED))) }
            assertEquals(FileJournalPhase.CONFIRMING,journal.load().single().phase)
            assertTrue("invalid Unicode must not commit a changed item",journal.load().single().items.isEmpty())
        }
        rejected { journal.begin(plan.request.id,listOf(FileCopyItemResult(source.copy(relativePath="x".repeat(70000)),FileCopyItemState.NOT_ATTEMPTED))) }
        assertEquals(FileJournalPhase.CONFIRMING,journal.load().single().phase)
        assertTrue(journal.load().single().items.isEmpty())
        journal.begin(plan.request.id,listOf(FileCopyItemResult(source,FileCopyItemState.NOT_ATTEMPTED)))
        assertEquals(unicode,journal.load().single().items.single().value.source.entry.name)
        sql("UPDATE budget SET bytes=bytes+1")
        rejected { FileTaskJournal(path).load() }
        assertTrue(path.isFile)
    }
    @Test fun directoryCheckpointsRetainCreationAndStopBeforeChildWrites()=runBlocking<Unit> {
        File(home,"source/a.txt").delete();File(home,"source/folder").mkdir();File(home,"source/folder/child").writeText("child")
        val plan=plan();var before=false;var after=false
        val run=prepared(plan) { value,point ->
            journal.checkpoint(plan.request.id,value,point)
            if(point==FileCopyCheckpoint.DIRECTORY_CREATE) before=!File(home,"target/folder").exists()
            if(point==FileCopyCheckpoint.DIRECTORY_CREATED) {
                after=File(home,"target/folder").isDirectory
                throw java.io.IOException("stop after created directory record")
            }
        }
        val result=withTimeout(15000) { run.execute() };run.awaitClosed()
        assertTrue("directory intent precedes mkdir",before);assertTrue("created directory is observed",after)
        assertNotNull(run.recordingError);assertTrue(File(home,"target/folder").isDirectory)
        assertFalse(File(home,"target/folder/child").exists())
        assertEquals(FileCopyItemState.CREATED,result.items.first { it.source.relativePath=="folder" }.state)
        val saved=journal.load().single().items.first { it.value.source.relativePath=="folder" }
        assertEquals(FileCopyCheckpoint.DIRECTORY_CREATED,saved.checkpoint);assertEquals("target/folder",saved.value.createdId)
    }
    @Test fun totalMetadataBudgetRollsBackLargePlanBeforeAnyFileWrite()=runBlocking<Unit> {
        val plan=plan();journal.load();journal.accept(plan.request);journal.phase(plan.request.id,FileJournalPhase.CONFIRMING)
        val node=plan.check.scan.entries.single()
        val values=(0 until 1200).map { i ->
            FileCopyItemResult(node.copy(entry=node.entry.copy(id="id-$i-"+"x".repeat(16000),name="item-$i"),relativePath="item-$i"),FileCopyItemState.NOT_ATTEMPTED)
        }
        rejected { journal.begin(plan.request.id,values) }
        val saved=journal.load().single();assertEquals(FileJournalPhase.CONFIRMING,saved.phase);assertTrue(saved.items.isEmpty())
        assertTrue(File(home,"target").list()!!.isEmpty())
    }
    @Test fun deeplyNestedAndNonCanonicalRecordsFailBeforeUnboundedParsing()=runBlocking<Unit> {
        val plan=plan();journal.load();journal.accept(plan.request)
        val bodies=listOf("{\"a\":".repeat(5000)+"0"+"}".repeat(5000),"{'a':'\"', 'deep':"+"[".repeat(5000)+"0"+"]".repeat(5000)+"}")
        for(body in bodies) {
            SQLiteDatabase.openDatabase(path.path,null,SQLiteDatabase.OPEN_READWRITE).use {
                it.update("tasks",android.content.ContentValues().apply { put("payload",body.toByteArray()) },null,null)
            }
            var failure:Throwable?=null
            try { journal.load() } catch(error:Throwable) { failure=error }
            assertTrue("bounded JSON rejection, never stack overflow: $failure",failure is IllegalArgumentException)
            assertTrue("explicit pre-parser bound or strict JSON rejection",failure!!.message!!.contains("嵌套过深") || failure.message!!.contains("不是有效 JSON"))
            assertTrue(path.exists())
        }
    }
    @Test fun historyLimitDoesNotEvictOrAdmitAnUnrecordedRequest()=runBlocking<Unit> {
        journal.load()
        repeat(FileTaskJournal.MAX_RECORDS) { journal.accept(plan().request) }
        val ids=journal.load().map { it.intent.id }.toSet()
        rejected { journal.accept(plan().request) }
        assertEquals(FileTaskJournal.MAX_RECORDS,ids.size);assertEquals(ids,journal.load().map { it.intent.id }.toSet())
        journal.remove(ids.first());journal.accept(plan().request)
        assertEquals(FileTaskJournal.MAX_RECORDS,journal.load().size)
        assertTrue(File(home,"target").list()!!.isEmpty())
    }
}
