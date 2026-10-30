package com.hanxi.ghosterm

import androidx.compose.foundation.layout.*
import androidx.compose.material3.*
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.unit.dp
import com.hanxi.ghosterm.data.IdentityReferences
import com.hanxi.ghosterm.data.originDescription

@Composable
fun HostIdentityEditor(model: HostsViewModel) {
    val draft = model.draft ?: return
    val own = draft.edit.identityFiles ?: model.identitySources.filter { it.explicitValue }.map { it.value }
    val enabled = model.canEditDraft && !model.busy && !model.identityLoading && model.identityError == null
    fun label(value: String): String {
        val id = IdentityReferences.decode(value) ?: return value
        val record = model.identityRecords.find { it.id == id }
        return record?.let { it.name + if (it.identity == null) "（不可用）" else "" } ?: "已移除的身份 · $id"
    }
    Column(Modifier.fillMaxWidth().testTag("host-identities"), verticalArrangement = Arrangement.spacedBy(8.dp)) {
        Text("SSH 身份", style = MaterialTheme.typography.titleMedium)
        Text("本块身份与继承身份按配置顺序累积；更改只影响后续连接。", style = MaterialTheme.typography.bodySmall)
        if (draft.edit.alias != draft.original?.alias && draft.original != null)
            Text("以下来源按原别名显示；改名保存后重新求值。", style = MaterialTheme.typography.bodySmall)
        if (model.identityLoading) LinearProgressIndicator(Modifier.fillMaxWidth())
        model.identityError?.let {
            Text(it, color = MaterialTheme.colorScheme.error, modifier = Modifier.testTag("host-identity-error"))
            TextButton(onClick = model::loadEditorIdentities, enabled = !model.busy) { Text("重新读取身份") }
        }
        if (!model.identityLoading && model.identityError == null) {
            Text("此 Host 块", style = MaterialTheme.typography.titleSmall)
            if (own.isEmpty()) Text("未设置本块身份，保留继承规则。")
            own.forEachIndexed { index, value ->
                Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Column(Modifier.weight(1f)) {
                        Text(label(value), Modifier.testTag("host-own-identity-$index"))
                        if (label(value) != value) Text(value, style = MaterialTheme.typography.bodySmall)
                        if (draft.edit.identityFiles == null) model.identitySources.filter { it.explicitValue }.getOrNull(index)?.let {
                            Text(it.originDescription(), style = MaterialTheme.typography.bodySmall)
                        }
                    }
                    TextButton(onClick = { model.setIdentityFiles(own.filterIndexed { i, _ -> i != index }) },
                        enabled = enabled, modifier = Modifier.testTag("host-remove-identity-$index")) { Text("移除") }
                }
            }
            if (own.isNotEmpty()) TextButton(onClick = { model.setIdentityFiles(emptyList()) }, enabled = enabled) { Text("身份恢复继承") }
            val inherited = model.identitySources.filter { !it.explicitValue }
            if (inherited.isNotEmpty()) {
                Text("继承的身份（只读）", style = MaterialTheme.typography.titleSmall)
                inherited.forEach { source ->
                    Text(label(source.value))
                    Text(source.value + "\n" + source.originDescription(), style = MaterialTheme.typography.bodySmall)
                }
            }
            Text("添加已导入身份", style = MaterialTheme.typography.titleSmall)
            model.identityRecords.filter { it.identity != null }.forEach { record ->
                val reference = IdentityReferences.encode(record.id)
                OutlinedButton(onClick = { model.setIdentityFiles(own + reference) }, enabled = enabled && reference !in own && own.size < 256,
                    modifier = Modifier.fillMaxWidth().testTag("host-add-identity-${record.id}")) { Text(record.name) }
            }
            if (model.identityRecords.none { it.identity != null }) Text("请先在设置 → SSH 身份中导入私钥。")
            Text("应用身份引用仅适用于本机加密容器；导出 SSH 配置不会导出私钥。普通文件路径保留原值。", style = MaterialTheme.typography.bodySmall)
        }
    }
}
