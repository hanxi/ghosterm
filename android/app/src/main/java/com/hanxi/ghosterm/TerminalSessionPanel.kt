package com.hanxi.ghosterm

import androidx.activity.compose.BackHandler
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.focus.focusProperties
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.input.key.*
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.semantics.isTraversalGroup
import androidx.compose.ui.semantics.paneTitle
import androidx.compose.ui.semantics.selected
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.PopupProperties
import java.util.UUID

/** Presentation uses UUIDs throughout; host labels never become connection identities. */
@OptIn(ExperimentalMaterial3Api::class, androidx.compose.ui.ExperimentalComposeUiApi::class)
@Composable
internal fun TerminalSessionPanel(
    sessions: List<TerminalSession>, selectedId: UUID, previous: TerminalSession?, creating: Boolean,
    onSelect: (UUID) -> Unit, onDismiss: () -> Unit, onNewLocal: () -> Unit, onNewSSH: () -> Unit,
    onClose: (TerminalSession) -> Unit, onCleanup: (List<UUID>) -> Unit,
) {
    var query by rememberSaveable { mutableStateOf("") }
    var expandedStopped by rememberSaveable { mutableStateOf(sessions.any { it.id == selectedId && it.state.terminal }) }
    val stopped = sessions.filter { it.state.terminal }
    val filtered = sessions.filter {
        listOfNotNull(it.remoteAlias, it.title, it.switcherTitle, it.backgroundSession,
            if (it.remoteAlias == null) "本机 本地 shell" else if (it.backgroundSession != null) "tmux" else "SSH")
            .any { text -> text.contains(query.trim(), ignoreCase = true) }
    }
    val groups = filtered.filter { !it.state.terminal }.groupBy { it.remoteAlias }
    val filteredStopped = filtered.filter { it.state.terminal }
    val fontScale = LocalDensity.current.fontScale
    val largeText = fontScale > 1.4f
    val activeCount = sessions.size - stopped.size
    val groupCount = sessions.filter { !it.state.terminal }.map { it.remoteAlias }.distinct().size
    val visibleRows = activeCount + if (expandedStopped || query.isNotBlank()) stopped.size else 0
    // Keep a short list near the thumb. Long lists still use a bounded viewport
    // with an independently scrolling body and fixed creation actions.
    val preferredHeight = ((104 + (if (previous != null) 72 else 0) + (if (sessions.size > 6) 64 else 0) +
        groupCount * 40 + visibleRows * 76 + (if (stopped.isNotEmpty()) 56 else 0) +
        (if (largeText) 160 else 100)) * fontScale.coerceAtLeast(1f)).dp
    BackHandler(onBack = onDismiss)
    val dismiss by rememberUpdatedState(onDismiss)
    val view = LocalView.current
    val configuration = LocalConfiguration.current
    DisposableEffect(view) {
        val remove = if (android.os.Build.VERSION.SDK_INT >= 33) registerOverlayBack(view) { dismiss() } else ({})
        onDispose { remove() }
    }
    // This layer shares the terminal's window. It neither opens a Dialog nor
    // requests focus, so opening the chooser leaves the served IME in place.
    BoxWithConstraints(Modifier.fillMaxSize().testTag("terminal-session-overlay")
        .onPreInterceptKeyBeforeSoftKeyboard { event ->
            if (event.key == Key.Back) {
                if (event.type == KeyEventType.KeyUp) onDismiss()
                true
            } else false
        }) {
        // IME animation must not move the focused search editor between slots.
        // Choose the compact structure from stable window configuration instead.
        val compactPanel = largeText || configuration.screenHeightDp < 480 ||
            (configuration.orientation == android.content.res.Configuration.ORIENTATION_LANDSCAPE && configuration.screenHeightDp < 600)
        Box(Modifier.fillMaxSize().background(MaterialTheme.colorScheme.scrim.copy(alpha = .45f))
            .focusProperties { canFocus = false }.clickable(onClick = onDismiss).testTag("terminal-sessions-scrim"))
        Surface(Modifier.align(Alignment.BottomCenter).fillMaxWidth()
            .semantics { paneTitle = "会话"; isTraversalGroup = true },
            shape = RoundedCornerShape(topStart = 28.dp, topEnd = 28.dp),
            color = MaterialTheme.colorScheme.surface, shadowElevation = 8.dp) {
            BoxWithConstraints(Modifier.fillMaxWidth().heightIn(max = preferredHeight).fillMaxHeight(.92f).testTag("terminal-session-panel")) {
                val short = maxHeight < 400.dp
                val header: @Composable () -> Unit = {
                    Row(Modifier.fillMaxWidth().padding(start = 20.dp, end = 8.dp, bottom = 8.dp),
                        verticalAlignment = Alignment.CenterVertically) {
                        Column(Modifier.weight(1f)) {
                            Text("会话", style = MaterialTheme.typography.titleLarge)
                            val connected = sessions.count { it.state.connected }
                            val pending = sessions.size - connected - stopped.size
                            Text("$connected 个已连接 · ${stopped.size} 个已停止" + if (pending > 0) " · $pending 个准备中" else "",
                                style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                        }
                        IconButton(onClick = onDismiss, modifier = Modifier.size(48.dp).testTag("terminal-sessions-dismiss")) {
                            GhostermIcon(UiIcon.Close, "收起会话面板")
                        }
                    }
                }
                val search: @Composable () -> Unit = {
                    if (sessions.size > 6) OutlinedTextField(query, { query = it }, singleLine = true,
                        placeholder = { Text("搜索主机或会话") }, leadingIcon = { GhostermIcon(UiIcon.Search) },
                        modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp).padding(bottom = 8.dp)
                            .testTag("terminal-session-search"))
                }
                val footer: @Composable () -> Unit = {
                    HorizontalDivider()
                    Column(Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 10.dp)
                        .testTag("terminal-session-footer"), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                        val local: @Composable (Modifier) -> Unit = { modifier ->
                            OutlinedButton(onClick = onNewLocal, enabled = !creating,
                                modifier = modifier.heightIn(min = 48.dp).testTag("terminal-new-local")) {
                                GhostermIcon(UiIcon.Plus, "新建", Modifier.size(18.dp))
                                Spacer(Modifier.width(6.dp))
                                Text(if (creating) "正在创建…" else "本地会话", maxLines = 1, overflow = TextOverflow.Ellipsis)
                            }
                        }
                        val ssh: @Composable (Modifier) -> Unit = { modifier ->
                            Button(onClick = onNewSSH, modifier = modifier.heightIn(min = 48.dp).testTag("terminal-new-ssh")) {
                                GhostermIcon(UiIcon.Plus, "新建", Modifier.size(18.dp))
                                Spacer(Modifier.width(6.dp))
                                Text("SSH 会话", maxLines = 1, overflow = TextOverflow.Ellipsis)
                            }
                        }
                        if (largeText) {
                            local(Modifier.fillMaxWidth()); ssh(Modifier.fillMaxWidth())
                        } else Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                            local(Modifier.weight(1f)); ssh(Modifier.weight(1f))
                        }
                        if (!short) Text("切换保留连接 · 关闭操作需确认", Modifier.align(Alignment.CenterHorizontally),
                            style = MaterialTheme.typography.labelSmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                    }
                }
                Column(Modifier.fillMaxSize()) {
                    if (!compactPanel) { header(); search() }
                    LazyColumn(Modifier.fillMaxWidth().weight(1f).testTag("terminal-session-list"),
                        contentPadding = PaddingValues(horizontal = 12.dp, vertical = 4.dp)) {
                        if (compactPanel) item(key = "compact-header") { header(); search() }
                        if (previous != null) item(key = "previous") {
                            Surface(color = MaterialTheme.colorScheme.surfaceContainerHigh, shape = RoundedCornerShape(14.dp),
                                modifier = Modifier.fillMaxWidth().padding(horizontal = 4.dp, vertical = 4.dp)) {
                                Row(Modifier.clickable { onSelect(previous.id) }.heightIn(min = 64.dp).padding(12.dp)
                                    .testTag("terminal-previous-session"), verticalAlignment = Alignment.CenterVertically,
                                    horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                                    GhostermIcon(UiIcon.Back, modifier = Modifier.size(20.dp), tint = MaterialTheme.colorScheme.primary)
                                    Column(Modifier.weight(1f)) {
                                        Text("返回上一会话", style = MaterialTheme.typography.titleSmall)
                                        Text(listOfNotNull(previous.remoteAlias, previous.switcherTitle).joinToString(" · "),
                                            style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant,
                                            maxLines = 1, overflow = TextOverflow.Ellipsis)
                                    }
                                    GhostermIcon(UiIcon.Arrow, modifier = Modifier.size(16.dp))
                                }
                            }
                        }
                        groups.forEach { (alias, entries) ->
                            item(key = "group:${entries.first().id}") {
                                Row(Modifier.fillMaxWidth().padding(horizontal = 8.dp, vertical = 10.dp),
                                    horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                                    Text(alias ?: "本机", Modifier.weight(1f), style = MaterialTheme.typography.labelMedium,
                                        color = MaterialTheme.colorScheme.onSurfaceVariant)
                                    Text("${entries.size} 个", style = MaterialTheme.typography.labelSmall,
                                        color = MaterialTheme.colorScheme.onSurfaceVariant)
                                }
                            }
                            items(entries, key = { it.id }) { item ->
                                TerminalSessionRow(item, item.id == selectedId, onSelect, onClose)
                            }
                        }
                        if (filteredStopped.isNotEmpty()) {
                            item(key = "stopped-header") {
                                HorizontalDivider(Modifier.padding(top = 8.dp))
                                Row(verticalAlignment = Alignment.CenterVertically) {
                                    TextButton(onClick = { expandedStopped = !expandedStopped }, modifier = Modifier.weight(1f)
                                        .heightIn(min = 48.dp).testTag("terminal-stopped-toggle")) {
                                        Text("已停止 · ${filteredStopped.size}", Modifier.weight(1f), style = MaterialTheme.typography.labelMedium)
                                        GhostermIcon(if (expandedStopped || query.isNotBlank()) UiIcon.Down else UiIcon.Arrow,
                                            if (expandedStopped) "折叠已停止会话" else "展开已停止会话", Modifier.size(16.dp))
                                    }
                                    TextButton(onClick = { onCleanup(stopped.map { it.id }) }, modifier = Modifier.heightIn(min = 48.dp)
                                        .testTag("terminal-clean-stopped")) { Text("清理") }
                                }
                            }
                            if (expandedStopped || query.isNotBlank()) items(filteredStopped, key = { it.id }) { item ->
                                TerminalSessionRow(item, item.id == selectedId, onSelect, onClose)
                            }
                        }
                        if (filtered.isEmpty()) item { Text("没有匹配的会话", Modifier.padding(20.dp),
                            color = MaterialTheme.colorScheme.onSurfaceVariant) }

                        if (compactPanel) item(key = "compact-footer") { footer() }
                    }
                    if (!compactPanel) footer()
                }
            }
        }
    }
}

