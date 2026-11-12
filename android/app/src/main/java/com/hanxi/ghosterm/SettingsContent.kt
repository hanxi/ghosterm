package com.hanxi.ghosterm

import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.selection.toggleable
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.saveable.rememberSaveableStateHolder
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.clearAndSetSemantics
import androidx.compose.ui.window.DialogProperties
import androidx.compose.ui.window.SecureFlagPolicy
import androidx.compose.ui.text.buildAnnotatedString
import androidx.compose.ui.text.SpanStyle
import androidx.compose.ui.text.withStyle
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.lifecycle.compose.LocalLifecycleOwner
import androidx.lifecycle.viewmodel.compose.viewModel
import com.hanxi.ghosterm.data.PreferencesState
import com.hanxi.ghosterm.domain.*
import kotlin.math.roundToInt
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive

internal fun PreferenceKey.title(): String = when (this) {
    PreferenceKey.APPEARANCE -> "应用外观"; PreferenceKey.TERMINAL_THEME -> "终端配色"
    PreferenceKey.FONT_FAMILY -> "终端字体"
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
    PreferenceKey.TERMINAL_THEME -> TerminalTheme.entries.map { v -> SettingOption(v.id,v.label,PreferenceEdit.Theme(v)) }
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
    PreferenceKey.FONT_FAMILY -> fontFamily.id
    PreferenceKey.LINE_HEIGHT -> lineHeight.id; PreferenceKey.CURSOR -> cursor.id; PreferenceKey.KEY_SET -> keySet.id
    PreferenceKey.CTRL_MODE -> ctrlMode.id; PreferenceKey.SORT_BY -> sortBy.id; PreferenceKey.CONFLICT -> conflict.preferenceId
    PreferenceKey.CLIPBOARD_AFTER_PASTE -> clipboardAfterPaste.id; else -> ""
}
private fun AppPreferences.label(key: PreferenceKey) = if(key==PreferenceKey.TERMINAL_THEME) terminalTheme.label else options(key).firstOrNull { it.id == optionId(key) }?.label.orEmpty()

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun SettingsContent(model: SettingsViewModel, configAvailable: Boolean, connectionsBusy: Boolean,
    onConfig: () -> Unit, onImport: () -> Unit,
    onIdentities: () -> Unit, onKnownHosts: () -> Unit, onAbout: () -> Unit,
    fonts:TerminalFontsViewModel=viewModel(), extensions: ExtensionPackagesViewModel=viewModel(),
    onPickPlugin: () -> Unit = {}, hostAliases: List<String> = emptyList(),
    onLaunchPlugin: (ExtensionPackagesViewModel.Entry, ExtensionController.Command, String, ExtensionRelayBootstrap.Request) -> Unit = { _, _, _, _ -> }) {
    var choice by rememberSaveable { mutableStateOf<PreferenceKey?>(null) }
    var fontPreview by remember { mutableStateOf<Int?>(null) }
    var launchSelection by remember { mutableStateOf<Pair<ExtensionPackagesViewModel.Entry, ExtensionController.Command>?>(null) }
    val savedSections = rememberSaveableStateHolder()
    val p = model.candidate
    val enabled = model.canEdit
    LaunchedEffect(model.section,p.fontFamily) { if(model.section==SettingsSection.APPEARANCE) fonts.refresh() }
    savedSections.SaveableStateProvider(model.section?.name ?: "overview") {
    Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(horizontal=GhostermDimensions.page,vertical=4.dp)) {
        val status = when {
            model.storage is PreferencesState.Loading -> "正在读取设置…"
            model.storage is PreferencesState.Unavailable -> "设置需要重新载入"
            fontPreview != null -> "预览中，松手后保存"
            model.inFlight > 0 -> "正在保存…"
            model.unsaved -> "有修改尚未保存"
            else -> "已保存"
        }
        val showStatusAtTop=model.storage !is PreferencesState.Ready || model.unsaved || model.inFlight>0 || fontPreview!=null
        if(showStatusAtTop) Text(status,Modifier.padding(vertical=8.dp).testTag("settings-save-status"),style=MaterialTheme.typography.bodySmall,color=MaterialTheme.colorScheme.error)
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
                Text("按你的习惯，调整 Ghosterm。",Modifier.padding(top=8.dp,bottom=16.dp),style=MaterialTheme.typography.bodyMedium,color=MaterialTheme.colorScheme.onSurfaceVariant)
                @Composable fun category(section: SettingsSection, summary: String) {
                    val icon=when(section) { SettingsSection.APPEARANCE->UiIcon.Terminal;SettingsSection.INPUT->UiIcon.Keyboard
                        SettingsSection.FILES,SettingsSection.PLUGINS->UiIcon.Folder;SettingsSection.SESSIONS->UiIcon.Server;SettingsSection.SECURITY->UiIcon.Shield }
                    GhostermActionRow(section.title,summary,icon,Modifier.testTag("settings-section-${section.name}")) { model.open(section) }
                }
                GhostermSectionLabel("显示与操作")
                GhostermGroup {
                    category(SettingsSection.APPEARANCE,"${p.label(PreferenceKey.TERMINAL_THEME)} · ${p.fontSize} sp");GhostermDivider()
                    category(SettingsSection.INPUT,if(p.auxKeys) "辅助键已开启 · "+if(p.ctrlMode==CtrlBehavior.ONCE) "Ctrl 单次" else "Ctrl 锁定" else "辅助键已关闭");GhostermDivider()
                    category(SettingsSection.FILES,"${if(p.showHidden) "显示" else "隐藏"}隐藏文件 · ${p.label(PreferenceKey.CONFLICT)}")
                }
                GhostermSectionLabel("会话与连接资料")
                GhostermGroup {
                    category(SettingsSection.SESSIONS,if(p.keepAwake) "前台终端保持常亮" else "使用系统息屏设置");GhostermDivider()
                    category(SettingsSection.SECURITY,"密钥信息与已验证主机")
                }
                GhostermSectionLabel("扩展")
                GhostermGroup { category(SettingsSection.PLUGINS, if (extensions.runtimeAvailable) "审查固定包；批准仅限本进程" else "JavaScript 插件运行时不可用") }
                TextButton(onConfig,enabled=configAvailable && !connectionsBusy,modifier=Modifier.heightIn(min=48.dp)) {
                    GhostermIcon(UiIcon.File);Spacer(Modifier.width(8.dp));Text("查看 SSH 配置")
                }
                Row(Modifier.fillMaxWidth(),horizontalArrangement=Arrangement.SpaceBetween) {
                    TextButton(onAbout) { Text("关于 Ghosterm") }
                    TextButton(model::requestReset,enabled=enabled && model.inFlight==0) { Text("恢复默认设置") }
                }
                Text("只恢复应用偏好，不删除主机、身份、验证记录、文件或运行中的会话与任务。",style=MaterialTheme.typography.bodySmall,color=MaterialTheme.colorScheme.onSurfaceVariant)
            }
            SettingsSection.APPEARANCE -> {
                val selectedFont=fonts.choices.firstOrNull { it.value==p.fontFamily }
                TerminalPreferencePreview(p.copy(fontSize = fontPreview ?: p.fontSize),selectedFont?.typeface,selectedFont?.problem ?: fonts.error)
                GhostermSectionLabel("界面与终端")
                GhostermGroup {
                    ChoiceRow(PreferenceKey.APPEARANCE,p,enabled) { choice=it };GhostermDivider()
                    ChoiceRow(PreferenceKey.TERMINAL_THEME,p,enabled) { choice=it }
                }
                GhostermSectionLabel("阅读体验")
                GhostermGroup {
                    GhostermActionRow("终端字体",selectedFont?.label ?: when(p.fontFamily) {
                        TerminalFont.SYSTEM -> "系统等宽字体";TerminalFont.JETBRAINS_MONO -> "JetBrains Mono"
                        TerminalFont.SOURCE_CODE_PRO -> "Source Code Pro";else -> "导入字体 · "+p.fontFamily.id.takeLast(8)
                    },modifier=Modifier.testTag("settings-row-fontFamily"),enabled=enabled) { choice=PreferenceKey.FONT_FAMILY }
                    GhostermDivider()
                    FontSizeRow(model) { fontPreview=it };GhostermDivider()
                    ChoiceRow(PreferenceKey.LINE_HEIGHT,p,enabled) { choice=it };GhostermDivider()
                    ChoiceRow(PreferenceKey.CURSOR,p,enabled) { choice=it };GhostermDivider()
                    SwitchRow(PreferenceKey.BLINK,p.blink,"关闭后光标保持静止",enabled) { model.edit(PreferenceEdit.Blink(it)) }
                }
                SettingsNote("终端外观立即生效，当前会话保持连接。")
            }
            SettingsSection.INPUT -> {
                Column(Modifier.fillMaxWidth().border(1.dp,MaterialTheme.colorScheme.outline,MaterialTheme.shapes.medium).padding(bottom=12.dp)) {
                    Text("辅助键预览",Modifier.padding(14.dp),style=MaterialTheme.typography.labelSmall,color=MaterialTheme.colorScheme.onSurfaceVariant)
                    val keys=listOf("Ctrl","Esc","Tab","↑","↓","←","→","/","-")+if(p.keySet==AuxiliaryKeySet.EXTENDED) listOf("Home","End","PgUp","PgDn") else emptyList()
                    Row(Modifier.fillMaxWidth().horizontalScroll(rememberScrollState()).padding(horizontal=10.dp),horizontalArrangement=Arrangement.spacedBy(5.dp)) {
                        keys.forEach { Text(it,Modifier.background(MaterialTheme.colorScheme.surfaceContainerHigh,MaterialTheme.shapes.extraSmall)
                            .padding(horizontal=10.dp,vertical=12.dp),style=MaterialTheme.typography.bodySmall.copy(fontFamily=FontFamily.Monospace)) }
                    }
                }
                GhostermSectionLabel("辅助输入")
                GhostermGroup {
                    SwitchRow(PreferenceKey.AUX_KEYS,p.auxKeys,"在终端底部提供 Esc、Ctrl 和方向键",enabled) { model.edit(PreferenceEdit.AuxKeys(it)) };GhostermDivider()
                    ChoiceRow(PreferenceKey.KEY_SET,p,enabled) { choice=it };GhostermDivider()
                    ChoiceRow(PreferenceKey.CTRL_MODE,p,enabled) { choice=it }
                }
                GhostermSectionLabel("打开会话时")
                GhostermGroup { SwitchRow(PreferenceKey.AUTO_KEYBOARD,p.autoKeyboard,"仅在新连接可输入时唤起；切换已有会话不自动弹出",enabled) { model.edit(PreferenceEdit.AutoKeyboard(it)) } }
                SettingsNote("系统输入法的语言、布局和自动纠正由 Android 管理。终端底部保留一个键盘开关。")
            }
            SettingsSection.SESSIONS -> {
                GhostermSectionLabel("前台使用")
                GhostermGroup { SwitchRow(PreferenceKey.KEEP_AWAKE,p.keepAwake,"只在前台终端常亮，进入文件、设置或后台即停止",enabled) { model.edit(PreferenceEdit.KeepAwake(it)) } }
                GhostermSectionLabel("连接资料")
                GhostermGroup {
                    ActionRow("查看与编辑 SSH 配置",configAvailable && !connectionsBusy,onConfig);GhostermDivider()
                    ActionRow("导入 SSH 配置",configAvailable && !connectionsBusy,onImport)
                }
                SettingsNote("导入会替换应用内副本，原文档不修改。")
                GhostermSectionLabel("会话规则")
                GhostermGroup { Text("普通 SSH 为默认入口，不自动连接或恢复。关闭连接始终确认；后台会话从具体主机显式选择 tmux。",Modifier.padding(16.dp),style=MaterialTheme.typography.bodyMedium) }
            }
            SettingsSection.FILES -> {
                GhostermSectionLabel("文件列表")
                GhostermGroup {
                    SwitchRow(PreferenceKey.SHOW_HIDDEN,p.showHidden,"在本机和远端显示以 . 开头的项目",enabled) { model.edit(PreferenceEdit.ShowHidden(it)) };GhostermDivider()
                    ChoiceRow(PreferenceKey.SORT_BY,p,enabled) { choice=it };GhostermDivider()
                    SwitchRow(PreferenceKey.FOLDERS_FIRST,p.foldersFirst,"文件夹排在文件前面",enabled) { model.edit(PreferenceEdit.FoldersFirst(it)) }
                }
                GhostermSectionLabel("复制与粘贴")
                GhostermGroup {
                    ChoiceRow(PreferenceKey.CONFLICT,p,enabled) { choice=it };GhostermDivider()
                    ChoiceRow(PreferenceKey.CLIPBOARD_AFTER_PASTE,p,enabled) { choice=it }
                }
                SettingsNote("点按打开，长按多选。更改列表外观不改变已复制的来源清单。")
            }
            SettingsSection.SECURITY -> {
                GhostermSectionLabel("已保存的身份")
                GhostermGroup { GhostermActionRow("SSH 身份","查看或导入安全存储中的密钥",UiIcon.Key,enabled=!connectionsBusy,onClick=onIdentities) }
                SettingsNote("SSH 身份加密保存在应用内，解密密钥由 Android Keystore 管理。")
                GhostermSectionLabel("已保存的验证记录")
                GhostermGroup { GhostermActionRow("主机验证记录","查看指纹与已验证的主机",UiIcon.Shield,enabled=!connectionsBusy,onClick=onKnownHosts) }
                SettingsNote("只显示身份信息；私钥材料和口令不会出现在此页面。")
            }
            SettingsSection.PLUGINS -> {
                GhostermSectionLabel("插件包来源")
                if (!extensions.runtimeAvailable) {
                    SettingsNote("当前构建未启用可用的 JavaScript 插件运行时。插件包不会被加载或执行。")
                } else {
                    SettingsNote("选择一个插件包文件夹进行固定快照与元数据审查。批准只对当前进程中的该包摘要有效；此页面不会安装或启动插件。")
                    Button(onClick=onPickPlugin, enabled=!extensions.busy, modifier=Modifier.fillMaxWidth().heightIn(min=48.dp)) {
                        Text("选择插件文件夹")
                    }
                    if (extensions.busy) LinearProgressIndicator(Modifier.fillMaxWidth().padding(vertical=8.dp))
                    extensions.error?.let { SettingsNotice(it) {} }
                    extensions.entries.forEach { entry ->
                        GhostermGroup(Modifier.padding(top=8.dp)) {
                            Text(entry.review.id, Modifier.padding(horizontal=16.dp, vertical=4.dp), style=MaterialTheme.typography.titleMedium)
                            Text("版本 ${entry.review.version} · 发布者 ${entry.review.publisher}", Modifier.padding(horizontal=16.dp), style=MaterialTheme.typography.bodyMedium)
                            Text("许可证 ${entry.review.license}", Modifier.padding(horizontal=16.dp), style=MaterialTheme.typography.bodySmall)
                            Text("运行时 ${entry.review.runtime} · 发布者身份 ${if (entry.review.publisherAuthenticated) "已认证" else "未认证"}", Modifier.padding(horizontal=16.dp), style=MaterialTheme.typography.bodySmall)
                            Text("声明的必需能力：${entry.review.requiredCapabilities.joinToString().ifEmpty { "无" }}", Modifier.padding(horizontal=16.dp), style=MaterialTheme.typography.bodySmall)
                            Text("缺失的可选能力：${entry.review.optionalMissingCapabilities.joinToString().ifEmpty { "无" }}", Modifier.padding(horizontal=16.dp), style=MaterialTheme.typography.bodySmall)
                            Text("SHA-256 ${entry.review.digest}", Modifier.padding(horizontal=16.dp, vertical=4.dp), style=MaterialTheme.typography.bodySmall)
                            entry.review.commands.forEach { command ->
                                Text("命令 ${command.path} · ${command.title}", Modifier.padding(horizontal=16.dp, vertical=2.dp), style=MaterialTheme.typography.bodySmall)
                                if (entry.decision == "已批准（本进程）") {
                                    TextButton(onClick = { launchSelection = entry to command },
                                        enabled = !extensions.launchBusy && !connectionsBusy,
                                        modifier = Modifier.padding(start = 8.dp).testTag("plugin-launch-${command.path}")) {
                                        Text("启动此命令…")
                                    }
                                }
                            }
                            Text(entry.decision, Modifier.padding(horizontal=16.dp, vertical=4.dp), style=MaterialTheme.typography.labelMedium)
                            Text("能力授权 ${if (entry.review.authorizationGranted) "已授予" else "未授予"}；来源批准不会授予能力。", Modifier.padding(horizontal=16.dp, vertical=4.dp), style=MaterialTheme.typography.bodySmall, color=MaterialTheme.colorScheme.onSurfaceVariant)
                            Row(Modifier.fillMaxWidth(), horizontalArrangement=Arrangement.End) {
                                TextButton(onClick={ extensions.decide(entry, false) }, enabled=!extensions.busy) { Text("拒绝") }
                                TextButton(onClick={ extensions.decide(entry, true) }, enabled=!extensions.busy) { Text("批准此摘要") }
                            }
                        }
                    }
                    extensions.activeSession?.let { active ->
                        Text("Session：${active.info.phase} · ${active.packageID} · ${active.info.peerIdentity}",
                            Modifier.padding(top = 8.dp).testTag("plugin-session-state"), style = MaterialTheme.typography.bodySmall)
                        if (active.info.phase == com.hanxi.ghosterm.bridge.ExtensionSession.Phase.ENDED ||
                            active.info.phase == com.hanxi.ghosterm.bridge.ExtensionSession.Phase.CLOSED) {
                            Text("Session 结束：${active.info.end} · Core ${active.info.error} · 清理 ${active.info.cleanupError}",
                                Modifier.testTag("plugin-session-end"), style = MaterialTheme.typography.bodySmall,
                                color = MaterialTheme.colorScheme.onSurfaceVariant)
                        }
                    }
                    extensions.activeRuntime?.let { active ->
                        Text("Runtime：${active.info.phase} · ${active.command} · steps ${active.info.steps}",
                            Modifier.padding(top = 4.dp).testTag("plugin-runtime-state"), style = MaterialTheme.typography.bodySmall)
                        if (active.info.phase == com.hanxi.ghosterm.bridge.ExtensionRuntime.Phase.FAILED ||
                            active.info.sessionEnd != com.hanxi.ghosterm.bridge.ExtensionSession.End.NONE ||
                            active.info.sessionError != 0 || active.info.cleanupError != 0) {
                            Text("Runtime 结果：${active.info.error} · Session ${active.info.sessionEnd}/${active.info.sessionError} · 清理 ${active.info.cleanupError}",
                                Modifier.testTag("plugin-runtime-result"), style = MaterialTheme.typography.bodySmall,
                                color = MaterialTheme.colorScheme.error)
                        }
                        PluginResourcesPanel(extensions.receivedResources)
                        if (active.info.phase in setOf(com.hanxi.ghosterm.bridge.ExtensionRuntime.Phase.CREATED,
                                com.hanxi.ghosterm.bridge.ExtensionRuntime.Phase.ACTIVE,
                                com.hanxi.ghosterm.bridge.ExtensionRuntime.Phase.STARTING)) {
                            TextButton(onClick = extensions::stopRuntime, enabled = !extensions.launchBusy) { Text("停止插件") }
                        } else {
                            TextButton(onClick = extensions::releaseFinished, enabled = !extensions.launchBusy) { Text("释放插件资源") }
                        }
                    }
                    if (extensions.activeRuntime == null && (extensions.activeSession?.info?.phase ==
                            com.hanxi.ghosterm.bridge.ExtensionSession.Phase.ENDED || extensions.activeSession?.info?.phase ==
                            com.hanxi.ghosterm.bridge.ExtensionSession.Phase.CLOSED)) {
                        TextButton(onClick = extensions::releaseFinished, enabled = !extensions.launchBusy) { Text("释放插件 Session") }
                    }
                    if (extensions.activeRuntime == null && extensions.activeSession?.info?.phase ==
                        com.hanxi.ghosterm.bridge.ExtensionSession.Phase.ACTIVE) {
                        TextButton(onClick = extensions::stopSession, enabled = !extensions.launchBusy) { Text("停止插件 Session") }
                    }
                }
            }
        }
        if(!showStatusAtTop) Text(status,Modifier.padding(vertical=12.dp).testTag("settings-save-status"),style=MaterialTheme.typography.labelSmall,color=MaterialTheme.colorScheme.onSurfaceVariant)
        Spacer(Modifier.height(8.dp))
    }
    }
    launchSelection?.let { (entry, command) ->
        PluginLaunchDialog(entry, command, hostAliases, onDismiss = { launchSelection = null }) { alias, request ->
            launchSelection = null
            onLaunchPlugin(entry, command, alias, request)
        }
    }
    extensions.activeSession?.takeIf { it.info.phase == com.hanxi.ghosterm.bridge.ExtensionSession.Phase.CONSENT }?.let { active ->
        PluginConsentDialog(active, extensions.launchBusy, extensions::decideConsent)
    }
    when {
        model.resetConfirmation -> AlertDialog(onDismissRequest = model::cancelReset,
            title = { Text("恢复默认设置？") }, text = { Text("恢复全部应用偏好，并放弃此页的候选修改。主机、身份、验证记录、文件、会话和传输保持不变。") },
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
            if(key==PreferenceKey.TERMINAL_THEME) TerminalThemeSheet(p.terminalTheme,enabled,{ choice=null }) { model.edit(PreferenceEdit.Theme(it));choice=null }
            else if(key==PreferenceKey.FONT_FAMILY) TerminalFontSheet(model,{ choice=null },{ model.edit(PreferenceEdit.FontFamily(it));choice=null },fonts)
            else
            ModalBottomSheet(dragHandle={ GhostermSheetHandle() },onDismissRequest={ choice=null },sheetState=rememberModalBottomSheetState(skipPartiallyExpanded=true)) {
                Column(Modifier.fillMaxWidth().verticalScroll(rememberScrollState()).padding(GhostermDimensions.page)) {
                    Text(key.title(),style=MaterialTheme.typography.titleLarge)
                    Spacer(Modifier.height(12.dp))
                    options(key).forEach { option -> Row(
                        Modifier.fillMaxWidth().heightIn(min = 48.dp).testTag("settings-option-${key.id}-${option.id}")
                            .selectable(p.optionId(key) == option.id, enabled = enabled, role = Role.RadioButton) {
                                model.edit(option.edit); choice = null
                            }.padding(vertical = 8.dp), verticalAlignment = Alignment.CenterVertically) {
                        RadioButton(p.optionId(key) == option.id, onClick = null, enabled = enabled)
                        Text(option.label, Modifier.padding(start = 12.dp).weight(1f))
                    } }
                    TextButton(onClick={ choice=null },modifier=Modifier.fillMaxWidth().heightIn(min=48.dp)) { Text("取消") }
                }
            }
        }
    }
}

