package com.hanxi.ghosterm

import android.content.ContentValues
import android.database.sqlite.SQLiteDatabase
import androidx.test.platform.app.InstrumentationRegistry
import com.hanxi.ghosterm.data.*
import com.hanxi.ghosterm.domain.*
import kotlinx.coroutines.runBlocking
import org.json.JSONObject
import org.junit.*
import org.junit.Assert.*
import java.io.File
import java.util.UUID

/** Persisted association, never a current inode/deletion authority or executable plan. */
class FileExecutionPlanTest {
    private val context=InstrumentationRegistry.getInstrumentation().targetContext
    private lateinit var root:File
    private lateinit var path:File
    private lateinit var journal:FileTaskJournal
    private val source=FileLocation(FileEndpoint.APP,"source")
    private val target=FileLocation(FileEndpoint.APP,"target")
    private val entry=BrowserEntry("source/a","a",FileKind.FILE,3u,null)
    @Before fun setup() {
        root=File(context.cacheDir,"execution-plan-${UUID.randomUUID()}").apply { mkdirs() }
        path=File(root,"history/tasks.db");journal=FileTaskJournal(path)
    }
    @After fun cleanup() { root.deleteRecursively() }
    private fun request(retry:String?=null)=FilePasteRequest.capture(FileClipboardSnapshot.capture(source,"source",listOf(entry)),target,"target",retryOf=retry)
    private fun item()=FileCopyItemResult(FileScanEntry(source,entry,"a"),FileCopyItemState.NOT_ATTEMPTED)
    private suspend fun accept(r:FilePasteRequest) { journal.load();journal.accept(r);journal.phase(r.id,FileJournalPhase.CONFIRMING) }
    private fun payload(id:String):ByteArray=SQLiteDatabase.openDatabase(path.path,null,SQLiteDatabase.OPEN_READONLY).use { db ->
        db.rawQuery("SELECT payload FROM tasks WHERE id=?",arrayOf(id)).use { check(it.moveToFirst());it.getBlob(0) }
    }
    private fun change(id:String,block:(JSONObject)->Unit) {
        val before=payload(id);val after=JSONObject(String(before,Charsets.UTF_8)).apply(block).toString().toByteArray(Charsets.UTF_8)
        SQLiteDatabase.openDatabase(path.path,null,SQLiteDatabase.OPEN_READWRITE).use { db ->
            db.beginTransaction()
            try {
                check(db.update("tasks",ContentValues().apply { put("payload",after) },"id=?",arrayOf(id))==1)
                db.execSQL("UPDATE budget SET bytes=bytes+? WHERE id=1",arrayOf(after.size-before.size))
                db.setTransactionSuccessful()
            } finally { db.endTransaction() }
        }
    }
    private suspend fun rejected(action:suspend()->Unit) {
        val error=try { action();null } catch(e:Exception) { e }
        assertNotNull("Operation must explicitly fail",error)
    }
    @Test fun planAndLineageSurviveNewStoreAndEveryTerminalPhase()=runBlocking<Unit> {
        val origin=UUID.randomUUID().toString();val r=request(origin);val plan=UUID.randomUUID().toString()
        accept(r)
        val registration=runCatching { journal.begin(r.id,listOf(item()),plan) }
        assertTrue("Plan registration must succeed: ${registration.exceptionOrNull()}",registration.isSuccess)
        var saved=FileTaskJournal(path).load().single()
        assertEquals(plan,saved.executionPlanId);assertEquals(origin,saved.intent.retryOf)
        assertEquals("3",JSONObject(String(payload(r.id))).getString("version"))
        journal.phase(r.id,FileJournalPhase.CANCELLING)
        assertEquals(plan,FileTaskJournal(path).load().single().executionPlanId)
        journal.finish(r.id,null,true,"cancelled")
        saved=FileTaskJournal(path).load().single()
        assertEquals(plan,saved.executionPlanId);assertEquals(origin,saved.intent.retryOf)
        assertTrue(saved.cancelled);assertEquals(FileJournalPhase.FINISHED,saved.phase)
        assertEquals(FileCopyItemState.NOT_ATTEMPTED,saved.items.single().value.state)
    }
    @Test fun oldHeadersDoNotGuessAPlanAndFreshPlansUseStrictVersionThree()=runBlocking<Unit> {
        val a=request();accept(a);journal.begin(a.id,listOf(item()))
        assertNull(FileTaskJournal(path).load().single().executionPlanId)
        assertEquals("1",JSONObject(String(payload(a.id))).getString("version"))
        val b=request(UUID.randomUUID().toString());accept(b)
        assertEquals("2",JSONObject(String(payload(b.id))).getString("version"))
        val plan=UUID.randomUUID().toString();journal.begin(b.id,listOf(item()),plan)
        val raw=JSONObject(String(payload(b.id)));assertEquals("3",raw.getString("version"));assertEquals(plan,raw.getString("plan"))
        assertNull(journal.load().single { it.intent.id==a.id }.executionPlanId)
    }
    @Test fun duplicateAndInvalidPlansFailBeforeAnyItemsAreInserted()=runBlocking<Unit> {
        val a=request();val b=request();val plan=UUID.randomUUID().toString()
        accept(a);journal.begin(a.id,listOf(item()),plan);accept(b)
        val before=journal.load()
        rejected { journal.begin(b.id,listOf(item()),plan) };assertEquals(before,journal.load())
        rejected { journal.begin(b.id,listOf(item()),"not-a-uuid") };assertEquals(before,journal.load())
        assertTrue(journal.load().single { it.intent.id==b.id }.items.isEmpty())
    }
    @Test fun failedPlanHeaderTransactionRollsBackItemsAndAssociation()=runBlocking<Unit> {
        val r=request();accept(r);val before=journal.load()
        SQLiteDatabase.openDatabase(path.path,null,SQLiteDatabase.OPEN_READWRITE).use {
            it.execSQL("CREATE TRIGGER reject_plan BEFORE UPDATE ON tasks BEGIN SELECT RAISE(ABORT,'plan refused'); END")
        }
        rejected { journal.begin(r.id,listOf(item()),UUID.randomUUID().toString()) }
        assertEquals(before,FileTaskJournal(path).load());assertTrue(journal.load().single().items.isEmpty())
    }
    @Test fun duplicateSavedPlansAreRejectedWithoutRepairOrDeletion()=runBlocking<Unit> {
        val a=request();val b=request();val plan=UUID.randomUUID().toString()
        accept(a);journal.begin(a.id,listOf(item()),plan);accept(b);journal.begin(b.id,listOf(item()),UUID.randomUUID().toString())
        change(b.id) { it.put("plan",plan) };val bytes=payload(b.id)
        rejected { FileTaskJournal(path).load() };assertArrayEquals(bytes,payload(b.id))
    }
    @Test fun registeredPlanRequiresCanonicalIdentityAndAnExecutionPhase()=runBlocking<Unit> {
        val r=request();accept(r);journal.begin(r.id,listOf(item()),UUID.randomUUID().toString())
        change(r.id) { it.put("phase","QUEUED") }
        SQLiteDatabase.openDatabase(path.path,null,SQLiteDatabase.OPEN_READWRITE).use {
            it.execSQL("UPDATE tasks SET phase='QUEUED' WHERE id=?",arrayOf(r.id))
        }
        rejected { journal.load() }
        change(r.id) { it.put("phase","RUNNING");it.put("plan","not-a-uuid") }
        SQLiteDatabase.openDatabase(path.path,null,SQLiteDatabase.OPEN_READWRITE).use {
            it.execSQL("UPDATE tasks SET phase='RUNNING' WHERE id=?",arrayOf(r.id))
        }
        rejected { journal.load() }
    }
    @Test fun missingPlanAndUnknownFieldsCannotDowngradeToLegacy()=runBlocking<Unit> {
        val r=request();accept(r);journal.begin(r.id,listOf(item()),UUID.randomUUID().toString())
        change(r.id) { it.remove("plan") };val bytes=payload(r.id)
        rejected { journal.load() };assertArrayEquals(bytes,payload(r.id))
        change(r.id) { it.put("plan",UUID.randomUUID().toString());it.put("authority",true) }
        rejected { journal.load() }
    }
}
