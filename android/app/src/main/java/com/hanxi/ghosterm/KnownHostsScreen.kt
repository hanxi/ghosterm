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
import com.hanxi.ghosterm.data.trustDisplay

@Composable
fun KnownHostsScreen(model: KnownHostsViewModel) {
    LazyColumn(Modifier.fillMaxSize().testTag("known-hosts-screen"), contentPadding = PaddingValues(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp)) {
        if (model.busy) item { LinearProgressIndicator(Modifier.fillMaxWidth()) }
        model.notice?.let { item { Text(it, Modifier.testTag("known-hosts-notice")) } }
        model.error?.let { item { Text(it, Modifier.testTag("known-hosts-error"), color = MaterialTheme.colorScheme.error) } }
        item { TextButton(onClick = model::reload, enabled = !model.writing) { Text("重新载入记录") } }
        val entry = model.selected
        if (entry == null) {
            item { Text("来自应用内 known_hosts。记录不代表当前已连接；首次连接仍需核对主机公钥。", style = MaterialTheme.typography.bodySmall) }
            val records = model.snapshot?.records
            if (!model.busy && model.error == null && records?.isEmpty() == true)
                item { Text("还没有主机验证记录", Modifier.testTag("known-hosts-empty")) }
            items(records.orEmpty(), key = { it.id }) { record ->
                OutlinedButton(onClick = { model.open(record.id) }, enabled = !model.busy,
                    modifier = Modifier.fillMaxWidth().testTag("known-host-row-${record.index}")) {
                    Column(Modifier.fillMaxWidth()) {
                        Text(record.record.hosts.trustDisplay().take(160).ifEmpty { "未识别记录" }, maxLines = 3)
                        Text("第 ${record.record.line} 行 · ${record.kind}", style = MaterialTheme.typography.bodySmall)
                    }
                }
            }
        } else {
            val record = entry.record
            item { Text(entry.kind, Modifier.testTag("known-host-kind"), style = MaterialTheme.typography.titleLarge) }
            item { Text("主机 / 匹配表达式"); SelectionContainer { Text(record.hosts.trustDisplay(), Modifier.testTag("known-host-hosts")) } }
            item { Text("原文件第 ${record.line} 行", style = MaterialTheme.typography.bodySmall) }
            if (record.marker.isNotEmpty()) item { Text("规则：${record.marker.trustDisplay()}") }
            if (record.hashed) item { Text("主机名已哈希，无法还原地址；当前连接器不支持匹配此格式。") }
            item { Text("记录算法：${record.keyType.trustDisplay().ifEmpty { "未知" }}", Modifier.testTag("known-host-algorithm")) }
            entry.info.fingerprint?.let { fingerprint -> item {
                Text("公钥 SHA256 指纹")
                SelectionContainer { Text(fingerprint, Modifier.testTag("known-host-fingerprint")) }
            } }
            entry.info.unavailable?.let { item { Text(it, Modifier.testTag("known-host-fingerprint-unavailable")) } }
            item { Text("指纹来自记录中的公钥，不代表本次已验证服务器。请通过可信渠道核对。", style = MaterialTheme.typography.bodySmall) }
            if (entry.removable) item {
                OutlinedButton(onClick = model::requestRemoval, enabled = !model.busy,
                    modifier = Modifier.fillMaxWidth()) { Text("移除此记录") }
            } else item { Text("此类记录暂不提供移除操作。", Modifier.testTag("known-host-readonly")) }
        }
    }
    if (model.confirmingRemoval) AlertDialog(onDismissRequest = model::cancelRemoval,
        title = { Text("移除这条主机验证记录？") },
        text = { Text("将移除原文件第 ${model.selected?.record?.line} 行的完整记录；该行包含的所有主机名或通配规则都会受影响。其他匹配记录仍可能使新连接被信任。已建立的连接、SSH 配置和身份保持不变。") },
        confirmButton = { TextButton(onClick = model::remove) { Text("确认移除记录") } },
        dismissButton = { TextButton(onClick = model::cancelRemoval) { Text("保留记录") } })
}
