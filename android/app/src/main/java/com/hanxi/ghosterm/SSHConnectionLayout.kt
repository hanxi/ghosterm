package com.hanxi.ghosterm

import androidx.activity.compose.BackHandler
import androidx.compose.foundation.layout.*
import androidx.compose.material3.*
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp

/** One presentation across DNS, handshake, automatic authentication and shell opening.
 * The current page/old terminal remains mounted while a new owner connects. */
@Composable
fun SSHConnectionLayout(model: SessionsViewModel, taskModel: SessionsViewModel? = null, content: @Composable () -> Unit) {
    // Match dialog ownership: a foreground connection takes precedence over task authentication.
    val owner = if (model.connecting == null && taskModel != null) taskModel else model
    val attempt = owner.connecting?.takeIf { it.filesReady == null && it.passiveProgress }
    Column(Modifier.fillMaxSize()) {
        if (attempt != null) Surface(color = MaterialTheme.colorScheme.surfaceContainer) {
            Column(Modifier.fillMaxWidth()
                .windowInsetsPadding(WindowInsets.safeDrawing.only(WindowInsetsSides.Top + WindowInsetsSides.Horizontal))
                .padding(horizontal = 16.dp, vertical = 8.dp).testTag("ssh-connection-progress")) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Column(Modifier.weight(1f)) {
                        Text("连接 ${attempt.target.alias}", style = MaterialTheme.typography.titleSmall,
                            maxLines = 1, overflow = TextOverflow.Ellipsis)
                        attempt.taskLabel?.let { Text(it, style = MaterialTheme.typography.bodySmall) }
                        Text(attempt.message, style = MaterialTheme.typography.bodySmall)
                    }
                    TextButton(onClick = { owner.cancelConnection(attempt.id) },
                        modifier = Modifier.testTag("ssh-connection-cancel")) { Text("取消连接") }
                }
                LinearProgressIndicator(Modifier.fillMaxWidth())
            }
        }
        Box(Modifier.weight(1f).then(if (attempt != null)
            Modifier.consumeWindowInsets(WindowInsets.safeDrawing.only(WindowInsetsSides.Top)) else Modifier)) { content() }
    }
    // Register after page handlers: Back cancels this attempt before navigating away.
    BackHandler(attempt != null) { attempt?.let { owner.cancelConnection(it.id) } }
}
