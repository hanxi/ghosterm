package com.hanxi.ghosterm

import androidx.activity.compose.BackHandler
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.compose.ui.window.PopupProperties
import androidx.compose.ui.text.style.TextOverflow
import com.hanxi.ghosterm.terminal.TerminalView
import kotlinx.coroutines.delay
import kotlinx.coroutines.withTimeoutOrNull

@OptIn(ExperimentalMaterial3Api::class, ExperimentalLayoutApi::class)
@Composable
fun TerminalScreen(model: SessionsViewModel,
    onRecover: ((TerminalSession) -> Unit)? = null, recoveryBusy: Boolean = false, recoveryError: String? = null,
    openFiles: () -> Unit) {
    val session = model.active ?: return
    val controls = model.terminalControls
    var terminal by remember(session.id) { mutableStateOf<TerminalView?>(null) }
    var ctrl by remember(session.id) { mutableIntStateOf(0) }
    var menu by remember { mutableStateOf(false) }
    var statusBarVisible by rememberSaveable { mutableStateOf(false) }
    TerminalSystemBars(statusBarVisible)
    var keyboardRequested by remember(session.id) { mutableStateOf(false) }
    var recoveryErrorDismissed by remember(session.id, recoveryError) { mutableStateOf(false) }
    var list by remember { mutableStateOf(false) }
    var confirmClear by remember(session.id) { mutableStateOf(false) }
    var confirmClose by remember(session.id) { mutableStateOf(false) }
    var cleanupIds by remember { mutableStateOf<List<java.util.UUID>?>(null) }
    val stopped = model.sessions.filter { it.state.terminal }
    val imeVisible = WindowInsets.ime.getBottom(LocalDensity.current) > 0
    val recoveryLabel = when {
        session.backgroundSession != null -> "查看后台会话"
        session.remoteAlias != null -> "重新连接"
        else -> "新建终端"
    }
    val recoveryEnabled = !recoveryBusy && !model.creating && model.connecting == null &&
        (onRecover != null || session.remoteAlias == null)
    val recover: () -> Unit = {
        if (session.state.terminal && recoveryEnabled) {
            terminal?.hideKeyboard()
            if (onRecover != null) onRecover(session) else model.restartLocal(session.id)
        }
    }
    LaunchedEffect(keyboardRequested, terminal) {
        val target = terminal
        if (keyboardRequested && target != null) {
            // Wait for popup dismissal and any pending window focus handoff.
            withFrameNanos { }
            withTimeoutOrNull(1_000) {
                while (!target.hasWindowFocus()) delay(25)
                if (target.isAttachedToWindow && target.isShown) target.showKeyboard()
            }
            keyboardRequested = false
        }
    }
    val back: () -> Unit = { if (imeVisible) terminal?.hideKeyboard() else if (terminal?.dismissSelection() != true) model.home() }
    BackHandler(onBack = back)
    BackHandler(enabled = menu) { menu = false }
    // API 29's stable edge-to-edge insets still include a hidden status bar.
    // Hidden status chrome releases the entire top band, including the cutout
    // inset. Keep side/bottom cutouts and desktop/freeform captions protected.
    val systemInsets = WindowInsets.systemBars.only(WindowInsetsSides.Horizontal + WindowInsetsSides.Bottom)
        .union(WindowInsets.captionBar)
        .union(if (statusBarVisible) WindowInsets.statusBars else WindowInsets(0, 0, 0, 0))
    BoxWithConstraints(Modifier.fillMaxSize().windowInsetsPadding(
        systemInsets.union(if (statusBarVisible) WindowInsets.displayCutout else
            WindowInsets.displayCutout.only(WindowInsetsSides.Horizontal + WindowInsetsSides.Bottom))
            .union(WindowInsets.ime))) {
    val density = LocalDensity.current
    val compact = maxHeight < 224.dp || (session.state.terminal && maxHeight < 360.dp)
    val metrics = LocalView.current.resources.displayMetrics
    val linePixels = kotlin.math.ceil(android.util.TypedValue.applyDimension(
        android.util.TypedValue.COMPLEX_UNIT_SP, model.terminalStyle.fontSize, metrics) *
        model.terminalStyle.lineHeight).toInt()
    // In very short landscape windows, place controls beside the terminal so text
    // keeps the full available height instead of losing another 48 dp to chrome.
    val sideControls = compact && maxWidth >= 480.dp &&
        maxHeight < 48.dp + with(density) { (linePixels * 2).toDp() }
    val sideWidth = maxWidth / 2
    val actions: @Composable () -> Unit = {
        DropdownMenu(expanded = menu, onDismissRequest = { menu = false },
            // Keep an open IME attached to the terminal. A focusable popup is
            // still useful for keyboard navigation when the IME is closed.
            properties = PopupProperties(focusable = !imeVisible)) {
            Column(Modifier.padding(horizontal = 16.dp, vertical = 8.dp)) {
                Text(session.title, style = MaterialTheme.typography.titleSmall)
                Text(session.state.description, style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
            if (model.backgroundState != null && !session.state.terminal) {
                DropdownMenuItem(text = { Text(when (model.backgroundState) {
                    TerminalBackgroundState.ACTIVE -> "后台连接保护已开启"
                    TerminalBackgroundState.STARTING -> "正在启动后台保护…"
                    else -> "启用后台连接保护"
                }) }, enabled = model.backgroundState !in listOf(TerminalBackgroundState.ACTIVE, TerminalBackgroundState.STARTING),
                    onClick = { menu = false; model.protectInBackground() })
                DropdownMenuItem(text = { Text(if (model.backgroundAwake) "关闭锁屏保活" else "锁屏时保持连接（耗电）") },
                    enabled = model.backgroundState == TerminalBackgroundState.ACTIVE,
                    onClick = { menu = false; model.toggleBackgroundAwake() }, modifier = Modifier.testTag("terminal-background-awake"))
                if (!model.backgroundNotificationsVisible) {
                    val context = LocalView.current.context
                    DropdownMenuItem(text = { Text("终端连接通知已关闭 · 打开设置") }, onClick = {
                        menu = false
                        context.startActivity(android.content.Intent(android.provider.Settings.ACTION_APP_NOTIFICATION_SETTINGS)
                            .putExtra(android.provider.Settings.EXTRA_APP_PACKAGE, context.packageName))
                    })
                }
            }
            if (session.state.terminal) DropdownMenuItem(text = { Text(recoveryLabel) }, enabled = recoveryEnabled,
                onClick = { menu = false; recover() }, modifier = Modifier.testTag("terminal-menu-recover"))
            DropdownMenuItem(text = { Text("显示键盘") }, enabled = !session.state.terminal,
                leadingIcon = { GhostermIcon(UiIcon.Keyboard) },
                onClick = { menu = false; keyboardRequested = true },
                modifier = Modifier.testTag("terminal-menu-keyboard"))
            DropdownMenuItem(text = { Text("文件") }, onClick = { menu = false; terminal?.hideKeyboard(); openFiles() }, modifier = Modifier.testTag("terminal-menu-files"))
            DropdownMenuItem(text = { Text(if (statusBarVisible) "隐藏系统状态栏" else "显示系统状态栏") },
                onClick = { menu = false; statusBarVisible = !statusBarVisible }, modifier = Modifier.testTag("terminal-menu-status-bar"))
            DropdownMenuItem(text = { Text("切换会话（${model.sessions.size}）") },
                onClick = { menu = false; terminal?.hideKeyboard(); list = true }, modifier = Modifier.testTag("terminal-menu-sessions")
                    .semantics { contentDescription = "切换会话，当前 ${session.title}，${model.sessions.size} 个会话" })
            DropdownMenuItem(text = { Text("返回主机列表") },
                onClick = { menu = false; terminal?.hideKeyboard(); model.home() }, modifier = Modifier.testTag("terminal-menu-home"))
            DropdownMenuItem(text = { Text("清除本地显示") }, onClick = { menu = false; confirmClear = true })
            if (stopped.isNotEmpty()) DropdownMenuItem(text = { Text("清理已停止会话（${stopped.size}）") },
                onClick = { menu = false; cleanupIds = stopped.map { it.id } }, modifier = Modifier.testTag("terminal-clean-stopped"))
            DropdownMenuItem(text = { Text("关闭会话") }, onClick = { menu = false; confirmClose = true })
        }
    }
    val inputBar: @Composable () -> Unit = {
        Column {
        if (session.state.terminal && !compact) Surface(color = MaterialTheme.colorScheme.surfaceContainerHigh) {
            Column(Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 8.dp).testTag("terminal-stopped")) {
                Column(Modifier.heightIn(max = 120.dp).verticalScroll(rememberScrollState())) {
                    Text(session.state.description, style = MaterialTheme.typography.bodyMedium)
                    Text(when {
                        session.backgroundSession != null -> "重新查看远端后台会话，再选择要连接的会话。"
                        session.remoteAlias != null -> "重新连接将打开新终端，旧画面保留。"
                        else -> "新终端会启动新的 shell，旧画面保留。"
                    }, style = MaterialTheme.typography.bodySmall)
                    recoveryError?.let { Text(it, color = MaterialTheme.colorScheme.error) }
                }
                Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Button(onClick = recover, enabled = recoveryEnabled, modifier = Modifier.testTag("terminal-recover")) {
                        Text(if (recoveryBusy || model.creating) "正在准备…" else recoveryLabel)
                    }
                    TextButton(onClick = { confirmClose = true }) { Text("移除旧会话") }
                }
            }
        }
        CompactTerminalBar(if (session.state.terminal) controls.copy(auxiliaryKeys = false) else controls, session.state.connected, ctrl,
            onCtrl = { terminal?.toggleCtrl() },
            onKey = { terminal?.sendKey(it) }, onText = { terminal?.commitText(it) },
            onMenu = { menu = true }, menu = actions,
            recoveryAction = if (session.state.terminal && compact) ({
                TextButton(onClick = recover, enabled = recoveryEnabled, modifier = Modifier.testTag("terminal-recover")
                    .semantics { contentDescription = session.state.description + "，" + recoveryLabel }) {
                    Text(recoveryLabel, maxLines = 1, overflow = TextOverflow.Ellipsis)
                }
            }) else null)
        }
    }
        Column(Modifier.fillMaxSize()) {
            (model.error ?: model.backgroundError)?.takeUnless { compact }?.let { message ->
                Row(Modifier.fillMaxWidth().padding(horizontal = 12.dp), verticalAlignment = Alignment.CenterVertically) {
                    Text(message, Modifier.weight(1f), color = MaterialTheme.colorScheme.error)
                    TextButton(onClick = { model.dismissError(); model.dismissBackgroundError() }) { Text("知道了") }
                }
            }
            Row(Modifier.fillMaxWidth().weight(1f)) {
            Box(Modifier.weight(1f).fillMaxHeight()) {
            key(session.id) {
                AndroidView(factory = { context -> TerminalView(context).also {
                    it.inputEnabled = !session.state.terminal
                    it.updateStyle(model.terminalStyle)
                    it.applyAuxiliaryPolicy(controls.auxiliaryKeys, controls.lockedCtrl)
                    it.requestKeepAwake(controls.keepAwake)
                    it.surface = session.surface; it.onError = model::reportError
                    it.onModifiersChanged = { value -> ctrl = value }; terminal = it
                    it.requestFocus()
                } }, update = { view ->
                    view.inputEnabled = !session.state.terminal
                    view.updateStyle(model.terminalStyle)
                    view.applyAuxiliaryPolicy(controls.auxiliaryKeys, controls.lockedCtrl)
                    view.requestKeepAwake(controls.keepAwake)
                }, modifier = Modifier.fillMaxSize().testTag("terminal-surface"))
            }
            if (compact) (model.error ?: model.backgroundError ?: recoveryError?.takeUnless { recoveryErrorDismissed })?.let { message ->
                Surface(color = MaterialTheme.colorScheme.errorContainer) {
                    Row(Modifier.fillMaxWidth().height(48.dp), verticalAlignment = Alignment.CenterVertically) {
                        Text(message, Modifier.weight(1f).padding(start = 8.dp), maxLines = 1, overflow = TextOverflow.Ellipsis)
                        TextButton(onClick = { model.dismissError(); model.dismissBackgroundError(); recoveryErrorDismissed = true }, contentPadding = PaddingValues(horizontal = 8.dp)) { Text("知道了", maxLines = 1) }
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
    LaunchedEffect(session.state.terminal) { if (session.state.terminal) { terminal?.ctrl = 0; terminal?.hideKeyboard() } }
    DisposableEffect(session.id) { onDispose { session.keyboardOffered = true } }
    LaunchedEffect(session.id, session.state.connected, terminal, controls.autoKeyboard) {
        if (session.state.connected && terminal != null && !session.keyboardOffered) {
            session.keyboardOffered = true
            if (controls.autoKeyboard) terminal?.requestAutomaticKeyboard()
        }
    }
    if (list) ModalBottomSheet(dragHandle={ GhostermSheetHandle() },onDismissRequest = { list = false },
        sheetState = rememberModalBottomSheetState(skipPartiallyExpanded = true)) {
        Column(Modifier.fillMaxWidth().verticalScroll(rememberScrollState()).padding(16.dp)) {
            Text("会话", style = MaterialTheme.typography.titleLarge)
            if (stopped.isNotEmpty()) TextButton(onClick = { list = false; cleanupIds = stopped.map { it.id } },
                modifier = Modifier.testTag("terminal-clean-stopped")) { Text("清理已停止会话（${stopped.size}）") }
            model.sessions.forEach { item ->
                TextButton(onClick = { model.show(item.id); list = false }, modifier = Modifier.fillMaxWidth()) {
                    Column(Modifier.fillMaxWidth()) { Text(item.title); Text(item.state.description, style = MaterialTheme.typography.bodySmall) }
                }
            }
            OutlinedButton(onClick = { list = false; model.selectNewRemote() }, modifier = Modifier.fillMaxWidth()) { Text("新建 SSH 会话") }
            Button(onClick = { list = false; model.newLocal() }, enabled = !model.creating, modifier = Modifier.fillMaxWidth()) { Text("新建本地会话") }
        }
    }
    cleanupIds?.let { ids ->
        AlertDialog(onDismissRequest = { cleanupIds = null }, title = { Text("清理 ${ids.size} 个已停止会话？") },
            text = { Text("这些会话的终端画面和滚动历史将被移除。已连接的终端与远端 tmux 任务不受影响。") },
            confirmButton = { TextButton(onClick = { cleanupIds = null; model.clearStopped(ids) },
                modifier = Modifier.testTag("terminal-clean-stopped-confirm")) { Text("清理") } },
            dismissButton = { TextButton(onClick = { cleanupIds = null }) { Text("保留") } })
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
