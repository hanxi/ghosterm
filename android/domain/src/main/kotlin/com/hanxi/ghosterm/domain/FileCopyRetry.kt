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
            val indexed=result.items.associateBy { it.source.relativePath }
            require(indexed.size==result.items.size) { "任务路径重复，无法重试" }
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
            result.items.filter { it.state !in safe }.forEach { unsafeSubtrees.addAll(lineage.getValue(it.source.relativePath)) }
            // Parents first, independent of map/event ordering. A pending whole
            // subtree can be retried once only when every descendant is safe.
            for(item in result.items.sortedBy { lineage.getValue(it.source.relativePath).size }) {
                val path=item.source.relativePath
                val parents=lineage.getValue(path)
                if(item.state !in safe || parents.any { it in selected }) continue
                if(item.source.entry.kind==FileKind.DIRECTORY && path in unsafeSubtrees) continue
                val parentResults=parents.map { indexed[it] }
                if(parentResults.any { it==null || it.state!=FileCopyItemState.COMPLETE || it.source.entry.kind!=FileKind.DIRECTORY || it.createdId==null }) continue
                val destination=if(parents.isEmpty()) request.destination else
                    FileLocation(request.destination.endpoint,checkNotNull(parentResults.last()!!.createdId))
                val key=Key(item.source.parent,destination)
                val destinationNames=parentResults.map { checkNotNull(it).targetName ?: it.source.entry.name }
                val group=groups.getOrPut(key) {
                    Pending(if(parents.isEmpty()) request.sourcePathLabel else request.sourcePathLabel+" / "+parents.last(),
                        if(destinationNames.isEmpty()) request.destinationPathLabel else request.destinationPathLabel+" / "+destinationNames.joinToString("/"),mutableListOf())
                }
                group.entries.add(item.source.entry);selected.add(path)
            }
            val covered=result.items.count { item -> item.state in safe &&
                (item.source.relativePath in selected || lineage.getValue(item.source.relativePath).any { it in selected }) }
            val retainedCount=result.items.count { it.state in retained }
            return FileCopyRetry(groups.map { (key,value) -> Group(key.source,value.sourceLabel,key.destination,value.destinationLabel,value.entries) },
                retainedCount,covered,result.items.size-retainedCount-covered)
        }
    }
}
