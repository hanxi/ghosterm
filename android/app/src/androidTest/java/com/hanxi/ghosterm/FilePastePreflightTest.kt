package com.hanxi.ghosterm

import android.system.Os
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
class FilePastePreflightTest {
    private val context=InstrumentationRegistry.getInstrumentation().targetContext
    private lateinit var root:File
    private lateinit var local:LocalFileBrowser
    private lateinit var access:FileDestinationAccess
    private val origin=FileLocation(FileEndpoint.APP,"source")
    private val destination=FileLocation(FileEndpoint.APP,"target")
    private fun file(id:String,name:String=id.substringAfterLast('/'),kind:FileKind=FileKind.FILE)=BrowserEntry(id,name,kind,3uL,null)
    @Before fun setup() {
        root=File(context.cacheDir,"preflight-${UUID.randomUUID()}").apply { mkdir() }
        File(root,"source").mkdir();File(root,"target").mkdir()
        local=LocalFileBrowser(root,context.contentResolver)
        access=FileDestinationAccess(root,context.contentResolver,local::browse)
    }
    @After fun cleanup() { File(root,"target").setWritable(true);root.deleteRecursively() }
    private fun scan(items:List<BrowserEntry> = listOf(file("source/a")), target:FileLocation=destination,
        source:FileLocation=origin, policy:FileConflictPolicy=FileConflictPolicy.ASK):FileTreeScan {
        val request=FilePasteRequest.capture(FileClipboardSnapshot.capture(source,"source",items),target,"target",policy)
        return FileTreeScan(request,items.map { FileScanEntry(source,it,it.name) },null,3uL,0)
    }
    private fun preflight(entries:List<BrowserEntry> = emptyList(),capability:FileCreateCapability=FileCreateCapability.ALLOWED,
        target:FileLocation=destination, relation:FileLocationRelation=FileLocationRelation.SEPARATE)=FilePastePreflight(
        { FileTargetDirectory(BrowserDirectory(target,entries),capability) },{ _,_ -> relation })

