package com.hanxi.ghosterm

import android.content.ContentValues
import android.database.sqlite.SQLiteDatabase
import android.system.Os
import androidx.test.platform.app.InstrumentationRegistry
import com.hanxi.ghosterm.data.*
import com.hanxi.ghosterm.domain.*
import kotlinx.coroutines.*
import org.json.JSONObject
import org.junit.*
import org.junit.Assert.*
import java.io.File
import java.io.IOException
import java.util.UUID

class LocalFileIdentityTest {
    private val context=InstrumentationRegistry.getInstrumentation().targetContext
    private lateinit var root:File
    private lateinit var home:File
    private lateinit var tasks:File
    private lateinit var owner:File
    private lateinit var stage:File
    private lateinit var target:File
    private lateinit var candidate:File
    private lateinit var reference:LocalFileIdentityReference
    private val key="user.ghosterm.copy"
    private val abc=FileContentDigest(3,"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")
    private fun dir(file:File)=file.apply { mkdirs();Os.chmod(path,448) }
    @Before fun setup() {
        root=File(context.cacheDir,"file-identity-${UUID.randomUUID()}").apply { mkdirs() }
        home=dir(File(root,"home"));target=dir(File(home,"target"))
        tasks=dir(File(root,"file-tasks"));owner=dir(File(tasks,UUID.randomUUID().toString()));stage=dir(File(owner,"destination"))
        candidate=File(stage,UUID.randomUUID().toString()).apply { writeText("abc");Os.chmod(path,384) }
        val token=UUID.randomUUID().toString();Os.setxattr(candidate.path,key,token.toByteArray(Charsets.US_ASCII),1)
        val file=Os.lstat(candidate.path);val from=Os.lstat(stage.path);val to=Os.lstat(target.path)
        reference=LocalFileIdentityReference(token,candidate.name,file.st_dev,file.st_ino,file.st_uid,from.st_dev,from.st_ino,to.st_dev,to.st_ino)
    }
    @After fun cleanup() { root.deleteRecursively() }
    private fun record(identity:LocalFileIdentityReference?=reference,path:String?=candidate.absolutePath):FileJournalRecord {
        val source=FileLocation(FileEndpoint.APP,"");val destination=FileLocation(FileEndpoint.APP,"target")
        val entry=BrowserEntry("a","a",FileKind.FILE,3u,null)
        val value=FileCopyItemResult(FileScanEntry(source,entry,"a"),FileCopyItemState.UNKNOWN,destination,"a",
            candidatePath=path,sourceDigest=abc,localIdentity=identity)
        return FileJournalRecord(FileJournalIntent(UUID.randomUUID().toString(),UUID.randomUUID().toString(),source,"home",listOf(entry),destination,"target"),
            FileJournalPhase.RUNNING,0,items=listOf(FileJournalItem(value,FileCopyCheckpoint.PUBLISH)),executionPlanId=owner.name)
    }
    private fun fds()=File("/proc/self/fd").listFiles()!!.count { runCatching { Os.readlink(it.path).contains("/${root.name}/") }.getOrDefault(false) }
    private suspend fun inspect(saved:FileJournalRecord=record(),privateCandidate:Boolean=false,checkpoint:()->Unit={}):LocalFileIdentityResult =
        LocalFileIdentityReader(home,tasks,checkpoint).inspect(saved,saved.items.single(),privateCandidate)
    @Test fun markerIdentifiesUnacknowledgedRenameWithoutPromotingTheOldResult()=runBlocking<Unit> {
        val saved=record()
        assertEquals(LocalFileIdentityState.SAME,inspect(saved,true).state)
        assertEquals(LocalFileIdentityState.MISSING,inspect(saved).state)
        assertTrue(candidate.renameTo(File(target,"a")))
        assertEquals(LocalFileIdentityState.SAME,inspect(saved).state)
        assertEquals(LocalFileIdentityState.MISSING,inspect(saved,true).state)
        assertEquals(FileCopyItemState.UNKNOWN,saved.items.single().value.state);assertEquals(0,fds())
    }
    @Test fun originalObjectRemainsIdentifiableAfterItsContentChanges()=runBlocking<Unit> {
        assertTrue(candidate.renameTo(File(target,"a")));File(target,"a").writeText("abd")
        assertEquals(LocalFileIdentityState.SAME,inspect().state)
        assertEquals("abd",File(target,"a").readText());assertEquals(0,fds())
    }
    @Test fun copiedMarkerAndIdenticalBytesCannotIdentifyAnotherInode()=runBlocking<Unit> {
        assertTrue(candidate.renameTo(File(target,"a")))
        val replacement=File(home,"replacement").apply { writeText("abc");Os.chmod(path,384) }
        Os.setxattr(replacement.path,key,reference.token.toByteArray(Charsets.US_ASCII),1)
        assertNotEquals(reference.inode,Os.lstat(replacement.path).st_ino)
        Os.rename(replacement.path,File(target,"a").path)
        assertEquals(LocalFileIdentityState.DIFFERENT,inspect().state);assertEquals(0,fds())
    }
    @Test fun absentOrDifferentMarkerIsNotTheRecordedCreation()=runBlocking<Unit> {
        assertTrue(candidate.renameTo(File(target,"a")));val file=File(target,"a")
        Os.removexattr(file.path,key);assertEquals(LocalFileIdentityState.DIFFERENT,inspect().state)
        Os.setxattr(file.path,key,"different".toByteArray(),1)
        assertEquals(LocalFileIdentityState.DIFFERENT,inspect().state);assertEquals(0,fds())
    }
    @Test fun sameObjectMovedIntoAReplacementDirectoryDoesNotMatchTheOldPosition()=runBlocking<Unit> {
        assertTrue(candidate.renameTo(File(target,"a")))
        val moved=File(home,"old-target");assertTrue(target.renameTo(moved));dir(target)
        assertTrue(File(moved,"a").renameTo(File(target,"a")))
        assertEquals(reference.inode,Os.lstat(File(target,"a").path).st_ino)
        assertEquals(LocalFileIdentityState.DIFFERENT,inspect().state);assertEquals(0,fds())
    }
    @Test fun parentReplacementWhileReadingCannotUseTheOldPinnedObject()=runBlocking<Unit> {
        assertTrue(candidate.renameTo(File(target,"a")));var moved=false
        val result=inspect(checkpoint={
            if(!moved) {
                moved=target.renameTo(File(home,"old-target"));check(moved);dir(target)
                File(target,"a").writeText("abc")
            }
        })
        assertTrue(moved);assertEquals(LocalFileIdentityState.UNKNOWN,result.state);assertEquals(0,fds())
    }
    @Test fun legacyAndInvalidPathsNeverReadAnotherPrivateNamespace()=runBlocking<Unit> {
        var reads=0
        assertEquals(LocalFileIdentityState.NO_REFERENCE,inspect(record(identity=null),true) { reads++ }.state)
        assertEquals(LocalFileIdentityState.UNKNOWN,inspect(record(identity=reference.copy(uid=reference.uid+1)),true) { reads++ }.state)
        val saved=record()
        assertEquals(LocalFileIdentityState.UNKNOWN,inspect(saved.copy(executionPlanId=UUID.randomUUID().toString()),true) { reads++ }.state)
        val foreign=saved.copy(intent=saved.intent.copy(destination=FileLocation(FileEndpoint("foreign","foreign",""),"target")))
        assertEquals(LocalFileIdentityState.UNKNOWN,inspect(foreign,true) { reads++ }.state)
        val wrongId=saved.copy(items=listOf(saved.items.single().copy(value=saved.items.single().value.copy(createdId="elsewhere/a"))))
        assertEquals(LocalFileIdentityState.UNKNOWN,inspect(wrongId,false) { reads++ }.state)
        val bad=listOf(File(root,"outside").absolutePath,candidate.absolutePath.replace("destination","source"),
            candidate.absolutePath+"/",File(stage,UUID.randomUUID().toString()).absolutePath)
        for(path in bad)assertEquals(LocalFileIdentityState.UNKNOWN,inspect(record(path=path),true) { reads++ }.state)
        assertEquals(0,reads);assertEquals(0,fds())
    }
    @Test fun unreadableObjectsRemainUnknownInsteadOfMatchingOrMissing()=runBlocking<Unit> {
        Os.chmod(candidate.path,0)
        try { assertEquals(LocalFileIdentityState.UNKNOWN,inspect(privateCandidate=true).state);assertEquals(0,fds()) }
        finally { Os.chmod(candidate.path,384) }
    }
    @Test fun createOnlyMarkerCannotReplaceAnExistingReference() {
        val type=Class.forName("com.hanxi.ghosterm.data.LocalCreationMarker")
        val instance=type.getField("INSTANCE").get(null)
        val method=type.getDeclaredMethod("create",java.io.FileDescriptor::class.java,java.io.FileDescriptor::class.java,
            java.io.FileDescriptor::class.java,String::class.java)
        com.hanxi.ghosterm.bridge.LocalPublicationFile.directory(stage,"").use { from ->
            com.hanxi.ghosterm.bridge.LocalPublicationFile.directory(target,"").use { to ->
                val name=UUID.randomUUID().toString()
                com.hanxi.ghosterm.bridge.LocalPublicationFile.candidate(from.fd,name).use { file ->
                    val first=method.invoke(instance,file.fileDescriptor,from.fileDescriptor,to.fileDescriptor,name) as LocalFileIdentityReference
                    var error:Exception?=null
                    try { method.invoke(instance,file.fileDescriptor,from.fileDescriptor,to.fileDescriptor,name) } catch(e:Exception) { error=e }
                    assertNotNull("Create-only marker must preserve the first reference",error)
                    assertEquals(first.token,Os.getxattr(File(stage,name).path,key).toString(Charsets.US_ASCII))
                }
            }
        }
        assertEquals(0,fds())
    }
    @Test fun linksAreNeverFollowedAndCancellationReleasesEveryHandle()=runBlocking<Unit> {
        Os.symlink(candidate.path,File(target,"a").path)
        assertEquals(LocalFileIdentityState.DIFFERENT,inspect().state)
        var cancelled=false
        try { inspect(privateCandidate=true) { throw CancellationException("cancel") } } catch(_:CancellationException) { cancelled=true }
        assertTrue(cancelled);assertEquals(0,fds());assertEquals("abc",candidate.readText())
    }
    private suspend fun plan():FileCopyPlan {
        File(home,"a").writeText("abc")
        val browser=LocalFileBrowser(home,context.contentResolver);val source=FileLocation(FileEndpoint.APP,"")
        val request=FilePasteRequest.capture(FileClipboardSnapshot.capture(source,"home",browser.browse(source).entries.filter { it.name=="a" }),FileLocation(FileEndpoint.APP,"target"),"target")
        val access=FileDestinationAccess(home,context.contentResolver,browser::browse)
        val decisions=FilePasteDecisions.begin(FilePastePreflight(access::inspect,access::relation).check(FileTreeScanner(browser::browse).scan(request)))
        return decisions.confirm(decisions.state().reference)
    }
    private fun connections(r:FilePasteRequest)=FileTaskConnections(r.source.endpoint,r.destination.endpoint,{ _,_->error("local only") },{ _,_,_->error("no auth") })
    @Test fun creationReferenceCommitsBeforePayloadAndSurvivesPublication()=runBlocking<Unit> {
        val plan=plan();val journal=FileTaskJournal(File(root,"history/tasks.db"))
        journal.load();journal.accept(plan.request);journal.phase(plan.request.id,FileJournalPhase.CONFIRMING)
        var observed=false;var empty=false;var durable=false;var markerMatches=false
        val run=FileCopyExecutor(home,context.contentResolver,tasks,::connections,record={ value,point ->
            journal.checkpoint(plan.request.id,value,point)
            if(point==FileCopyCheckpoint.LOCAL_CREATED) {
                observed=true;empty=File(checkNotNull(value.candidatePath)).length()==0L && !File(target,"a").exists()
                durable=journal.load().single().items.single().value.localIdentity==value.localIdentity && value.localIdentity!=null
                markerMatches=Os.getxattr(value.candidatePath,key).toString(Charsets.US_ASCII)==value.localIdentity?.token
            }
        }).prepare(plan)
        journal.begin(plan.request.id,run.outcome.items,plan.id)
        val result=run.execute();run.awaitClosed()
        val finished=runCatching { journal.finish(plan.request.id,result,false,null) }
        assertTrue("Creation reference and acknowledged result must be recorded: ${finished.exceptionOrNull()}",finished.isSuccess)
        assertTrue("Creation reference must commit before any payload or publication",observed && empty && durable && markerMatches)
        assertEquals(FileCopyState.COMPLETE,result.state)
        val saved=journal.load().single();assertNotNull(saved.items.single().value.localIdentity)
        val current=Os.lstat(File(target,"a").path);val origin=saved.items.single().value.localIdentity!!
        assertEquals(current.st_dev,origin.device);assertEquals(current.st_ino,origin.inode);assertEquals(current.st_uid,origin.uid)
        assertEquals(Os.lstat(File(tasks,"${plan.id}/destination").path).st_ino,origin.candidateInode)
        assertEquals(Os.lstat(target.path).st_ino,origin.destinationInode)
        assertEquals(LocalFileIdentityState.SAME,LocalFileIdentityReader(home,tasks).inspect(saved,saved.items.single()).state)
        assertEquals("abc",File(target,"a").readText());assertEquals(0,fds())
    }
    @Test fun refusedCreationReferenceStopsBeforePayloadOrPublication()=runBlocking<Unit> {
        val plan=plan();var created=false
        val run=FileCopyExecutor(home,context.contentResolver,tasks,::connections,record={ value,point ->
            if(point==FileCopyCheckpoint.LOCAL_CREATED) { created=value.localIdentity!=null;throw IOException("creation record refused") }
        }).prepare(plan)
        val result=run.execute();run.awaitClosed()
        assertTrue("Created identity must reach the durable checkpoint",created)
        assertNotNull(run.recordingError);assertNotEquals(FileCopyState.COMPLETE,result.state)
        assertFalse(File(target,"a").exists());assertEquals(0,File(tasks,"${plan.id}/destination").list()!!.size)
        assertEquals("abc",File(home,"a").readText());assertEquals(0,fds())
    }
    @Test fun codecCannotAttachCreationReferencesToForeignOrUnattemptedItems()=runBlocking<Unit> {
        val saved=record();val journal=FileTaskJournal(File(root,"history/tasks.db"))
        val request=FilePasteRequest.capture(FileClipboardSnapshot.capture(saved.intent.source,"home",saved.intent.selected),saved.intent.destination,"target")
        journal.load();journal.accept(request);journal.phase(request.id,FileJournalPhase.CONFIRMING)
        val before=journal.load();val item=saved.items.single().value
        val foreign=FileLocation(FileEndpoint("foreign","foreign","",treeUri="content://test/tree/folder"),"")
        for(bad in listOf(item.copy(targetParent=foreign),item.copy(state=FileCopyItemState.NOT_ATTEMPTED))) {
            var rejected=false
            try { journal.begin(request.id,listOf(bad),owner.name) } catch(_:Exception) { rejected=true }
            assertTrue("Foreign/unattempted reference must not commit",rejected);assertEquals(before,journal.load())
        }
    }
    @Test fun strictCodecRejectsChangedCreationReferenceWithoutRewritingIt()=runBlocking<Unit> {
        val saved=record();val path=File(root,"history/tasks.db");val journal=FileTaskJournal(path)
        val request=FilePasteRequest.capture(FileClipboardSnapshot.capture(saved.intent.source,"home",saved.intent.selected),saved.intent.destination,"target")
        journal.load();journal.accept(request);journal.phase(request.id,FileJournalPhase.CONFIRMING)
        journal.begin(request.id,listOf(saved.items.single().value),owner.name)
        assertEquals(reference,FileTaskJournal(path).load().single().items.single().value.localIdentity)
        val original=SQLiteDatabase.openDatabase(path.path,null,SQLiteDatabase.OPEN_READONLY).use { db ->
            db.rawQuery("SELECT payload FROM items",null).use { it.moveToFirst();it.getBlob(0) }
        }
        assertEquals("3",JSONObject(original.toString(Charsets.UTF_8)).getString("version"))
        val edits=listOf<(JSONObject)->Unit>(
            { it.getJSONObject("local").put("token","invalid") },
            { it.getJSONObject("local").put("candidate","not-a-uuid") },
            { it.getJSONObject("local").put("uid","-1") },
            { it.getJSONObject("local").put("deleteAuthority",true) }
        )
        for(edit in edits) {
            val after=JSONObject(original.toString(Charsets.UTF_8)).apply(edit).toString().toByteArray(Charsets.UTF_8)
            SQLiteDatabase.openDatabase(path.path,null,SQLiteDatabase.OPEN_READWRITE).use { db ->
                val before=db.rawQuery("SELECT payload FROM items",null).use { it.moveToFirst();it.getBlob(0) }
                db.update("items",ContentValues().apply { put("payload",after) },null,null)
                db.execSQL("UPDATE budget SET bytes=bytes+? WHERE id=1",arrayOf(after.size-before.size))
            }
            var rejected=false
            try { journal.load() } catch(_:Exception) { rejected=true };assertTrue("Invalid reference must be preserved and rejected",rejected)
            SQLiteDatabase.openDatabase(path.path,null,SQLiteDatabase.OPEN_READONLY).use { db ->
                val current=db.rawQuery("SELECT payload FROM items",null).use { it.moveToFirst();it.getBlob(0) }
                assertArrayEquals(after,current)
            }
        }
        assertEquals("abc",candidate.readText());assertEquals(0,fds())
    }
}
