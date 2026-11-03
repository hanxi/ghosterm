package com.hanxi.ghosterm

import android.os.Bundle
import android.content.Intent
import android.app.Activity
import android.text.InputType
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.InputConnection
import androidx.core.view.WindowCompat
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalFocusManager
import androidx.compose.ui.platform.LocalSoftwareKeyboardController
import androidx.compose.ui.platform.InterceptPlatformTextInput
import androidx.compose.ui.platform.PlatformTextInputInterceptor
import androidx.compose.ui.platform.PlatformTextInputMethodRequest
import com.hanxi.ghosterm.domain.AppAppearance
import androidx.activity.ComponentActivity
import androidx.activity.compose.BackHandler
import androidx.activity.compose.LocalActivity
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
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import androidx.lifecycle.viewmodel.compose.viewModel

class MainActivity : ComponentActivity() {
    private val fileTaskPage get() = androidx.lifecycle.ViewModelProvider(this)[FileTasksViewModel::class.java]
    override fun onResume() { super.onResume();fileTaskPage.resumed() }
    override fun onPause() { fileTaskPage.paused();super.onPause() }
    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if(hasFocus) fileTaskPage.focused()
    }
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        if(savedInstanceState==null && intent.action==FileTransferService.OPEN_TASKS) fileTaskPage.show()
        setContent { GhostermApp() }
    }
    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent);setIntent(intent)
        if(intent.action==FileTransferService.OPEN_TASKS) fileTaskPage.show()
    }
}

