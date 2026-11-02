package com.hanxi.ghosterm

import androidx.compose.foundation.layout.*
import androidx.compose.material3.*
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.hanxi.ghosterm.domain.FileCopyState

/** Observe application-owned work; opening the list never starts or confirms IO. */
@Composable
internal fun FileTaskDock(model: FileTasksViewModel) {
    val task = model.tasks.firstOrNull { it.active && it.phase != FileTaskPhase.QUEUED }
        ?: model.tasks.lastOrNull { it.active } ?: model.tasks.firstOrNull()
    val active = model.tasks.count { it.active }
    val error = model.journalError ?: task?.persistenceError ?: task?.error
    val uncertain = task?.result?.state in setOf(FileCopyState.FAILED, FileCopyState.UNKNOWN)
    Card(onClick = model::show, modifier = Modifier.fillMaxWidth().padding(horizontal = 12.dp, vertical = 6.dp)
        .testTag("file-task-dock")) {
        Column(Modifier.fillMaxWidth().heightIn(min = 64.dp).padding(12.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
            Text("文件任务", style = MaterialTheme.typography.labelLarge)
            Text(error ?: task?.let(::taskStatus) ?: when {
                model.journalLoading -> "正在读取任务记录"
                model.history.isNotEmpty() -> "查看 ${model.history.size} 条历史记录"
                else -> "暂无任务 · 查看历史记录"
            }, style = MaterialTheme.typography.bodyMedium,
                color = if(error != null || uncertain) MaterialTheme.colorScheme.error else MaterialTheme.colorScheme.onSurface,
                maxLines = 2, overflow = TextOverflow.Ellipsis, modifier = Modifier.testTag("file-task-dock-status"))
            if(task != null) {
                Text("${task.request.source.endpoint.label} / ${task.request.sourcePathLabel} → ${task.request.destination.endpoint.label} / ${task.request.destinationPathLabel}",
                    style = MaterialTheme.typography.bodySmall, maxLines = 1, overflow = TextOverflow.Ellipsis)
                if(active > 1) Text("另有 ${active - 1} 个待处理任务", style = MaterialTheme.typography.bodySmall)
            }
        }
    }
}
