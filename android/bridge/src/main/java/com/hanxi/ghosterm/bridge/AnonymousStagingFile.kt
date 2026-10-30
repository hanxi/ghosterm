package com.hanxi.ghosterm.bridge

import android.os.ParcelFileDescriptor
import java.io.File

/** Platform filesystem helper, called only on the staging IO worker.
 * The returned descriptor is owned by the caller. No named-file fallback. */
object AnonymousStagingFile {
    init { System.loadLibrary("ghosterm_android") }
    fun create(directory: File): ParcelFileDescriptor {
        val bytes=directory.absolutePath.toByteArray(Charsets.UTF_8)
        require(bytes.isNotEmpty() && bytes.none { it==0.toByte() }) { "无效的暂存路径" }
        return ParcelFileDescriptor.adoptFd(open(bytes))
    }
    private external fun open(directory: ByteArray): Int
}
