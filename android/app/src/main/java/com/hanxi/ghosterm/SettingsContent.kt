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
import androidx.compose.foundation.text.InlineTextContent
import androidx.compose.foundation.text.appendInlineContent
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
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
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
import kotlinx.coroutines.launch
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Job

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
    frontendCandidates: List<FrontendCandidate> = emptyList(), frontendDiscoveryIssue: String? = null,
    onDismissFrontend: (java.util.UUID) -> Unit = {},
    validateFrontend: (FrontendCandidate) -> Boolean = { !it.expired },
    onInspectPlugin: suspend (String, String, String, String) -> PluginForegroundDeclaration = { _, _, _, _ -> error("前台实例检查不可用") },
    pluginWorkspaceID: java.util.UUID? = null,
    onLaunchPluginGroup: () -> Unit = {},
    onLaunchPlugin: suspend (ExtensionPackagesViewModel.Entry, ExtensionController.Command, String, ExtensionRelayBootstrap.Request, ExtensionController.ClientWorkspace?) -> Unit = { _, _, _, _, _ -> }) {
    var choice by rememberSaveable { mutableStateOf<PreferenceKey?>(null) }
    var fontPreview by remember { mutableStateOf<Int?>(null) }
    var launchSelection by remember { mutableStateOf<Pair<ExtensionPackagesViewModel.Entry, ExtensionController.Command>?>(null) }
    var githubAddress by rememberSaveable { mutableStateOf("") }
    var githubPlanVisible by rememberSaveable { mutableStateOf(false) }
    val savedSections = rememberSaveableStateHolder()
    val p = model.candidate
    val enabled = model.canEdit
    LaunchedEffect(model.section,p.fontFamily) { if(model.section==SettingsSection.APPEARANCE) fonts.refresh() }
    if (githubPlanVisible) {
        val plan = extensions.githubInstallPlan()
        AlertDialog(onDismissRequest={ githubPlanVisible = false }, title={ Text("插件安装计划") }, text={
            if (plan == null) Text("安装计划无效，请重新读取仓库并选择插件。") else Column { Text("来源：${plan.source.repositoryPath}"); Text("Release：${plan.releaseTag}"); Text("发布者：${plan.publisher}"); plan.items.forEach { Text("${it.id} · ${it.version} · ${it.role}") }; Text("需要的权限和能力将在安装前再次确认。") }
        }, confirmButton={ TextButton(onClick={ githubPlanVisible = false }) { Text("返回修改") } }, dismissButton={ TextButton(onClick={ githubPlanVisible = false }) { Text("取消") } })
    }
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
                GhostermGroup { category(SettingsSection.PLUGINS, if (extensions.runtimeAvailable) "审查固定包；批准仅限本进程" else "Lua 插件运行时不可用") }
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
                if (frontendCandidates.isNotEmpty()) {
                    SettingsNote("终端候选尚未核实。外层 SSH 和来源终端不证明执行端身份；请审查对应 Client，并明确选择实际执行端后检查。")
                    frontendCandidates.forEach { candidate ->
                        GhostermGroup {
                            SelectionContainer { Text("${candidate.sourceDescription}\n${candidate.offer.directory}\nClient：${candidate.offer.clientDigest}",
                                Modifier.padding(12.dp), style = MaterialTheme.typography.bodySmall) }
                            TextButton(onClick = { onDismissFrontend(candidate.id) }) { Text("移除此候选") }
                        }
                    }
                }
                frontendDiscoveryIssue?.let { SettingsNote(it) }
                GhostermSectionLabel("插件包来源")
                if (!extensions.runtimeAvailable) {
                    SettingsNote("此构建未启用 Lua 插件运行时。")
                } else {
                    PluginChannelRefreshContent(extensions.controller, extensions::requestStopInstancesWithRevokedPublisher)
                    PluginArchiveReviewContent(extensions.controller)
                    PluginInstalledVersionsContent(extensions.controller, extensions::versionSelectionChanged, extensions::versionRemovalChanged)
                    SettingsNote("选择一个插件包文件夹进行固定快照与元数据审查。批准只对当前进程中的该包摘要有效；启动命令后仍需单独授权。")
                    Button(onClick=onPickPlugin, enabled=!extensions.busy, modifier=Modifier.fillMaxWidth().heightIn(min=48.dp)) {
                        Text("选择插件文件夹")
                    }
                    OutlinedTextField(value=githubAddress, onValueChange={ githubAddress=it }, label={ Text("GitHub 仓库") }, singleLine=true, modifier=Modifier.fillMaxWidth().testTag("plugin-github-address"))
                    Button(onClick={ extensions.readGitHubSource(githubAddress) }, enabled=!extensions.githubBusy && githubAddress.isNotBlank(), modifier=Modifier.fillMaxWidth().heightIn(min=48.dp).testTag("plugin-github-read")) { Text(if (extensions.githubBusy) "正在读取…" else "读取 GitHub 仓库") }
                    extensions.githubError?.let { SettingsNotice(it) {} }
                    extensions.githubIndex?.let { value ->
                        Text("${value.repository} · Release ${value.releaseTag} · ${value.publisher}", Modifier.testTag("plugin-github-index"))
                        Row(Modifier.fillMaxWidth(), horizontalArrangement=Arrangement.spacedBy(8.dp)) {
                            TextButton(onClick={ extensions.selectAllGitHubPlugins() }) { Text("全选") }
                            TextButton(onClick={ extensions.clearGitHubSelection() }) { Text("清除") }
                        }
                        value.plugins.forEach { entry ->
                            Row(Modifier.fillMaxWidth().toggleable(entry.id in extensions.githubSelectedIds, role=Role.Checkbox) { extensions.toggleGitHubPlugin(entry.id, it) }.padding(horizontal=8.dp), verticalAlignment=Alignment.CenterVertically) {
                                Checkbox(entry.id in extensions.githubSelectedIds, onCheckedChange=null)
                                Text("${entry.name} · ${entry.version} · ${entry.role}", Modifier.padding(start=8.dp), style=MaterialTheme.typography.bodySmall)
                            }
                        }
                        Button(onClick={ githubPlanVisible = true }, enabled=extensions.githubSelectedIds.isNotEmpty(), modifier=Modifier.fillMaxWidth().testTag("plugin-github-install-plan")) { Text("查看安装计划（${extensions.githubSelectedIds.size}）") }
                    }
                    if (extensions.busy) LinearProgressIndicator(Modifier.fillMaxWidth().padding(vertical=8.dp))
                    extensions.error?.let { SettingsNotice(it) {} }
                    extensions.entries.forEach { entry ->
                        key(entry.review.id, entry.review.digest) {
                        GhostermGroup(Modifier.padding(top=8.dp)) {
                            Text(entry.review.id, Modifier.padding(horizontal=16.dp, vertical=4.dp), style=MaterialTheme.typography.titleMedium)
                            Text("版本 ${entry.review.version} · 声明发布者 ${entry.review.publisher}", Modifier.padding(horizontal=16.dp), style=MaterialTheme.typography.bodyMedium)
                            Text("许可证 ${entry.review.license}", Modifier.padding(horizontal=16.dp), style=MaterialTheme.typography.bodySmall)
                            Text("运行时 ${entry.review.runtime}", Modifier.padding(horizontal=16.dp), style=MaterialTheme.typography.bodySmall)
                            PluginSignatureReviewContent(extensions.controller, entry.review)
                            Text("声明的必需能力：${entry.review.requiredCapabilities.joinToString().ifEmpty { "无" }}", Modifier.padding(horizontal=16.dp), style=MaterialTheme.typography.bodySmall)
                            Text("缺失的可选能力：${entry.review.optionalMissingCapabilities.joinToString().ifEmpty { "无" }}", Modifier.padding(horizontal=16.dp), style=MaterialTheme.typography.bodySmall)
                            Text("SHA-256 ${entry.review.digest}", Modifier.padding(horizontal=16.dp, vertical=4.dp), style=MaterialTheme.typography.bodySmall)
                            entry.review.commands.forEach { command ->
                                Text("命令 ${command.path} · ${command.title}", Modifier.padding(horizontal=16.dp, vertical=2.dp), style=MaterialTheme.typography.bodySmall)
                                if (entry.decision == "已批准（本进程）") {
                                    TextButton(onClick = { launchSelection = entry to command },
                                        enabled = !extensions.launchBusy && !connectionsBusy,
                                        modifier = Modifier.padding(start = 8.dp).testTag("plugin-launch-${command.path}").semantics {
                                            contentDescription = "启动插件命令 ${command.path}，${command.title}"
                                        }) {
                                        Text("启动此命令…")
                                    }
                                }
                            }
                            Text(entry.decision, Modifier.padding(horizontal=16.dp, vertical=4.dp), style=MaterialTheme.typography.labelMedium)
                            Text("能力授权 ${if (entry.review.authorizationGranted) "已授予" else "未授予"}；来源批准不会授予能力。", Modifier.padding(horizontal=16.dp, vertical=4.dp), style=MaterialTheme.typography.bodySmall, color=MaterialTheme.colorScheme.onSurfaceVariant)
                            Row(Modifier.fillMaxWidth(), horizontalArrangement=Arrangement.End) {
                                TextButton(onClick={ extensions.decide(entry, false) }, enabled=!extensions.busy,
                                    modifier = Modifier.semantics { contentDescription = "拒绝插件摘要 ${entry.review.id}" }) { Text("拒绝") }
                                TextButton(onClick={ extensions.decide(entry, true) }, enabled=!extensions.busy,
                                    modifier = Modifier.semantics { contentDescription = "批准插件摘要 ${entry.review.id}" }) { Text("批准此摘要") }
                            }
                        }
                        }
                    }
                    PluginConnectionGroupsContent(extensions, connectionsBusy, onLaunchPluginGroup)
                    val selected = extensions.selectedGroupEntry
                    val instance = selected?.model ?: extensions
                    key(instance) {
                        when {
                            selected?.closed == true -> Text("插件实例已关闭")
                            selected?.releaseFailed == true -> Text("插件实例仍有资源待释放，请在实例列表重试关闭。")
                            selected?.closeRequested == true -> Text("正在关闭插件实例…")
                            else -> PluginActiveInstanceContent(instance)
                        }
                    }

                }
            }
        }
        if(!showStatusAtTop) Text(status,Modifier.padding(vertical=12.dp).testTag("settings-save-status"),style=MaterialTheme.typography.labelSmall,color=MaterialTheme.colorScheme.onSurfaceVariant)
        Spacer(Modifier.height(8.dp))
    }
    }
    launchSelection?.let { (entry, command) ->
        PluginLaunchDialog(entry, command, hostAliases, onInspect = onInspectPlugin, frontendCandidates = frontendCandidates, validateFrontend = validateFrontend,
            onEnqueue = { alias, request, workspace, temporary ->
                extensions.validateAndEnqueueGroup(entry, command, alias, request, workspace, temporary, pluginWorkspaceID)
                    .onSuccess { launchSelection = null }
            },
            onDismiss = { launchSelection = null }, onLaunch = { alias, request, clientWorkspace ->
            onLaunchPlugin(entry, command, alias, request, clientWorkspace)
            launchSelection = null
        })
    }
    val selected = extensions.selectedGroupEntry
    val selectedInstance = selected?.model ?: extensions
    selectedInstance.activeSession?.takeIf {
        selected?.closeRequested != true && it.info.phase == com.hanxi.ghosterm.bridge.ExtensionSession.Phase.CONSENT
    }?.let { active ->
        key(selectedInstance) {
            PluginConsentDialog(active, selectedInstance.launchBusy, selectedInstance::decideConsent)
        }
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
    val resourceIds = resources.map { it.descriptor.resourceId }
    var selectedResourceId by remember { mutableStateOf(resourceIds.first()) }
    LaunchedEffect(resourceIds) {
        if (selectedResourceId !in resourceIds) selectedResourceId = resourceIds.first()
    }
    Column(Modifier.fillMaxWidth().padding(top = 8.dp).testTag("plugin-resources"), verticalArrangement = Arrangement.spacedBy(8.dp)) {
        Text("收到的预览资源", style = MaterialTheme.typography.titleSmall, modifier = Modifier.semantics {
            contentDescription = "收到的预览资源，共 ${resources.size} 项"
        })
        GhostermGroup(Modifier.fillMaxWidth().testTag("plugin-resource-list")) {
            resources.forEach { resource ->
                val selected = resource.descriptor.resourceId == selectedResourceId
                TextButton(
                    onClick = { selectedResourceId = resource.descriptor.resourceId },
                    modifier = Modifier.fillMaxWidth().testTag("plugin-resource-select-${resource.descriptor.resourceId}").semantics {
                        contentDescription = "${if (selected) "当前" else "选择"}资源 ${resource.descriptor.displayName}"
                    },
                    contentPadding = PaddingValues(horizontal = 16.dp, vertical = 8.dp),
                ) {
                    Text("${if (selected) "✓ " else ""}${resource.descriptor.displayName}",
                        Modifier.fillMaxWidth(), color = if (selected) MaterialTheme.colorScheme.primary
                        else MaterialTheme.colorScheme.onSurface)
                }
            }
        }
        resources.singleOrNull { it.descriptor.resourceId == selectedResourceId }?.let { resource ->
            GhostermGroup(Modifier.fillMaxWidth().testTag("plugin-resource-${resource.descriptor.resourceId}")) {
                Text(resource.descriptor.displayName, Modifier.padding(horizontal = 16.dp, vertical = 6.dp).semantics {
                    contentDescription = "资源 ${resource.descriptor.displayName}"
                },
                    style = MaterialTheme.typography.titleSmall)
                Text("${resource.descriptor.mime} · ${resource.bytes.size} 字节 · SHA-256 ${resource.descriptor.sha256}",
                    Modifier.padding(horizontal = 16.dp).semantics {
                        contentDescription = "类型 ${resource.descriptor.mime}，${resource.bytes.size} 字节，摘要 ${resource.descriptor.sha256}"
                    }, style = MaterialTheme.typography.bodySmall)
                key(resource.descriptor) {
                    PluginResourceBody(resource, resources)
                }
            }
        }
    }
}

