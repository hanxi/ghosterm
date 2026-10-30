package com.hanxi.ghosterm

import android.text.format.Formatter
import androidx.activity.compose.BackHandler
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.Alignment
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.selected
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.hanxi.ghosterm.domain.*

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun FilesScreen(model: FilesViewModel, pickFolder: () -> Unit) {
    var menu by remember { mutableStateOf(false) }
    val context=LocalContext.current
    BackHandler { if(menu) menu=false else model.back() }
    val preview=model.preview
    Scaffold(topBar={
        TopAppBar(title={ Text(if(preview!=null) preview.entry.name else if(model.selecting) "已选 ${model.selected.size} 项" else "文件",
            maxLines=1,overflow=TextOverflow.Ellipsis) },navigationIcon={ TextButton(onClick=model::back) { Text("返回") } },actions={
            if(preview==null) {
                if(model.selecting) TextButton(onClick=model::clearSelection) { Text("取消选择") }
                else TextButton(onClick=model::selectMode,enabled=!model.loading && model.error==null) { Text("选择") }
                Box {
                    TextButton(onClick={ menu=true }) { Text("更多") }
                    DropdownMenu(expanded=menu,onDismissRequest={ menu=false }) {
                        DropdownMenuItem(text={ Text("刷新") },onClick={ menu=false;model.reload() })
                        DropdownMenuItem(text={ Text(if(model.showHidden) "隐藏点文件" else "显示隐藏文件") },onClick={ menu=false;model.toggleHidden() })
                    }
                }
            }
        })
    }) { padding ->
        if(preview!=null) {
            Column(Modifier.fillMaxSize().padding(padding).consumeWindowInsets(padding).padding(16.dp)) {
                Text("只读预览 · UTF-8 文本",style=MaterialTheme.typography.labelMedium)
                if(preview.error!=null) Text(preview.error,color=MaterialTheme.colorScheme.error,modifier=Modifier.padding(top=16.dp).testTag("file-preview-error"))
                else if(preview.text==null) LinearProgressIndicator(Modifier.fillMaxWidth().padding(top=16.dp))
                else SelectionContainer(Modifier.fillMaxWidth().weight(1f).verticalScroll(rememberScrollState())) {
                    Text(preview.text, fontFamily=FontFamily.Monospace,modifier=Modifier.padding(vertical=16.dp).testTag("file-preview"))
                }
            }
        } else Column(Modifier.fillMaxSize().padding(padding).consumeWindowInsets(padding)) {
            TextButton(onClick=model::chooseLocations,modifier=Modifier.fillMaxWidth().testTag("file-location")) { Text("本机 · ${model.location.endpoint.label}") }
            Text(model.crumbs.joinToString(" / ") { it.label },Modifier.fillMaxWidth().padding(horizontal=16.dp,vertical=8.dp).testTag("file-breadcrumb"),
                style=MaterialTheme.typography.bodySmall,maxLines=2,overflow=TextOverflow.Ellipsis)
            when {
                model.loading -> LinearProgressIndicator(Modifier.fillMaxWidth())
                model.error!=null -> Column(Modifier.padding(16.dp)) {
                    Text(model.error!!,color=MaterialTheme.colorScheme.error,modifier=Modifier.testTag("file-list-error"))
                    TextButton(onClick=model::reload) { Text("重试") }
                    TextButton(onClick=model::chooseLocations) { Text("更换位置") }
                }
                model.visibleEntries.isEmpty() -> Text("此文件夹没有可见项目",Modifier.padding(24.dp).testTag("file-list-empty"))
            }
            LazyColumn(Modifier.weight(1f).fillMaxWidth().testTag("file-list")) {
                items(model.visibleEntries,key={ it.id }) { entry ->
                    val checked=entry.id in model.selected
                    ListItem(modifier=Modifier.fillMaxWidth().heightIn(min=64.dp).testTag("file-entry:${entry.name}")
                        .semantics { selected=checked }.combinedClickable(role=if(model.selecting) Role.Checkbox else Role.Button,
                            onClick={ model.tap(entry) },onLongClick={ model.longPress(entry) },onLongClickLabel="选择此项目"),
                        headlineContent={ Text(entry.name,maxLines=2,overflow=TextOverflow.Ellipsis) },
                        supportingContent={ Text(when(entry.kind) {
                            FileKind.DIRECTORY -> "文件夹"
                            FileKind.LINK -> "符号链接"
                            FileKind.OTHER -> "特殊文件"
                            FileKind.FILE -> entry.size?.let { Formatter.formatShortFileSize(context,it) } ?: "大小未知"
                        }) },leadingContent={ if(model.selecting) Checkbox(checked=checked,onCheckedChange=null) else Text(if(entry.kind==FileKind.DIRECTORY) "▸" else "·") })
                    HorizontalDivider()
                }
            }
            if(model.selecting) Row(Modifier.fillMaxWidth().padding(horizontal=16.dp),verticalAlignment=Alignment.CenterVertically) {
                Text("${model.selected.size} 项",Modifier.weight(1f))
                TextButton(onClick=model::all) { Text("全选") };TextButton(onClick=model::clearChecked) { Text("清除选择") }
            }
        }
    }
    if(model.choosingLocation) ModalBottomSheet(onDismissRequest=model::dismissLocations) {
        Column(Modifier.fillMaxWidth().verticalScroll(rememberScrollState()).padding(16.dp)) {
            Text("文件位置",style=MaterialTheme.typography.titleLarge)
            model.endpointError?.let { Text(it,color=MaterialTheme.colorScheme.error) }
            model.endpoints.forEach { endpoint -> TextButton(onClick={ model.choose(endpoint) },modifier=Modifier.fillMaxWidth()) { Text(endpoint.label) } }
            OutlinedButton(onClick=pickFolder,enabled=!model.pickerPending,modifier=Modifier.fillMaxWidth()) { Text("选择设备文件夹") }
        }
    }
}
