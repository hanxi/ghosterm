package com.hanxi.ghosterm.domain

/** Durable observations are deliberately not executable PasteRequests or conflict authority. */
enum class FileJournalPhase { QUEUED, SCANNING, CONFIRMING, RUNNING, CANCELLING, FINISHED }
enum class FileCopyCheckpoint { PREPARING, DIRECTORY_CREATE, DIRECTORY_CREATED, DOCUMENT_CREATE, DOCUMENT_CREATED, LOCAL_CREATE, REMOTE_CREATE, PUBLISH, RESULT }
data class FileJournalIntent(val id: String, val clipboardId: String,
    val source: FileLocation, val sourceLabel: String, val selected: List<BrowserEntry>,
    val destination: FileLocation, val destinationLabel: String)
data class FileJournalItem(val value: FileCopyItemResult, val checkpoint: FileCopyCheckpoint)
data class FileJournalRecord(val intent: FileJournalIntent, val phase: FileJournalPhase,
    val updatedMillis: Long, val outcome: FileCopyState? = null, val cancelled: Boolean = false,
    val error: String? = null, val items: List<FileJournalItem> = emptyList()) {
    val interrupted get() = phase != FileJournalPhase.FINISHED
}