@Composable internal fun PluginResourcesPanel(resources: List<com.hanxi.ghosterm.bridge.ExtensionResourceStore.Content>) {
    if (resources.isEmpty()) return
    Column(Modifier.fillMaxWidth().padding(top = 8.dp).testTag("plugin-resources"), verticalArrangement = Arrangement.spacedBy(8.dp)) {
        Text("收到的预览资源", style = MaterialTheme.typography.titleSmall)
        resources.forEach { resource ->
            GhostermGroup(Modifier.fillMaxWidth().testTag("plugin-resource-${resource.descriptor.resourceId}")) {
                Text(resource.descriptor.displayName, Modifier.padding(horizontal = 16.dp, vertical = 6.dp),
                    style = MaterialTheme.typography.titleSmall)
                Text("${resource.descriptor.mime} · ${resource.bytes.size} 字节 · SHA-256 ${resource.descriptor.sha256}",
                    Modifier.padding(horizontal = 16.dp), style = MaterialTheme.typography.bodySmall)
                PluginResourceBody(resource)
            }
        }
    }
}

@Composable private fun PluginResourceBody(resource: com.hanxi.ghosterm.bridge.ExtensionResourceStore.Content) {
    val mime = resource.descriptor.mime.substringBefore(';').lowercase()
    if (mime in setOf("image/png", "image/jpeg", "image/gif", "image/webp")) {
        val bitmap by produceState<android.graphics.Bitmap?>(null, resource.descriptor.sha256) {
            value = kotlinx.coroutines.withContext(kotlinx.coroutines.Dispatchers.Default) {
                decodePluginResourceBitmap(resource.bytes)
            }
        }
        if (bitmap != null) {
            Image(bitmap!!.asImageBitmap(), contentDescription = resource.descriptor.displayName,
                modifier = Modifier.fillMaxWidth().heightIn(max = 280.dp).padding(12.dp).testTag("plugin-resource-image"), contentScale = ContentScale.Fit)
        } else {
            Text("图片无法安全解码或尺寸超出预览范围", Modifier.padding(16.dp),
                color = MaterialTheme.colorScheme.onSurfaceVariant, style = MaterialTheme.typography.bodySmall)
        }
        return
    }
    if (mime.startsWith("text/") || mime == "application/json" || mime == "application/xml") {
        val preview = decodePluginResourceText(resource.bytes)
        if (preview != null) {
            SelectionContainer {
                Text(preview, Modifier.fillMaxWidth().heightIn(max = 220.dp).verticalScroll(rememberScrollState()).padding(12.dp),
                    maxLines = 80, style = MaterialTheme.typography.bodySmall.copy(fontFamily = FontFamily.Monospace),
                    color = MaterialTheme.colorScheme.onSurface)
            }
        } else {
            Text("文本不是有效 UTF-8 或超出显示预算", Modifier.padding(16.dp),
                color = MaterialTheme.colorScheme.onSurfaceVariant, style = MaterialTheme.typography.bodySmall)
        }
        if (mime == "text/html") Text("HTML 按源码显示，不执行脚本或加载网络内容。",
            Modifier.padding(horizontal = 16.dp, vertical = 4.dp), style = MaterialTheme.typography.labelSmall)
        return
    }
    Text("此格式已安全接收；当前只显示文件信息。", Modifier.padding(16.dp),
        color = MaterialTheme.colorScheme.onSurfaceVariant, style = MaterialTheme.typography.bodySmall)
}

