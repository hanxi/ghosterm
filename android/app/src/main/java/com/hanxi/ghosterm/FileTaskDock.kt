package com.hanxi.ghosterm

import androidx.compose.foundation.layout.*
import androidx.compose.material3.*
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.hanxi.ghosterm.domain.FileCopyState

internal val FileTasksViewModel.hasDockSummary: Boolean
    get()=tasks.isNotEmpty() || journalLoading || journalError!=null

/** Observe application-owned work; opening the list never starts or confirms IO. */
@Composable
internal fun FileTaskDock(model: FileTasksViewModel) {
    if(!model.hasDockSummary) return
    val task = model.tasks.firstOrNull { it.active && it.phase != FileTaskPhase.QUEUED }
        ?: model.tasks.lastOrNull { it.active } ?: model.tasks.firstOrNull()
    val active = model.tasks.count { it.active }
    val error = model.journalError ?: task?.persistenceError ?: task?.error
    val uncertain = task?.result?.state in setOf(FileCopyState.FAILED, FileCopyState.UNKNOWN)
    val compact = LocalConfiguration.current.screenHeightDp < 400
    val status = error ?: task?.let(::taskStatus) ?: when {
        model.journalLoading -> "正在读取任务记录"
        model.history.isNotEmpty() -> "查看 ${model.history.size} 条历史记录"
        else -> "暂无任务 · 查看历史记录"
    }
    val statusColor = if(error != null || uncertain) MaterialTheme.colorScheme.error else MaterialTheme.colorScheme.onSurface
    Card(onClick = model::show, modifier = Modifier.fillMaxWidth().padding(horizontal = 12.dp, vertical = if(compact) 2.dp else 6.dp)
        .testTag("file-task-dock"),colors=CardDefaults.cardColors(containerColor=MaterialTheme.colorScheme.surfaceContainerHigh),
        shape=MaterialTheme.shapes.small) {
        if (compact) Row(Modifier.fillMaxWidth().height(48.dp).padding(horizontal = 12.dp),
            verticalAlignment = androidx.compose.ui.Alignment.CenterVertically) {
            Text("文件任务 · $status", style = MaterialTheme.typography.bodyMedium,
                color = statusColor, maxLines = 1, overflow = TextOverflow.Ellipsis,
                modifier = Modifier.testTag("file-task-dock-status"))
        } else Row(Modifier.fillMaxWidth().heightIn(min = 64.dp).padding(12.dp),verticalAlignment=androidx.compose.ui.Alignment.CenterVertically,
            horizontalArrangement=Arrangement.spacedBy(12.dp)) {
            GhostermIcon(UiIcon.Copy,tint=MaterialTheme.colorScheme.primary)
            Column(Modifier.weight(1f),verticalArrangement = Arrangement.spacedBy(4.dp)) {
            Text("文件任务", style = MaterialTheme.typography.labelLarge)
            Text(status, style = MaterialTheme.typography.bodyMedium,
                color = statusColor, maxLines = 2, overflow = TextOverflow.Ellipsis,
                modifier = Modifier.testTag("file-task-dock-status"))
            if(task != null) {
                Text("${task.request.source.endpoint.label} / ${task.request.sourcePathLabel} → ${task.request.destination.endpoint.label} / ${task.request.destinationPathLabel}",
                    style = MaterialTheme.typography.bodySmall, maxLines = 1, overflow = TextOverflow.Ellipsis)
                if(active > 1) Text("另有 ${active - 1} 个待处理任务", style = MaterialTheme.typography.bodySmall)
            }
            }
            GhostermIcon(UiIcon.Arrow,tint=MaterialTheme.colorScheme.onSurfaceVariant)
        }
    }
}
