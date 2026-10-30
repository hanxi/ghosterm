package com.hanxi.ghosterm.data

import com.hanxi.ghosterm.domain.*
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.withTimeoutOrNull
import kotlinx.coroutines.yield

/** Side-effect-free source enumeration. The caller owns the source connection and dispatcher.
 * No preview, file reads, target queries, authentication or writes are performed here.
 * Cancelling the caller cancels the active browse; a provider may take longer to release IO.
 */
class FileTreeScanner(private val browse: suspend (FileLocation) -> BrowserDirectory) {
    data class Limits(val maxDepth: Int = 32, val maxEntries: Int = 16384,
        val maxMetadataBytes: Int = 4 * 1024 * 1024, val timeoutMillis: Long = 30000) {
        init {
            require(maxDepth in 1..64 && maxEntries in 1..16384)
            require(maxMetadataBytes in 1..16*1024*1024 && timeoutMillis in 1..120000)
        }
    }
    private class Abort(val failure: FileScanFailure) : Exception(failure.message)

    suspend fun scan(request: FilePasteRequest, limits: Limits = Limits()): FileTreeScan {
        val found=ArrayList<FileScanEntry>()
        val visited=HashSet<String>()
        var metadata=0L
        var known=0uL
        var unknown=0
        var at=""
        fun stop(kind: FileScanFailureKind, message: String): Nothing = throw Abort(FileScanFailure(kind,at,message))
        fun textValid(value: String): Boolean {
            var i=0
            while(i<value.length) {
                val c=value[i++]
                if(c=='\u0000' || Character.isLowSurrogate(c)) return false
                if(Character.isHighSurrogate(c) && (i>=value.length || !Character.isLowSurrogate(value[i++]))) return false
            }
            return true
        }
        fun entryValid(parent: FileLocation, entry: BrowserEntry) {
            if(entry.id.isEmpty() || !textValid(entry.id) || entry.name.isEmpty() || !textValid(entry.name) ||
                entry.name=="." || entry.name==".." || '/' in entry.name)
                stop(FileScanFailureKind.INVALID_ENTRY,"条目标识或名称不能安全复制")
            val expected=when {
                parent.endpoint==FileEndpoint.APP -> if(parent.directoryId.isEmpty()) entry.name else parent.directoryId+"/"+entry.name
                parent.endpoint.remote!=null -> parent.directoryId.trimEnd('/')+"/"+entry.name
                else -> null // SAF IDs are opaque, including separators and colons.
            }
            if(expected!=null && entry.id!=expected) stop(FileScanFailureKind.INVALID_ENTRY,"条目不属于来源文件夹")
        }
        suspend fun directory(location: FileLocation): List<BrowserEntry> {
            currentCoroutineContext().ensureActive()
            val result=browse(location)
            currentCoroutineContext().ensureActive()
            if(result.location!=location) stop(FileScanFailureKind.SOURCE_CHANGED,"来源文件夹身份已改变，请重新复制")
            val entries=result.entries.toList()
            if(entries.size>4096) stop(FileScanFailureKind.LIMIT,"文件夹超过 4096 项，扫描不完整")
            if(entries.map { it.id }.toSet().size!=entries.size || entries.map { it.name }.toSet().size!=entries.size)
                stop(FileScanFailureKind.INVALID_ENTRY,"来源含重复标识或同名条目，无法确定复制对象")
            yield()
            return entries
        }
        fun unchanged(expected: BrowserEntry, current: BrowserEntry?) {
            if(current==null || current.id!=expected.id || current.name!=expected.name || current.kind!=expected.kind ||
                (expected.size!=null && current.size!=expected.size) ||
                (expected.modifiedMillis!=null && current.modifiedMillis!=expected.modifiedMillis))
                stop(FileScanFailureKind.SOURCE_CHANGED,"来源已改变，请刷新后重新复制")
        }
        suspend fun visit(parent: FileLocation, entry: BrowserEntry, relative: String, depth: Int) {
            at=relative
            currentCoroutineContext().ensureActive()
            entryValid(parent,entry)
            if(depth>limits.maxDepth || found.size>=limits.maxEntries)
                stop(FileScanFailureKind.LIMIT,"扫描超过深度或条目上限，结果不完整")
            val strings=listOf(entry.id,entry.name,relative,parent.directoryId)
            if(strings.any { it.length>limits.maxMetadataBytes })
                stop(FileScanFailureKind.LIMIT,"扫描元数据超过上限，结果不完整")
            val cost=strings.sumOf { it.toByteArray(Charsets.UTF_8).size.toLong() }
            if(metadata+cost>limits.maxMetadataBytes) stop(FileScanFailureKind.LIMIT,"扫描元数据超过上限，结果不完整")
            val size=entry.size
            if(entry.kind==FileKind.FILE && size!=null && ULong.MAX_VALUE-known<size)
                stop(FileScanFailureKind.LIMIT,"文件总量超出可表示范围，结果不完整")
            metadata+=cost
            if(entry.kind==FileKind.FILE) { if(size==null) unknown++ else known+=size }
            val index=found.size
            found.add(FileScanEntry(parent,entry,relative))
            if(entry.kind==FileKind.DIRECTORY) {
                if(!visited.add(entry.id)) stop(FileScanFailureKind.REPEATED_DIRECTORY,"来源目录重复或形成循环，扫描不完整")
                val child=FileLocation(parent.endpoint,entry.id)
                val children=directory(child)
                found[index]=found[index].copy(childCount=children.size)
                for(item in children) visit(child,item,relative+"/"+item.name,depth+1)
                at=relative
                val after=directory(child)
                if(children.associateBy { it.id }!=after.associateBy { it.id })
                    stop(FileScanFailureKind.SOURCE_CHANGED,"扫描期间目录内容已改变，请重新扫描")
            }
            if(found.size%64==0) yield()
        }
        var failure: FileScanFailure?=null
        try {
            val finished=withTimeoutOrNull(limits.timeoutMillis) {
                val before=directory(request.source).associateBy { it.id }
                val selected=request.items.map { expected ->
                    at=expected.name
                    unchanged(expected,before[expected.id])
                    before.getValue(expected.id)
                }
                for(entry in selected) visit(request.source,entry,entry.name,1)
                at=""
                val after=directory(request.source).associateBy { it.id }
                for(entry in selected) { at=entry.name;unchanged(entry,after[entry.id]) }
                true
            }
            if(finished==null) failure=FileScanFailure(FileScanFailureKind.TIMEOUT,at,"扫描超时，结果不完整")
        } catch(cancelled: CancellationException) { throw cancelled }
        catch(abort: Abort) { failure=abort.failure }
        catch(error: Exception) { failure=FileScanFailure(FileScanFailureKind.READ,at,error.message ?: "来源读取失败，扫描不完整") }
        currentCoroutineContext().ensureActive()
        return FileTreeScan(request,found,failure,known,unknown)
    }
}
