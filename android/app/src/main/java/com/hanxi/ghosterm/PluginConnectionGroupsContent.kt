package com.hanxi.ghosterm

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.dp
import org.json.JSONArray

private data class PluginCloseRequest(val group: ExtensionConnectionModel,
    val instance: ExtensionConnectionModel.Instance? = null)

@Composable internal fun PluginConnectionGroupsContent(model: ExtensionPackagesViewModel,
    connectionsBusy: Boolean, onLaunchGroup: () -> Unit) {
    // The pending choice belongs to this view lifetime, never a saved approval.
    var closeRequest by remember(model) { mutableStateOf<PluginCloseRequest?>(null) }
    val selected = closeRequest?.takeIf { request ->
        model.connectionGroups.any { it === request.group } && !request.group.closed &&
            (request.instance == null || request.group.instances.any { it === request.instance && !it.closed })
    }
    if (selected != null) AlertDialog(
        onDismissRequest = { closeRequest = null },
        title = { Text(if (selected.instance == null) "停止并关闭插件连接？" else "停止并关闭插件实例？") },
        text = { Text(if (selected.instance == null)
            "将停止此连接的全部插件实例并释放资源。已经发生的操作不会撤销，未确认的结果仍按未知保留。"
            else "将停止「${selected.instance.title}」并释放其资源。其它实例继续运行。已经发生的操作不会撤销，未确认的结果仍按未知保留。") },
        confirmButton = { TextButton(onClick = {
            closeRequest = null
            if (model.connectionGroups.any { it === selected.group } && !selected.group.closed) {
                val instance = selected.instance
                if (instance == null) selected.group.requestClose()
                else if (selected.group.instances.any { it === instance && !it.closed }) selected.group.requestCloseInstance(instance)
            }
        }, modifier = Modifier.testTag("plugin-close-confirm")) { Text("停止并关闭") } },
        dismissButton = { TextButton(onClick = { closeRequest = null },
            modifier = Modifier.testTag("plugin-close-cancel")) { Text("取消") } })
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
                    request.releaseEvidence?.let { Text(it.reviewDescription) }
                    Text("Executor variant：${request.variantID ?: "未固定，由执行端核对唯一候选"}")
                    launch.clientWorkspace?.let { Text(it.reviewDescription) }
                    request.processPrograms.forEach { program ->
                        Text("业务进程：${program.name} · ${program.executable} · ${program.sha256}\n固定参数：${JSONArray(program.prefixArguments)}")
                    }
                    Text("选定环境名称：${request.processEnvironmentNames.joinToString()}")
                } }
                model.connectionGroups.filter { it.canAdd && it.acceptsConfiguration(invocation) }.forEach { group ->
                    TextButton(onClick = { model.addToConnection(group, invocation) },
                        enabled = !connectionsBusy && !model.groupSubmissionBusy,
                        modifier = Modifier.testTag("plugin-add-${group.id}-${invocation.id}")) {
                        Text("添加到现有连接：${group.title} · ${group.targetDescription}")
                    }
                }
                TextButton(onClick = { model.removePendingGroupItem(invocation) },
                    enabled = !connectionsBusy && !model.groupSubmissionBusy) { Text("移除此项") }
            }
        }
        TextButton(onClick = onLaunchGroup,
            enabled = !connectionsBusy && !model.groupSubmissionBusy && model.connectionGroups.none { it.busy },
            modifier = Modifier.testTag("plugin-group-launch")) { Text("连接并启动整组") }
        if (model.groupSubmissionBusy) LinearProgressIndicator(Modifier.fillMaxWidth())
    }
    if (model.connectionGroups.isNotEmpty()) {
        TextButton(onClick = { model.selectGroupInstance(null) }, Modifier.testTag("plugin-instance-default")) { Text("查看原单实例入口") }
    }
    model.connectionGroups.forEach { group ->
        Column(Modifier.fillMaxWidth().padding(vertical = 8.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
            Text("连接组：${group.title.ifEmpty { "正在准备" }} · ${group.state?.info?.instances ?: group.instances.size} 实例")
            Text("此连接实际目标：${group.targetDescription}", style = MaterialTheme.typography.bodySmall)
            if (!group.closed) Text("关闭全部实例后连接仍保持，可从待启动项继续添加。更换目标请新建连接。", style = MaterialTheme.typography.bodySmall)
            group.issue?.let { Text(it, color = MaterialTheme.colorScheme.error) }
            if (group.cleanupPending) Text("连接或资源仍待释放，请重试关闭。", color = MaterialTheme.colorScheme.error)
            group.instances.forEachIndexed { index, instance ->
                Row {
                    TextButton(onClick = { model.selectGroupInstance(instance.model) },
                        enabled = !group.busy || model.selectedGroupInstance === instance.model,
                        modifier = Modifier.weight(1f).testTag("plugin-instance-${instance.id}").semantics {
                            contentDescription = "${if (model.selectedGroupInstance === instance.model) "当前" else "选择"}实例 ${index + 1}，${instance.title}"
                        }) {
                        val phase = instance.model.activeRuntime?.info?.phase?.toString()
                            ?: instance.model.activeSession?.info?.phase?.toString() ?: "准备中"
                        Text("${if (model.selectedGroupInstance === instance.model) "当前 · " else ""}${index + 1}. ${instance.title} · ${if (instance.closed) "已关闭" else phase}")
                    }
                    TextButton(onClick = {
                        if (instance.closeNeedsConfirmation) closeRequest = PluginCloseRequest(group, instance)
                        else group.requestCloseInstance(instance)
                    }, enabled = !instance.closed && !group.busy,
                        modifier = Modifier.testTag("plugin-instance-close-${instance.id}").semantics {
                            contentDescription = "关闭实例 ${index + 1}，${instance.title}"
                        }) { Text("关闭") }
                }
            }
            TextButton(onClick = {
                if (group.closeNeedsConfirmation) closeRequest = PluginCloseRequest(group)
                else group.requestClose()
            }, enabled = !group.closed,
                modifier = Modifier.testTag("plugin-group-close-${group.id}")) { Text(if (group.closed) "已关闭连接" else "停止并关闭连接") }
        }
    }
}
