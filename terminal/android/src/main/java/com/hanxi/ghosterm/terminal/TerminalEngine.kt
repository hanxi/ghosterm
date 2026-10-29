package com.hanxi.ghosterm.terminal

/** VT engine boundary; has no knowledge of SSH or local shells. */
object TerminalEngine {
    init { System.loadLibrary("ghosterm_vt_android") }
    val version: String get() = versionBytes().toString(Charsets.UTF_8)
    private external fun versionBytes(): ByteArray
    /** Creates a native VT, feeds input, and returns cursor position (load/ABI probe). */
    external fun probeCursor(input: ByteArray): IntArray
}
