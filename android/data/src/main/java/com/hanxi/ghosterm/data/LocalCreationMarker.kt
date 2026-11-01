package com.hanxi.ghosterm.data

import android.os.Process
import android.system.Os
import android.system.OsConstants
import com.hanxi.ghosterm.domain.LocalFileIdentityReference
import java.io.FileDescriptor
import java.io.IOException
import java.util.UUID

internal object LocalCreationMarker {
    const val NAME="user.ghosterm.copy"
    // Linux UAPI <linux/xattr.h>. OsConstants does not expose this flag.
    private const val CREATE_ONLY=1
    fun create(candidate:FileDescriptor, staging:FileDescriptor, destination:FileDescriptor,
        name:String):LocalFileIdentityReference {
        val file=Os.fstat(candidate);val stage=Os.fstat(staging);val target=Os.fstat(destination)
        check(OsConstants.S_ISREG(file.st_mode) && file.st_uid==Process.myUid() && file.st_nlink==1L &&
            file.st_size==0L && (file.st_mode and 511)==384) { "创建标记只能写入本次独占创建的空文件" }
        check(OsConstants.S_ISDIR(stage.st_mode) && stage.st_uid==Process.myUid() &&
            OsConstants.S_ISDIR(target.st_mode) && target.st_uid==Process.myUid()) { "创建目录身份无效" }
        val token=UUID.randomUUID().toString();val bytes=token.toByteArray(Charsets.US_ASCII)
        // The public path-based xattr API resolves a live, owned fd for both calls.
        android.os.ParcelFileDescriptor.dup(candidate).use { fd ->
            try {
                val pinned="/proc/self/fd/${fd.fd}"
                Os.setxattr(pinned,NAME,bytes,CREATE_ONLY)
                check(Os.getxattr(pinned,NAME).contentEquals(bytes)) { "文件创建标记未能读回" }
            } catch(error:Exception) { throw IOException("当前存储无法保存文件创建标记，尚未写入文件内容",error) }
            finally { bytes.fill(0) }
        }
        val after=Os.fstat(candidate)
        check(after.st_dev==file.st_dev && after.st_ino==file.st_ino && after.st_uid==file.st_uid &&
            after.st_nlink==1L && after.st_size==0L) { "创建标记期间文件身份发生变化" }
        return LocalFileIdentityReference(token,name,file.st_dev,file.st_ino,file.st_uid,
            stage.st_dev,stage.st_ino,target.st_dev,target.st_ino)
    }
}
