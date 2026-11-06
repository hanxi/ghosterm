package com.hanxi.ghosterm

import androidx.activity.compose.BackHandler
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.unit.dp
import com.hanxi.ghosterm.bridge.*

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun TmuxScreen(model: TmuxViewModel) {
    var advanced by remember { mutableStateOf(false) }
    var create by remember { mutableStateOf(false) }
    var rename by remember { mutableStateOf<RemoteTmuxEntry?>(null) }
    var terminate by remember { mutableStateOf<RemoteTmuxEntry?>(null) }
    var leave by remember { mutableStateOf(false) }
    fun back() { if(model.changing)leave=true else model.leave() }
    BackHandler(onBack=::back)
    Scaffold(topBar={ GhostermTopBar(title={ Text(model.alias+if(model.unified) " · 连接主机" else " · 后台会话") },
        navigationIcon={ GhostermIconButton(UiIcon.Back,"返回",onClick=::back) }) }) { padding ->
        LazyColumn(Modifier.fillMaxSize().padding(padding).consumeWindowInsets(padding).testTag("tmux-page"),
            contentPadding=PaddingValues(16.dp),verticalArrangement=Arrangement.spacedBy(12.dp)) {
            if(model.unified)item {
                Button(onClick=model::openTerminal,enabled=!model.changing,modifier=Modifier.fillMaxWidth().testTag("remote-open-terminal")) { Text("打开普通终端") }
                Row {
                    Checkbox(checked=model.directTerminal,onCheckedChange=model::updateDirectTerminal,modifier=Modifier.testTag("remote-direct-terminal"))
                    Text("下次直接打开普通终端",modifier=Modifier.padding(top=12.dp))
                }
                Text("可从主机菜单的连接选项重新选择。",style=MaterialTheme.typography.bodySmall)
                HorizontalDivider()
                Text("后台会话",style=MaterialTheme.typography.titleMedium)
            }
            item {
                Text("关闭终端仅断开当前客户端，任务保留遵循远端 tmux 与系统策略。“结束会话”会终止其中的任务。",
                    style=MaterialTheme.typography.bodyMedium)
            }
            if(model.busy)item { LinearProgressIndicator(Modifier.fillMaxWidth()) }
            model.error?.let { message -> item { Text(message,color=MaterialTheme.colorScheme.error,modifier=Modifier.testTag("tmux-error")) } }
            model.notice?.let { message -> item { Text(message,modifier=Modifier.testTag("tmux-notice")) } }
            item {
                FlowRow(horizontalArrangement=Arrangement.spacedBy(8.dp)) {
                    OutlinedButton(onClick=model::probe,enabled=model.canRefresh) { Text("重新探测") }
                    OutlinedButton(onClick=model::refresh,enabled=model.canRefresh && model.selectedPath!=null) { Text("刷新列表") }
                    OutlinedButton(onClick=model::reconnect,enabled=!model.busy && !model.connecting) { Text("重新连接") }
                    Button(onClick={ create=true },enabled=model.canChange,modifier=Modifier.testTag("tmux-create")) { Text("新建会话") }
                }
            }
            if(model.candidates.isNotEmpty())item { TextButton(onClick={ advanced=!advanced }) { Text(if(advanced) "收起 tmux 路径" else "高级：tmux 路径") } }
            if(advanced)items(model.candidates,key={ it.path }) { candidate ->
                OutlinedCard(Modifier.fillMaxWidth()) {
                    Column(Modifier.padding(12.dp),verticalArrangement=Arrangement.spacedBy(8.dp)) {
                        Text(candidate.path,style=MaterialTheme.typography.bodyMedium)
                        Text(if(candidate.version.isEmpty()) "版本无法确认" else candidate.version)
                        if(!candidate.usable)Text(if(candidate.status==7) "此版本不支持" else "可用性无法确认（错误 "+candidate.status+"）")
                        TextButton(onClick={ model.select(candidate.path) },enabled=candidate.usable && model.canRefresh) {
                            Text(if(model.selectedPath==candidate.path) "当前路径" else "使用此路径")
                        }
                    }
                }
            }
            if(!model.busy && !model.connecting && model.error==null && model.entries.isEmpty())item {
                Text(when(model.server) {
                    RemoteTmuxServer.ABSENT -> "tmux 服务器尚未启动，可以新建后台会话。"
                    RemoteTmuxServer.RUNNING -> "当前没有后台会话。"
                    null -> "刷新列表后选择后台会话。"
                })
            }
            items(model.entries,key={ it.id.toString() }) { entry ->
                Card(Modifier.fillMaxWidth().testTag("tmux-entry-"+entry.id)) {
                    Column(Modifier.padding(12.dp),verticalArrangement=Arrangement.spacedBy(8.dp)) {
                        Text(entry.name,style=MaterialTheme.typography.titleMedium)
                        Text(entry.windows.toString()+" 个窗口 · "+entry.attached+" 个客户端")
                        if(entry.destroyUnattached!=0 || entry.exitUnattached!=0) {
                            Text(if(entry.destroyUnattached<0 || entry.exitUnattached<0) "退出后保留策略无法确认"
                                else "远端配置了客户端离开后的结束策略，请确认任务保留要求",
                                color=MaterialTheme.colorScheme.error)
                        }
                        FlowRow(horizontalArrangement=Arrangement.spacedBy(8.dp)) {
                            Button(onClick={ model.attach(entry) },enabled=model.canChange,modifier=Modifier.testTag("tmux-attach-"+entry.id)) { Text("连接") }
                            OutlinedButton(onClick={ rename=entry },enabled=model.canChange) { Text("重命名") }
                            TextButton(onClick={ terminate=entry },enabled=model.canChange) { Text("结束会话") }
                        }
                    }
                }
            }
        }
    }
    if(create)TmuxNameDialog("新建后台会话","",onDismiss={ create=false }) { name -> create=false;model.create(name) }
    rename?.let { entry -> TmuxNameDialog("重命名后台会话",entry.name,onDismiss={ rename=null }) { name ->
        rename=null;model.rename(entry,name)
    } }
    terminate?.let { entry ->
        AlertDialog(onDismissRequest={ terminate=null },title={ Text("结束 "+entry.name+"？") },
            text={ Text("这会终止该后台会话中的所有窗口和任务，无法撤销。仅需离开时，请关闭终端客户端。") },
            confirmButton={ TextButton(onClick={ terminate=null;model.terminate(entry) },modifier=Modifier.testTag("tmux-terminate-confirm")) { Text("结束会话") } },
            dismissButton={ TextButton(onClick={ terminate=null }) { Text("取消") } })
    }
    if(leave)AlertDialog(onDismissRequest={ leave=false },title={ Text("断开后台管理？") },
        text={ Text("变更正在执行。断开后结果可能无法确认，重新进入时请刷新实际会话，不要直接重复操作。") },
        confirmButton={ TextButton(onClick={ leave=false;model.leave() }) { Text("断开") } },
        dismissButton={ TextButton(onClick={ leave=false }) { Text("继续等待") } })
}

@Composable
private fun TmuxNameDialog(title: String,initial: String,onDismiss: ()->Unit,confirm: (String)->Unit) {
    var name by remember { mutableStateOf(initial) }
    val valid=name.isNotBlank() && name.toByteArray(Charsets.UTF_8).size<=1023 && name.none { it.isISOControl() }
    AlertDialog(onDismissRequest=onDismiss,title={ Text(title) },
        text={ OutlinedTextField(name,{ name=it },singleLine=true,label={ Text("会话名称") },modifier=Modifier.testTag("tmux-name")) },
        confirmButton={ TextButton(onClick={ confirm(name) },enabled=valid,modifier=Modifier.testTag("tmux-name-confirm")) { Text("确认") } },
        dismissButton={ TextButton(onClick=onDismiss) { Text("取消") } })
}
