package com.hanxi.ghosterm

import androidx.activity.compose.BackHandler
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.compose.ui.text.style.TextOverflow
import com.hanxi.ghosterm.terminal.TerminalView

@OptIn(ExperimentalMaterial3Api::class, ExperimentalLayoutApi::class)
@Composable
fun TerminalScreen(model: SessionsViewModel, openFiles: () -> Unit) {
    val session = model.active ?: return
    val controls = model.terminalControls
    var terminal by remember(session.id) { mutableStateOf<TerminalView?>(null) }
    var ctrl by remember(session.id) { mutableIntStateOf(0) }
    var menu by remember { mutableStateOf(false) }
    var menuCompact by remember { mutableStateOf(false) }
    var list by remember { mutableStateOf(false) }
    var confirmClear by remember(session.id) { mutableStateOf(false) }
    var confirmClose by remember(session.id) { mutableStateOf(false) }
    val imeVisible = WindowInsets.ime.getBottom(LocalDensity.current) > 0
    val back: () -> Unit = { if (imeVisible) terminal?.hideKeyboard() else if (terminal?.dismissSelection() != true) model.home() }
    BackHandler(onBack = back)
    BoxWithConstraints(Modifier.fillMaxSize().windowInsetsPadding(
        WindowInsets.systemBars.union(WindowInsets.displayCutout).union(WindowInsets.ime))) {
    val density = LocalDensity.current
    val compact = maxHeight < 224.dp || (menu && menuCompact)
    val metrics = LocalView.current.resources.displayMetrics
    val linePixels = kotlin.math.ceil(android.util.TypedValue.applyDimension(
        android.util.TypedValue.COMPLEX_UNIT_SP, model.terminalStyle.fontSize, metrics) *
        model.terminalStyle.lineHeight).toInt()
    // In very short landscape windows, place controls beside the terminal so text
    // keeps the full available height instead of losing another 52 dp to chrome.
    val sideControls = compact && maxWidth >= 480.dp &&
        maxHeight < 52.dp + with(density) { (linePixels * 2).toDp() }
    val sideWidth = maxWidth / 2
    val actions: @Composable () -> Unit = {
        DropdownMenu(expanded = menu, onDismissRequest = { menu = false }) {
            if (menuCompact) {
                DropdownMenuItem(text = { Text("会话") }, onClick = { menu = false; terminal?.hideKeyboard(); list = true }, modifier = Modifier.testTag("terminal-menu-sessions"))
                DropdownMenuItem(text = { Text("文件") }, onClick = { menu = false; terminal?.hideKeyboard(); openFiles() }, modifier = Modifier.testTag("terminal-menu-files"))
            }
            DropdownMenuItem(text = { Text("清除本地显示") }, onClick = { menu = false; confirmClear = true })
            DropdownMenuItem(text = { Text("关闭会话") }, onClick = { menu = false; confirmClose = true })
        }
    }
    val inputBar: @Composable () -> Unit = {
        CompactTerminalBar(controls, session.state.connected, !session.state.terminal, ctrl, imeVisible,
            onBack = back, onCtrl = { terminal?.toggleCtrl() },
            onKey = { terminal?.sendKey(it) }, onText = { terminal?.commitText(it) },
            onKeyboard = { if (imeVisible) terminal?.hideKeyboard() else terminal?.showKeyboard() },
            onMenu = { menuCompact = true; menu = true }, menu = actions, compactNavigation = compact)
    }
    Scaffold(topBar = {
        if (!compact)
        TopAppBar(title = {
            Row(Modifier.fillMaxWidth().heightIn(min = 48.dp).testTag("terminal-sessions")
                .semantics { contentDescription = "切换会话，当前 ${session.title}，${model.sessions.size} 个会话" }
                .clickable(role = Role.Button) { terminal?.hideKeyboard(); list = true },
                verticalAlignment = Alignment.CenterVertically) {
                Column(Modifier.weight(1f)) {
                    Text(session.title, style = MaterialTheme.typography.titleMedium, maxLines = 1, overflow = TextOverflow.Ellipsis)
                    Text("${session.state.description} · ${model.sessions.size} 个会话",
                        style = MaterialTheme.typography.labelSmall, color = MaterialTheme.colorScheme.onSurfaceVariant,
                        maxLines = 1, overflow = TextOverflow.Ellipsis)
                }
                Icon(painterResource(R.drawable.ic_terminal_expand), contentDescription = null,
                    modifier = Modifier.size(18.dp))
            }
        }, navigationIcon = {
            IconButton(onClick = back, modifier = Modifier.testTag("terminal-back")) {
                Icon(painterResource(R.drawable.ic_terminal_back), contentDescription = "返回")
            }
        }, actions = {
            IconButton(onClick = { terminal?.hideKeyboard(); openFiles() }, modifier = Modifier.testTag("terminal-files")) {
                Icon(painterResource(R.drawable.ic_terminal_folder), contentDescription = "当前会话的文件")
            }
            Box {
                IconButton(onClick = { menuCompact = false; menu = true }, modifier = Modifier.testTag("terminal-menu")) {
                    Icon(painterResource(R.drawable.ic_terminal_more), contentDescription = "更多")
                }
                actions()
            }
        })
    }) { padding ->
        Column(Modifier.fillMaxSize().padding(padding).consumeWindowInsets(padding).imePadding()) {
            model.error?.takeUnless { compact }?.let { message ->
                Row(Modifier.fillMaxWidth().padding(horizontal = 12.dp), verticalAlignment = Alignment.CenterVertically) {
                    Text(message, Modifier.weight(1f), color = MaterialTheme.colorScheme.error)
                    TextButton(onClick = model::dismissError) { Text("知道了") }
                }
            }
            Row(Modifier.fillMaxWidth().weight(1f)) {
            Box(Modifier.weight(1f).fillMaxHeight()) {
            key(session.id) {
                AndroidView(factory = { context -> TerminalView(context).also {
                    it.updateStyle(model.terminalStyle)
                    it.applyAuxiliaryPolicy(controls.auxiliaryKeys, controls.lockedCtrl)
                    it.requestKeepAwake(controls.keepAwake)
                    it.surface = session.surface; it.onError = model::reportError
                    it.onModifiersChanged = { value -> ctrl = value }; terminal = it
                    it.requestFocus()
                } }, update = { view ->
                    view.updateStyle(model.terminalStyle)
                    view.applyAuxiliaryPolicy(controls.auxiliaryKeys, controls.lockedCtrl)
                    view.requestKeepAwake(controls.keepAwake)
                }, modifier = Modifier.fillMaxSize().testTag("terminal-surface"))
            }
            if (compact) model.error?.let { message ->
                Surface(color = MaterialTheme.colorScheme.errorContainer) {
                    Row(Modifier.fillMaxWidth().height(48.dp), verticalAlignment = Alignment.CenterVertically) {
                        Text(message, Modifier.weight(1f).padding(start = 8.dp), maxLines = 1, overflow = TextOverflow.Ellipsis)
                        TextButton(onClick = model::dismissError, contentPadding = PaddingValues(horizontal = 8.dp)) { Text("知道了", maxLines = 1) }
                    }
                }
            }
            }
            if (sideControls) Box(Modifier.width(sideWidth).fillMaxHeight(), contentAlignment = Alignment.Center) {
                inputBar()
            }
            }
            if (!sideControls) inputBar()
        }
    }
    LaunchedEffect(session.state.terminal) { if (session.state.terminal) { terminal?.ctrl = 0; terminal?.hideKeyboard() } }
    DisposableEffect(session.id) { onDispose { session.keyboardOffered = true } }
    LaunchedEffect(session.id, session.state.connected, terminal, controls.autoKeyboard) {
        if (session.state.connected && terminal != null && !session.keyboardOffered) {
            session.keyboardOffered = true
            if (controls.autoKeyboard) terminal?.requestAutomaticKeyboard()
        }
    }
    if (list) ModalBottomSheet(onDismissRequest = { list = false },
        sheetState = rememberModalBottomSheetState(skipPartiallyExpanded = true)) {
        Column(Modifier.fillMaxWidth().verticalScroll(rememberScrollState()).padding(16.dp)) {
            Text("会话", style = MaterialTheme.typography.titleLarge)
            model.sessions.forEach { item ->
                TextButton(onClick = { model.show(item.id); list = false }, modifier = Modifier.fillMaxWidth()) {
                    Column(Modifier.fillMaxWidth()) { Text(item.title); Text(item.state.description, style = MaterialTheme.typography.bodySmall) }
                }
            }
            OutlinedButton(onClick = { list = false; model.selectNewRemote() }, modifier = Modifier.fillMaxWidth()) { Text("新建 SSH 会话") }
            Button(onClick = { list = false; model.newLocal() }, enabled = !model.creating, modifier = Modifier.fillMaxWidth()) { Text("新建本地会话") }
        }
    }
    if (confirmClear) AlertDialog(onDismissRequest = { confirmClear = false }, title = { Text("清除本地显示？") },
        text = { Text("清除此终端当前屏幕及其滚动历史。不会发送命令或中断正在运行的程序。") },
        confirmButton = { TextButton(onClick = { confirmClear = false; terminal?.clearLocal() }) { Text("清除") } },
        dismissButton = { TextButton(onClick = { confirmClear = false }) { Text("取消") } })
    if (confirmClose) AlertDialog(onDismissRequest = { confirmClose = false }, title = { Text("关闭 ${session.title}？") },
        text = { Text(if (session.state.terminal) "将移除此会话的终端画面。" else if(session.backgroundSession!=null) "仅断开当前 tmux 客户端。任务保留遵循远端 tmux 与系统策略；再次连接时请刷新后台会话列表。" else "将断开此终端，正在运行的程序可能终止。返回主机列表可保留连接。") },
        confirmButton = { TextButton(onClick = { confirmClose = false; model.close(session.id) }) { Text("关闭会话") } },
        dismissButton = { TextButton(onClick = { confirmClose = false }) { Text("取消") } })
    }
}
