package com.hanxi.ghosterm.domain

import java.util.Collections

enum class FileCopyItemState { NOT_ATTEMPTED, RUNNING, COMPLETE, SKIPPED, CONFLICT, FAILED, UNKNOWN, CREATED }
enum class FileCopyState { NOT_STARTED, RUNNING, COMPLETE, WITH_SKIPS, ALL_SKIPPED, FAILED, CANCELLED, UNKNOWN }

enum class FileCopyPause { RUNNING, REQUESTED, PAUSED }

enum class FileCopyStage { READING, WRITING, VERIFYING }
/** Transient IO observations; neither completed bytes nor a restart checkpoint.
 * Monotonic timestamps belong to this process. Total may be unknown, or smaller
 * than actual bytes when the source changed; never hide those observations. */
data class FileCopyActivity(val relativePath: String, val stage: FileCopyStage,
    val bytes: ULong, val totalBytes: ULong?, val startedMillis: Long, val observedMillis: Long, val pausedMillis: Long = 0) {
    init { require(startedMillis>=0 && observedMillis>=startedMillis && pausedMillis>=0) }
    fun averageBytesPerSecond(nowMillis: Long): Double? {
        val elapsed=nowMillis-startedMillis-pausedMillis
        return if(stage==FileCopyStage.VERIFYING || elapsed<1000 || bytes==0uL) null else bytes.toDouble()*1000/elapsed
    }
    /** Current byte stage only. Unknown size, close/verification, a reached total
     * or stale IO observations do not support a remaining-time estimate. */
    fun estimatedSeconds(nowMillis: Long): Long? {
        val total=totalBytes ?: return null
        if(bytes>=total || nowMillis-observedMillis>2000) return null
        val rate=averageBytesPerSecond(nowMillis) ?: return null
        return kotlin.math.ceil((total-bytes).toDouble()/rate).toLong()
    }
}

/** Request-wide byte work, counting source reads and destination writes.
 * This is distinct from committed bytes. No estimate for unknown totals,
 * stale observations, verification or exhausted byte work awaiting commit. */
data class FileCopyWork(val bytes: ULong, val totalBytes: ULong?, val startedMillis: Long,
    val observedMillis: Long, val pausedMillis: Long = 0, val verifying: Boolean = false) {
    init { require(startedMillis>=0 && observedMillis>=startedMillis && pausedMillis>=0) }
    fun estimatedSeconds(nowMillis: Long): Long? {
        val total=totalBytes ?: return null
        val elapsed=nowMillis-startedMillis-pausedMillis
        if(verifying || total<=bytes || bytes==0uL || elapsed<1000 || nowMillis-observedMillis>2000 || nowMillis<observedMillis) return null
        val rate=bytes.toDouble()*1000/elapsed
        return kotlin.math.ceil((total-bytes).toDouble()/rate).toLong()
    }
}

/** Actual side effects, independent of list visibility and clipboard replacement.
 * candidatePath is retained when remote publication was not acknowledged. */
data class FileCopyItemResult(val source: FileScanEntry, val state: FileCopyItemState,
    val targetParent: FileLocation? = null, val targetName: String? = null,
    val createdId: String? = null, val documentUri: String? = null,
    val candidatePath: String? = null, val bytes: ULong = 0u, val error: String? = null,
    val coreStatus: Int? = null, val remoteStatus: UInt? = null, val sourceDigest: FileContentDigest? = null,
    val localIdentity: LocalFileIdentityReference? = null)

class FileCopyProgress(val requestId: String, val clipboardId: String, val state: FileCopyState,
    items: Collection<FileCopyItemResult>, val error: String? = null, val activity: FileCopyActivity? = null, val pause: FileCopyPause = FileCopyPause.RUNNING, val work: FileCopyWork? = null) {
    val items: List<FileCopyItemResult> = Collections.unmodifiableList(ArrayList(items))
    val completed get() = items.count { it.state==FileCopyItemState.COMPLETE }
    /** Completed files only. Bytes copied to a temporary candidate are not committed bytes. */
    val committedBytes: ULong get() = items.filter { it.state==FileCopyItemState.COMPLETE && it.source.entry.kind==FileKind.FILE }
        .fold(0uL) { sum,item -> check(ULong.MAX_VALUE-sum>=item.bytes);sum+item.bytes }
    val mayClearClipboard get() = state==FileCopyState.COMPLETE
}
