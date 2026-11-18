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
import kotlinx.coroutines.launch
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
                    try {
                        if (policy == null) worker.inspectConfiguredChannelArchive(fd, descriptor, signature)
                        else worker.inspectSignedArchive(fd, descriptor, signature, policy)
                    }
                    finally { withContext(NonCancellable) { worker.close() } }
                }
            }, { captured, descriptor, signature, policy, expected ->
                captured.borrow().use { fd ->
                    val worker = controller.newReleaseInspector()
                    try {
                        if (policy == null) worker.installConfiguredChannelArchive(fd, descriptor, signature, expected)
                        else worker.installSignedArchive(fd, descriptor, signature, policy, expected)
                    }
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
        Text(if (model.useConfiguredChannel) "选择 ZIP、发布描述和公开签名。"
            else "选择 ZIP、发布描述、公开签名，以及从独立可信来源取得的渠道公钥策略。",
            style = MaterialTheme.typography.bodySmall)
        if (controller.hasConfiguredChannel) Row(Modifier.fillMaxWidth().heightIn(min = 48.dp)
            .testTag("plugin-zip-channel")
            .toggleable(model.useConfiguredChannel, enabled = !model.busy && model.pending == null,
                role = Role.Checkbox, onValueChange = model::selectConfiguredChannel)) {
            Checkbox(model.useConfiguredChannel, onCheckedChange = null)
            Text("使用已验证的渠道缓存")
        }
        PluginArchiveMaterial.entries.filter { !model.useConfiguredChannel || it != PluginArchiveMaterial.POLICY }.forEach { kind ->
            OutlinedButton(onClick = { model.beginPick(kind) }, enabled = !model.busy && model.pending == null,
                modifier = Modifier.fillMaxWidth().heightIn(min = 48.dp).testTag("plugin-zip-pick-${kind.name}")) { Text("选择${kind.label}") }
            val bytes = if (kind == PluginArchiveMaterial.ARCHIVE) model.archive?.bytes else model.documents[kind.signatureKind]?.size?.toLong()
            val hash = if (kind == PluginArchiveMaterial.ARCHIVE) model.archive?.sha256 else model.documents[kind.signatureKind]?.sha256
            if (bytes != null && hash != null) SelectionContainer {
                Text("已捕获 $bytes 字节\nSHA-256：$hash", Modifier.testTag("plugin-zip-material-${kind.name}"),
                    style = MaterialTheme.typography.bodySmall, fontFamily = FontFamily.Monospace)
            }
        }
        if (!model.useConfiguredChannel) Row(Modifier.fillMaxWidth().heightIn(min = 48.dp).testTag("plugin-zip-trust")
            .toggleable(model.trustedPolicy, enabled = !model.installing && PluginSignatureMaterial.POLICY in model.documents,
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
                result.channelGeneration?.let { Text("已验证渠道策略 · 第 ${it} 代") }
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
            Row(Modifier.fillMaxWidth().heightIn(min = 48.dp).testTag("plugin-zip-install-confirmation")
                .toggleable(model.installAcknowledged, enabled = !model.busy && model.installation == null,
                    role = Role.Checkbox, onValueChange = model::acknowledgeInstall)) {
                Checkbox(model.installAcknowledged, onCheckedChange = null)
                Text("我已审阅上述发布者、权限和依赖，确认保存此版本到本机。", style = MaterialTheme.typography.bodyMedium)
            }
            Button(onClick = model::installArchive, enabled = model.canInstall,
                modifier = Modifier.fillMaxWidth().heightIn(min = 48.dp).testTag("plugin-zip-install")) {
                Text(if (model.installing) "正在安装…" else "安装此版本")
            }
        }
        model.installation?.let { installed ->
            Text("本机版本已发布：${installed.snapshot.review.id} ${installed.snapshot.review.version}\n${if (installed.publication.durabilityConfirmed) "目录同步已确认" else "持久性未确认"} · 尚未选择活动版本",
                Modifier.testTag("plugin-zip-installed"), style = MaterialTheme.typography.bodyMedium)
            installed.publication.maintenanceIssue?.let { Text(it, color = MaterialTheme.colorScheme.error) }
        }
        Text("来源批准和 Session 授权需另行确认。本页仅安装本机版本；远端部署与活动版本选择另行操作。收起会释放导入快照，已发布的版本保留。", style = MaterialTheme.typography.bodySmall)
    }
}

@Composable
internal fun PluginChannelRefreshContent(controller: ExtensionController?, requestStop: suspend () -> Int) {
    if (controller?.canRefreshChannelPolicy != true) return
    val scope = rememberCoroutineScope()
    var busy by remember(controller) { mutableStateOf(false) }
    var status by remember(controller) { mutableStateOf<String?>(null) }
    var failed by remember(controller) { mutableStateOf(false) }
    var affected by remember(controller) { mutableIntStateOf(0) }
    TextButton(enabled = !busy, modifier = Modifier.testTag("plugin-channel-refresh"), onClick = {
        busy = true
        status = null
        scope.launch {
            try {
                val current = controller.refreshChannelPolicy()
                failed = false
                val count = controller.runningInstancesWithRevokedPublisher(current)
                affected = count
                status = "已更新 ${current.channel} 渠道策略至第 ${current.generation} 代。" +
                    if (count > 0) " 本应用仍持有 ${count} 个受撤销影响的实例，可在实例页停止。" else ""
            } catch (_: kotlinx.coroutines.TimeoutCancellationException) {
                failed = true
                status = "刷新插件渠道策略超时。"
            } catch (cancelled: kotlinx.coroutines.CancellationException) {
                throw cancelled
            } catch (failure: Exception) {
                failed = true
                status = failure.message ?: "无法刷新插件渠道。"
            } finally { busy = false }
        }
    }) { Text(if (busy) "正在刷新渠道策略…" else "刷新渠道策略") }
    if (affected > 0) TextButton(enabled = !busy, modifier = Modifier.testTag("plugin-channel-stop"), onClick = {
        busy = true
        scope.launch {
            try {
                val requested = requestStop()
                failed = false
                status = "已向 ${requested} 个实例发出停止请求。请在实例页核对停止与释放状态。"
            } catch (cancelled: kotlinx.coroutines.CancellationException) {
                throw cancelled
            } catch (failure: Exception) {
                failed = true
                status = failure.message ?: "无法核对撤销状态。"
            } finally { busy = false }
        }
    }) { Text("请求停止受渠道撤销影响的实例") }
    status?.let {
        Text(it, Modifier.testTag("plugin-channel-refresh-status"),
            color = if (failed) MaterialTheme.colorScheme.error else LocalContentColor.current)
    }
}
