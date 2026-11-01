package com.hanxi.ghosterm

import androidx.test.platform.app.InstrumentationRegistry
import com.hanxi.ghosterm.data.*
import com.hanxi.ghosterm.domain.*
import kotlinx.coroutines.*
import org.junit.Assert.*
import org.junit.Test
import java.io.File
import java.io.IOException
import java.util.UUID

class FileHistoryInspectionTest {
    private val destination=FileLocation(FileEndpoint.APP,"target")
    private val entry=BrowserEntry("source/a","a",FileKind.FILE,3u,0)
    private fun item(parent: FileLocation=destination, name: String="a", id: String?=null, candidate: String?=null)=
        FileJournalItem(FileCopyItemResult(FileScanEntry(FileLocation(FileEndpoint.APP,"source"),entry,"a"),
            FileCopyItemState.UNKNOWN,parent,name,createdId=id,candidatePath=candidate),FileCopyCheckpoint.PUBLISH)
    private fun record(items: List<FileJournalItem>, target: FileLocation=destination)=FileJournalRecord(
        FileJournalIntent(UUID.randomUUID().toString(),UUID.randomUUID().toString(),FileLocation(FileEndpoint.APP,"source"),"source",listOf(entry),target,"target"),
        FileJournalPhase.RUNNING,0,items=items)
    @Test fun realLocalMetadataDoesNotPromoteUnknownOrChangeFiles()= runBlocking {
        val context=InstrumentationRegistry.getInstrumentation().targetContext
        val home=File(context.cacheDir,"history-inspection-${UUID.randomUUID()}").apply { mkdirs() }
        try {
            File(home,"target").mkdir();File(home,"target/a").writeText("abc");File(home,"target/.candidate").writeText("partial")
            val saved=record(listOf(item(candidate="target/.candidate")))
            val result=FileHistoryInspector(LocalFileBrowser(home,context.contentResolver)::browse).inspect(saved)
            assertEquals(listOf(FileHistoryPresence.PRESENT,FileHistoryPresence.PRESENT),result.observations.map { it.presence })
            assertEquals(3uL,result.observations.first().entry!!.size)
            assertEquals(FileCopyItemState.UNKNOWN,saved.items.single().value.state)
            assertEquals("abc",File(home,"target/a").readText());assertEquals("partial",File(home,"target/.candidate").readText())
            assertEquals(setOf("a",".candidate"),File(home,"target").list()!!.toSet())
        } finally { home.deleteRecursively() }
    }
    @Test fun unreadableDirectoryRemainsUnknownInsteadOfAbsent()=runBlocking {
        val out=FileHistoryInspector { throw IOException("permission denied") }.inspect(record(listOf(item())))
        assertEquals(FileHistoryPresence.UNKNOWN,out.observations.single().presence)
        assertEquals("permission denied",out.observations.single().error)
    }
    @Test fun successfulEmptyDirectoryReportsOnlyCurrentAbsence()=runBlocking {
        val saved=record(listOf(item()))
        val out=FileHistoryInspector { BrowserDirectory(it,emptyList()) }.inspect(saved)
        assertEquals(FileHistoryPresence.ABSENT,out.observations.single().presence)
        assertEquals(FileCopyItemState.UNKNOWN,saved.items.single().value.state)
    }
    @Test fun safWithoutReturnedIdNeverGuessesByDisplayName()=runBlocking {
        val target=FileLocation(FileEndpoint("tree","tree","root","content://example/tree/root"),"root")
        var reads=0
        val out=FileHistoryInspector { reads++;BrowserDirectory(it,listOf(entry)) }.inspect(record(listOf(item(target)),target))
        assertEquals(FileHistoryPresence.UNKNOWN,out.observations.single().presence);assertEquals(0,reads)
    }
    @Test fun recordedIdDoesNotFallBackToSameNameReplacement()=runBlocking {
        val out=FileHistoryInspector { BrowserDirectory(it,listOf(entry.copy(id="replacement"))) }
            .inspect(record(listOf(item(id="original"))))
        assertEquals(FileHistoryPresence.ABSENT,out.observations.single().presence)
    }
    @Test fun ambiguousNameIsUnknown()=runBlocking {
        val out=FileHistoryInspector { BrowserDirectory(it,listOf(entry,entry.copy(id="another"))) }.inspect(record(listOf(item())))
        assertEquals(FileHistoryPresence.UNKNOWN,out.observations.single().presence)
    }
    @Test fun candidateCannotEscapeRecordedParent()=runBlocking {
        var reads=0
        val out=FileHistoryInspector { reads++;BrowserDirectory(it,emptyList()) }.inspect(record(listOf(item(candidate="target/../private"))))
        assertEquals(FileHistoryPresence.UNKNOWN,out.observations.last().presence);assertEquals(1,reads)
    }
    @Test fun changedDirectoryIdentityIsUnknown()=runBlocking {
        val out=FileHistoryInspector { BrowserDirectory(it.copy(directoryId="elsewhere"),emptyList()) }.inspect(record(listOf(item())))
        assertEquals(FileHistoryPresence.UNKNOWN,out.observations.single().presence)
    }
    @Test fun endpointMismatchIsRejectedBeforeReading()=runBlocking {
        var reads=0
        val other=FileLocation(FileEndpoint("other","other",""),"target")
        val out=FileHistoryInspector { reads++;BrowserDirectory(it,emptyList()) }.inspect(record(listOf(item(other))))
        assertEquals(FileHistoryPresence.UNKNOWN,out.observations.single().presence);assertEquals(0,reads)
    }
    @Test fun cancellationPropagatesWithoutPublishingPartialResult()=runBlocking {
        try {
            FileHistoryInspector { throw CancellationException("cancelled") }.inspect(record(listOf(item())))
            fail("Cancellation must propagate")
        } catch(expected: CancellationException) { assertEquals("cancelled",expected.message) }
    }
    @Test fun sameDirectoryIsReadOnceAndFailuresAreShared()=runBlocking {
        var reads=0
        val out=FileHistoryInspector { reads++;throw IOException("unavailable") }.inspect(record(listOf(item(),item(name="b"))))
        assertEquals(1,reads);assertTrue(out.observations.all { it.presence==FileHistoryPresence.UNKNOWN })
    }
    @Test fun perDirectoryEntryBudgetRejectsIncompleteView()=runBlocking {
        val out=FileHistoryInspector { BrowserDirectory(it,(0..4096).map { n -> entry.copy(id="id$n",name="name$n") }) }.inspect(record(listOf(item())))
        assertEquals(FileHistoryPresence.UNKNOWN,out.observations.single().presence)
    }
    @Test fun aggregateEntryBudgetStopsAdditionalReads()=runBlocking {
        var reads=0
        val saved=record((0..4).map { item(destination.copy(directoryId="target/$it")) })
        val out=FileHistoryInspector { reads++;BrowserDirectory(it,(0..4095).map { n -> entry.copy(id="id$n",name="name$n") }) }.inspect(saved)
        assertEquals(4,reads);assertEquals(FileHistoryPresence.UNKNOWN,out.observations.last().presence)
    }
    @Test fun safRecordedIdFindsRenamedDocumentWithoutNameFallback()=runBlocking {
        val target=FileLocation(FileEndpoint("tree","tree","root","content://example/tree/root"),"root")
        val out=FileHistoryInspector { BrowserDirectory(it,listOf(entry.copy(id="stable-id",name="renamed"))) }
            .inspect(record(listOf(item(target,id="stable-id")),target))
        assertEquals(FileHistoryPresence.PRESENT,out.observations.single().presence)
        assertEquals("renamed",out.observations.single().entry!!.name)
    }
    @Test fun directoryBudgetStopsFurtherReads()=runBlocking {
        var reads=0
        val saved=record((0..256).map { item(destination.copy(directoryId="target/$it")) })
        val out=FileHistoryInspector { reads++;BrowserDirectory(it,emptyList()) }.inspect(saved)
        assertEquals(256,reads);assertEquals(FileHistoryPresence.UNKNOWN,out.observations.last().presence)
    }
}