    @Test fun realPrivateDirectoriesReportConflictsWithoutChangingFiles() = runBlocking<Unit> {
        File(root,"source/a").writeText("new");File(root,"source/folder").mkdir()
        File(root,"target/a").writeText("old");File(root,"target/folder").writeText("kept")
        val input=FilePasteRequest.capture(FileClipboardSnapshot.capture(origin,"Source",local.list(origin)),destination,"Target")
        val scanned=FileTreeScanner(local::browse).scan(input)
        val checked=FilePastePreflight(access::inspect,access::relation).check(scanned)
        assertTrue(checked.failure.toString(),checked.checked)
        assertSame(scanned,checked.scan);assertEquals(FileCreateCapability.ALLOWED,checked.create)
        assertEquals(listOf("folder","a"),checked.conflicts.map { it.source.name })
        assertEquals(FileKind.DIRECTORY,checked.conflicts[0].source.kind)
        assertEquals(FileKind.FILE,checked.conflicts[0].destination.kind)
        assertEquals("old",File(root,"target/a").readText());assertEquals("new",File(root,"source/a").readText())
        assertEquals(setOf("a","folder"),File(root,"target").list()!!.toSet())
        assertThrows(UnsupportedOperationException::class.java) { (checked.targetEntries as MutableList).clear() }
        assertThrows(UnsupportedOperationException::class.java) { (checked.conflicts as MutableList).clear() }
    }
    @Test fun sameFolderAndSelectedSubtreesAreRejectedButSiblingIsAllowed() = runBlocking<Unit> {
        File(root,"source/folder/deep").mkdirs();File(root,"source/folder2").mkdir()
        val selected=listOf(file("source/folder",kind=FileKind.DIRECTORY))
        val service=FilePastePreflight(access::inspect,access::relation)
        assertEquals(FilePasteCheckFailureKind.SAME_DIRECTORY,service.check(scan(selected,origin)).failure?.kind)
        for(path in listOf("source/folder","source/folder/deep"))
            assertEquals(FilePasteCheckFailureKind.INSIDE_SOURCE,service.check(scan(selected,FileLocation(FileEndpoint.APP,path))).failure?.kind)
        assertTrue(service.check(scan(selected,FileLocation(FileEndpoint.APP,"source/folder2"))).checked)
        assertEquals(FileLocationRelation.DESCENDANT,access.relation(FileLocation(FileEndpoint.APP),origin))
    }
    @Test fun privateReadOnlyMissingAndSymlinkDestinationsNeverBecomeWritable() = runBlocking<Unit> {
        val target=File(root,"target");Os.chmod(target.path,320) // 0500, app UID cannot create.
        try {
            val checked=FilePastePreflight(access::inspect,access::relation).check(scan())
            assertEquals(FilePasteCheckFailureKind.READ_ONLY,checked.failure?.kind)
            assertEquals(FileCreateCapability.DENIED,checked.create)
        } finally { Os.chmod(target.path,448) }
        Os.symlink(target.path,File(root,"link").path)
        val service=FilePastePreflight(access::inspect,access::relation)
        for(path in listOf("missing","link","../target")) {
            val result=service.check(scan(target=FileLocation(FileEndpoint.APP,path)))
            assertEquals(FilePasteCheckFailureKind.TARGET_READ,result.failure?.kind);assertFalse(result.checked)
        }
        assertEquals(emptyList<String>(),target.list()!!.toList())
    }
    @Test fun incompleteScanAndUnknownRelationshipStopBeforeTargetReads() = runBlocking<Unit> {
        var reads=0
        val normal=scan()
        val incomplete=FileTreeScan(normal.request,normal.entries,FileScanFailure(FileScanFailureKind.READ,"a","failed"),3uL,0)
        val service=FilePastePreflight({ reads++;error("must not inspect") },{ _,_ -> FileLocationRelation.SEPARATE })
        assertEquals(FilePasteCheckFailureKind.INCOMPLETE_SOURCE,service.check(incomplete).failure?.kind)
        assertEquals(0,reads)
        assertEquals(FilePasteCheckFailureKind.RELATION_UNKNOWN,preflight(relation=FileLocationRelation.UNKNOWN).check(normal).failure?.kind)
        val selected=scan(listOf(file("source/folder",kind=FileKind.DIRECTORY)))
        val unknownChild=FilePastePreflight({ error("must not inspect") },{ source,_ -> if(source==origin) FileLocationRelation.SEPARATE else FileLocationRelation.UNKNOWN })
        assertEquals(FilePasteCheckFailureKind.RELATION_UNKNOWN,unknownChild.check(selected).failure?.kind)
    }
    @Test fun targetReadFailuresAndChangedLocationsAreNotEmptySuccess() = runBlocking<Unit> {
        val service=FilePastePreflight({ throw IllegalStateException("permission lost") },{ _,_ -> FileLocationRelation.SEPARATE })
        val failed=service.check(scan());assertEquals(FilePasteCheckFailureKind.TARGET_READ,failed.failure?.kind)
        assertTrue(failed.failure!!.message.contains("permission lost"));assertFalse(failed.checked)
        assertEquals(FilePasteCheckFailureKind.TARGET_CHANGED,preflight(target=origin).check(scan()).failure?.kind)
        assertTrue(preflight().check(scan()).checked)
        assertEquals(FilePasteCheckFailureKind.READ_ONLY,preflight(capability=FileCreateCapability.DENIED).check(scan()).failure?.kind)
    }
    @Test fun ambiguousUnsafeForeignAndOversizedTargetsFailExplicitly() = runBlocking<Unit> {
        val good=file("target/a")
        val invalid=listOf(listOf(good,good.copy(name="b")),listOf(good,file("target/b","a")),
            listOf(file("target/../bad","../bad")),listOf(file("elsewhere/a")),listOf(file("target/a","\uD800")),
            listOf(file("target/\uDC00","\uDC00")),
            listOf(file("","a")),listOf(file("target/a","\u0000")),List(4097) { file("target/$it") })
        for(entries in invalid) assertEquals(FilePasteCheckFailureKind.TARGET_INVALID,preflight(entries).check(scan()).failure?.kind)
        // SAF IDs have no path/name correspondence: each duplicate guard must stand alone.
        val opaque=FileLocation(FileEndpoint("saf","SAF","opaque-root","content://fixture/tree/opaque-root"))
        for(entries in listOf(listOf(file("opaque-one","a"),file("opaque-one","b")),
            listOf(file("opaque-one","a"),file("opaque-two","a")))) {
            assertEquals(FilePasteCheckFailureKind.TARGET_INVALID,preflight(entries,target=opaque).check(scan(target=opaque)).failure?.kind)
        }
        assertTrue(preflight(List(4096) { file("target/$it") }).check(scan()).checked)
        assertTrue(preflight(listOf(good)).check(scan(),maxMetadataBytes=9).checked)
        assertEquals(FilePasteCheckFailureKind.TARGET_INVALID,preflight(listOf(good)).check(scan(),maxMetadataBytes=8).failure?.kind)
        assertEquals(FilePasteCheckFailureKind.TARGET_INVALID,preflight(listOf(good,file("target/b"))).check(scan(),maxMetadataBytes=9).failure?.kind)
        assertEquals(FilePasteCheckFailureKind.TARGET_INVALID,preflight(listOf(file("target/巨","巨"))).check(scan(),maxMetadataBytes=10).failure?.kind)
    }
    @Test fun conflictsStayBoundToRequestAndUnknownWritePermissionIsPreserved() = runBlocking<Unit> {
        val entries=mutableListOf(file("target/a"),file("target/extra"))
        val first=scan(policy=FileConflictPolicy.SKIP)
        val result=preflight(entries,FileCreateCapability.UNKNOWN).check(first)
        entries.clear()
        assertTrue(result.checked);assertEquals(FileCreateCapability.UNKNOWN,result.create)
        assertEquals(2,result.targetEntries.size);assertEquals(1,result.conflicts.size)
        assertEquals(first.request.id,result.scan.request.id)
        assertEquals(FileConflictPolicy.SKIP,result.scan.request.conflictPolicy)
        val second=scan(listOf(file("source/other")),policy=FileConflictPolicy.KEEP_BOTH)
        assertTrue(preflight(result.targetEntries).check(second).conflicts.isEmpty())
        assertNotEquals(first.request.id,second.request.id)
        assertEquals("a",result.conflicts.single().source.name)
    }
    @Test fun remoteLogicalIdentityIgnoresAliasButDoesNotInventNamespaceEquivalence() = runBlocking<Unit> {
        fun endpoint(id:String,host:String="server",user:String="user",port:Int=22)=FileEndpoint(id,id,".",remote=RemoteFileIdentity(id,host,user,port))
        val a=endpoint("one");val b=endpoint("two","SERVER")
        val remoteLocation=FileLocation(a,"/a")
        val remoteAccess=FileDestinationAccess(root,context.contentResolver) { requested -> BrowserDirectory(requested,emptyList()) }
        assertEquals(FileCreateCapability.UNKNOWN,remoteAccess.inspect(remoteLocation).create)
        assertEquals(FileLocationRelation.SAME,access.relation(FileLocation(a,"/a"),FileLocation(b,"/a")))
        assertEquals(FileLocationRelation.DESCENDANT,access.relation(FileLocation(a,"/a"),FileLocation(b,"/a/deep")))
        assertEquals(FileLocationRelation.SEPARATE,access.relation(FileLocation(a,"/a"),FileLocation(b,"/ab")))
        assertEquals(FileLocationRelation.DESCENDANT,access.relation(FileLocation(a,"/"),FileLocation(b,"/a")))
        for(other in listOf(endpoint("x","elsewhere"),endpoint("x",user="other"),endpoint("x",port=2222)))
            assertEquals(FileLocationRelation.UNKNOWN,access.relation(FileLocation(a,"/a"),FileLocation(other,"/b")))
        assertEquals(FileLocationRelation.SEPARATE,access.relation(origin,FileLocation(a,"/a")))
        assertEquals(FileLocationRelation.SEPARATE,access.relation(FileLocation(a,"/a"),destination))
        for(path in listOf(".","/a/../b","/a//b","/a/")) {
            var failed=false;try { access.relation(FileLocation(a,path),FileLocation(b,"/b")) } catch(_:IllegalArgumentException) { failed=true }
            assertTrue(path,failed)
        }
    }
    @Test fun callerCancellationReachesCurrentTargetReadAndDoesNotDeliverResult() = runBlocking<Unit> {
        val entered=CompletableDeferred<Unit>();val released=CompletableDeferred<Unit>();var cancelled=false;var delivered=false
        val service=FilePastePreflight({ entered.complete(Unit);try { released.await() } finally { cancelled=!currentCoroutineContext().isActive };FileTargetDirectory(BrowserDirectory(destination,emptyList()),FileCreateCapability.ALLOWED) },{ _,_ -> FileLocationRelation.SEPARATE })
        val task=launch { service.check(scan());delivered=true }
        entered.await();task.cancel();yield()
        try { assertTrue("active target read receives cancellation",cancelled) }
        finally { released.complete(Unit);task.join() }
        assertFalse(delivered)
    }
    @Test fun deadlineKeepsIncompleteStateAndCancelsCurrentRelationshipCheck() = runBlocking<Unit> {
        var cancelled=false
        val service=FilePastePreflight({ error("not reached") },{ _,_ -> try { awaitCancellation() } finally { cancelled=true } })
        val result=service.check(scan(),100)
        assertEquals(FilePasteCheckFailureKind.TIMEOUT,result.failure?.kind);assertFalse(result.checked);assertTrue(cancelled)
        for(value in listOf(0L,120001L)) {
            var rejected=false;try { service.check(scan(),value) } catch(_:IllegalArgumentException) { rejected=true };assertTrue(rejected)
        }
    }
}