@Composable private fun PluginResourceBody(
    resource: com.hanxi.ghosterm.bridge.ExtensionResourceStore.Content,
    resources: List<com.hanxi.ghosterm.bridge.ExtensionResourceStore.Content>,
) {
    val mime = resource.descriptor.mime.substringBefore(';').lowercase()
    if (PluginResourceHTMLDocument.supports(mime)) {
        PluginHTMLBody(resource, resources)
        return
    }
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
    if (PluginResourceMarkdownDecoder.supports(mime, resource.descriptor.displayName)) {
        PluginMarkdownBody(resource, resources)
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
        val preview = decoder.decode(java.nio.ByteBuffer.wrap(bytes)).toString().take(8192)
        if (preview.lastOrNull()?.isHighSurrogate() == true) preview.dropLast(1) else preview
    } catch (_: java.nio.charset.CharacterCodingException) { null }
}

private data class ResolvedMarkdownImage(
    val reference: PluginResourceMarkdownDecoder.ImageReference,
    val resource: com.hanxi.ghosterm.bridge.ExtensionResourceStore.Content,
)

internal data class RenderedPluginMarkdown(
    val text: androidx.compose.ui.text.AnnotatedString,
    val inlineContent: Map<String, InlineTextContent>,
)

@Composable private fun PluginMarkdownBody(
    resource: com.hanxi.ghosterm.bridge.ExtensionResourceStore.Content,
    resources: List<com.hanxi.ghosterm.bridge.ExtensionResourceStore.Content>,
) {
    val decoded by produceState<PluginResourceMarkdownDecoder.Decoded?>(null, resource.descriptor.sha256) {
        value = kotlinx.coroutines.withContext(kotlinx.coroutines.Dispatchers.Default) {
            PluginResourceMarkdownDecoder.decodeDocument(resource.bytes)
        }
    }
    val resolved = remember(decoded, resource.descriptor.displayName, resources) {
        val document = decoded ?: return@remember emptyList()
        document.images.mapNotNull { image ->
            val path = PluginResourceMarkdownDecoder.resolveSameDirectoryImage(resource.descriptor.displayName, image.destination)
                ?: return@mapNotNull null
            val candidates = resources.filter { candidate ->
                candidate.descriptor.displayName == path &&
                    PluginResourceMarkdownDecoder.isCanonicalWorkspacePath(candidate.descriptor.displayName) &&
                    candidate.descriptor.mime.substringBefore(';').lowercase() in setOf("image/png", "image/jpeg", "image/gif", "image/webp") &&
                    candidate.descriptor.sizeKnown && candidate.descriptor.size == candidate.bytes.size.toLong() &&
                    candidate.bytes.size in 1..1048576
            }
            candidates.singleOrNull()?.let { ResolvedMarkdownImage(image, it) }
        }
    }
    val bitmaps = key(resolved) {
        produceState<Map<String, android.graphics.Bitmap>>(emptyMap()) {
            value = kotlinx.coroutines.withContext(kotlinx.coroutines.Dispatchers.Default) {
                resolved.mapNotNull { item ->
                    decodePluginResourceBitmap(item.resource.bytes)?.let { item.reference.marker to it }
                }.toMap()
            }
        }.value
    }
    val rendered = remember(decoded, bitmaps) {
        decoded?.let { pluginMarkdownAnnotatedString(it, bitmaps) }
    }
    if (decoded == null) {
        Text("Markdown 不是有效 UTF-8 或超出预览预算", Modifier.padding(16.dp),
            color = MaterialTheme.colorScheme.onSurfaceVariant, style = MaterialTheme.typography.bodySmall)
    } else if (rendered != null) {
        SelectionContainer {
            Text(rendered.text,
                Modifier.fillMaxWidth().heightIn(max = 300.dp).verticalScroll(rememberScrollState()).padding(12.dp)
                    .testTag("plugin-resource-markdown"),
                style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurface,
                inlineContent = rendered.inlineContent,
            )
        }
        Text("Markdown 链接不打开；图片与外部资源不加载。", Modifier.padding(horizontal = 16.dp, vertical = 4.dp),
            style = MaterialTheme.typography.labelSmall)
    }
}

