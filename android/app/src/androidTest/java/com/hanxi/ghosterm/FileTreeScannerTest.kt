package com.hanxi.ghosterm

import android.system.Os
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.hanxi.ghosterm.data.FileTreeScanner
import com.hanxi.ghosterm.data.LocalFileBrowser
import com.hanxi.ghosterm.domain.*
import kotlinx.coroutines.*
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File
import java.io.IOException
import java.util.UUID

@RunWith(AndroidJUnit4::class)
class FileTreeScannerTest {
    private val endpoint=FileEndpoint("fixture","Fixture","root","content://fixture/tree/root")
    private val location=FileLocation(endpoint)
    private fun item(id:String,name:String=id,kind:FileKind=FileKind.FILE,size:ULong?=1uL,time:Long?=null)=
        BrowserEntry(id,name,kind,size,time)
    private fun request(items:List<BrowserEntry>,from:FileLocation=location)=FilePasteRequest.capture(
        FileClipboardSnapshot.capture(from,"source",items),FileLocation(FileEndpoint.APP,"destination"),"destination")
    private fun scanner(tree:Map<String,List<BrowserEntry>>) = FileTreeScanner { place ->
        BrowserDirectory(place,tree[place.directoryId] ?: throw IOException("directory denied"))
    }
    private fun rejected(scan:FileTreeScan,kind:FileScanFailureKind) {
        assertFalse("incomplete enumeration cannot become a complete plan",scan.complete)
        assertEquals(kind,scan.failure?.kind);assertNull(scan.totalBytes)
    }

    @Test fun requestFreezesIndependentIdentitySourceDestinationAndPolicy() {
        val input=mutableListOf(item("opaque:one","名称"))
        val clipboard=FileClipboardSnapshot.capture(location,"original source",input)
        val destination=FileLocation(FileEndpoint("remote-session","target","/",
            remote=RemoteFileIdentity("alias","::1","user",2222)),"/destination")
        val first=FilePasteRequest.capture(clipboard,destination,"original target",FileConflictPolicy.SKIP)
        val second=FilePasteRequest.capture(clipboard,FileLocation(FileEndpoint.APP),"other")
        input.clear()
        UUID.fromString(first.id);assertNotEquals(first.id,second.id);assertNotEquals(first.id,clipboard.id)
        assertEquals(clipboard.id,first.clipboardId);assertEquals(location,first.source)
        assertEquals(destination,first.destination);assertEquals("original source",first.sourcePathLabel)
        assertEquals("original target",first.destinationPathLabel);assertEquals(FileConflictPolicy.SKIP,first.conflictPolicy)
        assertEquals(FileConflictPolicy.ASK,second.conflictPolicy);assertEquals(clipboard.items,first.items)
        assertNotSame(clipboard.items,first.items)
        var readonly=false
        try { (first.items as MutableList).clear() } catch(_:UnsupportedOperationException) { readonly=true }
        assertTrue("request selection cannot be mutated",readonly);assertEquals(1,first.items.size)
    }

    @Test fun realTreePreservesEmptyHiddenUnicodeAndUnsupportedWithoutReadingOrWriting() = runBlocking {
        val context=InstrumentationRegistry.getInstrumentation().targetContext
        val root=File(context.cacheDir,"tree-scan-${UUID.randomUUID()}").apply { mkdir() }
        try {
            File(root,"tree/empty").mkdirs();File(root,"tree/nested").mkdirs()
            File(root,"tree/.hidden").writeText("hi");File(root,"tree/nested/中文.txt").writeText("abc")
            File(root,"unselected").writeText("keep");File(root,"destination").mkdir()
            Os.symlink("tree",File(root,"link").path);Os.mkfifo(File(root,"pipe").path,384)
            val source=LocalFileBrowser(root,context.contentResolver)
            val from=FileLocation(FileEndpoint.APP)
            val items=source.list(from).filter { it.name in setOf("tree","link","pipe") }
            val scan=FileTreeScanner(source::browse).scan(request(items,from))
            assertTrue(scan.failure.toString(),scan.complete)
            assertEquals(setOf("tree","tree/empty","tree/nested","tree/.hidden","tree/nested/中文.txt","link","pipe"),
                scan.entries.map { it.relativePath }.toSet())
            assertEquals(2,scan.files);assertEquals(3,scan.directories);assertEquals(2,scan.unsupported)
            assertEquals(5uL,scan.knownBytes);assertEquals(0,scan.unknownSizeFiles);assertNull(scan.totalBytes)
            assertEquals(0,scan.entries.single { it.relativePath=="tree/empty" }.childCount)
            assertEquals("tree/nested",scan.entries.single { it.relativePath.endsWith("中文.txt") }.parent.directoryId)
            assertEquals("abc",File(root,"tree/nested/中文.txt").readText());assertEquals("keep",File(root,"unselected").readText())
            assertTrue(File(root,"destination").list()!!.isEmpty());assertEquals("tree",Os.readlink(File(root,"link").path))
            var readonly=false
            try { (scan.entries as MutableList).clear() } catch(_:UnsupportedOperationException) { readonly=true }
            assertTrue("scan result must own a read-only list",readonly);assertEquals(7,scan.entries.size)
        } finally { root.deleteRecursively() }
    }

