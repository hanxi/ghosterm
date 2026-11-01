package com.hanxi.ghosterm

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.selection.toggleable
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.saveable.rememberSaveableStateHolder
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.clearAndSetSemantics
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.lifecycle.compose.LocalLifecycleOwner
import com.hanxi.ghosterm.data.PreferencesState
import com.hanxi.ghosterm.domain.*
import kotlin.math.roundToInt
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive

internal fun PreferenceKey.title(): String = when (this) {
    PreferenceKey.APPEARANCE -> "应用外观"; PreferenceKey.TERMINAL_THEME -> "终端配色"
    PreferenceKey.FONT_SIZE -> "终端字号"; PreferenceKey.LINE_HEIGHT -> "行间距"; PreferenceKey.CURSOR -> "光标形状"
    PreferenceKey.BLINK -> "光标闪烁"; PreferenceKey.AUX_KEYS -> "显示辅助键条"; PreferenceKey.KEY_SET -> "辅助键布局"
    PreferenceKey.CTRL_MODE -> "Ctrl 键行为"; PreferenceKey.AUTO_KEYBOARD -> "连接后唤起键盘"
    PreferenceKey.KEEP_AWAKE -> "终端保持屏幕常亮"; PreferenceKey.SHOW_HIDDEN -> "显示隐藏文件"
    PreferenceKey.SORT_BY -> "默认排序"; PreferenceKey.FOLDERS_FIRST -> "文件夹优先"
    PreferenceKey.CONFLICT -> "同名文件默认处理"; PreferenceKey.CLIPBOARD_AFTER_PASTE -> "粘贴完成后"
}
private data class SettingOption(val id: String, val label: String, val edit: PreferenceEdit)
private fun options(key: PreferenceKey): List<SettingOption> = when (key) {
    PreferenceKey.APPEARANCE -> AppAppearance.entries.zip(listOf("跟随系统", "浅色", "深色")).map { (v, label) -> SettingOption(v.id, label, PreferenceEdit.Appearance(v)) }
    PreferenceKey.TERMINAL_THEME -> TerminalTheme.entries.zip(listOf("石墨蓝", "墨绿", "纸白")).map { (v, label) -> SettingOption(v.id, label, PreferenceEdit.Theme(v)) }
    PreferenceKey.LINE_HEIGHT -> TerminalLineHeight.entries.zip(listOf("紧凑", "标准", "宽松")).map { (v, label) -> SettingOption(v.id, label, PreferenceEdit.LineHeight(v)) }
    PreferenceKey.CURSOR -> TerminalCursor.entries.zip(listOf("方块", "竖线", "下划线")).map { (v, label) -> SettingOption(v.id, label, PreferenceEdit.Cursor(v)) }
    PreferenceKey.KEY_SET -> AuxiliaryKeySet.entries.zip(listOf("常用 · Esc / Ctrl / Tab / 方向键", "扩展 · 加入 Home / End / PgUp / PgDn")).map { (v, label) -> SettingOption(v.id, label, PreferenceEdit.KeySet(v)) }
    PreferenceKey.CTRL_MODE -> CtrlBehavior.entries.zip(listOf("单次 · 发送组合键后释放", "锁定 · 再点 Ctrl 才释放")).map { (v, label) -> SettingOption(v.id, label, PreferenceEdit.CtrlMode(v)) }
    PreferenceKey.SORT_BY -> FileSort.entries.zip(listOf("名称 · A → Z", "大小 · 从大到小")).map { (v, label) -> SettingOption(v.id, label, PreferenceEdit.Sort(v)) }
    PreferenceKey.CONFLICT -> FileConflictPolicy.entries.zip(listOf("逐项确认", "保留两者", "跳过同名")).map { (v, label) -> SettingOption(v.preferenceId, label, PreferenceEdit.Conflict(v)) }
    PreferenceKey.CLIPBOARD_AFTER_PASTE -> ClipboardAfterPaste.entries.zip(listOf("保留待粘贴清单", "清空已使用的清单")).map { (v, label) -> SettingOption(v.id, label, PreferenceEdit.Clipboard(v)) }
    else -> emptyList()
}
private fun AppPreferences.optionId(key: PreferenceKey): String = when (key) {
    PreferenceKey.APPEARANCE -> appearance.id; PreferenceKey.TERMINAL_THEME -> terminalTheme.id
    PreferenceKey.LINE_HEIGHT -> lineHeight.id; PreferenceKey.CURSOR -> cursor.id; PreferenceKey.KEY_SET -> keySet.id
    PreferenceKey.CTRL_MODE -> ctrlMode.id; PreferenceKey.SORT_BY -> sortBy.id; PreferenceKey.CONFLICT -> conflict.preferenceId
    PreferenceKey.CLIPBOARD_AFTER_PASTE -> clipboardAfterPaste.id; else -> ""
}
private fun AppPreferences.label(key: PreferenceKey) = options(key).firstOrNull { it.id == optionId(key) }?.label.orEmpty()

