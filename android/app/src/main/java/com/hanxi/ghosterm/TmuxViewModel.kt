package com.hanxi.ghosterm

import androidx.compose.runtime.*
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.hanxi.ghosterm.bridge.*
import kotlinx.coroutines.*

/** Explicit remote management page. Leaving closes management/authentication;
 * an already attached terminal has a separate SSH owner and keeps running. */
class TmuxViewModel : ViewModel() {
    var visible by mutableStateOf(false);private set
    var alias by mutableStateOf("");private set
    var busy by mutableStateOf(false);private set
    var changing by mutableStateOf(false);private set
    var connecting by mutableStateOf(false);private set
    var error by mutableStateOf<String?>(null);private set
    var notice by mutableStateOf<String?>(null);private set
    var candidates by mutableStateOf<List<RemoteTmuxCandidate>>(emptyList());private set
    var selectedPath by mutableStateOf<String?>(null);private set
    var server by mutableStateOf<RemoteTmuxServer?>(null);private set
    var entries by mutableStateOf<List<RemoteTmuxEntry>>(emptyList());private set
    private var generation=0L
    private var target: PreparedSSH?=null
    private var controller: SessionsViewModel?=null
    private var connection: RemoteTmuxSession?=null
    private var authentication: java.util.UUID?=null
    private var job: Job?=null
    val canChange get()=!busy && !connecting && selectedPath!=null && server!=null && error==null
    val canRefresh get()=!busy && !connecting && connection!=null
    fun open(request: PreparedSSH,sessions: SessionsViewModel) {
        leave()
        val token=++generation
        target=request.copy(identityFiles=request.identityFiles.toList());controller=sessions
        alias=request.alias;visible=true;connecting=true;error=null;notice=null
        val id=sessions.connectTmux(request) { remote,user ->
            if(generation!=token || !visible) { remote.close();return@connectTmux }
            authentication=null;connection=remote;target=target?.copy(user=user);connecting=false
            probe()
        }
        authentication=id
        if(id==null){connecting=false;error="另一个连接正在认证，请完成后重新连接后台会话"}
    }
    fun reconnect() {
        val request=target ?: return;val sessions=controller ?: return
        open(request,sessions)
    }
    fun authenticationDismissed() {
        if(authentication!=null && controller?.connecting?.id!=authentication) {
            authentication=null
            if(connecting) { connecting=false;error="后台会话连接已取消" }
            else notice="连接请求已结束，后续管理请刷新列表后重新选择"
        }
    }
    fun leave() {
        generation++;authentication?.let { controller?.cancelConnection(it) };authentication=null
        job?.cancel();job=null;connection?.close();connection=null
        visible=false;connecting=false;busy=false;changing=false
        candidates=emptyList();selectedPath=null;entries=emptyList();server=null
    }
    private fun operate(change: Boolean=false,action: suspend (RemoteTmuxSession)->Unit) {
        val remote=connection ?: return
        if(busy)return
        val token=generation;busy=true;changing=change;error=null;notice=null
        entries=emptyList();server=null
        job=viewModelScope.launch {
            try {
                action(remote)
            } catch(cancelled: CancellationException) { throw cancelled }
            catch(failure: Exception) {
                if(generation==token) { entries=emptyList();server=null;error=if(failure is RemoteTmuxUnknownException) "变更结果无法确认，请重新连接并核对实际会话。" else failure.message ?: "无法确认后台会话状态" }
            } finally {
                if(generation==token){busy=false;changing=false}
            }
        }
    }
    fun probe()=operate { remote ->
        selectedPath=null;candidates=emptyList()
        val result=remote.discover()
        candidates=result.candidates
        if(result.status!=0) {
            error=when(result.status) {
                2 -> "远端未找到 tmux"
                7 -> "远端 tmux 版本或环境不支持"
                else -> "无法确认远端 tmux 状态（错误 "+result.status+"）："+result.error
            }
        } else if(result.candidates.isEmpty()) error="远端未找到 tmux"
        else {
            val first=result.candidates.first()
            if(first.usable)selectAndList(remote,first.path)
            else error="默认 tmux 不可用或版本无法确认，请查看路径与版本后选择"
        }
    }
    fun select(path: String) {
        if(candidates.none { it.path==path && it.usable }) { error="此路径尚未确认可用";return }
        operate { remote -> selectAndList(remote,path) }
    }
    private suspend fun selectAndList(remote: RemoteTmuxSession,path: String) {
        selectedPath=null
        val selected=remote.selectExecutable(path)
        if(selected.status!=0){error="无法打开此 tmux（错误 "+selected.status+"）："+selected.error;return}
        selectedPath=path
        load(remote)
    }
    fun refresh()=operate { remote ->
        if(selectedPath==null) { error="请先探测并选择 tmux 路径";return@operate }
        load(remote)
    }
    private suspend fun load(remote: RemoteTmuxSession) {
        val result=remote.list()
        if(result.status==0) { server=checkNotNull(result.server);entries=result.entries;error=null }
        else { server=null;entries=emptyList();error="后台列表读取失败（错误 "+result.status+"）："+result.error }
    }
    fun create(name: String)=mutate("创建") { it.create(name) }
    fun rename(entry: RemoteTmuxEntry,name: String)=mutate("重命名") { it.rename(entry,name) }
    fun terminate(entry: RemoteTmuxEntry)=mutate("结束") { it.terminate(entry) }
    private fun mutate(label: String,action: suspend (RemoteTmuxSession)->RemoteTmuxChange) {
        if(!canChange)return
        operate(change=true) { remote ->
            val result=action(remote)
            when(result.result) {
                RemoteTmuxResult.CONFIRMED -> {
                    notice=label+"已确认"
                    load(remote)
                }
                RemoteTmuxResult.REJECTED -> error=label+"请求已拒绝（错误 "+result.status+"）："+result.error
                RemoteTmuxResult.UNKNOWN -> error=label+"结果无法确认，请刷新实际会话后再操作；不会自动重试。"+result.error
            }
        }
    }
    fun attach(entry: RemoteTmuxEntry) {
        if(!canChange)return
        val request=target ?: return;val remote=connection ?: return;val sessions=controller ?: return
        try {
            val selection=remote.target(entry)
            if(!sessions.attachTmux(request,selection,entry.name)) { error="另一个连接正在认证";return }
            authentication=sessions.connecting?.id
            entries=emptyList();server=null;notice="正在连接；之后的管理操作请刷新列表"
        } catch(failure: Exception) { error=failure.message ?: "后台会话选择已失效" }
    }
    override fun onCleared() { leave();super.onCleared() }
}
