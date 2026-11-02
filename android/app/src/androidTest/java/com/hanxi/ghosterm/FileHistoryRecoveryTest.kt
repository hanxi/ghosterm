package com.hanxi.ghosterm

import com.hanxi.ghosterm.domain.*
import org.junit.Assert.*
import org.junit.Test

class FileHistoryRecoveryTest {
    private val source=FileLocation(FileEndpoint.APP,"source")
    private val target=FileLocation(FileEndpoint.APP,"target")
    private fun entry(name:String,kind:FileKind=FileKind.FILE)=BrowserEntry("source/$name",name,kind,1uL,1)
    private fun record(roots:List<BrowserEntry>,items:List<FileJournalItem>,phase:FileJournalPhase=FileJournalPhase.RUNNING):FileJournalRecord {
        val request=FilePasteRequest.capture(FileClipboardSnapshot.capture(source,"source",roots),target,"target")
        return FileJournalRecord(FileJournalIntent(request.id,request.clipboardId,source,"source",roots,target,"target"),phase,0,items=items)
    }
    private fun saved(entry:BrowserEntry,state:FileCopyItemState=FileCopyItemState.UNKNOWN,point:FileCopyCheckpoint=FileCopyCheckpoint.RESULT,
        parent:FileLocation=source,path:String=entry.name)=FileJournalItem(FileCopyItemResult(FileScanEntry(parent,entry,path),state,target,entry.name),point)
    @Test fun selectionIsEmptyUntilUserChoosesAndCannotSelectForeignOrCompletedRoot() {
        val a=entry("a");val done=entry("done");val old=record(listOf(a,done),listOf(saved(a),saved(done,FileCopyItemState.COMPLETE)))
        val plan=FileHistoryRecovery.plan(old)
        assertEquals(listOf(a),plan.entries)
        assertThrows(IllegalArgumentException::class.java) { plan.select(emptySet()) }
        assertThrows(IllegalArgumentException::class.java) { plan.select(setOf(done.id)) }
        assertThrows(IllegalArgumentException::class.java) { plan.select(setOf("foreign")) }
        assertEquals(listOf(a),plan.select(setOf(a.id)))
    }
    @Test fun sideEffectCheckpointNeedsExplicitRecoveryDespiteStaleFailedState() {
        val a=entry("a")
        for(point in FileCopyCheckpoint.entries.filter { it!=FileCopyCheckpoint.RESULT }) {
            val old=record(listOf(a),listOf(saved(a,FileCopyItemState.FAILED,point)))
            assertTrue(FileCopyRetry.plan(old).groups.isEmpty())
            assertEquals("$point",listOf(a),FileHistoryRecovery.plan(old).entries)
        }
    }
    @Test fun untouchedOrAcknowledgedSafeResultsDoNotOfferUncertainRecovery() {
        val a=entry("a")
        for(state in listOf(FileCopyItemState.COMPLETE,FileCopyItemState.SKIPPED,FileCopyItemState.NOT_ATTEMPTED,FileCopyItemState.FAILED,FileCopyItemState.CONFLICT))
            assertTrue(FileHistoryRecovery.plan(record(listOf(a),listOf(saved(a,state)))).entries.isEmpty())
        val untouched=saved(a,FileCopyItemState.NOT_ATTEMPTED,FileCopyCheckpoint.PREPARING)
        assertTrue(FileHistoryRecovery.plan(record(listOf(a),listOf(untouched))).entries.isEmpty())
        val dirty=untouched.copy(value=untouched.value.copy(candidatePath="target/.candidate"))
        assertEquals(listOf(a),FileHistoryRecovery.plan(record(listOf(a),listOf(dirty))).entries)
    }
    @Test fun missingResultAfterRunningOffersOriginalRootsButQueuedIntentDoesNot() {
        val a=entry("a")
        for(phase in listOf(FileJournalPhase.RUNNING,FileJournalPhase.CANCELLING))
            assertEquals(listOf(a),FileHistoryRecovery.plan(record(listOf(a),emptyList(),phase)).entries)
        for(phase in listOf(FileJournalPhase.QUEUED,FileJournalPhase.SCANNING,FileJournalPhase.CONFIRMING,FileJournalPhase.FINISHED))
            assertTrue(FileHistoryRecovery.plan(record(listOf(a),emptyList(),phase)).entries.isEmpty())
        assertEquals(listOf(a),FileHistoryRecovery.plan(record(listOf(a),emptyList(),FileJournalPhase.FINISHED).copy(outcome=FileCopyState.UNKNOWN)).entries)
    }
    @Test fun uncertainChildSelectsOriginalTreeWithoutGuessingCreatedParentOrRewritingOldResults() {
        val tree=entry("tree",FileKind.DIRECTORY);val child=BrowserEntry("source/tree/child","child",FileKind.FILE,1uL,1)
        val directory=saved(tree,FileCopyItemState.COMPLETE).copy(value=saved(tree,FileCopyItemState.COMPLETE).value.copy(createdId="target/renamed"))
        val unknown=saved(child,parent=source.copy(directoryId="source/tree"),path="tree/child")
        val old=record(listOf(tree),listOf(directory,unknown));val before=old.copy(items=old.items.toList())
        val recovery=FileHistoryRecovery.plan(old)
        assertEquals(listOf(tree),recovery.entries)
        assertEquals(listOf(tree),recovery.select(setOf(tree.id)))
        assertEquals(before,old);assertEquals("target/renamed",old.items.first().value.createdId)
        assertTrue(FileCopyRetry.plan(old).groups.isEmpty())
    }
    @Test fun explicitSelectionCreatesFreshLinkedRequestWithAskAndNoPreviousConflictAuthority() {
        val a=entry("a");val old=record(listOf(a),listOf(saved(a)))
        val entries=FileHistoryRecovery.plan(old).select(setOf(a.id))
        val fresh=FilePasteRequest.capture(FileClipboardSnapshot.capture(old.intent.source,old.intent.sourceLabel,entries),old.intent.destination,old.intent.destinationLabel,retryOf=old.intent.id)
        assertNotEquals(old.intent.id,fresh.id);assertNotEquals(old.intent.clipboardId,fresh.clipboardId)
        assertEquals(old.intent.id,fresh.retryOf);assertEquals(FileConflictPolicy.ASK,fresh.conflictPolicy)
        assertEquals(FileCopyItemState.UNKNOWN,old.items.single().value.state)
    }
    @Test fun recoveryOptionsAreImmutableSnapshotsAndKeepOriginalSelectionOrder() {
        val a=entry("a");val b=entry("b");val roots=mutableListOf(a,b)
        val old=record(roots,emptyList())
        val plan=FileHistoryRecovery.plan(old)
        roots.clear()
        assertEquals(listOf(a,b),plan.entries)
        assertEquals(listOf(a,b),plan.select(linkedSetOf(b.id,a.id)))
        assertThrows(UnsupportedOperationException::class.java) { (plan.entries as MutableList<BrowserEntry>).clear() }
    }
    @Test fun rootSourceAndParentValidationStillAppliesBeforeOfferingRecovery() {
        val a=entry("a");val old=record(listOf(a),listOf(saved(a.copy(id="foreign"))))
        assertThrows(IllegalArgumentException::class.java) { FileHistoryRecovery.plan(old) }
        val foreign=saved(a).let { it.copy(value=it.value.copy(targetParent=FileLocation(FileEndpoint("foreign","foreign","")))) }
        assertThrows(IllegalArgumentException::class.java) { FileHistoryRecovery.plan(record(listOf(a),listOf(foreign))) }
    }
}
