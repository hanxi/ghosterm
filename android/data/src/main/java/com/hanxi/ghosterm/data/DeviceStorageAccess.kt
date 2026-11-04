package com.hanxi.ghosterm.data

import android.Manifest
import android.app.AppOpsManager
import android.content.Context
import android.content.pm.PackageManager
import android.os.Build
import android.os.Environment
import java.io.File

/** Direct shared-storage reads. This never treats a SAF URI as a filesystem path. */
object DeviceStorageAccess {
    fun granted(context: Context): Boolean = if(Build.VERSION.SDK_INT>=30)
        Environment.isExternalStorageManager()
    else Environment.isExternalStorageLegacy() &&
        context.checkSelfPermission(Manifest.permission.READ_EXTERNAL_STORAGE)==PackageManager.PERMISSION_GRANTED &&
        context.getSystemService(AppOpsManager::class.java).unsafeCheckOpNoThrow(
            AppOpsManager.OPSTR_READ_EXTERNAL_STORAGE,android.os.Process.myUid(),context.packageName)==AppOpsManager.MODE_ALLOWED

    @Suppress("DEPRECATION")
    fun root(): File {
        check(if(Build.VERSION.SDK_INT>=30) Environment.isExternalStorageManager() else Environment.isExternalStorageLegacy()) {
            "设备存储访问未授权，请点击「浏览设备存储」授权"
        }
        // Android 10 may expose the platform root through /storage/self/primary.
        // Resolve this trusted root once; child paths still reject symlinks.
        return Environment.getExternalStorageDirectory().canonicalFile.also {
            check(it.isDirectory && it.canRead()) { "设备存储不可读，请检查存储权限或挂载状态" }
        }
    }
}
