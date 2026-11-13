package com.hanxi.ghosterm

import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp

@Composable internal fun CodexChangeSnapshotControl(extensions: ExtensionPackagesViewModel, reference: CodexChangeReference) {
    val current by extensions.changeReview.state.collectAsState()
    val state = current?.takeIf { it.reference == reference }
    var expanded by remember(reference) { mutableStateOf(false) }
    Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
        if (state?.loading == true) {
            Text("读取完整补丁：${state.receivedBytes} / ${reference.totalBytes} 字节", style = MaterialTheme.typography.bodySmall)
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                CircularProgressIndicator(Modifier.size(20.dp))
                TextButton(onClick = { extensions.changeReview.cancel() }, modifier = Modifier.testTag("plugin-codex-change-cancel")) { Text("取消读取") }
            }
        } else {
            TextButton(onClick = {
                if (state?.document == null) { expanded = true; extensions.loadCodexChanges(reference) }
                else expanded = !expanded
            }, enabled = (state?.document != null || extensions.canReadCodexChanges) && current?.loading != true,
                modifier = Modifier.testTag("plugin-codex-read-change-${reference.snapshotID}")) {
                Text(if (state?.document == null) "读取完整补丁" else if (expanded) "收起完整补丁" else "展开完整补丁")
            }
            if (state?.document == null && !extensions.canReadCodexChanges) {
                Text("当前会话无法读取完整补丁。", style = MaterialTheme.typography.bodySmall)
            }
        }
        state?.issue?.let { Text(it, color = MaterialTheme.colorScheme.error, style = MaterialTheme.typography.bodySmall) }
        if (expanded) state?.document?.let { CodexCompleteChanges(it) }
    }
}

@Composable internal fun CodexCompleteChanges(document: CodexChangeDocument) {
    var selection by remember(document) { mutableIntStateOf(0) }
    Column(Modifier.fillMaxWidth().testTag("plugin-codex-complete-changes"), verticalArrangement = Arrangement.spacedBy(6.dp)) {
        Text("完整补丁 · ${document.files.size} 个文件", style = MaterialTheme.typography.labelLarge)
        if (document.files.isEmpty()) Text("此快照没有文件变更。")
        else {
            LazyColumn(Modifier.fillMaxWidth().heightIn(max = 140.dp).testTag("plugin-codex-change-files")) {
                itemsIndexed(document.files, key = { index, _ -> index }) { index, file ->
                    TextButton(onClick = { selection = index }, modifier = Modifier.fillMaxWidth().testTag("plugin-codex-change-file-$index")) {
                        Text(file.path, maxLines = 2, style = MaterialTheme.typography.bodySmall, fontFamily = FontFamily.Monospace)
                    }
                }
            }
            document.files.getOrNull(selection)?.let { file ->
                Text(when (file.kind) { "add" -> "新增"; "delete" -> "删除"; else -> "修改" }, style = MaterialTheme.typography.labelMedium)
                Text(file.path, Modifier.fillMaxWidth().horizontalScroll(rememberScrollState()), fontFamily = FontFamily.Monospace)
                file.movePath?.let { Text("→ $it", Modifier.horizontalScroll(rememberScrollState()), fontFamily = FontFamily.Monospace) }
                key(selection, document) {
                    Box(Modifier.fillMaxWidth().height(300.dp).verticalScroll(rememberScrollState()).horizontalScroll(rememberScrollState())) {
                        BasicTextField(value = file.diff, onValueChange = {}, readOnly = true,
                            modifier = Modifier.widthIn(min = 280.dp).testTag("plugin-codex-full-diff-text"),
                            textStyle = MaterialTheme.typography.bodySmall.copy(fontFamily = FontFamily.Monospace,
                                color = MaterialTheme.colorScheme.onSurface))
                    }
                }
            }
        }
    }
}
