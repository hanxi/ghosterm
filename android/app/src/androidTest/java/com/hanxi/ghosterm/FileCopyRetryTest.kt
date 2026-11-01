package com.hanxi.ghosterm

import androidx.test.ext.junit.runners.AndroidJUnit4
import com.hanxi.ghosterm.domain.*
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class FileCopyRetryTest {
    private val source=FileLocation(FileEndpoint.APP,"source")
    private val destination=FileLocation(FileEndpoint.APP,"target")
    private fun entry(name: String, kind: FileKind=FileKind.FILE)=BrowserEntry("source/$name",name,kind,1uL,1)
    private fun request()=FilePasteRequest.capture(FileClipboardSnapshot.capture(source,"source",listOf(entry("tree",FileKind.DIRECTORY),entry("other"))),destination,"target")
    private fun value(path: String, state: FileCopyItemState, kind: FileKind=FileKind.FILE, created: String?=null)=
        FileCopyItemResult(FileScanEntry(source,entry(path.substringAfterLast('/'),kind),path),state,createdId=created)
    @Test fun unknownParentAndKnownCreatedSideEffectsNeverBecomeRetryable() {
        val request=request()
        val items=listOf(value("tree",FileCopyItemState.UNKNOWN,FileKind.DIRECTORY,created="target/tree"),
            value("tree/child",FileCopyItemState.NOT_ATTEMPTED),value("other",FileCopyItemState.CREATED))
        val retry=FileCopyRetry.plan(request,FileCopyProgress(request.id,request.clipboardId,FileCopyState.UNKNOWN,items))
        assertTrue(retry.groups.isEmpty());assertEquals(3,retry.blocked);assertEquals(0,retry.retryItems)
    }
    @Test fun entireUnattemptedSubtreeIsRetriedOnceAndSkippedItemsStaySkipped() {
        val request=request()
        val items=listOf(value("tree/child",FileCopyItemState.NOT_ATTEMPTED),
            value("tree",FileCopyItemState.NOT_ATTEMPTED,FileKind.DIRECTORY),value("other",FileCopyItemState.SKIPPED))
        val retry=FileCopyRetry.plan(request,FileCopyProgress(request.id,request.clipboardId,FileCopyState.CANCELLED,items))
        assertEquals(listOf("tree"),retry.groups.single().entries.map { it.name })
        assertEquals(destination,retry.groups.single().destination)
        assertEquals(2,retry.retryItems);assertEquals(1,retry.retained);assertEquals(0,retry.blocked)
    }
    @Test fun acknowledgedParentIsRequiredAndUnrelatedResultsCannotAuthorizeRetry() {
        val request=request()
        val items=listOf(value("tree",FileCopyItemState.COMPLETE,FileKind.DIRECTORY),value("tree/child",FileCopyItemState.FAILED))
        val retry=FileCopyRetry.plan(request,FileCopyProgress(request.id,request.clipboardId,FileCopyState.FAILED,items))
        assertTrue(retry.groups.isEmpty());assertEquals(1,retry.blocked)
        assertThrows(IllegalArgumentException::class.java) {
            FileCopyRetry.plan(request,FileCopyProgress("other-request",request.clipboardId,FileCopyState.FAILED,items))
        }
    }
}