@Composable
fun SettingsContent(model: SettingsViewModel, configAvailable: Boolean, connectionsBusy: Boolean,
    onHosts: () -> Unit, onConfig: () -> Unit, onImport: () -> Unit,
    onIdentities: () -> Unit, onKnownHosts: () -> Unit, onAbout: () -> Unit) {
    var choice by rememberSaveable { mutableStateOf<PreferenceKey?>(null) }
    var fontPreview by remember { mutableStateOf<Int?>(null) }
    val savedSections = rememberSaveableStateHolder()
    val p = model.candidate
    val enabled = model.canEdit
    savedSections.SaveableStateProvider(model.section?.name ?: "overview") {
    Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
        val status = when {
            model.storage is PreferencesState.Loading -> "正在读取设置…"
            model.storage is PreferencesState.Unavailable -> "设置需要重新载入"
            fontPreview != null -> "预览中，松手后保存"
            model.inFlight > 0 -> "正在保存…"
            model.unsaved -> "有修改尚未保存"
            else -> "已保存"
        }
        Text(status, Modifier.testTag("settings-save-status"), style = MaterialTheme.typography.bodySmall)
        (model.storage as? PreferencesState.Unavailable)?.let { state ->
            SettingsNotice(state.message) { TextButton(model::requestReload, enabled = model.inFlight == 0) { Text("重新载入") } }
        }
        model.error?.let { message -> SettingsNotice(message) {
            if (model.unsaved && enabled) {
                TextButton(model::retry, enabled = model.inFlight == 0) { Text("重试保存") }
                TextButton(model::discard, enabled = model.inFlight == 0) { Text("放弃候选") }
            }
        } }
        (model.storage as? PreferencesState.Ready)?.issues?.takeIf { it.isNotEmpty() }?.let { issues ->
            Text("以下偏好值无法使用，当前采用默认值：${issues.joinToString("、") { it.key.title() }}。原文件会保留到你明确保存设置。",
                style = MaterialTheme.typography.bodySmall, modifier = Modifier.testTag("settings-value-issues"))
        }
        when (model.section) {
            null -> {
                SettingsSection.entries.forEach { section ->
                    Card { ListItem(headlineContent = { Text(section.title) }, supportingContent = { Text(section.subtitle) },
                        trailingContent = { Text("›", Modifier.clearAndSetSemantics { }, fontSize = 24.sp) },
                        modifier = Modifier.fillMaxWidth().heightIn(min = 64.dp).testTag("settings-section-${section.name}")
                            .clickable(role = Role.Button) { model.open(section) }) }
                }
                OutlinedButton(onAbout, Modifier.fillMaxWidth().heightIn(min = 48.dp)) { Text("关于 Ghosterm") }
                TextButton(model::requestReset, Modifier.fillMaxWidth().heightIn(min = 48.dp), enabled = enabled && model.inFlight == 0) { Text("恢复默认设置") }
                Text("只恢复应用偏好，不删除主机、身份、验证记录、文件或运行中的会话与任务。", style = MaterialTheme.typography.bodySmall)
            }
            SettingsSection.APPEARANCE -> {
                TerminalPreferencePreview(p.copy(fontSize = fontPreview ?: p.fontSize))
                ChoiceRow(PreferenceKey.APPEARANCE, p, enabled) { choice = it }
                ChoiceRow(PreferenceKey.TERMINAL_THEME, p, enabled) { choice = it }
                FontSizeRow(model) { fontPreview = it }
                ChoiceRow(PreferenceKey.LINE_HEIGHT, p, enabled) { choice = it }
                ChoiceRow(PreferenceKey.CURSOR, p, enabled) { choice = it }
                SwitchRow(PreferenceKey.BLINK, p.blink, "关闭后光标保持静止", enabled) { model.edit(PreferenceEdit.Blink(it)) }
            }
            SettingsSection.INPUT -> {
                SwitchRow(PreferenceKey.AUX_KEYS, p.auxKeys, "Esc、Ctrl、Tab 与方向键", enabled) { model.edit(PreferenceEdit.AuxKeys(it)) }
                ChoiceRow(PreferenceKey.KEY_SET, p, enabled) { choice = it }
                ChoiceRow(PreferenceKey.CTRL_MODE, p, enabled) { choice = it }
                SwitchRow(PreferenceKey.AUTO_KEYBOARD, p.autoKeyboard, "仅在新连接可输入时唤起；切换已有会话不自动弹出", enabled) { model.edit(PreferenceEdit.AutoKeyboard(it)) }
                Text("系统输入法的布局由 Android 管理。", style = MaterialTheme.typography.bodySmall)
            }
            SettingsSection.SESSIONS -> {
                SwitchRow(PreferenceKey.KEEP_AWAKE, p.keepAwake, "只在前台终端常亮，进入文件、设置或后台即停止", enabled) { model.edit(PreferenceEdit.KeepAwake(it)) }
                Text("连接资料", style = MaterialTheme.typography.titleMedium)
                ActionRow("管理主机", !connectionsBusy, onHosts)
                ActionRow("查看与编辑 SSH 配置", configAvailable && !connectionsBusy, onConfig)
                ActionRow("导入 SSH 配置", configAvailable && !connectionsBusy, onImport)
                Text("导入会替换应用内副本，原文档不修改。", style = MaterialTheme.typography.bodySmall)
            }
            SettingsSection.FILES -> {
                SwitchRow(PreferenceKey.SHOW_HIDDEN, p.showHidden, "在本机和远端显示以 . 开头的项目", enabled) { model.edit(PreferenceEdit.ShowHidden(it)) }
                ChoiceRow(PreferenceKey.SORT_BY, p, enabled) { choice = it }
                SwitchRow(PreferenceKey.FOLDERS_FIRST, p.foldersFirst, "文件夹排在文件前面", enabled) { model.edit(PreferenceEdit.FoldersFirst(it)) }
                ChoiceRow(PreferenceKey.CONFLICT, p, enabled) { choice = it }
                ChoiceRow(PreferenceKey.CLIPBOARD_AFTER_PASTE, p, enabled) { choice = it }
                Text("点按打开，长按多选。更改列表外观不改变已复制的来源清单。", style = MaterialTheme.typography.bodySmall)
            }
            SettingsSection.SECURITY -> {
                ActionRow("SSH 身份", !connectionsBusy, onIdentities)
                ActionRow("主机验证记录", !connectionsBusy, onKnownHosts)
                Text("SSH 身份加密保存在应用内，解密密钥由 Android Keystore 管理。主机验证记录单独保存。", style = MaterialTheme.typography.bodySmall)
            }
        }
        Spacer(Modifier.height(8.dp))
    }
    }
    when {
        model.resetConfirmation -> AlertDialog(onDismissRequest = model::cancelReset,
            title = { Text("恢复默认设置？") }, text = { Text("恢复全部 16 项应用偏好，并放弃此页的候选修改。主机、身份、验证记录、文件、会话和传输保持不变。") },
            confirmButton = { TextButton(model::confirmReset, enabled = enabled && model.inFlight == 0) { Text("恢复默认") } },
            dismissButton = { TextButton(model::cancelReset) { Text("保留设置") } })
        model.reloadConfirmation -> AlertDialog(onDismissRequest = model::cancelReload,
            title = { Text("重新载入设置？") }, text = { Text("成功载入后将放弃此页尚未保存的候选值，显示文件中实际保存的值。") },
            confirmButton = { TextButton(model::confirmReload) { Text("重新载入") } }, dismissButton = { TextButton(model::cancelReload) { Text("继续编辑") } })
        model.leaveConfirmation -> AlertDialog(onDismissRequest = model::keepEditing,
            title = { Text("放弃候选修改并离开？") }, text = { Text("只移除此页尚未保存的候选值，不会撤销已经发生的写入。保存结果尚未确认时，请重新载入后核对。") },
            confirmButton = { TextButton(model::discardAndLeave) { Text("放弃并离开") } }, dismissButton = { TextButton(model::keepEditing) { Text("继续编辑") } })
        choice != null -> {
            val key = checkNotNull(choice)
            AlertDialog(onDismissRequest = { choice = null }, title = { Text(key.title()) }, text = {
                Column(Modifier.heightIn(max = 300.dp).verticalScroll(rememberScrollState())) {
                    options(key).forEach { option -> Row(
                        Modifier.fillMaxWidth().heightIn(min = 48.dp).testTag("settings-option-${key.id}-${option.id}")
                            .selectable(p.optionId(key) == option.id, enabled = enabled, role = Role.RadioButton) {
                                model.edit(option.edit); choice = null
                            }.padding(vertical = 8.dp), verticalAlignment = Alignment.CenterVertically) {
                        RadioButton(p.optionId(key) == option.id, onClick = null, enabled = enabled)
                        Text(option.label, Modifier.padding(start = 12.dp).weight(1f))
                    } }
                }
            }, confirmButton = { TextButton({ choice = null }) { Text("取消") } })
        }
    }
}

