package com.hanxi.ghosterm

import android.view.KeyEvent
import androidx.compose.foundation.background
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.material3.*
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.semantics.selected
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.unit.dp

/** One input row in every window; navigation joins it only in short windows. */
@Composable
internal fun CompactTerminalBar(
    policy: TerminalControls, connected: Boolean, keyboardEnabled: Boolean, ctrl: Int, imeVisible: Boolean,
    onBack: () -> Unit, onCtrl: () -> Unit, onKey: (Int) -> Unit, onText: (String) -> Unit,
    onKeyboard: () -> Unit, onMenu: () -> Unit, menu: @Composable () -> Unit,
    compactNavigation: Boolean = true,
) {
    Row(Modifier.fillMaxWidth().height(52.dp).background(MaterialTheme.colorScheme.surface)
        .testTag(if (compactNavigation) "terminal-compact-bar" else "terminal-input-bar"), verticalAlignment = Alignment.CenterVertically) {
        if (compactNavigation) IconButton(onClick = onBack, modifier = Modifier.size(48.dp).testTag("terminal-back")) {
            GhostermIcon(UiIcon.Back,"返回")
        }
        if (policy.auxiliaryKeys) {
            TextButton(onClick = onCtrl, enabled = connected,colors=ButtonDefaults.textButtonColors(contentColor=MaterialTheme.colorScheme.onSurface), contentPadding = PaddingValues(horizontal = 8.dp),
                modifier = Modifier.height(48.dp)) {
                Text(when (ctrl) { 1 -> "Ctrl ✓"; 2 -> "Ctrl 锁定"; else -> "Ctrl" }, maxLines = 1)
            }
            Row(Modifier.weight(1f).height(48.dp).horizontalScroll(rememberScrollState()).testTag("terminal-auxiliary-keys"),
                verticalAlignment = Alignment.CenterVertically) {
                val keys = listOf("Esc" to KeyEvent.KEYCODE_ESCAPE, "Tab" to KeyEvent.KEYCODE_TAB,
                    "↑" to KeyEvent.KEYCODE_DPAD_UP, "↓" to KeyEvent.KEYCODE_DPAD_DOWN,
                    "←" to KeyEvent.KEYCODE_DPAD_LEFT, "→" to KeyEvent.KEYCODE_DPAD_RIGHT)
                keys.forEach { (label, code) ->
                    TextButton(onClick = { onKey(code) }, enabled = connected,colors=ButtonDefaults.textButtonColors(contentColor=MaterialTheme.colorScheme.onSurface), contentPadding = PaddingValues(horizontal = 8.dp),
                        modifier = Modifier.height(48.dp).widthIn(min = 48.dp)) { Text(label, maxLines = 1) }
                }
                listOf("/", "-").forEach { value ->
                    TextButton(onClick = { onText(value) }, enabled = connected,colors=ButtonDefaults.textButtonColors(contentColor=MaterialTheme.colorScheme.onSurface), contentPadding = PaddingValues(horizontal = 8.dp),
                        modifier = Modifier.size(48.dp)) { Text(value, maxLines = 1) }
                }
                if (policy.extendedKeys) listOf("Home" to KeyEvent.KEYCODE_MOVE_HOME, "End" to KeyEvent.KEYCODE_MOVE_END,
                    "PgUp" to KeyEvent.KEYCODE_PAGE_UP, "PgDn" to KeyEvent.KEYCODE_PAGE_DOWN).forEach { (label, code) ->
                    TextButton(onClick = { onKey(code) }, enabled = connected,colors=ButtonDefaults.textButtonColors(contentColor=MaterialTheme.colorScheme.onSurface), contentPadding = PaddingValues(horizontal = 8.dp),
                        modifier = Modifier.height(48.dp).widthIn(min = 48.dp)) { Text(label, maxLines = 1) }
                }
            }
        } else Spacer(Modifier.weight(1f))
        IconButton(onClick = onKeyboard, enabled = keyboardEnabled,
            modifier = Modifier.size(48.dp).testTag("terminal-keyboard")
                .semantics { selected = imeVisible }
                .background(if (imeVisible) MaterialTheme.colorScheme.primaryContainer else androidx.compose.ui.graphics.Color.Transparent,
                    MaterialTheme.shapes.medium)) {
            GhostermIcon(UiIcon.Keyboard,if (imeVisible) "收起键盘" else "显示键盘",tint=MaterialTheme.colorScheme.primary)
        }
        if (compactNavigation) Box {
            IconButton(onClick = onMenu, modifier = Modifier.size(48.dp).testTag("terminal-menu")) {
                GhostermIcon(UiIcon.More,"更多")
            }
            menu()
        }
    }
}