internal fun decodePluginResourceText(bytes: ByteArray): String? {
    if (bytes.size > 1048576) return null
    return try {
        val decoder = Charsets.UTF_8.newDecoder()
            .onMalformedInput(java.nio.charset.CodingErrorAction.REPORT)
            .onUnmappableCharacter(java.nio.charset.CodingErrorAction.REPORT)
        decoder.decode(java.nio.ByteBuffer.wrap(bytes)).toString().take(8192)
    } catch (_: java.nio.charset.CharacterCodingException) { null }
}

internal fun decodePluginResourceBitmap(bytes: ByteArray): android.graphics.Bitmap? {
    if (bytes.isEmpty() || bytes.size > 1048576) return null
    val bounds = android.graphics.BitmapFactory.Options().apply { inJustDecodeBounds = true }
    android.graphics.BitmapFactory.decodeByteArray(bytes, 0, bytes.size, bounds)
    if (bounds.outWidth <= 0 || bounds.outHeight <= 0 || bounds.outWidth > 32768 || bounds.outHeight > 32768 ||
        bounds.outWidth.toLong() * bounds.outHeight > 268435456L) return null
    val options = android.graphics.BitmapFactory.Options()
    var sample = 1
    while (bounds.outWidth / sample > 2048 || bounds.outHeight / sample > 2048 ||
        (bounds.outWidth.toLong() / sample) * (bounds.outHeight.toLong() / sample) > 4194304L) sample *= 2
    options.inSampleSize = sample
    return runCatching { android.graphics.BitmapFactory.decodeByteArray(bytes, 0, bytes.size, options) }.getOrNull()
}

