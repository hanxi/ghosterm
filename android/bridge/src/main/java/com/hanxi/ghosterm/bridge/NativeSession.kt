package com.hanxi.ghosterm.bridge

import com.hanxi.ghosterm.terminal.TerminalFrame
import com.hanxi.ghosterm.terminal.TerminalSurface
import java.io.Closeable

/** Control plane. Output and key encoding remain on the native IO owner. */
class LocalSession(home: String) : TerminalSurface, Closeable {
    private var handle = NativeSession.create(home.toByteArray(Charsets.UTF_8))
    @Synchronized override fun frame(after: Long): TerminalFrame? =
        if (handle == 0L) null else NativeSession.readFrame(handle, after)?.let(TerminalFrame::decode)
    @Synchronized override fun resize(cols: Int, rows: Int, cellWidth: Int, cellHeight: Int) {
        check(handle != 0L) { "终端会话已关闭" }
        NativeSession.setSize(handle, cols, rows, cellWidth, cellHeight)
    }
    @Synchronized override fun commit(text: String) = input(1, 0, 0, text)
    @Synchronized override fun key(code: Int, modifiers: Int, text: String) = input(2, code, modifiers, text)
    @Synchronized override fun scroll(lines: Int) = input(3, lines, 0, "")
    private fun input(kind: Int, code: Int, mods: Int, text: String) {
        check(handle != 0L) { "终端会话已关闭" }
        NativeSession.enqueue(handle, kind, code, mods, text.toByteArray(Charsets.UTF_8))
    }
    @Synchronized fun state(): SessionState {
        if (handle == 0L) return SessionState(2, null, 1, "会话已关闭")
        val value = NativeSession.readState(handle)
        return SessionState(value[0], if (value[1] != 0) value[2] else null, value[3],
            if (value[0] == 3) NativeSession.readError(handle).toString(Charsets.UTF_8) else null)
    }
    @Synchronized override fun close() {
        NativeSession.dispose(handle)
        handle = 0L
    }
}

data class SessionState(val phase: Int, val exitCode: Int?, val child: Int, val error: String?) {
    val connected: Boolean get() = phase == 1
    val terminal: Boolean get() = phase >= 2
    val description: String get() = when (phase) {
        0 -> "正在启动系统 shell…"
        1 -> "已连接 · 系统 shell"
        2 -> exitCode?.let { "已退出 · 状态码 $it" } ?: "连接已断开 · 退出状态未知"
        else -> error ?: "终端连接失败"
    }
}

/** Package-private operations are generation checked even if callers retain a stale ID. */
internal object NativeSession {
    init { System.loadLibrary("ghosterm_android") }
    external fun create(home: ByteArray): Long
    external fun dispose(handle: Long)
    external fun enqueue(handle: Long, kind: Int, key: Int, mods: Int, bytes: ByteArray)
    external fun setSize(handle: Long, cols: Int, rows: Int, cellWidth: Int, cellHeight: Int)
    external fun readFrame(handle: Long, after: Long): IntArray?
    external fun readState(handle: Long): IntArray
    external fun readError(handle: Long): ByteArray
    external fun ownerCount(): Int
}
