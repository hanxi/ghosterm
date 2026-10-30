package com.hanxi.ghosterm

import com.hanxi.ghosterm.bridge.NativeTerminalSession
import java.lang.reflect.InvocationTargetException

/** Test-only access to lifetime diagnostics; never expose native IDs in application UI. */
internal object NativeTerminalProbe {
    private val type = Class.forName("com.hanxi.ghosterm.bridge.NativeSession")
    private val instance = type.getField("INSTANCE").get(null)
    fun owners() = type.getMethod("ownerCount").invoke(instance) as Int
    fun handle(session: NativeTerminalSession): Long = NativeTerminalSession::class.java.getDeclaredField("handle").run {
        isAccessible = true; getLong(session)
    }
    fun readState(handle: Long): IntArray = try {
        type.getMethod("readState", Long::class.javaPrimitiveType).invoke(instance, handle) as IntArray
    } catch (error: InvocationTargetException) { throw error.targetException }
    fun dispose(handle: Long) { type.getMethod("dispose", Long::class.javaPrimitiveType).invoke(instance, handle) }
}
