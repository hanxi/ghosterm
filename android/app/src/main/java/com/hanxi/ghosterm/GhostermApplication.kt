package com.hanxi.ghosterm

import android.app.Application
import android.system.Os
import java.io.File

class GhostermApplication : Application() {
    override fun onCreate() {
        super.onCreate()
        // Native OpenSSH-compatible Include/~ resolution uses this app-private home.
        // This affects only the Android app process, never the developer's host shell.
        Os.setenv("HOME", File(filesDir, "home").absolutePath, true)
    }
}
