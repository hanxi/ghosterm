package com.hanxi.ghosterm

import androidx.compose.foundation.layout.*
import androidx.compose.foundation.selection.toggleable
import androidx.compose.material3.*
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.unit.dp

@Composable
internal fun PluginInstallMaintenanceContent(model: PluginInstallMaintenanceModel, removal: PluginVersionRemovalModel, canAct: Boolean, inspect: (PluginInstallJournal.Reference) -> Unit) {
    Text("安装归属维护", Modifier.testTag("plugin-install-maintenance"), style = MaterialTheme.typography.titleMedium)
    if (model.busy) LinearProgressIndicator(Modifier.fillMaxWidth())
    model.issue?.let { Text(it, Modifier.testTag("plugin-install-maintenance-issue"), color = MaterialTheme.colorScheme.error) }
    model.observation?.let { value ->
        Text("归属记录 ${value.entries.size} 项", Modifier.testTag("plugin-install-maintenance-count"))
        value.entries.forEachIndexed { index, entry ->
            val reference = entry.reference
            Text("${reference?.id ?: "无法核对的记录 ${index + 1}"}\n" + when (entry.state) {
                PluginInstallJournal.State.ACTIVE -> "目录仍有活动引用，暂不能维护"
                PluginInstallJournal.State.ABANDONED -> "已登记的遗留安装暂存，可核对完整清单"
                PluginInstallJournal.State.PUBLISHED -> "同一目录已在目标位置，只可退役记录"
                PluginInstallJournal.State.ABSENT -> "登记的目录已不在原位，只可退役记录"
                PluginInstallJournal.State.UNCONFIRMED -> "尚无可核对的目录归属，保留对象"
                PluginInstallJournal.State.CHANGED -> "目录或归属已变化，保留对象"
                PluginInstallJournal.State.INVALID -> "归属记录查不出来，保留对象"
            }, Modifier.testTag("plugin-install-maintenance-state-${reference?.id ?: index}"))
            if (reference != null) {
                Text("记录 SHA-256：${reference.digest}\n暂存名：${reference.name}\n目标发布描述 SHA-256：${reference.target}", style = MaterialTheme.typography.bodySmall)
                if (entry.state == PluginInstallJournal.State.ABANDONED) OutlinedButton(onClick = { inspect(reference) }, enabled = canAct && removal.canPreview && !model.busy,
                    modifier = Modifier.fillMaxWidth().heightIn(min = 48.dp).testTag("plugin-install-maintenance-remove-${reference.id}")) { Text("核对并删除此安装暂存…") }
                if (entry.state in setOf(PluginInstallJournal.State.PUBLISHED, PluginInstallJournal.State.ABSENT)) OutlinedButton(onClick = { model.choose(entry) }, enabled = canAct && !model.busy,
                    modifier = Modifier.fillMaxWidth().heightIn(min = 48.dp).testTag("plugin-install-maintenance-retire-${reference.id}")) { Text("只退役这条归属记录…") }
            }
        }
    }
    model.target?.let { reference ->
        Text("只移除归属记录 ${reference.id}；保留版本、未知对象和来源授权状态。", Modifier.testTag("plugin-install-retire-preview"))
        Row(Modifier.fillMaxWidth().heightIn(min = 48.dp).testTag("plugin-install-retire-acknowledge")
            .toggleable(model.acknowledged, enabled = !model.busy, role = Role.Checkbox, onValueChange = model::acknowledge)) {
            Checkbox(model.acknowledged, onCheckedChange = null); Text("确认只退役这条已解决的记录")
        }
        Button(onClick = model::confirm, enabled = model.canConfirm && canAct, modifier = Modifier.fillMaxWidth().testTag("plugin-install-retire-confirm")) { Text("确认退役记录") }
        TextButton(onClick = model::cancel, enabled = !model.committing) { Text("保留记录") }
    }
    model.result?.let { Text(if (it) "归属记录已退役，目录同步已确认。" else "归属记录已移除；持久性未确认，请刷新核对。", Modifier.testTag("plugin-install-retire-result")) }
}
