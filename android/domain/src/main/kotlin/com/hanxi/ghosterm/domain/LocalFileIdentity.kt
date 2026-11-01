package com.hanxi.ghosterm.domain

import java.util.UUID

/** A creation observation in application storage, not authority to delete or replay.
 * The random marker travels with the inode through rename. Bytes are checked separately.
 * Hostile same-UID writers and filesystem backup/restore are outside this reference. */
data class LocalFileIdentityReference(val token: String, val candidateName: String,
    val device: Long, val inode: Long, val uid: Int,
    val candidateDevice: Long, val candidateInode: Long,
    val destinationDevice: Long, val destinationInode: Long) {
    init {
        require(UUID.fromString(token).toString()==token && UUID.fromString(candidateName).toString()==candidateName && uid>=0) {
            "本机文件创建身份无效"
        }
    }
}
enum class LocalFileIdentityState { SAME, DIFFERENT, MISSING, NO_REFERENCE, UNKNOWN }
data class LocalFileIdentityResult(val key: FileHistoryContentKey, val state: LocalFileIdentityState,
    val observedMillis: Long = System.currentTimeMillis(), val error: String? = null)