@Composable internal fun PluginConsentDialog(
    active: ExtensionPackagesViewModel.ActiveSession,
    busy: Boolean,
    onDecision: (Boolean) -> Unit,
) {
    AlertDialog(onDismissRequest = { if (!busy) onDecision(false) },
        properties = DialogProperties(securePolicy = SecureFlagPolicy.SecureOn),
        title = { Text("插件权限请求") },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text("${active.packageID} · ${active.command.title}")
                Text("SSH 主机：${active.info.peerIdentity}")
                Text("Executor 版本：${active.info.peerHostVersion}")
                Text("远端完整启动命令：", style = MaterialTheme.typography.labelMedium)
                SelectionContainer { Text(active.remoteCommand,
                    Modifier.testTag("plugin-consent-remote-command"), style = MaterialTheme.typography.bodySmall) }
                Text("即将传入的 argv：", style = MaterialTheme.typography.labelMedium)
                SelectionContainer { Text(org.json.JSONArray(active.arguments).toString(),
                    Modifier.testTag("plugin-consent-argv"), style = MaterialTheme.typography.bodySmall) }
                Text("按固定包 argsSchema 解码：${active.decodedArguments ?: "无映射（空参数）"}",
                    Modifier.testTag("plugin-consent-decoded-arguments"), style = MaterialTheme.typography.bodySmall)
                Text("允许 Client 调用的 Executor RPC：${active.methods.joinToString().ifBlank { "无" }}")
                Text("允许 Executor 调用的 Host RPC：${active.hostMethods.joinToString().ifBlank { "无" }}")
                Text("需要能力：${active.requiredCapabilities.joinToString().ifBlank { "无" }}")
                Text("仅批准当前连接中的能力授权。授权窗口启用系统安全窗口标记；仍请避免把秘密放进命令参数。Client 包摘要和 SSH Executor 摘要均固定；发布者身份未认证不代表可信。",
                    style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
        },
        confirmButton = { TextButton(onClick = { onDecision(true) }, enabled = !busy) { Text("允许并启动") } },
        dismissButton = { TextButton(onClick = { onDecision(false) }, enabled = !busy) { Text("拒绝") } })
}

