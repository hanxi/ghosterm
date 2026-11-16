package com.hanxi.ghosterm

import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.selection.toggleable
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.withContext

@Composable
internal fun PluginArchiveReviewContent(controller: ExtensionController?) {
    var expanded by remember(controller) { mutableStateOf(false) }
    Column(Modifier.fillMaxWidth()) {
        TextButton(onClick = { expanded = !expanded }, modifier = Modifier.testTag("plugin-zip-disclosure")) {
            Text(if (expanded) "收起签名 ZIP 检查" else "检查签名 ZIP…")
        }
        if (expanded && controller != null) PluginArchiveInputs(controller)
    }
}

@Composable
private fun PluginArchiveInputs(controller: ExtensionController) {
    val context = LocalContext.current.applicationContext
    val scope = rememberCoroutineScope()
    val model = remember(controller) {
        PluginArchiveReviewModel(scope, { uri -> PluginArchiveCapture.read(context, context.contentResolver, uri) },
            { uri, kind -> PluginSignatureDocuments.read(context.contentResolver, uri, kind) },
            { captured, descriptor, signature, policy ->
                captured.borrow().use { fd ->
                    val worker = controller.newReleaseInspector()
                    try { worker.inspectSignedArchive(fd, descriptor, signature, policy) }
                    finally { withContext(NonCancellable) { worker.close() } }
                }
            })
    }
    DisposableEffect(model) { onDispose { model.close() } }
    model.pending?.let { request -> key(request) {
        val picker = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri -> model.completePick(request, uri) }
        LaunchedEffect(request) {
            try { picker.launch(arrayOf("*/*")) } catch (_: Exception) { model.failPick(request) }
        }
    } }
    Column(Modifier.padding(horizontal = 16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
        Text("选择 ZIP、发布描述、公开签名，以及从独立可信来源取得的渠道公钥策略。", style = MaterialTheme.typography.bodySmall)
        PluginArchiveMaterial.entries.forEach { kind ->
            OutlinedButton(onClick = { model.beginPick(kind) }, enabled = !model.busy && model.pending == null,
                modifier = Modifier.fillMaxWidth().heightIn(min = 48.dp).testTag("plugin-zip-pick-${kind.name}")) { Text("选择${kind.label}") }
            val bytes = if (kind == PluginArchiveMaterial.ARCHIVE) model.archive?.bytes else model.documents[kind.signatureKind]?.size?.toLong()
            val hash = if (kind == PluginArchiveMaterial.ARCHIVE) model.archive?.sha256 else model.documents[kind.signatureKind]?.sha256
            if (bytes != null && hash != null) SelectionContainer {
                Text("已捕获 $bytes 字节\nSHA-256：$hash", Modifier.testTag("plugin-zip-material-${kind.name}"),
                    style = MaterialTheme.typography.bodySmall, fontFamily = FontFamily.Monospace)
            }
        }
        Row(Modifier.fillMaxWidth().heightIn(min = 48.dp).testTag("plugin-zip-trust")
            .toggleable(model.trustedPolicy, enabled = PluginSignatureMaterial.POLICY in model.documents,
                role = Role.Checkbox, onValueChange = model::trustPolicy)) {
            Checkbox(model.trustedPolicy, onCheckedChange = null)
            Text("我已独立核对并信任此渠道策略快照", style = MaterialTheme.typography.bodyMedium)
        }
        Button(onClick = model::checkArchive, enabled = model.canInspect,
            modifier = Modifier.fillMaxWidth().heightIn(min = 48.dp).testTag("plugin-zip-check")) {
            Text(if (model.busy) "正在捕获或检查…" else "检查 ZIP 包")
        }
        if (model.busy) LinearProgressIndicator(Modifier.fillMaxWidth())
        model.issue?.let { Text(it, Modifier.testTag("plugin-zip-issue"), color = MaterialTheme.colorScheme.error) }
        model.receipt?.let { result ->
            Text("ZIP 已检查：${result.review.id} ${result.review.version}", Modifier.testTag("plugin-zip-verified"), style = MaterialTheme.typography.titleMedium)
            SelectionContainer { Column(Modifier.testTag("plugin-zip-declarations"), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text("签名发布者 ${result.proof.publisher} · 渠道 ${result.proof.channel}")
                Text("许可证 ${result.review.license} · Client ${result.review.runtime}")
                Text("SDK 声明 ${result.declarations.sdkRange} · 本机选择 ${result.declarations.selectedSDK}\n协议声明 ${result.declarations.protocolRange.joinToString()} · 可用交集 ${result.declarations.protocols.joinToString()}")
                Text("必需能力：${result.review.requiredCapabilities.joinToString().ifEmpty { "无" }}\n缺失的可选能力：${result.review.optionalMissingCapabilities.joinToString().ifEmpty { "无" }}")
                Text("目录包 SHA-256：${result.review.digest}\n清单 SHA-256：${result.declarations.manifestSHA256}\n策略 SHA-256：${result.proof.policySHA256}\n密钥 SHA-256：${result.proof.keyID}", fontFamily = FontFamily.Monospace)
                Text("权限声明（${result.declarations.permissions.size} 项）")
                result.declarations.permissions.forEach { item ->
                    Text("${item.side} · ${item.capability}\n用途：${item.purpose}\n范围：${item.scope}\n程序：${item.programs.joinToString().ifEmpty { "无" }}")
                }
                result.review.commands.forEach { Text("命令 ${it.path} · ${it.title}") }
                Text("Executor 声明\n${result.declarations.executorJSON ?: "无"}\n实际执行环境与依赖需另行核对。", fontFamily = FontFamily.Monospace)
            } }
        }
        Text("安装状态：尚未安装。来源批准和 Session 授权需另行确认。收起或离开页面会释放 ZIP 快照和本次确认。", style = MaterialTheme.typography.bodySmall)
    }
}
