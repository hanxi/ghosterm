package com.hanxi.ghosterm

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.unit.dp
import org.json.JSONArray

@Composable internal fun PluginConnectionGroupsContent(model: ExtensionPackagesViewModel,
    connectionsBusy: Boolean, onLaunchGroup: (String) -> Unit) {
    if (model.pendingGroup.isNotEmpty()) Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
        Text("待启动连接组（${model.pendingGroup.size}/8）", style = MaterialTheme.typography.titleSmall)
        model.pendingGroup.toList().forEach { invocation ->
            val launch = invocation.launch; val request = launch.request
            Column(Modifier.fillMaxWidth().padding(8.dp)) {
                SelectionContainer { Column {
                    Text("${invocation.alias} · ${launch.entry.review.id} · ${launch.command.path}")
                    Text("gterm：${request.executable}\nExecutor：${request.executorDirectory}\n临时根目录：${invocation.temporaryDirectory}")
                    Text("Client：${request.clientDigest}\nExecutor：${request.executorDigest}")
                    Text("参数：${JSONArray(request.arguments)}\n只读根：${request.workspaceRootDirectory ?: "无"}")
                    request.execution?.let { Text(it.reviewDescription) }
                    launch.clientWorkspace?.let { Text(it.reviewDescription) }
                    request.processPrograms.forEach { program ->
                        Text("业务进程：${program.name} · ${program.executable} · ${program.sha256}\n固定参数：${JSONArray(program.prefixArguments)}")
                    }
                    Text("选定环境名称：${request.processEnvironmentNames.joinToString()}")
                } }
                TextButton(onClick = { model.pendingGroup.remove(invocation) }) { Text("移除此项") }
            }
        }
        TextButton(onClick = { onLaunchGroup(model.pendingGroup.first().alias) },
            enabled = !connectionsBusy && !model.groupLaunchBusy && model.connectionGroups.none { it.busy },
            modifier = Modifier.testTag("plugin-group-launch")) { Text("连接并启动整组") }
    }
    if (model.connectionGroups.isNotEmpty()) {
        TextButton(onClick = { model.selectGroupInstance(null) }, Modifier.testTag("plugin-instance-default")) { Text("查看原单实例入口") }
    }
    model.connectionGroups.forEach { group ->
        Column(Modifier.fillMaxWidth().padding(vertical = 8.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
            Text("连接组：${group.title.ifEmpty { "正在准备" }} · ${group.state?.info?.instances ?: group.instances.size} 实例")
            group.issue?.let { Text(it, color = MaterialTheme.colorScheme.error) }
            if (group.cleanupPending) Text("连接或资源仍待释放，请重试关闭。", color = MaterialTheme.colorScheme.error)
            group.instances.forEachIndexed { index, instance ->
                Row {
                    TextButton(onClick = { model.selectGroupInstance(instance.model) },
                        modifier = Modifier.weight(1f).testTag("plugin-instance-${instance.id}")) {
                        val phase = instance.model.activeRuntime?.info?.phase?.toString()
                            ?: instance.model.activeSession?.info?.phase?.toString() ?: "准备中"
                        Text("${if (model.selectedGroupInstance === instance.model) "当前 · " else ""}${index + 1}. ${instance.title} · ${if (instance.closed) "已关闭" else phase}")
                    }
                    TextButton(onClick = { group.requestCloseInstance(instance) }, enabled = !instance.closed && !group.busy,
                        modifier = Modifier.testTag("plugin-instance-close-${instance.id}")) { Text("关闭") }
                }
            }
            TextButton(onClick = group::requestClose, enabled = !group.closed,
                modifier = Modifier.testTag("plugin-group-close-${group.id}")) { Text(if (group.closed) "已关闭连接" else "停止并关闭连接") }
        }
    }
}
