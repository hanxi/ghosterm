package com.hanxi.ghosterm

import android.os.Bundle
import android.content.Intent
import android.app.Activity
import androidx.activity.ComponentActivity
import androidx.activity.compose.BackHandler
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.selection.toggleable
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import com.hanxi.ghosterm.domain.HostField
import com.hanxi.ghosterm.domain.RemoteFileIdentity
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import androidx.lifecycle.viewmodel.compose.viewModel

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        setContent { GhostermApp() }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun GhostermApp(hosts: HostsViewModel = viewModel(), runtimeModel: RuntimeInfoViewModel = viewModel(), sessions: SessionsViewModel = viewModel(), identities: IdentitiesViewModel = viewModel(), knownHosts: KnownHostsViewModel = viewModel(), files: FilesViewModel = viewModel(), fileTasks: FileTasksViewModel = viewModel()) {
    val runtime by runtimeModel.state.collectAsState()
    val import = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri -> if (uri != null) hosts.readImport(uri) }
    val identityImport = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument(),identities::completeImport)
    val treePicker = rememberLauncherForActivityResult(ActivityResultContracts.StartActivityForResult()) { result ->
        val data=result.data
        files.picked(if(result.resultCode==Activity.RESULT_OK) data?.data?.toString() else null,data?.flags ?: 0)
    }
    fun pickFolder() {
        if(files.beginPicker()) try { treePicker.launch(Intent(Intent.ACTION_OPEN_DOCUMENT_TREE).addFlags(
            Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_WRITE_URI_PERMISSION or
                Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION or Intent.FLAG_GRANT_PREFIX_URI_PERMISSION)) }
        catch (_: Exception) { files.pickerFailed() }
    }
    fun back() {
        if (hosts.page == Page.KNOWN_HOSTS) {
            if (knownHosts.backToList()) return
            knownHosts.leave()
        }
        if (hosts.page == Page.IDENTITIES) {
            if (identities.backToList()) return
            identities.leave()
        }
        hosts.back()
    }
    BackHandler(!sessions.visible && !files.visible && !fileTasks.visible && hosts.page != Page.HOSTS) { back() }
    val colors = if (isSystemInDarkTheme()) darkColorScheme(primary = Color(0xFFBACDFF),
        background = Color(0xFF171B20), surface = Color(0xFF21262D), onSurface = Color(0xFFE4E8EE))
        else lightColorScheme(primary = Color(0xFF425F96), background = Color(0xFFF7F8FC), surface = Color(0xFFF7F8FC))
    MaterialTheme(colorScheme = colors) {
        SSHConnectionDialog(sessions)
        SSHSessionChoices(sessions)
        if(fileTasks.visible) { FileTasksScreen(fileTasks);return@MaterialTheme }
        if (files.visible) {
            FilesScreen(files, ::pickFolder, hosts.snapshot?.hosts?.map { it.alias } ?: emptyList(),hosts.error,
                onPaste={ files.clipboard?.let { snapshot ->
                    fileTasks.start(snapshot,files.location,
                        if(files.location.endpoint.remote!=null) files.location.directoryId else files.crumbs.joinToString(" / ") { it.label },
                        { request -> sessions.fileTaskConnections(request) { fileTasks.cancelRequest(request.id) } },
                        { result -> files.copyCompleted(result) })
                } },showTasks=fileTasks::show) { alias ->
                val host=hosts.snapshot?.hosts?.firstOrNull { it.alias==alias }
                if(host==null) files.remoteConnectionError("主机配置已不可用，请返回首页重新载入")
                else {
                    val token=files.beginRemoteConnection()
                    hosts.checkConnection(host) { target ->
                        if(files.acceptsRemoteConnection(token)) {
                            files.dismissLocations()
                            sessions.connectFiles(target) { connection,user ->
                                files.remoteConnected(RemoteFileIdentity(target.alias,target.host,user,target.port),connection,token)
                            }
                        }
                    }
                }
            }
            return@MaterialTheme
        }
        if (sessions.visible && sessions.active != null) {
            TerminalScreen(sessions, files::enter)
            return@MaterialTheme
        }
        Scaffold(topBar = {
            TopAppBar(title = { Text(when (hosts.page) {
                Page.HOSTS -> "主机"; Page.SETTINGS -> "设置"; Page.ABOUT -> "关于 Ghosterm"
                Page.IDENTITIES -> "SSH 身份"
                Page.KNOWN_HOSTS -> "主机验证记录"
                Page.CONFIG -> "SSH 配置"; Page.EDIT_HOST -> if (hosts.draft?.original == null) "添加主机" else "编辑主机"
            }) }, navigationIcon = {
                if (hosts.page != Page.HOSTS) TextButton(onClick = { back() }, enabled = !hosts.busy) { Text("返回") }
            }, actions = {
                when (hosts.page) {
                    Page.HOSTS -> {
                        TextButton(onClick=fileTasks::show) { Text("任务") }
                        TextButton(onClick = hosts::settings, enabled = !hosts.busy) { Text("设置") }
                    }
                    Page.EDIT_HOST -> TextButton(onClick = hosts::saveHost, enabled = !hosts.busy && hosts.canEditDraft) { Text("保存") }
                    Page.CONFIG -> TextButton(onClick = hosts::saveConfig, enabled = !hosts.busy && hosts.snapshot != null && hosts.dirty) { Text("保存") }
                    else -> Unit
                }
            })
        }, bottomBar = {
            if (hosts.page == Page.HOSTS) Row(Modifier.fillMaxWidth().navigationBarsPadding().padding(16.dp),
                horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                OutlinedButton(onClick = sessions::local, enabled = !sessions.creating, modifier = Modifier.weight(1f)) { Text(if (sessions.creating) "正在创建…" else if (sessions.sessions.none { it.remoteAlias == null }) "本地终端" else "继续本地终端") }
                Button(onClick = { hosts.edit() }, enabled = !hosts.busy && hosts.snapshot != null,
                    modifier = Modifier.weight(1f)) { Text("添加主机") }
            }
        }) { padding ->
            Column(Modifier.fillMaxSize().padding(padding).consumeWindowInsets(padding).imePadding()) {
                sessions.error?.let { Text(it, Modifier.padding(16.dp), color = colors.error) }
                if (hosts.busy) LinearProgressIndicator(Modifier.fillMaxWidth())
                hosts.error?.let { error ->
                    if (hosts.imported == null)
                    Card(Modifier.padding(horizontal = 16.dp, vertical = 8.dp), colors = CardDefaults.cardColors(containerColor = colors.errorContainer)) {
                        Column(Modifier.padding(12.dp)) {
                            Text(error, color = colors.onErrorContainer)
                            if (hosts.page == Page.HOSTS || hosts.page == Page.CONFIG || hosts.page == Page.EDIT_HOST)
                                TextButton(onClick = hosts::reload, enabled = !hosts.busy) { Text("重新载入") }
                        }
                    }
                }
                hosts.notice?.let { message ->
                    Row(Modifier.fillMaxWidth().padding(horizontal = 16.dp), verticalAlignment = Alignment.CenterVertically) {
                        Text(message, Modifier.weight(1f), style = MaterialTheme.typography.bodySmall)
                        TextButton(onClick = hosts::dismissNotice) { Text("知道了") }
                    }
                }
                when (hosts.page) {
                    Page.HOSTS -> HostsPage(hosts, sessions, files::enter)
                    Page.EDIT_HOST -> EditHostPage(hosts)
                    Page.KNOWN_HOSTS -> KnownHostsScreen(knownHosts)
                    Page.IDENTITIES -> IdentitiesScreen(identities) {
                        if (identities.beginImport()) try { identityImport.launch(arrayOf("*/*")) }
                        catch (_: Exception) { identities.importLaunchFailed() }
                    }
                    Page.CONFIG -> Column(Modifier.padding(horizontal = 16.dp).fillMaxSize()) {
                        Text("此副本保存在应用内。Include 相对应用私有 HOME/.ssh；保存前强制备份。", style = MaterialTheme.typography.bodySmall)
                        OutlinedTextField(hosts.configText, { hosts.configText = it }, enabled = !hosts.busy,
                            modifier = Modifier.fillMaxWidth().weight(1f).padding(vertical = 12.dp),
                            label = { Text("配置内容") }, textStyle = LocalTextStyle.current.copy(fontFamily = FontFamily.Monospace))
                    }
                    Page.SETTINGS -> Column(Modifier.verticalScroll(rememberScrollState()).padding(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
                        Text("会话与连接资料", style = MaterialTheme.typography.titleMedium)
                        OutlinedButton(onClick = hosts::back, modifier = Modifier.fillMaxWidth()) { Text("管理主机") }
                        OutlinedButton(onClick = { hosts.identities(); identities.enter() },enabled = !hosts.busy, modifier = Modifier.fillMaxWidth()) { Text("SSH 身份") }
                        OutlinedButton(onClick = { hosts.knownHosts(); knownHosts.enter() }, enabled = !hosts.busy,
                            modifier = Modifier.fillMaxWidth()) { Text("主机验证记录") }
                        OutlinedButton(onClick = hosts::config, enabled = hosts.snapshot != null, modifier = Modifier.fillMaxWidth()) { Text("查看与编辑 SSH 配置") }
                        OutlinedButton(onClick = { import.launch(arrayOf("*/*")) }, enabled = !hosts.busy && hosts.snapshot != null, modifier = Modifier.fillMaxWidth()) { Text("导入 SSH 配置") }
                        Text("导入会替换应用内副本，原文档不修改。Include 所需文件必须先可访问。", style = MaterialTheme.typography.bodySmall)
                        TextButton(onClick = hosts::about) { Text("关于 Ghosterm") }
                    }
                    Page.ABOUT -> Column(Modifier.padding(24.dp), verticalArrangement = Arrangement.spacedBy(16.dp)) {
                        Text("版本 ${BuildConfig.VERSION_NAME}")
                        when (val info = runtime) {
                            RuntimeInfo.Loading -> Text("正在加载终端组件…")
                            RuntimeInfo.Failed -> Text("终端组件加载失败", color = colors.error)
                            is RuntimeInfo.Ready -> { Text("Core ${info.core}"); Text("libghostty-vt ${info.terminal}") }
                        }
                        Text("原生 Android 客户端 · MIT License")
                    }
                }
            }
        }
        if (hosts.discard) AlertDialog(onDismissRequest = hosts::keepEditing, title = { Text("放弃未保存的修改？") },
            text = { Text("已保存的主机配置不会改变。") },
            confirmButton = { TextButton(onClick = hosts::confirmDiscard) { Text("放弃修改") } },
            dismissButton = { TextButton(onClick = hosts::keepEditing) { Text("继续编辑") } })
        hosts.imported?.let { document -> AlertDialog(onDismissRequest = hosts::cancelImport, title = { Text("替换应用内 SSH 配置？") },
            text = {
                Column(Modifier.heightIn(max = 440.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
                    hosts.error?.let { Text(it, Modifier.testTag("config-import-error"), color = colors.error) }
                    if (hosts.snapshot == null) {
                        Text("当前配置已变化或不可读取。重新载入后，请核对并再次确认导入。")
                        TextButton(onClick = hosts::reload, enabled = !hosts.busy) { Text("重新载入当前配置") }
                    }
                    Column(Modifier.weight(1f, fill = false).verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(12.dp)) {
                        Text(document.name, style = MaterialTheme.typography.titleSmall)
                        Text("${document.bytes.size} 字节 · UTF-8")
                        Text("将先备份当前配置，再导入所选文档的副本。原文档不会修改。Include 相对应用私有 HOME/.ssh，不是所选文档所在目录。")
                        if (document.truncated) Text("预览已截断；确认后导入完整文件。", Modifier.testTag("config-import-truncated"))
                        SelectionContainer {
                            Text(document.preview.ifEmpty { "（空配置）" }, Modifier.testTag("config-import-preview"), fontFamily = FontFamily.Monospace)
                        }
                    }
                }
            },
            confirmButton = { TextButton(onClick = hosts::confirmImport, enabled = !hosts.busy && hosts.snapshot != null) { Text("确认导入") } },
            dismissButton = { TextButton(onClick = hosts::cancelImport, enabled = !hosts.busy) { Text("取消") } })
        }
    }
}

@Composable
private fun HostsPage(model: HostsViewModel, sessions: SessionsViewModel, openFiles: () -> Unit) {
    LazyColumn(Modifier.fillMaxSize(), contentPadding = PaddingValues(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
        if (sessions.selectingNewRemote) item {
            Row { Text("选择要新建会话的主机",Modifier.weight(1f)); TextButton(onClick = sessions::cancelNewRemote) { Text("取消") } }
        }
        item { OutlinedButton(onClick=openFiles,modifier=Modifier.fillMaxWidth()) { Text("本机文件") } }
        item { OutlinedTextField(model.query, { model.query = it }, modifier = Modifier.fillMaxWidth(),
            label = { Text("搜索名称或地址") }, singleLine = true) }
        if (model.snapshot != null && model.filteredHosts.isEmpty()) item {
            Column(Modifier.fillMaxWidth().padding(vertical = 56.dp), horizontalAlignment = Alignment.CenterHorizontally) {
                Text(if (model.query.isBlank()) "还没有主机" else "没有匹配的主机", style = MaterialTheme.typography.titleLarge)
                Text(if (model.query.isBlank()) "添加主机，或在设置中导入 SSH 配置。" else "试试其他名称或地址。", Modifier.padding(top = 12.dp))
            }
        }
        items(model.filteredHosts, key = { it.alias }) { host ->
            Card(Modifier.fillMaxWidth()) {
                Row(Modifier.fillMaxWidth().padding(12.dp), verticalAlignment = Alignment.CenterVertically) {
                    TextButton(onClick = { model.checkConnection(host, sessions::connect) }, enabled = !model.busy, modifier = Modifier.weight(1f)) {
                        Column(Modifier.fillMaxWidth()) { Text(host.alias, style = MaterialTheme.typography.titleMedium); Text(host.address, style = MaterialTheme.typography.bodySmall) }
                    }
                    TextButton(onClick = { model.edit(host) }, enabled = !model.busy) { Text("编辑") }
                }
            }
        }
    }
}

@Composable
private fun EditHostPage(model: HostsViewModel) {
    val draft = model.draft ?: return
    val edit = draft.edit
    val source = draft.original?.source
    val enabled = !model.busy && model.canEditDraft
    Column(Modifier.verticalScroll(rememberScrollState()).padding(16.dp), verticalArrangement = Arrangement.spacedBy(16.dp)) {
        if (source != null) {
            Text(source.header.trim(), style = MaterialTheme.typography.titleMedium)
            Text("${source.path.ifEmpty { "应用内 SSH 配置" }} · 第 ${source.line} 行", style = MaterialTheme.typography.bodySmall)
            if (source.patternCount > 1) Text("此 Host 块包含多个别名或规则；修改会同时影响上方列出的全部名称。", color = MaterialTheme.colorScheme.primary)
        }
        if (!model.canEditDraft) {
            Text(source?.editRestriction ?: "无法确定主机配置归属，请重新载入", color = MaterialTheme.colorScheme.error)
            OutlinedButton(onClick = model::config, enabled = !model.busy && model.snapshot != null) { Text("查看与编辑 SSH 配置") }
        }
        Text("开启“使用继承值”时由配置规则或默认值决定；关闭后为此主机设置。未改字段保留原文。", style = MaterialTheme.typography.bodySmall)
        OutlinedTextField(edit.alias, { model.draft = draft.copy(edit = edit.copy(alias = it)) }, label = { Text("别名") }, singleLine = true,
            enabled = enabled, modifier = Modifier.fillMaxWidth(),
            supportingText = { Text("改名只修改此别名，同块其他名称保留；新名称可能匹配不同的继承规则。") })
        for (field in HostField.entries) {
            val provenance = field.source(draft.original)
            val inherits = field in edit.inherited
            val pendingInheritance = inherits && (draft.original == null || provenance?.explicitValue != null)
            Column {
                OutlinedTextField(if (pendingInheritance) "" else edit.value(field),
                    { model.draft = draft.copy(edit = edit.withValue(field, it)) },
                    label = { Text(field.label) }, singleLine = true,
                    placeholder = { if (pendingInheritance) Text("保存后重新求值") },
                    supportingText = {
                        Column {
                            provenance?.let { Text(it.description) }
                            if (provenance?.explicitValue != null && !provenance.effectiveIsExplicit)
                                Text("当前生效：${field.effective(draft.original!!).ifEmpty { "未设置" }}")
                            if (pendingInheritance) Text("待保存：使用其他规则或默认值，移除此 Host 块的设置。")
                        }
                    },
                    keyboardOptions = KeyboardOptions(keyboardType = if (field == HostField.PORT) KeyboardType.Number else KeyboardType.Text),
                    enabled = enabled && !inherits, modifier = Modifier.fillMaxWidth())
                Row(Modifier.fillMaxWidth().heightIn(min = 48.dp)
                    .toggleable(value = inherits, enabled = enabled, role = Role.Switch) {
                        model.draft = draft.copy(edit = edit.withInheritance(field, it))
                    }.semantics { contentDescription = "${field.label}使用继承值" },
                    verticalAlignment = Alignment.CenterVertically) {
                    Text("使用继承值", Modifier.weight(1f))
                    Switch(checked = inherits, onCheckedChange = null, enabled = enabled)
                }
            }
        }
        HostIdentityEditor(model)
    }
}
