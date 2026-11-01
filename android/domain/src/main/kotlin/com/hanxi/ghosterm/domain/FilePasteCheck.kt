package com.hanxi.ghosterm.domain

import java.util.Collections

/** Metadata observations, never a reservation or a promise that a later write succeeds. */
enum class FileCreateCapability { ALLOWED, DENIED, UNKNOWN }
enum class FileLocationRelation { SAME, DESCENDANT, SEPARATE, UNKNOWN }
data class FileTargetDirectory(val directory: BrowserDirectory, val create: FileCreateCapability)
data class FilePasteConflict(val source: BrowserEntry, val destination: BrowserEntry)
enum class FilePasteCheckFailureKind { INCOMPLETE_SOURCE, TARGET_READ, TARGET_CHANGED, TARGET_INVALID, READ_ONLY, SAME_DIRECTORY, INSIDE_SOURCE, RELATION_UNKNOWN, TIMEOUT }
data class FilePasteCheckFailure(val kind: FilePasteCheckFailureKind, val message: String)

class FilePasteCheck(val scan: FileTreeScan, targetEntries: List<BrowserEntry>,
    conflicts: List<FilePasteConflict>, val create: FileCreateCapability,
    val failure: FilePasteCheckFailure?, val namespaceConsent: FileNamespaceConsent? = null) {
    val targetEntries: List<BrowserEntry> = Collections.unmodifiableList(ArrayList(targetEntries))
    val conflicts: List<FilePasteConflict> = Collections.unmodifiableList(ArrayList(conflicts))
    val checked get() = failure == null
    // Unsupported source entries and conflicts still require explicit per-request decisions.
    // UNKNOWN remote write permission must remain visible, then be checked by actual creation.
}

/** User acknowledgement of an unknown relationship, never evidence of separate storage.
 * Bound to this in-memory intent; it is neither serialized nor inherited by retries. */
class FileNamespaceConsent private constructor(private val request: FilePasteRequest) {
    fun appliesTo(candidate: FilePasteRequest) = candidate === request
    companion object {
        fun confirm(review: FilePasteCheck): FileNamespaceConsent {
            require(review.scan.complete && review.failure?.kind==FilePasteCheckFailureKind.RELATION_UNKNOWN)
            require(review.scan.request.source.endpoint.remote!=null && review.scan.request.destination.endpoint.remote!=null)
            return FileNamespaceConsent(review.scan.request)
        }
    }
}
