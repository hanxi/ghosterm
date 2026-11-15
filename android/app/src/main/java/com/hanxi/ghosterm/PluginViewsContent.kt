package com.hanxi.ghosterm

import androidx.compose.foundation.layout.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.dp

/** Standard components keyed by Native handle; a widget close is not a task cancel. */
@Composable internal fun PluginViewsContent(model: PluginViewsModel) {
    val state by model.state.collectAsState()
    Column(Modifier.fillMaxWidth().testTag("plugin-views"), verticalArrangement = Arrangement.spacedBy(16.dp)) {
        state.issue?.let { Text(it, Modifier.testTag("plugin-views-issue"), color = MaterialTheme.colorScheme.error) }
        if (state.items.isEmpty() && model.available) Text("当前没有打开的插件视图", Modifier.testTag("plugin-views-empty"))
        state.items.forEach { item -> key(item.handle) {
            OutlinedCard(Modifier.fillMaxWidth().testTag("plugin-registered-view-${item.handle}")) {
                Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                        Text(item.registration.viewID, Modifier.weight(1f), style = MaterialTheme.typography.labelMedium)
                        TextButton(onClick = { model.closeView(item.handle) }, enabled = model.available && !item.closing,
                            modifier = Modifier.testTag("plugin-view-close").semantics { contentDescription = "关闭视图 ${item.registration.viewID}，不取消任务" }) {
                            Text(if (item.closing) "正在关闭视图…" else "关闭视图")
                        }
                    }
                    item.closeIssue?.let { Text(it, Modifier.testTag("plugin-view-close-issue"), color = MaterialTheme.colorScheme.error) }
                    when (val c = item.content) {
                        is PluginViewItem.Content.Table -> PluginTableContent(c.model)
                        is PluginViewItem.Content.Diff -> PluginDiffContent(c.model)
                        is PluginViewItem.Content.Task -> PluginTaskContent(c.model)
                    }
                }
            }
        } }
    }
}
