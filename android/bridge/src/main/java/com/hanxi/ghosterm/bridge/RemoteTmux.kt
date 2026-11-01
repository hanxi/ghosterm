package com.hanxi.ghosterm.bridge

import android.os.ParcelFileDescriptor
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.CharBuffer
import java.nio.charset.CodingErrorAction
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext

data class RemoteTmuxPeer(val host: String, val port: Int, val user: String)
data class RemoteTmuxState(val phase: Int, val error: Int,val operationRunning: Boolean=false) {
    val ready get() = phase==1
    val finished get() = phase>=2
}
data class RemoteTmuxCandidate(val path: String, val version: String, val status: Int) {
    val usable get() = status==0
}
enum class RemoteTmuxServer { RUNNING, ABSENT }
enum class RemoteTmuxResult { CONFIRMED, REJECTED, UNKNOWN }
data class RemoteTmuxDiscovery(val status: Int, val candidates: List<RemoteTmuxCandidate>, val error: String)
data class RemoteTmuxOutcome(val status: Int, val error: String)
data class RemoteTmuxChange(val status: Int, val result: RemoteTmuxResult, val error: String)
data class RemoteTmuxList(val status: Int, val server: RemoteTmuxServer?,
    val entries: List<RemoteTmuxEntry>, val error: String)

/** Display fields are immutable. Operations use an owner/snapshot/index token,
 * never reconstruct a Core identity from an editable name or UI fields. */
class RemoteTmuxEntry internal constructor(internal val owner: RemoteTmuxSession,
    internal val snapshot: Long, internal val index: Int,
    val serverPid: ULong, val serverStarted: ULong, val id: ULong, val created: ULong,
    val windows: UInt, val attached: UInt, val destroyUnattached: Int, val exitUnattached: Int,
    val name: String, val socketPath: String)

class RemoteTmuxUnknownException(cause: Exception): IllegalStateException("变更可能已经送出，但结果无法读取，请重新连接并核对实际会话",cause)

class RemoteTmuxTarget internal constructor(internal val owner: RemoteTmuxSession,
    internal val snapshot: Long, internal val index: Int,internal val address: ResolvedSSHAddress) {
    internal fun open(connection: ResolvedSSHConnection): Long = owner.openTarget(this,connection)
    /** Reuse the authenticated management endpoint; no second DNS lookup. */
    fun connection(timeoutMillis: Int=60_000): ResolvedSSHConnection=owner.connectionFor(this,timeoutMillis)
}

/** Dedicated management SSH; Core calls and attach validation run on one
 * native actor. Terminal bytes never pass through this control interface. */
