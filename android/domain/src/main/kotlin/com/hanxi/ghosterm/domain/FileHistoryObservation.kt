package com.hanxi.ghosterm.domain

/** Current metadata only. Presence never proves task ownership or copy success. */
enum class FileHistoryPresence { PRESENT, ABSENT, UNKNOWN }
data class FileHistoryObservation(val relativePath: String, val candidate: Boolean,
    val presence: FileHistoryPresence, val location: String?, val entry: BrowserEntry? = null,
    val error: String? = null)
data class FileHistoryInspection(val requestId: String, val observedMillis: Long,
    val observations: List<FileHistoryObservation>)

/** Observed bytes, never task ownership or permission to delete/replay. */
data class FileContentDigest(val bytes: Long, val sha256: String) {
    init { require(bytes>=0 && sha256.matches(Regex("[0-9a-f]{64}"))) { "文件内容摘要无效" } }
}
data class FileHistoryContentKey(val requestId: String, val relativePath: String, val candidate: Boolean)
enum class FileHistoryContentState { MATCH, DIFFERENT, MISSING, NO_REFERENCE, UNKNOWN }
data class FileHistoryContentResult(val key: FileHistoryContentKey, val state: FileHistoryContentState,
    val expected: FileContentDigest?, val actual: FileContentDigest? = null,
    val observedMillis: Long = System.currentTimeMillis(), val error: String? = null)
