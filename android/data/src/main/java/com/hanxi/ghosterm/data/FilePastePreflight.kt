package com.hanxi.ghosterm.data

import com.hanxi.ghosterm.domain.*
import kotlinx.coroutines.*

/** Read-only target/relationship checks for one frozen scan. Does not authenticate,
 * reserve a name, create directories, resolve conflicts or launch a transfer. */
class FilePastePreflight(
    private val inspect: suspend (FileLocation) -> FileTargetDirectory,
    private val relation: suspend (FileLocation, FileLocation) -> FileLocationRelation
) {
    private class Abort(val failure: FilePasteCheckFailure): Exception(failure.message)
    suspend fun check(scan: FileTreeScan, timeoutMillis: Long = 30000, maxMetadataBytes: Int = 4*1024*1024): FilePasteCheck {
        require(timeoutMillis in 1..120000)
        require(maxMetadataBytes in 1..16*1024*1024)
        var entries = emptyList<BrowserEntry>()
        var conflicts = emptyList<FilePasteConflict>()
        var capability = FileCreateCapability.UNKNOWN
        var failure: FilePasteCheckFailure? = null
        fun stop(kind: FilePasteCheckFailureKind, message: String): Nothing = throw Abort(FilePasteCheckFailure(kind,message))
        try {
            val done=withTimeoutOrNull(timeoutMillis) {
                currentCoroutineContext().ensureActive()
                if(!scan.complete) stop(FilePasteCheckFailureKind.INCOMPLETE_SOURCE,"来源扫描不完整，请重新扫描")
                val request=scan.request
                when(relation(request.source,request.destination)) {
                    FileLocationRelation.SAME -> stop(FilePasteCheckFailureKind.SAME_DIRECTORY,"来源和目标是同一文件夹")
                    FileLocationRelation.UNKNOWN -> stop(FilePasteCheckFailureKind.RELATION_UNKNOWN,"无法确认来源与目标的关系，尚不能开始复制")
                    else -> Unit // A descendant of the source parent may be a different sibling folder.
                }
                for(item in request.items.filter { it.kind==FileKind.DIRECTORY }) {
                    when(relation(FileLocation(request.source.endpoint,item.id),request.destination)) {
                        FileLocationRelation.SAME,FileLocationRelation.DESCENDANT ->
                            stop(FilePasteCheckFailureKind.INSIDE_SOURCE,"不能将目录复制到自身或其子目录")
                        FileLocationRelation.UNKNOWN -> stop(FilePasteCheckFailureKind.RELATION_UNKNOWN,"无法确认目录包含关系，尚不能开始复制")
                        FileLocationRelation.SEPARATE -> Unit
                    }
                    yield()
                }
                val target=inspect(request.destination)
                currentCoroutineContext().ensureActive()
                if(target.directory.location!=request.destination)
                    stop(FilePasteCheckFailureKind.TARGET_CHANGED,"目标文件夹身份已改变，请重新选择")
                capability=target.create
                if(target.create==FileCreateCapability.DENIED)
                    stop(FilePasteCheckFailureKind.READ_ONLY,"目标文件夹只读或不支持创建")
                val candidates=target.directory.entries.toList()
                if(candidates.size>4096 || candidates.map { it.id }.toSet().size!=candidates.size ||
                    candidates.map { it.name }.toSet().size!=candidates.size)
                    stop(FilePasteCheckFailureKind.TARGET_INVALID,"目标列表超限或包含重复条目，无法确认冲突")
                var metadata=0L
                for(item in candidates) {
                    if(item.id.length>maxMetadataBytes || item.name.length>maxMetadataBytes)
                        stop(FilePasteCheckFailureKind.TARGET_INVALID,"目标元数据超限，无法确认冲突")
                    if(item.id.isEmpty() || !validText(item.id) || !validName(item.name))
                        stop(FilePasteCheckFailureKind.TARGET_INVALID,"目标条目标识或名称无效")
                    metadata+=item.id.toByteArray(Charsets.UTF_8).size.toLong()+item.name.toByteArray(Charsets.UTF_8).size
                    if(metadata>maxMetadataBytes)
                        stop(FilePasteCheckFailureKind.TARGET_INVALID,"目标元数据超限，无法确认冲突")
                    val parent=request.destination
                    val expected=when {
                        parent.endpoint==FileEndpoint.APP -> if(parent.directoryId.isEmpty()) item.name else parent.directoryId+"/"+item.name
                        parent.endpoint.remote!=null -> parent.directoryId.trimEnd('/')+"/"+item.name
                        else -> null
                    }
                    if(expected!=null && item.id!=expected)
                        stop(FilePasteCheckFailureKind.TARGET_INVALID,"目标条目不属于目标文件夹")
                }
                entries=candidates
                val names=candidates.associateBy { it.name }
                conflicts=request.items.mapNotNull { source -> names[source.name]?.let { FilePasteConflict(source,it) } }
                true
            }
            if(done==null) failure=FilePasteCheckFailure(FilePasteCheckFailureKind.TIMEOUT,"目标检查超时，请重试")
        } catch(cancelled: CancellationException) { throw cancelled }
        catch(abort: Abort) { failure=abort.failure }
        catch(error: Exception) { failure=FilePasteCheckFailure(FilePasteCheckFailureKind.TARGET_READ,error.message ?: "无法检查目标文件夹") }
        currentCoroutineContext().ensureActive()
        return FilePasteCheck(scan,entries,conflicts,capability,failure)
    }
    private fun validName(name: String) = name.isNotEmpty() && name!="." && name!=".." && '/' !in name && validText(name)
    private fun validText(value: String): Boolean {
        var i=0
        while(i<value.length) {
            val c=value[i++]
            if(c=='\u0000' || Character.isLowSurrogate(c)) return false
            if(Character.isHighSurrogate(c) && (i>=value.length || !Character.isLowSurrogate(value[i++]))) return false
        }
        return true
    }
}
