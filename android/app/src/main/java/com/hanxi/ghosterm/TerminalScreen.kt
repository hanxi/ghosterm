package com.hanxi.ghosterm

import android.view.KeyEvent
import androidx.activity.compose.BackHandler
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import com.hanxi.ghosterm.terminal.TerminalView

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun TerminalScreen(model: SessionsViewModel) {
    val session = model.active ?: return
    var terminal by remember(session.id) { mutableStateOf<TerminalView?>(null) }
    var ctrl by remember(session.id) { mutableIntStateOf(0) }
    var menu by remember { mutableStateOf(false) }
    var list by remember { mutableStateOf(false) }
    var confirmClear by remember(session.id) { mutableStateOf(false) }
    var confirmClose by remember(session.id) { mutableStateOf(false) }
    val imeVisible = WindowInsets.ime.getBottom(LocalDensity.current) > 0
    val back: () -> Unit = { if (imeVisible) terminal?.hideKeyboard() else if (terminal?.dismissSelection() != true) model.home() }
    BackHandler(onBack = back)
    Scaffold(topBar = {
        TopAppBar(title = {
            Column { Text(session.title); Text(session.state.description, style = MaterialTheme.typography.labelSmall) }
        }, navigationIcon = { TextButton(onClick = back, modifier = Modifier.testTag("terminal-back")) { Text("返回") } }, actions = {
            Box {
                TextButton(onClick = { menu = true }, modifier = Modifier.testTag("terminal-menu")) { Text("更多") }
                DropdownMenu(expanded = menu, onDismissRequest = { menu = false }) {
                    DropdownMenuItem(text = { Text("清除本地显示") }, onClick = { menu = false; confirmClear = true })
                    DropdownMenuItem(text = { Text("关闭会话") }, onClick = { menu = false; confirmClose = true })
                }
            }
        })
    }) { padding ->
        Column(Modifier.fillMaxSize().padding(padding).consumeWindowInsets(padding).imePadding()) {
            model.error?.let { message ->
                Row(Modifier.fillMaxWidth().padding(horizontal = 12.dp), verticalAlignment = Alignment.CenterVertically) {
                    Text(message, Modifier.weight(1f), color = MaterialTheme.colorScheme.error)
                    TextButton(onClick = model::dismissError) { Text("知道了") }
                }
            }
            key(session.id) {
                AndroidView(factory = { context -> TerminalView(context).also {
                    it.surface = session.surface; it.onError = model::reportError
                    it.onModifiersChanged = { value -> ctrl = value }; terminal = it
                    it.requestFocus()
                } }, modifier = Modifier.fillMaxWidth().weight(1f).testTag("terminal-surface"))
            }
            Row(Modifier.fillMaxWidth().horizontalScroll(rememberScrollState()), verticalAlignment = Alignment.CenterVertically) {
                val keys = listOf("Esc" to KeyEvent.KEYCODE_ESCAPE, "Tab" to KeyEvent.KEYCODE_TAB,
                    "↑" to KeyEvent.KEYCODE_DPAD_UP, "↓" to KeyEvent.KEYCODE_DPAD_DOWN,
                    "←" to KeyEvent.KEYCODE_DPAD_LEFT, "→" to KeyEvent.KEYCODE_DPAD_RIGHT)
                TextButton(onClick = { terminal?.ctrl = (ctrl + 1) % 3 }, enabled = session.state.connected,
                    modifier = Modifier.heightIn(min = 48.dp)) { Text(when(ctrl) { 1 -> "Ctrl ✓"; 2 -> "Ctrl 锁定"; else -> "Ctrl" }) }
                keys.forEach { (label, code) -> TextButton(onClick = { terminal?.sendKey(code) }, enabled = session.state.connected,
                    modifier = Modifier.sizeIn(minWidth = 48.dp, minHeight = 48.dp)) { Text(label) } }
                listOf("/", "-").forEach { value -> TextButton(onClick = { terminal?.commitText(value) }, enabled = session.state.connected,
                    modifier = Modifier.sizeIn(minWidth = 48.dp, minHeight = 48.dp)) { Text(value) } }
                listOf("Home" to KeyEvent.KEYCODE_MOVE_HOME, "End" to KeyEvent.KEYCODE_MOVE_END,
                    "PgUp" to KeyEvent.KEYCODE_PAGE_UP, "PgDn" to KeyEvent.KEYCODE_PAGE_DOWN).forEach { (label, code) ->
                    TextButton(onClick = { terminal?.sendKey(code) }, enabled = session.state.connected,
                        modifier = Modifier.heightIn(min = 48.dp)) { Text(label) }
                }
            }
            Row(Modifier.fillMaxWidth().padding(horizontal = 12.dp), verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.SpaceBetween) {
                TextButton(onClick = model::home) { Text("主机") }
                TextButton(onClick = { list = true }) { Text("会话 · ${model.sessions.size}") }
                TextButton(onClick = { if (imeVisible) terminal?.hideKeyboard() else terminal?.showKeyboard() },
                    modifier = Modifier.testTag("terminal-keyboard"), enabled = !session.state.terminal) {
                    Text(if (imeVisible) "收起键盘" else "显示键盘")
                }
            }
        }
    }
    LaunchedEffect(session.state.terminal) { if (session.state.terminal) { terminal?.ctrl = 0; terminal?.hideKeyboard() } }
    if (list) ModalBottomSheet(onDismissRequest = { list = false }) {
        Column(Modifier.fillMaxWidth().verticalScroll(rememberScrollState()).padding(16.dp)) {
            Text("会话", style = MaterialTheme.typography.titleLarge)
            model.sessions.forEach { item ->
                TextButton(onClick = { model.show(item.id); list = false }, modifier = Modifier.fillMaxWidth()) {
                    Column(Modifier.fillMaxWidth()) { Text(item.title); Text(item.state.description, style = MaterialTheme.typography.bodySmall) }
                }
            }
            Button(onClick = { list = false; model.newLocal() }, enabled = !model.creating, modifier = Modifier.fillMaxWidth()) { Text("新建本地会话") }
        }
    }
    if (confirmClear) AlertDialog(onDismissRequest = { confirmClear = false }, title = { Text("清除本地显示？") },
        text = { Text("清除此终端当前屏幕及其滚动历史。不会发送命令或中断正在运行的程序。") },
        confirmButton = { TextButton(onClick = { confirmClear = false; terminal?.clearLocal() }) { Text("清除") } },
        dismissButton = { TextButton(onClick = { confirmClear = false }) { Text("取消") } })
    if (confirmClose) AlertDialog(onDismissRequest = { confirmClose = false }, title = { Text("关闭 ${session.title}？") },
        text = { Text(if (session.state.terminal) "将移除此会话的终端画面。" else "将断开此终端，正在运行的本地程序可能终止。返回主机列表可保留连接。") },
        confirmButton = { TextButton(onClick = { confirmClose = false; model.close(session.id) }) { Text("关闭会话") } },
        dismissButton = { TextButton(onClick = { confirmClose = false }) { Text("取消") } })
}