    @Test fun unknownAndUnsignedSizesNeverBecomeZeroOrOverflowedTotals() = runBlocking {
        val huge=item("huge",size=ULong.MAX_VALUE);val unknown=item("unknown",size=null)
        val source=scanner(mapOf("root" to listOf(huge,unknown)))
        val both=source.scan(request(listOf(huge,unknown)))
        assertTrue(both.complete);assertEquals(ULong.MAX_VALUE,both.knownBytes)
        assertEquals(1,both.unknownSizeFiles);assertNull(both.totalBytes)
        assertEquals(ULong.MAX_VALUE,source.scan(request(listOf(huge))).totalBytes)
        val small=item("small")
        val overflow=scanner(mapOf("root" to listOf(huge,small))).scan(request(listOf(huge,small)))
        rejected(overflow,FileScanFailureKind.LIMIT);assertEquals(ULong.MAX_VALUE,overflow.knownBytes)
    }

    @Test fun selectedIdentityTypeNameAndKnownAttributesAreRevalidated() = runBlocking {
        val original=item("file",size=7uL,time=100)
        for(now in listOf(null,original.copy(id="other"),original.copy(name="renamed"),
            original.copy(kind=FileKind.LINK),original.copy(size=8uL),original.copy(size=null),
            original.copy(modifiedMillis=101),original.copy(modifiedMillis=null))) {
            val scan=scanner(mapOf("root" to listOfNotNull(now))).scan(request(listOf(original)))
            rejected(scan,FileScanFailureKind.SOURCE_CHANGED);assertTrue(scan.entries.isEmpty())
        }
        val current=original.copy(size=8uL,modifiedMillis=101)
        val upgraded=scanner(mapOf("root" to listOf(current))).scan(request(listOf(original.copy(size=null,modifiedMillis=null))))
        assertTrue(upgraded.complete);assertEquals(current,upgraded.entries.single().entry)
    }

    @Test fun ambiguousUnsafeOrForeignEntriesNeverProduceACompleteTree() = runBlocking {
        val first=item("id","name")
        for(list in listOf(listOf(first,first.copy(name="different")),listOf(first,first.copy(id="different"))))
            rejected(scanner(mapOf("root" to list)).scan(request(listOf(first))),FileScanFailureKind.INVALID_ENTRY)
        for(name in listOf(".","..","","a/b","nul\u0000","\uD800","\uDC00")) {
            val bad=first.copy(name=name)
            rejected(scanner(mapOf("root" to listOf(bad))).scan(request(listOf(bad))),FileScanFailureKind.INVALID_ENTRY)
        }
        for(id in listOf("","bad\u0000id","\uD800")) {
            val bad=first.copy(id=id)
            rejected(scanner(mapOf("root" to listOf(bad))).scan(request(listOf(bad))),FileScanFailureKind.INVALID_ENTRY)
        }
        val wrong=item("outside/file","file")
        rejected(scanner(mapOf("" to listOf(wrong))).scan(request(listOf(wrong),FileLocation(FileEndpoint.APP))),FileScanFailureKind.INVALID_ENTRY)
        val remote=FileEndpoint("remote","remote","/",remote=RemoteFileIdentity("a","host","u",22))
        rejected(scanner(mapOf("/selected" to listOf(wrong))).scan(request(listOf(wrong),FileLocation(remote,"/selected"))),FileScanFailureKind.INVALID_ENTRY)
    }

    @Test fun depthEntryMetadataAndDirectoryLimitsFailExplicitlyAtBoundaries() = runBlocking {
        val folder=item("folder",kind=FileKind.DIRECTORY,size=null);val child=item("child")
        val source=scanner(mapOf("root" to listOf(folder),"folder" to listOf(child)))
        val intent=request(listOf(folder))
        val complete=source.scan(intent,FileTreeScanner.Limits(maxDepth=2,maxEntries=2))
        assertTrue(complete.complete);assertEquals(1uL,complete.totalBytes)
        for(limits in listOf(FileTreeScanner.Limits(maxDepth=1),FileTreeScanner.Limits(maxEntries=1),
            FileTreeScanner.Limits(maxMetadataBytes=1),FileTreeScanner.Limits(maxMetadataBytes=12)))
            rejected(source.scan(intent,limits),FileScanFailureKind.LIMIT)
        val tooMany=List(4097) { item("$it") }
        rejected(scanner(mapOf("root" to tooMany)).scan(request(listOf(tooMany.first()))),FileScanFailureKind.LIMIT)
        for(invalid in listOf<()->Unit>({ FileTreeScanner.Limits(maxDepth=65) },{ FileTreeScanner.Limits(maxEntries=0) },
            { FileTreeScanner.Limits(maxMetadataBytes=0) },{ FileTreeScanner.Limits(timeoutMillis=0) })) {
            var refused=false;try { invalid() } catch(_:IllegalArgumentException) { refused=true };assertTrue(refused)
        }
    }

