package com.hanxi.ghosterm.data

import android.system.Os
import com.hanxi.ghosterm.domain.FileContentDigest
import java.io.FileDescriptor
import java.security.MessageDigest

/** IO worker only. Explicit offsets leave the borrowed read-only fd untouched. */
internal object FileContentHasher {
    fun digest(fd: FileDescriptor, size: Long, checkpoint: () -> Unit): FileContentDigest {
        require(size>=0)
        val hash=MessageDigest.getInstance("SHA-256")
        val buffer=ByteArray(64*1024);var offset=0L
        try {
            while(offset<size) {
                checkpoint()
                val count=Os.pread(fd,buffer,0,minOf(buffer.size.toLong(),size-offset).toInt(),offset)
                check(count>0) { "内容快照长度不足，未取得完整摘要" }
                hash.update(buffer,0,count);offset+=count
            }
            checkpoint()
            check(Os.pread(fd,buffer,0,1,offset)==0) { "内容快照长度已改变" }
            return FileContentDigest(offset,hash.digest().joinToString("") { "%02x".format(it.toInt() and 255) })
        } finally { buffer.fill(0) }
    }
}
