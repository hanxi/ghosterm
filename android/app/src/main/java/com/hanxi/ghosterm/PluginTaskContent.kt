package com.hanxi.ghosterm

import androidx.compose.foundation.layout.*
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.semantics.*
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive

@OptIn(ExperimentalLayoutApi::class)
@Composable internal fun PluginTaskContent(model: PluginTaskModel) {
    val state by model.state.collectAsState()
    val title = state.descriptor?.title ?: "任务"
    fun statusSemantics(tag: String, message: String) = Modifier.testTag(tag).semantics {
        contentDescription = "$title，$message"
        liveRegion = LiveRegionMode.Polite
    }
    LaunchedEffect(model, model.available) {
        if (model.state.value.descriptor == null && model.state.value.issue == null) model.load()?.join()
        while (isActive && model.available) {
            delay(500)
            if (model.canAutoRefresh) model.load()?.join()
        }
    }
    Column(Modifier.fillMaxWidth().testTag("plugin-task"), verticalArrangement = Arrangement.spacedBy(8.dp)) {
        Text(title, Modifier.semantics { heading() }, style = MaterialTheme.typography.titleMedium)
        state.issue?.let { Text(it, statusSemantics("plugin-task-issue", it), color = MaterialTheme.colorScheme.error) }
        state.snapshot?.let { snapshot ->
            if (state.issue != null) Text("以下为上次读取的任务状态", style = MaterialTheme.typography.bodySmall)
            Text(snapshot.phase.label, statusSemantics("plugin-task-phase", snapshot.phase.label))
            SelectionContainer { Text(snapshot.message, Modifier.testTag("plugin-task-message")) }
            val total = snapshot.total
            val count = if (total == null) "${snapshot.completed} · 总量未知 · ${state.descriptor?.unit.orEmpty()}"
                else "${snapshot.completed} / $total · ${state.descriptor?.unit.orEmpty()}"
            SelectionContainer { Text(count, Modifier.testTag("plugin-task-count")) }
            if (total != null && total.toULong() > 0uL) {
                LinearProgressIndicator(progress = { (snapshot.completed.toULong().toDouble() / total.toULong().toDouble()).toFloat() },
                    modifier = Modifier.fillMaxWidth().semantics { contentDescription = "任务进度"; stateDescription = "${snapshot.completed} / $total" })
            }
            snapshot.result.text?.let { SelectionContainer { Text(it, Modifier.testTag("plugin-task-result")) } }
        }
        val cancellationMessage = when (state.cancellation) {
            PluginTaskModel.Cancellation.REQUESTED ->
                "取消请求已受理，仍需任务确认停止".takeUnless { state.snapshot?.phase?.terminal == true }
            PluginTaskModel.Cancellation.UNCONFIRMED -> "取消结果不明；不会重发取消请求"
            PluginTaskModel.Cancellation.ALREADY_FINISHED -> "取消到达时任务已结束"
            else -> null
        }
        cancellationMessage?.let {
            Text(it, statusSemantics("plugin-task-cancel-status", it))
        }
        if (state.busy) Text("正在读取任务…", Modifier.testTag("plugin-task-loading"))
        FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            TextButton(onClick = { model.load() }, enabled = model.available && !state.busy, modifier = Modifier.testTag("plugin-task-read")) { Text("读取任务状态") }
            TextButton(onClick = { model.cancel() }, enabled = model.canCancel, modifier = Modifier.testTag("plugin-task-cancel")) { Text("取消任务") }
        }
    }
}