@OptIn(ExperimentalMaterial3Api::class, androidx.compose.ui.ExperimentalComposeUiApi::class)
@Composable
fun GhostermApp(hosts: HostsViewModel = viewModel(), runtimeModel: RuntimeInfoViewModel = viewModel(), sessions: SessionsViewModel = viewModel(), identities: IdentitiesViewModel = viewModel(), knownHosts: KnownHostsViewModel = viewModel(), files: FilesViewModel = viewModel(), fileTasks: FileTasksViewModel = viewModel(), settings: SettingsViewModel = viewModel(), about: AboutViewModel = viewModel(), tmux: TmuxViewModel = viewModel()) {
    val focus = LocalFocusManager.current
    val keyboard = LocalSoftwareKeyboardController.current
    val applied by settings.settings.state.collectAsState()
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
        if (hosts.page == Page.ABOUT && about.back()) return
        if (hosts.page == Page.SETTINGS && settings.back()) return
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
    fun connectFileTarget(target: com.hanxi.ghosterm.bridge.PreparedSSH, token: Long) {
        if(!files.acceptsRemoteConnection(token)) return
        files.dismissLocations()
        sessions.connectFiles(target) { connection,user ->
            files.remoteConnected(RemoteFileIdentity(target.alias,target.host,user,target.port),connection,token)
        }
    }
    fun openTerminalFiles() {
        val session=sessions.active ?: return
        val target=session.remoteTarget
        if(target==null && session.remoteAlias==null) { files.enterLocal();return }
        files.enter()
        if(target==null) files.remoteConnectionError("无法确定此会话的远端主机，请从文件位置重新连接")
        else connectFileTarget(target,files.beginRemoteConnection())
    }
    BackHandler(!sessions.visible && !files.visible && !fileTasks.visible && hosts.page != Page.HOSTS) { back() }
    LaunchedEffect(settings.leaveRequested) { if (settings.consumeLeave() && hosts.page == Page.SETTINGS) hosts.back() }
    val dark = when (applied.committed.appearance) {
        AppAppearance.SYSTEM -> isSystemInDarkTheme(); AppAppearance.LIGHT -> false; AppAppearance.DARK -> true
    }
    val activity = LocalActivity.current
    SideEffect { activity?.let { WindowCompat.getInsetsController(it.window, it.window.decorView).apply {
        isAppearanceLightStatusBars = !dark; isAppearanceLightNavigationBars = !dark
    } } }
    val colors = if (dark) darkColorScheme(primary = Color(0xFFBACDFF),
        background = Color(0xFF171B20), surface = Color(0xFF21262D), onSurface = Color(0xFFE4E8EE))
        else lightColorScheme(primary = Color(0xFF425F96), background = Color(0xFFF7F8FC), surface = Color(0xFFF7F8FC))
    MaterialTheme(colorScheme = colors) {
        SSHConnectionDialog(sessions)
        SSHConnectionDialog(fileTasks.authentication,fileTasks.presenterId,
            visible=fileTasks.presentsAuthentication && sessions.connecting==null)
        LaunchedEffect(sessions.connecting?.id,tmux.visible) { if(tmux.visible) tmux.authenticationDismissed() }
        SSHSessionChoices(sessions)
        val identityWarningOwner = when {
            sessions.identityMemoryWarning != null -> sessions
            fileTasks.authentication.identityMemoryWarning != null -> fileTasks.authentication
            else -> null
        }
        identityWarningOwner?.let { owner ->
            AlertDialog(onDismissRequest = owner::dismissIdentityMemoryWarning,
                title = { Text("密钥未能记住") }, text = { Text(owner.identityMemoryWarning.orEmpty()) },
                confirmButton = { TextButton(onClick = owner::dismissIdentityMemoryWarning) { Text("知道了") } })
        }
        if(fileTasks.visible) { FileTasksScreen(fileTasks);return@MaterialTheme }
        if (files.visible) {
            FilesScreen(files, ::pickFolder, hosts.snapshot?.hosts?.map { it.alias } ?: emptyList(),hosts.error,
                onPaste={ if(!files.awaitingRemote) files.clipboard?.let { snapshot ->
                    fileTasks.startUserTask(snapshot,files.location,
                        if(files.location.endpoint.remote!=null) files.location.directoryId else files.crumbs.joinToString(" / ") { it.label })
                } },showTasks=fileTasks::show,taskBar={ FileTaskDock(fileTasks) }) { alias ->
                val host=hosts.snapshot?.hosts?.firstOrNull { it.alias==alias }
                if(host==null) files.remoteConnectionError("主机配置已不可用，请返回首页重新载入")
                else {
                    val token=files.beginRemoteConnection()
                    hosts.checkConnection(host) { target ->
                        connectFileTarget(target,token)
                    }
                }
            }
            return@MaterialTheme
        }
        if (sessions.visible && sessions.active != null) {
            TerminalScreen(sessions, ::openTerminalFiles)
            return@MaterialTheme
        }
        if(tmux.visible) { TmuxScreen(tmux);return@MaterialTheme }
        Scaffold(topBar = {
            TopAppBar(title = { Text(when (hosts.page) {
                Page.HOSTS -> "主机"; Page.SETTINGS -> settings.section?.title ?: "设置"; Page.ABOUT -> about.selected?.title ?: "关于 Ghosterm"
                Page.IDENTITIES -> "SSH 身份"
                Page.KNOWN_HOSTS -> "主机验证记录"
                Page.CONFIG -> "SSH 配置"; Page.EDIT_HOST -> if (hosts.draft?.original == null) "添加主机" else "编辑主机"
            }, maxLines = 1, overflow = TextOverflow.Ellipsis) }, navigationIcon = {
                if (hosts.page != Page.HOSTS) TextButton(onClick = { back() }, enabled = !hosts.busy) { Text("返回") }
            }, actions = {
                when (hosts.page) {
                    Page.HOSTS -> {
                        TextButton(onClick = hosts::settings, enabled = !hosts.busy) { Text("设置") }
                    }
                    Page.EDIT_HOST -> TextButton(onClick = { focus.clearFocus(); keyboard?.hide(); hosts.saveHost() }, enabled = !hosts.busy && hosts.canEditDraft) { Text("保存") }
                    Page.CONFIG -> TextButton(onClick = hosts::saveConfig, enabled = !hosts.busy && hosts.snapshot != null && hosts.dirty) { Text("保存") }
                    else -> Unit
                }
            })
        }, bottomBar = {
            if (hosts.page == Page.HOSTS) Column(Modifier.fillMaxWidth().navigationBarsPadding()) {
                FileTaskDock(fileTasks)
                Row(Modifier.fillMaxWidth().padding(horizontal=16.dp,vertical=8.dp),
                    horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                    OutlinedButton(onClick = sessions::local, enabled = !sessions.creating, modifier = Modifier.weight(1f)) { Text(if (sessions.creating) "正在创建…" else if (sessions.sessions.none { it.remoteAlias == null }) "本地终端" else "继续本地终端") }
                    Button(onClick = { hosts.edit() }, enabled = !hosts.busy && hosts.snapshot != null,
                        modifier = Modifier.weight(1f)) { Text("添加主机") }
                }
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
                    Page.HOSTS -> HostsPage(hosts, sessions, files::enterLocal) { host ->
                        hosts.checkConnection(host) { request -> tmux.open(request,sessions) }
                    }
                    Page.EDIT_HOST -> EditHostPage(hosts)
                    Page.KNOWN_HOSTS -> KnownHostsScreen(knownHosts)
                    Page.IDENTITIES -> IdentitiesScreen(identities) {
                        if (identities.beginImport()) try { identityImport.launch(arrayOf("*/*")) }
                        catch (_: Exception) { identities.importLaunchFailed() }
                    }
                    Page.CONFIG -> Column(Modifier.padding(horizontal = 16.dp).fillMaxSize()) {
                        Text("此副本保存在应用内。Include 相对应用私有 HOME/.ssh；保存前强制备份。", style = MaterialTheme.typography.bodySmall)
                        InterceptPlatformTextInput(interceptor = remember {
                            PlatformTextInputInterceptor { request, session ->
                                session.startInputMethod(object : PlatformTextInputMethodRequest {
                                    override fun createInputConnection(outAttributes: EditorInfo): InputConnection {
                                        val connection = request.createInputConnection(outAttributes)
                                        // Keep the visible multiline editor; ask the IME for literal input.
                                        outAttributes.inputType = (outAttributes.inputType and
                                            (InputType.TYPE_MASK_VARIATION or InputType.TYPE_TEXT_FLAG_AUTO_CORRECT).inv()) or
                                            InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD or InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS
                                        return connection
                                    }
                                })
                            }
                        }) {
                            OutlinedTextField(hosts.configText, { hosts.configText = it }, enabled = !hosts.busy,
                                modifier = Modifier.fillMaxWidth().weight(1f).padding(vertical = 12.dp),
                                keyboardOptions = KeyboardOptions(autoCorrectEnabled = false),
                                label = { Text("配置内容") }, textStyle = LocalTextStyle.current.copy(fontFamily = FontFamily.Monospace))
                        }
                    }
                    Page.SETTINGS -> SettingsContent(settings, hosts.snapshot != null, hosts.busy,
                        onHosts = hosts::back, onConfig = hosts::config,
                        onImport = { import.launch(arrayOf("*/*")) },
                        onIdentities = { hosts.identities(); identities.enter() },
                        onKnownHosts = { hosts.knownHosts(); knownHosts.enter() }, onAbout = hosts::about)
                    Page.ABOUT -> AboutContent(about, runtime)
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
private fun HostsPage(model: HostsViewModel, sessions: SessionsViewModel, openFiles: () -> Unit,
    background: (com.hanxi.ghosterm.domain.Host)->Unit) {
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
                TextButton(onClick={ background(host) },enabled=!model.busy && sessions.connecting==null,
                    modifier=Modifier.padding(horizontal=12.dp).testTag("host-background-"+host.alias)) { Text("后台会话") }
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
            keyboardOptions = KeyboardOptions(autoCorrectEnabled = false),
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
                    keyboardOptions = KeyboardOptions(autoCorrectEnabled = false, keyboardType = when (field) {
                        HostField.PORT -> KeyboardType.Number
                        HostField.HOSTNAME -> KeyboardType.Uri
                        else -> KeyboardType.Text
                    }),
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