internal fun pluginMarkdownAnnotatedString(
    decoded: PluginResourceMarkdownDecoder.Decoded,
    bitmaps: Map<String, android.graphics.Bitmap>,
): RenderedPluginMarkdown {
    val spanned = androidx.core.text.HtmlCompat.fromHtml(decoded.html, androidx.core.text.HtmlCompat.FROM_HTML_MODE_COMPACT)
    val source = spanned.toString()
    val spans = spanned.getSpans(0, spanned.length, Any::class.java)
    val inlineContent = linkedMapOf<String, InlineTextContent>()
    val text = androidx.compose.ui.text.buildAnnotatedString {
        fun appendStyledRange(start: Int, end: Int) {
            if (end <= start) return
            val outputStart = length
            append(source.substring(start, end))
            for (span in spans) {
                val spanStart = spanned.getSpanStart(span).coerceAtLeast(start)
                val spanEnd = spanned.getSpanEnd(span).coerceAtMost(end)
                if (spanStart >= spanEnd) continue
                val style = when (span) {
                    is android.text.style.StyleSpan -> androidx.compose.ui.text.SpanStyle(
                        fontWeight = if (span.style and android.graphics.Typeface.BOLD != 0) androidx.compose.ui.text.font.FontWeight.Bold else null,
                        fontStyle = if (span.style and android.graphics.Typeface.ITALIC != 0) androidx.compose.ui.text.font.FontStyle.Italic else null)
                    is android.text.style.TypefaceSpan -> if (span.family == "monospace") androidx.compose.ui.text.SpanStyle(fontFamily = androidx.compose.ui.text.font.FontFamily.Monospace) else null
                    is android.text.style.UnderlineSpan -> androidx.compose.ui.text.SpanStyle(textDecoration = androidx.compose.ui.text.style.TextDecoration.Underline)
                    is android.text.style.StrikethroughSpan -> androidx.compose.ui.text.SpanStyle(textDecoration = androidx.compose.ui.text.style.TextDecoration.LineThrough)
                    is android.text.style.RelativeSizeSpan -> androidx.compose.ui.text.SpanStyle(fontSize = (span.sizeChange * 14).sp)
                    else -> null
                }
                if (style != null) addStyle(style, outputStart + spanStart - start, outputStart + spanEnd - start)
            }
        }
        var cursor = 0
        for (image in decoded.images) {
            val markerAt = source.indexOf(image.marker, cursor)
            if (markerAt < 0) continue
            appendStyledRange(cursor, markerAt)
            val bitmap = bitmaps[image.marker]
            if (bitmap == null) {
                append(if (image.alt.isEmpty()) "[图片]" else "[图片: ${image.alt}]")
            } else {
                val id = "plugin-markdown-image-${inlineContent.size}"
                val maxSide = 96f
                val scale = minOf(1f, maxSide / maxOf(bitmap.width, bitmap.height))
                val widthSp = (bitmap.width * scale).coerceAtLeast(1f).sp
                val heightSp = (bitmap.height * scale).coerceAtLeast(1f).sp
                inlineContent[id] = InlineTextContent(
                    androidx.compose.ui.text.Placeholder(widthSp, heightSp,
                        androidx.compose.ui.text.PlaceholderVerticalAlign.TextCenter)) {
                    Image(bitmap.asImageBitmap(), image.alt.ifEmpty { null },
                        Modifier.fillMaxSize().testTag("plugin-resource-markdown-inline-image"), contentScale = ContentScale.Fit)
                }
                appendInlineContent(id, image.alt.ifEmpty { "图片" })
            }
            cursor = markerAt + image.marker.length
        }
        appendStyledRange(cursor, source.length)
    }
    return RenderedPluginMarkdown(text, inlineContent)
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
            Column(Modifier.heightIn(max = 480.dp).verticalScroll(rememberScrollState()),
                verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text("${active.packageID} · ${active.command.title}")
                Text("SSH 主机：${active.info.peerIdentity}")
                Text("Executor 版本：${active.info.peerHostVersion}")
                SelectionContainer { Text("Executor 运行档：${active.executorAttachment?.reviewDescription ?: "Lua（内嵌运行时）"}",
                    Modifier.testTag("plugin-consent-executor-runtime"), style = MaterialTheme.typography.bodySmall) }
                SelectionContainer { Text(active.clientWorkspace?.reviewDescription ?: "Client 本机资源目录：未开放",
                    Modifier.testTag("plugin-consent-client-workspace"), style = MaterialTheme.typography.bodySmall) }
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
        dismissButton = {
            TextButton(onClick = { onDecision(false) }, enabled = !busy,
                modifier = Modifier.testTag("plugin-consent-deny")) { Text("拒绝") }
        })
}

