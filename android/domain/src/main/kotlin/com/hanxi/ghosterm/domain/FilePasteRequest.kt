package com.hanxi.ghosterm.domain

import java.util.Collections
import java.util.UUID

enum class FileConflictPolicy { ASK, KEEP_BOTH, SKIP }

/** Frozen user intent. This is not authorization, a validated destination or a running task. */
class FilePasteRequest private constructor(
    val id: String,
    val clipboardId: String,
    val source: FileLocation,
    val sourcePathLabel: String,
    val items: List<BrowserEntry>,
    val destination: FileLocation,
    val destinationPathLabel: String,
    val conflictPolicy: FileConflictPolicy
) {
    companion object {
        fun capture(clipboard: FileClipboardSnapshot, destination: FileLocation,
            destinationPathLabel: String, conflictPolicy: FileConflictPolicy = FileConflictPolicy.ASK) =
            FilePasteRequest(UUID.randomUUID().toString(), clipboard.id, clipboard.source,
                clipboard.sourcePathLabel, Collections.unmodifiableList(ArrayList(clipboard.items)),
                destination, destinationPathLabel, conflictPolicy)
    }
}

enum class FileScanFailureKind { READ, SOURCE_CHANGED, INVALID_ENTRY, REPEATED_DIRECTORY, LIMIT, TIMEOUT }
data class FileScanFailure(val kind: FileScanFailureKind, val relativePath: String, val message: String)

/** Source metadata only. A directory with childCount == 0 is an observed empty directory.
 * LINK/OTHER entries remain visible for an explicit unsupported/skip decision. */
data class FileScanEntry(val parent: FileLocation, val entry: BrowserEntry,
    val relativePath: String, val childCount: Int? = null)

class FileTreeScan(
    val request: FilePasteRequest,
    entries: List<FileScanEntry>,
    val failure: FileScanFailure?,
    val knownBytes: ULong,
    val unknownSizeFiles: Int
) {
    val entries: List<FileScanEntry> = Collections.unmodifiableList(ArrayList(entries))
    val complete get() = failure == null
    val files get() = entries.count { it.entry.kind == FileKind.FILE }
    val directories get() = entries.count { it.entry.kind == FileKind.DIRECTORY }
    val unsupported get() = entries.count { it.entry.kind == FileKind.LINK || it.entry.kind == FileKind.OTHER }
    /** A complete enumeration is still not a content snapshot or permission to write. */
    val totalBytes: ULong? get() = if(complete && unknownSizeFiles == 0 && unsupported == 0) knownBytes else null
}
