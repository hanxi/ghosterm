package com.hanxi.ghosterm

import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.selection.toggleable
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.material3.*
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp

@Composable
internal fun PluginVersionRemovalContent(model: PluginVersionRemovalModel) {
    if (model.busy) LinearProgressIndicator(Modifier.fillMaxWidth())
    model.issue?.let { Text(it, Modifier.testTag("plugin-removal-issue"), color = MaterialTheme.colorScheme.error) }
    model.result?.let {
        Text("本次已移除 ${it.removed} 项；观察到此前已不在原位置 ${it.observedAbsent} 项。\n${if (it.complete) "此删除意图已处理完成" else "仍有内容或删除意图待核对"}；${if (it.durabilityConfirmed) "目录同步已确认" else "持久性未确认，请刷新核对"}。",
            Modifier.testTag("plugin-removal-result"))
        it.issue?.let { issue -> Text(issue, color = MaterialTheme.colorScheme.error) }
        it.memoryIssue?.let { issue -> Text(issue, color = MaterialTheme.colorScheme.error) }
    }
    model.withdrawalDurability?.let { Text(if (it) "删除意图已撤销，原目录保留。" else "撤销回执持久性未确认，请刷新核对。", Modifier.testTag("plugin-removal-withdrawal")) }
    val value = model.preview ?: model.saved ?: return
    Text(if (value.pending) "待处理的删除意图" else if (value.unclassified != null) "归属未知目录删除预览" else if (value.stage == null) "本机版本删除预览" else "安装暂存删除预览", Modifier.testTag("plugin-removal-preview"), style = MaterialTheme.typography.titleMedium)
    SelectionContainer { Text("目录：${value.directory}\n${if (value.unclassified != null) "目录名称" else if (value.stage == null) "发布描述" else "安装归属记录"} SHA-256：${value.releaseID}\n清单 ${value.members.size} 项，共 ${value.bytes} 字节。",
        Modifier.testTag("plugin-removal-summary"), fontFamily = FontFamily.Monospace, style = MaterialTheme.typography.bodySmall) }
    if (value.pending) Text("这是保存的原始清单。刷新和打开页面只读取记录；继续时会重新核对剩余内容，不能核对的内容会保留。")
    LazyColumn(Modifier.fillMaxWidth().height(260.dp).testTag("plugin-removal-inventory"), verticalArrangement = Arrangement.spacedBy(6.dp)) {
        items(value.members, key = { it.path }) { item -> SelectionContainer {
            Text("${if (item.directory) "目录" else "文件"} ${item.path}\n${if (item.directory) "" else "${item.bytes} 字节 · SHA-256 ${item.sha256}"}",
                Modifier.testTag("plugin-removal-member-${item.path}"), fontFamily = FontFamily.Monospace, style = MaterialTheme.typography.bodySmall)
        } }
    }
    Row(Modifier.fillMaxWidth().heightIn(min = 48.dp).testTag("plugin-removal-acknowledge")
        .toggleable(model.acknowledged, enabled = !model.busy, role = Role.Checkbox, onValueChange = model::acknowledge)) {
        Checkbox(model.acknowledged, onCheckedChange = null); Text(if (value.pending) "我已核对保存的清单，确认本次所选处理" else if (value.unclassified != null) "我已核对完整文件清单，确认删除当前目录" else if (value.stage == null) "我已核对完整文件清单，确认删除此本机版本" else "我已核对完整文件清单，确认删除此安装暂存")
    }
    if (value.unclassified != null) {
        Text("无法证明此目录的安装归属。本次只决定删除已核对的当前内容，不补写或退役旧归属记录。")
        Row(Modifier.fillMaxWidth().heightIn(min = 48.dp).testTag("plugin-unclassified-acknowledge")
            .toggleable(model.acknowledgedUnclassified, enabled = !model.busy, role = Role.Checkbox, onValueChange = model::acknowledgeUnclassified)) {
            Checkbox(model.acknowledgedUnclassified, onCheckedChange = null); Text("我了解归属未知，仍确认本次处理这个完整目录")
        }
    }
    if (value.pending) {
        Button(onClick = model::resume, enabled = model.canContinue, modifier = Modifier.fillMaxWidth().heightIn(min = 48.dp).testTag("plugin-removal-resume")) { Text("明确继续此删除意图") }
        OutlinedButton(onClick = model::withdraw, enabled = model.canContinue, modifier = Modifier.fillMaxWidth().heightIn(min = 48.dp).testTag("plugin-removal-withdraw")) { Text("撤销尚未开始移除的意图") }
    } else {
        Button(onClick = model::confirm, enabled = model.canConfirm, modifier = Modifier.fillMaxWidth().heightIn(min = 48.dp).testTag("plugin-removal-confirm")) { Text(if (value.unclassified != null) "确认删除此未知目录" else if (value.stage == null) "确认删除此本机版本" else "确认删除此安装暂存") }
        TextButton(onClick = model::cancel, enabled = !model.committing, modifier = Modifier.testTag("plugin-removal-cancel")) { Text("取消预览并保留文件") }
    }
}