@Composable private fun SettingsNotice(message: String, actions: @Composable RowScope.() -> Unit) {
    Card(colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.errorContainer)) {
        Column(Modifier.fillMaxWidth().padding(12.dp)) {
            Text(message, Modifier.testTag("settings-error"))
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.End, content = actions)
        }
    }
}
@Composable private fun ActionRow(label: String, enabled: Boolean, action: () -> Unit) {
    OutlinedButton(action, Modifier.fillMaxWidth().heightIn(min = 48.dp), enabled = enabled) { Text(label) }
}
@Composable private fun ChoiceRow(key: PreferenceKey, p: AppPreferences, enabled: Boolean, action: (PreferenceKey) -> Unit) {
    ListItem(headlineContent = { Text(key.title()) }, supportingContent = { Text(p.label(key)) },
        trailingContent = { Text("›", Modifier.clearAndSetSemantics { }, fontSize = 24.sp) },
        modifier = Modifier.fillMaxWidth().heightIn(min = 64.dp).testTag("settings-row-${key.id}")
            .clickable(enabled = enabled, role = Role.Button) { action(key) })
}
@Composable private fun SwitchRow(key: PreferenceKey, value: Boolean, description: String, enabled: Boolean, change: (Boolean) -> Unit) {
    ListItem(headlineContent = { Text(key.title()) }, supportingContent = { Text(description) },
        trailingContent = { Switch(value, onCheckedChange = null, enabled = enabled) },
        modifier = Modifier.fillMaxWidth().heightIn(min = 64.dp).testTag("settings-row-${key.id}")
            .toggleable(value, enabled = enabled, role = Role.Switch, onValueChange = change))
}
@OptIn(ExperimentalMaterial3Api::class)
@Composable private fun FontSizeRow(model: SettingsViewModel, preview: (Int?) -> Unit) {
    var value by remember { mutableFloatStateOf(model.candidate.fontSize.toFloat()) }
    var dragging by remember { mutableStateOf(false) }
    val interaction = remember { MutableInteractionSource() }
    DisposableEffect(Unit) { onDispose { preview(null) } }
    LaunchedEffect(model.candidate.fontSize) { if (!dragging) value = model.candidate.fontSize.toFloat() }
    Column(Modifier.fillMaxWidth().padding(12.dp)) {
        Text("终端字号 · ${value.roundToInt()} sp", Modifier.testTag("settings-font-value"))
        Slider(value, onValueChange = { dragging = true; value = it; preview(it.roundToInt()) }, enabled = model.canEdit, valueRange = 10f..20f, steps = 9,
            onValueChangeFinished = { dragging = false; model.edit(PreferenceEdit.FontSize(value.roundToInt())); preview(null) },
            interactionSource = interaction,
            // Material's visual thumb is 44dp; reserve 48dp inside its measured input layout.
            thumb = { Box(Modifier.height(48.dp), contentAlignment = Alignment.Center) {
                SliderDefaults.Thumb(interactionSource = interaction, enabled = model.canEdit)
            } },
            modifier = Modifier.fillMaxWidth().heightIn(min = 56.dp).testTag("settings-row-fontSize"))
        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) { Text("10 sp"); Text("20 sp") }
    }
}
@Composable private fun TerminalPreferencePreview(p: AppPreferences) {
    val lifecycle = LocalLifecycleOwner.current.lifecycle
    var previewActive by remember { mutableStateOf(lifecycle.currentState.isAtLeast(Lifecycle.State.STARTED)) }
    var cursorVisible by remember { mutableStateOf(true) }
    DisposableEffect(lifecycle) {
        val observer = LifecycleEventObserver { _, _ -> previewActive = lifecycle.currentState.isAtLeast(Lifecycle.State.STARTED) }
        lifecycle.addObserver(observer)
        onDispose { lifecycle.removeObserver(observer) }
    }
    LaunchedEffect(p.blink, previewActive) {
        cursorVisible = true
        if (p.blink && previewActive) while (isActive) { delay(600); cursorVisible = !cursorVisible }
    }
    val (background, foreground) = when (p.terminalTheme) {
        TerminalTheme.GRAPHITE -> Color(0xFF171B20) to Color(0xFFDCE3EC)
        TerminalTheme.FOREST -> Color(0xFF14241C) to Color(0xFFE2ECDF)
        TerminalTheme.PAPER -> Color(0xFFFAF8F2) to Color(0xFF283B33)
    }
    Column(Modifier.fillMaxWidth().background(background).padding(16.dp).testTag("settings-terminal-preview")) {
        Text("终端预览", color = foreground, style = MaterialTheme.typography.labelSmall)
        Text("ghosterm@android:~ $ ls\nDocuments  Downloads", color = foreground, fontFamily = FontFamily.Monospace,
            fontSize = p.fontSize.sp, lineHeight = (p.fontSize * p.lineHeight.factor).sp)
        val height = with(LocalDensity.current) { p.fontSize.sp.toDp() }
        Row(verticalAlignment = Alignment.Bottom) {
            Text("$ ", color = foreground, fontFamily = FontFamily.Monospace, fontSize = p.fontSize.sp)
            Box(Modifier.width(if (p.cursor == TerminalCursor.BAR) 2.dp else height * .6f)
                .height(if (p.cursor == TerminalCursor.UNDERLINE) 2.dp else height).background(if (cursorVisible) foreground else background))
        }
    }
}
