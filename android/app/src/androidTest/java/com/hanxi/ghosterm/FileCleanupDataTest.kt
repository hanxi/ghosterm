package com.hanxi.ghosterm

import android.database.sqlite.SQLiteDatabase
import android.os.ParcelFileDescriptor
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.hanxi.ghosterm.data.*
import com.hanxi.ghosterm.domain.*
import kotlinx.coroutines.*
import org.junit.*
import org.junit.Assert.*
import org.junit.runner.RunWith
import java.io.File
import java.nio.file.Files
import java.nio.file.attribute.FileTime
import java.util.UUID

@RunWith(AndroidJUnit4::class)
class FileCleanupDataTest {
    private val context=InstrumentationRegistry.getInstrumentation().targetContext
    private lateinit var root:File
    private lateinit var home:File
    private lateinit var journal:FileCleanupJournal
    private lateinit var cleaner:FileResidualCleanup
    private val destination=FileLocation(FileEndpoint.APP,"target")
    @Before fun setup() {
        root=File(context.cacheDir,"cleanup-${UUID.randomUUID()}").apply { mkdirs() }
        home=File(root,"home").apply { mkdir() };File(home,"target").mkdir()
        journal=FileCleanupJournal(File(root,"journal/operations.db"));cleaner=FileResidualCleanup(home,context.contentResolver,journal)
    }
    @After fun finish() { root.deleteRecursively() }
    private suspend fun record(name:String):FileJournalRecord {
        val target=LocalFileBrowser(home,context.contentResolver).browse(destination).entries.single { it.name==name }
        val source=BrowserEntry("source",name,target.kind,target.size,target.modifiedMillis)
        val location=FileLocation(FileEndpoint.APP)
        val request=FilePasteRequest.capture(FileClipboardSnapshot.capture(location,"source",listOf(source)),destination,"target")
        val item=FileJournalItem(FileCopyItemResult(FileScanEntry(location,source,name),FileCopyItemState.UNKNOWN,destination,name,createdId=target.id),FileCopyCheckpoint.PUBLISH)
        return FileJournalRecord(FileJournalIntent(request.id,request.clipboardId,location,"source",listOf(source),destination,"target"),FileJournalPhase.RUNNING,0,items=listOf(item))
    }
    private fun intent()=FileCleanupRecord(UUID.randomUUID().toString(),UUID.randomUUID().toString(),"a",false,"App","target","target/a","a",false,1,1)
    @Test fun reviewNeverDeletesAndExplicitRemovalIsDurableOneShotAndDoesNotRewriteCopy()=runBlocking {
        val file=File(home,"target/a").apply { writeText("keep until confirmed") };val old=record("a");val saved=old.copy(items=old.items.toList())
        val review=cleaner.review(old,old.items.single(),false)
        assertTrue("review must not delete",file.exists());assertTrue("review must not create delete intent",journal.load().isEmpty())
        val result=cleaner.remove(review)
        assertEquals(FileCleanupState.REMOVED,result.state);assertEquals(0,result.status);assertFalse(file.exists())
        assertEquals(listOf(result),FileCleanupJournal(File(root,"journal/operations.db")).load());assertEquals(saved,old)
        assertTrue(runCatching { cleaner.remove(review) }.exceptionOrNull() is IllegalArgumentException)
    }
    @Test fun metadataChangeAndIdenticalByteReplacementArePreserved()=runBlocking {
        for(replacement in listOf(false,true)) {
            val file=File(home,"target/a").apply { writeText("original") };val old=record("a");val review=cleaner.review(old,old.items.single(),false)
            ParcelFileDescriptor.open(file,ParcelFileDescriptor.MODE_READ_ONLY).use {
                if(replacement) { val time=file.lastModified();check(file.delete());file.writeText("original");Files.setLastModifiedTime(file.toPath(),FileTime.fromMillis(time)) }
                else file.writeText("changed size")
                val result=cleaner.remove(review)
                assertEquals("fresh metadata and local inode must both be checked",FileCleanupState.CHANGED,result.state)
                assertEquals(if(replacement)"original" else "changed size",file.readText())
            }
            file.delete()
        }
    }
    @Test fun directoriesAreNonRecursiveAndLinksAreNeverFollowed()=runBlocking {
        val empty=File(home,"target/empty").apply { mkdir() };val old=record("empty")
        assertEquals(FileCleanupState.REMOVED,cleaner.remove(cleaner.review(old,old.items.single(),false)).state);assertFalse(empty.exists())
        val directory=File(home,"target/full").apply { mkdir() };val child=File(directory,"keep").apply { writeText("safe") }
        val full=record("full");val result=cleaner.remove(cleaner.review(full,full.items.single(),false))
        assertNotEquals(FileCleanupState.REMOVED,result.state);assertEquals("safe",child.readText())
        Files.createSymbolicLink(File(home,"target/link").toPath(),child.toPath())
        val linked=record("link");assertTrue(runCatching { cleaner.review(linked,linked.items.single(),false) }.isFailure)
        assertEquals("safe",child.readText())
    }
    @Test fun disappearedPositionIsAnObservationAndForeignReviewsCannotDelete()=runBlocking {
        val file=File(home,"target/a").apply { writeText("safe") };val old=record("a");val review=cleaner.review(old,old.items.single(),false)
        val foreign=FileResidualCleanup(home,context.contentResolver,journal)
        assertTrue(runCatching { foreign.remove(review) }.exceptionOrNull() is IllegalArgumentException);assertTrue(file.exists())
        file.delete();val result=cleaner.remove(review);assertEquals(FileCleanupState.ABSENT,result.state)
        assertEquals(FileCleanupState.ABSENT,journal.load().single().state)
    }
    @Test fun durableUnknownIntentNeverReplaysAndResultsCannotBeReassignedOrOverwritten()=runBlocking {
        val file=File(home,"target/a").apply { writeText("safe") };val first=intent();journal.begin(first)
        val reopened=FileCleanupJournal(File(root,"journal/operations.db"))
        assertEquals(first,reopened.load().single());assertTrue(file.exists())
        val ack=first.copy(state=FileCleanupState.REMOVED,status=0,updatedMillis=2)
        assertTrue(runCatching { reopened.finish(ack.copy(entryId="other")) }.isFailure)
        reopened.finish(ack);assertEquals(ack,reopened.load().single())
        assertTrue(runCatching { reopened.finish(first.copy(state=FileCleanupState.REJECTED,status=3)) }.isFailure)
        reopened.remove(first.id);assertTrue(reopened.load().isEmpty());assertEquals("safe",file.readText())
    }
    @Test fun fullOrUnknownVersionJournalPreventsDeletionAndPreservesRecords()=runBlocking {
        repeat(FileCleanupJournal.MAX_RECORDS) { journal.begin(intent()) }
        val file=File(home,"target/a").apply { writeText("safe") };val old=record("a");val review=cleaner.review(old,old.items.single(),false)
        assertTrue(runCatching { cleaner.remove(review) }.isFailure);assertTrue("quota must stop deletion",file.exists());assertEquals("safe",file.readText());assertEquals(FileCleanupJournal.MAX_RECORDS,journal.load().size)
        SQLiteDatabase.openDatabase(File(root,"journal/operations.db").path,null,SQLiteDatabase.OPEN_READWRITE).use { it.version=2 }
        assertTrue(runCatching { journal.load() }.isFailure);assertEquals("safe",file.readText())
        SQLiteDatabase.openDatabase(File(root,"journal/operations.db").path,null,SQLiteDatabase.OPEN_READONLY).use { assertEquals(2,it.version) }
    }
}
