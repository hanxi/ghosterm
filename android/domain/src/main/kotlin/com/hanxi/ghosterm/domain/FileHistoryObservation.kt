package com.hanxi.ghosterm.domain

/** Current metadata only. Presence never proves task ownership or copy success. */
enum class FileHistoryPresence { PRESENT, ABSENT, UNKNOWN }
data class FileHistoryObservation(val relativePath: String, val candidate: Boolean,
    val presence: FileHistoryPresence, val location: String?, val entry: BrowserEntry? = null,
    val error: String? = null)
data class FileHistoryInspection(val requestId: String, val observedMillis: Long,
    val observations: List<FileHistoryObservation>)
