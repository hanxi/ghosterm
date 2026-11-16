package com.hanxi.ghosterm

import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.selection.toggleable
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp

@Composable
internal fun PluginVersionPolicyContent(model: PluginVersionSelectionModel, enabled: Boolean = true) {
    model.pending?.let { request -> key(request) {
        val picker = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { model.completePick(request, it) }
        LaunchedEffect(request) { try { picker.launch(arrayOf("*/*")) } catch (_: Exception) { model.failedPick(request) } }
    } }
    OutlinedButton(onClick = { model.beginPick() }, enabled = enabled && !model.busy && model.pending == null,
        modifier = Modifier.fillMaxWidth().heightIn(min = 48.dp).testTag("plugin-version-policy-pick")) { Text("选择本次可信渠道策略") }
    model.policy?.let { SelectionContainer { Text("本次策略 ${it.size} 字节\nSHA-256：${it.sha256}",
        style = MaterialTheme.typography.bodySmall, fontFamily = FontFamily.Monospace) } }
    Row(Modifier.fillMaxWidth().heightIn(min = 48.dp).testTag("plugin-version-policy-trust")
        .toggleable(model.trusted, enabled = enabled && model.policy != null && !model.committing, role = Role.Checkbox, onValueChange = model::trust)) {
        Checkbox(model.trusted, onCheckedChange = null); Text("我已独立核对并信任此策略快照")
    }
    model.issue?.let { Text(it, Modifier.testTag("plugin-version-selection-issue"), color = MaterialTheme.colorScheme.error) }
    if (model.busy) LinearProgressIndicator(Modifier.fillMaxWidth())
    model.result?.let { result ->
        Text("活动选择记录已更新${if (result.durabilityConfirmed) "，目录同步已确认" else "；持久性未确认，请刷新核对"}。\n来源批准和新 Session 授权需另行确认。",
            Modifier.testTag("plugin-version-selection-result"), style = MaterialTheme.typography.bodyMedium)
    }
}

@Composable
private fun VersionDeclarations(value: ExtensionController.SignedArchiveReview, prefix: String) {
    SelectionContainer { Column(Modifier.testTag("plugin-version-$prefix-declarations"), verticalArrangement = Arrangement.spacedBy(6.dp)) {
        Text("${value.review.id} ${value.review.version} · 签名发布者 ${value.proof.publisher} · 渠道 ${value.proof.channel}")
        Text("许可证 ${value.review.license} · Client ${value.review.runtime}")
        Text("SDK 声明 ${value.declarations.sdkRange} · 本机选择 ${value.declarations.selectedSDK}\n协议声明 ${value.declarations.protocolRange.joinToString()} · 可用 ${value.declarations.protocols.joinToString()}")
        Text("必需能力 ${value.review.requiredCapabilities.joinToString()}\n缺少的可选能力 ${value.review.optionalMissingCapabilities.joinToString().ifEmpty { "无" }}")
        Text("目录包 SHA-256 ${value.review.digest}\n清单 ${value.declarations.manifestSHA256}\n策略 ${value.proof.policySHA256}\n密钥 ${value.proof.keyID}", fontFamily = FontFamily.Monospace, style = MaterialTheme.typography.bodySmall)
        value.declarations.permissions.forEach { Text("${it.side} · ${it.capability}\n用途 ${it.purpose}\n范围 ${it.scope}\n程序 ${it.programs.joinToString().ifEmpty { "无" }}") }
        value.review.commands.forEach { Text("命令 ${it.path} · ${it.title}\n参数 ${it.argsSchema} · 视图 ${it.views.joinToString()}") }
        Text("完整 Executor / 依赖声明\n${value.declarations.executorJSON ?: "无"}\n实际执行环境与依赖需另行核对。")
    } }
}

