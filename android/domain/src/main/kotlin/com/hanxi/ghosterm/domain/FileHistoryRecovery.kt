package com.hanxi.ghosterm.domain

import java.util.Collections

/** Explicit selection for a NEW copy after an uncertain old outcome.
 * Never promotes old results, reuses conflict decisions, or grants cleanup authority. */
class FileHistoryRecovery private constructor(entries: List<BrowserEntry>) {
    val entries: List<BrowserEntry> = Collections.unmodifiableList(ArrayList(entries))
    fun select(ids: Set<String>): List<BrowserEntry> {
        val selected=ids.toSet()
        require(selected.isNotEmpty()) { "请明确选择要重新检查的来源" }
        require(selected.all { id -> entries.any { it.id==id } }) { "选择不属于本次结果不明的来源" }
        return Collections.unmodifiableList(ArrayList(entries.filter { it.id in selected }))
    }
    companion object {
        fun plan(record: FileJournalRecord): FileHistoryRecovery {
            // Reuse the full source/parent/endpoint validation. Ordinary safe retry
            // keeps its existing semantics and never opts into uncertain items.
            FileCopyRetry.plan(record)
            val roots=record.intent.selected
            require(roots.map { it.name }.toSet().size==roots.size) { "来源名称重复，请重新选择文件" }
            require(roots.map { it.id }.toSet().size==roots.size) { "来源标识重复，请重新选择文件" }
            if(record.items.isEmpty()) {
                val uncertain=record.phase in setOf(FileJournalPhase.RUNNING,FileJournalPhase.CANCELLING) ||
                    record.outcome==FileCopyState.UNKNOWN
                return FileHistoryRecovery(if(uncertain) roots else emptyList())
            }
            val uncertainRoots=record.items.filter { saved ->
                val untouched=saved.checkpoint==FileCopyCheckpoint.PREPARING &&
                    saved.value.state in setOf(FileCopyItemState.NOT_ATTEMPTED,FileCopyItemState.SKIPPED) &&
                    saved.value.createdId==null && saved.value.documentUri==null && saved.value.candidatePath==null && saved.value.bytes==0uL
                (!untouched && saved.checkpoint!=FileCopyCheckpoint.RESULT) ||
                    saved.value.state in setOf(FileCopyItemState.RUNNING,FileCopyItemState.CREATED,FileCopyItemState.UNKNOWN)
            }.map { it.value.source.relativePath.substringBefore('/') }.toSet()
            // Start at original selected roots. Never guess a destination for a
            // directory whose create/publish acknowledgement was lost.
            return FileHistoryRecovery(roots.filter { it.name in uncertainRoots })
        }
    }
}
