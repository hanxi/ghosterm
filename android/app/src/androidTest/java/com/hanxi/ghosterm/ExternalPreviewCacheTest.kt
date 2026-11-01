package com.hanxi.ghosterm

import android.content.Intent
import android.content.res.AssetFileDescriptor
import android.net.Uri
import android.os.ParcelFileDescriptor
import android.system.Os
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.hanxi.ghosterm.data.*
import com.hanxi.ghosterm.domain.*
import kotlinx.coroutines.*
import org.junit.After
import org.junit.Assert.*
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File
import java.util.UUID

@RunWith(AndroidJUnit4::class)
class ExternalPreviewCacheTest {
    private val context get()=InstrumentationRegistry.getInstrumentation().targetContext
    private lateinit var directory: File
    private lateinit var cache: ExternalPreviewCache
    private var clock=1_800_000_000_000L
    @Before fun setup() {
        directory=File(context.cacheDir,"external-test-${UUID.randomUUID()}").apply { mkdirs() }
        cache=ExternalPreviewCache(context,File(directory,"copies"),"com.hanxi.ghosterm.previews",{ clock },64,128,1000)
    }
    @After fun cleanup() { directory.deleteRecursively() }
    private suspend fun prepare(bytes: ByteArray="hello".toByteArray(), known: Boolean=true,
        owner: ExternalPreviewCache=cache, name: String="空 文.txt"): ExternalPreviewCache.Ready {
        val input=File(directory,"source-${UUID.randomUUID()}").apply { writeBytes(bytes) }
        return owner.prepare(name,if(known) bytes.size.toULong() else null) { store ->
            store.snapshot(if(known) bytes.size.toLong() else null) {
                AssetFileDescriptor(ParcelFileDescriptor.open(input,ParcelFileDescriptor.MODE_READ_ONLY),0,-1)
            }
        }
    }
    private suspend fun rejects(block: suspend () -> Unit): Exception {
        try { block() } catch(e: Exception) { return e };throw AssertionError("Expected rejection")
    }
    private fun copies()=File(directory,"copies").listFiles()!!.filter { it.name!="staging" }
    @Test fun rawBytesAndDisplayNameArePreservedWithoutExposingTheSource()=runBlocking {
        val raw=byteArrayOf(0,-1,4,8,10)
        val ready=prepare(raw)
        assertEquals("空 文.txt",cache.info(ready.uri).first);assertEquals(5L,ready.bytes)
        assertEquals("text/plain",ready.mime)
        cache.open(ready.uri,"r").use { fd -> ParcelFileDescriptor.AutoCloseInputStream(fd).use { assertArrayEquals(raw,it.readBytes()) } }
        assertEquals(1,copies().size);assertFalse(ready.uri.toString().contains(directory.path))
        assertEquals(256,Os.stat(copies().single().path).st_mode and 511)
    }
    @Test fun unknownAndKnownOversizeCannotBypassStreamingLimit()=runBlocking {
        var opened=false
        rejects { cache.prepare("huge.bin",65u) { opened=true;error("must not read") } }
        assertFalse(opened)
        rejects { prepare(ByteArray(65),known=false) }
        assertTrue(copies().isEmpty())
        assertEquals(5L,prepare().bytes)
    }
    @Test fun wholeCacheCapacityIncludesUnknownSizeReservationsAndReleasedFailures()=runBlocking {
        val first=prepare(ByteArray(64));val second=prepare(ByteArray(64),known=false)
        val failure=rejects { prepare(byteArrayOf(1)) };assertTrue(failure.message!!.contains("预览缓存已满"))
        cache.discard(first);assertEquals(1L,prepare(byteArrayOf(2)).bytes)
        cache.discard(second)
    }
    @Test fun expiredCopiesWaitForConsumerCloseBeforeReclamation()=runBlocking {
        val first=prepare();val descriptor=cache.open(first.uri,"r")
        descriptor.use {
            clock+=1001;cache.sweep()
            assertEquals(1,copies().size)
            ParcelFileDescriptor.AutoCloseInputStream(it).use { input -> assertEquals("hello",input.readBytes().toString(Charsets.UTF_8)) }
        }
        val deadline=System.nanoTime()+5_000_000_000
        while(copies().isNotEmpty() && System.nanoTime()<deadline) { cache.sweep();delay(20) }
        assertTrue(copies().isEmpty());assertNotNull(rejects { cache.open(first.uri,"r") })
    }
    @Test fun restartSweepRemovesOwnedPartialAndExpiredCopiesOnly()=runBlocking {
        val first=prepare()
        val partial=File(directory,"copies/${UUID.randomUUID()}.part").apply { writeText("unfinished") }
        clock+=1001
        val restarted=ExternalPreviewCache(context,File(directory,"copies"),"com.hanxi.ghosterm.previews",{clock},64,128,1000)
        restarted.sweep();assertFalse(partial.exists());assertTrue(copies().isEmpty())
        rejects { restarted.info(first.uri) }
        assertEquals(5L,prepare(owner=restarted).bytes)
    }
    @Test fun cancellationCleansItsPartialCopyAndKeepsTheCacheUsable()=runBlocking {
        val begun=CompletableDeferred<Unit>();val ended=CompletableDeferred<Unit>()
        val job=launch {
            cache.prepare("late.bin",null) {
                begun.complete(Unit)
                try { awaitCancellation() } finally { ended.complete(Unit) }
            }
        }
        begun.await();job.cancelAndJoin();ended.await()
        // Capacity is retained until the actual worker finally, not merely waiter cancellation.
        withTimeout(5000) { while(true) {
            try { assertEquals(5L,prepare().bytes);break } catch(_: IllegalStateException) { delay(10) }
        } }
        assertTrue(copies().none { it.name.endsWith(".part") })
    }
    @Test fun failedDownloadDoesNotPublishAndNormalReadRemainsIndependent()=runBlocking {
        val failure=rejects { cache.prepare("bad.bin",null) { throw IllegalStateException("provider refused") } }
        assertEquals("provider refused",failure.message);assertTrue(copies().isEmpty())
        assertEquals(5L,prepare().bytes)
    }
    @Test fun invalidRootsAndCleanupFailuresAreVisibleInsteadOfAdmitted()=runBlocking {
        val root=File(directory,"not-directory").apply { writeText("retain") }
        val broken=ExternalPreviewCache(context,root,"com.hanxi.ghosterm.previews",fileLimit=64,totalLimit=128)
        rejects { prepare(owner=broken) };assertEquals("retain",root.readText())
        val ready=prepare();val copy=copies().single()
        copy.delete();copy.mkdir();File(copy,"unowned").writeText("keep")
        val error=rejects { cache.sweep() };assertTrue(error.message!!.contains("无法确认"))
        assertEquals("keep",File(copy,"unowned").readText());assertNotNull(ready.uri)
    }
    @Test fun writesTraversalAndExpiredAddressesAreRejected()=runBlocking {
        val ready=prepare()
        for(mode in listOf("w","rw","rwt","wa")) rejects { cache.open(ready.uri,mode).use { } }
        for(uri in listOf(Uri.parse("content://com.hanxi.ghosterm.previews/../../preferences"),
            ready.uri.buildUpon().authority("wrong").build(), ready.uri.buildUpon().appendQueryParameter("path","/data").build(),
            Uri.parse("content://com.hanxi.ghosterm.previews"))) rejects { cache.info(uri) }
        clock+=1001;rejects { cache.open(ready.uri,"r") }
        assertEquals(1,copies().size)
    }
    @Test fun actionViewContainsOnlyTemporaryReadGrantAndClipData()=runBlocking {
        val ready=prepare();val intent=externalPreviewIntent(ready)
        assertEquals(Intent.ACTION_VIEW,intent.action);assertEquals(ready.uri,intent.data);assertEquals("text/plain",intent.type)
        assertEquals(Intent.FLAG_GRANT_READ_URI_PERMISSION,intent.flags)
        assertEquals(ready.uri,intent.clipData!!.getItemAt(0).uri)
        assertNull(intent.component);assertNull(intent.`package`)
    }
    @Test fun realProviderUsesTheSameReadOnlyCacheAndOpenableMetadata()=runBlocking {
        val app=context.applicationContext as GhostermApplication
        val ready=prepare(owner=app.externalPreviews,name="image.png")
        try {
            assertEquals("image/png",context.contentResolver.getType(ready.uri))
            context.contentResolver.query(ready.uri,null,null,null,null)!!.use {
                assertTrue(it.moveToFirst());assertEquals("image.png",it.getString(it.getColumnIndexOrThrow("_display_name")))
                assertEquals(5L,it.getLong(it.getColumnIndexOrThrow("_size")))
            }
            context.contentResolver.openInputStream(ready.uri)!!.use { assertEquals("hello",it.readBytes().toString(Charsets.UTF_8)) }
            rejects { context.contentResolver.openFileDescriptor(ready.uri,"rw").use { } }
            val info=context.packageManager.resolveContentProvider("${context.packageName}.previews",0)!!
            assertFalse(info.exported);assertTrue(info.grantUriPermissions)
        } finally {
            // Wait for Binder/PFD close callbacks before explicitly discarding this test-owned copy.
            withTimeout(5000) { while(true) { try { app.externalPreviews.discard(ready);break } catch(_: IllegalStateException) { delay(20) } } }
        }
    }
    @Test fun actualOtherUidNeedsTheIntentGrantAndCannotWrite()=runBlocking {
        val app=context.applicationContext as GhostermApplication
        val ready=prepare(owner=app.externalPreviews)
        val report=UUID.randomUUID().toString()
        val control=Uri.parse("content://com.hanxi.ghosterm.test.previewcontrol")
        try {
            val before=context.contentResolver.call(control,"probe",ready.uri.toString(),null)!!
            assertNotEquals(android.os.Process.myUid(),before.getInt("uid"));assertTrue(before.getBoolean("denied"))
            context.startActivity(externalPreviewIntent(ready).setClassName("com.hanxi.ghosterm.test",
                "com.hanxi.ghosterm.PreviewReceiverActivity").putExtra("report",report).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK))
            var result: android.os.Bundle?=null
            withTimeout(10_000) { while(result==null) { result=context.contentResolver.call(control,"report",report,null);delay(20) } }
            assertNull(result!!.getString("error"));assertTrue(result!!.getBoolean("writeDenied"))
            assertTrue(result!!.getBoolean("readOnlyFlags"));assertNotEquals(android.os.Process.myUid(),result!!.getInt("uid"))
            assertArrayEquals("hello".toByteArray(),result!!.getByteArray("bytes"))
        } finally {
            withTimeout(5000) { while(true) { try { app.externalPreviews.discard(ready);break } catch(_: IllegalStateException) { delay(20) } } }
        }
    }
    @Test fun realLocalSnapshotRejectsSymlinksAndKeepsOrdinaryBinaryBytes()=runBlocking {
        val home=File(directory,"home").apply { mkdir() }
        val file=File(home,"data.bin").apply { writeBytes(byteArrayOf(0,1,2,-1)) }
        val local=LocalFileBrowser(home,context.contentResolver);val location=FileLocation(FileEndpoint.APP)
        val entry=local.list(location).single()
        val ready=cache.prepare(entry.name,entry.size) { local.snapshot(location,entry,it) }
        assertEquals(4L,ready.bytes)
        Os.symlink(file.path,File(home,"link").path)
        val forged=entry.copy(id="link")
        rejects { cache.prepare(forged.name,null) { local.snapshot(location,forged,it) } }
        assertEquals(1,copies().size)
    }
    @Test fun repositoryRejectsRemoteEntryOutsideDirectoryWithoutConnection()=runBlocking {
        val browser=FileBrowserRepository(LocalFileBrowser(directory,context.contentResolver))
        val endpoint=FileEndpoint("unconnected","remote","/home",remote=RemoteFileIdentity("r","host","user",22))
        val entry=BrowserEntry("/outside/secret","secret",FileKind.FILE,5u,null)
        val error=rejects { cache.prepare(entry.name,entry.size) { browser.snapshot(FileLocation(endpoint,"/home"),entry,it) } }
        assertTrue(error.message?.contains("不属于当前文件夹")==true)
        assertTrue(copies().isEmpty())
    }
    @Test fun cancellationSignalsTheActualReaderBeforePageCapacityReturns()=runBlocking {
        val begun=CompletableDeferred<Unit>();val ended=CompletableDeferred<Unit>();val release=CompletableDeferred<Unit>()
        val job=launch {
            cache.prepare("pending",null) {
                begun.complete(Unit)
                try { release.await();error("test reader released") } finally { ended.complete(Unit) }
            }
        }
        try {
            withTimeout(5000) { begun.await() };job.cancelAndJoin()
            assertTrue("waiter cancellation must cancel the actual preparation worker",withTimeoutOrNull(1500) { ended.await();true }==true)
        } finally { release.complete(Unit);withTimeout(5000) { ended.await() };job.cancelAndJoin() }
    }

    @Test fun unlinkedButOpenCopiesRemainInTheCapacityBudget()=runBlocking {
        val first=prepare(ByteArray(64));prepare(ByteArray(64))
        cache.open(first.uri,"r").use {
            assertTrue(File(directory,"copies/${first.uri.pathSegments[0]}").delete())
            val failure=rejects { prepare(byteArrayOf(1)) }
            assertTrue(failure.message!!.contains("预览缓存已满"))
        }
        withTimeout(5000) { while(true) { try { assertEquals(1L,prepare(byteArrayOf(1)).bytes);break } catch(_: IllegalStateException) { delay(20) } } }
    }
    @Test fun consumerCanSeekWithoutReceivingTheBackingCacheDescriptor()=runBlocking {
        // A five-byte file fits one FUSE page: even a broken callback offset is
        // hidden by the kernel cache. Cross a page boundary before the first read.
        val raw=ByteArray(128*1024) { (it/4096).toByte() }
        val seekCache=ExternalPreviewCache(context,File(directory,"seek"),"com.hanxi.ghosterm.previews",fileLimit=256*1024,totalLimit=512*1024)
        val ready=prepare(raw,owner=seekCache)
        seekCache.open(ready.uri,"r").use { descriptor ->
            assertEquals(raw.size.toLong(),descriptor.statSize)
            Os.lseek(descriptor.fileDescriptor,64*1024+7,android.system.OsConstants.SEEK_SET)
            val data=ByteArray(3);assertEquals(3,Os.read(descriptor.fileDescriptor,data,0,3))
            assertArrayEquals(raw.copyOfRange(64*1024+7,64*1024+10),data)
            Os.lseek(descriptor.fileDescriptor,raw.size+100L,android.system.OsConstants.SEEK_SET)
            assertEquals(0,Os.read(descriptor.fileDescriptor,data,0,3))
        }
    }
    @Test fun cleanupUnlinkFailurePreservesBytesAndBlocksNewAdmission()=runBlocking {
        var deny=false;var read=false
        val tested=ExternalPreviewCache(context,File(directory,"copies"),"com.hanxi.ghosterm.previews",{clock},64,128,1000,
            deleteCopy={ file -> if(deny) false else file.delete() })
        val ready=prepare(owner=tested)
        clock+=1001;deny=true
        assertTrue(rejects { tested.sweep() }.message!!.contains("无法清理过期"))
        assertEquals("hello",copies().single().readText())
        rejects { tested.prepare("new",null) { read=true;error("must not read") } };assertFalse(read)
        deny=false;tested.sweep();assertTrue(copies().isEmpty())
        assertEquals(5L,prepare(owner=tested).bytes)
        assertNotNull(ready.uri)
    }

    @Test fun defaultFileLimitRejectsSeventeenMiBBeforeOpeningTheSource()=runBlocking {
        val defaults=ExternalPreviewCache(context,File(directory,"defaults"),"com.hanxi.ghosterm.previews")
        var opened=false
        val error=rejects { defaults.prepare("large.bin",17uL*1024u*1024u) { opened=true;error("unexpected read") } }
        assertFalse(opened);assertTrue(error.message!!.contains("16 MiB"))
    }
    @Test fun defaultRetentionKeepsTwentyThreeHoursButExpiresAfterTwentyFive()=runBlocking {
        val defaults=ExternalPreviewCache(context,File(directory,"defaults"),"com.hanxi.ghosterm.previews",now={clock})
        val ready=prepare(owner=defaults)
        clock+=23L*60*60*1000
        assertTrue("23-hour copy must remain available",runCatching { defaults.info(ready.uri) }.isSuccess)
        clock+=2L*60*60*1000
        assertTrue("25-hour copy must expire",runCatching { defaults.info(ready.uri) }.isFailure)
        defaults.sweep();assertTrue(File(directory,"defaults").listFiles()!!.none { it.name!="staging" })
    }
    @Test fun emptyFilesCannotBypassTheSixteenCopySlotLimit()=runBlocking {
        repeat(16) { prepare(ByteArray(0)) }
        var opened=false
        val error=rejects { cache.prepare("seventeenth",0u) { opened=true;error("unexpected read") } }
        assertFalse(opened);assertTrue(error.message!!.contains("预览缓存已满"))
        assertEquals(16,copies().size)
    }

    @Test fun explicitClearReclaimsRetiredSlotsAndAllowsAnotherOpen()=runBlocking {
        repeat(16) { cache.handedOff(prepare(ByteArray(0))) }
        rejects { prepare(ByteArray(0)) }
        val result=cache.clearCopies()
        assertEquals(16,result.removed);assertEquals(0L,result.bytes);assertEquals(0,result.retained);assertEquals(0,result.failed)
        assertTrue(copies().isEmpty());assertEquals(1L,prepare(byteArrayOf(7)).bytes)
        assertEquals(18,directory.listFiles()!!.count { it.name.startsWith("source-") })
    }
    @Test fun explicitClearPreservesReadersPendingHandoffsAndActualWriters()=runBlocking {
        val active=prepare();cache.handedOff(active)
        val pending=prepare(byteArrayOf(3))
        val fd=cache.open(active.uri,"r")
        val began=CompletableDeferred<Unit>();val gate=CompletableDeferred<Unit>()
        val input=File(directory,"writer-source").apply { writeBytes(byteArrayOf(4)) }
        val worker=async { cache.prepare("writer.txt",1u) { store ->
            began.complete(Unit);gate.await()
            store.snapshot(1) { AssetFileDescriptor(ParcelFileDescriptor.open(input,ParcelFileDescriptor.MODE_READ_ONLY),0,-1) }
        } }
        try {
            began.await()
            val held=cache.clearCopies();assertEquals(0,held.removed);assertEquals(3,held.retained)
            assertEquals(1L,cache.info(pending.uri).second)
            gate.complete(Unit);val finished=worker.await()
            cache.handedOff(pending);cache.handedOff(finished)
            val cleared=cache.clearCopies();assertEquals(2,cleared.removed);assertEquals(1,cleared.retained)
            ParcelFileDescriptor.AutoCloseInputStream(fd).use { assertEquals("hello",it.readBytes().toString(Charsets.UTF_8)) }
            var removed=0
            withTimeout(5000) { while(true) { val result=cache.clearCopies();removed+=result.removed;if(result.retained==0)break;delay(10) } }
            assertEquals(1,removed);assertTrue(copies().isEmpty());assertArrayEquals(byteArrayOf(4),input.readBytes())
        } finally { fd.close();gate.complete(Unit);worker.cancelAndJoin() }
    }
    @Test fun explicitClearReportsPartialFailureAndCanRetry()=runBlocking {
        var refuse=true;var protected=""
        cache=ExternalPreviewCache(context,File(directory,"copies"),"com.hanxi.ghosterm.previews",{clock},64,128,1000,
            deleteCopy={ if(refuse && it.name==protected) false else it.delete() })
        val first=prepare();protected=first.uri.pathSegments[0];cache.handedOff(first)
        val second=prepare();cache.handedOff(second)
        val result=cache.clearCopies()
        assertEquals(1,result.removed);assertEquals(1,result.failed);assertNotNull(cache.cleanupError.value)
        assertEquals(5L,cache.info(first.uri).second);rejects { cache.info(second.uri) }
        refuse=false;val retried=cache.clearCopies()
        assertEquals(1,retried.removed);assertEquals(0,retried.failed);assertNull(cache.cleanupError.value);assertTrue(copies().isEmpty())
    }
    @Test fun explicitClearRejectsUnknownOwnershipBeforeDeletingAnyCopy()=runBlocking {
        val ready=prepare();cache.handedOff(ready)
        val unknown=File(directory,"copies/unowned.txt").apply { writeText("keep") }
        assertTrue(rejects { cache.clearCopies() }.message!!.contains("无法确认"))
        assertNotNull(cache.cleanupError.value);assertEquals(5L,cache.info(ready.uri).second);assertEquals("keep",unknown.readText())
    }

    @Test fun failedDiscardDoesNotLeaveAnAbandonedPresenterPin()=runBlocking {
        val ready=prepare();val reader=cache.open(ready.uri,"r")
        try {
            assertTrue(rejects { cache.discard(ready) }.message!!.contains("正在使用"))
        } finally { reader.close() }
        var removed=0
        val finished=withTimeoutOrNull(5000) {
            while(true) { val result=cache.clearCopies();removed+=result.removed;if(result.retained==0)break;delay(20) }
            true
        }
        assertTrue("abandoned presenter must not pin the copy after its real reader closes",finished==true)
        assertEquals(1,removed);assertTrue(copies().isEmpty())
    }

}
