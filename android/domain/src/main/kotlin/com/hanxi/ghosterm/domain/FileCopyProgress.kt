package com.hanxi.ghosterm.domain

import java.util.Collections

enum class FileCopyItemState { NOT_ATTEMPTED, RUNNING, COMPLETE, SKIPPED, CONFLICT, FAILED, UNKNOWN, CREATED }
enum class FileCopyState { NOT_STARTED, RUNNING, COMPLETE, WITH_SKIPS, ALL_SKIPPED, FAILED, CANCELLED, UNKNOWN }

/** Actual side effects, independent of list visibility and clipboard replacement.
 * candidatePath is retained when remote publication was not acknowledged. */
data class FileCopyItemResult(val source: FileScanEntry, val state: FileCopyItemState,
    val targetParent: FileLocation? = null, val targetName: String? = null,
    val createdId: String? = null, val documentUri: String? = null,
    val candidatePath: String? = null, val bytes: ULong = 0u, val error: String? = null,
    val coreStatus: Int? = null, val remoteStatus: UInt? = null)

class FileCopyProgress(val requestId: String, val clipboardId: String, val state: FileCopyState,
    items: Collection<FileCopyItemResult>, val error: String? = null) {
    val items: List<FileCopyItemResult> = Collections.unmodifiableList(ArrayList(items))
    val completed get() = items.count { it.state==FileCopyItemState.COMPLETE }
    /** Completed files only. Bytes copied to a temporary candidate are not committed bytes. */
    val committedBytes: ULong get() = items.filter { it.state==FileCopyItemState.COMPLETE && it.source.entry.kind==FileKind.FILE }
        .fold(0uL) { sum,item -> check(ULong.MAX_VALUE-sum>=item.bytes);sum+item.bytes }
    val mayClearClipboard get() = state==FileCopyState.COMPLETE
}
