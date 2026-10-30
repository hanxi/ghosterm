package com.hanxi.ghosterm

import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.hanxi.ghosterm.data.*
import com.hanxi.ghosterm.domain.*
import kotlinx.coroutines.runBlocking
import org.junit.*
import org.junit.Assert.*
import org.junit.runner.RunWith
import java.io.File
import java.util.UUID

@RunWith(AndroidJUnit4::class)
class FilePasteDecisionsTest {
    private val context=InstrumentationRegistry.getInstrumentation().targetContext
    private val origin=FileLocation(FileEndpoint.APP,"source")
    private val destination=FileLocation(FileEndpoint.APP,"target")
    private lateinit var root:File
    private lateinit var home:File
    private lateinit var local:LocalFileBrowser
    @Before fun setup() {
        root=File(context.cacheDir,"decisions-${UUID.randomUUID()}").apply { mkdir() }
        home=File(root,"home").apply { mkdir() };File(home,"source").mkdir();File(home,"target").mkdir()
        local=LocalFileBrowser(home,context.contentResolver)
    }
    @After fun cleanup() { root.deleteRecursively() }
    private fun entry(name:String,kind:FileKind=FileKind.FILE,size:ULong?=3uL,id:String="source/$name")=BrowserEntry(id,name,kind,size,null)
    private fun check(items:List<BrowserEntry> = listOf(entry("a.txt")),existing:List<BrowserEntry> = items.map { it.copy(id="target/${it.name}") },
        policy:FileConflictPolicy=FileConflictPolicy.ASK,source:FileLocation=origin,nodes:List<FileScanEntry>?=null):FilePasteCheck {
        val request=FilePasteRequest.capture(FileClipboardSnapshot.capture(source,"Source",items),destination,"Target",policy)
        val scan=FileTreeScan(request,nodes ?: items.map { FileScanEntry(source,it,it.name,if(it.kind==FileKind.DIRECTORY) 0 else null) },null,0uL,0)
        val byName=existing.associateBy { it.name }
        return FilePasteCheck(scan,existing,items.mapNotNull { item -> byName[item.name]?.let { FilePasteConflict(item,it) } },FileCreateCapability.UNKNOWN,null)
    }
    private fun choose(owner:FilePasteDecisions,path:String,choice:FilePasteChoice,all:Boolean=false)=owner.choose(owner.state().reference,path,choice,all)
    private fun confirm(owner:FilePasteDecisions)=owner.confirm(owner.state().reference)
    private suspend fun real():FilePasteCheck {
        val request=FilePasteRequest.capture(FileClipboardSnapshot.capture(origin,"Source",local.list(origin).sortedByDescending { it.name }),destination,"Target")
        val access=FileDestinationAccess(home,context.contentResolver,local::browse)
        return FilePastePreflight(access::inspect,access::relation).check(FileTreeScanner(local::browse).scan(request))
    }

