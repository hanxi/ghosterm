package com.hanxi.ghosterm

import androidx.compose.foundation.layout.*
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.withContext

@Composable
internal fun PluginInstalledVersionsContent(controller: ExtensionController?) {
    var expanded by remember(controller) { mutableStateOf(false) }
    TextButton(onClick = { expanded = !expanded }, modifier = Modifier.testTag("plugin-versions-disclosure")) {
        Text(if (expanded) "收起本机版本" else "查看已保存的本机版本…")
    }
    if (expanded && controller != null) PluginInstalledVersionsList(controller)
}

@Composable
private fun PluginInstalledVersionsList(controller: ExtensionController) {
    val scope = rememberCoroutineScope()
    val model = remember(controller) { PluginInstalledVersionsModel(scope) {
        val worker = controller.newReleaseInspector()
        try { worker.installedVersions() } finally { withContext(NonCancellable) { worker.close() } }
    } }
    DisposableEffect(model) { onDispose { model.close() } }
    LaunchedEffect(model) { model.refresh() }
    Column(Modifier.fillMaxWidth().padding(horizontal = 16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
        OutlinedButton(onClick = model::refresh, enabled = !model.busy,
            modifier = Modifier.fillMaxWidth().heightIn(min = 48.dp).testTag("plugin-versions-refresh")) { Text("刷新版本列表") }
        if (model.busy) LinearProgressIndicator(Modifier.fillMaxWidth())
        model.issue?.let { Text(it, Modifier.testTag("plugin-versions-issue"), color = MaterialTheme.colorScheme.error) }
        model.observation?.let { value ->
            Text("已保存版本 ${value.versions.size} 项", Modifier.testTag("plugin-versions-count"), style = MaterialTheme.typography.titleMedium)
            Text("仅显示已保存的声明，尚未用当前可信策略核验；不授权或执行插件。", Modifier.testTag("plugin-versions-unverified"), style = MaterialTheme.typography.bodySmall)
            value.selectionIssue?.let { Text(it, Modifier.testTag("plugin-versions-selection-unknown"), color = MaterialTheme.colorScheme.error) }
            val selected = value.selection
            if (selected != null && selected.choices.isEmpty()) Text("当前没有活动选择记录。", Modifier.testTag("plugin-versions-no-selection"))
            selected?.choices?.toSortedMap()?.forEach { (id, choice) ->
                Text("$id\n活动选择 ${choice.releaseID}\n${if (choice.enabled) "记录为启用" else "记录为禁用"}",
                    Modifier.testTag("plugin-versions-choice-$id"), style = MaterialTheme.typography.bodySmall)
            }
            value.versions.forEach { version ->
                SelectionContainer { Column(Modifier.fillMaxWidth().testTag("plugin-version-${version.releaseID}"), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                    Text("${version.pluginID} ${version.version}", style = MaterialTheme.typography.titleSmall)
                    Text("声明发布者 ${version.publisher}")
                    Text("发布描述 SHA-256：${version.releaseID}\n目录包 SHA-256：${version.packageSHA256}", fontFamily = FontFamily.Monospace, style = MaterialTheme.typography.bodySmall)
                    Text(when {
                        selected == null -> "活动选择状态查不出来"
                        selected.choices[version.pluginID]?.releaseID != version.releaseID -> "未选择此版本"
                        selected.choices.getValue(version.pluginID).enabled -> "活动记录选择此版本（启用）"
                        else -> "活动记录选择此版本（禁用）"
                    })
                } }
            }
            value.issues.forEach { Text(it, Modifier.testTag("plugin-versions-entry-issue"), color = MaterialTheme.colorScheme.error) }
            if (value.maintenanceCount > 0) Text("安装暂存和未识别对象 ${value.maintenanceCount} 项。", Modifier.testTag("plugin-versions-maintenance"))
        }
    }
}