@Composable internal fun PluginLaunchDialog(
    entry: ExtensionPackagesViewModel.Entry,
    command: ExtensionController.Command,
    hostAliases: List<String>,
    onDismiss: () -> Unit,
    onLaunch: (String, ExtensionRelayBootstrap.Request) -> Unit,
) {
    var selectedHost by remember(entry.review.id, command.path) { mutableStateOf(hostAliases.firstOrNull().orEmpty()) }
    var hostMenu by remember { mutableStateOf(false) }
    var executable by remember { mutableStateOf("gterm") }
    var executorDirectory by remember { mutableStateOf("") }
    var workspaceRootDirectory by remember { mutableStateOf("") }
    var executorDigest by remember { mutableStateOf("") }
    var argumentsJson by remember { mutableStateOf("[]") }
    var methodsJson by remember { mutableStateOf("[]") }
    var hostMethodsJson by remember { mutableStateOf("[]") }
    var problem by remember { mutableStateOf<String?>(null) }
    AlertDialog(onDismissRequest = onDismiss,
        properties = DialogProperties(securePolicy = SecureFlagPolicy.SecureOn),
        title = { Text("启动 ${command.title}") },
        text = {
            Column(Modifier.fillMaxWidth().heightIn(max = 480.dp).verticalScroll(rememberScrollState()),
                verticalArrangement = Arrangement.spacedBy(10.dp)) {
                Text(command.path, style = MaterialTheme.typography.bodySmall)
                if (hostAliases.isEmpty()) Text("当前没有可用的 SSH 主机配置。", color = MaterialTheme.colorScheme.error)
                else {
                    Box {
                        OutlinedButton(onClick = { hostMenu = true }, modifier = Modifier.fillMaxWidth()) {
                            Text(if (selectedHost.isBlank()) "选择 SSH 主机" else selectedHost)
                        }
                        DropdownMenu(expanded = hostMenu, onDismissRequest = { hostMenu = false }) {
                            hostAliases.forEach { alias ->
                                DropdownMenuItem(text = { Text(alias) }, onClick = { selectedHost = alias; hostMenu = false })
                            }
                        }
                    }
                }
                OutlinedTextField(executable, { executable = it }, singleLine = true, label = { Text("远端 gterm 可执行文件") })
                OutlinedTextField(executorDirectory, { executorDirectory = it }, singleLine = true,
                    modifier = Modifier.testTag("plugin-executor-directory"), label = { Text("远端 Executor 包目录") })
                OutlinedTextField(workspaceRootDirectory, { workspaceRootDirectory = it }, singleLine = true,
                    modifier = Modifier.testTag("plugin-workspace-root"),
                    label = { Text("可选：允许 Executor 读取的工作区目录") })
                OutlinedTextField(executorDigest, { executorDigest = it }, singleLine = true,
                    modifier = Modifier.testTag("plugin-executor-digest"), label = { Text("Executor 包 SHA-256") })
                OutlinedTextField(argumentsJson, { argumentsJson = it }, minLines = 2, maxLines = 4,
                    label = { Text("命令 argv（JSON 字符串数组）") }, placeholder = { Text("[\"--limit\", \"10\"]") })
                OutlinedTextField(methodsJson, { methodsJson = it }, minLines = 2, maxLines = 4,
                    label = { Text("允许调用的 Executor RPC 方法") }, placeholder = { Text("[\"task.run\"]") })
                OutlinedTextField(hostMethodsJson, { hostMethodsJson = it }, minLines = 2, maxLines = 4,
                    label = { Text("允许 Executor 调用的 Host RPC 方法") }, placeholder = { Text("[\"host.confirm\"]") })
                Text("仅填写后才会把该目录作为 workspace 显式传给 Executor；留空时不开放文件读取。将通过 SSH 明确启动指定目录中的 Executor。开发摘要是本次显式信任选择，不代表发布者签名认证；不会自动下载或安装。",
                    style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                Text("argv 和两类 RPC 方法均需填写 JSON 字符串数组。方法名是本次连接的允许列表；输入与输出固定为 object。Host 方法由 Client 插件通过 Portable SDK 注册 handler；未注册的方法会返回 UNSUPPORTED。",
                    style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                problem?.let { Text(it, color = MaterialTheme.colorScheme.error, style = MaterialTheme.typography.bodySmall) }
            }
        },
        confirmButton = {
            TextButton(enabled = selectedHost in hostAliases && executable.isNotBlank() && executorDirectory.isNotBlank(), onClick = {
                try {
                    val request = ExtensionRelayBootstrap.Request(executable.trim(), executorDirectory,
                        executorDigest.trim(), entry.review.digest, ExtensionRelayBootstrap.parseMethodsJson(methodsJson),
                        ExtensionRelayBootstrap.parseHostMethodsJson(hostMethodsJson), command.path,
                        ExtensionRelayBootstrap.parseArgumentsJson(argumentsJson),
                        workspaceRootDirectory.takeUnless { it.isBlank() })
                    ExtensionRelayBootstrap.build(request)
                    onLaunch(selectedHost, request)
                } catch (failure: IllegalArgumentException) {
                    problem = failure.message ?: "插件启动参数无效"
                }
            }) { Text("连接并请求授权") }
        },
        dismissButton = { TextButton(onClick = onDismiss) { Text("取消") } })
}

@Composable private fun SettingsNotice(message: String, actions: @Composable RowScope.() -> Unit) {
    Card(colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.errorContainer)) {
        Column(Modifier.fillMaxWidth().padding(12.dp)) {
            Text(message, Modifier.testTag("settings-error"))
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.End, content = actions)
        }
    }
}
@Composable private fun SettingsNote(value: String) {
    Text(value,Modifier.padding(horizontal=4.dp,vertical=14.dp),style=MaterialTheme.typography.bodySmall,color=MaterialTheme.colorScheme.onSurfaceVariant)
}
@Composable private fun ActionRow(label: String, enabled: Boolean, action: () -> Unit) {
    GhostermActionRow(label,enabled=enabled,onClick=action)
}
@Composable private fun ChoiceRow(key: PreferenceKey, p: AppPreferences, enabled: Boolean, action: (PreferenceKey) -> Unit) {
    GhostermActionRow(key.title(),p.label(key),modifier=Modifier.testTag("settings-row-${key.id}"),enabled=enabled) { action(key) }
}
@Composable private fun SwitchRow(key: PreferenceKey, value: Boolean, description: String, enabled: Boolean, change: (Boolean) -> Unit) {
    Row(Modifier.fillMaxWidth().heightIn(min=76.dp).testTag("settings-row-${key.id}")
        .toggleable(value,enabled=enabled,role=Role.Switch,onValueChange=change).padding(horizontal=14.dp,vertical=12.dp),
        verticalAlignment=Alignment.CenterVertically,horizontalArrangement=Arrangement.spacedBy(12.dp)) {
        Column(Modifier.weight(1f),verticalArrangement=Arrangement.spacedBy(4.dp)) {
            Text(key.title(),style=MaterialTheme.typography.titleSmall)
            Text(description,style=MaterialTheme.typography.bodySmall,color=MaterialTheme.colorScheme.onSurfaceVariant)
        }
        GhostermSwitch(value,enabled)
    }
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
@Composable private fun TerminalPreferencePreview(p: AppPreferences,face:android.graphics.Typeface?,fontProblem:String?) {
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
    val background=Color(0xff000000.toInt() or p.terminalTheme.background)
    val foreground=Color(0xff000000.toInt() or p.terminalTheme.foreground)
    val family=face?.let(::FontFamily) ?: FontFamily.Monospace
    Column(Modifier.fillMaxWidth().clip(MaterialTheme.shapes.medium).background(background).padding(16.dp).testTag("settings-terminal-preview")) {
        Text("终端预览", color = foreground, style = MaterialTheme.typography.labelSmall)
        if(face==null && p.fontFamily!=TerminalFont.SYSTEM) {
            Text(fontProblem ?: "正在载入字体预览…",color=foreground,style=MaterialTheme.typography.bodySmall)
            return@Column
        }
        val green=Color(0xff000000.toInt() or p.terminalTheme.palette[2])
        val blue=Color(0xff000000.toInt() or p.terminalTheme.palette[4])
        Text(buildAnnotatedString {
            withStyle(SpanStyle(color=green)) { append("deploy@production ") }
            withStyle(SpanStyle(color=blue)) { append("~/ghosterm\n") }
            append("$ git status\nOn branch main\n")
            withStyle(SpanStyle(color=green)) { append("✓ working tree clean") }
        }, color = foreground, fontFamily = family,
            fontSize = p.fontSize.sp, lineHeight = (p.fontSize * p.lineHeight.factor).sp)
        val height = with(LocalDensity.current) { p.fontSize.sp.toDp() }
        Row(verticalAlignment = Alignment.Bottom) {
            Text("$ ", color = foreground, fontFamily = family, fontSize = p.fontSize.sp)
            Box(Modifier.width(if (p.cursor == TerminalCursor.BAR) 2.dp else height * .6f)
                .height(if (p.cursor == TerminalCursor.UNDERLINE) 2.dp else height).background(if (cursorVisible) Color(0xff000000.toInt() or p.terminalTheme.cursorColor) else background))
        }
    }
}
