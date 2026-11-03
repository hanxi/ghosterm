package com.hanxi.ghosterm

import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.selection.selectable
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.lifecycle.viewmodel.compose.viewModel
import com.hanxi.ghosterm.domain.*

@OptIn(ExperimentalMaterial3Api::class)
@Composable
internal fun TerminalThemeSheet(selected:TerminalTheme,enabled:Boolean,onDismiss:()->Unit,choose:(TerminalTheme)->Unit) {
    var query by rememberSaveable { mutableStateOf("") }
    var filter by rememberSaveable { mutableStateOf("all") }
    var filters by remember { mutableStateOf(false) }
    val compact=LocalConfiguration.current.screenHeightDp<400 || LocalConfiguration.current.fontScale>=1.5f
    val visible=remember(query,filter) { TerminalTheme.entries.filter {
        it.label.contains(query,ignoreCase=true) && (filter=="all" || it.light==(filter=="light"))
    } }
    ModalBottomSheet(onDismissRequest=onDismiss,dragHandle={ GhostermSheetHandle() },sheetState=rememberModalBottomSheetState(skipPartiallyExpanded=true)) {
        Column(Modifier.fillMaxWidth().fillMaxHeight(if(compact) 1f else .9f).padding(horizontal=18.dp)) {
            Row(Modifier.fillMaxWidth().heightIn(min=48.dp),verticalAlignment=Alignment.CenterVertically) {
                Text("终端配色",Modifier.weight(1f),style=MaterialTheme.typography.titleLarge)
                if(compact) Box {
                    GhostermIconButton(UiIcon.Grid,"筛选主题",onClick={ filters=true },modifier=Modifier.testTag("theme-filter-menu"))
                    DropdownMenu(expanded=filters,onDismissRequest={ filters=false }) {
                        listOf("all" to "全部","dark" to "深色","light" to "浅色").forEach { (id,label) ->
                            DropdownMenuItem(text={ Text(label) },onClick={ filter=id;filters=false },modifier=Modifier.testTag("theme-filter-$id"))
                        }
                    }
                }
                GhostermIconButton(UiIcon.Close,"取消",onClick=onDismiss)
            }
            if(!compact) {
                Text("当前：${selected.label} · ${TerminalTheme.entries.size} 套配色",style=MaterialTheme.typography.bodySmall,color=MaterialTheme.colorScheme.onSurfaceVariant)
                Spacer(Modifier.height(12.dp))
            }
            GhostermSearchField(query,{ query=it },Modifier.testTag("theme-search"),placeholder="搜索主题名称")
            if(!compact) Row(horizontalArrangement=Arrangement.spacedBy(8.dp)) {
                listOf("all" to "全部","dark" to "深色","light" to "浅色").forEach { (id,label) ->
                    FilterChip(selected=filter==id,onClick={ filter=id },label={ Text(label) },modifier=Modifier.heightIn(min=48.dp).testTag("theme-filter-$id"))
                }
            }
            if(visible.isEmpty()) Text("没有找到主题",Modifier.padding(vertical=16.dp).testTag("theme-empty"))
            LazyColumn(Modifier.weight(1f).testTag("theme-list")) {
                items(visible,key={ it.id }) { theme ->
                    Row(Modifier.fillMaxWidth().heightIn(min=84.dp).testTag("settings-option-terminalTheme-${theme.id}")
                        .selectable(selected.id==theme.id,enabled=enabled,role=Role.RadioButton,onClick={ choose(theme) }).padding(vertical=8.dp),
                        verticalAlignment=Alignment.CenterVertically,horizontalArrangement=Arrangement.spacedBy(12.dp)) {
                        Column(Modifier.width(82.dp).background(Color(0xff000000.toInt() or theme.background),MaterialTheme.shapes.extraSmall).padding(8.dp)) {
                            Text("$ Aa",color=Color(0xff000000.toInt() or theme.foreground),fontFamily=FontFamily.Monospace,style=MaterialTheme.typography.bodyMedium)
                            Row(Modifier.fillMaxWidth().height(5.dp)) { theme.palette.forEach { c -> Box(Modifier.weight(1f).fillMaxHeight().background(Color(0xff000000.toInt() or c))) } }
                        }
                        Column(Modifier.weight(1f)) {
                            Text(theme.label,style=MaterialTheme.typography.titleSmall)
                            Text(if(theme.light) "浅色" else "深色",style=MaterialTheme.typography.bodySmall,color=MaterialTheme.colorScheme.onSurfaceVariant)
                        }
                        RadioButton(selected.id==theme.id,onClick=null,enabled=enabled)
                    }
                    GhostermDivider()
                }
            }
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
internal fun TerminalFontSheet(settings:SettingsViewModel,onDismiss:()->Unit,choose:(TerminalFont)->Unit,
    fonts:TerminalFontsViewModel=viewModel()) {
    var removal by remember { mutableStateOf<TerminalFontChoice?>(null) }
    val picker=rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument(),fonts::completeImport)
    LaunchedEffect(Unit) { fonts.refresh() }
    val selected=settings.candidate.fontFamily
    val committed by settings.settings.state.collectAsState()
    val enabled=settings.canEdit && !fonts.busy && !fonts.pickerPending
    val dismiss={ fonts.cancelImport();onDismiss() }
    val compact=LocalConfiguration.current.screenHeightDp<400 || LocalConfiguration.current.fontScale>=1.5f
    ModalBottomSheet(onDismissRequest=dismiss,dragHandle={ GhostermSheetHandle() },sheetState=rememberModalBottomSheetState(skipPartiallyExpanded=true)) {
        Column(Modifier.fillMaxWidth().heightIn(max=(LocalConfiguration.current.screenHeightDp*(if(compact) 1f else .9f)).dp).padding(horizontal=18.dp)) {
            Row(Modifier.fillMaxWidth().heightIn(min=48.dp),verticalAlignment=Alignment.CenterVertically) {
                Text("终端字体",Modifier.weight(1f),style=MaterialTheme.typography.titleLarge)
                GhostermIconButton(UiIcon.Close,"取消",onClick=dismiss)
            }
            if(!compact) Text("正文使用等宽字体，中文、Emoji 和终端符号保留回退。",style=MaterialTheme.typography.bodySmall,color=MaterialTheme.colorScheme.onSurfaceVariant)
            if(fonts.busy) LinearProgressIndicator(Modifier.fillMaxWidth().padding(vertical=8.dp))
            fonts.error?.let { Text(it,color=MaterialTheme.colorScheme.error,modifier=Modifier.testTag("font-error")) }
            fonts.notice?.let { Text(it,style=MaterialTheme.typography.bodySmall,modifier=Modifier.testTag("font-notice")) }
            LazyColumn(Modifier.weight(1f,fill=false).testTag("font-list")) {
                items(fonts.choices,key={ it.value.id }) { entry ->
                    Row(Modifier.fillMaxWidth().heightIn(min=94.dp).testTag("settings-option-fontFamily-${entry.value.id}")
                        .selectable(selected==entry.value,enabled=enabled && entry.problem==null,role=Role.RadioButton,onClick={ choose(entry.value) })
                        .padding(vertical=10.dp),verticalAlignment=Alignment.CenterVertically) {
                        RadioButton(selected==entry.value,onClick=null,enabled=enabled && entry.problem==null)
                        Column(Modifier.weight(1f).padding(horizontal=8.dp),verticalArrangement=Arrangement.spacedBy(4.dp)) {
                            Text(entry.label,style=MaterialTheme.typography.titleSmall,maxLines=2,overflow=TextOverflow.Ellipsis)
                            Text(if(entry.value.id.startsWith("custom:")) "导入字体" else if(entry.value==TerminalFont.SYSTEM) "Android 系统" else "内置字体",
                                style=MaterialTheme.typography.bodySmall,color=MaterialTheme.colorScheme.onSurfaceVariant)
                            if(entry.typeface!=null) Text("Aa 0O 1l { } 终端",fontFamily=FontFamily(entry.typeface),style=MaterialTheme.typography.bodyMedium)
                            entry.problem?.let { Text(it,color=MaterialTheme.colorScheme.error,style=MaterialTheme.typography.bodySmall) }
                        }
                        if(entry.value.id.startsWith("custom:")) GhostermIconButton(UiIcon.Close,"移除 ${entry.label}",onClick={ removal=entry },
                            enabled=enabled && settings.inFlight==0 && entry.value!=selected && entry.value!=committed.committed.fontFamily,
                            modifier=Modifier.testTag("font-remove-${entry.value.id}"))
                    }
                    GhostermDivider()
                }
            }
            Row(Modifier.fillMaxWidth(),verticalAlignment=Alignment.CenterVertically) {
                TextButton(onClick=fonts::refresh,enabled=!fonts.busy && !fonts.pickerPending,modifier=Modifier.heightIn(min=48.dp)) { Text("重新载入") }
                Button(onClick={ if(fonts.beginImport()) try { picker.launch(arrayOf("*/*")) } catch(_:Exception) { fonts.importLaunchFailed() } },enabled=enabled,
                    modifier=Modifier.weight(1f).heightIn(min=48.dp).testTag("font-import")) { GhostermIcon(UiIcon.Plus);Spacer(Modifier.width(8.dp));Text("导入字体") }
            }
            if(!compact) Text("TTF / OTF · 每个最多 4 MiB · 导入副本保存在应用内",style=MaterialTheme.typography.bodySmall,color=MaterialTheme.colorScheme.onSurfaceVariant)
        }
    }
    removal?.let { entry -> AlertDialog(onDismissRequest={ removal=null },title={ Text("移除 ${entry.label}？") },
        text={ Text("删除应用内的字体副本，原文件保留。正在使用的字体需先切换后才能移除。") },
        confirmButton={ TextButton(onClick={ removal=null;fonts.remove(entry.value,settings.settings) },modifier=Modifier.testTag("font-remove-confirm")) { Text("移除字体") } },
        dismissButton={ TextButton(onClick={ removal=null }) { Text("保留") } }) }
}
