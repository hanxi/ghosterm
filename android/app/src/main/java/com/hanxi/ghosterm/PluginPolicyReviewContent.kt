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
internal fun PluginPolicyReviewContent(model: PluginPolicyReviewModel, canAct: Boolean, begin: () -> Unit) {
    OutlinedButton(onClick = begin, enabled = model.canReview && canAct,
        modifier = Modifier.fillMaxWidth().heightIn(min = 48.dp).testTag("plugin-policy-review-open")) { Text("按此策略复核全部已启用选择") }
    if (model.busy) LinearProgressIndicator(Modifier.fillMaxWidth())
    model.issue?.let { Text(it, Modifier.testTag("plugin-policy-review-issue"), color = MaterialTheme.colorScheme.error) }
    model.result?.let { Text("已禁用所选 ${it.pluginIDs.size} 项的后续新建${if (it.durabilityConfirmed) "，目录同步已确认。" else "；持久性未确认，请刷新核对。"}", Modifier.testTag("plugin-policy-review-result")) }
    model.review?.let { value ->
        Text("本次策略渠道 ${value.channel}\nSHA-256 ${value.policySHA256}", Modifier.testTag("plugin-policy-review-summary"))
        Text("这是独立选择的策略快照，无联网新鲜度或全局撤销保证；其它渠道也可能被此单一策略拒绝。查不出来不等于已撤销。")
        if (value.entries.isEmpty()) Text("没有已启用选择需要复核。", Modifier.testTag("plugin-policy-review-empty"))
        value.entries.forEach { entry ->
            Text("${entry.pluginID} · " + when (entry.state) {
                PluginPolicyReview.State.ACCEPTED -> "此策略接受"
                PluginPolicyReview.State.REJECTED -> "此策略拒绝"
                PluginPolicyReview.State.UNKNOWN -> "查不出来"
            }, Modifier.testTag("plugin-policy-state-${entry.pluginID}"))
            Text("${entry.releaseID}\n${entry.reason}", style = MaterialTheme.typography.bodySmall)
            if (entry.state != PluginPolicyReview.State.ACCEPTED) Row(Modifier.fillMaxWidth().heightIn(min = 48.dp).testTag("plugin-policy-select-${entry.pluginID}")
                .toggleable(entry.pluginID in model.selectedIDs, enabled = !model.busy && canAct, role = Role.Checkbox, onValueChange = { model.choose(entry.pluginID, it) })) {
                Checkbox(entry.pluginID in model.selectedIDs, onCheckedChange = null); Text("我选择禁用此插件的后续新建")
            }
        }
        Text("确认只禁用勾选的同一活动选择，不授予权限、不删除包、不停止已有运行；确认前重核策略快照和选择修订。")
        Button(onClick = model::apply, enabled = model.canApply && canAct, modifier = Modifier.fillMaxWidth().heightIn(min = 48.dp).testTag("plugin-policy-review-apply")) { Text("确认禁用以上勾选项") }
        TextButton(onClick = model::cancel, enabled = !model.committing, modifier = Modifier.testTag("plugin-policy-review-cancel")) { Text("保留选择并取消复核") }
    }
}
