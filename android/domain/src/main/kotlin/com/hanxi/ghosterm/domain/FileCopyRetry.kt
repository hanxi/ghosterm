package com.hanxi.ghosterm.domain

import java.util.Collections

/** A retry is fresh intent, never a resume token or permission to overwrite.
 * Completed/skipped entries remain in the old task. Acknowledged directories
 * may be selected as destinations; their identifiers are checked by ordinary
 * preflight again. No path is guessed for an unacknowledged parent. */
class FileCopyRetry private constructor(groups: List<Group>, val retained: Int,
    val retryItems: Int, val blocked: Int) {
    class Group(val source: FileLocation, val sourceLabel: String,
        val destination: FileLocation, val destinationLabel: String, entries: List<BrowserEntry>) {
        val entries: List<BrowserEntry> = Collections.unmodifiableList(ArrayList(entries))
    }
    val groups: List<Group> = Collections.unmodifiableList(ArrayList(groups))
    companion object {
        fun plan(request: FilePasteRequest, result: FileCopyProgress): FileCopyRetry {
            require(result.requestId==request.id && result.clipboardId==request.clipboardId) { "重试结果不属于此任务" }
            require(result.state !in setOf(FileCopyState.RUNNING,FileCopyState.NOT_STARTED)) { "请等待原任务结束" }
            return plan(request.sourcePathLabel,request.destination,request.destinationPathLabel,result.items)
        }
        /** Journal checkpoints are observations, never permission to resume. Only an
         * acknowledged result or an untouched plan item can seed a new user intent. */
        fun plan(record: FileJournalRecord): FileCopyRetry {
            val intent=record.intent
            if(record.items.isEmpty()) {
                val beforeWrite=record.phase in setOf(FileJournalPhase.QUEUED,FileJournalPhase.SCANNING,FileJournalPhase.CONFIRMING) ||
                    (record.phase==FileJournalPhase.FINISHED && record.outcome in setOf(null,FileCopyState.NOT_STARTED,FileCopyState.FAILED,FileCopyState.CANCELLED))
                return if(beforeWrite) FileCopyRetry(listOf(Group(intent.source,intent.sourceLabel,intent.destination,intent.destinationLabel,intent.selected)),0,intent.selected.size,0)
                    else FileCopyRetry(emptyList(),0,0,intent.selected.size)
            }
            val roots=intent.selected.associateBy { it.name }
            require(roots.size==intent.selected.size) { "原任务来源名称有歧义，请重新选择" }
            val byPath=record.items.associateBy { it.value.source.relativePath }
            require(byPath.size==record.items.size) { "历史项目路径重复" }
            for(saved in record.items) {
                val source=saved.value.source
                val root=roots[source.relativePath.substringBefore('/')]
                require(root!=null && source.parent.endpoint==intent.source.endpoint) { "历史项目不属于原来源" }
                if('/' !in source.relativePath) require(source.parent==intent.source && source.entry==root) { "历史来源身份不一致" }
                else {
                    val parent=byPath[source.relativePath.substringBeforeLast('/')]?.value?.source
                    require(parent!=null && parent.entry.kind==FileKind.DIRECTORY && source.parent.directoryId==parent.entry.id) { "历史来源父目录不完整或不一致" }
                }
                require(saved.value.targetParent==null || saved.value.targetParent.endpoint==intent.destination.endpoint) { "历史目标端点不一致" }
            }
            val items=record.items.map { saved ->
                val known=saved.checkpoint==FileCopyCheckpoint.RESULT ||
                    (saved.checkpoint==FileCopyCheckpoint.PREPARING && saved.value.state in setOf(FileCopyItemState.NOT_ATTEMPTED,FileCopyItemState.SKIPPED) &&
                        saved.value.createdId==null && saved.value.documentUri==null && saved.value.candidatePath==null && saved.value.bytes==0uL)
                if(known) saved.value else saved.value.copy(state=FileCopyItemState.UNKNOWN)
            }
            return plan(intent.sourceLabel,intent.destination,intent.destinationLabel,items)
        }
        private fun plan(sourceLabel: String, target: FileLocation, targetLabel: String, items: List<FileCopyItemResult>): FileCopyRetry {
            val indexed=items.associateBy { it.source.relativePath }
            require(indexed.size==items.size) { "任务路径重复，无法重试" }
            val safe=setOf(FileCopyItemState.NOT_ATTEMPTED,FileCopyItemState.FAILED,FileCopyItemState.CONFLICT)
            val retained=setOf(FileCopyItemState.COMPLETE,FileCopyItemState.SKIPPED)
            val selected=mutableSetOf<String>()
            data class Key(val source: FileLocation, val destination: FileLocation)
            data class Pending(val sourceLabel: String, val destinationLabel: String, val entries: MutableList<BrowserEntry>)
            val groups=linkedMapOf<Key,Pending>()
            fun ancestors(path: String): List<String> {
                val tokens=path.split('/')
                require(tokens.isNotEmpty() && tokens.size<=65 && tokens.none { it.isEmpty() || it=="." || it==".." || '\u0000' in it }) { "任务相对路径无效" }
                return (1 until tokens.size).map { tokens.take(it).joinToString("/") }
            }
            val lineage=indexed.keys.associateWith(::ancestors)
            val unsafeSubtrees=mutableSetOf<String>()
            items.filter { it.state !in safe }.forEach { unsafeSubtrees.addAll(lineage.getValue(it.source.relativePath)) }
            // Parents first, independent of map/event ordering. A pending whole
            // subtree can be retried once only when every descendant is safe.
            for(item in items.sortedBy { lineage.getValue(it.source.relativePath).size }) {
                val path=item.source.relativePath
                val parents=lineage.getValue(path)
                if(item.state !in safe || parents.any { it in selected }) continue
                if(item.source.entry.kind==FileKind.DIRECTORY && path in unsafeSubtrees) continue
                val parentResults=parents.map { indexed[it] }
                if(parentResults.any { it==null || it.state!=FileCopyItemState.COMPLETE || it.source.entry.kind!=FileKind.DIRECTORY || it.createdId==null }) continue
                val destination=if(parents.isEmpty()) target else
                    FileLocation(target.endpoint,checkNotNull(parentResults.last()!!.createdId))
                val key=Key(item.source.parent,destination)
                val destinationNames=parentResults.map { checkNotNull(it).targetName ?: it.source.entry.name }
                val group=groups.getOrPut(key) {
                    Pending(if(parents.isEmpty()) sourceLabel else sourceLabel+" / "+parents.last(),
                        if(destinationNames.isEmpty()) targetLabel else targetLabel+" / "+destinationNames.joinToString("/"),mutableListOf())
                }
                group.entries.add(item.source.entry);selected.add(path)
            }
            val covered=items.count { item -> item.state in safe &&
                (item.source.relativePath in selected || lineage.getValue(item.source.relativePath).any { it in selected }) }
            val retainedCount=items.count { it.state in retained }
            return FileCopyRetry(groups.map { (key,value) -> Group(key.source,value.sourceLabel,key.destination,value.destinationLabel,value.entries) },
                retainedCount,covered,items.size-retainedCount-covered)
        }
    }
}