    @Test fun realPlanReservesIncomingNamesAndDoesNotWrite() = runBlocking<Unit> {
        File(home,"source/a.txt").writeText("new");File(home,"source/a (2).txt").writeText("second")
        File(home,"target/a.txt").writeText("old")
        val checked=real();assertTrue(checked.checked)
        assertEquals("a.txt",checked.scan.request.items.first().name) // Rename before the unchanged incoming name.
        val owner=FilePasteDecisions.begin(checked)
        choose(owner,"a.txt",FilePasteChoice.KEEP_BOTH)
        val plan=confirm(owner)
        assertEquals(mapOf("a.txt" to "a (3).txt","a (2).txt" to "a (2).txt"),plan.roots.associate { it.source.entry.name to it.destinationName })
        assertEquals(9uL,plan.totalBytes);assertTrue(plan.hasWork);assertTrue(plan.skipped.isEmpty())
        assertSame(checked,plan.check);assertSame(checked.scan.request,plan.request)
        assertNotEquals(plan.request.id,plan.id);assertNotEquals(plan.request.clipboardId,plan.id)
        assertEquals(FileConflictPolicy.KEEP_BOTH,plan.roots.single { it.source.entry.name=="a.txt" }.onConflict)
        assertEquals(FileConflictPolicy.ASK,plan.roots.single { it.source.entry.name=="a (2).txt" }.onConflict)
        assertEquals("old",File(home,"target/a.txt").readText());assertEquals(setOf("a.txt"),File(home,"target").list()!!.toSet())
        assertEquals("new",File(home,"source/a.txt").readText())
    }
    @Test fun staleForeignAndRepeatedConfirmationCannotChangeRequest() {
        val checked=check();val owner=FilePasteDecisions.begin(checked);val initial=owner.state()
        assertThrows(IllegalStateException::class.java) { confirm(owner) }
        val other=FilePasteDecisions.begin(checked)
        assertNotEquals(initial.reference.sessionId,other.state().reference.sessionId)
        for(ref in listOf(initial.reference.copy(requestId="other"),initial.reference.copy(sessionId="other"),other.state().reference))
            assertThrows(IllegalArgumentException::class.java) { owner.choose(ref,"a.txt",FilePasteChoice.SKIP) }
        val changed=choose(owner,"a.txt",FilePasteChoice.KEEP_BOTH)
        assertEquals(initial.reference.revision+1,changed.reference.revision)
        assertTrue(initial.choices.isEmpty());assertEquals(1,initial.pending.size)
        assertThrows(IllegalArgumentException::class.java) { owner.confirm(initial.reference) }
        assertThrows(IllegalArgumentException::class.java) { owner.choose(initial.reference,"a.txt",FilePasteChoice.SKIP) }
        val plan=owner.confirm(changed.reference)
        assertEquals("a (2).txt",plan.roots.single().destinationName);assertTrue(owner.state().confirmed)
        val sealed=owner.state().reference
        assertThrows(IllegalStateException::class.java) { owner.confirm(sealed) }
        assertThrows(IllegalStateException::class.java) { owner.choose(sealed,"a.txt",FilePasteChoice.SKIP) }
        assertThrows(IllegalStateException::class.java) { owner.cancel(sealed) }
        assertEquals(1,other.state().pending.size)
    }
    @Test fun applyRemainingPreservesPriorChoicesAndNeverSilentlySkipsUnsupported() {
        val items=listOf(entry("a"),entry("b"),entry("c"),entry("link",FileKind.LINK),entry("fifo",FileKind.OTHER))
        val checked=check(items);val owner=FilePasteDecisions.begin(checked)
        choose(owner,"a",FilePasteChoice.SKIP)
        val state=choose(owner,"b",FilePasteChoice.KEEP_BOTH,true)
        assertEquals(mapOf("a" to FilePasteChoice.SKIP,"b" to FilePasteChoice.KEEP_BOTH,"c" to FilePasteChoice.KEEP_BOTH),state.choices)
        assertEquals(setOf("link","fifo"),state.pending.map { it.source.entry.name }.toSet())
        assertEquals(FileConflictPolicy.KEEP_BOTH,state.remainingPolicy)
        assertThrows(IllegalArgumentException::class.java) { choose(owner,"link",FilePasteChoice.KEEP_BOTH) }
        assertThrows(IllegalArgumentException::class.java) { choose(owner,"link",FilePasteChoice.SKIP,true) }
        assertThrows(IllegalArgumentException::class.java) { choose(owner,"missing",FilePasteChoice.SKIP) }
        choose(owner,"link",FilePasteChoice.SKIP);choose(owner,"fifo",FilePasteChoice.SKIP)
        val plan=confirm(owner);assertEquals(listOf("b (2)","c (2)"),plan.roots.map { it.destinationName })
        assertEquals(3,plan.skipped.size);assertEquals(6uL,plan.totalBytes)
        assertEquals(FileConflictPolicy.ASK,checked.scan.request.conflictPolicy)
        val fresh=FilePasteDecisions.begin(check(items));assertEquals(5,fresh.state().pending.size)
        assertEquals(FileConflictPolicy.ASK,fresh.state().remainingPolicy)
    }
    @Test fun initialPoliciesAndCancelRemainRequestLocal() {
        for(policy in listOf(FileConflictPolicy.KEEP_BOTH,FileConflictPolicy.SKIP)) {
            val owner=FilePasteDecisions.begin(check(listOf(entry("a"),entry("link",FileKind.LINK)),policy=policy))
            assertEquals(listOf("link"),owner.state().pending.map { it.source.entry.name })
            assertEquals(if(policy==FileConflictPolicy.SKIP) FilePasteChoice.SKIP else FilePasteChoice.KEEP_BOTH,owner.state().choices["a"])
            val before=owner.state().reference;val cancelled=owner.cancel(before)
            assertTrue(cancelled.cancelled);assertFalse(cancelled.confirmed)
            assertEquals(before.revision+1,cancelled.reference.revision)
            assertThrows(IllegalStateException::class.java) { owner.confirm(cancelled.reference) }
            assertThrows(IllegalStateException::class.java) { owner.choose(cancelled.reference,"link",FilePasteChoice.SKIP) }
        }
        val owner=FilePasteDecisions.begin(check(listOf(entry("a"),entry("b"))))
        choose(owner,"a",FilePasteChoice.SKIP,true)
        assertEquals(FileConflictPolicy.SKIP,owner.state().remainingPolicy)
        val plan=confirm(owner);assertFalse(plan.hasWork);assertEquals(0uL,plan.totalBytes)
        assertEquals(2,plan.skipped.size);assertTrue(plan.roots.isEmpty())
    }
    @Test fun directorySkipIncludesDescendantsAndCanBeRevisedBeforeConfirmation() {
        val folder=entry("a",FileKind.DIRECTORY);val child=entry("file",id="source/a/file")
        val link=entry("link",FileKind.LINK,id="source/a/link")
        val checked=check(listOf(folder),nodes=listOf(FileScanEntry(origin,folder,"a",2),
            FileScanEntry(origin.copy(directoryId=folder.id),child,"a/file"),FileScanEntry(origin.copy(directoryId=folder.id),link,"a/link")))
        val owner=FilePasteDecisions.begin(checked)
        choose(owner,"a",FilePasteChoice.SKIP)
        assertTrue(owner.state().pending.isEmpty())
        assertThrows(IllegalArgumentException::class.java) { choose(owner,"a/link",FilePasteChoice.SKIP) }
        choose(owner,"a",FilePasteChoice.KEEP_BOTH)
        assertEquals(listOf("a/link"),owner.state().pending.map { it.source.relativePath })
        choose(owner,"a/link",FilePasteChoice.SKIP)
        val plan=confirm(owner)
        assertEquals(listOf("a (2)","a (2)/file"),plan.entries.map { it.destinationRelativePath })
        assertEquals(FilePasteDecisionKind.UNSUPPORTED,plan.skipped.single().reason)
        val skipped=FilePasteDecisions.begin(checked);choose(skipped,"a",FilePasteChoice.SKIP)
        val none=confirm(skipped)
        assertFalse(none.hasWork);assertTrue(none.entries.isEmpty());assertTrue(none.roots.isEmpty())
        assertEquals(listOf("a","a/file","a/link"),none.skipped.map { it.source.relativePath })
        assertTrue(none.skipped.all { it.reason==FilePasteDecisionKind.CONFLICT });assertEquals(0uL,none.totalBytes)
    }
    @Test fun opaqueDirectoryIdsAndEmptyFoldersSurviveRootRebasing() {
        val saf=FileEndpoint("opaque","SAF","root-id","content://provider/tree/root-id")
        val source=FileLocation(saf);val folder=entry("a",FileKind.DIRECTORY,id="folder-id")
        val empty=entry("empty",FileKind.DIRECTORY,id="empty-id");val sibling=entry("ab",id="sibling-id")
        val child=entry("text.txt",id="child-id")
        val nodes=listOf(FileScanEntry(source,folder,"a",2),FileScanEntry(FileLocation(saf,folder.id),empty,"a/empty",0),
            FileScanEntry(FileLocation(saf,folder.id),child,"a/text.txt"),FileScanEntry(source,sibling,"ab"))
        val owner=FilePasteDecisions.begin(check(listOf(folder,sibling),existing=listOf(folder.copy(id="target-a")),source=source,nodes=nodes))
        choose(owner,"a",FilePasteChoice.KEEP_BOTH);val plan=confirm(owner)
        assertEquals(listOf("a (2)","a (2)/empty","a (2)/text.txt","ab"),plan.entries.map { it.destinationRelativePath })
        assertEquals(nodes,plan.entries.map { it.source });assertEquals(listOf("a (2)","ab"),plan.roots.map { it.destinationName })
        assertEquals(0,plan.entries[1].source.childCount);assertEquals(6uL,plan.totalBytes)
        assertEquals(FileCreateCapability.UNKNOWN,plan.check.create)
    }
    @Test fun plansAndStateExposeOnlyFrozenCollections() {
        val items=mutableListOf(entry("a"),entry("b"));val checked=check(items);items.clear()
        val owner=FilePasteDecisions.begin(checked);val before=owner.state()
        assertThrows(UnsupportedOperationException::class.java) { (before.pending as MutableList).clear() }
        assertThrows(UnsupportedOperationException::class.java) { (before.choices as MutableMap)["a"]=FilePasteChoice.SKIP }
        choose(owner,"a",FilePasteChoice.SKIP);choose(owner,"b",FilePasteChoice.KEEP_BOTH)
        val state=owner.state();val plan=confirm(owner)
        assertEquals(2,before.pending.size);assertTrue(before.choices.isEmpty());assertFalse(state.confirmed)
        assertThrows(UnsupportedOperationException::class.java) { (plan.roots as MutableList).clear() }
        assertThrows(UnsupportedOperationException::class.java) { (plan.entries as MutableList).clear() }
        assertThrows(UnsupportedOperationException::class.java) { (plan.skipped as MutableList).clear() }
        val other=FilePasteDecisions.begin(check(existing=emptyList()));assertNotEquals(plan.id,confirm(other).id)
    }
    @Test fun incompleteDeniedAndFailedPreflightCannotBecomePlans() {
        val good=check(existing=emptyList())
        val incomplete=FileTreeScan(good.scan.request,good.scan.entries,FileScanFailure(FileScanFailureKind.READ,"a","failed"),0uL,0)
        val failed=FilePasteCheckFailure(FilePasteCheckFailureKind.RELATION_UNKNOWN,"unknown")
        for(input in listOf(FilePasteCheck(incomplete,emptyList(),emptyList(),FileCreateCapability.UNKNOWN,null),
            FilePasteCheck(good.scan,emptyList(),emptyList(),FileCreateCapability.DENIED,null),
            FilePasteCheck(good.scan,emptyList(),emptyList(),FileCreateCapability.UNKNOWN,failed)))
            assertThrows(IllegalArgumentException::class.java) { FilePasteDecisions.begin(input) }
        assertTrue(confirm(FilePasteDecisions.begin(good)).hasWork)
    }
    @Test fun malformedTreeOwnershipRootsAndConflictMetadataAreRejected() {
        val parent=entry("folder",FileKind.DIRECTORY);val child=entry("file",id="child-id")
        val nodes=listOf(FileScanEntry(origin,parent,"folder",1),FileScanEntry(origin.copy(directoryId=parent.id),child,"folder/file"))
        val good=check(listOf(parent),existing=emptyList(),nodes=nodes)
        fun reject(changed:List<FileScanEntry>) {
            val scan=FileTreeScan(good.scan.request,changed,null,0uL,0)
            assertThrows(IllegalArgumentException::class.java) { FilePasteDecisions.begin(FilePasteCheck(scan,emptyList(),emptyList(),FileCreateCapability.UNKNOWN,null)) }
        }
        reject(emptyList());reject(nodes+nodes[1]);reject(nodes.reversed())
        reject(listOf(nodes[0].copy(childCount=2),nodes[1],nodes[1]))
        reject(listOf(nodes[0],nodes[1].copy(entry=child.copy(name="other"))))
        reject(listOf(nodes[0].copy(childCount=0),nodes[1]));reject(listOf(nodes[0].copy(childCount=null),nodes[1]))
        reject(listOf(nodes[0],nodes[1].copy(parent=origin)))
        reject(listOf(nodes[0],nodes[1].copy(parent=nodes[1].parent.copy(endpoint=FileEndpoint.APP.copy(id="foreign")))))
        reject(listOf(nodes[0].copy(parent=origin.copy(directoryId="elsewhere")),nodes[1]))
        reject(listOf(nodes[0].copy(entry=parent.copy(name="renamed")),nodes[1]))
        reject(listOf(nodes[0],nodes[1].copy(relativePath="folder/../file")))
        val wrongRoot=entry("other")
        assertThrows(IllegalArgumentException::class.java) { FilePasteDecisions.begin(check(nodes=listOf(FileScanEntry(origin,wrongRoot,"other")),existing=emptyList())) }
        assertThrows(IllegalArgumentException::class.java) { FilePasteDecisions.begin(FilePasteCheck(good.scan,listOf(entry("x"),entry("x",id="different")),emptyList(),FileCreateCapability.UNKNOWN,null)) }
        assertThrows(IllegalArgumentException::class.java) { FilePasteDecisions.begin(FilePasteCheck(good.scan,listOf(parent),emptyList(),FileCreateCapability.UNKNOWN,null)) }
        assertEquals(2,confirm(FilePasteDecisions.begin(good)).entries.size)
    }
    @Test fun candidateNamesHandleExtensionsUnicodeAndBoundedExhaustion() {
        fun next(name:String,kind:FileKind=FileKind.FILE)=FileCopyNames.next(name,kind,emptySet())
        assertEquals("archive.tar (2).gz",next("archive.tar.gz"));assertEquals(".env (2)",next(".env"))
        assertEquals("folder.txt (2)",next("folder.txt",FileKind.DIRECTORY));assertEquals("name (2)",next("name"))
        val long="😀".repeat(100)+".txt";val result=next(long)
        assertTrue(result.toByteArray().size<=255);assertTrue(result.endsWith(" (2).txt"))
        assertFalse(result.contains('\uFFFD'));assertEquals(result,String(result.toByteArray(),Charsets.UTF_8))
        val longExt=next("x."+"界".repeat(100));assertTrue(longExt.toByteArray().size<=255);assertTrue(longExt.endsWith(" (2)"))
        assertEquals("x (4)",FileCopyNames.next("x",FileKind.FILE,setOf("x (2)","x (3)"),3))
        assertThrows(IllegalStateException::class.java) { FileCopyNames.next("x",FileKind.FILE,setOf("x (2)","x (3)"),2) }
        for(name in listOf("",".","..","a/b","a\u0000b","\uDC00","\uD800"))
            assertThrows(IllegalArgumentException::class.java) { next(name) }
        for(kind in listOf(FileKind.LINK,FileKind.OTHER)) assertThrows(IllegalArgumentException::class.java) { next("x",kind) }
        for(limit in listOf(0,10001)) assertThrows(IllegalArgumentException::class.java) { FileCopyNames.next("x",FileKind.FILE,emptySet(),limit) }
    }
    @Test fun failedPlanningLeavesDecisionsEditableAndHonorsUtf8Budget() {
        val owner=FilePasteDecisions.begin(check(listOf(entry("巨")),existing=listOf(entry("巨",id="target/巨"),entry("巨 (2)",id="target/巨 (2)"))))
        choose(owner,"巨",FilePasteChoice.KEEP_BOTH);val ref=owner.state().reference
        assertThrows(IllegalStateException::class.java) { owner.confirm(ref,maxRenameAttempts=1) }
        assertEquals(ref,owner.state().reference);assertFalse(owner.state().confirmed)
        assertThrows(IllegalStateException::class.java) { owner.confirm(ref,maxMetadataBytes=6) }
        assertEquals(ref,owner.state().reference)
        for(limit in listOf(0,16*1024*1024+1)) assertThrows(IllegalArgumentException::class.java) { owner.confirm(ref,maxMetadataBytes=limit) }
        val plan=owner.confirm(ref,maxMetadataBytes=7);assertEquals("巨 (3)",plan.roots.single().destinationName)
        val skipped=FilePasteDecisions.begin(check());choose(skipped,"a.txt",FilePasteChoice.SKIP)
        assertFalse(skipped.confirm(skipped.state().reference,maxMetadataBytes=1).hasWork)
    }
    @Test fun unknownSizesOverflowAndSkippedTotalsDoNotInventProgress() {
        val unknown=FilePasteDecisions.begin(check(listOf(entry("known",size=5uL),entry("unknown",size=null)),existing=emptyList()))
        val plan=confirm(unknown);assertEquals(5uL,plan.knownBytes);assertEquals(1,plan.unknownSizeFiles);assertNull(plan.totalBytes)
        val overflow=FilePasteDecisions.begin(check(listOf(entry("huge",size=ULong.MAX_VALUE),entry("extra",size=1uL)),policy=FileConflictPolicy.KEEP_BOTH))
        assertThrows(IllegalStateException::class.java) { confirm(overflow) }
        assertFalse(overflow.state().confirmed);choose(overflow,"extra",FilePasteChoice.SKIP)
        val fits=confirm(overflow);assertEquals(ULong.MAX_VALUE,fits.totalBytes);assertEquals(1,fits.skipped.size)
    }
    @Test fun plannedNameDoesNotReserveTargetAndFinalPublicationPreservesRaceWinner() = runBlocking<Unit> {
        val bytes="new content".toByteArray();File(home,"source/a.txt").writeBytes(bytes)
        File(home,"target/a.txt").writeText("original")
        val owner=FilePasteDecisions.begin(real());choose(owner,"a.txt",FilePasteChoice.KEEP_BOTH)
        val plan=confirm(owner);val name=plan.roots.single().destinationName;assertEquals("a (2).txt",name)
        File(home,"target/$name").writeText("race winner")
        val publisher=LocalFilePublisher(home,File(root,"publications"))
        TransferStagingStore(File(root,"snapshots"),4096,8192,0).use { store ->
            local.snapshot(origin,plan.entries.single().source.entry,store).use { snapshot ->
                assertEquals(LocalFilePublisher.State.CONFLICT,publisher.publish("target",name,snapshot).state)
                val retry=FileCopyNames.next("a.txt",FileKind.FILE,local.list(destination).map { it.name }.toSet())
                assertEquals("a (3).txt",retry)
                assertEquals(LocalFilePublisher.State.PUBLISHED,publisher.publish("target",retry,snapshot).state)
                assertArrayEquals(bytes,File(home,"target/$retry").readBytes())
            }
            assertEquals(0L,store.reservedBytes)
        }
        assertEquals("original",File(home,"target/a.txt").readText());assertEquals("race winner",File(home,"target/$name").readText())
        assertArrayEquals(bytes,File(home,"source/a.txt").readBytes())
    }
}