class RemoteTmuxSession(connection: ResolvedSSHConnection, private val budgetMillis: Int=5000,
    socketPath: String="") : AuthenticatedSSH {
    init {
        require(budgetMillis in 1..60_000)
        require(socketPath.isEmpty() || socketPath.startsWith('/'))
    }
    val peer=RemoteTmuxPeer(connection.host,connection.port,connection.user)
    private val knownHosts=connection.knownHosts
    private val completion=ParcelFileDescriptor.createPipe()
    private val closed=CompletableDeferred<Unit>()
    private var handle=try {
        connection.openTmux(completion[1].fd,budgetMillis,encode(socketPath,4095)).also {
            check(it!=0L) { "无法创建后台会话连接" }
        }
    } catch(failure: Throwable) { completion[0].close();throw failure }
    finally { completion[1].close() }
    private val operations=Mutex()
    private var snapshot=0L
    @Synchronized override fun authentication(): SSHAuthState {
        check(handle!=0L) { "后台连接已关闭" }
        return SSHAuthState.decode(handle,NativeTmux.authentication(handle),this)
    }
    @Synchronized fun state(): RemoteTmuxState {
        if(handle==0L)return RemoteTmuxState(3,8)
        val values=NativeTmux.state(handle)
        check(values.size==3 && values[0] in 0..3 && values[1] in 0..9 && values[2] in 0..1) { "后台连接状态无效" }
        return RemoteTmuxState(values[0],values[1],values[2]==1)
    }
    override fun decide(prompt: SSHAuthState, accept: Boolean)=respond(prompt,if(accept) 1 else 2)
    override fun password(prompt: SSHAuthState,password: ByteArray)=respond(prompt,3,password)
    override fun privateKey(prompt: SSHAuthState,pem: ByteArray,passphrase: ByteArray)=respond(prompt,4,pem,passphrase)
    @Synchronized private fun respond(prompt: SSHAuthState,kind: Int,first: ByteArray=byteArrayOf(),second: ByteArray=byteArrayOf()) {
        try {
            check(handle!=0L && prompt.owner==handle && prompt.scope===this) { "凭据不属于此后台连接" }
            NativeTmux.respond(handle,prompt.request,kind,first,second)
        } finally { first.fill(0);second.fill(0) }
    }
    @Synchronized override fun close() {
        if(handle==0L)return
        NativeTmux.dispose(handle);handle=0;snapshot=0
        CoroutineScope(Dispatchers.IO).launch {
            try {
                ParcelFileDescriptor.AutoCloseInputStream(completion[0]).use {
                    check(it.read()==-1) { "后台连接释放通知无效" }
                }
                closed.complete(Unit)
            } catch(failure: Throwable) { closed.completeExceptionally(failure) }
        }
    }
    suspend fun closeAndAwait() { close();closed.await() }
    suspend fun discover(): RemoteTmuxDiscovery {
        val response=operation(1)
        return RemoteTmuxDiscovery(response.status,response.candidates,response.error)
    }
    suspend fun selectExecutable(path: String): RemoteTmuxOutcome {
        require(path.startsWith('/')) { "请选择探测到的 tmux 绝对路径" }
        val response=operation(2,argument=path)
        return RemoteTmuxOutcome(response.status,response.error)
    }
    suspend fun list(): RemoteTmuxList {
        val response=operation(3)
        return RemoteTmuxList(response.status,response.server,response.entries,response.error)
    }
    suspend fun create(name: String): RemoteTmuxChange=change(4,null,name)
    suspend fun rename(entry: RemoteTmuxEntry,name: String): RemoteTmuxChange=change(5,entry,name)
    suspend fun terminate(entry: RemoteTmuxEntry): RemoteTmuxChange=change(6,entry,"")
    private suspend fun change(kind: Int,entry: RemoteTmuxEntry?,name: String): RemoteTmuxChange {
        if(kind!=6)require(name.isNotBlank()) { "请填写会话名称" }
        val response=operation(kind,entry,name)
        return RemoteTmuxChange(response.status,checkNotNull(response.outcome),response.error)
    }
    @Synchronized fun target(entry: RemoteTmuxEntry): RemoteTmuxTarget {
        validate(entry)
        val data=ByteBuffer.wrap(NativeTmux.address(handle)).order(ByteOrder.BIG_ENDIAN)
        check(data.remaining()==8 || data.remaining()==20) { "后台连接地址格式无效" }
        val scope=data.int;check(scope>=0)
        val address=ByteArray(data.remaining());data.get(address)
        return RemoteTmuxTarget(this,entry.snapshot,entry.index,ResolvedSSHAddress(address,scope))
    }
    private fun validate(entry: RemoteTmuxEntry) {
        check(handle!=0L && entry.owner===this && snapshot==entry.snapshot && snapshot>0) { "后台会话列表已过期，请刷新后重新选择" }
    }
    @Synchronized internal fun openTarget(target: RemoteTmuxTarget,connection: ResolvedSSHConnection): Long {
        check(handle!=0L && target.owner===this && snapshot==target.snapshot && snapshot>0) { "后台会话列表已过期，请刷新后重新选择" }
        check(RemoteTmuxPeer(connection.host,connection.port,connection.user)==peer) { "后台连接身份已改变，请重新选择" }
        check(connection.knownHosts==knownHosts) { "后台连接的主机信任来源已改变" }
        return connectionFor(target,connection.timeoutMillis).openTmuxTarget(handle,target.snapshot,target.index).also { snapshot=0 }
    }
    @Synchronized internal fun connectionFor(target: RemoteTmuxTarget,timeoutMillis: Int): ResolvedSSHConnection {
        check(handle!=0L && target.owner===this && snapshot==target.snapshot && snapshot>0) { "后台会话列表已过期，请刷新后重新选择" }
        require(timeoutMillis in 1..60_000)
        return ResolvedSSHConnection(peer.host,peer.port,peer.user,knownHosts,listOf(target.address),timeoutMillis)
    }
    private data class Response(val status: Int,val outcome: RemoteTmuxResult?,val server: RemoteTmuxServer?,
        val error: String,val candidates: List<RemoteTmuxCandidate>,val entries: List<RemoteTmuxEntry>)
    private suspend fun operation(kind: Int,entry: RemoteTmuxEntry?=null,argument: String=""): Response=withContext(Dispatchers.IO) {
        val bytes=encode(argument,if(kind==2)4095 else 1023)
        operations.withLock {
            val (owner,request)=synchronized(this@RemoteTmuxSession) {
                check(handle!=0L) { "后台连接已关闭" }
                if(entry!=null)validate(entry)
                val request=NativeTmux.begin(handle,kind,entry?.snapshot ?: 0,entry?.index ?: -1,bytes)
                snapshot=0
                handle to request
            }
            var finished=false
            try {
                var result: ByteArray?=null
                while(result==null) {
                    result=synchronized(this@RemoteTmuxSession) {
                        check(handle==owner) { "后台连接已关闭" }
                        NativeTmux.result(owner,request)
                    }
                    if(result==null) {
                        val state=state()
                        check(!state.finished) { "后台连接已结束（错误 "+state.error+"）" }
                        delay(20)
                    }
                }
                val response=decode(kind,checkNotNull(result))
                synchronized(this@RemoteTmuxSession) {
                    check(handle==owner) { "后台结果已失效" }
                    NativeTmux.finish(owner,request)
                }
                finished=true;response
            } catch(cancelled: kotlinx.coroutines.CancellationException) { throw cancelled }
            catch(failure: Exception) {
                if(kind in 4..6)throw RemoteTmuxUnknownException(failure)
                throw failure
            } finally {
                if(!finished)withContext(NonCancellable) { closeAndAwait() }
            }
        }
    }
    private fun decode(kind: Int,bytes: ByteArray): Response {
        check(bytes.size in 36..2*1024*1024) { "后台结果长度无效" }
        val data=ByteBuffer.wrap(bytes).order(ByteOrder.BIG_ENDIAN)
        check(data.int==1 && data.int==kind) { "后台结果版本或操作不匹配" }
        val status=data.int;check(status in 0..9)
        val outcome=data.int;val server=data.int;val epoch=data.long;val count=data.int
        check(epoch>0 && count in 0..(if(kind==1)64 else if(kind==3)256 else 0))
        check(status==0 || count==0) { "失败的后台结果包含部分列表" }
        fun text(limit: Int): String {
            check(data.remaining()>=4)
            val length=data.int;check(length in 0..limit && length<=data.remaining())
            val raw=ByteArray(length);data.get(raw)
            check(raw.none { it==0.toByte() })
            return Charsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT)
                .onUnmappableCharacter(CodingErrorAction.REPORT).decode(ByteBuffer.wrap(raw)).toString()
        }
        val error=text(4095)
        val candidates=mutableListOf<RemoteTmuxCandidate>();val entries=mutableListOf<RemoteTmuxEntry>()
        repeat(count) { index ->
            if(kind==1) {
                check(data.remaining()>=4);val capability=data.int;check(capability in 0..9)
                val path=text(4095);val version=text(127);check(path.startsWith('/'))
                candidates+=RemoteTmuxCandidate(path,version,capability)
            } else {
                check(data.remaining()>=48)
                val pid=data.long.toULong();val started=data.long.toULong();val id=data.long.toULong();val created=data.long.toULong()
                val windows=data.int.toUInt();val attached=data.int.toUInt();val destroy=data.int;val exit=data.int
                val name=text(1023);val socket=text(4095)
                check(pid>0uL && name.isNotEmpty() && socket.startsWith('/') && destroy in -1..3 && exit in -1..1)
                entries+=RemoteTmuxEntry(this,epoch,index,pid,started,id,created,windows,attached,destroy,exit,name,socket)
            }
        }
        check(!data.hasRemaining()) { "后台结果包含多余内容" }
        val change=if(kind in 4..6) {
            check(outcome in 0..2);RemoteTmuxResult.entries[outcome]
        } else { check(outcome==-1);null }
        val serverState=if(kind==3 && status==0) {
            check(server in 0..1);RemoteTmuxServer.entries[server]
        } else null
        synchronized(this) { check(handle!=0L);snapshot=if(kind==3 && status==0) epoch else 0 }
        return Response(status,change,serverState,error,candidates.toList(),entries.toList())
    }
    companion object {
        private fun encode(text: String,limit: Int): ByteArray {
            require('\u0000' !in text)
            val buffer=Charsets.UTF_8.newEncoder().onMalformedInput(CodingErrorAction.REPORT)
                .onUnmappableCharacter(CodingErrorAction.REPORT).encode(CharBuffer.wrap(text))
            require(buffer.remaining()<=limit) { "后台参数过长" }
            return ByteArray(buffer.remaining()).also(buffer::get)
        }
    }
}

internal object NativeTmux {
    init { System.loadLibrary("ghosterm_android") }
    external fun create(host: ByteArray,port: Int,user: ByteArray,known: ByteArray,
        addresses: Array<ByteArray>,scopes: IntArray,timeout: Int,fence: Int,budget: Int,socket: ByteArray): Long
    external fun dispose(handle: Long)
    external fun authentication(handle: Long): Array<ByteArray>
    external fun respond(handle: Long,request: Long,kind: Int,first: ByteArray,second: ByteArray)
    external fun state(handle: Long): IntArray
    external fun address(handle: Long): ByteArray
    external fun begin(handle: Long,kind: Int,snapshot: Long,index: Int,argument: ByteArray): Long
    external fun result(handle: Long,request: Long): ByteArray?
    external fun finish(handle: Long,request: Long)
    external fun ownerCount(): Int
    external fun createRequestCount(): Long
}
