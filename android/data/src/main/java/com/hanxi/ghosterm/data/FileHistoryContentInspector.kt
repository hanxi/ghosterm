package com.hanxi.ghosterm.data

import com.hanxi.ghosterm.domain.*
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.ensureActive

/** Explicit single-file observation. The caller owns fresh authentication and bounded IO. */
class FileHistoryContentInspector(private val browse: suspend (FileLocation) -> BrowserDirectory,
    private val read: suspend (FileLocation,BrowserEntry) -> FileContentDigest) {
    suspend fun inspect(record: FileJournalRecord, item: FileJournalItem, candidate: Boolean = false): FileHistoryContentResult {
        require(item in record.items && record.items.count { it.value.source.relativePath==item.value.source.relativePath }==1)
        val key=FileHistoryContentKey(record.intent.id,item.value.source.relativePath,candidate)
        val expected=item.value.sourceDigest
        fun result(state:FileHistoryContentState,actual:FileContentDigest?=null,error:String?=null)=
            FileHistoryContentResult(key,state,expected,actual,error=error)
        if(expected==null) return result(FileHistoryContentState.NO_REFERENCE,error="这条历史记录没有来源内容摘要，无法比较")
        return try {
            currentCoroutineContext().ensureActive()
            val observation=FileHistoryInspector(browse).inspect(record.copy(items=listOf(item))).observations.single { it.candidate==candidate }
            when(observation.presence) {
                FileHistoryPresence.ABSENT -> result(FileHistoryContentState.MISSING,error="本次读取未找到该位置")
                FileHistoryPresence.UNKNOWN -> result(FileHistoryContentState.UNKNOWN,error=observation.error ?: "位置无法核对")
                FileHistoryPresence.PRESENT -> {
                    val entry=checkNotNull(observation.entry);val parent=checkNotNull(item.value.targetParent)
                    check(entry.kind==FileKind.FILE) { "该位置不是普通文件，不会跟随符号链接" }
                    val actual=read(parent,entry)
                    currentCoroutineContext().ensureActive()
                    val after=browse(parent)
                    check(after.location==parent && after.entries.singleOrNull { it.id==entry.id }==entry) { "核验期间文件信息已改变，请重新核验" }
                    result(if(actual==expected) FileHistoryContentState.MATCH else FileHistoryContentState.DIFFERENT,actual)
                }
            }
        } catch(cancel:CancellationException) { throw cancel }
        catch(error:Exception) { result(FileHistoryContentState.UNKNOWN,error=error.message ?: "无法读取文件内容") }
    }
}