@Composable
private fun TerminalSessionRow(item: TerminalSession, current: Boolean,
    onSelect: (UUID) -> Unit, onClose: (TerminalSession) -> Unit) {
    var menu by remember(item.id) { mutableStateOf(false) }
    val imeVisible = WindowInsets.ime.getBottom(LocalDensity.current) > 0
    val background = if (current) MaterialTheme.colorScheme.primaryContainer else MaterialTheme.colorScheme.surface
    Row(Modifier.fillMaxWidth().padding(vertical = 2.dp).clip(RoundedCornerShape(14.dp)).background(background)
        .testTag("terminal-session-row-${item.id}").semantics { selected = current }, verticalAlignment = Alignment.CenterVertically) {
        Box(Modifier.width(3.dp).height(24.dp).background(if (current) MaterialTheme.colorScheme.primary else background))
        Row(Modifier.weight(1f).clickable { onSelect(item.id) }.heightIn(min = 68.dp).padding(horizontal = 10.dp, vertical = 10.dp)
            .testTag("terminal-session-select-${item.id}"), verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(10.dp)) {
            Surface(color = MaterialTheme.colorScheme.surfaceContainerHigh, shape = RoundedCornerShape(10.dp)) {
                GhostermIcon(if (item.backgroundSession == null) UiIcon.Terminal else UiIcon.Grid,
                    modifier = Modifier.padding(8.dp).size(18.dp),
                    tint = if (current) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.onSurfaceVariant)
            }
            Column(Modifier.weight(1f)) {
                Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                    Text(item.switcherTitle, Modifier.weight(1f, fill = false), style = MaterialTheme.typography.titleSmall,
                        maxLines = 2, overflow = TextOverflow.Ellipsis)
                    if (current) Text("当前", style = MaterialTheme.typography.labelSmall, color = MaterialTheme.colorScheme.primary)
                }
                val type = if (item.remoteAlias == null) "本地 shell" else if (item.backgroundSession == null) "SSH" else "tmux"
                Text((if (item.state.terminal) listOfNotNull(item.remoteAlias, item.state.description).joinToString(" · ")
                    else "${if (item.state.connected) "已连接" else item.state.description} · $type"),
                    style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant,
                    maxLines = 2, overflow = TextOverflow.Ellipsis)
            }
        }
        Box {
            IconButton(onClick = { menu = true }, modifier = Modifier.size(48.dp).testTag("terminal-session-more-${item.id}")) {
                GhostermIcon(UiIcon.More, "${item.remoteAlias ?: "本机"} · ${item.switcherTitle}的操作")
            }
            DropdownMenu(menu, { menu = false }, properties = PopupProperties(focusable = !imeVisible)) {
                DropdownMenuItem(text = { Text(if (item.state.terminal) "查看旧画面" else "切换到此会话") },
                    onClick = { menu = false; onSelect(item.id) })
                DropdownMenuItem(text = { Text(if (item.state.terminal) "移除旧会话…" else "关闭会话…",
                    color = MaterialTheme.colorScheme.error) }, onClick = { menu = false; onClose(item) },
                    modifier = Modifier.testTag("terminal-session-close-${item.id}"))
            }
        }
    }
}

@androidx.annotation.RequiresApi(33)
private fun registerOverlayBack(view: android.view.View, onDismiss: () -> Unit): () -> Unit {
    val dispatcher = view.findOnBackInvokedDispatcher() ?: return {}
    val callback = android.window.OnBackInvokedCallback(onDismiss)
    dispatcher.registerOnBackInvokedCallback(android.window.OnBackInvokedDispatcher.PRIORITY_OVERLAY, callback)
    return { dispatcher.unregisterOnBackInvokedCallback(callback) }
}
