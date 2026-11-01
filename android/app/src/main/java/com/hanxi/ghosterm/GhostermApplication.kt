package com.hanxi.ghosterm

import android.app.Application
import android.system.Os
import java.io.File
import com.hanxi.ghosterm.data.PreferencesFile
import com.hanxi.ghosterm.data.PreferencesRepository

class GhostermApplication : Application() {
    val externalPreviews by lazy { ExternalPreviewCache(this,File(cacheDir,"external-previews"), "$packageName.previews") }
    val fileWork by lazy { FileWorkRuntime(this) }
    val settings by lazy { AppSettings(PreferencesRepository(PreferencesFile(File(filesDir, "preferences")))) }
    override fun onCreate() {
        super.onCreate()
        externalPreviews.sweepLater()
        // Native OpenSSH-compatible Include/~ resolution uses this app-private home.
        // This affects only the Android app process, never the developer's host shell.
        Os.setenv("HOME", File(filesDir, "home").absolutePath, true)
    }
}
