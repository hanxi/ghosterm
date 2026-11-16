package com.hanxi.ghosterm

import androidx.compose.foundation.layout.*
import androidx.compose.material3.*
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.unit.dp

@Composable
internal fun PluginUnclassifiedContent(model: PluginUnclassifiedModel, canAct: Boolean, inspect: (PluginUnclassifiedDirectories.Reference) -> Unit) {
    Text("归属未知的目录与条目", Modifier.testTag("plugin-unclassified-heading"), style = MaterialTheme.typography.titleMedium)
    Text("列出不代表安装来源或可信包。只能核对当前完整目录，再单独确认删除；不发布、授权或运行内容。")
    if (model.busy) LinearProgressIndicator(Modifier.fillMaxWidth())
    model.issue?.let { Text(it, Modifier.testTag("plugin-unclassified-issue"), color = MaterialTheme.colorScheme.error) }
    model.entries?.let { entries ->
        Text("未知条目 ${entries.size} 项", Modifier.testTag("plugin-unclassified-count"))
        entries.forEachIndexed { index, entry ->
            Text(entry.name, Modifier.testTag("plugin-unclassified-name-$index"))
            entry.issue?.let { Text(it) }
            entry.reference?.let { reference ->
                OutlinedButton(onClick = { inspect(reference) }, enabled = canAct && !model.busy,
                    modifier = Modifier.fillMaxWidth().heightIn(min = 48.dp).testTag("plugin-unclassified-preview-$index")) { Text("核对当前完整未知目录…") }
            }
        }
    }
}
