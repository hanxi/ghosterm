package com.hanxi.ghosterm.bridge

import android.os.ParcelFileDescriptor
import java.io.FileDescriptor

/** API 29-compatible POSIX descriptor controls, without hidden Android APIs.
 * A temporary duplicate pins the open file description during each status call.
 * F_SETFL affects the original description too; descriptor flags stay separate. */
object FileDescriptorFlags {
    init { System.loadLibrary("ghosterm_android") }
    fun status(fd: FileDescriptor): Int = ParcelFileDescriptor.dup(fd).use { getStatus(it.fd) }
    fun status(fd: FileDescriptor, flags: Int) {
        ParcelFileDescriptor.dup(fd).use { setStatus(it.fd, flags) }
    }
    /** Observe the caller-owned descriptor itself, including its CLOEXEC bit. */
    fun descriptorFlags(fd: Int): Int = getDescriptor(fd)
    private external fun getStatus(fd: Int): Int
    private external fun setStatus(fd: Int, flags: Int)
    private external fun getDescriptor(fd: Int): Int
}
