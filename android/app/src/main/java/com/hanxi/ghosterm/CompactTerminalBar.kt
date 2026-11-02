package com.hanxi.ghosterm

import android.view.KeyEvent
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.material3.*
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.unit.dp

/** One touch-sized row leaves a terminal line above the landscape IME. */
@Composable
internal fun CompactTerminalBar(
    policy: TerminalControls, connected: Boolean, keyboardEnabled: Boolean, ctrl: Int, imeVisible: Boolean,
    onBack: () -> Unit, onCtrl: () -> Unit, onKey: (Int) -> Unit, onText: (String) -> Unit,
    onKeyboard: () -> Unit, onMenu: () -> Unit, menu: @Composable () -> Unit,
) {
    Row(Modifier.fillMaxWidth().height(48.dp).testTag("terminal-compact-bar"), verticalAlignment = Alignment.CenterVertically) {
        IconButton(onClick = onBack, modifier = Modifier.size(48.dp).testTag("terminal-back")) {
            Icon(painterResource(R.drawable.ic_terminal_back), contentDescription = "返回")
        }
        if (policy.auxiliaryKeys) {
            TextButton(onClick = onCtrl, enabled = connected, contentPadding = PaddingValues(horizontal = 8.dp),
                modifier = Modifier.height(48.dp)) {
                Text(when (ctrl) { 1 -> "Ctrl ✓"; 2 -> "Ctrl 锁定"; else -> "Ctrl" }, maxLines = 1)
            }
            Row(Modifier.weight(1f).height(48.dp).horizontalScroll(rememberScrollState()).testTag("terminal-auxiliary-keys"),
                verticalAlignment = Alignment.CenterVertically) {
                val keys = listOf("Esc" to KeyEvent.KEYCODE_ESCAPE, "Tab" to KeyEvent.KEYCODE_TAB,
                    "↑" to KeyEvent.KEYCODE_DPAD_UP, "↓" to KeyEvent.KEYCODE_DPAD_DOWN,
                    "←" to KeyEvent.KEYCODE_DPAD_LEFT, "→" to KeyEvent.KEYCODE_DPAD_RIGHT)
                keys.forEach { (label, code) ->
                    TextButton(onClick = { onKey(code) }, enabled = connected, contentPadding = PaddingValues(horizontal = 8.dp),
                        modifier = Modifier.height(48.dp).widthIn(min = 48.dp)) { Text(label, maxLines = 1) }
                }
                listOf("/", "-").forEach { value ->
                    TextButton(onClick = { onText(value) }, enabled = connected, contentPadding = PaddingValues(horizontal = 8.dp),
                        modifier = Modifier.size(48.dp)) { Text(value, maxLines = 1) }
                }
                if (policy.extendedKeys) listOf("Home" to KeyEvent.KEYCODE_MOVE_HOME, "End" to KeyEvent.KEYCODE_MOVE_END,
                    "PgUp" to KeyEvent.KEYCODE_PAGE_UP, "PgDn" to KeyEvent.KEYCODE_PAGE_DOWN).forEach { (label, code) ->
                    TextButton(onClick = { onKey(code) }, enabled = connected, contentPadding = PaddingValues(horizontal = 8.dp),
                        modifier = Modifier.height(48.dp).widthIn(min = 48.dp)) { Text(label, maxLines = 1) }
                }
            }
        } else Spacer(Modifier.weight(1f))
        IconButton(onClick = onKeyboard, enabled = keyboardEnabled,
            modifier = Modifier.size(48.dp).testTag("terminal-keyboard")) {
            Icon(painterResource(R.drawable.ic_terminal_keyboard), contentDescription = if (imeVisible) "收起键盘" else "显示键盘")
        }
        Box {
            IconButton(onClick = onMenu, modifier = Modifier.size(48.dp).testTag("terminal-menu")) {
                Icon(painterResource(R.drawable.ic_terminal_more), contentDescription = "更多")
            }
            menu()
        }
    }
}