@Composable private fun PluginNativeResourcePublisher(extensions: ExtensionPackagesViewModel) {
    var path by remember(extensions.activeSession?.packageDigest, extensions.activeSession?.peerIdentity) { mutableStateOf("") }
    val runtime = extensions.activeRuntime
    val publication = runtime?.publication
    Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
        OutlinedTextField(path, { path = it }, singleLine = true,
            enabled = extensions.canInteractWithRuntime,
            modifier = Modifier.testTag("plugin-client-publication-path"), label = { Text("从已允许本机目录发布的相对文件名") })
        TextButton(onClick = { extensions.publishClientResource(path, instanceID = runtime?.instanceID) },
            enabled = path.isNotEmpty() && extensions.canInteractWithRuntime && publication?.released != false,
            modifier = Modifier.testTag("plugin-client-publish")) { Text("向 Executor 发布文件") }
        publication?.let {
            SelectionContainer { Text("${it.relativePath}：${it.reviewDescription}",
                Modifier.testTag("plugin-client-publication-state"), style = MaterialTheme.typography.bodySmall) }
            if (it.sha256.isNotEmpty()) SelectionContainer { Text("${it.bytes} 字节 · SHA-256 ${it.sha256}", style = MaterialTheme.typography.bodySmall) }
            if (!it.released) TextButton(onClick = { extensions.cancelClientPublication(it.id) },
                modifier = Modifier.testTag("plugin-client-publication-cancel")) { Text("取消发布") }
        }
    }
}

@Composable
internal fun TerminalPluginCommands(
    extensions: ExtensionPackagesViewModel,
    sessions: SessionsViewModel,
    hosts: HostsViewModel,
    onDismiss: () -> Unit,
    onStarted: () -> Unit,
) {
    var selection by remember {
        mutableStateOf<Pair<ExtensionPackagesViewModel.Entry, ExtensionController.Command>?>(null)
    }
    val workspaceID = sessions.active?.id ?: return
    val selected = selection
    if (selected == null) {
        AlertDialog(
            onDismissRequest = onDismiss,
            title = { Text("插件命令") },
            text = {
                Column(Modifier.heightIn(max = 380.dp).verticalScroll(rememberScrollState())) {
                    extensions.entries.filter { it.decision == "已批准（本进程）" }.forEach { entry ->
                        entry.review.commands.forEach { command ->
                            val panel = extensions.workspacePanel(workspaceID, entry, command)
                            TextButton(
                                onClick = {
                                    if (panel == null) selection = entry to command
                                    else {
                                        if (panel.first.workspacePanel) panel.first.requestClose()
                                        else panel.first.requestCloseInstance(panel.second)
                                        onDismiss()
                                    }
                                },
                                enabled = if (panel != null) panel.first.canCloseInstance(panel.second)
                                    else !extensions.launchBusy && !extensions.groupSubmissionBusy &&
                                        sessions.connecting == null && (command.togglePanel || extensions.activeSession == null),
                                modifier = Modifier.testTag("terminal-plugin-command-${command.path}"),
                            ) {
                                val label = if (panel != null) "关闭 ${command.path}" else command.path
                                Text("$label · ${command.title} · ${entry.review.id}")
                            }
                        }
                    }
                }
            },
            confirmButton = { TextButton(onClick = onDismiss) { Text("关闭") } },
        )
    } else {
        val (entry, command) = selected
        PluginLaunchDialog(entry, command, hosts.snapshot?.hosts?.map { it.alias }.orEmpty(),
            onDismiss = { selection = null },
            workspacePanel = command.togglePanel,
            onEnqueue = if (command.togglePanel) { alias, request, workspace, temporary ->
                val target = hosts.preparePluginTarget(alias)
                extensions.launchWorkspacePanel(entry, command, target, request, workspace, temporary, sessions, workspaceID)
                onStarted()
                Result.success(Unit)
            } else null,
            onLaunch = { alias, request, workspace ->
                val target = hosts.preparePluginTarget(alias)
                extensions.submitLaunch(entry, command, target, request, sessions, workspace)
                onStarted()
            },
        )
    }
}