    @Test fun repeatedOpaqueDirectoryIsAnErrorRatherThanSilentlySkipped() = runBlocking {
        val folder=item("opaque:folder","Folder",FileKind.DIRECTORY,null)
        val scan=scanner(mapOf("root" to listOf(folder),"opaque:folder" to listOf(folder.copy(name="Again"))))
            .scan(request(listOf(folder)))
        rejected(scan,FileScanFailureKind.REPEATED_DIRECTORY)
        assertEquals("Folder/Again",scan.failure!!.relativePath)
    }

    @Test fun directoryRedirectionAndChangesDuringEnumerationInvalidateThePlan() = runBlocking {
        val folder=item("folder",kind=FileKind.DIRECTORY,size=null);val file=item("file",time=1)
        val intent=request(listOf(folder))
        val redirected=FileTreeScanner { place -> BrowserDirectory(place.copy(directoryId="elsewhere"),listOf(folder)) }.scan(intent)
        rejected(redirected,FileScanFailureKind.SOURCE_CHANGED)
        var childReads=0
        val changed=FileTreeScanner { place -> BrowserDirectory(place,if(place.directoryId=="root") listOf(folder)
            else if(++childReads==1) listOf(file) else listOf(file.copy(size=2uL))) }.scan(intent)
        rejected(changed,FileScanFailureKind.SOURCE_CHANGED);assertEquals("folder",changed.failure!!.relativePath)
        var rootReads=0
        val rootChanged=FileTreeScanner { place -> BrowserDirectory(place,listOf(if(++rootReads==1) file else file.copy(modifiedMillis=2))) }
            .scan(request(listOf(file)))
        rejected(rootChanged,FileScanFailureKind.SOURCE_CHANGED)
        assertEquals("file",rootChanged.failure!!.relativePath)
    }

    @Test fun readFailureRetainsPartialEntriesButEmptyDirectoryIsComplete() = runBlocking {
        val file=item("file");val folder=item("folder",kind=FileKind.DIRECTORY,size=null)
        val failed=scanner(mapOf("root" to listOf(file,folder))).scan(request(listOf(file,folder)))
        rejected(failed,FileScanFailureKind.READ);assertEquals("folder",failed.failure!!.relativePath)
        assertEquals(listOf("file","folder"),failed.entries.map { it.relativePath });assertEquals(1uL,failed.knownBytes)
        assertNull(failed.entries.last().childCount)
        val empty=scanner(mapOf("root" to listOf(folder),"folder" to emptyList())).scan(request(listOf(folder)))
        assertTrue(empty.complete);assertEquals(0,empty.entries.single().childCount);assertEquals(0uL,empty.totalBytes)
    }

    @Test fun cancellationReachesActiveReadAndNeverDeliversAPlan() = runBlocking {
        val folder=item("folder",kind=FileKind.DIRECTORY,size=null)
        val entered=CompletableDeferred<Unit>();val release=CompletableDeferred<Unit>();val cancelled=CompletableDeferred<Boolean>()
        val source=FileTreeScanner { place ->
            if(place.directoryId=="folder") {
                entered.complete(Unit)
                try { release.await() } finally { cancelled.complete(!currentCoroutineContext().isActive) }
            }
            BrowserDirectory(place,if(place.directoryId=="root") listOf(folder) else emptyList())
        }
        var delivered=false
        val job=launch { source.scan(request(listOf(folder)));delivered=true }
        var observed:Boolean?=null
        try {
            withTimeout(3000) { entered.await() };job.cancel()
            observed=withTimeoutOrNull(1000) { cancelled.await() }
        } finally { release.complete(Unit);job.cancelAndJoin() }
        assertEquals("cancellation must reach the active source read",true,observed);assertFalse(delivered)
    }

    @Test fun deadlineReturnsIncompleteAndCancelsTheActiveRead() = runBlocking {
        val folder=item("folder",kind=FileKind.DIRECTORY,size=null);var cancelled=false
        val source=FileTreeScanner { place ->
            if(place.directoryId=="folder") try { awaitCancellation() } finally { cancelled=true }
            BrowserDirectory(place,listOf(folder))
        }
        val scan=source.scan(request(listOf(folder)),FileTreeScanner.Limits(timeoutMillis=100))
        rejected(scan,FileScanFailureKind.TIMEOUT);assertTrue(cancelled)
        assertEquals(listOf("folder"),scan.entries.map { it.relativePath })
    }
}
