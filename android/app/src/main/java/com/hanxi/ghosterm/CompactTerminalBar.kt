package com.hanxi.ghosterm

import android.view.KeyEvent
import androidx.compose.foundation.background
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.material3.*
import android.view.ViewConfiguration
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.interaction.PressInteraction
import androidx.compose.runtime.*
import androidx.compose.ui.platform.LocalWindowInfo
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.collectLatest
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.unit.dp

/** One input row in every window; navigation joins it only in short windows. */
@Composable
internal fun CompactTerminalBar(
    policy: TerminalControls, connected: Boolean, ctrl: Int,
    onBack: () -> Unit, onCtrl: () -> Unit, onKey: (Int) -> Unit, onText: (String) -> Unit,
    onMenu: () -> Unit, menu: @Composable () -> Unit,
    compactNavigation: Boolean = true,
    recoveryAction: (@Composable () -> Unit)? = null,
) {
    if (!compactNavigation && !policy.auxiliaryKeys) return
    Row(Modifier.fillMaxWidth().height(52.dp).background(MaterialTheme.colorScheme.surface)
        .testTag(if (compactNavigation) "terminal-compact-bar" else "terminal-input-bar"), verticalAlignment = Alignment.CenterVertically) {
        if (compactNavigation) IconButton(onClick = onBack, modifier = Modifier.size(48.dp).testTag("terminal-back")) {
            GhostermIcon(UiIcon.Back,"返回")
        }
        if (recoveryAction != null) Box(Modifier.weight(1f), contentAlignment = Alignment.Center) { recoveryAction() }
        else if (policy.auxiliaryKeys) {
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
                    RepeatingTerminalButton(onClick = { onKey(code) }, enabled = connected,
                        modifier = Modifier.height(48.dp).widthIn(min = 48.dp)) { Text(label, maxLines = 1) }
                }
                listOf("/", "-").forEach { value ->
                    RepeatingTerminalButton(onClick = { onText(value) }, enabled = connected,
                        modifier = Modifier.size(48.dp)) { Text(value, maxLines = 1) }
                }
                if (policy.extendedKeys) listOf("Home" to KeyEvent.KEYCODE_MOVE_HOME, "End" to KeyEvent.KEYCODE_MOVE_END,
                    "PgUp" to KeyEvent.KEYCODE_PAGE_UP, "PgDn" to KeyEvent.KEYCODE_PAGE_DOWN).forEach { (label, code) ->
                    RepeatingTerminalButton(onClick = { onKey(code) }, enabled = connected,
                        modifier = Modifier.height(48.dp).widthIn(min = 48.dp)) { Text(label, maxLines = 1) }
                }
            }
        } else Spacer(Modifier.weight(1f))
        if (compactNavigation) Box {
            IconButton(onClick = onMenu, modifier = Modifier.size(48.dp).testTag("terminal-menu")) {
                GhostermIcon(UiIcon.More,"更多")
            }
            menu()
        }
    }
}

/** Keep normal button semantics and keyboard activation; only a held press repeats. */
@Composable
internal fun RepeatingTerminalButton(enabled: Boolean, onClick: () -> Unit, modifier: Modifier = Modifier,
    content: @Composable androidx.compose.foundation.layout.RowScope.() -> Unit) {
    val interactions = remember { MutableInteractionSource() }
    val action by rememberUpdatedState(onClick)
    var repeated by remember { mutableStateOf(false) }
    val focused = LocalWindowInfo.current.isWindowFocused
    LaunchedEffect(interactions, enabled, focused) {
        repeated = false
        if (enabled && focused) interactions.interactions.collectLatest { interaction ->
            when (interaction) {
                is PressInteraction.Press -> {
                    repeated = false
                    delay(ViewConfiguration.getKeyRepeatTimeout().toLong())
                    while (true) {
                        repeated = true
                        action()
                        delay(ViewConfiguration.getKeyRepeatDelay().toLong())
                    }
                }
                is PressInteraction.Cancel -> repeated = false
                // Retain the flag until onClick, so releasing a hold adds no key.
                else -> Unit
            }
        }
    }
    TextButton(onClick = { if (!repeated) action(); repeated = false }, enabled = enabled,
        interactionSource = interactions,
        colors = ButtonDefaults.textButtonColors(contentColor = MaterialTheme.colorScheme.onSurface),
        contentPadding = PaddingValues(horizontal = 8.dp), modifier = modifier, content = content)
}