@Composable internal fun PluginLaunchDialog(
    entry: ExtensionPackagesViewModel.Entry,
    command: ExtensionController.Command,
    hostAliases: List<String>,
    onDismiss: () -> Unit,
    onLaunch: suspend (String, ExtensionRelayBootstrap.Request, ExtensionController.ClientWorkspace?) -> Unit,
    onEnqueue: (suspend (String, ExtensionRelayBootstrap.Request, ExtensionController.ClientWorkspace?, String) -> Result<Unit>)? = null,
    onInspect: suspend (String, String, String, String) -> PluginForegroundDeclaration = { _, _, _, _ -> error("前台实例检查不可用") },
    workspacePanel: Boolean = false,
    frontendCandidates: List<FrontendCandidate> = emptyList(),
    validateFrontend: (FrontendCandidate) -> Boolean = { !it.expired },
) {
    var groupLaunch by remember { mutableStateOf(workspacePanel) }
    var groupTemporaryDirectory by remember { mutableStateOf("") }
    var selectedHost by remember(entry.review.id, command.path) { mutableStateOf(hostAliases.firstOrNull().orEmpty()) }
    var hostMenu by remember { mutableStateOf(false) }
    var executable by remember { mutableStateOf("gterm") }
    var executorDirectory by remember { mutableStateOf("") }
    var workspaceRootDirectory by remember { mutableStateOf("") }
    var clientDirectory by remember { mutableStateOf("") }
    var executorDigest by remember { mutableStateOf("") }
    var executorVariantID by remember { mutableStateOf("") }
    var useReleaseEvidence by remember { mutableStateOf(false) }
    var releaseDirectory by remember { mutableStateOf("") }
    var releasePolicyPath by remember { mutableStateOf("") }
    var releasePolicySHA256 by remember { mutableStateOf("") }
    var argumentsJson by remember { mutableStateOf("[]") }
    var methodsJson by remember { mutableStateOf("[]") }
    var hostMethodsJson by remember { mutableStateOf("[]") }
    var processProgramsJson by remember { mutableStateOf("[]") }
    var processEnvironmentJson by remember { mutableStateOf("[]") }
    var attachExisting by remember { mutableStateOf(false) }
    val enqueueGroup = onEnqueue.takeIf { groupLaunch && !attachExisting }
    var ipcDirectory by remember { mutableStateOf("") }
    var ipcInstance by remember { mutableStateOf("") }
    var remoteProcesses by remember(entry.review.id, entry.review.digest, command.path, selectedHost, executable, attachExisting, ipcDirectory, ipcInstance) { mutableStateOf(false) }
    var scopedFiles by remember(entry.review.id, entry.review.digest, command.path, selectedHost, executable, attachExisting, ipcDirectory, ipcInstance) { mutableStateOf(false) }
    var attachmentAllowed by remember(selectedHost, executable, executorDigest, command.path, argumentsJson,
        methodsJson, hostMethodsJson, attachExisting, ipcDirectory, ipcInstance, remoteProcesses, scopedFiles) { mutableStateOf(false) }
    var clientAllowed by remember(entry.review.id, entry.review.digest, command.path,
        selectedHost, executable, executorDirectory, executorDigest, executorVariantID,
        attachExisting, ipcDirectory, ipcInstance, remoteProcesses, scopedFiles,
        clientDirectory, workspaceRootDirectory, argumentsJson, methodsJson, hostMethodsJson, processProgramsJson, processEnvironmentJson,
        useReleaseEvidence, releaseDirectory, releasePolicyPath, releasePolicySHA256) { mutableStateOf(false) }

    var releaseEvidenceAllowed by remember(entry.review.id, entry.review.digest, command.path,
        selectedHost, executable, executorDirectory, executorDigest, executorVariantID, attachExisting,
        useReleaseEvidence, releaseDirectory, releasePolicyPath, releasePolicySHA256,
        workspaceRootDirectory, argumentsJson, methodsJson, hostMethodsJson, processProgramsJson, processEnvironmentJson) { mutableStateOf(false) }

    var problem by remember { mutableStateOf<String?>(null) }
    val inspectionKey = listOf(entry.review.id, entry.review.digest, command.path, selectedHost, executable, attachExisting.toString(), ipcDirectory, ipcInstance)
    val currentInspectionKey by rememberUpdatedState(inspectionKey)
    var declaration by remember(inspectionKey) { mutableStateOf<PluginForegroundDeclaration?>(null) }
    var inspectionBusy by remember { mutableStateOf(false) }
    var submissionBusy by remember { mutableStateOf(false) }
    var inspectionJob by remember { mutableStateOf<Job?>(null) }
    val formScope = rememberCoroutineScope()
    DisposableEffect(inspectionKey) { onDispose { inspectionJob?.cancel() } }
    AlertDialog(onDismissRequest = onDismiss,
        properties = DialogProperties(securePolicy = SecureFlagPolicy.SecureOn),
        title = {
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text("启动 ${command.title}")
                problem?.let { Text(it, color = MaterialTheme.colorScheme.error, style = MaterialTheme.typography.bodySmall) }
            }
        },
        text = {
            Column(Modifier.fillMaxWidth().heightIn(max = 480.dp).verticalScroll(rememberScrollState()),
                verticalArrangement = Arrangement.spacedBy(10.dp)) {
                Text(command.path, style = MaterialTheme.typography.bodySmall)
                if (hostAliases.isEmpty()) Text("当前没有可用的 SSH 主机配置。", color = MaterialTheme.colorScheme.error)
                else {
                    Box {
                        OutlinedButton(onClick = { hostMenu = true }, modifier = Modifier.fillMaxWidth().semantics {
                            contentDescription = if (selectedHost.isBlank()) "选择 SSH 主机" else "当前 SSH 主机：$selectedHost，打开选择菜单"
                        }) {
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
                if (!workspacePanel) Row(verticalAlignment = Alignment.CenterVertically) {
                    Checkbox(attachExisting, { attachExisting = it; useReleaseEvidence = false; executorVariantID = "" },
                        Modifier.testTag("plugin-ipc-mode").semantics {
                            contentDescription = if (attachExisting) "已选择连接已有前台 gterm 实例" else "选择连接已有前台 gterm 实例"
                        })
                    Text("连接已有前台 gterm 实例")
                }
                frontendCandidates.filter { it.offer.clientDigest == entry.review.digest }.forEach { candidate ->
                    TextButton(modifier = Modifier.testTag("plugin-discovery-use-${candidate.id}").semantics {
                        contentDescription = "使用终端候选位置：${candidate.sourceDescription}"
                    }, onClick = {
                        if (!validateFrontend(candidate)) problem = "候选提示已过期或来源已停止，请重新运行前台命令。"
                        else {
                            selectedHost = ""
                            problem = null
                            attachExisting = true; useReleaseEvidence = false; executorVariantID = ""; ipcDirectory = candidate.offer.directory; ipcInstance = candidate.offer.instance
                            declaration = null; scopedFiles = false; remoteProcesses = false
                            attachmentAllowed = false; clientAllowed = false
                        }
                    }) { Text("使用终端候选位置：${candidate.sourceDescription}") }
                }
                if (attachExisting) {
                    OutlinedTextField(ipcDirectory, { ipcDirectory = it }, singleLine = true,
                        modifier = Modifier.testTag("plugin-ipc-directory"), label = { Text("远端私有 IPC 目录") })
                    OutlinedTextField(ipcInstance, { ipcInstance = it }, singleLine = true,
                        modifier = Modifier.testTag("plugin-ipc-instance"), label = { Text("前台显示的 32 位实例标识") })
                    TextButton(enabled = !inspectionBusy && !submissionBusy && selectedHost in hostAliases, onClick = {
                        if (inspectionBusy || submissionBusy || selectedHost !in hostAliases || !attachExisting) return@TextButton
                        val capturedKey = inspectionKey
                        val host = selectedHost
                        val program = executable
                        val directory = ipcDirectory
                        val instance = ipcInstance
                        inspectionBusy = true; problem = null; declaration = null
                        inspectionJob = formScope.launch {
                            try {
                                val value = onInspect(host, program, directory, instance)
                                if (isActive && capturedKey == currentInspectionKey) declaration = value
                            } catch (cancelled: CancellationException) { throw cancelled }
                            catch (failure: Exception) {
                                if (isActive && capturedKey == currentInspectionKey) problem = "前台检查失败：${failure.message ?: failure.javaClass.simpleName}"
                            } finally { inspectionBusy = false }
                        }
                    }, modifier = Modifier.testTag("plugin-ipc-inspect").semantics {
                        contentDescription = if (inspectionBusy) "正在检查前台实例" else "检查此位置的前台实例"
                    }) { Text(if (inspectionBusy) "正在检查前台实例…" else "检查此位置的前台实例") }
                    declaration?.let { value ->
                        SelectionContainer { Text("当前声明：${value.command} · ${value.runtime}\nExecutor：${value.executorDigest}\nClient：${value.clientDigest}\n参数：${org.json.JSONArray(value.arguments)}\nExecutor RPC：${value.methods.joinToString()}\nHost RPC：${value.hostMethods.joinToString()}\nClient 服务：${value.clientCapabilities.joinToString()}",
                            Modifier.testTag("plugin-ipc-declaration"), style = MaterialTheme.typography.bodySmall) }
                        TextButton(enabled = !inspectionBusy && value.clientDigest == entry.review.digest && value.command == command.path,
                            modifier = Modifier.testTag("plugin-ipc-adopt"), onClick = {
                                executorDigest = value.executorDigest
                                argumentsJson = org.json.JSONArray(value.arguments).toString()
                                methodsJson = org.json.JSONArray(value.methods).toString()
                                hostMethodsJson = org.json.JSONArray(value.hostMethods).toString()
                                scopedFiles = false; remoteProcesses = false; attachmentAllowed = false
                                clientAllowed = false
                            }) { Text("将核对后的命令和参数填入表单") }
                    }
                    Text("检查只读取当前声明，不启动插件或批准附着。填入后须重新确认连接与已有范围。", style = MaterialTheme.typography.bodySmall)
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Checkbox(scopedFiles, { scopedFiles = it }, Modifier.testTag("plugin-ipc-files").semantics {
                            contentDescription = if (scopedFiles) "已允许前台实例读取指定资源根" else "允许前台实例读取指定资源根"
                        })
                        Text("允许沿用此实例已选工作区的只读文件范围", style = MaterialTheme.typography.bodySmall)
                    }
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Checkbox(remoteProcesses, { remoteProcesses = it }, Modifier.testTag("plugin-ipc-processes").semantics {
                            contentDescription = if (remoteProcesses) "已允许前台实例使用远端业务进程" else "允许前台实例使用远端业务进程"
                        })
                        Text("允许沿用此实例已有的远端业务进程范围", style = MaterialTheme.typography.bodySmall)
                    }
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Checkbox(attachmentAllowed, { attachmentAllowed = it }, Modifier.testTag("plugin-ipc-allow").semantics {
                            contentDescription = if (attachmentAllowed) "已允许附着前台实例" else "允许附着前台实例"
                        })
                        Text("明确连接此主机上的该前台实例", style = MaterialTheme.typography.bodySmall)
                    }
                    Text("请核对前台显示的标识、命令与参数。运行档、工作区与程序映射沿用前台启动配置；失效或不匹配将报错。", style = MaterialTheme.typography.bodySmall)
                } else {
                OutlinedTextField(executorVariantID, { executorVariantID = it }, singleLine = true,
                    modifier = Modifier.testTag("plugin-executor-variant"), label = { Text("Executor variant ID（可选）") })
                Text("填写所选 Executor manifest 中的准确 variant ID。留空时由执行端检查候选是否唯一；不匹配、缺依赖或多候选均报错，不会切换运行档。",
                    style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                Row(verticalAlignment = Alignment.CenterVertically) {
                        Checkbox(useReleaseEvidence, { useReleaseEvidence = it }, Modifier.testTag("plugin-executor-release-mode").semantics {
                            contentDescription = if (useReleaseEvidence) "已选择签名发布证据模式" else "选择签名发布证据模式"
                        })
                    Text("核验执行端已安装版本的签名", style = MaterialTheme.typography.bodySmall)
                }
                if (useReleaseEvidence) {
                    OutlinedTextField(releaseDirectory, { releaseDirectory = it }, singleLine = true,
                        modifier = Modifier.testTag("plugin-executor-release-directory"), label = { Text("执行端签名版本目录") })
                    OutlinedTextField(releasePolicyPath, { releasePolicyPath = it }, singleLine = true,
                        modifier = Modifier.testTag("plugin-executor-release-policy"), label = { Text("执行端信任策略文件") })
                    OutlinedTextField(releasePolicySHA256, { releasePolicySHA256 = it }, singleLine = true,
                        modifier = Modifier.testTag("plugin-executor-release-policy-sha256"), label = { Text("策略 SHA-256") })
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Checkbox(releaseEvidenceAllowed, { releaseEvidenceAllowed = it }, Modifier.testTag("plugin-executor-release-allow").semantics {
                            contentDescription = if (releaseEvidenceAllowed) "已允许使用签名发布证据" else "允许使用签名发布证据"
                        })
                        Text("允许本次使用以上执行端策略核验签名版本", style = MaterialTheme.typography.bodySmall)
                    }
                    Text("填写执行机器上已有的位置与独立核对的策略摘要。执行端会核对签名、发布者、版本、包摘要和策略；不支持或不匹配会报错，校验失败不会回退开发目录。Client 来源批准与 Session 授权仍须单独完成。",
                        style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                }
                OutlinedTextField(executorDirectory, { executorDirectory = it }, singleLine = true,
                    modifier = Modifier.testTag("plugin-executor-directory"), label = { Text("远端 Executor 包目录") })
                OutlinedTextField(workspaceRootDirectory, { workspaceRootDirectory = it }, singleLine = true,
                    modifier = Modifier.testTag("plugin-workspace-root"),
                    label = { Text("可选：允许 Executor 读取的工作区目录") })
                }
                if ("resource.receive" in entry.review.requiredCapabilities) {
                    OutlinedTextField(clientDirectory, { clientDirectory = it }, singleLine = true,
                        modifier = Modifier.testTag("plugin-client-workspace"), label = { Text("Native 本机只读资源目录（可选）") })
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Checkbox(clientAllowed, { clientAllowed = it }, Modifier.testTag("plugin-client-workspace-allow").semantics {
                            contentDescription = if (clientAllowed) "已允许 Native Host 发布本机资源" else "允许 Native Host 发布本机资源"
                        })
                        Text("允许本次 Native Host 发布资源（单文件最多 256 KiB）", style = MaterialTheme.typography.bodySmall)
                    }
                    Text("请选择本应用可访问的普通目录。该目录仅供原生发布按钮使用。修改目录或连接配置须重新允许。",
                        style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                }
                OutlinedTextField(executorDigest, { executorDigest = it }, singleLine = true,
                    modifier = Modifier.testTag("plugin-executor-digest"), label = { Text("Executor 包 SHA-256") })
                OutlinedTextField(argumentsJson, { argumentsJson = it }, minLines = 2, maxLines = 4,
                    label = { Text("命令 argv（JSON 字符串数组）") }, placeholder = { Text("[\"--limit\", \"10\"]") })
                OutlinedTextField(methodsJson, { methodsJson = it }, minLines = 2, maxLines = 4,
                    label = { Text("允许调用的 Executor RPC 方法") }, placeholder = { Text("[\"task.run\"]") })
                OutlinedTextField(hostMethodsJson, { hostMethodsJson = it }, minLines = 2, maxLines = 4,
                    label = { Text("允许 Executor 调用的 Host RPC 方法") }, placeholder = { Text("[\"host.confirm\"]") })
                if (!attachExisting) {
                OutlinedTextField(processProgramsJson, { processProgramsJson = it }, minLines = 2, maxLines = 5,
                    modifier = Modifier.testTag("plugin-process-programs"),
                    label = { Text("远端 Executor 进程映射（可选）") },
                    placeholder = { Text("[{\"name\":\"codex\",\"executable\":\"/path/codex\",\"version\":\"0.166.0\",\"sha256\":\"…\"}]") })
                OutlinedTextField(processEnvironmentJson, { processEnvironmentJson = it }, minLines = 2, maxLines = 4,
                    modifier = Modifier.testTag("plugin-process-environment"),
                    label = { Text("额外执行端环境名称（可选）") }, placeholder = { Text("[\"CLAUDE_CONFIG_DIR\"]") })
                Text("仅填写后才会把该目录作为 workspace 显式传给 Executor；留空时不开放文件读取。将通过 SSH 明确启动指定目录中的 Executor。开发摘要是本次显式信任选择，不代表发布者签名认证；不会自动下载或安装。",
                    style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                Text("argv、RPC 方法和环境名称填写 JSON 字符串数组。进程映射可用 prefixArguments 指定固定参数，须同时提供 workspace root；批准后才请求 process.spawn / process.pipes。环境名称只选择远端执行机器上的值，不传本机值；子进程不会继承任意环境变量。",
                    style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                if (command.path == "ai codex") Text("Codex 会自动登记固定的 thread / turn / approval 与事件 RPC。还需添加 name 为 codex、版本填 codex-cli 0.166.0 的进程映射，并核对可执行文件 SHA-256；否则启动会被拒绝或版本锁校验失败。",
                    style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                if (command.path == "ai claude") Text("请在执行机器准备 Claude CLI 2.1.323，并配置名为 claude 的进程映射。发送消息时启动 CLI。",
                    style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                }
                if (onEnqueue != null && !attachExisting) {
                    if (!workspacePanel) Row(verticalAlignment = Alignment.CenterVertically) {
                        Checkbox(groupLaunch, { groupLaunch = it }, Modifier.testTag("plugin-group-mode").semantics {
                            contentDescription = if (groupLaunch) "已选择加入待启动组" else "选择加入待启动组"
                        })
                        Text("加入同一连接的待启动组")
                    }
                    if (groupLaunch) OutlinedTextField(groupTemporaryDirectory, { groupTemporaryDirectory = it },
                        modifier = Modifier.testTag("plugin-group-temporary"), label = { Text("执行端连接临时目录（绝对路径）") })
                }
            }
        },
        confirmButton = {
            TextButton(enabled = !inspectionBusy && !submissionBusy && selectedHost in hostAliases && executable.isNotBlank() && (attachExisting || executorDirectory.isNotBlank()), onClick = {
                if (inspectionBusy || submissionBusy || selectedHost !in hostAliases) return@TextButton
                problem = null
                try {
                    val request = ExtensionRelayBootstrap.Request(executable.trim(), if (attachExisting) "" else executorDirectory,
                        executorDigest.trim(), entry.review.digest, ExtensionRelayBootstrap.parseMethodsJson(methodsJson),
                        ExtensionRelayBootstrap.parseHostMethodsJson(hostMethodsJson), command.path,
                        ExtensionRelayBootstrap.parseArgumentsJson(argumentsJson),
                        workspaceRootDirectory.takeUnless { attachExisting || it.isBlank() },
                        if (attachExisting) emptyList() else ExtensionRelayBootstrap.parseProcessProgramsJson(processProgramsJson),
                        if (attachExisting) emptyList() else ExtensionRelayBootstrap.parseProcessEnvironmentJson(processEnvironmentJson),
                        attachment = if (attachExisting) ExtensionRelayBootstrap.Attachment(
                            ipcDirectory, ipcInstance, attachmentAllowed, remoteProcesses, scopedFiles) else null,
                        releaseEvidence = if (!attachExisting && useReleaseEvidence) ExtensionRelayBootstrap.ReleaseEvidence(
                            releaseDirectory, releasePolicyPath, releasePolicySHA256, releaseEvidenceAllowed) else null,
                        variantID = executorVariantID.takeUnless { attachExisting || it.isEmpty() })
                    if (enqueueGroup == null) ExtensionRelayBootstrap.build(request)
                    val clientWorkspace = clientDirectory.takeUnless { it.isEmpty() }?.let {
                        ExtensionController.ClientWorkspace(it, clientAllowed).also { grant -> grant.validate(entry.review.requiredCapabilities) }
                    }
                    val alias = selectedHost
                    val temporary = groupTemporaryDirectory
                    submissionBusy = true
                    formScope.launch {
                        try {
                            if (enqueueGroup != null) {
                                enqueueGroup(alias, request, clientWorkspace, temporary).getOrThrow()
                            } else {
                                onLaunch(alias, request, clientWorkspace)
                            }
                        } catch (cancelled: CancellationException) {
                            throw cancelled
                        } catch (failure: Exception) {
                            problem = failure.message ?: "插件启动预检失败"
                        } finally { submissionBusy = false }
                    }
                } catch (failure: IllegalArgumentException) {
                    problem = failure.message ?: "插件启动参数无效"
                }
            }, modifier = Modifier.semantics {
                contentDescription = when {
                    submissionBusy -> "正在提交插件启动请求"
                    workspacePanel -> "连接并请求插件授权"
                    enqueueGroup != null -> "加入待启动插件连接组"
                    else -> "连接并请求插件授权"
                }
            }) { Text(when {
                submissionBusy -> "正在提交…"
                workspacePanel -> "连接并请求授权"
                enqueueGroup != null -> "加入待启动组"
                else -> "连接并请求授权"
            }) }
        },
        dismissButton = { TextButton(onClick = onDismiss, modifier = Modifier.semantics {
            contentDescription = "取消插件启动"
        }) { Text("取消") } })
}

@Composable private fun PluginCodexPanel(extensions: ExtensionPackagesViewModel) {
    val state = extensions.codex
    val review by extensions.changeReview.state.collectAsState()
    var showEvents by remember { mutableStateOf(false) }
    Column(Modifier.fillMaxWidth().padding(top = 12.dp).testTag("plugin-codex-panel"),
        verticalArrangement = Arrangement.spacedBy(8.dp)) {
        Text("Codex 对话", style = MaterialTheme.typography.titleSmall)
        state.threadID?.let { Text("Thread：$it", Modifier.testTag("plugin-codex-thread"), style = MaterialTheme.typography.bodySmall) }
        if (state.timeline.isNotEmpty()) {
            Text("对话与工具活动", style = MaterialTheme.typography.labelLarge)
            state.timeline.forEach { item ->
                Card(Modifier.fillMaxWidth().testTag("plugin-codex-timeline-${item.id}")) {
                    Column(Modifier.fillMaxWidth().padding(10.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                        Text(item.title, style = MaterialTheme.typography.labelLarge)
                        if (item.fileChanges.isNotEmpty()) PluginCodexFileChanges(item.fileChanges, item.omittedChangeCount)
                        else if (item.detail.isNotEmpty()) SelectionContainer {
                            Text(item.detail, style = MaterialTheme.typography.bodySmall,
                                fontFamily = androidx.compose.ui.text.font.FontFamily.Monospace)
                        }
                        item.changeReference?.let { CodexChangeSnapshotControl(extensions, it) }
                        if (item.changeUnavailable) Text("完整补丁超过读取上限，当前只有部分预览。", style = MaterialTheme.typography.bodySmall)
                    }
                }
            }
        }
        if (state.approvals.isNotEmpty()) {
            Text("等待中的 Codex 批准请求", style = MaterialTheme.typography.labelLarge)
            state.approvals.forEach { approval ->
                val snapshotID = extensions.codexApprovalSnapshot(approval, review)
                Card(Modifier.fillMaxWidth().testTag("plugin-codex-approval-${approval.id}")) {
                    Column(Modifier.fillMaxWidth().padding(10.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                        Text(approval.method, style = MaterialTheme.typography.labelMedium)
                        if (approval.method == "item/fileChange/requestApproval") {
                            val item = approval.itemID?.let { id -> state.timeline.firstOrNull { it.id == "item:$id" } }
                            if (item != null) {
                                if (item.fileChanges.isNotEmpty()) PluginCodexFileChanges(item.fileChanges, item.omittedChangeCount)
                                item.changeReference?.let { CodexChangeSnapshotControl(extensions, it) }
                                if (item.changeUnavailable) Text("完整补丁超过读取上限，当前只有部分预览。", style = MaterialTheme.typography.bodySmall)
                            } else Text("正在等待 Codex 提供补丁内容。", style = MaterialTheme.typography.bodySmall,
                                color = MaterialTheme.colorScheme.onSurfaceVariant)
                        } else SelectionContainer {
                            Text(approval.paramsJSON, style = MaterialTheme.typography.bodySmall,
                                fontFamily = androidx.compose.ui.text.font.FontFamily.Monospace)
                        }
                        if (approval.method == "item/permissions/requestApproval") {
                            Text("以下完整权限范围仅用于当前轮，可能允许范围内后续操作。", style = MaterialTheme.typography.bodySmall)
                        }
                        ConversationDecisions("plugin-codex-approval", state.callBusy || !extensions.canInteractWithRuntime,
                            approval.method == "item/commandExecution/requestApproval" || snapshotID != null,
                            allowLabel = if (approval.method == "item/permissions/requestApproval") "允许本轮所列权限" else "批准一次",
                            deny = { extensions.respondCodexApproval(approval.id, false, snapshotID) },
                            allow = { extensions.respondCodexApproval(approval.id, true, snapshotID) })
                        if (approval.method == "item/fileChange/requestApproval" && snapshotID == null) {
                            Text("先读取并核对当前完整补丁，才能批准此版本。", style = MaterialTheme.typography.bodySmall)
                        }
                    }
                }
            }
        }
        if (state.threadID == null) {
            TextButton(onClick = extensions::startCodexThread, enabled = extensions.canInteractWithRuntime && !state.callBusy,
                modifier = Modifier.testTag("plugin-codex-start-thread")) {
                Text(if (state.callBusy) "正在启动…" else "启动 Codex 对话")
            }
        } else {
            ConversationComposer(state.messageDraft, extensions::editCodexMessage, state.callBusy,
                !state.turnActive && !state.sendUnconfirmed, "plugin-codex", "发送给 Codex", extensions::sendCodexMessage,
                enabled = extensions.canInteractWithRuntime)
            if (state.sendUnconfirmed && !state.callBusy) {
                Text("上一条消息的发送结果未知，当前会话无法再次发送。",
                    Modifier.testTag("plugin-codex-send-unconfirmed"), style = MaterialTheme.typography.bodySmall)
            }
            if (state.turnActive) TextButton(onClick = extensions::interruptCodexTurn,
                enabled = extensions.canInteractWithRuntime && !state.callBusy, modifier = Modifier.testTag("plugin-codex-interrupt")) { Text("中断当前 turn") }
        }
        if (state.rawEvents.isNotEmpty()) {
            TextButton(onClick = { showEvents = !showEvents }, modifier = Modifier.testTag("plugin-codex-raw-toggle")) {
                Text(if (showEvents) "收起协议事件" else "最近原始协议事件（${state.rawEvents.size}）")
            }
            if (showEvents) Text("仅显示近期原始事件；待处理审批单独保留。", style = MaterialTheme.typography.bodySmall)
            if (showEvents) state.rawEvents.forEachIndexed { index, event ->
                SelectionContainer { Text(event, Modifier.testTag("plugin-codex-raw-$index"),
                    style = MaterialTheme.typography.bodySmall, fontFamily = androidx.compose.ui.text.font.FontFamily.Monospace) }
            }
        }
    }
}

@Composable private fun PluginCodexFileChanges(changes: List<CodexFileChange>, omittedCount: Int) {
    Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
        changes.forEach { change ->
            Card(colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceVariant)) {
                Column(Modifier.fillMaxWidth().padding(8.dp), verticalArrangement = Arrangement.spacedBy(2.dp)) {
                    Text("${when (change.kind) { "add" -> "新增"; "delete" -> "删除"; else -> "修改" }} · ${change.path}" +
                        (change.movePath?.let { " → $it" } ?: ""), style = MaterialTheme.typography.labelMedium,
                        fontFamily = FontFamily.Monospace)
                    change.lines.forEach { line ->
                        val color = when (line.kind) {
                            CodexDiffLine.Kind.ADDED -> MaterialTheme.colorScheme.primary
                            CodexDiffLine.Kind.REMOVED -> MaterialTheme.colorScheme.error
                            CodexDiffLine.Kind.HUNK -> MaterialTheme.colorScheme.tertiary
                            CodexDiffLine.Kind.CONTEXT -> MaterialTheme.colorScheme.onSurfaceVariant
                        }
                        SelectionContainer {
                            Text(line.text.ifEmpty { " " }, style = MaterialTheme.typography.bodySmall,
                                fontFamily = FontFamily.Monospace, color = color)
                        }
                    }
                    if (change.diffTruncated) Text("补丁内容较长，当前显示部分预览。",
                        style = MaterialTheme.typography.labelSmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                }
            }
        }
        if (omittedCount > 0) Text("另有 $omittedCount 个文件未包含在本次预览中。",
            style = MaterialTheme.typography.labelSmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
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

@Composable internal fun PluginActiveInstanceContent(extensions: ExtensionPackagesViewModel) {
    extensions.error?.let { Text(it, color = MaterialTheme.colorScheme.error) }
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
    if (extensions.activeSession?.clientWorkspace != null && extensions.activeRuntime != null) {
        PluginNativeResourcePublisher(extensions)
    }
    extensions.activeRuntime?.let { active ->
        Text((if (extensions.runtimeStateIsUnknown) "Runtime 最后观测（当前状态未知）：" else "Runtime：") + "${active.info.phase} · ${active.command} · steps ${active.info.steps}",
            Modifier.padding(top = 4.dp).testTag("plugin-runtime-state").semantics {
                contentDescription = "${if (extensions.runtimeStateIsUnknown) "Runtime 状态未知，最后观测" else "Runtime 当前状态"}：${active.info.phase}，命令 ${active.command}"
            }, style = MaterialTheme.typography.bodySmall)
        if (active.info.phase == com.hanxi.ghosterm.bridge.ExtensionRuntime.Phase.FAILED ||
            active.info.sessionEnd != com.hanxi.ghosterm.bridge.ExtensionSession.End.NONE ||
            active.info.sessionError != 0 || active.info.cleanupError != 0) {
            Text("Runtime 结果：${active.info.error} · Session ${active.info.sessionEnd}/${active.info.sessionError} · 清理 ${active.info.cleanupError}",
                Modifier.testTag("plugin-runtime-result"), style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.error)
        }
        PluginResourcesPanel(extensions.receivedResources)
        extensions.pendingPluginForm?.takeIf { extensions.canInteractWithRuntime }?.let { pending ->
            key(pending.token) {
                PluginFormContent(pending.form, active.packageID, extensions.answeringPluginForm) { answer ->
                    extensions.answerPluginForm(pending.token, answer)
                }
            }
        }
        if (active.command == "ai codex") PluginCodexPanel(extensions)
        if (active.command == "ai claude") extensions.claudeConversation?.let { ClaudeConversationContent(it) }
        extensions.table?.let { PluginTableContent(it) }
        extensions.diff?.let { PluginDiffContent(it) }
        extensions.task?.let { PluginTaskContent(it) }
        extensions.registeredViews?.let { PluginViewsContent(it) }
        if (active.info.phase in setOf(com.hanxi.ghosterm.bridge.ExtensionRuntime.Phase.CREATED,
                com.hanxi.ghosterm.bridge.ExtensionRuntime.Phase.ACTIVE,
                com.hanxi.ghosterm.bridge.ExtensionRuntime.Phase.STARTING)) {
            TextButton(onClick = extensions::stopRuntime, enabled = !extensions.launchBusy,
                modifier = Modifier.semantics { contentDescription = "停止插件 Runtime" }) { Text("停止插件") }
        } else {
            TextButton(onClick = extensions::releaseFinished, enabled = !extensions.launchBusy,
                modifier = Modifier.semantics { contentDescription = "释放插件 Runtime 资源" }) { Text("释放插件资源") }
        }
    }
    if (extensions.activeRuntime == null && (extensions.activeSession?.info?.phase ==
            com.hanxi.ghosterm.bridge.ExtensionSession.Phase.ENDED || extensions.activeSession?.info?.phase ==
            com.hanxi.ghosterm.bridge.ExtensionSession.Phase.CLOSED)) {
        TextButton(onClick = extensions::releaseFinished, enabled = !extensions.launchBusy,
            modifier = Modifier.semantics { contentDescription = "释放已结束的插件 Session" }) { Text("释放插件 Session") }
    }
    if (extensions.activeRuntime == null && extensions.activeSession?.info?.phase ==
        com.hanxi.ghosterm.bridge.ExtensionSession.Phase.ACTIVE) {
        TextButton(onClick = extensions::stopSession, enabled = !extensions.launchBusy,
            modifier = Modifier.semantics { contentDescription = "停止活动插件 Session" }) { Text("停止插件 Session") }
    }

}
