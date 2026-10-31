package com.hanxi.ghosterm

import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.DialogProperties
import androidx.compose.ui.window.SecureFlagPolicy
import com.hanxi.ghosterm.bridge.SSHAuthPhase
import com.hanxi.ghosterm.data.IdentityReferences

/** Credentials remain in this non-saveable dialog, never in a route or saved-state bundle. */
@Composable
fun SSHConnectionDialog(model: SessionsViewModel) {
    // Registration survives cancellation and stays in the same position after Activity recreation.
    val importer = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument(),model::completeIdentityImport)
    val attempt = model.connecting ?: return
    val prompt = attempt.auth
    var username by remember(attempt.id) { mutableStateOf(attempt.username) }
    var secret by remember(attempt.id, prompt?.request) { mutableStateOf("") }
    var identityId by remember(attempt.id, prompt?.request) { mutableStateOf<String?>(null) }
    var otherIdentity by remember(attempt.id, prompt?.request) { mutableStateOf(false) }
    val candidates = remember(attempt.target.identityFiles, attempt.identities) {
        IdentityReferences.candidates(attempt.target.identityFiles, attempt.identities)
    }
    LaunchedEffect(attempt.id, prompt?.request, candidates.identities) {
        if (identityId == null && !otherIdentity) identityId = candidates.identities.firstOrNull()?.id
    }
    var passwordMode by remember(attempt.id, prompt?.request) { mutableStateOf(prompt?.methods?.and(1) == 0) }
    DisposableEffect(attempt.id, prompt?.request) { onDispose { secret = "" } }
    val selected = attempt.identities.firstOrNull { it.id == identityId }
    val needsUser = attempt.username.isBlank() && !attempt.failed
    val hostkey = prompt?.phase == SSHAuthPhase.HOSTKEY && !attempt.failed
    val credentials = prompt?.phase == SSHAuthPhase.CREDENTIALS && !attempt.failed
    val ready = prompt != null && prompt.request > 0 && !attempt.credentialBusy && !model.identityPickerPending
    AlertDialog(onDismissRequest = { model.cancelConnection(attempt.id) },
        properties = DialogProperties(securePolicy = SecureFlagPolicy.SecureOn),
        title = { Text(when { needsUser -> "远端用户名"; hostkey -> "确认主机密钥"; credentials -> "SSH 认证"; else -> "连接 ${attempt.target.alias}" }) },
        text = {
            Column(Modifier.fillMaxWidth().heightIn(max = 480.dp).verticalScroll(rememberScrollState()),
                verticalArrangement = Arrangement.spacedBy(12.dp)) {
                attempt.taskLabel?.let { Text(it, modifier = Modifier.testTag("ssh-task-role")) }
                Text("${attempt.target.alias} · ${attempt.target.host}:${attempt.target.port}")
                if (attempt.username.isNotBlank()) Text("用户：${attempt.username}")
                when {
                    needsUser -> {
                        Text("配置没有指定用户名。本次填写不会修改 SSH 配置。")
                        OutlinedTextField(username, { username = it }, singleLine = true, label = { Text("远端用户名") },
                            modifier = Modifier.testTag("ssh-username"))
                    }
                    attempt.failed -> Text(attempt.message, color = MaterialTheme.colorScheme.error, modifier = Modifier.testTag("ssh-failure"))
                    hostkey -> {
                        Text("首次连接此主机。请与服务器管理员核对以下指纹，确认后保存到主机信任记录。")
                        Text("算法：${prompt!!.keyType}")
                        SelectionContainer { Text(prompt.fingerprint, modifier = Modifier.testTag("ssh-fingerprint")) }
                    }
                    credentials -> {
                        Text(attempt.message)
                        val passwordAvailable = prompt!!.methods and 2 != 0
                        val keyAvailable = prompt.methods and 1 != 0
                        if (passwordAvailable && keyAvailable) Row {
                            TextButton(onClick = { passwordMode = true; secret = "" }) { Text("使用密码") }
                            TextButton(onClick = { passwordMode = false; secret = "" }) { Text("使用密钥") }
                        }
                        if (passwordMode && passwordAvailable) {
                            OutlinedTextField(secret, { secret = it }, label = { Text("登录密码") }, singleLine = true,
                                visualTransformation = PasswordVisualTransformation(), keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Password),
                                modifier = Modifier.testTag("ssh-secret"), enabled = ready)
                        } else if (keyAvailable) {
                            if (attempt.target.identityFiles.isNotEmpty()) {
                                Text("主机配置中的身份", style = MaterialTheme.typography.titleSmall)
                                candidates.issues.forEach { Text(it, style = MaterialTheme.typography.bodySmall) }
                                TextButton(onClick = { otherIdentity = !otherIdentity; identityId = null; secret = "" }, enabled = ready,
                                    modifier = Modifier.testTag("ssh-other-identity")) {
                                    Text(if (otherIdentity) "返回配置身份" else "选择其他身份（仅本次）")
                                }
                            }
                            if (attempt.identities.isEmpty()) Text("还没有导入的密钥。导入会保存到本机加密容器，原文档不修改。")
                            val choices = if (attempt.target.identityFiles.isEmpty() || otherIdentity) attempt.identities else candidates.identities
                            choices.forEach { identity ->
                                TextButton(onClick = { identityId = identity.id; secret = "" }, enabled = ready,
                                    modifier = Modifier.fillMaxWidth().testTag("ssh-key-${identity.id}")) {
                                    Text((if (selected?.id == identity.id) "✓ " else "") + identity.name)
                                }
                            }
                            OutlinedButton(onClick = { if (model.beginIdentityImport(attempt.id,prompt)) {
                                otherIdentity = true; identityId = null; secret = ""
                                try { importer.launch(arrayOf("*/*")) }
                                catch (_: Exception) { model.identityImportLaunchFailed() }
                            } },enabled = ready) { Text("导入密钥") }
                            if (selected?.needsPassphrase == true) OutlinedTextField(secret, { secret = it }, label = { Text("密钥口令") }, singleLine = true,
                                visualTransformation = PasswordVisualTransformation(), keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Password),
                                modifier = Modifier.testTag("ssh-secret"), enabled = ready)
                        } else Text("服务器未提供当前支持的密码或密钥认证方式。",color = MaterialTheme.colorScheme.error)
                        if (attempt.credentialBusy) LinearProgressIndicator(Modifier.fillMaxWidth())
                    }
                    else -> { Text(attempt.message); LinearProgressIndicator(Modifier.fillMaxWidth()) }
                }
                attempt.credentialError?.let { Text(it,color = MaterialTheme.colorScheme.error,modifier = Modifier.testTag("ssh-import-error")) }
            }
        },
        confirmButton = {
            when {
                needsUser -> TextButton(onClick = { model.setConnectionUsername(attempt.id,username) }) { Text("继续连接") }
                hostkey -> TextButton(onClick = { model.decide(attempt.id,prompt!!,true) },enabled = ready) { Text("信任并连接") }
                credentials -> TextButton(onClick = {
                    val bytes = secret.toByteArray(Charsets.UTF_8); secret = ""
                    if (passwordMode && prompt!!.methods and 2 != 0) model.password(attempt.id,prompt,bytes)
                    else selected?.let { model.useIdentity(attempt.id,prompt!!,it,bytes) } ?: bytes.fill(0)
                }, enabled = ready && ((passwordMode && prompt!!.methods and 2 != 0) || selected != null)) { Text("认证并连接") }
                attempt.failed -> TextButton(onClick = { model.cancelConnection(attempt.id) }) { Text("关闭") }
            }
        },
        dismissButton = {
            Row {
                if (hostkey) TextButton(onClick = { model.decide(attempt.id,prompt!!,false) },enabled = ready) { Text("拒绝") }
                if (!attempt.failed) TextButton(onClick = { model.cancelConnection(attempt.id) }) { Text("取消连接") }
            }
        })
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun SSHSessionChoices(model: SessionsViewModel) {
    val alias = model.choosingRemote ?: return
    ModalBottomSheet(onDismissRequest = model::dismissRemoteChoices) {
        Column(Modifier.fillMaxWidth().verticalScroll(rememberScrollState()).padding(16.dp)) {
            Text("选择 $alias 的会话",style = MaterialTheme.typography.titleLarge)
            model.sessions.filter { it.remoteAlias == alias && it.state.connected }.forEachIndexed { index, session ->
                TextButton(onClick = { model.show(session.id) },modifier = Modifier.fillMaxWidth().testTag("ssh-session-${session.id}")) {
                    Text("会话 ${index + 1}" + if (model.selected == session.id) " · 当前" else "")
                }
            }
            TextButton(onClick = { model.dismissRemoteChoices(); model.selectNewRemote() },modifier = Modifier.fillMaxWidth()) { Text("新建 SSH 会话") }
        }
    }
}