@Composable
internal fun PluginVersionPreviewContent(model: PluginVersionSelectionModel) {
    model.preview?.let { preview ->
        Text("活动选择切换预览", Modifier.testTag("plugin-version-preview"), style = MaterialTheme.typography.titleMedium)
        Text(when (preview.previousState) {
            ExtensionController.PreviousVersionState.NONE -> "没有旧活动选择，这是首次选择。"
            ExtensionController.PreviousVersionState.VERIFIED -> "旧版已用本次策略核验；此前${if (preview.previousEnabled == true) "启用" else "禁用"}。"
            ExtensionController.PreviousVersionState.UNKNOWN -> "旧版状态查不出来，不能把旧权限当作空，也不能判断没有新增权限。"
        }, Modifier.testTag("plugin-version-previous-state"))
        val addedCapabilities = preview.addedCapabilities
        val removedCapabilities = preview.removedCapabilities
        if (addedCapabilities != null && removedCapabilities != null) {
            Text("必需能力声明新增：${addedCapabilities.joinToString().ifEmpty { "无" }}；移除：${removedCapabilities.joinToString().ifEmpty { "无" }}。",
                Modifier.testTag("plugin-version-capability-diff"))
        } else Text("旧能力声明查不出来，无法计算完整差异。", Modifier.testTag("plugin-version-capability-unknown"))
        preview.addedPermissions?.let { added ->
            Text("新增或修改权限 ${added.size} 项；移除或修改旧权限 ${preview.removedPermissions!!.size} 项。")
            added.forEach { Text("＋ ${it.side} · ${it.capability} · ${it.purpose} · ${it.scope} · ${it.programs.joinToString()}") }
            preview.removedPermissions!!.forEach { Text("－ ${it.side} · ${it.capability} · ${it.purpose} · ${it.scope} · ${it.programs.joinToString()}") }
        }
        if (preview.addedPermissions == null) Text("旧权限声明查不出来，无法计算完整差异；请核对候选完整声明。", Modifier.testTag("plugin-version-permission-unknown"))
        VersionDeclarations(preview.candidate, "candidate")
        preview.previous?.let { VersionDeclarations(it, "previous") }
        Text("切换影响后续新实例，已有实例保留原包；完整声明不等于代码审计，来源批准与 Session 授权仍须独立确认。")
        Row(Modifier.fillMaxWidth().heightIn(min = 48.dp).testTag("plugin-version-acknowledge")
            .toggleable(model.acknowledged, enabled = !model.busy, role = Role.Checkbox, onValueChange = model::acknowledge)) {
            Checkbox(model.acknowledged, onCheckedChange = null); Text("我已核对候选完整声明和旧版状态，确认本次活动选择切换")
        }
        Button(onClick = model::confirm, enabled = model.canConfirm, modifier = Modifier.fillMaxWidth().heightIn(min = 48.dp).testTag("plugin-version-confirm")) { Text("确认切换活动选择") }
        TextButton(onClick = model::cancel, enabled = !model.committing, modifier = Modifier.testTag("plugin-version-cancel")) { Text("保留原选择并取消") }
    }
    model.disableTarget?.let { target ->
        Text("禁用 ${target.pluginID} 的后续新实例；已有实例保持原包，可另行停止。", Modifier.testTag("plugin-version-disable-preview"))
        Row(Modifier.fillMaxWidth().heightIn(min = 48.dp).testTag("plugin-version-disable-acknowledge")
            .toggleable(model.acknowledged, enabled = !model.busy, role = Role.Checkbox, onValueChange = model::acknowledge)) {
            Checkbox(model.acknowledged, onCheckedChange = null); Text("确认禁用后续新实例")
        }
        Button(onClick = model::confirmDisable, enabled = model.canDisable, modifier = Modifier.fillMaxWidth().heightIn(min = 48.dp).testTag("plugin-version-disable-confirm")) { Text("确认禁用") }
        TextButton(onClick = model::cancel, enabled = !model.committing) { Text("保留当前选择") }
    }
}
