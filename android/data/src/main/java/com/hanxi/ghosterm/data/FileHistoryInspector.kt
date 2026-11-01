package com.hanxi.ghosterm.data

import com.hanxi.ghosterm.domain.*
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.ensureActive

/** Read-only, explicit inspection. No executable request, content transfer, mutation,
 * result promotion or cleanup authority is derived from a journal record. */
class FileHistoryInspector(private val browse: suspend (FileLocation) -> BrowserDirectory) {
    companion object { const val DIRECTORY_LIMIT = 256; const val ENTRY_LIMIT = 4096 }
    suspend fun inspect(record: FileJournalRecord): FileHistoryInspection {
        require(record.items.size<=16384) { "核对项目超过上限" }
        var entriesRead=0
        val directories=mutableMapOf<FileLocation,Result<BrowserDirectory>>()
        suspend fun observe(item: FileCopyItemResult, candidate: Boolean): FileHistoryObservation {
            val location=if(candidate) item.candidatePath else item.documentUri ?: item.createdId ?: item.targetName
            fun unknown(message: String)=FileHistoryObservation(item.source.relativePath,candidate,FileHistoryPresence.UNKNOWN,location,error=message)
            return try {
                currentCoroutineContext().ensureActive()
                val parent=item.targetParent ?: return unknown("没有记录目标目录，无法核对")
                require(parent.endpoint==record.intent.destination.endpoint) { "记录的目标端点不一致" }
                val name: String?
                val id: String?
                if(candidate) {
                    require(parent.endpoint.treeUri==null) { "文档目标没有可核对的暂存路径" }
                    val path=checkNotNull(item.candidatePath)
                    val prefix=parent.directoryId.trimEnd('/').let { if(it.isEmpty() && parent.endpoint.remote==null) "" else "$it/" }
                    require(path.startsWith(prefix)) { "暂存位置不属于已记录的目标目录" }
                    name=path.removePrefix(prefix)
                    require(name.isNotEmpty() && name!="." && name!=".." && '/' !in name && '\u0000' !in name) { "暂存位置无效" }
                    id=if(parent.endpoint.remote!=null) path else null
                } else {
                    id=item.createdId
                    name=if(id==null) item.targetName else null
                    if(parent.endpoint.treeUri!=null && id==null) return unknown("没有记录文档标识；不能按显示名称判断创建结果")
                    if(id==null && name==null) return unknown("没有记录目标位置，无法核对")
                    require(name==null || (name.isNotEmpty() && name!="." && name!=".." && '/' !in name && '\u0000' !in name)) { "目标名称无效" }
                }
                val found=directories[parent] ?: run {
                    check(directories.size<DIRECTORY_LIMIT) { "本次核对最多读取 $DIRECTORY_LIMIT 个目录，请分批在文件页查看" }
                    val result=try {
                        check(entriesRead<16384) { "本次核对的条目总数已达上限" }
                        val directory=browse(parent)
                        require(directory.location==parent) { "目录身份已改变" }
                        require(directory.entries.size<=ENTRY_LIMIT) { "目录条目超过核对上限" }
                        check(entriesRead+directory.entries.size<=16384) { "本次核对的条目总数超过上限" }
                        entriesRead+=directory.entries.size
                        Result.success(directory)
                    } catch(cancel: CancellationException) { throw cancel }
                    catch(error: Exception) { Result.failure(error) }
                    directories[parent]=result;result
                }
                val matches=found.getOrThrow().entries.filter { if(id!=null) it.id==id else it.name==name }
                require(matches.size<=1) { "位置标识或名称不唯一，无法核对" }
                val entry=matches.singleOrNull()
                FileHistoryObservation(item.source.relativePath,candidate,
                    if(entry==null) FileHistoryPresence.ABSENT else FileHistoryPresence.PRESENT,location,entry)
            } catch(cancel: CancellationException) { throw cancel }
            catch(error: Exception) { unknown(error.message ?: "无法读取当前位置") }
        }
        val observations=ArrayList<FileHistoryObservation>()
        for(item in record.items) {
            observations+=observe(item.value,false)
            if(item.value.candidatePath!=null) observations+=observe(item.value,true)
        }
        currentCoroutineContext().ensureActive()
        return FileHistoryInspection(record.intent.id,System.currentTimeMillis(),observations.toList())
    }
}
