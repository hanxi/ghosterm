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

@Composable
internal fun PluginSignatureReviewContent(controller: ExtensionController?, review: ExtensionController.Review) {
    var expanded by remember(review.id, review.digest) { mutableStateOf(false) }
    Column(Modifier.fillMaxWidth().padding(horizontal = 16.dp)) {
        TextButton(onClick = { expanded = !expanded }, modifier = Modifier.testTag("plugin-signature-disclosure")) {
            Text(if (expanded) "收起目录包签名" else "核对目录包发布签名…")
        }
        if (expanded && controller != null) PluginSignatureInputs(controller, review)
    }
}

@Composable
private fun PluginSignatureInputs(controller: ExtensionController, review: ExtensionController.Review) {
    val scope = rememberCoroutineScope()
    val resolver = LocalContext.current.contentResolver
    val model = remember(controller, review.id, review.digest) {
        PluginSignatureReviewModel(scope, { uri, kind -> PluginSignatureDocuments.read(resolver, uri, kind) },
            { descriptor, signature, policy -> controller.verifyDirectorySignature(review.id, review.digest, descriptor, signature, policy) })
    }
    DisposableEffect(model) { onDispose { model.close() } }
    model.pending?.let { request ->
        key(request) {
            val picker = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
                model.completePick(request, uri)
            }
            LaunchedEffect(request) {
                try { picker.launch(arrayOf("*/*")) }
                catch (_: Exception) { model.failPick(request) }
            }
        }
    }
    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
        Text("选择发布描述、公开签名和从独立可信来源取得的渠道公钥策略。",
            style = MaterialTheme.typography.bodySmall)
        PluginSignatureMaterial.entries.forEach { kind ->
            OutlinedButton(onClick = {
                model.beginPick(kind)
            }, enabled = !model.busy && model.pending == null,
                modifier = Modifier.fillMaxWidth().heightIn(min = 48.dp).testTag("plugin-signature-pick-${kind.name}")) {
                Text("选择${kind.label}")
            }
            model.documents[kind]?.let { document ->
                SelectionContainer {
                    Text("已读取 ${document.size} 字节\nSHA-256：${document.sha256}",
                        Modifier.testTag("plugin-signature-material-${kind.name}"), style = MaterialTheme.typography.bodySmall,
                        fontFamily = FontFamily.Monospace)
                }
            }
        }
        Row(Modifier.fillMaxWidth().heightIn(min = 48.dp).testTag("plugin-signature-trust")
            .toggleable(model.trustedPolicy, enabled = PluginSignatureMaterial.POLICY in model.documents,
                role = Role.Checkbox, onValueChange = model::trustPolicy)) {
            Checkbox(model.trustedPolicy, onCheckedChange = null)
            Text("我已独立核对并信任此渠道策略快照", style = MaterialTheme.typography.bodyMedium)
        }
        Button(onClick = model::checkSignature, enabled = model.canVerify,
            modifier = Modifier.fillMaxWidth().heightIn(min = 48.dp).testTag("plugin-signature-verify")) {
            Text(if (model.busy) "正在读取或核对…" else "核对目录包签名")
        }
        if (model.busy) LinearProgressIndicator(Modifier.fillMaxWidth())
        model.receipt?.let { proof ->
            Text("签名已核对：${proof.publisher} · 渠道 ${proof.channel}（本次策略快照）",
                Modifier.testTag("plugin-signature-verified"), style = MaterialTheme.typography.bodyMedium)
            SelectionContainer {
                Text("密钥 SHA-256：${proof.keyID}\n策略 SHA-256：${proof.policySHA256}\n目录包 SHA-256：${proof.packageSHA256}",
                    style = MaterialTheme.typography.bodySmall, fontFamily = FontFamily.Monospace)
            }
        }
        model.issue?.let { Text(it, Modifier.testTag("plugin-signature-issue"), color = MaterialTheme.colorScheme.error) }
        Text("本次仅核对目录包和所选策略快照。未检查 ZIP，未安装。来源批准和 Session 授权需单独确认。",
            style = MaterialTheme.typography.bodySmall)
    }
}
