package com.hanxi.ghosterm

import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.material3.*
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.unit.dp
import com.hanxi.ghosterm.data.IdentityReferences
import com.hanxi.ghosterm.data.originDescription

@Composable
fun IdentitiesScreen(model: IdentitiesViewModel, import: () -> Unit) {
    LazyColumn(Modifier.fillMaxSize().testTag("identities-screen"),contentPadding = PaddingValues(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp)) {
        if (model.busy) item { LinearProgressIndicator(Modifier.fillMaxWidth()) }
        model.error?.let { message -> item {
            Text(message,Modifier.testTag("identity-error"),color = MaterialTheme.colorScheme.error)
            TextButton(onClick = model::reload) { Text("重新载入身份") }
        } }
        model.notice?.let { item { Text(it,Modifier.testTag("identity-notice")) } }
        val record = model.selected
        if (record == null) {
            item {
                Button(onClick = import,enabled = !model.busy && !model.pickerPending,modifier = Modifier.fillMaxWidth()) { Text("导入私钥") }
                Text("导入副本由 Android 安全存储保护，原文件不会修改。",style = MaterialTheme.typography.bodySmall)
            }
            if (!model.busy && model.records.isEmpty() && model.error == null) item { Text("还没有保存的身份") }
            items(model.records,key = { it.id }) { entry ->
                OutlinedButton(onClick = { model.open(entry) },enabled = !model.busy && !model.pickerPending,
                    modifier = Modifier.fillMaxWidth().testTag("identity-row-${entry.id}")) {
                    Column(Modifier.fillMaxWidth()) {
                        Text(entry.name)
                        Text(entry.problem ?: if(entry.identity?.needsPassphrase == true) "连接时输入密钥口令" else "无需密钥口令",
                            style = MaterialTheme.typography.bodySmall)
                    }
                }
            }
        } else {
            item { Text(record.name,style = MaterialTheme.typography.titleLarge) }
            item {
                Text("使用方式：在主机编辑中关联，或仅为本次连接选择。")
                val reference = (IdentityReferences.PREFIX + record.id).takeIf { IdentityReferences.decode(it) != null }
                if (reference != null) SelectionContainer { Text(reference, style = MaterialTheme.typography.bodySmall) }
                else Text("此条目的身份编号无效，不能用于主机关联。", color = MaterialTheme.colorScheme.error)
                Text("此引用只适用于本机。重新导入会创建新身份，需重新关联；配置导出不含私钥。", style = MaterialTheme.typography.bodySmall)
            }
            model.info?.let { info -> item {
                info.algorithm?.let { Text("类型：$it",Modifier.testTag("identity-algorithm")) }
                info.fingerprint?.let { fingerprint -> SelectionContainer {
                    Text(fingerprint,Modifier.testTag("identity-fingerprint"))
                } }
                info.unavailable?.let { Text(it,Modifier.testTag("identity-info-unavailable")) }
            } }
            item { Text("引用主机", style = MaterialTheme.typography.titleMedium) }
            model.usageError?.let { message -> item {
                Text(message, Modifier.testTag("identity-usage-error"), color = MaterialTheme.colorScheme.error)
                TextButton(onClick = { model.open(record) }, enabled = !model.busy) { Text("重新读取引用") }
            } }
            if (!model.busy && model.usageError == null && model.usages.isEmpty()) item {
                Text("当前已列出的主机未引用此身份", Modifier.testTag("identity-no-usages"))
            }
            items(model.usages, key = { it.alias }) { usage ->
                Text(usage.alias, Modifier.testTag("identity-usage-${usage.alias}"))
                usage.sources.forEach { Text(it.originDescription(), style = MaterialTheme.typography.bodySmall) }
            }
            item { Text("按当前配置中的可连接主机查询；通配规则还可能影响未列出的名称。", style = MaterialTheme.typography.bodySmall) }
            item {
                OutlinedButton(onClick = model::requestRemoval,enabled = !model.busy && record.revision != null,
                    modifier = Modifier.fillMaxWidth()) { Text("移除此身份") }
            }
        }
    }
    if (model.confirmingRemoval) AlertDialog(onDismissRequest = model::cancelRemoval,
        title = { Text("移除 ${model.selected?.name}？") },
        text = { Text("将删除应用内副本及其安全密钥。原私钥文件和已建立的连接保留。SSH 配置中的引用不会删除，之后连接会提示身份不可用；重新导入后须重新关联。" +
            if (model.usageError != null) "当前引用情况无法确定。" else "当前列表中有 ${model.usages.size} 台主机引用此身份。") },
        confirmButton = { TextButton(onClick = model::remove) { Text("确认移除") } },
        dismissButton = { TextButton(onClick = model::cancelRemoval) { Text("保留身份") } })
}
