package com.hanxi.ghosterm.bridge

import com.hanxi.ghosterm.terminal.KeyAction
import com.hanxi.ghosterm.terminal.TerminalFrame
import com.hanxi.ghosterm.terminal.TerminalSurface
import java.io.Closeable
import com.hanxi.ghosterm.terminal.SelectionOperation
import kotlinx.coroutines.delay
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock

/** Control plane. Output and key encoding remain on the native IO owner. */
open class NativeTerminalSession internal constructor(private var handle: Long, private val remote: Boolean = false) : TerminalSurface, Closeable {
    private val actionMutex = Mutex()
    @Synchronized override fun frame(after: Long): TerminalFrame? =
        if (handle == 0L) null else NativeSession.readFrame(handle, after)?.let(TerminalFrame::decode)
    @Synchronized override fun resize(cols: Int, rows: Int, cellWidth: Int, cellHeight: Int) {
        check(handle != 0L) { "终端会话已关闭" }
        NativeSession.setSize(handle, cols, rows, cellWidth, cellHeight)
    }
    @Synchronized override fun commit(text: String) = input(1, 0, 0, text)
    @Synchronized override fun key(code: Int, modifiers: Int, text: String, unshiftedCodepoint: Int, action: KeyAction) =
        input(2, code, modifiers, text, unshiftedCodepoint, action)
    @Synchronized override fun scroll(lines: Int) = input(3, lines, 0, "")
    private fun input(kind: Int, code: Int, mods: Int, text: String, unshiftedCodepoint: Int = 0, action: KeyAction = KeyAction.PRESS) {
        check(handle != 0L) { "终端会话已关闭" }
        NativeSession.enqueue(handle, kind, code, mods, unshiftedCodepoint, action.code, text.toByteArray(Charsets.UTF_8))
    }
    private suspend fun action(kind: Int, x: Int = 0, y: Int = 0, bytes: ByteArray = byteArrayOf()): ByteArray = actionMutex.withLock {
        val (owner, request) = synchronized(this) {
            check(handle != 0L) { "终端会话已关闭" }
            handle to NativeSession.beginAction(handle, kind, x, y, bytes)
        }
        try {
            var response: ByteArray? = null
            while (response == null) {
                response = synchronized(this) {
                    check(handle == owner) { "终端会话已关闭" }
                    NativeSession.readAction(owner, request)
                }
                if (response == null) delay(10)
            }
            check(response[0].toInt() != 3) { "终端文本操作失败，选区可能已失效或超过 1 MiB" }
            response
        } finally {
            synchronized(this) { if (handle == owner) NativeSession.endAction(owner, request) }
        }
    }
    override suspend fun paste(text: String, allowUnsafe: Boolean): Boolean =
        action(1, if (allowUnsafe) 1 else 0, bytes = text.toByteArray(Charsets.UTF_8))[0].toInt() == 1
    override suspend fun select(operation: SelectionOperation, x: Int, y: Int) {
        action(operation.code, x, y)
    }
    override suspend fun extendSelection(operation: SelectionOperation, x: Int, y: Int, scrollLines: Int) {
        require(operation == SelectionOperation.START || operation == SelectionOperation.END)
        require(scrollLines in -1..1)
        action(if (operation == SelectionOperation.START) 10 else 11, x, y, byteArrayOf(scrollLines.toByte()))
    }
    override suspend fun clearLocal() { action(8) }
    override suspend fun copySelection(): String = action(7).let { it.copyOfRange(1, it.size).toString(Charsets.UTF_8) }
    @Synchronized fun state(): SessionState {
        if (handle == 0L) return SessionState(2, null, 1, "会话已关闭")
        val value = NativeSession.readState(handle)
        val auth = if (remote) authentication() else null
        val authFailed = auth?.phase in listOf(SSHAuthPhase.FAILED, SSHAuthPhase.CANCELLED)
        return SessionState(value[0], if (value[1] != 0) value[2] else null, value[3],
            if (value[0] == 3) { if (authFailed) auth?.description else NativeSession.readError(handle).toString(Charsets.UTF_8) } else null,
            auth?.description)
    }
    @Synchronized fun authentication(): SSHAuthState {
        check(remote && handle != 0L) { "SSH 会话已关闭或不可用" }
        return SSHAuthState.decode(handle, NativeSession.readAuthentication(handle))
    }
    @Synchronized internal fun respond(prompt: SSHAuthState, kind: Int, first: ByteArray = byteArrayOf(), second: ByteArray = byteArrayOf()) {
        try {
            check(remote && handle != 0L && prompt.owner == handle) { "SSH 请求不属于此会话" }
            NativeSession.respondAuthentication(handle, prompt.request, kind, first, second)
        } finally { first.fill(0); second.fill(0) }
    }
    @Synchronized override fun close() {
        NativeSession.dispose(handle)
        handle = 0L
    }
}

class LocalSession(home: String) : NativeTerminalSession(NativeSession.create(home.toByteArray(Charsets.UTF_8)))

data class SessionState(val phase: Int, val exitCode: Int?, val child: Int, val error: String?, val remoteDescription: String? = null) {
    val connected: Boolean get() = phase == 1
    val terminal: Boolean get() = phase >= 2
    val description: String get() = when (phase) {
        0 -> remoteDescription ?: "正在启动系统 shell…"
        1 -> if (remoteDescription == null) "已连接 · 系统 shell" else "已连接 · SSH"
        2 -> exitCode?.let { "已退出 · 状态码 $it" } ?: "连接已断开 · 退出状态未知"
        else -> error ?: "终端连接失败"
    }
}

/** Package-private operations are generation checked even if callers retain a stale ID. */
internal object NativeSession {
    init { System.loadLibrary("ghosterm_android") }
    external fun create(home: ByteArray): Long
    external fun createRemote(host: ByteArray, port: Int, user: ByteArray, knownHosts: ByteArray, address: ByteArray, scope: Int, timeout: Int): Long
    external fun readAuthentication(handle: Long): Array<ByteArray>
    external fun respondAuthentication(handle: Long, request: Long, kind: Int, first: ByteArray, second: ByteArray)
    external fun dispose(handle: Long)
    external fun enqueue(handle: Long, kind: Int, key: Int, mods: Int, unshiftedCodepoint: Int, action: Int, bytes: ByteArray)
    external fun setSize(handle: Long, cols: Int, rows: Int, cellWidth: Int, cellHeight: Int)
    external fun readFrame(handle: Long, after: Long): IntArray?
    external fun readState(handle: Long): IntArray
    external fun readError(handle: Long): ByteArray
    external fun beginAction(handle: Long, kind: Int, x: Int, y: Int, text: ByteArray): Long
    external fun readAction(handle: Long, request: Long): ByteArray?
    external fun endAction(handle: Long, request: Long)
    external fun ownerCount(): Int
}
