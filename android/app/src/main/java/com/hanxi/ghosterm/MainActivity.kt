package com.hanxi.ghosterm

import android.os.Bundle
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
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
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
fun GhostermApp(hosts: HostsViewModel = viewModel(), runtimeModel: RuntimeInfoViewModel = viewModel()) {
    val runtime by runtimeModel.state.collectAsState()
    val import = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri -> if (uri != null) hosts.readImport(uri) }
    BackHandler(hosts.page != Page.HOSTS) { hosts.back() }
    val colors = if (isSystemInDarkTheme()) darkColorScheme(primary = Color(0xFFBACDFF),
        background = Color(0xFF171B20), surface = Color(0xFF21262D), onSurface = Color(0xFFE4E8EE))
        else lightColorScheme(primary = Color(0xFF425F96), background = Color(0xFFF7F8FC), surface = Color(0xFFF7F8FC))
    MaterialTheme(colorScheme = colors) {
        Scaffold(topBar = {
            TopAppBar(title = { Text(when (hosts.page) {
                Page.HOSTS -> "主机"; Page.SETTINGS -> "设置"; Page.ABOUT -> "关于 Ghosterm"
                Page.CONFIG -> "SSH 配置"; Page.EDIT_HOST -> if (hosts.draft?.original == null) "添加主机" else "编辑主机"
            }) }, navigationIcon = {
                if (hosts.page != Page.HOSTS) TextButton(onClick = hosts::back, enabled = !hosts.busy) { Text("返回") }
            }, actions = {
                when (hosts.page) {
                    Page.HOSTS -> TextButton(onClick = hosts::settings, enabled = !hosts.busy) { Text("设置") }
                    Page.EDIT_HOST -> TextButton(onClick = hosts::saveHost, enabled = !hosts.busy) { Text("保存") }
                    Page.CONFIG -> TextButton(onClick = hosts::saveConfig, enabled = !hosts.busy && hosts.dirty) { Text("保存") }
                    else -> Unit
                }
            })
        }, bottomBar = {
            if (hosts.page == Page.HOSTS) Row(Modifier.fillMaxWidth().navigationBarsPadding().padding(16.dp),
                horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                OutlinedButton(onClick = hosts::notConnected, modifier = Modifier.weight(1f)) { Text("本地终端") }
                Button(onClick = { hosts.edit() }, enabled = !hosts.busy && hosts.snapshot != null,
                    modifier = Modifier.weight(1f)) { Text("添加主机") }
            }
        }) { padding ->
            Column(Modifier.fillMaxSize().padding(padding).consumeWindowInsets(padding).imePadding()) {
                if (hosts.busy) LinearProgressIndicator(Modifier.fillMaxWidth())
                hosts.error?.let { error ->
                    Card(Modifier.padding(horizontal = 16.dp, vertical = 8.dp), colors = CardDefaults.cardColors(containerColor = colors.errorContainer)) {
                        Column(Modifier.padding(12.dp)) {
                            Text(error, color = colors.onErrorContainer)
                            if (hosts.page == Page.HOSTS || hosts.page == Page.CONFIG)
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
                    Page.HOSTS -> HostsPage(hosts)
                    Page.EDIT_HOST -> EditHostPage(hosts)
                    Page.CONFIG -> Column(Modifier.padding(horizontal = 16.dp).fillMaxSize()) {
                        Text("此副本保存在应用内。Include 相对应用私有 HOME/.ssh；保存前强制备份。", style = MaterialTheme.typography.bodySmall)
                        OutlinedTextField(hosts.configText, { hosts.configText = it }, enabled = !hosts.busy,
                            modifier = Modifier.fillMaxWidth().weight(1f).padding(vertical = 12.dp),
                            label = { Text("配置内容") }, textStyle = LocalTextStyle.current.copy(fontFamily = FontFamily.Monospace))
                    }
                    Page.SETTINGS -> Column(Modifier.verticalScroll(rememberScrollState()).padding(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
                        Text("会话与连接资料", style = MaterialTheme.typography.titleMedium)
                        OutlinedButton(onClick = hosts::back, modifier = Modifier.fillMaxWidth()) { Text("管理主机") }
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
        if (hosts.imported != null) AlertDialog(onDismissRequest = hosts::cancelImport, title = { Text("替换应用内 SSH 配置？") },
            text = { Text("将先备份当前配置，再导入所选文档的副本。原文档不会修改。") },
            confirmButton = { TextButton(onClick = hosts::confirmImport, enabled = !hosts.busy) { Text("确认导入") } },
            dismissButton = { TextButton(onClick = hosts::cancelImport, enabled = !hosts.busy) { Text("取消") } })
    }
}

@Composable
private fun HostsPage(model: HostsViewModel) {
    LazyColumn(Modifier.fillMaxSize(), contentPadding = PaddingValues(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
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
                    TextButton(onClick = model::notConnected, modifier = Modifier.weight(1f)) {
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
    Column(Modifier.verticalScroll(rememberScrollState()).padding(16.dp), verticalArrangement = Arrangement.spacedBy(16.dp)) {
        Text("显示配置求值后的生效值。未修改的字段不会写回；同一 Host 块的多个别名共享修改。", style = MaterialTheme.typography.bodySmall)
        OutlinedTextField(draft.alias, { model.draft = draft.copy(alias = it) }, label = { Text("别名") }, singleLine = true,
            enabled = !model.busy && draft.original == null, modifier = Modifier.fillMaxWidth())
        OutlinedTextField(draft.hostname, { model.draft = draft.copy(hostname = it) }, label = { Text("主机地址") }, singleLine = true,
            enabled = !model.busy, modifier = Modifier.fillMaxWidth())
        OutlinedTextField(draft.user, { model.draft = draft.copy(user = it) }, label = { Text("远端用户名") }, singleLine = true,
            enabled = !model.busy, modifier = Modifier.fillMaxWidth())
        OutlinedTextField(draft.port, { model.draft = draft.copy(port = it) }, label = { Text("端口") }, singleLine = true,
            keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number), enabled = !model.busy, modifier = Modifier.fillMaxWidth())
        Text("身份与认证将在连接功能中接入。现有 IdentityFile 和其他指令保留。", style = MaterialTheme.typography.bodySmall)
    }
}
