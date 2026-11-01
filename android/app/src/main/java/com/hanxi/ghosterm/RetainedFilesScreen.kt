package com.hanxi.ghosterm

import androidx.activity.compose.BackHandler
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.unit.dp
import java.text.DateFormat
import java.util.Date

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun RetainedFilesScreen(model: FileTasksViewModel) {
    BackHandler { if((model.retainedReview!=null || model.emptyRetainedReview!=null) && !model.retainedLoading)model.dismissRetained() else model.hideRetained() }
    Scaffold(topBar={ TopAppBar(title={ Text("保留的本机暂存") },navigationIcon={
        TextButton(onClick=model::hideRetained) { Text("返回任务") }
    },actions={ TextButton(onClick=model::refreshRetained,enabled=!model.busy && !model.retainedLoading && model.retainedReview==null && model.emptyRetainedReview==null,
        modifier=Modifier.testTag("retained-refresh")) { Text("刷新") } }) }) { padding ->
        LazyColumn(Modifier.fillMaxSize().padding(padding).testTag("retained-list"),contentPadding=PaddingValues(16.dp),verticalArrangement=Arrangement.spacedBy(12.dp)) {
            item { Text("仅列出本应用私有目录里的当前本机暂存。记录引用不证明文件属于原任务；远端和设备文件夹中的未确认结果请到原位置核对。") }
            model.retainedReleaseError?.let { item { Text(it,color=MaterialTheme.colorScheme.error) } }
            model.retainedNotice?.let { item { Text(it,modifier=Modifier.testTag("retained-notice")) } }
            model.retainedError?.let { item { Text(it,color=MaterialTheme.colorScheme.error,modifier=Modifier.testTag("retained-error")) } }
            if(model.retainedLoading) item { LinearProgressIndicator(Modifier.fillMaxWidth());Text("正在核对暂存…") }
            val snapshot=model.retainedSnapshot
            snapshot?.error?.let { item { Text("列表未完整读取：$it",color=MaterialTheme.colorScheme.error,modifier=Modifier.testTag("retained-incomplete")) } }
            if(snapshot!=null) item { Text("本次读取 ${snapshot.items.size} 项",Modifier.testTag("retained-count")) }
            if((snapshot?.emptyRequestCount ?: 0)>0) item {
                TextButton(onClick=model::requestEmptyRetained,enabled=!model.busy && !model.retainedLoading && model.retainedReview==null && model.emptyRetainedReview==null,
                    modifier=Modifier.testTag("retained-empty-directories")) { Text("整理已核对的空目录（${snapshot!!.emptyRequestCount}）") }
            }
            if(snapshot?.complete==true && snapshot.items.isEmpty()) item { Text("没有保留的本机暂存",Modifier.testTag("retained-empty")) }
            items(snapshot?.items.orEmpty(),key={ it.referencePath }) { item ->
                ElevatedCard(Modifier.fillMaxWidth().testTag("retained-item").clickable(enabled=!model.busy && !model.retainedLoading && model.emptyRetainedReview==null) { model.openRetained(item) }) {
                    Column(Modifier.padding(16.dp),verticalArrangement=Arrangement.spacedBy(6.dp)) {
                        Text(model.retainedLabel(item),style=MaterialTheme.typography.titleMedium)
                        Text("${item.size} 字节 · ${DateFormat.getDateTimeInstance().format(Date(item.modifiedMillis))}")
                        Text("点按查看当前位置和只读预览")
                    }
                }
            }
        }
    }
    model.emptyRetainedReview?.let { review ->
        AlertDialog(onDismissRequest={ if(!model.retainedLoading)model.dismissRetained() },title={ Text("整理空暂存目录？") },text={
            Text("重新核对并整理本次列出的 ${review.count} 个空暂存目录，释放后续复制的核对名额。仅移除仍为空的目录；文件、来源、目标和原任务记录保持不变。变化或无法确认的条目会保留并报告。")
        },confirmButton={ TextButton(onClick=model::clearEmptyRetained,enabled=!model.busy && !model.retainedLoading,
            modifier=Modifier.testTag("retained-empty-confirm")) { Text(if(model.retainedLoading) "正在整理…" else "整理空目录") } },
            dismissButton={ TextButton(onClick=model::dismissRetained,enabled=!model.retainedLoading,modifier=Modifier.testTag("retained-empty-cancel")) { Text("保留目录") } })
    }
    model.retainedReview?.let { review ->
        val preview=review.preview
        var confirming by remember(review.item.ref) { mutableStateOf(false) }
        if(confirming) AlertDialog(onDismissRequest={ if(!model.retainedLoading)confirming=false },title={ Text("清除此暂存文件？") },text={
            Text("清理当前核对的 ${review.item.size} 字节暂存文件，无法恢复。不会改变目标文件或原任务结果；无法据记录确认它属于原任务。")
        },confirmButton={ TextButton(onClick=model::clearRetained,enabled=!model.busy && !model.retainedLoading,modifier=Modifier.testTag("retained-clear-confirm")) { Text(if(model.retainedLoading) "正在清理…" else "清除暂存") } },
            dismissButton={ TextButton(onClick={ confirming=false },enabled=!model.retainedLoading) { Text("保留文件") } })
        else AlertDialog(onDismissRequest=model::dismissRetained,title={ Text("核对当前暂存") },text={
            LazyColumn(Modifier.fillMaxWidth().heightIn(max=380.dp),verticalArrangement=Arrangement.spacedBy(8.dp)) {
                item { Text(model.retainedLabel(review.item));Text("${review.item.size} 字节") }
                item { Text("暂存标识：${review.item.referencePath.substringAfterLast('/').take(8)}") }
                item { Text("仅预览当前打开的文件；目标与原任务记录保持不变。") }
                if(preview==null) item { Text("无法作为文本预览，仅显示文件信息。") }
                else item { Text(preview.ifEmpty { "空文件" },modifier=Modifier.testTag("retained-preview")) }
                if(review.truncated) item { Text("仅预览开头部分内容。") }
            }
        },confirmButton={ TextButton(onClick={ confirming=true },enabled=!model.busy && !model.retainedLoading,modifier=Modifier.testTag("retained-clear-request")) { Text("清理此文件…") } },
            dismissButton={ TextButton(onClick=model::dismissRetained) { Text("返回列表") } })
    }
}
